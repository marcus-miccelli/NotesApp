#include "note_window.h"
#include "store.h"
#include "markdown.h"
#include <windowsx.h>
#include <richedit.h>
#include <stdlib.h>
#include <string.h>

#define NOTE_CLASS L"StickyNoteWindow"
#define ID_EDIT     1001
#define TITLEBAR_H  26
#define BTN_W       22
#define DEBOUNCE_MS 150
#define IDT_DEBOUNCE 1

/* Dark palette */
static const COLORREF COL_BG     = RGB(0x1e, 0x1e, 0x22);
static const COLORREF COL_TEXT   = RGB(0xe6, 0xe6, 0xe6);
static const COLORREF COL_TITLE  = RGB(0x2b, 0x3b, 0x55); /* "slate" accent */

typedef struct {
    AppState* app;
    NoteMeta* meta;
    HWND edit;
    HBRUSH bg_brush;
    HBRUSH title_brush;
} NoteWin;

static HMODULE g_richedit = NULL;

static NoteWin* nw_get(HWND h) { return (NoteWin*)GetWindowLongPtrW(h, GWLP_USERDATA); }

static void nw_layout(HWND hwnd, NoteWin* nw) {
    RECT rc; GetClientRect(hwnd, &rc);
    MoveWindow(nw->edit, 0, TITLEBAR_H, rc.right, rc.bottom - TITLEBAR_H, TRUE);
}

static void nw_load_content(NoteWin* nw) {
    char path[260];
    app_note_path(nw->app, nw->meta, path, sizeof path);
    char* txt = NULL; size_t n = 0;
    if (store_read_note(path, &txt, &n)) {
        SetWindowTextA(nw->edit, txt);   /* RichEdit accepts ANSI text */
        free(txt);
    }
}

