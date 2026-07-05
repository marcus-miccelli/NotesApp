# Clickable Task Toggle (C2) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user plain-click a task-list `[ ]`/`[x]` checkbox to toggle it, with a hand cursor over checkboxes and the caret left undisturbed.

**Architecture:** Add a pure-logic `markdown_task_at` builder query (is an offset inside a checkbox?) to `src/markdown.c`, then a body-RichEdit subclass in `src/note_window.c` that handles `WM_LBUTTONDOWN` (toggle + consume) and `WM_SETCURSOR` (hand), flipping the source char and reformatting.

**Tech Stack:** C11, MSYS2 mingw-w64 gcc, md4c (existing), Win32 RichEdit (MSFTEDIT), `SetWindowSubclass`.

## Global Constraints

- Pure C, `-std=c11`, `-mwindows`, gcc 13.2 (MSYS2 mingw-w64). No C++. No new deps.
- Markdown knowledge lives in `src/markdown.c`; `note_window.c` = RichEdit mechanics only.
- Offsets are RichEdit `\r`/CP_ACP char offsets (body read via `EM_GETTEXTEX GT_DEFAULT + CP_ACP` = `nw_body_text`; `EM_CHARFROMPOS`/`EM_EXSETSEL` share that space).
- Gestures: **plain click** on `[ ]`/`[x]` toggles; the click is consumed so the **caret stays put**; a **hand cursor** shows over checkboxes.
- Testing: `markdown_task_at` is pure logic → real TDD via `tests/test_markdown.c` (`CHECK` from `tests/test.h`), gated by `make test`. The subclass (click/toggle/cursor) is GUI — gate is `make` clean + `make test` green; visual confirmation deferred to the human (scripted GUI focus unavailable here).

---

### Task 1: `markdown_task_at` builder query

**Files:**
- Modify: `src/markdown.h`, `src/markdown.c`
- Test: `tests/test_markdown.c`

**Interfaces:**
- Produces: `int markdown_task_at(const char* text, size_t len, size_t off, size_t* mark_off, int* checked);` — returns 1 if `off` is inside a task checkbox (`[`, mark, or `]`), setting `*mark_off` to the mark char's source offset and `*checked` to 1/0; returns 0 otherwise. `mark_off`/`checked` may be non-NULL only when it returns 1 (callers pass valid pointers).

- [ ] **Step 1: Add the prototype + failing tests**

In `src/markdown.h`, after the `markdown_fmt_at` prototype, add:
```c
/* If `off` is inside a task-list checkbox "[ ]"/"[x]", return 1 and set
 * *mark_off to the source offset of the mark char and *checked to its state;
 * else return 0. */
int markdown_task_at(const char* text, size_t len, size_t off,
                     size_t* mark_off, int* checked);
```

Append inside `test_markdown(void)` in `tests/test_markdown.c`:
```c
    /* --- markdown_task_at --- */
    {
        const char* t = "- [ ] a";   /* '[' at 2, mark(space) at 3, ']' at 4 */
        size_t mo = 0; int ck = 9;
        CHECK(markdown_task_at(t, strlen(t), 2, &mo, &ck) == 1);  /* on '[' */
        CHECK(mo == 3 && ck == 0);
        CHECK(markdown_task_at(t, strlen(t), 3, &mo, &ck) == 1);  /* on mark */
        CHECK(markdown_task_at(t, strlen(t), 4, &mo, &ck) == 1);  /* on ']' */
        CHECK(markdown_task_at(t, strlen(t), 6, &mo, &ck) == 0);  /* in text */
    }
    {
        const char* t = "- [x] b";
        size_t mo = 0; int ck = 0;
        CHECK(markdown_task_at(t, strlen(t), 3, &mo, &ck) == 1);
        CHECK(ck == 1);
    }
    {
        const char* t = "- a";       /* not a task item */
        size_t mo = 0; int ck = 0;
        CHECK(markdown_task_at(t, strlen(t), 2, &mo, &ck) == 0);
    }
```

- [ ] **Step 2: Run tests — verify they fail**

