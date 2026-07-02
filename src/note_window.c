#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  /* Windows 10 — needed for GetDpiForWindow */
#endif
#include "note_window.h"
#include "store.h"
#include "markdown.h"
#include "resource.h"
#include "gfx_d2d.h"
#include <richedit.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOTE_CLASS   L"StickyNoteWindow"
#define ID_EDIT      1001
#define ID_RENAME    1003
#define DEBOUNCE_MS  150
#define IDT_DEBOUNCE 1

/* Formatting sidebar buttons. */
#define IDB_BOLD     1100
#define IDB_ITALIC   1101
#define IDB_STRIKE   1102
#define IDB_BULLET   1103
#define IDB_NUM      1104
#define NUM_BTNS     5

/* Layout (px). Three bordered boxes — sidebar, title, body — sit inside the
 * client area with an outer pad and a gap between them; each control is inset
 * a little inside its box so the border shows and text has padding. */
#define PAD_OUTER    8     /* client edge -> boxes */
#define REGION_GAP   8     /* between boxes */
#define SIDEBAR_W    40    /* sidebar width (buttons are this wide, square) */
#define TOP_FRAME_H  7     /* restored-only top resize/gutter strip */
#define TITLE_CONTENT_H 34 /* tab/caption content row height */
#define WINBTN_W     46    /* min/max/close button width */
#define BTN_GAP      3     /* between sidebar buttons */

/* Custom window-chrome buttons (min / max / close), top-right of the title bar. */
#define IDW_MIN      1200
#define IDW_MAX      1201
#define IDW_CLOSE    1202
#define NUM_WBTNS    3

/* Tab strip geometry (px). Windows-Terminal look: tabs are nested in the bar
 * (a gap above so they don't span it) with rounded top corners, and the ✕ / +
 * controls are uniform squares. */
#define DRAG_GAP      36   /* gap between last tab/+ and the window buttons */
#define TAB_TOP_GAP    3   /* gap between the title-bar top and a tab's top */
#define TAB_RADIUS     7   /* rounded top-corner radius of a tab */
#define TAB_CLOSE_W    28
#define CTL_SIZE      18   /* uniform square for the ✕ (per tab) and the + */
#define PLUS_W        40
#define PLUS_H        28
#define PLUS_OVERLAP TAB_RADIUS
#define PLUS_GAP       8   /* gap between the last tab and the + button */
#define TAB_MIN       48   /* minimum tab width */
#define TAB_MAX      200   /* maximum tab width */
#define DRAG_THRESHOLD 6   /* px movement before a tab drag begins (Task 8) */

typedef enum { HIT_NONE, HIT_TAB, HIT_CLOSE, HIT_PLUS } TabHit;

/* Notifications we want from the RichEdit: text changes (for live-format
 * debounce), key events (shortcuts via EN_MSGFILTER), and selection changes
 * (to keep the sidebar toggle highlights in sync with the caret). */
#define EDIT_EVENT_MASK (ENM_CHANGE | ENM_KEYEVENTS | ENM_SELCHANGE)

/* DWM dark title bar. Attribute 20 = DWMWA_USE_IMMERSIVE_DARK_MODE on
 * Windows 10 2004+ (older insider builds used 19). */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

/* Dark palette (Windows-Terminal-inspired: near-black bg) */
static const COLORREF COL_BG      = RGB(0x0C, 0x0C, 0x0C);  /* app background */
static const COLORREF COL_TEXT    = RGB(0xe6, 0xe6, 0xe6);
static const COLORREF COL_BORDER  = RGB(0x2a, 0x2a, 0x2e);  /* dividers */
static const COLORREF COL_CLOSE   = RGB(0xc0, 0x3a, 0x3a);  /* close button hover */
static const COLORREF COL_HOVER   = RGB(0x26, 0x26, 0x2b);  /* min/max + control hover */
static const COLORREF COL_TAB_ACT = RGB(0x1b, 0x1b, 0x1f);  /* active tab fill */
static const COLORREF COL_TAB_HOT = RGB(0x15, 0x15, 0x17);  /* hovered tab fill */
static const COLORREF COL_RENAME  = RGB(0x0f, 0x0f, 0x12);  /* rename field bg */
static const COLORREF COL_ACCENT  = RGB(0x3a, 0x7a, 0xfe);  /* rename outline */
static const COLORREF COL_CODE_BG = RGB(0x18, 0x18, 0x1c);  /* code-block background */

typedef struct {
    char id[16];      /* note id */
    HWND edit;        /* this tab's body RichEdit */
} NoteTab;

typedef struct {
    AppState* app;
    char  win_id[16];          /* resolve WinMeta* fresh via prefs_find_window */
    NoteTab tab[WIN_MAX_TABS];
    int   ntabs;
    int   active;
    HWND  btns[NUM_BTNS];
    int   whot[NUM_WBTNS];
    int   bhot[NUM_BTNS];      /* sidebar button hover (cursor over the square) */
    int   active_fmt[NUM_BTNS];
    /* tab hover state (WT look) */
    int   hot_tab;    /* index of tab under the cursor, or -1 */
    int   hot_close;  /* 1 when the cursor is over the hot tab's ✕ */
    int   hot_plus;   /* 1 when the cursor is over the + button */
    /* drag-to-reorder state (Task 9) */
    int   drag_tab;   /* index of tab being dragged, or -1 when idle */
    int   drag_dx;    /* cursor x-offset from the tab's left edge at press */
    int   press_x;    /* x at button-down, used for threshold detection */
    int   dragging;   /* 1 once the threshold has been exceeded */
    /* rename-overlay state (Task 10) */
    HWND  rename_edit;  /* NULL when no rename in progress */
    HFONT rename_font;  /* font for the rename edit; freed on commit */
    int   rename_tab;   /* index of the tab being renamed */
    GfxD2D* gfx;        /* Direct2D chrome renderer; lazy-created in WM_PAINT */
    size_t last_caret_para;   /* start offset of the paragraph last revealed */
} NoteWin;

static HMODULE g_richedit = NULL;
static HBRUSH  g_bg_brush = NULL;     /* class background, dark; created once */
static HBRUSH  g_border_brush = NULL; /* region outline; created once */
static HBRUSH  g_rename_brush = NULL; /* rename-field background; created once */
static HICON   g_app_icon = NULL;     /* qN, painted in the top-left cell */

static NoteWin* nw_get(HWND h) { return (NoteWin*)GetWindowLongPtrW(h, GWLP_USERDATA); }

/* Window DPI (PerMonitorV2). GetDpiForWindow needs Win10 1607+; 96 is the
 * safe fallback. All chrome constants are authored at 96 dpi and scaled here. */
static int nw_dpi(HWND h) { UINT d = GetDpiForWindow(h); return d ? (int)d : 96; }
static int nw_s(HWND h, int v) { return MulDiv(v, nw_dpi(h), 96); }

/* Body char height in twips, scaled to the window DPI (10pt base = 200 twips). */
static LONG nw_body_yheight(HWND hwnd) { return (LONG)nw_s(hwnd, 200); }

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

/* Resolve the WinMeta* / active tab fresh at point of use. prefs arrays can be
 * realloc'd or memmove'd, so cached pointers would dangle. Always NULL-check. */
static WinMeta* nw_win(NoteWin* nw) { return prefs_find_window(&nw->app->prefs, nw->win_id); }
static NoteTab* nw_cur(NoteWin* nw) { return (nw->ntabs > 0) ? &nw->tab[nw->active] : NULL; }
static HWND     nw_edit(NoteWin* nw) { NoteTab* t = nw_cur(nw); return t ? t->edit : NULL; }
static NoteMeta* nw_note_meta(NoteWin* nw, int i) {
    return prefs_find(&nw->app->prefs, nw->tab[i].id);
}

/* ---- best-effort open-window registry, keyed by window id ----
 * Lets the tray dedupe reopen requests. If full, the new entry just isn't
 * tracked (treated as "not open"); correctness still holds. */
#define NW_REGISTRY_CAP 64
typedef struct { char win_id[16]; HWND hwnd; } NwRegEntry;
static NwRegEntry s_registry[NW_REGISTRY_CAP];

