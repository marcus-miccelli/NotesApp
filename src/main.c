#include <windows.h>
#include "app.h"
#include "note_window.h"

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev; (void)cmd; (void)show;
    AppState app;
    if (!app_init(&app, NULL)) return 1;
    note_window_register_class(hInst);
    NoteMeta* m = app.prefs.count ? &app.prefs.notes[0] : app_new_note(&app);
    note_window_open(&app, m);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    app_shutdown(&app);
    return 0;
}
