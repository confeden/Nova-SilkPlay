@echo off
setlocal
cd /d "%~dp0"
call "D:\Programs\Microsoft\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
if not exist ".\obj\" mkdir ".\obj\"
cl.exe /nologo /EHsc /std:c++20 /W4 /O2 /utf-8 cadence_test.cpp /Fo.\obj\cadence_test.obj /Fe:cadence_test.exe
exit /b %errorlevel%
