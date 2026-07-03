# Markdown C1: Correctness Polish + Cleanups — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Clear the visible link/quote/paste warts deferred from A and B, plus two trivial cleanups, on the decoration engine.

**Architecture:** Extend `markdown_decorate` in `src/markdown.c` (URL pool for full link-type support, blockquote-continuation hiding) and adjust the `note_window.c` executor (URL-pool plumbing, paste-scoping fallback, per-tab caret field, rename). Builder changes are TDD; executor changes are build + manual.

**Tech Stack:** C11, MSYS2 mingw-w64 gcc, md4c (existing), Win32 RichEdit.

## Global Constraints

- Pure C, `-std=c11`, `-mwindows`, gcc 13.2 (MSYS2 mingw-w64). No C++. No new deps.
- All markdown knowledge in `src/markdown.c`; `note_window.c` = RichEdit mechanics only.
- Decorations style/hide existing source chars only; marker hides via `push_hide` (cursor-reveal). Offsets are RichEdit `\r`/CP_ACP char offsets.
- Link URLs come from md4c's decoded `MD_SPAN_A_DETAIL.href` (verified present, with `title` + `is_autolink`; `MD_ATTRIBUTE.text`/`size` are NOT NUL-terminated — copy by `size`). Stored in a per-parse URL pool; `DECO_LINK.aux_start`/`aux_len` index the pool.
- Link security unchanged: `note_window` still opens only http/https/mailto on Ctrl+click.
- Testing: builder is pure logic → real TDD via `tests/test_markdown.c` (`CHECK` from `tests/test.h`), gated by `make test`. Executor changes are GUI/mechanical — gate is `make` clean + `make test` green; visual confirmation deferred to the human (scripted GUI focus unavailable here).

---

### Task 1: Link overhaul — URL pool + full inline/reference/shortcut/autolink support

**Files:**
- Modify: `src/markdown.h` (`markdown_decorate` signature)
- Modify: `src/markdown.c` (URL pool, `MD_SPAN_A` handling, `markdown_fmt_at`)
- Modify: `src/note_window.c` (`nw_apply_decos` URL-pool param + fill, `nw_restyle` plumbing)
- Test: `tests/test_markdown.c`

**Interfaces:**
- Produces: `size_t markdown_decorate(const char* text, size_t len, size_t sel_lo, size_t sel_hi, Deco** out, char** urlpool);` — `*urlpool` is a `malloc`'d buffer (or NULL if no links); caller frees. `DECO_LINK.aux_start`/`aux_len` index it.
- Consumes: existing `push_link`, `DECO_LINK`, `MD_FMT_LINK`, `aux_start`/`aux_len` fields.

- [ ] **Step 1: Update the signature + write failing tests**

In `src/markdown.h`, change the `markdown_decorate` prototype to:
```c
size_t markdown_decorate(const char* text, size_t len, size_t sel_lo,
                         size_t sel_hi, Deco** out, char** urlpool);
```

