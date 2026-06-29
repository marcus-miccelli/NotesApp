#include "note_window.h"
#include "store.h"
#include "markdown.h"
#include <richedit.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOTE_CLASS   L"StickyNoteWindow"
#define ID_EDIT      1001
#define ID_TITLE     1002
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
#define SIDEBAR_W    30    /* sidebar width (buttons are ~this wide, square) */
#define TITLE_H      30    /* title box height */
#define BTN_GAP      3     /* between sidebar buttons */

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

typedef struct {
    AppState* app;
    char id[16];               /* note id; resolve NoteMeta* fresh via nw_meta() */
    HWND title;                /* single-line H1 title box; content = note name */
    HWND edit;                 /* multi-line markdown body */
    HWND btns[NUM_BTNS];       /* formatting sidebar buttons */
    int  active[NUM_BTNS];     /* which formats apply at the caret (highlight) */
} NoteWin;

static HMODULE g_richedit = NULL;
static HBRUSH  g_bg_brush = NULL;     /* class background, dark; created once */
static HBRUSH  g_border_brush = NULL; /* region outline; created once */

static NoteWin* nw_get(HWND h) { return (NoteWin*)GetWindowLongPtrW(h, GWLP_USERDATA); }

/* Resolve the current NoteMeta* by id at point of use. The prefs.notes[] array
 * can be realloc'd (add) or memmove'd (remove), so a cached NoteMeta* would
 * dangle. Always resolve fresh and NULL-check (the note may have been deleted). */
static NoteMeta* nw_meta(NoteWin* nw) { return prefs_find(&nw->app->prefs, nw->id); }

/* ---- best-effort open-window registry, keyed by note id ----
 * Lets the tray dedupe reopen requests. If full, the new entry just isn't
 * tracked (treated as "not open"); correctness still holds. */
#define NW_REGISTRY_CAP 64
typedef struct { char id[16]; HWND hwnd; } NwRegEntry;
static NwRegEntry s_registry[NW_REGISTRY_CAP];

static void nw_registry_add(const char* id, HWND hwnd) {
    for (int i = 0; i < NW_REGISTRY_CAP; i++) {
        if (s_registry[i].hwnd == NULL) {
            snprintf(s_registry[i].id, sizeof s_registry[i].id, "%s", id);
            s_registry[i].hwnd = hwnd;
            return;
        }
    }
    /* full: leave untracked (best-effort) */
}

static void nw_registry_remove(HWND hwnd) {
    for (int i = 0; i < NW_REGISTRY_CAP; i++) {
        if (s_registry[i].hwnd == hwnd) {
            s_registry[i].hwnd = NULL;
            s_registry[i].id[0] = '\0';
            return;
        }
    }
}

HWND note_window_find_open(const char* id) {
    for (int i = 0; i < NW_REGISTRY_CAP; i++) {
        if (s_registry[i].hwnd != NULL && strcmp(s_registry[i].id, id) == 0)
            return s_registry[i].hwnd;
    }
    return NULL;
}

/* Layout geometry. Two dividers cross the whole window: a vertical line at
 * x=SIDEBAR_W and a full-width horizontal line at y=PAD_OUTER+TITLE_H. They make
 * four quadrants — top-left an empty bordered square, top-right the title,
 * bottom-left the button sidebar, bottom-right the body. vx/hy are the divider
 * lines; title/body are control rects. Any out param may be NULL. */
static void nw_regions(HWND hwnd, RECT* title, RECT* body, int* vx, int* hy) {
    RECT rc; GetClientRect(hwnd, &rc);
    int v  = SIDEBAR_W;                              /* vertical divider x */
    int h  = PAD_OUTER + TITLE_H;                    /* horizontal divider y */
    int cl = v + REGION_GAP;                         /* content left */
    int cr = rc.right - PAD_OUTER; if (cr < cl) cr = cl;
    int b_top = h + REGION_GAP, b_bot = rc.bottom - PAD_OUTER;
    if (b_bot < b_top) b_bot = b_top;
    if (title) SetRect(title, cl, PAD_OUTER, cr, h - 3);
    if (body)  SetRect(body,  cl, b_top, cr, b_bot);
    if (vx) *vx = v;
    if (hy) *hy = h;
}

