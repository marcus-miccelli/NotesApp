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
    /* inline span stack */
    struct { MdFmt fmt; int marklen; int start_set; } sp[64];
    int    depth;
    int    over;   /* tracked opens dropped past the depth cap; unwound on leave */
    size_t last_text_end;
    size_t close_cursor;
    /* list state (single level) */
    ParaKind listkind;
    int      listnum;      /* next ordinal for ordered lists */
    int      in_li;
    int      li_first_text;
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
}

static void push_hide(DCtx* c, size_t start, size_t len) {
    if (len == 0) return;
    if (in_reveal(c, start, len)) return;   /* caret paragraph: show markers */
    push(c, DECO_HIDE, start, len, 0, PARA_NONE, 0);
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
    if (t == MD_BLOCK_LI) { c->in_li = 1; c->li_first_text = 0; }
    if (t == MD_BLOCK_CODE) { c->in_code = 1; c->code_first_text = 0; }
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
        size_t ce = c->last_text_end;
        /* Skip any CR/LF after the code content, but only if within bounds */
        if (ce < c->len && c->base[ce] == '\r') ce++;
        if (ce < c->len && c->base[ce] == '\n') ce++;      /* closing fence line start */
        /* One contiguous shaded range over the whole block body (incl. the
         * newlines between code lines) so the background is not striped. */
        if (c->code_first_text && ce > c->code_content_start)
            push(c, DECO_FMT, c->code_content_start, ce - c->code_content_start,
                 MD_FMT_CODEBLOCK, PARA_NONE, 0);
        if (ce < c->len) {
            size_t fe = ce;
            while (fe < c->len && c->base[fe] != '\n' && c->base[fe] != '\r') fe++;
            if (fe > ce) push_hide(c, ce, fe - ce);
        }
        c->in_code = 0;
        c->code_first_text = 0;
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
    (void)detail; DCtx* c = (DCtx*)ud;
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
    if (!span_fmt(t)) return 0;
    if (c->over > 0) { c->over--; return 0; }       /* unwind a dropped open */
    if (c->depth <= 0) return 0;
    c->depth--;
    int ml = c->sp[c->depth].marklen; if (ml < 0) ml = 0;
    size_t base = c->last_text_end > c->close_cursor ? c->last_text_end
                                                     : c->close_cursor;
    if (ml > 0) push_hide(c, base, (size_t)ml);
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
     * innermost first, consuming delimiter chars leftward from off. */
    size_t cursor = off;
    for (int i = c->depth - 1; i >= 0 && !c->sp[i].start_set; i--) {
        int ml = c->sp[i].marklen;
        if (ml < 0) {                         /* code: count backticks left */
            size_t j = cursor; int k = 0;
            while (j > 0 && c->base[j-1] == '`') { j--; k++; }
            ml = k; c->sp[i].marklen = ml;
        }
        if ((size_t)ml > cursor) ml = (int)cursor;   /* clamp: never underflow */
        if (ml > 0)
            push_hide(c, cursor - (size_t)ml, (size_t)ml);
        cursor -= (size_t)ml;
        c->sp[i].start_set = 1;
    }

    if (c->in_li && !c->li_first_text) {
        c->li_first_text = 1;
        size_t ls = off;
        while (ls > 0 && c->base[ls-1] != '\n' && c->base[ls-1] != '\r') ls--;
        push_hide(c, ls, off - ls);           /* "- "/"N. " */
        push(c, DECO_PARA, ls, (off - ls) + (size_t)size,
             0, c->listkind,
             c->listkind == PARA_NUMBER ? c->listnum : 0);
    }

    MdFmt f = 0;
    for (int i = 0; i < c->depth; i++) f |= c->sp[i].fmt;
    if (c->in_heading) {
        if (!c->h_first_text) {
            c->h_first_text = 1;
            size_t ls = off;
            while (ls > 0 && c->base[ls-1] != '\n' && c->base[ls-1] != '\r') ls--;
            push_hide(c, ls, off - ls);
        }
        f |= c->h_fmt;
    }
    if (f) push(c, DECO_FMT, off, (size_t)size, f, PARA_NONE, 0);
    c->last_text_end = off + (size_t)size;
    return 0;
}

size_t markdown_decorate(const char* text, size_t len,
                         size_t sel_lo, size_t sel_hi, Deco** out) {
    DCtx c; memset(&c, 0, sizeof c);
    c.base = text; c.len = len;
    c.sel_lo = sel_lo; c.sel_hi = sel_hi;
    MD_PARSER p; memset(&p, 0, sizeof p);
    p.abi_version = 0;
    p.flags = MD_DIALECT_COMMONMARK | MD_FLAG_STRIKETHROUGH;
    p.enter_block = d_enter_block;
    p.leave_block = d_leave_block;
    p.text        = d_text;
    p.enter_span = d_enter_span;
    p.leave_span = d_leave_span;
    md_parse(text, (MD_SIZE)len, &p, &c);
    *out = c.arr;
    return c.count;
}

MdFmt markdown_fmt_at(const char* text, size_t len, size_t caret) {
    Deco* d = NULL;
    size_t n = markdown_decorate(text, len, (size_t)-1, 0, &d);
    MdFmt f = 0;
    for (size_t i = 0; i < n; i++) {
        if (d[i].kind != DECO_FMT) continue;
        size_t a = d[i].start, b = a + d[i].len;
        /* cover the char on either side of the caret so a boundary still lights */
        if ((caret > a && caret <= b) || (caret >= a && caret < b))
            f |= d[i].fmt;
    }
    free(d);
    f &= ~(MdFmt)(MD_FMT_H1 | MD_FMT_H2 | MD_FMT_H3);   /* headings not toggles */
    return f;
}
