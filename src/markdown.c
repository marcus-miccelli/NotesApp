#include "markdown.h"
#include "md4c.h"
#include <string.h>
#include <stdlib.h>

/* ---- decoration builder (markdown_decorate) ---- */

typedef struct {
    const char* base;
    size_t len;
    Deco*  arr;
    size_t count, cap;
    /* heading state */
    int    in_heading;
    int    h_first_text;
    MdFmt  h_fmt;
    /* code block state */
    int    in_code;
    int    code_first_text;
    size_t code_content_start;
    /* quote block state */
    int    in_quote;
    int    quote_first_text;
    size_t quote_start;
    /* inline span stack */
    struct { MdFmt fmt; int marklen; int start_set;
             size_t open_start; int open_len; } sp[64];
    int    depth;
    int    over;   /* tracked opens dropped past the depth cap; unwound on leave */
    size_t last_text_end;
    size_t close_cursor;
    /* list state (single level) */
    ParaKind listkind;
    int      listnum;      /* next ordinal for ordered lists */
    int      in_li;
    int      li_first_text;
    int    li_is_task;
    int    li_task_checked;
    size_t li_task_mark_off;    /* source offset of the mark char between [ ] */
    /* link state */
    int    in_a;
    int    a_text_set;
    size_t a_text_start;
    int    a_is_autolink;
    size_t a_url_off, a_url_len;   /* url span in the pool */
    size_t a_open_off;             /* recorded '['/'<' (pushed at leave) */
    size_t a_open_len;
    int    a_open_set;
    char*  urlpool;
    size_t upcount, upcap;
    /* reveal window */
    size_t sel_lo, sel_hi;   /* reveal window; lo>hi = hide all */
} DCtx;

/* Does the source paragraph containing [start, start+len) intersect the reveal
 * window [sel_lo, sel_hi]?  Paragraph = run between \n/\r boundaries. */
static int in_reveal(DCtx* c, size_t start, size_t len) {
    if (c->sel_lo > c->sel_hi) return 0;               /* hide-all sentinel */
    size_t ps = start;
    while (ps > 0 && c->base[ps-1] != '\n' && c->base[ps-1] != '\r') ps--;
    size_t pe = start + len;
    while (pe < c->len && c->base[pe] != '\n' && c->base[pe] != '\r') pe++;
    return c->sel_lo <= pe && c->sel_hi >= ps;         /* ranges overlap */
}

/* Append n bytes to the url pool, returning the start offset. Not NUL-terminated
 * in the pool (the consumer terminates its own copy). */
static size_t pool_add(DCtx* c, const char* s, size_t n) {
    if (c->upcount + n > c->upcap) {
        size_t nc = c->upcap ? c->upcap * 2 : 128;
        while (nc < c->upcount + n) nc *= 2;
        char* np = realloc(c->urlpool, nc);
        if (!np) return c->upcount;    /* drop on OOM; url stays empty */
        c->urlpool = np; c->upcap = nc;
    }
    size_t off = c->upcount;
    if (n) { memcpy(c->urlpool + off, s, n); c->upcount += n; }
    return off;
}

static void push(DCtx* c, DecoKind k, size_t start, size_t len,
                 MdFmt fmt, ParaKind para, int number) {
    if (len == 0 && k != DECO_PARA) return;
    if (c->count == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 32;
        Deco* na = realloc(c->arr, nc * sizeof(Deco));
        if (!na) return;               /* drop on OOM; render stays safe */
        c->arr = na; c->cap = nc;
    }
    Deco* d = &c->arr[c->count++];
    d->kind = k; d->start = start; d->len = len;
    d->fmt = fmt; d->para = para; d->number = number;
    d->aux_start = 0; d->aux_len = 0;
}

static void push_link(DCtx* c, size_t start, size_t len, size_t url_start, size_t url_len) {
    push(c, DECO_LINK, start, len, MD_FMT_LINK, PARA_NONE, 0);
    if (c->count > 0) {
        Deco* d = &c->arr[c->count - 1];
        d->aux_start = url_start; d->aux_len = url_len;
    }
}

static void push_hide(DCtx* c, size_t start, size_t len) {
    if (len == 0) return;
    if (in_reveal(c, start, len)) return;   /* caret paragraph: show markers */
    push(c, DECO_HIDE, start, len, 0, PARA_NONE, 0);
}

/* Inclusive touch: caret offsets are between-char positions, so a window
 * [lo, hi] touches an extent [a, b] when lo <= b && hi >= a. Shared by the
 * builder gate below and markdown_reveal_sig — they must agree exactly. */
