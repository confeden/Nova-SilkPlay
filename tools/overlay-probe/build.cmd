@echo off
setlocal
rem build.cmd - overlay_probe.exe (S-M2 overlay / composition measurement probe).
rem Modelled on tools\caps-probe\build.cmd: one vcvars call, one cl invocation,
rem loud non-zero exit on any failure.
rem
rem FLAG NOTE (deliberate deviation, do not "fix" it back):
rem   /await is NOT passed. Under /std:c++20 coroutines are enabled by default and
rem   MSVC answers /await with "Command line warning D9047: option 'await' has been
rem   deprecated". The house rule is /W4 clean with no avoidable warnings, so the
rem   flag is dropped; C++/WinRT's co_await still compiles. If a future toolchain
rem   ever needs it, use /await:strict, never bare /await.
rem   /bigobj IS passed: the C++/WinRT projection headers push wgc_capture.obj past
rem   the 65535-section limit.

cd /d "%~dp0"

call "D:\Programs\Microsoft\VC\Auxiliary\Build\vcvars64.bat" >nul
if %errorlevel% neq 0 (
    echo FAILED: vcvars64.bat did not run successfully.
    exit /b 1
)

rem The four translation units. overlay_win.cpp and wgc_capture.cpp are owned by
rem other authors; say so plainly instead of letting cl print a bare C1083.
for %%F in (main.cpp probe_common.cpp overlay_win.cpp wgc_capture.cpp) do (
    if not exist "%%F" (
        echo FAILED: missing source file %%F
        exit /b 1
    )
)

if not exist ".\obj\" mkdir ".\obj\"
if %errorlevel% neq 0 (
    echo FAILED: could not create the obj directory.
    exit /b 1
)

cl.exe /nologo /EHsc /std:c++20 /W4 /bigobj ^
    /Fo.\obj\ ^
    main.cpp probe_common.cpp overlay_win.cpp wgc_capture.cpp ^
    /Fe:".\overlay_probe.exe" ^
    /link d3d11.lib dxgi.lib dcomp.lib dwmapi.lib user32.lib dxguid.lib windowsapp.lib
if %errorlevel% neq 0 (
    echo FAILED: Compilation or link failed.
    exit /b 1
)

echo SUCCESS: overlay_probe.exe built.
endlocal
