@echo off
setlocal
rem build_rectfind_test.cmd - rectfind_test.exe, the windowed video rect logic (P11).
cd /d "%~dp0"
call "D:\Programs\Microsoft\VC\Auxiliary\Build\vcvars64.bat" >nul
if %errorlevel% neq 0 (
    echo FAILED: vcvars64.bat did not run successfully.
    exit /b 1
)
if not exist ".\obj\" mkdir ".\obj\"
cl.exe /nologo /EHsc /std:c++20 /W4 /O2 /Fo.\obj\ /I.. rectfind_test.cpp ..\nsp_rectfind.cpp /Fe:".\rectfind_test.exe"
if %errorlevel% neq 0 (
    echo FAILED: Compilation or link failed.
    exit /b 1
)
echo SUCCESS: rectfind_test.exe built.
endlocal
