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

    /* --- markdown_decorate: inline --- */
    {   /* "**bold**": hide [0,2], fmt [2,4] BOLD, hide [6,2] */
        const char* t = "**bold**";
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        CHECK(n == 3);
        CHECK(d[0].kind == DECO_HIDE && d[0].start == 0 && d[0].len == 2);
        CHECK(d[1].kind == DECO_FMT  && d[1].start == 2 && d[1].len == 4 &&
              d[1].fmt == MD_FMT_BOLD);
        CHECK(d[2].kind == DECO_HIDE && d[2].start == 6 && d[2].len == 2);
        free(d);
    }
    {   /* "*i*": hide[0,1] fmt[1,1] hide[2,1] */
        const char* t = "*i*";
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        CHECK(n == 3);
        CHECK(d[1].kind == DECO_FMT && d[1].fmt == MD_FMT_ITALIC &&
              d[1].start == 1 && d[1].len == 1);
        free(d);
    }
    {   /* "~~x~~": strike, markers len 2 */
        const char* t = "~~x~~";
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        CHECK(n == 3);
        CHECK(d[0].kind == DECO_HIDE && d[0].len == 2);
        CHECK(d[1].fmt == MD_FMT_STRIKE && d[1].start == 2 && d[1].len == 1);
        CHECK(d[2].kind == DECO_HIDE && d[2].start == 3 && d[2].len == 2);
        free(d);
    }
    {   /* "`c`": code, backtick markers len 1 */
        const char* t = "`c`";
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        CHECK(n == 3);
        CHECK(d[0].kind == DECO_HIDE && d[0].start == 0 && d[0].len == 1);
        CHECK(d[1].fmt == MD_FMT_CODE && d[1].start == 1 && d[1].len == 1);
        CHECK(d[2].kind == DECO_HIDE && d[2].start == 2 && d[2].len == 1);
        free(d);
    }
    {   /* nested "**a *b* c**": a BOLD run, inner ITALIC over b (bold+italic) */
        const char* t = "**a *b* c**";
        Deco* d = NULL; size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d);
        /* opening ** hidden at 0..2; inner * markers around b hidden; the "b"
         * run carries BOLD|ITALIC; closing ** hidden at end. */
        int saw_bi = 0;
        for (size_t i = 0; i < n; i++)
            if (d[i].kind == DECO_FMT &&
                d[i].fmt == (MD_FMT_BOLD | MD_FMT_ITALIC)) saw_bi = 1;
        CHECK(saw_bi == 1);
        CHECK(d[0].kind == DECO_HIDE && d[0].start == 0 && d[0].len == 2);
        free(d);
    }
}
