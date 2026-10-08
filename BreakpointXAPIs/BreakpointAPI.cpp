#include "../Common.h"
#include "BreakpointAPI.hpp"
#include "Inspector/Inspector.hpp"
#include "Runtime/RuntimeMode.hpp"
#include "Utilities/ScopeExit.hpp"
#include <algorithm>
#include <memory>

namespace
{
	using Linky::BreakpointX::InspectorSnapshot;
	using Linky::BreakpointX::InstanceSnapshot;
	using Linky::BreakpointX::ObjectSelectionSnapshot;
	namespace EventNumber = Linky::BreakpointX::EventNumber;
	namespace FusionRuntime = Linky::BreakpointX::FusionRuntime;
#ifdef BREAKPOINTX_TESTING
	bool g_testEditorRun = false;
#endif
	// The app running this DLL doesn't change, so we only need to check its path once.
	bool CanUseBreakpoints()
	{
#ifdef BREAKPOINTX_TESTING
		if (g_testEditorRun) return true;
#endif
		try
		{
			static const bool editorRun = Linky::BreakpointX::IsEditorTestRun(hInstLib);
			return editorRun;
		}
		catch (...) { return false; }
	}
	// Each runtime has its own state. BreakpointX instances in that runtime share it.
	Linky::BreakpointX::EventTraceRegistry g_eventTraces;
	// An object number is a slot in Fusion's object table. Check it before reading the object.
	static headerObject* ObjectAt(LPRH rhPtr, int objectNumber)
	{
		if (rhPtr == NULL || rhPtr->rhObjectList == NULL)
			return NULL;
		if (objectNumber < 0 || objectNumber >= rhPtr->rhMaxObjects)
			return NULL;

		return rhPtr->rhObjectList[objectNumber].oblOffset;
	}

	// An FV puts the object number and creation ID into one number. Cool right?
	static DWORD MakeFixedValue(headerObject* object)
	{
		if (object == NULL)
			return 0;

		return static_cast<DWORD>(FixedVal(reinterpret_cast<LPRO>(object)));
	}

	// Find the instance we saved earlier.
	static headerObject* ObjectFromFixed(LPRH rhPtr, DWORD fixedValue)
	{
		headerObject* object = ObjectAt(rhPtr, static_cast<int>(LOWORD(fixedValue)));
		if (object == NULL)
			return NULL;
		// If the creation ID changed, Fusion reused the slot for a different instance. Same address, new tenant.
		if (object->hoCreationId != HIWORD(fixedValue))
			return NULL;

		return object;
	}

	// Read the name without going past the SDK buffer. Use a resource label if there's no name.
	static std::wstring GetObjectName(const objInfoList& oil, int oiIndex)
	{
		size_t length = 0;
		while (length < _countof(oil.oilName) && oil.oilName[length] != L'\0')
			++length;

		if (length != 0)
			return std::wstring(oil.oilName, oil.oilName + length);

		const wchar_t* format = nullptr;
		const int formatLength = LoadStringW(hInstLib, IDS_INSPECTOR_OBJECT_NAME, reinterpret_cast<LPWSTR>(&format), 0);
		if (formatLength <= 0) return {};
		const std::wstring pattern(format, formatLength);
		const int size = _scwprintf(pattern.c_str(), oiIndex);
		if (size < 0) return {};
		std::vector<wchar_t> text(static_cast<size_t>(size) + 1);
		swprintf_s(text.data(), text.size(), pattern.c_str(), oiIndex);
		return text.data();
	}

