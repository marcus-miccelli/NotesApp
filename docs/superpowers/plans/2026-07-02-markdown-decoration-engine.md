# Markdown Decoration Engine (sub-project A) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the ad-hoc live-preview styling with one testable module that turns `(text, selection)` into a flat decoration list (CodeMirror pattern), applied to RichEdit paragraph-scoped, adding cursor-aware marker reveal, exact md4c-derived markers, native list rendering, and uncapped dynamic storage.

**Architecture:** A grown `src/markdown.c` builds a dynamic `Deco[]` (typed range ops: `DECO_FMT` / `DECO_HIDE` / `DECO_PARA`) directly from md4c callbacks, deriving marker positions from span structure. `src/note_window.c` becomes a dumb executor: reset base formatting over a range, then apply the decorations intersecting it. Whole-document apply first (Task 6); paragraph-scoped apply + cursor reveal on top (Task 7).

**Tech Stack:** C11, MSYS2 mingw-w64 gcc, md4c (existing, `third_party/md4c`), Win32 RichEdit (MSFTEDIT).

## Global Constraints

- Language/toolchain: pure C, `-std=c11`, `-mwindows`, gcc 13.2 (MSYS2 mingw-w64). No C++.
- Parser stays **md4c**; editor stays **RichEdit**. No new dependencies.
- All markdown knowledge lives in `src/markdown.c` (the decoration builder). `note_window.c` holds only RichEdit mechanics — no markdown parsing/scanning.
- Decoration offsets are RichEdit character offsets: the text is read via `EM_GETTEXTEX` with `GT_DEFAULT` + `CP_ACP` (single-char `\r` paragraph separators), matching RichEdit's internal indexing. The builder must be fed exactly that buffer.
- Marker lengths are **derived deterministically** from md4c span structure, never by scanning arbitrary adjacent punctuation: bold `**`=2, em `*`/`_`=1, strike `~~`=2, code = the contiguous backtick run at the content boundary, heading = leading `#… ` from line start to content.
- Dynamic storage — no fixed span/deco cap.
- Existing char sizes (twips) are unchanged from today: body/base `200`, H1 `360`, H2 `300`, H3 `260`. (Whether these should scale with DPI is a **separate, already-tracked** decision — out of scope here.)
- Palette `COLORREF`s (`COL_TEXT`, `COL_BG`) reused as-is.
- Testing: the builder is pure logic — real TDD via `tests/test_markdown.c` (`CHECK`/`CHECK_STR` from `tests/test.h`), gated by `make test`. RichEdit application/reveal is GUI: gate is `make` clean + `make test` green; visual confirmation is deferred to the human (scripted GUI focus is unavailable in this environment).

---

### Task 1: Decoration types + builder skeleton + headings

**Files:**
- Modify: `src/markdown.h`
- Modify: `src/markdown.c`
- Test: `tests/test_markdown.c`

**Interfaces:**
- Consumes: existing `MdFmt` flags + md4c (`third_party/md4c/md4c.h`).
- Produces:
  - `typedef enum { DECO_FMT, DECO_HIDE, DECO_PARA } DecoKind;`
  - `typedef enum { PARA_NONE, PARA_BULLET, PARA_NUMBER } ParaKind;`
  - `typedef struct { DecoKind kind; size_t start; size_t len; MdFmt fmt; ParaKind para; int number; } Deco;`
  - `size_t markdown_decorate(const char* text, size_t len, size_t sel_lo, size_t sel_hi, Deco** out);` — `*out` is `malloc`'d, caller `free`s; returns count. `sel_lo`/`sel_hi` are accepted now but unused until Task 4.
  - Internal `push()` helper + `DCtx` context (extended by later tasks).
- `markdown_spans` / `MdSpan` remain untouched (retired in Task 6).

- [ ] **Step 1: Add types + prototype to the header**

In `src/markdown.h`, after the `MdSpan` line (before `#endif`), add:

```c
typedef enum { DECO_FMT, DECO_HIDE, DECO_PARA } DecoKind;
typedef enum { PARA_NONE, PARA_BULLET, PARA_NUMBER } ParaKind;

typedef struct {
    DecoKind kind;
    size_t   start;   /* RichEdit char offset */
    size_t   len;
    MdFmt    fmt;     /* DECO_FMT */
    ParaKind para;    /* DECO_PARA */
    int      number;  /* DECO_PARA + PARA_NUMBER: ordinal */
} Deco;

/* Build the full-document decoration list. Markers hide everywhere except the
 * paragraph(s) intersecting [sel_lo, sel_hi]; pass sel_lo > sel_hi to hide all.
 * *out is malloc'd (free() it). Returns the count. Grows dynamically. */
size_t markdown_decorate(const char* text, size_t len,
                         size_t sel_lo, size_t sel_hi, Deco** out);
```

- [ ] **Step 2: Write the failing tests (headings + plain)**

Append to `tests/test_markdown.c` inside `test_markdown(void)`:

```c
    /* --- markdown_decorate: headings --- */
    {
        const char* t = "# Hi";
        Deco* d = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        CHECK(n == 2);
        CHECK(d[0].kind == DECO_HIDE && d[0].start == 0 && d[0].len == 2);  /* "# " */
        CHECK(d[1].kind == DECO_FMT && d[1].start == 2 && d[1].len == 2 &&
              d[1].fmt == MD_FMT_H1);
        free(d);
    }
    {
        const char* t = "plain words";
        Deco* d = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        CHECK(n == 0);
        free(d);
    }
```

- [ ] **Step 3: Run tests — verify they fail to compile/link**

Run: `make test`
Expected: FAIL — `markdown_decorate` / `Deco` undefined (link or compile error).

- [ ] **Step 4: Implement the builder core + headings**

In `src/markdown.c`, add `#include <stdlib.h>` at the top (after the existing includes), then append this block at the end of the file:

