@echo off
setlocal
call "D:\Programs\Microsoft\VC\Auxiliary\Build\vcvars64.bat" >nul
if %errorlevel% neq 0 (
    echo FAILED: vcvars64.bat did not run successfully.
    exit /b 1
)
cl.exe /nologo /EHsc /std:c++20 /W4 /WX- "%~dp0caps_probe.cpp" /Fe:"%~dp0caps_probe.exe" /link d3d11.lib d3d12.lib dxgi.lib dxguid.lib
if %errorlevel% neq 0 (
    echo FAILED: Compilation failed.
    exit /b 1
)
echo SUCCESS: caps_probe.exe built.
endlocal
