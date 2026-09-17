@echo off
setlocal
rem build_settings_test.cmd - settings_test.exe, the settings module test.

cd /d "%~dp0"

call "D:\Programs\Microsoft\VC\Auxiliary\Build\vcvars64.bat" >nul
if %errorlevel% neq 0 (
    echo FAILED: vcvars64.bat did not run successfully.
    exit /b 1
)

if not exist ".\obj\" mkdir ".\obj\"

cl.exe /nologo /EHsc /std:c++20 /W4 /bigobj /O2 ^
    /Fo.\obj\ ^
    /I.. ^
    settings_test.cpp ..\nsp_settings.cpp ..\nsp_common.cpp ^
    /Fe:".\settings_test.exe" ^
    /link windowsapp.lib shell32.lib ole32.lib dwmapi.lib user32.lib
if %errorlevel% neq 0 (
    echo FAILED: Compilation or link failed.
    exit /b 1
)

echo SUCCESS: settings_test.exe built.
endlocal