```c
/* ---- decoration builder (markdown_decorate) ---- */

typedef struct {
    const char* base;
    size_t len;
    Deco*  arr;
    size_t count, cap;
    /* heading state */
    int    in_heading;
    int    h_first_text;
    MdFmt  h_fmt;
} DCtx;

static void push(DCtx* c, DecoKind k, size_t start, size_t len,
                 MdFmt fmt, ParaKind para, int number) {
    if (len == 0 && k != DECO_PARA) return;
    if (c->count == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 32;
        Deco* na = realloc(c->arr, nc * sizeof(Deco));
        if (!na) return;               /* drop on OOM; render stays safe */
        c->arr = na; c->cap = nc;
    }
    Deco* d = &c->arr[c->count++];
    d->kind = k; d->start = start; d->len = len;
    d->fmt = fmt; d->para = para; d->number = number;
}

static MdFmt dh_fmt(unsigned level) {
    return level == 1 ? MD_FMT_H1 : level == 2 ? MD_FMT_H2 : MD_FMT_H3;
}

static int d_enter_block(MD_BLOCKTYPE t, void* detail, void* ud) {
    DCtx* c = (DCtx*)ud;
    if (t == MD_BLOCK_H) {
        MD_BLOCK_H_DETAIL* d = (MD_BLOCK_H_DETAIL*)detail;
        c->in_heading = 1; c->h_first_text = 0; c->h_fmt = dh_fmt(d->level);
    }
    return 0;
}
static int d_leave_block(MD_BLOCKTYPE t, void* detail, void* ud) {
    (void)detail; DCtx* c = (DCtx*)ud;
    if (t == MD_BLOCK_H) c->in_heading = 0;
    return 0;
}
static int d_text(MD_TEXTTYPE tt, const MD_CHAR* text, MD_SIZE size, void* ud) {
    (void)tt; DCtx* c = (DCtx*)ud;
    size_t off = (size_t)(text - c->base);
    MdFmt f = 0;
    if (c->in_heading) {
        if (!c->h_first_text) {
            c->h_first_text = 1;
            size_t ls = off;
            while (ls > 0 && c->base[ls-1] != '\n' && c->base[ls-1] != '\r') ls--;
            push(c, DECO_HIDE, ls, off - ls, 0, PARA_NONE, 0);   /* "# " */
        }
        f |= c->h_fmt;
    }
    if (f) push(c, DECO_FMT, off, (size_t)size, f, PARA_NONE, 0);
    return 0;
}

size_t markdown_decorate(const char* text, size_t len,
                         size_t sel_lo, size_t sel_hi, Deco** out) {
    (void)sel_lo; (void)sel_hi;   /* reveal wired in a later task */
    DCtx c; memset(&c, 0, sizeof c);
    c.base = text; c.len = len;
    MD_PARSER p; memset(&p, 0, sizeof p);
    p.abi_version = 0;
    p.flags = MD_DIALECT_COMMONMARK | MD_FLAG_STRIKETHROUGH;
    p.enter_block = d_enter_block;
    p.leave_block = d_leave_block;
    p.text        = d_text;
    md_parse(text, (MD_SIZE)len, &p, &c);
    *out = c.arr;
    return c.count;
}
```

- [ ] **Step 5: Run tests — verify pass**

Run: `make test`
Expected: PASS (all existing + the two new heading/plain checks; `0 failed`).

- [ ] **Step 6: Commit**

```bash
git add src/markdown.h src/markdown.c tests/test_markdown.c
git commit -m "feat(md): decoration builder core + heading decorations"
```

---

### Task 2: Inline decorations (bold / italic / strike / code) with exact markers

**Files:**
- Modify: `src/markdown.c`
- Test: `tests/test_markdown.c`

**Interfaces:**
- Consumes: Task 1's `DCtx`, `push`, `d_text`, `markdown_decorate`.
- Produces: `markdown_decorate` now also emits `DECO_FMT` for inline spans and `DECO_HIDE` for their delimiters, with correct nesting.

**Algorithm (marker derivation):** md4c does not report delimiter offsets, so derive them. Keep a stack of open inline spans. When a text run arrives at offset `O`, any spans opened since the last text get their **opening** markers by consuming delimiter chars leftward from `O`, innermost first (`cursor=O; marker=[cursor-marklen, cursor]; cursor-=marklen`). On `leave_span`, place the **closing** marker rightward from `max(last_text_end, close_cursor)`. `marklen` is fixed for emphasis/strike; for code it is the contiguous backtick run counted leftward from `cursor`.

- [ ] **Step 1: Write the failing tests**

Append inside `test_markdown(void)`:

```c
    /* --- markdown_decorate: inline --- */
    {   /* "**bold**": hide [0,2], fmt [2,4] BOLD, hide [6,2] */
        const char* t = "**bold**";
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        CHECK(n == 3);
        CHECK(d[0].kind == DECO_HIDE && d[0].start == 0 && d[0].len == 2);
        CHECK(d[1].kind == DECO_FMT  && d[1].start == 2 && d[1].len == 4 &&
              d[1].fmt == MD_FMT_BOLD);
        CHECK(d[2].kind == DECO_HIDE && d[2].start == 6 && d[2].len == 2);
        free(d);
    }
    {   /* "*i*": hide[0,1] fmt[1,1] hide[2,1] */
        const char* t = "*i*";
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        CHECK(n == 3);
        CHECK(d[1].kind == DECO_FMT && d[1].fmt == MD_FMT_ITALIC &&
              d[1].start == 1 && d[1].len == 1);
        free(d);
    }
    {   /* "~~x~~": strike, markers len 2 */
        const char* t = "~~x~~";
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        CHECK(n == 3);
        CHECK(d[0].kind == DECO_HIDE && d[0].len == 2);
        CHECK(d[1].fmt == MD_FMT_STRIKE && d[1].start == 2 && d[1].len == 1);
        CHECK(d[2].kind == DECO_HIDE && d[2].start == 3 && d[2].len == 2);
        free(d);
    }
    {   /* "`c`": code, backtick markers len 1 */
        const char* t = "`c`";
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        CHECK(n == 3);
        CHECK(d[0].kind == DECO_HIDE && d[0].start == 0 && d[0].len == 1);
        CHECK(d[1].fmt == MD_FMT_CODE && d[1].start == 1 && d[1].len == 1);
        CHECK(d[2].kind == DECO_HIDE && d[2].start == 2 && d[2].len == 1);
        free(d);
    }
    {   /* nested "**a *b* c**": a BOLD run, inner ITALIC over b (bold+italic) */
        const char* t = "**a *b* c**";
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        /* opening ** hidden at 0..2; inner * markers around b hidden; the "b"
         * run carries BOLD|ITALIC; closing ** hidden at end. */
        int saw_bi = 0;
        for (size_t i = 0; i < n; i++)
            if (d[i].kind == DECO_FMT &&
                d[i].fmt == (MD_FMT_BOLD | MD_FMT_ITALIC)) saw_bi = 1;
        CHECK(saw_bi == 1);
        CHECK(d[0].kind == DECO_HIDE && d[0].start == 0 && d[0].len == 2);
        free(d);
    }
```

- [ ] **Step 2: Run tests — verify they fail**

Run: `make test`
Expected: FAIL — e.g. `"**bold**"` currently yields `n == 0` (no inline handling yet).

- [ ] **Step 3: Implement inline spans**

In `src/markdown.c`, extend `DCtx` (add fields to the struct) so it reads:

```c
typedef struct {
    const char* base;
    size_t len;
    Deco*  arr;
    size_t count, cap;
    /* heading state */
    int    in_heading;
    int    h_first_text;
    MdFmt  h_fmt;
    /* inline span stack */
    struct { MdFmt fmt; int marklen; int start_set; } sp[64];
    int    depth;
    size_t last_text_end;
    size_t close_cursor;
} DCtx;
```

Add these helpers above `d_enter_block`:

```c
static MdFmt span_fmt(MD_SPANTYPE t) {
    if (t == MD_SPAN_STRONG) return MD_FMT_BOLD;
    if (t == MD_SPAN_EM)     return MD_FMT_ITALIC;
    if (t == MD_SPAN_DEL)    return MD_FMT_STRIKE;
    if (t == MD_SPAN_CODE)   return MD_FMT_CODE;
    return 0;
}
static int span_marklen(MD_SPANTYPE t) {
    if (t == MD_SPAN_STRONG || t == MD_SPAN_DEL) return 2;
    if (t == MD_SPAN_EM)                          return 1;
    return -1;   /* code: computed lazily from the backtick run */
}
static int d_enter_span(MD_SPANTYPE t, void* detail, void* ud) {
    (void)detail; DCtx* c = (DCtx*)ud;
    if (!span_fmt(t) || c->depth >= 64) return 0;
    c->sp[c->depth].fmt = span_fmt(t);
    c->sp[c->depth].marklen = span_marklen(t);
    c->sp[c->depth].start_set = 0;
    c->depth++;
    return 0;
}
static int d_leave_span(MD_SPANTYPE t, void* detail, void* ud) {
    (void)detail; DCtx* c = (DCtx*)ud;
    if (!span_fmt(t) || c->depth <= 0) return 0;
    c->depth--;
    int ml = c->sp[c->depth].marklen; if (ml < 0) ml = 0;
    size_t base = c->last_text_end > c->close_cursor ? c->last_text_end
                                                     : c->close_cursor;
    if (ml > 0) push(c, DECO_HIDE, base, (size_t)ml, 0, PARA_NONE, 0);
    c->close_cursor = base + (size_t)ml;
    return 0;
}
```

Replace `d_text` with the version that resolves opening markers and combines inline formats:

```c
static int d_text(MD_TEXTTYPE tt, const MD_CHAR* text, MD_SIZE size, void* ud) {
    (void)tt; DCtx* c = (DCtx*)ud;
    size_t off = (size_t)(text - c->base);

    /* resolve opening markers for spans opened since the last text run,
     * innermost first, consuming delimiter chars leftward from off. */
    size_t cursor = off;
    for (int i = c->depth - 1; i >= 0 && !c->sp[i].start_set; i--) {
        int ml = c->sp[i].marklen;
        if (ml < 0) {                         /* code: count backticks left */
            size_t j = cursor; int k = 0;
            while (j > 0 && c->base[j-1] == '`') { j--; k++; }
            ml = k; c->sp[i].marklen = ml;
        }
        if ((size_t)ml <= cursor)
            push(c, DECO_HIDE, cursor - (size_t)ml, (size_t)ml, 0, PARA_NONE, 0);
        cursor -= (size_t)ml;
        c->sp[i].start_set = 1;
    }

    MdFmt f = 0;
    for (int i = 0; i < c->depth; i++) f |= c->sp[i].fmt;
    if (c->in_heading) {
        if (!c->h_first_text) {
            c->h_first_text = 1;
            size_t ls = off;
            while (ls > 0 && c->base[ls-1] != '\n' && c->base[ls-1] != '\r') ls--;
            push(c, DECO_HIDE, ls, off - ls, 0, PARA_NONE, 0);
        }
        f |= c->h_fmt;
    }
    if (f) push(c, DECO_FMT, off, (size_t)size, f, PARA_NONE, 0);
    c->last_text_end = off + (size_t)size;
    return 0;
}
```

Wire the span callbacks into the parser in `markdown_decorate` (add two lines after `p.text = d_text;`):

```c
    p.enter_span = d_enter_span;
    p.leave_span = d_leave_span;
