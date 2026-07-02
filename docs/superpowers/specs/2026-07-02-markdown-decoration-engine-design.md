# Design: Decoration-driven markdown styling engine (sub-project A)

Date: 2026-07-02
Status: Draft (pending review)

## Context: where this sits

"Smoother notetaking" decomposed into three sequenced sub-projects:

- **A (this doc) — rearchitect the styling engine** (foundation; folds in rendering correctness).
- **B — more markdown features** (fenced code, blockquotes, links, task lists, tables). Later, its own spec.
- **C — editing feel / performance** (cross-buffer incremental parse, scroll/latency polish). Later, its own spec.

Stack decision (made during brainstorming): **keep md4c + RichEdit**. The parser
(md4c) is fast and correct and is not the bottleneck; the friction is that
RichEdit is not a decoration-native editor. Alternatives were weighed —
WebView2+CodeMirror6 (best fidelity but abandons native-C editing),
Scintilla (no clean inline hide), custom DirectWrite editor (months, high
risk) — and rejected in favor of adapting the proven CodeMirror *decoration
pattern* onto RichEdit.

## Problem

The current live-preview styling is spread across `note_window.c` and mixes
concerns:

- `markdown.c` flattens md4c output to inline/heading spans only; **lists are
  handled by ad-hoc line scanning** (`nw_apply_lists`, `nw_marker_len`).
- **Marker hiding is heuristic** (`nw_apply_hidden` scans adjacent `* _ \` ~`),
  which mis-hides at some boundaries.
- The span array is a **fixed 256 cap** — large notes lose formatting.
- Styling logic lives in the GUI layer, so it is **not unit-testable**.
- Markers are **always hidden**, so editing the raw markdown is awkward.

## Chosen approach: a decoration list (the CodeMirror pattern)

Research across Obsidian / Logseq / Zettlr shows one shared model: an editor
plus an incremental parser feeding a **Decoration API** — a flat, sorted set of
typed range operations (mark = style, replace = hide, widget = insert). We
adopt the same model, sourced from md4c and applied to RichEdit.

A single module owns **all** markdown knowledge and produces a decoration list
from `(text, selection)`. `note_window.c` becomes a dumb executor that applies
the list to RichEdit. Because the module is pure logic, it is unit-testable.

## Components

### 1. Module interface (`src/markdown.{c,h}`, extended)

```c
typedef enum { DECO_FMT, DECO_HIDE, DECO_PARA } DecoKind;

typedef enum { PARA_NONE, PARA_BULLET, PARA_NUMBER } ParaKind;

typedef struct {
    DecoKind kind;
    size_t   start;      /* RichEdit char offset (\r-separated, CP_ACP) */
    size_t   len;
    MdFmt    fmt;        /* DECO_FMT: bold/italic/code/strike/H1..H3   */
    ParaKind para;       /* DECO_PARA: list kind                       */
    int      number;     /* DECO_PARA + PARA_NUMBER: the ordinal       */
} Deco;

/* Build the full-document decoration list. Markers are hidden everywhere
 * EXCEPT in the paragraph(s) intersecting [sel_lo, sel_hi] (cursor-aware
 * reveal). Pass sel_lo > sel_hi (e.g. (size_t)-1, 0) to hide all markers.
 * *out is malloc'd and owned by the caller (free()). Returns the count.
 * Grows dynamically — no fixed cap. */
size_t markdown_decorate(const char* text, size_t len,
                         size_t sel_lo, size_t sel_hi, Deco** out);

/* Inline format flags active at a caret offset, for the sidebar toggle
 * highlights — replaces the separate markdown_spans walk in
 * nw_update_toggles so the document is parsed one way only. */
MdFmt markdown_fmt_at(const char* text, size_t len, size_t caret);
```

`markdown_spans` (and the `MdSpan` type) is retired once `nw_update_toggles`
and the styling path move to the new API; `test_markdown.c` is rewritten
against `markdown_decorate` / `markdown_fmt_at`.

### 2. Decoration production (inside the module)

Driven entirely by md4c callbacks:

- **Inline** (`MD_SPAN_STRONG/EM/CODE/DEL`): emit `DECO_FMT` over the content
  range; emit `DECO_HIDE` for the delimiter runs. **Marker lengths are derived
  deterministically** from span type — bold `**`=2, em `*`/`_`=1, strike `~~`=2,
  code = the contiguous backtick run at each content boundary — not by scanning
  arbitrary adjacent punctuation. This fixes the current mis-hide cases.
- **Headings** (`MD_BLOCK_H`): `DECO_FMT` (H1/H2/H3 sizing) over the content;
  `DECO_HIDE` the leading `#… ` from line start to content start.