	// DarkEdif::GetEventNumber reads evgInhibit in the old SDK for Fusion 2.5,
	// and evgIdentifier for MMF2. Read it before we run On Breakpoint.
	static long ResolveCurrentEventNumber(LPRH rhPtr)
	{
		if (rhPtr == NULL || rhPtr->rhEventGroup == NULL)
			return EventNumber::Unavailable;

		const mv* runtime = rhPtr->rh4.rh4Mv;
		if (runtime == NULL || runtime->mvGetVersion == NULL)
			return EventNumber::Unavailable;

		// This is the Fusion 2.5 version value used by DarkEdif.
		const DWORD fusion25Version = FusionRuntime::Version25;
		const DWORD version = runtime->mvGetVersion() & MMFVERSION_MASK;
		WORD eventNumber = 0;
		if (version == fusion25Version)
			eventNumber = static_cast<WORD>(rhPtr->rhEventGroup->evgInhibit);
		else if (version == MMFVERSION_20)
			eventNumber = rhPtr->rhEventGroup->evgIdentifier;
		else
			return EventNumber::Unavailable;

		// The old header makes evgInhibit signed. Keep all 16 bits when reading it.
		// Like DarkEdif::GetCurrentFusionEventNum, we treat zero as an unknown event number.
		return eventNumber != 0 ? static_cast<long>(eventNumber) : EventNumber::Unavailable;
	}

	// Save what the Inspector will show before On Breakpoint changes anything.
	static InspectorSnapshot CaptureInspectorSnapshot(LPRDATA rdPtr)
	{
		InspectorSnapshot snapshot;
		if (rdPtr == NULL || rdPtr->rHo.hoAdRunHeader == NULL)
			return snapshot;

		LPRH rhPtr = rdPtr->rHo.hoAdRunHeader;
		snapshot.eventNumber = ResolveCurrentEventNumber(rhPtr);
		// Save Fusion's current selection counter along with the event details.
		snapshot.selectionGeneration = rhPtr->rh2.rh2EventCount;

		if (rhPtr->rhEventGroup != NULL)
			snapshot.eventGroupIdentifier = rhPtr->rhEventGroup->evgIdentifier;

		if (rhPtr->rhOiList == NULL || rhPtr->rhObjectList == NULL)
			return snapshot;

		// An OI is an object type, like Active. Each copy of that Active is an instance.
		// Go through the object types, then collect the selected instances for each one.
		for (int oiIndex = 0; oiIndex < rhPtr->rhNumberOi; ++oiIndex)
		{
			objInfoList& oil = rhPtr->rhOiList[oiIndex];

			if (oil.oilOi < 0)
				continue;

			// Skip the calling BreakpointX's object type in its own Inspector.
			if (rdPtr->rHo.hoOiList == &oil)
				continue;

			// Matching counters mean Fusion has picked a set of instances for this type, possibly an empty set.
			// If they don't match, Fusion treats the type as implicitly selected. We skip it in this view.
			if (oil.oilEventCount != snapshot.selectionGeneration)
				continue;

			// Save the count Fusion reports. Below, we'll follow the list to get the actual instances.
			ObjectSelectionSnapshot objectSnapshot;
			objectSnapshot.name = GetObjectName(oil, oiIndex);
			objectSnapshot.oi = oil.oilOi;
			objectSnapshot.totalInstances = oil.oilNObjects;
			objectSnapshot.reportedSelected = oil.oilNumOfSelected;

			// oilListSelected gives us the first selected object's slot. A negative number means the list has ended.
			int objectNumber = oil.oilListSelected;
			int guard = 0;
			// Stop after at most rhMaxObjects steps. We have other plans today besides walking a broken list.
			while (objectNumber >= 0 && guard++ < rhPtr->rhMaxObjects)
			{
				headerObject* object = ObjectAt(rhPtr, objectNumber);
				if (object == NULL)
					break;

				// Copy these values so the Inspector can still show them after callbacks change the objects.
				InstanceSnapshot instance;
				instance.fixedValue = MakeFixedValue(object);
				instance.objectNumber = object->hoNumber;
				instance.x = object->hoX;
				instance.y = object->hoY;
				instance.destroyed = (object->hoFlags & HOF_DESTROYED) != 0;
				objectSnapshot.selectedInstances.push_back(instance);

				// hoNextSelected gives us the next selected object's slot.
				objectNumber = object->hoNextSelected;
			}

			snapshot.filteredObjects.push_back(std::move(objectSnapshot));
		}

		return snapshot;
	}

