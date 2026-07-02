# Design: DPI-aware + Direct2D chrome rendering

Date: 2026-07-02
Status: Approved (pending spec review)

## Problem

The sticky-note window draws its custom chrome (title bar, tabs, dividers, hover
squares) with GDI. Two quality defects:

1. **No DPI awareness.** The process declares no DPI awareness, so on any monitor
   at >100% scaling Windows bitmap-stretches the entire window like an image —
   blurry text and chrome.
2. **Aliased rounded fills.** Rounded tab tops and the ✕ / + hover squares are
   filled through `CreateRoundRectRgn` + `FillRgn` (`nw_fill_round`,
   `nw_fill_round_top`), which has no antialiasing — visibly jagged corners.

Goal: render the chrome at native resolution on every monitor, with
antialiased rounded corners, and stop the title-bar paint from flickering during
resize/hover.

## Constraints

- Pure C (gcc 13.2 / MSYS2 mingw-w64, `-std=c11`, `-mwindows`).
- Existing custom frame: `WM_NCCALCSIZE` claims the caption area,
  `DwmExtendFrameIntoClientArea` with `MARGINS {0,0,1,0}` keeps the resize frame
  and shadow. This stays.
- RichEdit body controls and owner-drawn child buttons are separate child HWNDs
  that self-paint; they are not part of the parent `WM_PAINT`.

## Chosen approach

**Direct2D + DirectWrite for the parent-window chrome paint, PerMonitorV2 DPI
awareness.** Direct2D's `ID2D1HwndRenderTarget` gives GPU-accelerated
antialiased geometry and presents atomically (inherently flicker-free, so it
also satisfies the double-buffering goal with no separate memory DC).

### Feasibility (verified)

- `d2d1.h` / `dwrite.h` compile as C with `#define COBJMACROS`.
- Links with `-ld2d1 -ldwrite`, provided `INITGUID` is defined in exactly one
  translation unit so the interface GUIDs (`IID_ID2D1Factory`,
  `IID_IDWriteFactory`) are instantiated.

### Alternatives considered

- **Supersample + GDI** (draw at 3–4× into a memory DC, `StretchBlt` down with
  `HALFTONE`): pure C, zero new deps, but pseudo-AA only and blurs text.
- **GDI+ flat C API**: real vector AA but awkward hand-declared prototypes in C
  and a new runtime dependency; not meaningfully simpler than D2D here.
- **Compile `note_window.c` as C++**: cleanest GDI+ usage but changes the
  translation unit / linkage of a large existing C file.

Direct2D was chosen as the best long-term rendering path.

## Components

### 1. DPI awareness — manifest (PerMonitorV2, modern only)

Add `assets/app.manifest`:

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

No legacy `<dpiAware>` fallback — modern PerMonitorV2 only.

Embed through the existing resource compile by adding to `assets/app.rc`:

```
1 RT_MANIFEST "app.manifest"
```

`windres` folds it into the existing `app_res.o`. The manifest applies before
any window is created — required for correct Per-Monitor behavior — so no
runtime `SetProcessDpiAwarenessContext` call is needed.

### 2. Scale all pixel constants by DPI

Constants remain 96-dpi base values. One helper pair routes every geometry site
through the window DPI:

```c
static int nw_dpi(HWND h){ UINT d = GetDpiForWindow(h); return d ? (int)d : 96; }
static int nw_s(HWND h, int v){ return MulDiv(v, nw_dpi(h), 96); }
```

Sites to convert (all geometry already funnels through a small set of helpers):
`nw_regions`, `nw_layout`, `nw_tab_w`, `nw_tab_rect`, `nw_close_rect`,
`nw_plus_rect`, `nw_strip_left/right`, `nw_tab_hit`, `nw_rename_rect`, plus the
new `nw_wbtn_rect` (§4); the sidebar button font `lfHeight` (`nw_draw_button`);
and the RichEdit `CHARFORMAT2W.yHeight` used at load/format time. (Chrome text
sizing moves to DirectWrite text formats, §4.)

