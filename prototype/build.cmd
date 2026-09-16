@echo off
setlocal
rem build.cmd - silkplay.exe, the browser-path prototype.
rem Modelled on tools\overlay-probe\build.cmd: one vcvars call, one cl invocation,
rem loud non-zero exit on any failure.
rem
rem FLAG NOTES (deliberate, do not "fix" back):
rem   /await is NOT passed: under /std:c++20 coroutines are on by default and MSVC
rem   answers a bare /await with D9047. C++/WinRT's co_await still compiles.
rem   /bigobj IS passed: the C++/WinRT projection headers push nsp_capture.obj
rem   past the 65535-section object limit.

cd /d "%~dp0"

call "D:\Programs\Microsoft\VC\Auxiliary\Build\vcvars64.bat" >nul
if %errorlevel% neq 0 (
    echo FAILED: vcvars64.bat did not run successfully.
    exit /b 1
)

for %%F in (main.cpp nsp_common.cpp nsp_overlay.cpp nsp_capture.cpp nsp_synth.cpp nsp_dump.cpp nsp_ofa.cpp nsp_image.cpp nsp_offline.cpp nsp_tray.cpp nsp_winwatch.cpp) do (
    if not exist "%%F" (
        echo FAILED: missing source file %%F
        exit /b 1
    )
)

if not exist ".\obj\" mkdir ".\obj\"

cl.exe /nologo /EHsc /std:c++20 /W4 /bigobj /O2 ^
    /Fo.\obj\ ^
    main.cpp nsp_common.cpp nsp_overlay.cpp nsp_capture.cpp nsp_synth.cpp nsp_dump.cpp nsp_ofa.cpp nsp_image.cpp nsp_offline.cpp nsp_tray.cpp nsp_winwatch.cpp ^
    /Fe:".\silkplay.exe" ^
    /link d3d11.lib dxgi.lib dcomp.lib dwmapi.lib user32.lib dxguid.lib d3dcompiler.lib windowscodecs.lib bcrypt.lib windowsapp.lib shell32.lib
if %errorlevel% neq 0 (
    echo FAILED: Compilation or link failed.
    exit /b 1
)

echo SUCCESS: silkplay.exe built.
endlocal