In `tests/test_markdown.c`, add these new tests inside `test_markdown(void)` (they use the new signature):
```c
    /* --- reference link: url comes from the [r]: definition --- */
    {
        const char* t = "[go][r]\n\n[r]: http://x";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int ok = 0, hid_open = 0;
        for (size_t i = 0; i < n; i++) {
            if (d[i].kind == DECO_LINK && d[i].aux_len == 8 &&
                strncmp(pool + d[i].aux_start, "http://x", 8) == 0) ok = 1;
            if (d[i].kind == DECO_HIDE && d[i].start == 0 && d[i].len == 1) hid_open = 1;
        }
        CHECK(ok == 1);        /* url resolved from the reference definition */
        CHECK(hid_open == 1);  /* "[" hidden */
        free(d); free(pool);
    }
    /* --- autolink: <url> ; visible text is the url, < and > hidden --- */
    {
        const char* t = "<http://x>";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int ok = 0, hid_lt = 0, hid_gt = 0;
        for (size_t i = 0; i < n; i++) {
            if (d[i].kind == DECO_LINK && d[i].aux_len == 8 &&
                strncmp(pool + d[i].aux_start, "http://x", 8) == 0) ok = 1;
            if (d[i].kind == DECO_HIDE && d[i].start == 0 && d[i].len == 1) hid_lt = 1;   /* "<" */
            if (d[i].kind == DECO_HIDE && d[i].start == 9 && d[i].len == 1) hid_gt = 1;   /* ">" */
        }
        CHECK(ok == 1);
        CHECK(hid_lt == 1 && hid_gt == 1);
        free(d); free(pool);
    }
    /* --- inline link regression: url from href, still correct --- */
    {
        const char* t = "[go](http://x)";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int ok = 0;
        for (size_t i = 0; i < n; i++)
            if (d[i].kind == DECO_LINK && d[i].aux_len == 8 &&
                strncmp(pool + d[i].aux_start, "http://x", 8) == 0) ok = 1;
        CHECK(ok == 1);
        free(d); free(pool);
    }
```

Then update EVERY existing `markdown_decorate(...)` call in `tests/test_markdown.c` to the new signature: add a `char* pool = NULL;` alongside each `Deco* d = NULL;`, pass `&pool` as the final argument, and add `free(pool);` next to each `free(d);`. (There are several existing blocks — headings, inline, lists, reveal, code, quote, task, link. Update all of them.)

- [ ] **Step 2: Run tests — verify they fail to build**

Run: `make test`
Expected: FAIL — signature mismatch / new tests reference the pool (compile error until Step 3).

- [ ] **Step 3: Implement the URL pool + full link handling in the builder**

In `src/markdown.c`, add pool + link fields to `DCtx` (inside the struct, after the existing `/* link state */` fields `int in_a; int a_text_set; size_t a_text_start;`):
```c
    int    a_is_autolink;
    size_t a_url_off, a_url_len;   /* url span in the pool */
    char*  urlpool;
    size_t upcount, upcap;
```

Add a pool helper above `push` (right after the `DCtx` typedef / `in_reveal`):
```c
/* Append n bytes to the url pool, returning the start offset. Not NUL-terminated
 * in the pool (the consumer terminates its own copy). */
static size_t pool_add(DCtx* c, const char* s, size_t n) {
    if (c->upcount + n > c->upcap) {
        size_t nc = c->upcap ? c->upcap * 2 : 128;
        while (nc < c->upcount + n) nc *= 2;
        char* np = realloc(c->urlpool, nc);
        if (!np) return c->upcount;    /* drop on OOM; url stays empty */
        c->urlpool = np; c->upcap = nc;
    }
    size_t off = c->upcount;
    if (n) { memcpy(c->urlpool + off, s, n); c->upcount += n; }
    return off;
}
```

Rewrite `d_enter_span`'s `MD_SPAN_A` branch to capture the href + autolink flag. Replace:
```c
    if (t == MD_SPAN_A) { c->in_a = 1; c->a_text_set = 0; return 0; }
```
with:
```c
    if (t == MD_SPAN_A) {
        MD_SPAN_A_DETAIL* a = (MD_SPAN_A_DETAIL*)detail;
        c->in_a = 1; c->a_text_set = 0;
        c->a_is_autolink = a->is_autolink;
        c->a_url_len = (size_t)a->href.size;
        c->a_url_off = pool_add(c, a->href.text, c->a_url_len);
        return 0;
    }
```
(and remove the leading `(void)detail;` in `d_enter_span` since `detail` is now used).

