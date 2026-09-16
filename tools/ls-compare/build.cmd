@echo off
setlocal
rem build.cmd - ddacap.exe (Desktop Duplication recorder). One vcvars call, one cl.
cd /d "%~dp0"
call "D:\Programs\Microsoft\VC\Auxiliary\Build\vcvars64.bat" >nul
if %errorlevel% neq 0 (
    echo FAILED: vcvars64.bat did not run successfully.
    exit /b 1
)
if not exist ".\obj\" mkdir ".\obj\"
cl.exe /nologo /EHsc /std:c++20 /W4 /O2 /Fo.\obj\ ddacap.cpp /Fe:".\ddacap.exe" /link d3d11.lib dxgi.lib user32.lib
if %errorlevel% neq 0 (
    echo FAILED: Compilation or link failed.
    exit /b 1
)
echo SUCCESS: ddacap.exe built.
endlocal
