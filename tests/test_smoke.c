#include "test.h"

void test_smoke(void) {
    CHECK(1 + 1 == 2);
    CHECK_STR("ab", "ab");
}
