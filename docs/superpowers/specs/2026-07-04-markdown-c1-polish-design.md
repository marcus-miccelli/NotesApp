# Design: Correctness polish + cleanups (sub-project C1)

Date: 2026-07-04
Status: Draft (pending review)

## Context

Sub-projects A (decoration engine) and B (code blocks / quotes / tasks / links)
are merged. The remaining work was grouped as C (editing-feel / performance) and
decomposed into **C1 (correctness polish + cleanups, this doc)**, C2 (clickable
task toggle), C3 (incremental parse/perf). C1 clears the visible warts deferred
from A and B plus two trivial cleanups.

Constraint unchanged from A/B: decorations style or hide existing source
characters only; all markdown knowledge lives in `src/markdown.c`;
`note_window.c` is RichEdit mechanics only; marker hides route through
`push_hide` (cursor-reveal); offsets are RichEdit `\r`/CP_ACP char offsets.

## Items

### 1. Link overhaul — support inline + reference + shortcut + autolink

**Problem (from B review):** links used a source-adjacent url scan and only
completed the inline `[t](url)` form. Reference (`[t][ref]`), shortcut (`[t]`),
and autolink (`<http://x>`) links got their opening `[` hidden and `MD_FMT_LINK`
applied but were never completed — leaving orphaned syntax, no table entry, and
no click.

**Approach:** md4c already resolves *all* link types and hands the decoded URL
in `MD_SPAN_A_DETAIL.href` (email autolinks arrive with `mailto:` prepended). A
reference link's URL lives in a `[ref]: url` definition elsewhere, so it is not
adjacent in source — therefore the URL must come from `href`, not a source scan.

Because `href` is decoded text in md4c's transient callback buffer (not our
source buffer), and strings cannot live in a fixed `Deco`, add a **per-parse URL
pool**:

```c
/* markdown_decorate gains an out-param for the URL pool. */
size_t markdown_decorate(const char* text, size_t len, size_t sel_lo,
                         size_t sel_hi, Deco** out, char** urlpool);
```

- `urlpool` is a `malloc`'d char buffer the caller frees (NULL-safe; may be NULL
  when there are no links). `DECO_LINK.aux_start`/`aux_len` become offsets/length
  into `urlpool` (no longer into the source buffer).
- On `MD_SPAN_A` enter, copy `MD_SPAN_A_DETAIL.href` (concatenating its
  `MD_ATTRIBUTE` substrings) into the pool; record the pool offset/len for the
  matching `DECO_LINK` emitted on leave.
- `markdown_fmt_at` calls `markdown_decorate` and now also frees the pool.

**Per-type syntax hiding**, all completed in `d_leave_span`'s `MD_SPAN_A` branch
(the B inline-only guard `base[te]==']' && base[te+1]=='('` is removed):

- inline `[t](url)` → hide `[` (before text) and `](url)` (scan `]` `(` … `)`).
- reference `[t][ref]` → hide `[` and `][ref]` (scan `]` `[` … `]`).
- shortcut / collapsed `[t]` / `[t][]` → hide `[` and the trailing `]` (or `][]`).
- autolink `<http://x>` (`MD_SPAN_A_DETAIL.is_autolink`) → visible text is the
  URL itself; hide the `<` at `a_text_start-1` and the `>` at the text end.

Since md4c emits `MD_SPAN_A` only for genuinely resolved links, every span is
completed — the orphaned-`[` case cannot occur. `note_window`'s link-table fill
copies the URL from `urlpool` (using `aux_start`/`aux_len`) instead of the
source buffer; everything else in the table / `EN_LINK` Ctrl+click path is
unchanged (still scheme-gated to http/https/mailto).

### 2. Multi-paragraph paste scoping (A fix)

**Problem:** `nw_restyle` only widens to whole-document when the *selection*
spans multiple paragraphs. A multi-line paste collapses the caret, so the
debounce restyle scopes to the caret's paragraph and leaves the earlier pasted
paragraphs unstyled until the next cross-paragraph caret move.

**Fix:** track a per-tab line count (`EM_GETLINECOUNT` or counting `\n` in the
read buffer). On the debounce restyle, if the current line count differs from the
stored one (a multi-line insert/delete happened), force whole-document restyle
(`scoped = 0`); otherwise keep the scoped fast path. Update the stored count each
restyle. Correctness fallback layered over the scoped optimization.

### 3. Blockquote continuation `> ` (B fix)

**Problem:** only the first line's `> ` is hidden; continuation `> ` on wrapped
source lines of a multi-line blockquote stay visible.

**Fix:** on `MD_BLOCK_QUOTE` leave, scan the quote's source extent
(`[quote_start, last_text_end)`, where `quote_start` is the line start recorded
at the quote's first text) and `push_hide` the leading `>`/`> ` at every
contained source line-start. Reveal-gated per line via `push_hide`, so the caret
line still shows its `> `.

### 4. Per-tab `last_caret_para` (A cleanup)

Move the single `NoteWin.last_caret_para` field onto each `NoteTab`, so switching
tabs can't collide offsets and suppress a needed reveal restyle. The
`EN_SELCHANGE` handler reads/writes the active tab's field.

### 5. Rename `nw_hide_range2` → `nw_hide_range` (A cleanup)

The original `nw_hide_range` was deleted during A's integration; the replacement
was named `nw_hide_range2` to avoid a redeclaration. Rename it back now that the
old one is gone.

## Testing

- Builder changes are pure logic → real TDD in `tests/test_markdown.c`:
  - reference link `"[t][r]\n\n[r]: http://x"` → `DECO_LINK` whose pooled URL is
    `http://x`, with `[` and `][r]` hidden.
  - autolink `"<http://x>"` → `DECO_LINK` URL `http://x`, `<`/`>` hidden, visible
    text is the URL.
  - inline link still correct (regression), including nested emphasis
    (`[**b**](http://x)`).
  - 2-line blockquote `"> a\n> b"` → both `> ` markers hidden.
- Paste-scoping, per-tab caret, and the rename are GUI/mechanical — gate is
  `make` clean + `make test` green; visual confirmation deferred to the human
  (scripted GUI focus unavailable here).

## Scope boundaries

- **In C1:** the five items above.
- **Deferred:** clickable task toggle → C2; incremental parse/perf → C3;
  `DECO_LINK.start` including hidden leading markers when link text starts with
  markup (benign hit-test, still present) — left as-is.

## Risks

- **URL pool lifetime / offsets:** `aux_start`/`aux_len` now index `urlpool`, not
  the source buffer — every consumer (`note_window` link-table fill,
  `markdown_fmt_at`) must be updated together, and both allocations
  (`Deco*` + pool) freed. A missed free leaks; a stale source-offset read would
  corrupt the URL.
- **Autolink offsets:** confirmed the vendored md4c exposes
  `MD_SPAN_A_DETAIL.href` / `title` / `is_autolink` and `MD_ATTRIBUTE.text`/
  `size` (strings are NOT NUL-terminated — copy by `size`). Copy `href.text[0..
  href.size]` into the pool; use `is_autolink` to pick the `<`/`>` hiding form.
- **Reference-link source scan:** `][ref]` hiding must find the correct closing
  `]` when `ref` is empty (`[t][]`) or the shortcut form (`[t]`) — bounded scans
  stopping at line end.
- **Blockquote extent scan:** nested block structures inside a quote (a list in a
  quote) are uncommon; the line-start `> ` scan should hide only the quote
  prefix, not misfire inside nested content — keep it to lines beginning with
  optional spaces then `>`.
