#include <windows.h>

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hInst; (void)hPrev; (void)cmd; (void)show;
    MessageBoxW(NULL, L"Sticky Notes scaffold", L"StickyNotes", MB_OK);
    return 0;
}