**Explicit render-target DPI decision.** We deliberately work in *physical
pixel* coordinates and prevent Direct2D from applying a second DPI scale. After
creating the `ID2D1HwndRenderTarget` we call
`ID2D1RenderTarget_SetDpi(rt, 96.0f, 96.0f)`, so 1 DIP = 1 physical pixel. All
DPI scaling is done by us via `nw_s(...)`; Direct2D does none. This keeps paint
coordinates and GDI mouse hit-test coordinates in one shared pixel space.

> Do **not** "fix" this later by setting the render-target DPI to the monitor
> DPI. That would make Direct2D scale on top of our already-`nw_s`-scaled values
> and double-scale the entire chrome. The 96-DPI pin is intentional and load-
> bearing.

Because font sizes are baked as physical pixels (`nw_s(base)`), the DirectWrite
text formats are window-DPI-specific — see §4.

### 3. WM_DPICHANGED (Per-Monitor live rescale)

New handler in `nw_proc`:

1. `SetWindowPos` to the OS-suggested rect (`lParam` → `RECT*`, with
   `SWP_NOZORDER | SWP_NOACTIVATE`).
2. `nw_layout(hwnd, nw)` — re-reads DPI via `nw_s`, repositions children.
3. Recreate the per-window DirectWrite text formats at the new scaled sizes
   (chrome text — tab labels, ✕, +, min/max/close glyphs — is DPI-sized; see §4).
   Because DirectWrite owns all chrome text, this is a single "release + recreate
   the cached formats" step rather than chasing many `HFONT`s.
4. Reapply the body `CHARFORMAT2W` to each tab so RichEdit re-renders at the new
   DPI, and re-run `nw_apply_format_edit` so heading `yHeight`s track the change.
5. The sidebar B/I/S buttons (still GDI owner-draw) create their `HFONT` per
   `WM_DRAWITEM` from an `nw_s`-scaled `lfHeight`, so they self-correct on the
   next repaint — no cached font to refresh. `InvalidateRect(hwnd, NULL, TRUE)`.

The D2D target is pixel-based, so it only needs `Resize` (already handled in
`WM_SIZE`, which fires as part of the `SetWindowPos`).

### 4. Direct2D paint path — `src/gfx_d2d.{c,h}`

New module isolates all COM boilerplate behind a handle-based interface, keeping
`note_window.c` focused. `INITGUID` is defined at the top of `gfx_d2d.c`.

**Direct2D owns the entire titlebar/chrome**: titlebar background, dividers,
tabs, the + button, per-tab close ✕, and the window min/max/close buttons. The
sidebar B/I/S buttons and the RichEdit bodies remain child controls (self-
painting) — they are not chrome and stay as-is.

**Process-wide resources** (created in `note_window_register_class`, released at
shutdown): `ID2D1Factory`, `IDWriteFactory`.

**Per-window resources**: `ID2D1HwndRenderTarget` bound to the hwnd, created
lazily on first paint, `ID2D1RenderTarget_SetDpi(rt, 96, 96)` immediately after
creation (see §2), `ID2D1HwndRenderTarget_Resize` on `WM_SIZE`, released on
`WM_DESTROY`. Cached alongside the target: solid-color palette brushes, and the
`IDWriteTextFormat`s (tab label Segoe UI SemiBold, the ✕ / + glyphs, and the
Segoe MDL2 Assets min/max/close glyphs, plus the "qN" wordmark). Text-format
font sizes are `nw_s`-scaled physical pixels, so they are **per-window and
recreated on `WM_DPICHANGED`** (§3), not shared process-wide.

**Paint** (`WM_PAINT` → rewritten `nw_paint_tabs`, all in one `BeginDraw` pass):

1. `BeginDraw`.
2. `Clear` to `COL_BG` (titlebar background).
3. Dividers via `FillRectangle`.
4. "qN" wordmark in the top-left cell via DirectWrite `DrawText` (replaces the
   GDI icon interop — the mark is tiny and interop is not worth its complexity).
5. Tab fills: per-tab `ID2D1PathGeometry` with the top-left and top-right
   corners rounded via arc segments and square bottom (mirrors the current
   top-only-rounded shape, but antialiased). Built per tab per paint — a handful
   of tabs, negligible cost.