static void nw_registry_add(const char* win_id, HWND hwnd) {
    for (int i = 0; i < NW_REGISTRY_CAP; i++)
        if (s_registry[i].hwnd == NULL) {
            snprintf(s_registry[i].win_id, sizeof s_registry[i].win_id, "%s", win_id);
            s_registry[i].hwnd = hwnd; return;
        }
    /* full: leave untracked (best-effort) */
}
static void nw_registry_remove(HWND hwnd) {
    for (int i = 0; i < NW_REGISTRY_CAP; i++)
        if (s_registry[i].hwnd == hwnd) { s_registry[i].hwnd = NULL; s_registry[i].win_id[0] = '\0'; return; }
}
HWND note_window_find_open_window(const char* win_id) {
    for (int i = 0; i < NW_REGISTRY_CAP; i++)
        if (s_registry[i].hwnd && strcmp(s_registry[i].win_id, win_id) == 0)
            return s_registry[i].hwnd;
    return NULL;
}
HWND note_window_find_by_note(const char* note_id) {
    for (int i = 0; i < NW_REGISTRY_CAP; i++) {
        if (!s_registry[i].hwnd) continue;
        NoteWin* nw = nw_get(s_registry[i].hwnd);
        if (!nw) continue;
        for (int t = 0; t < nw->ntabs; t++)
            if (strcmp(nw->tab[t].id, note_id) == 0) return s_registry[i].hwnd;
    }
    return NULL;
}

/* Layout geometry. Two dividers cross the whole client area: a vertical line at
 * x=SIDEBAR_W and a full-width horizontal line at the dynamic titlebar height.
 * They make four cells — top-left the app-icon square, top-right the title bar
 * (tabs + window buttons), bottom-left the format sidebar, bottom-right the
 * body. vx/hy are the dividers; title/body are control rects. Any out param may
 * be NULL. */
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

/* Tab strip geometry helpers — shared by paint (Task 6) and hit-test (Task 7). */
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

static TabHit nw_tab_hit(NoteWin* nw, HWND hwnd, int cx, int cy, int* out_idx) {
    int tab_top = nw_top_gutter(hwnd);
    int tab_bottom = tab_top + nw_s(hwnd, TITLE_CONTENT_H);

    if (cy < tab_top || cy >= tab_bottom) return HIT_NONE;
    for (int i = 0; i < nw->ntabs; i++) {
        RECT r; nw_tab_rect(nw, hwnd, i, &r);
        if (cx >= r.left && cx < r.right) {
            if (out_idx) *out_idx = i;
            if (cx >= r.right - nw_s(hwnd, CTL_SIZE) - nw_s(hwnd, 8)) return HIT_CLOSE;
            return HIT_TAB;
        }
    }
    RECT pr; nw_plus_rect(nw, hwnd, &pr);
    if (cx >= pr.left && cx < pr.right) return HIT_PLUS;
    return HIT_NONE;
}

/* Sidebar buttons are squares the full sidebar width, stacked below the
 * horizontal divider. Window buttons sit at the top-right of the title bar.
 * Title and body fill the content column to the right of the vertical divider. */
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

/* Owner-draw a sidebar button: blends with the dark sidebar (no distinct fill)
 * until it is pressed or its format is active at the caret, when the square
 * fills with the toggle accent. The glyph is rendered in the style it applies
 * (bold B, italic I, struck-through S). */
static void nw_draw_button(NoteWin* nw, const DRAWITEMSTRUCT* d) {
    int idx = (int)d->CtlID - IDB_BOLD;
    int on = (d->itemState & ODS_SELECTED) != 0 ||
             (idx >= 0 && idx < NUM_BTNS && nw->active_fmt[idx]);
    int hot = (idx >= 0 && idx < NUM_BTNS && nw->bhot[idx]);
    /* pressed/active -> active-tab fill; else hover -> same COL_HOVER as the + button */
    HBRUSH bg = CreateSolidBrush(on ? COL_TAB_ACT : (hot ? COL_HOVER : COL_BG));
    FillRect(d->hDC, &d->rcItem, bg);
    DeleteObject(bg);

    LOGFONTW lf; memset(&lf, 0, sizeof lf);
    lf.lfHeight = -nw_s(d->hwndItem, 16); lf.lfWeight = FW_BOLD; wcscpy(lf.lfFaceName, L"IBM Plex Mono");
    if (d->CtlID == IDB_ITALIC) lf.lfItalic = TRUE;
    if (d->CtlID == IDB_STRIKE) lf.lfStrikeOut = TRUE;
    HFONT f = CreateFontIndirectW(&lf);
    HFONT old = (HFONT)SelectObject(d->hDC, f);

    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, RGB(0xe6,0xe6,0xe6));
    wchar_t txt[8]; GetWindowTextW(d->hwndItem, txt, 8);
    RECT r = d->rcItem;
    DrawTextW(d->hDC, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(d->hDC, old);
    DeleteObject(f);
}

/* Subclass for sidebar buttons: track hover so the square highlights (COL_HOVER)
 * like the + button. BS_OWNERDRAW alone gives no hot state, and the buttons are
 * child HWNDs so the parent's WM_MOUSEMOVE never fires over them. id is the
 * button index (0..NUM_BTNS-1). */
