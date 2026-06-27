#include "app.h"
#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned g_seeded = 0;

bool app_init(AppState* a, const char* root_override) {
    if (!paths_resolve(&a->paths, root_override)) return false;
    if (!paths_ensure_dirs(&a->paths)) return false;
    prefs_load(&a->prefs, a->paths.prefs);   /* tolerant: false ok */
    return true;
}

void app_shutdown(AppState* a) {
    prefs_save(&a->prefs, a->paths.prefs);
    prefs_free(&a->prefs);
}

void app_gen_id(char* out16) {
    if (!g_seeded) { srand((unsigned)time(NULL) ^ (unsigned)clock()); g_seeded = 1; }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) out16[i] = hex[rand() & 0xF];
    out16[8] = '\0';
}

bool app_note_path(const AppState* a, const NoteMeta* m, char* out, size_t outsz) {
    snprintf(out, outsz, "%s\\%s", a->paths.notes, m->file);
    return true;
}

NoteMeta* app_new_note(AppState* a) {
    char id[16], file[24];
    app_gen_id(id);
    snprintf(file, sizeof file, "%s.md", id);
    NoteMeta* m = prefs_add_note(&a->prefs, id, file);
    if (!m) return NULL;
    char path[260];
    app_note_path(a, m, path, sizeof path);
    if (!store_write_note(path, "", 0)) {  /* create empty file */
        prefs_remove(&a->prefs, id);       /* roll back the just-added entry */
        return NULL;
    }
    m->open = true;
    return m;
}

bool app_delete_note(AppState* a, const char* id) {
    NoteMeta* m = prefs_find(&a->prefs, id);
    if (!m) return false;
    char path[260];
    app_note_path(a, m, path, sizeof path);
    store_delete_note(path);
    return prefs_remove(&a->prefs, id);
}
