@echo off
setlocal
rem build.cmd - silkplay.exe, the browser-path prototype.
rem Modelled on tools\overlay-probe\build.cmd: one vcvars call, one rc and one cl invocation,
rem loud non-zero exit on any failure.
rem
rem FLAG NOTES (deliberate, do not "fix" back):
rem   /await is NOT passed: under /std:c++20 coroutines are on by default and MSVC
rem   answers a bare /await with D9047. C++/WinRT's co_await still compiles.
rem   /bigobj IS passed: the C++/WinRT projection headers push nsp_capture.obj
rem   past the 65535-section object limit.
rem   /utf-8 IS passed: nsp_ui_text.cpp holds the Russian interface strings as UTF-8
rem   literals, which the default (system code page) reading of the source would mangle.

cd /d "%~dp0"

call "D:\Programs\Microsoft\VC\Auxiliary\Build\vcvars64.bat" >nul
if %errorlevel% neq 0 (
    echo FAILED: vcvars64.bat did not run successfully.
    exit /b 1
)

set "SOURCES=main.cpp nsp_common.cpp nsp_overlay.cpp nsp_capture.cpp nsp_synth.cpp nsp_dump.cpp nsp_ofa.cpp nsp_image.cpp nsp_offline.cpp nsp_tray.cpp nsp_winwatch.cpp nsp_settings.cpp nsp_settings_window.cpp nsp_ui_text.cpp nsp_gamegate.cpp nsp_autostart.cpp nsp_drm.cpp nsp_rectfind.cpp nsp_mediawatch.cpp"

for %%F in (%SOURCES% silkplay.rc silkplay.manifest res\silk.ico res\silk_off.ico) do (
    if not exist "%%F" (
        echo FAILED: missing source file %%F
        exit /b 1
    )
)

rem VERSION GUARD. nsp_version.h is the single source: the .rc #includes it, the exe's
rem VERSIONINFO comes from that, and installer\silkplay.iss reads the version back OUT of the
rem built exe. silkplay.manifest is the one string the preprocessor cannot reach - rc.exe embeds
rem the file verbatim - so its assemblyIdentity version is checked here instead. A drift would
rem otherwise ship an installer whose name disagrees with the binary's side-by-side identity.
set "MAJ=" & set "MIN=" & set "PAT=" & set "BLD="
for /f "tokens=3" %%V in ('findstr /c:"#define NSP_VERSION_MAJOR" nsp_version.h') do set "MAJ=%%V"
for /f "tokens=3" %%V in ('findstr /c:"#define NSP_VERSION_MINOR" nsp_version.h') do set "MIN=%%V"
for /f "tokens=3" %%V in ('findstr /c:"#define NSP_VERSION_PATCH" nsp_version.h') do set "PAT=%%V"
for /f "tokens=3" %%V in ('findstr /c:"#define NSP_VERSION_BUILD" nsp_version.h') do set "BLD=%%V"
if "%MAJ%%MIN%%PAT%%BLD%" == "" (
    echo FAILED: could not read NSP_VERSION_* out of nsp_version.h.
    exit /b 1
)
set "QUAD=%MAJ%.%MIN%.%PAT%.%BLD%"
rem The \" is findstr's own escape for a literal quote inside /c:, so this matches the exact
rem attribute and not a substring of some other version in the file.
findstr /c:"version=\"%QUAD%\"" silkplay.manifest >nul
if %errorlevel% neq 0 (
    echo FAILED: silkplay.manifest does not carry version="%QUAD%" from nsp_version.h.
    exit /b 1
)

if not exist ".\obj\" mkdir ".\obj\"

rem The commit shown in the Settings window. "+" marks a build with uncommitted changes to
rem tracked files; "local" a build outside git. The "=" in the git option must be escaped:
rem inside for /f's command an unescaped one splits the command and git never sees the option.
set "NSP_COMMIT=local"
for /f %%H in ('git rev-parse --short HEAD 2^>nul') do set "NSP_COMMIT=%%H"
set "NSP_DIRTY="
for /f %%L in ('git status --porcelain --untracked-files^=no 2^>nul') do set "NSP_DIRTY=+"
> ".\obj\nsp_build.h" echo #define NSP_BUILD_COMMIT L"%NSP_COMMIT%%NSP_DIRTY%"

rc.exe /nologo /fo ".\obj\silkplay.res" silkplay.rc
if %errorlevel% neq 0 (
    echo FAILED: resource compilation failed.
    exit /b 1
)

cl.exe /nologo /EHsc /std:c++20 /W4 /bigobj /O2 /utf-8 /I.\obj ^
    /Fo.\obj\ ^
    %SOURCES% .\obj\silkplay.res ^
    /Fe:".\silkplay.exe" ^
    /link /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib dcomp.lib dwmapi.lib user32.lib dxguid.lib d3dcompiler.lib windowscodecs.lib bcrypt.lib windowsapp.lib shell32.lib comctl32.lib gdiplus.lib shcore.lib ole32.lib gdi32.lib pdh.lib advapi32.lib
if %errorlevel% neq 0 (
    echo FAILED: Compilation or link failed.
    exit /b 1
)

echo SUCCESS: silkplay.exe built.
endlocal
