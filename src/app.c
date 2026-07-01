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

/* Write the current preferences (open windows + their tabs + notes) to disk now.
 * Called on every session change so a cold start always resumes the last state,
 * regardless of how the app exited (clean Quit, crash, or kill). */
void app_persist(AppState* a) {
    prefs_save(&a->prefs, a->paths.prefs);
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

/* Next "Untitled N": one past the highest N already in use among note names. */
static int app_next_untitled(const AppState* a) {
    int max = 0;
    for (size_t i = 0; i < a->prefs.count; i++) {
        int n = 0;
        if (sscanf(a->prefs.notes[i].name, "Untitled %d", &n) == 1 && n > max)
            max = n;
    }
    return max + 1;
}

NoteMeta* app_new_note(AppState* a) {
    char id[16], file[24];
    app_gen_id(id);
    snprintf(file, sizeof file, "%s.md", id);
    NoteMeta* m = prefs_add_note(&a->prefs, id, file);
    if (!m) return NULL;
    snprintf(m->name, sizeof m->name, "Untitled %d", app_next_untitled(a));
    char path[260];
    app_note_path(a, m, path, sizeof path);
    if (!store_write_note(path, "", 0)) {
        prefs_remove(&a->prefs, id);
        return NULL;
    }
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

static void app_gen_win_id(AppState* a, char* out16) {
    /* "wXXXXXXX" hex, distinct from note ids and other window ids */
    for (;;) {
        char id[16]; id[0] = 'w';
        static const char hex[] = "0123456789abcdef";
        for (int i = 1; i < 8; i++) id[i] = hex[rand() & 0xF];
        id[8] = '\0';
        if (!prefs_find_window(&a->prefs, id)) { strcpy(out16, id); return; }
    }
}

WinMeta* app_new_window(AppState* a) {
    NoteMeta* m = app_new_note(a);
    if (!m) return NULL;
    char nid[16]; strcpy(nid, m->id);   /* copy: app_new_note may realloc notes */
    char wid[16]; app_gen_win_id(a, wid);
    WinMeta* w = prefs_add_window(&a->prefs, wid);
    if (!w) { app_delete_note(a, nid); return NULL; }
    w->ntabs = 1; w->active = 0;
    strcpy(w->tabs[0], nid);
    return w;
}

WinMeta* app_open_note_in_window(AppState* a, const char* note_id) {
    if (!prefs_find(&a->prefs, note_id)) return NULL;
    char wid[16]; app_gen_win_id(a, wid);
    WinMeta* w = prefs_add_window(&a->prefs, wid);
    if (!w) return NULL;
    w->ntabs = 1; w->active = 0;
    snprintf(w->tabs[0], sizeof w->tabs[0], "%s", note_id);
    return w;
}

WinMeta* app_window_of_note(AppState* a, const char* note_id) {
    for (size_t i = 0; i < a->prefs.wcount; i++) {
        WinMeta* w = &a->prefs.windows[i];
        for (int t = 0; t < w->ntabs; t++)
            if (strcmp(w->tabs[t], note_id) == 0) return w;
    }
    return NULL;
}
