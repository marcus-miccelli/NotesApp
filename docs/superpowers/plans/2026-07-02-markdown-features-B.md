# Markdown Features B Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add fenced code blocks, blockquotes, links (Ctrl+click open), and task-list rendering to the decoration engine, expressed purely as style + hide of source characters.

**Architecture:** Extend `markdown_decorate` in `src/markdown.c` with new md4c block/span handlers emitting new decoration kinds/flags; extend the `note_window.c` executor (`nw_fmt_range`/`nw_para_range`/`nw_apply_decos`) to render them, plus a per-window link table and an `EN_LINK` Ctrl+click handler. All new markers route through the existing `push_hide`, so cursor-reveal works for free.

**Tech Stack:** C11, MSYS2 mingw-w64 gcc, md4c (existing), Win32 RichEdit (MSFTEDIT), `ShellExecuteW` (shell32, already linked).

## Global Constraints

- Pure C, `-std=c11`, `-mwindows`, gcc 13.2 (MSYS2 mingw-w64). No C++. No new dependencies.
- Parser stays md4c. All markdown knowledge lives in `src/markdown.c`; `note_window.c` is RichEdit mechanics only.
- Decorations style or hide existing source characters — never insert. Marker offsets are RichEdit char offsets (`\r`/CP_ACP space, fed from `EM_GETTEXTEX GT_DEFAULT+CP_ACP`).
- Marker positions derived deterministically from md4c structure + source scanning (not decoded-href lengths).
- Dynamic decoration storage (existing `push`/`realloc`); no fixed cap on decorations.
- New `MdFmt` bits extend the existing set: `MD_FMT_CODEBLOCK (1u<<7)`, `MD_FMT_QUOTE (1u<<8)`, `MD_FMT_LINK (1u<<9)`.
- Link security: open only `http://`, `https://`, `mailto:` URLs via `ShellExecuteW`; reject all other schemes (incl. `file:`, `javascript:`). Ctrl+click only.
- Code-block background shade: `COL_CODE_BG = RGB(0x18,0x18,0x1c)`.
- Char twips unchanged (200/360/300/260).
- Testing: builder is pure logic → real TDD via `tests/test_markdown.c` (`CHECK` from `tests/test.h`), gated by `make test`. RichEdit rendering + Ctrl+click are GUI — gate is `make` clean + `make test` green; visual confirmation deferred to the human (scripted GUI focus unavailable here).

---

### Task 1: Fenced code blocks

**Files:**
- Modify: `src/markdown.h`, `src/markdown.c`
- Modify: `src/note_window.c`
- Test: `tests/test_markdown.c`

**Interfaces:**
- Produces: `MD_FMT_CODEBLOCK` decorations; renders as monospace + `COL_CODE_BG` background, fence lines hidden.

- [ ] **Step 1: Add the flag + failing test**

In `src/markdown.h`, after `#define MD_FMT_STRIKE (1u<<6)` add:
```c
#define MD_FMT_CODEBLOCK (1u<<7)
```

Append inside `test_markdown(void)` in `tests/test_markdown.c`:
```c
    /* --- fenced code block --- */
    {
        const char* t = "```\ncode\n```";
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        int codefmt = 0, hides = 0;
        for (size_t i = 0; i < n; i++) {
            if (d[i].kind == DECO_FMT && (d[i].fmt & MD_FMT_CODEBLOCK)) codefmt = 1;
            if (d[i].kind == DECO_HIDE) hides++;
        }
        CHECK(codefmt == 1);   /* "code" styled as code block */
        CHECK(hides >= 2);     /* opening + closing fences hidden */
        free(d);
    }
```

- [ ] **Step 2: Run — verify fail**

Run: `make test`
Expected: FAIL — `MD_FMT_CODEBLOCK` unhandled, `codefmt == 0`.

- [ ] **Step 3: Implement in the builder**

In `src/markdown.c`, add code-block state to `DCtx` (inside the struct, after the heading-state fields):
```c
    int    in_code;
    int    code_first_text;
```

In `d_enter_block`, before `return 0;` add:
```c
    if (t == MD_BLOCK_CODE) { c->in_code = 1; c->code_first_text = 0; }
```

