# BreakpointX Tests

These are the checks we run after changing BreakpointX. They help catch things like a wrong hit count, a broken button, or an Inspector window that leaves Fusion stuck.

If you're new to tests, start with the Inspector checks below. A successful run prints a few PASS messages and ends with "All Inspector tests passed." A failure prints the check and line that need a look.

## Run the Inspector Checks

Build **Release Unicode** in Visual Studio first. The tests use the resources from that MFX.

Open an **x86 Native Tools Command Prompt for Visual Studio**, go to the BreakpointX project folder, and run:

```cmd
Tests\RunInspectorTests.cmd
```

A few Inspector windows will appear and close as the tests press buttons and switch tabs. Let them finish and be patient!

The checks cover window sizes, detached tabs, keyboard controls, saved settings, and screen positioning. They also simulate Fusion callbacks to check nested breakpoints, object selections, separate runtimes, and what happens when an object is destroyed during a pause. Some checks deliberately cause errors to make sure the windows close and their owner becomes usable again.

Registry checks use a temporary key under:

```text
HKEY_CURRENT_USER\Software\BreakpointXInspectorTests
```

Each run gets its own subkey and removes it when it finishes. Your saved Inspector layout stays where it is.

## Run the Trace Checks

From the same command prompt and project folder:

```cmd
if not exist Obj mkdir Obj
cl /nologo /EHsc /W4 /std:c++17 BreakpointXAPIs\Runtime\EventTrace.cpp Tests\EventTraceTests.cpp /Fe:Obj\EventTraceTests.exe /Fo:Obj\
Obj\EventTraceTests.exe
```

These checks feed hits into the recorder and compare the result with what we expect. They cover hit counts, execution order, cycle changes, snapshots, the 10,000-row limit, and shared history between instances.

Keep `NDEBUG` undefined when building these tests. It turns off the assertions that perform the checks.

## Try it Inside Fusion!

The automated checks simulate parts of Fusion. A small MFA is still VERY useful for checking how everything behaves in an actual project.

### Trace Counts & Order

- Hit Event 3 once, Event 8 four times in a row, and Event 19 once. Repeat that in the next cycle. Total hits should go from 1/4/1 to 2/8/2. Hits per cycle should be 1/4/1 in both cycles.
- Try 3 to 8 to 3. You should get three rows, with Event 3 showing totals of 1 and then 2.
- Put hits inside a Fastloop. They should stay in the enclosing Fusion cycle.
- Disable breakpoints and call Break Here using both the Condition and the Action. Those calls should leave the trace unchanged.
- Record more than 10,000 interleaved rows. The oldest rows should drop off, and the view should tell you about the discarded history. Note that a partly retained cycle should be marked. Totals should keep counting.
- Check that cycle headings are bold and the latest cycle is marked as in progress.

### Continue & Next Breakpoint

- Press Continue at Event 3. Later hits in that cycle should still be recorded. The next pause should happen at an enabled breakpoint in a later cycle.
- Those skipped pauses should leave On Breakpoint quiet. The Event-number Expression should still report the latest recorded hit.
- Press Next Breakpoint. The next Break Here should pause, even in the same cycle. Also check its one-shot behavior when breakpoints are disabled.
- Try Disable, Cancel, Escape, and the window's X button.

### Windows & Saved Layout

- Switch between all three tabs with the mouse and F8. Try the arrow keys too.
- Resize each tab. Simplified should reach its compact button layout and stop growing at its full-control size.
- Right-click a tab or press Shift+F10 to open its menu. Detach tabs, return them, and try closing the main window. Each tab should have one home, and a window's final tab should stay attached.
- Open all three views at once. A breakpoint command in any window should end the pause. Closing one window with X should return its tabs to a surviving window. Closing the last window should act like Cancel.
- Toggle the Simplified checkbox. The other views should show or hide their breakpoint buttons immediately and use the available space. F5, F6, F7, and Escape should keep working.
- Move the windows, resize columns, adjust the Object Selection splitter, and scroll the trace. Check titles, tooltips, keyboard focus, and accessible control names.
- Pause again, change Frames, and restart Fusion. Check that tab sizes, positions, selected tabs, and detached windows come back as expected.
- Try monitors with different scaling settings. Disconnect a monitor that had an Inspector window on it and check that the window comes back on an available screen.

Layout preferences are saved under `HKEY_CURRENT_USER\Software\Clickteam\Extensions\BreakpointX`.

### Runtime & Callback Checks

- Add two BreakpointX instances to one Frame. They should share breakpoint state and trace history.
- Try a separate subapplication. Its runtime should have independent state.
- Restart or change the Frame. The new runtime should start with empty history and breakpoints enabled.
- Trigger a nested Break Here from On Breakpoint. Geive it a Condition that stops it from calling itself forever. Check that each Inspector keeps its captured snapshot and that object selections are restored afterward.
- Through a runtime callback, destroy the final BreakpointX instance while paused. The Inspector windows should close safely. Try changing Frames during a pause too.

### Editor Runs & Built Applications

Run Application, Run Frame, and Run Project should allow breakpoints.

Try Build and Run, and a normal built EXE. Break Here should leave tracing, Event-number values, On Breakpoint, and the Inspector untouched. Enable and Toggle should keep that behavior. Actions after a Break Here Condition should still run.

The editor check uses the runtime executable's location and the matching Fusion installation. An unfamiliar host is treated as a standalone app. Please note that Visual Studio's Debug and Release configs don't decide this!
