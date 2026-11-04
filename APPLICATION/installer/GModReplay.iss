#ifndef PayloadDir
  #error PayloadDir must point to the staged runtime files.
#endif
#ifndef ReleaseDir
  #error ReleaseDir must point to the installer output directory.
#endif
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
; Edition=public builds the public edition: its own AppId, folder, mutex and file type, so both
; editions can be installed side by side.
#ifndef Edition
  #define Edition "private"
#endif
#if Edition == "public"
  #define AppIdGuid "{{4C3BB5DC-CA78-4F6E-AB97-A52BE2C737B5}"
  #define ProductName "GMod Replay Public"
  #define MutexName "Local\GModReplayPublic.Application"
  #define ProgId "GModReplayPublic.Demo"
  #define SetupPrefix "GModReplay-Public-Setup"
  #define GuideFile "PublicGuide.txt"
  #define GuideName "Guide"
#else
  #define AppIdGuid "{{E6D15476-975D-4D0E-A7DE-0C191CF384E8}"
  #define ProductName "GMod Replay"
  #define MutexName "Local\GModReplay.Application"
  #define ProgId "GModReplay.Demo"
  #define SetupPrefix "GModReplay-Setup"
  #define GuideFile "TesterGuide.txt"
  #define GuideName "Testing guide"
#endif

[Setup]
AppId={#AppIdGuid}
AppName={#ProductName}
AppVersion={#AppVersion}
AppMutex={#MutexName}
AppVerName={#ProductName} {#AppVersion} (Test Build)
DefaultDirName={localappdata}\Programs\{#ProductName}
DefaultGroupName={#ProductName}
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
DisableProgramGroupPage=yes
WizardStyle=modern
OutputDir={#ReleaseDir}
OutputBaseFilename={#SetupPrefix}-{#AppVersion}-win-x64
Compression=lzma2
SolidCompression=yes
SetupIconFile={#SourcePath}\..\assets\windows\GModReplay.ico
UninstallDisplayIcon={app}\GModReplay.exe
InfoBeforeFile={#GuideFile}
CloseApplications=yes
RestartApplications=no
ChangesAssociations=yes
UsePreviousAppDir=yes
UsePreviousTasks=yes
UsePreviousGroup=yes

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Registry]
Root: HKCU; Subkey: "Software\Classes\{#ProgId}"; ValueType: string; ValueName: ""; ValueData: "{#ProductName} demo"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\{#ProgId}\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\GModReplay.exe,0"
Root: HKCU; Subkey: "Software\Classes\{#ProgId}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\GModReplay.exe"" ""%1"""
; Both editions ship GModReplay.exe, so only the private one registers the shared Applications key.
#if Edition != "public"
Root: HKCU; Subkey: "Software\Classes\Applications\GModReplay.exe"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "{#ProductName}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Applications\GModReplay.exe\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\GModReplay.exe"" ""%1"""
Root: HKCU; Subkey: "Software\Classes\Applications\GModReplay.exe\SupportedTypes"; ValueType: none; ValueName: ".dem"
#endif
Root: HKCU; Subkey: "Software\Classes\.dem\OpenWithProgids"; ValueType: none; ValueName: "{#ProgId}"; Flags: uninsdeletevalue

[Icons]
Name: "{group}\{#ProductName}"; Filename: "{app}\GModReplay.exe"; WorkingDir: "{app}"
Name: "{group}\{#GuideName}"; Filename: "{app}\{#GuideFile}"
Name: "{autodesktop}\{#ProductName}"; Filename: "{app}\GModReplay.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\GModReplay.exe"; Description: "Launch {#ProductName}"; Flags: nowait postinstall skipifsilent
