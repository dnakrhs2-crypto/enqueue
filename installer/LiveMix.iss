; LiveMix installer (Inno Setup 6.3+ / 7).  Compile with tools/release.py --app livemix or:
;   ISCC.exe /DAppVersion=0.1.0 /DSourceDir=..\build\vs2022\LiveMix_artefacts\Release installer\LiveMix.iss
; UTF-8 with BOM (Inno Setup 6+): the publisher name is Korean. Other user-facing text comes from the Inno language files.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\vs2022\LiveMix_artefacts\Release"
#endif
#ifndef OutputDir
  #define OutputDir "output"
#endif

#define AppName "LiveMix"
#define AppExe "LiveMix.exe"
#define AppPublisher "곰튀김"

[Setup]
AppId={{7D2A9C41-3E6B-4F0A-9B7C-2C1F5E8D6A03}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
OutputDir={#OutputDir}
OutputBaseFilename=LiveMix-Setup-{#AppVersion}
SetupIconFile=..\livemix\assets\LiveMix.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1607 or later (the exe imports GetDpiForWindow / EnableNonClientDpiScaling)
MinVersion=10.0.14393
; Per-user install by default (no UAC, so WinSparkle can run updates unattended)
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline
ChangesAssociations=yes
CloseApplications=yes
RestartApplications=no
DisableProgramGroupPage=yes
UsePreviousGroup=no

[Languages]
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

; localised strings of our own (UTF-8 with BOM); this file itself stays ASCII apart from the publisher
#include "Enqueue.messages.iss"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"
; Coupang Partners desktop shortcut: an affiliate link, offered with the disclosure and the way to remove it.
; Checked by default; the shortcut file is written by [Code] so a silent auto-update never recreates one the user deleted.
Name: "coupang"; Description: "{cm:CoupangTask}"; GroupDescription: "{cm:CoupangGroup}"

[Files]
Source: "{#SourceDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\WinSparkle.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "coupang.ico"; DestDir: "{app}"; Flags: ignoreversion; Tasks: coupang

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; .livemix session files open with LiveMix (HKA = HKCU for per-user installs, HKLM for all-users)
Root: HKA; Subkey: "Software\Classes\.livemix"; ValueType: string; ValueName: ""; ValueData: "LiveMix.Session"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\LiveMix.Session"; ValueType: string; ValueName: ""; ValueData: "LiveMix Session"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\LiveMix.Session\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"
Root: HKA; Subkey: "Software\Classes\LiveMix.Session\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""

[Run]
; also after a silent auto-update (WinSparkle runs Setup with /SILENT): the app comes back by itself.
; Scripted installs pass /NORUN=1 to keep it closed.
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall; Check: not NoRunRequested

[UninstallRun]
; the "start with Windows" entry the app may have written
Filename: "reg.exe"; Parameters: "delete HKCU\Software\Microsoft\Windows\CurrentVersion\Run /v LiveMix /f"; Flags: runhidden; RunOnceId: "LiveMixRunKey"

[Code]
const
  CoupangShortcutUrl = 'https://xn--jb0byyo90f.com/coupang/';   { 곰튀김.com }

function NoRunRequested: Boolean;
begin
  Result := ExpandConstant('{param:NORUN|0}') = '1';
end;

function CoupangShortcutPath: String;
begin
  Result := ExpandConstant('{userdesktop}\') + CustomMessage('CoupangShortcutName') + '.url';
end;

{ The shortcut is written when the user saw the checkbox (an interactive install) or asked for it on the command
  line (/TASKS=coupang). A silent auto-update remembers the task but must not bring back a deleted shortcut. }
function CoupangShortcutWanted: Boolean;
begin
  Result := WizardIsTaskSelected('coupang')
    and ((not WizardSilent) or (Pos('coupang', Lowercase(ExpandConstant('{param:TASKS|}'))) > 0));
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Path: String;
begin
  if (CurStep = ssPostInstall) and CoupangShortcutWanted then
  begin
    Path := CoupangShortcutPath;
    SetIniString('InternetShortcut', 'URL', CoupangShortcutUrl, Path);
    SetIniString('InternetShortcut', 'IconFile', ExpandConstant('{app}\coupang.ico'), Path);
    SetIniString('InternetShortcut', 'IconIndex', '0', Path);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    DeleteFile(CoupangShortcutPath);
end;