In `d_leave_block`, before `return 0;` add (hides the closing fence line):
```c
    if (t == MD_BLOCK_CODE) {
        size_t ce = c->last_text_end;
        if (ce < c->len && c->base[ce] == '\r') ce++;
        if (ce < c->len && c->base[ce] == '\n') ce++;      /* closing fence line start */
        size_t fe = ce;
        while (fe < c->len && c->base[fe] != '\n' && c->base[fe] != '\r') fe++;
        push_hide(c, ce, fe - ce);
        c->in_code = 0;
    }
```

In `d_text`, at the very top (right after `size_t off = ...;`), handle code specially and return (code content has no inline spans):
```c
    if (c->in_code) {
        if (!c->code_first_text) {
            c->code_first_text = 1;
            size_t p = off;                                 /* hide opening fence line */
            if (p > 0 && c->base[p-1] == '\n') p--;
            if (p > 0 && c->base[p-1] == '\r') p--;
            size_t fs = p;
            while (fs > 0 && c->base[fs-1] != '\n' && c->base[fs-1] != '\r') fs--;
            push_hide(c, fs, off - fs);
        }
        push(c, DECO_FMT, off, (size_t)size, MD_FMT_CODEBLOCK, PARA_NONE, 0);
        c->last_text_end = off + (size_t)size;
        return 0;
    }
```

- [ ] **Step 4: Run — verify pass**

Run: `make test`
Expected: PASS, `0 failed`.

- [ ] **Step 5: Render the background shade**

In `src/note_window.c`, add the palette constant next to the other `COL_*` definitions (near the top, with the `static const COLORREF COL_*` block):
```c
static const COLORREF COL_CODE_BG = RGB(0x18, 0x18, 0x1c);  /* code-block background */
```

In `nw_fmt_range`, add `CFM_BACKCOLOR` to the mask and a default back color, then set it for code blocks. Replace:
```c
    cf.dwMask = CFM_BOLD|CFM_ITALIC|CFM_STRIKEOUT|CFM_SIZE|CFM_FACE|CFM_COLOR|CFM_HIDDEN;
    cf.crTextColor = COL_TEXT;
    cf.yHeight = 200;
```
with:
```c
    cf.dwMask = CFM_BOLD|CFM_ITALIC|CFM_STRIKEOUT|CFM_SIZE|CFM_FACE|CFM_COLOR|CFM_HIDDEN|CFM_BACKCOLOR;
    cf.crTextColor = COL_TEXT;
    cf.crBackColor = COL_BG;
    if (fmt & MD_FMT_CODEBLOCK) cf.crBackColor = COL_CODE_BG;
    cf.yHeight = 200;
```

In `nw_apply_decos`, add `CFM_BACKCOLOR` to the base reset mask and reset the back color. Replace:
```c
    bf.dwMask = CFM_BOLD|CFM_ITALIC|CFM_STRIKEOUT|CFM_SIZE|CFM_FACE|CFM_COLOR|CFM_HIDDEN;
    bf.yHeight = 200; bf.crTextColor = COL_TEXT; wcscpy(bf.szFaceName, L"IBM Plex Mono");
```
with:
```c
    bf.dwMask = CFM_BOLD|CFM_ITALIC|CFM_STRIKEOUT|CFM_SIZE|CFM_FACE|CFM_COLOR|CFM_HIDDEN|CFM_BACKCOLOR;
    bf.yHeight = 200; bf.crTextColor = COL_TEXT; bf.crBackColor = COL_BG;
    wcscpy(bf.szFaceName, L"IBM Plex Mono");
```

- [ ] **Step 6: Build + test**

Run: `make clean && make`
Expected: links clean, no new warnings.
Run: `make test`
Expected: PASS, `0 failed`.
GUI (deferred to human): fenced blocks render monospace on a darker background, fences hidden.

- [ ] **Step 7: Commit**

```bash
git add src/markdown.h src/markdown.c src/note_window.c tests/test_markdown.c
git commit -m "feat(md): fenced code blocks (background shade, hidden fences)"
```

---

### Task 2: Blockquotes

