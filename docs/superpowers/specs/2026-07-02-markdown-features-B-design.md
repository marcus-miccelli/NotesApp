# Design: More markdown features on the decoration engine (sub-project B)

Date: 2026-07-02
Status: Draft (pending review)

## Context

Sub-project A shipped the decoration-list engine (`markdown_decorate` in
`src/markdown.c` emits a flat `Deco[]` of `DECO_FMT` / `DECO_HIDE` / `DECO_PARA`;
`note_window.c` applies them to RichEdit; cursor-aware paragraph reveal). B adds
more markdown syntax on that engine. C (perf / editing-feel) follows.

Feature set (chosen during brainstorming): **fenced code blocks, blockquotes,
links (Ctrl+click to open), task-list rendering**. Tables are excluded (they
need table *structure*, not styling — outside the decoration model). Clickable
task toggling and nested lists remain deferred to C / A's known limitations.

Constraint that shaped scope: **decorations can only style or hide existing
characters — they cannot insert glyphs or structure.** Every B feature is
expressed as style + hide of source characters.

## Components

### 1. Parser + data-model extensions (`src/markdown.h`, `src/markdown.c`)

- Add `MD_FLAG_TASKLISTS` to the md4c parser flags (blockquotes, fenced code,
  and links are already CommonMark under the existing flags).
- New `MdFmt` bits (extend the existing `1u<<0 … 1u<<6`):
  - `MD_FMT_CODEBLOCK` (`1u<<7`)
  - `MD_FMT_QUOTE` (`1u<<8`)
  - `MD_FMT_LINK` (`1u<<9`)
- New `ParaKind`: `PARA_QUOTE`.
- New `DecoKind`: `DECO_LINK`. Links need a URL, which the current `Deco`
  cannot carry. `DECO_LINK` stores **offsets into the same source buffer** for
  the url span — no strings in `Deco`:
  ```c
  typedef enum { DECO_FMT, DECO_HIDE, DECO_PARA, DECO_LINK } DecoKind;
  /* Deco gains two fields, used only by DECO_LINK: */
  size_t aux_start;  /* url span start in the source buffer */
  size_t aux_len;    /* url span length */
  ```
  A `DECO_LINK` marks the visible link-text range in `start`/`len` and the url
  in `aux_start`/`aux_len`. (Chosen over embedding a `char[]` in every `Deco`,
  which would bloat the struct; the url is copied once, at apply time, into the
  per-tab link table where clicks are handled.)

### 2. Builder additions (`src/markdown.c`)

Driven by md4c callbacks, same deterministic marker derivation as A. All hides
go through the existing `push_hide`, so cursor-reveal works for the new syntax
automatically (paragraph-scoped).

- **Fenced code block** (`MD_BLOCK_CODE`): emit `DECO_FMT` `MD_FMT_CODEBLOCK`
  over the block's content; `push_hide` the opening and closing fence lines
  (line-span each). Content is md4c literal text — not re-parsed for inline.
- **Blockquote** (`MD_BLOCK_QUOTE` / its child blocks): per contained line emit
  `DECO_FMT` `MD_FMT_QUOTE` over the content and `DECO_PARA` `PARA_QUOTE` on the
  paragraph; `push_hide` the leading `> ` (line-start → content).
- **Link** (`MD_SPAN_A`, detail `MD_SPAN_A_DETAIL.href`): emit `DECO_FMT`
  `MD_FMT_LINK` over the visible link text and a `DECO_LINK` carrying the same
  text range + the url offsets. `push_hide` the `[` immediately before the text
  and the `](url)` run after it (scan forward from text end across `]`, `(`, the
  url, to the matching `)` — md4c has already validated the link).
- **Task list** (`MD_BLOCK_LI` with task detail under `MD_FLAG_TASKLISTS`): emit
  the existing bullet `DECO_PARA` for the item, `push_hide` the `- ` marker, and
  emit `DECO_FMT` over the `[ ]`/`[x]` so it styles as a checkbox (checked →
  dim/strike via a fmt flag reuse; see rendering). Render-only.

### 3. Rendering (executor — `src/note_window.c`)

Extend `nw_fmt_range` / `nw_para_range` and add a link table.

- `MD_FMT_CODEBLOCK` → keep IBM Plex Mono + set `CFM_BACKCOLOR` /
  `crBackColor = COL_CODE_BG` (new palette constant `RGB(0x18,0x18,0x1c)`).
