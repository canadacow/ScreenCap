; ScreenCap Inno Setup installer script
; Build with: "C:\Program Files (x86)\Inno Setup 6\iscc.exe" installer\ScreenCap.iss

#define MyAppName "ScreenCap"
#define MyAppVersion "1.0"
#define MyAppPublisher "Dean Beeler"
#define MyAppExeName "ScreenCap.exe"
#define MyAppURL "https://github.com/canadacow/ScreenCap"

[Setup]
AppId={{E7A3B2F1-4C5D-4E6F-8A9B-0C1D2E3F4A5B}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=Output
OutputBaseFilename=ScreenCapSetup
Compression=lzma2/ultra64
SolidCompression=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}
WizardStyle=modern
CloseApplications=force

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startup"; Description: "Launch {#MyAppName} on Windows startup"; Flags: checkedonce

[Files]
Source: "..\out\build\vs2022-x64\RelWithDebInfo\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; AppUserModelID: "{#MyAppName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
; Start Menu shortcut with AUMID (required for toast notifications)
Name: "{userprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; AppUserModelID: "{#MyAppName}"

[Registry]
; Auto-start on login (only if the user checked the startup task)
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
  ValueType: string; ValueName: "{#MyAppName}"; ValueData: """{app}\{#MyAppExeName}"""; \
  Flags: uninsdeletevalue; Tasks: startup

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; \
  Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Clean up toast thumbnail temp file
Type: files; Name: "{tmp}\ScreenCap_thumb.png"

[UninstallRun]
; Kill the app before uninstalling
Filename: "taskkill"; Parameters: "/IM {#MyAppExeName} /F"; Flags: runhidden; RunOnceId: "KillApp"