	// GenerateEvent() can change which objects the caller has selected.
	// Save those selections first, then put them back using Fusion's current
	// selection counter when On Breakpoint finishes.
	class SelectionBackup
	{
	public:
		// Save the selection before calling On Breakpoint. The shared state tells us if the runtime ends.
		explicit SelectionBackup(LPRDATA rdPtr, std::shared_ptr<Linky::BreakpointX::BreakpointRuntime> lifetime)
			: _rhPtr(rdPtr != NULL ? rdPtr->rHo.hoAdRunHeader : NULL), _lifetime(std::move(lifetime))
		{
			Capture();
		}

		// Put the selection back when this object is destroyed too, even if a callback throws.
		~SelectionBackup() noexcept { Restore(); }
		// Don't copy the backup. We only want one object in charge of putting the selection back.
		SelectionBackup(const SelectionBackup&) = delete;
		SelectionBackup& operator=(const SelectionBackup&) = delete;

		// Restore the selection once, as long as the runtime is still alive.
		void Restore() noexcept
		{
			// Check active before reading the runtime header. The callback might have ended the frame.
			if (_restored || !_lifetime->active) return;
			_restored = true;
			if (_rhPtr == NULL || _rhPtr->rhOiList == NULL || _rhPtr->rhObjectList == NULL)
				return;

			// The callback may have changed Fusion's counters. Use the new counters when restoring
			// the selected objects so Fusion knows this selection belongs to the current event generation.
			const int translatedEventCount = _rhPtr->rh2.rh2EventCount;
			const int translatedOrCount = _rhPtr->rh4.rh4EventCountOR;
			// Only restore object types that exist in both our saved list and the current runtime.
			const int oiCount = std::min<int>(
				_rhPtr->rhNumberOi,
				static_cast<int>(_oiStates.size()));

			for (int oiIndex = 0; oiIndex < oiCount; ++oiIndex)
			{
				OiState& state = _oiStates[oiIndex];
				objInfoList& oil = _rhPtr->rhOiList[oiIndex];

				// If the old OR counter was current, update it to the new one. Otherwise, keep its saved value.
				oil.oilEventCountOR =
					(state.eventCountOR == _originalOrCount)
					? translatedOrCount
					: state.eventCountOR;

				if (!state.wasExplicit)
				{
					// Put back the saved counters and list details for this implicitly selected type.
					// Fusion ignores that list while its event counter is out of date.
					oil.oilEventCount = state.eventCount;
					oil.oilListSelected = state.listSelected;
					oil.oilNumOfSelected = state.numSelected;
					continue;
				}

				// Give the saved selected-object list Fusion's current event counter.
				oil.oilEventCount = translatedEventCount;
				oil.oilListSelected = FusionRuntime::NoObject;
				oil.oilNumOfSelected = 0;

				// Start with an empty list and add the selected objects back in their original order.
				headerObject* previous = NULL;
				for (DWORD fixedValue : state.selectedFixedValues)
				{
					// Find the instance again. Skip it if it's gone or has been marked destroyed.
					headerObject* object = ObjectFromFixed(_rhPtr, fixedValue);
					if (object == NULL || (object->hoFlags & HOF_DESTROYED) != 0)
						continue;

					// This is the end of the list for now. The next object we add will follow it.
					object->hoNextSelected = FusionRuntime::NoObject;

					// The first object becomes the list's head. Link each later object to the one before it.
					if (previous == NULL)
						oil.oilListSelected = object->hoNumber;
					else
						previous->hoNextSelected = object->hoNumber;

					previous = object;
					// Count the objects we add back. Some of the old selection may have been destroyed.
					++oil.oilNumOfSelected;
				}
			}

			// Put each instance's OR flag back after restoring the object-type selections.
			for (int objectNumber = 0; objectNumber < _rhPtr->rhMaxObjects; ++objectNumber)
			{
				headerObject* object = ObjectAt(_rhPtr, objectNumber);
				if (object == NULL)
					continue;

				const DWORD fixedValue = MakeFixedValue(object);
				// Find the saved OR flag by fixed value. Objects made during the callback get zero.
				auto found = _selectedInOr.find(fixedValue);
				object->hoSelectedInOR =
					(found != _selectedInOr.end()) ? found->second : 0;
			}
		}