static LRESULT CALLBACK nw_btn_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                   UINT_PTR id, DWORD_PTR ref) {
    NoteWin* nw = (NoteWin*)ref;
    if (msg == WM_MOUSEMOVE) {
        if (id < NUM_BTNS && !nw->bhot[id]) {
            nw->bhot[id] = 1;
            TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(h, NULL, TRUE);
        }
    } else if (msg == WM_MOUSELEAVE) {
        if (id < NUM_BTNS) { nw->bhot[id] = 0; InvalidateRect(h, NULL, TRUE); }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

/* Read the body as RichEdit-indexed text (\r breaks, CP_ACP); caller frees.
 * *out_len gets the length (0 for empty). Returns NULL only on OOM. */
static char* nw_body_text(HWND edit, int* out_len) {
    GETTEXTLENGTHEX gtl; gtl.flags = GTL_DEFAULT; gtl.codepage = CP_ACP;
    int len = (int)SendMessageW(edit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
    if (len < 0) len = 0;
    char* buf = malloc((size_t)len + 1);
    if (!buf) { *out_len = 0; return NULL; }
    if (len > 0) {
        GETTEXTEX gte; gte.cb = (DWORD)(len + 1); gte.flags = GT_DEFAULT;
        gte.codepage = CP_ACP; gte.lpDefaultChar = NULL; gte.lpUsedDefChar = NULL;
        SendMessageW(edit, EM_GETTEXTEX, (WPARAM)&gte, (LPARAM)buf);
    } else buf[0] = '\0';
    *out_len = len;
    return buf;
}

/* Is the line starting at ls an ATX heading? (<=3 spaces, 1-6 '#', then space
 * or line end) — matching the CommonMark headings the renderer styles. */
static int nw_line_is_heading(const char* buf, int len, LONG ls) {
    LONG p = ls; int sp = 0;
    while (p < len && buf[p] == ' ' && sp < 3) { p++; sp++; }
    int h = 0;
    while (p < len && buf[p] == '#' && h < 6) { p++; h++; }
    if (h == 0) return 0;
    return (p >= len || buf[p] == ' ' || buf[p] == '\r' || buf[p] == '\n');
}

/* Does the body caret/selection start on a heading line? Headings are hard H1
 * and not formattable, so this gates both the highlights and the actions. */
static int nw_caret_on_heading(NoteWin* nw) {
    HWND e = nw_edit(nw);
    CHARRANGE sel; SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
    int len; char* buf = nw_body_text(e, &len);
    if (!buf) return 0;
    LONG ls = sel.cpMin; if (ls > len) ls = len;
    while (ls > 0 && buf[ls-1] != '\r' && buf[ls-1] != '\n') ls--;
    int r = nw_line_is_heading(buf, len, ls);
    free(buf);
    return r;
}

/* Clear all toggle highlights, repainting any that change. */
static void nw_clear_toggles(NoteWin* nw) {
    for (int i = 0; i < NUM_BTNS; i++)
        if (nw->active_fmt[i]) { nw->active_fmt[i] = 0; InvalidateRect(nw->btns[i], NULL, TRUE); }
}

/* Recompute which formats apply at the body caret/selection and repaint any
 * button whose highlight changed. Inline styles are read from the markdown
 * spans covering the caret (so several nested formats — bold+italic+strike —
 * all light at once); lists come from the paragraph numbering. Headings
 * highlight nothing. */
static void nw_update_toggles(NoteWin* nw) {
    if (nw_caret_on_heading(nw)) { nw_clear_toggles(nw); return; }
    int s[NUM_BTNS] = { 0 };
    HWND e = nw_edit(nw);

    CHARRANGE sel; SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
    int len; char* buf = nw_body_text(e, &len);
    if (buf) {
        MdFmt f = markdown_fmt_at(buf, (size_t)len, (size_t)sel.cpMin);
        s[0] = (f & MD_FMT_BOLD)   != 0;
        s[1] = (f & MD_FMT_ITALIC) != 0;
        s[2] = (f & MD_FMT_STRIKE) != 0;
        free(buf);
    }

    PARAFORMAT2 pf; memset(&pf, 0, sizeof pf); pf.cbSize = sizeof pf;
    pf.dwMask = PFM_NUMBERING;
    SendMessageW(e, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
    s[3] = (pf.wNumbering == PFN_BULLET);
    s[4] = (pf.wNumbering == PFN_ARABIC);

    for (int i = 0; i < NUM_BTNS; i++) {
        if (s[i] != nw->active_fmt[i]) {
            nw->active_fmt[i] = s[i];
            InvalidateRect(nw->btns[i], NULL, TRUE);
        }
    }
}

/* Load tab i's note: "# name\n\n body" -> name is metadata, body fills the edit. */
static void nw_load_tab(NoteWin* nw, int i) {
    NoteMeta* m = prefs_find(&nw->app->prefs, nw->tab[i].id);
    if (!m) return;
    char path[260];
    app_note_path(nw->app, m, path, sizeof path);
    char* txt = NULL; size_t n = 0;
    const char* body = "";
    if (store_read_note(path, &txt, &n)) {
        body = txt;
        if (n >= 2 && txt[0] == '#' && txt[1] == ' ') {
            size_t k = 2; while (k < n && txt[k] != '\n' && txt[k] != '\r') k++;
            if (k < n && txt[k] == '\r') k++;
            if (k < n && txt[k] == '\n') k++;
            size_t j = k;
            if (j < n && txt[j] == '\r') j++;
            if (j < n && txt[j] == '\n') k = j + 1;
            body = txt + k;
        }
    }
    SetWindowTextA(nw->tab[i].edit, body);
    free(txt);
}

/* Save tab i's body, prefixing the current name as "# name". */
static void nw_save_tab(NoteWin* nw, int i) {
    NoteMeta* m = prefs_find(&nw->app->prefs, nw->tab[i].id);
    if (!m) return;
    int bl = GetWindowTextLengthA(nw->tab[i].edit);
    char* b = malloc((size_t)bl + 1); if (!b) return;
    GetWindowTextA(nw->tab[i].edit, b, bl + 1);
    int tl = (int)strlen(m->name);
    size_t need = (tl > 0 ? (size_t)tl + 4 : 0) + (size_t)bl + 1;
    char* buf = malloc(need); if (!buf) { free(b); return; }
    size_t off = 0;
    if (tl > 0) off += (size_t)sprintf(buf + off, "# %s\n\n", m->name);
    memcpy(buf + off, b, (size_t)bl); off += (size_t)bl;
    char path[260];
    app_note_path(nw->app, m, path, sizeof path);
    store_write_note(path, buf, off);
    free(buf); free(b);
}

/* Persist window geometry, but only when the window is in its normal state.
 * A minimized window's GetWindowRect is (-32000,-32000); a maximized one is
 * the whole work area. Saving either would lose the user's real placement, so
 * skip and keep the last good values. */
static void nw_save_geometry(HWND hwnd, WinMeta* w) {
    if (!w || IsIconic(hwnd) || IsZoomed(hwnd)) return;
    RECT wr; GetWindowRect(hwnd, &wr);
    w->x = wr.left; w->y = wr.top;
    w->w = wr.right - wr.left; w->h = wr.bottom - wr.top;
}

/*
 * nw_apply_decos / nw_apply_format — live markdown styling on each debounce tick.
 *
 * CRLF/offset note: GetWindowTextA returns \r\n (2 bytes per line break), but
 * RichEdit's internal character positions (used by EM_EXSETSEL / CHARRANGE) count
 * each paragraph break as a SINGLE character.  Feeding a \r\n buffer to
 * markdown_decorate would produce byte offsets that drift +1 per preceding line
 * break, causing formatting to land on wrong characters for multi-line notes.
 *
 * Fix (option b — clean): nw_apply_format uses EM_GETTEXTEX with GT_DEFAULT and
 * CP_ACP, which returns the text with \r-only paragraph separators (matching
 * RichEdit's internal indexing).  The resulting byte offsets from
 * markdown_decorate are correct for EM_EXSETSEL.  nw_save_tab is unaffected —
 * it still uses GetWindowTextA whose \r\n output is correct for .md files on
 * Windows.
 *
 * md4c treats bare \r as a valid line ending (CommonMark §2.3), so parsing is
 * unaffected.
 */
/* Map an MdFmt to a CHARFORMAT2W and apply it to [start, start+len). */
static void nw_fmt_range(HWND edit, size_t start, size_t len, MdFmt fmt) {
    CHARRANGE r = { (LONG)start, (LONG)(start + len) };
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&r);
    CHARFORMAT2W cf; memset(&cf, 0, sizeof cf); cf.cbSize = sizeof cf;
    cf.dwMask = CFM_BOLD|CFM_ITALIC|CFM_STRIKEOUT|CFM_SIZE|CFM_FACE|CFM_COLOR|CFM_HIDDEN|CFM_BACKCOLOR;
    cf.crTextColor = COL_TEXT;
    cf.crBackColor = COL_BG;
    if (fmt & MD_FMT_CODEBLOCK) cf.crBackColor = COL_CODE_BG;
    cf.yHeight = 200;
    if (fmt & MD_FMT_H1) cf.yHeight = 360;
    else if (fmt & MD_FMT_H2) cf.yHeight = 300;
    else if (fmt & MD_FMT_H3) cf.yHeight = 260;
    if (fmt & (MD_FMT_BOLD | MD_FMT_H1 | MD_FMT_H2 | MD_FMT_H3)) cf.dwEffects |= CFE_BOLD;
    if (fmt & MD_FMT_ITALIC) cf.dwEffects |= CFE_ITALIC;
    if (fmt & MD_FMT_STRIKE) cf.dwEffects |= CFE_STRIKEOUT;
    wcscpy(cf.szFaceName, L"IBM Plex Mono");
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

static void nw_hide_range2(HWND edit, size_t start, size_t len) {
    CHARRANGE r = { (LONG)start, (LONG)(start + len) };
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&r);
    CHARFORMAT2W cf; memset(&cf, 0, sizeof cf); cf.cbSize = sizeof cf;
    cf.dwMask = CFM_HIDDEN; cf.dwEffects = CFE_HIDDEN;
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

static void nw_para_range(HWND edit, size_t start, size_t len, ParaKind kind) {
    CHARRANGE r = { (LONG)start, (LONG)(start + len) };
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&r);
    PARAFORMAT2 pf; memset(&pf, 0, sizeof pf); pf.cbSize = sizeof pf;
    pf.dwMask = PFM_NUMBERING | PFM_NUMBERINGSTYLE | PFM_NUMBERINGTAB
              | PFM_OFFSET | PFM_STARTINDENT;
    if (kind == PARA_BULLET || kind == PARA_NUMBER) {
        pf.wNumbering = (kind == PARA_NUMBER) ? PFN_ARABIC : PFN_BULLET;
        pf.wNumberingStyle = (kind == PARA_NUMBER) ? PFNS_PERIOD : PFNS_PLAIN;
        pf.wNumberingTab = 280; pf.dxStartIndent = 280; pf.dxOffset = 280;
    }
    SendMessageW(edit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
}

/* Reset [lo,hi] to base formatting, then apply every decoration intersecting it. */
static void nw_apply_decos(HWND edit, int len, Deco* d, size_t n, size_t lo, size_t hi) {
    if (hi > (size_t)len) hi = (size_t)len;
    CHARRANGE base = { (LONG)lo, (LONG)hi };
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&base);
    CHARFORMAT2W bf; memset(&bf, 0, sizeof bf); bf.cbSize = sizeof bf;
    bf.dwMask = CFM_BOLD|CFM_ITALIC|CFM_STRIKEOUT|CFM_SIZE|CFM_FACE|CFM_COLOR|CFM_HIDDEN|CFM_BACKCOLOR;
    bf.yHeight = 200; bf.crTextColor = COL_TEXT; bf.crBackColor = COL_BG;
    wcscpy(bf.szFaceName, L"IBM Plex Mono");
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&bf);
    /* also clear paragraph numbering across the range, so un-listing reverts */
    nw_para_range(edit, lo, hi > lo ? hi - lo : 0, PARA_NONE);

    for (size_t i = 0; i < n; i++) {
        size_t a = d[i].start, b = a + d[i].len;
        if (b <= lo || a >= hi) continue;                 /* outside range */
        if (d[i].kind == DECO_FMT)  nw_fmt_range(edit, a, d[i].len, d[i].fmt);
        else if (d[i].kind == DECO_HIDE) nw_hide_range2(edit, a, d[i].len);
        else if (d[i].kind == DECO_PARA) nw_para_range(edit, a, d[i].len, d[i].para);
    }
}

/* Expand [lo,hi] to full source-paragraph boundaries in buf. */
static void nw_para_bounds(const char* buf, int len, size_t* lo, size_t* hi) {
    size_t a = *lo, b = *hi;
    while (a > 0 && buf[a-1] != '\r' && buf[a-1] != '\n') a--;
    while (b < (size_t)len && buf[b] != '\r' && buf[b] != '\n') b++;
    *lo = a; *hi = b;
}

/* Restyle a tab body. scoped=1 limits the reset+apply to the paragraph(s) the
 * selection touches (fast path for edits/caret moves); scoped=0 does the whole
 * document (load). Markers reveal in the selection's paragraph(s). */
static void nw_restyle(NoteWin* nw, int tab, int scoped) {
    HWND edit = nw->tab[tab].edit;
    SendMessageW(edit, EM_SETEVENTMASK, 0, 0);

    GETTEXTLENGTHEX gtl; gtl.flags = GTL_DEFAULT; gtl.codepage = CP_ACP;
    int len = (int)SendMessageW(edit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
    if (len < 0) len = 0;
    char* buf = malloc((size_t)len + 1);
    if (!buf) { SendMessageW(edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK); return; }
    if (len > 0) {
        GETTEXTEX gte; gte.cb = (DWORD)(len + 1); gte.flags = GT_DEFAULT;
        gte.codepage = CP_ACP; gte.lpDefaultChar = NULL; gte.lpUsedDefChar = NULL;
        SendMessageW(edit, EM_GETTEXTEX, (WPARAM)&gte, (LPARAM)buf);
    } else buf[0] = '\0';

    CHARRANGE saved; SendMessageW(edit, EM_EXGETSEL, 0, (LPARAM)&saved);
    size_t sel_lo = (size_t)saved.cpMin, sel_hi = (size_t)saved.cpMax;

    /* apply window: whole doc, unless scoped to a single-paragraph selection */
    size_t lo = 0, hi = (size_t)len;
    if (scoped) {
        size_t plo = sel_lo, phi = sel_hi;
        nw_para_bounds(buf, len, &plo, &phi);
        /* only scope when the selection stays within one paragraph */
        size_t clo = sel_hi, chi = sel_hi; nw_para_bounds(buf, len, &clo, &chi);
        if (plo == clo && phi == chi) { lo = plo; hi = phi; }
    }

    Deco* d = NULL;
    size_t n = markdown_decorate(buf, (size_t)len, sel_lo, sel_hi, &d);
    nw_apply_decos(edit, len, d, n, lo, hi);
    free(d);
    free(buf);

    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&saved);
    SendMessageW(edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
}

/* Live markdown restyle of one tab's body RichEdit. */
static void nw_apply_format_edit(NoteWin* nw, int i) {
    nw_restyle(nw, i, 0);   /* whole-document (load / programmatic reformat) */
}

static void nw_apply_format(NoteWin* nw) {
    if (nw->ntabs > 0) nw_apply_format_edit(nw, nw->active);
}

/* Persist + restyle the body now (used after a programmatic edit), with redraw
 * suppressed to avoid flicker. */
static void nw_reformat_now(NoteWin* nw) {
    HWND e = nw_edit(nw);
    SendMessageW(e, WM_SETREDRAW, FALSE, 0);
    nw_save_tab(nw, nw->active);
    nw_apply_format(nw);
    SendMessageW(e, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(e, NULL, TRUE);
    nw_update_toggles(nw);    /* apply_format muted EN_SELCHANGE; refresh now */
}

/* Length of a list marker at position p ("- " or "N. "), else 0. */
static int nw_marker_len(const char* buf, int len, LONG p) {
    if (p + 1 < len && buf[p] == '-' && buf[p+1] == ' ') return 2;
    LONG q = p; while (q < len && buf[q] >= '0' && buf[q] <= '9') q++;
    if (q > p && q + 1 < len && buf[q] == '.' && buf[q+1] == ' ')
        return (int)(q - p) + 2;
    return 0;
}

/* Apply/toggle an inline markdown marker ("**", "*", "~~"; idx is the button
 * index, used to read the live highlight state). With a selection it wraps, or
 * unwraps if already wrapped. With a collapsed caret it depends on the format:
 *   - caret between an empty pair (e.g. **|**): remove the markers entirely.
 *   - caret inside a non-empty run of this format: escape it — jump past the
 *     closing marker and insert a space (continue typing unformatted).
 *   - otherwise: start the format (insert an empty pair, caret between them).
 * The live formatter then renders + hides the markers. */
static void nw_wrap_inline(NoteWin* nw, const char* marker, int idx) {
    if (nw_caret_on_heading(nw)) return;     /* headings are hard H1 */
    HWND e = nw_edit(nw);
    int mlen = (int)strlen(marker);
    wchar_t wmark[8];
    for (int k = 0; k < mlen; k++) wmark[k] = (wchar_t)marker[k];
    wmark[mlen] = 0;

    SendMessageW(e, EM_SETEVENTMASK, 0, 0);   /* mute; we reformat explicitly */
    CHARRANGE sel; SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
    LONG a = sel.cpMin, b = sel.cpMax;
    int len; char* buf = nw_body_text(e, &len);
    if (!buf) { SendMessageW(e, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK); return; }

    if (a == b) {                              /* collapsed caret: escape logic */
        int empty_pair = (a >= mlen && a + mlen <= len &&
                          memcmp(buf + a - mlen, marker, (size_t)mlen) == 0 &&
                          memcmp(buf + a, marker, (size_t)mlen) == 0);
        if (empty_pair) {                      /* **|** -> remove entirely */
            free(buf);
            CHARRANGE r = { a - mlen, a + mlen };
            SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&r);
            SendMessageW(e, EM_REPLACESEL, TRUE, (LPARAM)L"");
            SendMessageW(e, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
            nw_reformat_now(nw);
            return;
        }
        if (idx >= 0 && idx < NUM_BTNS && nw->active_fmt[idx]) {  /* inside this fmt */
            LONG cpos = -1;                    /* find closing marker on this line */
            for (LONG p = a; p + mlen <= len; p++) {
                if (buf[p] == '\r' || buf[p] == '\n') break;
                if (memcmp(buf + p, marker, (size_t)mlen) == 0) { cpos = p; break; }
            }
            free(buf);
            if (cpos >= 0) {                   /* escape past it + a space */
                CHARRANGE r = { cpos + mlen, cpos + mlen };
                SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&r);
                SendMessageW(e, EM_REPLACESEL, TRUE, (LPARAM)L" ");
                SendMessageW(e, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
                nw_reformat_now(nw);
            } else {
                SendMessageW(e, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
            }
            return;
        }
        free(buf);
        /* not in this format: start it (insert empty pair, caret between) */
        CHARRANGE r = { a, a };
        SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&r);
        wchar_t pair[16]; wcscpy(pair, wmark); wcscat(pair, wmark);
        SendMessageW(e, EM_REPLACESEL, TRUE, (LPARAM)pair);
        CHARRANGE c = { a + mlen, a + mlen };
        SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&c);
        SendMessageW(e, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
        nw_reformat_now(nw);
        return;
    }

    int wrapped = (a >= mlen && b + mlen <= len &&
                   memcmp(buf + a - mlen, marker, (size_t)mlen) == 0 &&
                   memcmp(buf + b, marker, (size_t)mlen) == 0);
    free(buf);

    if (wrapped) {
        CHARRANGE r1 = { b, b + mlen };           /* delete trailing first */
        SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&r1);
        SendMessageW(e, EM_REPLACESEL, TRUE, (LPARAM)L"");
        CHARRANGE r2 = { a - mlen, a };           /* then leading */
        SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&r2);
        SendMessageW(e, EM_REPLACESEL, TRUE, (LPARAM)L"");
        CHARRANGE ns = { a - mlen, b - mlen };
        SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&ns);
    } else {
        CHARRANGE r1 = { b, b };                  /* insert end marker first */
        SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&r1);
        SendMessageW(e, EM_REPLACESEL, TRUE, (LPARAM)wmark);
        CHARRANGE r2 = { a, a };                  /* then start marker */
        SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&r2);
        SendMessageW(e, EM_REPLACESEL, TRUE, (LPARAM)wmark);
        CHARRANGE ns = { a + mlen, b + mlen };    /* reselect original text */
        SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&ns);
    }

    SendMessageW(e, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
    nw_reformat_now(nw);
}

