#include "tray.h"
#include "note_window.h"
#include <shellapi.h>
#include <string.h>
#include <wchar.h>

#define WM_TRAY     (WM_APP + 1)
#define TRAY_UID    1
#define IDM_NEW     2000
#define IDM_QUIT    2001
#define IDM_NOTE0   3000   /* note i -> IDM_NOTE0 + i */

static HWND s_owner = NULL;
static AppState* s_app = NULL;
static NOTIFYICONDATAW s_nid;

static void tray_show_menu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_NEW, L"New Note");
    if (s_app->prefs.count > 0) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    for (size_t i = 0; i < s_app->prefs.count; i++) {
        wchar_t label[96];
        const NoteMeta* nm = &s_app->prefs.notes[i];
        const char* disp = nm->name[0] ? nm->name : nm->id;
        swprintf(label, 96, L"%hs%s", disp, nm->open ? L"  (open)" : L"");
        AppendMenuW(m, MF_STRING, IDM_NOTE0 + (UINT)i, label);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"Quit");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
}

static LRESULT CALLBACK owner_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP)
            tray_show_menu(h);
        return 0;
    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        if (id == IDM_NEW) {
            NoteMeta* m = app_new_note(s_app);
            if (m) note_window_open(s_app, m);
        } else if (id == IDM_QUIT) {
            PostQuitMessage(0);
        } else if (id >= IDM_NOTE0) {
            size_t i = id - IDM_NOTE0;
            if (i < s_app->prefs.count) {
                NoteMeta* nm = &s_app->prefs.notes[i];
                HWND existing = note_window_find_open(nm->id);
                if (existing) SetForegroundWindow(existing);
                else note_window_open(s_app, nm);
            }
        }
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool tray_init(AppState* app, HINSTANCE hInst) {
    s_app = app;
    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.lpfnWndProc = owner_proc;
    wc.hInstance = hInst; wc.lpszClassName = L"StickyNotesOwner";
    RegisterClassExW(&wc);
    s_owner = CreateWindowExW(0, L"StickyNotesOwner", L"", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);
    if (!s_owner) return false;

    memset(&s_nid, 0, sizeof s_nid);
    s_nid.cbSize = sizeof s_nid;
    s_nid.hWnd = s_owner;
    s_nid.uID = TRAY_UID;
    s_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    s_nid.uCallbackMessage = WM_TRAY;
    s_nid.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512)); /* IDI_APPLICATION */
    wcscpy(s_nid.szTip, L"Sticky Notes");
    if (!Shell_NotifyIconW(NIM_ADD, &s_nid)) {
        DestroyWindow(s_owner);
        s_owner = NULL;
        return false;
    }
    return true;
}

void tray_shutdown(void) {
    Shell_NotifyIconW(NIM_DELETE, &s_nid);
    if (s_owner) DestroyWindow(s_owner);
    s_owner = NULL;
}
