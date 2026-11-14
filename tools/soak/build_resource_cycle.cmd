@echo off
setlocal
cd /d "%~dp0"
call "D:\Programs\Microsoft\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist obj mkdir obj
cl.exe /nologo /EHsc /std:c++20 /W4 /bigobj /O2 /utf-8 /I..\..\prototype /Foobj\ ^
    resource_cycle.cpp ..\..\prototype\nsp_capture.cpp ..\..\prototype\nsp_common.cpp ^
    ..\..\prototype\nsp_overlay.cpp ..\..\prototype\nsp_ofa.cpp ^
    /Fe:resource_cycle.exe /link /SUBSYSTEM:CONSOLE d3d11.lib dxgi.lib dcomp.lib dwmapi.lib ^
    user32.lib gdi32.lib dxguid.lib d3dcompiler.lib windowsapp.lib shell32.lib ole32.lib
if errorlevel 1 exit /b 1
echo SUCCESS: resource_cycle.exe built.
exit /b 0