Run: `make test`
Expected: FAIL — `markdown_task_at` undefined (link error).

- [ ] **Step 3: Implement in the builder**

Append to `src/markdown.c` (after `markdown_fmt_at`). The no-op callbacks are
required because md4c dereferences every callback pointer it uses:
```c
typedef struct {
    size_t off;       /* query offset */
    int    found;
    size_t mark_off;
    int    checked;
} TaskCtx;

static int tq_enter_block(MD_BLOCKTYPE t, void* detail, void* ud) {
    TaskCtx* c = (TaskCtx*)ud;
    if (t == MD_BLOCK_LI) {
        MD_BLOCK_LI_DETAIL* d = (MD_BLOCK_LI_DETAIL*)detail;
        if (d->is_task) {
            size_t m  = (size_t)d->task_mark_offset;
            size_t lo = m > 0 ? m - 1 : 0;   /* '[' */
            size_t hi = m + 2;               /* one past ']' */
            if (c->off >= lo && c->off < hi) {
                c->found = 1;
                c->mark_off = m;
                c->checked = (d->task_mark == 'x' || d->task_mark == 'X');
            }
        }
    }
    return 0;
}
static int tq_noop_b(MD_BLOCKTYPE t, void* d, void* u) { (void)t;(void)d;(void)u; return 0; }
static int tq_noop_s(MD_SPANTYPE t, void* d, void* u)  { (void)t;(void)d;(void)u; return 0; }
static int tq_noop_t(MD_TEXTTYPE t, const MD_CHAR* x, MD_SIZE n, void* u) {
    (void)t;(void)x;(void)n;(void)u; return 0;
}

int markdown_task_at(const char* text, size_t len, size_t off,
                     size_t* mark_off, int* checked) {
    TaskCtx c; memset(&c, 0, sizeof c); c.off = off;
    MD_PARSER p; memset(&p, 0, sizeof p);
    p.abi_version = 0;
    p.flags = MD_DIALECT_COMMONMARK | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;
    p.enter_block = tq_enter_block;
    p.leave_block = tq_noop_b;
    p.enter_span  = tq_noop_s;
    p.leave_span  = tq_noop_s;
    p.text        = tq_noop_t;
    md_parse(text, (MD_SIZE)len, &p, &c);
    if (c.found) {
        if (mark_off) *mark_off = c.mark_off;
        if (checked)  *checked  = c.checked;
    }
    return c.found;
}
```

- [ ] **Step 4: Run tests — verify pass**

Run: `make test`
Expected: PASS, `0 failed`.

- [ ] **Step 5: Commit**

```bash
git add src/markdown.h src/markdown.c tests/test_markdown.c
git commit -m "feat(md): markdown_task_at checkbox hit-test query"
```

---

