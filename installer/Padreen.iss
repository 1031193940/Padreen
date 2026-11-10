; Padreen installer (Inno Setup 7).  Build:  powershell -ExecutionPolicy Bypass -File tools\make-installer.ps1
; Installs to D: by default (the user's rule: software on D:, not C:), needs administrator rights once for
;   - the Windows Firewall rules (inbound TCP 38702 / UDP 38703, local subnet only, this program only)
;   - the power helper service (padreen.exe --power-service): while the tablet display is in use it sets "closing the lid" to
;     "do nothing" and puts the original value back afterwards (changing it needs administrator rights).  Because that service runs
;     as SYSTEM, the program folder is locked down (only administrators may modify it); settings and logs then live in %LOCALAPPDATA%.
; Nothing else on the PC is touched.  The uninstaller removes what the installer added.
;
; The virtual display driver is NOT part of this installer: it is Parsec's software, and no permission to redistribute it has been found.  Padreen offers to
; fetch it from Parsec's own download address at its first start (one more UAC prompt, no script).  So the installer does not install it, and the
; uninstaller does not remove it (other programs may use it too): Device Manager > Display adapters > Parsec Virtual Display Adapter > Uninstall device.
;
; Padreen was called PaperDisplay (its tray program: PaperHost) before version 1.1.  Installing it replaces the old product: the old program,
; its service, its firewall rules and its shortcuts are removed first.  The settings and the paired tablets stay where they are (in
; %LOCALAPPDATA%\PaperDisplay) and are taken over by Padreen on its first start.  The virtual display driver is never removed by that step.

#define AppName "Padreen"
#define AppVersion "1.1.3"
#define Src "..\dist\Padreen"

[Setup]
AppId={{162D520D-0087-4422-96F0-E36B5AF01211}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Padreen
DefaultDirName={code:DefaultDir}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=Padreen-Setup-{#AppVersion}
SetupIconFile=..\assets\icon\app.ico
UninstallDisplayIcon={app}\padreen.exe
Compression=lzma2/max
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
VersionInfoVersion={#AppVersion}.0
VersionInfoDescription=Padreen Setup
VersionInfoProductName={#AppName}
CloseApplications=no
RestartApplications=no

[Languages]
Name: "chs"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Types]
Name: "full"; Description: "完整安装"
Name: "custom"; Description: "自定义"; Flags: iscustom

[Components]
Name: "main"; Description: "Padreen"; Types: full custom; Flags: fixed
Name: "usb"; Description: "USB 连接"; Types: full custom
Name: "power"; Description: "合盖不休眠（电源助手）"; Types: full custom

[Tasks]
Name: "firewall"; Description: "Wi-Fi 连接（防火墙规则）"; GroupDescription: "网络："
Name: "autostart"; Description: "开机启动"; GroupDescription: "启动："; Flags: unchecked
Name: "desktopicon"; Description: "桌面快捷方式"; GroupDescription: "快捷方式："; Flags: unchecked

[Files]
Source: "{#Src}\padreen.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "{#Src}\padreen.apk"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "{#Src}\padreen-white.png"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "{#Src}\padreen.ini.sample"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "{#Src}\使用说明.txt"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "{#Src}\adb\*"; DestDir: "{app}\adb"; Flags: ignoreversion recursesubdirs; Components: usb

[Icons]
Name: "{autoprograms}\{#AppName}\Padreen"; Filename: "{app}\padreen.exe"
Name: "{autoprograms}\{#AppName}\平板 App"; Filename: "{win}\explorer.exe"; Parameters: "/select,""{app}\padreen.apk"""
Name: "{autoprograms}\{#AppName}\使用说明"; Filename: "{app}\使用说明.txt"
Name: "{autoprograms}\{#AppName}\卸载 Padreen"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Padreen"; Filename: "{app}\padreen.exe"; Tasks: desktopicon

[Run]
; the power helper runs as SYSTEM from this folder: only administrators may modify it (users: read / execute)
Filename: "{sys}\icacls.exe"; Parameters: """{app}"" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX /C /Q"; Flags: runhidden; StatusMsg: "正在设置权限…"; Components: power
; the files inside re-inherit the folder permissions (applying /inheritance:r with /T would leave them without any permission)
Filename: "{sys}\icacls.exe"; Parameters: """{app}\*"" /reset /T /C /Q"; Flags: runhidden; Components: power
Filename: "{app}\padreen.exe"; Parameters: "--install-power-service"; Flags: runhidden waituntilterminated; StatusMsg: "正在安装电源助手…"; Components: power
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Padreen LAN TCP ({app}\padreen.exe)"""; Flags: runhidden; Tasks: firewall
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Padreen LAN UDP ({app}\padreen.exe)"""; Flags: runhidden; Tasks: firewall
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Padreen LAN TCP ({app}\padreen.exe)"" dir=in action=allow protocol=TCP localport=38702 program=""{app}\padreen.exe"" profile=any remoteip=localsubnet enable=yes"; Flags: runhidden; StatusMsg: "正在添加防火墙规则…"; Tasks: firewall
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""Padreen LAN UDP ({app}\padreen.exe)"" dir=in action=allow protocol=UDP localport=38703 program=""{app}\padreen.exe"" profile=any remoteip=localsubnet enable=yes"; Flags: runhidden; StatusMsg: "正在添加防火墙规则…"; Tasks: firewall
Filename: "{app}\padreen.exe"; Parameters: "--autostart-on"; Flags: runasoriginaluser runhidden; Tasks: autostart
Filename: "{app}\padreen.exe"; Description: "运行 Padreen"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\padreen.exe"; Parameters: "--autostart-off"; Flags: runhidden; RunOnceId: "AutostartOff"
; stops the helper service (which restores the lid action) and deletes it - before the files go away
Filename: "{app}\padreen.exe"; Parameters: "--remove-power-service"; Flags: runhidden waituntilterminated; RunOnceId: "RemovePower"; Check: PowerServiceInstalled
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Padreen LAN TCP ({app}\padreen.exe)"""; Flags: runhidden; RunOnceId: "DelFwTcp"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""Padreen LAN UDP ({app}\padreen.exe)"""; Flags: runhidden; RunOnceId: "DelFwUdp"

[UninstallDelete]
Type: files; Name: "{app}\*.ini"
Type: files; Name: "{app}\*.log"
Type: files; Name: "{app}\*.old"
Type: filesandordirs; Name: "{app}\driver"
Type: filesandordirs; Name: "{app}\driver-cache"
Type: dirifempty; Name: "{app}"

[Code]
function FindWindowW(lpClassName, lpWindowName: String): HWND; external 'FindWindowW@user32.dll stdcall';
function PostMessageW(hWnd: HWND; Msg: UINT; wParam: Longint; lParam: Longint): BOOL; external 'PostMessageW@user32.dll stdcall';

const
  // the uninstall entry of the old product (PaperDisplay 1.0.x, its Inno Setup AppId) and its per-user "start with Windows" value
  LegacyUninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6F1D3A52-8C4B-4E0F-9B7A-2D5E1C9A7F30}_is1';
  RunKey = 'Software\Microsoft\Windows\CurrentVersion\Run';

var
  LegacyDir: String;            // where the old PaperDisplay is installed ('' = it is not)
  LegacyAutostart: Boolean;     // the old program started with Windows (the choice moves over to Padreen)
  LegacyVddFlag: Boolean;       // the old installer had installed the virtual display driver: its flag is removed before the old uninstaller runs (it would remove the driver)

function DefaultDir(Param: String): String;
begin
  // the user's rule: programs go to D: (when there is a D: drive), never C: by default
  if DirExists('D:\') then Result := 'D:\Padreen'
  else Result := ExpandConstant('{autopf}\Padreen');
end;

// the power helper service runs padreen.exe: it must stop before the files are replaced (stopping it restores the lid action)
procedure StopPowerService(Name: String);
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\sc.exe'), 'stop ' + Name, '', SW_HIDE, ewWaitUntilTerminated, Code);
  Sleep(1500);
end;

function PowerServiceInstalled: Boolean;
var
  Code: Integer;
begin
  Result := Exec(ExpandConstant('{sys}\sc.exe'), 'query PadreenPower', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;

// Padreen starts its own adb server from <folder>\adb: that file stays in use (the upgrade could not replace it) until the server is stopped.
// Only a server that runs from OUR folder is stopped - another adb server (e.g. the Android SDK's) is none of our business.
procedure StopOwnAdb(Dir: String);
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    '-NoProfile -ExecutionPolicy Bypass -Command "Get-CimInstance Win32_Process -Filter \"Name=''adb.exe''\" | Where-Object { $_.ExecutablePath -like ''' + Dir + '\adb\*'' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"',
    '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

// quit a running tray program the polite way (it restores the wallpaper and unplugs the virtual monitor), then wait
procedure StopTrayProgram(WindowClass: String);
var
  H: HWND;
  I: Integer;
begin
  H := FindWindowW(WindowClass, '');
  if H = 0 then Exit;
  PostMessageW(H, $0111, 1007, 0);       // WM_COMMAND, IDM_QUIT
  for I := 1 to 60 do
  begin
    Sleep(200);
    if FindWindowW(WindowClass, '') = 0 then Exit;
  end;
end;

// Remove the old PaperDisplay (it was renamed Padreen): program, service, firewall rules, shortcuts.  Settings and pairings stay.
procedure RemoveLegacyProduct;
var
  Code, I: Integer;
  Unins, Data, Held: String;
  Moved: Boolean;
begin
  StopTrayProgram('PaperHostTray');
  if LegacyDir <> '' then StopOwnAdb(RemoveBackslash(LegacyDir));
  // the old power helper service (stopping it puts the lid action back)
  StopPowerService('PaperDisplayPower');
  Exec(ExpandConstant('{sys}\sc.exe'), 'delete PaperDisplayPower', '', SW_HIDE, ewWaitUntilTerminated, Code);
  // the old firewall rules: every copy of the old program (installed, portable, development) had its own pair
  Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    '-NoProfile -ExecutionPolicy Bypass -Command "Get-NetFirewallRule -DisplayName ''PaperDisplay LAN*'' -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue"',
    '', SW_HIDE, ewWaitUntilTerminated, Code);
  if LegacyDir = '' then Exit;
  // the driver must survive the old uninstaller (it only removes the driver when its own flag says so); Padreen itself never removes it
  if LegacyVddFlag then RegDeleteValue(HKLM, 'Software\PaperDisplay', 'VddInstalledByUs');
  Unins := AddBackslash(LegacyDir) + 'unins000.exe';
  if FileExists(Unins) then
  begin
    // the old uninstaller asks whether to delete the settings in AppData - and a silent run answers "yes": keep the folder out of its way meanwhile
    Data := ExpandConstant('{localappdata}\PaperDisplay');
    Held := Data + '.keep';
    Moved := False;
    if DirExists(Held) and not DirExists(Data) then RenameFile(Held, Data);       // an interrupted earlier run
    if DirExists(Data) and not DirExists(Held) then Moved := RenameFile(Data, Held);
    Exec(Unins, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-', '', SW_HIDE, ewWaitUntilTerminated, Code);
    for I := 1 to 150 do                   // the uninstaller may finish in a second process: wait until its entry is gone
    begin
      if not RegKeyExists(HKLM, LegacyUninstallKey) then Break;
      Sleep(400);
    end;
    if Moved then
    begin
      if DirExists(Data) then DelTree(Data, True, True, True);
      RenameFile(Held, Data);
    end;
  end;
  DelTree(LegacyDir, True, True, True);      // whatever is left (a file that is still in use stays until the next restart)
end;

function InitializeSetup: Boolean;
begin
  Result := True;
  LegacyDir := '';
  if not RegQueryStringValue(HKLM, LegacyUninstallKey, 'Inno Setup: App Path', LegacyDir) then LegacyDir := '';
  LegacyAutostart := RegValueExists(HKCU, RunKey, 'PaperDisplay');
  LegacyVddFlag := RegValueExists(HKLM, 'Software\PaperDisplay', 'VddInstalledByUs');
  if LegacyDir <> '' then
    MsgBox('将先卸载旧版 PaperDisplay（已更名为 Padreen），设置和配对记录会保留。', mbInformation, MB_OK);
end;

procedure InitializeWizard;
begin
  if LegacyAutostart then WizardSelectTasks('autostart');      // the old program started with Windows: so does the new one
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  StopTrayProgram('PadreenTray');
  StopOwnAdb(ExpandConstant('{app}'));
  StopPowerService('PadreenPower');
  RemoveLegacyProduct;
  // an earlier install may have left the folder locked (or damaged): let the files inherit the folder permissions again so they can be replaced
  if DirExists(ExpandConstant('{app}')) then
    Exec(ExpandConstant('{sys}\icacls.exe'), '"' + ExpandConstant('{app}') + '\*" /reset /T /C /Q', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Result := '';
end;

function InitializeUninstall: Boolean;
begin
  StopTrayProgram('PadreenTray');
  StopOwnAdb(ExpandConstant('{app}'));
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    if DirExists(ExpandConstant('{localappdata}\Padreen')) then
      if MsgBox('同时删除设置和配对记录？', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(ExpandConstant('{localappdata}\Padreen'), True, True, True);
end;