static int md_touch(size_t lo, size_t hi, size_t a, size_t b) {
    return lo <= b && hi >= a;
}

/* Hide an inline span marker: revealed only while the reveal window touches
 * the span's full extent [ext_s, ext_e] (span-level reveal, vs. the
 * paragraph-level push_hide). The extent rides on the deco's aux fields so
 * cached hide-all lists can answer reveal queries without a parse. */
static void push_hide_span(DCtx* c, size_t start, size_t len,
                           size_t ext_s, size_t ext_e) {
    if (len == 0) return;
    if (c->sel_lo <= c->sel_hi && md_touch(c->sel_lo, c->sel_hi, ext_s, ext_e))
        return;                            /* caret touches span: show markers */
    size_t before = c->count;
    push(c, DECO_HIDE, start, len, 0, PARA_NONE, 0);
    if (c->count > before) {
        c->arr[c->count - 1].aux_start = ext_s;
        c->arr[c->count - 1].aux_len   = ext_e - ext_s;
    }
}

static MdFmt dh_fmt(unsigned level) {
    return level == 1 ? MD_FMT_H1 : level == 2 ? MD_FMT_H2 : MD_FMT_H3;
}

static int d_enter_block(MD_BLOCKTYPE t, void* detail, void* ud) {
    DCtx* c = (DCtx*)ud;
    if (t == MD_BLOCK_H) {
        MD_BLOCK_H_DETAIL* d = (MD_BLOCK_H_DETAIL*)detail;
        c->in_heading = 1; c->h_first_text = 0; c->h_fmt = dh_fmt(d->level);
    }
    if (t == MD_BLOCK_UL) { c->listkind = PARA_BULLET; }
    if (t == MD_BLOCK_OL) {
        MD_BLOCK_OL_DETAIL* d = (MD_BLOCK_OL_DETAIL*)detail;
        c->listkind = PARA_NUMBER; c->listnum = (int)d->start;
    }
    if (t == MD_BLOCK_LI) {
        MD_BLOCK_LI_DETAIL* d = (MD_BLOCK_LI_DETAIL*)detail;
        c->in_li = 1; c->li_first_text = 0;
        c->li_is_task = d->is_task;
        c->li_task_checked = d->is_task && (d->task_mark == 'x' || d->task_mark == 'X');
        c->li_task_mark_off = (size_t)d->task_mark_offset;
    }
    if (t == MD_BLOCK_CODE) { c->in_code = 1; c->code_first_text = 0; }
    if (t == MD_BLOCK_QUOTE) { c->in_quote = 1; c->quote_first_text = 0; }
    return 0;
}
static int d_leave_block(MD_BLOCKTYPE t, void* detail, void* ud) {
    (void)detail; DCtx* c = (DCtx*)ud;
    if (t == MD_BLOCK_H) c->in_heading = 0;
    if (t == MD_BLOCK_LI) {
        c->in_li = 0;
        if (c->listkind == PARA_NUMBER) c->listnum++;
    }
    if (t == MD_BLOCK_UL || t == MD_BLOCK_OL) c->listkind = PARA_NONE;
    if (t == MD_BLOCK_CODE) {
        if (c->code_first_text) {
            size_t ce = c->last_text_end;
            /* Skip any CR/LF after the code content, but only if within bounds */
            if (ce < c->len && c->base[ce] == '\r') ce++;
            if (ce < c->len && c->base[ce] == '\n') ce++;  /* closing fence line start */
            /* One contiguous shaded range over the whole block body (incl. the
             * newlines between code lines) so the background is not striped. */
            if (ce > c->code_content_start)
                push(c, DECO_FMT, c->code_content_start, ce - c->code_content_start,
                     MD_FMT_CODEBLOCK, PARA_NONE, 0);
            if (ce < c->len) {
                size_t fe = ce;
                while (fe < c->len && c->base[fe] != '\n' && c->base[fe] != '\r') fe++;
                if (fe > ce) push_hide(c, ce, fe - ce);
            }
        }
        c->in_code = 0;
        c->code_first_text = 0;
    }
    if (t == MD_BLOCK_QUOTE) {
        if (c->quote_first_text) {
            /* indent the whole quote, then hide the "> " prefix on every line */
            push(c, DECO_PARA, c->quote_start,
                 c->last_text_end - c->quote_start, 0, PARA_QUOTE, 0);
            size_t p = c->quote_start;
            while (p < c->last_text_end) {
                size_t ls = p, q = p;
                while (q < c->len && c->base[q] == ' ') q++;   /* optional indent */
                if (q < c->len && c->base[q] == '>') {
                    q++;
                    if (q < c->len && c->base[q] == ' ') q++;
                    push_hide(c, ls, q - ls);                  /* ">"/"> " */
                }
                while (p < c->len && c->base[p] != '\n' && c->base[p] != '\r') p++;
                if (p < c->len && c->base[p] == '\r') p++;
                if (p < c->len && c->base[p] == '\n') p++;
            }
        }
        c->in_quote = 0;
        c->quote_first_text = 0;
    }
    return 0;
}
static MdFmt span_fmt(MD_SPANTYPE t) {
    if (t == MD_SPAN_STRONG) return MD_FMT_BOLD;
    if (t == MD_SPAN_EM)     return MD_FMT_ITALIC;
    if (t == MD_SPAN_DEL)    return MD_FMT_STRIKE;
    if (t == MD_SPAN_CODE)   return MD_FMT_CODE;
    return 0;
}
static int span_marklen(MD_SPANTYPE t) {
    if (t == MD_SPAN_STRONG || t == MD_SPAN_DEL) return 2;
    if (t == MD_SPAN_EM)                          return 1;
    return -1;   /* code: computed lazily from the backtick run */
}
static int d_enter_span(MD_SPANTYPE t, void* detail, void* ud) {
    DCtx* c = (DCtx*)ud;
    if (t == MD_SPAN_A) {
        MD_SPAN_A_DETAIL* a = (MD_SPAN_A_DETAIL*)detail;
        c->in_a = 1; c->a_text_set = 0; c->a_open_set = 0;
        c->a_is_autolink = a->is_autolink;
        size_t before = c->upcount;
        c->a_url_off = pool_add(c, a->href.text, (size_t)a->href.size);
        c->a_url_len = c->upcount - before;   /* 0 if pool_add failed (OOM) */
        return 0;
    }
    if (!span_fmt(t)) return 0;
    if (c->depth >= 64) { c->over++; return 0; }   /* dropped; balance on leave */
    c->sp[c->depth].fmt = span_fmt(t);
    c->sp[c->depth].marklen = span_marklen(t);
    c->sp[c->depth].start_set = 0;
    c->depth++;
    return 0;
}
static int d_leave_span(MD_SPANTYPE t, void* detail, void* ud) {
    (void)detail; DCtx* c = (DCtx*)ud;
    if (t == MD_SPAN_A) {
        size_t te = c->last_text_end > c->close_cursor ? c->last_text_end
                                                       : c->close_cursor;
        size_t close_e = te;
        if (c->a_is_autolink) {
            if (te < c->len && c->base[te] == '>') close_e = te + 1;  /* '>' */
        } else if (te < c->len && c->base[te] == ']') {
            size_t p = te + 1;
            if (p < c->len && c->base[p] == '(') {                /* inline "](url)" */
                while (p < c->len && c->base[p] != ')' && c->base[p] != '\n') p++;
                if (p < c->len && c->base[p] == ')') p++;
            } else if (p < c->len && c->base[p] == '[') {         /* reference "][ref]" */
                p++;
                while (p < c->len && c->base[p] != ']' && c->base[p] != '\n') p++;
                if (p < c->len && c->base[p] == ']') p++;
            }
            /* else shortcut "]": p stays te+1 */
            close_e = p;
        }
        if (c->a_open_set) {
            /* span-level reveal over the whole link syntax */
            size_t ext_s = c->a_open_off, ext_e = close_e;
            push_hide_span(c, c->a_open_off, c->a_open_len, ext_s, ext_e);
            if (close_e > te) push_hide_span(c, te, close_e - te, ext_s, ext_e);
        } else if (close_e > te) {
            push_hide(c, te, close_e - te);   /* no opener recorded: legacy gate */
        }
        if (c->a_text_set)
            push_link(c, c->a_text_start, te - c->a_text_start,
                      c->a_url_off, c->a_url_len);
        c->in_a = 0;
        return 0;
    }
    if (!span_fmt(t)) return 0;
    if (c->over > 0) { c->over--; return 0; }       /* unwind a dropped open */
    if (c->depth <= 0) return 0;
    c->depth--;
    int ml = c->sp[c->depth].marklen; if (ml < 0) ml = 0;
    size_t base = c->last_text_end > c->close_cursor ? c->last_text_end
                                                     : c->close_cursor;
    /* full span extent incl. both markers; start_set guards a textless span
     * (no opening marker recorded -> never emit a bogus hide) */
    size_t ext_s = c->sp[c->depth].start_set ? c->sp[c->depth].open_start : base;
    size_t ext_e = base + (size_t)ml;
    if (c->sp[c->depth].start_set && c->sp[c->depth].open_len > 0)
        push_hide_span(c, c->sp[c->depth].open_start,
                       (size_t)c->sp[c->depth].open_len, ext_s, ext_e);
    if (ml > 0) push_hide_span(c, base, (size_t)ml, ext_s, ext_e);
    c->close_cursor = base + (size_t)ml;
    return 0;
}
static int d_text(MD_TEXTTYPE tt, const MD_CHAR* text, MD_SIZE size, void* ud) {
    (void)tt; DCtx* c = (DCtx*)ud;
    size_t off = (size_t)(text - c->base);

    if (c->in_code) {
        if (!c->code_first_text && off < c->len) {
            c->code_first_text = 1;
            c->code_content_start = off;
            size_t p = off;                                 /* hide opening fence line */
            if (p > 0 && c->base[p-1] == '\n') p--;
            if (p > 0 && c->base[p-1] == '\r') p--;
            size_t fs = p;
            while (fs > 0 && c->base[fs-1] != '\n' && c->base[fs-1] != '\r') fs--;
            push_hide(c, fs, off - fs);
        }
        if (off + (size_t)size <= c->len)
            c->last_text_end = off + (size_t)size;
        return 0;
    }

    /* resolve opening markers for spans opened since the last text run,
     * innermost first, consuming delimiter chars leftward from off. The
     * hide itself is pushed at d_leave_span, where the extent is known. */
    size_t cursor = off;
    for (int i = c->depth - 1; i >= 0 && !c->sp[i].start_set; i--) {
        int ml = c->sp[i].marklen;
        if (ml < 0) {                         /* code: count backticks left */
            size_t j = cursor; int k = 0;
            while (j > 0 && c->base[j-1] == '`') { j--; k++; }
            ml = k; c->sp[i].marklen = ml;
        }
        if ((size_t)ml > cursor) ml = (int)cursor;   /* clamp: never underflow */
        c->sp[i].open_start = cursor - (size_t)ml;
        c->sp[i].open_len   = ml;
        cursor -= (size_t)ml;
        c->sp[i].start_set = 1;
    }

    if (c->in_a && !c->a_text_set) {
        c->a_text_set = 1;
        c->a_text_start = off;
        if (c->a_is_autolink) {
            if (off > 0) { c->a_open_off = off - 1; c->a_open_len = 1;
                           c->a_open_set = 1; }         /* '<' */
        } else {
            size_t ab = off;                            /* scan left to the '[' */
            while (ab > 0 && c->base[ab-1] != '[') ab--;
            if (ab > 0) { c->a_open_off = ab - 1; c->a_open_len = 1;
                          c->a_open_set = 1; }          /* '[' */
        }
    }

    if (c->in_li && !c->li_first_text) {
        c->li_first_text = 1;
        size_t ls = off;
        while (ls > 0 && c->base[ls-1] != '\n' && c->base[ls-1] != '\r') ls--;
        if (c->li_is_task) {
            /* keep "[x]" visible: hide "- " before it and the run after it up to
             * the content; the mark char sits at li_task_mark_off (between [ ]). */
            size_t cb0 = c->li_task_mark_off > 0 ? c->li_task_mark_off - 1 : 0; /* '[' */
            size_t cb1 = c->li_task_mark_off + 2;                               /* past ']' */
            push_hide(c, ls, cb0 - ls);        /* "- " */
            if (off > cb1) push_hide(c, cb1, off - cb1);   /* space(s) after "]" */
            /* checkbox range [cb0, cb1) as a DECO_TASK: aux_start=mark, number=checked.
             * Unconditional push (never reveal-gated) so task hit-test always works.
             * push may drop on OOM; overwrite the new deco only if it landed. */
            push(c, DECO_TASK, cb0, cb1 - cb0, 0, PARA_NONE, 0);
            if (c->count) {
                Deco* td = &c->arr[c->count - 1];
                td->aux_start = c->li_task_mark_off;
                td->number = c->li_task_checked ? 1 : 0;
            }
        } else {
            push_hide(c, ls, off - ls);        /* "- "/"N. " */
        }
        push(c, DECO_PARA, ls, (off - ls) + (size_t)size,
             0, c->listkind,
             c->listkind == PARA_NUMBER ? c->listnum : 0);
    }

    MdFmt f = 0;
    for (int i = 0; i < c->depth; i++) f |= c->sp[i].fmt;
    if (c->in_li && c->li_task_checked) f |= MD_FMT_STRIKE;
    if (c->in_a) f |= MD_FMT_LINK;
    if (c->in_heading) {
        if (!c->h_first_text) {
            c->h_first_text = 1;
            size_t ls = off;
            while (ls > 0 && c->base[ls-1] != '\n' && c->base[ls-1] != '\r') ls--;
            push_hide(c, ls, off - ls);
        }
        f |= c->h_fmt;
    }
    if (c->in_quote) {
        if (!c->quote_first_text) {
            c->quote_first_text = 1;
            size_t ls = off;
            while (ls > 0 && c->base[ls-1] != '\n' && c->base[ls-1] != '\r') ls--;
            c->quote_start = ls;
        }
        f |= MD_FMT_QUOTE;
    }
    if (f) push(c, DECO_FMT, off, (size_t)size, f, PARA_NONE, 0);
    c->last_text_end = off + (size_t)size;
    return 0;
}