**Files:**
- Modify: `src/markdown.h`, `src/markdown.c`
- Modify: `src/note_window.c`
- Test: `tests/test_markdown.c`

**Interfaces:**
- Produces: `MD_FMT_QUOTE` + `PARA_QUOTE` decorations; renders as dim + indented, `> ` hidden. Single-line/single-paragraph quotes (continuation-line `> ` on wrapped source lines is a documented limitation).

- [ ] **Step 1: Add the flag + PARA_QUOTE + failing test**

In `src/markdown.h`: add after `MD_FMT_CODEBLOCK`:
```c
#define MD_FMT_QUOTE (1u<<8)
```
and change the `ParaKind` enum to include `PARA_QUOTE`:
```c
typedef enum { PARA_NONE, PARA_BULLET, PARA_NUMBER, PARA_QUOTE } ParaKind;
```

Append inside `test_markdown(void)`:
```c
    /* --- blockquote --- */
    {
        const char* t = "> quoted";
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        int qfmt = 0, qpara = 0, hidmark = 0;
        for (size_t i = 0; i < n; i++) {
            if (d[i].kind == DECO_FMT && (d[i].fmt & MD_FMT_QUOTE)) qfmt = 1;
            if (d[i].kind == DECO_PARA && d[i].para == PARA_QUOTE) qpara = 1;
            if (d[i].kind == DECO_HIDE && d[i].start == 0 && d[i].len == 2) hidmark = 1;
        }
        CHECK(qfmt == 1);
        CHECK(qpara == 1);
        CHECK(hidmark == 1);   /* "> " hidden */
        free(d);
    }
```

- [ ] **Step 2: Run — verify fail**

Run: `make test`
Expected: FAIL — `qfmt == 0` (blockquote unhandled).

- [ ] **Step 3: Implement in the builder**

In `DCtx` add (after the code-block fields):
```c
    int    in_quote;
    int    quote_first_text;
```

In `d_enter_block`, before `return 0;`:
```c
    if (t == MD_BLOCK_QUOTE) { c->in_quote = 1; c->quote_first_text = 0; }
```
In `d_leave_block`, before `return 0;`:
```c
    if (t == MD_BLOCK_QUOTE) c->in_quote = 0;
```

In `d_text`, in the block that computes `MdFmt f` (right after the heading `if (c->in_heading) { ... }` block, before `if (f) push(...)`), add quote handling:
```c
    if (c->in_quote) {
        if (!c->quote_first_text) {
            c->quote_first_text = 1;
            size_t ls = off;
            while (ls > 0 && c->base[ls-1] != '\n' && c->base[ls-1] != '\r') ls--;
            push_hide(c, ls, off - ls);                    /* "> " */
            push(c, DECO_PARA, ls, (off - ls) + (size_t)size, 0, PARA_QUOTE, 0);
        }
        f |= MD_FMT_QUOTE;
    }
```

- [ ] **Step 4: Run — verify pass**

Run: `make test`
Expected: PASS, `0 failed`.

- [ ] **Step 5: Render dim + indent**

In `src/note_window.c`, in `nw_fmt_range`, set a dim color for quotes. After the line `if (fmt & MD_FMT_CODEBLOCK) cf.crBackColor = COL_CODE_BG;` add:
```c
    if (fmt & MD_FMT_QUOTE) cf.crTextColor = RGB(0x9a, 0x9a, 0xa2);   /* dim */
```

In `nw_para_range`, handle `PARA_QUOTE` (indent, no numbering). Replace:
```c
    if (kind == PARA_BULLET || kind == PARA_NUMBER) {
        pf.wNumbering = (kind == PARA_NUMBER) ? PFN_ARABIC : PFN_BULLET;
        pf.wNumberingStyle = (kind == PARA_NUMBER) ? PFNS_PERIOD : PFNS_PLAIN;
        pf.wNumberingTab = 280; pf.dxStartIndent = 280; pf.dxOffset = 280;
    }
```
with:
```c
    if (kind == PARA_BULLET || kind == PARA_NUMBER) {
        pf.wNumbering = (kind == PARA_NUMBER) ? PFN_ARABIC : PFN_BULLET;
        pf.wNumberingStyle = (kind == PARA_NUMBER) ? PFNS_PERIOD : PFNS_PLAIN;
        pf.wNumberingTab = 280; pf.dxStartIndent = 280; pf.dxOffset = 280;
    } else if (kind == PARA_QUOTE) {
        pf.dxStartIndent = 280;                            /* indent, no bullet */
    }
```

