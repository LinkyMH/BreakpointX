// Standalone Win32 regression tester! Uses the production dialog procedures
// and compiled MFX resources, without loading Fusion or changing its preferences.
#define BREAKPOINTX_TESTING
#include "../BreakpointXAPIs/BreakpointAPI.cpp"
#include "../BreakpointXAPIs/Inspector/Inspector.cpp"
#include <cstdio>
#include <stdexcept>
#include <type_traits>

HINSTANCE hInstLib = NULL;
static std::wstring testKey;

#define CHECK(value) do { if (!(value)) { std::printf("FAIL line %d: %s\n", __LINE__, #value); throw std::runtime_error(#value); } } while (0)

static SIZE WindowSize(HWND window)
{
	RECT rect = {};
	GetWindowRect(window, &rect);
	return { rect.right - rect.left, rect.bottom - rect.top };
}

static bool ControlShown(HWND window, int id)
{
	return (GetWindowLongPtr(InspectorItem(window, id), GWL_STYLE) & WS_VISIBLE) != 0;
}

static DialogContext* FindTab(InspectorSession& session, int tab)
{
	for (auto& window : session.windows)
		if (window && window->window && (window->tabMask & (1 << tab))) return window.get();
	return NULL;
}

static void SelectTab(DialogContext& context, int selected)
{
	for (int i = 0; i < 3 && context.selectedTab != selected; ++i)
		SendMessage(context.window, WM_COMMAND, IDC_INSPECTOR_TABS, 0);
	CHECK(context.selectedTab == selected);
}

static void CheckOwnership(InspectorSession& session, int expectedWindows)
{
	int mask = 0, count = 0;
	for (auto& window : session.windows)
		if (window && window->window)
		{
			CHECK(!(mask & window->tabMask));
			mask |= window->tabMask;
			++count;
			CHECK(window->snapshot == session.snapshot);
			CHECK(window->trace == session.trace);
		}
	CHECK(mask == 7);
	CHECK(count == expectedWindows);
}


static void RuntimeModeTests()
{
	using Linky::BreakpointX::FindEditorInstallation;
	const std::wstring root = L"C:\\Fusion\\";
	const std::wstring extension = root + L"Extensions\\Unicode\\BreakpointX.mfx";
	CHECK(FindEditorInstallation(root + L"Data\\Runtime\\edrt.exe", extension) == root);
	CHECK(FindEditorInstallation(root + L"Data\\Runtime\\Unicode\\edrtex.exe", extension) == root);
	CHECK(FindEditorInstallation(root + L"Data\\Runtime\\Hwa\\Unicode\\edrt.exe", extension) == root);
	CHECK(FindEditorInstallation(L"C:/Fusion/Data/Runtime/Unicode/EDRT.EXE", L"c:/fusion/extensions/unicode/BreakpointX.mfx") == root);
	CHECK(FindEditorInstallation(root + L"Data\\Runtime\\edrt.exe", root + L"Extensions\\BreakpointX.mfx") == root);
	CHECK(FindEditorInstallation(L"C:\\Games\\Game.exe", extension).empty());
	CHECK(FindEditorInstallation(L"C:\\Games\\edrt.exe", extension).empty());
	CHECK(FindEditorInstallation(root + L"Data\\Runtime\\stdrt.exe", extension).empty());
	CHECK(FindEditorInstallation(root + L"Data\\Runtime\\edrt.exe", root + L"Data\\Runtime\\Unicode\\BreakpointX.mfx").empty());
	CHECK(FindEditorInstallation(root + L"Data\\Runtime\\edrt.exe", L"D:\\OtherFusion\\Extensions\\Unicode\\BreakpointX.mfx").empty());
	CHECK(FindEditorInstallation(L"", L"").empty());
	tagRDATA object = {};
	RunHeader runtime = {};
	object.rHo.hoAdRunHeader = &runtime;
	// This tester is a standalone app so enable/next cannot bypass the gate.
	CHECK(!Linky::BreakpointX::IsEditorTestRun(GetModuleHandle(NULL)));

	Linky::BreakpointX::BreakpointAPI::SetEnabled(&object, true);
	CHECK(!Linky::BreakpointX::BreakpointAPI::IsEnabled(&object));
	Linky::BreakpointX::BreakpointAPI::ToggleEnabled(&object);
	CHECK(!Linky::BreakpointX::BreakpointAPI::IsEnabled(&object));


	Linky::BreakpointX::BreakpointAPI::AttachTrace(&object);
	CHECK(!g_eventTraces.Find(&runtime));
	CHECK(!Linky::BreakpointX::BreakpointAPI::Hit(&object));
	CHECK(Linky::BreakpointX::BreakpointAPI::GetLastBreakEvent(&object) == -1);
	// A standalone app should still skip the hit, even with state already set up and Next requested.
	g_eventTraces.Attach(&runtime, &object);
	g_eventTraces.Acquire(&runtime)->breakOnNext = true;
	CHECK(!Linky::BreakpointX::BreakpointAPI::Hit(&object));
	CHECK(g_eventTraces.Find(&runtime)->Capture().rows.empty());
	Linky::BreakpointX::BreakpointAPI::DetachTrace(&object);
	std::puts("PASS editor runtime paths and standalone no-op gate, including Enable and Next");
}

