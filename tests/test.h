#ifndef TEST_H
#define TEST_H
#include <stdio.h>
#include <string.h>

extern int g_tests_run;
extern int g_tests_failed;

#define CHECK(cond) do {                                          \
    g_tests_run++;                                                \
    if (!(cond)) { g_tests_failed++;                              \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); }  \
} while (0)

#define CHECK_STR(a,b) do {                                       \
    g_tests_run++;                                                \
    if (strcmp((a),(b)) != 0) { g_tests_failed++;                 \
        printf("FAIL %s:%d: \"%s\" != \"%s\"\n",                  \
               __FILE__, __LINE__, (a), (b)); }                   \
} while (0)

#endif
