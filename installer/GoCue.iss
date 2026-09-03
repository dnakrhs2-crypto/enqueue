; GoCue installer (Inno Setup 6.3+ / 7).  Compile with tools/release.py or:
;   ISCC.exe /DAppVersion=0.1.0 /DSourceDir=..\build\vs2022\GoCue_artefacts\Release installer\GoCue.iss
; Keep this file pure ASCII; user-facing text comes from the Inno language files.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\vs2022\GoCue_artefacts\Release"
#endif
#ifndef OutputDir
  #define OutputDir "output"
#endif

#define AppName "GoCue"
#define AppExe "GoCue.exe"
#define AppPublisher "GoCue"

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
OutputBaseFilename=GoCue-Setup-{#AppVersion}
SetupIconFile=..\assets\GoCue.ico
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

[Languages]
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\WinSparkle.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; .gocue project files open with GoCue (HKA = HKCU for per-user installs, HKLM for all-users)
Root: HKA; Subkey: "Software\Classes\.gocue"; ValueType: string; ValueName: ""; ValueData: "GoCue.Project"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\GoCue.Project"; ValueType: string; ValueName: ""; ValueData: "GoCue Project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\GoCue.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"
Root: HKA; Subkey: "Software\Classes\GoCue.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""

[Run]
; also after a silent auto-update (WinSparkle runs Setup with /SILENT): the app comes back by itself and announces
; the new version. Scripted installs pass /NORUN=1 to keep it closed.
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall; Check: not NoRunRequested

[Code]
function NoRunRequested: Boolean;
begin
  Result := ExpandConstant('{param:NORUN|0}') = '1';
end;
