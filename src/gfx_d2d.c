/* Direct2D/DirectWrite chrome renderer. Pure-C use of the COM APIs:
 * COBJMACROS gives the ID2D1X_Method(This, ...) call forms, and INITGUID
 * (defined here, in exactly one TU) instantiates IID_ID2D1Factory /
 * IID_IDWriteFactory so they link. The hwnd render target is pinned to 96 dpi
 * so 1 DIP = 1 physical pixel; callers pass already-DPI-scaled pixel rects. */
#define COBJMACROS
#define INITGUID
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dxgiformat.h>
#include <string.h>
#include <stdlib.h>
#include "gfx_d2d.h"

static ID2D1Factory*   g_d2d = NULL;
static IDWriteFactory* g_dw  = NULL;

struct GfxD2D {
    HWND hwnd;
    int  dpi;
    ID2D1HwndRenderTarget* rt;
    ID2D1SolidColorBrush*  brush;
    IDWriteTextFormat*     fmt[GFX_FMT_COUNT];
};

int gfx_global_init(void) {
    if (!g_d2d) {
        D2D1_FACTORY_OPTIONS o; o.debugLevel = D2D1_DEBUG_LEVEL_NONE;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                     &IID_ID2D1Factory, &o, (void**)&g_d2d)))
            return 0;
    }
    if (!g_dw) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                       &IID_IDWriteFactory, (IUnknown**)&g_dw))) {
            if (g_d2d) { ID2D1Factory_Release(g_d2d); g_d2d = NULL; }
            return 0;
        }
    }
    return 1;
}

void gfx_global_shutdown(void) {
    if (g_dw)  { IDWriteFactory_Release(g_dw);  g_dw  = NULL; }
    if (g_d2d) { ID2D1Factory_Release(g_d2d);   g_d2d = NULL; }
}

static D2D1_COLOR_F col_f(unsigned c) {
    D2D1_COLOR_F f;
    f.r = GetRValue(c) / 255.0f;
    f.g = GetGValue(c) / 255.0f;
    f.b = GetBValue(c) / 255.0f;
    f.a = 1.0f;
    return f;
}

