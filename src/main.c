#include <windows.h>
#include "app.h"
#include "note_window.h"
#include "tray.h"

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev; (void)cmd; (void)show;
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
     * it before the multiline body could insert a newline. Shortcuts are
     * handled via the RichEdit EN_MSGFILTER, not accelerators. */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    tray_shutdown();
    app_shutdown(&app);   /* saves preferences.json */
    return 0;
}
