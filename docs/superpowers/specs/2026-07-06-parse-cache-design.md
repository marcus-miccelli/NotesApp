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
  `start` = the `[`, `len` = 3 (`[ ]`), `aux_start` = the mark char offset,
  `number` = checked (0/1). Emitted at the existing task branch in `d_text`
  (`src/markdown.c:303-320`, the `if (c->li_is_task)` block).
- **`MdFmt markdown_fmt_from_decos(const Deco* d, size_t n, size_t caret)`** —
  the current `markdown_fmt_at` walk (OR of `DECO_FMT` covering the caret,
  headings masked out), lifted to operate on a supplied list.
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
When `cache_dirty` (or no cache yet): read the body via `nw_body_text`, call
`markdown_decorate(buf, len, (size_t)-1, 0, &decos, &pool)` (hide-all), free the
old cache, store the new list + pool, clear `cache_dirty`. Otherwise return the
stored list untouched. The cache is freed (`cache_decos` + `cache_pool`) wherever
a tab's edit is destroyed and when the `NoteWin` is freed.

### 3. Invalidation

Set `cache_dirty = 1` for the affected tab on **every** text mutation — an
enumerable set, so the accessor always reparses before returning stale offsets:

- `EN_CHANGE` handler (user typing / paste) → active tab dirty.
- `nw_reformat_now` (the tail of every programmatic mutation: format shortcuts
  `nw_wrap_inline`/`nw_list_prefix`/`nw_handle_enter`, and `nw_toggle_task`) →
  active tab dirty.
- `nw_load_tab` (sets a tab's content) → that tab dirty.

A missed dirty-set would serve stale offsets, so the set is kept small and
explicit; correctness is guarded by the existing suite (behavior-preserving
refactor) plus the fact that dirty defaults to 1 for a fresh (calloc'd) tab.

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
  text mutation. Mitigations: the set is small/enumerable (§3); `cache_dirty`
  defaults to 1 for a fresh tab; the query helpers tolerate an out-of-range
  offset (return 0 / no fmt) rather than reading past the list.
- **Cache lifetime:** `cache_decos`/`cache_pool` must be freed on tab close and
  `NoteWin` destroy, matching the existing `Deco*`/pool free discipline in
  `nw_restyle`; a missed free leaks per reparse.
- **`nw_tab_of_edit` on a stale HWND:** the subclass only runs for a live edit,
  so the lookup always finds the tab; if it ever returns -1 the handler falls
  through to default (no toggle / I-beam), which is safe.
- **Offset space:** the cached decos index the same `\r`/CP_ACP buffer the
  queries use (`nw_body_text`), identical to today — no new coordinate risk.
