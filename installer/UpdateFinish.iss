// Shared automatic-update and Coupang shortcut handling for Enqueue, LiveMix and Tally.
// Included inside [Code]. Keep this file UTF-8 with BOM.
const
  CoupangShortcutUrl = 'https://xn--jb0byyo90f.com/coupang/';   { 곰튀김.com }
  { Enqueue 0.9.0-0.9.3 wrote this address; it died with the repository rename (GitHub Pages keeps no redirect). }
  OldCoupangShortcutUrl = 'https://dnakrhs2-crypto.github.io/gocue/coupang/';

{ WinSparkle runs Setup with /SILENT /SP- /NORESTART /AUTOUPDATE=1 (tools/release.py writes the appcast): only the
  progress window shows, the running app is closed by Setup, and the app comes back by itself at the end. }
function IsAutoUpdate: Boolean;
begin
  Result := ExpandConstant('{param:AUTOUPDATE|0}') = '1';
end;

function IsVerySilent: Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/VERYSILENT') = 0 then
    begin
      Result := True;
      Exit;
    end;
end;

function NoRunRequested: Boolean;
begin
  Result := ExpandConstant('{param:NORUN|0}') = '1';
end;

function CoupangShortcutPath: String;
begin
  Result := ExpandConstant('{userdesktop}\') + CustomMessage('CoupangShortcutName') + '.url';
end;

function CoupangShortcutExists: Boolean;
begin
  { Either name may belong to another installer (Bandizip writes 쿠팡.url too): never replaced, never asked again.
    The all-users desktop shows on the user's desktop as well, so a shortcut there counts too. }
  Result := FileExists(ExpandConstant('{userdesktop}\쿠팡.url'))
    or FileExists(ExpandConstant('{userdesktop}\Coupang.url'))
    or FileExists(ExpandConstant('{commondesktop}\쿠팡.url'))
    or FileExists(ExpandConstant('{commondesktop}\Coupang.url'));
end;

{ The installer's own task: written when the user saw the checkbox (an interactive install) or asked for it on the
  command line (/TASKS=coupang). An automatic update never uses the remembered task - it asks in its own window. }
function CoupangShortcutWanted: Boolean;
begin
  Result := (not IsAutoUpdate) and WizardIsTaskSelected('coupang')
    and ((not WizardSilent) or (Pos('coupang', Lowercase(ExpandConstant('{param:TASKS|}'))) > 0));
end;

function WriteCoupangShortcut(const Path: String): Boolean;
begin
  Result := SetIniString('InternetShortcut', 'URL', CoupangShortcutUrl, Path)
    and SetIniString('InternetShortcut', 'IconFile', ExpandConstant('{app}\coupang.ico'), Path)
    and SetIniString('InternetShortcut', 'IconIndex', '0', Path);
end;

{ Written in the temporary folder and copied in with FailIfExists: a shortcut that appeared meanwhile is never
  overwritten. }
procedure CreateCoupangShortcutOnce;
var
  TempPath: String;
begin
  if CoupangShortcutExists then
    Exit;
  TempPath := ExpandConstant('{tmp}\coupang-shortcut.url');
  if WriteCoupangShortcut(TempPath) then
  begin
    if not CoupangShortcutExists then
      if not FileCopy(TempPath, CoupangShortcutPath, True) then
        Log('Could not create the Coupang desktop shortcut (existing files are preserved).');
  end
  else
    Log('Could not prepare the Coupang desktop shortcut.');
  DeleteFile(TempPath);
end;

{ After an automatic update, only when the desktop has no Coupang shortcut: a small "update complete" window -
  the question (ticked), the disclosure under it, and 완료. Closing it with X creates nothing. It shows before the
  postinstall [Run] entries, so the app comes back after 완료. }
procedure AskCoupangAfterUpdate;
var
  Form: TSetupForm;
  Done: TNewStaticText;
  Box: TNewCheckBox;
  Note: TNewStaticText;
  Finish: TNewButton;
  Margin, Y: Integer;
begin
  Margin := ScaleX(16);
  { Unscaled on purpose: CreateCustomForm scales the size itself (InitializeFont), so ScaleX here would scale twice
    on a high-DPI screen. The controls below are placed from the form's final ClientWidth. }
  Form := CreateCustomForm(460, 150, False, True);
  try
    Form.Caption := FmtMessage(CustomMessage('UpdateDoneTitle'), ['{#AppName}']);

    Done := TNewStaticText.Create(Form);
    Done.Parent := Form;
    Done.AutoSize := False;
    Done.WordWrap := True;
    Done.Left := Margin;
    Done.Top := ScaleY(16);
    Done.Width := Form.ClientWidth - 2 * Margin;
    Done.Caption := FmtMessage(CustomMessage('UpdateDoneText'), ['{#AppName}', '{#AppVersion}']);
    Done.AdjustHeight;
    Y := Done.Top + Done.Height + ScaleY(14);

    Box := TNewCheckBox.Create(Form);
    Box.Parent := Form;
    Box.Left := Margin;
    Box.Top := Y;
    Box.Width := Form.ClientWidth - 2 * Margin;
    Box.Height := ScaleY(20);
    Box.Caption := CustomMessage('CoupangFinishAsk');
    Box.Checked := True;
    Y := Box.Top + Box.Height + ScaleY(2);

    Note := TNewStaticText.Create(Form);
    Note.Parent := Form;
    Note.AutoSize := False;
    Note.WordWrap := True;
    Note.Left := Margin + ScaleX(18);
    Note.Top := Y;
    Note.Width := Form.ClientWidth - Note.Left - Margin;
    Note.Caption := CustomMessage('CoupangFinishNote');
    Note.AdjustHeight;
    Y := Note.Top + Note.Height + ScaleY(18);

    Finish := TNewButton.Create(Form);
    Finish.Parent := Form;
    Finish.Caption := CustomMessage('UpdateDoneButton');
    Finish.Width := Form.CalculateButtonWidth([Finish.Caption]);
    Finish.Height := ScaleY(23);
    Finish.Left := Form.ClientWidth - Finish.Width - Margin;
    Finish.Top := Y;
    Finish.ModalResult := mrOk;
    Finish.Default := True;

    Form.ClientHeight := Finish.Top + Finish.Height + ScaleY(14);
    Form.ActiveControl := Finish;

    if (Form.ShowModal = mrOk) and Box.Checked then
      CreateCoupangShortcutOnce;
  finally
    Form.Free;
  end;
end;

{ Our own shortcut from 0.9.0-0.9.3 still points at the dead address: point it at the live one. Only a file whose URL
  is exactly the old address is touched (another installer's 쿠팡.url never is), and nothing is created. }
procedure RepairOldCoupangShortcut(const Path: String);
begin
  if FileExists(Path) and (GetIniString('InternetShortcut', 'URL', '', Path) = OldCoupangShortcutUrl) then
    if not SetIniString('InternetShortcut', 'URL', CoupangShortcutUrl, Path) then
      Log('Could not repair the old Coupang desktop shortcut: ' + Path);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep <> ssPostInstall then
    Exit;
  RepairOldCoupangShortcut(ExpandConstant('{userdesktop}\쿠팡.url'));
  RepairOldCoupangShortcut(ExpandConstant('{userdesktop}\Coupang.url'));
  if CoupangShortcutWanted then
    WriteCoupangShortcut(CoupangShortcutPath)
  else if IsAutoUpdate and WizardSilent and (not IsVerySilent) and (not CoupangShortcutExists) then
    AskCoupangAfterUpdate;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  OtherPath: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    DeleteFile(CoupangShortcutPath);
    OtherPath := ExpandConstant('{userdesktop}\') + CustomMessage('CoupangOtherShortcutName') + '.url';
    if (OtherPath <> CoupangShortcutPath)
      and (GetIniString('InternetShortcut', 'URL', '', OtherPath) = CoupangShortcutUrl) then
      DeleteFile(OtherPath);
  end;
end;
