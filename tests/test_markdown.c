#include <string.h>
#include <stdlib.h>
#include "test.h"
#include "markdown.h"

void test_markdown(void) {
    /* --- markdown_decorate: headings --- */
    {
        const char* t = "# Hi";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        CHECK(n == 2);
        CHECK(d[0].kind == DECO_HIDE && d[0].start == 0 && d[0].len == 2);  /* "# " */
        CHECK(d[1].kind == DECO_FMT && d[1].start == 2 && d[1].len == 2 &&
              d[1].fmt == MD_FMT_H1);
        free(d); free(pool);
    }
    {
        const char* t = "plain words";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        CHECK(n == 0);
        free(d); free(pool);
    }

    /* --- markdown_decorate: inline --- */
    {   /* "**bold**": fmt [2,4] BOLD, hide [0,2], hide [6,2].
         * Span-level reveal (see feature/span-reveal) defers inline-marker
         * hides to d_leave_span, where the span's extent is known, so they
         * now land AFTER the span's DECO_FMT in the list (was: hide, fmt,
         * hide) — this reorders the array but not the set of decos. */
        const char* t = "**bold**";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        CHECK(n == 3);
        CHECK(d[0].kind == DECO_FMT  && d[0].start == 2 && d[0].len == 4 &&
              d[0].fmt == MD_FMT_BOLD);
        CHECK(d[1].kind == DECO_HIDE && d[1].start == 0 && d[1].len == 2);
        CHECK(d[2].kind == DECO_HIDE && d[2].start == 6 && d[2].len == 2);
        free(d); free(pool);
    }
    {   /* "*i*": fmt[1,1] hide[0,1] hide[2,1] (see reorder note above) */
        const char* t = "*i*";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        CHECK(n == 3);
        CHECK(d[0].kind == DECO_FMT && d[0].fmt == MD_FMT_ITALIC &&
              d[0].start == 1 && d[0].len == 1);
        free(d); free(pool);
    }
    {   /* "~~x~~": strike, markers len 2 (see reorder note above) */
        const char* t = "~~x~~";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        CHECK(n == 3);
        CHECK(d[0].kind == DECO_FMT && d[0].fmt == MD_FMT_STRIKE &&
              d[0].start == 2 && d[0].len == 1);
        CHECK(d[1].kind == DECO_HIDE && d[1].len == 2);
        CHECK(d[2].kind == DECO_HIDE && d[2].start == 3 && d[2].len == 2);
        free(d); free(pool);
    }
    {   /* "`c`": code, backtick markers len 1 (see reorder note above) */
        const char* t = "`c`";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        CHECK(n == 3);
        CHECK(d[0].kind == DECO_FMT && d[0].fmt == MD_FMT_CODE &&
              d[0].start == 1 && d[0].len == 1);
        CHECK(d[1].kind == DECO_HIDE && d[1].start == 0 && d[1].len == 1);
        CHECK(d[2].kind == DECO_HIDE && d[2].start == 2 && d[2].len == 1);
        free(d); free(pool);
    }
    {   /* nested "**a *b* c**": a BOLD run, inner ITALIC over b (bold+italic) */
        const char* t = "**a *b* c**";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        /* opening ** hidden at 0..2; inner * markers around b hidden; the "b"
         * run carries BOLD|ITALIC; closing ** hidden at end. Order-independent
         * (see reorder note above: inline hides now land at leave_span). */
        int saw_bi = 0, saw_open_hide = 0;
        for (size_t i = 0; i < n; i++) {
            if (d[i].kind == DECO_FMT &&
                d[i].fmt == (MD_FMT_BOLD | MD_FMT_ITALIC)) saw_bi = 1;
            if (d[i].kind == DECO_HIDE && d[i].start == 0 && d[i].len == 2)
                saw_open_hide = 1;
        }
        CHECK(saw_bi == 1);
        CHECK(saw_open_hide == 1);
        free(d); free(pool);
    }

    /* --- markdown_decorate: lists --- */
    {   /* bullet list: "- a\n- b" */
        const char* t = "- a\n- b";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int bullets = 0, hides = 0;
        for (size_t i = 0; i < n; i++) {
            if (d[i].kind == DECO_PARA && d[i].para == PARA_BULLET) bullets++;
            if (d[i].kind == DECO_HIDE && d[i].len == 2) hides++;  /* "- " */
        }
        CHECK(bullets == 2);
        CHECK(hides >= 2);
        free(d); free(pool);
    }
    {   /* numbered list: "1. a\n2. b" -> ordinals 1 and 2 */
        const char* t = "1. a\n2. b";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int n1 = 0, n2 = 0;
        for (size_t i = 0; i < n; i++)
            if (d[i].kind == DECO_PARA && d[i].para == PARA_NUMBER) {
                if (d[i].number == 1) n1 = 1;
                if (d[i].number == 2) n2 = 1;
            }
        CHECK(n1 == 1 && n2 == 1);
        free(d); free(pool);
    }

    /* --- markdown_decorate: cursor-aware reveal --- */
    {   /* two paragraphs; caret in the first reveals its markers only */
        const char* t = "**a**\n**b**";   /* para1 [0,5], \n at 5, para2 [6,11] */
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), 1, 1, &d, &pool);  /* caret in para1 */
        int hide_in_p1 = 0, hide_in_p2 = 0;
        for (size_t i = 0; i < n; i++) if (d[i].kind == DECO_HIDE) {
            if (d[i].start < 5) hide_in_p1 = 1;
            if (d[i].start > 5) hide_in_p2 = 1;
        }
        CHECK(hide_in_p1 == 0);   /* revealed */
        CHECK(hide_in_p2 == 1);   /* still hidden */
        free(d); free(pool);
    }
    {   /* hide-all sentinel still hides both */
        const char* t = "**a**\n**b**";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int hides = 0;
        for (size_t i = 0; i < n; i++) if (d[i].kind == DECO_HIDE) hides++;
        CHECK(hides == 4);
        free(d); free(pool);
    }

    /* --- markdown_fmt_at --- */
    {
        const char* t = "**bold**";
        CHECK(markdown_fmt_at(t, strlen(t), 4) == MD_FMT_BOLD);   /* inside */
        CHECK(markdown_fmt_at(t, strlen(t), 0) == 0);             /* on marker */
    }
    {
        const char* t = "a *i* b";
        CHECK(markdown_fmt_at(t, strlen(t), 3) == MD_FMT_ITALIC); /* inside i */
        CHECK(markdown_fmt_at(t, strlen(t), 0) == 0);             /* plain */
    }

    /* --- fenced code block --- */
    {
        const char* t = "```\ncode\n```";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int codefmt = 0, hides = 0;
        for (size_t i = 0; i < n; i++) {
            if (d[i].kind == DECO_FMT && (d[i].fmt & MD_FMT_CODEBLOCK)) codefmt = 1;
            if (d[i].kind == DECO_HIDE) hides++;
        }
        CHECK(codefmt == 1);   /* "code" styled as code block */
        CHECK(hides >= 2);     /* opening + closing fences hidden */
        free(d); free(pool);
    }

    /* --- multi-line code block: shaded range is contiguous (covers the \n) --- */
    {
        const char* t = "```\na\nb\n```";   /* a at 4, \n at 5, b at 6 */
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int contiguous = 0;
        for (size_t i = 0; i < n; i++)
            if (d[i].kind == DECO_FMT && (d[i].fmt & MD_FMT_CODEBLOCK) &&
                d[i].start <= 4 && d[i].start + d[i].len >= 7)  /* covers a, \n, b */
                contiguous = 1;
        CHECK(contiguous == 1);
        free(d); free(pool);
    }

    /* --- task list --- */
    {
        const char* t = "- [ ] todo\n- [x] done";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int bullets = 0, struck = 0, checkbox_visible = 1;
        for (size_t i = 0; i < n; i++) {
            if (d[i].kind == DECO_PARA && d[i].para == PARA_BULLET) bullets++;
            if (d[i].kind == DECO_FMT && (d[i].fmt & MD_FMT_STRIKE)) struck = 1;
            /* a hide covering the first-line checkbox "[ ]" (offsets 2..5) would
             * mean the checkbox was hidden — it must NOT be */
            if (d[i].kind == DECO_HIDE && d[i].start <= 2 && d[i].start + d[i].len > 3)
                checkbox_visible = 0;
        }
        CHECK(bullets == 2);        /* both items are bullets */
        CHECK(struck == 1);         /* the checked item strikes its content */
        CHECK(checkbox_visible == 1); /* "[ ]" stays visible */
        int todo_struck = 0;
        for (size_t i = 0; i < n; i++)
            if (d[i].kind == DECO_FMT && (d[i].fmt & MD_FMT_STRIKE) &&
                d[i].start <= 6 && d[i].start + d[i].len > 6)
                todo_struck = 1;
        CHECK(todo_struck == 0);   /* unchecked item is NOT struck */
        free(d); free(pool);
    }

    /* --- blockquote --- */
    {
        const char* t = "> quoted";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int qfmt = 0, qpara = 0, hidmark = 0;
        for (size_t i = 0; i < n; i++) {
            if (d[i].kind == DECO_FMT && (d[i].fmt & MD_FMT_QUOTE)) qfmt = 1;
            if (d[i].kind == DECO_PARA && d[i].para == PARA_QUOTE) qpara = 1;
            if (d[i].kind == DECO_HIDE && d[i].start == 0 && d[i].len == 2) hidmark = 1;
        }
        CHECK(qfmt == 1);
        CHECK(qpara == 1);
        CHECK(hidmark == 1);   /* "> " hidden */
        free(d); free(pool);
    }

    /* --- multi-line blockquote: BOTH "> " markers hidden --- */
    {
        const char* t = "> a\n> b";   /* line1 "> " at 0, line2 "> " at 4 */
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int hid1 = 0, hid2 = 0;
        for (size_t i = 0; i < n; i++) if (d[i].kind == DECO_HIDE) {
            if (d[i].start == 0 && d[i].len == 2) hid1 = 1;
            if (d[i].start == 4 && d[i].len == 2) hid2 = 1;
        }
        CHECK(hid1 == 1);   /* first line "> " */
        CHECK(hid2 == 1);   /* continuation "> " */
        free(d); free(pool);
    }

    /* --- link --- */
    {
        const char* t = "[go](http://x)";  /* text 1..3, url 5..13 */
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int linkfmt = 0, linkdeco = 0, hidopen = 0;
        for (size_t i = 0; i < n; i++) {
            if (d[i].kind == DECO_FMT && (d[i].fmt & MD_FMT_LINK)) linkfmt = 1;
            if (d[i].kind == DECO_LINK) {
                linkdeco = 1;
                CHECK(d[i].start == 1 && d[i].len == 2);        /* "go" */
                CHECK(strncmp(pool + d[i].aux_start, "http://x", 8) == 0);
                CHECK(d[i].aux_len == 8);
            }
            if (d[i].kind == DECO_HIDE && d[i].start == 0 && d[i].len == 1) hidopen = 1;
        }
        CHECK(linkfmt == 1);
        CHECK(linkdeco == 1);
        CHECK(hidopen == 1);   /* "[" hidden */
        free(d); free(pool);
    }

    {   /* link text with nested emphasis: url span still correct */
        const char* t = "[**b**](http://x)";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int ok = 0;
        for (size_t i = 0; i < n; i++)
            if (d[i].kind == DECO_LINK &&
                d[i].aux_len == 8 && strncmp(pool + d[i].aux_start, "http://x", 8) == 0)
                ok = 1;
        CHECK(ok == 1);   /* aux is "http://x", not "**](http://x" */
        free(d); free(pool);
    }

    /* --- reference link: url comes from the [r]: definition --- */
    {
        const char* t = "[go][r]\n\n[r]: http://x";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int ok = 0, hid_open = 0;
        for (size_t i = 0; i < n; i++) {
            if (d[i].kind == DECO_LINK && d[i].aux_len == 8 &&
                strncmp(pool + d[i].aux_start, "http://x", 8) == 0) ok = 1;
            if (d[i].kind == DECO_HIDE && d[i].start == 0 && d[i].len == 1) hid_open = 1;
        }
        CHECK(ok == 1);        /* url resolved from the reference definition */
        CHECK(hid_open == 1);  /* "[" hidden */
        free(d); free(pool);
    }
    /* --- autolink: <url> ; visible text is the url, < and > hidden --- */
    {
        const char* t = "<http://x>";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int ok = 0, hid_lt = 0, hid_gt = 0;
        for (size_t i = 0; i < n; i++) {
            if (d[i].kind == DECO_LINK && d[i].aux_len == 8 &&
                strncmp(pool + d[i].aux_start, "http://x", 8) == 0) ok = 1;
            if (d[i].kind == DECO_HIDE && d[i].start == 0 && d[i].len == 1) hid_lt = 1;   /* "<" */
            if (d[i].kind == DECO_HIDE && d[i].start == 9 && d[i].len == 1) hid_gt = 1;   /* ">" */
        }
        CHECK(ok == 1);
        CHECK(hid_lt == 1 && hid_gt == 1);
        free(d); free(pool);
    }
    /* --- inline link regression: url from href, still correct --- */
    {
        const char* t = "[go](http://x)";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        int ok = 0;
        for (size_t i = 0; i < n; i++)
            if (d[i].kind == DECO_LINK && d[i].aux_len == 8 &&
                strncmp(pool + d[i].aux_start, "http://x", 8) == 0) ok = 1;
        CHECK(ok == 1);
        free(d); free(pool);
    }

    /* --- markdown_task_at --- */
    {
        const char* t = "- [ ] a";   /* '[' at 2, mark(space) at 3, ']' at 4 */
        size_t mo = 0; int ck = 9;
        CHECK(markdown_task_at(t, strlen(t), 2, &mo, &ck) == 1);  /* on '[' */
        CHECK(mo == 3 && ck == 0);
        CHECK(markdown_task_at(t, strlen(t), 3, &mo, &ck) == 1);  /* on mark */
        CHECK(markdown_task_at(t, strlen(t), 4, &mo, &ck) == 1);  /* on ']' */
        CHECK(markdown_task_at(t, strlen(t), 6, &mo, &ck) == 0);  /* in text */
    }
    {
        const char* t = "- [x] b";
        size_t mo = 0; int ck = 0;
        CHECK(markdown_task_at(t, strlen(t), 3, &mo, &ck) == 1);
        CHECK(ck == 1);
    }
    {
        const char* t = "- a";       /* not a task item */
        size_t mo = 0; int ck = 0;
        CHECK(markdown_task_at(t, strlen(t), 2, &mo, &ck) == 0);
    }

    /* --- DECO_TASK emission + from_decos queries --- */
    {
        const char* t = "- [ ] a\n- [x] b";
        Deco* d = NULL; char* pool = NULL;
        size_t n = markdown_decorate(t, strlen(t), (size_t)-1, 0, &d, &pool);
        /* first item: mark at 3, checkbox range [2,5); second: mark at 11 */
        int t0 = 0, t1 = 0;
        for (size_t i = 0; i < n; i++) if (d[i].kind == DECO_TASK) {
            if (d[i].start == 2 && d[i].len == 3 && d[i].aux_start == 3 && d[i].number == 0) t0 = 1;
            if (d[i].aux_start == 11 && d[i].number == 1) t1 = 1;
        }
        CHECK(t0 == 1);   /* "- [ ]" -> unchecked checkbox at [2,5) */
        CHECK(t1 == 1);   /* "- [x]" -> checked */

        /* markdown_task_from_decos over the same list */
        size_t mo = 0; int ck = 9;
        CHECK(markdown_task_from_decos(d, n, 2, &mo, &ck) == 1);  /* on '[' */
        CHECK(mo == 3 && ck == 0);
        CHECK(markdown_task_from_decos(d, n, 3, &mo, &ck) == 1);  /* on mark */
        CHECK(markdown_task_from_decos(d, n, 4, &mo, &ck) == 1);  /* on ']' */
        CHECK(markdown_task_from_decos(d, n, 6, &mo, &ck) == 0);  /* in text */

        /* markdown_fmt_from_decos: the checked item's content is struck */
        CHECK((markdown_fmt_from_decos(d, n, 14) & MD_FMT_STRIKE) != 0); /* inside "b" */
        free(d); free(pool);
    }
}

