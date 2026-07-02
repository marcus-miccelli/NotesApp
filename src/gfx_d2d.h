#ifndef GFX_D2D_H
#define GFX_D2D_H

#include <windows.h>

typedef struct GfxD2D GfxD2D;

#define GFX_FMT_LABEL  0
#define GFX_FMT_CLOSE  1
#define GFX_FMT_PLUS   2
#define GFX_FMT_WGLYPH 3
#define GFX_FMT_MARK   4
#define GFX_FMT_COUNT  5

int  gfx_global_init(void);
void gfx_global_shutdown(void);

GfxD2D* gfx_create(HWND hwnd, int dpi);
void    gfx_destroy(GfxD2D* g);
void    gfx_resize(GfxD2D* g, int w, int h);
void    gfx_set_dpi(GfxD2D* g, int dpi);

int  gfx_begin(GfxD2D* g, unsigned bg);   /* COLORREF bg; 1 => proceed */
int  gfx_end(GfxD2D* g);                  /* 1 => recreate target */

void gfx_fill_rect (GfxD2D* g, RECT r, unsigned col);
void gfx_fill_round(GfxD2D* g, RECT r, int radius, unsigned col);
void gfx_fill_tab  (GfxD2D* g, RECT r, int radius, unsigned col);
void gfx_text      (GfxD2D* g, const wchar_t* s, RECT r, int fmt, unsigned col);

#endif /* GFX_D2D_H */
