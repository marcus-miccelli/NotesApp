#ifndef APP_H
#define APP_H
#include <stdbool.h>
#include <stddef.h>
#include "paths.h"
#include "prefs.h"

typedef struct {
    Paths paths;
    Prefs prefs;
} AppState;

bool      app_init(AppState* a, const char* root_override);
void      app_shutdown(AppState* a);
void      app_gen_id(char* out16);
NoteMeta* app_new_note(AppState* a);
bool      app_delete_note(AppState* a, const char* id);
bool      app_note_path(const AppState* a, const NoteMeta* m, char* out, size_t outsz);
WinMeta*  app_new_window(AppState* a);
WinMeta*  app_open_note_in_window(AppState* a, const char* note_id);
WinMeta*  app_window_of_note(AppState* a, const char* note_id);

#endif
