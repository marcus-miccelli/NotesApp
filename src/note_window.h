#ifndef NOTE_WINDOW_H
#define NOTE_WINDOW_H
#include <windows.h>
#include "app.h"

void note_window_register_class(HINSTANCE hInst);
HWND note_window_open(AppState* app, NoteMeta* meta);
void note_window_close(HWND hwnd);

#endif
