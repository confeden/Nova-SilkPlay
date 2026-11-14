# Installer

Build `prototype\silkplay.exe` first, then run `installer\build_installer.cmd`.
The installer reads its version from that executable and writes
`installer\out\Nova.SilkPlay-<version>-setup.exe`.

Release signing is opt-in: `installer\build_installer.cmd sign` signs the payload
and completed setup using the project's post-cleanup certificate and a required
DigiCert RFC3161 SHA-256 timestamp. Private keys stay in the Windows certificate
store. The uninstaller is not separately signed.

If SignTool's WinHTTP connection cannot reach the timestamp authority, set
`NSP_TIMESTAMP_RELAY=1` before building. This requires Python 3 and forwards the
original timestamp request through a temporary loopback server; no system proxy
settings or certificate stores change.

Verification rejects missing timestamps, unexpected certificates and modified
files. The project's certificate is self-signed, so Windows and SmartScreen may
still warn on machines that do not trust the publisher.