size_t markdown_decorate(const char* text, size_t len,
                         size_t sel_lo, size_t sel_hi, Deco** out, char** urlpool) {
    DCtx c; memset(&c, 0, sizeof c);
    c.base = text; c.len = len;
    c.sel_lo = sel_lo; c.sel_hi = sel_hi;
    MD_PARSER p; memset(&p, 0, sizeof p);
    p.abi_version = 0;
    p.flags = MD_DIALECT_COMMONMARK | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;
    p.enter_block = d_enter_block;
    p.leave_block = d_leave_block;
    p.text        = d_text;
    p.enter_span = d_enter_span;
    p.leave_span = d_leave_span;
    md_parse(text, (MD_SIZE)len, &p, &c);
    *out = c.arr;
    *urlpool = c.urlpool;
    return c.count;
}

MdFmt markdown_fmt_from_decos(const Deco* d, size_t n, size_t caret) {
    MdFmt f = 0;
    for (size_t i = 0; i < n; i++) {
        if (d[i].kind != DECO_FMT) continue;
        size_t a = d[i].start, b = a + d[i].len;
        /* cover the char on either side of the caret so a boundary still lights */
        if ((caret > a && caret <= b) || (caret >= a && caret < b))
            f |= d[i].fmt;
    }
    f &= ~(MdFmt)(MD_FMT_H1 | MD_FMT_H2 | MD_FMT_H3);   /* headings not toggles */
    return f;
}

