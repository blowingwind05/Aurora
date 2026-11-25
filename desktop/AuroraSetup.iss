#define ViGEmBusInstaller "ViGEmBus_1.22.0_x64_x86_arm64.exe"
#define ViGEmBusSha256 "89220A7865076B342892F98865F3499FB7C4CFD673159E89D352C360FD014C6A"

[Setup]
AppId={{2EBC707A-8C2F-45C4-9F9C-BAD150CB483B}
AppName=Aurora
AppVersion={#AuroraVersion}
VersionInfoVersion={#AuroraVersion}
DefaultDirName={autopf}\Aurora
DisableDirPage=no
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\Aurora.exe
SetupIconFile={#AuroraIcon}
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
PrivilegesRequired=admin
AlwaysRestart=no
RestartIfNeededByRun=no
SetupLogging=yes
OutputBaseFilename=AuroraSetup
WizardStyle=modern
Compression=lzma2/fast
SolidCompression=yes

[Files]
Source: "{#AuroraSourceDir}\Components\ViGEmBus\{#ViGEmBusInstaller}"; DestDir: "{app}\Components\ViGEmBus"; Flags: ignoreversion
Source: "{#AuroraSourceDir}\*"; DestDir: "{app}"; Excludes: "\Components\ViGEmBus\{#ViGEmBusInstaller}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Aurora"; Filename: "{app}\Aurora.exe"; WorkingDir: "{app}"

[Code]
const
  ServiceNotFound = 1060;
  RestartRequired = 3010;

var
  GamepadRestartPending: Boolean;

function UpdateReadyMemo(const Space, NewLine, MemoUserInfoInfo, MemoDirInfo,
  MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo + NewLine + NewLine +
    'ViGEmBus virtual gamepad driver will be installed if it is missing.';
end;

function QueryGamepadDriver(var Code: Integer): Boolean;
begin
  Result := Exec(ExpandConstant('{sys}\sc.exe'), 'query ViGEmBus', '',
    SW_HIDE, ewWaitUntilTerminated, Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
  Installer: String;
begin
  Result := '';
  if not QueryGamepadDriver(Code) then
  begin
    Result := 'Could not check the ViGEmBus driver: ' + SysErrorMessage(Code);
    Exit;
  end;
  if Code = 0 then
  begin
    Log('ViGEmBus is already installed.');
    Exit;
  end;
  if Code <> ServiceNotFound then
  begin
    Result := Format('Could not determine whether ViGEmBus is installed (Windows error %d).', [Code]);
    Exit;
  end;

  try
    ExtractTemporaryFile('{#ViGEmBusInstaller}');
    Installer := ExpandConstant('{tmp}\{#ViGEmBusInstaller}');
    if CompareText(GetSHA256OfFile(Installer), '{#ViGEmBusSha256}') <> 0 then
    begin
      Result := 'The ViGEmBus installer failed SHA-256 verification.';
      Exit;
    end;
  except
    Result := 'Could not prepare the ViGEmBus installer: ' + GetExceptionMessage;
    Exit;
  end;

  Log('Installing the bundled ViGEmBus driver.');
  if not Exec(Installer, '/exenoui /qn /norestart', ExpandConstant('{tmp}'),
    SW_HIDE, ewWaitUntilTerminated, Code) then
    Result := 'Could not start the ViGEmBus installer: ' + SysErrorMessage(Code)
  else if Code = RestartRequired then
  begin
    GamepadRestartPending := True;
    Log('ViGEmBus requires a Windows restart before it can be used.');
  end
  else if Code <> 0 then
    Result := Format('ViGEmBus installation failed (exit code %d).', [Code])
  else if not QueryGamepadDriver(Code) or (Code <> 0) then
    Result := Format('The ViGEmBus installer finished, but Windows could not confirm the driver (error %d).', [Code]);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and GamepadRestartPending then
    WizardForm.FinishedLabel.Caption := WizardForm.FinishedLabel.Caption + #13#10#13#10 +
      'Restart Windows when convenient to use the virtual gamepad.';
end;

function GetCustomSetupExitCode: Integer;
begin
  Result := 0;
  if GamepadRestartPending then
    Result := RestartRequired;
end;