- [ ] **Step 6: Build + test**

Run: `make clean && make`
Expected: links clean, no new warnings.
Run: `make test`
Expected: PASS, `0 failed`.
GUI (deferred to human): `> ` lines render dim + indented, marker hidden.

- [ ] **Step 7: Commit**

```bash
git add src/markdown.h src/markdown.c src/note_window.c tests/test_markdown.c
git commit -m "feat(md): blockquotes (dim + indent, hidden marker)"
```

---

### Task 3: Task-list rendering

**Files:**
- Modify: `src/markdown.c`
- Test: `tests/test_markdown.c`

**Interfaces:**
- Produces: task list items render as bullets with the `[ ]`/`[x]` checkbox left visible (only `- ` hidden); checked items strike their content. Render-only — no click toggle. Needs `MD_FLAG_TASKLISTS` on the parser.

- [ ] **Step 1: Failing test**

Append inside `test_markdown(void)`:
```c
    /* --- task list --- */
    {
        const char* t = "- [ ] todo\n- [x] done";
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        int bullets = 0, struck = 0, checkbox_visible = 1;
        for (size_t i = 0; i < n; i++) {
            if (d[i].kind == DECO_PARA && d[i].para == PARA_BULLET) bullets++;
            if (d[i].kind == DECO_FMT && (d[i].fmt & MD_FMT_STRIKE)) struck = 1;
            /* a hide covering the first-line checkbox "[ ]" (offsets 2..5) would
             * mean the checkbox was hidden — it must NOT be */
            if (d[i].kind == DECO_HIDE && d[i].start <= 2 && d[i].start + d[i].len > 3)
                checkbox_visible = 0;
        }
        CHECK(bullets == 2);        /* both items are bullets */
        CHECK(struck == 1);         /* the checked item strikes its content */
        CHECK(checkbox_visible == 1); /* "[ ]" stays visible */
        free(d);
    }
```

- [ ] **Step 2: Run — verify fail**

Run: `make test`
Expected: FAIL — tasklists not enabled; `struck == 0` and the whole `- [ ] ` marker is hidden (`checkbox_visible == 0`).

- [ ] **Step 3: Enable tasklists + implement**

In `markdown_decorate`, add the tasklist flag. Replace:
```c
    p.flags = MD_DIALECT_COMMONMARK | MD_FLAG_STRIKETHROUGH;
```
with:
```c
    p.flags = MD_DIALECT_COMMONMARK | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;
```

In `DCtx`, add task state (after the list-state fields):
```c
    int    li_is_task;
    int    li_task_checked;
    size_t li_task_mark_off;    /* source offset of the mark char between [ ] */
```

In `d_enter_block`, extend the `MD_BLOCK_LI` case. Replace:
```c
    if (t == MD_BLOCK_LI) { c->in_li = 1; c->li_first_text = 0; }
```
with:
```c
    if (t == MD_BLOCK_LI) {
        MD_BLOCK_LI_DETAIL* d = (MD_BLOCK_LI_DETAIL*)detail;
        c->in_li = 1; c->li_first_text = 0;
        c->li_is_task = d->is_task;
        c->li_task_checked = d->is_task && (d->task_mark == 'x' || d->task_mark == 'X');
        c->li_task_mark_off = (size_t)d->task_mark_offset;
    }
```