Rewrite the `MD_SPAN_A` opening-hide in `d_text`. Replace:
```c
    if (c->in_a && !c->a_text_set) {
        c->a_text_set = 1;
        c->a_text_start = off;
        size_t ab = off;                       /* scan left to the '[' */
        while (ab > 0 && c->base[ab-1] != '[') ab--;
        if (ab > 0) push_hide(c, ab - 1, 1);   /* hide '[' */
    }
```
with:
```c
    if (c->in_a && !c->a_text_set) {
        c->a_text_set = 1;
        c->a_text_start = off;
        if (c->a_is_autolink) {
            if (off > 0) push_hide(c, off - 1, 1);      /* hide '<' */
        } else {
            size_t ab = off;                            /* scan left to the '[' */
            while (ab > 0 && c->base[ab-1] != '[') ab--;
            if (ab > 0) push_hide(c, ab - 1, 1);        /* hide '[' */
        }
    }
```

Rewrite `d_leave_span`'s `MD_SPAN_A` branch to complete every link type using the pooled url. Replace the whole `if (t == MD_SPAN_A) { ... return 0; }` block with:
```c
    if (t == MD_SPAN_A) {
        size_t te = c->last_text_end > c->close_cursor ? c->last_text_end
                                                       : c->close_cursor;
        if (c->a_is_autolink) {
            size_t he = te;
            if (te < c->len && c->base[te] == '>') he = te + 1;   /* hide '>' */
            push_hide(c, te, he - te);
        } else if (te < c->len && c->base[te] == ']') {
            size_t p = te + 1;
            if (p < c->len && c->base[p] == '(') {                /* inline "](url)" */
                while (p < c->len && c->base[p] != ')' && c->base[p] != '\n') p++;
                if (p < c->len && c->base[p] == ')') p++;
            } else if (p < c->len && c->base[p] == '[') {         /* reference "][ref]" */
                p++;
                while (p < c->len && c->base[p] != ']' && c->base[p] != '\n') p++;
                if (p < c->len && c->base[p] == ']') p++;
            }
            /* else shortcut "]": p stays te+1 */
            push_hide(c, te, p - te);
        }
        if (c->a_text_set)
            push_link(c, c->a_text_start, te - c->a_text_start,
                      c->a_url_off, c->a_url_len);
        c->in_a = 0;
        return 0;
    }
```

Update `markdown_decorate` to emit the pool. Replace its body:
```c
size_t markdown_decorate(const char* text, size_t len,
                         size_t sel_lo, size_t sel_hi, Deco** out) {
    DCtx c; memset(&c, 0, sizeof c);
    c.base = text; c.len = len;
    c.sel_lo = sel_lo; c.sel_hi = sel_hi;
    MD_PARSER p; memset(&p, 0, sizeof p);
    p.abi_version = 0;
    p.flags = MD_DIALECT_COMMONMARK | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;
    p.enter_block = d_enter_block;
    p.leave_block = d_leave_block;
    p.text        = d_text;
    p.enter_span = d_enter_span;
    p.leave_span = d_leave_span;
    md_parse(text, (MD_SIZE)len, &p, &c);
    *out = c.arr;
    return c.count;
}
```
with:
```c
size_t markdown_decorate(const char* text, size_t len,
                         size_t sel_lo, size_t sel_hi, Deco** out, char** urlpool) {
    DCtx c; memset(&c, 0, sizeof c);
    c.base = text; c.len = len;
    c.sel_lo = sel_lo; c.sel_hi = sel_hi;
    MD_PARSER p; memset(&p, 0, sizeof p);
    p.abi_version = 0;
    p.flags = MD_DIALECT_COMMONMARK | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;
    p.enter_block = d_enter_block;
    p.leave_block = d_leave_block;
    p.text        = d_text;
    p.enter_span = d_enter_span;
    p.leave_span = d_leave_span;
    md_parse(text, (MD_SIZE)len, &p, &c);
    *out = c.arr;
    *urlpool = c.urlpool;
    return c.count;
}
```

Update `markdown_fmt_at` to pass + free the pool. Replace:
```c
    Deco* d = NULL;
    size_t n = markdown_decorate(text, len, (size_t)-1, 0, &d);
```
with:
```c
    Deco* d = NULL; char* pool = NULL;
    size_t n = markdown_decorate(text, len, (size_t)-1, 0, &d, &pool);
```
and change its `free(d);` to:
```c
    free(d); free(pool);
```

