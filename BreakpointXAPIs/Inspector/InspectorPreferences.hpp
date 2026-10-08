#pragma once
#include "InspectorConstants.hpp"
#include <windows.h>

namespace Linky::BreakpointX
{
	/// @brief A tab's saved window size and the DPI it used.
	/// A size of zero lets the Inspector choose its default.
	struct InspectorTabSize
	{
		LONG width = 0;
		LONG height = 0;
		UINT dpi = Inspector::BaseDpi;
	};

	/// @brief A window's saved position, size, tabs, and selected tab.
	struct InspectorPlacement
	{
		RECT bounds = {};
		UINT dpi = Inspector::BaseDpi;
		// One bit for each tab. Zero means this window slot isn't used.
		int tabs = 0;
		int selected = Inspector::Tab::Simplified;
	};

	/// @brief The layout settings we keep between pauses and save in the Registry.
	/// We save these structs as bytes, so changing their layout also means changing the saved format.
	struct InspectorPreferences
	{
		InspectorTabSize sizes[Inspector::Tab::Count];
		InspectorPlacement windows[Inspector::MaxWindows];
		bool showControls = false;
		int splitter = 0;
		int typeColumn = 0;
		int objectColumns[Inspector::ObjectColumn::Count] = {};
		int traceColumns[Inspector::TraceColumn::Count] = {};
		// The table widths and splitter use this DPI.
		UINT columnDpi = Inspector::BaseDpi;

		// Start with all three tabs in the main window.
		InspectorPreferences() { windows[Inspector::MainWindowIndex].tabs = Inspector::Tab::All; }
	};

	/// @brief Load the settings we can read. Use defaults for missing or bad values.
	/// @param registryPath The key path under HKCU. Pass null to use Software\Clickteam\Extensions\BreakpointX.
	InspectorPreferences LoadInspectorPreferences(const wchar_t* registryPath = NULL);

	/// @brief Save the layout and create the HKCU key if we need it.
	/// If a Registry write fails, we still have the current settings in memory.
	/// @param preferences The layout settings to write to the Registry.
	/// @param registryPath The key path under HKCU. Pass null to use Software\Clickteam\Extensions\BreakpointX.
	void SaveInspectorPreferences(const InspectorPreferences& preferences, const wchar_t* registryPath = NULL);
} // namespace Linky::BreakpointX