static void RegistryTests()
{
	using namespace Linky::BreakpointX;
	auto defaults = LoadInspectorPreferences(testKey.c_str());
	CHECK(!defaults.showControls && defaults.windows[0].tabs == 7);
	InspectorPreferences preferences;
	preferences.showControls = true;
	preferences.sizes[1] = { 851, 491, 144 };
	preferences.sizes[2] = { 942, 621, 192 };
	preferences.windows[0].tabs = 1;
	preferences.windows[1].tabs = 2;
	preferences.windows[1].selected = 1;
	preferences.windows[1].bounds = { -1500, 30, -649, 521 };
	preferences.windows[2].tabs = 4;
	preferences.windows[2].selected = 2;
	preferences.typeColumn = 151;
	preferences.splitter = 213;
	preferences.objectColumns[0] = 132;
	preferences.traceColumns[0] = 193;
	SaveInspectorPreferences(preferences, testKey.c_str());
	auto loaded = LoadInspectorPreferences(testKey.c_str());
	CHECK(loaded.showControls && loaded.windows[2].tabs == 4);
	CHECK(loaded.sizes[1].width == 851 && loaded.sizes[1].dpi == 144);
	CHECK(loaded.sizes[2].height == 621 && loaded.sizes[2].dpi == 192);
	CHECK(loaded.windows[1].bounds.left == -1500);
	CHECK(loaded.splitter == 213 && loaded.typeColumn == 151);
	CHECK(loaded.objectColumns[0] == 132 && loaded.traceColumns[0] == 193);
	// Save the same tab in two windows. Loading should put all tabs back in the main window.
	preferences.windows[0].tabs = 3;
	SaveInspectorPreferences(preferences, testKey.c_str());
	loaded = LoadInspectorPreferences(testKey.c_str());
	CHECK(loaded.windows[0].tabs == 7 && loaded.windows[1].tabs == 0);
	preferences.sizes[1].dpi = 0;
	SaveInspectorPreferences(preferences, testKey.c_str());
	loaded = LoadInspectorPreferences(testKey.c_str());
	CHECK(loaded.sizes[1].width == 0 && loaded.sizes[1].dpi == 96);
	HKEY key = NULL;
	CHECK(RegOpenKeyExW(HKEY_CURRENT_USER, testKey.c_str(), 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS);
	DWORD bad = 99;
	RegSetValueExW(key, L"ShowControls", 0, REG_DWORD, reinterpret_cast<BYTE*>(&bad), sizeof(bad));
	RegCloseKey(key);
	CHECK(!LoadInspectorPreferences(testKey.c_str()).showControls);
	std::puts("PASS registry defaults, round trip, DPI metadata, invalid ownership and values");
}

static void WindowTests()
{
	InspectorSnapshot snapshot;
	snapshot.eventNumber = 67;
	Linky::BreakpointX::EventTrace trace;
	trace.Record(1, 3);
	trace.Record(1, 8);
	trace.Record(1, 8);
	const auto traceSnapshot = trace.Capture();
	InspectorSession session;
	session.runtime = std::make_shared<Linky::BreakpointX::BreakpointRuntime>();
	session.snapshot = &snapshot;
	session.trace = &traceSnapshot;
	session.registryPath = testKey.c_str();
	auto* main = CreateInspectorWindow(session, session.preferences.windows[0]);
	CHECK(main && main->initialized);
	session.restoring = false;
	wchar_t title[128] = {};
	GetWindowTextW(main->window, title, 128);
	CHECK(std::wstring(title) == L"Event #67 - BreakpointX Inspector");
	CHECK(GetDlgItem(main->page, IDC_INSPECTOR_EVENT) == NULL);
	CHECK(GetWindowLongPtr(main->window, GWL_STYLE) & WS_THICKFRAME);
	CHECK(ControlShown(main->window, IDOK));
	CHECK(ControlShown(main->window, IDC_INSPECTOR_SHOW_CONTROLS));
	const SIZE simple = WindowSize(main->window);
	CHECK(simple.cx > main->simpleSize.cx && simple.cy == main->simpleSize.cy);
	MINMAXINFO limits = {};
	SendMessage(main->window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
	CHECK(limits.ptMinTrackSize.x == main->simpleSize.cx);
	CHECK(limits.ptMinTrackSize.y == main->simpleSize.cy);
	CHECK(limits.ptMaxTrackSize.x == simple.cx && limits.ptMaxTrackSize.y == simple.cy);
	CHECK(!(GetWindowLongPtr(main->window, GWL_STYLE) & WS_MAXIMIZEBOX));
	session.preferences.sizes[0] = { 2000, 1500, InspectorDpi(main->window) };
	ResizeInspectorTab(main->window, *main);
	CHECK(WindowSize(main->window).cx == simple.cx && WindowSize(main->window).cy == simple.cy);
	SetWindowPos(main->window, NULL, 20, 20, limits.ptMinTrackSize.x, limits.ptMinTrackSize.y, SWP_NOZORDER);
	SendMessage(main->window, WM_EXITSIZEMOVE, 0, 0);
	CHECK(main->compactButtons);
	RECT page = {};
	GetClientRect(main->page, &page);
	for (int id : { IDOK, IDC_INSPECTOR_NEXT, IDC_INSPECTOR_DISABLE, IDCANCEL, IDC_INSPECTOR_SHOW_CONTROLS })
	{
		RECT control = {};
		GetWindowRect(InspectorItem(main->window, id), &control);
		MapWindowPoints(NULL, main->page, reinterpret_cast<POINT*>(&control), 2);
		CHECK(control.left >= 0 && control.right <= page.right);
		CHECK(control.top >= 0 && control.bottom <= page.bottom);
	}
	const SIZE compact = WindowSize(main->window);
	CHECK(compact.cx < simple.cx);
	SelectTab(*main, 1);
	CHECK(GetWindowLongPtr(main->window, GWL_STYLE) & WS_THICKFRAME);
	CHECK(!ControlShown(main->window, IDOK));
	SetWindowPos(main->window, NULL, 20, 20, 810, 430, SWP_NOZORDER);
	SendMessage(main->window, WM_EXITSIZEMOVE, 0, 0);
	const SIZE objects = WindowSize(main->window);
	SelectTab(*main, 2);
	SetWindowPos(main->window, NULL, 30, 30, 930, 610, SWP_NOZORDER);
	SendMessage(main->window, WM_EXITSIZEMOVE, 0, 0);
	const SIZE events = WindowSize(main->window);
	SelectTab(*main, 0);
	CHECK(WindowSize(main->window).cy == compact.cy);
	CHECK(WindowSize(main->window).cx == compact.cx);
	SelectTab(*main, 1);
	CHECK(WindowSize(main->window).cx == objects.cx);
	CHECK(WindowSize(main->window).cy == objects.cy);
	SelectTab(*main, 2);
	CHECK(WindowSize(main->window).cx == events.cx);
	CHECK(WindowSize(main->window).cy == events.cy);
	RECT hidden = {}, shown = {};
	GetWindowRect(InspectorItem(main->window, IDC_INSPECTOR_TRACE), &hidden);
	CheckDlgButton(main->page, IDC_INSPECTOR_SHOW_CONTROLS, BST_CHECKED);
	SendMessage(main->window, WM_COMMAND, IDC_INSPECTOR_SHOW_CONTROLS, 0);
	CHECK(ControlShown(main->window, IDOK));
	GetWindowRect(InspectorItem(main->window, IDC_INSPECTOR_TRACE), &shown);
	CHECK(shown.top > hidden.top && shown.bottom == hidden.bottom);
	SelectTab(*main, 1);
	ExecuteInspectorTabCommand(main->window, *main, 1);
	CheckOwnership(session, 2);
	auto* objectWindow = FindTab(session, 1);
	CHECK(objectWindow != main);
	ExecuteInspectorTabCommand(objectWindow->window, *objectWindow, 1);
	CheckOwnership(session, 2);
	SelectTab(*main, 2);
	ExecuteInspectorTabCommand(main->window, *main, 1);
	CheckOwnership(session, 3);
	auto loaded = Linky::BreakpointX::LoadInspectorPreferences(testKey.c_str());
	CHECK(loaded.windows[0].tabs == 1 && loaded.windows[1].tabs && loaded.windows[2].tabs);
	const HWND oldMain = main->window;
	SendMessage(oldMain, WM_CLOSE, 0, 0);
	CHECK(!IsWindow(oldMain) && !session.ending && session.main != oldMain);
	CheckOwnership(session, 2);
	auto* eventWindow = FindTab(session, 2);
	CHECK(eventWindow->window != session.main);
	ExecuteInspectorTabCommand(eventWindow->window, *eventWindow, 2);
	CheckOwnership(session, 1);
	main = FindTab(session, 0);
	CHECK(main->window == session.main);
	SendMessage(main->window, WM_COMMAND, IDOK, 0);
	CHECK(session.ending && session.result == IDOK);
	for (auto& window : session.windows) CHECK(!window || !window->window);
	loaded = Linky::BreakpointX::LoadInspectorPreferences(testKey.c_str());
	CHECK(loaded.windows[0].tabs == 7); // Shutdown did not erase the arrangement.
	CHECK(loaded.showControls);
	std::puts("PASS Native Dialogs, Title, Compact Sizing, Tab Sizes, Controls, Detach/Return, Main-Window Close and Shared Continue");
}

static InspectorSession* pumpSession;
static int pumpWindows;
static void CALLBACK FinishPause(HWND, UINT, UINT_PTR timer, DWORD)
{
	KillTimer(NULL, timer);
	CHECK(pumpSession && !pumpSession->ending);
	CheckOwnership(*pumpSession, pumpWindows);
	auto* window = FindTab(*pumpSession, 2);
	CHECK(window);
	SendMessage(window->window, WM_COMMAND, IDC_INSPECTOR_NEXT, 0);
}

static void PumpTests()
{
	InspectorSnapshot snapshot;
	Linky::BreakpointX::TraceSnapshot trace;
	InspectorSession session;
	session.runtime = std::make_shared<Linky::BreakpointX::BreakpointRuntime>();
	session.snapshot = &snapshot;
	session.trace = &trace;
	session.registryPath = testKey.c_str();
	g_preferencesLoaded = true;
	g_inspectorPreferences = Linky::BreakpointX::InspectorPreferences();
	g_inspectorPreferences.windows[0].tabs = 1;
	g_inspectorPreferences.windows[1].tabs = 2;
	g_inspectorPreferences.windows[1].selected = 1;
	g_inspectorPreferences.windows[2].tabs = 4;
	g_inspectorPreferences.windows[2].selected = 2;
	pumpSession = &session;
	pumpWindows = 3;
	CHECK(SetTimer(NULL, 0, 30, FinishPause) != 0);
	CHECK(RunInspectorSession(session) == IDC_INSPECTOR_NEXT);
	CHECK(session.runtime->breakOnNext);
	CHECK(g_activeInspector == NULL);
	std::puts("PASS message loop restores three windows and resumes once through Next");
}

static void CommandTests()
{
	InspectorSnapshot snapshot;
	snapshot.eventNumber = -1;
	Linky::BreakpointX::EventTrace trace;
	trace.Record(8, 67);
	const auto capture = trace.Capture();
	int runtime = 0, instance = 0;
	g_eventTraces.Attach(&runtime, &instance);
	g_eventTraces.Find(&runtime)->Record(8, 67);
	for (int command : { IDOK, IDC_INSPECTOR_NEXT, IDC_INSPECTOR_DISABLE, IDCANCEL, 0 })
	{
		InspectorSession session;
		session.runtime = std::make_shared<Linky::BreakpointX::BreakpointRuntime>();
		session.snapshot = &snapshot;
		session.trace = &capture;
		session.runtimeIdentity = &runtime;
		session.runtime = g_eventTraces.Acquire(&runtime);
		session.registryPath = testKey.c_str();
		auto* window = CreateInspectorWindow(session, session.preferences.windows[0]);
		CHECK(window);
		session.restoring = false;
		wchar_t title[128] = {};
		GetWindowTextW(window->window, title, 128);
		CHECK(std::wstring(title) == L"Event Unavailable - BreakpointX Inspector");
		session.runtime->enabled = true;
		session.runtime->breakOnNext = false;
		g_eventTraces.Find(&runtime)->BreakNext();
		if (command) SendMessage(window->window, WM_COMMAND, command, 0);
		else SendMessage(window->window, WM_CLOSE, 0, 0);
		CHECK(session.ending);
		CHECK(session.result == (command ? command : IDCANCEL));
		if (command == IDOK)
		{
			CHECK(!g_eventTraces.Find(&runtime)->ShouldPause(8, false));
			CHECK(g_eventTraces.Find(&runtime)->ShouldPause(9, false));
		}
		if (command == IDC_INSPECTOR_NEXT) CHECK(session.runtime->breakOnNext);
		if (command == IDC_INSPECTOR_DISABLE) CHECK(!session.runtime->enabled);
		if (command == IDCANCEL || !command) CHECK(session.runtime->enabled && !session.runtime->breakOnNext);
	}
	g_eventTraces.Detach(&runtime, &instance);
	// Monitor recovery and dimensions saved at a different DPI.
	InspectorSession session;
	session.runtime = std::make_shared<Linky::BreakpointX::BreakpointRuntime>();
	session.snapshot = &snapshot;
	session.trace = &capture;
	session.registryPath = testKey.c_str();
	session.preferences.windows[0].selected = 2;
	session.preferences.windows[0].bounds = { 90000, 90000, 90900, 90600 };
	session.preferences.sizes[2] = { 1200, 800, 192 };
	auto* window = CreateInspectorWindow(session, session.preferences.windows[0]);
	CHECK(window);
	session.restoring = false;
	RECT bounds = {};
	GetWindowRect(window->window, &bounds);
	MONITORINFO monitor = { sizeof(monitor) };
	CHECK(GetMonitorInfo(MonitorFromWindow(window->window, MONITOR_DEFAULTTONEAREST), &monitor));
	CHECK(bounds.left >= monitor.rcWork.left && bounds.top >= monitor.rcWork.top);
	CHECK(bounds.right <= monitor.rcWork.right && bounds.bottom <= monitor.rcWork.bottom);
	const auto size = WindowSize(window->window);
	CHECK(size.cx == std::min<int>(MulDiv(1200, InspectorDpi(window->window), 192), monitor.rcWork.right - monitor.rcWork.left));
	SendMessage(window->window, WM_COMMAND, IDCANCEL, 0);
	std::puts("PASS All Pause Commands, Last-Window Close, Unavailable Title, Off-Screen Recovery and DPI Scaling");
}

static InspectorSession* outerSession;
static InspectorSession* innerSession;
static bool nestedDisabled = false;
static void CALLBACK FinishNested(HWND, UINT, UINT_PTR timer, DWORD)
{
	KillTimer(NULL, timer);
	auto* outer = FindTab(*outerSession, 0);
	auto* inner = FindTab(*innerSession, 0);
	nestedDisabled = outer && inner && !IsWindowEnabled(outer->window);
	if (inner) SendMessage(inner->window, WM_COMMAND, IDCANCEL, 0);
	else PostQuitMessage(9);
}

static void NestedTests()
{
	InspectorSnapshot outerSnapshot, innerSnapshot;
	outerSnapshot.eventNumber = 67;
	innerSnapshot.eventNumber = 99;
	Linky::BreakpointX::TraceSnapshot outerTrace, innerTrace;
	InspectorSession outer;
	outer.snapshot = &outerSnapshot;
	outer.trace = &outerTrace;
	outer.registryPath = testKey.c_str();
	auto* window = CreateInspectorWindow(outer, outer.preferences.windows[0]);
	CHECK(window);
	outer.restoring = false;
	g_activeInspector = &outer;
	InspectorSession inner;
	inner.snapshot = &innerSnapshot;
	inner.trace = &innerTrace;
	inner.registryPath = testKey.c_str();
	g_inspectorPreferences = Linky::BreakpointX::InspectorPreferences();
	outerSession = &outer;
	innerSession = &inner;
	CHECK(SetTimer(NULL, 0, 30, FinishNested));
	CHECK(RunInspectorSession(inner) == IDCANCEL);
	CHECK(nestedDisabled && IsWindowEnabled(window->window));
	CHECK(window->snapshot->eventNumber == 67);
	CHECK(!outer.ending && g_activeInspector == &outer);
	SendMessage(window->window, WM_COMMAND, IDCANCEL, 0);
	g_activeInspector = NULL;
	std::puts("PASS nested pause disables outer windows and preserves outer snapshot and lifetime");
}


using API = Linky::BreakpointX::BreakpointAPI;
static DWORD CALLBACK TestFusionVersion() { return 0x02050000; }
static void (*onGeneratedEvent)(tagRDATA*) = nullptr;
static long WINAPI DispatchTestEvent(headerObject* object, WPARAM eventId, LPARAM)
{
	CHECK(eventId == CND_ONBREAK);
	if (onGeneratedEvent) onGeneratedEvent(reinterpret_cast<tagRDATA*>(object));
	return 0;
}

struct RuntimeFixture
{
	RunHeader runtime = {};
	mv runtimeInfo = {};
	eventGroup event = {};
	objInfoList selection = {};
	std::remove_reference_t<decltype(*runtime.rhObjectList)> objects[2] = {};
	headerObject selected = {};
	tagRDATA breakpoint = {}, sibling = {};

	RuntimeFixture()
	{
		runtimeInfo.mvGetVersion = TestFusionVersion;
		runtime.rh4.rh4Mv = &runtimeInfo;
		runtime.rh4.rh4KpxFunctions[RFUNCTION_GENERATEEVENT].routine = DispatchTestEvent;
		runtime.rhEventGroup = &event;
		runtime.rhLoopCount = 7;
		runtime.rhMaxObjects = 2;
		runtime.rhNumberOi = 1;
		runtime.rhObjectList = objects;
		runtime.rhOiList = &selection;
		runtime.rh2.rh2EventCount = 10;
		runtime.rh4.rh4EventCountOR = 10;
		event.evgInhibit = 67;
		selection.oilOi = 1;
		selection.oilEventCount = 10;
		selection.oilEventCountOR = 10;
		selection.oilListSelected = 0;
		selection.oilNumOfSelected = selection.oilNObjects = 1;
		selected.hoNumber = 0;
		selected.hoCreationId = 1;
		selected.hoNextSelected = -1;
		selected.hoSelectedInOR = 1;
		selected.hoX = 42;
		objects[0].oblOffset = &selected;
		breakpoint.rHo.hoAdRunHeader = sibling.rHo.hoAdRunHeader = &runtime;
		API::AttachTrace(&breakpoint);
	}
	~RuntimeFixture()
	{
		API::DetachTrace(&breakpoint);
		API::DetachTrace(&sibling);
	}
	void CheckRestored() const
	{
		CHECK(selection.oilEventCount == runtime.rh2.rh2EventCount);
		CHECK(selection.oilListSelected == 0 && selection.oilNumOfSelected == 1);
		CHECK(selected.hoNextSelected == -1 && selected.hoSelectedInOR == 1);
	}
};

static void MutateSelection(tagRDATA* object)
{
	auto* runtime = object->rHo.hoAdRunHeader;
	++runtime->rh2.rh2EventCount;
	++runtime->rh4.rh4EventCountOR;
	runtime->rhOiList[0].oilListSelected = -1;
	runtime->rhOiList[0].oilNumOfSelected = 0;
	runtime->rhObjectList[0].oblOffset->hoNextSelected = 1;
	runtime->rhObjectList[0].oblOffset->hoSelectedInOR = 0;
}

static int inspectedPauses = 0;
static tagRDATA* destroyDuringPause = nullptr;
static void FinishHit(InspectorSession& session)
{
	++inspectedPauses;
	CHECK(session.snapshot->eventNumber == session.trace->rows.back().eventNumber);
	CHECK(session.snapshot->filteredObjects.size() == 1);
	CHECK(session.snapshot->filteredObjects[0].selectedInstances.size() == 1);
	CHECK(session.snapshot->filteredObjects[0].selectedInstances[0].x == 42);
	auto* window = FindTab(session, 0);
	CHECK(window);
	SendMessage(window->window, WM_COMMAND, IDCANCEL, 0);
}

static void HitIntegrationTests()
{
	g_testEditorRun = true;
	g_testRegistryPath = testKey.c_str();
	g_preferencesLoaded = true;
	g_inspectorPreferences = Linky::BreakpointX::InspectorPreferences();
	g_testSessionReady = FinishHit;
	Linky::BreakpointX::ScopeExit reset([]() noexcept
		{
			g_testEditorRun = false;
			g_testSessionReady = nullptr;
			g_testRegistryPath = nullptr;
			onGeneratedEvent = nullptr;
		});
	RuntimeFixture first, second;
	API::AttachTrace(&first.sibling);
	CHECK(g_eventTraces.Acquire(&first.runtime) != g_eventTraces.Acquire(&second.runtime));
	API::SetEnabled(&first.breakpoint, false);
	CHECK(!API::IsEnabled(&first.sibling) && API::IsEnabled(&second.breakpoint));
	CHECK(!API::Hit(&first.breakpoint));
	CHECK(g_eventTraces.Find(&first.runtime)->Capture().rows.empty());
	API::SetEnabled(&first.sibling, true);
	onGeneratedEvent = MutateSelection;
	CHECK(API::Hit(&first.breakpoint));
	first.CheckRestored();
	CHECK(API::GetLastBreakEvent(&first.sibling) == 67);
	CHECK(API::GetLastBreakEvent(&second.breakpoint) == -1);

	// Call Hit again from the SDK callback to check a breakpoint inside another breakpoint.
	onGeneratedEvent = [](tagRDATA* object)
		{
			auto* runtime = object->rHo.hoAdRunHeader;
			if (runtime->rhEventGroup->evgInhibit == 67)
			{
				runtime->rhEventGroup->evgInhibit = 99;
				CHECK(API::Hit(object));
				runtime->rhEventGroup->evgInhibit = 67;
			}
			MutateSelection(object);
		};
	const int before = inspectedPauses;
	CHECK(API::Hit(&first.breakpoint));
	CHECK(inspectedPauses == before + 2);
	first.CheckRestored();

	// If the callback throws, put the selection back and catch the error before returning to Fusion.
	onGeneratedEvent = [](tagRDATA* object) { MutateSelection(object); throw std::runtime_error("Callback Failure"); };
	CHECK(!API::Hit(&first.breakpoint));
	first.CheckRestored();
	CHECK(g_activeInspector == nullptr);

	// Throw after the windows open. Check that they close and the selection is put back.
	onGeneratedEvent = MutateSelection;
	g_testSessionReady = [](InspectorSession&) { throw std::bad_alloc(); };
	CHECK(!API::Hit(&first.breakpoint));
	CHECK(g_activeInspector == nullptr);
	first.CheckRestored();
	g_testSessionReady = FinishHit;

	// Remove the instance that hit the breakpoint. Its Inspector should never open.
	onGeneratedEvent = [](tagRDATA* object) { API::DetachTrace(object); };
	const int priorDestroy = inspectedPauses;
	CHECK(!API::Hit(&first.breakpoint));
	CHECK(inspectedPauses == priorDestroy);
	CHECK(g_eventTraces.Find(&first.runtime) != nullptr); // sibling is still attached

	// Remove the last instance. Cleanup should leave the runtime's object lists alone now.
	onGeneratedEvent = [](tagRDATA* object)
		{
			auto* runtime = object->rHo.hoAdRunHeader;
			API::DetachTrace(object);
			object->rHo.hoAdRunHeader = nullptr;
			runtime->rhOiList = nullptr;
			runtime->rhObjectList = nullptr;
		};
	auto retired = g_eventTraces.Acquire(&second.runtime);
	CHECK(!API::Hit(&second.breakpoint));
	CHECK(!retired->active && !g_eventTraces.Find(&second.runtime));
	second.breakpoint.rHo.hoAdRunHeader = &second.runtime;
	API::AttachTrace(&second.breakpoint);
	auto restarted = g_eventTraces.Acquire(&second.runtime);
	CHECK(restarted != retired && restarted->active && restarted->enabled);
	CHECK(restarted->lastEvent == -1 && restarted->trace.Capture().rows.empty());
	// Remove the last instance during a pause. All windows for that pause should close.
	RuntimeFixture paused;
	destroyDuringPause = &paused.breakpoint;
	onGeneratedEvent = nullptr;
	g_testSessionReady = [](InspectorSession& session)
		{
			API::DetachTrace(destroyDuringPause);
			CHECK(session.ending && !session.runtime->active);
			for (const auto& window : session.windows) CHECK(!window || !window->window);
		};
	CHECK(API::Hit(&paused.breakpoint));
	CHECK(g_activeInspector == nullptr);
	std::puts("PASS Enabled Hit, SDK Event Dispatch, Nested Callbacks, Selection Restoration, Runtime Isolation and Teardown");
}

static HWND failedWindow = nullptr;
static void ExceptionCleanupTests()
{
	InspectorSnapshot snapshot;
	Linky::BreakpointX::TraceSnapshot trace;
	InspectorSession outer;
	outer.snapshot = &snapshot;
	outer.trace = &trace;
	outer.registryPath = testKey.c_str();
	auto* window = CreateInspectorWindow(outer, outer.preferences.windows[0]);
	CHECK(window);
	outer.restoring = false;
	g_activeInspector = &outer;
	Linky::BreakpointX::ScopeExit reset([&]() noexcept
		{
			g_testSessionReady = nullptr;
			g_activeInspector = nullptr;
			if (window->window) DestroyWindow(window->window);
		});
	InspectorSession inner;
	inner.snapshot = &snapshot;
	inner.trace = &trace;
	inner.owner = window->window;
	inner.registryPath = testKey.c_str();
	g_inspectorPreferences = Linky::BreakpointX::InspectorPreferences();
	g_testSessionReady = [](InspectorSession& session)
		{
			failedWindow = FindTab(session, 0)->window;
			CHECK(!IsWindowEnabled(session.owner));
			throw std::bad_alloc();
		};
	bool caught = false;
	try { RunInspectorSession(inner); }
	catch (const std::bad_alloc&) { caught = true; }
	CHECK(caught && !IsWindow(failedWindow));
	CHECK(IsWindowEnabled(window->window) && g_activeInspector == &outer);

	// Throw inside the dialog callback. Catch the error before it gets back to Windows.
	InspectorSession callback;
	callback.snapshot = &snapshot;
	callback.trace = &trace;
	callback.owner = window->window;
	callback.registryPath = testKey.c_str();
	g_testSessionReady = [](InspectorSession& session)
		{
			failedWindow = FindTab(session, 0)->window;
			g_testDialogFailure = true;
			SendMessage(failedWindow, WM_COMMAND, IDOK, 0);
		};
	CHECK(RunInspectorSession(callback) == -1);
	CHECK(!IsWindow(failedWindow) && IsWindowEnabled(window->window));
	CHECK(g_activeInspector == &outer);
	std::puts("PASS Exception Cleanup Restores Owners, Nested Sessions and Destroys Windows; Dialog Exceptions Stay Inside Callback");
}

int wmain(int argc, wchar_t** argv)
{
	if (argc != 2) return 2;
	hInstLib = LoadLibraryExW(argv[1], NULL, LOAD_LIBRARY_AS_DATAFILE);
	if (!hInstLib) return 3;
	INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_WIN95_CLASSES };
	if (!InitCommonControlsEx(&controls)) return 4;
	testKey = L"Software\\BreakpointXInspectorTests\\" + std::to_wstring(GetCurrentProcessId());
	int result = 0;
	try
	{
		RuntimeModeTests();
		RegistryTests();
		WindowTests();
		PumpTests();
		CommandTests();
		NestedTests();
		HitIntegrationTests();
		ExceptionCleanupTests();
		std::puts("All Inspector tests passed.");
	}
	catch (const std::exception& e)
	{
		std::printf("Inspector tests failed: %s\n", e.what());
		result = 1;
	}
	RegDeleteTreeW(HKEY_CURRENT_USER, testKey.c_str());
	FreeLibrary(hInstLib);
	return result;
}
