#include <stdio.h>
#include "test.h"

int g_tests_run = 0;
int g_tests_failed = 0;

/* Each test file defines one of these. Add new ones as modules land. */
void test_smoke(void);

int main(void) {
    test_smoke();
    printf("\n%d checks, %d failed\n", g_tests_run, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
