# DPI-aware + Direct2D Chrome Rendering — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the sticky-note window's custom chrome at native resolution on every monitor with antialiased corners and no flicker, by adding PerMonitorV2 DPI awareness and moving the whole titlebar paint to Direct2D/DirectWrite.

**Architecture:** A PerMonitorV2 manifest turns off Windows bitmap-scaling. All chrome geometry constants stay as 96-dpi base values and are scaled at each use site through `nw_s(hwnd, v)`. A new `src/gfx_d2d.{c,h}` module wraps Direct2D/DirectWrite (process-wide factories, per-window hwnd render target pinned to 96 DPI so 1 DIP = 1 physical pixel). The window `WM_PAINT` is rewritten to draw the titlebar background, dividers, "qN" wordmark, tabs, per-tab ✕, +, and the min/max/close buttons through that module. The min/max/close buttons stop being child `BUTTON` controls and become D2D-drawn regions hit-tested by the parent. The sidebar B/I/S buttons and RichEdit bodies stay as GDI child controls.

**Tech Stack:** C11, MSYS2 mingw-w64 gcc 13.2, Win32, Direct2D (`-ld2d1`), DirectWrite (`-ldwrite`), `windres` for the manifest/icon resource.

## Global Constraints

- Language/toolchain: pure C, `-std=c11`, `-mwindows`, gcc 13.2 (MSYS2 mingw-w64). No C++.
- D2D/DirectWrite from C requires `#define COBJMACROS` before the headers, and `INITGUID` defined in exactly one translation unit (`gfx_d2d.c`) so `IID_ID2D1Factory` / `IID_IDWriteFactory` link.
- Direct2D render target DPI is pinned to 96 (`dpiX=dpiY=96`). **All** DPI scaling is done by us via `nw_s`. Never set the render-target DPI to the monitor DPI — that double-scales the chrome.
- Constants in `note_window.c` remain 96-dpi base values; every geometry/font use site scales through `nw_s`.
- Existing custom frame stays: `WM_NCCALCSIZE` caption claim + `DwmExtendFrameIntoClientArea` `MARGINS {0,0,1,0}`.
- Existing palette `COLORREF` constants (`COL_BG`, `COL_TAB_ACT`, …) are reused; Direct2D colors are derived from them at draw time.
- Testing reality: chrome paint is not unit-testable. Every task's gate is: (a) `make` builds with no new warnings, (b) `make test` still passes the existing pure-logic suite, (c) launch a fresh instance and visually confirm / screenshot. Scripted GUI focus is unavailable in this environment — verify by launching fresh and screenshotting.

---

### Task 1: PerMonitorV2 manifest

**Files:**
- Create: `assets/app.manifest`
- Modify: `assets/app.rc`
- Modify: `Makefile` (the `$(RES_OBJ)` dependency line, ~line 39)

**Interfaces:**
- Consumes: nothing.
- Produces: process is PerMonitorV2 DPI-aware at launch. No symbols for later tasks.

- [ ] **Step 1: Create the manifest**

Create `assets/app.manifest`:

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">PerMonitorV2</dpiAwareness>
    </windowsSettings>
  </application>
</assembly>
```

- [ ] **Step 2: Reference the manifest from the resource script**

Append to `assets/app.rc` (below the existing `IDI_APP ICON` line):

```
/* RT_MANIFEST = 24; id 1 = CREATEPROCESS_MANIFEST_RESOURCE_ID for an EXE. */
1 24 "app.manifest"
```

- [ ] **Step 3: Add the manifest as a build dependency**

In `Makefile`, change the resource-object rule so editing the manifest triggers a rebuild. Replace:

```make
$(RES_OBJ): assets/app.rc assets/quicknote.ico src/resource.h
	$(WINDRES) -Isrc -Iassets -o $@ assets/app.rc
```

with:

```make
$(RES_OBJ): assets/app.rc assets/quicknote.ico assets/app.manifest src/resource.h
	$(WINDRES) -Isrc -Iassets -o $@ assets/app.rc
```

- [ ] **Step 4: Build**

Run: `make clean && make`
Expected: links `quicknote.exe` with no errors and no new warnings.

- [ ] **Step 5: Verify DPI awareness is declared**

Run: `make test`
Expected: existing suite passes (unchanged).

Then launch a fresh instance and confirm the window opens normally:
Run: `./quicknote.exe &`
Expected: window appears. On a >100%-scaled monitor it now renders un-blurred but with tiny chrome (constants not yet scaled — fixed in Task 2). On a 100% monitor it looks identical to before. Close the window.

- [ ] **Step 6: Commit**

```bash
git add assets/app.manifest assets/app.rc Makefile
git commit -m "feat(dpi): declare PerMonitorV2 DPI awareness via manifest"
```

---

### Task 2: DPI-scale all chrome geometry and fonts

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `static int nw_dpi(HWND h)` → window DPI (96 fallback).
  - `static int nw_s(HWND h, int v)` → `MulDiv(v, nw_dpi(h), 96)`.
  - Signature changes used by Task 4/5 paint code:
    - `nw_strip_left(HWND h)` (was `void`)
    - `nw_window_buttons_width(HWND h)` (was `void`)
  - All geometry helpers now return physical-pixel rects for the current DPI.

- [ ] **Step 1: Add the DPI helpers**

In `src/note_window.c`, immediately after `static NoteWin* nw_get(...)` (~line 121), add:

```c
/* Window DPI (PerMonitorV2). GetDpiForWindow needs Win10 1607+; 96 is the
 * safe fallback. All chrome constants are authored at 96 dpi and scaled here. */
static int nw_dpi(HWND h) { UINT d = GetDpiForWindow(h); return d ? (int)d : 96; }
static int nw_s(HWND h, int v) { return MulDiv(v, nw_dpi(h), 96); }
```

- [ ] **Step 2: Scale the title-bar / gutter helpers**

Replace the block from `static int nw_top_gutter` through `nw_window_buttons_left` (~lines 123-142) with:

```c
static int nw_top_gutter(HWND hwnd) {
    return IsZoomed(hwnd) ? 0 : nw_s(hwnd, TOP_FRAME_H);
}

static int nw_titlebar_h(HWND hwnd) {
    return nw_top_gutter(hwnd) + nw_s(hwnd, TITLE_CONTENT_H);
}

static int nw_tab_top(HWND hwnd) {
    return nw_top_gutter(hwnd) + nw_s(hwnd, TAB_TOP_GAP);
}

static int nw_window_buttons_width(HWND hwnd) {
    return NUM_WBTNS * nw_s(hwnd, WINBTN_W);
}