/* Toggle a list prefix on every body line touched by the selection. Bullet uses
 * "- ", numbered uses "N. " (renumbered 1..k). If the first line already has the
 * requested kind it is removed from all lines; otherwise it is added (converting
 * the other kind in place). The live formatter turns markers into bullets. */
static void nw_list_prefix(NoteWin* nw, int numbered) {
    if (nw_caret_on_heading(nw)) return;     /* headings are hard H1 */
    HWND e = nw_edit(nw);
    SendMessageW(e, EM_SETEVENTMASK, 0, 0);
    CHARRANGE sel; SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
    int len; char* buf = nw_body_text(e, &len);
    if (!buf) { SendMessageW(e, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK); return; }
    LONG a = sel.cpMin, b = sel.cpMax;

    LONG ls = a;                                  /* start of line containing a */
    while (ls > 0 && buf[ls-1] != '\r' && buf[ls-1] != '\n') ls--;

    /* Collapsed caret on a line already of this list kind: escape it. Empty
     * item -> remove the marker; non-empty -> end it and drop to a new plain
     * line (act as Return). */
    if (a == b) {
        LONG lend = ls; while (lend < len && buf[lend] != '\r' && buf[lend] != '\n') lend++;
        int ml = nw_marker_len(buf, len, ls);
        int isBullet = (ml == 2 && buf[ls] == '-');
        int isNum    = (ml > 0 && buf[ls] >= '0' && buf[ls] <= '9');
        if (numbered ? isNum : isBullet) {
            int hasContent = (lend > ls + ml);
            free(buf);
            if (!hasContent) {
                CHARRANGE r = { ls, ls + ml };
                SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&r);
                SendMessageW(e, EM_REPLACESEL, TRUE, (LPARAM)L"");
            } else {
                CHARRANGE r = { lend, lend };
                SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&r);
                SendMessageW(e, EM_REPLACESEL, TRUE, (LPARAM)L"\n");
            }
            SendMessageW(e, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
            nw_reformat_now(nw);
            return;
        }
    }

    LONG starts[256]; int ns = 0;                 /* line starts in [ls, b] */
    for (LONG p = ls;;) {
        if (ns < 256) starts[ns++] = p;
        LONG q = p; while (q < len && buf[q] != '\r' && buf[q] != '\n') q++;
        if (q >= len) break;
        if (buf[q] == '\r') q++;
        if (q < len && buf[q] == '\n') q++;
        if (q > b) break;
        p = q;
    }

    int firstMl = nw_marker_len(buf, len, starts[0]);
    int firstBullet = (firstMl == 2 && buf[starts[0]] == '-');
    int firstNum = (firstMl > 0 && buf[starts[0]] >= '0' && buf[starts[0]] <= '9');
    int remove = numbered ? firstNum : firstBullet;

    for (int i = ns - 1; i >= 0; i--) {           /* last->first keeps offsets valid */
        LONG p = starts[i];
        int ml = nw_marker_len(buf, len, p);
        wchar_t ins[16];
        if (remove) {
            if (ml == 0) continue;
            CHARRANGE r = { p, p + ml };
            SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&r);
            SendMessageW(e, EM_REPLACESEL, TRUE, (LPARAM)L"");
        } else {
            if (numbered) swprintf(ins, 16, L"%d. ", i + 1);
            else wcscpy(ins, L"- ");
            CHARRANGE r = { p, p + ml };          /* replaces any existing marker */
            SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&r);
            SendMessageW(e, EM_REPLACESEL, TRUE, (LPARAM)ins);
        }
    }
    free(buf);

    SendMessageW(e, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
    nw_reformat_now(nw);
}