```

- [ ] **Step 4: Run tests — verify pass**

Run: `make test`
Expected: PASS, `0 failed` (inline + nested cases green).

- [ ] **Step 5: Commit**

```bash
git add src/markdown.c tests/test_markdown.c
git commit -m "feat(md): inline decorations (bold/italic/strike/code) with exact markers"
```

---

### Task 3: List decorations (native md4c lists)

**Files:**
- Modify: `src/markdown.c`
- Test: `tests/test_markdown.c`

**Interfaces:**
- Consumes: Task 1–2 `DCtx`, `push`, block callbacks.
- Produces: `markdown_decorate` emits `DECO_PARA` (PARA_BULLET / PARA_NUMBER + ordinal) per list item and `DECO_HIDE` for the literal `- ` / `N. ` marker. Single-level lists (nested lists deferred — see risks).

- [ ] **Step 1: Write the failing tests**

Append inside `test_markdown(void)`:

```c
    /* --- markdown_decorate: lists --- */
    {   /* bullet list: "- a\n- b" */
        const char* t = "- a\n- b";
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        int bullets = 0, hides = 0;
        for (size_t i = 0; i < n; i++) {
            if (d[i].kind == DECO_PARA && d[i].para == PARA_BULLET) bullets++;
            if (d[i].kind == DECO_HIDE && d[i].len == 2) hides++;  /* "- " */
        }
        CHECK(bullets == 2);
        CHECK(hides >= 2);
        free(d);
    }
    {   /* numbered list: "1. a\n2. b" -> ordinals 1 and 2 */
        const char* t = "1. a\n2. b";
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        int n1 = 0, n2 = 0;
        for (size_t i = 0; i < n; i++)
            if (d[i].kind == DECO_PARA && d[i].para == PARA_NUMBER) {
                if (d[i].number == 1) n1 = 1;
                if (d[i].number == 2) n2 = 1;
            }
        CHECK(n1 == 1 && n2 == 1);
        free(d);
    }
```

- [ ] **Step 2: Run tests — verify they fail**

Run: `make test`
Expected: FAIL — no `DECO_PARA` emitted yet (`bullets == 0`).

- [ ] **Step 3: Implement lists**

Extend `DCtx` with list state (add to the struct):

```c
    /* list state (single level) */
    ParaKind listkind;
    int      listnum;      /* next ordinal for ordered lists */
    int      in_li;
    int      li_first_text;
```

In `d_enter_block`, handle the list block types (add before `return 0;`):

```c
    if (t == MD_BLOCK_UL) { c->listkind = PARA_BULLET; }
    if (t == MD_BLOCK_OL) {
        MD_BLOCK_OL_DETAIL* d = (MD_BLOCK_OL_DETAIL*)detail;
        c->listkind = PARA_NUMBER; c->listnum = (int)d->start;
    }
    if (t == MD_BLOCK_LI) { c->in_li = 1; c->li_first_text = 0; }
```

In `d_leave_block`, close list state (add before `return 0;`):

```c
    if (t == MD_BLOCK_LI) {
        c->in_li = 0;
        if (c->listkind == PARA_NUMBER) c->listnum++;
    }
    if (t == MD_BLOCK_UL || t == MD_BLOCK_OL) c->listkind = PARA_NONE;
```

In `d_text`, emit the list-item paragraph + hide its marker on the item's first
text (insert just before the `MdFmt f = 0;` line):

```c
    if (c->in_li && !c->li_first_text) {
        c->li_first_text = 1;
        size_t ls = off;
        while (ls > 0 && c->base[ls-1] != '\n' && c->base[ls-1] != '\r') ls--;
        push(c, DECO_HIDE, ls, off - ls, 0, PARA_NONE, 0);           /* "- "/"N. " */
        push(c, DECO_PARA, ls, (off - ls) + (size_t)size,
             0, c->listkind,
             c->listkind == PARA_NUMBER ? c->listnum : 0);
    }
```

- [ ] **Step 4: Run tests — verify pass**

Run: `make test`
Expected: PASS, `0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/markdown.c tests/test_markdown.c
git commit -m "feat(md): native list decorations (bullet/number) via md4c"
```

---

### Task 4: Cursor-aware reveal

**Files:**
- Modify: `src/markdown.c`
- Test: `tests/test_markdown.c`

**Interfaces:**
- Consumes: Task 1–3 builder.
- Produces: `markdown_decorate` suppresses `DECO_HIDE` for any marker whose **source paragraph** intersects `[sel_lo, sel_hi]`. `DECO_FMT`/`DECO_PARA` are unaffected. Passing `sel_lo > sel_hi` hides everything (unchanged behavior).

- [ ] **Step 1: Write the failing tests**

Append inside `test_markdown(void)`:

```c
    /* --- markdown_decorate: cursor-aware reveal --- */
    {   /* two paragraphs; caret in the first reveals its markers only */
        const char* t = "**a**\n**b**";   /* para1 [0,5], \n at 5, para2 [6,11] */
        Deco* d = NULL;
        size_t n = markdown_decorate(t, strlen(t), 1, 1, &d);  /* caret in para1 */
        int hide_in_p1 = 0, hide_in_p2 = 0;
        for (size_t i = 0; i < n; i++) if (d[i].kind == DECO_HIDE) {
            if (d[i].start < 5) hide_in_p1 = 1;
            if (d[i].start > 5) hide_in_p2 = 1;
        }
        CHECK(hide_in_p1 == 0);   /* revealed */
        CHECK(hide_in_p2 == 1);   /* still hidden */
        free(d);
    }
    {   /* hide-all sentinel still hides both */
        const char* t = "**a**\n**b**";
        Deco* d = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        int hides = 0;
        for (size_t i = 0; i < n; i++) if (d[i].kind == DECO_HIDE) hides++;
        CHECK(hides == 4);
        free(d);
    }
