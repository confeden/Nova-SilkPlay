@echo off
setlocal
cd /d "%~dp0"
call "D:\Programs\Microsoft\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl.exe /nologo /EHsc /std:c++20 /W4 /O2 /utf-8 ofa_lifetime_test.cpp /Fo.\obj\ /Fe:ofa_lifetime_test.exe ..\nsp_ofa.cpp ..\obj\nsp_common.obj /link d3d11.lib dxgi.lib dxguid.lib user32.lib dwmapi.lib shell32.lib ole32.lib
exit /b %errorlevel%
