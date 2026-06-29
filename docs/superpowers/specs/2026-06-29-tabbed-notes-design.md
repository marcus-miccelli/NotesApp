# Design: Windows-Terminal-style tabbed notes

Date: 2026-06-29
Status: approved

## Goal

Transform quickNote from **1 window = 1 note** into **1 window = N notes as tabs**,
in the visual/interaction style of Windows Terminal: a tab strip in the custom
title bar, a `+` to add tabs, dynamically-sized tabs, drag-to-reorder, an active
tab, and a reserved drag gap before the window controls. Rename happens only via
Ctrl+R on the active tab; tab labels are otherwise uneditable. The separate H1
title box is removed — the note name lives in its tab, rendered at an H3-ish size.

## Out of scope (deferred)

- **Detach**: dragging a tab out of the strip to spawn a new window.
- Tab overflow **scrolling** (when tabs shrink past the minimum).
- Dragging a tab onto another window to **merge**.

These are explicit follow-ups, not part of this implementation.

## Data model — preferences.json schema v2

Today (`src/prefs.h`): a flat `NoteMeta[]` where each note carries its own
`x,y,w,h` geometry and `open` flag. With tabs, geometry and open-state belong to
the *window group*, not the note.

New structures:

```c
typedef struct {
    char id[16];
    char file[24];
    char name[64];     /* note title; shown in tab + tray; default "Untitled N" */
    char color[16];
} NoteMeta;             /* geometry + open REMOVED */

#define WIN_MAX_TABS 32
typedef struct {
    char id[16];                    /* stable window id (resolve fresh, like notes) */
    int  x, y, w, h;
    int  active;                    /* index into tabs[] of the active note */
    char tabs[WIN_MAX_TABS][16];    /* ordered note ids */
    int  ntabs;
} WinMeta;

typedef struct {
    int       version;              /* bumped to 2 */
    char      theme[16];
    NoteMeta* notes;  size_t count, cap;     /* master list of ALL notes */
    WinMeta*  windows; size_t wcount, wcap;  /* open window groups */
} Prefs;
```

Invariants:
- `notes[]` is the master list of every note that exists on disk.
- A note is **open** iff some `windows[].tabs[]` contains its id. A note in no
  window is closed (still on disk, reopenable from the tray).
- A note id appears in at most one window and at most once.
- `active` is a valid index in `[0, ntabs)` whenever `ntabs > 0`; a window with
  `ntabs == 0` does not persist (it is closed).

New prefs API (additive):

```c
WinMeta* prefs_add_window(Prefs* p, const char* id);
WinMeta* prefs_find_window(Prefs* p, const char* id);
bool     prefs_remove_window(Prefs* p, const char* id);
/* tab list edits live in note_window.c via the resolved WinMeta* */
```

`prefs_load` / `prefs_save` gain a `windows` array in the JSON. `prefs_save`
writes `version: 2`.

### Migration v1 -> v2

On load, if `version < 2` (or there is no `windows` array but notes exist with
legacy geometry): for each note that was `open` in v1, create a 1-tab window
using that note's old `x,y,w,h`; closed notes get no window. Legacy
per-note geometry keys are read only for this migration, then dropped on the
next save. (Low stakes in practice — the user wipes data between tests — but it
must not crash on an old file.)

## Window object (`NoteWin`) — a tab group

`note_window.c`'s `NoteWin` changes from holding one note to holding a group:

```c
typedef struct {
    char id[16];      /* note id */
    HWND edit;        /* this tab's body RichEdit (one per tab) */
} NoteTab;

typedef struct {
    AppState* app;
    char  win_id[16];          /* resolve WinMeta* fresh via prefs_find_window */
    NoteTab tab[WIN_MAX_TABS];
    int   ntabs;
    int   active;
    /* tab-strip interaction state */
    int   hot_tab;             /* tab under cursor, -1 none */
    int   hot_close;           /* close-glyph hovered on hot_tab */
    int   drag_tab;            /* tab being dragged, -1 none */
    int   drag_dx;             /* cursor offset within the dragged tab */
    int   press_x, press_y;    /* mouse-down point, for drag threshold */
    /* rename overlay */
    HWND  rename_edit;         /* temp single-line edit, NULL unless renaming */
    int   rename_tab;
    /* shared chrome (unchanged) */
    HWND  btns[NUM_BTNS];      /* format sidebar */
    HWND  wbtns[NUM_WBTNS];    /* min/max/close */
    int   whot[NUM_WBTNS];
    int   active_fmt[NUM_BTNS];/* sidebar highlight for the active tab's caret */
} NoteWin;
```

**One RichEdit body per tab.** The active tab's edit is shown; the rest are
hidden (`ShowWindow(SW_HIDE)`). This preserves each note's scroll position,
caret, and undo stack across tab switches, and keeps the existing live-format /
debounce machinery per control. All edits use control id `ID_EDIT`; the handler
resolves "which tab" by matching the notify `hwnd` against `tab[i].edit`. The
shared sidebar and caption buttons are unchanged.

The **H1 title box is removed**. `nw_load_content` / `nw_save_content` now act on
a given tab's edit; the note name is metadata (`NoteMeta.name`) shown in the tab
and still written as the leading `# name` line of the `.md` so the file stays
self-describing.

## Tab strip

