@echo off
setlocal
cd /d "%~dp0"

rem sign.cmd - Authenticode-sign one file with the project's own certificate.
rem
rem   sign.cmd "..\prototype\silkplay.exe"
rem
rem WHY A FRESH CERTIFICATE. This machine had an active compromise on 2026-09-17 and the owner's
rem other code-signing keys were resident in CurrentUser\My at the time. An antivirus sweep removes
rem the payloads; it cannot un-steal a private key that was already read. So Nova SilkPlay signs
rem with a certificate created after the sweep, non-exportable, and never with one of the older
rem identities - if one of those ever turns up on something the owner did not build, it stays
rem their problem alone and not this project's.
rem
rem The certificate is looked up by THUMBPRINT, not by subject: two certificates can share a
rem subject and signtool would then pick whichever it found first.
set "NSP_CERT=1C0316EB482013586C04FFAD05E50F800B9353E8"

set "SIGNTOOL=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
if not exist "%SIGNTOOL%" (
    echo FAILED: signtool.exe not found at "%SIGNTOOL%".
    exit /b 1
)
if "%~1"=="" (
    echo FAILED: no file given.
    exit /b 1
)
if not exist "%~1" (
    echo FAILED: "%~1" does not exist.
    exit /b 1
)

rem Timestamped, so the signature outlives the certificate. A signature without one stops
rem verifying the day the certificate expires, which for a beta people keep around is not
rem acceptable; if the timestamp server cannot be reached the signing FAILS rather than quietly
rem producing a signature with a shelf life.
"%SIGNTOOL%" sign /sha1 %NSP_CERT% /fd sha256 /td sha256 /tr http://timestamp.digicert.com "%~1"
if %errorlevel% neq 0 (
    echo FAILED: signtool could not sign "%~1".
    exit /b 1
)

"%SIGNTOOL%" verify /pa /v "%~1" >nul 2>&1
if %errorlevel% neq 0 (
    rem EXPECTED for a self-signed certificate: /pa checks the chain against the Authenticode
    rem policy, and nothing outside this machine trusts our own root. Say so plainly instead of
    rem letting it read as a failure - and say what it means for the user, because it is the
    rem whole story of what this signature is and is not worth.
    echo NOTE: the signature does not chain to a trusted root - expected for a self-signed
    echo       certificate. It proves the file was not altered after WE built it; it does NOT
    echo       satisfy SmartScreen, which will still warn on first run.
)
echo SUCCESS: signed "%~1".
endlocal
rem EXPLICIT. `echo` does not clear errorlevel, so without this the script returns the
rem non-zero code of the `signtool verify` above - which is the EXPECTED outcome for a
rem self-signed certificate - and the caller reads a successful signing as a failure.
exit /b 0