6. Hover squares (✕ / +) via `FillRoundedRectangle`.
7. Window min/max/close buttons: hover/pressed fills via `FillRectangle` (close
   → red on hover) then the Segoe MDL2 Assets glyphs via DirectWrite.
8. Labels and ✕ / + glyphs via DirectWrite `DrawText`, with the label format
   set to trim with an ellipsis sign to match the old `DT_END_ELLIPSIS`.
9. `EndDraw`; if it returns `D2DERR_RECREATE_TARGET`, release per-window
   resources and `InvalidateRect` to rebuild on the next paint.

`ID2D1HwndRenderTarget` presents atomically → flicker-free during resize/hover,
satisfying the double-buffering goal with no separate memory DC.

**Migrating min/max/close off child controls.** They are currently owner-draw
child `BUTTON` HWNDs (`nw->wbtns[]`) with a hover subclass (`nw_wbtn_sub`),
drawn in `WM_DRAWITEM` (`nw_draw_wbutton`) and actioned in `WM_COMMAND`. Moving
them into the D2D chrome pass removes all of that:

- Delete `wbtns[]` creation, the subclass, the `WM_DRAWITEM` window-button
  branch, and their `nw_layout` positioning.
- Add hit rects (an `nw_wbtn_rect(hwnd, i, *r)` helper, right-aligned like the
  old layout) and draw them in `nw_paint_tabs`.
- Hover: reuse the existing `whot[]` state, now updated from the parent's
  `WM_MOUSEMOVE` / `WM_MOUSELEAVE` hit-test (same pattern already used for tab
  hover) instead of the per-control subclass.
- Clicks: hit-test in `WM_LBUTTONDOWN`; min → `SW_MINIMIZE`, max → toggle
  `SW_MAXIMIZE`/`SW_RESTORE`, close → `WM_CLOSE`. `WM_NCHITTEST` already returns
  `HTCLIENT` for this region (the `pt.x >= btns_l` branch), so clicks reach the
  window proc — keep it.

**Deliberately out of scope**: the sidebar B/I/S / bullet / number buttons stay
GDI owner-draw child controls — they are not titlebar chrome, are flat
rectangles needing no AA, and already get DPI-scaled fonts per §2/§3.

### 5. Build

- `Makefile`: `LDLIBS += -ld2d1 -ldwrite`. New object `src/gfx_d2d.o` added to
  `APP_OBJ` / `APP_SRC`.
- `INITGUID` defined once, in `gfx_d2d.c`.
- Manifest: only the `app.rc` line changes; the existing `windres` rule and
  `app_res.o` target are unchanged.

## Testing / verification

- Pure-logic tests (`make test`) are unaffected — the changes are all GUI paint
  and process manifest.
- Manual verification (scripted GUI focus is blocked in this environment — verify
  by launching fresh and screenshotting):
  1. `make clean && make` builds with no new warnings.
  2. Launch at 100% scaling — chrome and tabs render identically to before, but
     with smooth rounded corners.
  3. Switch the monitor to 150% scaling, launch fresh — chrome is crisp (not
     bitmap-blurred), constants scale proportionally.
  4. On a mixed-DPI multi-monitor setup, drag the window across monitors — it
     rescales via `WM_DPICHANGED` without blur.
  5. Resize / hover tabs — no flicker.

## Risks

- Direct2D painting over the DWM-extended custom frame: expected fine (still
  ordinary client-area paint), but confirm the 1px `MARGINS` top strip still
  renders correctly.
- `WM_DPICHANGED` + RichEdit font scaling is the fiddliest area; the body font
  size must track DPI so headings don't look wrong after a monitor change.
- Migrating min/max/close from child controls to D2D-drawn regions: hover state
  now comes from parent `WM_MOUSEMOVE` hit-testing rather than a per-control
  subclass — verify hover/press feel matches the old buttons and that clicks
  still land (the `WM_NCHITTEST` `HTCLIENT` region must cover the new hit rects).
- Drawing "qN" as DirectWrite text sidesteps GDI icon interop. If interop is ever
  reintroduced (e.g. to draw the real icon), the render target must be created
  with `D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE` and the interop DC released
  before `EndDraw`, or the draw fails.
