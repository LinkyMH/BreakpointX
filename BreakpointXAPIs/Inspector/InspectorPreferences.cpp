#include "../../Common.h"
#include "InspectorPreferences.hpp"

namespace Linky::BreakpointX
{
	namespace
	{
		namespace Schema = Inspector::Preferences;
		const wchar_t* const RegistryPath = L"Software\\Clickteam\\Extensions\\BreakpointX";

		// Check the saved value's type and size before using it.
		bool ReadValue(HKEY key, const wchar_t* name, void* value, DWORD size, DWORD expected)
		{
			DWORD type = 0, actual = size;
			return RegQueryValueExW(key, name, NULL, &type, static_cast<BYTE*>(value), &actual) == ERROR_SUCCESS
				&& type == expected && actual == size;
		}

		// Check the saved DPI before using it to resize anything.
		bool ValidDpi(UINT dpi) { return dpi >= Schema::MinDpi && dpi <= Schema::MaxDpi; }
		// Zero lets us use the default. The size limit catches values that are way too big.
		bool ValidDimension(LONG value) { return value >= 0 && value <= Schema::MaxDimension; }
		// Check the window position, size, tabs, and selected tab before loading them.
		bool ValidPlacement(const InspectorPlacement& value)
		{
			return ValidDpi(value.dpi) && value.tabs >= 0 && value.tabs <= Inspector::Tab::All
				&& Inspector::Tab::IsValid(value.selected)
				&& (!value.tabs || (value.tabs & Inspector::Tab::Mask(value.selected)))
				&& value.bounds.left >= Schema::MinPosition && value.bounds.left <= Schema::MaxPosition
				&& value.bounds.top >= Schema::MinPosition && value.bounds.top <= Schema::MaxPosition
				&& value.bounds.right >= Schema::MinPosition && value.bounds.right <= Schema::MaxExtent
				&& value.bounds.bottom >= Schema::MinPosition && value.bounds.bottom <= Schema::MaxExtent
				&& ValidDimension(value.bounds.right - value.bounds.left)
				&& ValidDimension(value.bounds.bottom - value.bounds.top);
		}
	}

	// Start with defaults. Replace a setting when we find a valid saved value.
	InspectorPreferences LoadInspectorPreferences(const wchar_t* registryPath)
	{
		InspectorPreferences result;
		HKEY key = NULL;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, registryPath ? registryPath : RegistryPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
			return result;
		// Check the version so we know how to read the saved data.
		DWORD version = 0;
		if (!ReadValue(key, L"LayoutVersion", &version, sizeof(version), REG_DWORD) || version != Schema::LayoutVersion)
		{
			RegCloseKey(key);
			return result;
		}
		DWORD controls = 0;
		if (ReadValue(key, L"ShowControls", &controls, sizeof(controls), REG_DWORD) && controls <= TRUE)
			result.showControls = controls != 0;
		// Each tab saves its own size and the DPI it used.
		for (int i = 0; i < Inspector::Tab::Count; ++i)
		{
			wchar_t name[Schema::ValueNameCapacity];
			swprintf_s(name, L"Tab%dSize", i);
			InspectorTabSize size;
			if (ReadValue(key, name, &size, sizeof(size), REG_BINARY) && ValidDpi(size.dpi)
				&& ValidDimension(size.width) && ValidDimension(size.height))
				result.sizes[i] = size;
		}
		// Check all the windows together. Every tab should belong to exactly one window.
		InspectorPlacement windows[Inspector::MaxWindows];
		// Keep track of tab bits so we can spot a tab assigned to two windows.
		int used = 0;
		bool valid = ReadValue(key, L"Windows", windows, sizeof(windows), REG_BINARY);
		for (const auto& window : windows)
		{
			if (!ValidPlacement(window) || (used & window.tabs)) valid = false;
			used |= window.tabs;
		}
		if (valid && used == Inspector::Tab::All && windows[Inspector::MainWindowIndex].tabs)
			for (int i = 0; i < Inspector::MaxWindows; ++i) result.windows[i] = windows[i];
		// This part saves sizes only. Object filters and runtime addresses aren't saved here.
		// Keep the saved values in this order so we can still read older layouts.
		int columns[Schema::ColumnRecordSize] = {};
		if (ReadValue(key, L"Columns", columns, sizeof(columns), REG_BINARY))
		{
			bool validColumns = ValidDpi(static_cast<UINT>(columns[Schema::DpiSlot]));
			for (int i = 0; i < Schema::DimensionSlotCount; ++i)
				if (!ValidDimension(columns[i])) validColumns = false;
			if (validColumns)
			{
				result.splitter = columns[Schema::SplitterSlot];
				result.typeColumn = columns[Schema::TypeColumnSlot];
				for (int i = 0; i < Inspector::ObjectColumn::Count; ++i) result.objectColumns[i] = columns[i + Schema::ObjectColumnsStart];
				// All the table widths use the same saved DPI.
				result.columnDpi = columns[Schema::DpiSlot];
			}
		}
		// The trace and object columns use the same saved DPI.
		int trace[Inspector::TraceColumn::Count] = {};
		if (ReadValue(key, L"TraceColumns", trace, sizeof(trace), REG_BINARY))
		{
			for (int i = 0; i < Inspector::TraceColumn::Count; ++i)
				if (ValidDimension(trace[i])) result.traceColumns[i] = trace[i];
		}
		RegCloseKey(key);
		return result;
	}

	// Create the Registry key if it's missing. If that fails, we can still use the settings in memory.
	void SaveInspectorPreferences(const InspectorPreferences& value, const wchar_t* registryPath)
	{
		HKEY key = NULL;
		if (RegCreateKeyExW(HKEY_CURRENT_USER, registryPath ? registryPath : RegistryPath, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
			return;
		auto write = [&](const wchar_t* name, const void* data, DWORD size, DWORD type)
			{
				RegSetValueExW(key, name, 0, type, static_cast<const BYTE*>(data), size);
			};
		// Save the version and checkbox setting, then the window layout.
		const DWORD version = Schema::LayoutVersion, controls = value.showControls ? TRUE : FALSE;
		write(L"LayoutVersion", &version, sizeof(version), REG_DWORD);
		write(L"ShowControls", &controls, sizeof(controls), REG_DWORD);
		for (int i = 0; i < Inspector::Tab::Count; ++i)
		{
			wchar_t name[Schema::ValueNameCapacity];
			swprintf_s(name, L"Tab%dSize", i);
			write(name, &value.sizes[i], sizeof(value.sizes[i]), REG_BINARY);
		}
		write(L"Windows", value.windows, sizeof(value.windows), REG_BINARY);
		int columns[Schema::ColumnRecordSize] = { value.splitter, value.typeColumn };
		for (int i = 0; i < Inspector::ObjectColumn::Count; ++i) columns[i + Schema::ObjectColumnsStart] = value.objectColumns[i];
		columns[Schema::DpiSlot] = value.columnDpi;
		write(L"Columns", columns, sizeof(columns), REG_BINARY);
		write(L"TraceColumns", value.traceColumns, sizeof(value.traceColumns), REG_BINARY);
		RegCloseKey(key);
	}
} // namespace Linky::BreakpointX
