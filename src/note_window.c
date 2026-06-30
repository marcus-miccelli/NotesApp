#include "note_window.h"
#include "store.h"
#include "markdown.h"
#include "resource.h"
#include <richedit.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOTE_CLASS   L"StickyNoteWindow"
#define ID_EDIT      1001
#define DEBOUNCE_MS  150
#define IDT_DEBOUNCE 1

/* Formatting sidebar buttons. */
#define IDB_BOLD     1100
#define IDB_ITALIC   1101
#define IDB_STRIKE   1102
#define IDB_BULLET   1103
#define IDB_NUM      1104
#define NUM_BTNS     5

/* Custom window-chrome buttons (min / max / close), top-right of the title bar. */
#define IDW_MIN      1200
#define IDW_MAX      1201
#define IDW_CLOSE    1202
#define NUM_WBTNS    3

/* Layout (px). Three bordered boxes — sidebar, title, body — sit inside the
 * client area with an outer pad and a gap between them; each control is inset
 * a little inside its box so the border shows and text has padding. */
#define PAD_OUTER    8     /* client edge -> boxes */
#define REGION_GAP   8     /* between boxes */
#define SIDEBAR_W    40    /* sidebar width (buttons are this wide, square) */
#define TITLEBAR_H   34    /* custom title bar height (horizontal divider y) */
#define WINBTN_W     46    /* min/max/close button width (native caption width) */
#define BTN_GAP      3     /* between sidebar buttons */

/* Tab strip geometry (px). */
#define DRAG_GAP      36   /* gap between last tab/+ and the window buttons */
#define PLUS_W        28   /* width of the + add-tab button */
#define TAB_MIN       48   /* minimum tab width */
#define TAB_MAX      200   /* maximum tab width */
#define TAB_CLOSE_W   18   /* per-tab close (✕) region on the right */
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

/* Dark palette */
static const COLORREF COL_BG     = RGB(0x1e, 0x1e, 0x22);
static const COLORREF COL_TEXT   = RGB(0xe6, 0xe6, 0xe6);
static const COLORREF COL_BORDER = RGB(0x44, 0x44, 0x4e);  /* region outlines */
static const COLORREF COL_TOGGLE = RGB(0x3a, 0x3a, 0x46);  /* pressed button fill */
static const COLORREF COL_CLOSE  = RGB(0xc0, 0x3a, 0x3a);  /* close button hover */
static const COLORREF COL_HOVER  = RGB(0x3a, 0x3a, 0x44);  /* min/max button hover */

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
    HWND  wbtns[NUM_WBTNS];
    int   whot[NUM_WBTNS];
    int   active_fmt[NUM_BTNS];
} NoteWin;

static HMODULE g_richedit = NULL;
static HBRUSH  g_bg_brush = NULL;     /* class background, dark; created once */
static HBRUSH  g_border_brush = NULL; /* region outline; created once */
static HICON   g_app_icon = NULL;     /* qN, painted in the top-left cell */

static NoteWin* nw_get(HWND h) { return (NoteWin*)GetWindowLongPtrW(h, GWLP_USERDATA); }

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
 * x=SIDEBAR_W and a full-width horizontal line at y=TITLEBAR_H. They make four
 * cells — top-left the app-icon square, top-right the title bar (name + window
 * buttons), bottom-left the format sidebar, bottom-right the body. vx/hy are the
 * dividers; title/body are control rects. Any out param may be NULL. */
static void nw_regions(HWND hwnd, RECT* title, RECT* body, int* vx, int* hy) {
    RECT rc; GetClientRect(hwnd, &rc);
    int v  = SIDEBAR_W;                              /* vertical divider x */
    int h  = TITLEBAR_H;                             /* horizontal divider y */
    int cl = v + REGION_GAP;                         /* content left */
    int btns_l = rc.right - NUM_WBTNS * WINBTN_W;    /* window buttons start */
    int t_r = btns_l - REGION_GAP; if (t_r < cl) t_r = cl;
    int cr = rc.right - PAD_OUTER; if (cr < cl) cr = cl;
    int b_top = h + REGION_GAP, b_bot = rc.bottom - PAD_OUTER;
    if (b_bot < b_top) b_bot = b_top;
    if (title) SetRect(title, cl, 4, t_r, h - 4);    /* name, left of the buttons */
    if (body)  SetRect(body,  cl, b_top, cr, b_bot);
    if (vx) *vx = v;
    if (hy) *hy = h;
}

