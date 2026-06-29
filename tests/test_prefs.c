#include <stdio.h>
#include <string.h>
#include "test.h"
#include "prefs.h"

void test_prefs(void) {
    Prefs p;
    prefs_init_default(&p);
    CHECK(p.version == 1);
    CHECK_STR(p.theme, "dark");
    CHECK(p.count == 0);
    CHECK(p.wcount == 0);

    NoteMeta* m = prefs_add_note(&p, "a1b2c3", "a1b2c3.md");
    CHECK(m != NULL);
    CHECK(p.count == 1);
    strcpy(m->color, "amber");
    strcpy(m->name, "Hello");

    /* window CRUD (in-memory; reload round-trip is verified in Task 2) */
    WinMeta* w = prefs_add_window(&p, "w0001");
    CHECK(w != NULL);
    CHECK(p.wcount == 1);
    CHECK(prefs_find_window(&p, "w0001") == w);
    CHECK(prefs_find_window(&p, "nope") == NULL);
    CHECK(prefs_remove_window(&p, "w0001"));
    CHECK(p.wcount == 0);
    /* re-add so the saved file carries a window for the Task 2 round-trip */
    w = prefs_add_window(&p, "w0001");
    CHECK(w != NULL);
    w->x = 10; w->y = 20; w->w = 300; w->h = 400;
    w->ntabs = 1; strcpy(w->tabs[0], "a1b2c3"); w->active = 0;

    const char* path = "C:\\tmp\\sntest_prefs.json";
    CHECK(prefs_save(&p, path));
    prefs_free(&p);

    Prefs q;
    CHECK(prefs_load(&q, path));
    CHECK(q.count == 1);
    NoteMeta* r = prefs_find(&q, "a1b2c3");
    CHECK(r != NULL);
    CHECK_STR(r->file, "a1b2c3.md");
    CHECK_STR(r->color, "amber");
    CHECK_STR(r->name, "Hello");

    /* NOTE: window-reload asserts are added in Task 2 (persistence lands there) */
    CHECK(prefs_remove(&q, "a1b2c3"));
    CHECK(q.count == 0);
    prefs_free(&q);

    /* missing file -> false but usable default */
    Prefs z;
    CHECK(!prefs_load(&z, "C:\\tmp\\sntest_does_not_exist.json"));
    CHECK(z.version == 1 && z.count == 0 && z.wcount == 0);
    prefs_free(&z);

    /* corrupt file -> false + defaults + a .bak backup with the original bytes */
    const char* corrupt_path = "C:\\tmp\\sntest_prefs_corrupt.json";
    const char* bak_path = "C:\\tmp\\sntest_prefs_corrupt.json.bak";
    const char* corrupt_bytes = "{ this is : not json";
    remove(bak_path);
    FILE* cf = fopen(corrupt_path, "wb");
    CHECK(cf != NULL);
    if (cf) { fputs(corrupt_bytes, cf); fclose(cf); }

    Prefs c;
    CHECK(!prefs_load(&c, corrupt_path));
    CHECK(c.version == 1 && c.count == 0);
    prefs_free(&c);

    FILE* bf = fopen(bak_path, "rb");
    CHECK(bf != NULL);
    if (bf) {
        char rbuf[64]; size_t rn = fread(rbuf, 1, sizeof rbuf - 1, bf);
        rbuf[rn] = '\0'; fclose(bf);
        CHECK_STR(rbuf, corrupt_bytes);
    }
    remove(corrupt_path);
    remove(bak_path);
}
