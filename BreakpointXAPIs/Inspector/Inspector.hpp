#pragma once
#include "../Runtime/EventTrace.hpp"
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

namespace Linky::BreakpointX
{
	/// @brief What one instance looked like when we reached Break Here.
	struct InstanceSnapshot
	{
		// The creation ID lets us tell if Fusion reused this object's slot for a new instance.
		DWORD fixedValue = 0;
		short objectNumber = FusionRuntime::NoObject;
		int x = 0;
		int y = 0;
		bool destroyed = false;
	};

	/// @brief One object type and the instances picked for this event.
	struct ObjectSelectionSnapshot
	{
		std::wstring name;
		short oi = FusionRuntime::NoObject;
		int totalInstances = 0;
		// The count Fusion gave us. selectedInstances holds the objects we found by following the list.
		int reportedSelected = 0;
		std::vector<InstanceSnapshot> selectedInstances;
	};

	/// @brief A saved copy of the event and selected objects shown during this pause.
	struct InspectorSnapshot
	{
		long eventNumber = EventNumber::Unavailable;
		int eventGroupIdentifier = FusionRuntime::UnknownEventGroup;
		// Fusion's selection counter when we took the copy. It isn't the event number you see in the title.
		int selectionGeneration = FusionRuntime::UnknownSelectionGeneration;
		std::vector<ObjectSelectionSnapshot> filteredObjects;
	};

	/// @brief Open the saved Inspector windows and wait until the pause ends.
	/// @return False if setting up the Inspector or reading window messages fails.
	/// Memory errors can throw here. The breakpoint API catches them.
	/// @param snapshot The saved event and object selection. Keep it alive until this call returns.
	/// @param trace The saved trace rows and counts. Keep this copy alive until this call returns.
	/// @param runtime The shared state for this pause. Its active flag tells us if the runtime has ended.
	/// @param identity The Fusion runtime header address used to match pauses to their runtime.
	/// @param owner The Fusion window that owns the Inspector windows.
	bool ShowInspector(const InspectorSnapshot& snapshot, const TraceSnapshot& trace,
		const std::shared_ptr<BreakpointRuntime>& runtime, const void* identity, HWND owner);

	/// @brief Close all Inspector pauses for a runtime that's ending.
	/// @param identity The same runtime header address passed to ShowInspector. We only compare it.
	void CloseRuntimeInspectors(const void* identity) noexcept;
}