```

- [ ] **Step 2: Run tests — verify they fail**

Run: `make test`
Expected: FAIL — reveal not implemented; `hide_in_p1` is 1 (markers still hidden in para1).

- [ ] **Step 3: Implement reveal**

Store the selection on `DCtx` and gate `DECO_HIDE` pushes by paragraph. Add to `DCtx`:

```c
    size_t sel_lo, sel_hi;   /* reveal window; lo>hi = hide all */
```

Set them in `markdown_decorate` (replace the `(void)sel_lo; (void)sel_hi;` line):

```c
    /* (reveal handled via c.sel_lo/c.sel_hi below) */
```

and after `c.base = text; c.len = len;` add:

```c
    c.sel_lo = sel_lo; c.sel_hi = sel_hi;
```

Add a paragraph-intersection helper above `push` (it needs `DCtx`, so place it
just after the `DCtx` typedef):

```c
/* Does the source paragraph containing [start, start+len) intersect the reveal
 * window [sel_lo, sel_hi]?  Paragraph = run between \n/\r boundaries. */
static int in_reveal(DCtx* c, size_t start, size_t len) {
    if (c->sel_lo > c->sel_hi) return 0;               /* hide-all sentinel */
    size_t ps = start;
    while (ps > 0 && c->base[ps-1] != '\n' && c->base[ps-1] != '\r') ps--;
    size_t pe = start + len;
    while (pe < c->len && c->base[pe] != '\n' && c->base[pe] != '\r') pe++;
    return c->sel_lo <= pe && c->sel_hi >= ps;         /* ranges overlap */
}
```

Route every `DECO_HIDE` through reveal by adding a guarded helper and using it
for hides. Add just below `push`:

```c
static void push_hide(DCtx* c, size_t start, size_t len) {
    if (len == 0) return;
    if (in_reveal(c, start, len)) return;   /* caret paragraph: show markers */
    push(c, DECO_HIDE, start, len, 0, PARA_NONE, 0);
}
```

Then replace every `push(c, DECO_HIDE, ...)` call in `d_text`, `d_leave_span`
(there are: heading `# `, list marker, inline opening, inline closing) with the
equivalent `push_hide(c, start, len)`. For example the inline opening becomes
`push_hide(c, cursor - (size_t)ml, (size_t)ml);` and the closing becomes
`push_hide(c, base, (size_t)ml);`.

- [ ] **Step 4: Run tests — verify pass**

Run: `make test`
Expected: PASS, `0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/markdown.c tests/test_markdown.c
git commit -m "feat(md): cursor-aware marker reveal by paragraph"
```

---

### Task 5: Caret format query (`markdown_fmt_at`)

**Files:**
- Modify: `src/markdown.h`, `src/markdown.c`
- Test: `tests/test_markdown.c`

**Interfaces:**
- Produces: `MdFmt markdown_fmt_at(const char* text, size_t len, size_t caret);` — the OR of inline formats active at `caret` (for the sidebar toggle highlights). Headings return 0 (headings are non-formattable, matching current behavior).

- [ ] **Step 1: Add the prototype + failing tests**

In `src/markdown.h`, after the `markdown_decorate` prototype:

```c
/* Inline format flags active at a caret offset (for sidebar toggles). */
MdFmt markdown_fmt_at(const char* text, size_t len, size_t caret);
```

Append inside `test_markdown(void)`:

```c
    /* --- markdown_fmt_at --- */
    {
        const char* t = "**bold**";
        CHECK(markdown_fmt_at(t, strlen(t), 4) == MD_FMT_BOLD);   /* inside */
        CHECK(markdown_fmt_at(t, strlen(t), 0) == 0);             /* on marker */
    }
    {
        const char* t = "a *i* b";
        CHECK(markdown_fmt_at(t, strlen(t), 3) == MD_FMT_ITALIC); /* inside i */
        CHECK(markdown_fmt_at(t, strlen(t), 0) == 0);             /* plain */
    }
```

- [ ] **Step 2: Run tests — verify they fail**

Run: `make test`
Expected: FAIL — `markdown_fmt_at` undefined.

- [ ] **Step 3: Implement via the decoration list**

The format at the caret is the `DECO_FMT` covering it (excluding heading-only
formats). Append to `src/markdown.c`:

```c
MdFmt markdown_fmt_at(const char* text, size_t len, size_t caret) {
    Deco* d = NULL;
    size_t n = markdown_decorate(text, len, (size_t)-1, 0, &d);
    MdFmt f = 0;
    for (size_t i = 0; i < n; i++) {
        if (d[i].kind != DECO_FMT) continue;
        size_t a = d[i].start, b = a + d[i].len;
        /* cover the char on either side of the caret so a boundary still lights */
        if ((caret > a && caret <= b) || (caret >= a && caret < b))
            f |= d[i].fmt;
    }
    free(d);
    f &= ~(MdFmt)(MD_FMT_H1 | MD_FMT_H2 | MD_FMT_H3);   /* headings not toggles */
    return f;
}
```

- [ ] **Step 4: Run tests — verify pass**

Run: `make test`
Expected: PASS, `0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/markdown.h src/markdown.c tests/test_markdown.c
git commit -m "feat(md): markdown_fmt_at caret format query"
```

---

### Task 6: Integrate the engine into note_window (whole-document apply)

**Files:**
- Modify: `src/note_window.c`
- Modify: `tests/test_markdown.c` (remove retired `markdown_spans` tests)
- Modify: `src/markdown.h`, `src/markdown.c` (remove retired `markdown_spans` / `MdSpan`)

