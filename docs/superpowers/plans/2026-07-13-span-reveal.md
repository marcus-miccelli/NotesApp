# Span-Level Reveal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Inline markdown markers (bold/italic/strike, code spans, links) reveal per-span — they re-hide the instant the caret escapes the span's extent, even within one line — while block markers keep today's paragraph reveal.

**Architecture:** The decoration builder (`src/markdown.c`) defers inline marker hides to span close, where the full extent `[open_marker_start, close_marker_end)` is known, and gates them on an inclusive extent-touch test instead of the paragraph test. Each inline-marker `DECO_HIDE` carries its extent in the (previously unused for HIDE) aux fields, so the C3a hide-all cache can answer "which spans does the caret touch" without a parse; `note_window.c` compares that signature on every `EN_SELCHANGE` and re-styles only when it changes.

**Tech Stack:** C11, md4c, RichEdit, existing test harness (`tests/test_markdown.c`, `make test`).

Spec: `docs/superpowers/specs/2026-07-13-span-reveal-design.md`

## Global Constraints

- Markdown knowledge stays in `src/markdown.c`; `src/note_window.c` is RichEdit mechanics only. Offsets are RichEdit `\r`/CP_ACP char offsets.
- Scope: **inline spans only** (STRONG/EM/DEL/CODE spans + links). Block markers (heading `#`, list/task prefixes, blockquote `>`, code fences) keep the paragraph-level `in_reveal` gate — do not touch their call sites.
- Boundary rule: a span is revealed while the reveal window `[sel_lo, sel_hi]` touches its extent `[ext_s, ext_e]` **boundary-inclusive** (`sel_lo <= ext_e && sel_hi >= ext_s`). The hide-all sentinel (`sel_lo > sel_hi`) still hides everything.
- One touch predicate shared by the builder gate and `markdown_reveal_sig` — they must agree exactly or the UI flickers at span edges.
- `markdown_reveal_sig` considers only `kind == DECO_HIDE && aux_len > 0` (DECO_LINK's `aux_start` is a url-*pool* offset, DECO_TASK's a mark offset — misreading either as a source extent gives spurious touches). Fold is **summed** (`aux_start*2654435761u + aux_len*40503u`), not XORed — open+close markers share one extent and XOR would cancel the pair.
- The close-time opening-marker push is guarded by `sp[i].start_set` (a textless span records no opening marker; never emit a bogus offset-0 hide).
- Existing suite must stay green; new checks added on top of the current 150.
- Working branch: `feature/span-reveal` (create from `main` before Task 1's first commit).

---

### Task 1: Extent-gated inline hides + `markdown_reveal_sig` (builder + tests)

**Files:**
- Modify: `src/markdown.h` (Deco aux comments; `markdown_reveal_sig` decl)
- Modify: `src/markdown.c` (DCtx span stack + link fields; new `md_touch`/`push_hide_span`; defer pushes in `d_text` resolve loop + link-open block; push at `d_leave_span`; new `markdown_reveal_sig`)
- Test: `tests/test_markdown.c`

**Interfaces:**
- Consumes: existing `markdown_decorate(text, len, sel_lo, sel_hi, &decos, &pool)`; `Deco` struct; existing test harness macros (open `tests/test_markdown.c`, follow its existing CHECK/section conventions and register the new test function like its neighbors).
- Produces: inline-marker `DECO_HIDE` entries carrying `aux_start` = extent start, `aux_len` = extent length (block hides keep `aux_len == 0`); `size_t markdown_reveal_sig(const Deco* d, size_t n, size_t lo, size_t hi)`. Task 2 relies on exactly that signature.

- [ ] **Step 1: Create branch**

```bash
git checkout -b feature/span-reveal main
```

- [ ] **Step 2: Write the failing tests**

Add to `tests/test_markdown.c`, following the file's existing helper/CHECK conventions (there are existing helpers that scan a `Deco*` list — reuse them; if none matches "has a DECO_HIDE at (start,len)", add a small static helper). Test content (adapt macro names to the harness, keep offsets exactly):

```c
/* --- span-level reveal: inline marker hides gate on span extent --- */
static int find_hide(const Deco* d, size_t n, size_t start, size_t len,
                     size_t* aux_s, size_t* aux_l) {
    for (size_t i = 0; i < n; i++)
        if (d[i].kind == DECO_HIDE && d[i].start == start && d[i].len == len) {
            if (aux_s) *aux_s = d[i].aux_start;
            if (aux_l) *aux_l = d[i].aux_len;
            return 1;
        }
    return 0;
}

static void test_span_reveal(void) {
    Deco* d; char* pool; size_t n;

    /* "a **b** c" — extent [2,7): open "**"@2, close "**"@5 */
    const char* s1 = "a **b** c";
    /* caret 0: same paragraph but outside the span -> markers hidden
     * (this is the span-vs-paragraph proof: old code revealed them) */
    n = markdown_decorate(s1, 9, 0, 0, &d, &pool);
    size_t as, al;
    CHECK(find_hide(d, n, 2, 2, &as, &al)); CHECK(as == 2 && al == 5);
    CHECK(find_hide(d, n, 5, 2, &as, &al)); CHECK(as == 2 && al == 5);
    free(d); free(pool);
    /* caret 2 == extent start (touching) -> revealed (no hides) */
    n = markdown_decorate(s1, 9, 2, 2, &d, &pool);
    CHECK(!find_hide(d, n, 2, 2, NULL, NULL));
    CHECK(!find_hide(d, n, 5, 2, NULL, NULL));
    free(d); free(pool);
    /* caret 7 == extent end (touching) -> revealed */
    n = markdown_decorate(s1, 9, 7, 7, &d, &pool);
    CHECK(!find_hide(d, n, 2, 2, NULL, NULL));
    free(d); free(pool);
    /* caret 8 (one past extent end) -> hidden again */
    n = markdown_decorate(s1, 9, 8, 8, &d, &pool);
    CHECK(find_hide(d, n, 2, 2, NULL, NULL));
    free(d); free(pool);

    /* nested "**a *b* c**" — outer [0,11), inner [4,7) */
    const char* s2 = "**a *b* c**";
    /* caret 5 (inside inner): all four marker hides revealed */
    n = markdown_decorate(s2, 11, 5, 5, &d, &pool);
    CHECK(!find_hide(d, n, 0, 2, NULL, NULL));
    CHECK(!find_hide(d, n, 4, 1, NULL, NULL));
    CHECK(!find_hide(d, n, 6, 1, NULL, NULL));
    CHECK(!find_hide(d, n, 9, 2, NULL, NULL));
    free(d); free(pool);
    /* caret 8 (outer only): inner hides back, outer still revealed */
    n = markdown_decorate(s2, 11, 8, 8, &d, &pool);
    CHECK(find_hide(d, n, 4, 1, &as, &al)); CHECK(as == 4 && al == 3);
    CHECK(find_hide(d, n, 6, 1, NULL, NULL));
    CHECK(!find_hide(d, n, 0, 2, NULL, NULL));
    CHECK(!find_hide(d, n, 9, 2, NULL, NULL));
    free(d); free(pool);

    /* code span "a `b` c" — extent [2,5) */
    const char* s3 = "a `b` c";
    n = markdown_decorate(s3, 7, 3, 3, &d, &pool);   /* caret in span */
    CHECK(!find_hide(d, n, 2, 1, NULL, NULL));
    CHECK(!find_hide(d, n, 4, 1, NULL, NULL));
    free(d); free(pool);
    n = markdown_decorate(s3, 7, 0, 0, &d, &pool);   /* caret outside */
    CHECK(find_hide(d, n, 2, 1, &as, &al)); CHECK(as == 2 && al == 3);
    free(d); free(pool);

    /* link "x [t](u)" — '['@2, "](u)"@4 len4, extent [2,8) */
    const char* s4 = "x [t](u)";
    n = markdown_decorate(s4, 8, 0, 0, &d, &pool);   /* caret outside */
    CHECK(find_hide(d, n, 2, 1, &as, &al)); CHECK(as == 2 && al == 6);
    CHECK(find_hide(d, n, 4, 4, &as, &al)); CHECK(as == 2 && al == 6);
    free(d); free(pool);
    n = markdown_decorate(s4, 8, 6, 6, &d, &pool);   /* caret in url */
    CHECK(!find_hide(d, n, 2, 1, NULL, NULL));
    CHECK(!find_hide(d, n, 4, 4, NULL, NULL));
    free(d); free(pool);

    /* autolink "<http://x>" — '<'@0, '>'@9, extent [0,10) */
    const char* s5 = "<http://x>";
    n = markdown_decorate(s5, 10, 4, 4, &d, &pool);  /* caret inside */
    CHECK(!find_hide(d, n, 0, 1, NULL, NULL));
    CHECK(!find_hide(d, n, 9, 1, NULL, NULL));
    free(d); free(pool);

    /* block markers keep PARAGRAPH reveal: "# h\r\nx" — caret on the
     * heading line reveals '#', caret on the other line hides it */
    const char* s6 = "# h\r\nx";
    n = markdown_decorate(s6, 6, 1, 1, &d, &pool);
    CHECK(!find_hide(d, n, 0, 2, NULL, NULL));
    free(d); free(pool);
    n = markdown_decorate(s6, 6, 5, 5, &d, &pool);
    CHECK(find_hide(d, n, 0, 2, &as, &al)); CHECK(al == 0);  /* block: no extent */
    free(d); free(pool);

    /* textless-span guard: bare "``" must not emit a bogus hide */
    n = markdown_decorate("``", 2, (size_t)-1, 0, &d, &pool);
    for (size_t i = 0; i < n; i++) CHECK(d[i].kind != DECO_HIDE || d[i].start < 2);
    free(d); free(pool);
}

static void test_reveal_sig(void) {
    Deco* d; char* pool; size_t n;

    const char* s1 = "a **b** c";                    /* extent [2,7) */
    n = markdown_decorate(s1, 9, (size_t)-1, 0, &d, &pool);   /* hide-all */
    CHECK(markdown_reveal_sig(d, n, 0, 0) == 0);               /* plain text */
    size_t in1 = markdown_reveal_sig(d, n, 4, 4);
    CHECK(in1 != 0);
    CHECK(markdown_reveal_sig(d, n, 2, 2) == in1);             /* edges equal */
    CHECK(markdown_reveal_sig(d, n, 7, 7) == in1);
    CHECK(markdown_reveal_sig(d, n, 8, 8) == 0);               /* escaped */
    CHECK(markdown_reveal_sig(d, n, 0, 4) == in1);             /* selection overlap */
    free(d); free(pool);

    const char* s2 = "**a *b* c**";                  /* outer [0,11) inner [4,7) */
    n = markdown_decorate(s2, 11, (size_t)-1, 0, &d, &pool);
    size_t inner = markdown_reveal_sig(d, n, 5, 5);
    size_t outer = markdown_reveal_sig(d, n, 8, 8);
    CHECK(inner != 0 && outer != 0 && inner != outer);
    free(d); free(pool);

    /* DECO_LINK aux is a POOL offset — must be ignored by the sig.
     * "x [t](u)": link deco has aux_start==0; a misread extent [0,~1]
     * would touch caret 0. Assert sig at 0 is exactly 0. */
    const char* s4 = "x [t](u)";
    n = markdown_decorate(s4, 8, (size_t)-1, 0, &d, &pool);
    CHECK(markdown_reveal_sig(d, n, 0, 0) == 0);
    CHECK(markdown_reveal_sig(d, n, 5, 5) != 0);
    free(d); free(pool);

    /* DECO_TASK aux ignored: "- [ ] a" */
    const char* s7 = "- [ ] a";
    n = markdown_decorate(s7, 7, (size_t)-1, 0, &d, &pool);
    CHECK(markdown_reveal_sig(d, n, 6, 6) == 0);
    free(d); free(pool);
}
```

Register both functions in the file's test runner list exactly like the neighboring tests.

- [ ] **Step 3: Run tests to verify they fail**

Run: `make test`
Expected: FAIL — `markdown_reveal_sig` undeclared (compile error). That is the red state; do not stub it to make compilation pass without the real implementation.

- [ ] **Step 4: Implement in `src/markdown.h`**

Update the aux-field comments and add the declaration:

```c
    size_t   aux_start; /* DECO_LINK: url offset into the parse's url pool;
                         * DECO_HIDE (inline markers): span extent start;
                         * DECO_TASK: source offset of the mark char */
    size_t   aux_len;   /* DECO_LINK: url length in the pool;
                         * DECO_HIDE (inline markers): span extent length
                         * (0 on block-level hides) */
```

```c
/* Signature of the set of inline span extents the reveal window [lo, hi]
 * touches (boundary-inclusive; caret = lo == hi). 0 when none. Computed
 * over a hide-all decoration list (extents ride on DECO_HIDE aux fields).
 * Equal windows-touch-sets give equal signatures. */
size_t markdown_reveal_sig(const Deco* d, size_t n, size_t lo, size_t hi);
```

- [ ] **Step 5: Implement in `src/markdown.c`**

5a. DCtx changes — span stack gains the recorded opening marker, link state gains the recorded opener:

```c
    /* inline span stack */
    struct { MdFmt fmt; int marklen; int start_set;
             size_t open_start; int open_len; } sp[64];
```

```c
    /* link state */
    int    in_a;
    int    a_text_set;
    size_t a_text_start;
    int    a_is_autolink;
    size_t a_url_off, a_url_len;   /* url span in the pool */
    size_t a_open_off;             /* recorded '['/'<' (pushed at leave) */
    size_t a_open_len;
    int    a_open_set;
```

In `d_enter_span`'s `MD_SPAN_A` branch add `c->a_open_set = 0;` beside `c->a_text_set = 0;`.

5b. New helpers directly below `push_hide` (keep `push_hide` itself unchanged — block markers still use it):

```c
/* Inclusive touch: caret offsets are between-char positions, so a window
 * [lo, hi] touches an extent [a, b] when lo <= b && hi >= a. Shared by the
 * builder gate below and markdown_reveal_sig — they must agree exactly. */
static int md_touch(size_t lo, size_t hi, size_t a, size_t b) {
    return lo <= b && hi >= a;
}

/* Hide an inline span marker: revealed only while the reveal window touches
 * the span's full extent [ext_s, ext_e] (span-level reveal, vs. the
 * paragraph-level push_hide). The extent rides on the deco's aux fields so
 * cached hide-all lists can answer reveal queries without a parse. */
static void push_hide_span(DCtx* c, size_t start, size_t len,
                           size_t ext_s, size_t ext_e) {
    if (len == 0) return;
    if (c->sel_lo <= c->sel_hi && md_touch(c->sel_lo, c->sel_hi, ext_s, ext_e))
        return;                            /* caret touches span: show markers */
    size_t before = c->count;
    push(c, DECO_HIDE, start, len, 0, PARA_NONE, 0);
    if (c->count > before) {
        c->arr[c->count - 1].aux_start = ext_s;
        c->arr[c->count - 1].aux_len   = ext_e - ext_s;
    }
}
```

5c. `d_text` resolve loop (currently pushes the opening hide) — record instead:

```c
    /* resolve opening markers for spans opened since the last text run,
     * innermost first, consuming delimiter chars leftward from off. The
     * hide itself is pushed at d_leave_span, where the extent is known. */
    size_t cursor = off;
    for (int i = c->depth - 1; i >= 0 && !c->sp[i].start_set; i--) {
        int ml = c->sp[i].marklen;
        if (ml < 0) {                         /* code: count backticks left */
            size_t j = cursor; int k = 0;
            while (j > 0 && c->base[j-1] == '`') { j--; k++; }
            ml = k; c->sp[i].marklen = ml;
        }
        if ((size_t)ml > cursor) ml = (int)cursor;   /* clamp: never underflow */
        c->sp[i].open_start = cursor - (size_t)ml;
        c->sp[i].open_len   = ml;
        cursor -= (size_t)ml;
        c->sp[i].start_set = 1;
    }
