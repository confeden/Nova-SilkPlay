; silkplay.iss - Nova SilkPlay installer. Per-user, no elevation (D6, D14).
;
; Build:  installer\build_installer.cmd
;    or:  "D:\Programs\Inno Setup 6\ISCC.exe" installer\silkplay.iss
;
; Signing is opt-in through build_installer.cmd sign, using the post-cleanup certificate
; and a required online RFC3161 timestamp. Unsigned local builds remain available.
;
; Every version string comes out of the BUILT EXE, which rc.exe filled from prototype\nsp_version.h.
; That makes the header the single source: build.cmd additionally checks silkplay.manifest against
; it, because a manifest is embedded verbatim and the preprocessor cannot reach it.

; SourcePath is the directory of THIS script. Using it instead of a bare relative path means the
; version lookups work no matter which directory ISCC was started from (the verification command
; runs it from the repo root; build_installer.cmd runs it from installer\).
#define AppExe      AddBackslash(SourcePath) + "..\prototype\silkplay.exe"
#define AppVerText  GetStringFileInfo(AppExe, PRODUCT_VERSION)
#define AppVerQuad  GetVersionNumbersString(AppExe)
#define AppName     "Nova SilkPlay"

[Setup]
AppId={{B3F1D7A2-5C48-4E9B-9F21-7A0C6E3D4155}
AppName={#AppName}
AppVersion={#AppVerText}
VersionInfoVersion={#AppVerQuad}
AppPublisher={#AppName}
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
UsePreviousAppDir=yes
; Per-user and unelevated on purpose. UIPI means an elevated process could not capture a normal
; browser window at all, so admin rights would not just be friction - they would break the product.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
OutputDir=out
OutputBaseFilename=Nova.SilkPlay-{#AppVerText}-setup
LicenseFile=LICENSE.txt
SetupIconFile=..\prototype\res\silk.ico
UninstallDisplayIcon={app}\silkplay.exe
UninstallDisplayName={#AppName} {#AppVerText}
; The running instance is detected by this session-namespace mutex (main.cpp creates it next to
; the engine mutex). A Global\ name is impossible: an unelevated process has no
; SeCreateGlobalPrivilege, so the create would simply fail.
AppMutex=NovaSilkPlayAppMutex
; Restart Manager force-kills a process that does not answer in time, and a killed silkplay never
; reaches Shell_NotifyIcon(NIM_DELETE) - leaving a ghost tray icon until the user sweeps the
; notification area. That is a visible defect on a D22 project. We ask the app to quit instead,
; through its own quit event, in [Code]. Do not "fix" this back to yes.
CloseApplications=no

; Signing does NOT go through ISCC: its signer receives an unquoted path and this project's
; own path has a space in it. build_installer.cmd signs the payload before compiling and the
; setup after, with installer\sign.cmd.

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "autostart"; Description: "{cm:AutoStartProgram,{#AppName}}"; \
    GroupDescription: "{cm:AutoStartProgramGroupDescription}"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; \
    GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\prototype\silkplay.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
; NOTHING ELSE, and the omissions are deliberate. The exe is self-contained: both shaders and
; both icons are already in its resources, so res\*.ico would only be duplicates. Explicitly NOT
; shipped: *.ppm and *.png measurement dumps, *.log, obj\, the Python harness (*.py, tests\),
; testmotion.html, prototype\README.md, lab.cmd, build.cmd, tools\ and every source file.

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\silkplay.exe"
Name: "{userstartup}\{#AppName}"; Filename: "{app}\silkplay.exe"; WorkingDir: "{app}"; Tasks: autostart
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\silkplay.exe"; Tasks: desktopicon

[Registry]
; Retire the old Run entry on upgrade; Settings and Setup both use the Startup shortcut.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; \
    ValueName: "{#AppName}"; Flags: deletevalue uninsdeletevalue

[InstallDelete]
Type: files; Name: "{userstartup}\{#AppName}.lnk"; Tasks: not autostart

[Run]
Filename: "{app}\silkplay.exe"; Description: "{cm:LaunchProgram,{#AppName}}"; \
    Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\Nova SilkPlay\logs"
; Settings can create this shortcut without the Setup task, and Inno removes only what it recorded.
Type: files; Name: "{userstartup}\{#AppName}.lnk"
; settings.json and its directory are NOT listed on purpose: see CurUninstallStepChanged.

[CustomMessages]
en.AskRemoveSettings=Also delete your Nova SilkPlay settings?%n%nChoose No if you plan to reinstall.
ru.AskRemoveSettings=Удалить также настройки Nova SilkPlay?%n%nВыберите «Нет», если планируете установить программу снова.
en.StillRunning=Nova SilkPlay is still running. Quit it from the tray icon (or press Ctrl+Alt+X) and click Retry.
ru.StillRunning=Nova SilkPlay ещё работает. Закройте её через значок в трее (или Ctrl+Alt+X) и нажмите «Повторить».

[Code]
const
  EVENT_MODIFY_STATE = $0002;
  SYNCHRONIZE        = $00100000;
  QuitEventName      = 'Local\NovaSilkPlay.Quit';
  AppMutexName       = 'NovaSilkPlayAppMutex';
  RunKey             = 'Software\Microsoft\Windows\CurrentVersion\Run';

// DWORD/Cardinal rather than BOOL for the inherit-handle argument: Pascal Script's Boolean is one
// byte and the marshalling of it into a 4-byte Win32 BOOL is not something to bet an installer on.
function OpenEventW(DesiredAccess: Cardinal; InheritHandle: Cardinal; Name: String): THandle;
  external 'OpenEventW@kernel32.dll stdcall';
function OpenMutexW(DesiredAccess: Cardinal; InheritHandle: Cardinal; Name: String): THandle;
  external 'OpenMutexW@kernel32.dll stdcall';
function SetEvent(hEvent: THandle): Cardinal;
  external 'SetEvent@kernel32.dll stdcall';
function CloseHandle(hObject: THandle): Cardinal;
  external 'CloseHandle@kernel32.dll stdcall';

// The same mutex [Setup]'s AppMutex names. Its existence is the whole signal: main.cpp creates it
// at startup and the handle dies with the process, so "gone" means "really gone", not "asked to go".
function AppIsRunning(): Boolean;
var
  h: THandle;
begin
  h := OpenMutexW(SYNCHRONIZE, 0, AppMutexName);
  Result := h <> 0;
  if Result then
    CloseHandle(h);
end;

// Ask a running instance to quit the way Ctrl+Alt+X does. main.cpp creates the manual-reset event
// Local\NovaSilkPlay.Quit and its main loop leaves by the same path as the hotkey - shutting the
// engine down and calling NIM_DELETE, so no orphan tray icon is left behind. Returns True once the
// AppMutex is gone, or if it was never there.
function AskAppToQuit(): Boolean;
var
  h: THandle;
  i: Integer;
begin
  if not AppIsRunning() then
  begin
    Result := True;
    Exit;
  end;
  h := OpenEventW(EVENT_MODIFY_STATE, 0, QuitEventName);
  if h <> 0 then
  begin
    SetEvent(h);
    CloseHandle(h);
  end;
  // Up to 10 s. Shutdown has a GPU device and a capture session to tear down, so it is not instant.
  for i := 1 to 40 do
  begin
    Sleep(250);
    if not AppIsRunning() then
    begin
      Result := True;
      Exit;
    end;
  end;
  Result := not AppIsRunning();
end;

// Earliest hook there is, because Setup's own AppMutex check runs before the wizard appears and
// would otherwise greet the user with "close the application" before we ever got to ask it. If the
// app will not go, we say nothing and return True: that AppMutex check is then the right fallback,
// and it offers Retry.
function InitializeSetup(): Boolean;
begin
  AskAppToQuit();
  Result := True;
end;

procedure InitializeWizard();
var
  v: String;
  approved: AnsiString;
  registered: Boolean;
begin
  // Preserve the existing choice on upgrade, including legacy Run installations.
  registered := FileExists(ExpandConstant('{userstartup}\{#AppName}.lnk'));
  if not registered and RegQueryStringValue(HKEY_CURRENT_USER, RunKey, '{#AppName}', v) then
  begin
    registered := True;
    if RegQueryBinaryValue(HKEY_CURRENT_USER,
        'Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run',
        '{#AppName}', approved) then
      if Length(approved) > 0 then
        registered := (Ord(approved[1]) and 1) = 0;
  end;
  if registered then
    WizardSelectTasks('*autostart')
  else
    WizardSelectTasks('*!autostart');
end;

// Backstop for an instance started while the wizard was open.
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not AskAppToQuit() then
    Result := ExpandConstant('{cm:StillRunning}');
end;

function InitializeUninstall(): Boolean;
begin
  Result := AskAppToQuit();
  if not Result then
    MsgBox(ExpandConstant('{cm:StillRunning}'), mbError, MB_OK);
end;

// Settings survive an uninstall unless the user says otherwise: uninstall-then-reinstall is how a
// beta gets updated by hand, and silently discarding the user's language, capture and counter
// choices every time is a bad surprise. Default is No, so a /SILENT uninstall keeps them.
procedure CurUninstallStepChanged(CurStep: TUninstallStep);
begin
  if CurStep = usPostUninstall then
    if SuppressibleMsgBox(ExpandConstant('{cm:AskRemoveSettings}'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES then
      DelTree(ExpandConstant('{localappdata}\Nova SilkPlay'), True, True, True);
end;
