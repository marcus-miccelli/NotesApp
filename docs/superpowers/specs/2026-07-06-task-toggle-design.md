# Design: Clickable task toggle (sub-project C2)

Date: 2026-07-06
Status: Draft (pending review)

## Context

Sub-projects A (decoration engine), B (code/quote/task/link rendering), and C1
(correctness polish) are merged. Task-list items already render as bullets with
the literal `[ ]`/`[x]` checkbox visible and checked items struck through
(md4c `MD_FLAG_TASKLISTS`; the builder keeps the checkbox visible and only hides
the `- ` marker). C2 makes those checkboxes **clickable to toggle** — the
highest-value editing gap for a notes app. It is self-contained and does not
depend on C1.

Constraint unchanged: markdown knowledge lives in `src/markdown.c`;
`note_window.c` is RichEdit mechanics only; offsets are RichEdit `\r`/CP_ACP
char offsets (the body is read via `EM_GETTEXTEX GT_DEFAULT + CP_ACP`, matching
`EM_EXSETSEL`/`EM_CHARFROMPOS` indexing).

## Decisions (from brainstorming)

- **Plain click** on the `[ ]`/`[x]` toggles it (like Obsidian/GitHub todos).
- **Caret stays put** on toggle — the click flips the box and is consumed; it
  does not move the caret.
- **Hand cursor** shows over a checkbox (affordance).

## Components

### 1. Builder query (`src/markdown.{c,h}`)

A pure-logic helper keeps checkbox detection in the builder:

```c
/* If `off` falls inside a task-list checkbox ("[ ]"/"[x]"), return 1 and set
 * *mark_off to the source offset of the mark char (between the brackets) and
 * *checked to its current state (1 = checked); else return 0. */
int markdown_task_at(const char* text, size_t len, size_t off,
                     size_t* mark_off, int* checked);
```

Implementation: parse with the same flags as `markdown_decorate`
(`MD_FLAG_TASKLISTS`), and for each task `MD_BLOCK_LI` (`is_task`), test whether
`off` ∈ `[task_mark_offset - 1, task_mark_offset + 2)` — i.e. the `[`, the mark
char, or the `]`. On a hit, set `*mark_off = task_mark_offset`,
`*checked = (task_mark == 'x' || task_mark == 'X')`, return 1. This reuses the
existing `MD_BLOCK_LI_DETAIL` fields already consumed by the renderer.

Because md4c parsing is fast, this is called fresh on each click / cursor query
— no persistent checkbox table to keep in sync.

### 2. Body-RichEdit subclass (`src/note_window.c`)

`WM_SETCURSOR` is not delivered through the existing `EN_MSGFILTER` path, so both
the click and the cursor are handled in a subclass on each tab's body RichEdit
(the same `SetWindowSubclass` pattern used elsewhere in the file). Keyboard
shortcuts stay on the `EN_MSGFILTER` path, unchanged.

```c
static LRESULT CALLBACK nw_body_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR id, DWORD_PTR ref);
```

- **`WM_LBUTTONDOWN`:** build a `POINTL` from `lp` (client coords of the edit),
  `EM_CHARFROMPOS` → char offset. Read the body via `nw_body_text` and call
  `markdown_task_at`. If it is a checkbox → toggle it (below) and **consume** the
  message (return 0 without calling `DefSubclassProc`, so RichEdit never moves
  the caret). Otherwise `DefSubclassProc` (normal caret placement / selection).
- **`WM_SETCURSOR`:** `GetCursorPos` → `ScreenToClient(h)` → `EM_CHARFROMPOS` →
  `markdown_task_at`. If a checkbox, `SetCursor(LoadCursorW(NULL, IDC_HAND))` and
  return `TRUE`; otherwise `DefSubclassProc` (default I-beam).

The subclass is attached wherever a tab's body edit is created (`WM_CREATE`
per-tab loop and `nw_add_tab`), keyed by the `NoteWin*` (`ref`).

### 3. Toggle mutation

Flip the single source char at `mark_off` between space and `x`:

- Mute the edit's event mask (`EM_SETEVENTMASK 0`), as the other text-mutating
  shortcuts do, so the programmatic edit does not re-arm the debounce.
- `EM_EXSETSEL {mark_off, mark_off+1}`; `EM_REPLACESEL` with `L"x"` (if currently
  unchecked) or `L" "` (if checked).
- Run the existing save + reformat path (`nw_reformat_now` — persists the note
  and restyles), which flips the strike-through and `[ ]`↔`[x]` rendering.
- Restore the event mask.

The caret is not repositioned by the toggle (the click was consumed before
RichEdit set a caret).

## Testing

- `markdown_task_at` is pure logic → real TDD in `tests/test_markdown.c`:
  - offset on the `[`, on the mark, and on the `]` of `- [ ] a` → returns 1 with
    the right `mark_off` and `checked == 0`.
  - `- [x] b` → `checked == 1`.
  - an offset in the item text (not the checkbox) → returns 0.
  - a non-task bullet `- a` → returns 0.
- Click→toggle, consume-vs-passthrough, and the hand cursor are GUI — gate is
  `make` clean + `make test` green; visual confirmation (click toggles, caret
  unmoved, hand over the box, non-checkbox clicks place the caret) is deferred
  to the human (scripted GUI focus unavailable here).

## Scope boundaries

- **In C2:** `markdown_task_at`, the body-edit subclass (click + cursor), the
  toggle mutation.
- **Deferred:** incremental parse / large-note perf → C3. Rendering the checkbox
  as a ☐/☑ glyph instead of literal `[ ]` (decorations can't insert glyphs —
  would need an OLE object; out of scope).

## Risks

- **`EM_CHARFROMPOS` edge:** a click just past a line end maps to the nearest
  char, which could be on an adjacent line; `markdown_task_at` then correctly
  returns 0 for a non-checkbox offset, so a stray click just places the caret —
  acceptable.
- **Subclass lifetime:** the subclass must be removed or is implicitly dropped
  when the edit is destroyed; `SetWindowSubclass` with the process-lifetime
  window is fine (matches the existing subclasses, which are not explicitly
  removed). No new leak beyond the established pattern.
- **Toggle offset validity:** `mark_off` comes from a fresh parse of the current
  body, so it indexes the live text; the `EM_EXSETSEL`/`EM_REPLACESEL` mutate
  exactly that char. If the char is unexpectedly not a space/`x` (parser drift),
  the flip still writes a valid state and the next reparse reconciles.