/* Enter inside a list item: continue the list. On a non-empty item, insert a
 * newline plus the next marker ("- " or "N+1. "); on an empty item, remove the
 * marker to leave the list. Returns 1 if it handled Enter (caller consumes it),
 * 0 to let RichEdit insert a normal newline. */
static int nw_handle_enter(NoteWin* nw) {
    HWND e = nw_edit(nw);
    CHARRANGE sel; SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
    int len; char* buf = nw_body_text(e, &len);
    if (!buf) return 0;

    LONG ls = sel.cpMin; if (ls > len) ls = len;
    while (ls > 0 && buf[ls-1] != '\r' && buf[ls-1] != '\n') ls--;
    LONG lend = ls; while (lend < len && buf[lend] != '\r' && buf[lend] != '\n') lend++;

    int ml = nw_marker_len(buf, len, ls);
    if (ml == 0) { free(buf); return 0; }            /* not a list line */
    int isNum = (buf[ls] >= '0' && buf[ls] <= '9');
    int hasContent = (lend > ls + ml);

    SendMessageW(e, EM_SETEVENTMASK, 0, 0);
    if (!hasContent) {                                /* empty item -> leave list */
        CHARRANGE r = { ls, ls + ml };
        SendMessageW(e, EM_EXSETSEL, 0, (LPARAM)&r);
        SendMessageW(e, EM_REPLACESEL, TRUE, (LPARAM)L"");
    } else {                                          /* continue list */
        wchar_t ins[24];
        if (isNum) {
            int n = 0;
            for (LONG p = ls; p < ls + ml - 2; p++) n = n * 10 + (buf[p] - '0');
            swprintf(ins, 24, L"\n%d. ", n + 1);
        } else {
            wcscpy(ins, L"\n- ");
        }
        SendMessageW(e, EM_REPLACESEL, TRUE, (LPARAM)ins);
    }
    free(buf);
    SendMessageW(e, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
    nw_reformat_now(nw);
    return 1;
}

/* Mirror the runtime tab[] / active / ntabs into the persisted WinMeta so
 * that preferences / registry stay correct after add or activate. */
static void nw_sync_winmeta(NoteWin* nw) {
    WinMeta* w = nw_win(nw);
    if (!w) return;
    w->ntabs = nw->ntabs; w->active = nw->active;
    for (int i = 0; i < nw->ntabs; i++)
        snprintf(w->tabs[i], sizeof w->tabs[i], "%s", nw->tab[i].id);
    app_persist(nw->app);   /* session changed (add/close/reorder/activate) — save now */
}

/* Show tab i, hide the previous active one. No-op if i is invalid or already
 * the active tab. */
static void nw_activate(NoteWin* nw, HWND hwnd, int i) {
    if (i < 0 || i >= nw->ntabs || i == nw->active) return;
    nw_save_tab(nw, nw->active);
    KillTimer(hwnd, IDT_DEBOUNCE);
    ShowWindow(nw->tab[nw->active].edit, SW_HIDE);
    nw->active = i;
    ShowWindow(nw->tab[i].edit, SW_SHOW);
    nw_sync_winmeta(nw);
    InvalidateRect(hwnd, NULL, TRUE);
    SetFocus(nw->tab[i].edit);
    nw_update_toggles(nw);
}

/* Shared tab-removal helper: the edit for tab i MUST already be destroyed by
 * the caller (nw_close_tab saves first; nw_delete_note skips save).  Shifts
 * tab[] down, decrements ntabs, and either closes the window (if it was the
 * last tab) or fixes active + redraws. */
static void nw_remove_tab(NoteWin* nw, HWND hwnd, int i) {
    for (int k = i; k < nw->ntabs - 1; k++) nw->tab[k] = nw->tab[k+1];
    nw->ntabs--;
    if (nw->ntabs == 0) {
        WinMeta* w = nw_win(nw);
        if (w) prefs_remove_window(&nw->app->prefs, w->id);
        app_persist(nw->app);   /* window gone — persist before it's destroyed */
        DestroyWindow(hwnd);
        return;   /* WM_DESTROY frees nw — do NOT touch nw after this */
    }
    if (nw->active >= nw->ntabs) nw->active = nw->ntabs - 1;
    else if (i < nw->active)     nw->active--;
    ShowWindow(nw->tab[nw->active].edit, SW_SHOW);
    nw_layout(hwnd, nw);
    nw_sync_winmeta(nw);
    InvalidateRect(hwnd, NULL, TRUE);
    SetFocus(nw->tab[nw->active].edit);
    nw_update_toggles(nw);
}

/* Close tab i: save its content, destroy its edit, then remove it from the
 * window.  If it was the last tab the window itself closes. */
static void nw_close_tab(NoteWin* nw, HWND hwnd, int i) {
    if (i < 0 || i >= nw->ntabs) return;
    nw_save_tab(nw, i);
    if (nw->tab[i].edit) DestroyWindow(nw->tab[i].edit);
    nw->tab[i].edit = NULL;
    nw_remove_tab(nw, hwnd, i);
    /* nw_remove_tab may have destroyed hwnd; do not touch nw after this */
}

/* Create a new note + its body RichEdit as a new tab and make it active. */
static void nw_add_tab(NoteWin* nw, HWND hwnd) {
    if (nw->ntabs >= WIN_MAX_TABS) return;
    NoteMeta* m = app_new_note(nw->app);
    if (!m) return;
    int i = nw->ntabs++;
    snprintf(nw->tab[i].id, sizeof nw->tab[i].id, "%s", m->id);
    nw->tab[i].edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
        WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
        0, 0, 100, 100, hwnd, (HMENU)ID_EDIT, GetModuleHandleW(NULL), NULL);
    SendMessageW(nw->tab[i].edit, EM_SETBKGNDCOLOR, 0, (LPARAM)COL_BG);
    CHARFORMAT2W cf; memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf; cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
    cf.crTextColor = COL_TEXT; cf.yHeight = nw_body_yheight(hwnd); wcscpy(cf.szFaceName, L"IBM Plex Mono");
    SendMessageW(nw->tab[i].edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    SendMessageW(nw->tab[i].edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
    nw_load_tab(nw, i);
    nw_layout(hwnd, nw);
    nw_save_tab(nw, nw->active);
    KillTimer(hwnd, IDT_DEBOUNCE);
    ShowWindow(nw->tab[nw->active].edit, SW_HIDE);
    nw->active = i;
    ShowWindow(nw->tab[i].edit, SW_SHOW);
    nw_sync_winmeta(nw);
    InvalidateRect(hwnd, NULL, TRUE);
    SetFocus(nw->tab[i].edit);
    nw_update_toggles(nw);
}

/* Delete the active note (with confirmation), removing its .md and index entry.
 * Reuses nw_remove_tab so only the deleted tab is removed; if it was the last
 * tab the window closes.  No save — the note is being deleted. */
static void nw_delete_note(NoteWin* nw, HWND hwnd) {
    if (MessageBoxW(hwnd, L"Delete this note permanently?",
                    L"Delete", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    int i = nw->active;
    if (i < 0 || i >= nw->ntabs) return;
    char id[16]; strcpy(id, nw->tab[i].id);
    AppState* app = nw->app;
    /* Destroy the edit first (no save), then delete from disk, then remove tab. */
    if (nw->tab[i].edit) DestroyWindow(nw->tab[i].edit);
    nw->tab[i].edit = NULL;
    app_delete_note(app, id);
    nw_remove_tab(nw, hwnd, i);
    /* nw_remove_tab may have destroyed hwnd; do not touch nw after this */
}

/* ---- Rename overlay (Task 10 / Ctrl+R) ---- */
static void nw_commit_rename(NoteWin* nw, HWND hwnd, int accept); /* forward */

static LRESULT CALLBACK nw_rename_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR id, DWORD_PTR ref) {
    (void)id;
    NoteWin* nw = (NoteWin*)ref;
    HWND parent = GetParent(h);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { nw_commit_rename(nw, parent, 1); return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { nw_commit_rename(nw, parent, 0); return 0; }
    if (msg == WM_KILLFOCUS) { if (nw->rename_edit) { nw_commit_rename(nw, parent, 1); return 0; } }
    return DefSubclassProc(h, msg, wp, lp);
}

/* The rename field's rect: nested inside the tab being renamed, leaving room for
 * the 2px accent frame drawn around it. Shared by begin_rename and the painter. */
static void nw_rename_rect(NoteWin* nw, HWND hwnd, RECT* r) {
    RECT t; nw_tab_rect(nw, hwnd, nw->rename_tab, &t);
    int tab_top = nw_tab_top(hwnd);
    int ex = t.left + nw_s(hwnd, 8), ey = tab_top + nw_s(hwnd, 3);
    int ew = (t.right - t.left) - nw_s(hwnd, 16); if (ew < nw_s(hwnd, 40)) ew = nw_s(hwnd, 40);
    int eh = nw_titlebar_h(hwnd) - tab_top - nw_s(hwnd, 6);
    SetRect(r, ex, ey, ex + ew, ey + eh);
}

static void nw_begin_rename(NoteWin* nw, HWND hwnd) {
    if (nw->rename_edit || nw->ntabs == 0) return;
    int i = nw->active;
    NoteMeta* m = prefs_find(&nw->app->prefs, nw->tab[i].id);
    nw->rename_tab = i;
    RECT er; nw_rename_rect(nw, hwnd, &er);
    nw->rename_edit = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,     /* no border — we draw an accent frame */
        er.left, er.top, er.right - er.left, er.bottom - er.top,
        hwnd, (HMENU)ID_RENAME, GetModuleHandleW(NULL), NULL);
    nw->rename_font = CreateFontW(-nw_s(hwnd,14),0,0,0,FW_SEMIBOLD,0,0,0,0,0,0,0,0,L"Segoe UI");
    SendMessageW(nw->rename_edit, WM_SETFONT, (WPARAM)nw->rename_font, TRUE);
    SetWindowTextA(nw->rename_edit, m && m->name[0] ? m->name : "");
    SendMessageW(nw->rename_edit, EM_SETSEL, 0, -1);
    SetWindowSubclass(nw->rename_edit, nw_rename_sub, 0, (DWORD_PTR)nw);
    InvalidateRect(hwnd, NULL, FALSE);   /* draw the accent frame */
    SetFocus(nw->rename_edit);
}

static void nw_commit_rename(NoteWin* nw, HWND hwnd, int accept) {
    if (!nw->rename_edit) return;
    HWND e = nw->rename_edit;
    nw->rename_edit = NULL;              /* clear first: kill-focus reentry guard */
    if (accept) {
        char buf[64];
        GetWindowTextA(e, buf, sizeof buf);
        char* s = buf; while (*s == ' ') s++;
        size_t n = strlen(s); while (n > 0 && s[n-1] == ' ') s[--n] = '\0';
        if (s[0]) {
            NoteMeta* m = prefs_find(&nw->app->prefs, nw->tab[nw->rename_tab].id);
            if (m) { snprintf(m->name, sizeof m->name, "%s", s); nw_save_tab(nw, nw->rename_tab); app_persist(nw->app); }
        }
    }
    DestroyWindow(e);
    if (nw->rename_font) { DeleteObject(nw->rename_font); nw->rename_font = NULL; }
    InvalidateRect(hwnd, NULL, TRUE);
    if (nw_edit(nw)) SetFocus(nw_edit(nw));
}

/* Handle shortcuts forwarded from a RichEdit via EN_MSGFILTER. Returns nonzero
 * if the keystroke was consumed (so the control ignores it). Global shortcuts
 * (new/delete) work from either box; formatting shortcuts apply to the body
 * only and consume the key so RichEdit's built-in Ctrl+B/I don't also fire.
 * (Alt+N — new window — is a system key that RichEdit does not forward via
 * EN_MSGFILTER, so it is handled in the main message loop, not here.) */
static LRESULT nw_handle_key(NoteWin* nw, HWND hwnd, const MSGFILTER* mf) {
    if (mf->msg != WM_KEYDOWN) return 0;
    BOOL ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    BOOL shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    if (!ctrl) return 0;
    if (mf->wParam == 'N' && !shift) { nw_add_tab(nw, hwnd); return 1; }
    if (mf->wParam == 'R' && !shift) { nw_begin_rename(nw, hwnd); return 1; }
    if (mf->wParam == 'D' && shift)  { nw_delete_note(nw, hwnd); return 1; }

    if (mf->nmhdr.idFrom == ID_EDIT) {
        if (mf->wParam == 'B' && !shift) { nw_wrap_inline(nw, "**", 0); return 1; }
        if (mf->wParam == 'I' && !shift) { nw_wrap_inline(nw, "*",  1); return 1; }
        if (mf->wParam == 'X' && shift)  { nw_wrap_inline(nw, "~~", 2); return 1; }
        if (mf->wParam == '8' && shift)  { nw_list_prefix(nw, 0);    return 1; }
        if (mf->wParam == '7' && shift)  { nw_list_prefix(nw, 1);    return 1; }
    }
    return 0;
}

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
        RECT shape = { fill_left, r.top + nw_s(hwnd, TAB_TOP_GAP), r.right, r.bottom };

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

    nw_paint_tabs(nw, hwnd);
}

static LRESULT CALLBACK nw_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    NoteWin* nw = nw_get(hwnd);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        nw = (NoteWin*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)nw);
        nw->drag_tab = -1;   /* calloc zeros, but 0 is a valid tab index */
        nw->hot_tab  = -1;

        /* Dark native title bar (best-effort; ignored on older Windows). */
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);

        /* Formatting sidebar: owner-drawn buttons (so they paint dark). */
        static const struct { int id; const wchar_t* glyph; } btndefs[NUM_BTNS] = {
            { IDB_BOLD, L"B" }, { IDB_ITALIC, L"I" }, { IDB_STRIKE, L"S" },
            { IDB_BULLET, L"\x2022" }, { IDB_NUM, L"1." },
        };
        for (int i = 0; i < NUM_BTNS; i++) {
            nw->btns[i] = CreateWindowExW(0, L"BUTTON", btndefs[i].glyph,
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, SIDEBAR_W, SIDEBAR_W, hwnd,   /* real size set in nw_layout */
                (HMENU)(INT_PTR)btndefs[i].id, cs->hInstance, NULL);
            SetWindowSubclass(nw->btns[i], nw_btn_sub, (UINT_PTR)i, (DWORD_PTR)nw);
        }

        /* Custom frame: extend the client over the caption (so we draw the title
         * bar) while keeping the real resize frame + shadow. */
        MARGINS m = { 0, 0, 1, 0 };
        DwmExtendFrameIntoClientArea(hwnd, &m);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

        /* one body RichEdit per tab; active shown, rest hidden */
        WinMeta* w = prefs_find_window(&nw->app->prefs, nw->win_id);
        nw->ntabs = w ? w->ntabs : 0;
        if (nw->ntabs > WIN_MAX_TABS) nw->ntabs = WIN_MAX_TABS;
        nw->active = (w && w->active < nw->ntabs) ? w->active : 0;
        if (nw->active < 0) nw->active = 0;
        for (int i = 0; i < nw->ntabs; i++) {
            snprintf(nw->tab[i].id, sizeof nw->tab[i].id, "%s", w->tabs[i]);
            nw->tab[i].edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL
                  | (i == nw->active ? WS_VISIBLE : 0),
                0, 0, 100, 100, hwnd, (HMENU)ID_EDIT, cs->hInstance, NULL);
            SendMessageW(nw->tab[i].edit, EM_SETBKGNDCOLOR, 0, (LPARAM)COL_BG);
            CHARFORMAT2W cf; memset(&cf, 0, sizeof cf);
            cf.cbSize = sizeof cf; cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
    cf.crTextColor = COL_TEXT; cf.yHeight = nw_body_yheight(hwnd); wcscpy(cf.szFaceName, L"IBM Plex Mono");
            SendMessageW(nw->tab[i].edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
            SendMessageW(nw->tab[i].edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
            nw_load_tab(nw, i);
            nw_apply_format_edit(nw, i);    /* style each loaded body */
        }
        nw_layout(hwnd, nw);
        nw_update_toggles(nw);
        if (nw_edit(nw)) SetFocus(nw_edit(nw));
        return 0;
    }
    case WM_SIZE:
        if (nw) {
            if (nw->gfx) {
                RECT rc; GetClientRect(hwnd, &rc);
                gfx_resize(nw->gfx, rc.right - rc.left, rc.bottom - rc.top);
            }
            nw_layout(hwnd, nw);
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
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
    case WM_NCCALCSIZE:                   /* claim the caption area as client */
        if (wp) {
            NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lp;
            int bx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            int by = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
            p->rgrc[0].left   += bx;
            p->rgrc[0].right  -= bx;
            p->rgrc[0].bottom -= by;
            if (IsZoomed(hwnd))
                p->rgrc[0].top += by;   /* maximized: keep top on-screen */
            /* else leave top: client extends over where the caption was */
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_NCHITTEST: {
        /* Let DWM/native frame handle real non-client things first:
         * resize borders, corners, shadow, and related frame behavior. */
        LRESULT dwm_hit = 0;
        if (DwmDefWindowProc(hwnd, msg, wp, lp, &dwm_hit)) {
            return dwm_hit;
        }

        LRESULT def = DefWindowProcW(hwnd, msg, wp, lp);
        if (def != HTCLIENT) {
            return def;
        }

        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);

        RECT rc;
        GetClientRect(hwnd, &rc);

        /* Custom caption buttons own the full titlebar height, including the
        * restored-only top gutter, matching Windows Terminal. */
        int btns_l = nw_window_buttons_left(hwnd);
        if (pt.y >= 0 && pt.y < nw_titlebar_h(hwnd) && pt.x >= btns_l) {
            return HTCLIENT;
        }

        /* Top resize strip inside our custom titlebar area. This keeps the
        * Terminal-like top edge draggable/resizable when restored. */
        int resize_h = nw_top_gutter(hwnd);

        if (!IsZoomed(hwnd) && pt.y >= 0 && pt.y < resize_h) {
            if (pt.x < resize_h) return HTTOPLEFT;
            if (pt.x >= rc.right - resize_h) return HTTOPRIGHT;
            return HTTOP;
        }

        if (pt.y >= 0 && pt.y < nw_titlebar_h(hwnd)) {
            int idx = -1;
            TabHit h = nw ? nw_tab_hit(nw, hwnd, pt.x, pt.y, &idx) : HIT_NONE;

            if (h != HIT_NONE) {
                return HTCLIENT;
            }

            return HTCAPTION;
        }

        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 240;
        mmi->ptMinTrackSize.y = 180;
        return 0;
    }
    case WM_CTLCOLOREDIT:                 /* dark background for the rename field */
        if (nw && (HWND)lp == nw->rename_edit) {
            HDC dc = (HDC)wp;
            SetTextColor(dc, COL_TEXT);
            SetBkColor(dc, COL_RENAME);
            if (!g_rename_brush) g_rename_brush = CreateSolidBrush(COL_RENAME);
            return (LRESULT)g_rename_brush;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_SETFOCUS:
        if (nw && nw_edit(nw)) SetFocus(nw_edit(nw));   /* keep focus on the editor */
        return 0;
    case WM_LBUTTONDOWN:
        if (nw) {
            int wb = nw_wbtn_hit(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (wb == 0) { ShowWindow(hwnd, SW_MINIMIZE); return 0; }
            if (wb == 1) { ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); return 0; }
            if (wb == 2) { SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
            int idx = -1;
            TabHit h = nw_tab_hit(nw, hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &idx);
            if (h == HIT_PLUS)  { nw_add_tab(nw, hwnd); return 0; }
            if (h == HIT_TAB) {
                nw_activate(nw, hwnd, idx);
                RECT r; nw_tab_rect(nw, hwnd, idx, &r);
                nw->drag_tab = idx;
                nw->drag_dx  = GET_X_LPARAM(lp) - r.left;
                nw->press_x  = GET_X_LPARAM(lp);
                nw->dragging = 0;
                SetCapture(hwnd);
                return 0;
            }
            if (h == HIT_CLOSE) { nw_close_tab(nw, hwnd, idx); return 0; }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (nw && GetCapture() == hwnd && nw->drag_tab >= 0) {
            int x = GET_X_LPARAM(lp);
            if (!nw->dragging && abs(x - nw->press_x) > DRAG_THRESHOLD) nw->dragging = 1;
            if (nw->dragging) {
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
                    InvalidateRect(hwnd, NULL, TRUE);
                }
            }
            return 0;
        }
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
    case WM_LBUTTONUP:
        if (nw && GetCapture() == hwnd) {
            ReleaseCapture();
            nw->drag_tab = -1;
            nw->dragging = 0;
        }
        return 0;
    case WM_MBUTTONDOWN:
        if (nw) {
            int idx = -1;
            TabHit h = nw_tab_hit(nw, hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &idx);
            if (h == HIT_TAB || h == HIT_CLOSE) { nw_close_tab(nw, hwnd, idx); return 0; }
        }
        return 0;
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (nw && hdr->code == EN_SELCHANGE && hdr->idFrom == ID_EDIT) {
            {
                HWND e = nw_edit(nw);
                CHARRANGE sel; SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
                int len; char* b = nw_body_text(e, &len);
                if (b) {
                    size_t lo = (size_t)sel.cpMin, hi = lo;
                    nw_para_bounds(b, len, &lo, &hi);
                    free(b);
                    if (lo != nw->last_caret_para) {
                        nw->last_caret_para = lo;
                        SendMessageW(e, WM_SETREDRAW, FALSE, 0);
                        nw_restyle(nw, nw->active, 0);   /* whole doc: flip reveal */
                        SendMessageW(e, WM_SETREDRAW, TRUE, 0);
                        InvalidateRect(e, NULL, TRUE);
                    }
                }
            }
            nw_update_toggles(nw);
            return 0;
        }
        if (nw && hdr->code == EN_MSGFILTER && hdr->idFrom == ID_EDIT) {
            MSGFILTER* mf = (MSGFILTER*)lp;
            /* Enter in the body continues a list item, if on one. */
            if (mf->msg == WM_KEYDOWN &&
                mf->wParam == VK_RETURN && nw_handle_enter(nw)) {
                return 1;
            }
            return nw_handle_key(nw, hwnd, mf);
        }
        return 0;
    }
    case WM_CLOSE:                     /* native title-bar X */
        note_window_close(hwnd);
        return 0;
    case WM_EXITSIZEMOVE: {
        if (!nw) return 0;
        WinMeta* w = nw_win(nw);
        if (!w) return 0;
        nw_save_geometry(hwnd, w);
        app_persist(nw->app);   /* geometry moved — persist for session restore */
        return 0;
    }
    case WM_DRAWITEM:
        if (nw && wp >= IDB_BOLD && wp <= IDB_NUM) {
            nw_draw_button(nw, (const DRAWITEMSTRUCT*)lp);
            return TRUE;
        }
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED && nw) {
            switch (LOWORD(wp)) {
            case IDB_BOLD:   nw_wrap_inline(nw, "**", 0); break;
            case IDB_ITALIC: nw_wrap_inline(nw, "*",  1); break;
            case IDB_STRIKE: nw_wrap_inline(nw, "~~", 2); break;
            case IDB_BULLET: nw_list_prefix(nw, 0);    break;
            case IDB_NUM:    nw_list_prefix(nw, 1);    break;
            default: return 0;
            }
            if (nw_edit(nw)) SetFocus(nw_edit(nw));   /* return focus to the body after a click */
            return 0;
        }
        if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == ID_EDIT) {
            SetTimer(hwnd, IDT_DEBOUNCE, DEBOUNCE_MS, NULL);  /* coalesces */
        }
        return 0;
    case WM_TIMER:
        if (wp == IDT_DEBOUNCE && nw && nw_edit(nw)) {
            HWND e = nw_edit(nw);
            KillTimer(hwnd, IDT_DEBOUNCE);
            SendMessageW(e, WM_SETREDRAW, FALSE, 0);
            nw_save_tab(nw, nw->active);
            nw_restyle(nw, nw->active, 1);   /* scoped to the edited paragraph */
            SendMessageW(e, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(e, NULL, TRUE);
            nw_update_toggles(nw);
        }
        return 0;
    case WM_DESTROY:
        nw_registry_remove(hwnd);
        if (nw) {
            if (nw->gfx) { gfx_destroy(nw->gfx); nw->gfx = NULL; }
            if (nw->rename_font) { DeleteObject(nw->rename_font); nw->rename_font = NULL; }
            free(nw);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void note_window_register_class(HINSTANCE hInst) {
    if (!g_richedit) g_richedit = LoadLibraryW(L"Msftedit.dll");
    if (!g_bg_brush) g_bg_brush = CreateSolidBrush(COL_BG);
    if (!g_border_brush) g_border_brush = CreateSolidBrush(COL_BORDER);
    gfx_global_init();
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = nw_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); /* IDC_ARROW */
    wc.hbrBackground = g_bg_brush;     /* dark fill avoids white flash on resize */
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));   /* qN, taskbar */
    wc.hIconSm = wc.hIcon;
    if (!g_app_icon) g_app_icon = wc.hIcon;                  /* painted in top-left cell */
    wc.lpszClassName = NOTE_CLASS;
    RegisterClassExW(&wc);
}

