# Tab Drag Slide Animation — Design

**Date:** 2026-07-13
**Status:** Approved (revised after independent referee review)

## Goal

Windows-Terminal-style animation for tab drag-to-reorder in the note window:

- The dragged tab follows the cursor pixel-exact and floats on top of its neighbors.
- Displaced tabs slide smoothly into their new slots instead of teleporting.
- On release, the dragged tab glides into its final slot (settle animation).

Scope is **drag only**. Tab open/close keeps instant reflow (the mechanism can be
extended later if wanted).

## Background

Drag-to-reorder already works (`src/note_window.c`, Task 9 of the tabbed-notes
plan): when the dragged tab's center crosses into another slot, `tab[]` is
shuffled and the whole window repaints. Tabs are laid out on a fixed slot grid
(`nw_tab_rect`), painted with Direct2D in `nw_paint_tabs`. There is no animation
infrastructure; the only timer precedent is the restyle debounce (`IDT_DEBOUNCE`).

## Approach

Chosen: **WM_TIMER at 10 ms + delta-time exponential easing** (single driver,
one code path, matches the existing debounce-timer pattern; ~64 Hz is smooth
enough for a 30-px-tall tab strip).

Rejected:
- *Mousemove-driven frames + settle-only timer* — smoother tracking while the
  mouse moves, but two code paths and easing tied to paint timing.
- *DirectComposition / render thread* — vsync-perfect, massive overkill for a
  single-threaded Win32 tab strip.

## State

New `NoteWin` fields:

```c
/* tab slide animation (drag reorder) */
float anim_x[WIN_MAX_TABS];  /* current draw-x of each tab's left edge */
int   anim_on;               /* 1 while IDT_TABANIM timer is alive */
LARGE_INTEGER anim_last;     /* QPC timestamp of the previous tick */
int   drag_x;                /* latest cursor x while dragging */
```

New constants: `IDT_TABANIM` (timer id), tick period 10 ms, easing time constant
`TAU` ≈ 70 ms, settle threshold 0.5 px.

## Behavior

1. **Drag begins** (existing 6-px threshold crossed): if `anim_on == 0`, seed
   `anim_x[i]` with each tab's slot-x; if a settle animation is still running,
   **keep the current `anim_x[]`** (re-grabbing mid-settle must not teleport
   sliding tabs). Compute `drag_dx` from the grabbed tab's *draw*-x, not its
   slot rect, so the tab doesn't jump under the cursor. Clear hover state
   (`hot_tab = -1`, `hot_close = 0`, `hot_plus = 0`) — the drag branch of
   WM_MOUSEMOVE returns before the hover code, so stale press-time hover would
   otherwise paint a displaced neighbor as hot while it slides. Stamp
   `anim_last` from QPC, `SetTimer(hwnd, IDT_TABANIM, 10, NULL)`, `anim_on = 1`.
2. **WM_MOUSEMOVE while dragging:** store `drag_x`. Target-slot computation is
   unchanged (`center / w`). On reorder, shuffle `tab[]` as today **and shuffle
   `anim_x[]` identically** — displaced tabs keep their current visual x and
   slide from there. Invalidate the tab-bar rect with erase = FALSE (replaces
   today's full-window `InvalidateRect(hwnd, NULL, TRUE)` — D2D clears and
   repaints the whole target every frame anyway, so the sub-rect + FALSE only
   avoids redundant GDI background erase).
3. **WM_TIMER tick:** compute dt via `QueryPerformanceCounter` against
   `anim_last`, clamp dt to ≤ 100 ms (absorbs modal/menu stalls that would
   otherwise make the first tick after a stall snap instantly). For every tab
   except the dragged one: `anim_x += (slot_x - anim_x) * (1 - expf(-dt/TAU))`.
   Invalidate the tab-bar rect, erase = FALSE. When all tabs are within 0.5 px
   of their slots and no drag is active: snap exact, `KillTimer`, `anim_on = 0`.
