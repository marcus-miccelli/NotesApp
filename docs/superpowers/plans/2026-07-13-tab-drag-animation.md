# Tab Drag Slide Animation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Windows-Terminal-style tab drag animation — dragged tab follows the cursor and floats on top, displaced tabs slide smoothly, released tab glides into its slot.

**Architecture:** All changes live in `src/note_window.c`. A per-window `WM_TIMER` (10 ms) eases each tab's animated draw-x toward its slot-x with frame-rate-independent exponential easing; the dragged tab is painted last at the cursor position. Structural tab changes (add/close/delete/rename) cancel any drag/animation first.

**Tech Stack:** C11, Win32, Direct2D (existing `gfx_d2d` helpers). No new dependencies; `<math.h>` (`expf`, `fabsf`) added in Task 3.

**Spec:** `docs/superpowers/specs/2026-07-13-tab-drag-animation-design.md`

## Global Constraints

- GUI code (`note_window.c`) is NOT in the test binary (`LOGIC_SRC` in Makefile) — no unit tests possible for these handlers. Every task's test cycle is: `make app test -j4` must build warning-clean and keep `150 checks, 0 failed`, plus the manual check listed in the task.
- Constants at 96 dpi, scaled via `nw_s()` where they are pixel dimensions. Timer ids are raw ints.
- Match existing comment style: block comments explaining *why*, aligned field comments.
- Commit after each task.

---

### Task 1: Drag-cancel infrastructure, structural-change guards, WM_CAPTURECHANGED

Fixes a latent pre-existing bug family and gives Tasks 2–3 their state fields. Mouse capture routes messages to the window but does not block them: middle-click closes a tab mid-drag, and EN_MSGFILTER shortcuts (Ctrl+N add, Ctrl+Shift+D delete incl. modal MessageBox, Ctrl+R rename) fire mid-drag because keyboard focus stays on the RichEdit. Each desyncs `drag_tab` from the shifted `tab[]`. Also: capture can vanish without WM_LBUTTONUP (modal box, external SetCapture) leaving `drag_tab` stuck.

**Files:**
- Modify: `src/note_window.c` only.

**Interfaces:**
- Produces: `nw_anim_cancel(NoteWin*, HWND)` — hard-cancel drag + animation (snap, no settle); `nw_drag_release(NoteWin*, HWND)` — normal end-of-drag (Task 3 adds settle seeding here); struct fields `anim_x[]`, `anim_on`, `anim_last`, `drag_x`; constants `IDT_TABANIM`, `TAB_ANIM_TICK_MS`, `TAB_ANIM_TAU_MS`.

- [ ] **Step 1: Add constants** — in `src/note_window.c` directly under `#define IDT_DEBOUNCE 1` (line ~22):

```c
#define IDT_TABANIM  2       /* tab slide animation tick (Task: drag anim) */
#define TAB_ANIM_TICK_MS 10
#define TAB_ANIM_TAU_MS  70.0f  /* easing time constant; ~5*tau to fully settle */
```

- [ ] **Step 2: Add state fields** — in the `NoteWin` struct, directly after the existing drag block (`int dragging;` line ~119):

```c
    /* tab slide animation (drag reorder) */
    float anim_x[WIN_MAX_TABS]; /* current draw-x of each tab's left edge */
    int   anim_on;    /* 1 while IDT_TABANIM is alive */
    LARGE_INTEGER anim_last;    /* QPC stamp of the previous tick */
    int   drag_x;     /* latest cursor x while dragging */
```

- [ ] **Step 3: Add the two helpers** — place directly above `nw_close_tab` (line ~1025):

```c
/* Cancel any drag / slide animation outright (snap, no settle). Called before
 * structural tab changes: mouse capture routes messages here but does not
 * block them, so add/close/delete/rename CAN arrive mid-drag (middle-click,
 * EN_MSGFILTER shortcuts) and would desync drag_tab/anim_x from the shifted
 * tab[]. Clear state BEFORE ReleaseCapture so WM_CAPTURECHANGED no-ops. */
static void nw_anim_cancel(NoteWin* nw, HWND hwnd) {
    nw->drag_tab = -1;
    nw->dragging = 0;
    if (nw->anim_on) { KillTimer(hwnd, IDT_TABANIM); nw->anim_on = 0; }
    if (GetCapture() == hwnd) ReleaseCapture();
}

/* Normal end of a drag, from WM_LBUTTONUP or WM_CAPTURECHANGED (capture can
 * vanish without a buttonup — modal MessageBox, external SetCapture). */
static void nw_drag_release(NoteWin* nw, HWND hwnd) {
    (void)hwnd;
    nw->drag_tab = -1;
    nw->dragging = 0;
}
```

