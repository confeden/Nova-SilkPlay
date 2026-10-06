@echo off
cd /d "%~dp0"
call "D:\Programs\Microsoft\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /EHsc /O2 /W4 /std:c++20 /utf-8 uia_video.cpp /Fe:uia_video.exe /link /SUBSYSTEM:CONSOLE || exit /b 1
del uia_video.obj 2>nul