- [ ] **Step 4: Plumb the URL pool through `note_window.c`**

In `src/note_window.c`, change `nw_apply_decos` to take the pool and copy the url from it. Replace the signature line:
```c
static void nw_apply_decos(NoteWin* nw, HWND edit, const char* buf, int len,
                           Deco* d, size_t n, size_t lo, size_t hi) {
```
with:
```c
static void nw_apply_decos(NoteWin* nw, HWND edit, const char* buf, const char* urlpool,
                           int len, Deco* d, size_t n, size_t lo, size_t hi) {
```
and in the link-table fill loop, replace:
```c
        memcpy(nw->links[nw->nlinks].url, buf + d[i].aux_start, ul);
```
with:
```c
        memcpy(nw->links[nw->nlinks].url, urlpool + d[i].aux_start, ul);
```
(`buf` stays a parameter — it is still used for the paragraph-numbering reset context; only the url source changes to `urlpool`.)

In `nw_restyle`, thread the pool through the one call site. Replace:
```c
    Deco* d = NULL;
    size_t n = markdown_decorate(buf, (size_t)len, sel_lo, sel_hi, &d);
    nw_apply_decos(nw, edit, buf, len, d, n, lo, hi);
    free(d);
    free(buf);
```
with:
```c
    Deco* d = NULL; char* pool = NULL;
    size_t n = markdown_decorate(buf, (size_t)len, sel_lo, sel_hi, &d, &pool);
    nw_apply_decos(nw, edit, buf, pool, len, d, n, lo, hi);
    free(d);
    free(pool);
    free(buf);
```

- [ ] **Step 5: Build + test**

Run: `make clean && make`
Expected: links clean, no new warnings.
Run: `make test`
Expected: PASS, `0 failed` (reference/autolink/inline link tests + all updated existing tests).
GUI (deferred to human): inline, reference, and autolink links all render accent+underline with syntax hidden and open on Ctrl+click.

- [ ] **Step 6: Commit**

```bash
git add src/markdown.h src/markdown.c src/note_window.c tests/test_markdown.c
git commit -m "feat(md): full inline/reference/shortcut/autolink support via url pool"
```

---

### Task 2: Blockquote continuation `> ` hiding

**Files:**
- Modify: `src/markdown.c`
- Test: `tests/test_markdown.c`

**Interfaces:**
- Consumes: `push_hide`, quote state in `DCtx`.
- Produces: every source line of a blockquote gets its leading `>`/`> ` hidden and the whole quote indented via one `PARA_QUOTE`.

- [ ] **Step 1: Write the failing test**

In `tests/test_markdown.c`, add inside `test_markdown(void)` (new signature):
```c
    /* --- multi-line blockquote: BOTH "> " markers hidden --- */
    {
        const char* t = "> a\n> b";   /* line1 "> " at 0, line2 "> " at 4 */
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int hid1 = 0, hid2 = 0;
        for (size_t i = 0; i < n; i++) if (d[i].kind == DECO_HIDE) {
            if (d[i].start == 0 && d[i].len == 2) hid1 = 1;
            if (d[i].start == 4 && d[i].len == 2) hid2 = 1;
        }
        CHECK(hid1 == 1);   /* first line "> " */
        CHECK(hid2 == 1);   /* continuation "> " */
        free(d); free(pool);
    }
```

- [ ] **Step 2: Run — verify fail**

Run: `make test`
Expected: FAIL — only the first `> ` is hidden; `hid2 == 0`.

- [ ] **Step 3: Implement**

Add `size_t quote_start;` to `DCtx` (next to the `/* quote block state */` fields `int in_quote; int quote_first_text;`).