**Interfaces:**
- Consumes: `markdown_decorate`, `markdown_fmt_at`.
- Produces: `nw_apply_decos(HWND edit, const char* text, int len, Deco* d, size_t n, size_t lo, size_t hi)` — reset base char format over `[lo,hi]`, then apply every decoration intersecting `[lo,hi]`. `nw_apply_format_edit` calls it whole-document (`lo=0, hi=len`, hide-all). `nw_update_toggles` uses `markdown_fmt_at`. The heuristic helpers `nw_apply_hidden`, `nw_apply_lists`, `nw_set_range_fmt`, `nw_marker_len` are removed.

- [ ] **Step 1: Add the decoration applier**

In `src/note_window.c`, replace the body of `nw_apply_format_edit` and its
helpers with a decoration-driven applier. First add the applier (place it just
above `nw_apply_format_edit`):

```c
/* Map an MdFmt to a CHARFORMAT2W and apply it to [start, start+len). */
static void nw_fmt_range(HWND edit, size_t start, size_t len, MdFmt fmt) {
    CHARRANGE r = { (LONG)start, (LONG)(start + len) };
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&r);
    CHARFORMAT2W cf; memset(&cf, 0, sizeof cf); cf.cbSize = sizeof cf;
    cf.dwMask = CFM_BOLD|CFM_ITALIC|CFM_STRIKEOUT|CFM_SIZE|CFM_FACE|CFM_COLOR|CFM_HIDDEN;
    cf.crTextColor = COL_TEXT;
    cf.yHeight = 200;
    if (fmt & MD_FMT_H1) cf.yHeight = 360;
    else if (fmt & MD_FMT_H2) cf.yHeight = 300;
    else if (fmt & MD_FMT_H3) cf.yHeight = 260;
    if (fmt & (MD_FMT_BOLD | MD_FMT_H1 | MD_FMT_H2 | MD_FMT_H3)) cf.dwEffects |= CFE_BOLD;
    if (fmt & MD_FMT_ITALIC) cf.dwEffects |= CFE_ITALIC;
    if (fmt & MD_FMT_STRIKE) cf.dwEffects |= CFE_STRIKEOUT;
    wcscpy(cf.szFaceName, L"IBM Plex Mono");
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

static void nw_hide_range2(HWND edit, size_t start, size_t len) {
    CHARRANGE r = { (LONG)start, (LONG)(start + len) };
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&r);
    CHARFORMAT2W cf; memset(&cf, 0, sizeof cf); cf.cbSize = sizeof cf;
    cf.dwMask = CFM_HIDDEN; cf.dwEffects = CFE_HIDDEN;
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

static void nw_para_range(HWND edit, size_t start, size_t len, ParaKind kind) {
    CHARRANGE r = { (LONG)start, (LONG)(start + len) };
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&r);
    PARAFORMAT2 pf; memset(&pf, 0, sizeof pf); pf.cbSize = sizeof pf;
    pf.dwMask = PFM_NUMBERING | PFM_NUMBERINGSTYLE | PFM_NUMBERINGTAB
              | PFM_OFFSET | PFM_STARTINDENT;
    if (kind == PARA_BULLET || kind == PARA_NUMBER) {
        pf.wNumbering = (kind == PARA_NUMBER) ? PFN_ARABIC : PFN_BULLET;
        pf.wNumberingStyle = (kind == PARA_NUMBER) ? PFNS_PERIOD : PFNS_PLAIN;
        pf.wNumberingTab = 280; pf.dxStartIndent = 280; pf.dxOffset = 280;
    }
    SendMessageW(edit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
}

/* Reset [lo,hi] to base formatting, then apply every decoration intersecting it. */
static void nw_apply_decos(HWND edit, int len, Deco* d, size_t n, size_t lo, size_t hi) {
    if (hi > (size_t)len) hi = (size_t)len;
    CHARRANGE base = { (LONG)lo, (LONG)hi };
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&base);
    CHARFORMAT2W bf; memset(&bf, 0, sizeof bf); bf.cbSize = sizeof bf;
    bf.dwMask = CFM_BOLD|CFM_ITALIC|CFM_STRIKEOUT|CFM_SIZE|CFM_FACE|CFM_COLOR|CFM_HIDDEN;
    bf.yHeight = 200; bf.crTextColor = COL_TEXT; wcscpy(bf.szFaceName, L"IBM Plex Mono");
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&bf);
    /* also clear paragraph numbering across the range, so un-listing reverts */
    nw_para_range(edit, lo, hi > lo ? hi - lo : 0, PARA_NONE);

    for (size_t i = 0; i < n; i++) {
        size_t a = d[i].start, b = a + d[i].len;
        if (b <= lo || a >= hi) continue;                 /* outside range */
        if (d[i].kind == DECO_FMT)  nw_fmt_range(edit, a, d[i].len, d[i].fmt);
        else if (d[i].kind == DECO_HIDE) nw_hide_range2(edit, a, d[i].len);
        else if (d[i].kind == DECO_PARA) nw_para_range(edit, a, d[i].len, d[i].para);
    }
}
```

- [ ] **Step 2: Rewrite `nw_apply_format_edit`**

Replace the entire body of `nw_apply_format_edit` with:

```c
static void nw_apply_format_edit(NoteWin* nw, int i) {
    HWND edit = nw->tab[i].edit;
    SendMessageW(edit, EM_SETEVENTMASK, 0, 0);            /* mute reentrancy */

    GETTEXTLENGTHEX gtl; gtl.flags = GTL_DEFAULT; gtl.codepage = CP_ACP;
    int len = (int)SendMessageW(edit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
    if (len <= 0) { SendMessageW(edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK); return; }
    char* buf = malloc((size_t)len + 1);
    if (!buf) { SendMessageW(edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK); return; }
    GETTEXTEX gte; gte.cb = (DWORD)(len + 1); gte.flags = GT_DEFAULT;
    gte.codepage = CP_ACP; gte.lpDefaultChar = NULL; gte.lpUsedDefChar = NULL;
    SendMessageW(edit, EM_GETTEXTEX, (WPARAM)&gte, (LPARAM)buf);

    CHARRANGE saved; SendMessageW(edit, EM_EXGETSEL, 0, (LPARAM)&saved);

    Deco* d = NULL;
    size_t n = markdown_decorate(buf, (size_t)len, (size_t)-1, 0, &d);  /* hide all */
    nw_apply_decos(edit, len, d, n, 0, (size_t)len);
    free(d);
    free(buf);

    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&saved);   /* restore caret */
    SendMessageW(edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
}
```

