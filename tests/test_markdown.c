#include <string.h>
#include "test.h"
#include "markdown.h"

void test_markdown(void) {
    MdSpan s[32];

    /* "**bold**" -> one BOLD span over "bold" at offset 2, len 4 */
    {
        const char* t = "**bold**";
        size_t n = markdown_spans(t, strlen(t), s, 32);
        CHECK(n == 1);
        CHECK(s[0].fmt == MD_FMT_BOLD);
        CHECK(s[0].start == 2);
        CHECK(s[0].len == 4);
    }

    /* "# Hi" -> one H1 span over "Hi" at offset 2, len 2 */
    {
        const char* t = "# Hi";
        size_t n = markdown_spans(t, strlen(t), s, 32);
        CHECK(n == 1);
        CHECK(s[0].fmt == MD_FMT_H1);
        CHECK(s[0].start == 2);
        CHECK(s[0].len == 2);
    }

    /* "`x`" -> one CODE span over "x" at offset 1, len 1 */
    {
        const char* t = "`x`";
        size_t n = markdown_spans(t, strlen(t), s, 32);
        CHECK(n == 1);
        CHECK(s[0].fmt == MD_FMT_CODE);
        CHECK(s[0].start == 1);
        CHECK(s[0].len == 1);
    }

    /* plain text -> no spans */
    {
        const char* t = "just words";
        size_t n = markdown_spans(t, strlen(t), s, 32);
        CHECK(n == 0);
    }
}