```

5d. `d_text` link-open block — record instead of push:

```c
    if (c->in_a && !c->a_text_set) {
        c->a_text_set = 1;
        c->a_text_start = off;
        if (c->a_is_autolink) {
            if (off > 0) { c->a_open_off = off - 1; c->a_open_len = 1;
                           c->a_open_set = 1; }         /* '<' */
        } else {
            size_t ab = off;                            /* scan left to the '[' */
            while (ab > 0 && c->base[ab-1] != '[') ab--;
            if (ab > 0) { c->a_open_off = ab - 1; c->a_open_len = 1;
                          c->a_open_set = 1; }          /* '[' */
        }
    }
```

5e. `d_leave_span` link branch — compute the close range as today but push both sides extent-gated (fall back to the legacy paragraph gate when no opener was recorded):

```c
    if (t == MD_SPAN_A) {
        size_t te = c->last_text_end > c->close_cursor ? c->last_text_end
                                                       : c->close_cursor;
        size_t close_e = te;
        if (c->a_is_autolink) {
            if (te < c->len && c->base[te] == '>') close_e = te + 1;  /* '>' */
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
            close_e = p;
        }
        if (c->a_open_set) {
            /* span-level reveal over the whole link syntax */
            size_t ext_s = c->a_open_off, ext_e = close_e;
            push_hide_span(c, c->a_open_off, c->a_open_len, ext_s, ext_e);
            if (close_e > te) push_hide_span(c, te, close_e - te, ext_s, ext_e);
        } else if (close_e > te) {
            push_hide(c, te, close_e - te);   /* no opener recorded: legacy gate */
        }
        if (c->a_text_set)
            push_link(c, c->a_text_start, te - c->a_text_start,
                      c->a_url_off, c->a_url_len);
        c->in_a = 0;
        return 0;
    }
