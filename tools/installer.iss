; MeetMind 安装脚本（Inno Setup 6）
; 用法: ISCC.exe tools\installer.iss
; 前置: 已用 tools/build.sh gpu 构建出 build\bin
#define MyAppName "MeetMind"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Ray-Huan"
#define MyAppURL "https://github.com/Ray-Huan/MeetMind"
#define MyAppExeName "MeetMind.exe"

[Setup]
AppId={{7E8D3C1A-4F2B-4E9A-9C5D-2B3E8A1F6D4C}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=MeetMind-v{#MyAppVersion}-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayIcon={app}\{#MyAppExeName}
VersionInfoVersion={#MyAppVersion}
VersionInfoDescription=MeetMind - On-device meeting transcription & minutes
VersionInfoTextVersion={#MyAppVersion}

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional tasks:"

[Files]
Source: "..\build\bin\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\meetmind-cli.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\bin\platforms\*"; DestDir: "{app}\platforms"; Flags: recursesubdirs ignoreversion
Source: "..\build\bin\imageformats\*"; DestDir: "{app}\imageformats"; Flags: recursesubdirs ignoreversion
Source: "..\build\bin\iconengines\*"; DestDir: "{app}\iconengines"; Flags: recursesubdirs ignoreversion
Source: "..\build\bin\multimedia\*"; DestDir: "{app}\multimedia"; Flags: recursesubdirs ignoreversion
Source: "..\build\bin\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: recursesubdirs ignoreversion
Source: "..\build\bin\styles\*"; DestDir: "{app}\styles"; Flags: recursesubdirs ignoreversion
Source: "..\build\bin\tls\*"; DestDir: "{app}\tls"; Flags: recursesubdirs ignoreversion
Source: "..\data\*.txt"; DestDir: "{app}\data"; Flags: ignoreversion
Source: "..\config\meetmind.json"; DestDir: "{app}\config"; Flags: ignoreversion
Source: "assets\下载模型.bat"; DestDir: "{app}"; Flags: ignoreversion
Source: "assets\使用说明.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\models"
