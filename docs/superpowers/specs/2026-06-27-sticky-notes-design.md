# Sticky Notes — Design Spec

Date: 2026-06-27
Status: Approved (design), pending implementation plan

## Summary

A native Windows desktop sticky-note app written in C. Each note is a small,
borderless, dark-mode window floating on the desktop. Note content is Markdown,
stored one `.md` file per note. The app persists which notes are open and their
window geometry, so reopening the app restores the previous desktop state.
Markdown is formatted live as the user types.

This is a sticky-note app (many small floating notes), **not** an Obsidian-style
single-window note manager.

## Goals

- Native, clean, performant. No animations.
- Each note is a Markdown `.md` file on disk.
- Reopen all previously-open notes on launch, at their saved positions/sizes.
- Dark mode.
- Built to grow (clear module boundaries, central state index), without
  over-engineering v1.

## Non-Goals (v1 — YAGNI)

- No authentication, no accounts, no cloud sync.
- No app-level multi-user profiles (single OS user only).
- No markdown export/import beyond reading/writing the `.md` files themselves.
- No tabbed/single-window manager UI.

## Tech Stack

- **Language:** C (C11).
- **GUI:** Win32 API. Each sticky note is a real top-level OS window.
- **Text/editing:** Win32 RichEdit control (`MSFTEDIT_DLL` / `Msftedit.dll`,
  RichEdit 4.1+) for formatted display.
