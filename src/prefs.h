#ifndef PREFS_H
#define PREFS_H
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char id[16];
    char file[24];
    char name[64];     /* note title; shown in tab + tray; default "Untitled N" */
    char color[16];
} NoteMeta;

#define WIN_MAX_TABS 32
typedef struct {
    char id[16];                    /* stable window id */
    int  x, y, w, h;
    int  active;                    /* index into tabs[] */
    char tabs[WIN_MAX_TABS][16];    /* ordered note ids */
    int  ntabs;
} WinMeta;

typedef struct {
    int version;
    char theme[16];
    NoteMeta* notes;   size_t count,  cap;
    WinMeta*  windows; size_t wcount, wcap;
} Prefs;

void      prefs_init_default(Prefs* p);
bool      prefs_load(Prefs* p, const char* json_path);
bool      prefs_save(const Prefs* p, const char* json_path);
NoteMeta* prefs_add_note(Prefs* p, const char* id, const char* file);
NoteMeta* prefs_find(Prefs* p, const char* id);
bool      prefs_remove(Prefs* p, const char* id);
void      prefs_free(Prefs* p);

WinMeta* prefs_add_window(Prefs* p, const char* id);
WinMeta* prefs_find_window(Prefs* p, const char* id);
bool     prefs_remove_window(Prefs* p, const char* id);

#endif
