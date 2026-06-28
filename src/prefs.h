#ifndef PREFS_H
#define PREFS_H
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char id[16];
    char file[24];
    int x, y, w, h;
    char color[16];
    bool open;
} NoteMeta;

typedef struct {
    int version;
    char theme[16];
    NoteMeta* notes;
    size_t count;
    size_t cap;
} Prefs;

void      prefs_init_default(Prefs* p);
bool      prefs_load(Prefs* p, const char* json_path);
bool      prefs_save(const Prefs* p, const char* json_path);
NoteMeta* prefs_add_note(Prefs* p, const char* id, const char* file);
NoteMeta* prefs_find(Prefs* p, const char* id);
bool      prefs_remove(Prefs* p, const char* id);
void      prefs_free(Prefs* p);

#endif