	private:
		// The saved selection details for one object type, including the fixed values of its selected instances.
		struct OiState
		{
			int eventCount = 0;
			int eventCountOR = 0;
			short listSelected = FusionRuntime::NoObject;
			int numSelected = 0;
			bool wasExplicit = false;
			std::vector<DWORD> selectedFixedValues;
		};

		// Save the type's counters and list details, then the selected instances and OR flags.
		void Capture()
		{
			if (_rhPtr == NULL || _rhPtr->rhOiList == NULL || _rhPtr->rhObjectList == NULL)
				return;

			// Remember the selection counters from when we reached Break Here.
			_originalEventCount = _rhPtr->rh2.rh2EventCount;
			_originalOrCount = _rhPtr->rh4.rh4EventCountOR;
			_oiStates.resize(_rhPtr->rhNumberOi);

			for (int oiIndex = 0; oiIndex < _rhPtr->rhNumberOi; ++oiIndex)
			{
				const objInfoList& oil = _rhPtr->rhOiList[oiIndex];
				OiState& state = _oiStates[oiIndex];

				state.eventCount = oil.oilEventCount;
				state.eventCountOR = oil.oilEventCountOR;
				state.listSelected = oil.oilListSelected;
				state.numSelected = oil.oilNumOfSelected;
				// Matching counters mean this type has a specific selection for the current event generation.
				state.wasExplicit = oil.oilEventCount == _originalEventCount;

				// For an implicit selection, saving the counters is enough here. The old list doesn't tell us what's selected now.
				if (!state.wasExplicit)
					continue;

				int objectNumber = oil.oilListSelected;
				int guard = 0;
				while (objectNumber >= 0 && guard++ < _rhPtr->rhMaxObjects)
				{
					headerObject* object = ObjectAt(_rhPtr, objectNumber);
					if (object == NULL)
						break;

					// Save fixed values so we can spot deleted objects and slots that got reused.
					state.selectedFixedValues.push_back(MakeFixedValue(object));
					objectNumber = object->hoNextSelected;
				}
			}

			for (int objectNumber = 0; objectNumber < _rhPtr->rhMaxObjects; ++objectNumber)
			{
				headerObject* object = ObjectAt(_rhPtr, objectNumber);
				if (object != NULL)
					// OR flags belong to each instance. Save them even when the type's selection is implicit.
					_selectedInOr.emplace(MakeFixedValue(object), object->hoSelectedInOR);
			}
		}

		// Keep our state around while this backup exists. The active flag still tells us if the runtime has ended.
		std::shared_ptr<Linky::BreakpointX::BreakpointRuntime> _lifetime;
		bool _restored = false;
		LPRH _rhPtr = NULL;
		int _originalEventCount = 0;
		int _originalOrCount = 0;
		std::vector<OiState> _oiStates;
		std::unordered_map<DWORD, BYTE> _selectedInOr;
	};
}

namespace Linky::BreakpointX
{
	// Add this instance to its runtime's entry. Note that built apps skip this!
	void BreakpointAPI::AttachTrace(tagRDATA* rdPtr)
	{
		if (rdPtr && CanUseBreakpoints()) g_eventTraces.Attach(rdPtr->rHo.hoAdRunHeader, rdPtr);
	}

	// Remove this instance. If it was the last one, mark the state inactive and close its Inspectors.
	void BreakpointAPI::DetachTrace(tagRDATA* rdPtr) noexcept
	{
		if (!rdPtr) return;
		const void* identity = rdPtr->rHo.hoAdRunHeader;
		g_eventTraces.Detach(identity, rdPtr);
		if (!g_eventTraces.Find(identity)) CloseRuntimeInspectors(identity);
	}