static int nw_window_buttons_left(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    return rc.right - nw_window_buttons_width(hwnd);
}
```

- [ ] **Step 3: Scale `nw_regions`**

Replace the body of `nw_regions` (~lines 195-219) with:

```c
static void nw_regions(HWND hwnd, RECT* title, RECT* body, int* vx, int* hy) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    int v  = nw_s(hwnd, SIDEBAR_W);
    int h  = nw_titlebar_h(hwnd);
    int cl = v + nw_s(hwnd, REGION_GAP);

    int btns_l = nw_window_buttons_left(hwnd);
    int t_r = btns_l - nw_s(hwnd, REGION_GAP);
    if (t_r < cl) t_r = cl;

    int cr = rc.right - nw_s(hwnd, PAD_OUTER);
    if (cr < cl) cr = cl;

    int b_top = h + nw_s(hwnd, REGION_GAP);
    int b_bot = rc.bottom - nw_s(hwnd, PAD_OUTER);
    if (b_bot < b_top) b_bot = b_top;

    if (title) SetRect(title, cl, nw_s(hwnd, 4), t_r, h - nw_s(hwnd, 4));
    if (body)  SetRect(body,  cl, b_top, cr, b_bot);
    if (vx) *vx = v;
    if (hy) *hy = h;
}
```

- [ ] **Step 4: Scale the tab-strip geometry helpers**

Replace the block from `static int nw_strip_left` through `nw_plus_rect` (~lines 222-280) with:

```c
static int nw_strip_left(HWND hwnd)  { return nw_s(hwnd, SIDEBAR_W) + 1; }
static int nw_strip_right(HWND hwnd) {
    int btns_l = nw_window_buttons_left(hwnd);
    return btns_l - nw_s(hwnd, DRAG_GAP);
}
static int nw_tab_w(NoteWin* nw, HWND hwnd) {
    int plus_visible = nw_s(hwnd, TITLE_CONTENT_H) - nw_s(hwnd, TAB_TOP_GAP) - 1;
    int avail = nw_strip_right(hwnd) - nw_strip_left(hwnd) - plus_visible;
    int n = nw->ntabs > 0 ? nw->ntabs : 1;
    int w = avail / n;
    if (w > nw_s(hwnd, TAB_MAX)) w = nw_s(hwnd, TAB_MAX);
    if (w < nw_s(hwnd, TAB_MIN)) w = nw_s(hwnd, TAB_MIN);
    return w;
}
static void nw_tab_rect(NoteWin* nw, HWND hwnd, int i, RECT* r) {
    int w = nw_tab_w(nw, hwnd);
    int l = nw_strip_left(hwnd) + i * w;
    int top = nw_top_gutter(hwnd);
    SetRect(r, l, top, l + w, top + nw_s(hwnd, TITLE_CONTENT_H));
}
static void nw_close_rect(NoteWin* nw, HWND hwnd, int i, RECT* r) {
    RECT t;
    nw_tab_rect(nw, hwnd, i, &t);
    int top = t.top + nw_s(hwnd, TAB_TOP_GAP);
    int bottom = t.bottom;
    SetRect(r, t.right - nw_s(hwnd, TAB_CLOSE_W), top, t.right, bottom);
}
static void nw_plus_rect(NoteWin* nw, HWND hwnd, RECT* r) {
    int w = nw_tab_w(nw, hwnd);
    int tab_top = nw_top_gutter(hwnd);
    int top = tab_top + nw_s(hwnd, TAB_TOP_GAP);
    int bottom = tab_top + nw_s(hwnd, TITLE_CONTENT_H);
    int visible_size = bottom - top;
    int visible_left = nw_strip_left(hwnd) + nw->ntabs * w;
    SetRect(r, visible_left - nw_s(hwnd, PLUS_OVERLAP), top,
            visible_left + visible_size, bottom);
}
```

- [ ] **Step 5: Scale the hit-test**

In `nw_tab_hit` (~lines 282-298), replace the two lines that use raw constants:

```c
    int tab_top = nw_top_gutter(hwnd);
    int tab_bottom = tab_top + TITLE_CONTENT_H;
```
with:
```c
    int tab_top = nw_top_gutter(hwnd);
    int tab_bottom = tab_top + nw_s(hwnd, TITLE_CONTENT_H);
```
and replace:
```c
            if (cx >= r.right - CTL_SIZE - 8) return HIT_CLOSE;
```
with:
```c
            if (cx >= r.right - nw_s(hwnd, CTL_SIZE) - nw_s(hwnd, 8)) return HIT_CLOSE;
```

- [ ] **Step 6: Scale `nw_layout`**

Replace the body of `nw_layout` (~lines 303-343) with:

```c
static void nw_layout(HWND hwnd, NoteWin* nw) {
    RECT rc, bd;
    int vx, hy;

    GetClientRect(hwnd, &rc);
    nw_regions(hwnd, NULL, &bd, &vx, &hy);

    int side = vx;
    int gap  = nw_s(hwnd, BTN_GAP);
    for (int i = 0; i < NUM_BTNS; i++) {
        MoveWindow(nw->btns[i], 0, hy + 1 + i * (side + gap), side, side, TRUE);
    }

    for (int i = 0; i < nw->ntabs; i++) {
        MoveWindow(nw->tab[i].edit, bd.left, bd.top,
                   bd.right - bd.left, bd.bottom - bd.top, TRUE);
    }
}
```

Note: the `wbtns[]` positioning loop is intentionally dropped here — Task 5 removes those child controls. Until then, the window buttons are laid out by the loop you are deleting, so **for Task 2 keep the `wbtns[]` loop** exactly as-is but scale it. Use this body instead for Task 2:

```c
static void nw_layout(HWND hwnd, NoteWin* nw) {
    RECT rc, bd;
    int vx, hy;

    GetClientRect(hwnd, &rc);
    nw_regions(hwnd, NULL, &bd, &vx, &hy);

    int side = vx;
    int gap  = nw_s(hwnd, BTN_GAP);
    for (int i = 0; i < NUM_BTNS; i++) {
        MoveWindow(nw->btns[i], 0, hy + 1 + i * (side + gap), side, side, TRUE);
    }

    int wbtn_w = nw_s(hwnd, WINBTN_W);
    for (int i = 0; i < NUM_WBTNS; i++) {
        MoveWindow(nw->wbtns[i], rc.right - (NUM_WBTNS - i) * wbtn_w, 0,
                   wbtn_w, nw_titlebar_h(hwnd), TRUE);
    }

    for (int i = 0; i < nw->ntabs; i++) {
        MoveWindow(nw->tab[i].edit, bd.left, bd.top,
                   bd.right - bd.left, bd.bottom - bd.top, TRUE);
    }
}
```

- [ ] **Step 7: Scale owner-draw and RichEdit fonts**

In `nw_draw_button` (~line 358) replace:
```c
    lf.lfHeight = -16; lf.lfWeight = FW_BOLD; wcscpy(lf.lfFaceName, L"IBM Plex Mono");
```
with:
```c
    lf.lfHeight = -nw_s(d->hwndItem, 16); lf.lfWeight = FW_BOLD; wcscpy(lf.lfFaceName, L"IBM Plex Mono");
```

In `nw_draw_wbutton` (~line 395) replace:
```c
    lf.lfHeight = -10; lf.lfWeight = FW_NORMAL; wcscpy(lf.lfFaceName, L"Segoe MDL2 Assets");
```
with:
```c
    lf.lfHeight = -nw_s(d->hwndItem, 10); lf.lfWeight = FW_NORMAL; wcscpy(lf.lfFaceName, L"Segoe MDL2 Assets");
