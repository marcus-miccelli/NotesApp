# Document Parse Cache (C3a) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Serve the two hot per-event markdown queries (sidebar-toggle `fmt_at` on every caret move, checkbox `task_at` on every mouse-move) from a per-tab cached parse instead of re-parsing the whole document each time.

**Architecture:** Add a `DECO_TASK` decoration and two pure query helpers (`markdown_fmt_from_decos`, `markdown_task_from_decos`) to `src/markdown.c` so one cached (selection-independent) decoration list answers both queries. Add a per-tab cache in `src/note_window.c` (`nw_decos` accessor, invalidated on edit), and rewire the two consumers onto it.

**Tech Stack:** C11, MSYS2 mingw-w64 gcc, md4c (existing), Win32 RichEdit.

## Global Constraints

- Pure C, `-std=c11`, `-mwindows`, gcc 13.2 (MSYS2 mingw-w64). No C++. No new deps.
- Markdown knowledge stays in `src/markdown.c`; `note_window.c` = RichEdit mechanics only.
- Offsets are RichEdit `\r`/CP_ACP char offsets (`nw_body_text` reads `EM_GETTEXTEX GT_DEFAULT + CP_ACP`; cached decos index that same buffer).
- The cache stores the **hide-all** (`sel_lo > sel_hi`, i.e. `(size_t)-1, 0`) parse — selection-independent, which is exactly what both hot queries need.
- `DECO_TASK` is emitted with an **unconditional `push`** (never `push_hide` / never reveal-gated), range `[mark_off-1, mark_off+2)` (the `[ ]`).
- First-parse safety is the `cache_decos == NULL` branch of `nw_decos`, NOT a dirty default (`NoteWin` is `calloc`'d → `cache_dirty` starts 0).
- Build gate: `make` clean with **no new warnings** (a defined-but-unused static function is a warning — every task keeps its new statics used); `make test` green.
- Testing: the builder helpers are pure logic → real TDD via `tests/test_markdown.c` (`CHECK` from `tests/test.h`). The cache + rewiring is a behavior-preserving refactor → gate is build clean + `make test` green; no-visible-change confirmed by the human (toggle still works, hand cursor still shows, sidebar toggles still track the caret).

---

### Task 1: `DECO_TASK` + `*_from_decos` query helpers

**Files:**
- Modify: `src/markdown.h`, `src/markdown.c`
- Test: `tests/test_markdown.c`

**Interfaces:**
- Produces:
  - `DecoKind` gains `DECO_TASK` (checkbox: `start` = `[`, `len` = 3, `aux_start` = mark offset, `number` = checked 0/1).
  - `MdFmt markdown_fmt_from_decos(const Deco* d, size_t n, size_t caret);`
  - `int markdown_task_from_decos(const Deco* d, size_t n, size_t off, size_t* mark_off, int* checked);`
  - `markdown_fmt_at` / `markdown_task_at` keep their signatures but are reimplemented on top of the helpers (one parse shape; `markdown_task_at`'s separate parser is removed).

- [ ] **Step 1: Add `DECO_TASK` + the two prototypes to the header, and write failing tests**

In `src/markdown.h`, change the `DecoKind` enum:
```c
typedef enum { DECO_FMT, DECO_HIDE, DECO_PARA, DECO_LINK, DECO_TASK } DecoKind;
```
and add, after the `markdown_task_at` prototype:
```c
/* Pure queries over an existing decoration list (from markdown_decorate). */
MdFmt markdown_fmt_from_decos(const Deco* d, size_t n, size_t caret);
int   markdown_task_from_decos(const Deco* d, size_t n, size_t off,
                               size_t* mark_off, int* checked);
```

Append inside `test_markdown(void)` in `tests/test_markdown.c`:
```c
    /* --- DECO_TASK emission + from_decos queries --- */
    {
        const char* t = "- [ ] a\n- [x] b";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        /* first item: mark at 3, checkbox range [2,5); second: mark at 11 */
        int t0 = 0, t1 = 0;
        for (size_t i = 0; i < n; i++) if (d[i].kind == DECO_TASK) {
            if (d[i].start == 2 && d[i].len == 3 && d[i].aux_start == 3 && d[i].number == 0) t0 = 1;
            if (d[i].aux_start == 11 && d[i].number == 1) t1 = 1;
        }
        CHECK(t0 == 1);   /* "- [ ]" -> unchecked checkbox at [2,5) */
        CHECK(t1 == 1);   /* "- [x]" -> checked */

        /* markdown_task_from_decos over the same list */
        size_t mo = 0; int ck = 9;
        CHECK(markdown_task_from_decos(d, n, 2, &mo, &ck) == 1);  /* on '[' */
        CHECK(mo == 3 && ck == 0);
        CHECK(markdown_task_from_decos(d, n, 3, &mo, &ck) == 1);  /* on mark */
        CHECK(markdown_task_from_decos(d, n, 4, &mo, &ck) == 1);  /* on ']' */
        CHECK(markdown_task_from_decos(d, n, 6, &mo, &ck) == 0);  /* in text */

        /* markdown_fmt_from_decos: the checked item's content is struck */
        CHECK((markdown_fmt_from_decos(d, n, 13) & MD_FMT_STRIKE) != 0); /* inside "b" */
        free(d); free(pool);
    }
```

- [ ] **Step 2: Run tests — verify they fail**

Run: `make test`
Expected: FAIL — `DECO_TASK` / `markdown_task_from_decos` / `markdown_fmt_from_decos` undefined, and no `DECO_TASK` emitted yet (`t0 == 0`).

- [ ] **Step 3: Emit `DECO_TASK` in the builder**

In `src/markdown.c`, in `d_text`'s task branch, add the `DECO_TASK` push. Replace:
```c
        if (c->li_is_task) {
            /* keep "[x]" visible: hide "- " before it and the run after it up to
             * the content; the mark char sits at li_task_mark_off (between [ ]). */
            size_t cb0 = c->li_task_mark_off > 0 ? c->li_task_mark_off - 1 : 0; /* '[' */
            size_t cb1 = c->li_task_mark_off + 2;                               /* past ']' */
            push_hide(c, ls, cb0 - ls);        /* "- " */
            if (off > cb1) push_hide(c, cb1, off - cb1);   /* space(s) after "]" */
        } else {
```
with:
```c
        if (c->li_is_task) {
            /* keep "[x]" visible: hide "- " before it and the run after it up to
             * the content; the mark char sits at li_task_mark_off (between [ ]). */
            size_t cb0 = c->li_task_mark_off > 0 ? c->li_task_mark_off - 1 : 0; /* '[' */
            size_t cb1 = c->li_task_mark_off + 2;                               /* past ']' */
            push_hide(c, ls, cb0 - ls);        /* "- " */
            if (off > cb1) push_hide(c, cb1, off - cb1);   /* space(s) after "]" */
            /* checkbox range [cb0, cb1) as a DECO_TASK: aux_start=mark, number=checked.
             * Unconditional push (never reveal-gated) so task hit-test always works.
             * push may drop on OOM; overwrite the new deco only if it landed. */
            push(c, DECO_TASK, cb0, cb1 - cb0, 0, PARA_NONE, 0);
            if (c->count) {
                Deco* td = &c->arr[c->count - 1];
                td->aux_start = c->li_task_mark_off;
                td->number = c->li_task_checked ? 1 : 0;
            }
        } else {
```
(`push` zeroes `aux_start`/`aux_len`/`number` from its args, exactly as `push_link` does before overwriting — here we overwrite `aux_start` + `number` on the just-pushed deco.)

- [ ] **Step 4: Add the two query helpers + reimplement `fmt_at`/`task_at`**

In `src/markdown.c`, replace the existing `markdown_fmt_at` function AND the `TaskCtx`/`tq_*`/`markdown_task_at` block (everything from `MdFmt markdown_fmt_at(...)` through the end of `markdown_task_at`) with:
```c
MdFmt markdown_fmt_from_decos(const Deco* d, size_t n, size_t caret) {
    MdFmt f = 0;
    for (size_t i = 0; i < n; i++) {
        if (d[i].kind != DECO_FMT) continue;
        size_t a = d[i].start, b = a + d[i].len;
        /* cover the char on either side of the caret so a boundary still lights */
        if ((caret > a && caret <= b) || (caret >= a && caret < b))
            f |= d[i].fmt;
    }
    f &= ~(MdFmt)(MD_FMT_H1 | MD_FMT_H2 | MD_FMT_H3);   /* headings not toggles */
    return f;
}

int markdown_task_from_decos(const Deco* d, size_t n, size_t off,
                             size_t* mark_off, int* checked) {
    for (size_t i = 0; i < n; i++) {
        if (d[i].kind != DECO_TASK) continue;
        size_t a = d[i].start, b = a + d[i].len;
        if (off >= a && off < b) {
            if (mark_off) *mark_off = d[i].aux_start;
            if (checked)  *checked  = d[i].number;
            return 1;
        }
    }
    return 0;
}

MdFmt markdown_fmt_at(const char* text, size_t len, size_t caret) {
    Deco* d = NULL; char* pool = NULL;
    size_t n = markdown_decorate(text, len, (size_t)-1, 0, &d, &pool);
    MdFmt f = markdown_fmt_from_decos(d, n, caret);
    free(d); free(pool);
    return f;
}

int markdown_task_at(const char* text, size_t len, size_t off,
                     size_t* mark_off, int* checked) {
    Deco* d = NULL; char* pool = NULL;
    size_t n = markdown_decorate(text, len, (size_t)-1, 0, &d, &pool);
    int hit = markdown_task_from_decos(d, n, off, mark_off, checked);
    free(d); free(pool);
    return hit;
}
```

- [ ] **Step 5: Run tests — verify pass**

Run: `make test`
Expected: PASS, `0 failed` (new DECO_TASK/from_decos checks + the existing `markdown_fmt_at`/`markdown_task_at` tests, now routed through the helpers).

Run: `make clean && make`
Expected: links clean, no new warnings (the `TaskCtx`/`tq_*` statics are gone; nothing left unused).

- [ ] **Step 6: Commit**

```bash
git add src/markdown.h src/markdown.c tests/test_markdown.c
git commit -m "feat(md): DECO_TASK + from_decos queries; route fmt_at/task_at through them"
```

---

### Task 2: Per-tab parse cache + lifecycle + invalidation + fmt_at rewire

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: `markdown_decorate`, `markdown_fmt_from_decos` (Task 1), `nw_body_text`.
- Produces: `NoteTab` cache fields; `nw_cache_free(NoteTab*)`; `static const Deco* nw_decos(NoteWin*, int tab, size_t* n, const char** pool)`. `nw_update_toggles` reads the cache (no parse). Cache freed/nulled/inited across the full tab lifecycle.

- [ ] **Step 1: Add cache fields to `NoteTab`**

In `src/note_window.c`, extend the `NoteTab` struct:
```c
typedef struct {
    char id[16];      /* note id */
    HWND edit;        /* this tab's body RichEdit */
    int  last_lines;  /* line count at last restyle; a change forces whole-doc */
    size_t last_caret_para;   /* paragraph offset last revealed (per tab) */
    Deco*  cache_decos;   /* cached hide-all decoration list (NULL until built) */
    size_t cache_n;
    char*  cache_pool;    /* url pool backing cached DECO_LINK aux (unused by C3a readers; freed each reparse) */
    int    cache_dirty;   /* 1 => text changed since the cache was built */
} NoteTab;
```

- [ ] **Step 2: Add `nw_cache_free` + `nw_decos` (before `nw_update_toggles`)**

In `src/note_window.c`, immediately after `nw_body_text` (ends ~line 388/402) and before `nw_update_toggles`, add:
```c
/* Free a tab's cached parse (free(NULL) is safe). */
static void nw_cache_free(NoteTab* t) {
    free(t->cache_decos); t->cache_decos = NULL;
    free(t->cache_pool);  t->cache_pool = NULL;
    t->cache_n = 0;
}

/* Return the tab's cached sel-independent (hide-all) decoration list, reparsing
 * only when the tab's text changed since the last parse (or no cache yet).
 * *n / *pool receive the count / url pool (either may be NULL). */
static const Deco* nw_decos(NoteWin* nw, int tab, size_t* n, const char** pool) {
    NoteTab* t = &nw->tab[tab];
    if (t->cache_decos == NULL || t->cache_dirty) {
        int len; char* buf = nw_body_text(t->edit, &len);
        if (buf) {
            Deco* d = NULL; char* p = NULL;
            size_t cn = markdown_decorate(buf, (size_t)len, (size_t)-1, 0, &d, &p);
            free(buf);
            nw_cache_free(t);
            t->cache_decos = d; t->cache_n = cn; t->cache_pool = p;
            t->cache_dirty = 0;
        }
        /* buf NULL (OOM): keep whatever we had (may be NULL) */
    }
    if (n)    *n    = t->cache_n;
    if (pool) *pool = t->cache_pool;
    return t->cache_decos;
}
```

- [ ] **Step 3: Rewire `nw_update_toggles` onto the cache**

In `nw_update_toggles`, replace the `buf`-reading `markdown_fmt_at` block. Replace:
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
with:
```c
    CHARRANGE sel; SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
    size_t dn; const Deco* dd = nw_decos(nw, nw->active, &dn, NULL);
    if (dd) {
        MdFmt f = markdown_fmt_from_decos(dd, dn, (size_t)sel.cpMin);
        s[0] = (f & MD_FMT_BOLD)   != 0;
        s[1] = (f & MD_FMT_ITALIC) != 0;
        s[2] = (f & MD_FMT_STRIKE) != 0;
    }
```

- [ ] **Step 4: Invalidate on edit**

**`nw_reformat_now`** — set the active tab dirty at the TOP (before its own `nw_update_toggles`, which reads the cache). Replace:
```c
static void nw_reformat_now(NoteWin* nw) {
    HWND e = nw_edit(nw);
```
with:
```c
static void nw_reformat_now(NoteWin* nw) {
    nw->tab[nw->active].cache_dirty = 1;
    HWND e = nw_edit(nw);
```

**`EN_CHANGE` handler** (user typing / paste / cut / undo). Replace:
```c
        if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == ID_EDIT) {
            SetTimer(hwnd, IDT_DEBOUNCE, DEBOUNCE_MS, NULL);  /* coalesces */
        }
```
with:
```c
        if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == ID_EDIT) {
            nw->tab[nw->active].cache_dirty = 1;
            SetTimer(hwnd, IDT_DEBOUNCE, DEBOUNCE_MS, NULL);  /* coalesces */
        }
```

**`nw_load_tab`** — mark the loaded tab dirty. At the very end of `nw_load_tab` (after the `SetWindowTextA` / free), add:
```c
    nw->tab[i].cache_dirty = 1;
```
(If `nw_load_tab` has multiple returns, add it right before each `return` that follows a successful content set, or restructure so it runs on the content-set path. The simplest: add it as the final statement of the function body.)

- [ ] **Step 5: Cache lifecycle — free / null / init across tab reuse**

**`nw_close_tab`** — free before destroying the edit. Replace:
```c
    nw_save_tab(nw, i);
    if (nw->tab[i].edit) DestroyWindow(nw->tab[i].edit);
```
with:
```c
    nw_save_tab(nw, i);
    nw_cache_free(&nw->tab[i]);
    if (nw->tab[i].edit) DestroyWindow(nw->tab[i].edit);
```

**`nw_delete_note`** — same. Replace:
```c
    if (nw->tab[i].edit) DestroyWindow(nw->tab[i].edit);
    nw->tab[i].edit = NULL;
    app_delete_note(app, id);
```
with:
```c
    nw_cache_free(&nw->tab[i]);
    if (nw->tab[i].edit) DestroyWindow(nw->tab[i].edit);
    nw->tab[i].edit = NULL;
    app_delete_note(app, id);
```

**`nw_remove_tab`** — after the shift, null the vacated top slot (the struct copy duplicated its cache pointer into the live slot below it). Replace:
```c
    for (int k = i; k < nw->ntabs - 1; k++) nw->tab[k] = nw->tab[k+1];
    nw->ntabs--;
```
with:
```c
    for (int k = i; k < nw->ntabs - 1; k++) nw->tab[k] = nw->tab[k+1];
    /* the struct copy duplicated the top slot's cache pointer into the slot
     * below; null the vacated top slot so it is not double-freed or reused */
    nw->tab[nw->ntabs - 1].cache_decos = NULL;
    nw->tab[nw->ntabs - 1].cache_pool  = NULL;
    nw->tab[nw->ntabs - 1].cache_n     = 0;
    nw->tab[nw->ntabs - 1].cache_dirty = 1;
    nw->ntabs--;
```

**`nw_add_tab`** — init the reused slot's cache. Replace:
```c
    int i = nw->ntabs++;
    snprintf(nw->tab[i].id, sizeof nw->tab[i].id, "%s", m->id);
```
with:
```c
    int i = nw->ntabs++;
    nw->tab[i].cache_decos = NULL; nw->tab[i].cache_pool = NULL;
    nw->tab[i].cache_n = 0; nw->tab[i].cache_dirty = 1;
    snprintf(nw->tab[i].id, sizeof nw->tab[i].id, "%s", m->id);
```

**`WM_CREATE` per-tab init loop** — init each slot's cache at the top of the loop body. Replace:
```c
        for (int i = 0; i < nw->ntabs; i++) {
            snprintf(nw->tab[i].id, sizeof nw->tab[i].id, "%s", w->tabs[i]);
```
with:
```c
        for (int i = 0; i < nw->ntabs; i++) {
            nw->tab[i].cache_decos = NULL; nw->tab[i].cache_pool = NULL;
            nw->tab[i].cache_n = 0; nw->tab[i].cache_dirty = 1;
            snprintf(nw->tab[i].id, sizeof nw->tab[i].id, "%s", w->tabs[i]);
```

**`WM_DESTROY`** — free every tab's cache before `free(nw)`. Replace:
```c
        if (nw) {
            if (nw->gfx) { gfx_destroy(nw->gfx); nw->gfx = NULL; }
            if (nw->rename_font) { DeleteObject(nw->rename_font); nw->rename_font = NULL; }
            free(nw);
```
with:
```c
        if (nw) {
            for (int i = 0; i < nw->ntabs; i++) nw_cache_free(&nw->tab[i]);
            if (nw->gfx) { gfx_destroy(nw->gfx); nw->gfx = NULL; }
            if (nw->rename_font) { DeleteObject(nw->rename_font); nw->rename_font = NULL; }
            free(nw);
```

- [ ] **Step 6: Build + test**

Run: `make clean && make`
Expected: links clean, no new warnings (`nw_decos`/`nw_cache_free` are now used by `nw_update_toggles` + the lifecycle sites).
Run: `make test`
Expected: PASS, `0 failed` (logic unchanged).
GUI (deferred to human): sidebar B/I/S toggles still light correctly as the caret moves through bold/italic/strike text.

- [ ] **Step 7: Commit**

```bash
git add src/note_window.c
git commit -m "feat(md): per-tab parse cache with lifecycle; serve sidebar toggles from it"
```

---

### Task 3: Rewire the task-checkbox hit-test onto the cache

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: `nw_decos` (Task 2), `markdown_task_from_decos` (Task 1).
- Produces: `nw_tab_of_edit(NoteWin*, HWND)`; `nw_body_sub` does zero parsing (checkbox hit-test reads the cache). Removes the per-mouse-move whole-doc reparse (the C2-deferred perf item).

- [ ] **Step 1: Add `nw_tab_of_edit` (before `nw_body_sub`)**

In `src/note_window.c`, just before `nw_body_sub`, add:
```c
/* Tab index whose body edit is `edit`, or -1. */
static int nw_tab_of_edit(NoteWin* nw, HWND edit) {
    for (int i = 0; i < nw->ntabs; i++)
        if (nw->tab[i].edit == edit) return i;
    return -1;
}
```

- [ ] **Step 2: Rewire both `nw_body_sub` arms onto the cache**

Replace the `WM_LBUTTONDOWN` and `WM_SETCURSOR` bodies in `nw_body_sub`. Replace:
```c
    if (msg == WM_LBUTTONDOWN) {
        POINTL pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int cp = (int)SendMessageW(h, EM_CHARFROMPOS, 0, (LPARAM)&pt);
        if (cp >= 0) {
            int len; char* buf = nw_body_text(h, &len);
            if (buf) {
                size_t mo; int ck;
                int hit = markdown_task_at(buf, (size_t)len, (size_t)cp, &mo, &ck);
                free(buf);
                if (hit) { nw_toggle_task(nw, h, mo, ck); return 0; }  /* consume */
            }
        }
    } else if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
        POINTL pt = { p.x, p.y };
        int cp = (int)SendMessageW(h, EM_CHARFROMPOS, 0, (LPARAM)&pt);
        if (cp >= 0) {
            int len; char* buf = nw_body_text(h, &len);
            if (buf) {
                size_t mo; int ck;
                int hit = markdown_task_at(buf, (size_t)len, (size_t)cp, &mo, &ck);
                free(buf);
                if (hit) { SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32649))); return TRUE; }  /* IDC_HAND */
            }
        }
    }
```
with:
```c
    if (msg == WM_LBUTTONDOWN) {
        POINTL pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int cp = (int)SendMessageW(h, EM_CHARFROMPOS, 0, (LPARAM)&pt);
        int tab = nw_tab_of_edit(nw, h);
        if (cp >= 0 && tab >= 0) {
            size_t dn; const Deco* dd = nw_decos(nw, tab, &dn, NULL);
            size_t mo; int ck;
            if (dd && markdown_task_from_decos(dd, dn, (size_t)cp, &mo, &ck)) {
                nw_toggle_task(nw, h, mo, ck); return 0;   /* consume */
            }
        }
    } else if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
        POINTL pt = { p.x, p.y };
        int cp = (int)SendMessageW(h, EM_CHARFROMPOS, 0, (LPARAM)&pt);
        int tab = nw_tab_of_edit(nw, h);
        if (cp >= 0 && tab >= 0) {
            size_t dn; const Deco* dd = nw_decos(nw, tab, &dn, NULL);
            size_t mo; int ck;
            if (dd && markdown_task_from_decos(dd, dn, (size_t)cp, &mo, &ck)) {
                SetCursor(LoadCursorW(NULL, MAKEINTRESOURCEW(32649))); return TRUE;  /* IDC_HAND */
            }
        }
    }
```

- [ ] **Step 3: Build + test**

Run: `make clean && make`
Expected: links clean, no new warnings.
Run: `make test`
Expected: PASS, `0 failed`.
GUI (deferred to human): clicking a `- [ ]` checkbox still toggles it (caret unmoved); the hand cursor still shows over checkboxes; moving the mouse over a large note no longer re-parses on every move (perf — not visible, but the reparse-per-mousemove is gone).

- [ ] **Step 4: Commit**

```bash
git add src/note_window.c
git commit -m "perf(md): task hit-test reads the parse cache (no per-mousemove reparse)"
```

---

## Self-Review Notes

**Spec coverage:**
- `DECO_TASK` + `*_from_decos` helpers + `fmt_at`/`task_at` reimplemented on them → Task 1.
- Per-tab cache (`nw_decos`), invalidation (EN_CHANGE / `nw_reformat_now` top / `nw_load_tab`), full lifecycle (free on close/delete, null vacated slot, init in add_tab/WM_CREATE, per-tab free in WM_DESTROY), fmt_at consumer rewire → Task 2.
- task_at consumer rewire (kills the per-mousemove reparse; subsumes the C2-deferred item) → Task 3.
- `nw_restyle` untouched (its sel-independence unification is C3b) → respected (no task modifies it).
- Deferred to C3b (block reparse) — not in any task, matching the spec.

**Placeholder scan:** none — every code step carries complete before/after code; every test step has real `CHECK` assertions + expected `make test` output.

**Type consistency:** `DECO_TASK` fields (`start`/`len`/`aux_start`/`number`) are written in Task 1's emission and read identically in `markdown_task_from_decos` (same task) and by no one else (`nw_apply_decos` ignores unknown kinds). `nw_decos(nw, tab, &n, &pool)` and `nw_cache_free(NoteTab*)` are defined in Task 2 and reused unchanged in Task 3. `markdown_fmt_from_decos`/`markdown_task_from_decos` signatures are fixed in Task 1 and called with those exact shapes in Tasks 2/3. Every task compiles warning-clean because each new static (`nw_decos`, `nw_cache_free`, `nw_tab_of_edit`) gains a caller in the same task that introduces it.

**Known limitations (documented):** `cache_pool` is retained but unread by C3a (freed each reparse; pre-wires C3b). A cache miss on OOM (`nw_body_text` returns NULL) keeps the prior list — degraded but safe. `nw_caret_on_heading` (the heading gate at the top of `nw_update_toggles`) is a cheap line-scan, not an md4c parse, so it is left as-is.