	// Both Break Here forms call this. Catch errors here so they don't get passed back into Fusion.
	bool BreakpointAPI::Hit(tagRDATA* rdPtr) noexcept
	{
		try
		{
			if (!CanUseBreakpoints() || !rdPtr || !rdPtr->rHo.hoAdRunHeader) return false;
			LPRH runtime = rdPtr->rHo.hoAdRunHeader;
			auto state = g_eventTraces.Acquire(runtime);
			if (!state) return false;
			// Use Next's request once, then clear it. Disabled calls stop here unless Next forced this hit.
			const bool forceNext = std::exchange(state->breakOnNext, false);
			if (!state->enabled && !forceNext) return false;
			// Use Fusion's loop number directly. Fastloops stay in the cycle that called them.
			const auto cycle = static_cast<std::uint32_t>(runtime->rhLoopCount);
			const long eventNumber = ResolveCurrentEventNumber(runtime);
			state->trace.Record(cycle, static_cast<int>(eventNumber));
			state->lastEvent = eventNumber;
			// Continue still records hits in this cycle. We skip On Breakpoint and the Inspector until another pause is due.
			if (!state->trace.ShouldPause(cycle, forceNext)) return false;

			// Copy both views before On Breakpoint can change the event or its selected objects.
			const InspectorSnapshot snapshot = CaptureInspectorSnapshot(rdPtr);
			const TraceSnapshot traceSnapshot = state->trace.Capture();
			// Save the owner window now. The calling object might be destroyed by the callback.
			const HWND owner = runtime->rhHMainWin;
			SelectionBackup selection(rdPtr, state);
			// Run On Breakpoint. It can call Break Here again while we're inside this function.
			GenerateEvent(rdPtr, CND_ONBREAK);
			// Put the selected objects that still exist back before opening the Inspector.
			selection.Restore();

			// A callback can destroy the calling object or end the whole runtime.
			// Don't read rdPtr after the callback. Check that the instance and runtime still exist before opening the Inspector.
			if (!state->active || !g_eventTraces.Contains(runtime, rdPtr)) return false;
			return ShowInspector(snapshot, traceSnapshot, state, runtime, owner);
		}
		catch (...)
		{
			// Write a message to the debugger and let execution continue if processing the breakpoint throws.
			OutputDebugStringW(L"BreakpointX: Breakpoint processing failed. Execution resumed!\n");
			return false;
		}
	}

	// Check if this runtime allows breakpoints and if we're running through the editor.
	bool BreakpointAPI::IsEnabled(tagRDATA* rdPtr) noexcept
	{
		if (!CanUseBreakpoints() || !rdPtr) return false;
		auto state = g_eventTraces.Acquire(rdPtr->rHo.hoAdRunHeader);
		return state && state->enabled;
	}

	// Change the shared setting for this frame. Disabling also clears any pending Next request.
	void BreakpointAPI::SetEnabled(tagRDATA* rdPtr, bool enabled) noexcept
	{
		if (!rdPtr || !CanUseBreakpoints()) return;
		auto state = g_eventTraces.Acquire(rdPtr->rHo.hoAdRunHeader);
		if (!state) return;
		state->enabled = enabled;
		if (!enabled) state->breakOnNext = false;
	}

	// Use the same setter so Toggle follows the same rules as Enable and Disable.
	void BreakpointAPI::ToggleEnabled(tagRDATA* rdPtr) noexcept
	{
		SetEnabled(rdPtr, !IsEnabled(rdPtr));
	}

	// Get the last recorded event for this frame. Return Unavailable if there hasn't been one.
	long BreakpointAPI::GetLastBreakEvent(tagRDATA* rdPtr) noexcept
	{
		auto state = rdPtr ? g_eventTraces.Acquire(rdPtr->rHo.hoAdRunHeader) : nullptr;
		return state ? state->lastEvent : EventNumber::Unavailable;
	}
}