4. **WM_LBUTTONUP:** set `anim_x[drag_tab]` to the dragged tab's floating x at
   release, then clear `drag_tab`. The timer keeps running, so the released tab
   glides into its slot via the same tick path.
5. **WM_CAPTURECHANGED:** new handler. If `drag_tab >= 0`, treat exactly like
   WM_LBUTTONUP (seed the released tab's `anim_x`, clear drag state). Without
   it, any capture loss without a buttonup (modal MessageBox mid-drag, external
   SetCapture) leaves `drag_tab` stuck ≥ 0 and the settle condition in step 3
   never fires — perpetual 64 Hz timer with a frozen floating tab.
6. Slot targets are recomputed from `nw_tab_rect` on every tick, so a resize or
   DPI change during the settle animation self-corrects.

## Paint

- New helper `nw_tab_draw_x(nw, hwnd, i)`:
  - dragging and `i == drag_tab` →
    `clamp(drag_x - drag_dx, strip_left, strip_left + (ntabs-1)*w)` — the upper
    bound is the **last slot's left edge**, not `strip_right - w`. With few
    tabs `nw_tab_w` clamps to TAB_MAX and slots end far left of `strip_right`;
    the wider clamp would let the tab drag deep into empty strip past the +
    button and fly back on release. Last-slot clamp matches Windows Terminal
    and removes the +-overlap case.
  - else if `anim_on` → `(int)anim_x[i]`
  - else → slot-x (existing behavior, zero cost when idle)
- The fill / label / ✕ loops in `nw_paint_tabs` use the draw-x instead of the
  slot left edge.
- The dragged tab is skipped in the normal back-to-front fill loop and painted
  **after the + glyph, before the rename frame**, with its label and ✕, so it
  floats on top of sliding neighbors and the + button.
- The dragged tab's shape is painted from its draw-x directly, **without** the
  `fill_left -= radius` left-extension the loop applies for `i > 0` — that
  extension exists to be covered by the left neighbor's later paint; drawn on
  top, nothing covers it and the tab would appear `radius` px too wide with a
  width-pop at settle end.
- Hit-testing stays slot-based (unchanged). Mid-settle clicks land on
  final-slot tabs — acceptable ~300 ms window.

## Edge cases

- **Structural changes while drag or settle is active.** Mouse capture routes
  messages to this window but does not block them: middle-click closes a tab
  mid-drag (WM_MBUTTONDOWN), and keyboard focus stays on the RichEdit so
  EN_MSGFILTER shortcuts fire mid-drag — Ctrl+N adds a tab, Ctrl+Shift+D
  deletes one (including a modal MessageBox → capture loss), Ctrl+R starts a
  rename whose accent frame paints at the slot rect while the tab is still
  sliding. One rule: any tab add / close / delete / rename-begin while
  `drag_tab >= 0` or `anim_on` first **cancels drag and animation** (release
  capture if held, snap all tabs, kill timer), then proceeds. Guard applied in
  nw_add_tab / nw_close_tab / nw_delete_note / nw_begin_rename entry points.
  (The stale-index corruption this prevents is a latent pre-existing bug —
  today's mid-drag Ctrl+N already desyncs `drag_tab` from the shifted `tab[]`.)
- **Capture loss without buttonup** → WM_CAPTURECHANGED handler (Behavior §5).
- **Last-tab close destroys the window mid-settle:** hwnd-bound timers die with
  the window — no leak, no special handling.
- **Ticks while minimized:** WM_TIMER still fires, animation converges, timer
  self-kills. Harmless.

## Testing

Animation is GUI-only; the existing test suites (`tests/test_app.c`,
`tests/test_prefs.c`) cover pure logic. The easing math is a single inline
expression — not worth extracting for a unit test. Verification: build, launch
fresh, drag tabs, eyeball the motion. (Screenshots cannot capture motion;
manual check.)