```

5f. `d_leave_span` emphasis/code tail — push both markers extent-gated:

```c
    if (!span_fmt(t)) return 0;
    if (c->over > 0) { c->over--; return 0; }       /* unwind a dropped open */
    if (c->depth <= 0) return 0;
    c->depth--;
    int ml = c->sp[c->depth].marklen; if (ml < 0) ml = 0;
    size_t base = c->last_text_end > c->close_cursor ? c->last_text_end
                                                     : c->close_cursor;
    /* full span extent incl. both markers; start_set guards a textless span
     * (no opening marker recorded -> never emit a bogus hide) */
    size_t ext_s = c->sp[c->depth].start_set ? c->sp[c->depth].open_start : base;
    size_t ext_e = base + (size_t)ml;
    if (c->sp[c->depth].start_set && c->sp[c->depth].open_len > 0)
        push_hide_span(c, c->sp[c->depth].open_start,
                       (size_t)c->sp[c->depth].open_len, ext_s, ext_e);
    if (ml > 0) push_hide_span(c, base, (size_t)ml, ext_s, ext_e);
    c->close_cursor = base + (size_t)ml;
    return 0;
```

5g. `markdown_reveal_sig` — add near the other `*_from_decos` queries:

```c
size_t markdown_reveal_sig(const Deco* d, size_t n, size_t lo, size_t hi) {
    size_t sig = 0;
    for (size_t i = 0; i < n; i++) {
        if (d[i].kind != DECO_HIDE || d[i].aux_len == 0) continue;
        if (md_touch(lo, hi, d[i].aux_start, d[i].aux_start + d[i].aux_len))
            /* summed, not XORed: a span's open+close hides share one extent
             * and XOR would cancel the pair to 0 */
            sig += d[i].aux_start * 2654435761u + d[i].aux_len * 40503u;
    }
    return sig;
}
```

- [ ] **Step 6: Run tests to verify they pass**

Run: `make test`
Expected: PASS, zero failures, no compiler warnings. Existing checks must all still pass — if a pre-existing reveal test now fails because it asserted paragraph-reveal of an *inline* marker, that assertion is the old behavior this feature replaces: update that test to the span rule and say so in the commit message.

- [ ] **Step 7: Commit**

```bash
git add src/markdown.h src/markdown.c tests/test_markdown.c
git commit -m "feat(md): span-level reveal for inline markers + markdown_reveal_sig"
```

### Task 2: Restyle trigger on reveal-set change

**Files:**
- Modify: `src/note_window.c` (NoteTab field ~line 94; EN_SELCHANGE handler ~line 1616; tab-init points)

**Interfaces:**
- Consumes: `size_t markdown_reveal_sig(const Deco* d, size_t n, size_t lo, size_t hi)` from Task 1; existing `nw_decos(nw, tab, &n, &pool)` cache accessor; existing `nw_restyle(nw, tab, 0)` path.
- Produces: user-visible behavior only.

- [ ] **Step 1: Add the per-tab field**

In the `NoteTab` struct (beside `last_caret_para`):

```c
    size_t last_caret_para;   /* paragraph offset last revealed (per tab) */
    size_t last_reveal_sig;   /* inline-span reveal signature at last restyle */
