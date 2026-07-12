# Design: QuickNote Windows installer (Inno Setup)

Date: 2026-07-12
Status: Approved (referee-reviewed; fixes folded in)

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
- `AppName=QuickNote`, `AppPublisher=Marcus Miccelli`, `AppVersion` taken
  from a `/DAppVersion="…"` compiler define passed by make (quoted; single
  source of truth in the Makefile). `VersionInfoVersion={#AppVersion}`
  stamps the setup exe's version resource.
- `PrivilegesRequired=lowest` → per-user.
- `ArchitecturesAllowed=x64compatible` +
  `ArchitecturesInstallIn64BitMode=x64compatible` — 64-bit mingw exe;
  refuse incompatible architectures.
- `DefaultDirName={localappdata}\Programs\QuickNote`.
- `[Files]` — `quicknote.exe` only.
- `[Icons]` — exactly one entry: `{userprograms}\QuickNote` (i.e.
  `Start Menu\Programs\QuickNote.lnk`) → `{app}\quicknote.exe`. Uses
  `{userprograms}` directly — **no `{group}`/`DefaultGroupName`** (no
  program-group folder). This shortcut is what makes the app indexable by
  Start-menu search. **No desktop icon** (YAGNI; app lives in tray).
- `[Tasks]` — one task `startupicon`, description "Start QuickNote when
  Windows starts", checked by default.
- `[Registry]` — when `startupicon` task selected: value
  `QuickNote = """{app}\quicknote.exe"""` (quoted — survives paths with
  spaces) under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`,
  flag `uninsdeletevalue` so uninstall removes it.
- `[Run]` — post-install "Launch QuickNote" checkbox
  (`postinstall nowait skipifsilent`).
- **Running-app handling (upgrade + uninstall):** Restart Manager alone
  is unreliable for a tray app (session-end messages target top-level
  windows). The app **already** owns a named mutex for its process
  lifetime — `quickNote_SingleInstance_Mutex`, created in `main.c`
  (single-instance handoff; the first instance never closes the handle).
  The script reuses it — **no app code change**:
  - `.iss`: `AppMutex=quickNote_SingleInstance_Mutex` — Setup *and* the
    uninstaller prompt the user to close QuickNote before continuing,
    instead of a silent rename-on-reboot leaving the old instance
    running.
  - `CloseApplications=yes` kept as second line of defense.
- `OutputDir=..\dist`, `OutputBaseFilename=quicknote-setup-{#AppVersion}`.
- Uninstaller: auto-registered by Inno in Settings → Apps. Removes exe,
  shortcut, Run value. **Never touches `%APPDATA%\quickNote`** (user
  notes survive uninstall; no `[UninstallDelete]` for data).
- `SetupIconFile=..\assets\quicknote.ico` — installer exe gets the app
  icon; `UninstallDisplayIcon={app}\quicknote.exe` — Apps-list entry
  shows it too.

### 2. Makefile additions

- `VERSION := 1.0.0` near the top (single source of truth).
- `installer` phony target, depends on `quicknote.exe`. Recipes run under
  MSYS2 bash, so the Windows path must be POSIX-style and quoted
  (spaces + parens in `Program Files (x86)`):
  - Locate: `command -v ISCC` on PATH, else fall back to
    `"/c/Program Files (x86)/Inno Setup 6/ISCC.exe"`; if neither exists,
    fail with a clear `install Inno Setup 6 (winget install
    JRSoftware.InnoSetup)` message.
  - Run: `"$$ISCC" /DAppVersion="$(VERSION)" installer/quicknote.iss`.
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

The release tag must match the Makefile `VERSION` — manual step, no
enforcement (accepted; scripting it is YAGNI at one release).

## Testing

Installer logic is not unit-testable; verification is a human checklist
(GUI rule — scripted focus/screenshots unavailable):

1. `make installer` succeeds; setup exe lands in `dist/`.
2. Run installer → no UAC prompt; completes.
3. Start menu search "QuickNote" finds the app; launches.
4. Autostart task checked → Run value present in HKCU (verifiable via
   `reg query`).
5. Re-run installer (upgrade path) with app running → AppMutex prompt
   asks to close QuickNote; after closing, replaced cleanly, single Apps
   entry, no reboot-pending state.
6. Uninstall from Settings → Apps → exe/shortcut/Run value gone;
   `%APPDATA%\quickNote` notes intact.
7. Uninstall **with app running** → uninstaller prompts to close first
   (AppMutex); no orphaned files after.

Registry/file assertions (4, 6, 7) are scriptable post-hoc checks; the
install/launch/search steps are human-verified.

## Scope boundaries

- **In:** `.iss` script, Makefile target + VERSION, `.gitignore`, README
  install docs. No app code changes (`AppMutex` reuses the existing
  single-instance mutex).
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
- **Tray app ignores Restart Manager** — mitigated by `AppMutex`
  reusing the app's existing single-instance mutex (referee finding);
  `CloseApplications` retained as backup. Residual: user can cancel the
  close prompt → Setup falls back to rename-on-reboot (standard Inno
  behavior).
- **Mutex rename hazard** — if `main.c` ever renames
  `quickNote_SingleInstance_Mutex`, the `.iss` `AppMutex` must follow;
  a comment beside each names the other.
