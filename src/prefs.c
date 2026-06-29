#include "prefs.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_str(char* dst, size_t cap, const char* src, const char* fallback) {
    const char* s = src ? src : fallback;
    snprintf(dst, cap, "%s", s);
}

void prefs_init_default(Prefs* p) {
    p->version = 1;
    snprintf(p->theme, sizeof p->theme, "%s", "dark");
    p->notes = NULL; p->count = 0; p->cap = 0;
    p->windows = NULL; p->wcount = 0; p->wcap = 0;
}

void prefs_free(Prefs* p) {
    free(p->notes);   p->notes = NULL;   p->count = 0;  p->cap = 0;
    free(p->windows); p->windows = NULL; p->wcount = 0; p->wcap = 0;
}

NoteMeta* prefs_add_note(Prefs* p, const char* id, const char* file) {
    if (p->count == p->cap) {
        size_t nc = p->cap ? p->cap * 2 : 8;
        NoteMeta* n = realloc(p->notes, nc * sizeof(NoteMeta));
        if (!n) return NULL;
        p->notes = n; p->cap = nc;
    }
    NoteMeta* m = &p->notes[p->count++];
    memset(m, 0, sizeof *m);
    copy_str(m->id, sizeof m->id, id, "");
    copy_str(m->file, sizeof m->file, file, "");
    snprintf(m->color, sizeof m->color, "%s", "slate");
    return m;
}

NoteMeta* prefs_find(Prefs* p, const char* id) {
    for (size_t i = 0; i < p->count; i++)
        if (strcmp(p->notes[i].id, id) == 0) return &p->notes[i];
    return NULL;
}

bool prefs_remove(Prefs* p, const char* id) {
    for (size_t i = 0; i < p->count; i++) {
        if (strcmp(p->notes[i].id, id) == 0) {
            memmove(&p->notes[i], &p->notes[i+1],
                    (p->count - i - 1) * sizeof(NoteMeta));
            p->count--;
            return true;
        }
    }
    return false;
}

WinMeta* prefs_add_window(Prefs* p, const char* id) {
    if (p->wcount == p->wcap) {
        size_t nc = p->wcap ? p->wcap * 2 : 4;
        WinMeta* n = realloc(p->windows, nc * sizeof(WinMeta));
        if (!n) return NULL;
        p->windows = n; p->wcap = nc;
    }
    WinMeta* w = &p->windows[p->wcount++];
    memset(w, 0, sizeof *w);
    copy_str(w->id, sizeof w->id, id, "");
    w->x = 200; w->y = 200; w->w = 480; w->h = 360;   /* 4:3 default */
    w->active = 0; w->ntabs = 0;
    return w;
}

WinMeta* prefs_find_window(Prefs* p, const char* id) {
    for (size_t i = 0; i < p->wcount; i++)
        if (strcmp(p->windows[i].id, id) == 0) return &p->windows[i];
    return NULL;
}

bool prefs_remove_window(Prefs* p, const char* id) {
    for (size_t i = 0; i < p->wcount; i++) {
        if (strcmp(p->windows[i].id, id) == 0) {
            memmove(&p->windows[i], &p->windows[i+1],
                    (p->wcount - i - 1) * sizeof(WinMeta));
            p->wcount--;
            return true;
        }
    }
    return false;
}

static int json_int(const cJSON* o, const char* k, int dflt) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valueint : dflt;
}
static const char* json_str(const cJSON* o, const char* k, const char* dflt) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : dflt;
}
bool prefs_load(Prefs* p, const char* json_path) {
    prefs_init_default(p);
    FILE* f = fopen(json_path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return false; }
    size_t r = fread(buf, 1, (size_t)sz, f); buf[r] = '\0'; fclose(f);

    cJSON* root = cJSON_Parse(buf);
    if (!root) {
        /* File existed and was non-empty but is corrupt: preserve a backup
         * before defaults get saved over it on shutdown. */
        char bak[600];
        snprintf(bak, sizeof bak, "%s.bak", json_path);
        FILE* bf = fopen(bak, "wb");
        if (bf) { fwrite(buf, 1, r, bf); fclose(bf); }
        free(buf);
        return false;
    }
    free(buf);

    p->version = json_int(root, "version", 1);
    copy_str(p->theme, sizeof p->theme, json_str(root, "theme", "dark"), "dark");

    const cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "notes");
    const cJSON* it = NULL;
    cJSON_ArrayForEach(it, arr) {
        const char* id = json_str(it, "id", NULL);
        const char* file = json_str(it, "file", NULL);
        if (!id || !file) continue;
        NoteMeta* m = prefs_add_note(p, id, file);
        if (!m) break;
        copy_str(m->name, sizeof m->name, json_str(it, "name", ""), "");
        copy_str(m->color, sizeof m->color, json_str(it, "color", "slate"), "slate");
    }
    cJSON_Delete(root);
    return true;
}

bool prefs_save(const Prefs* p, const char* json_path) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", p->version);
    cJSON_AddStringToObject(root, "theme", p->theme);
    cJSON* arr = cJSON_AddArrayToObject(root, "notes");
    for (size_t i = 0; i < p->count; i++) {
        const NoteMeta* m = &p->notes[i];
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", m->id);
        cJSON_AddStringToObject(o, "file", m->file);
        cJSON_AddStringToObject(o, "name", m->name);
        cJSON_AddStringToObject(o, "color", m->color);
        cJSON_AddItemToArray(arr, o);
    }
    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) return false;
    FILE* f = fopen(json_path, "wb");
    if (!f) { free(text); return false; }
    fputs(text, f);
    fclose(f);
    free(text);
    return true;
}
