@echo off
echo ControllerMouse (ctrlmouse) - Builder (C++ / MSVC)
echo ==================================================
echo.

rem  compile.bat            -> portable build, runs from anywhere
rem  compile.bat uiaccess   -> adds uiAccess to the manifest, which lets the
rem                            on-screen keyboard draw above the Start menu.
rem                            Windows only grants that to a signed binary in a
rem                            protected folder, so this build is meant to be
rem                            passed to install-uiaccess.ps1 rather than run
rem                            from a Downloads folder.

set OUT=ctrlmouse.exe
set UAC=
if /i "%~1"=="uiaccess" (
    set OUT=ctrlmouse-uiaccess.exe
    set UAC=/MANIFESTUAC:"level='asInvoker' uiAccess='true'"
    echo Building the uiAccess variant - must be signed and installed to
    echo Program Files before it will run. See install-uiaccess.ps1.
    echo.
)

where cl >nul 2>nul
if %errorlevel% neq 0 (
    echo ERROR: MSVC compiler 'cl' not found.
    echo Open a "x64 Native Tools Command Prompt for VS" and run this there,
    echo or run vcvars64.bat first.
    pause
    exit /b 1
)

rc /nologo /fo ctrlmouse.res ctrlmouse.rc
if %errorlevel% neq 0 (
    echo Resource compile FAILED.
    pause
    exit /b 1
)

cl /nologo /EHsc /O2 /W3 /DUNICODE /D_UNICODE ctrlmouse.cpp ctrlmouse.res ^
   /Fe:%OUT% ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED %UAC%

if %errorlevel% neq 0 (
    echo.
    echo Build FAILED.
    pause
    exit /b 1
)

del ctrlmouse.obj ctrlmouse.res >nul 2>nul
echo.
echo ============================================
echo  Build complete: %OUT%
echo.
echo  Launch it: a window opens with the controls.
echo  Close the window to send it to the system tray.
echo  Right-click the tray icon for Show / Quit.
echo ============================================
echo.
pause
