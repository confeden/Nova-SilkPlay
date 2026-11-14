@echo off
setlocal
cd /d "%~dp0"
call "D:\Programs\Microsoft\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist ".\obj\" mkdir ".\obj\"
cl.exe /nologo /EHsc /std:c++20 /W4 /O2 /utf-8 /Fo.\obj\ /I.. ^
    autostart_test.cpp ..\nsp_common.cpp /Fe:".\autostart_test.exe" ^
    /link windowsapp.lib shell32.lib ole32.lib dwmapi.lib user32.lib advapi32.lib
if errorlevel 1 exit /b 1
echo SUCCESS: autostart_test.exe built.
endlocal
