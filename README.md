# quickNote

Native Windows sticky-note app in C. Each window holds multiple notes as tabs,
editing Markdown `.md` files with live preview. Windows and tab order are
restored on launch.

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

## Build

Requires MinGW-w64 (gcc + windres) and GNU Make.

    make app      # builds quicknote.exe
    make test     # builds and runs the headless unit tests
    make clean

The app icon ("qN") is built from `assets/quicknote.ico` via `assets/app.rc`.

## Data

Stored under `%APPDATA%\quickNote\`:
- `notes\<id>.md` — one Markdown file per note; the leading `# <title>` H1 is
  the note name, the rest is the body
- `preferences.json` — schema v2: a `windows` array (geometry, ordered
  tab note-ids, active index) and a `notes` array (name, id)

## Usage

Run `quicknote.exe`. A tray icon appears in the notification area.

### Windows and tabs

Each window holds one or more notes as **tabs** in a Windows-Terminal-style
strip across the title bar:

- **`+` / Ctrl+N** — add a new tab (new note) to the current window
- **Alt+N** — open a new window (with one new note)
- **Drag a tab** — reorder tabs within the window
- **✕ (tab close button) / middle-click** — close a tab; the note stays on
  disk. Closing the last tab closes the window.
- **Ctrl+R** — rename the active tab/note (press Enter to confirm, Escape to
  cancel)
- **Ctrl+Shift+D** — delete the active note permanently (confirms first)
- **Title-bar X** — close the window. Its notes stay on disk and remain
  reopenable from the tray, but the closed window is not restored. On launch
  the app reopens whichever windows were open when you last **Quit** (or a
  fresh window if none)

### Tray

The tray menu lists open note names. **New Note** opens a new window. Clicking
a listed note focuses its window and activates that tab; if the window was
closed, it reopens. **Quit** saves state and exits.

### Markdown editing

Markdown is rendered live as you type (headings, **bold**, *italic*,
~~strikethrough~~, `code`, bullet/numbered lists). Delimiters are hidden
(`**bold**` displays as just bold) but kept in the `.md` file, so note files
stay plain Markdown on disk.

A formatting **sidebar** (left edge) toggles styles on the selection:

| Button | Action | Shortcut |
|--------|--------|----------|
| **B**  | Bold `**…**`       | Ctrl+B |
| *I*    | Italic `*…*`       | Ctrl+I |
| ~~S~~  | Strikethrough `~~…~~` | Ctrl+Shift+X |
| •      | Bullet list `- `   | Ctrl+Shift+8 |
| 1.     | Numbered list `N. `| Ctrl+Shift+7 |

Each button toggles: applying again on already-formatted text removes it.

## Tech

Win32 + RichEdit, md4c (markdown), cJSON (state). Third-party sources
vendored under `third_party/`.
