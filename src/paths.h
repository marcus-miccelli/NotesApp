#ifndef PATHS_H
#define PATHS_H
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char base[260];
    char notes[260];
    char prefs[260];
} Paths;

bool paths_resolve(Paths* out, const char* root_override);
bool paths_ensure_dirs(const Paths* p);
void paths_note_file(const Paths* p, const char* id, char* out, size_t outsz);

#endif