In `d_text`, replace the quote block:
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
with (record the quote start; defer all marker hiding + the paragraph indent to leave, so every line is covered):
```c
    if (c->in_quote) {
        if (!c->quote_first_text) {
            c->quote_first_text = 1;
            size_t ls = off;
            while (ls > 0 && c->base[ls-1] != '\n' && c->base[ls-1] != '\r') ls--;
            c->quote_start = ls;
        }
        f |= MD_FMT_QUOTE;
    }
```

In `d_leave_block`, replace:
```c
    if (t == MD_BLOCK_QUOTE) c->in_quote = 0;
```
with:
```c
    if (t == MD_BLOCK_QUOTE) {
        if (c->quote_first_text) {
            /* indent the whole quote, then hide the "> " prefix on every line */
            push(c, DECO_PARA, c->quote_start,
                 c->last_text_end - c->quote_start, 0, PARA_QUOTE, 0);
            size_t p = c->quote_start;
            while (p < c->last_text_end) {
                size_t ls = p, q = p;
                while (q < c->len && c->base[q] == ' ') q++;   /* optional indent */
                if (q < c->len && c->base[q] == '>') {
                    q++;
                    if (q < c->len && c->base[q] == ' ') q++;
                    push_hide(c, ls, q - ls);                  /* ">"/"> " */
                }
                while (p < c->len && c->base[p] != '\n' && c->base[p] != '\r') p++;
                if (p < c->len && c->base[p] == '\r') p++;
                if (p < c->len && c->base[p] == '\n') p++;
            }
        }
        c->in_quote = 0;
        c->quote_first_text = 0;
    }
```

- [ ] **Step 4: Run — verify pass**

Run: `make test`
Expected: PASS, `0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/markdown.c tests/test_markdown.c
git commit -m "feat(md): hide blockquote continuation-line markers"
```

---

### Task 3: Multi-paragraph paste scoping fallback

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: `nw_restyle`, `NoteTab`.
- Produces: `NoteTab.last_lines`; a line-count-delta check in `nw_restyle` that forces whole-document restyle when the paragraph count changed (multi-line paste/cut), even with a collapsed caret.

- [ ] **Step 1: Add the per-tab line count + force-whole-doc logic**

In `src/note_window.c`, add a field to `NoteTab`:
```c
typedef struct {
    char id[16];      /* note id */
    HWND edit;        /* this tab's body RichEdit */
    int  last_lines;  /* line count at last restyle; a change forces whole-doc */
} NoteTab;
```

In `nw_restyle`, after the buffer is read (right before the `CHARRANGE saved;` line), compute the current line count and force `scoped = 0` if it changed since the last restyle:
```c
    /* A multi-line paste/cut collapses the caret, so a scoped restyle would miss
     * the other changed paragraphs. Detect it via a line-count delta and fall
     * back to a whole-document restyle. */
    int cur_lines = 1;
    for (int k = 0; k < len; k++) if (buf[k] == '\n' || buf[k] == '\r') cur_lines++;
    if (cur_lines != nw->tab[tab].last_lines) scoped = 0;
    nw->tab[tab].last_lines = cur_lines;
```
(Place this after the `else buf[0] = '\0';` block that fills `buf`, and before `CHARRANGE saved; ...`. `scoped` is the function parameter and may be reassigned.)

- [ ] **Step 2: Build + test**

Run: `make clean && make`
Expected: links clean, no new warnings.
Run: `make test`
Expected: PASS, `0 failed` (logic tests unchanged; this is GUI wiring).
GUI (deferred to human): paste a multi-paragraph markdown block — all pasted paragraphs render styled immediately (no unstyled paragraphs until the next caret move).

- [ ] **Step 3: Commit**

```bash
git add src/note_window.c
git commit -m "fix(md): force whole-doc restyle on multi-line paste (line-count delta)"
```

---

### Task 4: Cleanups — per-tab `last_caret_para` + rename `nw_hide_range2`

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: `NoteTab`, the `EN_SELCHANGE` reveal handler, `nw_apply_decos`.
- Produces: `NoteTab.last_caret_para` (replacing the shared `NoteWin.last_caret_para`); `nw_hide_range` (renamed from `nw_hide_range2`).