/* Buttons are squares the full sidebar width (up to the vertical divider),
 * stacked from just below the horizontal divider. Title and body fill the
 * content column to the right of the vertical divider. */
static void nw_layout(HWND hwnd, NoteWin* nw) {
    RECT tt, bd; int vx, hy;
    nw_regions(hwnd, &tt, &bd, &vx, &hy);

    int side = vx;                                   /* square == sidebar width */
    for (int i = 0; i < NUM_BTNS; i++)
        MoveWindow(nw->btns[i], 0, hy + 1 + i * (side + BTN_GAP),
                   side, side, TRUE);

    MoveWindow(nw->title, tt.left, tt.top, tt.right - tt.left, tt.bottom - tt.top, TRUE);
    MoveWindow(nw->edit,  bd.left, bd.top, bd.right - bd.left, bd.bottom - bd.top, TRUE);
}

/* Owner-draw a sidebar button: blends with the dark sidebar (no distinct fill)
 * until it is pressed or its format is active at the caret, when the square
 * fills with the toggle accent. The glyph is rendered in the style it applies
 * (bold B, italic I, struck-through S). */
static void nw_draw_button(NoteWin* nw, const DRAWITEMSTRUCT* d) {
    int idx = (int)d->CtlID - IDB_BOLD;
    int on = (d->itemState & ODS_SELECTED) != 0 ||
             (idx >= 0 && idx < NUM_BTNS && nw->active[idx]);
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
    CHARRANGE sel; SendMessageW(nw->edit, EM_EXGETSEL, 0, (LPARAM)&sel);
    int len; char* buf = nw_body_text(nw->edit, &len);
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
        if (nw->active[i]) { nw->active[i] = 0; InvalidateRect(nw->btns[i], NULL, TRUE); }
}

/* Recompute which formats apply at the body caret/selection and repaint any
 * button whose highlight changed. Inline styles come from the character format,
 * lists from the paragraph numbering. Headings highlight nothing. */
