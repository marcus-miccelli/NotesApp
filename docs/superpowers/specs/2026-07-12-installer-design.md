# Design: QuickNote Windows installer (Inno Setup)

Date: 2026-07-12
Status: Approved

## Context

QuickNote (`quicknote.exe`) is a self-contained Win32 exe — verified via
`objdump -p`: it imports only system DLLs (COMCTL32, d2d1, dwmapi, DWrite,
GDI32, KERNEL32, msvcrt, SHELL32, USER32), no MSYS2 runtime. The installer
therefore ships exactly one file plus shortcuts and registry entries.

Goals (user-stated):
- Proper installer, downloadable from GitHub by anyone.
- App registered so it is **searchable from the Windows Start menu**.

Decisions made during brainstorming:
- **Tech: Inno Setup 6** (free; the VS Code pattern). One `.iss` script in
  the repo compiles to a single `setup.exe` via the `ISCC.exe` CLI.
- **Per-user install** (`PrivilegesRequired=lowest`) — no admin/UAC prompt,
  works for any downloader.
- **Unsigned** — SmartScreen will warn on first run ("More info → Run
  anyway"); documented in README. Code signing deferred (cost) unless
  demand appears.
- **Autostart**: optional installer checkbox, default checked.
- **Publishing: local `make installer` target** + manual
  `gh release create`. No CI (deferred; possible follow-up).

## Components

### 1. `installer/quicknote.iss`

Inno Setup script. Key directives:

- `AppId` — a GUID generated once and hardcoded in the script. Stable
  across versions so upgrades replace in place (no duplicate entry in
  Settings → Apps).
- `AppName=QuickNote`, `AppVersion` taken from a `/DAppVersion=…`
  compiler define passed by make (single source of truth in the Makefile).
- `PrivilegesRequired=lowest` → per-user.
- `DefaultDirName={localappdata}\Programs\QuickNote`.
- `[Files]` — `quicknote.exe` only.
- `[Icons]` — Start menu shortcut in `{userprograms}` (this is what makes
  the app indexable by Start-menu search). **No desktop icon** (YAGNI; app
  lives in tray).
- `[Tasks]` — one task `startupicon`, description "Start QuickNote when
  Windows starts", checked by default.
- `[Registry]` — when `startupicon` task selected: value
  `QuickNote = "{app}\quicknote.exe"` under
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, flag
  `uninsdeletevalue` so uninstall removes it.
- `[Run]` — post-install "Launch QuickNote" checkbox
  (`postinstall nowait skipifsilent`).
- `CloseApplications=yes` — Restart Manager closes a running
  quicknote.exe before overwrite on upgrade.
- `OutputDir=..\dist`, `OutputBaseFilename=quicknote-setup-{#AppVersion}`.
- Uninstaller: auto-registered by Inno in Settings → Apps. Removes exe,
  shortcut, Run value. **Never touches `%APPDATA%\quickNote`** (user
  notes survive uninstall; no `[UninstallDelete]` for data).
- `SetupIconFile=..\assets\quicknote.ico` — installer exe gets the app icon.

### 2. Makefile additions

- `VERSION := 1.0.0` near the top (single source of truth).
- `installer` phony target, depends on `quicknote.exe`:
  - Locates `ISCC.exe`: try `ISCC` on PATH, fall back to
    `C:\Program Files (x86)\Inno Setup 6\ISCC.exe`.
  - Runs `ISCC /DAppVersion=$(VERSION) installer/quicknote.iss`.
  - Output: `dist/quicknote-setup-$(VERSION).exe`.
- `dist/` added to `.gitignore`.
- `clean` leaves `dist/` alone (release artifacts are deliberate).

### 3. README

Short **Install** section:
- Download `quicknote-setup-<ver>.exe` from GitHub Releases, run it.
- SmartScreen note: unsigned binary → "Windows protected your PC" →
  "More info" → "Run anyway".
- Building the installer from source: install Inno Setup 6
  (`winget install JRSoftware.InnoSetup`), run `make installer`.

## Release flow (manual)

```
make installer
gh release create v1.0.0 dist/quicknote-setup-1.0.0.exe --title "QuickNote 1.0.0"
```

## Testing

Installer logic is not unit-testable; verification is a human checklist
(GUI rule — scripted focus/screenshots unavailable):

1. `make installer` succeeds; setup exe lands in `dist/`.
2. Run installer → no UAC prompt; completes.
3. Start menu search "QuickNote" finds the app; launches.
4. Autostart task checked → Run value present in HKCU (verifiable via
   `reg query`).
5. Re-run installer (upgrade path) with app running → app closed,
   replaced, single Apps entry.
6. Uninstall from Settings → Apps → exe/shortcut/Run value gone;
   `%APPDATA%\quickNote` notes intact.

Registry/file assertions (4, 6) are scriptable post-hoc checks; the
install/launch/search steps are human-verified.

## Scope boundaries

- **In:** `.iss` script, Makefile target + VERSION, `.gitignore`, README
  install docs.
- **Out (deferred):** code signing, CI release automation, desktop icon,
  MSI/winget/store packaging, in-app version resource stamping,
  auto-update.

## Risks

- **SmartScreen friction** for downloaders — accepted, documented.
- **ISCC not installed** on build machine — make target fails with a
  clear "install Inno Setup 6" message rather than a cryptic error.
- **AppId churn** — if the GUID were regenerated per build, upgrades
  would stack duplicate entries; hardcoding it in the committed `.iss`
  prevents this.
- **Autostart entry orphaned** — prevented by `uninsdeletevalue`.
