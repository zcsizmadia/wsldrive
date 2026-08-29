; wsldrive GUI installer (Inno Setup 6).
;
; A thin front-end over scripts\install.ps1: it collects the same choices through
; a wizard, lays the binaries into Program Files, then hands off to the script
; (install.ps1 -Unattended ...) which does the real work (WinFsp check, hvsocket
; registration, scheduled tasks, optional WSL restart). The script stays the
; single source of truth and remains usable on its own for people who can't or
; won't run a Windows installer.
;
; Build:  scripts\build-installer.ps1   (compiles this with ISCC; see that script)
; The binaries must be built first; paths are passed in as /D defines by the build
; script, with sensible defaults below so the .iss also opens in the Inno IDE.

#ifndef BinDir
  #define BinDir "..\build\msvc-release\src\tools"
#endif
#ifndef LinuxBin
  #define LinuxBin "..\build\linux-release\src\tools\wsldrive"
#endif
#ifndef LinuxAgent
  #define LinuxAgent "..\build\linux-release\src\tools\wsldrived"
#endif
#ifndef AppVersion
  #define AppVersion "0.9.0"
#endif
#define AppName "wsldrive"

[Setup]
AppId={{7C3E9A2E-3B7B-4E2C-9A1D-2F4B6D8E0A11}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=wsldrive
DefaultDirName={autopf}\wsldrive
DefaultGroupName=wsldrive
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\wsldrive.exe
LicenseFile=..\LICENSE
OutputBaseFilename=wsldrive-setup
OutputDir=dist
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible

[Files]
Source: "{#BinDir}\wsldrive.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\wsldrived.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BinDir}\wsldrivew.exe"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "..\scripts\install.ps1";          DestDir: "{app}\scripts"; Flags: ignoreversion
Source: "..\scripts\register-hvsocket.ps1"; DestDir: "{app}\scripts"; Flags: ignoreversion
Source: "..\README.md";    DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "..\CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";   DestDir: "{app}"; Flags: ignoreversion
; The Linux binaries are optional; include them if built. wsldrive-linux is the
; client (Direction B); wsldrived-linux is the agent that serves the WSL tree (Direction A).
Source: "{#LinuxBin}";   DestDir: "{app}"; DestName: "wsldrive-linux";  Flags: ignoreversion skipifsourcedoesntexist
Source: "{#LinuxAgent}"; DestDir: "{app}"; DestName: "wsldrived-linux"; Flags: ignoreversion skipifsourcedoesntexist

[Code]
var
  PageChoices: TInputOptionWizardPage;   { 0=Dir A  1=Dir B  2=hvsocket  3=restart WSL }
  PageA: TInputQueryWizardPage;          { 0=drive letter  1=distro  2=folder }
  PageB: TInputQueryWizardPage;          { 0=Windows folder  1=mount point  2=distro }

{ Run a command line, capture stdout as a string (via a temp file). }
function RunCapture(const CmdLine: string): string;
var
  Tmp: string;
  Content: AnsiString;   // LoadStringFromFile takes an AnsiString (var), not String
  Res: Integer;
begin
  Result := '';
  Tmp := ExpandConstant('{tmp}\wsld_out.txt');
  if Exec(ExpandConstant('{cmd}'), '/C ' + CmdLine + ' > "' + Tmp + '" 2>NUL',
          '', SW_HIDE, ewWaitUntilTerminated, Res) then
    if LoadStringFromFile(Tmp, Content) then
      Result := String(Content);
end;

function DefaultDistro(): string;
var
  S: string;
  P: Integer;
begin
  { `wsl.exe -l -q` (WSL_UTF8 set) lists distros, default first. }
  S := RunCapture('set WSL_UTF8=1 && wsl.exe -l -q');
  { take the first non-empty line }
  Result := '';
  while (S <> '') do
  begin
    P := Pos(#10, S);
    if P = 0 then P := Length(S) + 1;
    Result := Trim(Copy(S, 1, P - 1));
    { strip stray CRs / NULs that survive from UTF-16 sources }
    StringChangeEx(Result, #13, '', True);
    StringChangeEx(Result, #0, '', True);
    if Result <> '' then Break;
    S := Copy(S, P + 1, Length(S));
  end;
end;

{ Direction A needs the WinFsp runtime. install.ps1 checks too, but from this
  wizard its console closes before the message can be read, so say it here,
  before anything is copied. }
function InitializeSetup(): Boolean;
var
  Dir: string;
begin
  Result := True;
  if not RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\WinFsp', 'InstallDir', Dir) then
  begin
    if MsgBox('wsldrive needs WinFsp (https://winfsp.dev) to mount a WSL folder as a drive letter, '
              + 'and it is not installed.' + #13#10#13#10
              + 'Install WinFsp first (the free runtime is enough), then run this setup again.' + #13#10#13#10
              + 'Continue anyway? Only the advanced Direction B (a Windows folder inside WSL) will work.',
              mbConfirmation, MB_YESNO) = IDNO then
      Result := False;
  end;
end;

procedure InitializeWizard();
var
  distro: string;
begin
  PageChoices := CreateInputOptionPage(wpWelcome,
    'What should wsldrive set up?',
    'Pick the directions to mount. You can re-run this installer to change them.',
    'Each selected direction is mounted automatically at every logon.', False, False);
  PageChoices.Add('Mount a WSL folder as a Windows drive letter (recommended)');
  PageChoices.Add('Advanced: also mount a Windows folder inside WSL (Direction B)');
  PageChoices.Add('Use Hyper-V sockets for Direction B (recommended)');
  PageChoices.Add('Restart WSL now if Direction B needs it (closes running WSL sessions)');
  PageChoices.Values[0] := True;   { Direction A on by default (easy mode) }
  PageChoices.Values[2] := True;
  PageChoices.Values[3] := True;

  distro := DefaultDistro();
  if distro = '' then distro := 'Ubuntu';

  PageA := CreateInputQueryPage(PageChoices.ID,
    'Direction A — WSL folder as a drive', 'Choose the drive and folder.',
    'wsldrive will map the WSL folder below to the drive letter.');
  PageA.Add('Drive letter (e.g. W):', False);
  PageA.Add('WSL distro:', False);
  PageA.Add('Folder inside the distro (~ = home, / = whole distro):', False);
  PageA.Values[0] := 'W';
  PageA.Values[1] := distro;
  PageA.Values[2] := '~';

  PageB := CreateInputQueryPage(PageA.ID,
    'Direction B — Windows folder in WSL', 'Choose the folder and mount point.',
    'wsldrive will mount the Windows folder below inside the distro.');
  PageB.Add('Windows folder (e.g. C:\projects):', False);
  PageB.Add('Mount point inside the distro:', False);
  PageB.Add('WSL distro:', False);
  PageB.Values[0] := '';
  PageB.Values[1] := '~/win';
  PageB.Values[2] := distro;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if PageID = PageA.ID then Result := not PageChoices.Values[0];
  if PageID = PageB.ID then Result := not PageChoices.Values[1];
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  dl: string;
begin
  Result := True;
  if CurPageID = PageChoices.ID then
  begin
    if (not PageChoices.Values[0]) and (not PageChoices.Values[1]) then
    begin
      MsgBox('Select at least one direction to mount.', mbError, MB_OK);
      Result := False;
    end;
  end
  else if CurPageID = PageA.ID then
  begin
    dl := Uppercase(Trim(PageA.Values[0]));
    StringChangeEx(dl, ':', '', True);
    if (Length(dl) <> 1) or (dl < 'A') or (dl > 'Z') then
    begin
      MsgBox('Enter a single drive letter A-Z.', mbError, MB_OK);
      Result := False;
    end;
  end
  else if CurPageID = PageB.ID then
  begin
    if Trim(PageB.Values[0]) = '' then
    begin
      MsgBox('Enter the Windows folder to expose.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

{ Build the install.ps1 argument line from the wizard choices. }
function BuildArgs(): string;
var
  a, dl, distro: string;
begin
  a := '-ExecutionPolicy Bypass -File "' + ExpandConstant('{app}\scripts\install.ps1') + '"' +
       ' -Unattended -Yes -NoShutdown' +
       ' -InstallDir "' + ExpandConstant('{app}') + '"' +
       ' -BinDir "' + ExpandConstant('{app}') + '"';
  if not PageChoices.Values[2] then a := a + ' -NoHvsocket';

  { install.ps1 uses one -Distro for both directions; pass it exactly once. }
  if PageChoices.Values[0] then distro := Trim(PageA.Values[1]) else distro := Trim(PageB.Values[2]);
  a := a + ' -Distro "' + distro + '"';

  if PageChoices.Values[0] then
  begin
    dl := Uppercase(Trim(PageA.Values[0])); StringChangeEx(dl, ':', '', True);
    a := a + ' -DriveLetter ' + dl + ' -WslRoot "' + Trim(PageA.Values[2]) + '"';
    if FileExists(ExpandConstant('{app}\wsldrived-linux')) then
      a := a + ' -LinuxAgent "' + ExpandConstant('{app}\wsldrived-linux') + '"';
  end;
  if PageChoices.Values[1] then
  begin
    a := a + ' -WinRoot "' + Trim(PageB.Values[0]) + '"' +
             ' -Mountpoint "' + Trim(PageB.Values[1]) + '"';
    if FileExists(ExpandConstant('{app}\wsldrive-linux')) then
      a := a + ' -LinuxBin "' + ExpandConstant('{app}\wsldrive-linux') + '"';
  end;
  Result := a;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Res: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    { Hand off to the script: it copies binaries, stages the Linux agent/client
      into WSL, registers hvsocket (Direction B), and creates the logon tasks. }
    if not Exec('powershell.exe', BuildArgs(), '', SW_SHOW, ewWaitUntilTerminated, Res) then
      MsgBox('Setup could not run install.ps1.', mbError, MB_OK)
    else if Res <> 0 then
      MsgBox('install.ps1 exited with code ' + IntToStr(Res) + '. The drive was not set up.' + #13#10#13#10
             + 'Details are in ' + ExpandConstant('{%TEMP}') + '\wsldrive-install.log', mbError, MB_OK);

    { A WSL restart is only needed for Direction B over hvsocket (Direction A uses
      loopback TCP). Offer it only then. }
    if PageChoices.Values[1] and PageChoices.Values[2] and PageChoices.Values[3] then
    begin
      if MsgBox('Restart WSL now so the Hyper-V socket transport takes effect? '
                + 'This closes running WSL sessions.', mbConfirmation, MB_YESNO) = IDYES then
        Exec(ExpandConstant('{cmd}'), '/C wsl.exe --shutdown', '', SW_HIDE, ewWaitUntilTerminated, Res);
    end;
  end;
end;

[Run]
Filename: "{app}\wsldrive.exe"; Parameters: "doctor"; Description: "Check the wsldrive environment"; Flags: postinstall runascurrentuser skipifsilent

[UninstallRun]
Filename: "powershell.exe"; \
  Parameters: "-ExecutionPolicy Bypass -File ""{app}\scripts\install.ps1"" -Uninstall -Unattended -InstallDir ""{app}"""; \
  Flags: runhidden; RunOnceId: "wsldriveuninstall"