### Task 2: Body-edit subclass — click toggle + hand cursor

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: `markdown_task_at` (Task 1); existing `nw_body_text(HWND, int*)`, `nw_reformat_now(NoteWin*)`, `EDIT_EVENT_MASK`, `nw_edit(NoteWin*)`.
- Produces: `nw_body_sub` (a `SUBCLASSPROC` on each tab's body RichEdit) + `nw_toggle_task`.

- [ ] **Step 1: Add the toggle helper + subclass proc**

In `src/note_window.c`, add these two functions just above `nw_add_tab` (which
is the function containing the first body-edit `CreateWindowExW`, near line 1001;
place the new code before that function definition). `GET_X_LPARAM`/`GET_Y_LPARAM`
come from `<windowsx.h>` (already included).

```c
/* Flip the task mark at mark_off between space and 'x', then save + reformat. */
static void nw_toggle_task(NoteWin* nw, HWND edit, size_t mark_off, int checked) {
    SendMessageW(edit, EM_SETEVENTMASK, 0, 0);            /* mute; we reformat */
    CHARRANGE r = { (LONG)mark_off, (LONG)(mark_off + 1) };
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&r);
    SendMessageW(edit, EM_REPLACESEL, TRUE, (LPARAM)(checked ? L" " : L"x"));
    SendMessageW(edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
    nw_reformat_now(nw);
}

/* Body RichEdit subclass: plain-click a task checkbox toggles it (caret stays
 * put), and the cursor shows a hand over checkboxes. Non-checkbox mouse input
 * falls through to RichEdit's default handling. */
static LRESULT CALLBACK nw_body_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR id, DWORD_PTR ref) {
    (void)id;
    NoteWin* nw = (NoteWin*)ref;
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
    } else if (msg == WM_SETCURSOR) {
        POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
        POINTL pt = { p.x, p.y };
        int cp = (int)SendMessageW(h, EM_CHARFROMPOS, 0, (LPARAM)&pt);
        if (cp >= 0) {
            int len; char* buf = nw_body_text(h, &len);
            if (buf) {
                size_t mo; int ck;
                int hit = markdown_task_at(buf, (size_t)len, (size_t)cp, &mo, &ck);
                free(buf);
                if (hit) { SetCursor(LoadCursorW(NULL, IDC_HAND)); return TRUE; }
            }
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}
```

- [ ] **Step 2: Attach the subclass at both body-edit creation sites**

In `nw_add_tab` (the `CreateWindowExW(0, MSFTEDIT_CLASS, ...)` around line 1001),
right after the `nw->tab[i].edit = CreateWindowExW(...)` statement and before
`EM_SETBKGNDCOLOR`, add:
```c
    SetWindowSubclass(nw->tab[i].edit, nw_body_sub, 0, (DWORD_PTR)nw);
```

In the `WM_CREATE` per-tab load loop (the second `CreateWindowExW(0, MSFTEDIT_CLASS, ...)`
around line 1270), right after that `nw->tab[i].edit = CreateWindowExW(...)`
statement, add:
```c
            SetWindowSubclass(nw->tab[i].edit, nw_body_sub, 0, (DWORD_PTR)nw);
```

- [ ] **Step 3: Build + test**

Run: `make clean && make`
Expected: links clean, no new warnings.
Run: `make test`
Expected: PASS, `0 failed` (logic tests unchanged; this task is GUI wiring).

GUI (deferred to human): clicking a `- [ ]` checkbox toggles it to `- [x]` (and
strikes the item) and back; the caret does not jump to the click; the cursor is
a hand while hovering a checkbox and an I-beam elsewhere; clicking the item text
(not the checkbox) places the caret normally.

- [ ] **Step 4: Commit**

```bash
git add src/note_window.c
git commit -m "feat(md): click to toggle task checkboxes + hand cursor"
```

---

## Self-Review Notes

**Spec coverage:**
- `markdown_task_at` builder query → Task 1.
- Body-edit subclass: `WM_LBUTTONDOWN` toggle (consume, caret unmoved) + `WM_SETCURSOR` hand → Task 2.
- Toggle mutation (flip `' '`↔`'x'`, save + reformat) → Task 2 (`nw_toggle_task`).
- Plain-click gesture, caret stays put, hand cursor → Task 2.
- Deferred (perf → C3; ☐/☑ glyph rendering) → not in any task, matching the spec.

**Placeholder scan:** none — every code step carries complete code; every test step has real `CHECK` assertions + expected `make test` output.

**Type consistency:** `markdown_task_at(text, len, off, size_t* mark_off, int* checked)` defined in Task 1 (header + impl) and called with that exact signature in Task 2's subclass. `nw_toggle_task(nw, edit, mark_off, checked)` and `nw_body_sub` are defined and used within Task 2. Reused existing symbols (`nw_body_text`, `nw_reformat_now`, `EDIT_EVENT_MASK`) keep their current signatures.

**Notes / risks:** `EM_CHARFROMPOS` on RichEdit takes `wParam = 0`, `lParam = POINTL*`, returns the char index (or -1). A click past a line's end maps to the nearest char; `markdown_task_at` returns 0 for a non-checkbox offset, so a stray click just places the caret. The subclass is attached for the process lifetime (not explicitly removed on edit destroy), matching the file's existing subclasses.
