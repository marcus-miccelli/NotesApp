#ifndef TRAY_H
#define TRAY_H
#include <windows.h>
#include "app.h"

bool tray_init(AppState* app, HINSTANCE hInst);
void tray_shutdown(void);

#endif