- [ ] **Step 4: Guard the four structural entry points** — first line of each function body:

In `nw_close_tab` (line ~1025), `nw_add_tab` (line ~1089), `nw_delete_note` (line ~1123), `nw_begin_rename` (line ~1164), insert as the first statement:

```c
    nw_anim_cancel(nw, hwnd);
```

(`nw_close_tab` has an early `if (i < 0 || i >= nw->ntabs) return;` — put the cancel BEFORE it; a cancel on a bogus index is still correct.)

- [ ] **Step 5: Route WM_LBUTTONUP through capture change and add WM_CAPTURECHANGED** — replace the existing `WM_LBUTTONUP` case (line ~1600):

```c
    case WM_LBUTTONUP:
        if (nw && GetCapture() == hwnd)
            ReleaseCapture();   /* WM_CAPTURECHANGED does the actual release */
        return 0;
    case WM_CAPTURECHANGED:
        if (nw && nw->drag_tab >= 0) nw_drag_release(nw, hwnd);
        return 0;
```

- [ ] **Step 6: Build + tests**

Run: `make app test -j4`
Expected: warning-clean build, `150 checks, 0 failed`.

- [ ] **Step 7: Manual check** — launch `./quicknote.exe`: drag-reorder still works (instant swap, as before); Ctrl+Shift+D mid-drag no longer leaves a stuck drag (tab under cursor stops following after the MessageBox appears).

- [ ] **Step 8: Commit**

```bash
git add src/note_window.c
git commit -m "fix: cancel tab drag on structural changes and capture loss"
```

---

### Task 2: Floating dragged tab (follows cursor, painted on top)

The dragged tab now draws at the cursor x instead of snapping to slot positions. Displaced tabs still swap instantly (Task 3 makes them slide).

**Files:**
- Modify: `src/note_window.c` only.