```

(These `nw_draw_wbutton` changes are removed in Task 5 with the whole function; scaling them now keeps Task 2 self-consistent.)

For the RichEdit body font, add a scaled base-size helper and use it. Right after the `nw_s` definition (Step 1) add:

```c
/* Body char height in twips, scaled to the window DPI (10pt base = 200 twips). */
static LONG nw_body_yheight(HWND hwnd) { return (LONG)nw_s(hwnd, 200); }
```

Then in the two places that set `cf.yHeight = 200;` for the body edit (in `nw_add_tab` ~line 1080 and in `WM_CREATE` ~line 1411) replace `cf.yHeight = 200;` with `cf.yHeight = nw_body_yheight(hwnd);`.

- [ ] **Step 8: Rename-rect scaling**

Replace the body of `nw_rename_rect` (~lines 1130-1137) with:

```c
static void nw_rename_rect(NoteWin* nw, HWND hwnd, RECT* r) {
    RECT t; nw_tab_rect(nw, hwnd, nw->rename_tab, &t);
    int tab_top = nw_tab_top(hwnd);
    int ex = t.left + nw_s(hwnd, 8), ey = tab_top + nw_s(hwnd, 3);
    int ew = (t.right - t.left) - nw_s(hwnd, 16); if (ew < nw_s(hwnd, 40)) ew = nw_s(hwnd, 40);
    int eh = nw_titlebar_h(hwnd) - tab_top - nw_s(hwnd, 6);
    SetRect(r, ex, ey, ex + ew, ey + eh);
}
```

Also scale the rename font in `nw_begin_rename` (~line 1149): replace
```c
    nw->rename_font = CreateFontW(-14,0,0,0,FW_SEMIBOLD,0,0,0,0,0,0,0,0,L"IBM Plex Mono");
```
with
```c
    nw->rename_font = CreateFontW(-nw_s(hwnd,14),0,0,0,FW_SEMIBOLD,0,0,0,0,0,0,0,0,L"IBM Plex Mono");
```

- [ ] **Step 9: Fix the one `nw_strip_left()` caller and any `nw_window_buttons_width()` caller**

Search for `nw_strip_left()` and `nw_window_buttons_width()`. The drag handler in `WM_MOUSEMOVE` (~line 1555) calls `nw_strip_left()`; change it to `nw_strip_left(hwnd)`. `nw_window_buttons_width` is only called by `nw_window_buttons_left`, already updated in Step 2.

Run: `grep -n "nw_strip_left()\|nw_window_buttons_width()" src/note_window.c`
Expected: no matches remain.

- [ ] **Step 10: Build and verify**

Run: `make clean && make`
Expected: no errors, no new warnings.

Run: `make test`
Expected: existing suite passes.

Launch fresh; at 100% the chrome is unchanged. If a HiDPI monitor is available, set it to 150%, launch fresh, and confirm the tabs/sidebar/buttons scale up proportionally (still GDI-aliased corners — smoothing comes in Task 4).

- [ ] **Step 11: Commit**

```bash
git add src/note_window.c
git commit -m "feat(dpi): scale all chrome geometry and fonts by window DPI"
```

---

### Task 3: Direct2D/DirectWrite module skeleton (`src/gfx_d2d`)

**Files:**
- Create: `src/gfx_d2d.h`
- Create: `src/gfx_d2d.c`
- Modify: `Makefile` (`APP_SRC`, `LDLIBS`)

**Interfaces:**
- Produces (consumed by Task 4/5):
  - `int  gfx_global_init(void);` — create process-wide `ID2D1Factory` + `IDWriteFactory`. Returns 1 on success, 0 on failure. Idempotent.
  - `void gfx_global_shutdown(void);`
  - `typedef struct GfxD2D GfxD2D;` — opaque per-window handle.
  - `GfxD2D* gfx_create(HWND hwnd, int dpi);` — hwnd render target pinned to 96 dpi + brush + text formats sized for `dpi`. NULL on failure.
  - `void gfx_destroy(GfxD2D* g);`
  - `void gfx_resize(GfxD2D* g, int w, int h);`
  - `void gfx_set_dpi(GfxD2D* g, int dpi);` — release + recreate text formats at new scaled size.
  - `int  gfx_begin(GfxD2D* g, unsigned bg);` — `BeginDraw` + `Clear(bg)`. Returns 1 if drawing may proceed.
  - `int  gfx_end(GfxD2D* g);` — `EndDraw`. Returns 1 if the target must be recreated (caller destroys + invalidates).
  - Draw primitives (COLORREF in, called between begin/end):
    - `void gfx_fill_rect(GfxD2D* g, RECT r, unsigned col);`
    - `void gfx_fill_round(GfxD2D* g, RECT r, int radius, unsigned col);`  (all four corners)
    - `void gfx_fill_tab(GfxD2D* g, RECT r, int radius, unsigned col);`   (top corners rounded, bottom square)
    - `void gfx_text(GfxD2D* g, const wchar_t* s, RECT r, int fmt, unsigned col);`
  - Text-format selectors:
    - `#define GFX_FMT_LABEL 0` (Segoe UI SemiBold, left, ellipsis)
    - `#define GFX_FMT_CLOSE 1` (Segoe UI, centered — tab ✕)
    - `#define GFX_FMT_PLUS  2` (Segoe UI, centered, larger — the +)
    - `#define GFX_FMT_WGLYPH 3` (Segoe MDL2 Assets, centered — min/max/close)
    - `#define GFX_FMT_MARK  4` (Segoe UI SemiBold, centered — "qN")
  - `unsigned` color args are plain Win32 `COLORREF` values.

- [ ] **Step 1: Write the header**

Create `src/gfx_d2d.h`:

```c
#ifndef GFX_D2D_H
#define GFX_D2D_H

#include <windows.h>

typedef struct GfxD2D GfxD2D;

#define GFX_FMT_LABEL  0
#define GFX_FMT_CLOSE  1
#define GFX_FMT_PLUS   2
#define GFX_FMT_WGLYPH 3
#define GFX_FMT_MARK   4
#define GFX_FMT_COUNT  5

int  gfx_global_init(void);
void gfx_global_shutdown(void);

GfxD2D* gfx_create(HWND hwnd, int dpi);
void    gfx_destroy(GfxD2D* g);
void    gfx_resize(GfxD2D* g, int w, int h);
void    gfx_set_dpi(GfxD2D* g, int dpi);

int  gfx_begin(GfxD2D* g, unsigned bg);   /* COLORREF bg; 1 => proceed */
int  gfx_end(GfxD2D* g);                  /* 1 => recreate target */

void gfx_fill_rect (GfxD2D* g, RECT r, unsigned col);
void gfx_fill_round(GfxD2D* g, RECT r, int radius, unsigned col);
void gfx_fill_tab  (GfxD2D* g, RECT r, int radius, unsigned col);
void gfx_text      (GfxD2D* g, const wchar_t* s, RECT r, int fmt, unsigned col);

#endif /* GFX_D2D_H */
```

- [ ] **Step 2: Write the module**