- [ ] **Step 3: Delete the retired helpers and switch toggles**

Delete these functions from `src/note_window.c` entirely: `nw_apply_hidden`,
`nw_apply_lists`, `nw_set_range_fmt`, `nw_hide_range`, `nw_marker_len`. (Search
each name; remove its definition. `nw_marker_len` is still used by the editing
shortcuts `nw_handle_enter`/`nw_list_prefix` — keep `nw_marker_len` if those
call it; only remove helpers with no remaining callers. Verify with grep in
Step 5.)

In `nw_update_toggles`, replace the `markdown_spans` block that computes inline
state with `markdown_fmt_at`. Find the section that declares `MdSpan spans[256];`
and loops; replace from the `int len; char* buf = nw_body_text(e, &len);` down to
its `free(buf);` with:

```c
    CHARRANGE sel; SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
    int len; char* buf = nw_body_text(e, &len);
    if (buf) {
        MdFmt f = markdown_fmt_at(buf, (size_t)len, (size_t)sel.cpMin);
        s[0] = (f & MD_FMT_BOLD)   != 0;
        s[1] = (f & MD_FMT_ITALIC) != 0;
        s[2] = (f & MD_FMT_STRIKE) != 0;
        free(buf);
    }
```

(The `PARAFORMAT2`/list-toggle portion of `nw_update_toggles` — `s[3]`, `s[4]`
via `EM_GETPARAFORMAT` — stays unchanged.)

- [ ] **Step 4: Retire `markdown_spans` / `MdSpan`**

Remove the `MdSpan` typedef and `markdown_spans` prototype from
`src/markdown.h`, remove the old `markdown_spans` implementation and its `Ctx`
+ callbacks from `src/markdown.c`, and delete the five `markdown_spans` test
blocks from `tests/test_markdown.c` (the original bold/H1/code/strike/plain
cases at the top of `test_markdown`). The new `markdown_decorate` tests cover
the same ground.

- [ ] **Step 5: Build, verify no dangling references, test**

Run: `grep -n "markdown_spans\|MdSpan\|nw_apply_hidden\|nw_apply_lists\|nw_set_range_fmt\|nw_hide_range\b" src/note_window.c src/markdown.c src/markdown.h tests/test_markdown.c`
Expected: no matches (except `nw_marker_len` if still used by editing shortcuts).

Run: `make clean && make`
Expected: links clean, no new warnings.

Run: `make test`
Expected: PASS, `0 failed`.

GUI (deferred to human): launch fresh, confirm bold/italic/code/strike/headings/lists render as before, markers hidden, large notes fully formatted (past the old 256 cap). Note visual confirmation deferred.

- [ ] **Step 6: Commit**

```bash
git add src/note_window.c src/markdown.c src/markdown.h tests/test_markdown.c
git commit -m "feat(md): drive live-preview from decoration engine; retire heuristics"
```

---

### Task 7: Paragraph-scoped apply + cursor-aware reveal on caret move

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: `nw_apply_decos`, `markdown_decorate`.
- Produces: `nw_restyle(NoteWin* nw, int tab, int scoped)` — decorate with the current selection (reveal), applying whole-document or scoped to the caret paragraph(s). Wired into the debounce tick and `EN_SELCHANGE` so markers reveal in the caret's paragraph and re-hide when it leaves.

- [ ] **Step 1: Add paragraph helpers + `nw_restyle`**

Add above `nw_apply_format_edit` in `src/note_window.c`:

```c
/* Expand [lo,hi] to full source-paragraph boundaries in buf. */
static void nw_para_bounds(const char* buf, int len, size_t* lo, size_t* hi) {
    size_t a = *lo, b = *hi;
    while (a > 0 && buf[a-1] != '\r' && buf[a-1] != '\n') a--;
    while (b < (size_t)len && buf[b] != '\r' && buf[b] != '\n') b++;
    *lo = a; *hi = b;
}

/* Restyle a tab body. scoped=1 limits the reset+apply to the paragraph(s) the
 * selection touches (fast path for edits/caret moves); scoped=0 does the whole
 * document (load). Markers reveal in the selection's paragraph(s). */
static void nw_restyle(NoteWin* nw, int tab, int scoped) {
    HWND edit = nw->tab[tab].edit;
    SendMessageW(edit, EM_SETEVENTMASK, 0, 0);

    GETTEXTLENGTHEX gtl; gtl.flags = GTL_DEFAULT; gtl.codepage = CP_ACP;
    int len = (int)SendMessageW(edit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
    if (len < 0) len = 0;
    char* buf = malloc((size_t)len + 1);
    if (!buf) { SendMessageW(edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK); return; }
    if (len > 0) {
        GETTEXTEX gte; gte.cb = (DWORD)(len + 1); gte.flags = GT_DEFAULT;
        gte.codepage = CP_ACP; gte.lpDefaultChar = NULL; gte.lpUsedDefChar = NULL;
        SendMessageW(edit, EM_GETTEXTEX, (WPARAM)&gte, (LPARAM)buf);
    } else buf[0] = '\0';

    CHARRANGE saved; SendMessageW(edit, EM_EXGETSEL, 0, (LPARAM)&saved);
    size_t sel_lo = (size_t)saved.cpMin, sel_hi = (size_t)saved.cpMax;

    /* apply window: whole doc, unless scoped to a single-paragraph selection */
    size_t lo = 0, hi = (size_t)len;
    int single_para = 0;
    if (scoped) {
        size_t plo = sel_lo, phi = sel_hi;
        nw_para_bounds(buf, len, &plo, &phi);
        /* only scope when the selection stays within one paragraph */
        size_t clo = sel_hi, chi = sel_hi; nw_para_bounds(buf, len, &clo, &chi);
        if (plo == clo && phi == chi) { lo = plo; hi = phi; single_para = 1; }
    }
    (void)single_para;

    Deco* d = NULL;
    size_t n = markdown_decorate(buf, (size_t)len, sel_lo, sel_hi, &d);
    nw_apply_decos(edit, len, d, n, lo, hi);
    free(d);
    free(buf);

    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&saved);
    SendMessageW(edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
}
```

