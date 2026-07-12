# Tab Drag Slide Animation — Design

**Date:** 2026-07-13
**Status:** Approved

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

1. **Drag begins** (existing 6-px threshold crossed): seed `anim_x[i]` with each
   tab's slot-x, `SetTimer(hwnd, IDT_TABANIM, 10, NULL)`, `anim_on = 1`.
2. **WM_MOUSEMOVE while dragging:** store `drag_x`. Target-slot computation is
   unchanged (`center / w`). On reorder, shuffle `tab[]` as today **and shuffle
   `anim_x[]` identically** — displaced tabs keep their current visual x and
   slide from there.
3. **WM_TIMER tick:** compute dt via `QueryPerformanceCounter`. For every tab
   except the dragged one: `anim_x += (slot_x - anim_x) * (1 - expf(-dt/TAU))`.
   Invalidate the tab-bar rect only (same rect the hover code invalidates).
   When all tabs are within 0.5 px of their slots and no drag is active: snap
   exact, `KillTimer`, `anim_on = 0`.
4. **WM_LBUTTONUP:** set `anim_x[drag_tab]` to the dragged tab's floating x at
   release, then clear `drag_tab`. The timer keeps running, so the released tab
   glides into its slot via the same tick path.
5. Slot targets are recomputed from `nw_tab_rect` on every tick, so a resize or
   DPI change during the settle animation self-corrects.

## Paint

- New helper `nw_tab_draw_x(nw, hwnd, i)`:
  - dragging and `i == drag_tab` → `clamp(drag_x - drag_dx, strip_left, strip_right - w)`
  - else if `anim_on` → `(int)anim_x[i]`
  - else → slot-x (existing behavior, zero cost when idle)
- The fill / label / ✕ loops in `nw_paint_tabs` use the draw-x instead of the
  slot left edge.
- The dragged tab is skipped in the normal back-to-front fill loop and painted
  **last, with its label and ✕**, so it floats on top of sliding neighbors.
- Hit-testing stays slot-based (unchanged). Hover states are idle during drag
  (mouse captured) and rename cannot coexist with a drag, so there are no
  conflicts.

## Edge cases

- **Tab close/add while the settle animation runs** (✕ click mid-glide): cancel
  the animation — snap all tabs, kill the timer. One rule, no half states.
- Mid-drag close/resize is impossible: the mouse is captured.

## Testing

Animation is GUI-only; the existing test suites (`tests/test_app.c`,
`tests/test_prefs.c`) cover pure logic. The easing math is a single inline
expression — not worth extracting for a unit test. Verification: build, launch
fresh, drag tabs, eyeball the motion. (Screenshots cannot capture motion;
manual check.)