In `d_text`, replace the existing list-item block:
```c
    if (c->in_li && !c->li_first_text) {
        c->li_first_text = 1;
        size_t ls = off;
        while (ls > 0 && c->base[ls-1] != '\n' && c->base[ls-1] != '\r') ls--;
        push_hide(c, ls, off - ls);           /* "- "/"N. " */
        push(c, DECO_PARA, ls, (off - ls) + (size_t)size,
             0, c->listkind,
             c->listkind == PARA_NUMBER ? c->listnum : 0);
    }
```
with:
```c
    if (c->in_li && !c->li_first_text) {
        c->li_first_text = 1;
        size_t ls = off;
        while (ls > 0 && c->base[ls-1] != '\n' && c->base[ls-1] != '\r') ls--;
        if (c->li_is_task) {
            /* keep "[x]" visible: hide "- " before it and the run after it up to
             * the content; the mark char sits at li_task_mark_off (between [ ]). */
            size_t cb0 = c->li_task_mark_off > 0 ? c->li_task_mark_off - 1 : 0; /* '[' */
            size_t cb1 = c->li_task_mark_off + 2;                               /* past ']' */
            push_hide(c, ls, cb0 - ls);        /* "- " */
            if (off > cb1) push_hide(c, cb1, off - cb1);   /* space(s) after "]" */
        } else {
            push_hide(c, ls, off - ls);        /* "- "/"N. " */
        }
        push(c, DECO_PARA, ls, (off - ls) + (size_t)size,
             0, c->listkind,
             c->listkind == PARA_NUMBER ? c->listnum : 0);
    }
```