```

- [ ] **Step 2: Initialize it wherever `last_caret_para`/cache fields are initialized**

Find every place `last_caret_para` or the C3a cache fields are reset for a (re)used tab slot — `nw_add_tab`, the `WM_CREATE` per-tab init loop, `nw_load_tab`, and the vacated-slot reset in `nw_remove_tab` — and add `last_reveal_sig = 0;` alongside, matching the surrounding style. (Grep: `last_caret_para` and `cache_dirty` mark all of them.)

- [ ] **Step 3: Extend the EN_SELCHANGE trigger**

Current handler (src/note_window.c:1616-1633) restyles only on paragraph change. Replace the inner block with:

```c
            {
                HWND e = nw_edit(nw);
                CHARRANGE sel; SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
                int len; char* b = nw_body_text(e, &len);
                if (b) {
                    size_t lo = (size_t)sel.cpMin, hi = lo;
                    nw_para_bounds(b, len, &lo, &hi);
                    free(b);
                    /* inline-span reveal: restyle when the set of spans the
                     * selection touches changes, not only on paragraph change */
                    size_t dn; const char* dp;
                    const Deco* dd = nw_decos(nw, nw->active, &dn, &dp);
                    size_t sig = dd ? markdown_reveal_sig(dd, dn,
                                          (size_t)sel.cpMin, (size_t)sel.cpMax)
                                    : 0;
                    NoteTab* t = &nw->tab[nw->active];
                    if (lo != t->last_caret_para || sig != t->last_reveal_sig) {
                        t->last_caret_para = lo;
                        t->last_reveal_sig = sig;
                        SendMessageW(e, WM_SETREDRAW, FALSE, 0);
                        nw_restyle(nw, nw->active, 0);   /* whole doc: flip reveal */
                        SendMessageW(e, WM_SETREDRAW, TRUE, 0);
                        InvalidateRect(e, NULL, TRUE);
                    }
                }
            }
```

Note: `nw_decos` may reparse if the cache is dirty — that is the existing C3a contract, and this handler already touches the cache via `nw_update_toggles` below it, so no new parse is introduced. A stale sig immediately after a text edit (EN_SELCHANGE arriving before EN_CHANGE) is accepted per the spec — bounded and self-healing.

- [ ] **Step 4: Build + regression gate**

Run: `make && make test`
Expected: clean compile, no warnings; all checks pass (150 pre-existing + Task 1's new ones).

- [ ] **Step 5: Commit**

```bash
git add src/note_window.c
git commit -m "feat(md): restyle on inline-span reveal-set change (EN_SELCHANGE sig compare)"
```

## Human verification checklist (post-merge, GUI rule)

1. Type `a **bold** c` on one line. Arrow the caret out of the span **without leaving the line** → `**` collapse instantly; arrow back to either edge → they reappear.
2. Nested `**a *b* c**`: caret in `*b*` shows all markers; caret on `c` shows only outer's.
3. Link `[text](url)`: caret inside url keeps full syntax visible; caret outside collapses to styled text.
4. Heading `#` still reveals for the caret anywhere on its line (unchanged).
5. Arrow-keying through plain text: no flicker, no lag.
