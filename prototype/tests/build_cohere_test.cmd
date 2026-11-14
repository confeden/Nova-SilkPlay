@echo off
setlocal
cd /d "%~dp0"
call "D:\Programs\Microsoft\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl.exe /nologo /EHsc /std:c++20 /W4 /bigobj /O2 /utf-8 cohere_test.cpp /Fo.\obj\cohere_test.obj /Fe:cohere_test.exe ..\obj\nsp_common.obj ..\obj\nsp_ofa.obj ..\obj\nsp_image.obj /link d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib windowscodecs.lib bcrypt.lib user32.lib ole32.lib gdi32.lib dwmapi.lib shell32.lib
exit /b %errorlevel%
