# Sticky Notes

Native Windows sticky-note app in C. Each note is a borderless dark-mode
window editing a Markdown `.md` file. Open notes and their positions are
restored on launch.

## Build

Requires MinGW-w64 (gcc) and GNU Make.

    make app      # builds stickynotes.exe
    make test     # builds and runs the headless unit tests
    make clean

## Data

Stored under `%APPDATA%\StickyNotes\`:
- `notes\<id>.md` — one markdown file per note (pure content)
- `preferences.json` — open state, window geometry, color, theme

## Usage

- Run `stickynotes.exe`. A tray icon manages notes.
- Each note is a dark native window with a standard title bar (minimize,
  maximize, close); the **X** closes it (keeps the file).
- **Ctrl+N**: new note. **Ctrl+Shift+D**: delete the note permanently (confirms first).
- Tray menu: New Note, reopen a note, Quit.
- Markdown is rendered live as you type (headings, **bold**, *italic*, `code`).
  The delimiter characters are hidden (`**bold**` shows as bold) but kept in the
  file, so the `.md` stays plain Markdown on disk.

## Tech

Win32 + RichEdit, md4c (markdown), cJSON (state). Third-party sources
vendored under `third_party/`.
