#include <windows.h>
#include "app.h"
#include "note_window.h"
#include "tray.h"

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev; (void)cmd; (void)show;
    AppState app;
    if (!app_init(&app, NULL)) return 1;
    note_window_register_class(hInst);

    /* open every note marked open; if none, create one */
    int opened = 0;
    for (size_t i = 0; i < app.prefs.count; i++) {
        if (app.prefs.notes[i].open) { note_window_open(&app, &app.prefs.notes[i]); opened++; }
    }
    if (opened == 0) {
        NoteMeta* m = app_new_note(&app);
        if (m) note_window_open(&app, m);
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