Create `src/gfx_d2d.c`:

```c
/* Direct2D/DirectWrite chrome renderer. Pure-C use of the COM APIs:
 * COBJMACROS gives the ID2D1X_Method(This, ...) call forms, and INITGUID
 * (defined here, in exactly one TU) instantiates IID_ID2D1Factory /
 * IID_IDWriteFactory so they link. The hwnd render target is pinned to 96 dpi
 * so 1 DIP = 1 physical pixel; callers pass already-DPI-scaled pixel rects. */
#define COBJMACROS
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dxgiformat.h>
#include <string.h>
#include <stdlib.h>
#include "gfx_d2d.h"

static ID2D1Factory*   g_d2d = NULL;
static IDWriteFactory* g_dw  = NULL;

struct GfxD2D {
    HWND hwnd;
    int  dpi;
    ID2D1HwndRenderTarget* rt;
    ID2D1SolidColorBrush*  brush;
    IDWriteTextFormat*     fmt[GFX_FMT_COUNT];
};

int gfx_global_init(void) {
    if (!g_d2d) {
        D2D1_FACTORY_OPTIONS o; o.debugLevel = D2D1_DEBUG_LEVEL_NONE;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                     &IID_ID2D1Factory, &o, (void**)&g_d2d)))
            return 0;
    }
    if (!g_dw) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                       &IID_IDWriteFactory, (IUnknown**)&g_dw)))
            return 0;
    }
    return 1;
}

void gfx_global_shutdown(void) {
    if (g_dw)  { IDWriteFactory_Release(g_dw);  g_dw  = NULL; }
    if (g_d2d) { ID2D1Factory_Release(g_d2d);   g_d2d = NULL; }
}

static D2D1_COLOR_F col_f(unsigned c) {
    D2D1_COLOR_F f;
    f.r = GetRValue(c) / 255.0f;
    f.g = GetGValue(c) / 255.0f;
    f.b = GetBValue(c) / 255.0f;
    f.a = 1.0f;
    return f;
}

static void make_fmt(GfxD2D* g, int i, const wchar_t* face,
                     DWRITE_FONT_WEIGHT w, int base_px,
                     DWRITE_TEXT_ALIGNMENT ta, int ellipsis) {
    float size = (float)MulDiv(base_px, g->dpi, 96);
    IDWriteTextFormat* f = NULL;
    if (FAILED(IDWriteFactory_CreateTextFormat(g_dw, face, NULL, w,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size, L"", &f)) || !f) { g->fmt[i] = NULL; return; }
    IDWriteTextFormat_SetTextAlignment(f, ta);
    IDWriteTextFormat_SetParagraphAlignment(f, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    IDWriteTextFormat_SetWordWrapping(f, DWRITE_WORD_WRAPPING_NO_WRAP);
    if (ellipsis) {
        DWRITE_TRIMMING tr; memset(&tr, 0, sizeof tr);
        tr.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        IDWriteInlineObject* sign = NULL;
        IDWriteFactory_CreateEllipsisTrimmingSign(g_dw, f, &sign);
        IDWriteTextFormat_SetTrimming(f, &tr, sign);
        if (sign) IDWriteInlineObject_Release(sign);
    }
    g->fmt[i] = f;
}

static void make_all_formats(GfxD2D* g) {
    make_fmt(g, GFX_FMT_LABEL,  L"Segoe UI",           DWRITE_FONT_WEIGHT_SEMI_BOLD, 14, DWRITE_TEXT_ALIGNMENT_LEADING, 1);
    make_fmt(g, GFX_FMT_CLOSE,  L"Segoe UI",           DWRITE_FONT_WEIGHT_NORMAL,    11, DWRITE_TEXT_ALIGNMENT_CENTER,  0);
    make_fmt(g, GFX_FMT_PLUS,   L"Segoe UI",           DWRITE_FONT_WEIGHT_NORMAL,    15, DWRITE_TEXT_ALIGNMENT_CENTER,  0);
    make_fmt(g, GFX_FMT_WGLYPH, L"Segoe MDL2 Assets",  DWRITE_FONT_WEIGHT_NORMAL,    10, DWRITE_TEXT_ALIGNMENT_CENTER,  0);
    make_fmt(g, GFX_FMT_MARK,   L"Segoe UI",           DWRITE_FONT_WEIGHT_SEMI_BOLD, 15, DWRITE_TEXT_ALIGNMENT_CENTER,  0);
}

static void free_formats(GfxD2D* g) {
    for (int i = 0; i < GFX_FMT_COUNT; i++)
        if (g->fmt[i]) { IDWriteTextFormat_Release(g->fmt[i]); g->fmt[i] = NULL; }
}

GfxD2D* gfx_create(HWND hwnd, int dpi) {
    if (!g_d2d || !g_dw) return NULL;
    GfxD2D* g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->hwnd = hwnd;
    g->dpi  = dpi ? dpi : 96;

    RECT rc; GetClientRect(hwnd, &rc);
    D2D1_SIZE_U size;
    size.width  = (UINT32)(rc.right  - rc.left);
    size.height = (UINT32)(rc.bottom - rc.top);
    if (size.width  == 0) size.width  = 1;
    if (size.height == 0) size.height = 1;

    D2D1_RENDER_TARGET_PROPERTIES rtp; memset(&rtp, 0, sizeof rtp);
    rtp.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    rtp.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    rtp.dpiX = 96.0f;   /* pin: 1 DIP = 1 physical pixel; we scale ourselves */
    rtp.dpiY = 96.0f;
    rtp.usage    = D2D1_RENDER_TARGET_USAGE_NONE;
    rtp.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;

    D2D1_HWND_RENDER_TARGET_PROPERTIES hp; memset(&hp, 0, sizeof hp);
    hp.hwnd = hwnd;
    hp.pixelSize = size;
    hp.presentOptions = D2D1_PRESENT_OPTIONS_NONE;

    if (FAILED(ID2D1Factory_CreateHwndRenderTarget(g_d2d, &rtp, &hp, &g->rt)) || !g->rt) {
        free(g); return NULL;
    }
    D2D1_COLOR_F black = { 0, 0, 0, 1 };
    ID2D1RenderTarget_CreateSolidColorBrush((ID2D1RenderTarget*)g->rt, &black, NULL, &g->brush);
    make_all_formats(g);
    return g;
}

void gfx_destroy(GfxD2D* g) {
    if (!g) return;
    free_formats(g);
    if (g->brush) ID2D1SolidColorBrush_Release(g->brush);
    if (g->rt)    ID2D1HwndRenderTarget_Release(g->rt);
    free(g);
}

void gfx_resize(GfxD2D* g, int w, int h) {
    if (!g || !g->rt) return;
    D2D1_SIZE_U s; s.width = (UINT32)(w > 0 ? w : 1); s.height = (UINT32)(h > 0 ? h : 1);
    ID2D1HwndRenderTarget_Resize(g->rt, &s);
}

void gfx_set_dpi(GfxD2D* g, int dpi) {
    if (!g) return;
    g->dpi = dpi ? dpi : 96;
    free_formats(g);
    make_all_formats(g);
}

int gfx_begin(GfxD2D* g, unsigned bg) {
    if (!g || !g->rt) return 0;
    ID2D1RenderTarget* rt = (ID2D1RenderTarget*)g->rt;
    ID2D1RenderTarget_BeginDraw(rt);
    D2D1_COLOR_F c = col_f(bg);
    ID2D1RenderTarget_Clear(rt, &c);
    return 1;
}

int gfx_end(GfxD2D* g) {
    if (!g || !g->rt) return 0;
    HRESULT hr = ID2D1RenderTarget_EndDraw((ID2D1RenderTarget*)g->rt, NULL, NULL);
    return (hr == D2DERR_RECREATE_TARGET) ? 1 : 0;
}

static void set_col(GfxD2D* g, unsigned col) {
    D2D1_COLOR_F c = col_f(col);
    ID2D1SolidColorBrush_SetColor(g->brush, &c);
}

void gfx_fill_rect(GfxD2D* g, RECT r, unsigned col) {
    if (!g || !g->rt) return;
    set_col(g, col);
    D2D1_RECT_F rf = { (float)r.left, (float)r.top, (float)r.right, (float)r.bottom };
    ID2D1RenderTarget_FillRectangle((ID2D1RenderTarget*)g->rt, &rf, (ID2D1Brush*)g->brush);
}

void gfx_fill_round(GfxD2D* g, RECT r, int radius, unsigned col) {
    if (!g || !g->rt) return;
    set_col(g, col);
    D2D1_ROUNDED_RECT rr;
    rr.rect.left = (float)r.left; rr.rect.top = (float)r.top;
    rr.rect.right = (float)r.right; rr.rect.bottom = (float)r.bottom;
    rr.radiusX = (float)radius; rr.radiusY = (float)radius;
    ID2D1RenderTarget_FillRoundedRectangle((ID2D1RenderTarget*)g->rt, &rr, (ID2D1Brush*)g->brush);
}

/* Tab shape: both top corners rounded, bottom square. Built as a closed path:
 * bottom-left -> up left edge -> arc top-left -> top edge -> arc top-right ->
 * down right edge -> (close back along the bottom). */
void gfx_fill_tab(GfxD2D* g, RECT r, int radius, unsigned col) {
    if (!g || !g->rt || !g_d2d) return;
    ID2D1PathGeometry* geo = NULL;
    if (FAILED(ID2D1Factory_CreatePathGeometry(g_d2d, &geo)) || !geo) return;
    ID2D1GeometrySink* sink = NULL;
    if (FAILED(ID2D1PathGeometry_Open(geo, &sink)) || !sink) {
        ID2D1PathGeometry_Release(geo); return;
    }
    float L = (float)r.left, T = (float)r.top, R = (float)r.right, B = (float)r.bottom;
    float rad = (float)radius;
    D2D1_POINT_2F p;
    p.x = L; p.y = B;      ID2D1GeometrySink_BeginFigure(sink, p, D2D1_FIGURE_BEGIN_FILLED);
    p.x = L; p.y = T + rad; ID2D1GeometrySink_AddLine(sink, p);

    D2D1_ARC_SEGMENT a;
    a.size.width = rad; a.size.height = rad;
    a.rotationAngle = 0.0f;
    a.sweepDirection = D2D1_SWEEP_DIRECTION_CLOCKWISE;
    a.arcSize = D2D1_ARC_SIZE_SMALL;

    a.point.x = L + rad; a.point.y = T; ID2D1GeometrySink_AddArc(sink, &a);
    p.x = R - rad; p.y = T; ID2D1GeometrySink_AddLine(sink, p);
    a.point.x = R; a.point.y = T + rad; ID2D1GeometrySink_AddArc(sink, &a);
    p.x = R; p.y = B; ID2D1GeometrySink_AddLine(sink, p);

    ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
    ID2D1GeometrySink_Close(sink);

    set_col(g, col);
    ID2D1RenderTarget_FillGeometry((ID2D1RenderTarget*)g->rt,
        (ID2D1Geometry*)geo, (ID2D1Brush*)g->brush, NULL);

    ID2D1GeometrySink_Release(sink);
    ID2D1PathGeometry_Release(geo);
}

void gfx_text(GfxD2D* g, const wchar_t* s, RECT r, int fmt, unsigned col) {
    if (!g || !g->rt || fmt < 0 || fmt >= GFX_FMT_COUNT || !g->fmt[fmt] || !s) return;
    set_col(g, col);
    D2D1_RECT_F rf = { (float)r.left, (float)r.top, (float)r.right, (float)r.bottom };
    ID2D1RenderTarget_DrawText((ID2D1RenderTarget*)g->rt, s, (UINT32)wcslen(s),
        g->fmt[fmt], &rf, (ID2D1Brush*)g->brush,
        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}
```

