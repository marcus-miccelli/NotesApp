# Design: Span-level reveal for inline markdown markers

Date: 2026-07-13
Status: Approved

## Context

The live preview currently reveals markdown syntax per **paragraph**:
`in_reveal` (`src/markdown.c:53`) shows every hidden marker whose source
paragraph intersects the reveal window `[sel_lo, sel_hi]`. The user wants
**span-level** reveal for inline formatting — `**bold**` re-renders the
moment the caret escapes the span, even when the caret stays on the same
line. This is Obsidian Live Preview behavior (CodeMirror 6 decorations —
the same pattern the sub-project A engine copied).

Decisions made during brainstorming:

- **Scope: inline only.** Bold/italic/strikethrough, code spans, and links
  (inline + autolink) become span-revealed. Block markers — heading `#`,
  list/task prefixes, blockquote `>`, code fences — keep the current
  line/paragraph reveal (Obsidian does the same; a heading's `#` re-hiding
  while still editing the title feels jumpy).
- **Boundaries: touching = raw.** A span stays revealed while the caret or
  selection touches its extent `[open_marker_start … close_marker_end]`,
  **boundary-inclusive** (caret position immediately before the opening
  marker's first char or immediately after the closing marker's last char
  still counts as touching). Typing the closing `**` leaves the span raw;
  moving one more position away renders it. Matches the existing
  boundary-inclusive caret test in `markdown_fmt_from_decos`.
- **Nesting: one rule.** The caret inside an inner span necessarily
  touches every enclosing span's extent, so all enclosing spans reveal.
  No special cases.
- **Links: whole-syntax extent.** `[text](url)` reveals while the caret is
  anywhere from `[` through `)` (url included); autolink `<url>` from `<`
  through `>`.

Constraint unchanged: markdown knowledge in `src/markdown.c`;
`note_window.c` = RichEdit mechanics only; offsets are RichEdit `\r`/CP_ACP
char offsets; no new dependencies; C11.

## Components

### 1. Builder: extent-gated inline hides (`src/markdown.c`)

Today an inline span's **opening** marker hide is pushed when the span's
first text run resolves it (`d_text` resolve loop, `src/markdown.c:277-289`)
and its **closing** marker hide at span close (`d_span_close`,
`src/markdown.c:250`) — both gated by the paragraph-level `in_reveal`.
Changes:

- **Record, don't push, opening markers.** The open-span stack entry
  (`c->sp[i]`) gains the opening marker's start offset and length, recorded
  where the resolve loop currently pushes the hide. The push moves to span
  close, where the full extent `[open_start, close_end)` is known. md4c
  reports only balanced spans (an unclosed `**abc` is literal text and
  never reaches the span callbacks), so every recorded opening marker is
  eventually pushed exactly once.
- **New gate.** `in_reveal_span(c, ext_start, ext_end)`: reveal (i.e. skip
  the hide) iff `sel_lo <= ext_end && sel_hi >= ext_start` — an inclusive
  overlap test between the reveal window and the span extent, replacing the
  paragraph test for inline marker hides only. The hide-all sentinel
  (`sel_lo > sel_hi`) still hides everything. Block-level hides keep
  `in_reveal` unchanged.
- **Links.** The `[`/`<` hide currently pushed at link-text start
  (`src/markdown.c:291-300`) is likewise recorded and pushed at the link's
  span close alongside the `](url)` / `>` hides, all gated on the link's
  full extent.
- **Extent on the Deco.** Every inline-marker `DECO_HIDE` carries its span
  extent in the aux fields (unused by `DECO_HIDE` today): `aux_start` =
  extent start, `aux_len` = extent length. Block-level hides leave them 0.
  The C3a hide-all cache therefore contains every inline hide *with* its
  extent — which powers the trigger helper below and keeps `nw_restyle`'s
  own sel-specific parse and the cache structurally identical.
- **New pure query.** `size_t markdown_reveal_sig(const Deco* d, size_t n,
  size_t caret)` — a deterministic signature of the set of inline span
  extents the caret touches (e.g. fold of `aux_start*2654435761u ^ aux_len`
  over touching extents; 0 when none). Two caret positions produce the same
  signature iff they touch the same extent set (collisions astronomically
  unlikely; worst case a missed restyle on a hash collision — cosmetic,
  self-heals on the next caret move). Pure list walk over the cached
  hide-all decos; no parse.

