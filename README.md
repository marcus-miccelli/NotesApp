# quickNote

Native Windows sticky-note app in C. Each note is a dark-mode window editing a
Markdown `.md` file. Open notes and their positions are restored on launch.

## Build

Requires MinGW-w64 (gcc + windres) and GNU Make.

    make app      # builds quicknote.exe
    make test     # builds and runs the headless unit tests
    make clean

The app icon ("qN") is built from `assets/quicknote.ico` via `assets/app.rc`.

## Data

Stored under `%APPDATA%\quickNote\`:
- `notes\<id>.md` — one markdown file per note; the title is its leading
  `# <title>` H1, the rest is the body
- `preferences.json` — note name, open state, window geometry, color, theme

## Usage

- Run `quicknote.exe`. A tray icon manages notes.
- Each note is a dark native window with a standard title bar (minimize,
  maximize, close); the **X** closes it (keeps the file).
- A note has a **title box** (styled as Heading 1) above the body; new notes
  are named `Untitled N`. Press Enter in the title to jump to the body.
- **Ctrl+N**: new note. **Ctrl+Shift+D**: delete the note permanently (confirms first).
- Tray menu shows note names (New Note, reopen a note, Quit).
- Markdown is rendered live as you type (headings, **bold**, *italic*,
  ~~strikethrough~~, `code`, bullet/numbered lists). Delimiters are hidden
  (`**bold**` shows as bold) but kept in the file, so the `.md` stays plain
  Markdown on disk.
- A formatting **sidebar** (left edge) toggles styles on the selection:

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
