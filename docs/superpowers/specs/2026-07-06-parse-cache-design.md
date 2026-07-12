# Design: Document parse cache (sub-project C3a)

Date: 2026-07-06
Status: Draft (pending review)

## Context

Sub-projects A (decoration engine), B (features), C1 (correctness polish), and
C2 (task toggle) are merged. C ("editing feel / performance") was decomposed
into **C3a (this doc — document parse cache)** and C3b (block-level incremental
reparse, sequenced after C3a behind a measure gate).

**YAGNI re-check outcome (recorded):** md4c has no incremental API and is
microsecond-fast; the debounce restyle is already throttled (150 ms) and
apply-scoped to the caret paragraph. So the big incremental-parse rework is not
where the cost is. The *measured* hot path is the per-event query storm:

- `markdown_fmt_at` runs on **every caret move** (`EN_SELCHANGE` → `nw_update_toggles`),
- `markdown_task_at` runs on **every mouse-move** over the editor (`WM_SETCURSOR`) and every click,

and each does a **fresh whole-document md4c parse**. On a large note, moving the
mouse reparses the whole buffer continuously. C3a removes that by caching the
parse and serving both hot queries from the cache.

Enabling observation: both hot queries call the parser with the **hide-all
sentinel** (`sel_lo > sel_hi`) — a *selection-independent* parse. Only
`nw_restyle` passes a real reveal window. So the sel-independent decoration list
is cacheable and serves both queries directly.

Constraint unchanged: markdown knowledge in `src/markdown.c`; `note_window.c` =
RichEdit mechanics only; offsets are RichEdit `\r`/CP_ACP char offsets.

## Components

### 1. `DECO_TASK` + pure query helpers (`src/markdown.{c,h}`)

Today `markdown_task_at` runs its **own separate parse** (task-only callbacks),
and `markdown_fmt_at` runs a second parse. To let one cached decoration list
answer both, the checkbox is added to the decoration model and both queries
become pure list walks.

- **`DECO_TASK`** — new `DecoKind`. For each task-list item the builder emits it
  over the `[ ]`/`[x]` checkbox range, reusing existing `Deco` fields:
  `start` = the `[` (= `mark_off - 1`), `len` = 3 (i.e. range `[m-1, m+2)`,
  mirroring `markdown_task_at`), `aux_start` = the mark char offset,
  `number` = checked (0/1). Emitted at the existing task branch in `d_text`
  (`src/markdown.c:303-320`, the `if (c->li_is_task)` block) via an
  **unconditional `push`** (NOT `push_hide` — it must be additive to the existing
  task hides and never reveal-gated, or `task_at` would fail inside a revealed
  paragraph). Because it is emitted inside `markdown_decorate` itself,
  `nw_restyle`'s own parse now also yields `DECO_TASK`; `nw_apply_decos` switches
  only on `DECO_FMT`/`DECO_HIDE`/`DECO_PARA`/`DECO_LINK`, so an unknown
  `DECO_TASK` is silently ignored there (no mis-render).
- **`MdFmt markdown_fmt_from_decos(const Deco* d, size_t n, size_t caret)`** —
  the current `markdown_fmt_at` walk lifted to a supplied list: OR of `DECO_FMT`
  whose range covers the caret using the **boundary-inclusive test**
  `(caret>a && caret<=b) || (caret>=a && caret<b)`, then mask out
  `MD_FMT_H1|H2|H3` (headings are not toggles) — both details carried verbatim
  from `markdown_fmt_at`.
- **`int markdown_task_from_decos(const Deco* d, size_t n, size_t off,
  size_t* mark_off, int* checked)`** — scan `DECO_TASK`; if `off` ∈
  `[start, start+len)` set `*mark_off = aux_start`, `*checked = number`,
  return 1; else 0.
- **`markdown_fmt_at` / `markdown_task_at`** are reimplemented as
  `markdown_decorate(hide-all) → *_from_decos → free`. This keeps the existing
  test-facing API working and retires `markdown_task_at`'s separate parser (now
  one parse shape for everything). Callers that already have a cached list use
  the `*_from_decos` variants and parse nothing.