- `MD_FMT_QUOTE` → dim text color (e.g. `RGB(0x9a,0x9a,0xa2)`); `PARA_QUOTE` →
  `PARAFORMAT2` `dxStartIndent` (e.g. `nw_s(hwnd, 280)`-equivalent twips).
- `MD_FMT_LINK` → accent color (`COL_ACCENT`) + underline (`CFE_UNDERLINE`) +
  `CFE_LINK` (so RichEdit emits `EN_LINK` over the range).
- Task checkbox → the `[ ]`/`[x]` styled dim; a checked item's text may render
  strikethrough (reuse `MD_FMT_STRIKE` on the item content) — decided at build
  time from md4c's `is_checked`.
- **Link table:** a `struct { LONG a, b; char url[512]; }` list on `NoteWin`,
  cleared and refilled inside `nw_apply_decos` from the `DECO_LINK` decorations
  (copying `buf[aux_start..]`). Only the active tab's body is visible/focused, so
  `EN_LINK` only fires for it; the single table tracks whatever was last
  restyled. Bounded count (e.g. 256 entries); extra links beyond the cap are
  simply not clickable (still styled).

### 4. Link activation + safety (`src/note_window.c`)

- Add `ENM_LINK` to `EDIT_EVENT_MASK` so RichEdit reports `EN_LINK`.
- Handle `EN_LINK` in `WM_NOTIFY`: on `((ENLINK*)lp)->msg == WM_LBUTTONUP` **and**
  `GetKeyState(VK_CONTROL) & 0x8000`, take the reported `chrg`, find the link
  table entry whose `[a,b)` contains it, and open its url. Non-Ctrl clicks fall
  through to default (caret placement).
- **Security:** open only `http:`, `https:`, `mailto:` urls (scheme parsed
  case-insensitively from the front of the url). Everything else — including
  `file:` (a shell-opened local path can launch executables), `javascript:`,
  and unknown schemes — is rejected (no-op). Open via `ShellExecuteW(NULL,
  L"open", url, NULL, NULL, SW_SHOWNORMAL)`.

### 5. Testing

`tests/test_markdown.c` (pure logic, TDD) asserts decoration output per feature:

- code block: `` ```\ncode\n``` `` → `MD_FMT_CODEBLOCK` over `code`, fence lines
  hidden.
- blockquote: `> quoted` → `MD_FMT_QUOTE` + `PARA_QUOTE`, `> ` hidden.
- link: `[t](http://x)` → `DECO_FMT` `MD_FMT_LINK` over `t`, a `DECO_LINK` whose
  `aux` span is `http://x`, and the `[` + `](http://x)` hidden.
- task list: `- [ ] a` / `- [x] b` → bullet `DECO_PARA`, `- ` hidden, `[ ]`/`[x]`
  styled; checked item carries the strike fmt.
- reveal: a selection inside a code block / quote / link paragraph reveals its
  markers while other paragraphs stay hidden.

Rendering (backcolor, indent, link color) and Ctrl+click activation are GUI —
verified by the human (scripted GUI focus unavailable in this environment).
`make test` stays the logic gate; `make` clean is the build gate.

## Scope boundaries

- **In B:** fenced code blocks, blockquotes, links (render + Ctrl+click open,
  scheme-limited), task-list rendering; the `DECO_LINK` data-model + link table
  + `EN_LINK` handler.
- **Deferred:** clickable task toggling (needs text mutation → C); tables
  (non-decoration mechanism); nested lists (A's limitation); colored blockquote
  left-bar (not feasible in the child RichEdit).

## Risks

- **Link syntax hiding** must exactly cover `[` and `](url)` including the real
  source url length (which may differ from the md4c-decoded href for
  percent-encoding/entities) — derive by scanning source, not by href length.
- **Code block fence lines**: hiding whole lines interacts with cursor-reveal
  per paragraph; confirm a caret on a fence line reveals it and the block body
  stays styled.
- **`CFE_LINK` + custom color**: RichEdit may impose its own link color; verify
  `crTextColor` still applies, else accept RichEdit's link styling.
- **Link table bound** (512 chars/url, fixed count): overly long urls truncate;
  acceptable, but size the table generously and clamp safely.