static void nw_update_toggles(NoteWin* nw) {
    if (nw_caret_on_heading(nw)) { nw_clear_toggles(nw); return; }
    int s[NUM_BTNS] = { 0 };
    CHARFORMAT2W cf; memset(&cf, 0, sizeof cf); cf.cbSize = sizeof cf;
    cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT;
    SendMessageW(nw->edit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    s[0] = (cf.dwMask & CFM_BOLD)     && (cf.dwEffects & CFE_BOLD);
    s[1] = (cf.dwMask & CFM_ITALIC)   && (cf.dwEffects & CFE_ITALIC);
    s[2] = (cf.dwMask & CFM_STRIKEOUT)&& (cf.dwEffects & CFE_STRIKEOUT);

    PARAFORMAT2 pf; memset(&pf, 0, sizeof pf); pf.cbSize = sizeof pf;
    pf.dwMask = PFM_NUMBERING;
    SendMessageW(nw->edit, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
    s[3] = (pf.wNumbering == PFN_BULLET);
    s[4] = (pf.wNumbering == PFN_ARABIC);

    for (int i = 0; i < NUM_BTNS; i++) {
        if (s[i] != nw->active[i]) {
            nw->active[i] = s[i];
            InvalidateRect(nw->btns[i], NULL, TRUE);
        }
    }
}

/* Style the whole title box (and its default for new input) as a dark H1. */
static void nw_style_title(HWND title) {
    CHARRANGE all = { 0, -1 };
    SendMessageW(title, EM_EXSETSEL, 0, (LPARAM)&all);
    CHARFORMAT2W cf; memset(&cf, 0, sizeof cf); cf.cbSize = sizeof cf;
    cf.dwMask = CFM_BOLD | CFM_SIZE | CFM_FACE | CFM_COLOR;
    cf.dwEffects = CFE_BOLD;
    cf.yHeight = 360;                 /* ~18pt H1 */
    cf.crTextColor = COL_TEXT;
    wcscpy(cf.szFaceName, L"Segoe UI");
    SendMessageW(title, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    CHARRANGE caret = { 0, 0 };
    SendMessageW(title, EM_EXSETSEL, 0, (LPARAM)&caret);
}

/* The .md is "# <title>\n\n<body>": the first line's H1 is the note title, the
 * rest is the body. Split on load so each goes to its own control. If there is
 * no leading H1 (legacy/empty file), the whole file is body and the title falls
 * back to the persisted note name. */
static void nw_load_content(NoteWin* nw) {
    NoteMeta* m = nw_meta(nw);
    if (!m) return;
    char path[260];
    app_note_path(nw->app, m, path, sizeof path);
    char* txt = NULL; size_t n = 0;
    char* title = NULL;
    const char* body = "";
    if (store_read_note(path, &txt, &n)) {
        body = txt;
        if (n >= 2 && txt[0] == '#' && txt[1] == ' ') {
            size_t i = 2;
            while (i < n && txt[i] != '\n' && txt[i] != '\r') i++;
            size_t tlen = i - 2;
            title = malloc(tlen + 1);
            if (title) { memcpy(title, txt + 2, tlen); title[tlen] = '\0'; }
            if (i < n && txt[i] == '\r') i++;
            if (i < n && txt[i] == '\n') i++;
            /* skip a single blank separator line after the title */
            size_t j = i;
            if (j < n && txt[j] == '\r') j++;
            if (j < n && txt[j] == '\n') i = j + 1;
            body = txt + i;
        }
    }
    const char* tshow = (title && title[0]) ? title : m->name;
    SetWindowTextA(nw->title, tshow ? tshow : "");
    nw_style_title(nw->title);
    SetWindowTextA(nw->edit, body);
    snprintf(m->name, sizeof m->name, "%s", tshow ? tshow : "");
    SetWindowTextA(GetParent(nw->title), m->name[0] ? m->name : "Sticky Note");
    free(title);
    free(txt);
}

static void nw_save_content(NoteWin* nw) {
    NoteMeta* m = nw_meta(nw);
    if (!m) return;
    int tl = GetWindowTextLengthA(nw->title);
    int bl = GetWindowTextLengthA(nw->edit);
    char* t = malloc((size_t)tl + 1); if (!t) return;
    char* b = malloc((size_t)bl + 1); if (!b) { free(t); return; }
    GetWindowTextA(nw->title, t, tl + 1);
    GetWindowTextA(nw->edit, b, bl + 1);
    /* title is single-line; neutralize any stray break just in case */
    for (char* p = t; *p; ++p) if (*p == '\r' || *p == '\n') *p = ' ';

    size_t need = (tl > 0 ? (size_t)tl + 4 : 0) + (size_t)bl + 1;  /* "# " + t + "\n\n" + body */
    char* buf = malloc(need);
    if (!buf) { free(t); free(b); return; }
    size_t off = 0;
    if (tl > 0) off += (size_t)sprintf(buf + off, "# %s\n\n", t);
    memcpy(buf + off, b, (size_t)bl); off += (size_t)bl;

    char path[260];
    app_note_path(nw->app, m, path, sizeof path);
    store_write_note(path, buf, off);

    snprintf(m->name, sizeof m->name, "%s", t);
    SetWindowTextA(GetParent(nw->title), m->name[0] ? m->name : "Sticky Note");
    free(buf); free(b); free(t);
}

/* Persist window geometry, but only when the window is in its normal state.
 * A minimized window's GetWindowRect is (-32000,-32000); a maximized one is
 * the whole work area. Saving either would lose the user's real placement, so
 * skip and keep the last good values. */
static void nw_save_geometry(HWND hwnd, NoteMeta* m) {
    if (!m || IsIconic(hwnd) || IsZoomed(hwnd)) return;
    RECT wr; GetWindowRect(hwnd, &wr);
    m->x = wr.left; m->y = wr.top;
    m->w = wr.right - wr.left; m->h = wr.bottom - wr.top;
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
 * are correct for EM_EXSETSEL.  nw_save_content is unaffected — it still uses
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

static void nw_apply_format(NoteWin* nw) {
    /* Applying CHARFORMAT makes RichEdit emit EN_CHANGE/EN_UPDATE. Without
     * suppressing them, each reformat re-arms the debounce timer and we loop
     * forever (constant reflow = flicker + the text never settles formatted).
     * Mute notifications for the duration of the programmatic restyle. */
    SendMessageW(nw->edit, EM_SETEVENTMASK, 0, 0);

    /* Use EM_GETTEXTEX with GT_DEFAULT + CP_ACP: returns \r-only line endings
     * whose byte offsets match RichEdit's internal character positions exactly. */
    GETTEXTLENGTHEX gtl; gtl.flags = GTL_DEFAULT; gtl.codepage = CP_ACP;
    int len = (int)SendMessageW(nw->edit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
    if (len <= 0) { SendMessageW(nw->edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK); return; }
    char* buf = malloc((size_t)len + 1);
    if (!buf) { SendMessageW(nw->edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK); return; }
    GETTEXTEX gte; gte.cb = (DWORD)(len + 1); gte.flags = GT_DEFAULT;
    gte.codepage = CP_ACP; gte.lpDefaultChar = NULL; gte.lpUsedDefChar = NULL;
    SendMessageW(nw->edit, EM_GETTEXTEX, (WPARAM)&gte, (LPARAM)buf);

    /* capture real caret BEFORE any selection changes */
    CHARRANGE saved_sel;
    SendMessageW(nw->edit, EM_EXGETSEL, 0, (LPARAM)&saved_sel);

    /* reset everything to default first */
    CHARRANGE all = { 0, -1 };
    SendMessageW(nw->edit, EM_EXSETSEL, 0, (LPARAM)&all);
    CHARFORMAT2W base; memset(&base, 0, sizeof base); base.cbSize = sizeof base;
    /* CFM_HIDDEN (effect off) un-hides everything so deleted/moved markers
     * reappear before we re-hide the current ones. */
    base.dwMask = CFM_BOLD|CFM_ITALIC|CFM_STRIKEOUT|CFM_SIZE|CFM_FACE|CFM_COLOR|CFM_HIDDEN;
    base.yHeight = 200; base.crTextColor = COL_TEXT; wcscpy(base.szFaceName, L"Segoe UI");
    SendMessageW(nw->edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&base);

    MdSpan spans[256];
    size_t n = markdown_spans(buf, (size_t)len, spans, 256);
    if (n > 256) n = 256;
    for (size_t i = 0; i < n; i++) {
        nw_set_range_fmt(nw->edit, spans[i].start, spans[i].len, spans[i].fmt);
        nw_apply_hidden(nw->edit, buf, len, &spans[i]);   /* consume markers */
    }
    nw_apply_lists(nw->edit, buf, len);     /* render "- "/"N. " as bullets/numbers */
    free(buf);

    /* restore the real caret/selection */
    SendMessageW(nw->edit, EM_EXSETSEL, 0, (LPARAM)&saved_sel);

    /* re-enable change + key notifications now that the restyle is done */
    SendMessageW(nw->edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
}

/* Persist + restyle the body now (used after a programmatic edit), with redraw
 * suppressed to avoid flicker. */
static void nw_reformat_now(NoteWin* nw) {
    SendMessageW(nw->edit, WM_SETREDRAW, FALSE, 0);
    nw_save_content(nw);
    nw_apply_format(nw);
    SendMessageW(nw->edit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(nw->edit, NULL, TRUE);
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

/* Toggle inline emphasis around the body selection by inserting/removing a
 * markdown marker ("**", "*", "~~"). If the selection is already wrapped in the
 * marker it is unwrapped; otherwise it is wrapped (with no selection, the caret
 * lands between the markers). The live formatter then renders + hides them. */
static void nw_wrap_inline(NoteWin* nw, const char* marker) {
    if (nw_caret_on_heading(nw)) return;     /* headings are hard H1 */
    HWND e = nw->edit;
    int mlen = (int)strlen(marker);
    wchar_t wmark[8];
    for (int k = 0; k < mlen; k++) wmark[k] = (wchar_t)marker[k];
    wmark[mlen] = 0;

    SendMessageW(e, EM_SETEVENTMASK, 0, 0);   /* mute; we reformat explicitly */
    CHARRANGE sel; SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
    LONG a = sel.cpMin, b = sel.cpMax;
    int len; char* buf = nw_body_text(e, &len);
    if (!buf) { SendMessageW(e, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK); return; }
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
    HWND e = nw->edit;
    SendMessageW(e, EM_SETEVENTMASK, 0, 0);
    CHARRANGE sel; SendMessageW(e, EM_EXGETSEL, 0, (LPARAM)&sel);
    int len; char* buf = nw_body_text(e, &len);
    if (!buf) { SendMessageW(e, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK); return; }
    LONG a = sel.cpMin, b = sel.cpMax;

    LONG ls = a;                                  /* start of line containing a */
    while (ls > 0 && buf[ls-1] != '\r' && buf[ls-1] != '\n') ls--;

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
    HWND e = nw->edit;
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

/* Spawn a new note offset from this one. */
static void nw_new_note(NoteWin* nw) {
    int ox = 224, oy = 224;
    NoteMeta* cur = nw_meta(nw);     /* read coords BEFORE app_new_note (it may realloc) */
    if (cur) { ox = cur->x + 24; oy = cur->y + 24; }
    NoteMeta* m = app_new_note(nw->app);
    if (m) { m->x = ox; m->y = oy; note_window_open(nw->app, m); }
}

/* Delete this note (with confirmation), removing its .md and index entry. */
static void nw_delete_note(NoteWin* nw, HWND hwnd) {
    if (MessageBoxW(hwnd, L"Delete this note permanently?",
                    L"Delete", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    /* Capture before DestroyWindow: WM_DESTROY frees nw synchronously. */
    char id[16]; strcpy(id, nw->id);
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
    if (mf->wParam == 'N' && !shift) { nw_new_note(nw); return 1; }
    if (mf->wParam == 'D' && shift)  { nw_delete_note(nw, hwnd); return 1; }

    if (mf->nmhdr.idFrom == ID_EDIT) {
        if (mf->wParam == 'B' && !shift) { nw_wrap_inline(nw, "**"); return 1; }
        if (mf->wParam == 'I' && !shift) { nw_wrap_inline(nw, "*");  return 1; }
        if (mf->wParam == 'X' && shift)  { nw_wrap_inline(nw, "~~"); return 1; }
        if (mf->wParam == '8' && shift)  { nw_list_prefix(nw, 0);    return 1; }
        if (mf->wParam == '7' && shift)  { nw_list_prefix(nw, 1);    return 1; }
    }
    return 0;
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

        /* Title: single-line H1 box (no ES_MULTILINE). */
        nw->title = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 100, TITLE_H, hwnd, (HMENU)ID_TITLE, cs->hInstance, NULL);
        SendMessageW(nw->title, EM_SETBKGNDCOLOR, 0, (LPARAM)COL_BG);
        SendMessageW(nw->title, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
        nw_style_title(nw->title);

        nw->edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
            0, 0, 100, 100, hwnd, (HMENU)ID_EDIT, cs->hInstance, NULL);
        /* dark background + text color for RichEdit */
        SendMessageW(nw->edit, EM_SETBKGNDCOLOR, 0, (LPARAM)COL_BG);
        CHARFORMAT2W cf; memset(&cf, 0, sizeof cf);
        cf.cbSize = sizeof cf; cf.dwMask = CFM_COLOR; cf.crTextColor = COL_TEXT;
        SendMessageW(nw->edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
        SendMessageW(nw->edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
        nw_load_content(nw);
        nw_layout(hwnd, nw);
        nw_update_toggles(nw);
        SetFocus(nw->edit);
        return 0;
    }
    case WM_SIZE:
        if (nw) { nw_layout(hwnd, nw); InvalidateRect(hwnd, NULL, TRUE); }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int vx, hy; nw_regions(hwnd, NULL, NULL, &vx, &hy);
        RECT vline = { vx, 0, vx + 1, rc.bottom };          /* sidebar | content */
        RECT hline = { 0, hy, rc.right, hy + 1 };           /* full-width divider */
        FillRect(hdc, &vline, g_border_brush);
        FillRect(hdc, &hline, g_border_brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETFOCUS:
        if (nw) SetFocus(nw->edit);   /* keep focus on the editor */
        return 0;
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (nw && hdr->code == EN_SELCHANGE && hdr->idFrom == ID_EDIT) {
            nw_update_toggles(nw);
            return 0;
        }
        if (nw && hdr->code == EN_SELCHANGE && hdr->idFrom == ID_TITLE) {
            nw_clear_toggles(nw);   /* title is hard H1; nothing to highlight */
            return 0;
        }
        if (nw && hdr->code == EN_MSGFILTER &&
            (hdr->idFrom == ID_EDIT || hdr->idFrom == ID_TITLE)) {
            MSGFILTER* mf = (MSGFILTER*)lp;
            /* Enter in the title moves to the body instead of being eaten. */
            if (hdr->idFrom == ID_TITLE && mf->msg == WM_KEYDOWN &&
                mf->wParam == VK_RETURN) {
                SetFocus(nw->edit);
                return 1;
            }
            /* Enter in the body continues a list item, if on one. */
            if (hdr->idFrom == ID_EDIT && mf->msg == WM_KEYDOWN &&
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
        NoteMeta* m = nw_meta(nw);
        if (!m) return 0;
        nw_save_geometry(hwnd, m);
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
            case IDB_BOLD:   nw_wrap_inline(nw, "**"); break;
            case IDB_ITALIC: nw_wrap_inline(nw, "*");  break;
            case IDB_STRIKE: nw_wrap_inline(nw, "~~"); break;
            case IDB_BULLET: nw_list_prefix(nw, 0);    break;
            case IDB_NUM:    nw_list_prefix(nw, 1);    break;
            default: return 0;
            }
            SetFocus(nw->edit);   /* return focus to the body after a click */
            return 0;
        }
        if (HIWORD(wp) == EN_CHANGE &&
            (LOWORD(wp) == ID_EDIT || LOWORD(wp) == ID_TITLE)) {
            SetTimer(hwnd, IDT_DEBOUNCE, DEBOUNCE_MS, NULL);  /* coalesces */
        }
        return 0;
    case WM_TIMER:
        if (wp == IDT_DEBOUNCE && nw) {
            KillTimer(hwnd, IDT_DEBOUNCE);
            SendMessageW(nw->edit, WM_SETREDRAW, FALSE, 0);
            nw_save_content(nw);
            nw_apply_format(nw);
            SendMessageW(nw->edit, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(nw->edit, NULL, TRUE);
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
    wc.lpszClassName = NOTE_CLASS;
    RegisterClassExW(&wc);
}

HWND note_window_open(AppState* app, NoteMeta* meta) {
    NoteWin* nw = calloc(1, sizeof *nw);
    if (!nw) return NULL;
    nw->app = app;
    snprintf(nw->id, sizeof nw->id, "%s", meta->id);
    meta->open = true;
    /* Standard captioned, resizable window: full-height title bar with
     * minimize/maximize/close buttons and a taskbar button. */
    HWND h = CreateWindowExW(0, NOTE_CLASS, L"Sticky Note",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        meta->x, meta->y, meta->w, meta->h,
        NULL, NULL, GetModuleHandleW(NULL), nw);
    if (h) nw_registry_add(nw->id, h);
    else   free(nw);
    return h;
}

void note_window_close(HWND hwnd) {
    NoteWin* nw = nw_get(hwnd);
    if (nw) {
        NoteMeta* m = nw_meta(nw);
        if (m) {
            nw_save_geometry(hwnd, m);
            nw_save_content(nw);
            m->open = false;
        }
    }
    DestroyWindow(hwnd);
}