/* Offset a NEW window's position in a cascade, down-right from the window the
 * user was on (the foreground note window, else the most recent open one),
 * wrapping to the work-area top-left when it would run off screen. The step is
 * the system caption height + frame — the classic Windows cascade offset
 * (~30px). Restored windows keep their saved geometry and must NOT call this. */
void note_window_place_cascade(AppState* app, WinMeta* w) {
    (void)app;
    if (!w) return;
    int step = GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYSIZEFRAME)
             + GetSystemMetrics(SM_CXPADDEDBORDER);
    if (step < 8) step = 30;                       /* sane fallback */

    HWND ref = NULL;                               /* the window to cascade from */
    HWND fg = GetForegroundWindow();
    wchar_t cls[32];
    if (fg && GetClassNameW(fg, cls, 32) && wcscmp(cls, NOTE_CLASS) == 0) ref = fg;
    if (!ref)
        for (int i = NW_REGISTRY_CAP - 1; i >= 0; i--)
            if (s_registry[i].hwnd) { ref = s_registry[i].hwnd; break; }

    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int bx = w->x, by = w->y;
    if (ref) { RECT rr; GetWindowRect(ref, &rr); bx = rr.left + step; by = rr.top + step; }
    if (bx + w->w > wa.right || by + w->h > wa.bottom) { bx = wa.left + step; by = wa.top + step; }
    if (bx < wa.left) bx = wa.left + step;
    if (by < wa.top)  by = wa.top + step;
    w->x = bx; w->y = by;
}

