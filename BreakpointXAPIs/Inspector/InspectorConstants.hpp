#pragma once

namespace Linky::BreakpointX::Inspector::Tab
{
	// Each tab has an ID and a bit we use to track which window it belongs to.
	enum Id { Simplified, ObjectSelection, EventsTrace, Count };

	/// @brief Get the bit for this tab.
	/// @param tab The bit position to use. Count is also used to build the All mask.
	constexpr int Mask(int tab) { return 1 << tab; }

	constexpr int All = Mask(Count) - 1;

	/// @brief Check if this tab ID is valid.
	/// @param tab The tab ID to check. Count itself isn't a tab.
	constexpr bool IsValid(int tab) { return tab >= Simplified && tab < Count; }
}

// These IDs follow the object table's column order.
namespace Linky::BreakpointX::Inspector::ObjectColumn
{
	enum Id { Type, FixedValue, X, Y, Status, Count };
}

// These IDs tell us which trace column needs text.
namespace Linky::BreakpointX::Inspector::TraceColumn
{
	enum Id { Event, Hits, CycleHits, Count };
}

namespace Linky::BreakpointX::Inspector
{
	// Convert saved column widths to this DPI so they all use the same scale.
	constexpr unsigned BaseDpi = 96;
	// Each tab lives in one window, so we never need more windows than tabs.
	constexpr int MaxWindows = Tab::Count;
	constexpr int MainWindowIndex = 0;
	constexpr int TypeListColumn = 0;
	enum TabCommand : unsigned { Detach = 1, ReturnToMain = 2 };
	constexpr int BreakpointButtonCount = 4;
	constexpr int ButtonGapCount = BreakpointButtonCount - 1;
}

namespace Linky::BreakpointX::Inspector::Layout
{
	// These sizes use dialog units. Values with Pixels in the name use pixels.
	constexpr int Margin = 7;
	constexpr int Gap = 4;
	constexpr int ButtonHeight = 16;
	constexpr int FullButtonMinWidth = 50;
	constexpr int ButtonTextPadding = 14;
	// Small buttons still need enough room to click them.
	constexpr int CompactButtonMinWidth = 16;
	constexpr int CompactButtonMaxWidth = 32;
	constexpr int CompactButtonTextPadding = 8;
	constexpr int CheckboxTextPadding = 14;
	constexpr int TextLineHeight = 12;
	constexpr int SimplifiedContentHeight = ButtonHeight + Gap + TextLineHeight;
	// The splitter's width and keyboard movement also use dialog units.
	constexpr int SplitterWidth = 5;
	constexpr int SplitterFineStep = 1;
	constexpr int SplitterStep = 8;
	constexpr int MinObjectPaneWidth = 60;
	constexpr int DefaultTypePaneWidth = 140;
	// Use these sizes when the data tab has no saved size yet.
	constexpr int DefaultWindowWidth = 480;
	constexpr int DefaultWindowHeight = 300;
	constexpr int MinWindowWidth = 150;
	constexpr int MinWindowHeight = 120;
	constexpr int MinWindowWidthPixels = 260;
	constexpr int MinWindowHeightPixels = 190;
	constexpr int TooltipMaxWidth = 260;
	constexpr int DetachedWindowOffset = 16;
	constexpr int TypeColumnWidth = 100;
	constexpr int FixedColumnWidth = 75;
	constexpr int PositionColumnWidth = 35;
	constexpr int StatusColumnWidth = 50;
	// Keep the widths in the same order as the object columns.
	constexpr int ObjectColumnWidths[ObjectColumn::Count] =
	{ TypeColumnWidth, FixedColumnWidth, PositionColumnWidth, PositionColumnWidth, StatusColumnWidth };
	constexpr int TraceEventColumnWidth = 230;
	constexpr int TraceHitsColumnWidth = 90;
	constexpr int TraceCycleHitsColumnWidth = 100;
	// Start the trace columns at these widths until the user saves new ones.
	constexpr int TraceColumnWidths[TraceColumn::Count] =
	{ TraceEventColumnWidth, TraceHitsColumnWidth, TraceCycleHitsColumnWidth };
}

namespace Linky::BreakpointX::Inspector::Preferences
{
	// Check this version if the saved data format changes.
	constexpr unsigned LayoutVersion = 1;
	constexpr unsigned MinDpi = BaseDpi / 2;
	constexpr unsigned MaxDpi = BaseDpi * 8;
	constexpr long MaxDimension = 32768;
	// Monitors to the left or above the main screen can have negative coordinates.
	constexpr long MinPosition = -100000;
	constexpr long MaxPosition = 100000;
	constexpr long MaxExtent = MaxPosition + MaxDimension;
	constexpr int ValueNameCapacity = 32;
	// These are the positions in the saved Columns data. Changing their order changes the file format in the Registry.
	constexpr int SplitterSlot = 0;
	constexpr int TypeColumnSlot = SplitterSlot + 1;
	constexpr int ObjectColumnsStart = TypeColumnSlot + 1;
	constexpr int DpiSlot = ObjectColumnsStart + ObjectColumn::Count;
	constexpr int DimensionSlotCount = DpiSlot;
	constexpr int ColumnRecordSize = DpiSlot + 1;
}
