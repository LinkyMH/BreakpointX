#pragma once
#include "../BreakpointConstants.hpp"
#include "../Inspector/InspectorConstants.hpp"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Linky::BreakpointX
{
	/// @brief One event row with the counts from the last hit added to it.
	struct TraceRow
	{
		// Fusion's loop number. We show it as it is in the Inspector.
		std::uint32_t cycle;
		// Our own counter for cycle changes. We use it to group rows from the same cycle.
		std::uint64_t cycleSerial;
		int eventNumber;
		// The event's frame total when this row was recorded. Note that later hits don't change it!
		std::uint64_t hits;
		std::uint64_t cycleHits;
		// True if this row started the cycle. The history limit might remove that first row later.
		bool startsCycle;
	};

	/// @brief A copy that stays the same while callbacks record more hits.
	struct TraceSnapshot
	{
		std::vector<TraceRow> rows;
		std::uint64_t discardedRows = 0;
		std::uint64_t currentCycleSerial = 0;
		bool firstCyclePartial = false;
	};

	/// @brief Count hits and handle Continue for one runtime. This part doesn't use Windows controls.
	class EventTrace
	{
	public:
		// This limit counts event rows. The Inspector adds the cycle headings.
		static constexpr std::size_t MaxRows = 10000;

		/// @brief Record a hit. Combine it with the last row only if it's the same event in the same cycle.
		/// @param cycle Fusion's rhLoopCount for this hit. Fastloops use their enclosing cycle.
		/// @param eventNumber The event number, or EventNumber::Unavailable if we couldn't get it.
		void Record(std::uint32_t cycle, int eventNumber);

		/// @brief Keep recording this cycle without pausing again.
		/// @param cycle The Fusion loop number whose remaining hits should skip pausing.
		void ContinueCycle(std::uint32_t cycle) { _continuedCycle = cycle; _continuing = true; }

		/// @brief Stop skipping pauses after Continue.
		void BreakNext() { _continuing = false; }

		/// @brief Allow another pause when the cycle changes or Next forces one.
		/// @param cycle Fusion's current loop number.
		/// @param force True when Next Breakpoint should allow a pause even after Continue.
		bool ShouldPause(std::uint32_t cycle, bool force)
		{
			if (force || (_continuing && cycle != _continuedCycle))
				_continuing = false;
			return !_continuing;
		}

		/// @brief Copy the rows we still have and the history details for the Inspector.
		TraceSnapshot Capture() const;

		/// @brief Get the tab ID saved on this trace.
		int SelectedTab() const { return _selectedTab; }

		/// @brief Save the tab ID if it's valid.
		/// @param tab The Inspector tab ID to remember. Invalid IDs are ignored.
		void SelectTab(int tab) { if (Inspector::Tab::IsValid(tab)) _selectedTab = tab; }

	private:
		std::deque<TraceRow> _rows;
		// Keep counts separately so removing old rows doesn't lose their hits.
		std::unordered_map<int, std::uint64_t> _totals, _cycleTotals;
		std::uint32_t _cycle = 0;
		std::uint64_t _cycleSerial = 0, _discardedRows = 0;
		bool _hasCycle = false;
		int _selectedTab = Inspector::Tab::Simplified;
		std::uint32_t _continuedCycle = 0;
		bool _continuing = false;
	};

	// Fusion's runtime/UI thread uses this state. Shared pointers keep it alive during nested callbacks.
	// The active flag becomes false when the last registered instance is destroyed.
	struct BreakpointRuntime
	{
		EventTrace trace;
		bool enabled = true;
		bool breakOnNext = false;
		// A shared pointer can keep this around after the runtime ends. Check active before using the runtime.
		bool active = true;
		long lastEvent = EventNumber::Unavailable;
	};

	/// @brief Keep one shared state for all instances in the same runtime.
	class EventTraceRegistry
	{
	public:
		/// @brief Add an instance. The first one in a runtime gets new state.
		/// @param runtime The Fusion runtime header address. We use it as a key without reading through it.
		/// @param instance The BreakpointX instance address to register. Both addresses must be non-null.
		void Attach(const void* runtime, const void* instance);

		/// @brief Remove an instance. The last one marks the state inactive and removes its entry.
		/// @param runtime The runtime header address used when this instance was registered.
		/// @param instance The registered instance address to remove.
		void Detach(const void* runtime, const void* instance);

		/// @brief Get the recorder pointer, or null if this runtime has no entry.
		/// Use Acquire if a callback could destroy the last instance while you still need the state.
		/// @param runtime The runtime header address whose recorder we want.
		EventTrace* Find(const void* runtime);

		/// @brief Keep the state around during nested calls. Remember to check active too.
		/// @param runtime The runtime header address whose shared state we want.
		std::shared_ptr<BreakpointRuntime> Acquire(const void* runtime) const noexcept;

		/// @brief Check if this instance is still registered with this runtime.
		/// @param runtime The runtime header address to look up.
		/// @param instance The instance address to look for in that runtime.
		bool Contains(const void* runtime, const void* instance) const noexcept;

	private:
		struct Entry { std::shared_ptr<BreakpointRuntime> state; std::unordered_set<const void*> instances; };
		std::unordered_map<const void*, Entry> _entries;
	};
} // namespace Linky::BreakpointX