HWND note_window_open(AppState* app, WinMeta* win) {
    NoteWin* nw = calloc(1, sizeof *nw);
    if (!nw) return NULL;
    nw->app = app;
    snprintf(nw->win_id, sizeof nw->win_id, "%s", win->id);
    /* Standard overlapped window (native frame: resize, snap, shadow, system
     * menu, taskbar), but WM_NCCALCSIZE extends the client over the caption so
     * the title bar (icon, name, min/max/close) is drawn by us. */
    HWND h = CreateWindowExW(0, NOTE_CLASS, L"quickNote",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE,
        win->x, win->y, win->w, win->h,
        NULL, NULL, GetModuleHandleW(NULL), nw);
    if (h) { nw_registry_add(nw->win_id, h); app_persist(app); }
    else   free(nw);
    return h;
}

void note_window_close(HWND hwnd) {
    NoteWin* nw = nw_get(hwnd);
    if (nw) {
        WinMeta* w = nw_win(nw);
        if (w) {
            nw_save_geometry(hwnd, w);
            for (int i = 0; i < nw->ntabs; i++) nw_save_tab(nw, i);
            w->active = nw->active;
            /* Remove from prefs: the X-closed window should not reopen next launch.
             * The notes themselves remain in prefs.notes and on disk. */
            prefs_remove_window(&nw->app->prefs, w->id);
            app_persist(nw->app);   /* window closed — persist for session restore */
        }
    }
    DestroyWindow(hwnd);
}

