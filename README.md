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
- `+` on a note: new note. `x`: close (keeps file).
- Shift+`x`: delete the note permanently (confirms first).
- Tray menu: New Note, reopen a note, Quit.

## Tech

Win32 + RichEdit, md4c (markdown), cJSON (state). Third-party sources
vendored under `third_party/`.