/* --- span-level reveal: inline marker hides gate on span extent --- */
static int find_hide(const Deco* d, size_t n, size_t start, size_t len,
                     size_t* aux_s, size_t* aux_l) {
    for (size_t i = 0; i < n; i++)
        if (d[i].kind == DECO_HIDE && d[i].start == start && d[i].len == len) {
            if (aux_s) *aux_s = d[i].aux_start;
            if (aux_l) *aux_l = d[i].aux_len;
            return 1;
        }
    return 0;
}

void test_span_reveal(void) {
    Deco* d; char* pool; size_t n;

    /* "a **b** c" — extent [2,7): open "**"@2, close "**"@5 */
    const char* s1 = "a **b** c";
    /* caret 0: same paragraph but outside the span -> markers hidden
     * (this is the span-vs-paragraph proof: old code revealed them) */
    n = markdown_decorate(s1, 9, 0, 0, &d, &pool);
    size_t as = 0, al = 0;   /* init: silences -Wmaybe-uninitialized, not a semantic change */
    CHECK(find_hide(d, n, 2, 2, &as, &al)); CHECK(as == 2 && al == 5);
    CHECK(find_hide(d, n, 5, 2, &as, &al)); CHECK(as == 2 && al == 5);
    free(d); free(pool);
    /* caret 2 == extent start (touching) -> revealed (no hides) */
    n = markdown_decorate(s1, 9, 2, 2, &d, &pool);
    CHECK(!find_hide(d, n, 2, 2, NULL, NULL));
    CHECK(!find_hide(d, n, 5, 2, NULL, NULL));
    free(d); free(pool);
    /* caret 7 == extent end (touching) -> revealed */
    n = markdown_decorate(s1, 9, 7, 7, &d, &pool);
    CHECK(!find_hide(d, n, 2, 2, NULL, NULL));
    free(d); free(pool);
    /* caret 8 (one past extent end) -> hidden again */
    n = markdown_decorate(s1, 9, 8, 8, &d, &pool);
    CHECK(find_hide(d, n, 2, 2, NULL, NULL));
    free(d); free(pool);

    /* nested "**a *b* c**" — outer [0,11), inner [4,7) */
    const char* s2 = "**a *b* c**";
    /* caret 5 (inside inner): all four marker hides revealed */
    n = markdown_decorate(s2, 11, 5, 5, &d, &pool);
    CHECK(!find_hide(d, n, 0, 2, NULL, NULL));
    CHECK(!find_hide(d, n, 4, 1, NULL, NULL));
    CHECK(!find_hide(d, n, 6, 1, NULL, NULL));
    CHECK(!find_hide(d, n, 9, 2, NULL, NULL));
    free(d); free(pool);
    /* caret 8 (outer only): inner hides back, outer still revealed */
    n = markdown_decorate(s2, 11, 8, 8, &d, &pool);
    CHECK(find_hide(d, n, 4, 1, &as, &al)); CHECK(as == 4 && al == 3);
    CHECK(find_hide(d, n, 6, 1, NULL, NULL));
    CHECK(!find_hide(d, n, 0, 2, NULL, NULL));
    CHECK(!find_hide(d, n, 9, 2, NULL, NULL));
    free(d); free(pool);

    /* code span "a `b` c" — extent [2,5) */
    const char* s3 = "a `b` c";
    n = markdown_decorate(s3, 7, 3, 3, &d, &pool);   /* caret in span */
    CHECK(!find_hide(d, n, 2, 1, NULL, NULL));
    CHECK(!find_hide(d, n, 4, 1, NULL, NULL));
    free(d); free(pool);
    n = markdown_decorate(s3, 7, 0, 0, &d, &pool);   /* caret outside */
    CHECK(find_hide(d, n, 2, 1, &as, &al)); CHECK(as == 2 && al == 3);
    free(d); free(pool);

    /* link "x [t](u)" — '['@2, "](u)"@4 len4, extent [2,8) */
    const char* s4 = "x [t](u)";
    n = markdown_decorate(s4, 8, 0, 0, &d, &pool);   /* caret outside */
    CHECK(find_hide(d, n, 2, 1, &as, &al)); CHECK(as == 2 && al == 6);
    CHECK(find_hide(d, n, 4, 4, &as, &al)); CHECK(as == 2 && al == 6);
    free(d); free(pool);
    n = markdown_decorate(s4, 8, 6, 6, &d, &pool);   /* caret in url */
    CHECK(!find_hide(d, n, 2, 1, NULL, NULL));
    CHECK(!find_hide(d, n, 4, 4, NULL, NULL));
    free(d); free(pool);

    /* autolink "<http://x>" — '<'@0, '>'@9, extent [0,10) */
    const char* s5 = "<http://x>";
    n = markdown_decorate(s5, 10, 4, 4, &d, &pool);  /* caret inside */
    CHECK(!find_hide(d, n, 0, 1, NULL, NULL));
    CHECK(!find_hide(d, n, 9, 1, NULL, NULL));
    free(d); free(pool);

    /* block markers keep PARAGRAPH reveal: "# h\r\nx" — caret on the
     * heading line reveals '#', caret on the other line hides it */
    const char* s6 = "# h\r\nx";
    n = markdown_decorate(s6, 6, 1, 1, &d, &pool);
    CHECK(!find_hide(d, n, 0, 2, NULL, NULL));
    free(d); free(pool);
    n = markdown_decorate(s6, 6, 5, 5, &d, &pool);
    CHECK(find_hide(d, n, 0, 2, &as, &al)); CHECK(al == 0);  /* block: no extent */
    free(d); free(pool);

    /* textless-span guard: bare "``" must not emit a bogus hide */
    n = markdown_decorate("``", 2, (size_t)-1, 0, &d, &pool);
    for (size_t i = 0; i < n; i++) CHECK(d[i].kind != DECO_HIDE || d[i].start < 2);
    free(d); free(pool);
}

