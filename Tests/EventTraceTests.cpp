#include "../BreakpointXAPIs/Runtime/EventTrace.hpp"
#include <cassert>
#include <cstdio>
#include <type_traits>
using namespace Linky::BreakpointX;

static void ExampleAndSnapshots()
{
	EventTrace trace;
	assert(trace.Capture().rows.empty());
	for (unsigned cycle = 1; cycle <= 2; ++cycle)
	{
		trace.Record(cycle, 3);
		for (int i = 0; i < 4; ++i) trace.Record(cycle, 8);
		trace.Record(cycle, 19);
	}
	auto snapshot = trace.Capture();
	assert(snapshot.rows.size() == 6);
	for (size_t cycle = 0; cycle < 2; ++cycle)
	{
		const auto offset = cycle * 3;
		assert(snapshot.rows[offset].eventNumber == 3);
		assert(snapshot.rows[offset].hits == cycle + 1);
		assert(snapshot.rows[offset].cycleHits == 1);
		assert(snapshot.rows[offset + 1].eventNumber == 8);
		assert(snapshot.rows[offset + 1].hits == (cycle + 1) * 4);
		assert(snapshot.rows[offset + 1].cycleHits == 4);
		assert(snapshot.rows[offset + 2].eventNumber == 19);
		assert(snapshot.rows[offset + 2].hits == cycle + 1);
		assert(snapshot.rows[offset + 2].cycleHits == 1);
	}
	// Simulate a nested callback recording after the outer Inspector snapshot.
	trace.Record(2, 19);
	assert(snapshot.rows.back().hits == 2);
	assert(trace.Capture().rows.back().hits == 3);
	assert(snapshot.currentCycleSerial == snapshot.rows.back().cycleSerial);
}
static void OrderAndCycles()
{
	EventTrace trace;
	trace.Record(0, 3); trace.Record(0, 8); trace.Record(0, 3);
	auto snapshot = trace.Capture();
	assert(snapshot.rows.size() == 3);
	assert(snapshot.rows[0].hits == 1 && snapshot.rows[0].cycleHits == 1);
	assert(snapshot.rows[2].hits == 2 && snapshot.rows[2].cycleHits == 2);
	trace.Record(0, 3); // Consecutive action/condition hits or fastloop hits merge.
	assert(trace.Capture().rows.size() == 3);
	assert(trace.Capture().rows.back().cycleHits == 3);
	trace.Record(7, 3); // Skip empty cycles, never merge across cycles.
	snapshot = trace.Capture();
	assert(snapshot.rows.size() == 4 && snapshot.rows.back().cycle == 7);
	assert(snapshot.rows.back().hits == 4 && snapshot.rows.back().cycleHits == 1);
	trace.Record(0xffffffffu, 3); trace.Record(0, 3);
	snapshot = trace.Capture();
	assert(snapshot.rows.back().cycle == 0 && snapshot.rows.back().cycleHits == 1);
	assert(snapshot.rows.back().cycleSerial != snapshot.rows.front().cycleSerial);
	trace.Record(0, -1); trace.Record(0, -1); trace.Record(0, 0);
	snapshot = trace.Capture();
	assert(snapshot.rows.size() == 9);
	for (size_t i = 6; i < 9; ++i)
		assert(snapshot.rows[i].eventNumber == -1 && snapshot.rows[i].hits == 0 && snapshot.rows[i].cycleHits == 0);
	trace.Record(0, 65535);
	assert(trace.Capture().rows.back().eventNumber == 65535);
	static_assert(sizeof(decltype(TraceRow::hits)) == 8, "Totals must be 64-bit");
	static_assert(sizeof(decltype(TraceRow::cycleHits)) == 8, "Cycle counts must be 64-bit");
}
static void Retention()
{
	EventTrace trace;
	for (int i = 0; i < 10002; ++i) trace.Record(1, i % 2 ? 8 : 3);
	auto snapshot = trace.Capture();
	assert(snapshot.rows.size() == EventTrace::MaxRows);
	assert(snapshot.discardedRows == 2 && snapshot.firstCyclePartial);
	assert(snapshot.rows.back().hits == 5001 && snapshot.rows.back().cycleHits == 5001);
	trace.Record(1, 8);
	snapshot = trace.Capture();
	assert(snapshot.rows.size() == EventTrace::MaxRows && snapshot.discardedRows == 2);
	assert(snapshot.rows.back().hits == 5002 && snapshot.rows.back().cycleHits == 5002);
	trace.Record(20, 8);
	snapshot = trace.Capture();
	assert(snapshot.discardedRows == 3);
	assert(snapshot.rows.back().hits == 5003 && snapshot.rows.back().cycleHits == 1);
	EventTrace wholeCycles;
	for (unsigned i = 0; i <= 10000; ++i) wholeCycles.Record(i, 3);
	snapshot = wholeCycles.Capture();
	assert(snapshot.rows.size() == EventTrace::MaxRows && snapshot.discardedRows == 1);
	assert(!snapshot.firstCyclePartial && snapshot.rows.front().cycle == 1);
	assert(snapshot.rows.back().hits == 10001);
}
static void RuntimeOwnership()
{
	EventTraceRegistry registry;
	int frameA, frameB, first, second, third;
	assert(registry.Find(&frameA) == nullptr);
	registry.Attach(&frameA, &first); registry.Attach(&frameA, &first);
	registry.Attach(&frameA, &second); registry.Attach(&frameB, &third);
	registry.Find(&frameA)->Record(1, 3);
	registry.Find(&frameA)->SelectTab(2);
	registry.Find(&frameA)->SelectTab(9);
	assert(registry.Find(&frameA)->SelectedTab() == 2);
	assert(registry.Find(&frameB)->Capture().rows.empty());
	registry.Detach(&frameA, &first);
	assert(registry.Find(&frameA)->Capture().rows.size() == 1);
	assert(registry.Find(&frameA)->SelectedTab() == 2);
	const auto frozen = registry.Find(&frameA)->Capture();
	registry.Detach(&frameA, &second);
	assert(registry.Find(&frameA) == nullptr && registry.Find(&frameB) != nullptr);
	registry.Attach(&frameA, &first); // Restart and reuse the same runtime address.
	assert(registry.Find(&frameA)->Capture().rows.empty());
	assert(registry.Find(&frameA)->SelectedTab() == 0);
	assert(frozen.rows.size() == 1 && frozen.rows[0].hits == 1);
	registry.Detach(nullptr, nullptr);
}
static void ContinueBehavior()
{
	EventTrace trace;
	trace.Record(1, 3);
	assert(trace.ShouldPause(1, false));
	trace.ContinueCycle(1);
	for (int i = 0; i < 4; ++i)
	{
		trace.Record(1, 8);
		assert(!trace.ShouldPause(1, false));
	}
	trace.Record(1, 19);
	assert(!trace.ShouldPause(1, false));
	auto snapshot = trace.Capture();
	assert(snapshot.rows.size() == 3 && snapshot.rows[1].hits == 4);
	trace.Record(2, 3);
	assert(trace.ShouldPause(2, false));
	assert(trace.Capture().rows.back().hits == 2);
	trace.ContinueCycle(2);
	assert(trace.ShouldPause(2, true));
	trace.ContinueCycle(2);
	trace.BreakNext();
	assert(trace.ShouldPause(2, false));
	EventTrace otherRuntime;
	assert(otherRuntime.ShouldPause(2, false));
	trace.ContinueCycle(0xffffffffu);
	assert(trace.ShouldPause(0, false));
	trace.ContinueCycle(10);
	assert(trace.ShouldPause(15, false));
}
int main()
{
	ExampleAndSnapshots(); OrderAndCycles(); Retention(); RuntimeOwnership(); ContinueBehavior();
	puts("Events Trace: Example, Order, Cycle, Snapshot, Retention and Lifecycle Tests Passed!");
}