static void make_fmt(GfxD2D* g, int i, const wchar_t* face,
                     DWRITE_FONT_WEIGHT w, int base_px,
                     DWRITE_TEXT_ALIGNMENT ta, int ellipsis) {
    float size = (float)MulDiv(base_px, g->dpi, 96);
    IDWriteTextFormat* f = NULL;
    if (FAILED(IDWriteFactory_CreateTextFormat(g_dw, face, NULL, w,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size, L"", &f)) || !f) { g->fmt[i] = NULL; return; }
    IDWriteTextFormat_SetTextAlignment(f, ta);
    IDWriteTextFormat_SetParagraphAlignment(f, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    IDWriteTextFormat_SetWordWrapping(f, DWRITE_WORD_WRAPPING_NO_WRAP);
    if (ellipsis) {
        DWRITE_TRIMMING tr; memset(&tr, 0, sizeof tr);
        tr.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        IDWriteInlineObject* sign = NULL;
        IDWriteFactory_CreateEllipsisTrimmingSign(g_dw, f, &sign);
        IDWriteTextFormat_SetTrimming(f, &tr, sign);
        if (sign) IDWriteInlineObject_Release(sign);
    }
    g->fmt[i] = f;
}

static void make_all_formats(GfxD2D* g) {
    make_fmt(g, GFX_FMT_LABEL,  L"Segoe UI",           DWRITE_FONT_WEIGHT_SEMI_BOLD, 14, DWRITE_TEXT_ALIGNMENT_LEADING, 1);
    make_fmt(g, GFX_FMT_CLOSE,  L"Segoe UI",           DWRITE_FONT_WEIGHT_NORMAL,    11, DWRITE_TEXT_ALIGNMENT_CENTER,  0);
    make_fmt(g, GFX_FMT_PLUS,   L"Segoe UI",           DWRITE_FONT_WEIGHT_NORMAL,    15, DWRITE_TEXT_ALIGNMENT_CENTER,  0);
    make_fmt(g, GFX_FMT_WGLYPH, L"Segoe MDL2 Assets",  DWRITE_FONT_WEIGHT_NORMAL,    10, DWRITE_TEXT_ALIGNMENT_CENTER,  0);
    make_fmt(g, GFX_FMT_MARK,   L"Segoe UI",           DWRITE_FONT_WEIGHT_SEMI_BOLD, 15, DWRITE_TEXT_ALIGNMENT_CENTER,  0);
}

static void free_formats(GfxD2D* g) {
    for (int i = 0; i < GFX_FMT_COUNT; i++)
        if (g->fmt[i]) { IDWriteTextFormat_Release(g->fmt[i]); g->fmt[i] = NULL; }
}

GfxD2D* gfx_create(HWND hwnd, int dpi) {
    if (!g_d2d || !g_dw) return NULL;
    GfxD2D* g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->hwnd = hwnd;
    g->dpi  = dpi ? dpi : 96;

    RECT rc; GetClientRect(hwnd, &rc);
    D2D1_SIZE_U size;
    size.width  = (UINT32)(rc.right  - rc.left);
    size.height = (UINT32)(rc.bottom - rc.top);
    if (size.width  == 0) size.width  = 1;
    if (size.height == 0) size.height = 1;

    D2D1_RENDER_TARGET_PROPERTIES rtp; memset(&rtp, 0, sizeof rtp);
    rtp.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    rtp.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    rtp.dpiX = 96.0f;   /* pin: 1 DIP = 1 physical pixel; we scale ourselves */
    rtp.dpiY = 96.0f;
    rtp.usage    = D2D1_RENDER_TARGET_USAGE_NONE;
    rtp.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;

    D2D1_HWND_RENDER_TARGET_PROPERTIES hp; memset(&hp, 0, sizeof hp);
    hp.hwnd = hwnd;
    hp.pixelSize = size;
    hp.presentOptions = D2D1_PRESENT_OPTIONS_NONE;

    if (FAILED(ID2D1Factory_CreateHwndRenderTarget(g_d2d, &rtp, &hp, &g->rt)) || !g->rt) {
        free(g); return NULL;
    }
    D2D1_COLOR_F black = { 0, 0, 0, 1 };
    if (FAILED(ID2D1RenderTarget_CreateSolidColorBrush(
            (ID2D1RenderTarget*)g->rt, &black, NULL, &g->brush)) || !g->brush) {
        ID2D1HwndRenderTarget_Release(g->rt);
        free(g);
        return NULL;
    }
    make_all_formats(g);
    return g;
}

void gfx_destroy(GfxD2D* g) {
    if (!g) return;
    free_formats(g);
    if (g->brush) ID2D1SolidColorBrush_Release(g->brush);
    if (g->rt)    ID2D1HwndRenderTarget_Release(g->rt);
    free(g);
}

void gfx_resize(GfxD2D* g, int w, int h) {
    if (!g || !g->rt) return;
    D2D1_SIZE_U s; s.width = (UINT32)(w > 0 ? w : 1); s.height = (UINT32)(h > 0 ? h : 1);
    ID2D1HwndRenderTarget_Resize(g->rt, &s);
}

void gfx_set_dpi(GfxD2D* g, int dpi) {
    if (!g) return;
    g->dpi = dpi ? dpi : 96;
    free_formats(g);
    make_all_formats(g);
}

int gfx_begin(GfxD2D* g, unsigned bg) {
    if (!g || !g->rt) return 0;
    ID2D1RenderTarget* rt = (ID2D1RenderTarget*)g->rt;
    ID2D1RenderTarget_BeginDraw(rt);
    D2D1_COLOR_F c = col_f(bg);
    ID2D1RenderTarget_Clear(rt, &c);
    return 1;
}

int gfx_end(GfxD2D* g) {
    if (!g || !g->rt) return 0;
    HRESULT hr = ID2D1RenderTarget_EndDraw((ID2D1RenderTarget*)g->rt, NULL, NULL);
    return (hr == (HRESULT)D2DERR_RECREATE_TARGET) ? 1 : 0;
}

static void set_col(GfxD2D* g, unsigned col) {
    D2D1_COLOR_F c = col_f(col);
    ID2D1SolidColorBrush_SetColor(g->brush, &c);
}

void gfx_fill_rect(GfxD2D* g, RECT r, unsigned col) {
    if (!g || !g->rt) return;
    set_col(g, col);
    D2D1_RECT_F rf = { (float)r.left, (float)r.top, (float)r.right, (float)r.bottom };
    ID2D1RenderTarget_FillRectangle((ID2D1RenderTarget*)g->rt, &rf, (ID2D1Brush*)g->brush);
}

void gfx_fill_round(GfxD2D* g, RECT r, int radius, unsigned col) {
    if (!g || !g->rt) return;
    set_col(g, col);
    D2D1_ROUNDED_RECT rr;
    rr.rect.left = (float)r.left; rr.rect.top = (float)r.top;
    rr.rect.right = (float)r.right; rr.rect.bottom = (float)r.bottom;
    rr.radiusX = (float)radius; rr.radiusY = (float)radius;
    ID2D1RenderTarget_FillRoundedRectangle((ID2D1RenderTarget*)g->rt, &rr, (ID2D1Brush*)g->brush);
}

/* Tab shape: both top corners rounded, bottom square. Built as a closed path:
 * bottom-left -> up left edge -> arc top-left -> top edge -> arc top-right ->
 * down right edge -> (close back along the bottom). */
void gfx_fill_tab(GfxD2D* g, RECT r, int radius, unsigned col) {
    if (!g || !g->rt || !g_d2d) return;
    ID2D1PathGeometry* geo = NULL;
    if (FAILED(ID2D1Factory_CreatePathGeometry(g_d2d, &geo)) || !geo) return;
    ID2D1GeometrySink* sink = NULL;
    if (FAILED(ID2D1PathGeometry_Open(geo, &sink)) || !sink) {
        ID2D1PathGeometry_Release(geo); return;
    }
    float L = (float)r.left, T = (float)r.top, R = (float)r.right, B = (float)r.bottom;
    float rad = (float)radius;
    D2D1_POINT_2F p;
    p.x = L; p.y = B;      ID2D1GeometrySink_BeginFigure(sink, p, D2D1_FIGURE_BEGIN_FILLED);
    p.x = L; p.y = T + rad; ID2D1GeometrySink_AddLine(sink, p);

    D2D1_ARC_SEGMENT a;
    a.size.width = rad; a.size.height = rad;
    a.rotationAngle = 0.0f;
    a.sweepDirection = D2D1_SWEEP_DIRECTION_CLOCKWISE;
    a.arcSize = D2D1_ARC_SIZE_SMALL;

    a.point.x = L + rad; a.point.y = T; ID2D1GeometrySink_AddArc(sink, &a);
    p.x = R - rad; p.y = T; ID2D1GeometrySink_AddLine(sink, p);
    a.point.x = R; a.point.y = T + rad; ID2D1GeometrySink_AddArc(sink, &a);
    p.x = R; p.y = B; ID2D1GeometrySink_AddLine(sink, p);

    ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
    ID2D1GeometrySink_Close(sink);

    set_col(g, col);
    ID2D1RenderTarget_FillGeometry((ID2D1RenderTarget*)g->rt,
        (ID2D1Geometry*)geo, (ID2D1Brush*)g->brush, NULL);

    ID2D1GeometrySink_Release(sink);
    ID2D1PathGeometry_Release(geo);
}

void gfx_text(GfxD2D* g, const wchar_t* s, RECT r, int fmt, unsigned col) {
    if (!g || !g->rt || fmt < 0 || fmt >= GFX_FMT_COUNT || !g->fmt[fmt] || !s) return;
    set_col(g, col);
    D2D1_RECT_F rf = { (float)r.left, (float)r.top, (float)r.right, (float)r.bottom };
    ID2D1RenderTarget_DrawText((ID2D1RenderTarget*)g->rt, s, (UINT32)wcslen(s),
        g->fmt[fmt], &rf, (ID2D1Brush*)g->brush,
        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}
