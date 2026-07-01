#ifndef MARKDOWN_H
#define MARKDOWN_H
#include <stddef.h>

typedef unsigned MdFmt;
#define MD_FMT_BOLD   (1u<<0)
#define MD_FMT_ITALIC (1u<<1)
#define MD_FMT_CODE   (1u<<2)
#define MD_FMT_H1     (1u<<3)
#define MD_FMT_H2     (1u<<4)
#define MD_FMT_H3     (1u<<5)
#define MD_FMT_STRIKE (1u<<6)

typedef struct { size_t start; size_t len; MdFmt fmt; } MdSpan;

size_t markdown_spans(const char* text, size_t len, MdSpan* out, size_t out_cap);

#endif
