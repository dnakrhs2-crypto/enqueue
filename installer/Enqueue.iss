; Enqueue installer (Inno Setup 6.3+ / 7).  Compile with tools/release.py or:
;   ISCC.exe /DAppVersion=0.1.0 /DSourceDir=..\build\vs2022\Enqueue_artefacts\Release installer\Enqueue.iss
; UTF-8 with BOM (Inno Setup 6+): the publisher name is Korean. Other user-facing text comes from the Inno language files.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\vs2022\Enqueue_artefacts\Release"
#endif
#ifndef OutputDir
  #define OutputDir "output"
#endif

#define AppName "Enqueue"
#define AppExe "Enqueue.exe"
#define AppPublisher "곰튀김"

[Setup]
AppId={{B7E3F2C1-6C5D-4C3E-9C2B-7F1E9D2A5C10}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
OutputDir={#OutputDir}
OutputBaseFilename=Enqueue-Setup-{#AppVersion}
SetupIconFile=..\assets\Enqueue.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Per-user install by default (no UAC, so WinSparkle can run updates unattended);
; the user may still choose "all users" in the dialog.
PrivilegesRequired=lowest
; per-user install only, no "all users / current user" question (an /ALLUSERS switch is still honoured)
PrivilegesRequiredOverridesAllowed=commandline
ChangesAssociations=yes
CloseApplications=yes
RestartApplications=no
DisableProgramGroupPage=yes
; the Start Menu group is always "Enqueue": an upgrade from GoCue must not keep writing into the "GoCue" group
UsePreviousGroup=no

[Languages]
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

; localised strings of our own (UTF-8 with BOM); this file itself stays ASCII
#include "Enqueue.messages.iss"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
; Coupang Partners desktop shortcut: an affiliate link, offered with the disclosure and the way to remove it
; (the consent the Korean network act asks for). Checked by default like Bandizip's; the shortcut file is written
; by [Code] so a silent auto-update never recreates one the user deleted.
Name: "coupang"; Description: "{cm:CoupangTask}"; GroupDescription: "{cm:CoupangGroup}"

[Files]
Source: "{#SourceDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\WinSparkle.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "coupang.ico"; DestDir: "{app}"; Flags: ignoreversion; Tasks: coupang

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; .enqueue project files (and the old .gocue) open with Enqueue (HKA = HKCU for per-user installs, HKLM for all-users)
Root: HKA; Subkey: "Software\Classes\.enqueue"; ValueType: string; ValueName: ""; ValueData: "Enqueue.Project"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.gocue"; ValueType: string; ValueName: ""; ValueData: "Enqueue.Project"; Flags: uninsdeletevalue uninsdeletekeyifempty
; a user who once picked GoCue in "Open with" has UserChoice = GoCue.Project: that ProgId stays, pointing at Enqueue
Root: HKA; Subkey: "Software\Classes\.gocue\OpenWithProgids"; ValueType: string; ValueName: "Enqueue.Project"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\GoCue.Project"; ValueType: string; ValueName: ""; ValueData: "Enqueue Project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\GoCue.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"
Root: HKA; Subkey: "Software\Classes\GoCue.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""
Root: HKA; Subkey: "Software\Classes\Enqueue.Project"; ValueType: string; ValueName: ""; ValueData: "Enqueue Project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\Enqueue.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"
Root: HKA; Subkey: "Software\Classes\Enqueue.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""

[InstallDelete]
; an upgrade from GoCue (same AppId): the old exe and the shortcuts that pointed at it
Type: files; Name: "{app}\GoCue.exe"
Type: files; Name: "{group}\GoCue.lnk"
Type: files; Name: "{autodesktop}\GoCue.lnk"
Type: files; Name: "{autoprograms}\GoCue\GoCue.lnk"
Type: files; Name: "{autoprograms}\GoCue\Enqueue.lnk"
Type: dirifempty; Name: "{autoprograms}\GoCue"

[Run]
; also after a silent auto-update (WinSparkle runs Setup with /SILENT): the app comes back by itself and announces
; the new version. Scripted installs pass /NORUN=1 to keep it closed.
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall; Check: not NoRunRequested

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
