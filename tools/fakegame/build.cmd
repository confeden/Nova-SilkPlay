@echo off
setlocal
rem build.cmd - fakegame.exe
rem Modelled on prototype\build.cmd: one vcvars call, one cl invocation,
rem loud non-zero exit on any failure.

cd /d "%~dp0"

call "D:\Programs\Microsoft\VC\Auxiliary\Build\vcvars64.bat" >nul
if %errorlevel% neq 0 (
    echo FAILED: vcvars64.bat did not run successfully.
    exit /b 1
)

cl.exe /nologo /EHsc /std:c++17 /W4 /O2 ^
    /Fo.\ ^
    fakegame.cpp ^
    /Fe:".\fakegame.exe" ^
    /link d3d11.lib dxgi.lib user32.lib d3dcompiler.lib
if %errorlevel% neq 0 (
    echo FAILED: Compilation or link failed.
    exit /b 1
)

echo SUCCESS: fakegame.exe built.
endlocal