- [ ] **Step 1: Move `last_caret_para` onto the tab**

In `src/note_window.c`, remove the shared field from `struct NoteWin` — delete this line:
```c
    size_t last_caret_para;   /* start offset of the paragraph last revealed */
```
and add it to `NoteTab` (alongside `last_lines` from Task 3):
```c
    size_t last_caret_para;   /* paragraph offset last revealed (per tab) */
```
So `NoteTab` is now:
```c
typedef struct {
    char id[16];      /* note id */
    HWND edit;        /* this tab's body RichEdit */
    int  last_lines;  /* line count at last restyle; a change forces whole-doc */
    size_t last_caret_para;   /* paragraph offset last revealed (per tab) */
} NoteTab;
```

In the `EN_SELCHANGE` reveal handler, replace the two `nw->last_caret_para` uses:
```c
                    if (lo != nw->last_caret_para) {
                        nw->last_caret_para = lo;
```
with the active tab's field:
```c
                    if (lo != nw->tab[nw->active].last_caret_para) {
                        nw->tab[nw->active].last_caret_para = lo;
```

- [ ] **Step 2: Rename `nw_hide_range2` → `nw_hide_range`**

The original `nw_hide_range` was deleted during A; restore the clean name. In `src/note_window.c`, rename the function definition:
```c
static void nw_hide_range2(HWND edit, size_t start, size_t len) {
```
to:
```c
static void nw_hide_range(HWND edit, size_t start, size_t len) {
```
and update its one call site in `nw_apply_decos`:
```c
        else if (d[i].kind == DECO_HIDE) nw_hide_range2(edit, a, d[i].len);
```
to:
```c
        else if (d[i].kind == DECO_HIDE) nw_hide_range(edit, a, d[i].len);
```

- [ ] **Step 3: Build + verify no dangling references**

Run: `grep -n "nw_hide_range2\|nw->last_caret_para" src/note_window.c`
Expected: no matches.

Run: `make clean && make`
Expected: links clean, no new warnings.
Run: `make test`
Expected: PASS, `0 failed`.

- [ ] **Step 4: Commit**

```bash
git add src/note_window.c
git commit -m "refactor(md): per-tab last_caret_para; rename nw_hide_range2 -> nw_hide_range"
```

---

## Self-Review Notes

**Spec coverage:**
- Link overhaul (url pool, inline/reference/shortcut/autolink, per-type hiding, href-sourced url) → Task 1.
- Blockquote continuation `> ` → Task 2.
- Multi-paragraph paste scoping → Task 3.
- Per-tab `last_caret_para` + rename `nw_hide_range2` → Task 4.
- Deferred (task toggle → C2, perf → C3, `DECO_LINK.start` marker inclusion) → not in any task, matching the spec.

**Placeholder scan:** none — every code step carries complete code; every test step has real `CHECK` assertions + expected `make test` output.

**Type consistency:** `markdown_decorate`'s new `char** urlpool` param is defined in Task 1 (header + impl) and every call site is updated in the same task (`markdown_fmt_at`, `nw_restyle`, all `tests/test_markdown.c` blocks). `nw_apply_decos`'s new `const char* urlpool` param + its single call site are both in Task 1. `NoteTab` gains `last_lines` (Task 3) then `last_caret_para` (Task 4); Task 4 shows the full struct so the fields are consistent. `pool_add`/`a_url_off`/`a_url_len`/`a_is_autolink`/`quote_start` are defined and used within their tasks.

**Known limitations (carried):** `DECO_LINK.start` still includes hidden leading markers when link text starts with markup (benign hit-test); reference-link `][ref]` scan stops at line end (a ref label spanning lines would under-hide — not valid CommonMark). Blockquotes containing nested block structures (a list inside a quote) hide only the `>` prefix per line, which is correct for the prefix but does not special-case nested-block indentation.
