@echo off
setlocal
cd /d "%~dp0"

rem build_installer.cmd - compile installer\silkplay.iss into installer\out\.
rem
rem It deliberately does NOT build silkplay.exe: prototype\build.cmd owns that, and an installer
rem that quietly rebuilds its payload can ship a binary nobody looked at.

set "ISCC=D:\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
    echo FAILED: Inno Setup not found at "%ISCC%".
    exit /b 1
)
if not exist "..\prototype\silkplay.exe" (
    echo FAILED: ..\prototype\silkplay.exe is missing - run prototype\build.cmd first.
    exit /b 1
)

rem SIGNING. Off by default; `build_installer.cmd sign` turns it on.
rem
rem The owner confirmed on 2026-09-20 that the machine was cleaned after the 2026-09-17
rem compromise, so releases may be built here again. Nova SilkPlay nevertheless signs with a
rem certificate created AFTER that sweep (see sign.cmd): an antivirus pass removes payloads, it
rem cannot un-steal a private key that was already read, and the owner's older signing identities
rem were resident at the time.
rem
rem Order matters: the exe is signed FIRST, because the installer embeds it byte for byte, and
rem then the completed setup is signed. The uninstaller is not separately signed.
set "NSP_SIGN="
if /i "%~1"=="sign" set "NSP_SIGN=1"
set "NSP_APP_VERSION="
for /f "delims=" %%V in ('powershell.exe -NoProfile -Command "(Get-Item -LiteralPath '..\prototype\silkplay.exe').VersionInfo.ProductVersion"') do set "NSP_APP_VERSION=%%V"
if not defined NSP_APP_VERSION (
    echo FAILED: could not read the built application's version.
    exit /b 1
)
set "NSP_SETUP=out\Nova.SilkPlay-%NSP_APP_VERSION%-setup.exe"

if defined NSP_SIGN (
    call "%~dp0sign.cmd" "..\prototype\silkplay.exe"
    if errorlevel 1 (
        echo FAILED: could not sign silkplay.exe.
        exit /b 1
    )
) else (
    echo NOTE: signing is OFF. Pass "sign" to sign the payload and the setup.
    echo       The setup this produces is UNSIGNED and SmartScreen will warn about it.
)

rem ISCC's own SignTool hook is NOT used, and the reason is worth keeping: the signer it
rem runs is given the file WITHOUT quotes once the command reaches signtool through cmd, and
rem this project's path has a space in it - signtool then received
rem   D:\Documents\Coding\Nova   and   SilkPlay\installer\out\uninst.e32.tmp
rem as two arguments and failed on both. setup.exe is signed here instead, after it exists.
rem What that costs: the UNINSTALLER is generated from setup.exe at install time and is not
rem separately signed. The payload and the setup are.
"%ISCC%" silkplay.iss
if %errorlevel% neq 0 (
    echo FAILED: ISCC.
    exit /b 1
)

if defined NSP_SIGN (
    call "%~dp0sign.cmd" "%NSP_SETUP%"
    if errorlevel 1 (
        echo FAILED: could not sign the current setup.
        exit /b 1
    )
)

echo SUCCESS: installer\out\ built.
endlocal