### 2. Per-tab parse cache (`src/note_window.c`)

`NoteTab` gains:
```c
    Deco*  cache_decos;   /* cached hide-all decoration list (NULL until built) */
    size_t cache_n;
    char*  cache_pool;    /* url pool that backs cached DECO_LINK aux offsets */
    int    cache_dirty;   /* 1 => text changed since the cache was built */
```

Accessor:
```c
/* Return the tab's cached sel-independent (hide-all) decoration list, reparsing
 * only when the tab's text changed since the last parse. *n / *pool out. */
static const Deco* nw_decos(NoteWin* nw, int tab, size_t* n, const char** pool);
```
When `cache_decos == NULL` (no cache yet) **or** `cache_dirty`: read the body via
`nw_body_text`, call `markdown_decorate(buf, len, (size_t)-1, 0, &decos, &pool)`
(hide-all), free the old cache, store the new list + pool, clear `cache_dirty`.
Otherwise return the stored list untouched. (First-parse safety comes from the
`cache_decos == NULL` branch — see §3; do not rely on a dirty default.)

**Cache lifetime (must be exact — `NoteTab` slots are a reused fixed array).**
The tab array is shifted in place, so cache pointers must be freed and re-inited
at the right points or a reopened/shifted slot inherits a stale or dangling
`cache_decos`:

- **Free** `cache_decos` + `cache_pool` (and null them, `cache_n = 0`) in
  `nw_close_tab` and `nw_delete_note`, at the point the tab's edit is destroyed,
  **before** `nw_remove_tab` shifts the array.