- [ ] **Step 3: Wire the module into the build**

In `Makefile`, add the library flags — replace:
```make
LDLIBS  := -lgdi32 -lcomctl32 -lole32 -lshell32 -ldwmapi
```
with:
```make
LDLIBS  := -lgdi32 -lcomctl32 -lole32 -lshell32 -ldwmapi -ld2d1 -ldwrite
```

And add the source — replace:
```make
APP_SRC := src/main.c src/note_window.c src/tray.c
```
with:
```make
APP_SRC := src/main.c src/note_window.c src/tray.c src/gfx_d2d.c
```

- [ ] **Step 4: Build (link check)**

Run: `make clean && make`
Expected: `src/gfx_d2d.o` compiles and `quicknote.exe` links with no undefined-reference errors (confirms `-ld2d1 -ldwrite` + `INITGUID` are correct). The module is not called yet, so runtime behavior is unchanged.

Run: `make test`
Expected: existing suite passes (test binary does not link gfx_d2d).

- [ ] **Step 5: Commit**

```bash
git add src/gfx_d2d.h src/gfx_d2d.c Makefile
git commit -m "feat(gfx): add Direct2D/DirectWrite chrome renderer module"
```

---

### Task 4: Paint the chrome (dividers, qN, tabs, ✕, +) through Direct2D

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: everything from Task 3 (`gfx_*`), and the DPI helpers from Task 2.
- Produces:
  - `NoteWin.gfx` field (`GfxD2D*`).
  - `nw_paint_chrome(NoteWin*, HWND)` — draws dividers + qN + tabs + ✕ + rename frame via `nw->gfx` (window buttons still painted by the child controls in this task).
- The old GDI helpers `nw_fill_round` / `nw_fill_round_top` are deleted.

- [ ] **Step 1: Include the module and add the handle field**

At the top of `src/note_window.c`, after `#include "resource.h"` (~line 4) add:
```c
#include "gfx_d2d.h"
```

In `struct NoteWin` (~line 113, after `HFONT rename_font;` / near the other fields) add:
```c
    GfxD2D* gfx;        /* Direct2D chrome renderer; lazy-created in WM_PAINT */
```

- [ ] **Step 2: Initialize/shutdown the D2D globals with the class**

