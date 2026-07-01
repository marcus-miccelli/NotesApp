#ifndef TRAY_H
#define TRAY_H
#include <windows.h>
#include "app.h"

/* Single-instance plumbing shared with main.c: a later launch finds the first
 * instance's message-only owner window (by this class) and posts the registered
 * "open a new window" message (registered by this name) to it. */
#define TRAY_OWNER_CLASS   L"StickyNotesOwner"
#define TRAY_NEWWIN_MSG    L"quickNoteNewWindow"

bool tray_init(AppState* app, HINSTANCE hInst);
void tray_shutdown(void);

#endif