int markdown_task_from_decos(const Deco* d, size_t n, size_t off,
                             size_t* mark_off, int* checked) {
    for (size_t i = 0; i < n; i++) {
        if (d[i].kind != DECO_TASK) continue;
        size_t a = d[i].start, b = a + d[i].len;
        if (off >= a && off < b) {
            if (mark_off) *mark_off = d[i].aux_start;
            if (checked)  *checked  = d[i].number;
            return 1;
        }
    }
    return 0;
}

size_t markdown_reveal_sig(const Deco* d, size_t n, size_t lo, size_t hi) {
    size_t sig = 0;
    for (size_t i = 0; i < n; i++) {
        if (d[i].kind != DECO_HIDE || d[i].aux_len == 0) continue;
        if (md_touch(lo, hi, d[i].aux_start, d[i].aux_start + d[i].aux_len))
            /* summed, not XORed: a span's open+close hides share one extent
             * and XOR would cancel the pair to 0 */
            sig += d[i].aux_start * 2654435761u + d[i].aux_len * 40503u;
    }
    return sig;
}

MdFmt markdown_fmt_at(const char* text, size_t len, size_t caret) {
    Deco* d = NULL; char* pool = NULL;
    size_t n = markdown_decorate(text, len, (size_t)-1, 0, &d, &pool);
    MdFmt f = markdown_fmt_from_decos(d, n, caret);
    free(d); free(pool);
    return f;
}

int markdown_task_at(const char* text, size_t len, size_t off,
                     size_t* mark_off, int* checked) {
    Deco* d = NULL; char* pool = NULL;
    size_t n = markdown_decorate(text, len, (size_t)-1, 0, &d, &pool);
    int hit = markdown_task_from_decos(d, n, off, mark_off, checked);
    free(d); free(pool);
    return hit;
}
