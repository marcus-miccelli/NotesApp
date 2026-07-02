#include <string.h>
#include <stdlib.h>
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

    /* "~~gone~~" -> one STRIKE span over "gone" at offset 2, len 4 */
    {
        const char* t = "~~gone~~";
        size_t n = markdown_spans(t, strlen(t), s, 32);
        CHECK(n == 1);
        CHECK(s[0].fmt == MD_FMT_STRIKE);
        CHECK(s[0].start == 2);
        CHECK(s[0].len == 4);
    }

    /* plain text -> no spans */
    {
        const char* t = "just words";
        size_t n = markdown_spans(t, strlen(t), s, 32);
        CHECK(n == 0);
    }

    /* --- markdown_decorate: headings --- */
    {
        const char* t = "# Hi";
        Deco* d = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        CHECK(n == 2);
        CHECK(d[0].kind == DECO_HIDE && d[0].start == 0 && d[0].len == 2);  /* "# " */
        CHECK(d[1].kind == DECO_FMT && d[1].start == 2 && d[1].len == 2 &&
              d[1].fmt == MD_FMT_H1);
        free(d);
    }
    {
        const char* t = "plain words";
        Deco* d = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        CHECK(n == 0);
        free(d);
    }
}