- [ ] **Step 2: Route load / edit / caret through `nw_restyle`**

`nw_apply_format_edit` (from Task 6) is the whole-document path — make it a thin
wrapper so existing callers (load, `nw_reformat_now`, debounce) keep working:

```c
static void nw_apply_format_edit(NoteWin* nw, int i) {
    nw_restyle(nw, i, 0);   /* whole-document (load / programmatic reformat) */
}
```

Change the **debounce tick** to scoped. In `WM_TIMER` (the `IDT_DEBOUNCE`
branch) replace `nw_apply_format(nw);` with:

```c
            nw_restyle(nw, nw->active, 1);   /* scoped to the edited paragraph */
```

Add caret-move reveal. In the `WM_NOTIFY` `EN_SELCHANGE` branch (which currently
calls `nw_update_toggles(nw);`), also re-decorate so markers reveal/hide as the
caret crosses paragraphs. Track the last active paragraph on `NoteWin` to avoid
needless restyles. Add to `struct NoteWin`:

```c
    size_t last_caret_para;   /* start offset of the paragraph last revealed */
```

Then in the `EN_SELCHANGE` handler, before `nw_update_toggles(nw);`:

```c
        {
            HWND e = nw_edit(nw);
            CHARRANGE sel; SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
            int len; char* b = nw_body_text(e, &len);
            if (b) {
                size_t lo = (size_t)sel.cpMin, hi = lo;
                nw_para_bounds(b, len, &lo, &hi);
                free(b);
                if (lo != nw->last_caret_para) {
                    nw->last_caret_para = lo;
                    SendMessageW(e, WM_SETREDRAW, FALSE, 0);
                    nw_restyle(nw, nw->active, 0);   /* whole doc: flip reveal */
                    SendMessageW(e, WM_SETREDRAW, TRUE, 0);
                    InvalidateRect(e, NULL, TRUE);
                }
            }
        }
```

(Whole-document restyle on paragraph change is correct and simplest; the scoped
optimization for the two affected paragraphs specifically is a sub-project C
refinement. `nw_body_text` already exists.)

- [ ] **Step 3: Build and test**

Run: `make clean && make`
Expected: links clean, no new warnings.

Run: `make test`
Expected: PASS, `0 failed` (logic unchanged; this task is GUI wiring).

GUI (deferred to human): launch fresh; typing in a paragraph shows its raw
markers, moving the caret to another paragraph re-hides the first and reveals the
new one; editing a large note stays responsive. Note visual confirmation deferred.

- [ ] **Step 4: Commit**

```bash
git add src/note_window.c
git commit -m "feat(md): paragraph-scoped restyle + cursor-aware reveal on caret move"
```

---

## Self-Review Notes

**Spec coverage:**
- Decoration model (`DECO_FMT/HIDE/PARA`) + module owns markdown → Tasks 1–5; `note_window` dumb executor → Tasks 6–7.
- Exact markers from md4c structure → Task 2 (inline stack algorithm), Task 1 (headings), Task 3 (lists). Retires heuristic → Task 6.
- Native md4c lists, retire `nw_apply_lists`/`nw_marker_len` scanning → Task 3 + Task 6 (note: `nw_marker_len` kept only if the editing shortcuts still use it — that alignment is explicitly deferred to C in the spec).
- Dynamic storage, no 256 cap → Task 1 `push`/`realloc`; verified by large-input rendering (Task 6 GUI note).
- Cursor-aware reveal, paragraph granularity → Task 4 (builder) + Task 7 (wiring).
- Paragraph-scoped restyle with full-restyle fallback for multi-paragraph selections → Task 7 (`nw_restyle` scoped path only when selection stays in one paragraph).
- Reuse one parse path for toggles (`markdown_fmt_at`) → Task 5 + Task 6.
- Char sizes unchanged (200/360/300/260) → Task 6 `nw_fmt_range` (DPI-yHeight question left out of scope, per spec).
- Deferred B (new syntax) and C (cross-buffer incremental, shortcut alignment) — not in any task, matching the spec.

**Placeholder scan:** none — every code step carries complete code; every test step has real `CHECK` assertions with expected `make test` output.

**Type consistency:** `Deco`/`DecoKind`/`ParaKind` defined in Task 1, used unchanged in Tasks 2–7. `markdown_decorate(text,len,sel_lo,sel_hi,&out)` signature stable from Task 1. `nw_apply_decos(edit,len,d,n,lo,hi)` defined Task 6, reused Task 7. `nw_restyle(nw,tab,scoped)` defined Task 7; `nw_apply_format_edit` becomes its `scoped=0` wrapper so existing callers are unaffected. `markdown_fmt_at` signature stable Task 5→6.

**Known limitations (documented, not defects):** nested lists collapse to single-level (spec risk); CommonMark code-span single-space stripping may offset a backtick-marker edge (spec risk); `last_caret_para` restyle on paragraph change is whole-document (C optimizes).
