@echo off
setlocal
rem build.cmd - ofhints.exe, the NVOFA external-hint capability probe.
rem Modelled on tools\caps-probe\build.cmd: one vcvars call, one cl invocation,
rem loud non-zero exit on any failure.

call "D:\Programs\Microsoft\VC\Auxiliary\Build\vcvars64.bat" >nul
if %errorlevel% neq 0 (
    echo FAILED: vcvars64.bat did not run successfully.
    exit /b 1
)

if not exist "%~dp0ofhints.cpp" (
    echo FAILED: missing source file ofhints.cpp
    exit /b 1
)

cl.exe /nologo /EHsc /std:c++20 /W4 /WX- /O2 ^
    /Fo:"%~dp0ofhints.obj" ^
    "%~dp0ofhints.cpp" ^
    /Fe:"%~dp0ofhints.exe" ^
    /link d3d11.lib dxgi.lib dxguid.lib
if %errorlevel% neq 0 (
    echo FAILED: Compilation or link failed.
    exit /b 1
)

echo SUCCESS: ofhints.exe built.
endlocal