- In `nw_remove_tab`, after the `nw->tab[k] = nw->tab[k+1]` shift, **null the
  vacated top slot's** cache fields (`cache_decos = NULL; cache_pool = NULL;
  cache_n = 0; cache_dirty = 1`) so the duplicated pointer left by the struct
  copy is not double-freed or reused.
- Explicitly init the cache fields (`cache_decos = NULL; cache_pool = NULL;
  cache_n = 0; cache_dirty = 1`) for a (re)used slot in `nw_add_tab` and in the
  `WM_CREATE` per-tab init loop — `nw_add_tab` reuses `i = nw->ntabs++` and would
  otherwise carry a prior occupant's cache.
- In `WM_DESTROY`, free every tab's cache in a per-tab loop **before** `free(nw)`
  — on window close the OS auto-destroys the child edits without routing through
  `nw_close_tab`, so the close-path free does not fire on window teardown.

### 3. Invalidation

Set `cache_dirty = 1` for the affected tab on **every** text mutation — an
enumerable set, so the accessor always reparses before returning stale offsets:

- `EN_CHANGE` handler (user typing / paste / cut / undo — all fire `EN_CHANGE`
  via the `ENM_CHANGE` mask) → active tab dirty.
- `nw_reformat_now` (the tail of every programmatic mutation: format shortcuts
  `nw_wrap_inline`/`nw_list_prefix`/`nw_handle_enter`, and `nw_toggle_task`) →
  active tab dirty. **Set the dirty flag at the TOP of `nw_reformat_now`**, before
  its own `nw_update_toggles` call — otherwise that post-edit toggle read serves
  stale offsets from the pre-edit cache.
- `nw_load_tab` (sets a tab's content via `SetWindowTextA`) → that tab dirty.

`nw_restyle` changes only char/paragraph *formatting* (no text-length change),
so it correctly does not invalidate. A missed dirty-set would serve stale
offsets, so the set is kept small and explicit; correctness is guarded by the
existing suite (behavior-preserving refactor). Note a fresh `calloc`'d tab has
`cache_dirty == 0`, so **first-parse safety rests on the `cache_decos == NULL`
branch of `nw_decos`, not on a dirty default** (§2).

### 4. Rewire the hot queries

- `nw_update_toggles` → replace `markdown_fmt_at(buf,...)` (a parse) with
  `markdown_fmt_from_decos(nw_decos(nw, nw->active, ...), caret)` — no parse.
- `nw_body_sub` (`WM_LBUTTONDOWN` + `WM_SETCURSOR`) → replace the two
  `markdown_task_at(buf,...)` parses with a lookup over the cached list via
  `markdown_task_from_decos`. A small `nw_tab_of_edit(nw, h)` maps the subclassed
  edit HWND back to its tab index for the cache. This **subsumes the two
  C2-deferred items**: the per-mouse-move reparse becomes a cache read, and the
  click/cursor hit-test unifies on one `nw_decos` path (the duplicated hit-test
  collapses into one helper).

`nw_restyle` is **deliberately not touched** — it keeps its own sel-specific
parse (throttled debounce, not the hot path, and the carefully reviewed reveal
path). Unifying it onto the cache needs a sel-independence refactor (reveal
filtering moved from parse-time to apply-time); that is C3b's foundation, not
C3a.

## Testing

- `DECO_TASK` emission + the two `*_from_decos` helpers are pure logic → real
  TDD in `tests/test_markdown.c`: a `- [ ] a` / `- [x] b` doc yields `DECO_TASK`
  with the right range / `mark_off` / `checked`; `markdown_task_from_decos` hits
  on `[`/mark/`]` and misses in text; `markdown_fmt_from_decos` matches the old
  `markdown_fmt_at` on bold/italic/strike; `markdown_fmt_at`/`markdown_task_at`
  still pass their existing tests (now routed through `*_from_decos`).
- The per-tab cache + rewiring is GUI/mechanical and behavior-preserving — gate
  is `make` clean + `make test` green; the win (no per-event reparse) and no
  visible behavior change are confirmed by the human (click still toggles, hand
  cursor still shows, sidebar toggles still track the caret).

## Scope boundaries

- **In C3a:** `DECO_TASK`, the `*_from_decos` query helpers, the per-tab
  hide-all parse cache + invalidation, rewiring `fmt_at`/`task_at` consumers onto
  the cache, freeing the cache on tab/window teardown. Subsumes the two
  C2-deferred perf items.
- **Deferred to C3b (behind a measure gate):** block-level incremental reparse
  (region-scoped dirty reparse + boundary-ripple detection) and the
  `nw_restyle` sel-independence unification it builds on.

## Risks

- **Stale cache from a missed dirty-set:** the invalidation set must cover every
  text mutation. Mitigations: the set is small/enumerable (§3); a fresh tab
  parses via the `cache_decos == NULL` branch (not a dirty default — `calloc`
  zeroes `cache_dirty` to 0); the query helpers tolerate an out-of-range offset
  (return 0 / no fmt) rather than reading past the list.
- **Cache lifetime across tab-slot reuse (highest risk):** `NoteTab` is a reused
  fixed array; the free/null/init points in §2 must all be present or a shifted
  or reopened slot inherits a stale/dangling `cache_decos` (wrong offsets or
  double-free). `WM_DESTROY` must free every tab's cache in a loop — child edits
  are OS-destroyed on window close without passing through `nw_close_tab`.
- **`cache_pool` is unused by C3a readers** (`markdown_{fmt,task}_from_decos`
  ignore link `aux`/pool; the only pool consumer, `nw_apply_decos`'s link table,
  runs off `nw_restyle`'s own parse). It must still be **freed** with each
  reparse (the `Deco*`/pool ownership contract from `nw_restyle`); retaining it
  across calls is harmless and pre-wires C3b, but it is not load-bearing here.
- **`nw_tab_of_edit` on a stale HWND:** the subclass only runs for a live edit,
  so the lookup always finds the tab; if it ever returns -1 the handler falls
  through to default (no toggle / I-beam), which is safe.
- **Offset space:** the cached decos index the same `\r`/CP_ACP buffer the
  queries use (`nw_body_text`), identical to today — no new coordinate risk.