**Interfaces:**
- Consumes: `nw->drag_x`, `nw->anim_x`, `nw->anim_on` (Task 1 fields).
- Produces: `nw_tab_draw_x(NoteWin*, HWND, int) -> int` — animated/floating left edge of tab i (Task 3's tick relies on its non-drag branches); `nw_paint_tab_decor(NoteWin*, HWND, int, int, int)` — label + ✕ at an explicit x.

- [ ] **Step 1: Add draw-x helper** — directly below `nw_plus_rect` (line ~297):

```c
/* Animated left edge of tab i. The dragged tab rides the cursor, clamped to
 * [strip_left, last slot's left edge] — NOT strip_right: with few tabs
 * nw_tab_w clamps to TAB_MAX and slots end far left of strip_right, and the
 * wider bound would let the tab drag past the + button into empty strip.
 * anim_x is only trusted while anim_on (Task: slide animation). */
static int nw_tab_draw_x(NoteWin* nw, HWND hwnd, int i) {
    if (nw->dragging && i == nw->drag_tab) {
        int w  = nw_tab_w(nw, hwnd);
        int x  = nw->drag_x - nw->drag_dx;
        int lo = nw_strip_left(hwnd);
        int hi = lo + (nw->ntabs - 1) * w;
        if (x < lo) x = lo;
        if (x > hi) x = hi;
        return x;
    }
    if (nw->anim_on) return (int)nw->anim_x[i];
    RECT r; nw_tab_rect(nw, hwnd, i, &r);
    return r.left;
}
```

- [ ] **Step 2: Track drag_x, clear stale hover, invalidate bar-only** — in the `WM_MOUSEMOVE` drag branch (line ~1540), replace the whole `if (nw && GetCapture() == hwnd && nw->drag_tab >= 0) { ... }` block with:

```c
        if (nw && GetCapture() == hwnd && nw->drag_tab >= 0) {
            int x = GET_X_LPARAM(lp);
            if (!nw->dragging && abs(x - nw->press_x) > DRAG_THRESHOLD) {
                nw->dragging = 1;
                /* the drag branch returns before the hover code below, so
                 * press-time hot state would go stale and paint a displaced
                 * neighbor as hovered while it moves */
                nw->hot_tab = -1; nw->hot_close = 0; nw->hot_plus = 0;
            }
            if (nw->dragging) {
                nw->drag_x = x;
                int w = nw_tab_w(nw, hwnd);
                int center = x - nw->drag_dx + w / 2;      /* dragged tab center */
                int target = (center - nw_strip_left(hwnd)) / w;
                if (target < 0) target = 0;
                if (target > nw->ntabs - 1) target = nw->ntabs - 1;
                if (target != nw->drag_tab) {
                    NoteTab moved = nw->tab[nw->drag_tab];
                    if (target > nw->drag_tab)
                        for (int k = nw->drag_tab; k < target; k++) nw->tab[k] = nw->tab[k+1];
                    else
                        for (int k = nw->drag_tab; k > target; k--) nw->tab[k] = nw->tab[k-1];
                    nw->tab[target] = moved;
                    nw->active = target;
                    nw->drag_tab = target;
                    nw_sync_winmeta(nw);
                }
                /* D2D clears and repaints the whole target every frame; the
                 * sub-rect + FALSE only skips the redundant GDI erase */
                RECT bar; GetClientRect(hwnd, &bar); bar.bottom = nw_titlebar_h(hwnd);
                InvalidateRect(hwnd, &bar, FALSE);
            }
            return 0;
        }
```

- [ ] **Step 3: Factor label/✕ painting and use draw-x in nw_paint_tabs** — replace `nw_paint_tabs` (line ~1231) entirely with:

```c
/* Label + ✕ for tab i with its left edge at x (width w). Shared by the strip
 * loop and the floating dragged tab. */
static void nw_paint_tab_decor(NoteWin* nw, HWND hwnd, int i, int x, int w) {
    GfxD2D* g = nw->gfx;
    RECT r; nw_tab_rect(nw, hwnd, i, &r);          /* top/bottom only */
    int isact = (i == nw->active);
    int ishot = (i == nw->hot_tab);

    NoteMeta* m = nw_note_meta(nw, i);
    wchar_t wname[128];
    MultiByteToWideChar(CP_ACP, 0, m && m->name[0] ? m->name : "Untitled", -1, wname, 128);

    RECT tr = { x + nw_s(hwnd, 12), r.top + nw_s(hwnd, TAB_TOP_GAP),
                x + w - nw_s(hwnd, TAB_CLOSE_W) - nw_s(hwnd, 4), r.bottom - 1 };
    unsigned lc = isact ? COL_TEXT
                : (ishot ? RGB(0xc9,0xc9,0xd0) : RGB(0x8a,0x8a,0x90));
    gfx_text(g, wname, tr, GFX_FMT_LABEL, lc);

    RECT xr = { x + w - nw_s(hwnd, TAB_CLOSE_W), r.top + nw_s(hwnd, TAB_TOP_GAP),
                x + w, r.bottom };
    int overx = (ishot && nw->hot_close);
    if (overx) gfx_fill_tab(g, xr, nw_s(hwnd, TAB_RADIUS), RGB(0x3a,0x3a,0x40));
    gfx_text(g, L"\x2715", xr, GFX_FMT_CLOSE, overx ? RGB(0xff,0xff,0xff) : RGB(0xc0,0xc0,0xc8));
}

/* Draw the tab strip with Direct2D: + button fill behind, tab fills back-to-
 * front (top corners rounded, antialiased), then labels and ✕ glyphs, the +
 * glyph, and finally the dragged tab floating on top of everything (after the
 * + so it covers it in the overlap zone). Colors from the existing palette. */
static void nw_paint_tabs(NoteWin* nw, HWND hwnd) {
    GfxD2D* g = nw->gfx;
    int radius = nw_s(hwnd, TAB_RADIUS);
    int w = nw_tab_w(nw, hwnd);
    int drag = nw->dragging ? nw->drag_tab : -1;

    RECT pr; nw_plus_rect(nw, hwnd, &pr);
    if (nw->hot_plus) gfx_fill_tab(g, pr, radius, COL_HOVER);

    for (int i = nw->ntabs - 1; i >= 0; i--) {
        if (i == drag) continue;
        RECT r; nw_tab_rect(nw, hwnd, i, &r);
        int x = nw_tab_draw_x(nw, hwnd, i);
        int isact = (i == nw->active);
        int ishot = (i == nw->hot_tab);

        /* extend under the left neighbor only while visually flush with it —
         * the extension exists to be covered by the neighbor's later paint;
         * when a slide opens a gap it would show as a bare square stub */
        int fill_left = x;
        if (i > 0 && x - nw_tab_draw_x(nw, hwnd, i - 1) <= w) fill_left -= radius;
        RECT shape = { fill_left, r.top + nw_s(hwnd, TAB_TOP_GAP), x + w, r.bottom };

        unsigned fill = COL_BG;
        if (isact)      fill = COL_TAB_ACT;
        else if (ishot) fill = COL_TAB_HOT;
        gfx_fill_tab(g, shape, radius, fill);
    }

    for (int i = 0; i < nw->ntabs; i++) {
        if (i == drag) continue;
        nw_paint_tab_decor(nw, hwnd, i, nw_tab_draw_x(nw, hwnd, i), w);
    }

    RECT pg = pr; pg.left += radius;
    gfx_text(g, L"\x002B", pg, GFX_FMT_PLUS, nw->hot_plus ? RGB(0xff,0xff,0xff) : RGB(0x9a,0x9a,0xa2));

    if (drag >= 0) {
        RECT r; nw_tab_rect(nw, hwnd, drag, &r);
        int x = nw_tab_draw_x(nw, hwnd, drag);
        /* no radius left-extension: drawn on top, nothing covers it */
        RECT shape = { x, r.top + nw_s(hwnd, TAB_TOP_GAP), x + w, r.bottom };
        gfx_fill_tab(g, shape, radius, COL_TAB_ACT);   /* dragged tab is active */
        nw_paint_tab_decor(nw, hwnd, drag, x, w);
    }

    if (nw->rename_edit) {
        RECT er; nw_rename_rect(nw, hwnd, &er);
        RECT fr = { er.left - 2, er.top - 2, er.right + 2, er.bottom + 2 };
        gfx_fill_round(g, fr, nw_s(hwnd, 5), COL_ACCENT);
    }
}
```

- [ ] **Step 4: Build + tests**

Run: `make app test -j4`
Expected: warning-clean build, `150 checks, 0 failed`.

- [ ] **Step 5: Manual check** — launch `./quicknote.exe` with 3+ tabs: dragged tab follows the cursor and floats over neighbors; it cannot be dragged past the last slot toward the + button; on release it lands in its slot (instantly — settle comes in Task 3); hover highlight does not stick to a neighbor during drag.

- [ ] **Step 6: Commit**

```bash
git add src/note_window.c
git commit -m "feat: dragged tab floats under cursor, painted on top"
```

---

### Task 3: Slide + settle animation (timer, easing, anim_x shuffle)

**Files:**
- Modify: `src/note_window.c` only.

**Interfaces:**
- Consumes: everything from Tasks 1–2. `nw_drag_release` body changes here.

- [ ] **Step 1: Include math.h** — add `#include <math.h>` after `#include <string.h>` (line ~16).

- [ ] **Step 2: Seed animation at drag start** — in the `WM_MOUSEMOVE` drag branch from Task 2 Step 2, extend the threshold-crossing block:

```c
            if (!nw->dragging && abs(x - nw->press_x) > DRAG_THRESHOLD) {
                nw->dragging = 1;
                nw->hot_tab = -1; nw->hot_close = 0; nw->hot_plus = 0;
                /* re-grab mid-settle keeps current anim_x — re-seeding would
                 * teleport tabs that are still sliding */
                if (!nw->anim_on) {
                    for (int i = 0; i < nw->ntabs; i++) {
                        RECT tr2; nw_tab_rect(nw, hwnd, i, &tr2);
                        nw->anim_x[i] = (float)tr2.left;
                    }
                    nw->anim_on = 1;
                }
                QueryPerformanceCounter(&nw->anim_last);
                SetTimer(hwnd, IDT_TABANIM, TAB_ANIM_TICK_MS, NULL);
            }
```

- [ ] **Step 3: Shuffle anim_x with tab[] on reorder** — in the same branch, replace the reorder block with:

```c
                if (target != nw->drag_tab) {
                    NoteTab moved = nw->tab[nw->drag_tab];
                    float   mx    = nw->anim_x[nw->drag_tab];
                    if (target > nw->drag_tab)
                        for (int k = nw->drag_tab; k < target; k++) {
                            nw->tab[k]    = nw->tab[k+1];
                            nw->anim_x[k] = nw->anim_x[k+1];
                        }
                    else
                        for (int k = nw->drag_tab; k > target; k--) {
                            nw->tab[k]    = nw->tab[k-1];
                            nw->anim_x[k] = nw->anim_x[k-1];
                        }
                    nw->tab[target]    = moved;
                    nw->anim_x[target] = mx;
                    nw->active = target;
                    nw->drag_tab = target;
                    nw_sync_winmeta(nw);
                }
```

- [ ] **Step 4: Grab offset from draw position** — in `WM_LBUTTONDOWN` (line ~1527), replace `nw->drag_dx  = GET_X_LPARAM(lp) - r.left;` with:

```c
                nw->drag_dx  = GET_X_LPARAM(lp) - nw_tab_draw_x(nw, hwnd, idx);
```

(`nw_tab_draw_x` returns the slot-x when idle — identical to today — and the mid-settle draw-x on a re-grab, so the tab doesn't jump under the cursor.)

- [ ] **Step 5: Settle seed on release** — replace `nw_drag_release` body:

```c
/* Normal end of a drag, from WM_LBUTTONUP or WM_CAPTURECHANGED (capture can
 * vanish without a buttonup — modal MessageBox, external SetCapture). Seeds
 * the released tab's anim_x from its floating position so the running timer
 * glides it into its slot. */
static void nw_drag_release(NoteWin* nw, HWND hwnd) {
    if (nw->dragging && nw->drag_tab >= 0 && nw->anim_on)
        nw->anim_x[nw->drag_tab] = (float)nw_tab_draw_x(nw, hwnd, nw->drag_tab);
    nw->drag_tab = -1;
    nw->dragging = 0;
}
```

(The draw-x read must happen BEFORE clearing `dragging` — it selects the floating branch.)

- [ ] **Step 6: The tick** — in the `WM_TIMER` case (line ~1698), add the IDT_TABANIM branch ABOVE the existing IDT_DEBOUNCE branch:

```c
    case WM_TIMER:
        if (wp == IDT_TABANIM && nw) {
            LARGE_INTEGER now, f;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&f);
            float dt = (float)(now.QuadPart - nw->anim_last.QuadPart) * 1000.0f
                     / (float)f.QuadPart;
            nw->anim_last = now;
            if (dt > 100.0f) dt = 100.0f;   /* absorb modal/menu stalls */
            float k = 1.0f - expf(-dt / TAB_ANIM_TAU_MS);

            int settled = 1;
            for (int i = 0; i < nw->ntabs; i++) {
                if (nw->dragging && i == nw->drag_tab) continue;
                RECT r; nw_tab_rect(nw, hwnd, i, &r);   /* live slot: resize/DPI self-corrects */
                float target = (float)r.left;
                nw->anim_x[i] += (target - nw->anim_x[i]) * k;
                if (fabsf(nw->anim_x[i] - target) > 0.5f) settled = 0;
                else nw->anim_x[i] = target;
            }
            if (settled && !nw->dragging) {
                KillTimer(hwnd, IDT_TABANIM);
                nw->anim_on = 0;
            }
            RECT bar; GetClientRect(hwnd, &bar); bar.bottom = nw_titlebar_h(hwnd);
            InvalidateRect(hwnd, &bar, FALSE);
            return 0;
        }
        if (wp == IDT_DEBOUNCE && nw && nw_edit(nw)) {
            /* ... existing debounce body unchanged ... */
        }
        return 0;
```

- [ ] **Step 7: Build + tests**

Run: `make app test -j4`
Expected: warning-clean build, `150 checks, 0 failed`.

- [ ] **Step 8: Manual check** — launch `./quicknote.exe` with 4+ tabs:
  - Drag a tab across several slots: neighbors slide smoothly, no teleports.
  - Release mid-strip: released tab glides into its slot (~⅓ s).
  - Grab another tab while tabs are still settling: nothing teleports, grabbed tab doesn't jump under the cursor.
  - Ctrl+Shift+D mid-drag → Cancel in the box: drag is cancelled, tabs snapped, no perpetual repaint (Task Manager: quicknote CPU back to ~0 when idle).

- [ ] **Step 9: Commit**

```bash
git add src/note_window.c
git commit -m "feat: tabs slide during drag reorder, settle on release"
```
