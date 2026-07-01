#include <windows.h>
#include "app.h"
#include "note_window.h"
#include "tray.h"

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev; (void)cmd; (void)show;

    /* Single instance. The first launch owns this named mutex and runs the app;
     * a later launch finds the first instance's message-only owner window and
     * posts the registered "new window" message to it, so a second click opens
     * another window in the SAME process (shared tray + shared preferences.json)
     * rather than spawning a duplicate process that would fight over the file. */
    HANDLE inst_mutex = CreateMutexW(NULL, TRUE, L"quickNote_SingleInstance_Mutex");
    if (inst_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND owner = FindWindowExW(HWND_MESSAGE, NULL, TRAY_OWNER_CLASS, NULL);
        if (owner) PostMessageW(owner, RegisterWindowMessageW(TRAY_NEWWIN_MSG), 0, 0);
        if (inst_mutex) CloseHandle(inst_mutex);
        return 0;                      /* hand off to the running instance and exit */
    }

    AppState app;
    if (!app_init(&app, NULL)) return 1;
    note_window_register_class(hInst);

    /* open every saved window; on first run create one window + note */
    if (app.prefs.wcount == 0) {
        WinMeta* w = app_new_window(&app);     /* first run: one window, one note */
        if (w) note_window_open(&app, w);
    } else {
        for (size_t i = 0; i < app.prefs.wcount; i++)
            note_window_open(&app, &app.prefs.windows[i]);
    }

    if (!tray_init(&app, hInst)) return 1;

    /* Plain dispatch — no IsDialogMessage. The note window is not a dialog;
     * IsDialogMessage would treat Enter as a default-button press and swallow
     * it before the multiline body could insert a newline. Most shortcuts are
     * handled via the RichEdit EN_MSGFILTER, not accelerators.
     *
     * Alt+N (new window) is the exception: it is a system key (WM_SYSKEYDOWN)
     * that RichEdit does not surface through EN_MSGFILTER, so intercept it here
     * before dispatch. GetMessage delivers WM_SYSKEYDOWN whenever one of this
     * app's windows has focus. Consuming it also avoids the menu-mode beep. */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_SYSKEYDOWN && msg.wParam == 'N' &&
            (GetKeyState(VK_MENU) & 0x8000)) {
            WinMeta* w = app_new_window(&app);
            if (w) note_window_open(&app, w);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    note_window_save_all();   /* flush every open window's geometry + tab bodies */
    tray_shutdown();
    app_shutdown(&app);   /* saves preferences.json */
    return 0;
}
