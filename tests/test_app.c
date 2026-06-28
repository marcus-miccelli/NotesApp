#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "app.h"
#include "store.h"

void test_app(void) {
    AppState a;
    CHECK(app_init(&a, "C:\\tmp\\sntest_app"));
    CHECK(a.prefs.count == 0);

    char id1[16], id2[16];
    app_gen_id(id1); app_gen_id(id2);
    CHECK(strlen(id1) == 8);
    CHECK(strcmp(id1, id2) != 0);   /* ids differ */

    NoteMeta* m = app_new_note(&a);
    CHECK(m != NULL);
    CHECK(a.prefs.count == 1);
    CHECK(m->open == true);

    char path[260];
    CHECK(app_note_path(&a, m, path, sizeof path));

    /* file exists and is readable */
    char* txt = NULL; size_t n = 0;
    CHECK(store_read_note(path, &txt, &n));
    free(txt);

    char saved[16]; strcpy(saved, m->id);
    CHECK(app_delete_note(&a, saved));
    CHECK(a.prefs.count == 0);

    app_shutdown(&a);
}
