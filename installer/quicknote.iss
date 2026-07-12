; QuickNote installer (Inno Setup 6).
; Compile via `make installer` — it passes /DAppVersion="x.y.z" from the
; Makefile VERSION variable (single source of truth).
#ifndef AppVersion
  #error Pass /DAppVersion="x.y.z" (use `make installer`)
#endif

[Setup]
; AppId is stable across releases so upgrades replace in place.
; NEVER regenerate this GUID.
AppId={{ADFF8E05-0AD8-497C-A956-AD109ED260A8}
AppName=QuickNote
AppVersion={#AppVersion}
AppPublisher=Marcus Miccelli
VersionInfoVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\QuickNote
; Per-user install: no UAC, works for any downloader.
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
; Must exactly match the single-instance mutex in src/main.c — Setup and
; the uninstaller prompt the user to close a running QuickNote (a tray app
; that Restart Manager alone cannot reliably close).
AppMutex=quickNote_SingleInstance_Mutex
CloseApplications=yes
OutputDir=..\dist
OutputBaseFilename=quicknote-setup-{#AppVersion}
SetupIconFile=..\assets\quicknote.ico
UninstallDisplayIcon={app}\quicknote.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Tasks]
; No `unchecked` flag => checked by default.
Name: "startupicon"; Description: "Start QuickNote when Windows starts"

[Files]
Source: "..\quicknote.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
; Start-menu search indexes {userprograms}; this single shortcut is the
; "registration" that makes QuickNote findable from the Start menu.
Name: "{userprograms}\QuickNote"; Filename: "{app}\quicknote.exe"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
  ValueType: string; ValueName: "QuickNote"; \
  ValueData: """{app}\quicknote.exe"""; \
  Tasks: startupicon; Flags: uninsdeletevalue

[Run]
Filename: "{app}\quicknote.exe"; Description: "Launch QuickNote"; \
  Flags: postinstall nowait skipifsilent
