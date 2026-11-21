; Inno Setup script for Wireless Battery Center (https://jrsoftware.org/isinfo.php).
; Per-user install, no administrator rights needed.
; Build with installer\build-installer.ps1. Installer and portable ZIP use the same staged files.

#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef StageDir
  #error StageDir is required
#endif
#define Root ".."

[Setup]
AppId={{29FA890E-D56F-4B01-8D9F-CB7B03DE1524}
AppName=Wireless Battery Center
AppVersion={#AppVersion}
AppVerName=Wireless Battery Center {#AppVersion}
AppPublisher=Wireless Battery Center contributors
DefaultDirName={localappdata}\Programs\Wireless Battery Center
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#Root}\dist
OutputBaseFilename=WirelessBatteryCenter-{#AppVersion}-windows-x64-setup
SetupIconFile={#Root}\res\app.ico
UninstallDisplayIcon={app}\WirelessBatteryCenterPanel.exe
UninstallDisplayName=Wireless Battery Center
VersionInfoVersion={#AppVersion}
VersionInfoProductName=Wireless Battery Center
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=no

[Tasks]
Name: "autostart"; Description: "Start Wireless Battery Center when I sign in to Windows"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{userprograms}\Wireless Battery Center"; Filename: "{app}\WirelessBatteryCenterPanel.exe"; WorkingDir: "{app}"

[Run]
Filename: "{app}\WirelessBatteryCenterPanel.exe"; Description: "Open Wireless Battery Center"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\WirelessBatteryCenter.exe"; Parameters: "--quit"; Flags: runhidden waituntilterminated; RunOnceId: "QuitCenter"

[Code]
const
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';

procedure QuitRunningApp();
var
  Code: Integer;
begin
  if FileExists(ExpandConstant('{app}\WirelessBatteryCenter.exe')) then
    Exec(ExpandConstant('{app}\WirelessBatteryCenter.exe'), '--quit', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function InstalledRunValue(): String;
begin
  Result := '"' + ExpandConstant('{app}\WirelessBatteryCenter.exe') + '"';
end;

{ Removes the startup entry only if it points at this installed copy, so a portable copy's
  "Start with Windows" setting is left alone. }
procedure RemoveOwnAutostart();
var
  Value: String;
begin
  if RegQueryStringValue(HKCU, RunKey, 'WirelessBatteryCenter', Value) and (CompareText(Value, InstalledRunValue()) = 0) then
    RegDeleteValue(HKCU, RunKey, 'WirelessBatteryCenter');
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if FileExists(ExpandConstant('{app}\WirelessBattery.exe')) or
     FileExists(ExpandConstant('{app}\WirelessBatteryPanel.exe')) then
    Result := 'Choose a separate folder for Wireless Battery Center. The selected folder contains Wireless Battery.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    QuitRunningApp();  { upgrade: close the old version so its files can be replaced }
  if CurStep = ssPostInstall then
  begin
    { Same value the app's own "Start with Windows" menu item writes, so the two stay in sync. }
    if WizardIsTaskSelected('autostart') then
      RegWriteStringValue(HKCU, RunKey, 'WirelessBatteryCenter', InstalledRunValue())
    else
      RemoveOwnAutostart();
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveOwnAutostart();
end;
