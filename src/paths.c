#include "paths.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    snprintf(out->base,  sizeof out->base,  "%s\\quickNote", root);
    snprintf(out->notes, sizeof out->notes, "%s\\notes", out->base);
    snprintf(out->prefs, sizeof out->prefs, "%s\\preferences.json", out->base);
    return true;
}

static bool ensure_dir(const char* path) {
    return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* mkdir -p: create every ancestor of path, then path itself. Needed so a
 * nested base (e.g. a test root under C:\tmp\... whose parent doesn't exist)
 * is created in full; CreateDirectoryA alone won't make intermediate dirs. */
static bool ensure_dir_p(const char* path) {
    char tmp[260];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char* q = tmp + 1; *q; ++q) {
        if (*q != '\\' && *q != '/') continue;
        char sep = *q; *q = '\0';
        /* skip a bare drive root like "C:" */
        if (!(strlen(tmp) == 2 && tmp[1] == ':') && !ensure_dir(tmp)) {
            *q = sep; return false;
        }
        *q = sep;
    }
    return ensure_dir(tmp);
}

bool paths_ensure_dirs(const Paths* p) {
    return ensure_dir_p(p->base) && ensure_dir_p(p->notes);
}

void paths_note_file(const Paths* p, const char* id, char* out, size_t outsz) {
    snprintf(out, outsz, "%s\\%s.md", p->notes, id);
}
