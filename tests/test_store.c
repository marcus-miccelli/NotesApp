#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "store.h"

void test_store(void) {
    const char* path = "C:\\tmp\\sntest_note.md";
    const char* body = "# Hello\n\n- a\n- b\n";

    CHECK(store_write_note(path, body, strlen(body)));

    char* got = NULL; size_t n = 0;
    CHECK(store_read_note(path, &got, &n));
    CHECK(n == strlen(body));
    CHECK_STR(got, body);
    free(got);

    CHECK(store_delete_note(path));
    CHECK(store_delete_note(path));            /* absent -> still true */
    char* g2 = NULL; size_t n2 = 0;
    CHECK(!store_read_note(path, &g2, &n2));    /* missing -> false */
}