/* Tab strip geometry helpers — shared by paint (Task 6) and hit-test (Task 7). */
static int nw_strip_left(void)  { return SIDEBAR_W + 1; }
static int nw_strip_right(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int btns_l = rc.right - NUM_WBTNS * WINBTN_W;
    return btns_l - DRAG_GAP;
}
static int nw_tab_w(NoteWin* nw, HWND hwnd) {
    int avail = nw_strip_right(hwnd) - nw_strip_left() - PLUS_W;
    int n = nw->ntabs > 0 ? nw->ntabs : 1;
    int w = avail / n;
    if (w > TAB_MAX) w = TAB_MAX;
    if (w < TAB_MIN) w = TAB_MIN;
    return w;
}
static void nw_tab_rect(NoteWin* nw, HWND hwnd, int i, RECT* r) {
    int w = nw_tab_w(nw, hwnd), l = nw_strip_left() + i * w;
    SetRect(r, l, 0, l + w, TITLEBAR_H);
}
static void nw_plus_rect(NoteWin* nw, HWND hwnd, RECT* r) {
    int w = nw_tab_w(nw, hwnd), l = nw_strip_left() + nw->ntabs * w;
    SetRect(r, l, 0, l + PLUS_W, TITLEBAR_H);
}
static TabHit nw_tab_hit(NoteWin* nw, HWND hwnd, int cx, int cy, int* out_idx) {
    if (cy < 0 || cy >= TITLEBAR_H) return HIT_NONE;
    for (int i = 0; i < nw->ntabs; i++) {
        RECT r; nw_tab_rect(nw, hwnd, i, &r);
        if (cx >= r.left && cx < r.right) {
            if (out_idx) *out_idx = i;
            if (cx >= r.right - TAB_CLOSE_W) return HIT_CLOSE;
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
    RECT rc; GetClientRect(hwnd, &rc);
    RECT tt, bd; int vx, hy;
    nw_regions(hwnd, &tt, &bd, &vx, &hy);

    int side = vx;                                   /* square == sidebar width */
    for (int i = 0; i < NUM_BTNS; i++)
        MoveWindow(nw->btns[i], 0, hy + 1 + i * (side + BTN_GAP),
                   side, side, TRUE);

    for (int i = 0; i < NUM_WBTNS; i++)              /* min, max, close L->R */
        MoveWindow(nw->wbtns[i], rc.right - (NUM_WBTNS - i) * WINBTN_W, 0,
                   WINBTN_W, hy, TRUE);

    (void)tt;
    for (int i = 0; i < nw->ntabs; i++)
        MoveWindow(nw->tab[i].edit, bd.left, bd.top,
                   bd.right - bd.left, bd.bottom - bd.top, TRUE);
}

/* Owner-draw a sidebar button: blends with the dark sidebar (no distinct fill)
 * until it is pressed or its format is active at the caret, when the square
 * fills with the toggle accent. The glyph is rendered in the style it applies
 * (bold B, italic I, struck-through S). */
static void nw_draw_button(NoteWin* nw, const DRAWITEMSTRUCT* d) {
    int idx = (int)d->CtlID - IDB_BOLD;
    int on = (d->itemState & ODS_SELECTED) != 0 ||
             (idx >= 0 && idx < NUM_BTNS && nw->active_fmt[idx]);
    HBRUSH bg = CreateSolidBrush(on ? COL_TOGGLE : COL_BG);
    FillRect(d->hDC, &d->rcItem, bg);
    DeleteObject(bg);

    LOGFONTW lf; memset(&lf, 0, sizeof lf);
    lf.lfHeight = -16; lf.lfWeight = FW_BOLD; wcscpy(lf.lfFaceName, L"Segoe UI");
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

/* Owner-draw a window caption button (min/max/close) like the native ones:
 * transparent until hovered/pressed (close goes red), with a Segoe MDL2 Assets
 * glyph. The maximize button shows the restore glyph while the window is zoomed. */
static void nw_draw_wbutton(NoteWin* nw, HWND parent, const DRAWITEMSTRUCT* d) {
    int idx = (int)d->CtlID - IDW_MIN;
    int hot = (idx >= 0 && idx < NUM_WBTNS && nw->whot[idx]);
    int pressed = (d->itemState & ODS_SELECTED) != 0;

    COLORREF fill = COL_BG;
    if (d->CtlID == IDW_CLOSE) { if (hot || pressed) fill = COL_CLOSE; }
    else if (pressed)          fill = COL_TOGGLE;
    else if (hot)              fill = COL_HOVER;
    HBRUSH bg = CreateSolidBrush(fill);
    FillRect(d->hDC, &d->rcItem, bg);
    DeleteObject(bg);

    const wchar_t* glyph = L"\xE921";                      /* min */
    if (d->CtlID == IDW_MAX)   glyph = IsZoomed(parent) ? L"\xE923" : L"\xE922"; /* restore/max */
    if (d->CtlID == IDW_CLOSE) glyph = L"\xE8BB";          /* close */

    LOGFONTW lf; memset(&lf, 0, sizeof lf);
    lf.lfHeight = -10; lf.lfWeight = FW_NORMAL; wcscpy(lf.lfFaceName, L"Segoe MDL2 Assets");
    HFONT f = CreateFontIndirectW(&lf);
    HFONT old = (HFONT)SelectObject(d->hDC, f);
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, RGB(0xe6,0xe6,0xe6));
    RECT r = d->rcItem;
    DrawTextW(d->hDC, glyph, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(d->hDC, old);
    DeleteObject(f);
}

/* Subclass for caption buttons: track hover so the button highlights like the
 * native ones (BS_OWNERDRAW alone gives no hot state). */
static LRESULT CALLBACK nw_wbtn_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR id, DWORD_PTR ref) {
    NoteWin* nw = (NoteWin*)ref;
    if (msg == WM_MOUSEMOVE) {
        if (!nw->whot[id]) {
            nw->whot[id] = 1;
            TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(h, NULL, TRUE);
        }
    } else if (msg == WM_MOUSELEAVE) {
        nw->whot[id] = 0;
        InvalidateRect(h, NULL, TRUE);
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
    LONG caret = sel.cpMin;
    int len; char* buf = nw_body_text(e, &len);
    if (buf) {
        MdSpan spans[256];
        size_t n = markdown_spans(buf, (size_t)len, spans, 256);
        if (n > 256) n = 256;
        MdFmt f = 0;
        for (size_t i = 0; i < n; i++) {
            LONG a = (LONG)spans[i].start, b = a + (LONG)spans[i].len;
            /* cover the char on either side of the caret, so the highlight
             * holds right at a format boundary too */
            if ((caret > a && caret <= b) || (caret >= a && caret < b))
                f |= spans[i].fmt;
        }
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
 * nw_set_range_fmt / nw_apply_format — live markdown styling on each debounce tick.
 *
 * CRLF/offset note: GetWindowTextA returns \r\n (2 bytes per line break), but
 * RichEdit's internal character positions (used by EM_EXSETSEL / CHARRANGE) count
 * each paragraph break as a SINGLE character.  Feeding a \r\n buffer to
 * markdown_spans would produce byte offsets that drift +1 per preceding line break,
 * causing formatting to land on wrong characters for multi-line notes.
 *
 * Fix (option b — clean): nw_apply_format uses EM_GETTEXTEX with GT_DEFAULT and
 * CP_ACP, which returns the text with \r-only paragraph separators (matching
 * RichEdit's internal indexing).  The resulting byte offsets from markdown_spans
 * are correct for EM_EXSETSEL.  nw_save_tab is unaffected — it still uses
 * GetWindowTextA whose \r\n output is correct for .md files on Windows.
 *
 * md4c treats bare \r as a valid line ending (CommonMark §2.3), so parsing is
 * unaffected.
 */
/* Apply CFE_HIDDEN to a character range: the chars stay in the buffer (so the
 * .md keeps its markers) but render at zero width. Used to "consume" markdown
 * delimiters so **bold** displays as just bold. */
static void nw_hide_range(HWND edit, size_t start, size_t len) {
    if (len == 0) return;
    CHARRANGE r; r.cpMin = (LONG)start; r.cpMax = (LONG)(start + len);
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&r);
    CHARFORMAT2W cf; memset(&cf, 0, sizeof cf); cf.cbSize = sizeof cf;
    cf.dwMask = CFM_HIDDEN; cf.dwEffects = CFE_HIDDEN;
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

/* Hide the delimiter characters belonging to one formatted span, derived from
 * the literal markers adjacent to its content in buf:
 *   headings  -> the leading "#...# " up to the content (no trailing marker)
 *   code      -> the run of backticks on each side
 *   bold/italic -> up to (bold?2:0)+(italic?1:0) of * or _ on each side
 * We only hide markers next to a span md4c actually recognized, so stray or
 * escaped asterisks (no span) are left visible. */
static void nw_apply_hidden(HWND edit, const char* buf, int len, const MdSpan* s) {
    size_t a = s->start, b = s->start + s->len;
    MdFmt f = s->fmt;
    if (f & (MD_FMT_H1 | MD_FMT_H2 | MD_FMT_H3)) {
        size_t ls = a;
        while (ls > 0 && buf[ls-1] != '\n' && buf[ls-1] != '\r') ls--;
        nw_hide_range(edit, ls, a - ls);          /* "# ", "## ", ... */
        return;
    }
    if (f & MD_FMT_CODE) {
        size_t i = a, cnt = 0; while (i > 0 && buf[i-1] == '`') { i--; cnt++; }
        nw_hide_range(edit, a - cnt, cnt);
        size_t j = b; cnt = 0; while (j < (size_t)len && buf[j] == '`') { j++; cnt++; }
        nw_hide_range(edit, b, cnt);
        return;
    }
    int want = ((f & MD_FMT_BOLD) ? 2 : 0) + ((f & MD_FMT_ITALIC) ? 1 : 0);
    if (want) {
        size_t i = a; int cnt = 0;
        while (i > 0 && cnt < want && (buf[i-1] == '*' || buf[i-1] == '_')) { i--; cnt++; }
        nw_hide_range(edit, a - (size_t)cnt, (size_t)cnt);
        size_t j = b; cnt = 0;
        while (j < (size_t)len && cnt < want && (buf[j] == '*' || buf[j] == '_')) { j++; cnt++; }
        nw_hide_range(edit, b, (size_t)cnt);
    }
    if (f & MD_FMT_STRIKE) {                 /* ~~ on each side */
        size_t i = a; int cnt = 0;
        while (i > 0 && cnt < 2 && buf[i-1] == '~') { i--; cnt++; }
        nw_hide_range(edit, a - (size_t)cnt, (size_t)cnt);
        size_t j = b; cnt = 0;
        while (j < (size_t)len && cnt < 2 && buf[j] == '~') { j++; cnt++; }
        nw_hide_range(edit, b, (size_t)cnt);
    }
}

static void nw_set_range_fmt(HWND edit, size_t start, size_t len, MdFmt fmt) {
    CHARRANGE saved; SendMessageW(edit, EM_EXGETSEL, 0, (LPARAM)&saved);
    CHARRANGE r; r.cpMin = (LONG)start; r.cpMax = (LONG)(start + len);
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&r);

    CHARFORMAT2W cf; memset(&cf, 0, sizeof cf); cf.cbSize = sizeof cf;
    /* include CFM_HIDDEN (effect bit off) so content is always shown, even if a
     * prior pass had hidden these positions. */
    cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_SIZE | CFM_FACE | CFM_COLOR | CFM_HIDDEN;
    cf.crTextColor = COL_TEXT;
    cf.yHeight = 200;                       /* 10pt default (twips) */
    if (fmt & MD_FMT_H1) cf.yHeight = 360;
    else if (fmt & MD_FMT_H2) cf.yHeight = 300;
    else if (fmt & MD_FMT_H3) cf.yHeight = 260;
    if (fmt & (MD_FMT_BOLD | MD_FMT_H1 | MD_FMT_H2 | MD_FMT_H3))
        cf.dwEffects |= CFE_BOLD;
    if (fmt & MD_FMT_ITALIC) cf.dwEffects |= CFE_ITALIC;
    if (fmt & MD_FMT_STRIKE) cf.dwEffects |= CFE_STRIKEOUT;
    if (fmt & MD_FMT_CODE) wcscpy(cf.szFaceName, L"Consolas");
    else wcscpy(cf.szFaceName, L"Segoe UI");
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&saved);  /* restore caret */
}

/* Render markdown list lines: every paragraph starting with "- " or "N. " gets
 * its literal marker hidden and a real RichEdit bullet/number with a hanging
 * indent. Non-list paragraphs have numbering cleared, so un-listing a line
 * reverts cleanly. Runs each reformat (events already muted by the caller). */
static void nw_apply_lists(HWND edit, const char* buf, int len) {
    size_t i = 0;
    for (;;) {
        size_t ls = i, le = ls;
        while (le < (size_t)len && buf[le] != '\r' && buf[le] != '\n') le++;

        WORD numbering = 0;          /* 0 none, else PFN_BULLET / PFN_ARABIC */
        size_t mend = ls;            /* end of literal marker to hide */
        if (ls + 1 < le && buf[ls] == '-' && buf[ls+1] == ' ') {
            numbering = PFN_BULLET; mend = ls + 2;
        } else {
            size_t q = ls;
            while (q < le && buf[q] >= '0' && buf[q] <= '9') q++;
            if (q > ls && q + 1 < le && buf[q] == '.' && buf[q+1] == ' ') {
                numbering = PFN_ARABIC; mend = q + 2;
            }
        }

        CHARRANGE r = { (LONG)ls, (LONG)le };
        SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&r);
        PARAFORMAT2 pf; memset(&pf, 0, sizeof pf); pf.cbSize = sizeof pf;
        pf.dwMask = PFM_NUMBERING | PFM_NUMBERINGSTYLE | PFM_NUMBERINGTAB
                  | PFM_OFFSET | PFM_STARTINDENT;
        if (numbering) {
            pf.wNumbering = numbering;
            pf.wNumberingStyle = (numbering == PFN_ARABIC) ? PFNS_PERIOD : PFNS_PLAIN;
            pf.wNumberingTab = 280;
            pf.dxStartIndent = 280;
            pf.dxOffset = 280;       /* hanging indent for wrapped lines */
        }
        SendMessageW(edit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
        if (numbering) nw_hide_range(edit, ls, mend - ls);

        if (le >= (size_t)len) break;
        i = le;
        if (i < (size_t)len && buf[i] == '\r') i++;
        if (i < (size_t)len && buf[i] == '\n') i++;
    }
}

/* Live markdown restyle of one tab's body RichEdit. */
static void nw_apply_format_edit(NoteWin* nw, int i) {
    HWND edit = nw->tab[i].edit;
    /* Applying CHARFORMAT makes RichEdit emit EN_CHANGE/EN_UPDATE. Without
     * suppressing them, each reformat re-arms the debounce timer and we loop
     * forever (constant reflow = flicker + the text never settles formatted).
     * Mute notifications for the duration of the programmatic restyle. */
    SendMessageW(edit, EM_SETEVENTMASK, 0, 0);

    /* Use EM_GETTEXTEX with GT_DEFAULT + CP_ACP: returns \r-only line endings
     * whose byte offsets match RichEdit's internal character positions exactly. */
    GETTEXTLENGTHEX gtl; gtl.flags = GTL_DEFAULT; gtl.codepage = CP_ACP;
    int len = (int)SendMessageW(edit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
    if (len <= 0) { SendMessageW(edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK); return; }
    char* buf = malloc((size_t)len + 1);
    if (!buf) { SendMessageW(edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK); return; }
    GETTEXTEX gte; gte.cb = (DWORD)(len + 1); gte.flags = GT_DEFAULT;
    gte.codepage = CP_ACP; gte.lpDefaultChar = NULL; gte.lpUsedDefChar = NULL;
    SendMessageW(edit, EM_GETTEXTEX, (WPARAM)&gte, (LPARAM)buf);

    /* capture real caret BEFORE any selection changes */
    CHARRANGE saved_sel;
    SendMessageW(edit, EM_EXGETSEL, 0, (LPARAM)&saved_sel);

    /* reset everything to default first */
    CHARRANGE all = { 0, -1 };
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&all);
    CHARFORMAT2W base; memset(&base, 0, sizeof base); base.cbSize = sizeof base;
    /* CFM_HIDDEN (effect off) un-hides everything so deleted/moved markers
     * reappear before we re-hide the current ones. */
    base.dwMask = CFM_BOLD|CFM_ITALIC|CFM_STRIKEOUT|CFM_SIZE|CFM_FACE|CFM_COLOR|CFM_HIDDEN;
    base.yHeight = 200; base.crTextColor = COL_TEXT; wcscpy(base.szFaceName, L"Segoe UI");
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&base);

    MdSpan spans[256];
    size_t n = markdown_spans(buf, (size_t)len, spans, 256);
    if (n > 256) n = 256;
    for (size_t k = 0; k < n; k++) {
        nw_set_range_fmt(edit, spans[k].start, spans[k].len, spans[k].fmt);
        nw_apply_hidden(edit, buf, len, &spans[k]);   /* consume markers */
    }
    nw_apply_lists(edit, buf, len);     /* render "- "/"N. " as bullets/numbers */
    free(buf);

    /* restore the real caret/selection */
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&saved_sel);

    /* re-enable change + key notifications now that the restyle is done */
    SendMessageW(edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
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
}

/* Show tab i, hide the previous active one. No-op if i is invalid or already
 * the active tab. */
static void nw_activate(NoteWin* nw, HWND hwnd, int i) {
    if (i < 0 || i >= nw->ntabs || i == nw->active) return;
    ShowWindow(nw->tab[nw->active].edit, SW_HIDE);
    nw->active = i;
    ShowWindow(nw->tab[i].edit, SW_SHOW);
    nw_sync_winmeta(nw);
    InvalidateRect(hwnd, NULL, TRUE);
    SetFocus(nw->tab[i].edit);
    nw_update_toggles(nw);
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
    cf.cbSize = sizeof cf; cf.dwMask = CFM_COLOR; cf.crTextColor = COL_TEXT;
    SendMessageW(nw->tab[i].edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    SendMessageW(nw->tab[i].edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
    nw_load_tab(nw, i);
    nw_layout(hwnd, nw);
    ShowWindow(nw->tab[nw->active].edit, SW_HIDE);
    nw->active = i;
    ShowWindow(nw->tab[i].edit, SW_SHOW);
    nw_sync_winmeta(nw);
    InvalidateRect(hwnd, NULL, TRUE);
    SetFocus(nw->tab[i].edit);
}

/* Spawn a new note in a brand-new window (offset from this one).
 * Currently unused; repurposed for Alt+N in Task 11. */
static void __attribute__((unused)) nw_new_note(NoteWin* nw) {
    WinMeta* cur = nw_win(nw);       /* read coords BEFORE app_new_window (may realloc) */
    int ox = 224, oy = 224;
    if (cur) { ox = cur->x + 24; oy = cur->y + 24; }
    WinMeta* w = app_new_window(nw->app);
    if (w) { w->x = ox; w->y = oy; note_window_open(nw->app, w); }
}

/* Delete the active note (with confirmation), removing its .md and index entry. */
static void nw_delete_note(NoteWin* nw, HWND hwnd) {
    if (MessageBoxW(hwnd, L"Delete this note permanently?",
                    L"Delete", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    NoteTab* t = nw_cur(nw);
    if (!t) return;
    /* Capture before DestroyWindow: WM_DESTROY frees nw synchronously. */
    char id[16]; strcpy(id, t->id);
    AppState* app = nw->app;
    DestroyWindow(hwnd);
    app_delete_note(app, id);
}

/* Handle shortcuts forwarded from a RichEdit via EN_MSGFILTER. Returns nonzero
 * if the keystroke was consumed (so the control ignores it). Global shortcuts
 * (new/delete) work from either box; formatting shortcuts apply to the body
 * only and consume the key so RichEdit's built-in Ctrl+B/I don't also fire. */
static LRESULT nw_handle_key(NoteWin* nw, HWND hwnd, const MSGFILTER* mf) {
    if (mf->msg != WM_KEYDOWN) return 0;
    BOOL ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    BOOL shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    if (!ctrl) return 0;
    if (mf->wParam == 'N' && !shift) { nw_add_tab(nw, hwnd); return 1; }
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

/* Draw Windows-Terminal-style tab strip in the title bar area. */
static void nw_paint_tabs(NoteWin* nw, HWND hwnd, HDC hdc) {
    HFONT f = CreateFontW(-15,0,0,0,FW_SEMIBOLD,0,0,0,0,0,0,0,0,L"Segoe UI"); /* ~H3 */
    HFONT old = (HFONT)SelectObject(hdc, f);
    SetBkMode(hdc, TRANSPARENT);
    for (int i = 0; i < nw->ntabs; i++) {
        RECT r; nw_tab_rect(nw, hwnd, i, &r);
        int isact = (i == nw->active);
        HBRUSH bg = CreateSolidBrush(isact ? COL_TOGGLE : COL_BG);
        FillRect(hdc, &r, bg); DeleteObject(bg);
        if (isact) {                                  /* accent top edge */
            RECT top = { r.left, 0, r.right, 2 };
            HBRUSH ac = CreateSolidBrush(RGB(0x6a,0x9a,0xff));
            FillRect(hdc, &top, ac); DeleteObject(ac);
        }
        NoteMeta* m = nw_note_meta(nw, i);
        wchar_t wname[128];
        MultiByteToWideChar(CP_ACP, 0, m && m->name[0] ? m->name : "Untitled", -1, wname, 128);
        RECT tr = { r.left + 8, r.top, r.right - TAB_CLOSE_W, r.bottom };
        SetTextColor(hdc, COL_TEXT);
        DrawTextW(hdc, wname, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT xr = { r.right - TAB_CLOSE_W, r.top, r.right, r.bottom };   /* close glyph */
        SetTextColor(hdc, RGB(0xb0,0xb0,0xb8));
        DrawTextW(hdc, L"\x2715", -1, &xr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    RECT pr; nw_plus_rect(nw, hwnd, &pr);            /* + button */
    SetTextColor(hdc, COL_TEXT);
    DrawTextW(hdc, L"+", -1, &pr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old); DeleteObject(f);
}

static LRESULT CALLBACK nw_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    NoteWin* nw = nw_get(hwnd);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        nw = (NoteWin*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)nw);

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
        }

        /* Window-chrome buttons: minimize, maximize, close (— ▢ ✕). */
        static const struct { int id; const wchar_t* glyph; } wbtndefs[NUM_WBTNS] = {
            { IDW_MIN, L"\x2013" }, { IDW_MAX, L"\x25A1" }, { IDW_CLOSE, L"\x2715" },
        };
        for (int i = 0; i < NUM_WBTNS; i++) {
            nw->wbtns[i] = CreateWindowExW(0, L"BUTTON", wbtndefs[i].glyph,
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, WINBTN_W, TITLEBAR_H, hwnd,
                (HMENU)(INT_PTR)wbtndefs[i].id, cs->hInstance, NULL);
            SetWindowSubclass(nw->wbtns[i], nw_wbtn_sub, (UINT_PTR)i, (DWORD_PTR)nw);
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
        for (int i = 0; i < nw->ntabs; i++) {
            snprintf(nw->tab[i].id, sizeof nw->tab[i].id, "%s", w->tabs[i]);
            nw->tab[i].edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL
                  | (i == nw->active ? WS_VISIBLE : 0),
                0, 0, 100, 100, hwnd, (HMENU)ID_EDIT, cs->hInstance, NULL);
            SendMessageW(nw->tab[i].edit, EM_SETBKGNDCOLOR, 0, (LPARAM)COL_BG);
            CHARFORMAT2W cf; memset(&cf, 0, sizeof cf);
            cf.cbSize = sizeof cf; cf.dwMask = CFM_COLOR; cf.crTextColor = COL_TEXT;
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
            nw_layout(hwnd, nw);
            InvalidateRect(hwnd, NULL, TRUE);
            InvalidateRect(nw->wbtns[1], NULL, TRUE);   /* max<->restore glyph */
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int vx, hy; nw_regions(hwnd, NULL, NULL, &vx, &hy);
        /* qN icon centered in the top-left cell */
        if (g_app_icon) {
            int side = (vx < hy ? vx : hy) - 12; if (side < 8) side = 8;
            int ix = (vx - side) / 2, iy = (hy - side) / 2;
            DrawIconEx(hdc, ix, iy, g_app_icon, side, side, 0, NULL, DI_NORMAL);
        }
        RECT vline = { vx, 0, vx + 1, rc.bottom };          /* sidebar | content */
        RECT hline = { 0, hy, rc.right, hy + 1 };           /* full-width divider */
        FillRect(hdc, &vline, g_border_brush);
        FillRect(hdc, &hline, g_border_brush);

        /* Tab strip: all tabs + ✕ glyphs + the + button (Task 6). */
        if (nw) nw_paint_tabs(nw, hwnd, hdc);
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
            if (IsZoomed(hwnd)) p->rgrc[0].top += by;   /* maximized: keep top on-screen */
            /* else leave top: client extends over where the caption was */
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_NCHITTEST: {                  /* resize borders + draggable title bar */
        LRESULT def = DefWindowProcW(hwnd, msg, wp, lp);
        if (def != HTCLIENT) return def;  /* sides / bottom / corners */
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        RECT rc; GetClientRect(hwnd, &rc);
        const int RS = 6;
        if (pt.y < RS && !IsZoomed(hwnd)) {
            if (pt.x < RS) return HTTOPLEFT;
            if (pt.x >= rc.right - RS) return HTTOPRIGHT;
            return HTTOP;
        }
        if (pt.y < TITLEBAR_H) {
            int idx; TabHit h = nw ? nw_tab_hit(nw, hwnd, pt.x, pt.y, &idx) : HIT_NONE;
            if (h != HIT_NONE) return HTCLIENT;     /* tabs/+ handle the click */
            return HTCAPTION;                       /* bare bar / drag gap */
        }
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 240;
        mmi->ptMinTrackSize.y = 180;
        return 0;
    }
    case WM_SETFOCUS:
        if (nw && nw_edit(nw)) SetFocus(nw_edit(nw));   /* keep focus on the editor */
        return 0;
    case WM_LBUTTONDOWN:
        if (nw) {
            int idx = -1;
            TabHit h = nw_tab_hit(nw, hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &idx);
            if (h == HIT_PLUS) { nw_add_tab(nw, hwnd); return 0; }
            if (h == HIT_TAB)  { nw_activate(nw, hwnd, idx); return 0; }
            /* HIT_CLOSE handled in Task 8 */
        }
        return 0;
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (nw && hdr->code == EN_SELCHANGE && hdr->idFrom == ID_EDIT) {
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
        return 0;
    }
    case WM_DRAWITEM:
        if (nw && wp >= IDB_BOLD && wp <= IDB_NUM) {
            nw_draw_button(nw, (const DRAWITEMSTRUCT*)lp);
            return TRUE;
        }
        if (nw && wp >= IDW_MIN && wp <= IDW_CLOSE) {
            nw_draw_wbutton(nw, hwnd, (const DRAWITEMSTRUCT*)lp);
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
            case IDW_MIN:    ShowWindow(hwnd, SW_MINIMIZE); return 0;
            case IDW_MAX:    ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); return 0;
            case IDW_CLOSE:  SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
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
            nw_apply_format(nw);
            SendMessageW(e, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(e, NULL, TRUE);
            nw_update_toggles(nw);
        }
        return 0;
    case WM_DESTROY:
        nw_registry_remove(hwnd);
        if (nw) {
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
    if (h) nw_registry_add(nw->win_id, h);
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
            /* window stays in prefs.windows with its tab list (reopened next launch) */
        }
    }
    DestroyWindow(hwnd);
}

void note_window_activate_note(HWND hwnd, const char* note_id) {
    NoteWin* nw = nw_get(hwnd);
    if (!nw) return;
    for (int t = 0; t < nw->ntabs; t++) {
        if (strcmp(nw->tab[t].id, note_id) == 0) {
            if (t != nw->active) {
                ShowWindow(nw->tab[nw->active].edit, SW_HIDE);
                nw->active = t;
                ShowWindow(nw->tab[t].edit, SW_SHOW);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            break;
        }
    }
    SetForegroundWindow(hwnd);
    if (nw_edit(nw)) SetFocus(nw_edit(nw));
}
