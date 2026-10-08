@echo off
setlocal
pushd "%~dp0.."
if not exist "Obj\InspectorTests" mkdir "Obj\InspectorTests"
cl /nologo /EHsc /W3 /std:c++17 /MT /Zc:wchar_t- /DWIN32 /D_WINDOWS /DSTRICT /DUNICODE /D_UNICODE /DHWABETA /I"..\..\Inc" Tests\InspectorTests.cpp BreakpointXAPIs\Inspector\InspectorPreferences.cpp BreakpointXAPIs\Runtime\EventTrace.cpp BreakpointXAPIs\Runtime\RuntimeMode.cpp /Fo:Obj\InspectorTests\ /Fe:Obj\InspectorTests\InspectorTests.exe /link user32.lib gdi32.lib advapi32.lib
if errorlevel 1 goto failed
Obj\InspectorTests\InspectorTests.exe "%CD%\Obj\Unicode\Release\BreakpointX.mfx"
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
