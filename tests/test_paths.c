#include <stdio.h>
#include <string.h>
#include "test.h"
#include "paths.h"

void test_paths(void) {
    Paths p;
    CHECK(paths_resolve(&p, "C:\\tmp\\sntest"));
    CHECK_STR(p.base,  "C:\\tmp\\sntest\\quickNote");
    CHECK_STR(p.notes, "C:\\tmp\\sntest\\quickNote\\notes");
    CHECK_STR(p.prefs, "C:\\tmp\\sntest\\quickNote\\preferences.json");

    char f[260];
    paths_note_file(&p, "a1b2c3", f, sizeof f);
    CHECK_STR(f, "C:\\tmp\\sntest\\quickNote\\notes\\a1b2c3.md");

    CHECK(paths_ensure_dirs(&p));   /* idempotent: safe to call again */
    CHECK(paths_ensure_dirs(&p));
}