In `note_window_register_class` (~line 1691), after the `g_border_brush` creation line add:
```c
    gfx_global_init();
```
(`gfx_global_init` is idempotent and safe even if D2D is unavailable — `gfx_create` will simply return NULL and paint is skipped.)

There is no matching class-unregister function; the process-wide factories are released at exit by the OS. Leave `gfx_global_shutdown` available for future use (it is declared in the header) but it need not be called.

- [ ] **Step 3: Delete the GDI rounded-fill helpers**

Delete `nw_fill_round` (~lines 1203-1210) and `nw_fill_round_top` (~lines 1218-1228) entirely. They are replaced by `gfx_fill_round` / `gfx_fill_tab`.

- [ ] **Step 4: Rewrite `nw_paint_tabs` as D2D drawing**

Replace the entire `nw_paint_tabs` function (~lines 1234-1348) with a D2D version that draws only the tab strip pieces (tabs, ✕, +, rename frame). It uses `nw->gfx`:

```c
/* Draw the tab strip with Direct2D: + button fill behind, tab fills back-to-
 * front (top corners rounded, antialiased), then labels, ✕ glyphs, and the +
 * glyph. Colors come from the existing palette. */
static void nw_paint_tabs(NoteWin* nw, HWND hwnd) {
    GfxD2D* g = nw->gfx;
    int radius = nw_s(hwnd, TAB_RADIUS);

    RECT pr; nw_plus_rect(nw, hwnd, &pr);
    if (nw->hot_plus) gfx_fill_tab(g, pr, radius, COL_HOVER);

    for (int i = nw->ntabs - 1; i >= 0; i--) {
        RECT r; nw_tab_rect(nw, hwnd, i, &r);
        int isact = (i == nw->active);
        int ishot = (i == nw->hot_tab);

        int fill_left = r.left;
        if (i > 0) fill_left -= radius;
        RECT shape = { fill_left, r.top + nw_s(hwnd, TAB_TOP_GAP), r.right, r.bottom - 1 };

        unsigned fill = COL_BG;
        if (isact)      fill = COL_TAB_ACT;
        else if (ishot) fill = COL_TAB_HOT;
        gfx_fill_tab(g, shape, radius, fill);
    }

    for (int i = 0; i < nw->ntabs; i++) {
        RECT r; nw_tab_rect(nw, hwnd, i, &r);
        int isact = (i == nw->active);
        int ishot = (i == nw->hot_tab);

        NoteMeta* m = nw_note_meta(nw, i);
        wchar_t wname[128];
        MultiByteToWideChar(CP_ACP, 0, m && m->name[0] ? m->name : "Untitled", -1, wname, 128);

        RECT tr = { r.left + nw_s(hwnd, 12), r.top + nw_s(hwnd, TAB_TOP_GAP),
                    r.right - nw_s(hwnd, TAB_CLOSE_W) - nw_s(hwnd, 4), r.bottom - 1 };
        unsigned lc = isact ? COL_TEXT
                    : (ishot ? RGB(0xc9,0xc9,0xd0) : RGB(0x8a,0x8a,0x90));
        gfx_text(g, wname, tr, GFX_FMT_LABEL, lc);

        RECT xr; nw_close_rect(nw, hwnd, i, &xr);
        int overx = (ishot && nw->hot_close);
        if (overx) gfx_fill_tab(g, xr, radius, RGB(0x3a,0x3a,0x40));
        gfx_text(g, L"\x2715", xr, GFX_FMT_CLOSE, overx ? RGB(0xff,0xff,0xff) : RGB(0xc0,0xc0,0xc8));
    }

    RECT pg = pr; pg.left += radius;
    gfx_text(g, L"\x002B", pg, GFX_FMT_PLUS, nw->hot_plus ? RGB(0xff,0xff,0xff) : RGB(0x9a,0x9a,0xa2));

    if (nw->rename_edit) {
        RECT er; nw_rename_rect(nw, hwnd, &er);
        RECT fr = { er.left - 2, er.top - 2, er.right + 2, er.bottom + 2 };
        gfx_fill_round(g, fr, nw_s(hwnd, 5), COL_ACCENT);
    }
}
```

- [ ] **Step 5: Add `nw_paint_chrome` (dividers + qN + tabs)**

Immediately after `nw_paint_tabs`, add the top-level chrome painter that replaces the old `WM_PAINT` GDI body:

```c
/* Full chrome paint (inside gfx_begin/gfx_end): dividers, the qN wordmark in
 * the top-left cell, and the tab strip. Window min/max/close buttons are still
 * child controls in this task (painted by WM_DRAWITEM). */
static void nw_paint_chrome(NoteWin* nw, HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int vx, hy; nw_regions(hwnd, NULL, NULL, &vx, &hy);

    RECT vline = { vx, 0, vx + 1, rc.bottom };
    RECT hline = { 0, hy, rc.right, hy + 1 };
    gfx_fill_rect(nw->gfx, vline, COL_BORDER);
    gfx_fill_rect(nw->gfx, hline, COL_BORDER);

    RECT mark = { 0, 0, vx, hy };
    gfx_text(nw->gfx, L"qN", mark, GFX_FMT_MARK, COL_TEXT);

    nw_paint_tabs(nw, hwnd);
}
```

- [ ] **Step 6: Rewrite `WM_PAINT`**

Replace the `WM_PAINT` case (~lines 1429-1448) with:

```c
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps);
        if (nw) {
            if (!nw->gfx) nw->gfx = gfx_create(hwnd, nw_dpi(hwnd));
            if (nw->gfx && gfx_begin(nw->gfx, COL_BG)) {
                nw_paint_chrome(nw, hwnd);
                if (gfx_end(nw->gfx)) {          /* target lost: rebuild next paint */
                    gfx_destroy(nw->gfx);
                    nw->gfx = NULL;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
```

- [ ] **Step 7: Resize the render target on `WM_SIZE`**

In the `WM_SIZE` case (~lines 1422-1428), after `nw_layout(hwnd, nw);` add a resize call. Replace the case body with:

```c
    case WM_SIZE:
        if (nw) {
            if (nw->gfx) {
                RECT rc; GetClientRect(hwnd, &rc);
                gfx_resize(nw->gfx, rc.right - rc.left, rc.bottom - rc.top);
            }
            nw_layout(hwnd, nw);
            InvalidateRect(hwnd, NULL, TRUE);
            if (nw->wbtns[1]) InvalidateRect(nw->wbtns[1], NULL, TRUE);
        }
        return 0;
```

- [ ] **Step 8: Destroy the render target on `WM_DESTROY`**

In the `WM_DESTROY` case (~lines 1679-1686), inside `if (nw) {`, before `free(nw);` add:
```c
            if (nw->gfx) { gfx_destroy(nw->gfx); nw->gfx = NULL; }
```

- [ ] **Step 9: Drop the now-unused `hdc`-based icon include if needed**

No include changes required (`DrawIconEx`/`g_app_icon` remain declared but the icon is no longer painted here; `g_app_icon` is still assigned in `register_class` and used nowhere else — that is fine, leave it). Confirm the build has no "unused" errors (only warnings at most; `-Wall -Wextra` treats unused statics as warnings, not errors). If `g_app_icon`/`DrawIconEx` produce an unused warning, that is acceptable for this task and cleaned up naturally — do not add code to suppress it.