Deco order note: deferring opening-marker pushes to close time makes the
`Deco[]` no longer strictly left-to-right. `nw_apply_decos` and the
`*_from_decos` helpers iterate without order assumptions; tests pin this.

### 2. Trigger: restyle on reveal-set change (`src/note_window.c`)

Today a caret move re-styles only when the caret changes paragraph
(`last_caret_para`, sub-project C1). Span-level reveal must also react to
moves *within* a paragraph, but only when they matter:

- `NoteTab` gains `size_t last_reveal_sig`.
- On `EN_SELCHANGE`: compute `markdown_reveal_sig` over the C3a cached
  decos (`nw_decos` — cache read, no parse) at the new caret. If it differs
  from `last_reveal_sig` (or the paragraph changed, as today), trigger the
  existing restyle path and store the new signature.
- Arrow-keying through plain text: signature stays 0 → no restyle, same
  cost as today.
- The restyle must re-apply the paragraph the caret **left** as well as the
  one it entered — the existing paragraph-transition machinery already
  re-applies old + new caret paragraphs; entering/leaving a span within one
  paragraph re-applies that paragraph. No new apply scope logic.
- Invalidation unchanged: text mutations already dirty the cache (C3a);
  `last_reveal_sig` is recomputed from fresh decos on the next selchange.

### 3. What does not change

- Block markers (`#`, `- `, `>` , fences, task `[ ]` hide behavior) keep
  paragraph reveal via `in_reveal`.
- `markdown_fmt_at` / `markdown_task_at` / `markdown_fmt_from_decos` /
  `markdown_task_from_decos` semantics (aux fields on DECO_HIDE are new
  payload, ignored by existing consumers; `DECO_LINK`/`DECO_TASK` untouched).
- `nw_restyle`'s debounce and apply scoping; the C3a cache lifecycle.

## Testing

Pure TDD in `tests/test_markdown.c` (the builder + queries are pure logic):

- `a **b** c` with caret in/at-edges/outside the span: marker hides absent
  while touching (both boundaries inclusive), present once escaped —
  same-paragraph caret both times (proves the gate is span, not paragraph).
- Nested `**a *b* c**`: caret in inner span → both spans' hides absent;
  caret in outer only → inner's hides present, outer's absent.
- Link `[t](u)`: caret inside url → all link syntax hides absent; caret
  outside → present. Autolink `<u>` likewise.
- Code span backticks same gate.
- Block markers still paragraph-revealed (heading `#` shows with caret
  anywhere on its line, hides from another line).
- Hide-all sentinel emits every hide with correct `aux_start`/`aux_len`
  extents (cache contract).
- `markdown_reveal_sig`: 0 in plain text; nonzero and stable inside a span;
  changes when crossing a span boundary; equal for two carets inside the
  same span; inner-vs-outer nested positions differ.
- GUI feel (flicker-free arrow-keying, escape-renders-instantly) is
  human-verified (GUI rule).

## Scope boundaries

- **In:** extent-gated inline marker hides + extent payload on DECO_HIDE,
  `in_reveal_span`, deferred opening-marker pushes, `markdown_reveal_sig`,
  `last_reveal_sig` trigger wiring in `note_window.c`, tests.
- **Out:** block-marker span reveal, apply-time reveal filtering (C3b
  foundation), any change to `nw_restyle` debounce/apply scoping, incremental
  reparse.

## Risks

- **Deco reorder** (deferred opening pushes): consumers verified
  order-independent; pinned by tests.
- **Boundary off-by-one:** the inclusive-touch definition is pinned by
  explicit edge tests at both span boundaries (the C2 caret work showed
  RichEdit offset edges are where regressions hide).
- **Missed restyle on signature collision:** hash fold makes this
  astronomically rare; consequence is cosmetic (marker stays revealed until
  the next caret move recomputes).
- **Restyle frequency:** worst case one extra restyle per span
  entry/exit — bounded, paragraph-scoped, and far below the per-event
  reparse storm C3a removed. The signature check itself is a cache-read
  list walk (µs).
- **Same-paragraph leave/enter:** both old and new positions lie in
  re-applied paragraphs via the existing transition machinery; the tests
  cannot cover the apply step (GUI), so the human checklist includes
  same-line span exit.