Also make checked items strike their content: in the `MdFmt f` computation (after the list block, near where `f` is OR'd), add:
```c
    if (c->in_li && c->li_task_checked) f |= MD_FMT_STRIKE;
```
(Place this alongside the other `f |= ...` lines, before `if (f) push(c, DECO_FMT, ...)`.)

- [ ] **Step 4: Run — verify pass**

Run: `make test`
Expected: PASS, `0 failed`.

No `note_window.c` change is needed — the bullet + strike render through the existing `nw_para_range`/`nw_fmt_range`.

- [ ] **Step 5: Build + commit**

Run: `make clean && make` → clean; `make test` → `0 failed`.
GUI (deferred to human): `- [ ]`/`- [x]` render as bullets with a visible checkbox; checked items struck through.
```bash
git add src/markdown.c tests/test_markdown.c
git commit -m "feat(md): task-list rendering (visible checkbox, strike when checked)"
```

---

### Task 4: Links — builder (`DECO_LINK` + syntax hiding)

**Files:**
- Modify: `src/markdown.h`, `src/markdown.c`
- Test: `tests/test_markdown.c`

**Interfaces:**
- Produces:
  - `DecoKind` gains `DECO_LINK`; `Deco` gains `size_t aux_start, aux_len;` (url source span, used only by `DECO_LINK`).
  - `MD_FMT_LINK (1u<<9)`.
  - For each `[text](url)`: a `DECO_FMT` `MD_FMT_LINK` over `text`, a `DECO_LINK` with `start`/`len` = the text range and `aux_start`/`aux_len` = the url source span, and the `[` + `](url)` syntax hidden.

- [ ] **Step 1: Extend types + failing test**

In `src/markdown.h`:
- Add the flag: `#define MD_FMT_LINK (1u<<9)`.
- Extend `DecoKind`: `typedef enum { DECO_FMT, DECO_HIDE, DECO_PARA, DECO_LINK } DecoKind;`
- Add fields to `Deco` (after `int number;`):
  ```c
  size_t   aux_start; /* DECO_LINK: url source span start */
  size_t   aux_len;   /* DECO_LINK: url source span length */
  ```

Append inside `test_markdown(void)`:
```c
    /* --- link --- */
    {
        const char* t = "[go](http://x)";  /* text 1..3, url 5..13 */
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        int linkfmt = 0, linkdeco = 0, hidopen = 0;
        for (size_t i = 0; i < n; i++) {
            if (d[i].kind == DECO_FMT && (d[i].fmt & MD_FMT_LINK)) linkfmt = 1;
            if (d[i].kind == DECO_LINK) {
                linkdeco = 1;
                CHECK(d[i].start == 1 && d[i].len == 2);        /* "go" */
                CHECK(strncmp(t + d[i].aux_start, "http://x", 8) == 0);
                CHECK(d[i].aux_len == 8);
            }
            if (d[i].kind == DECO_HIDE && d[i].start == 0 && d[i].len == 1) hidopen = 1;
        }
        CHECK(linkfmt == 1);
        CHECK(linkdeco == 1);
        CHECK(hidopen == 1);   /* "[" hidden */
        free(d);
    }
```

- [ ] **Step 2: Run — verify fail**

Run: `make test`
Expected: FAIL — `DECO_LINK` undefined / `linkdeco == 0`.

- [ ] **Step 3: Implement in the builder**

Make `push` initialize the new fields. In `push`, after `d->fmt = fmt; d->para = para; d->number = number;` add:
```c
    d->aux_start = 0; d->aux_len = 0;
```

Add a link emitter above `push_hide` (after `push`):
```c
static void push_link(DCtx* c, size_t start, size_t len, size_t url_start, size_t url_len) {
    push(c, DECO_LINK, start, len, MD_FMT_LINK, PARA_NONE, 0);
    if (c->count > 0) {
        Deco* d = &c->arr[c->count - 1];
        d->aux_start = url_start; d->aux_len = url_len;
    }
}
```

Add link state to `DCtx` (after the task fields):
```c
    int    in_a;
    int    a_text_set;
    size_t a_text_start;
```

Handle `MD_SPAN_A` in `d_enter_span` — add at the very top, before the `if (!span_fmt(t))` guard:
```c
    if (t == MD_SPAN_A) { c->in_a = 1; c->a_text_set = 0; return 0; }
```

In `d_text`, capture the link text start + hide the `[` + flag `MD_FMT_LINK`. Add this right after the opening-marker loop (before the list-item block), and OR the flag into `f` in the format section:
```c
    if (c->in_a && !c->a_text_set) {
        c->a_text_set = 1;
        c->a_text_start = off;
        size_t ab = off;                       /* scan left to the '[' */
        while (ab > 0 && c->base[ab-1] != '[') ab--;
        if (ab > 0) push_hide(c, ab - 1, 1);   /* hide '[' */
    }
```
and in the `MdFmt f` section (with the other `f |= ...`):
```c
    if (c->in_a) f |= MD_FMT_LINK;
```

Handle `MD_SPAN_A` in `d_leave_span` — add at the very top, before the `if (!span_fmt(t))` guard:
```c
    if (t == MD_SPAN_A) {
        size_t te = c->last_text_end;          /* end of visible link text */
        size_t p = te;
        if (p < c->len && c->base[p] == ']') p++;
        if (p < c->len && c->base[p] == '(') p++;
        size_t url_start = p;
        while (p < c->len && c->base[p] != ')' && c->base[p] != ' ' &&
               c->base[p] != '\t' && c->base[p] != '\n') p++;
        size_t url_len = p - url_start;
        while (p < c->len && c->base[p] != ')' && c->base[p] != '\n') p++;  /* skip title */
        if (p < c->len && c->base[p] == ')') p++;                            /* include ')' */
        push_hide(c, te, p - te);              /* hide "](url...)" */
        if (c->a_text_set)
            push_link(c, c->a_text_start, te - c->a_text_start, url_start, url_len);
        c->in_a = 0;
        return 0;
    }
```

- [ ] **Step 4: Run — verify pass**

Run: `make test`
Expected: PASS, `0 failed`.

Note: `note_window.c` does not yet handle `DECO_LINK` (its `nw_apply_decos` switch ignores unknown kinds) or `MD_FMT_LINK` (no matching effect), so links render as plain text for now — wired up in Task 5. Build stays clean.

- [ ] **Step 5: Build + commit**

Run: `make clean && make` → clean, no warnings; `make test` → `0 failed`.
```bash
git add src/markdown.h src/markdown.c tests/test_markdown.c
git commit -m "feat(md): link decorations (DECO_LINK + url span, hidden syntax)"
```

---

### Task 5: Links — render + Ctrl+click activation

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: Task 4's `DECO_LINK` (with `aux_start`/`aux_len`) and `MD_FMT_LINK`.
- Produces: link text rendered accent + underline + `CFE_LINK`; a per-window link table filled from `DECO_LINK`; `EN_LINK` Ctrl+click opens `http`/`https`/`mailto` URLs via `ShellExecuteW`.

- [ ] **Step 1: Add the link table + event mask**

In `src/note_window.c`, add `#include <shellapi.h>` near the other includes (top of file, after the existing `#include`s).

Add `ENM_LINK` to the edit event mask. Replace:
```c
#define EDIT_EVENT_MASK (ENM_CHANGE | ENM_KEYEVENTS | ENM_SELCHANGE)
```
with:
```c
#define EDIT_EVENT_MASK (ENM_CHANGE | ENM_KEYEVENTS | ENM_SELCHANGE | ENM_LINK)
```

In `struct NoteWin`, add the link table (after the `GfxD2D* gfx;` field):
```c
    struct { LONG a, b; char url[512]; } links[256];
    int   nlinks;
```

- [ ] **Step 2: Render link char format**

In `nw_fmt_range`, add link effects. Add `CFM_LINK|CFM_UNDERLINE` to the mask — replace:
```c
    cf.dwMask = CFM_BOLD|CFM_ITALIC|CFM_STRIKEOUT|CFM_SIZE|CFM_FACE|CFM_COLOR|CFM_HIDDEN|CFM_BACKCOLOR;
```
with:
```c
    cf.dwMask = CFM_BOLD|CFM_ITALIC|CFM_STRIKEOUT|CFM_SIZE|CFM_FACE|CFM_COLOR|CFM_HIDDEN|CFM_BACKCOLOR|CFM_LINK|CFM_UNDERLINE;
```
and after the `if (fmt & MD_FMT_STRIKE) cf.dwEffects |= CFE_STRIKEOUT;` line add:
```c
    if (fmt & MD_FMT_LINK) {
        cf.crTextColor = COL_ACCENT;
        cf.dwEffects |= CFE_LINK | CFE_UNDERLINE;
    }
```

In `nw_apply_decos`, add the same bits to the base reset mask so link/underline clears on reset — replace:
```c
    bf.dwMask = CFM_BOLD|CFM_ITALIC|CFM_STRIKEOUT|CFM_SIZE|CFM_FACE|CFM_COLOR|CFM_HIDDEN|CFM_BACKCOLOR;
```
with:
```c
    bf.dwMask = CFM_BOLD|CFM_ITALIC|CFM_STRIKEOUT|CFM_SIZE|CFM_FACE|CFM_COLOR|CFM_HIDDEN|CFM_BACKCOLOR|CFM_LINK|CFM_UNDERLINE;
```

- [ ] **Step 3: Fill the link table in the apply pass**

`nw_apply_decos` needs the source buffer + the `NoteWin` to record links. Change its signature and callers.

Replace the signature line:
```c
static void nw_apply_decos(HWND edit, int len, Deco* d, size_t n, size_t lo, size_t hi) {
```
with:
```c
static void nw_apply_decos(NoteWin* nw, HWND edit, const char* buf, int len,
                           Deco* d, size_t n, size_t lo, size_t hi) {
```

At the top of `nw_apply_decos`, before the base reset, clear the table when doing a whole-document apply:
```c
    if (lo == 0 && hi >= (size_t)len) nw->nlinks = 0;   /* rebuild on full restyle */
```

In the decoration loop, add a `DECO_LINK` branch (after the `DECO_PARA` branch):
```c
        else if (d[i].kind == DECO_LINK) {
            if (nw->nlinks < 256) {
                size_t ul = d[i].aux_len; if (ul > 511) ul = 511;
                nw->links[nw->nlinks].a = (LONG)a;
                nw->links[nw->nlinks].b = (LONG)b;
                memcpy(nw->links[nw->nlinks].url, buf + d[i].aux_start, ul);
                nw->links[nw->nlinks].url[ul] = '\0';
                nw->nlinks++;
            }
        }
```

Update the two call sites (both in the restyle path). Find `nw_apply_decos(edit, len, d, n, 0, (size_t)len);` (in `nw_apply_format_edit`/`nw_restyle` whole-doc path) and `nw_apply_decos(edit, len, d, n, lo, hi);` (scoped path) and change them to pass `nw` and `buf`:
```c
    nw_apply_decos(nw, edit, buf, len, d, n, 0, (size_t)len);
```
and
```c
    nw_apply_decos(nw, edit, buf, len, d, n, lo, hi);
```
(`nw` and `buf` are already in scope in `nw_restyle`.)

- [ ] **Step 4: Handle EN_LINK Ctrl+click**

Add a URL-open helper above `nw_proc`:
```c
/* Open http/https/mailto URLs only; reject everything else. */
static void nw_open_url(const char* url) {
    if (!( _strnicmp(url, "http://", 7) == 0 ||
           _strnicmp(url, "https://", 8) == 0 ||
           _strnicmp(url, "mailto:", 7) == 0 )) return;
    wchar_t wurl[1024];
    if (MultiByteToWideChar(CP_ACP, 0, url, -1, wurl, 1024) == 0) return;
    ShellExecuteW(NULL, L"open", wurl, NULL, NULL, SW_SHOWNORMAL);
}
```

In `nw_proc`'s `WM_NOTIFY` handler, add an `EN_LINK` case alongside the existing `EN_SELCHANGE` / `EN_MSGFILTER` checks:
```c
        if (nw && hdr->code == EN_LINK && hdr->idFrom == ID_EDIT) {
            ENLINK* el = (ENLINK*)lp;
            if (el->msg == WM_LBUTTONUP && (GetKeyState(VK_CONTROL) & 0x8000)) {
                for (int i = 0; i < nw->nlinks; i++)
                    if (el->chrg.cpMin >= nw->links[i].a && el->chrg.cpMin < nw->links[i].b) {
                        nw_open_url(nw->links[i].url);
                        break;
                    }
                return 1;
            }
            return 0;
        }
```

- [ ] **Step 5: Build + test**

Run: `make clean && make`
Expected: links clean, no new warnings (`shellapi.h`/`ShellExecuteW` resolve via the already-linked `-lshell32`).
Run: `make test`
Expected: PASS, `0 failed` (this task is GUI wiring; logic tests unchanged).
GUI (deferred to human): links render accent + underline; the `[]()` syntax hidden; Ctrl+click opens `http`/`https`/`mailto` in the browser/mail client; plain click just places the caret; non-http schemes do nothing.

- [ ] **Step 6: Commit**

```bash
git add src/note_window.c
git commit -m "feat(md): link rendering + Ctrl+click open (http/https/mailto)"
```

---

## Self-Review Notes

**Spec coverage:**
- Parser/data-model (`MD_FLAG_TASKLISTS`, new `MdFmt` bits, `PARA_QUOTE`, `DECO_LINK` + `aux`) → Tasks 1–4 add each where first needed.
- Fenced code blocks (style + `COL_CODE_BG` bg + hidden fences) → Task 1.
- Blockquotes (dim + indent + hidden `> `) → Task 2.
- Links (hide `[]()`, accent+underline+`CFE_LINK`, per-window link table, `EN_LINK` Ctrl+click, http/https/mailto only) → Task 4 (builder) + Task 5 (render + click + security).
- Task-list rendering (visible checkbox, `- ` hidden, checked → strike, render-only) → Task 3.
- Cursor-reveal for new markers → automatic (all hides via `push_hide`); asserted implicitly (hide decorations honor the reveal window, unchanged from A).
- Deferred (tables, clickable task toggle, nested lists, colored quote bar) → not in any task, matching the spec.

**Placeholder scan:** none — every code step carries complete code; every test step has real `CHECK` assertions + expected `make test` output.

**Type consistency:** `MD_FMT_CODEBLOCK/QUOTE/LINK` bits (1u<<7/8/9) defined once, used in builder + `nw_fmt_range`. `PARA_QUOTE` added to the enum (Task 2) and handled in `nw_para_range` (Task 2). `DECO_LINK` + `aux_start`/`aux_len` defined Task 4, consumed Task 5. `nw_apply_decos` signature change (Task 5) updates both call sites. `push`'s aux-zeroing (Task 4) keeps every non-link `Deco` well-defined. `nw_open_url`/link-table field names consistent between Task 3/5 definitions and uses.

**Known limitations (documented):** blockquote continuation-line `> ` on wrapped source lines may stay visible (single-paragraph handling); link visible-text containing a literal `[` could mis-locate the opening bracket (rare); url span scan assumes standard `](url)` / `](url "title")` forms.
