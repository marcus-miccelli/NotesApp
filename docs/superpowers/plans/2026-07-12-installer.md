# QuickNote Installer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A single `setup.exe` (Inno Setup, per-user, unsigned) that installs QuickNote with a Start-menu-searchable shortcut, optional autostart, and a clean uninstaller — built by `make installer`.

**Architecture:** One committed Inno Setup script (`installer/quicknote.iss`) compiled by the Inno Setup 6 CLI (`ISCC.exe`) via a new Makefile target. The app exe is self-contained (system DLLs only), so the installer ships exactly one file plus a shortcut and registry entries. No app code changes.

**Tech Stack:** Inno Setup 6 (ISPP preprocessor), GNU Make under MSYS2 bash, existing mingw-w64 build.

Spec: `docs/superpowers/specs/2026-07-12-installer-design.md`

## Global Constraints

- Display name **QuickNote**; exe `quicknote.exe`; data dir `%APPDATA%\quickNote` must NEVER be touched by install or uninstall.
- Per-user install only: `PrivilegesRequired=lowest`, `DefaultDirName={localappdata}\Programs\QuickNote`. No UAC prompt.
- `AppId` GUID is `{{ADFF8E05-0AD8-497C-A956-AD109ED260A8}` — hardcoded, never regenerated.
- `AppMutex=quickNote_SingleInstance_Mutex` — must exactly match the mutex name in `src/main.c:14`. Comment beside each names the other.
- Version single source of truth: `VERSION := 1.0.0` in the Makefile, passed to ISCC as `/DAppVersion="$(VERSION)"`. The `.iss` must `#error` if the define is missing.
- Start menu shortcut: exactly one, `{userprograms}\QuickNote` → `{app}\quicknote.exe`. No `{group}`/`DefaultGroupName`, no desktop icon.
- Autostart: one `[Tasks]` entry `startupicon`, checked by default, writing quoted `"""{app}\quicknote.exe"""` to `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` with `uninsdeletevalue`.
- Makefile recipes run under MSYS2 bash: the ISCC fallback path must be POSIX-style and quoted: `/c/Program Files (x86)/Inno Setup 6/ISCC.exe`.
- Working branch: `feature/installer` (create from `main` before Task 1's first commit).

---

### Task 1: Inno Setup script

**Files:**
- Create: `installer/quicknote.iss`
- Modify: `.gitignore` (append `dist/` under the "Build temp dir" section)

**Interfaces:**
- Consumes: `quicknote.exe` at repo root (built by `make app`); `assets/quicknote.ico`; mutex name `quickNote_SingleInstance_Mutex` from `src/main.c:14`.
- Produces: `installer/quicknote.iss` compilable as `ISCC /DAppVersion="X.Y.Z" installer/quicknote.iss` → `dist/quicknote-setup-X.Y.Z.exe`. Task 2's make target invokes exactly that command line.

- [ ] **Step 1: Create branch**

```bash
git checkout -b feature/installer main
```

- [ ] **Step 2: Ensure Inno Setup 6 is installed**

```bash
ls "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" || winget install --id JRSoftware.InnoSetup -e --accept-source-agreements --accept-package-agreements
```

Expected: file listed, or winget completes installation. After install, `ls` again to confirm the path exists. (Machine dependency, not a repo change — nothing to commit for this step.)

- [ ] **Step 3: Write `installer/quicknote.iss`**

```iss
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
```

- [ ] **Step 4: Append `dist/` to `.gitignore`**

At the end of `.gitignore` (after the `.build-tmp/` line), append:

```gitignore

# Installer output
dist/
```

- [ ] **Step 5: Verify the script compiles**

```bash
make app
"/c/Program Files (x86)/Inno Setup 6/ISCC.exe" /DAppVersion="0.0.0" installer/quicknote.iss
ls dist/quicknote-setup-0.0.0.exe
git status --short   # dist/ must NOT appear (ignored)
rm -f dist/quicknote-setup-0.0.0.exe
```

Expected: ISCC prints `Successful compile`; the setup exe exists; `git status` shows only the two new/modified repo files. Then delete the test artifact.

- [ ] **Step 6: Verify missing-define guard**

```bash
"/c/Program Files (x86)/Inno Setup 6/ISCC.exe" installer/quicknote.iss; echo "exit=$?"
```

Expected: compile FAILS with the `#error` message `Pass /DAppVersion="x.y.z" (use \`make installer\`)`, nonzero exit.

- [ ] **Step 7: Commit**

```bash
git add installer/quicknote.iss .gitignore
git commit -m "feat(installer): Inno Setup script — per-user, Start menu shortcut, autostart task"
```

### Task 2: Makefile `installer` target

**Files:**
- Modify: `Makefile` (add `VERSION` near the top; add `installer` to `.PHONY`; add target at the bottom before `clean`)

**Interfaces:**
- Consumes: `installer/quicknote.iss` from Task 1 (contract: `ISCC /DAppVersion="X.Y.Z" installer/quicknote.iss` → `dist/quicknote-setup-X.Y.Z.exe`); existing `quicknote.exe` target.
- Produces: `make installer` → `dist/quicknote-setup-$(VERSION).exe`. Task 3's README documents this command.

- [ ] **Step 1: Add `VERSION` after the `WINDRES` line**

In `Makefile`, directly below `WINDRES := windres`, add:

```make
VERSION := 1.0.0
```

- [ ] **Step 2: Add the `installer` target**

Change `.PHONY: all app test clean` to:

```make
.PHONY: all app test clean installer
```

Before the `clean:` rule, add:

```make
# Installer (Inno Setup 6). Recipes run under MSYS2 bash, so fallback paths
# are POSIX-style and quoted. Checked in order: PATH, the winget per-user
# location, then the classic machine-wide "Program Files (x86)" location.
# The per-user dir is resolved with `cygpath -F 28` (CSIDL_LOCAL_APPDATA,
# via the Windows API) because recipe environments here scrub Windows env
# vars like LOCALAPPDATA — the same phenomenon as TMP/TEMP at the top of
# this file. Both candidates are overridable (used by the error-path test).
# MSYS2_ARG_CONV_EXCL="/D" stops the MSYS runtime from rewriting the
# /DAppVersion switch into a Windows path (which ISCC would then reject as
# a second script filename).
ISCC_LOCAL := $(shell cygpath -u -F 28 2>/dev/null)/Programs/Inno Setup 6/ISCC.exe
ISCC_PF    := /c/Program Files (x86)/Inno Setup 6/ISCC.exe

installer: quicknote.exe
	@ISCC="$$(command -v ISCC || true)"; \
	for p in "$(ISCC_LOCAL)" "$(ISCC_PF)"; do \
	  if [ -z "$$ISCC" ] && [ -x "$$p" ]; then ISCC="$$p"; fi; \
	done; \
	if [ -z "$$ISCC" ]; then \
	  echo "error: ISCC.exe not found — install Inno Setup 6: winget install JRSoftware.InnoSetup"; \
	  exit 1; \
	fi; \
	MSYS2_ARG_CONV_EXCL="/D" "$$ISCC" /DAppVersion="$(VERSION)" installer/quicknote.iss
```

(Tab-indented recipe lines, as in the rest of the Makefile. On this
machine winget installed Inno Setup per-user, so `ISCC_LOCAL` is the
candidate that fires — verified:
`/c/Users/Marcus/AppData/Local/Programs/Inno Setup 6/ISCC.exe` exists,
nothing in `Program Files (x86)`, nothing on PATH. `cygpath -u -F 28`
verified to return `/c/Users/Marcus/AppData/Local` from inside a recipe
even though `$LOCALAPPDATA` is empty there.)

- [ ] **Step 3: Verify**

```bash
make installer
ls dist/quicknote-setup-1.0.0.exe
```

Expected: ISCC runs, `Successful compile`, file exists with the version from the Makefile.

- [ ] **Step 4: Verify the not-installed error path**

Run immediately after Step 3 so `quicknote.exe` is already up to date —
the stripped PATH below has no gcc, and a stale exe would otherwise
trigger a rebuild that fails for the wrong reason. `ISCC_LOCAL` is
overridden to defeat the per-user candidate; the `Program Files (x86)`
candidate is absent on this machine (verified), so the error path fires.

```bash
PATH="/usr/bin:/c/msys64/usr/bin" make installer ISCC_LOCAL=/nonexistent 2>&1 | tail -2; echo "exit=$?"
```

(make lives in `/c/msys64/usr/bin` on this machine, so that dir stays on
the stripped PATH — it contains no ISCC, so the assertion is unchanged.)

Expected: the clear `error: ISCC.exe not found — install Inno Setup 6…` message (not a cryptic bash error). (`tail` exit code is 0; the message text is the assertion here.)

- [ ] **Step 5: Run existing suite (regression gate)**

```bash
make test
```

Expected: all checks pass (150 checks, 0 failed).

- [ ] **Step 6: Commit**

```bash
git add Makefile
git commit -m "build: make installer target compiles Inno Setup script (VERSION single source)"
```

### Task 3: README install docs

**Files:**
- Modify: `README.md` (insert an `## Install` section between the intro paragraph and `## Build`)

**Interfaces:**
- Consumes: Task 2's `make installer` command; release filename pattern `quicknote-setup-<version>.exe`.
- Produces: user-facing docs only.

- [ ] **Step 1: Insert the Install section**

In `README.md`, after the intro paragraph (`...restored on launch.`) and before `## Build`, insert:

```markdown
## Install

Download `quicknote-setup-<version>.exe` from
[GitHub Releases](../../releases) and run it. Installs per-user (no admin
prompt), adds **QuickNote** to the Start menu, and offers a
"start with Windows" checkbox. Uninstall from Settings → Apps; your notes
in `%APPDATA%\quickNote` are never deleted.

> **SmartScreen note:** the installer is unsigned, so Windows may show
> "Windows protected your PC". Click **More info → Run anyway**.

To build the installer yourself: install
[Inno Setup 6](https://jrsoftware.org/isinfo.php)
(`winget install JRSoftware.InnoSetup`), then:

    make installer   # emits dist/quicknote-setup-<version>.exe
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: README install section (GitHub Releases, SmartScreen, make installer)"
```

## Human verification checklist (post-merge, from the spec)

Not part of the tasks — the user runs these by hand (GUI rule):

1. Run `dist/quicknote-setup-1.0.0.exe` → no UAC prompt; completes.
2. Start menu search "QuickNote" finds it; launches.
3. `reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v QuickNote` → quoted exe path (if checkbox left checked).
4. Re-run installer with app running → prompt to close QuickNote; clean replace; single Apps entry.
5. Uninstall with app running → prompt to close; afterwards exe/shortcut/Run value gone; `%APPDATA%\quickNote` intact.