/* Force-save every open window's geometry + all tab bodies (used on Quit, when
 * windows aren't individually closed so their pending edits would otherwise be
 * lost). Mirrors runtime state into the persisted WinMeta for each. */
void note_window_save_all(void) {
    for (int r = 0; r < NW_REGISTRY_CAP; r++) {
        HWND hwnd = s_registry[r].hwnd;
        if (!hwnd) continue;
        NoteWin* nw = nw_get(hwnd);
        if (!nw) continue;
        WinMeta* w = nw_win(nw);
        if (w) {
            nw_save_geometry(hwnd, w);
            for (int i = 0; i < nw->ntabs; i++) nw_save_tab(nw, i);
            w->active = nw->active;
            nw_sync_winmeta(nw);
        }
    }
}

void note_window_activate_note(HWND hwnd, const char* note_id) {
    NoteWin* nw = nw_get(hwnd);
    if (!nw) return;
    for (int t = 0; t < nw->ntabs; t++) {
        if (strcmp(nw->tab[t].id, note_id) == 0) {
            nw_activate(nw, hwnd, t);   /* handles hide/show/sync/toggles/focus (no-op if already active) */
            SetForegroundWindow(hwnd);
            if (nw_edit(nw)) SetFocus(nw_edit(nw));
            return;
        }
    }
}
