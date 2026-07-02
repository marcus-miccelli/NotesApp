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

typedef enum { DECO_FMT, DECO_HIDE, DECO_PARA } DecoKind;
typedef enum { PARA_NONE, PARA_BULLET, PARA_NUMBER } ParaKind;

typedef struct {
    DecoKind kind;
    size_t   start;   /* RichEdit char offset */
    size_t   len;
    MdFmt    fmt;     /* DECO_FMT */
    ParaKind para;    /* DECO_PARA */
    int      number;  /* DECO_PARA + PARA_NUMBER: ordinal */
} Deco;

/* Build the full-document decoration list. Markers hide everywhere except the
 * paragraph(s) intersecting [sel_lo, sel_hi]; pass sel_lo > sel_hi to hide all.
 * *out is malloc'd (free() it). Returns the count. Grows dynamically. */
size_t markdown_decorate(const char* text, size_t len,
                         size_t sel_lo, size_t sel_hi, Deco** out);

/* Inline format flags active at a caret offset (for sidebar toggles). */
MdFmt markdown_fmt_at(const char* text, size_t len, size_t caret);

#endif
