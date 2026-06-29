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
#define DEBOUNCE_MS  150
#define IDT_DEBOUNCE 1

/* Notifications we want from the RichEdit: text changes (for live-format
 * debounce) and key events (for Ctrl+N / Ctrl+Shift+D via EN_MSGFILTER). */
#define EDIT_EVENT_MASK (ENM_CHANGE | ENM_KEYEVENTS)

/* DWM dark title bar. Attribute 20 = DWMWA_USE_IMMERSIVE_DARK_MODE on
 * Windows 10 2004+ (older insider builds used 19). */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

/* Dark palette */
static const COLORREF COL_BG   = RGB(0x1e, 0x1e, 0x22);
static const COLORREF COL_TEXT = RGB(0xe6, 0xe6, 0xe6);

typedef struct {
    AppState* app;
    char id[16];               /* note id; resolve NoteMeta* fresh via nw_meta() */
    HWND edit;
} NoteWin;

static HMODULE g_richedit = NULL;
static HBRUSH  g_bg_brush = NULL;   /* class background, dark; created once */

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

/* The RichEdit fills the entire client area (the native title bar provides the
 * window chrome now). */
static void nw_layout(HWND hwnd, NoteWin* nw) {
    RECT rc; GetClientRect(hwnd, &rc);
    MoveWindow(nw->edit, 0, 0, rc.right, rc.bottom, TRUE);
}

static void nw_load_content(NoteWin* nw) {
    NoteMeta* m = nw_meta(nw);
    if (!m) return;
    char path[260];
    app_note_path(nw->app, m, path, sizeof path);
    char* txt = NULL; size_t n = 0;
    if (store_read_note(path, &txt, &n)) {
        SetWindowTextA(nw->edit, txt);   /* RichEdit accepts ANSI text */
        free(txt);
    }
}

static void nw_save_content(NoteWin* nw) {
    NoteMeta* m = nw_meta(nw);
    if (!m) return;
    int len = GetWindowTextLengthA(nw->edit);
    char* buf = malloc((size_t)len + 1);
    if (!buf) return;
    GetWindowTextA(nw->edit, buf, len + 1);
    char path[260];
    app_note_path(nw->app, m, path, sizeof path);
    store_write_note(path, buf, (size_t)len);
    free(buf);
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
}

static void nw_set_range_fmt(HWND edit, size_t start, size_t len, MdFmt fmt) {
    CHARRANGE saved; SendMessageW(edit, EM_EXGETSEL, 0, (LPARAM)&saved);
    CHARRANGE r; r.cpMin = (LONG)start; r.cpMax = (LONG)(start + len);
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&r);

    CHARFORMAT2W cf; memset(&cf, 0, sizeof cf); cf.cbSize = sizeof cf;
    /* include CFM_HIDDEN (effect bit off) so content is always shown, even if a
     * prior pass had hidden these positions. */
    cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_SIZE | CFM_FACE | CFM_COLOR | CFM_HIDDEN;
    cf.crTextColor = COL_TEXT;
    cf.yHeight = 200;                       /* 10pt default (twips) */
    if (fmt & MD_FMT_H1) cf.yHeight = 360;
    else if (fmt & MD_FMT_H2) cf.yHeight = 300;
    else if (fmt & MD_FMT_H3) cf.yHeight = 260;
    if (fmt & (MD_FMT_BOLD | MD_FMT_H1 | MD_FMT_H2 | MD_FMT_H3))
        cf.dwEffects |= CFE_BOLD;
    if (fmt & MD_FMT_ITALIC) cf.dwEffects |= CFE_ITALIC;
    if (fmt & MD_FMT_CODE) wcscpy(cf.szFaceName, L"Consolas");
    else wcscpy(cf.szFaceName, L"Segoe UI");
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);

    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&saved);  /* restore caret */
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
    base.dwMask = CFM_BOLD|CFM_ITALIC|CFM_SIZE|CFM_FACE|CFM_COLOR|CFM_HIDDEN;
    base.yHeight = 200; base.crTextColor = COL_TEXT; wcscpy(base.szFaceName, L"Segoe UI");
    SendMessageW(nw->edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&base);

    MdSpan spans[256];
    size_t n = markdown_spans(buf, (size_t)len, spans, 256);
    if (n > 256) n = 256;
    for (size_t i = 0; i < n; i++) {
        nw_set_range_fmt(nw->edit, spans[i].start, spans[i].len, spans[i].fmt);
        nw_apply_hidden(nw->edit, buf, len, &spans[i]);   /* consume markers */
    }
    free(buf);

    /* restore the real caret/selection */
    SendMessageW(nw->edit, EM_EXSETSEL, 0, (LPARAM)&saved_sel);

    /* re-enable change + key notifications now that the restyle is done */
    SendMessageW(nw->edit, EM_SETEVENTMASK, 0, EDIT_EVENT_MASK);
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

/* Handle Ctrl+N / Ctrl+Shift+D forwarded from the RichEdit via EN_MSGFILTER.
 * Returns nonzero if the keystroke was consumed (so the control ignores it). */
static LRESULT nw_handle_key(NoteWin* nw, HWND hwnd, const MSGFILTER* mf) {
    if (mf->msg != WM_KEYDOWN) return 0;
    BOOL ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    BOOL shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    if (!ctrl) return 0;
    if (mf->wParam == 'N' && !shift) { nw_new_note(nw); return 1; }
    if (mf->wParam == 'D' && shift)  { nw_delete_note(nw, hwnd); return 1; }
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
        SetFocus(nw->edit);
        return 0;
    }
    case WM_SIZE:
        if (nw) nw_layout(hwnd, nw);
        return 0;
    case WM_SETFOCUS:
        if (nw) SetFocus(nw->edit);   /* keep focus on the editor */
        return 0;
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (nw && hdr->idFrom == ID_EDIT && hdr->code == EN_MSGFILTER)
            return nw_handle_key(nw, hwnd, (MSGFILTER*)lp);
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
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == ID_EDIT) {
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
