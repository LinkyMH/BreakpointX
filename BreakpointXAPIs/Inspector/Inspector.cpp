#include "../../Common.h"
#include "Inspector.hpp"
#include "../Utilities/ScopeExit.hpp"
#include "InspectorConstants.hpp"
#include "../Runtime/EventTrace.hpp"
#include "InspectorPreferences.hpp"
#include <memory>

#include <commctrl.h>
#include <initguid.h>
#include <oleacc.h>
#include <uxtheme.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace
{
	namespace Inspector = Linky::BreakpointX::Inspector;
	namespace Layout = Inspector::Layout;
	namespace EventNumber = Linky::BreakpointX::EventNumber;
	namespace FusionRuntime = Linky::BreakpointX::FusionRuntime;
	constexpr UINT_PTR ControlSubclassId = 1;
	constexpr int RectanglePointCount = 2;
	constexpr int IconImageCount = 1;
	constexpr int IconImageIndex = 0;
	// Use WM_KEYDOWN's previous-state bit to ignore repeated keys when a key is held down.
	constexpr LPARAM PreviousKeyStateMask = 1L << 30;
	constexpr int DialogFailure = -1;
	constexpr int MessageLoopFailure = -1;
	constexpr int InvalidControlIndex = -1;
	constexpr int AllListItems = -1;
	constexpr int KeyboardContextMenuCoordinate = -1;
	const COLORREF IconMaskColor = RGB(255, 0, 255);

	using Linky::BreakpointX::InspectorSnapshot;
	using Linky::BreakpointX::ObjectSelectionSnapshot;
	using Linky::BreakpointX::InstanceSnapshot;
	// Read a label from resources and copy the text Windows gives us.
	static std::wstring InspectorText(UINT id)
	{
		const wchar_t* text = NULL;
		const int length = LoadStringW(hInstLib, id, reinterpret_cast<LPWSTR>(&text), 0);
		return length > 0 ? std::wstring(text, length) : std::wstring();
	}

	// Work out how much room the formatted text needs before making the buffer.
	template<typename... Args>
	static std::wstring InspectorFormat(UINT id, Args... args)
	{
		const std::wstring format = InspectorText(id);
		const int length = _scwprintf(format.c_str(), args...);
		if (length < 0)
			return std::wstring();
		std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
		swprintf_s(text.data(), text.size(), format.c_str(), args...);
		return std::wstring(text.data());
	}

	// These pointers use data in the snapshot. Keep that snapshot alive until the windows close.
	struct InspectorRow
	{
		const ObjectSelectionSnapshot* object;
		const InstanceSnapshot* instance;
	};

	// Keep the latest layout in memory between pauses, even if saving to the Registry fails.
	Linky::BreakpointX::InspectorPreferences g_inspectorPreferences;
	bool g_preferencesLoaded = false;
	struct InspectorSession;

	// A display row points to an event row. It can show that event or its cycle heading.
	struct TraceDisplayRow { size_t index; bool heading; };
	// The controls and settings for one window, plus pointers to the shared snapshots.
	struct DialogContext
	{
		const InspectorSnapshot* snapshot = NULL;
		const Linky::BreakpointX::TraceSnapshot* trace = NULL;
		const void* runtimeIdentity = NULL;
		int selectedTab = Inspector::Tab::Simplified;
		int tabMask = Inspector::Tab::All;
		HWND window = NULL;
		InspectorSession* session = NULL;
		// Work out the smallest and largest Simplified sizes from the actual button text.
		SIZE simpleSize = {};
		SIZE simpleMaxSize = {};
		bool resizing = false;
		std::vector<TraceDisplayRow> traceRows;
		HFONT traceBoldFont = NULL;
		bool traceScrolled = false;
		std::vector<InspectorRow> rows;
		HWND tooltips = NULL;
		HICON icon = NULL;
		bool initialized = false;
		// Moving controls can send more messages. This stops us from running layout again inside itself.
		bool layingOut = false;
		HWND page = NULL;
		IAccPropServices* accessibility = NULL;
		bool uninitializeCom = false;
		bool compactButtons = false;
		bool dragging = false;
		// Remember the splitter width the user wanted, even if the current window is too small for it.
		int preferredTypeWidth = 0;
		int typeWidth = 0;
		int maxTypeWidth = 0;
		int dragOffset = 0;
		int dragOriginalWidth = 0;
		int paneLeft = 0;
		DWORD error = ERROR_SUCCESS;
	};

	// All the windows for one pause share the same snapshots, runtime state, and result.
	struct InspectorSession
	{
		const InspectorSnapshot* snapshot = NULL;
		const Linky::BreakpointX::TraceSnapshot* trace = NULL;
		const void* runtimeIdentity = NULL;
		HWND owner = NULL;
		HWND main = NULL;
		Linky::BreakpointX::InspectorPreferences preferences;
		std::unique_ptr<DialogContext> windows[Inspector::MaxWindows];
		bool ending = false;
		bool restoring = true;
		INT_PTR result = IDCANCEL;
		DWORD error = ERROR_SUCCESS;
		const wchar_t* registryPath = NULL;
		std::shared_ptr<Linky::BreakpointX::BreakpointRuntime> runtime;
		// Remember the earlier pause when another breakpoint opens inside it.
		InspectorSession* previous = NULL;
	};
	// Mark the pause as failed so its cleanup can close the windows.
	static void FailInspector(DialogContext* context) noexcept
	{
		if (!context || !context->session) return;
		context->session->error = ERROR_NOT_ENOUGH_MEMORY;
		context->session->result = DialogFailure;
		context->session->ending = true;
	}
	InspectorSession* g_activeInspector = NULL;
#ifdef BREAKPOINTX_TESTING
	void (*g_testSessionReady)(InspectorSession&) = nullptr;
	const wchar_t* g_testRegistryPath = nullptr;
	bool g_testDialogFailure = false;
#endif
	const UINT WM_INSPECTOR_SAVE = WM_APP + 81;
	static INT_PTR CALLBACK BreakDialogProc(HWND, UINT, WPARAM, LPARAM);
	static void LayoutInspector(HWND, DialogContext&);
	static void ResizeInspectorTab(HWND, DialogContext&);
	static void SaveSession(InspectorSession&);
	static void CloseInspectorWindow(HWND, DialogContext&);
	static void InspectorTabMenu(HWND, DialogContext&, LPARAM);
	static void ExecuteInspectorTabCommand(HWND, DialogContext&, UINT);
	// Read the tab's saved ID. Its position in the tab strip can change when tabs move to other windows.
	static int CurrentTab(HWND hDlg)
	{
		auto* context = reinterpret_cast<DialogContext*>(GetWindowLongPtr(hDlg, DWLP_USER));
		return context ? context->selectedTab : Inspector::Tab::Simplified;
	}
	// Try the window's own DPI function if Windows has it. Use the older method otherwise.
	static UINT InspectorDpi(HWND window)
	{
		using GetDpi = UINT(WINAPI*)(HWND);
		static auto getDpi = reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
		if (getDpi) return getDpi(window);
		HDC dc = GetDC(window);
		const UINT dpi = GetDeviceCaps(dc, LOGPIXELSX);
		ReleaseDC(window, dc);
		return dpi ? dpi : Inspector::BaseDpi;
	}

	// Convert the width from dialog units to pixels using this dialog's font.
	static int DialogX(HWND hDlg, int value)
	{
		RECT rect = { 0, 0, value, 0 };
		MapDialogRect(hDlg, &rect);
		return rect.right;
	}

	// Convert the height from dialog units to pixels using this dialog's font.
	static int DialogY(HWND hDlg, int value)
	{
		RECT rect = { 0, 0, 0, value };
		MapDialogRect(hDlg, &rect);
		return rect.bottom;
	}

	// Make an icon from the resource bitmap. The mask color marks the transparent parts.
	static HICON CreateInspectorIcon()
	{
		HBITMAP bitmap = reinterpret_cast<HBITMAP>(LoadImageW(hInstLib,
			MAKEINTRESOURCEW(IDB_INSPECTOR_ICON), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
		if (bitmap == NULL)
			return NULL;
		BITMAP info = {};
		GetObject(bitmap, sizeof(info), &info);
		HIMAGELIST images = ImageList_Create(info.bmWidth, info.bmHeight, ILC_COLOR32 | ILC_MASK, IconImageCount, 0);
		HICON icon = NULL;
		if (images != NULL)
		{
			if (ImageList_AddMasked(images, bitmap, IconMaskColor) >= 0)
				icon = ImageList_GetIcon(images, IconImageIndex, ILD_NORMAL);
			ImageList_Destroy(images);
		}
		DeleteObject(bitmap);
		return icon;
	}

	// Look for the control in the outer dialog, then in its child page.
	static HWND InspectorItem(HWND hDlg, int id)
	{
		HWND control = GetDlgItem(hDlg, id);
		auto* context = reinterpret_cast<DialogContext*>(GetWindowLongPtr(hDlg, DWLP_USER));
		return control != NULL ? control : GetDlgItem(context->page, id);
	}

	// Send the page's control messages to the outer dialog, which handles the pause.
	static INT_PTR InspectorPageProcImpl(HWND page, UINT message, WPARAM wParam, LPARAM lParam)
	{
		if (message == WM_COMMAND || message == WM_NOTIFY)
		{
			const LRESULT result = SendMessage(GetParent(page), message, wParam, lParam);
			SetWindowLongPtr(page, DWLP_MSGRESULT, result);
			return TRUE;
		}
		return FALSE;
	}

	// Catch exceptions here befere they can get back into Windows.
	static INT_PTR CALLBACK InspectorPageProc(HWND page, UINT message, WPARAM wParam, LPARAM lParam)
	{
		try { return InspectorPageProcImpl(page, message, wParam, lParam); }
		catch (...) { FailInspector(reinterpret_cast<DialogContext*>(GetWindowLongPtr(GetParent(page), DWLP_USER))); return FALSE; }
	}


	// Save this tab's size and the column widths for tabs in this window.
	static void SaveInspectorState(HWND hDlg, DialogContext& context)
	{
		// Don't save sizes while setting up the window or resizing it from code.
		if (!context.initialized || context.resizing) return;
		auto& saved = context.session->preferences;
		const UINT dpi = InspectorDpi(hDlg);
		if (!IsIconic(hDlg) && !IsZoomed(hDlg))
		{
			RECT rect = {};
			GetWindowRect(hDlg, &rect);
			auto& size = saved.sizes[context.selectedTab];
			size.width = rect.right - rect.left;
			size.height = rect.bottom - rect.top;
			size.dpi = dpi;
		}
		// Each window also has hidden controls for other tabs. Only save widths for tabs in this window.
		if (context.tabMask & Inspector::Tab::Mask(Inspector::Tab::ObjectSelection))
		{
			// Convert table sizes to the base DPI before saving them.
			saved.splitter = MulDiv(context.preferredTypeWidth, Inspector::BaseDpi, dpi);
			saved.typeColumn = MulDiv(ListView_GetColumnWidth(InspectorItem(hDlg, IDC_INSPECTOR_TYPES), Inspector::TypeListColumn), Inspector::BaseDpi, dpi);
			for (int i = 0; i < Inspector::ObjectColumn::Count; ++i)
				saved.objectColumns[i] = MulDiv(ListView_GetColumnWidth(InspectorItem(hDlg, IDC_INSPECTOR_OBJECTS), i), Inspector::BaseDpi, dpi);
		}
		if (context.tabMask & Inspector::Tab::Mask(Inspector::Tab::EventsTrace))
			for (int i = 0; i < Inspector::TraceColumn::Count; ++i)
				saved.traceColumns[i] = MulDiv(ListView_GetColumnWidth(InspectorItem(hDlg, IDC_INSPECTOR_TRACE), i), Inspector::BaseDpi, dpi);
		saved.columnDpi = Inspector::BaseDpi;
	}

	// Adjust saved widths for this monitor's DPI. Keep the startng width if the saved value is zero.
	static void RestoreInspectorState(HWND hDlg, DialogContext& context)
	{
		const auto& saved = context.session->preferences;
		const UINT dpi = InspectorDpi(hDlg);
		if (saved.typeColumn > 0)
		{
			context.preferredTypeWidth = MulDiv(saved.splitter, dpi, saved.columnDpi);
			ListView_SetColumnWidth(InspectorItem(hDlg, IDC_INSPECTOR_TYPES), Inspector::TypeListColumn, MulDiv(saved.typeColumn, dpi, saved.columnDpi));
		}
		for (int i = 0; i < Inspector::ObjectColumn::Count; ++i)
			if (saved.objectColumns[i] > 0)
				ListView_SetColumnWidth(InspectorItem(hDlg, IDC_INSPECTOR_OBJECTS), i, MulDiv(saved.objectColumns[i], dpi, saved.columnDpi));
		for (int i = 0; i < Inspector::TraceColumn::Count; ++i)
			if (saved.traceColumns[i] > 0)
				ListView_SetColumnWidth(InspectorItem(hDlg, IDC_INSPECTOR_TRACE), i, MulDiv(saved.traceColumns[i], dpi, saved.columnDpi));
	}

	// Save the whole window layout while the windows are still open.
	static void SaveSession(InspectorSession& session)
	{
		if (session.restoring || session.ending) return;
		// Clear the unused slots so closed windows don't come back next time. No ghost windows, please.
		for (auto& placement : session.preferences.windows) placement = Linky::BreakpointX::InspectorPlacement();
		int index = 0;
		auto save = [&](DialogContext& context)
			{
				if (!context.window || !context.initialized) return;
				SaveInspectorState(context.window, context);
				auto& placement = session.preferences.windows[index++];
				WINDOWPLACEMENT wp = { sizeof(wp) };
				GetWindowPlacement(context.window, &wp);
				// For minimized or maximized windows, save the position and size they'd have after Restore.
				if (IsIconic(context.window) || IsZoomed(context.window))
					placement.bounds = wp.rcNormalPosition;
				else GetWindowRect(context.window, &placement.bounds);
				placement.dpi = InspectorDpi(context.window);
				placement.tabs = context.tabMask;
				placement.selected = context.selectedTab;
			};
		for (auto& context : session.windows)
			// Save the main window first soi it stays the main window next time.
			if (context && context->window == session.main) save(*context);
		for (auto& context : session.windows)
			if (context && context->window != session.main) save(*context);
		// Save a copy in memory before writing to the Registry. We can still use it if the write fails.
		g_inspectorPreferences = session.preferences;
		Linky::BreakpointX::SaveInspectorPreferences(g_inspectorPreferences, session.registryPath);
	}

	// End the pause once, no matter which window's button was pressed.
	static void CloseInspector(HWND, DialogContext* context, INT_PTR result)
	{
		if (!context || context->session->ending) return;
		auto& session = *context->session;
		SaveSession(session);
		session.ending = true;
		session.result = result;
		for (auto& window : session.windows)
			if (window && window->window) DestroyWindow(window->window);
	}

	// Fill the object table with the checked object types from our saved copy.
	static void RefreshInspectorRows(HWND hDlg, DialogContext& context)
	{
		HWND types = InspectorItem(hDlg, IDC_INSPECTOR_TYPES);
		HWND table = InspectorItem(hDlg, IDC_INSPECTOR_OBJECTS);
		context.rows.clear();
		size_t total = 0;
		for (size_t i = 0; i < context.snapshot->filteredObjects.size(); ++i)
		{
			const auto& object = context.snapshot->filteredObjects[i];
			total += object.selectedInstances.size();
			if (!ListView_GetCheckState(types, static_cast<int>(i)))
				continue;
			for (const auto& instance : object.selectedInstances)
				// These checkboxes only choose what we show. They don't change Fusion's selected objects.
				context.rows.push_back({ &object, &instance });
		}
		ListView_SetItemState(table, AllListItems, 0, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_SetItemCountEx(table, static_cast<int>(context.rows.size()), 0);
		InvalidateRect(table, NULL, TRUE);
		SetWindowTextW(InspectorItem(hDlg, IDC_INSPECTOR_SUMMARY),
			InspectorFormat(IDS_INSPECTOR_SUMMARY, context.rows.size(), total).c_str());
	}

	// Place the controls in the space this window has.
	static void LayoutInspector(HWND hDlg, DialogContext& context)
	{
		if (context.layingOut)
			return;
		context.layingOut = true;
		RECT client = {};
		GetClientRect(hDlg, &client);
		HWND tabs = InspectorItem(hDlg, IDC_INSPECTOR_TABS);
		const bool selection = context.selectedTab == Inspector::Tab::ObjectSelection;
		const bool tracing = context.selectedTab == Inspector::Tab::EventsTrace;
		// Simplified always shows the buttons. Basically other tabs depend on the checkbox setting.
		const bool showControls = context.selectedTab == Inspector::Tab::Simplified || context.session->preferences.showControls;
		const int margin = DialogX(hDlg, Layout::Margin);
		const int gap = DialogX(hDlg, Layout::Gap);
		MoveWindow(tabs, margin, margin, std::max<int>(0, client.right - 2 * margin),
			std::max<int>(0, client.bottom - 2 * margin), TRUE);
		RECT page = {};
		GetClientRect(tabs, &page);
		// Find the area inside the tabs, then get its position relative to the outer dialog.
		TabCtrl_AdjustRect(tabs, FALSE, &page);
		MapWindowPoints(tabs, hDlg, reinterpret_cast<POINT*>(&page), RectanglePointCount);
		SetWindowPos(context.page, HWND_TOP, page.left, page.top, std::max<LONG>(0, page.right - page.left),
			std::max<LONG>(0, page.bottom - page.top), SWP_NOACTIVATE);
		GetClientRect(context.page, &client);
		const int width = std::max<int>(0, client.right - 2 * margin);
		const int buttonHeight = DialogY(hDlg, Layout::ButtonHeight);
		const int buttons[] = { IDOK, IDC_INSPECTOR_NEXT, IDC_INSPECTOR_DISABLE, IDCANCEL };
		const UINT labels[] = { IDS_INSPECTOR_CONTINUE, IDS_INSPECTOR_NEXT,
			IDS_INSPECTOR_DISABLE, IDS_INSPECTOR_CANCEL };
		const UINT symbols[] = { IDS_INSPECTOR_SYMBOL_CONTINUE, IDS_INSPECTOR_SYMBOL_NEXT,
			IDS_INSPECTOR_SYMBOL_DISABLE, IDS_INSPECTOR_SYMBOL_CANCEL };
		// Measure the full button text to see if we need the smaller symbols.
		int buttonWidths[_countof(buttons)] = {};
		int fullWidth = Inspector::ButtonGapCount * gap;
		HDC dc = GetDC(context.page);
		HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessage(hDlg, WM_GETFONT, 0, 0)));
		for (int i = 0; i < _countof(buttons); ++i)
		{
			const std::wstring text = InspectorText(labels[i]);
			SIZE extent = {};
			GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
			buttonWidths[i] = std::max<int>(DialogX(hDlg, Layout::FullButtonMinWidth), extent.cx + DialogX(hDlg, Layout::ButtonTextPadding));
			fullWidth += buttonWidths[i];
		}
		SelectObject(dc, oldFont);
		ReleaseDC(context.page, dc);
		const bool compact = width < fullWidth;
		auto place = [&](int id, int x, int y, int w, int h, bool visible)
			{
				HWND control = InspectorItem(hDlg, id);
				// Move focus before hiding the control so the keyboard still has somewhere to go.
				if (!visible && GetFocus() == control)
					SetFocus(tabs);
				ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
				MoveWindow(control, x, y, std::max<int>(0, w), std::max<int>(0, h), TRUE);
			};
		int x = margin;
		for (int i = 0; i < _countof(buttons); ++i)
		{
			if (compact != context.compactButtons || !context.initialized)
				SetWindowTextW(InspectorItem(hDlg, buttons[i]), InspectorText(compact ? symbols[i] : labels[i]).c_str());
			const int buttonWidth = compact ? std::min<int>(DialogX(hDlg, Layout::CompactButtonMaxWidth), (width - Inspector::ButtonGapCount * gap) / Inspector::BreakpointButtonCount) : buttonWidths[i];
			place(buttons[i], x, margin, buttonWidth, buttonHeight, showControls);
			x += buttonWidth + gap;
		}
		SetWindowTextW(InspectorItem(hDlg, IDC_INSPECTOR_SHOW_CONTROLS),
			InspectorText(compact ? IDS_INSPECTOR_SHOW_CONTROLS_SHORT : IDS_INSPECTOR_SHOW_CONTROLS).c_str());
		context.compactButtons = compact;
		place(IDC_INSPECTOR_SHOW_CONTROLS, margin, margin + buttonHeight + DialogY(hDlg, Layout::Gap),
			width, DialogY(hDlg, Layout::TextLineHeight), context.selectedTab == Inspector::Tab::Simplified);
		// When the buttons are hidden, the table can use their space.
		const int tableY = showControls ? margin + buttonHeight + DialogY(hDlg, Layout::Gap) : margin;
		const int summaryY = client.bottom - margin - DialogY(hDlg, Layout::TextLineHeight);
		const int tableHeight = std::max<int>(0, summaryY - DialogY(hDlg, Layout::Gap) - tableY);
		const int splitterWidth = DialogX(hDlg, Layout::SplitterWidth);
		context.paneLeft = margin;
		context.maxTypeWidth = std::max<int>(0, width - splitterWidth - DialogX(hDlg, Layout::MinObjectPaneWidth));
		if (!selection && context.dragging)
			SendMessage(InspectorItem(hDlg, IDC_INSPECTOR_SPLITTER), WM_CANCELMODE, 0, 0);
		// Use the width that fits now, but keep the user's chosen width for when there's more room.
		context.typeWidth = std::min<int>(context.preferredTypeWidth, context.maxTypeWidth);
		place(IDC_INSPECTOR_TYPES, margin, tableY, context.typeWidth, tableHeight, selection && context.typeWidth > 0);
		place(IDC_INSPECTOR_SPLITTER, margin + context.typeWidth, tableY, splitterWidth, tableHeight, selection);
		place(IDC_INSPECTOR_OBJECTS, margin + context.typeWidth + splitterWidth, tableY,
			width - context.typeWidth - splitterWidth, tableHeight, selection);
		place(IDC_INSPECTOR_SUMMARY, margin, summaryY, width, DialogY(hDlg, Layout::TextLineHeight), selection);
		place(IDC_INSPECTOR_TRACE, margin, tableY, width, tableHeight, tracing);
		place(IDC_INSPECTOR_TRACE_SUMMARY, margin, summaryY, width, DialogY(hDlg, Layout::TextLineHeight), tracing);
		// Scroll to the latest hit when the view first opens. Let the user scroll freely after that.
		if (tracing && !context.traceScrolled && !context.traceRows.empty())
		{
			ListView_EnsureVisible(InspectorItem(hDlg, IDC_INSPECTOR_TRACE),
				static_cast<int>(context.traceRows.size() - 1), FALSE);
			context.traceScrolled = true;
		}
		context.layingOut = false;
		InvalidateRect(context.page, NULL, TRUE);
	}

	// Use the same shortcut keys for the dialog and the controls inside it.
	static int InspectorShortcut(WPARAM key)
	{
		switch (key)
		{
		case VK_F5: return IDOK;
		case VK_F6: return IDC_INSPECTOR_NEXT;
		case VK_F7: return IDC_INSPECTOR_DISABLE;
		case VK_ESCAPE: return IDCANCEL;
		case VK_F8: return IDC_INSPECTOR_TABS;
		default: return 0;
		}
	}

	static LRESULT CALLBACK InspectorControlProc(HWND control, UINT message, WPARAM wParam,
		LPARAM lParam, UINT_PTR subclassId, DWORD_PTR reference);

	// Handle dragging the splitter and using shortcuts while a control has keyboard focus.
	static LRESULT InspectorControlProcImpl(HWND control, UINT message, WPARAM wParam,
		LPARAM lParam, UINT_PTR subclassId, DWORD_PTR reference)
	{
		HWND hDlg = reinterpret_cast<HWND>(reference);
		auto* context = reinterpret_cast<DialogContext*>(GetWindowLongPtr(hDlg, DWLP_USER));
		if (GetDlgCtrlID(control) == IDC_INSPECTOR_SPLITTER && context != NULL)
		{
			switch (message)
			{
			case WM_SETCURSOR:
				SetCursor(LoadCursor(NULL, IDC_SIZEWE));
				return TRUE;
			case WM_GETDLGCODE:
				if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_HOME || wParam == VK_END)
					return DLGC_WANTMESSAGE;
				break;
			case WM_LBUTTONDOWN:
			{
				SetFocus(control);
				// Remember the width before the drag so Escape can put it back.
				context->dragOriginalWidth = context->preferredTypeWidth;
				POINT point = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
				MapWindowPoints(control, context->page, &point, 1);
				context->dragOffset = point.x - context->paneLeft - context->typeWidth;
				context->dragging = true;
				SetCapture(control);
				return 0;
			}
			case WM_MOUSEMOVE:
				if (context->dragging && GetCapture() == control)
				{
					POINT point = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
					MapWindowPoints(control, context->page, &point, 1);
					context->preferredTypeWidth = std::max<int>(0, std::min<int>(context->maxTypeWidth,
						point.x - context->paneLeft - context->dragOffset));
					LayoutInspector(hDlg, *context);
				}
				return 0;
			case WM_LBUTTONUP:
				if (GetCapture() == control)
					ReleaseCapture();
				return 0;
			case WM_CANCELMODE:
				if (context->dragging)
				{
					context->preferredTypeWidth = context->dragOriginalWidth;
					ReleaseCapture();
					LayoutInspector(hDlg, *context);
				}
				return 0;
				// Finish the drag and ask the dialog to save after this message is handled.
			case WM_CAPTURECHANGED:
				context->dragging = false;
				PostMessage(hDlg, WM_INSPECTOR_SAVE, 0, 0);
				return 0;
			case WM_KEYDOWN:
				if (wParam == VK_ESCAPE && context->dragging)
				{
					SendMessage(control, WM_CANCELMODE, 0, 0);
					return 0;
				}
				if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_HOME || wParam == VK_END)
				{
					// Holding Shift moves the splitter in smaller steps.
					const int step = DialogX(hDlg, GetKeyState(VK_SHIFT) < 0 ? Layout::SplitterFineStep : Layout::SplitterStep);
					int width = context->typeWidth + (wParam == VK_LEFT ? -step : step);
					if (wParam == VK_HOME) width = 0;
					if (wParam == VK_END) width = context->maxTypeWidth;
					context->preferredTypeWidth = std::max<int>(0, std::min<int>(context->maxTypeWidth, width));
					LayoutInspector(hDlg, *context);
					PostMessage(hDlg, WM_INSPECTOR_SAVE, 0, 0);
					return 0;
				}
				break;
			case WM_SETFOCUS:
			case WM_KILLFOCUS:
				InvalidateRect(control, NULL, TRUE);
				break;
			case WM_PAINT:
			{
				// Let Windows draw the splitter, then add a rectangle to show keyboard focus.
				const LRESULT result = DefSubclassProc(control, message, wParam, lParam);
				if (GetFocus() == control)
				{
					RECT rect = {};
					GetClientRect(control, &rect);
					HDC dc = GetDC(control);
					DrawFocusRect(dc, &rect);
					ReleaseDC(control, dc);
				}
				return result;
			}
			}
		}
		if (GetDlgCtrlID(control) == IDC_INSPECTOR_TABS && message == WM_CONTEXTMENU)
		{
			SendMessage(hDlg, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(control), lParam);
			return 0;
		}
		// Ask to handle these keys before the dialog uses them for its own keyboard navigation.
		if (message == WM_GETDLGCODE && InspectorShortcut(wParam) != 0)
			return DefSubclassProc(control, message, wParam, lParam) | DLGC_WANTMESSAGE;
		if (message == WM_KEYDOWN && InspectorShortcut(wParam) != 0)
		{
			if ((lParam & PreviousKeyStateMask) == 0)
				SendMessage(hDlg, WM_COMMAND, InspectorShortcut(wParam), 0);
			return 0;
		}
		if (message == WM_NCDESTROY)
			RemoveWindowSubclass(control, InspectorControlProc, subclassId);
		return DefSubclassProc(control, message, wParam, lParam);
	}

	static LRESULT CALLBACK InspectorControlProc(HWND control, UINT message, WPARAM wParam,
		LPARAM lParam, UINT_PTR subclassId, DWORD_PTR reference)
	{
		// Catch exceptions here so the pause can clean up without passing them back into Windows.
		try { return InspectorControlProcImpl(control, message, wParam, lParam, subclassId, reference); }
		catch (...)
		{
			FailInspector(reinterpret_cast<DialogContext*>(GetWindowLongPtr(reinterpret_cast<HWND>(reference), DWLP_USER)));
			return 0;
		}
	}


	// Give each child control the shortcut handler and its outer dialog's window handle.
	static BOOL CALLBACK SubclassInspectorControl(HWND control, LPARAM parameter)
	{
		return SetWindowSubclass(control, InspectorControlProc, ControlSubclassId, static_cast<DWORD_PTR>(parameter));
	}

	// Get the text for one trace table cell from the saved rows.
	static std::wstring TraceCellText(const DialogContext& context, int rowIndex, int column)
	{
		if (rowIndex < 0 || static_cast<size_t>(rowIndex) >= context.traceRows.size())
			return std::wstring();
		const TraceDisplayRow& display = context.traceRows[rowIndex];
		const auto& row = context.trace->rows[display.index];
		// Put the cycle title in the event column. Add notes if it's still running or some rows were removed.
		if (display.heading)
		{
			if (column != Inspector::TraceColumn::Event) return std::wstring();
			std::wstring text = InspectorFormat(IDS_TRACE_CYCLE, static_cast<unsigned long>(row.cycle));
			if (row.cycleSerial == context.trace->currentCycleSerial)
				text += InspectorText(IDS_TRACE_IN_PROGRESS);
			if (display.index == 0 && context.trace->firstCyclePartial)
				text += InspectorText(IDS_TRACE_PARTIAL);
			return text;
		}
		if (column == Inspector::TraceColumn::Event)
			return EventNumber::IsValid(row.eventNumber) ? std::to_wstring(row.eventNumber) : InspectorText(IDS_TRACE_UNAVAILABLE);
		// Leave counts blank when we don't know the event number.
		if (!EventNumber::IsValid(row.eventNumber)) return std::wstring();
		if (column == Inspector::TraceColumn::Hits) return std::to_wstring(row.hits);
		if (column == Inspector::TraceColumn::CycleHits) return std::to_wstring(row.cycleHits);
		return std::wstring();
	}


	// Save the tab we're leaving before resizing for the next one.
	static void RememberInspectorTab(HWND hDlg, DialogContext& context)
	{
		SaveInspectorState(hDlg, context);
		TCITEMW item = {};
		item.mask = TCIF_PARAM;
		TabCtrl_GetItem(InspectorItem(hDlg, IDC_INSPECTOR_TABS),
			TabCtrl_GetCurSel(InspectorItem(hDlg, IDC_INSPECTOR_TABS)), &item);
		// Read the saved tab ID, even if only some of the tabs are in this window.
		context.selectedTab = static_cast<int>(item.lParam);
		ResizeInspectorTab(hDlg, context);
		LayoutInspector(hDlg, context);
		SaveSession(*context.session);
	}

	// Make the trace columns and keep track of which saved row each displayed row uses.
	static bool PopulateTrace(HWND hDlg, DialogContext& context)
	{
		HWND table = InspectorItem(hDlg, IDC_INSPECTOR_TRACE);
		SetWindowTextW(table, InspectorText(IDS_INSPECTOR_TRACE).c_str());
		if (FAILED(context.accessibility->SetHwndPropStr(table, OBJID_CLIENT, CHILDID_SELF,
			PROPID_ACC_NAME, InspectorText(IDS_INSPECTOR_TRACE).c_str())))
			return false;
		ListView_SetExtendedListViewStyle(table, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
			LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
		const UINT titles[] = { IDS_TRACE_EVENT, IDS_TRACE_HITS, IDS_TRACE_CYCLE_HITS };
		const auto& widths = Layout::TraceColumnWidths;
		for (int i = 0; i < Inspector::TraceColumn::Count; ++i)
		{
			std::wstring text = InspectorText(titles[i]);
			LVCOLUMNW column = {};
			column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
			column.pszText = const_cast<LPWSTR>(text.c_str());
			column.cx = DialogX(hDlg, widths[i]);
			column.fmt = LVCFMT_LEFT;
			if (SendMessageW(table, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&column)) == InvalidControlIndex)
				return false;
		}
		// Add a heading when a new cycle starts. Headings don't count toward the recorder's row limit.
		for (size_t i = 0; i < context.trace->rows.size(); ++i)
		{
			if (i == 0 || context.trace->rows[i].cycleSerial != context.trace->rows[i - 1].cycleSerial)
				context.traceRows.push_back({ i, true });
			context.traceRows.push_back({ i, false });
		}
		LOGFONTW font = {};
		if (GetObjectW(reinterpret_cast<HFONT>(SendMessage(hDlg, WM_GETFONT, 0, 0)), sizeof(font), &font))
		{
			// Make a bold font for headings. Event rows keep the normal font.
			font.lfWeight = FW_BOLD;
			context.traceBoldFont = CreateFontIndirectW(&font);
		}
		if (context.traceBoldFont == NULL) return false;
		ListView_SetItemCountEx(table, static_cast<int>(context.traceRows.size()), 0);
		// Show how many old rows were removed after we reached the history limit.
		const std::wstring summary = context.trace->discardedRows != 0
			? InspectorFormat(IDS_TRACE_TRUNCATED, context.trace->rows.size(), context.trace->discardedRows)
			: InspectorFormat(IDS_TRACE_SUMMARY, context.trace->rows.size());
		SetWindowTextW(InspectorItem(hDlg, IDC_INSPECTOR_TRACE_SUMMARY), summary.c_str());
		return true;
	}

	// Make the page, add names for screen readers, and fill it with the saved pause data.
	static bool PopulateInspector(HWND hDlg, DialogContext& context)
	{
		context.page = CreateDialogParamW(hInstLib, MAKEINTRESOURCEW(DB_INSPECTOR_PAGE),
			hDlg, InspectorPageProc, 0);
		if (context.page == NULL)
			return false;
		EnableThemeDialogTexture(context.page, ETDT_ENABLETAB);
		context.preferredTypeWidth = DialogX(hDlg, Layout::DefaultTypePaneWidth);
		// Remember if we initialized COM here so we can call CoUninitialize later.
		const HRESULT comResult = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
		context.uninitializeCom = SUCCEEDED(comResult);
		if (FAILED(CoCreateInstance(CLSID_AccPropServices, NULL, CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&context.accessibility))))
			return false;
		const int buttons[] = { IDOK, IDC_INSPECTOR_NEXT, IDC_INSPECTOR_DISABLE, IDCANCEL };
		const UINT labels[] = { IDS_INSPECTOR_CONTINUE, IDS_INSPECTOR_NEXT, IDS_INSPECTOR_DISABLE, IDS_INSPECTOR_CANCEL };
		for (int i = 0; i < _countof(buttons); ++i)
		{
			if (FAILED(context.accessibility->SetHwndPropStr(InspectorItem(hDlg, buttons[i]),
				OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME, InspectorText(labels[i]).c_str())))
				return false;
		}
		HWND splitter = InspectorItem(hDlg, IDC_INSPECTOR_SPLITTER);
		SetWindowTextW(splitter, InspectorText(IDS_INSPECTOR_SPLITTER).c_str());
		VARIANT role = {};
		role.vt = VT_I4;
		// Tell screen readers that this control is a separator.
		role.lVal = ROLE_SYSTEM_SEPARATOR;
		context.accessibility->SetHwndProp(splitter, OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_ROLE, role);
		const InspectorSnapshot& snapshot = *context.snapshot;
		SetWindowTextW(hDlg, (snapshot.eventNumber >= 0
			? InspectorFormat(IDS_INSPECTOR_EVENT, snapshot.eventNumber)
			: InspectorText(IDS_INSPECTOR_EVENT_UNKNOWN)).c_str());
		HWND tabs = InspectorItem(hDlg, IDC_INSPECTOR_TABS);
		const UINT tabTitles[] = { IDS_INSPECTOR_SIMPLIFIED, IDS_INSPECTOR_SELECTION, IDS_INSPECTOR_TRACE };
		for (int i = 0; i < _countof(tabTitles); ++i)
		{
			if (!(context.tabMask & Inspector::Tab::Mask(i))) continue;
			std::wstring text = InspectorText(tabTitles[i]);
			TCITEMW item = {};
			item.mask = TCIF_TEXT | TCIF_PARAM;
			// Save the tab ID with the item. Its position in the tab strip might change.
			item.lParam = i;
			item.pszText = const_cast<LPWSTR>(text.c_str());
			if (SendMessageW(tabs, TCM_INSERTITEMW, TabCtrl_GetItemCount(tabs), reinterpret_cast<LPARAM>(&item)) == InvalidControlIndex)
				return false;
		}
		HWND types = InspectorItem(hDlg, IDC_INSPECTOR_TYPES);
		HWND table = InspectorItem(hDlg, IDC_INSPECTOR_OBJECTS);
		int selectedIndex = 0;
		for (int i = 0; i < context.selectedTab; ++i) if (context.tabMask & Inspector::Tab::Mask(i)) ++selectedIndex;
		TabCtrl_SetCurSel(tabs, selectedIndex);
		SetWindowTextW(InspectorItem(hDlg, IDC_INSPECTOR_SHOW_CONTROLS), InspectorText(IDS_INSPECTOR_SHOW_CONTROLS).c_str());
		context.accessibility->SetHwndPropStr(InspectorItem(hDlg, IDC_INSPECTOR_SHOW_CONTROLS), OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME, InspectorText(IDS_INSPECTOR_SHOW_CONTROLS).c_str());
		CheckDlgButton(context.page, IDC_INSPECTOR_SHOW_CONTROLS, context.session->preferences.showControls ? BST_CHECKED : BST_UNCHECKED);
		if (!PopulateTrace(hDlg, context)) return false;
		SetWindowTextW(types, InspectorText(IDS_INSPECTOR_TYPES).c_str());
		SetWindowTextW(table, InspectorText(IDS_INSPECTOR_OBJECTS).c_str());
		ListView_SetExtendedListViewStyle(types, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
		ListView_SetExtendedListViewStyle(table, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
		const UINT titles[] = { IDS_INSPECTOR_TYPE, IDS_INSPECTOR_FIXED, IDS_INSPECTOR_X, IDS_INSPECTOR_Y, IDS_INSPECTOR_STATUS };
		const auto& widths = Layout::ObjectColumnWidths;
		for (int i = 0; i < _countof(titles); ++i)
		{
			std::wstring text = InspectorText(titles[i]);
			LVCOLUMNW column = {};
			column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
			column.pszText = const_cast<LPWSTR>(text.c_str());
			column.cx = DialogX(hDlg, widths[i]);
			column.fmt = LVCFMT_LEFT;
			if (SendMessageW(table, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&column)) == InvalidControlIndex)
				return false;
			if (i == Inspector::ObjectColumn::Type && SendMessageW(types, LVM_INSERTCOLUMNW, Inspector::TypeListColumn, reinterpret_cast<LPARAM>(&column)) == InvalidControlIndex)
				return false;
		}
		for (size_t i = 0; i < snapshot.filteredObjects.size(); ++i)
		{
			const auto& object = snapshot.filteredObjects[i];
			std::wstring text = InspectorFormat(IDS_INSPECTOR_TYPE_COUNT, object.name.c_str(),
				object.selectedInstances.size(), object.totalInstances);
			LVITEMW item = {};
			item.mask = LVIF_TEXT;
			item.iItem = static_cast<int>(i);
			item.pszText = const_cast<LPWSTR>(text.c_str());
			if (SendMessageW(types, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)) == InvalidControlIndex)
				return false;
			// Show all the saved object types to start with.
			ListView_SetCheckState(types, item.iItem, TRUE);
		}
		ListView_SetColumnWidth(types, Inspector::TypeListColumn, LVSCW_AUTOSIZE_USEHEADER);
		RefreshInspectorRows(hDlg, context);
		context.icon = CreateInspectorIcon();
		if (context.icon == NULL)
			return false;
		SendMessage(hDlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(context.icon));
		SendMessage(hDlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(context.icon));
		// Use one tooltip window for these controls. The text comes from resources.
		context.tooltips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
			WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
			CW_USEDEFAULT, CW_USEDEFAULT, hDlg, NULL, hInstLib, NULL);
		if (context.tooltips == NULL)
			return false;
		SendMessage(context.tooltips, TTM_SETMAXTIPWIDTH, 0, DialogX(hDlg, Layout::TooltipMaxWidth));
		const int controls[] = { IDOK, IDC_INSPECTOR_NEXT, IDC_INSPECTOR_DISABLE, IDCANCEL,
			IDC_INSPECTOR_TABS, IDC_INSPECTOR_TYPES, IDC_INSPECTOR_OBJECTS, IDC_INSPECTOR_SHOW_CONTROLS, IDC_INSPECTOR_SPLITTER, IDC_INSPECTOR_TRACE };
		const UINT tips[] = { IDS_INSPECTOR_TIP_CONTINUE, IDS_INSPECTOR_TIP_NEXT,
			IDS_INSPECTOR_TIP_DISABLE, IDS_INSPECTOR_TIP_CANCEL, IDS_INSPECTOR_TIP_TABS,
			IDS_INSPECTOR_TIP_TYPES, IDS_INSPECTOR_TIP_OBJECTS, IDS_INSPECTOR_TIP_SHOW_CONTROLS, IDS_INSPECTOR_TIP_SPLITTER, IDS_TRACE_TIP };
		for (int i = 0; i < _countof(controls); ++i)
		{
			// Fusion can run with Visual Themes off. Use the tooltip structure size supported by older common controls too.
			TOOLINFOW tool = { TTTOOLINFOW_V2_SIZE };
			tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
			tool.hwnd = hDlg;
			tool.uId = reinterpret_cast<UINT_PTR>(InspectorItem(hDlg, controls[i]));
			tool.hinst = hInstLib;
			tool.lpszText = MAKEINTRESOURCEW(tips[i]);
			if (!SendMessageW(context.tooltips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)))
				return false;
		}
		return EnumChildWindows(hDlg, SubclassInspectorControl, reinterpret_cast<LPARAM>(hDlg)) != FALSE;
	}

	// Handle messages for this window and the pause it belongs to.
	static INT_PTR BreakDialogProcImpl(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
	{
#ifdef BREAKPOINTX_TESTING
		if (message == WM_COMMAND && std::exchange(g_testDialogFailure, false)) throw std::bad_alloc();
#endif
		auto* context = reinterpret_cast<DialogContext*>(GetWindowLongPtr(hDlg, DWLP_USER));
		switch (message)
		{
			// Save the context pointer before making child controls. They can send messages during setup.
		case WM_INITDIALOG:
			context = reinterpret_cast<DialogContext*>(lParam);
			SetWindowLongPtr(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(context));
			context->window = hDlg;
			SetLastError(ERROR_SUCCESS);
			if (!PopulateInspector(hDlg, *context))
			{
				context->error = GetLastError();
				if (context->error == ERROR_SUCCESS)
					context->error = ERROR_NOT_ENOUGH_MEMORY;
				DestroyWindow(hDlg);
				return TRUE;
			}
			RestoreInspectorState(hDlg, *context);
			ResizeInspectorTab(hDlg, *context);
			LayoutInspector(hDlg, *context);
			context->initialized = true;
			return TRUE;

			// Tell Windows how small and large the window can be, including Simplified's maximum size.
		case WM_GETMINMAXINFO:
		{
			RECT minimum = { 0, 0, std::max<int>(Layout::MinWindowWidthPixels, DialogX(hDlg, Layout::MinWindowWidth)),
				std::max<int>(Layout::MinWindowHeightPixels, DialogY(hDlg, Layout::MinWindowHeight)) };

			AdjustWindowRectEx(&minimum, static_cast<DWORD>(GetWindowLongPtr(hDlg, GWL_STYLE)),
				FALSE, static_cast<DWORD>(GetWindowLongPtr(hDlg, GWL_EXSTYLE)));
			auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
			limits->ptMinTrackSize.x = minimum.right - minimum.left;
			limits->ptMinTrackSize.y = minimum.bottom - minimum.top;
			if (context && context->selectedTab == Inspector::Tab::Simplified && context->simpleSize.cx > 0)
			{
				limits->ptMinTrackSize.x = context->simpleSize.cx;
				limits->ptMinTrackSize.y = context->simpleSize.cy;
				limits->ptMaxTrackSize.x = limits->ptMaxSize.x = context->simpleMaxSize.cx;
				limits->ptMaxTrackSize.y = limits->ptMaxSize.y = context->simpleMaxSize.cy;
			}
			return TRUE;
		}

		case WM_CONTEXTMENU:
			if (context && reinterpret_cast<HWND>(wParam) == InspectorItem(hDlg, IDC_INSPECTOR_TABS))
			{
				InspectorTabMenu(hDlg, *context, lParam);
				return TRUE;
			}
			break;

			// Save after a move or resize finishes, or when a control asks us to save.
		case WM_EXITSIZEMOVE:
		case WM_INSPECTOR_SAVE:
			if (context && context->initialized) SaveSession(*context->session);
			return TRUE;

			// Use the rectangle Windows suggests, then adjust the tab size for the new DPI.
		case WM_DPICHANGED:
			if (context && context->initialized)
			{
				const RECT* rect = reinterpret_cast<const RECT*>(lParam);
				SetWindowPos(hDlg, NULL, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
				ResizeInspectorTab(hDlg, *context);
				LayoutInspector(hDlg, *context);
				SaveSession(*context->session);
			}
			return TRUE;

		case WM_SIZE:
			if (context != NULL && context->initialized && wParam != SIZE_MINIMIZED)
				LayoutInspector(hDlg, *context);
			return TRUE;

		case WM_NOTIFY:
			if (context != NULL && context->initialized)
			{
				auto* notification = reinterpret_cast<NMHDR*>(lParam);
				if (notification->code == HDN_ENDTRACKW || notification->code == HDN_ENDTRACKA)
					PostMessage(hDlg, WM_INSPECTOR_SAVE, 0, 0);
				if (notification->idFrom == IDC_INSPECTOR_TRACE)
				{
					// The ListView asks us for text when it needs it. Get that text from the saved trace.
					if (notification->code == LVN_GETDISPINFOW)
					{
						auto* info = reinterpret_cast<NMLVDISPINFOW*>(lParam);
						if ((info->item.mask & LVIF_TEXT) != 0 && info->item.pszText != NULL && info->item.cchTextMax > 0)
						{
							const std::wstring text = TraceCellText(*context, info->item.iItem, info->item.iSubItem);
							wcsncpy_s(info->item.pszText, info->item.cchTextMax, text.c_str(), _TRUNCATE);
						}
						return TRUE;
					}
					// Draw cycle headings in bold with a different background.
					if (notification->code == NM_CUSTOMDRAW)
					{
						auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
						LRESULT result = CDRF_DODEFAULT;
						if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
							result = CDRF_NOTIFYITEMDRAW;
						else if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT &&
							draw->nmcd.dwItemSpec < context->traceRows.size() &&
							context->traceRows[draw->nmcd.dwItemSpec].heading)
						{
							SelectObject(draw->nmcd.hdc, context->traceBoldFont);
							draw->clrTextBk = GetSysColor(COLOR_BTNFACE);
							result = CDRF_NEWFONT;
						}
						SetWindowLongPtr(hDlg, DWLP_MSGRESULT, result);
						return TRUE;
					}
				}
				if (notification->idFrom == IDC_INSPECTOR_TABS && notification->code == TCN_SELCHANGE)
				{
					RememberInspectorTab(hDlg, *context);
					LayoutInspector(hDlg, *context);
					return TRUE;
				}
				if (notification->idFrom == IDC_INSPECTOR_TYPES && notification->code == LVN_ITEMCHANGED)
				{
					auto* change = reinterpret_cast<NMLISTVIEW*>(lParam);
					if ((change->uChanged & LVIF_STATE) != 0 &&
						((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK) != 0)
						RefreshInspectorRows(hDlg, *context);
					return TRUE;
				}
				if (notification->idFrom == IDC_INSPECTOR_OBJECTS && notification->code == LVN_GETDISPINFOW)
				{
					auto* info = reinterpret_cast<NMLVDISPINFOW*>(lParam);
					if ((info->item.mask & LVIF_TEXT) == 0)
						return TRUE;
					std::wstring text;
					if (info->item.iItem >= 0 && static_cast<size_t>(info->item.iItem) < context->rows.size())
					{
						const auto& row = context->rows[info->item.iItem];
						switch (info->item.iSubItem)
						{
						case Inspector::ObjectColumn::Type: text = row.object->name; break;
						case Inspector::ObjectColumn::FixedValue: text = std::to_wstring(static_cast<int>(row.instance->fixedValue)); break;
						case Inspector::ObjectColumn::X: text = std::to_wstring(row.instance->x); break;
						case Inspector::ObjectColumn::Y: text = std::to_wstring(row.instance->y); break;
						case Inspector::ObjectColumn::Status: text = InspectorText(row.instance->destroyed ? IDS_INSPECTOR_REMOVED : IDS_INSPECTOR_PICKED); break;
						}
					}
					if (info->item.pszText != NULL && info->item.cchTextMax > 0)
						wcsncpy_s(info->item.pszText, info->item.cchTextMax, text.c_str(), _TRUNCATE);
					return TRUE;
				}
			}
			break;

		case WM_KEYDOWN:
			if (InspectorShortcut(wParam) != 0)
			{
				SendMessage(hDlg, WM_COMMAND, InspectorShortcut(wParam), 0);
				return TRUE;
			}
			break;

		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
				// Update the buttons in every window and resize the tables to fit.
			case IDC_INSPECTOR_SHOW_CONTROLS:
				if (context)
				{
					auto& session = *context->session;
					session.preferences.showControls = IsDlgButtonChecked(context->page, IDC_INSPECTOR_SHOW_CONTROLS) == BST_CHECKED;
					for (auto& window : session.windows)
						if (window && window->window)
						{
							CheckDlgButton(window->page, IDC_INSPECTOR_SHOW_CONTROLS, session.preferences.showControls ? BST_CHECKED : BST_UNCHECKED);
							LayoutInspector(window->window, *window);
						}
					SaveSession(session);
				}
				return TRUE;
				// F8 goes through the tabs in this window.
			case IDC_INSPECTOR_TABS:
				if (context != NULL && context->initialized)
				{
					HWND tabs = InspectorItem(hDlg, IDC_INSPECTOR_TABS);
					TabCtrl_SetCurSel(tabs, (TabCtrl_GetCurSel(tabs) + 1) % TabCtrl_GetItemCount(tabs));
					RememberInspectorTab(hDlg, *context);
					LayoutInspector(hDlg, *context);
					SetFocus(tabs);
				}
				return TRUE;
				// Continue skips more pauses in this cycle while still recording the hits.
			case IDOK:
				if (context != NULL && context->trace != NULL && !context->trace->rows.empty())
				{
					auto* trace = context->session->runtime ? &context->session->runtime->trace : nullptr;
					if (trace != NULL) trace->ContinueCycle(context->trace->rows.back().cycle);
				}
				if (context && context->session->runtime) context->session->runtime->breakOnNext = false;
				CloseInspector(hDlg, context, IDOK);
				return TRUE;
				// Stop skipping pauses and make the next Break Here pause once.
			case IDC_INSPECTOR_NEXT:
				if (context != NULL)
				{
					auto* trace = context->session->runtime ? &context->session->runtime->trace : nullptr;
					if (trace != NULL) trace->BreakNext();
				}
				if (context && context->session->runtime) context->session->runtime->breakOnNext = true;
				CloseInspector(hDlg, context, IDC_INSPECTOR_NEXT);
				return TRUE;
				// Disable breakpoints for this runtime and clear Next before closing the pause.
			case IDC_INSPECTOR_DISABLE:
				if (context && context->session->runtime) context->session->runtime->enabled = false;
				if (context && context->session->runtime) context->session->runtime->breakOnNext = false;
				CloseInspector(hDlg, context, IDC_INSPECTOR_DISABLE);
				return TRUE;
				// Cancel closes the pause and leaves the enabled setting alone.
			case IDCANCEL:
				CloseInspector(hDlg, context, IDCANCEL);
				return TRUE;
			}
			break;

			// X closes this window. If another window is open, it gets this window's tabs.
		case WM_CLOSE:
			if (context) CloseInspectorWindow(hDlg, *context);
			return TRUE;

			// Free this window's fonts, COM services, tooltips, and icon.
		case WM_DESTROY:
			if (context != NULL)
			{
				context->window = NULL;
				context->layingOut = true;
				if (context->traceBoldFont != NULL) DeleteObject(context->traceBoldFont);
				if (context->dragging)
					ReleaseCapture();
				if (context->accessibility != NULL)
				{
					const MSAAPROPID properties[] = { PROPID_ACC_NAME, PROPID_ACC_ROLE };
					const int controls[] = { IDOK, IDC_INSPECTOR_NEXT, IDC_INSPECTOR_DISABLE, IDCANCEL, IDC_INSPECTOR_SPLITTER, IDC_INSPECTOR_TRACE, IDC_INSPECTOR_SHOW_CONTROLS };
					for (int id : controls)
						context->accessibility->ClearHwndProps(InspectorItem(hDlg, id), OBJID_CLIENT, CHILDID_SELF,
							properties, _countof(properties));
					context->accessibility->Release();
				}
				if (context->uninitializeCom)
					CoUninitialize();
				if (context->tooltips != NULL)
					DestroyWindow(context->tooltips);
				if (context->icon != NULL)
				{
					SendMessage(hDlg, WM_SETICON, ICON_SMALL, 0);
					SendMessage(hDlg, WM_SETICON, ICON_BIG, 0);
					DestroyIcon(context->icon);
				}
			}
			break;
		}
		return FALSE;
	}

	static INT_PTR CALLBACK BreakDialogProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
	{
		// Catch exceptions here so we can close the pause safely.
		try { return BreakDialogProcImpl(hDlg, message, wParam, lParam); }
		catch (...) { FailInspector(reinterpret_cast<DialogContext*>(GetWindowLongPtr(hDlg, DWLP_USER))); return FALSE; }
	}


	// Measure the small or full controls, then add room for the tabs and window frame.
	static SIZE SimplifiedSize(HWND hDlg, bool compact)
	{
		const int margin = DialogX(hDlg, Layout::Margin), gap = DialogX(hDlg, Layout::Gap);
		const UINT labels[] = { IDS_INSPECTOR_CONTINUE, IDS_INSPECTOR_NEXT, IDS_INSPECTOR_DISABLE, IDS_INSPECTOR_CANCEL };
		HDC dc = GetDC(hDlg);
		HGDIOBJ previous = SelectObject(dc, reinterpret_cast<HFONT>(SendMessage(hDlg, WM_GETFONT, 0, 0)));
		// Put the old font back and release the drawing context, even if formatting text throws.
		Linky::BreakpointX::ScopeExit releaseDc([&]() noexcept
			{
				SelectObject(dc, previous);
				ReleaseDC(hDlg, dc);
			});
		int width = Inspector::ButtonGapCount * gap;
		int compactWidth = DialogX(hDlg, Layout::CompactButtonMinWidth);
		const UINT symbols[] = { IDS_INSPECTOR_SYMBOL_CONTINUE, IDS_INSPECTOR_SYMBOL_NEXT, IDS_INSPECTOR_SYMBOL_DISABLE, IDS_INSPECTOR_SYMBOL_CANCEL };
		for (int i = 0; i < Inspector::BreakpointButtonCount; ++i)
		{
			const auto text = InspectorText(compact ? symbols[i] : labels[i]);
			SIZE extent = {};
			GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &extent);
			width += std::max<int>(DialogX(hDlg, Layout::FullButtonMinWidth), extent.cx + DialogX(hDlg, Layout::ButtonTextPadding));
			compactWidth = std::max<int>(compactWidth, extent.cx + DialogX(hDlg, Layout::CompactButtonTextPadding));
		}
		if (compact) width = Inspector::BreakpointButtonCount * compactWidth + Inspector::ButtonGapCount * gap;
		const auto checkbox = InspectorText(compact ? IDS_INSPECTOR_SHOW_CONTROLS_SHORT : IDS_INSPECTOR_SHOW_CONTROLS);
		SIZE extent = {};
		GetTextExtentPoint32W(dc, checkbox.c_str(), static_cast<int>(checkbox.size()), &extent);
		width = std::max<int>(width, extent.cx + DialogX(hDlg, Layout::CheckboxTextPadding));
		RECT rect = { 0, 0, width + 2 * margin, 2 * margin + DialogY(hDlg, Layout::SimplifiedContentHeight) };
		TabCtrl_AdjustRect(InspectorItem(hDlg, IDC_INSPECTOR_TABS), TRUE, &rect);
		rect.right = rect.right - rect.left + 2 * margin;
		rect.bottom = rect.bottom - rect.top + 2 * margin;
		rect.left = rect.top = 0;
		AdjustWindowRectEx(&rect, static_cast<DWORD>(GetWindowLongPtr(hDlg, GWL_STYLE)), FALSE,
			static_cast<DWORD>(GetWindowLongPtr(hDlg, GWL_EXSTYLE)));
		SIZE result = { rect.right - rect.left, rect.bottom - rect.top };
		return result;
	}

	// Keep the window inside the nearest screen's work area in case its old monitor is gone.
	static void ClampInspector(HWND hDlg, int x, int y, int width, int height)
	{
		RECT bounds = { x, y, x + width, y + height };
		MONITORINFO monitor = { sizeof(monitor) };
		if (GetMonitorInfo(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &monitor))
		{
			width = std::min<int>(width, monitor.rcWork.right - monitor.rcWork.left);
			height = std::min<int>(height, monitor.rcWork.bottom - monitor.rcWork.top);
			x = std::max<int>(monitor.rcWork.left, std::min<int>(x, monitor.rcWork.right - width));
			y = std::max<int>(monitor.rcWork.top, std::min<int>(y, monitor.rcWork.bottom - height));
		}
		SetWindowPos(hDlg, NULL, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	}

	// Use this tab's saved size and keep the window where it is.
	static void ResizeInspectorTab(HWND hDlg, DialogContext& context)
	{
		// Stop saving sizes while we apply the saved one.
		context.resizing = true;
		// Restore the window from maximized before setting this tab's size.
		if (IsZoomed(hDlg)) ShowWindow(hDlg, SW_RESTORE);
		LONG_PTR style = GetWindowLongPtr(hDlg, GWL_STYLE);
		style |= WS_THICKFRAME;
		if (context.selectedTab == Inspector::Tab::Simplified) style &= ~WS_MAXIMIZEBOX;
		else style |= WS_MAXIMIZEBOX;
		SetWindowLongPtr(hDlg, GWL_STYLE, style);
		RECT rect = {};
		GetWindowRect(hDlg, &rect);
		const UINT dpi = InspectorDpi(hDlg);
		const auto& saved = context.session->preferences.sizes[context.selectedTab];
		int width = saved.width ? MulDiv(saved.width, dpi, saved.dpi) : DialogX(hDlg, Layout::DefaultWindowWidth);
		int height = saved.height ? MulDiv(saved.height, dpi, saved.dpi) : DialogY(hDlg, Layout::DefaultWindowHeight);
		if (context.selectedTab == Inspector::Tab::Simplified)
		{
			// Measure Simplified's smallest and largest sizes, then keep the saved size within that range.
			context.simpleSize = SimplifiedSize(hDlg, true);
			const SIZE preferred = context.simpleMaxSize = SimplifiedSize(hDlg, false);
			width = std::max<int>(context.simpleSize.cx, std::min<int>(preferred.cx, saved.width ? width : preferred.cx));
			height = std::max<int>(context.simpleSize.cy, std::min<int>(preferred.cy, saved.height ? height : preferred.cy));
		}
		else
		{
			RECT minimum = { 0, 0, std::max<int>(Layout::MinWindowWidthPixels, DialogX(hDlg, Layout::MinWindowWidth)), std::max<int>(Layout::MinWindowHeightPixels, DialogY(hDlg, Layout::MinWindowHeight)) };
			AdjustWindowRectEx(&minimum, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(GetWindowLongPtr(hDlg, GWL_EXSTYLE)));
			width = std::max<int>(width, minimum.right - minimum.left);
			height = std::max<int>(height, minimum.bottom - minimum.top);
		}
		ClampInspector(hDlg, rect.left, rect.top, width, height);
		context.resizing = false;
	}

	// Make the tab strip again using the tabs that belong to this window.
	static void RebuildInspectorTabs(DialogContext& context)
	{
		HWND tabs = InspectorItem(context.window, IDC_INSPECTOR_TABS);
		TabCtrl_DeleteAllItems(tabs);
		const UINT titles[] = { IDS_INSPECTOR_SIMPLIFIED, IDS_INSPECTOR_SELECTION, IDS_INSPECTOR_TRACE };
		int selectedIndex = 0;
		for (int i = 0; i < Inspector::Tab::Count; ++i)
		{
			if (!(context.tabMask & Inspector::Tab::Mask(i))) continue;
			auto text = InspectorText(titles[i]);
			TCITEMW item = {};
			item.mask = TCIF_TEXT | TCIF_PARAM;
			item.pszText = const_cast<wchar_t*>(text.c_str());
			item.lParam = i;
			const int index = TabCtrl_GetItemCount(tabs);
			TabCtrl_InsertItem(tabs, index, &item);
			if (i == context.selectedTab) selectedIndex = index;
		}
		TabCtrl_SetCurSel(tabs, selectedIndex);
		RestoreInspectorState(context.window, context);
		ResizeInspectorTab(context.window, context);
		LayoutInspector(context.window, context);
		SetFocus(tabs);
	}

	// Find a free window slot and create a window using the pause's shared snapshots.
	static DialogContext* CreateInspectorWindow(InspectorSession& session, const Linky::BreakpointX::InspectorPlacement& placement)
	{
		std::unique_ptr<DialogContext>* slot = NULL;
		for (auto& candidate : session.windows)
			if (!candidate || !candidate->window) { slot = &candidate; break; }
		if (!slot) return NULL;
		slot->reset(new DialogContext());
		DialogContext& context = **slot;
		context.session = &session;
		// All detached windows show the same saved data.
		context.snapshot = session.snapshot;
		context.trace = session.trace;
		context.runtimeIdentity = session.runtimeIdentity;
		context.tabMask = placement.tabs;
		context.selectedTab = placement.selected;
		HWND window = CreateDialogParamW(hInstLib, MAKEINTRESOURCEW(DB_INSPECTOR), session.owner,
			BreakDialogProc, reinterpret_cast<LPARAM>(&context));
		// Close the unfinished window if setup failed.
		if (!window || !context.initialized)
		{
			session.error = context.error ? context.error : GetLastError();
			if (context.window) DestroyWindow(context.window);
			return NULL;
		}
		if (!session.main) session.main = window;
		if (placement.bounds.right > placement.bounds.left)
		{
			RECT rect = {};
			GetWindowRect(window, &rect);
			ClampInspector(window, placement.bounds.left, placement.bounds.top, rect.right - rect.left, rect.bottom - rect.top);
			ResizeInspectorTab(window, context);
		}
		ShowWindow(window, SW_SHOW);
		return &context;
	}

	// Move this window's tabs to another open window. If it's the last window, cancel the pause.
	static void CloseInspectorWindow(HWND hDlg, DialogContext& context)
	{
		auto& session = *context.session;
		DialogContext* destination = NULL;
		for (auto& other : session.windows)
			if (other && other->window && other->window != hDlg)
			{
				if (!destination || other->window == session.main) destination = other.get();
			}
		if (!destination)
		{
			CloseInspector(hDlg, &context, IDCANCEL);
			return;
		}
		SaveSession(session);
		// Move the tabs before closing their old window.
		destination->tabMask |= context.tabMask;
		// Make the remaining window the main one if we're closing the main window.
		if (hDlg == session.main) session.main = destination->window;
		DestroyWindow(hDlg);
		RebuildInspectorTabs(*destination);
		SaveSession(session);
		SetForegroundWindow(destination->window);
	}

	// Use the clicked tab for the mouse, or the selected tab when opening the menu with the keyboard.
	static void InspectorTabMenu(HWND hDlg, DialogContext& context, LPARAM location)
	{
		HWND tabs = InspectorItem(hDlg, IDC_INSPECTOR_TABS);
		POINT point = { static_cast<short>(LOWORD(location)), static_cast<short>(HIWORD(location)) };
		if (point.x == KeyboardContextMenuCoordinate && point.y == KeyboardContextMenuCoordinate)
		{
			RECT rect = {};
			TabCtrl_GetItemRect(tabs, TabCtrl_GetCurSel(tabs), &rect);
			point.x = rect.left;
			point.y = rect.bottom;
			ClientToScreen(tabs, &point);
		}
		else
		{
			TCHITTESTINFO hit = {};
			hit.pt = point;
			ScreenToClient(tabs, &hit.pt);
			const int index = TabCtrl_HitTest(tabs, &hit);
			if (index < 0) return;
			if (index != TabCtrl_GetCurSel(tabs))
			{
				TabCtrl_SetCurSel(tabs, index);
				RememberInspectorTab(hDlg, context);
			}
		}
		HMENU menu = CreatePopupMenu();
		if (!menu) return;
		// Free the menu even if making its labels throws.
		Linky::BreakpointX::ScopeExit releaseMenu([&]() noexcept { DestroyMenu(menu); });
		AppendMenuW(menu, MF_STRING | (TabCtrl_GetItemCount(tabs) > 1 ? 0 : MF_GRAYED), Inspector::Detach, InspectorText(IDS_INSPECTOR_DETACH).c_str());
		AppendMenuW(menu, MF_STRING | (hDlg != context.session->main ? 0 : MF_GRAYED), Inspector::ReturnToMain, InspectorText(IDS_INSPECTOR_REATTACH).c_str());
		const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, hDlg, NULL);
		if (!command) return;
		ExecuteInspectorTabCommand(hDlg, context, command);
	}

	// Move tabs between windows in the same pause.
	static void ExecuteInspectorTabCommand(HWND hDlg, DialogContext& context, UINT command)
	{
		HWND tabs = InspectorItem(hDlg, IDC_INSPECTOR_TABS);
		auto& session = *context.session;
		SaveSession(session);
		if (command == Inspector::Detach && TabCtrl_GetItemCount(tabs) > 1)
		{
			Linky::BreakpointX::InspectorPlacement placement;
			placement.tabs = Inspector::Tab::Mask(context.selectedTab);
			placement.selected = context.selectedTab;
			GetWindowRect(hDlg, &placement.bounds);
			OffsetRect(&placement.bounds, DialogX(hDlg, Layout::DetachedWindowOffset), DialogY(hDlg, Layout::DetachedWindowOffset));
			// Make the new window first. If that fails, the tab stays where it was.
			DialogContext* detached = CreateInspectorWindow(session, placement);
			if (!detached) return;
			context.tabMask &= ~placement.tabs;
			for (int i = 0; i < Inspector::Tab::Count; ++i) if (context.tabMask & Inspector::Tab::Mask(i)) { context.selectedTab = i; break; }
			RebuildInspectorTabs(context);
			SetForegroundWindow(detached->window);
			SetFocus(InspectorItem(detached->window, IDC_INSPECTOR_TABS));
		}
		else if (command == Inspector::ReturnToMain && hDlg != session.main)
		{
			for (auto& main : session.windows)
				if (main && main->window == session.main)
				{
					main->tabMask |= Inspector::Tab::Mask(context.selectedTab);
					main->selectedTab = context.selectedTab;
					context.tabMask &= ~(Inspector::Tab::Mask(context.selectedTab));
					// Close the detached window when its last tab goes back to the main window.
					if (!context.tabMask) DestroyWindow(hDlg);
					else
					{
						for (int i = 0; i < Inspector::Tab::Count; ++i) if (context.tabMask & Inspector::Tab::Mask(i)) { context.selectedTab = i; break; }
						RebuildInspectorTabs(context);
					}
					RebuildInspectorTabs(*main);
					SetForegroundWindow(main->window);
					break;
				}
		}
		SaveSession(session);
	}

	// Wait here while the Inspector windows handle messages, keeping the calling event paused.
	static INT_PTR RunInspectorSession(InspectorSession& session)
	{
		if (!g_preferencesLoaded)
		{
			g_inspectorPreferences = Linky::BreakpointX::LoadInspectorPreferences();
			g_preferencesLoaded = true;
		}
		session.preferences = g_inspectorPreferences;
		// Remember earlier pauses so we can disable their windows now and enable them again later.
		session.previous = g_activeInspector;
		std::vector<HWND> disabledInspectors;
		const bool enableOwner = session.owner && IsWindowEnabled(session.owner);
		// Set up cleanup before disabling windows. It also runs if something throws.
		Linky::BreakpointX::ScopeExit cleanup([&]() noexcept
			{
				session.ending = true;
				for (auto& window : session.windows)
					if (window && window->window) DestroyWindow(window->window);
				if (enableOwner && IsWindow(session.owner))
				{
					EnableWindow(session.owner, TRUE);
					SetActiveWindow(session.owner);
				}
				for (HWND window : disabledInspectors)
					if (IsWindow(window)) EnableWindow(window, TRUE);
				g_activeInspector = session.previous;
			});
		g_activeInspector = &session;
		for (auto* previous = session.previous; previous; previous = previous->previous)
			for (auto& window : previous->windows)
				if (window && window->window && IsWindowEnabled(window->window))
				{
					// Remember which windows were enabled before we disable them, so we can put them back as they were.
					disabledInspectors.push_back(window->window);
					EnableWindow(window->window, FALSE);
				}
		if (enableOwner) EnableWindow(session.owner, FALSE);
		// Copy the saved layout before creating its windows.
		const auto placements = session.preferences;
		for (const auto& placement : placements.windows)
			if (placement.tabs && !CreateInspectorWindow(session, placement))
			{
				session.result = DialogFailure;
				session.ending = true;
				break;
			}
		session.restoring = false;
#ifdef BREAKPOINTX_TESTING
		if (g_testSessionReady) g_testSessionReady(session);
#endif
		MSG message = {};
		bool quit = false;
		while (!session.ending)
		{
			// Wait for a Windows message. Stop the pause if we get Quit or an error.
			const BOOL status = GetMessage(&message, NULL, 0, 0);
			if (status <= 0)
			{
				quit = status == 0;
				if (status == MessageLoopFailure) { session.error = GetLastError(); session.result = DialogFailure; }
				break;
			}
			bool handled = false;
			for (auto& window : session.windows)
				if (window && window->window && (message.hwnd == window->window || IsChild(window->window, message.hwnd)))
				{
					// Let the dialog handle keyboard navigation first.
					handled = IsDialogMessage(window->window, &message) != FALSE;
					break;
				}
			if (!handled) { TranslateMessage(&message); DispatchMessage(&message); }
			// Stop if Fusion's owner window was closed while we were handling a message.
			if (session.owner && !IsWindow(session.owner)) break;
		}
		if (!session.ending) SaveSession(session);

		// Send Quit back out so the loop that called us can handle it too.
		if (quit) PostQuitMessage(static_cast<int>(message.wParam));
		return session.result;
	}


}