Lives in the top-right cell of the existing two-divider layout: between the
vertical divider (qN cell, `x = SIDEBAR_W`) and the window buttons, above the
horizontal divider (`y = TITLEBAR_H`). Custom-drawn in `WM_PAINT` — not child
controls — because reorder-drag and dynamic widths are awkward with real buttons.

Geometry:
- Strip rect: `left = SIDEBAR_W + 1`, `top = 0`, `right = (window buttons left) -
  DRAG_GAP`, `bottom = TITLEBAR_H`. `DRAG_GAP` (~36px) is the reserved draggable
  area before the controls (WT/browser behavior).
- The `+` button sits immediately after the last tab (fixed width ~28px).
- Tab width = `clamp((strip_w - PLUS_W) / ntabs, TAB_MIN, TAB_MAX)` with
  `TAB_MIN ~48`, `TAB_MAX ~200`. Tabs shrink as count grows; if they hit
  `TAB_MIN` and still overflow, they shrink to fit for now (overflow scrolling
  deferred).
- Each tab paints: name (ellipsized, ~H3 size) + a ✕ glyph on the right (shown
  when the tab is hot or active). Active tab gets the accent fill + top edge;
  hot tab a lighter fill.

Hit-testing & mouse:
- `WM_NCHITTEST`: over the strip (tabs / `+`) return `HTCLIENT` so we get clicks;
  over `DRAG_GAP` and the rest of the bare title bar return `HTCAPTION` (drag /
  double-click maximize). Top resize strip and qN cell unchanged.
- `WM_MOUSEMOVE` (client): update `hot_tab` / `hot_close`, repaint; if a drag is
  active past threshold, reorder.
- `WM_LBUTTONDOWN` in strip:
  - on `+` -> new tab.
  - on a tab's ✕ -> close that tab.
  - on a tab body -> activate it; record `press_*`, `drag_tab`, `drag_dx` and
    `SetCapture` to arm a possible reorder.
- `WM_MOUSEMOVE` with capture + moved past `DRAG_THRESHOLD` (~6px): enter reorder
  — the dragged tab follows the cursor; when its center crosses a neighbor's
  midpoint, swap order in the runtime `tab[]` (and mirror into `WinMeta.tabs`).
- `WM_LBUTTONUP`: `ReleaseCapture`; commit/clear drag state; repaint.
- `WM_MBUTTONDOWN` on a tab -> close it (middle-click close).

## Interactions

| Action | Result |
|--------|--------|
| `+` button / **Ctrl+N** | New note as a new tab in this window; becomes active. |
| **Alt+N** | New window with one fresh note. |
| Tray "New Note" | New window with one fresh note. |
| Click a tab | Activate it (show its edit, focus, refresh sidebar). |
| Drag a tab | Reorder within the strip; commit order on drop. |
| **Ctrl+R** | Rename the active tab (see below). |
| Close tab (✕ / middle-click) | Remove tab from window; note stays on disk. Last tab -> close window. |
| Close window (title-bar X) | Close all tabs (notes persist); remove window. |
| **Ctrl+Shift+D** | Delete the active note's `.md` + entry; remove its tab; if window empties, close it. |

### Rename (Ctrl+R)

Overlay a temporary single-line edit (`rename_edit`) positioned over the active
tab's rect, pre-filled with the current name, text selected. Commit on **Enter**
or focus loss: write the trimmed text to `NoteMeta.name`, rewrite the leading
`# name` line in the `.md`, destroy the overlay, repaint the strip. **Esc**
cancels. Tab labels are never directly editable otherwise.

## Tray

- "New Note" -> open a new window (one fresh note).
- Reopen a note (menu lists note names): if the note is a tab in an open window,
  focus that window and activate the tab; else open a new window containing it.
- Quit unchanged.

The window registry (today keyed by note id) becomes keyed by **window id**, plus
a lookup "which open window/tab holds note id" for tray reopen and dedupe.

## Persistence

On shutdown (and on the existing geometry/content save points): each open
`NoteWin` writes its geometry, ordered tab ids, and active index into its
`WinMeta`; bodies save to their `.md` as today. `prefs_save` serializes
`windows[]` + `notes[]` at `version: 2`.

## Layout constants (additions to note_window.c)

```
DRAG_GAP       36   /* reserved drag area before the window buttons */
PLUS_W         28   /* + button width */
TAB_MIN        48   /* min tab width before (deferred) overflow */
TAB_MAX        200  /* max tab width */
TAB_CLOSE_W    18   /* hit area for a tab's close glyph */
DRAG_THRESHOLD 6    /* px before a press becomes a reorder drag */
```

`TITLEBAR_H` (34) and `SIDEBAR_W` (40) and the two dividers are unchanged; the
top-left qN cell, format sidebar, and body cell keep their positions.

## Testing

Headless unit tests (no GUI) cover the data model and migration:
- `prefs_add_window` / `prefs_find_window` / `prefs_remove_window`.
- Save -> load round-trip of `windows[]` (geometry, tab order, active) preserves
  values; `version: 2` written.
- v1 -> v2 migration: a v1 prefs with two notes (one open, one closed) loads into
  one window (the open note, with its old geometry) and leaves the closed note in
  `notes[]` with no window.
- Existing note/markdown/paths tests stay green (update any that asserted the
  removed `NoteMeta` fields).

GUI behaviors (tab activate, reorder, +, Ctrl+R, close, Alt+N, drag gap, dynamic
width) are verified by launching the exe and screenshotting, as in prior work.
```
