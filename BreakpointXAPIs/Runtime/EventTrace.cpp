#include "EventTrace.hpp"

namespace Linky::BreakpointX
{
	// Record one Break Here call. The API has already checked if it's allowed.
	void EventTrace::Record(std::uint32_t cycle, int eventNumber)
	{
		const bool startsCycle = !_hasCycle || cycle != _cycle;
		// New cycle, new counts for that cycle. The frame totals keep going.
		if (startsCycle)
		{
			_hasCycle = true;
			_cycle = cycle;
			++_cycleSerial;
			_cycleTotals.clear();
		}
		// We don't know which events these are, so keep each unknown hit in its own row.
		const bool known = EventNumber::IsValid(eventNumber);
		// Save the counts as they are right now. Unknown events get no counts.
		TraceRow row = { cycle, _cycleSerial, known ? eventNumber : EventNumber::Unavailable,
		 known ? ++_totals[eventNumber] : 0,
		 known ? ++_cycleTotals[eventNumber] : 0, startsCycle };
		// Only combine hits with the last row. For 3, 8, 3, we still need three rows.
		if (known && !_rows.empty() && _rows.back().cycleSerial == _cycleSerial &&
			_rows.back().eventNumber == eventNumber)
		{
			// Keep whether this row started the cycle when we update its counts.
			row.startsCycle = _rows.back().startsCycle;
			_rows.back() = row;
		}
		else
		{
			_rows.push_back(row);
			// Drop the oldest row. Its hits are still included in the totals. The row is gone, its crimes are remembered.
			if (_rows.size() > MaxRows)
			{
				_rows.pop_front();
				++_discardedRows;
			}
		}
	}

	// Copy the rows now so On Breakpoint can't change what this Inspector shows.
	TraceSnapshot EventTrace::Capture() const
	{
		TraceSnapshot snapshot;
		snapshot.rows.assign(_rows.begin(), _rows.end());
		snapshot.discardedRows = _discardedRows;
		snapshot.currentCycleSerial = _cycleSerial;
		// If the first row we kept came from mid-cycle, show that some of the cycle is missing.
		snapshot.firstCyclePartial = !_rows.empty() && !_rows.front().startsCycle;
		return snapshot;
	}


	// Finish making the new entry before adding it. If we run out of memory, there's nothing half-made in the map.
	void EventTraceRegistry::Attach(const void* runtime, const void* instance)
	{
		if (!runtime || !instance) return;
		auto found = _entries.find(runtime);
		// Another BreakpointX in this frame uses the same state.
		if (found != _entries.end()) { found->second.instances.insert(instance); return; }
		Entry entry;
		entry.state = std::make_shared<BreakpointRuntime>();
		entry.instances.insert(instance);
		_entries.emplace(runtime, std::move(entry));
	}

	void EventTraceRegistry::Detach(const void* runtime, const void* instance)
	{
		auto it = _entries.find(runtime);
		if (it == _entries.end()) return;
		it->second.instances.erase(instance);
		if (it->second.instances.empty())
		{
			// Mark the state as inactive before removing it. Other code might still have a shared pointer to it.
			it->second.state->active = false;
			_entries.erase(it);
		}
	}

	// Keep the state around while a callback adds or removes instances.
	std::shared_ptr<BreakpointRuntime> EventTraceRegistry::Acquire(const void* runtime) const noexcept
	{
		auto it = _entries.find(runtime);
		return it == _entries.end() ? nullptr : it->second.state;
	}

	// Check if this instance is still in the map. We don't need to read the instance itself.
	bool EventTraceRegistry::Contains(const void* runtime, const void* instance) const noexcept
	{
		auto it = _entries.find(runtime);
		return it != _entries.end() && it->second.instances.count(instance) != 0;
	}

	// Get the recorder pointer. It stays valid while its entry is in the map.
	EventTrace* EventTraceRegistry::Find(const void* runtime)
	{
		auto state = Acquire(runtime);
		return state ? &state->trace : nullptr;
	}
}
