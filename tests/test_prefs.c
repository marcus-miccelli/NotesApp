#include <string.h>
#include "test.h"
#include "prefs.h"

void test_prefs(void) {
    Prefs p;
    prefs_init_default(&p);
    CHECK(p.version == 1);
    CHECK_STR(p.theme, "dark");
    CHECK(p.count == 0);

    NoteMeta* m = prefs_add_note(&p, "a1b2c3", "a1b2c3.md");
    CHECK(m != NULL);
    CHECK(p.count == 1);
    m->x = 10; m->y = 20; m->w = 300; m->h = 400;
    m->open = false; strcpy(m->color, "amber");

    const char* path = "C:\\tmp\\sntest_prefs.json";
    CHECK(prefs_save(&p, path));
    prefs_free(&p);

    Prefs q;
    CHECK(prefs_load(&q, path));
    CHECK(q.version == 1);
    CHECK(q.count == 1);
    NoteMeta* r = prefs_find(&q, "a1b2c3");
    CHECK(r != NULL);
    CHECK_STR(r->file, "a1b2c3.md");
    CHECK(r->x == 10 && r->y == 20 && r->w == 300 && r->h == 400);
    CHECK(r->open == false);
    CHECK_STR(r->color, "amber");

    CHECK(prefs_remove(&q, "a1b2c3"));
    CHECK(q.count == 0);
    CHECK(prefs_find(&q, "a1b2c3") == NULL);
    prefs_free(&q);

    /* missing file -> false but usable default */
    Prefs z;
    CHECK(!prefs_load(&z, "C:\\tmp\\sntest_does_not_exist.json"));
    CHECK(z.version == 1 && z.count == 0);
    prefs_free(&z);
}
