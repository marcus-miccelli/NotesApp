#ifndef NOTE_WINDOW_H
#define NOTE_WINDOW_H
#include <windows.h>
#include "app.h"

void note_window_register_class(HINSTANCE hInst);
HWND note_window_open(AppState* app, WinMeta* win);
HWND note_window_find_open_window(const char* win_id);
HWND note_window_find_by_note(const char* note_id);
void note_window_activate_note(HWND hwnd, const char* note_id);
void note_window_close(HWND hwnd);
void note_window_save_all(void);

#endif