- [ ] **Step 10: Build and verify**

Run: `make clean && make`
Expected: builds, no errors.

Run: `make test`
Expected: existing suite passes.

Launch fresh. Confirm:
- Dividers, the "qN" mark, tabs, labels, ✕ and + all render.
- Tab top corners are now smooth (antialiased), not jagged.
- Hovering tabs / ✕ / + still highlights; clicking still activates/closes/adds tabs.
- Resizing the window shows no flicker in the titlebar.
The min/max/close buttons still look the same (still GDI child controls) — expected; migrated in Task 5.

Screenshot for the record.

- [ ] **Step 11: Commit**

```bash
git add src/note_window.c
git commit -m "feat(gfx): render titlebar chrome (tabs, dividers, qN) via Direct2D"
```

---

### Task 5: Move min/max/close into the Direct2D chrome

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: Task 4's `nw_paint_chrome`, Task 2's `nw_s` and `nw_window_buttons_left`.
- Produces:
  - `nw_wbtn_rect(HWND, int i, RECT* r)` — hit/draw rect for window button i (0=min,1=max,2=close), right-aligned.
  - `nw_wbtn_hit(HWND, int cx, int cy)` — returns button index 0..2 or -1.
  - Window buttons are no longer child HWNDs; `NoteWin.wbtns` is removed. `whot[]` stays (now driven by parent hit-testing).

- [ ] **Step 1: Remove the child-button state field**

In `struct NoteWin`, delete the line:
```c
    HWND  wbtns[NUM_WBTNS];
```
Keep `int whot[NUM_WBTNS];` (still used for hover state).

- [ ] **Step 2: Delete the child-button draw + subclass code**

Delete these entirely:
- `nw_draw_wbutton` (~lines 377-404).
- `nw_wbtn_sub` (~lines 408-424).

- [ ] **Step 3: Add window-button geometry + hit-test**

Add, right after `nw_window_buttons_left` (from Task 2):

```c
/* Rect of window button i (0=min, 1=max, 2=close), right-aligned in the
 * titlebar, each WINBTN_W wide and the full titlebar tall. */
static void nw_wbtn_rect(HWND hwnd, int i, RECT* r) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = nw_s(hwnd, WINBTN_W);
    int right = rc.right - (NUM_WBTNS - 1 - i) * w;
    SetRect(r, right - w, 0, right, nw_titlebar_h(hwnd));
}

/* Window button under (cx,cy) in client coords, or -1. */
static int nw_wbtn_hit(HWND hwnd, int cx, int cy) {
    for (int i = 0; i < NUM_WBTNS; i++) {
        RECT r; nw_wbtn_rect(hwnd, i, &r);
        if (cx >= r.left && cx < r.right && cy >= r.top && cy < r.bottom) return i;
    }
    return -1;
}
```

- [ ] **Step 4: Stop creating the child buttons**

In `WM_CREATE`, delete the whole block that creates the window-chrome buttons — the `wbtndefs` array and its `CreateWindowExW` / `SetWindowSubclass` loop (~lines 1378-1387). (Keep the sidebar `btns[]` creation loop above it untouched.)

- [ ] **Step 5: Stop laying out the child buttons**

In `nw_layout` (from Task 2 Step 6), delete the window-button loop:
```c
    int wbtn_w = nw_s(hwnd, WINBTN_W);
    for (int i = 0; i < NUM_WBTNS; i++) {
        MoveWindow(nw->wbtns[i], rc.right - (NUM_WBTNS - i) * wbtn_w, 0,
                   wbtn_w, nw_titlebar_h(hwnd), TRUE);
    }
```
so `nw_layout` now positions only the sidebar buttons and the tab edits.

- [ ] **Step 6: Remove the `wbtns` invalidate in `WM_SIZE`**

In `WM_SIZE` (from Task 4 Step 7), delete the line:
```c
            if (nw->wbtns[1]) InvalidateRect(nw->wbtns[1], NULL, TRUE);
```
(The whole titlebar already repaints via `InvalidateRect(hwnd, NULL, TRUE)`.)

- [ ] **Step 7: Remove the window-button branch from `WM_DRAWITEM`**

In `WM_DRAWITEM` (~lines 1637-1646), delete the second `if` that dispatched to `nw_draw_wbutton`:
```c
        if (nw && wp >= IDW_MIN && wp <= IDW_CLOSE) {
            nw_draw_wbutton(nw, hwnd, (const DRAWITEMSTRUCT*)lp);
            return TRUE;
        }
```
Keep the sidebar `IDB_BOLD..IDB_NUM` branch.

- [ ] **Step 8: Remove the window-button `WM_COMMAND` cases**

In `WM_COMMAND` (~lines 1655-1657), delete the three cases:
```c
            case IDW_MIN:    ShowWindow(hwnd, SW_MINIMIZE); return 0;
            case IDW_MAX:    ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); return 0;
            case IDW_CLOSE:  SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
```
(The button actions move to `WM_LBUTTONDOWN`, Step 11.)

- [ ] **Step 9: Draw the window buttons in `nw_paint_chrome`**

In `nw_paint_chrome` (Task 4 Step 5), before the `nw_paint_tabs(nw, hwnd);` call, add the window-button drawing:

```c
    for (int i = 0; i < NUM_WBTNS; i++) {
        RECT wr; nw_wbtn_rect(hwnd, i, &wr);
        int is_close = (i == 2);
        unsigned fill = COL_BG;
        if (is_close) { if (nw->whot[i]) fill = COL_CLOSE; }
        else if (nw->whot[i]) fill = COL_HOVER;
        if (fill != COL_BG) gfx_fill_rect(nw->gfx, wr, fill);

        const wchar_t* glyph = L"\xE921";                 /* min */
        if (i == 1) glyph = IsZoomed(hwnd) ? L"\xE923" : L"\xE922";  /* restore/max */
        if (i == 2) glyph = L"\xE8BB";                    /* close */
        gfx_text(nw->gfx, glyph, wr, GFX_FMT_WGLYPH, COL_TEXT);
    }
```

- [ ] **Step 10: Track hover for the window buttons in `WM_MOUSEMOVE` / `WM_MOUSELEAVE`**

In the hover branch of `WM_MOUSEMOVE` (~lines 1573-1586, the `if (nw) { ... hover highlight ... }` block that is not the drag branch), add window-button hover tracking. Right after the existing tab-hover `TabHit h = nw_tab_hit(...)` computation and its `InvalidateRect`, extend the block so it also updates `whot[]`. Replace that hover block with:

```c
        if (nw) {                                     /* hover highlight (WT look) */
            int idx = -1, ht = -1, hc = 0, hp = 0;
            TabHit h = nw_tab_hit(nw, hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &idx);
            if (h == HIT_TAB)        ht = idx;
            else if (h == HIT_CLOSE) { ht = idx; hc = 1; }
            else if (h == HIT_PLUS)  hp = 1;

            int wb = nw_wbtn_hit(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            int wchanged = 0;
            for (int i = 0; i < NUM_WBTNS; i++) {
                int on = (i == wb);
                if (on != nw->whot[i]) { nw->whot[i] = on; wchanged = 1; }
            }

            if (ht != nw->hot_tab || hc != nw->hot_close || hp != nw->hot_plus || wchanged) {
                nw->hot_tab = ht; nw->hot_close = hc; nw->hot_plus = hp;
                RECT bar; GetClientRect(hwnd, &bar); bar.bottom = nw_titlebar_h(hwnd);
                InvalidateRect(hwnd, &bar, TRUE);
            }
            TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
```

In `WM_MOUSELEAVE` (~lines 1588-1594), also clear the window-button hover. Replace the case body with:

```c
    case WM_MOUSELEAVE:
        if (nw) {
            int wchanged = 0;
            for (int i = 0; i < NUM_WBTNS; i++)
                if (nw->whot[i]) { nw->whot[i] = 0; wchanged = 1; }
            if (nw->hot_tab != -1 || nw->hot_close || nw->hot_plus || wchanged) {
                nw->hot_tab = -1; nw->hot_close = 0; nw->hot_plus = 0;
                RECT bar; GetClientRect(hwnd, &bar); bar.bottom = nw_titlebar_h(hwnd);
                InvalidateRect(hwnd, &bar, TRUE);
            }
        }
        return 0;
```

- [ ] **Step 11: Handle window-button clicks in `WM_LBUTTONDOWN`**

In `WM_LBUTTONDOWN` (~lines 1530-1547), at the very top of `if (nw) {`, before the `nw_tab_hit` call, add:

```c
            int wb = nw_wbtn_hit(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (wb == 0) { ShowWindow(hwnd, SW_MINIMIZE); return 0; }
            if (wb == 1) { ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); return 0; }
            if (wb == 2) { SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
```

- [ ] **Step 12: Confirm `WM_NCHITTEST` still routes the button region to HTCLIENT**

No change needed: the existing branch returns `HTCLIENT` for `pt.x >= btns_l` within the titlebar (~lines 1484-1487), so button clicks reach `WM_LBUTTONDOWN`. Verify the branch is intact after edits.

Run: `grep -n "pt.x >= btns_l" src/note_window.c`
Expected: one match inside `WM_NCHITTEST`.

- [ ] **Step 13: Build and verify**

Run: `make clean && make`
Expected: no errors, no references to `wbtns`, `nw_draw_wbutton`, or `nw_wbtn_sub` remain.

Run: `grep -n "wbtns\|nw_draw_wbutton\|nw_wbtn_sub" src/note_window.c`
Expected: no matches.

Run: `make test`
Expected: existing suite passes.

Launch fresh. Confirm:
- min/max/close render in the top-right, same look, drawn by D2D.
- Hover highlights each (close → red); pressing/clicking minimizes, maximizes/restores (glyph toggles), and closes.
- Maximize/restore glyph updates on `WM_SIZE`.

Screenshot for the record.

- [ ] **Step 14: Commit**

```bash
git add src/note_window.c
git commit -m "feat(gfx): draw min/max/close via Direct2D, drop child button controls"
```

---

### Task 6: WM_DPICHANGED live rescale

**Files:**
- Modify: `src/note_window.c`

**Interfaces:**
- Consumes: `gfx_set_dpi` (Task 3), `nw_layout` (Task 2), `nw_apply_format_edit` (existing), `nw->gfx` (Task 4).
- Produces: correct live rescale when the window moves between monitors of different scale.

- [ ] **Step 1: Add the handler**

In `nw_proc`, add a new case (place it next to `WM_SIZE`, before `WM_PAINT`):

```c
    case WM_DPICHANGED:
        if (nw) {
            RECT* sug = (RECT*)lp;             /* OS-suggested position + size */
            SetWindowPos(hwnd, NULL, sug->left, sug->top,
                         sug->right - sug->left, sug->bottom - sug->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            if (nw->gfx) gfx_set_dpi(nw->gfx, nw_dpi(hwnd));   /* rescale text formats */
            for (int i = 0; i < nw->ntabs; i++) {
                /* re-establish body char formats at the new DPI, then restyle */
                CHARFORMAT2W cf; memset(&cf, 0, sizeof cf);
                cf.cbSize = sizeof cf; cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
                cf.crTextColor = COL_TEXT; cf.yHeight = nw_body_yheight(hwnd);
                wcscpy(cf.szFaceName, L"IBM Plex Mono");
                SendMessageW(nw->tab[i].edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
                nw_apply_format_edit(nw, i);
            }
            nw_layout(hwnd, nw);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
```

The `SetWindowPos` triggers `WM_SIZE`, which already calls `gfx_resize` + `nw_layout` (Task 4). Re-calling `nw_layout` here is harmless and keeps the handler correct even if the size is unchanged.

- [ ] **Step 2: Build and verify**

Run: `make clean && make`
Expected: no errors.

Run: `make test`
Expected: existing suite passes.

Verification (needs a mixed-DPI multi-monitor setup; if unavailable, verify single-monitor by changing the display scale to 150% while the app runs and dragging, then note in the commit that multi-monitor drag was not testable here):
- Launch fresh on a 100% monitor.
- Drag the window onto a 150% monitor. The chrome (tabs, labels, buttons, qN) and the body text rescale crisply with no bitmap blur, and layout stays correct.
- Drag back to 100%; it rescales down cleanly.

Screenshot both DPIs for the record.

- [ ] **Step 3: Commit**

```bash
git add src/note_window.c
git commit -m "feat(dpi): rescale chrome and body on WM_DPICHANGED"
```

---

## Self-Review Notes

**Spec coverage:**
- §1 manifest → Task 1. §2 constant scaling → Task 2. §3 WM_DPICHANGED → Task 6. §4 Direct2D module → Task 3; chrome paint (dividers/qN/tabs/✕/+) → Task 4; min/max/close migration → Task 5; per-window text formats + DPI-recreate → Task 3 (`gfx_set_dpi`) wired in Task 6. §5 build (`-ld2d1 -ldwrite`, `INITGUID`, `app.rc`) → Tasks 1 + 3. §6 testing → per-task verify steps.
- Sidebar B/I/S staying GDI (spec "out of scope") → respected; only their fonts are DPI-scaled (Task 2 Step 7).
- 96-DPI pin decision → enforced in `gfx_create` (`rtp.dpiX/Y = 96`) and never overridden.

**Type consistency:** `gfx_*` signatures used in Tasks 4–6 match the header defined in Task 3. `nw_strip_left(HWND)` / `nw_window_buttons_width(HWND)` signature changes (Task 2) are reflected at all call sites (Task 2 Step 9). `nw_wbtn_rect` / `nw_wbtn_hit` defined and used within Task 5. `nw_body_yheight` defined in Task 2 Step 7 and reused in Task 6.

**Testing caveat:** GUI paint has no automated tests; the regression gate is the existing `make test` logic suite (must stay green) plus manual launch/screenshot at each task. This is stated in Global Constraints and repeated per task.
