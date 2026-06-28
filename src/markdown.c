#include "markdown.h"
#include "md4c.h"
#include <string.h>

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
    return 0;
}
static int cb_leave_span(MD_SPANTYPE type, void* detail, void* ud) {
    (void)detail; Ctx* c = (Ctx*)ud;
    if (type == MD_SPAN_STRONG) c->stack &= ~MD_FMT_BOLD;
    else if (type == MD_SPAN_EM) c->stack &= ~MD_FMT_ITALIC;
    else if (type == MD_SPAN_CODE) c->stack &= ~MD_FMT_CODE;
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
    parser.flags = MD_DIALECT_COMMONMARK;
    parser.enter_block = cb_enter_block;
    parser.leave_block = cb_leave_block;
    parser.enter_span  = cb_enter_span;
    parser.leave_span  = cb_leave_span;
    parser.text        = cb_text;
    md_parse(text, (MD_SIZE)len, &parser, &c);
    return c.count;
}
