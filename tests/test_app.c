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
    CHECK(strlen(m->id) == 8);

    char path[260];
    CHECK(app_note_path(&a, m, path, sizeof path));

    /* file exists and is readable */
    char* txt = NULL; size_t n = 0;
    CHECK(store_read_note(path, &txt, &n));
    free(txt);

    char saved[16]; strcpy(saved, m->id);
    CHECK(app_delete_note(&a, saved));
    CHECK(a.prefs.count == 0);

    /* window helpers */
    WinMeta* w = app_new_window(&a);
    CHECK(w != NULL);
    CHECK(w->ntabs == 1);
    CHECK(strlen(w->tabs[0]) == 8);             /* a fresh note id */
    CHECK(app_window_of_note(&a, w->tabs[0]) == w);

    NoteMeta* extra = app_new_note(&a);
    CHECK(extra != NULL);
    char extra_id[16]; strcpy(extra_id, extra->id);
    CHECK(app_window_of_note(&a, extra_id) == NULL);   /* not in any window */
    WinMeta* w2 = app_open_note_in_window(&a, extra_id);
    CHECK(w2 != NULL && w2 != w);
    CHECK(w2->ntabs == 1);
    CHECK_STR(w2->tabs[0], extra_id);
    CHECK(app_open_note_in_window(&a, "nosuch") == NULL);

    app_shutdown(&a);
}
