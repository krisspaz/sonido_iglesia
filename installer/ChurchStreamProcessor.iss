#define MyAppName "Church Stream Processor"
#define MyAppVersion "0.1.0"
#define MyAppExeName "Church Stream Processor.exe"

[Setup]
AppId={{B8B9EB65-4178-48D3-83D0-5FC4C42910B5}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=Church Stream Processor
DefaultDirName={autopf}\Church Stream Processor
DefaultGroupName=Church Stream Processor
OutputDir=..\dist\installer
OutputBaseFilename=ChurchStreamProcessor-{#MyAppVersion}-Windows-x64
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\{#MyAppExeName}

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "autostart"; Description: "Start with Windows"; GroupDescription: "Startup:"; Flags: unchecked
Name: "startminimized"; Description: "Start minimized in the system tray"; GroupDescription: "Startup:"; Flags: unchecked

[Files]
Source: "..\dist\app\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\dist\app\ChurchStreamProcessorBenchmark.exe"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "uninstall-virtual-driver.ps1"; DestDir: "{app}\driver"; Flags: ignoreversion
#if DirExists("..\dist\driver")
Source: "..\dist\driver\*"; DestDir: "{app}\driver"; Flags: ignoreversion recursesubdirs createallsubdirs
#endif

[Icons]
Name: "{group}\Church Stream Processor"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\Church Stream Processor"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "ChurchStreamProcessor"; ValueData: """{app}\{#MyAppExeName}"""; Tasks: autostart and not startminimized; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "ChurchStreamProcessor"; ValueData: """{app}\{#MyAppExeName}"" --minimized"; Tasks: autostart and startminimized; Flags: uninsdeletevalue

[Run]
Filename: "{sys}\pnputil.exe"; Parameters: "/add-driver ""{app}\driver\ChurchStreamVirtual.inf"" /install"; StatusMsg: "Installing the local virtual audio endpoint..."; Flags: runhidden waituntilterminated; Check: FileExists(ExpandConstant('{app}\driver\ChurchStreamVirtual.inf'))
Filename: "{app}\{#MyAppExeName}"; Description: "Launch Church Stream Processor"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/IM ""{#MyAppExeName}"" /T /F"; Flags: runhidden waituntilterminated; RunOnceId: "StopChurchStreamProcessor"
Filename: "{sysnative}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\driver\uninstall-virtual-driver.ps1"""; Flags: runhidden waituntilterminated; RunOnceId: "RemoveChurchStreamVirtual"

[UninstallDelete]
Type: filesandordirs; Name: "{userappdata}\ChurchStreamProcessor"
Type: dirifempty; Name: "{app}"
