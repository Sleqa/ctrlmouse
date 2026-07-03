@echo off
echo ControllerMouse (ctrlmouse) - Builder (C++ / MSVC)
echo ==================================================
echo.

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
   /Fe:ctrlmouse.exe ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED

if %errorlevel% neq 0 (
    echo.
    echo Build FAILED.
    pause
    exit /b 1
)

del ctrlmouse.obj ctrlmouse.res >nul 2>nul
echo.
echo ============================================
echo  Build complete: ctrlmouse.exe
echo.
echo  Launch it: a window opens with the controls.
echo  Close the window to send it to the system tray.
echo  Right-click the tray icon for Show / Quit.
echo ============================================
echo.
pause
