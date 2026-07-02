#include "markdown.h"
#include "md4c.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    const char* base;       /* input start, for offset math */
    MdSpan* out;
    size_t cap;
    size_t count;           /* total spans seen (may exceed cap) */
    MdFmt stack;            /* OR of currently-active inline/heading formats */
} Ctx;

static MdFmt heading_fmt(unsigned level) {
    if (level == 1) return MD_FMT_H1;
    if (level == 2) return MD_FMT_H2;
    return MD_FMT_H3;   /* level 3+ all map to H3 styling */
}

static int cb_enter_block(MD_BLOCKTYPE type, void* detail, void* ud) {
    Ctx* c = (Ctx*)ud;
    if (type == MD_BLOCK_H) {
        MD_BLOCK_H_DETAIL* d = (MD_BLOCK_H_DETAIL*)detail;
        c->stack |= heading_fmt(d->level);
    }
    return 0;
}
static int cb_leave_block(MD_BLOCKTYPE type, void* detail, void* ud) {
    Ctx* c = (Ctx*)ud;
    if (type == MD_BLOCK_H) {
        MD_BLOCK_H_DETAIL* d = (MD_BLOCK_H_DETAIL*)detail;
        c->stack &= ~heading_fmt(d->level);
    }
    return 0;
}
static int cb_enter_span(MD_SPANTYPE type, void* detail, void* ud) {
    (void)detail; Ctx* c = (Ctx*)ud;
    if (type == MD_SPAN_STRONG) c->stack |= MD_FMT_BOLD;
    else if (type == MD_SPAN_EM) c->stack |= MD_FMT_ITALIC;
    else if (type == MD_SPAN_CODE) c->stack |= MD_FMT_CODE;
    else if (type == MD_SPAN_DEL) c->stack |= MD_FMT_STRIKE;
    return 0;
}
static int cb_leave_span(MD_SPANTYPE type, void* detail, void* ud) {
    (void)detail; Ctx* c = (Ctx*)ud;
    if (type == MD_SPAN_STRONG) c->stack &= ~MD_FMT_BOLD;
    else if (type == MD_SPAN_EM) c->stack &= ~MD_FMT_ITALIC;
    else if (type == MD_SPAN_CODE) c->stack &= ~MD_FMT_CODE;
    else if (type == MD_SPAN_DEL) c->stack &= ~MD_FMT_STRIKE;
    return 0;
}
static int cb_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* ud) {
    (void)type; Ctx* c = (Ctx*)ud;
    if (c->stack == 0 || size == 0) return 0;   /* unformatted run: skip */
    size_t off = (size_t)(text - c->base);
    if (c->count < c->cap) {
        c->out[c->count].start = off;
        c->out[c->count].len = (size_t)size;
        c->out[c->count].fmt = c->stack;
    }
    c->count++;
    return 0;
}

size_t markdown_spans(const char* text, size_t len, MdSpan* out, size_t out_cap) {
    Ctx c = { text, out, out_cap, 0, 0 };
    MD_PARSER parser;
    memset(&parser, 0, sizeof parser);
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_COMMONMARK | MD_FLAG_STRIKETHROUGH;
    parser.enter_block = cb_enter_block;
    parser.leave_block = cb_leave_block;
    parser.enter_span  = cb_enter_span;
    parser.leave_span  = cb_leave_span;
    parser.text        = cb_text;
    md_parse(text, (MD_SIZE)len, &parser, &c);
    return c.count;
}

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
} DCtx;

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

static MdFmt dh_fmt(unsigned level) {
    return level == 1 ? MD_FMT_H1 : level == 2 ? MD_FMT_H2 : MD_FMT_H3;
}

static int d_enter_block(MD_BLOCKTYPE t, void* detail, void* ud) {
    DCtx* c = (DCtx*)ud;
    if (t == MD_BLOCK_H) {
        MD_BLOCK_H_DETAIL* d = (MD_BLOCK_H_DETAIL*)detail;
        c->in_heading = 1; c->h_first_text = 0; c->h_fmt = dh_fmt(d->level);
    }
    return 0;
}
static int d_leave_block(MD_BLOCKTYPE t, void* detail, void* ud) {
    (void)detail; DCtx* c = (DCtx*)ud;
    if (t == MD_BLOCK_H) c->in_heading = 0;
    return 0;
}
static int d_text(MD_TEXTTYPE tt, const MD_CHAR* text, MD_SIZE size, void* ud) {
    (void)tt; DCtx* c = (DCtx*)ud;
    size_t off = (size_t)(text - c->base);
    MdFmt f = 0;
    if (c->in_heading) {
        if (!c->h_first_text) {
            c->h_first_text = 1;
            size_t ls = off;
            while (ls > 0 && c->base[ls-1] != '\n' && c->base[ls-1] != '\r') ls--;
            push(c, DECO_HIDE, ls, off - ls, 0, PARA_NONE, 0);   /* "# " */
        }
        f |= c->h_fmt;
    }
    if (f) push(c, DECO_FMT, off, (size_t)size, f, PARA_NONE, 0);
    return 0;
}

size_t markdown_decorate(const char* text, size_t len,
                         size_t sel_lo, size_t sel_hi, Deco** out) {
    (void)sel_lo; (void)sel_hi;   /* reveal wired in a later task */
    DCtx c; memset(&c, 0, sizeof c);
    c.base = text; c.len = len;
    MD_PARSER p; memset(&p, 0, sizeof p);
    p.abi_version = 0;
    p.flags = MD_DIALECT_COMMONMARK | MD_FLAG_STRIKETHROUGH;
    p.enter_block = d_enter_block;
    p.leave_block = d_leave_block;
    p.text        = d_text;
    md_parse(text, (MD_SIZE)len, &p, &c);
    *out = c.arr;
    return c.count;
}