void test_reveal_sig(void) {
    Deco* d; char* pool; size_t n;

    const char* s1 = "a **b** c";                    /* extent [2,7) */
    n = markdown_decorate(s1, 9, (size_t)-1, 0, &d, &pool);   /* hide-all */
    CHECK(markdown_reveal_sig(d, n, 0, 0) == 0);               /* plain text */
    size_t in1 = markdown_reveal_sig(d, n, 4, 4);
    CHECK(in1 != 0);
    CHECK(markdown_reveal_sig(d, n, 2, 2) == in1);             /* edges equal */
    CHECK(markdown_reveal_sig(d, n, 7, 7) == in1);
    CHECK(markdown_reveal_sig(d, n, 8, 8) == 0);               /* escaped */
    CHECK(markdown_reveal_sig(d, n, 0, 4) == in1);             /* selection overlap */
    free(d); free(pool);

    const char* s2 = "**a *b* c**";                  /* outer [0,11) inner [4,7) */
    n = markdown_decorate(s2, 11, (size_t)-1, 0, &d, &pool);
    size_t inner = markdown_reveal_sig(d, n, 5, 5);
    size_t outer = markdown_reveal_sig(d, n, 8, 8);
    CHECK(inner != 0 && outer != 0 && inner != outer);
    free(d); free(pool);

    /* DECO_LINK aux is a POOL offset — must be ignored by the sig.
     * "x [t](u)": link deco has aux_start==0; a misread extent [0,~1]
     * would touch caret 0. Assert sig at 0 is exactly 0. */
    const char* s4 = "x [t](u)";
    n = markdown_decorate(s4, 8, (size_t)-1, 0, &d, &pool);
    CHECK(markdown_reveal_sig(d, n, 0, 0) == 0);
    CHECK(markdown_reveal_sig(d, n, 5, 5) != 0);
    free(d); free(pool);

    /* DECO_TASK aux ignored: "- [ ] a" */
    const char* s7 = "- [ ] a";
    n = markdown_decorate(s7, 7, (size_t)-1, 0, &d, &pool);
    CHECK(markdown_reveal_sig(d, n, 6, 6) == 0);
    free(d); free(pool);
}
