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
#define MD_FMT_CODEBLOCK (1u<<7)
#define MD_FMT_QUOTE (1u<<8)
#define MD_FMT_LINK (1u<<9)

typedef enum { DECO_FMT, DECO_HIDE, DECO_PARA, DECO_LINK, DECO_TASK } DecoKind;
typedef enum { PARA_NONE, PARA_BULLET, PARA_NUMBER, PARA_QUOTE } ParaKind;

typedef struct {
    DecoKind kind;
    size_t   start;   /* RichEdit char offset */
    size_t   len;
    MdFmt    fmt;     /* DECO_FMT */
    ParaKind para;    /* DECO_PARA */
    int      number;  /* DECO_PARA + PARA_NUMBER: ordinal */
    size_t   aux_start; /* DECO_LINK: url offset into the parse's url pool;
                         * DECO_HIDE (inline markers): span extent start;
                         * DECO_TASK: source offset of the mark char */
    size_t   aux_len;   /* DECO_LINK: url length in the pool;
                         * DECO_HIDE (inline markers): span extent length
                         * (0 on block-level hides) */
} Deco;

/* Build the full-document decoration list. Markers hide everywhere except the
 * paragraph(s) intersecting [sel_lo, sel_hi]; pass sel_lo > sel_hi to hide all.
 * *out is malloc'd (free() it). Returns the count. Grows dynamically. */
size_t markdown_decorate(const char* text, size_t len, size_t sel_lo,
                         size_t sel_hi, Deco** out, char** urlpool);

/* Inline format flags active at a caret offset (for sidebar toggles). */
MdFmt markdown_fmt_at(const char* text, size_t len, size_t caret);

/* If `off` is inside a task-list checkbox "[ ]"/"[x]", return 1 and set
 * *mark_off to the source offset of the mark char and *checked to its state;
 * else return 0. */
int markdown_task_at(const char* text, size_t len, size_t off,
                     size_t* mark_off, int* checked);

/* Pure queries over an existing decoration list (from markdown_decorate). */
MdFmt markdown_fmt_from_decos(const Deco* d, size_t n, size_t caret);
int   markdown_task_from_decos(const Deco* d, size_t n, size_t off,
                               size_t* mark_off, int* checked);

/* Signature of the set of inline span extents the reveal window [lo, hi]
 * touches (boundary-inclusive; caret = lo == hi). 0 when none. Computed
 * over a hide-all decoration list (extents ride on DECO_HIDE aux fields).
 * Equal windows-touch-sets give equal signatures. */
size_t markdown_reveal_sig(const Deco* d, size_t n, size_t lo, size_t hi);

#endif
