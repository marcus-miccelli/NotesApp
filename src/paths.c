#include "paths.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <shlobj.h>

bool paths_resolve(Paths* out, const char* root_override) {
    char root[260];
    if (root_override) {
        snprintf(root, sizeof root, "%s", root_override);
    } else {
        const char* appdata = getenv("APPDATA");
        if (!appdata) return false;
        snprintf(root, sizeof root, "%s", appdata);
    }
    snprintf(out->base,  sizeof out->base,  "%s\\StickyNotes", root);
    snprintf(out->notes, sizeof out->notes, "%s\\notes", out->base);
    snprintf(out->prefs, sizeof out->prefs, "%s\\preferences.json", out->base);
    return true;
}

bool paths_ensure_dirs(const Paths* p) {
    if (!CreateDirectoryA(p->base, NULL)
        && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    if (!CreateDirectoryA(p->notes, NULL)
        && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    return true;
}

void paths_note_file(const Paths* p, const char* id, char* out, size_t outsz) {
    snprintf(out, outsz, "%s\\%s.md", p->notes, id);
}
