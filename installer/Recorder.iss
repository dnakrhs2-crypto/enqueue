; Tally (formerly Recorder), generated ProductIdentity + audited staging directory only.
; Compile through tools/release.py --app recorder --package-only.
#ifndef IdentityFile
  #error Recorder requires a generated IdentityFile from tools/release.py
#endif
#include IdentityFile
#ifndef SourceDir
  #error Recorder requires an audited SourceDir
#endif
#ifndef OutputDir
  #error Recorder requires an isolated OutputDir
#endif
#ifndef AppVersion
  #define AppVersion IdentityVersion
#endif
#if AppVersion != IdentityVersion
  #error AppVersion differs from ProductIdentity
#endif

[Setup]
AppId={#InstallerAppId}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\{#SettingsFolder}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
OutputDir={#OutputDir}
OutputBaseFilename={#PackageStem}-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Candidate admission floor only; minimum supported OS remains a round-34 gate.
MinVersion=10.0.19045
; Per-user install by default (no UAC, so WinSparkle can run updates unattended)
PrivilegesRequired=lowest
ChangesAssociations=yes
CloseApplications=no
; Do not force a running recording process closed. Round 28 controls update shutdown.
InfoBeforeFile={#SourceDir}\licenses\NOTICE.txt
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
; SourceDir contains only the manifest-enumerated payload, including FFmpeg/WinSparkle and notices.
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "coupang.ico"; DestDir: "{app}"; Flags: ignoreversion; Tasks: coupang

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; {#ProjectExtension} session files open with LiveMix (HKA = HKCU for per-user installs, HKLM for all-users)
Root: HKCU; Subkey: "Software\Classes\{#ProjectExtension}"; ValueType: string; ValueName: ""; ValueData: "{#FileType}"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\Classes\{#FileType}"; ValueType: string; ValueName: ""; ValueData: "{#AppName} 프로젝트"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\{#FileType}\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#AppExe},0"
Root: HKCU; Subkey: "Software\Classes\{#FileType}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#AppExe}"" ""%1"""

[InstallDelete]
; 0.1.8 renamed the product Recorder (가칭) -> Tally: the old executable, its parked copy (see PrepareToInstall)
; and the shortcuts of the old name (group folder + optional desktop shortcut) are not carried over by Inno.
Type: files; Name: "{app}\Recorder.exe"
Type: files; Name: "{app}\Recorder.exe.old"
Type: filesandordirs; Name: "{autoprograms}\Recorder (가칭)"
Type: files; Name: "{autodesktop}\Recorder (가칭).lnk"

[UninstallDelete]
Type: files; Name: "{app}\Recorder.exe"
Type: files; Name: "{app}\Recorder.exe.old"

[Run]
; also after a silent auto-update (WinSparkle runs Setup with /SILENT): the app comes back by itself.
; Scripted installs pass /NORUN=1 to keep it closed.
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall; Check: not NoRunRequested

; No project paths and no settings-folder deletion in install/update/uninstall.
; Community link remains the existing site policy; no new browser-launch task.

[Code]
const
  CoupangShortcutUrl = 'https://xn--jb0byyo90f.com/coupang/';   { 곰튀김.com }

{ WinSparkle starts Setup and only then asks the running 0.1.7 Recorder.exe to shut down, so the old image can still
  be mapped when [InstallDelete] runs. Retry the delete for a while; if the process is still alive, rename the file
  (Windows allows renaming a running executable) so no Recorder.exe remains for old shortcuts to launch, and the
  parked copy is removed by the next install/uninstall. }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Old, Parked: String;
  Attempt: Integer;
begin
  Result := '';
  Old := ExpandConstant('{app}\Recorder.exe');
  if not FileExists(Old) then
    Exit;
  for Attempt := 0 to 30 do
  begin
    if DeleteFile(Old) then
      Exit;
    Sleep(500);
  end;
  Parked := Old + '.old';
  DeleteFile(Parked);
  if RenameFile(Old, Parked) then
    Log('Recorder.exe was still running; parked it as Recorder.exe.old')
  else
    Log('Recorder.exe is still in use and could not be removed or parked');
end;

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