- **Lists** (`MD_BLOCK_UL/OL` + `MD_BLOCK_LI`): emit `DECO_PARA`
  (PARA_BULLET / PARA_NUMBER + ordinal) per list item and `DECO_HIDE` the
  literal `- ` / `N. ` marker. This **retires `nw_apply_lists` and
  `nw_marker_len`** — native md4c list parsing replaces line scanning.

Content offsets come from md4c's text callback (as today); block/span
structure gives exact boundaries.

### 3. Cursor-aware reveal (paragraph granularity)

`markdown_decorate` receives the selection. For every marker it would hide, it
checks whether the marker's **source paragraph** (line up to `\n`/`\r`)
intersects the selection range; if so, the `DECO_HIDE` is omitted (raw markdown
shows) — for all paragraphs the selection touches. Everywhere else, markers
hide. `DECO_FMT`/`DECO_PARA` are unaffected (text stays styled while revealed).

### 4. Paragraph-scoped application (`note_window.c`)

The executor restyles at **paragraph granularity**, not whole-buffer:

- **On load** (`WM_CREATE` per tab): decorate the whole document, reset base
  char format over `[0, len]`, apply all decos.
- **On edit** (debounce tick): decorate the whole document (md4c parse is
  cheap). Scope the reset + reapply to the paragraph(s) the caret/selection
  occupies at debounce time — edits land at the caret, so that is the changed
  region. **Correctness fallback:** if the selection spans more than one
  paragraph (e.g. a multi-paragraph paste or cut), reset + reapply the whole
  buffer. Scoping is an optimization layered over an always-correct full
  restyle; when in doubt, restyle everything.
- **On caret move across paragraphs** (`EN_SELCHANGE`): re-decorate with the new
  selection and reapply to the **previously-active and newly-active
  paragraphs** only, flipping their marker visibility (the reveal effect).

Rationale: md4c reparsing whole-doc is microseconds; the expense is RichEdit
messages + reflow, so scoping *application* to affected paragraphs is what
makes reveal smooth. This also seeds the incremental foundation sub-project C
extends. (Full cross-buffer incremental parsing stays in C.)

The apply step keeps the existing safeguards: mute `EM_SETEVENTMASK` during
programmatic styling, save/restore the real caret, `WM_SETREDRAW` around bulk
changes.

### 5. Integration points

- `nw_apply_format_edit` → build decos via `markdown_decorate`, apply
  paragraph-scoped. `nw_apply_hidden`, `nw_apply_lists`, `nw_set_range_fmt`
  helpers collapse into the executor + module.
- `nw_update_toggles` → `markdown_fmt_at` (one parse path, no duplicate).
- Text-mutating shortcuts (`nw_wrap_inline`, `nw_list_prefix`,
  `nw_handle_enter`) stay as-is; they still trigger a reformat. Aligning their
  internal line-scanning with md4c is out of scope for A.

## Testing

`test_markdown.c` is rewritten to assert decoration lists directly (pure logic,
no GUI):

- inline: `**bold**`, `*em*`, `` `code` ``, `~~del~~`, nested (`**a *b* c**`),
  content that begins/ends with punctuation (regression for the old scan).
- headings: `# H1`..`### H3`, hidden `#… ` range.
- lists: bullets, numbered with correct ordinals, hidden markers.
- reveal: same input with a selection inside one paragraph asserts that
  paragraph's markers are NOT hidden while others are.
- `markdown_fmt_at` at various caret offsets incl. format boundaries.
- large input beyond the old 256 cap returns a complete list (no truncation).

`make test` remains the logic gate. Paragraph-scoped application and reveal in
RichEdit are GUI behavior — verified manually by launch + screenshot (scripted
GUI focus is unavailable in this environment).

## Scope boundaries

- **In A:** decoration model + module, exact markers, native md4c lists,
  dynamic storage, cursor-aware paragraph reveal, paragraph-scoped restyle,
  unit tests. Visible feature set ≈ today (bold/italic/code/strike/H1–H3/lists)
  but correct, revealing, and uncapped.
- **Deferred to B:** fenced code blocks, blockquotes, links, task lists, tables.
- **Deferred to C:** cross-buffer incremental parsing, typing latency, scroll
  preservation, aligning the text-mutating shortcuts with md4c.

## Risks

- **Marker derivation for code spans / nested emphasis** — must match md4c's
  actual tokenization; covered by the nested + punctuation-boundary tests.
- **Paragraph identification** must use the same `\r`/`\n` offset convention as
  RichEdit's internal char positions (the existing CP_ACP/`EM_GETTEXTEX` path),
  or decos land on wrong characters — same invariant the current code relies on.
- **Reveal churn on caret movement** — scoped application keeps it cheap, but if
  a note is one giant paragraph the scope degrades to whole-buffer; acceptable
  for A, revisited in C.
- **Retiring `markdown_spans`** touches `nw_update_toggles`; verify sidebar
  highlights still track the caret after the switch to `markdown_fmt_at`.