namespace Linky::BreakpointX
{
	// Keep this session around until all of its windows have finished.
	bool ShowInspector(const InspectorSnapshot& snapshot, const TraceSnapshot& trace,
		const std::shared_ptr<BreakpointRuntime>& runtime, const void* identity, HWND owner)
	{
		InspectorSession session;
		session.snapshot = &snapshot;
		session.trace = &trace;
		session.runtime = runtime;
		session.runtimeIdentity = identity;
		session.owner = owner;
#ifdef BREAKPOINTX_TESTING
		session.registryPath = g_testRegistryPath;
#endif
		INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_WIN95_CLASSES };
		const auto result = InitCommonControlsEx(&controls) ? RunInspectorSession(session) : DialogFailure;
		if (result == DialogFailure)
		{
			const DWORD error = session.error ? session.error : GetLastError();
			MessageBoxW(owner, InspectorFormat(IDS_INSPECTOR_ERROR, error).c_str(),
				InspectorText(IDS_INSPECTOR_TITLE).c_str(), MB_OK | MB_ICONERROR);
			return false;
		}
		return true;
	}

	// Check earlier nested pauses too. Close every pause for the runtime that's ending.
	void CloseRuntimeInspectors(const void* identity) noexcept
	{
		for (auto* session = g_activeInspector; session; session = session->previous)
			if (session->runtimeIdentity == identity)
			{
				// Try to save the layout. We still need to close the windows if saving throws.
				try { SaveSession(*session); }
				catch (...) {}
				session->ending = true;
				for (auto& window : session->windows)
					if (window && window->window) DestroyWindow(window->window);
			}
	}
}