static void nw_save_content(NoteWin* nw) {
    int len = GetWindowTextLengthA(nw->edit);
    char* buf = malloc((size_t)len + 1);
    if (!buf) return;
    GetWindowTextA(nw->edit, buf, len + 1);
    char path[260];
    app_note_path(nw->app, nw->meta, path, sizeof path);
    store_write_note(path, buf, (size_t)len);
    free(buf);
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
static void nw_set_range_fmt(HWND edit, size_t start, size_t len, MdFmt fmt) {
    CHARRANGE saved; SendMessageW(edit, EM_EXGETSEL, 0, (LPARAM)&saved);
    CHARRANGE r; r.cpMin = (LONG)start; r.cpMax = (LONG)(start + len);
    SendMessageW(edit, EM_EXSETSEL, 0, (LPARAM)&r);

    CHARFORMAT2W cf; memset(&cf, 0, sizeof cf); cf.cbSize = sizeof cf;
    cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_SIZE | CFM_FACE | CFM_COLOR;
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
    /* Use EM_GETTEXTEX with GT_DEFAULT + CP_ACP: returns \r-only line endings
     * whose byte offsets match RichEdit's internal character positions exactly. */
    GETTEXTLENGTHEX gtl; gtl.flags = GTL_DEFAULT; gtl.codepage = CP_ACP;
    int len = (int)SendMessageW(nw->edit, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
    if (len <= 0) return;
    char* buf = malloc((size_t)len + 1);
    if (!buf) return;
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
    base.dwMask = CFM_BOLD|CFM_ITALIC|CFM_SIZE|CFM_FACE|CFM_COLOR;
    base.yHeight = 200; base.crTextColor = COL_TEXT; wcscpy(base.szFaceName, L"Segoe UI");
    SendMessageW(nw->edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&base);

    MdSpan spans[256];
    size_t n = markdown_spans(buf, (size_t)len, spans, 256);
    if (n > 256) n = 256;
    for (size_t i = 0; i < n; i++)
        nw_set_range_fmt(nw->edit, spans[i].start, spans[i].len, spans[i].fmt);
    free(buf);

    /* restore the real caret/selection */
    SendMessageW(nw->edit, EM_EXSETSEL, 0, (LPARAM)&saved_sel);
}

static LRESULT CALLBACK nw_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    NoteWin* nw = nw_get(hwnd);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        nw = (NoteWin*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)nw);
        nw->bg_brush = CreateSolidBrush(COL_BG);
        nw->title_brush = CreateSolidBrush(COL_TITLE);
        nw->edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
            0, TITLEBAR_H, 100, 100, hwnd, (HMENU)ID_EDIT,
            cs->hInstance, NULL);
        /* dark background + text color for RichEdit */
        SendMessageW(nw->edit, EM_SETBKGNDCOLOR, 0, (LPARAM)COL_BG);
        CHARFORMAT2W cf; memset(&cf, 0, sizeof cf);
        cf.cbSize = sizeof cf; cf.dwMask = CFM_COLOR; cf.crTextColor = COL_TEXT;
        SendMessageW(nw->edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
        SendMessageW(nw->edit, EM_SETEVENTMASK, 0, ENM_CHANGE);
        nw_load_content(nw);
        nw_layout(hwnd, nw);
        return 0;
    }
    case WM_SIZE:
        if (nw) nw_layout(hwnd, nw);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
        if (!nw) { EndPaint(hwnd, &ps); return 0; }
        RECT rc; GetClientRect(hwnd, &rc);
        RECT title = { 0, 0, rc.right, TITLEBAR_H };
        FillRect(dc, &title, nw->title_brush);
        /* draw + and x buttons */
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, COL_TEXT);
        RECT bplus = { rc.right - 2*BTN_W, 0, rc.right - BTN_W, TITLEBAR_H };
        RECT bclose = { rc.right - BTN_W, 0, rc.right, TITLEBAR_H };
        DrawTextW(dc, L"+", 1, &bplus, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        DrawTextW(dc, L"x", 1, &bclose, DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc; GetClientRect(hwnd, &rc);
        if (pt.y < TITLEBAR_H) {
            if (pt.x >= rc.right - BTN_W) {            /* x: close (or delete w/ shift) */
                if (GetKeyState(VK_SHIFT) & 0x8000) {
                    if (MessageBoxW(hwnd, L"Delete this note permanently?",
                                    L"Delete", MB_YESNO|MB_ICONWARNING) == IDYES) {
                        char id[16]; strcpy(id, nw->meta->id);
                        DestroyWindow(hwnd);
                        app_delete_note(nw->app, id);
                        return 0;
                    }
                } else {
                    note_window_close(hwnd);
                    return 0;
                }
            } else if (pt.x >= rc.right - 2*BTN_W) {   /* + : new note */
                NoteMeta* m = app_new_note(nw->app);
                if (m) { m->x = nw->meta->x + 24; m->y = nw->meta->y + 24;
                         note_window_open(nw->app, m); }
                return 0;
            }
            /* else: drag the window via the title bar */
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
        break;
    }
    case WM_EXITSIZEMOVE: {
        if (!nw) return 0;
        RECT wr; GetWindowRect(hwnd, &wr);
        nw->meta->x = wr.left; nw->meta->y = wr.top;
        nw->meta->w = wr.right - wr.left; nw->meta->h = wr.bottom - wr.top;
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == ID_EDIT) {
            SetTimer(hwnd, IDT_DEBOUNCE, DEBOUNCE_MS, NULL);  /* coalesces */
        }
        return 0;
    case WM_TIMER:
        if (wp == IDT_DEBOUNCE) {
            KillTimer(hwnd, IDT_DEBOUNCE);
            SendMessageW(nw->edit, WM_SETREDRAW, FALSE, 0);
            nw_save_content(nw);
            nw_apply_format(nw);
            SendMessageW(nw->edit, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(nw->edit, NULL, TRUE);
        }
        return 0;
    case WM_DESTROY:
        if (nw) {
            DeleteObject(nw->bg_brush);
            DeleteObject(nw->title_brush);
            free(nw);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void note_window_register_class(HINSTANCE hInst) {
    if (!g_richedit) g_richedit = LoadLibraryW(L"Msftedit.dll");
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = nw_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); /* IDC_ARROW */
    wc.lpszClassName = NOTE_CLASS;
    RegisterClassExW(&wc);
}

HWND note_window_open(AppState* app, NoteMeta* meta) {
    NoteWin* nw = calloc(1, sizeof *nw);
    nw->app = app; nw->meta = meta;
    meta->open = true;
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, NOTE_CLASS, L"Note",
        WS_POPUP | WS_THICKFRAME | WS_VISIBLE,
        meta->x, meta->y, meta->w, meta->h,
        NULL, NULL, GetModuleHandleW(NULL), nw);
    return h;
}

void note_window_close(HWND hwnd) {
    NoteWin* nw = nw_get(hwnd);
    if (nw) {
        RECT wr; GetWindowRect(hwnd, &wr);
        nw->meta->x = wr.left; nw->meta->y = wr.top;
        nw->meta->w = wr.right - wr.left; nw->meta->h = wr.bottom - wr.top;
        nw_save_content(nw);
        nw->meta->open = false;
    }
    DestroyWindow(hwnd);
}