- **Markdown parser:** [md4c](https://github.com/mity/md4c) — callback ("push")
  parser, single source file, no dependencies, CommonMark. Bundled in
  `third_party/`.
- **JSON:** [cJSON](https://github.com/DaveGamble/cJSON) — single source file, no
  dependencies. Bundled in `third_party/`.
- **Build:** MinGW-w64 (gcc) + GNU Make. Link `gdi32 comctl32 ole32 shell32`.
- **Platform:** Windows 10+ (single OS user account).

## Architecture

Native Win32 application. Components:

```
main()
 └─ AppState (in-memory): note list, resolved paths, theme
     ├─ tray.c        → system tray icon + context menu (New, note list, Quit)
     ├─ note_window.c → per-note window: RichEdit + custom title bar (+ / x),
     │                   drag, resize, message handling
     ├─ markdown.c    → md4c callbacks -> RichEdit CHARFORMAT/PARAFORMAT mapping
     ├─ store.c       → load/save note .md files; enumerate notes dir
     ├─ prefs.c       → read/write preferences.json via cJSON (index + geometry)
     └─ paths.c       → resolve %APPDATA%\StickyNotes\, ensure dirs exist
```

- A hidden message-only window owns the tray icon and overall app lifecycle.
- Each note window owns exactly one note. Windows share nothing mutable beyond
  the central `AppState` (note list + paths + theme).
- **Layering discipline:** `paths`, `store`, `prefs`, and the md4c→format mapping
  in `markdown` are pure logic with no Win32 window dependencies, so they can be
  unit-tested headless. `tray` and `note_window` are the Win32 glue layer.

## Storage Layout

```
%APPDATA%\StickyNotes\
 ├─ preferences.json      app state + per-note metadata index
 └─ notes\
     ├─ <id>.md           pure markdown content, one file per note
     └─ ...
```

- `<id>` = random hex string (e.g. 6–8 chars), used as both note identity and
  filename stem.
- `.md` files contain **only** markdown content — no frontmatter, no metadata —
  so they stay portable and md4c sees clean input.
- All per-note metadata lives in the central index (below).

### preferences.json schema

```json
{
  "version": 1,
  "theme": "dark",
  "notes": [
    {
      "id": "a1b2c3",
      "file": "a1b2c3.md",
      "x": 200,
      "y": 150,
      "w": 280,
      "h": 320,
      "color": "slate",
      "open": true
    }
  ]
}
```

- `version` — schema version for future migrations.
- `theme` — `"dark"` in v1 (single value; field reserved for growth).
- `notes[]` — one entry per existing note (open or closed).
  - `x,y,w,h` — last window geometry (screen coordinates, pixels).
  - `color` — named accent from a small fixed dark-mode palette (e.g. `slate`,
    `amber`, `teal`, `rose`). Tints the title bar only. Not arbitrary RGB.
  - `open` — whether the note window should be shown on launch.

Unknown/missing fields are tolerated with sensible defaults on load.

## Note Lifecycle — Close vs Delete

Two distinct, clearly separated actions:

- **Close (`x` button on note title bar):** hides the window, sets `open:false`
  in the index. The `.md` file is **kept**. The note can be reopened from the
  tray menu's note list.
- **Delete (tray menu item, or Shift+click the `x`):** removes the note from the
  index **and deletes its `.md` file**. Because this is irreversible, it shows a
  confirmation prompt first.

- **New (`+` button on any note, or tray "New"):** creates a new blank note with
  a fresh `id`, offset slightly from the current/last note position. Tray "New"
  is the entry point when no notes are currently open.

## Window & Rendering Behavior

- Borderless dark window (`WS_POPUP`), no native title bar.
- **Custom title bar:** thin strip tinted with the note's accent color, holding a
  `+` (new) and `x` (close) button. Dragging anywhere on the bar moves the window
  (handle `WM_NCHITTEST` / `HTCAPTION`).
- **Resize:** bottom-right grip resizes the window.
- **Body:** a RichEdit control filling the rest of the window; dark background,
  light foreground text.
- **Live markdown formatting:**
  - On `EN_CHANGE`, start/restart a short debounce timer (~150 ms).
  - On timer fire, run md4c over the current buffer text; map callbacks to
    RichEdit formatting (headings = larger + bold; `**bold**`, `*italic*`,
    `` `code` `` = monospace; bullet/numbered lists indented).
  - Save and restore caret position and scroll offset around the reformat so
    typing is not disrupted.
- **Persistence triggers:**
  - Note content written to its `.md` on debounce-idle and on close.
  - Geometry (`x,y,w,h`) written to the index on move/resize end
    (`WM_EXITSIZEMOVE`).
  - Frequent small writes keep state crash-resilient without an explicit
    save action.

## Startup / Shutdown Flow

**Startup:**
1. Resolve `%APPDATA%\StickyNotes\`; create `StickyNotes\` and `notes\` if absent.
2. Load `preferences.json` (create a default empty one if missing/corrupt).
3. For each note with `open:true`: read its `.md`, create its window at saved
   geometry, render content.
4. If no notes are open (or none exist), create one blank note.
5. Register the system tray icon and message-only owner window.

**Shutdown (tray → Quit):**
1. Flush all dirty note contents to their `.md` files.
2. Write current geometry/open-state of all notes into `preferences.json`.
3. Release tray icon and exit.

## Error Handling

- Missing/corrupt `preferences.json` → log, back it up as `.bak`, start with a
  fresh default index (existing `.md` files in `notes\` can be re-indexed).
- Missing `.md` referenced by index → drop that entry from the index on load.
- File write failure → keep in-memory state, surface a non-blocking error; retry
  on next persistence trigger.
- RichEdit DLL load failure → fatal startup error with a clear message box.

## Build & Test

- `Makefile` (MinGW-w64 gcc). Targets: `app` (the exe), `test`, `clean`.
- Libraries linked: `gdi32`, `comctl32`, `ole32`, `shell32` (+ RichEdit via
  `LoadLibrary("Msftedit.dll")` at runtime).
- `third_party/` holds md4c and cJSON sources, compiled into the build.
- **Unit tests (headless):** a small test runner compiles the pure-logic modules
  (`paths`, `store`, `prefs`, md4c→format mapping) without the GUI and asserts:
  prefs round-trip (write→read equality), note file read/write, path resolution,
  and markdown→format-span mapping for representative markdown.
- **Manual verification:** Win32 window behavior (drag, resize, tray, live
  formatting, reopen-on-launch) checked by running the app.

## Directory Layout (source)

```
NotesApp\
 ├─ src\
 │   ├─ main.c
 │   ├─ app.c / app.h          (AppState)
 │   ├─ tray.c / tray.h
 │   ├─ note_window.c / .h
 │   ├─ markdown.c / .h
 │   ├─ store.c / .h
 │   ├─ prefs.c / .h
 │   └─ paths.c / .h
 ├─ tests\
 │   └─ test_*.c
 ├─ third_party\
 │   ├─ md4c\
 │   └─ cjson\
 ├─ Makefile
 └─ docs\superpowers\specs\2026-06-27-sticky-notes-design.md
```
