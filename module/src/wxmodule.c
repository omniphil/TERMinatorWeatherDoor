// The Weather TRACE module: the picture half of the door.
//
// The door fetches the forecast and sends it down as small structs (protocol.h);
// this module draws everything: an animated sky that matches the real conditions
// and the real sun and moon, the details, an hourly chart and a 7-day strip,
// plus a location picker. Mouse and keyboard both drive all of it.
//
// It lays out in a 640x480 logical space and renders at 1x or 2x (640x480 or
// 1280x960) depending on how big the picture is on screen, with fonts baked at
// both sizes, so text is always drawn at its real pixel size. TERMinator scales
// the frame with nearest-neighbour, which would smear anything scaled up.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "trace_api.h"
#include "protocol.h"
#include "fonts.h"
#include "wxcommon.h"

#define LW 640                  // logical layout size
#define LH 480
#define DEG "\x7f"              // the degree sign, character 127 in fonts.h
#define PI_F 3.14159265f

// ---------------------------------------------------------------- colours ---

#define C_BG0      0x090E1C
#define C_BG1      0x0E1530
#define C_BAR      0x0D1428
#define C_PANEL    0x131B35
#define C_PANEL_HI 0x1A2446
#define C_EDGE     0x243058
#define C_EDGE_HI  0x3A4C84
#define C_TEXT     0xEAF0FA
#define C_TEXT2    0x9AA6C6
#define C_TEXT3    0x5C688C
#define C_ACCENT   0x4FD1FF
#define C_MAGENTA  0xFF5FD2
#define C_RAIN     0x4C9AFF
#define C_SUN      0xFFC53D
#define C_WARN     0xFFB547
#define C_ERR      0xFF6B6B
#define C_GOOD     0x6EDC8C

// ------------------------------------------------------------------ frame ---

static uint32_t *g_frame = NULL;
static int g_S = 1, g_W = LW, g_H = LH;         // device scale and size
static int g_wantS = 1;                         // what the last resize asked for
static int g_clipX0, g_clipY0, g_clipX1, g_clipY1;

static inline int D(float v) { return (int)lroundf(v * (float)g_S); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smooth01(float t) { t = clampf(t, 0, 1); return t * t * (3 - 2 * t); }

static inline uint32_t mix(uint32_t a, uint32_t b, int t)
{
    if (t <= 0) return a & 0xFFFFFF;
    if (t >= 255) return b & 0xFFFFFF;
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return (uint32_t)((ar + ((br - ar) * t) / 255) << 16 | (ag + ((bg - ag) * t) / 255) << 8 |
                      (ab + ((bb - ab) * t) / 255));
}

static inline uint32_t mixf(uint32_t a, uint32_t b, float t) { return mix(a, b, (int)(clampf(t, 0, 1) * 255)); }

static void clip_all(void) { g_clipX0 = 0; g_clipY0 = 0; g_clipX1 = g_W; g_clipY1 = g_H; }

static void clip_l(float x, float y, float w, float h)
{
    g_clipX0 = D(x); g_clipY0 = D(y); g_clipX1 = D(x + w); g_clipY1 = D(y + h);
    if (g_clipX0 < 0) g_clipX0 = 0;
    if (g_clipY0 < 0) g_clipY0 = 0;
    if (g_clipX1 > g_W) g_clipX1 = g_W;
    if (g_clipY1 > g_H) g_clipY1 = g_H;
}

static inline void blendp(int x, int y, uint32_t rgb, int a)
{
    if (a <= 0 || x < g_clipX0 || y < g_clipY0 || x >= g_clipX1 || y >= g_clipY1) return;
    uint32_t *p = &g_frame[y * g_W + x];
    *p = (a >= 255 ? rgb : mix(*p, rgb, a)) | 0xFF000000u;
}

static void fill_d(int x0, int y0, int x1, int y1, uint32_t rgb, int a)
{
    if (x0 < g_clipX0) x0 = g_clipX0;
    if (y0 < g_clipY0) y0 = g_clipY0;
    if (x1 > g_clipX1) x1 = g_clipX1;
    if (y1 > g_clipY1) y1 = g_clipY1;
    if (a <= 0 || x0 >= x1 || y0 >= y1) return;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = g_frame + y * g_W;
        if (a >= 255) for (int x = x0; x < x1; x++) row[x] = rgb | 0xFF000000u;
        else          for (int x = x0; x < x1; x++) row[x] = mix(row[x], rgb, a) | 0xFF000000u;
    }
}

static void fill_l(float x, float y, float w, float h, uint32_t rgb, int a)
{
    fill_d(D(x), D(y), D(x + w), D(y + h), rgb, a);
}

// Vertical gradient; alpha may fade too (a0 at the top, a1 at the bottom).
static void vgrad_l(float x, float y, float w, float h, uint32_t top, uint32_t bot, int a0, int a1)
{
    int x0 = D(x), y0 = D(y), x1 = D(x + w), y1 = D(y + h);
    int n = y1 - y0;
    for (int yy = y0; yy < y1; yy++) {
        float t = n > 1 ? (float)(yy - y0) / (float)(n - 1) : 0;
        fill_d(x0, yy, x1, yy + 1, mixf(top, bot, t), (int)lerpf((float)a0, (float)a1, t));
    }
}

// Coverage of a rounded box (device units) at a pixel centre, from its signed distance.
static inline float rbox_cov(float px, float py, float cx, float cy, float hw, float hh, float r)
{
    float qx = fabsf(px - cx) - (hw - r), qy = fabsf(py - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
    return clampf(0.5f - d, 0, 1);
}

static void rrect_l(float x, float y, float w, float h, float r, uint32_t rgb, int a)
{
    int X0 = D(x), Y0 = D(y), X1 = D(x + w), Y1 = D(y + h);
    float R = r * g_S;
    int band = (int)ceilf(R);
    float cx = (X0 + X1) * 0.5f, cy = (Y0 + Y1) * 0.5f, hw = (X1 - X0) * 0.5f, hh = (Y1 - Y0) * 0.5f;
    for (int yy = Y0; yy < Y1; yy++) {
        if (yy >= Y0 + band && yy < Y1 - band) { fill_d(X0, yy, X1, yy + 1, rgb, a); continue; }
        fill_d(X0 + band, yy, X1 - band, yy + 1, rgb, a);
        for (int xx = X0; xx < X0 + band; xx++)
            blendp(xx, yy, rgb, (int)(a * rbox_cov(xx + 0.5f, yy + 0.5f, cx, cy, hw, hh, R)));
        for (int xx = X1 - band; xx < X1; xx++)
            blendp(xx, yy, rgb, (int)(a * rbox_cov(xx + 0.5f, yy + 0.5f, cx, cy, hw, hh, R)));
    }
}

static void rrect_stroke_l(float x, float y, float w, float h, float r, float width, uint32_t rgb, int a)
{
    int X0 = D(x), Y0 = D(y), X1 = D(x + w), Y1 = D(y + h);
    float R = r * g_S, W = width * g_S;
    float cx = (X0 + X1) * 0.5f, cy = (Y0 + Y1) * 0.5f, hw = (X1 - X0) * 0.5f, hh = (Y1 - Y0) * 0.5f;
    float Ri = R - W > 0 ? R - W : 0;
    int band = (int)ceilf(R > W ? R : W) + 1;
    for (int yy = Y0; yy < Y1; yy++) {
        int mid = yy >= Y0 + band && yy < Y1 - band;
        for (int xx = X0; xx < X1; xx++) {
            if (mid && xx == X0 + band) xx = X1 - band;
            float px = xx + 0.5f, py = yy + 0.5f;
            float c = rbox_cov(px, py, cx, cy, hw, hh, R) - rbox_cov(px, py, cx, cy, hw - W, hh - W, Ri);
            if (c > 0) blendp(xx, yy, rgb, (int)(a * c));
        }
    }
}

static void circle_d(float cx, float cy, float r, uint32_t rgb, int a)
{
    int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1);
    int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
    if (x0 < g_clipX0) x0 = g_clipX0;
    if (y0 < g_clipY0) y0 = g_clipY0;
    if (x1 > g_clipX1) x1 = g_clipX1;
    if (y1 > g_clipY1) y1 = g_clipY1;
    float inner = r - 0.7f > 0 ? (r - 0.7f) * (r - 0.7f) : 0;
    for (int y = y0; y < y1; y++) {
        float dy = y + 0.5f - cy;
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, d2 = dx * dx + dy * dy;
            if (d2 <= inner) { blendp(x, y, rgb, a); continue; }
            float c = r - sqrtf(d2) + 0.5f;
            if (c > 0) blendp(x, y, rgb, (int)(a * clampf(c, 0, 1)));
        }
    }
}

static void circle_l(float cx, float cy, float r, uint32_t rgb, int a)
{
    circle_d(cx * g_S, cy * g_S, r * g_S, rgb, a);
}

// A soft glow: alpha falls off with distance (quadratically) out to r.
static void glow_l(float cx, float cy, float r, uint32_t rgb, int a)
{
    float CX = cx * g_S, CY = cy * g_S, R = r * g_S;
    int x0 = (int)(CX - R), x1 = (int)(CX + R) + 1, y0 = (int)(CY - R), y1 = (int)(CY + R) + 1;
    if (x0 < g_clipX0) x0 = g_clipX0;
    if (y0 < g_clipY0) y0 = g_clipY0;
    if (x1 > g_clipX1) x1 = g_clipX1;
    if (y1 > g_clipY1) y1 = g_clipY1;
    float inv = 1.0f / (R * R);
    for (int y = y0; y < y1; y++) {
        float dy = y + 0.5f - CY;
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - CX, t = 1.0f - (dx * dx + dy * dy) * inv;
            if (t > 0) blendp(x, y, rgb, (int)(a * t * t));
        }
    }
}

static void ring_l(float cx, float cy, float r, float w, uint32_t rgb, int a)
{
    float CX = cx * g_S, CY = cy * g_S, R = r * g_S, hw = w * g_S * 0.5f;
    int x0 = (int)(CX - R - hw - 1), x1 = (int)(CX + R + hw + 2);
    int y0 = (int)(CY - R - hw - 1), y1 = (int)(CY + R + hw + 2);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - CX, dy = y + 0.5f - CY;
            float c = hw - fabsf(sqrtf(dx * dx + dy * dy) - R) + 0.5f;
            if (c > 0) blendp(x, y, rgb, (int)(a * clampf(c, 0, 1)));
        }
}

// An anti-aliased line with round caps (device units).
static void line_d(float x0, float y0, float x1, float y1, float w, uint32_t rgb, int a)
{
    float hw = w * 0.5f;
    int bx0 = (int)floorf(fminf(x0, x1) - hw - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + hw + 1);
    int by0 = (int)floorf(fminf(y0, y1) - hw - 1), by1 = (int)ceilf(fmaxf(y0, y1) + hw + 1);
    if (bx0 < g_clipX0) bx0 = g_clipX0;
    if (by0 < g_clipY0) by0 = g_clipY0;
    if (bx1 > g_clipX1) bx1 = g_clipX1;
    if (by1 > g_clipY1) by1 = g_clipY1;
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = len2 > 0 ? clampf((px * dx + py * dy) / len2, 0, 1) : 0;
            float ex = px - t * dx, ey = py - t * dy;
            float c = hw - sqrtf(ex * ex + ey * ey) + 0.5f;
            if (c > 0) blendp(x, y, rgb, (int)(a * clampf(c, 0, 1)));
        }
}

static void line_l(float x0, float y0, float x1, float y1, float w, uint32_t rgb, int a)
{
    line_d(x0 * g_S, y0 * g_S, x1 * g_S, y1 * g_S, w * g_S, rgb, a);
}

// A filled polygon, anti-aliased by 4x4 supersampling (small shapes only: the bolt).
static void poly_l(const float *pts, int n, uint32_t rgb, int a)
{
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) {
        minx = fminf(minx, pts[i * 2] * g_S); maxx = fmaxf(maxx, pts[i * 2] * g_S);
        miny = fminf(miny, pts[i * 2 + 1] * g_S); maxy = fmaxf(maxy, pts[i * 2 + 1] * g_S);
    }
    for (int y = (int)miny; y <= (int)maxy; y++)
        for (int x = (int)minx; x <= (int)maxx; x++) {
            int hits = 0;
            for (int s = 0; s < 16; s++) {
                float px = x + (s % 4 + 0.5f) / 4, py = y + (s / 4 + 0.5f) / 4;
                int in = 0;
                for (int i = 0, j = n - 1; i < n; j = i++) {
                    float xi = pts[i * 2] * g_S, yi = pts[i * 2 + 1] * g_S;
                    float xj = pts[j * 2] * g_S, yj = pts[j * 2 + 1] * g_S;
                    if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) in = !in;
                }
                hits += in;
            }
            if (hits) blendp(x, y, rgb, a * hits / 16);
        }
}

// ------------------------------------------------------------------- text ---

static const FontFace *face(int f) { return &g_fonts[g_S - 1][f]; }

static const FontGlyph *glyph(const FontFace *F, unsigned char ch)
{
    if (ch < F->first || ch >= F->first + F->count) return NULL;
    return &F->g[ch - F->first];
}

// Width in logical pixels; sp is extra letter spacing (logical).
static float text_w(int f, const char *s, float sp)
{
    const FontFace *F = face(f);
    int w = 0, spd = (int)lroundf(sp * g_S), n = 0;
    for (; *s; s++, n++) {
        const FontGlyph *g = glyph(F, (unsigned char)*s);
        if (g) w += g->adv;
        if (s[1]) w += spd;
    }
    (void)n;
    return (float)w / (float)g_S;
}

// Draws with the top of the line at y; returns the pen position after it.
static float text_sp(int f, float x, float y, const char *s, uint32_t rgb, int a, float sp)
{
    const FontFace *F = face(f);
    int pen = D(x), top = D(y), spd = (int)lroundf(sp * g_S);
    for (; *s; s++) {
        const FontGlyph *g = glyph(F, (unsigned char)*s);
        if (!g) continue;
        const uint8_t *src = F->alpha + g->off;
        int gx = pen + g->x, gy = top + g->y;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                int c = src[j * g->w + i];
                if (c) blendp(gx + i, gy + j, rgb, c * a / 255);
            }
        pen += g->adv + spd;
    }
    return (float)pen / (float)g_S;
}

static float text(int f, float x, float y, const char *s, uint32_t rgb)
{
    return text_sp(f, x, y, s, rgb, 255, 0);
}

static void text_c(int f, float cx, float y, const char *s, uint32_t rgb)
{
    text(f, cx - text_w(f, s, 0) / 2, y, s, rgb);
}

static void text_r(int f, float rx, float y, const char *s, uint32_t rgb)
{
    text(f, rx - text_w(f, s, 0), y, s, rgb);
}

// Letter-spaced capitals, for the small labels.
static float label(float x, float y, const char *s, uint32_t rgb)
{
    return text_sp(F_SMALL, x, y, s, rgb, 255, 0.8f);
}

static void label_c(float cx, float y, const char *s, uint32_t rgb)
{
    label(cx - text_w(F_SMALL, s, 0.8f) / 2, y, s, rgb);
}

// Text with a soft drop shadow, for writing over the sky.
static float text_shadow(int f, float x, float y, const char *s, uint32_t rgb)
{
    float o = g_S == 1 ? 1.0f : 0.75f;
    text_sp(f, x + o, y + o, s, 0x000000, 110, 0);
    text_sp(f, x + o * 2, y + o * 2, s, 0x000000, 40, 0);
    return text(f, x, y, s, rgb);
}

// Copies s into out, shortened with "..." to fit maxw.
static void fit(int f, const char *s, float maxw, char *out, size_t size)
{
    snprintf(out, size, "%s", s);
    if (text_w(f, out, 0) <= maxw) return;
    size_t n = strlen(out);
    while (n > 0) {
        out[--n] = 0;
        char tmp[128];
        snprintf(tmp, sizeof tmp, "%.100s...", out);
        if (text_w(f, tmp, 0) <= maxw) { memcpy(out, tmp, strlen(tmp) + 1 < size ? strlen(tmp) + 1 : size); out[size - 1] = 0; return; }
    }
}

// ------------------------------------------------------------ presenting ---
//
// TERMinator fits the picture into its window at 4:3 and scales it with
// nearest-neighbour. Shrinking that way drops whole rows and columns, so thin
// strokes vanish here and there and text looks nicked. So the module works out
// the exact size the picture will be shown at (trace_on_resize gives the
// window's device pixels), renders at 2x, and hands over a frame of exactly that
// size, shrunk with an area-averaging filter. Bigger than the 2x frame, the 2x
// frame goes as it is: nearest-neighbour only ever duplicates pixels then.

static int g_dispW = LW, g_dispH = LH;     // the picture on screen, device pixels
static int g_presW = LW, g_presH = LH;     // the frame last presented (mouse positions are in this)

// Call from trace_on_resize: the largest 4:3 rectangle in the window.
static void set_display(int32_t w, int32_t h)
{
    if (w <= 0 || h <= 0) return;
    if ((int64_t)w * 3 > (int64_t)h * 4) { g_dispH = h; g_dispW = h * 4 / 3; }
    else { g_dispW = w; g_dispH = w * 3 / 4; }
    g_wantS = g_dispW > 700 ? 2 : 1;
}

// One axis of the area filter: for each output pixel, the source pixels it
// covers and how much of each (weights in 1/256, summing to 256).
typedef struct { int first, count; uint16_t w[4]; } Span;

static Span *spans(int src, int dst)
{
    Span *s = malloc(sizeof(Span) * (size_t)dst);
    if (!s) return NULL;
    double scale = (double)src / dst;
    for (int i = 0; i < dst; i++) {
        double a = i * scale, b = (i + 1) * scale;
        int first = (int)a, last = (int)ceil(b) - 1;
        if (last >= src) last = src - 1;
        if (last - first + 1 > 4) last = first + 3;
        s[i].first = first;
        s[i].count = last - first + 1;
        int total = 0;
        for (int k = 0; k < s[i].count; k++) {
            double lo = first + k > a ? first + k : a, hi = first + k + 1 < b ? first + k + 1 : b;
            int wgt = (int)lround((hi - lo) / scale * 256);
            if (wgt < 0) wgt = 0;
            s[i].w[k] = (uint16_t)wgt;
            total += wgt;
        }
        s[i].w[0] = (uint16_t)(s[i].w[0] + 256 - total);   // rounding goes to the first
    }
    return s;
}

static void present_fitted(int32_t flags)
{
    if (g_dispW >= g_W || g_dispW < 64) {
        g_presW = g_W;
        g_presH = g_H;
        trace_present(g_frame, g_W, g_H, flags);
        return;
    }
    static uint32_t *tmp = NULL, *out = NULL;
    static Span *sx = NULL, *sy = NULL;
    static int forW = 0, forH = 0, fromW = 0, fromH = 0;
    int dw = g_dispW, dh = g_dispH;
    if (dw != forW || dh != forH || g_W != fromW || g_H != fromH) {
        free(tmp); free(out); free(sx); free(sy);
        tmp = malloc((size_t)dw * g_H * 4);
        out = malloc((size_t)dw * dh * 4);
        sx = spans(g_W, dw);
        sy = spans(g_H, dh);
        forW = dw; forH = dh; fromW = g_W; fromH = g_H;
        if (!tmp || !out || !sx || !sy) {
            free(tmp); free(out); free(sx); free(sy);
            tmp = out = NULL; sx = sy = NULL; forW = forH = 0;
            trace_present(g_frame, g_W, g_H, flags);
            return;
        }
    }
    // Across, then down; red and blue ride together in one word, green alone.
    for (int y = 0; y < g_H; y++) {
        const uint32_t *row = g_frame + y * g_W;
        uint32_t *o = tmp + y * dw;
        for (int x = 0; x < dw; x++) {
            const Span *s = &sx[x];
            uint32_t rb = 0, g = 0;
            for (int k = 0; k < s->count; k++) {
                uint32_t p = row[s->first + k], w = s->w[k];
                rb += (p & 0x00FF00FFu) * w;
                g  += (p & 0x0000FF00u) * w;
            }
            o[x] = ((rb >> 8) & 0x00FF00FFu) | ((g >> 8) & 0x0000FF00u) | 0xFF000000u;
        }
    }
    for (int y = 0; y < dh; y++) {
        const Span *s = &sy[y];
        uint32_t *o = out + y * dw;
        for (int x = 0; x < dw; x++) {
            uint32_t rb = 0, g = 0;
            for (int k = 0; k < s->count; k++) {
                uint32_t p = tmp[(s->first + k) * dw + x], w = s->w[k];
                rb += (p & 0x00FF00FFu) * w;
                g  += (p & 0x0000FF00u) * w;
            }
            o[x] = ((rb >> 8) & 0x00FF00FFu) | ((g >> 8) & 0x0000FF00u) | 0xFF000000u;
        }
    }
    g_presW = dw;
    g_presH = dh;
    trace_present(out, dw, dh, flags);
}

// A mouse position (in the presented frame's pixels) in layout coordinates.
static float mouse_lx(int32_t x) { return (float)x * LW / (g_presW ? g_presW : LW); }
static float mouse_ly(int32_t y) { return (float)y * LH / (g_presH ? g_presH : LH); }

// ------------------------------------------------------------------- data ---

static WxCurrent g_cur;
static WxHourly  g_hr;
static WxDaily   g_dy;
static int       g_haveCur = 0, g_haveHr = 0, g_haveDy = 0;
static int32_t   g_recvMs = 0;                  // when g_cur.doorNow arrived
static int       g_units = WX_UNITS_IMPERIAL;

static WxPlace   g_places[WX_MAX_PLACES];
static int       g_placeCount = -1;             // -1 = no search answered yet

static char      g_status[128];
static int       g_statusKind = WX_STATUS_INFO;
static int32_t   g_statusAt = -100000;

static int g_quitting = 0;
static int g_started = 0;

static int have_data(void) { return g_haveCur && g_haveHr && g_haveDy; }

static int64_t now_utc(void)
{
    if (!g_haveCur) return 0;
    return (int64_t)g_cur.doorNow + (trace_time_ms() - g_recvMs) / 1000;
}

static int64_t floordiv(int64_t a, int64_t b) { int64_t q = a / b; return (a % b < 0) ? q - 1 : q; }

typedef struct { int year, mon, mday, wday, hour, min; } LocalTime;

static LocalTime local_time(int64_t utc)
{
    LocalTime lt;
    int64_t t = utc + g_cur.utcOffset;
    int64_t days = floordiv(t, 86400), secs = t - days * 86400;
    lt.hour = (int)(secs / 3600);
    lt.min = (int)(secs % 3600 / 60);
    lt.wday = (int)(((days + 4) % 7 + 7) % 7);     // 1970-01-01 was a Thursday
    // Howard Hinnant's civil_from_days
    int64_t z = days + 719468, era = floordiv(z, 146097);
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    lt.mday = (int)(doy - (153 * mp + 2) / 5 + 1);
    lt.mon = (int)(mp < 10 ? mp + 3 : mp - 9);
    lt.year = (int)(yoe + era * 400 + (lt.mon <= 2));
    return lt;
}

static const char *const WDAY[7]  = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const WDAY3[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char *const MON3[13] = { "", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// "3:42 PM" (imperial) or "15:42" (metric); withMin 0 gives "3 PM" / "15:00".
static void fmt_clock(char *out, size_t n, int64_t utc, int withMin)
{
    LocalTime lt = local_time(utc);
    if (g_units == WX_UNITS_METRIC) { snprintf(out, n, "%02d:%02d", lt.hour, withMin ? lt.min : 0); return; }
    int h = lt.hour % 12 ? lt.hour % 12 : 12;
    const char *ap = lt.hour < 12 ? "AM" : "PM";
    if (withMin) snprintf(out, n, "%d:%02d %s", h, lt.min, ap);
    else         snprintf(out, n, "%d %s", h, ap);
}

// ------------------------------------------------------------------ units ---

static int temp_i(int16_t c10)
{
    float c = c10 / 10.0f;
    return (int)lroundf(g_units == WX_UNITS_IMPERIAL ? c * 9 / 5 + 32 : c);
}

static void fmt_temp(char *out, size_t n, int16_t c10)
{
    if (c10 == WX_NONE) snprintf(out, n, "--");
    else snprintf(out, n, "%d" DEG, temp_i(c10));
}

static void fmt_wind(char *out, size_t n, uint16_t kmh10)
{
    float k = kmh10 / 10.0f;
    if (g_units == WX_UNITS_IMPERIAL) snprintf(out, n, "%d mph", (int)lroundf(k * 0.621371f));
    else snprintf(out, n, "%d km/h", (int)lroundf(k));
}

static int wind_num(uint16_t kmh10)
{
    float k = kmh10 / 10.0f;
    return (int)lroundf(g_units == WX_UNITS_IMPERIAL ? k * 0.621371f : k);
}

static void fmt_pressure(char *out, size_t n, uint16_t hpa10)
{
    float h = hpa10 / 10.0f;
    if (g_units == WX_UNITS_IMPERIAL) snprintf(out, n, "%.2f in", h * 0.02953f);
    else snprintf(out, n, "%d hPa", (int)lroundf(h));
}

static void fmt_vis(char *out, size_t n, uint16_t m)
{
    float v = g_units == WX_UNITS_IMPERIAL ? m / 1609.34f : m / 1000.0f;
    const char *u = g_units == WX_UNITS_IMPERIAL ? "mi" : "km";
    if (v >= 9.95f) snprintf(out, n, "%d %s", (int)lroundf(v), u);
    else snprintf(out, n, "%.1f %s", v, u);
}

// A colour for a temperature (deg C), cold purple through to hot red.
static uint32_t temp_color(float c)
{
    static const struct { float t; uint32_t c; } S[] = {
        { -15, 0xA78BFA }, { -5, 0x7C9CFF }, { 3, 0x56B4FF }, { 10, 0x45D6C8 },
        { 17, 0x7ED957 }, { 23, 0xFFD447 }, { 29, 0xFF9A3D }, { 36, 0xFF5A4E }, { 45, 0xE0335E },
    };
    const int n = (int)(sizeof S / sizeof S[0]);
    if (c <= S[0].t) return S[0].c;
    for (int i = 1; i < n; i++)
        if (c <= S[i].t) return mixf(S[i - 1].c, S[i].c, (c - S[i - 1].t) / (S[i].t - S[i - 1].t));
    return S[n - 1].c;
}

// ------------------------------------------------------------------ icons ---

// Every icon is alive: rays turn, clouds bob, rain and snow fall, the bolt
// flickers, fog slides and the moon glows. Each icon gets its own phase from
// where it sits, so a row of them never moves in lockstep.
static float icon_time(void) { return trace_time_ms() / 1000.0f; }

static void icon_sun(float cx, float cy, float s, int a, float ph)
{
    float t = icon_time() + ph;
    float r = s * 0.2f;
    float rot = t * 0.45f;                              // the rays turn slowly
    float pulse = 1.0f + 0.12f * sinf(t * 2.2f);        // and breathe in and out
    glow_l(cx, cy, r * 2.6f, C_SUN, (int)(a * 0.22f));
    for (int i = 0; i < 8; i++) {
        float ang = i * PI_F / 4 + rot;
        float c = cosf(ang), sn = sinf(ang);
        line_l(cx + c * r * 1.5f, cy + sn * r * 1.5f, cx + c * r * 2.1f * pulse, cy + sn * r * 2.1f * pulse,
               s * 0.065f, C_SUN, a);
    }
    circle_l(cx, cy, r, C_SUN, a);
}

// A crescent: the disc minus an offset disc, per pixel, with a soft glow.
static void icon_moon(float cx, float cy, float s, int a, float ph)
{
    float t = icon_time() + ph;
    float r = s * 0.26f;
    glow_l(cx, cy, r * 2.4f, 0xBFD2FF, (int)(a * (0.16f + 0.1f * sinf(t * 1.6f))));
    float CX = cx * g_S, CY = cy * g_S, R = r * g_S;
    float ox = CX + R * 0.55f, oy = CY - R * 0.35f, R2 = R * 0.92f;
    for (int y = (int)(CY - R - 1); y <= (int)(CY + R + 1); y++)
        for (int x = (int)(CX - R - 1); x <= (int)(CX + R + 1); x++) {
            float dx = x + 0.5f - CX, dy = y + 0.5f - CY;
            float c1 = clampf(R - sqrtf(dx * dx + dy * dy) + 0.5f, 0, 1);
            float ex = x + 0.5f - ox, ey = y + 0.5f - oy;
            float c2 = clampf(R2 - sqrtf(ex * ex + ey * ey) + 0.5f, 0, 1);
            float c = c1 * (1 - c2);
            if (c > 0) blendp(x, y, 0xF2E9C9, (int)(a * c));
        }
}

static void icon_cloud(float cx, float cy, float s, uint32_t col, int a)
{
    circle_l(cx - s * 0.17f, cy + s * 0.02f, s * 0.17f, col, a);
    circle_l(cx + s * 0.05f, cy - s * 0.08f, s * 0.23f, col, a);
    circle_l(cx + s * 0.24f, cy + s * 0.05f, s * 0.14f, col, a);
    rrect_l(cx - s * 0.34f, cy + s * 0.02f, s * 0.72f, s * 0.17f, s * 0.085f, col, a);
}

// A cloud that bobs gently side to side.
static void icon_cloud_bob(float cx, float cy, float s, uint32_t col, int a, float ph)
{
    float t = icon_time() + ph;
    icon_cloud(cx + sinf(t * 0.9f) * s * 0.04f, cy + sinf(t * 1.3f) * s * 0.012f, s, col, a);
}

static void icon_bolt(float cx, float cy, float s, int a)
{
    float p[] = { cx + s * 0.04f, cy - s * 0.02f, cx - s * 0.13f, cy + s * 0.26f, cx + s * 0.0f, cy + s * 0.26f,
                  cx - s * 0.07f, cy + s * 0.48f, cx + s * 0.16f, cy + s * 0.16f, cx + s * 0.02f, cy + s * 0.16f,
                  cx + s * 0.12f, cy - s * 0.02f };
    poly_l(p, 7, 0xFFD84D, a);
}

static void icon(int code, int day, float cx, float cy, float s)
{
    Kind k = kind_of(code);
    uint32_t light = 0xE8EDF5, mid = 0xB9C3D3, dark = 0x7D889C;
    float ph = cx * 0.037f + cy * 0.013f;               // this icon's own phase
    float t = icon_time() + ph;
    switch (k) {
    case K_CLEAR:
        if (day) icon_sun(cx, cy, s, 255, ph); else icon_moon(cx, cy, s, 255, ph);
        break;
    case K_MOSTLY:
    case K_PARTLY:
        if (day) icon_sun(cx - s * 0.14f, cy - s * 0.12f, s * 0.85f, 255, ph);
        else     icon_moon(cx - s * 0.12f, cy - s * 0.12f, s * 0.9f, 255, ph);
        icon_cloud_bob(cx + s * 0.06f, cy + s * 0.1f, s * (k == K_MOSTLY ? 0.62f : 0.8f), light, 255, ph);
        break;
    case K_CLOUDY:
        icon_cloud_bob(cx - s * 0.1f, cy - s * 0.08f, s * 0.7f, dark, 255, ph + 1.7f);
        icon_cloud_bob(cx + s * 0.04f, cy + s * 0.04f, s * 0.9f, mid, 255, ph);
        break;
    case K_FOG:
        icon_cloud_bob(cx, cy - s * 0.14f, s * 0.8f, mid, 255, ph);
        for (int i = 0; i < 3; i++) {
            float dx = sinf(t * 0.8f + i * 1.9f) * s * 0.07f;      // the bands slide
            line_l(cx - s * 0.32f + i * s * 0.04f + dx, cy + s * (0.14f + i * 0.11f),
                   cx + s * 0.3f - i * s * 0.06f + dx, cy + s * (0.14f + i * 0.11f), s * 0.06f, 0xA6B0C0, 230);
        }
        break;
    case K_DRIZZLE:
    case K_RAIN:
    case K_HEAVY:
    case K_FREEZE:
    case K_THUNDER: {
        // Drops fall from under the cloud and fade out, each on its own beat.
        int drops = k == K_HEAVY || k == K_THUNDER ? 4 : 3;
        float speed = k == K_DRIZZLE ? 0.9f : k == K_HEAVY ? 2.0f : 1.4f;
        float len = k == K_DRIZZLE ? 0.05f : 0.12f;
        for (int i = 0; i < drops; i++) {
            float p = fmodf(t * speed + i * 0.37f, 1.0f);
            float x = cx - s * 0.2f + i * s * (0.4f / (drops - 1)) - p * s * 0.06f;
            float y = cy + s * (0.14f + p * 0.3f);
            uint32_t col = k == K_FREEZE && i % 2 ? 0xE8F4FF : 0x5AA9FF;
            int a = (int)(255 * (1 - p) * clampf(p * 6, 0, 1));
            line_l(x, y, x - s * 0.04f, y + s * len, s * 0.06f, col, a);
        }
        if (k == K_THUNDER) {
            icon_cloud_bob(cx, cy - s * 0.16f, s * 0.9f, dark, 255, ph);
            // The bolt flickers: two quick flashes every few seconds, dim between.
            float b = fmodf(t, 3.2f);
            int bright = (b < 0.12f) || (b > 0.24f && b < 0.34f);
            icon_bolt(cx, cy, s, bright ? 255 : 150);
            if (bright) glow_l(cx, cy + s * 0.2f, s * 0.5f, 0xFFF3B0, 70);
        } else {
            icon_cloud_bob(cx, cy - s * 0.14f, s * 0.9f, k == K_HEAVY ? dark : mid, 255, ph);
        }
        break;
    }
    case K_SNOW:
    case K_HEAVYSNOW: {
        int n = k == K_HEAVYSNOW ? 4 : 3;
        for (int i = 0; i < n; i++) {
            float p = fmodf(t * 0.45f + i * 0.31f, 1.0f);
            float x = cx - s * 0.2f + i * s * (0.4f / (n - 1)) + sinf(t * 2 + i) * s * 0.03f;
            float y = cy + s * (0.16f + p * 0.3f);
            float r = s * 0.06f;
            int a = (int)(255 * (1 - p * p) * clampf(p * 6, 0, 1));
            float spin = t * 1.5f + i;
            for (int j = 0; j < 3; j++) {
                float ang = j * PI_F / 3 + spin;
                line_l(x - cosf(ang) * r, y - sinf(ang) * r, x + cosf(ang) * r, y + sinf(ang) * r,
                       s * 0.035f, 0xF4F8FF, a);
            }
        }
        icon_cloud_bob(cx, cy - s * 0.14f, s * 0.9f, mid, 255, ph);
        break;
    }
    }
}

// A small raindrop, for "chance of rain".
static void droplet(float cx, float cy, float s, uint32_t col, int a)
{
    circle_l(cx, cy + s * 0.15f, s * 0.32f, col, a);
    float p[] = { cx - s * 0.28f, cy + s * 0.05f, cx, cy - s * 0.45f, cx + s * 0.28f, cy + s * 0.05f };
    poly_l(p, 3, col, a);
}

static void pin_icon(float cx, float cy, float s, uint32_t col)
{
    circle_l(cx, cy - s * 0.12f, s * 0.3f, col, 255);
    float p[] = { cx - s * 0.26f, cy - s * 0.02f, cx, cy + s * 0.48f, cx + s * 0.26f, cy - s * 0.02f };
    poly_l(p, 3, col, 255);
    circle_l(cx, cy - s * 0.12f, s * 0.11f, C_BAR, 255);
}

static void refresh_icon(float cx, float cy, float s, uint32_t col, float spin)
{
    float r = s * 0.3f;
    int steps = 20;
    float a0 = spin + 0.5f, a1 = spin + 2 * PI_F - 0.3f;
    for (int i = 0; i < steps; i++) {
        float t0 = a0 + (a1 - a0) * i / steps, t1 = a0 + (a1 - a0) * (i + 1) / steps;
        line_l(cx + cosf(t0) * r, cy + sinf(t0) * r, cx + cosf(t1) * r, cy + sinf(t1) * r, s * 0.09f, col, 255);
    }
    float hx = cx + cosf(a1) * r, hy = cy + sinf(a1) * r;
    float tx = -sinf(a1), ty = cosf(a1);           // tangent, direction of travel
    float p[] = { hx + tx * s * 0.16f, hy + ty * s * 0.16f,
                  hx - cosf(a1) * s * 0.14f, hy - sinf(a1) * s * 0.14f,
                  hx + cosf(a1) * s * 0.14f, hy + sinf(a1) * s * 0.14f };
    poly_l(p, 3, col, 255);
}

static void close_icon(float cx, float cy, float s, uint32_t col)
{
    float r = s * 0.22f;
    line_l(cx - r, cy - r, cx + r, cy + r, s * 0.09f, col, 255);
    line_l(cx - r, cy + r, cx + r, cy - r, s * 0.09f, col, 255);
}

static void spinner(float cx, float cy, float r, uint32_t col)
{
    float t = trace_time_ms() / 1000.0f;
    for (int i = 0; i < 8; i++) {
        float ang = i * PI_F / 4 + t * 5;
        int a = 60 + (int)(195 * (i / 7.0f));
        circle_l(cx + cosf(ang) * r, cy + sinf(ang) * r, r * 0.22f, col, a);
    }
}

// A key cap ("Esc", "L"), for the hint lines.
static float keycap(float x, float y, const char *k)
{
    float w = text_w(F_SMALL, k, 0) + 8;
    if (w < 14) w = 14;
    rrect_l(x, y, w, 13, 3, C_PANEL_HI, 255);
    rrect_stroke_l(x, y, w, 13, 3, 1, C_EDGE_HI, 255);
    text_c(F_SMALL, x + w / 2, y + 1, k, C_TEXT2);
    return x + w;
}

static float hint(float x, float y, const char *k, const char *what)
{
    x = keycap(x, y, k) + 4;
    return text(F_SMALL, x, y + 1, what, C_TEXT3) + 12;
}

// ---------------------------------------------------------------- layout ---

typedef struct { float x, y, w, h; } Rect;

#define BAR_H 28
static const Rect R_HERO    = { 8, 34, 404, 200 };
static const Rect R_DETAILS = { 418, 34, 214, 200 };
static const Rect R_CHART   = { 8, 242, 624, 118 };
static const Rect R_DAYS    = { 8, 368, 624, 88 };
#define FOOT_Y 463
#define CARD_W 84
#define CARD_GAP 6
static const Rect R_CLOSE   = { 608, 4, 24, 20 };
static const Rect R_REFRESH = { 580, 4, 24, 20 };
static const Rect R_UNITS   = { 516, 5, 58, 18 };
static const Rect R_PICKER  = { 130, 72, 380, 330 };
#define PICK_FIELD_Y (R_PICKER.y + 62)
#define PICK_ROWS_Y  (R_PICKER.y + 108)
#define PICK_ROW_H   26
#define PICK_VISIBLE 6

static int inside(const Rect *r, float x, float y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

static Rect g_placeBtn = { 170, 4, 200, 20 };      // sized to the name each frame

// ------------------------------------------------------------------- UI state ---

static float g_mx = -1, g_my = -1;              // mouse, logical
static int   g_mouseOver = 0, g_haveMouse = 0;
static int   g_selDay = 0;
static int   g_chartStart = 0;                  // first hour index shown in the chart
static int   g_followNow = 1;                   // the chart tracks "now" until the player moves it
static int   g_hover = -1;                      // hovered hour within the window, -1 = none
static int   g_hoverFromKey = 0;
static int   g_picker = 0;
static char  g_query[WX_SEARCH_LEN + 1];
static int   g_qlen = 0;
static char  g_lastSearch[WX_SEARCH_LEN + 1];
static int   g_pickSel = 0;
static int   g_searching = 0;
static int   g_firstRun = 0;
static int32_t g_refreshSpinUntil = 0;
static int32_t g_lastFrameMs = 0;

#define CHART_HOURS 24

static int hour_count(void) { return g_haveHr ? g_hr.count : 0; }

static int now_hour_index(void)
{
    if (!g_haveHr) return 0;
    int64_t i = floordiv(now_utc() - (int64_t)g_hr.start, 3600);
    if (i < 0) i = 0;
    if (i >= g_hr.count) i = g_hr.count - 1;
    return (int)i;
}

static int max_start(void) { int m = hour_count() - CHART_HOURS; return m > 0 ? m : 0; }

static void set_chart_start(int s)
{
    if (s < 0) s = 0;
    if (s > max_start()) s = max_start();
    g_chartStart = s;
    // The day card that owns most of the window is the "selected" day.
    int d = (s + CHART_HOURS / 2) / 24;
    if (d >= WX_DAYS) d = WX_DAYS - 1;
    g_selDay = d;
}

static void select_day(int d)
{
    if (d < 0) d = 0;
    if (g_haveDy && d >= g_dy.count) d = g_dy.count - 1;
    g_followNow = d == 0;
    set_chart_start(d == 0 ? now_hour_index() : d * 24);
    g_selDay = d;
    if (g_hoverFromKey) g_hover = 0;
}

// ---------------------------------------------------------------- door link ---

static void send_msg(uint8_t type, const void *payload, uint32_t len)
{
    uint8_t buf[sizeof(WxMsgHeader) + 64];
    if (len > 64) return;
    WxMsgHeader h = { type, 0, 1, len };
    memcpy(buf, &h, sizeof h);
    if (payload && len) memcpy(buf + sizeof h, payload, len);
    uint32_t total = sizeof h + len;
    if ((uint32_t)trace_send_room() >= total) trace_send(buf, (int32_t)total);
}

static void set_status(int kind, const char *s)
{
    snprintf(g_status, sizeof g_status, "%s", s);
    g_statusKind = kind;
    g_statusAt = trace_time_ms();
}

static void do_search(void)
{
    if (g_qlen == 0) return;
    uint8_t buf[1 + WX_SEARCH_LEN];
    buf[0] = (uint8_t)g_qlen;
    memcpy(buf + 1, g_query, (size_t)g_qlen);
    send_msg(WX_OUT_SEARCH, buf, 1 + (uint32_t)g_qlen);
    snprintf(g_lastSearch, sizeof g_lastSearch, "%s", g_query);
    g_searching = 1;
    g_placeCount = -1;
    g_pickSel = 0;
}

static void do_pick(int i)
{
    if (i < 0 || i >= g_placeCount) return;
    uint8_t b = (uint8_t)i;
    send_msg(WX_OUT_PICK, &b, 1);
    g_picker = 0;
    g_firstRun = 0;
    g_followNow = 1;
    set_status(WX_STATUS_BUSY, "Fetching the forecast...");
}

static void do_units(int u)
{
    if (u == g_units) return;
    g_units = u;
    uint8_t b = (uint8_t)u;
    send_msg(WX_OUT_UNITS, &b, 1);
}

static void do_refresh(void)
{
    send_msg(WX_OUT_REFRESH, NULL, 0);
    g_refreshSpinUntil = trace_time_ms() + 900;
    set_status(WX_STATUS_BUSY, "Refreshing...");
}

static void open_picker(void)
{
    g_picker = 1;
    g_qlen = 0;
    g_query[0] = 0;
    g_lastSearch[0] = 0;
    g_placeCount = -1;
    g_pickSel = 0;
    g_searching = 0;
}

// ------------------------------------------------------------------- sky ---

// Everything the sky shows follows the real conditions: the sun and moon ride
// arcs set by the day's actual sunrise and sunset, the moon shows its real
// phase, and the clouds, rain, snow, fog and lightning follow the weather code.

typedef struct { float x, y, v, len; } Drop;
typedef struct { float x, y, v, ph, r; } Flake;
typedef struct { float x, y, speed, scale; int sprite, layer; } Cloud;
typedef struct { float x, y, tw; } Star;
typedef struct { float x, w, h; int antenna; } Building;

#define MAX_DROPS 420
#define MAX_FLAKES 260
#define MAX_CLOUDS 12
#define N_STARS 90
#define MAX_BUILDINGS 40
#define N_SPRITES 4

static Drop     g_drops[MAX_DROPS];
static Flake    g_flakes[MAX_FLAKES];
static Cloud    g_clouds[MAX_CLOUDS];
static Star     g_stars[N_STARS];
static Building g_bld[MAX_BUILDINGS];
static int      g_nBld = 0;
static uint32_t g_rng = 0x12345678u;

static float frand(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return (float)(g_rng >> 8) / 16777216.0f;
}

// Cloud sprites: coverage and shade (top lit, underside darker), baked at the
// current scale, so drawing one is a straight per-pixel blend.
typedef struct { int w, h; uint8_t *cov, *shade; } Sprite;
#define SPRITE_W 170
#define SPRITE_H 90
#define SPRITE_TOP 26       // empty rows above where a puff can reach; drawn this much higher
static Sprite g_sprites[N_SPRITES];
static int    g_spriteScale = 0;

static void bake_sprites(void)
{
    for (int s = 0; s < N_SPRITES; s++) {
        Sprite *sp = &g_sprites[s];
        free(sp->cov); free(sp->shade);
        // Roomy enough that no puff reaches an edge: a puff that did had its
        // top sliced off flat.
        int W = SPRITE_W * g_S, H = SPRITE_H * g_S;
        sp->w = W; sp->h = H;
        sp->cov = calloc((size_t)W * H, 1);
        sp->shade = calloc((size_t)W * H, 1);
        if (!sp->cov || !sp->shade) continue;
        // A row of puffs, biggest in the middle, on a flat base.
        uint32_t seed = 0x9E3779B9u * (uint32_t)(s + 1);
        int n = 5 + s % 3;
        float px[8], py[8], pr[8];
        for (int i = 0; i < n; i++) {
            seed = seed * 1664525u + 1013904223u;
            float t = (i + 0.5f) / n;
            float bulge = sinf(t * PI_F);
            pr[i] = (10 + 12 * bulge + (seed >> 29)) * g_S;       // at most 29
            px[i] = (32 + t * 106) * g_S;                         // 32..138 of 170
            py[i] = H - 14.0f * g_S - pr[i] * 0.55f;
        }
        float base = H - 10.0f * g_S;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float fx = x + 0.5f, fy = y + 0.5f, c = 0;
                for (int i = 0; i < n; i++) {
                    float dx = fx - px[i], dy = fy - py[i];
                    float v = clampf((pr[i] - sqrtf(dx * dx + dy * dy)) / (3.0f * g_S), 0, 1);
                    if (v > c) c = v;
                }
                // Flatten the bottom, softly.
                c *= clampf((base - fy) / (4.0f * g_S) + 0.5f, 0, 1);
                sp->cov[y * W + x] = (uint8_t)(c * 255);
                sp->shade[y * W + x] = (uint8_t)(255 * clampf((fy - 8 * g_S) / (H - 18.0f * g_S), 0, 1));
            }
    }
    g_spriteScale = g_S;
}

static void draw_sprite(const Sprite *sp, float x, float y, float scale, uint32_t lit, uint32_t shadow, int a)
{
    if (!sp->cov) return;
    int X = D(x), Y = D(y);
    int W = (int)(sp->w * scale), H = (int)(sp->h * scale);
    float inv = 1.0f / scale;
    for (int j = 0; j < H; j++) {
        int yy = Y + j;
        if (yy < g_clipY0 || yy >= g_clipY1) continue;
        int sy = (int)(j * inv);
        if (sy >= sp->h) sy = sp->h - 1;
        for (int i = 0; i < W; i++) {
            int xx = X + i;
            if (xx < g_clipX0 || xx >= g_clipX1) continue;
            int sx = (int)(i * inv);
            if (sx >= sp->w) sx = sp->w - 1;
            int c = sp->cov[sy * sp->w + sx];
            if (!c) continue;
            blendp(xx, yy, mix(lit, shadow, sp->shade[sy * sp->w + sx]), c * a / 255);
        }
    }
}

static void sky_init(void)
{
    const Rect *R = &R_HERO;
    for (int i = 0; i < N_STARS; i++) {
        g_stars[i].x = R->x + frand() * R->w;
        g_stars[i].y = R->y + frand() * (R->h * 0.7f);
        g_stars[i].tw = frand() * 6.28f;
    }
    for (int i = 0; i < MAX_DROPS; i++) {
        g_drops[i].x = R->x + frand() * (R->w + 60);
        g_drops[i].y = R->y + frand() * R->h;
        g_drops[i].v = 260 + frand() * 160;
        g_drops[i].len = 7 + frand() * 7;
    }
    for (int i = 0; i < MAX_FLAKES; i++) {
        g_flakes[i].x = R->x + frand() * R->w;
        g_flakes[i].y = R->y + frand() * R->h;
        g_flakes[i].v = 14 + frand() * 22;
        g_flakes[i].ph = frand() * 6.28f;
        g_flakes[i].r = 0.8f + frand() * 1.6f;
    }
    for (int i = 0; i < MAX_CLOUDS; i++) {
        g_clouds[i].layer = i % 2;
        g_clouds[i].sprite = i % N_SPRITES;
        g_clouds[i].scale = g_clouds[i].layer ? 0.95f + frand() * 0.45f : 0.55f + frand() * 0.3f;
        g_clouds[i].speed = (g_clouds[i].layer ? 7 : 3.5f) + frand() * 4;
        g_clouds[i].x = R->x - 150 + frand() * (R->w + 150);
        g_clouds[i].y = R->y + 6 + frand() * (g_clouds[i].layer ? 70 : 40);
    }
    // The skyline: deterministic, so it's the same town every visit.
    uint32_t keep = g_rng;
    g_rng = 0xC0FFEEu;
    float x = R->x - 4;
    g_nBld = 0;
    while (x < R->x + R->w && g_nBld < MAX_BUILDINGS) {
        Building *b = &g_bld[g_nBld++];
        b->x = x;
        b->w = 12 + frand() * 22;
        b->h = 12 + frand() * frand() * 40;
        b->antenna = frand() < 0.18f;
        x += b->w + (frand() < 0.3f ? 2 : 0);
    }
    g_rng = keep;
}

typedef struct {
    float dayAmt;       // 0 night .. 1 day
    float twilight;     // 0..1 near sunrise/sunset
    float overcast;     // 0..1 greyness
    float cloudCover;   // 0..1 how many clouds
    int   rain;         // drops to show
    int   snow;         // flakes to show
    int   thunder, fog, freezing;
    float sunFrac;      // 0..1 across the day arc, <0 = below the horizon
    float moonFrac;     // same for the night arc
    int   rising;       // twilight is dawn (1) or dusk (0)
} SkyState;

static int day_index_for(int64_t t)
{
    if (!g_haveDy) return -1;
    for (int i = 0; i < g_dy.count; i++)
        if (t >= (int64_t)g_dy.d[i].date && t < (int64_t)g_dy.d[i].date + 86400) return i;
    return -1;
}

static SkyState sky_state(void)
{
    SkyState s;
    memset(&s, 0, sizeof s);
    s.sunFrac = -1;
    s.moonFrac = -1;
    if (!have_data()) {
        s.dayAmt = 0.15f;
        s.cloudCover = 0.5f;
        s.overcast = 0.3f;
        return s;
    }
    int64_t t = now_utc();
    int di = day_index_for(t);
    if (di >= 0) {
        float rise = (float)((int64_t)g_dy.d[di].sunrise - t), set = (float)((int64_t)g_dy.d[di].sunset - t);
        // dayAmt eases in over the 40 minutes either side of sunrise and sunset.
        s.dayAmt = smooth01((-rise + 2400) / 4800) * smooth01((set + 2400) / 4800);
        float dr = fabsf(rise), ds = fabsf(set);
        s.twilight = 1 - smooth01(fminf(dr, ds) / 3600);
        s.rising = dr < ds;
        float len = (float)(g_dy.d[di].sunset - g_dy.d[di].sunrise);
        if (len > 0) {
            float f = (float)(t - (int64_t)g_dy.d[di].sunrise) / len;
            if (f >= -0.02f && f <= 1.02f) s.sunFrac = f;
        }
        // The night arc runs from one sunset to the next sunrise.
        int64_t setPrev = (int64_t)g_dy.d[di].sunset - 86400, riseNext = (int64_t)g_dy.d[di].sunrise + 86400;
        int64_t n0 = t < (int64_t)g_dy.d[di].sunrise ? setPrev : (int64_t)g_dy.d[di].sunset;
        int64_t n1 = t < (int64_t)g_dy.d[di].sunrise ? (int64_t)g_dy.d[di].sunrise : riseNext;
        if (t < (int64_t)g_dy.d[di].sunrise || t > (int64_t)g_dy.d[di].sunset)
            s.moonFrac = (float)(t - n0) / (float)(n1 - n0);
    } else {
        s.dayAmt = g_cur.isDay ? 1.0f : 0.0f;
    }

    Kind k = kind_of(g_cur.code);
    float cc = g_cur.cloud / 100.0f;
    s.cloudCover = cc;
    s.overcast = clampf((cc - 0.45f) / 0.55f, 0, 1) * 0.75f;
    switch (k) {
    case K_CLEAR:  s.cloudCover = 0; s.overcast = 0; break;
    case K_MOSTLY: s.cloudCover = clampf(cc, 0.15f, 0.35f); s.overcast = 0; break;
    case K_PARTLY: s.cloudCover = clampf(cc, 0.35f, 0.6f); s.overcast *= 0.4f; break;
    case K_CLOUDY: s.cloudCover = 1; s.overcast = fmaxf(s.overcast, 0.8f); break;
    case K_FOG:    s.cloudCover = 0.5f; s.overcast = 0.85f; s.fog = 1; break;
    case K_DRIZZLE: s.cloudCover = 1; s.overcast = 0.85f; s.rain = 90; break;
    case K_RAIN:   s.cloudCover = 1; s.overcast = 0.9f; s.rain = 200; break;
    case K_HEAVY:  s.cloudCover = 1; s.overcast = 1; s.rain = 400; break;
    case K_FREEZE: s.cloudCover = 1; s.overcast = 0.9f; s.rain = 150; s.freezing = 1; break;
    case K_SNOW:   s.cloudCover = 1; s.overcast = 0.85f; s.snow = 130; break;
    case K_HEAVYSNOW: s.cloudCover = 1; s.overcast = 0.95f; s.snow = 250; break;
    case K_THUNDER: s.cloudCover = 1; s.overcast = 1; s.rain = 320; s.thunder = 1; break;
    }
    return s;
}

// The moon's age as a fraction of the synodic month: 0 new, 0.5 full.
static float moon_phase(int64_t t)
{
    double days = (double)(t - 947182440LL) / 86400.0;     // new moon 2000-01-06 18:14 UTC
    double p = fmod(days / 29.530588853, 1.0);
    return (float)(p < 0 ? p + 1 : p);
}

static void draw_moon(float cx, float cy, float r, float phase, int a)
{
    glow_l(cx, cy, r * 3.2f, 0xBFD2FF, a * 40 / 255);
    float CX = cx * g_S, CY = cy * g_S, R = r * g_S;
    float k = cosf(2 * PI_F * phase);
    for (int y = (int)(CY - R - 1); y <= (int)(CY + R + 1); y++)
        for (int x = (int)(CX - R - 1); x <= (int)(CX + R + 1); x++) {
            float dx = x + 0.5f - CX, dy = y + 0.5f - CY;
            float c = clampf(R - sqrtf(dx * dx + dy * dy) + 0.5f, 0, 1);
            if (c <= 0) continue;
            float ny = dy / R, nx = dx / R;
            float w = sqrtf(fmaxf(0, 1 - ny * ny));
            // The terminator is an ellipse; soften it over a few pixels.
            float edge = phase < 0.5f ? (nx - w * k) : (-w * k - nx);
            float lit = clampf(edge * R / 1.5f + 0.5f, 0, 1);
            uint32_t col = mixf(0x2A3350, 0xF4EDD2, lit);
            blendp(x, y, col, (int)(a * c));
        }
}

static void draw_sky(float dt)
{
    const Rect *R = &R_HERO;
    SkyState s = sky_state();
    float t = trace_time_ms() / 1000.0f;

    // ---- the gradient
    uint32_t dayTop = 0x2E6CD6, dayBot = 0x9FD0F7, nightTop = 0x040817, nightBot = 0x18244D;
    uint32_t dawnTop = 0x3B4B92, dawnBot = 0xFFB27A, duskTop = 0x2B2A6B, duskBot = 0xFF7F5C;
    uint32_t greyDayTop = 0x5E6878, greyDayBot = 0xA3ACBA, greyNightTop = 0x0B0E16, greyNightBot = 0x262B38;
    uint32_t top = mixf(nightTop, dayTop, s.dayAmt), bot = mixf(nightBot, dayBot, s.dayAmt);
    float tw = s.twilight * (1 - s.overcast * 0.8f);
    top = mixf(top, s.rising ? dawnTop : duskTop, tw * 0.8f);
    bot = mixf(bot, s.rising ? dawnBot : duskBot, tw * 0.9f);
    uint32_t gTop = mixf(greyNightTop, greyDayTop, s.dayAmt), gBot = mixf(greyNightBot, greyDayBot, s.dayAmt);
    if (s.thunder || kind_of(g_cur.code) == K_HEAVY) { gTop = mix(gTop, 0x000000, 90); gBot = mix(gBot, 0x000000, 70); }
    top = mixf(top, gTop, s.overcast);
    bot = mixf(bot, gBot, s.overcast);
    vgrad_l(R->x, R->y, R->w, R->h, top, bot, 255, 255);

    // ---- stars
    float starA = (1 - s.dayAmt) * (1 - s.overcast) * (1 - s.cloudCover * 0.6f);
    if (starA > 0.05f)
        for (int i = 0; i < N_STARS; i++) {
            float tw2 = 0.55f + 0.45f * sinf(t * 1.7f + g_stars[i].tw);
            int a = (int)(220 * starA * tw2);
            if (i % 9 == 0) circle_l(g_stars[i].x, g_stars[i].y, 0.9f, 0xFFFFFF, a);
            else fill_l(g_stars[i].x, g_stars[i].y, 1.0f / g_S * (g_S == 1 ? 1 : 2), 1.0f / g_S * (g_S == 1 ? 1 : 2), 0xFFFFFF, a);
        }

    // ---- sun and moon ride arcs across the right-hand part of the sky, clear of the text
    float arcX0 = R->x + 170, arcX1 = R->x + R->w - 26, horizon = R->y + R->h - 18, arcH = R->h - 50;
    float bodyA = 1 - s.overcast * 0.95f;
    if (s.sunFrac >= 0 && bodyA > 0.05f) {
        float f = s.sunFrac;
        float x = lerpf(arcX0, arcX1, f), y = horizon - sinf(clampf(f, 0, 1) * PI_F) * arcH;
        uint32_t sc = mixf(0xFFE7A0, 0xFF9F5A, s.twilight);
        float breathe = 1 + 0.08f * sinf(t * 0.9f);
        // Long soft rays that turn slowly, so a cloudless day still moves.
        for (int i = 0; i < 12; i++) {
            float ang = i * PI_F / 6 + t * 0.06f;
            float len = (i % 2 ? 44 : 62) * breathe;
            line_l(x + cosf(ang) * 18, y + sinf(ang) * 18, x + cosf(ang) * len, y + sinf(ang) * len,
                   i % 2 ? 2.5f : 4, sc, (int)(26 * bodyA));
        }
        glow_l(x, y, 70 * breathe, sc, (int)(90 * bodyA));
        glow_l(x, y, 30, 0xFFFFFF, (int)(80 * bodyA));
        circle_l(x, y, 13, mixf(0xFFF6D8, 0xFFC27A, s.twilight), (int)(255 * bodyA));
    }
    if (s.moonFrac >= 0 && bodyA > 0.05f && have_data()) {
        float f = s.moonFrac;
        float x = lerpf(arcX0, arcX1, f), y = horizon - sinf(clampf(f, 0, 1) * PI_F) * arcH;
        draw_moon(x, y, 11, moon_phase(now_utc()), (int)(255 * bodyA));
    }

    // ---- clouds: far layer, then near
    int want = (int)lroundf(s.cloudCover * MAX_CLOUDS);
    if (s.cloudCover > 0.15f && want < 2) want = 2;
    uint32_t lit = mixf(0x3A4466, 0xFFFFFF, s.dayAmt), shadow = mixf(0x151B30, 0xB8C4D6, s.dayAmt);
    if (s.twilight > 0.1f) lit = mixf(lit, s.rising ? 0xFFD2B0 : 0xFFB9A0, s.twilight * 0.6f);
    uint32_t greyLit = mixf(0x2A3040, 0xC3CAD4, s.dayAmt), greyShadow = mixf(0x10131C, 0x6E7788, s.dayAmt);
    if (s.thunder || s.rain > 250) { greyLit = mix(greyLit, 0x000000, 70); greyShadow = mix(greyShadow, 0x000000, 90); }
    lit = mixf(lit, greyLit, s.overcast);
    shadow = mixf(shadow, greyShadow, s.overcast);
    float windPush = 1 + (g_haveCur ? g_cur.wind10 / 150.0f : 0);
    for (int layer = 0; layer < 2; layer++)
        for (int i = 0; i < MAX_CLOUDS; i++) {
            Cloud *c = &g_clouds[i];
            if (c->layer != layer) continue;
            c->x += c->speed * windPush * dt;
            if (c->x > R->x + R->w + 10) {
                c->x = R->x - SPRITE_W * c->scale - frand() * 60;
                c->y = R->y + 6 + frand() * (layer ? 70 : 40);
            }
            if (i >= want) continue;
            int a = layer ? 235 : 170;
            draw_sprite(&g_sprites[c->sprite], c->x - 10 * c->scale, c->y - SPRITE_TOP * c->scale,
                        c->scale, lit, shadow, a);
        }
    // Full overcast: a ceiling of cloud across the top.
    if (s.overcast > 0.5f)
        vgrad_l(R->x, R->y, R->w, 60, shadow, lit, (int)(200 * (s.overcast - 0.5f) * 2), 0);

    // ---- lightning
    static float flash = 0, nextStrike = 3, boltLife = 0;
    static float bolt[16];
    if (s.thunder) {
        nextStrike -= dt;
        if (nextStrike <= 0) {
            flash = 1;
            boltLife = 0.18f;
            nextStrike = 2.5f + frand() * 6;
            float bx = R->x + 180 + frand() * (R->w - 220), by = R->y + 10;
            for (int i = 0; i < 8; i++) {
                bolt[i * 2] = bx;
                bolt[i * 2 + 1] = by;
                bx += (frand() - 0.5f) * 26;
                by += (R->h - 50) / 7.0f;
            }
        }
    }
    if (boltLife > 0) {
        for (int i = 0; i < 7; i++) {
            line_l(bolt[i * 2], bolt[i * 2 + 1], bolt[i * 2 + 2], bolt[i * 2 + 3], 5, 0xB9C8FF, 70);
            line_l(bolt[i * 2], bolt[i * 2 + 1], bolt[i * 2 + 2], bolt[i * 2 + 3], 1.6f, 0xFFFFFF, 255);
        }
        boltLife -= dt;
    }

    // ---- the skyline, with windows lit at night
    float groundY = R->y + R->h;
    uint32_t hill = mixf(mix(bot, 0x000000, 60), mix(bot, 0x1A2A40, 120), 0.5f);
    for (int i = 0; i < 4; i++) {
        float cx = R->x + 40 + i * 120, r = 70 + (i % 2) * 25;
        circle_l(cx, groundY + r - 26 - (i % 2) * 6, r, hill, 255);
    }
    uint32_t bld = mixf(0x070A14, mix(bot, 0x10182A, 170), s.dayAmt * 0.6f);
    for (int i = 0; i < g_nBld; i++) {
        Building *b = &g_bld[i];
        fill_l(b->x, groundY - b->h, b->w, b->h, bld, 255);
        if (b->antenna) fill_l(b->x + b->w / 2, groundY - b->h - 8, 1, 8, bld, 255);
        if (s.dayAmt < 0.6f) {
            float wa = 1 - s.dayAmt / 0.6f;
            uint32_t seed = (uint32_t)i * 2654435761u;
            for (float wy = groundY - b->h + 4; wy < groundY - 4; wy += 5)
                for (float wx = b->x + 3; wx < b->x + b->w - 3; wx += 5) {
                    seed = seed * 1103515245u + 12345u;
                    if ((seed >> 16) % 100 < 28) {
                        // A few windows switch on and off over time.
                        int blink = ((seed >> 8) % 23 == 0) && ((int)(t / 4 + (seed & 7)) % 3 == 0);
                        if (!blink) fill_l(wx, wy, 2, 2, 0xFFD27A, (int)(200 * wa));
                    }
                }
        }
    }

    // ---- rain and snow, in front of everything
    float slant = g_haveCur ? clampf(g_cur.wind10 / 10.0f / 60.0f, 0, 0.6f) : 0.15f;
    int rainN = s.rain < MAX_DROPS ? s.rain : MAX_DROPS;
    uint32_t rainCol = s.dayAmt > 0.5f ? 0xDDE8F5 : 0x9FB3D6;
    for (int i = 0; i < rainN; i++) {
        Drop *d = &g_drops[i];
        d->y += d->v * dt;
        d->x += d->v * slant * dt;
        if (d->y > groundY) { d->y = R->y - frand() * 20; d->x = R->x - 40 + frand() * (R->w + 40); }
        if (d->x > R->x + R->w + 20) d->x -= R->w + 60;
        line_l(d->x, d->y, d->x - d->len * slant, d->y - d->len, 1.0f, s.freezing && i % 3 == 0 ? 0xFFFFFF : rainCol,
               s.rain > 250 ? 150 : 115);
    }
    int snowN = s.snow < MAX_FLAKES ? s.snow : MAX_FLAKES;
    for (int i = 0; i < snowN; i++) {
        Flake *f = &g_flakes[i];
        f->y += f->v * dt;
        f->x += (sinf(t * 1.3f + f->ph) * 12 + slant * 30) * dt;
        if (f->y > groundY) { f->y = R->y - 4; f->x = R->x + frand() * R->w; }
        if (f->x > R->x + R->w) f->x -= R->w;
        if (f->x < R->x) f->x += R->w;
        circle_l(f->x, f->y, f->r, 0xFFFFFF, 220);
    }

    // ---- fog banks drift across
    if (s.fog) {
        uint32_t fc = mixf(0x3A4152, 0xD5DAE2, s.dayAmt);
        for (int b = 0; b < 3; b++) {
            float by = R->y + 60 + b * 45;
            int x0 = D(R->x), x1 = D(R->x + R->w), y0 = D(by), y1 = D(by + 50);
            for (int y = y0; y < y1; y++) {
                float v = sinf((float)(y - y0) / (float)(y1 - y0) * PI_F);
                for (int x = x0; x < x1; x += 1) {
                    float w = 0.6f + 0.4f * sinf((float)x / (40.0f * g_S) + t * (0.3f + b * 0.1f) + b);
                    blendp(x, y, fc, (int)(110 * v * w));
                }
            }
        }
    }

    // ---- the lightning flash lights the whole scene
    if (flash > 0) {
        fill_l(R->x, R->y, R->w, R->h, 0xE6ECFF, (int)(150 * flash));
        flash -= dt * 3.5f;
    }

    // Keep the text readable whatever the sky is doing: the brighter the sky,
    // the more it is darkened behind the words.
    uint32_t m = mix(top, bot, 128);
    float lum = (0.3f * ((m >> 16) & 255) + 0.59f * ((m >> 8) & 255) + 0.11f * (m & 255)) / 255.0f;
    int scrim = (int)(80 + 150 * lum);
    int x0 = D(R->x), x1 = D(R->x + 300);
    for (int x = x0; x < x1; x++) {
        float f = 1 - (float)(x - x0) / (float)(x1 - x0);
        fill_d(x, D(R->y), x + 1, D(R->y + R->h), 0x050814, (int)(scrim * f * f));
    }
}

// ------------------------------------------------------------------ panels ---

static void panel(const Rect *r)
{
    rrect_l(r->x, r->y, r->w, r->h, 8, C_PANEL, 255);
    rrect_stroke_l(r->x, r->y, r->w, r->h, 8, 1, C_EDGE, 255);
}

// Round the sky's corners by painting the background back over them.
static void round_corners(const Rect *r, float rad)
{
    int X0 = D(r->x), Y0 = D(r->y), X1 = D(r->x + r->w), Y1 = D(r->y + r->h);
    float R = rad * g_S;
    int band = (int)ceilf(R);
    float cx = (X0 + X1) * 0.5f, cy = (Y0 + Y1) * 0.5f, hw = (X1 - X0) * 0.5f, hh = (Y1 - Y0) * 0.5f;
    for (int y = Y0; y < Y1; y++) {
        if (y >= Y0 + band && y < Y1 - band) continue;
        uint32_t bg = mixf(C_BG0, C_BG1, (float)y / g_H);
        for (int pass = 0; pass < 2; pass++)
            for (int x = pass ? X1 - band : X0; x < (pass ? X1 : X0 + band); x++) {
                float c = rbox_cov(x + 0.5f, y + 0.5f, cx, cy, hw, hh, R);
                if (c < 1) blendp(x, y, bg, (int)(255 * (1 - c)));
            }
    }
    rrect_stroke_l(r->x, r->y, r->w, r->h, rad, 1, C_EDGE, 200);
}

static void draw_hero(float dt)
{
    const Rect *R = &R_HERO;
    clip_l(R->x, R->y, R->w, R->h);
    draw_sky(dt);
    clip_all();
    round_corners(R, 10);

    float x = R->x + 18;
    if (!have_data()) {
        spinner(x + 10, R->y + 42, 9, C_TEXT);
        text_shadow(F_HEAD, x + 28, R->y + 33, "Fetching the forecast...", C_TEXT);
        return;
    }
    char buf[64];
    // The big temperature, the degree sign set smaller like a superscript.
    snprintf(buf, sizeof buf, "%d", temp_i(g_cur.temp10));
    float y = R->y + 4;
    float ex = text_shadow(F_HUGE, x - 4, y, buf, C_TEXT);
    ring_l(ex + 7, y + 26, 5.5f, 2.2f, 0x000000, 70);
    ring_l(ex + 6, y + 25, 5.5f, 2.2f, C_TEXT, 255);
    text_shadow(F_HEAD, ex + 15, y + 17, g_units == WX_UNITS_IMPERIAL ? "F" : "C", C_TEXT);

    text_shadow(F_TITLE, x, R->y + 98, desc_of(g_cur.code, g_cur.isDay), C_TEXT);

    char feels[24], hi[16], lo[16];
    fmt_temp(feels, sizeof feels, g_cur.feels10);
    int di = day_index_for(now_utc());
    if (di < 0) di = 0;
    fmt_temp(hi, sizeof hi, g_dy.d[di].hi10);
    fmt_temp(lo, sizeof lo, g_dy.d[di].lo10);
    snprintf(buf, sizeof buf, "Feels like %s", feels);
    float lx = text_shadow(F_BODY, x, R->y + 126, buf, 0xD5DCEA);
    circle_l(lx + 7, R->y + 133, 1.5f, 0xB8C2D8, 255);
    lx = text_shadow(F_BODY, lx + 14, R->y + 126, "H ", 0xB8C2D8);
    lx = text_shadow(F_BOLD, lx, R->y + 126, hi, C_TEXT);
    lx = text_shadow(F_BODY, lx + 8, R->y + 126, "L ", 0xB8C2D8);
    text_shadow(F_BOLD, lx, R->y + 126, lo, 0xD5DCEA);
}

static void tile(float x, float y, float w, float h, const char *name)
{
    rrect_l(x, y, w, h, 7, C_PANEL, 255);
    rrect_stroke_l(x, y, w, h, 7, 1, C_EDGE, 255);
    label(x + 9, y + 7, name, C_TEXT3);
}

static void draw_details(void)
{
    const Rect *R = &R_DETAILS;
    float tw = (R->w - 6) / 2, th = 45, g = 6;
    float x0 = R->x, x1 = R->x + tw + g;
    float y[4] = { R->y, R->y + th + g, R->y + 2 * (th + g), R->y + 3 * (th + g) };
    char buf[48];
    int ok = have_data();

    // Wind, with a compass that points where the wind is going.
    snprintf(buf, sizeof buf, "WIND %s", ok ? compass(g_cur.windDir) : "");
    tile(x0, y[0], tw, th, buf);
    {
        float cx = x0 + tw - 20, cy = y[0] + th / 2 + 1;
        ring_l(cx, cy, 12, 1, C_EDGE_HI, 255);
        for (int i = 0; i < 4; i++) {
            float a = i * PI_F / 2;
            line_l(cx + cosf(a) * 10, cy + sinf(a) * 10, cx + cosf(a) * 12.5f, cy + sinf(a) * 12.5f, 1.2f, C_TEXT3, 255);
        }
        if (ok) {
            float a = (g_cur.windDir + 180) * PI_F / 180 - PI_F / 2;      // blowing towards
            float ca = cosf(a), sa = sinf(a);
            float tipx = cx + ca * 10, tipy = cy + sa * 10;
            line_l(cx - ca * 7, cy - sa * 7, tipx - ca * 3, tipy - sa * 3, 1.6f, C_ACCENT, 255);
            float p[] = { tipx, tipy, tipx - ca * 6 - sa * 3.5f, tipy - sa * 6 + ca * 3.5f,
                          tipx - ca * 6 + sa * 3.5f, tipy - sa * 6 - ca * 3.5f };
            poly_l(p, 3, C_ACCENT, 255);
            snprintf(buf, sizeof buf, "%d", wind_num(g_cur.wind10));
            float ex = text(F_HEAD, x0 + 9, y[0] + 20, buf, C_TEXT);
            text(F_SMALL, ex + 3, y[0] + 25, g_units == WX_UNITS_IMPERIAL ? "mph" : "km/h", C_TEXT2);
        }
    }

    // Humidity, with a little gauge.
    tile(x1, y[0], tw, th, "HUMIDITY");
    if (ok) {
        snprintf(buf, sizeof buf, "%d%%", g_cur.humidity);
        text(F_HEAD, x1 + 9, y[0] + 20, buf, C_TEXT);
        float gx = x1 + tw - 14, gy = y[0] + 9, gh = th - 18;
        rrect_l(gx, gy, 5, gh, 2.5f, C_PANEL_HI, 255);
        float fh = gh * g_cur.humidity / 100.0f;
        rrect_l(gx, gy + gh - fh, 5, fh, 2.5f, C_RAIN, 255);
    }

    tile(x0, y[1], tw, th, "PRESSURE");
    if (ok) { fmt_pressure(buf, sizeof buf, g_cur.pressure10); text(F_HEAD, x0 + 9, y[1] + 20, buf, C_TEXT); }

    tile(x1, y[1], tw, th, "DEW POINT");
    if (ok) { fmt_temp(buf, sizeof buf, g_cur.dew10); text(F_HEAD, x1 + 9, y[1] + 20, buf, C_TEXT); }

    tile(x0, y[2], tw, th, "VISIBILITY");
    if (ok) { fmt_vis(buf, sizeof buf, g_cur.visibility); text(F_HEAD, x0 + 9, y[2] + 20, buf, C_TEXT); }

    tile(x1, y[2], tw, th, "UV INDEX");
    if (ok) {
        uint32_t col;
        const char *lvl = uv_level(g_cur.uv10, &col);
        snprintf(buf, sizeof buf, "%d", (g_cur.uv10 + 5) / 10);
        float ex = text(F_HEAD, x1 + 9, y[2] + 20, buf, C_TEXT);
        float lw = text_w(F_SMALL, lvl, 0.6f) + 10;
        rrect_l(ex + 6, y[2] + 22, lw, 13, 6.5f, col, 50);
        text_sp(F_SMALL, ex + 11, y[2] + 23, lvl, col, 255, 0.6f);
    }

    // Sunrise to sunset, with the sun's place on its arc right now.
    float wy = y[3], ww = R->w;
    tile(x0, wy, ww, th, "SUN");
    if (ok) {
        int di = day_index_for(now_utc());
        if (di < 0) di = 0;
        const WxDay *d = &g_dy.d[di];
        char rise[16], set[16];
        fmt_clock(rise, sizeof rise, d->sunrise, 1);
        fmt_clock(set, sizeof set, d->sunset, 1);
        float ax0 = x0 + 80, ax1 = x0 + ww - 80, base = wy + th - 9, ah = 21;
        // The arc, dashed below and solid where the sun has travelled.
        float f = (float)(now_utc() - (int64_t)d->sunrise) / (float)(d->sunset - d->sunrise);
        int steps = 40;
        for (int i = 0; i < steps; i++) {
            float t0 = (float)i / steps, t1 = (float)(i + 1) / steps;
            float px0 = lerpf(ax0, ax1, t0), py0 = base - sinf(t0 * PI_F) * ah;
            float px1 = lerpf(ax0, ax1, t1), py1 = base - sinf(t1 * PI_F) * ah;
            int done = t1 <= f;
            if (done || i % 2 == 0) line_l(px0, py0, px1, py1, done ? 1.6f : 1.0f, done ? C_SUN : C_EDGE_HI, 255);
        }
        line_l(ax0 - 6, base, ax1 + 6, base, 1, C_EDGE_HI, 255);
        if (f >= 0 && f <= 1) {
            float sx = lerpf(ax0, ax1, f), sy = base - sinf(f * PI_F) * ah;
            glow_l(sx, sy, 12, C_SUN, 110);
            circle_l(sx, sy, 4, C_SUN, 255);
        }
        // Rise on the left, set on the right, each with a little arrow.
        float ty = wy + 25;
        line_l(x0 + 13, ty + 10, x0 + 13, ty + 2, 1.4f, C_SUN, 255);
        line_l(x0 + 10, ty + 5, x0 + 13, ty + 2, 1.4f, C_SUN, 255);
        line_l(x0 + 16, ty + 5, x0 + 13, ty + 2, 1.4f, C_SUN, 255);
        text(F_BOLD, x0 + 21, ty, rise, C_TEXT);
        float sw = text_w(F_BOLD, set, 0);
        float ax = x0 + ww - 22 - sw;
        line_l(ax + sw + 9, ty + 2, ax + sw + 9, ty + 10, 1.4f, 0xFF9F5A, 255);
        line_l(ax + sw + 6, ty + 7, ax + sw + 9, ty + 10, 1.4f, 0xFF9F5A, 255);
        line_l(ax + sw + 12, ty + 7, ax + sw + 9, ty + 10, 1.4f, 0xFF9F5A, 255);
        text(F_BOLD, ax, ty, set, C_TEXT);
        int mins = (int)((d->sunset - d->sunrise) / 60);
        snprintf(buf, sizeof buf, "%dh %02dm daylight", mins / 60, mins % 60);
        text_r(F_SMALL, x0 + ww - 9, wy + 7, buf, C_TEXT3);
    }
}

// ------------------------------------------------------------------ chart ---

typedef struct { float px0, px1, top, bot, barBot, cw; } ChartGeom;

static ChartGeom chart_geom(void)
{
    const Rect *R = &R_CHART;
    ChartGeom g;
    g.px0 = R->x + 12;
    g.px1 = R->x + R->w - 12;
    g.cw = (g.px1 - g.px0) / CHART_HOURS;
    g.top = R->y + 50;
    g.bot = R->y + 84;
    g.barBot = R->y + 98;
    return g;
}

static int chart_hour_at(float mx)
{
    ChartGeom g = chart_geom();
    int i = (int)((mx - g.px0) / g.cw);
    if (i < 0) i = 0;
    if (i >= CHART_HOURS) i = CHART_HOURS - 1;
    return i;
}

static void draw_tooltip(float px, float py, int hi)
{
    const WxHour *h = &g_hr.h[hi];
    int64_t t = (int64_t)g_hr.start + (int64_t)hi * 3600;
    char l1[48], l2[48], l3[64], tm[16], tp[16], wind[16];
    fmt_clock(tm, sizeof tm, t, 0);
    fmt_temp(tp, sizeof tp, h->temp10);
    LocalTime lt = local_time(t);
    snprintf(l1, sizeof l1, "%s %s", WDAY3[lt.wday], tm);
    snprintf(l2, sizeof l2, "%s", desc_of(h->code, h->isDay));
    fmt_wind(wind, sizeof wind, h->wind10);
    snprintf(l3, sizeof l3, "Rain %d%%   Wind %s   Hum %d%%", h->precipProb, wind, h->humidity);
    float w = fmaxf(text_w(F_BOLD, l1, 0) + text_w(F_HEAD, tp, 0) + 20, text_w(F_SMALL, l3, 0)) + 22;
    float wl2 = text_w(F_BODY, l2, 0) + 22;
    if (wl2 > w) w = wl2;
    float h2 = 56;
    float x = px + 12, y = py - h2 - 10;
    if (x + w > R_CHART.x + R_CHART.w - 4) x = px - w - 12;
    if (y < R_CHART.y + 4) y = R_CHART.y + 4;
    rrect_l(x + 2, y + 3, w, h2, 7, 0x000000, 90);
    rrect_l(x, y, w, h2, 7, 0x1F2A52, 250);
    rrect_stroke_l(x, y, w, h2, 7, 1, C_EDGE_HI, 255);
    text(F_BOLD, x + 11, y + 7, l1, C_TEXT);
    text_r(F_HEAD, x + w - 11, y + 5, tp, temp_color(h->temp10 / 10.0f));
    text(F_BODY, x + 11, y + 23, l2, C_TEXT2);
    text(F_SMALL, x + 11, y + 40, l3, C_TEXT3);
}

static void draw_chart(void)
{
    const Rect *R = &R_CHART;
    panel(R);
    ChartGeom g = chart_geom();

    if (!have_data()) {
        text(F_BOLD, R->x + 12, R->y + 8, "Hourly", C_TEXT2);
        // A shimmering placeholder while the forecast arrives.
        float t = trace_time_ms() / 1000.0f;
        for (int i = 0; i < CHART_HOURS; i++) {
            float x = g.px0 + i * g.cw + g.cw / 2;
            float y = lerpf(g.top, g.bot, 0.5f + 0.35f * sinf(i * 0.5f + t * 2));
            circle_l(x, y, 2, C_EDGE_HI, 255);
        }
        return;
    }

    int start = g_chartStart, n = hour_count();
    if (start + CHART_HOURS > n) start = n - CHART_HOURS;
    if (start < 0) start = 0;
    int nowI = now_hour_index();

    // ---- title
    char title[64];
    LocalTime lt0 = local_time((int64_t)g_hr.start + (int64_t)start * 3600);
    if (g_followNow || start == nowI) snprintf(title, sizeof title, "Next 24 hours");
    else if (start % 24 == 0)         snprintf(title, sizeof title, "%s, %s %d", WDAY[lt0.wday], MON3[lt0.mon], lt0.mday);
    else {
        char tm[16];
        fmt_clock(tm, sizeof tm, (int64_t)g_hr.start + (int64_t)start * 3600, 0);
        snprintf(title, sizeof title, "%s from %s", WDAY[lt0.wday], tm);
    }
    text(F_BOLD, R->x + 12, R->y + 8, title, C_TEXT);
    // legend
    float lx = R->x + R->w - 12;
    lx -= text_w(F_SMALL, "Chance of rain", 0);
    text(F_SMALL, lx, R->y + 10, "Chance of rain", C_TEXT3);
    rrect_l(lx - 12, R->y + 10, 7, 9, 2, C_RAIN, 170);
    lx -= 26 + text_w(F_SMALL, "Temperature", 0);
    text(F_SMALL, lx, R->y + 10, "Temperature", C_TEXT3);
    line_l(lx - 16, R->y + 15, lx - 5, R->y + 15, 2, C_SUN, 255);

    // ---- range
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < CHART_HOURS; i++) {
        float v = (float)temp_i(g_hr.h[start + i].temp10);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (hi - lo < 6) { float m = (hi + lo) / 2; lo = m - 3; hi = m + 3; }
    float px[CHART_HOURS], py[CHART_HOURS];
    for (int i = 0; i < CHART_HOURS; i++) {
        px[i] = g.px0 + i * g.cw + g.cw / 2;
        py[i] = lerpf(g.bot, g.top, ((float)temp_i(g_hr.h[start + i].temp10) - lo) / (hi - lo));
    }

    // ---- day boundaries
    for (int i = 1; i < CHART_HOURS; i++)
        if ((start + i) % 24 == 0) {
            float x = g.px0 + i * g.cw;
            for (float y = R->y + 26; y < g.barBot; y += 4) fill_l(x, y, 1, 2, C_EDGE_HI, 255);
        }

    // ---- rain bars
    for (int i = 0; i < CHART_HOURS; i++) {
        int pp = g_hr.h[start + i].precipProb;
        if (pp < 5) continue;
        float bh = (g.barBot - g.top + 8) * pp / 100.0f * 0.55f;
        if (bh < 4) bh = 4;
        rrect_l(px[i] - g.cw * 0.28f, g.barBot - bh, g.cw * 0.56f, bh, 2, C_RAIN, 70 + pp);
    }

    // ---- the temperature area and line, coloured by temperature
    for (int i = 0; i < CHART_HOURS - 1; i++) {
        int X0 = D(px[i]), X1 = D(px[i + 1]);
        for (int X = X0; X < X1; X++) {
            float f = (float)(X - X0) / (float)(X1 - X0);
            float yl = lerpf(py[i], py[i + 1], f);
            float tc = lerpf(g_hr.h[start + i].temp10, g_hr.h[start + i + 1].temp10, f) / 10.0f;
            uint32_t col = temp_color(tc);
            int Y0 = D(yl), Y1 = D(g.barBot);
            for (int Y = Y0; Y < Y1; Y++) {
                float v = 1 - (float)(Y - Y0) / (float)(Y1 - Y0 + 1);
                blendp(X, Y, col, (int)(55 * v * v));
            }
        }
    }
    for (int i = 0; i < CHART_HOURS - 1; i++) {
        uint32_t col = temp_color((g_hr.h[start + i].temp10 + g_hr.h[start + i + 1].temp10) / 20.0f);
        line_l(px[i], py[i], px[i + 1], py[i + 1], 2.2f, col, 255);
    }

    // ---- icons, temperatures and hour labels every 3 hours
    int64_t nt = now_utc();
    float nf = (float)(nt - (int64_t)g_hr.start) / 3600.0f - start;
    float nowX = nf >= 0 && nf < CHART_HOURS ? g.px0 + nf * g.cw : -1000;
    for (int i = 0; i < CHART_HOURS; i++) {
        int hi2 = start + i;
        int64_t t = (int64_t)g_hr.start + (int64_t)hi2 * 3600;
        LocalTime lt = local_time(t);
        if (lt.hour % 3 != 0) continue;
        icon(g_hr.h[hi2].code, g_hr.h[hi2].isDay, px[i], R->y + 34, 16);
        char buf[16];
        fmt_temp(buf, sizeof buf, g_hr.h[hi2].temp10);
        text_c(F_SMALL, px[i] + 2, py[i] - 14, buf, C_TEXT);
        circle_l(px[i], py[i], 2.4f, temp_color(g_hr.h[hi2].temp10 / 10.0f), 255);
        if (fabsf(px[i] - nowX) < 30) continue;          // "NOW" sits there
        if (lt.hour == 0) label_c(px[i], R->y + 102, WDAY3[lt.wday], C_ACCENT);
        else { fmt_clock(buf, sizeof buf, t, 0); text_c(F_SMALL, px[i], R->y + 102, buf, C_TEXT3); }
    }

    // ---- now
    if (nowX > -1000) {
        for (float y = R->y + 44; y < g.barBot; y += 5) fill_l(nowX, y, 1, 3, C_ACCENT, 200);
        float lw = text_w(F_SMALL, "NOW", 0.8f) + 10;
        float lx2 = clampf(nowX - lw / 2, R->x + 4, R->x + R->w - lw - 4);
        rrect_l(lx2, R->y + 100, lw, 13, 6.5f, C_ACCENT, 40);
        label(lx2 + 5, R->y + 101, "NOW", C_ACCENT);
    }

    // ---- hover
    if (g_hover >= 0 && g_hover < CHART_HOURS) {
        float x = px[g_hover];
        fill_l(x, R->y + 26, 1, g.barBot - R->y - 26, C_TEXT, 90);
        uint32_t col = temp_color(g_hr.h[start + g_hover].temp10 / 10.0f);
        circle_l(x, py[g_hover], 6, col, 70);
        circle_l(x, py[g_hover], 3.8f, col, 255);
        circle_l(x, py[g_hover], 1.8f, C_PANEL, 255);
        draw_tooltip(x, py[g_hover], start + g_hover);
    }
}

// ------------------------------------------------------------------- days ---

static void draw_days(void)
{
    const Rect *R = &R_DAYS;
    int n = g_haveDy ? g_dy.count : WX_DAYS;
    float wlo = 1e9f, whi = -1e9f;
    if (have_data())
        for (int i = 0; i < g_dy.count; i++) {
            wlo = fminf(wlo, g_dy.d[i].lo10 / 10.0f);
            whi = fmaxf(whi, g_dy.d[i].hi10 / 10.0f);
        }
    for (int i = 0; i < n && i < WX_DAYS; i++) {
        float x = R->x + i * (CARD_W + CARD_GAP), y = R->y, cx = x + CARD_W / 2;
        Rect card = { x, y, CARD_W, R->h };
        int sel = have_data() && i == g_selDay;
        int hot = !g_picker && g_mouseOver && inside(&card, g_mx, g_my);
        rrect_l(x, y, CARD_W, R->h, 8, sel ? C_PANEL_HI : hot ? 0x172042 : C_PANEL, 255);
        rrect_stroke_l(x, y, CARD_W, R->h, 8, sel ? 1.5f : 1, sel ? C_ACCENT : hot ? C_EDGE_HI : C_EDGE, 255);
        if (!have_data()) {
            rrect_l(cx - 18, y + 10, 36, 8, 4, C_PANEL_HI, 255);
            circle_l(cx, y + 36, 11, C_PANEL_HI, 255);
            rrect_l(cx - 24, y + 56, 48, 8, 4, C_PANEL_HI, 255);
            continue;
        }
        const WxDay *d = &g_dy.d[i];
        LocalTime lt = local_time((int64_t)d->date + 43200);
        label_c(cx, y + 8, i == 0 ? "TODAY" : WDAY3[lt.wday], sel ? C_TEXT : C_TEXT2);
        icon(d->code, 1, cx, y + 34, 28);

        char hi[16], lo[16];
        fmt_temp(hi, sizeof hi, d->hi10);
        fmt_temp(lo, sizeof lo, d->lo10);
        float wh = text_w(F_BOLD, hi, 0), wl = text_w(F_BODY, lo, 0);
        float tx = cx - (wh + 8 + wl) / 2;
        text(F_BOLD, tx, y + 50, hi, C_TEXT);
        text(F_BODY, tx + wh + 8, y + 50, lo, C_TEXT3);

        // This day's range against the whole week's.
        float bx = x + 12, bw = CARD_W - 24, by = y + 68;
        rrect_l(bx, by, bw, 4, 2, 0x0B1124, 255);
        if (whi > wlo) {
            float f0 = (d->lo10 / 10.0f - wlo) / (whi - wlo), f1 = (d->hi10 / 10.0f - wlo) / (whi - wlo);
            float sx0 = bx + f0 * bw, sx1 = bx + f1 * bw;
            if (sx1 - sx0 < 4) sx1 = sx0 + 4;
            int X0 = D(sx0), X1 = D(sx1);
            for (int X = X0; X < X1; X++) {
                float f = (float)(X - X0) / (float)(X1 - X0 > 1 ? X1 - X0 - 1 : 1);
                uint32_t col = temp_color(lerpf(d->lo10, d->hi10, f) / 10.0f);
                fill_d(X, D(by), X + 1, D(by + 4), col, 255);
            }
            circle_l(sx0 + 1.5f, by + 2, 2, temp_color(d->lo10 / 10.0f), 255);
            circle_l(sx1 - 1.5f, by + 2, 2, temp_color(d->hi10 / 10.0f), 255);
        }

        char pp[8];
        snprintf(pp, sizeof pp, "%d%%", d->precipProb);
        float pw = text_w(F_SMALL, pp, 0) + 10;
        uint32_t pc = d->precipProb >= 30 ? C_RAIN : C_TEXT3;
        droplet(cx - pw / 2 + 3, y + 79, 7, pc, 255);
        text(F_SMALL, cx - pw / 2 + 10, y + 74, pp, pc);
    }
}

// ------------------------------------------------------------------ chrome ---

static void draw_bar(void)
{
    fill_l(0, 0, LW, BAR_H, C_BAR, 255);
    fill_l(0, BAR_H, LW, 1, C_EDGE, 255);

    // Brand, in TERMinator's colours.
    float x = 12;
    icon(2, 1, x + 9, 15, 18);
    x += 24;
    x = text(F_HEAD, x, 5, "TERM", C_MAGENTA);
    x = text(F_HEAD, x, 5, "inator", C_ACCENT);
    x = text(F_HEAD, x + 6, 5, "Weather", C_TEXT);

    // The place: a button that opens the picker.
    char name[64];
    const char *src = g_haveCur && g_cur.place[0] ? g_cur.place : "Choose a location";
    float maxw = R_UNITS.x - 110 - (x + 20);
    fit(F_BODY, src, maxw, name, sizeof name);
    float bw = text_w(F_BODY, name, 0) + 42;
    g_placeBtn = (Rect){ x + 16, 4, bw, 20 };
    int hot = !g_picker && g_mouseOver && inside(&g_placeBtn, g_mx, g_my);
    rrect_l(g_placeBtn.x, g_placeBtn.y, bw, 20, 10, hot ? C_PANEL_HI : C_PANEL, 255);
    rrect_stroke_l(g_placeBtn.x, g_placeBtn.y, bw, 20, 10, 1, hot ? C_ACCENT : C_EDGE, 255);
    pin_icon(g_placeBtn.x + 13, 14, 12, C_ACCENT);
    text(F_BODY, g_placeBtn.x + 22, 7, name, C_TEXT);
    // the little "change" chevron
    float chx = g_placeBtn.x + bw - 12;
    line_l(chx - 3, 12, chx, 15, 1.4f, C_TEXT2, 255);
    line_l(chx, 15, chx + 3, 12, 1.4f, C_TEXT2, 255);

    // Local time at the place.
    if (g_haveCur) {
        char clock[32], tm[16];
        fmt_clock(tm, sizeof tm, now_utc(), 1);
        LocalTime lt = local_time(now_utc());
        snprintf(clock, sizeof clock, "%s %s", WDAY3[lt.wday], tm);
        text_r(F_BODY, R_UNITS.x - 12, 7, clock, C_TEXT2);
    }

    // Units: a two-way switch.
    const Rect *u = &R_UNITS;
    rrect_l(u->x, u->y, u->w, u->h, 9, C_PANEL, 255);
    rrect_stroke_l(u->x, u->y, u->w, u->h, 9, 1, C_EDGE, 255);
    int metric = g_units == WX_UNITS_METRIC;
    float half = u->w / 2;
    rrect_l(u->x + 2 + (metric ? half - 2 : 0), u->y + 2, half, u->h - 4, 7, C_ACCENT, 255);
    int hotU = !g_picker && g_mouseOver && inside(u, g_mx, g_my);
    text_c(F_BOLD, u->x + half / 2 + 1, u->y + 2, DEG "F", !metric ? C_BAR : hotU ? C_TEXT : C_TEXT2);
    text_c(F_BOLD, u->x + half + half / 2 - 1, u->y + 2, DEG "C", metric ? C_BAR : hotU ? C_TEXT : C_TEXT2);

    int hotR = !g_picker && g_mouseOver && inside(&R_REFRESH, g_mx, g_my);
    if (hotR) rrect_l(R_REFRESH.x, R_REFRESH.y, R_REFRESH.w, R_REFRESH.h, 6, C_PANEL_HI, 255);
    int32_t now = trace_time_ms();
    float spin = now < g_refreshSpinUntil || g_statusKind == WX_STATUS_BUSY ? now / 120.0f : 0;
    refresh_icon(R_REFRESH.x + 12, R_REFRESH.y + 10, 20, hotR ? C_TEXT : C_TEXT2, spin);

    int hotC = !g_picker && g_mouseOver && inside(&R_CLOSE, g_mx, g_my);
    if (hotC) rrect_l(R_CLOSE.x, R_CLOSE.y, R_CLOSE.w, R_CLOSE.h, 6, 0x5A2030, 255);
    close_icon(R_CLOSE.x + 12, R_CLOSE.y + 10, 20, hotC ? 0xFFFFFF : C_TEXT2);
}

static void draw_footer(void)
{
    float y = FOOT_Y;
    int32_t now = trace_time_ms();
    int showStatus = g_status[0] &&
        (g_statusKind == WX_STATUS_BUSY || now - g_statusAt < (g_statusKind == WX_STATUS_ERROR ? 12000 : 5000));
    if (showStatus) {
        uint32_t col = g_statusKind == WX_STATUS_ERROR ? C_ERR : g_statusKind == WX_STATUS_BUSY ? C_WARN : C_GOOD;
        float x = 12;
        if (g_statusKind == WX_STATUS_BUSY) { spinner(x + 5, y + 7, 5, col); x += 16; }
        else { circle_l(x + 4, y + 7, 3, col, 255); x += 13; }
        text(F_BODY, x, y, g_status, col);
    } else {
        float x = 12;
        // With a mouse, most of it is clickable: fewer keys to list.
        if (g_haveMouse) x = text(F_SMALL, x, y + 2, "Click a day, hover the chart", C_TEXT3) + 14;
        else { x = hint(x, y, "<>", "hours"); x = hint(x, y, "Up/Dn", "days"); }
        x = hint(x, y, "L", "location");
        x = hint(x, y, "U", "units");
        if (!g_haveMouse) x = hint(x, y, "R", "refresh");
        hint(x, y, "Esc", "quit");
    }
    // Open-Meteo's licence (CC BY 4.0) asks for the credit.
    const char *credit = "Data: Open-Meteo.com";
    float rx = LW - 12 - text_w(F_SMALL, credit, 0);
    text(F_SMALL, rx, y + 2, credit, C_TEXT3);
    if (g_haveCur && g_cur.fetchedAt && !showStatus) {
        char tm[16], upd[32];
        fmt_clock(tm, sizeof tm, g_cur.fetchedAt, 1);
        snprintf(upd, sizeof upd, "Updated %s", tm);
        circle_l(rx - 8, y + 8, 1.3f, C_TEXT3, 255);
        text_r(F_SMALL, rx - 15, y + 2, upd, C_TEXT3);
    }
}

// ------------------------------------------------------------------ picker ---

static int picker_row_at(float mx, float my)
{
    const Rect *P = &R_PICKER;
    if (mx < P->x + 16 || mx > P->x + P->w - 16) return -1;
    int i = (int)((my - PICK_ROWS_Y) / PICK_ROW_H);
    if (my < PICK_ROWS_Y || i < 0 || i >= PICK_VISIBLE || i >= g_placeCount) return -1;
    return i;
}

static void draw_picker(void)
{
    // Dim everything behind.
    fill_d(0, 0, g_W, g_H, 0x02040A, 175);
    const Rect *P = &R_PICKER;
    rrect_l(P->x + 3, P->y + 5, P->w, P->h, 12, 0x000000, 110);
    rrect_l(P->x, P->y, P->w, P->h, 12, 0x131B38, 255);
    rrect_stroke_l(P->x, P->y, P->w, P->h, 12, 1, C_EDGE_HI, 255);

    pin_icon(P->x + 26, P->y + 26, 18, C_ACCENT);
    text(F_HEAD, P->x + 42, P->y + 16, "Choose a location", C_TEXT);
    text(F_SMALL, P->x + 42, P->y + 38, "A city, \"City, State\", or a ZIP / postcode", C_TEXT3);

    // The field.
    float fx = P->x + 16, fy = PICK_FIELD_Y, fw = P->w - 32, fh = 34;
    rrect_l(fx, fy, fw, fh, 8, 0x0B1124, 255);
    rrect_stroke_l(fx, fy, fw, fh, 8, 1.5f, C_ACCENT, 255);
    // magnifier
    ring_l(fx + 17, fy + 15, 5.5f, 1.8f, C_TEXT2, 255);
    line_l(fx + 21, fy + 19, fx + 25, fy + 23, 2, C_TEXT2, 255);
    float tx = fx + 34;
    if (g_qlen) tx = text(F_HEAD, tx, fy + 8, g_query, C_TEXT);
    else text(F_HEAD, tx, fy + 8, "Start typing...", C_TEXT3);
    if ((trace_time_ms() / 530) % 2 == 0) fill_l(g_qlen ? tx + 1 : fx + 33, fy + 8, 1.5f, 18, C_ACCENT, 255);

    // Results, or what's happening.
    float ry = PICK_ROWS_Y;
    if (g_searching) {
        spinner(P->x + P->w / 2 - 44, ry + 40, 6, C_TEXT2);
        text(F_BODY, P->x + P->w / 2 - 32, ry + 33, "Searching...", C_TEXT2);
    } else if (g_placeCount == 0) {
        text_c(F_BODY, P->x + P->w / 2, ry + 26, "No places found.", C_TEXT2);
        text_c(F_SMALL, P->x + P->w / 2, ry + 46, "Check the spelling, or try a nearby city or a ZIP.", C_TEXT3);
    } else if (g_placeCount < 0) {
        text_c(F_SMALL, P->x + P->w / 2, ry + 30, "Press Enter to search.", C_TEXT3);
    } else {
        for (int i = 0; i < g_placeCount && i < PICK_VISIBLE; i++) {
            float y = ry + i * PICK_ROW_H;
            int sel = i == g_pickSel;
            if (sel) {
                rrect_l(P->x + 16, y + 1, P->w - 32, PICK_ROW_H - 2, 7, C_PANEL_HI, 255);
                rrect_stroke_l(P->x + 16, y + 1, P->w - 32, PICK_ROW_H - 2, 7, 1, C_EDGE_HI, 255);
            }
            pin_icon(P->x + 32, y + 13, 11, sel ? C_ACCENT : C_TEXT3);
            char nm[64];
            fit(F_BODY, g_places[i].name, P->w - 110, nm, sizeof nm);
            text(F_BODY, P->x + 44, y + 6, nm, sel ? C_TEXT : C_TEXT2);
            char ll[32];
            snprintf(ll, sizeof ll, "%.1f, %.1f", g_places[i].lat1e4 / 1e4, g_places[i].lon1e4 / 1e4);
            text_r(F_SMALL, P->x + P->w - 26, y + 8, ll, C_TEXT3);
        }
    }

    // Hints.
    float hy = P->y + P->h - 26;
    fill_l(P->x + 1, hy - 8, P->w - 2, 1, C_EDGE, 255);
    float hx = P->x + 18;
    hx = hint(hx, hy, "Enter", g_placeCount > 0 && !strcmp(g_query, g_lastSearch) ? "choose" : "search");
    if (g_placeCount > 0) hx = hint(hx, hy, "Up/Dn", "move");
    hint(hx, hy, "Esc", g_firstRun && !g_haveCur ? "skip" : "cancel");
}

// ------------------------------------------------------------- TRACE API ---

static void set_scale(int s)
{
    if (s == g_S && g_frame) return;
    uint32_t *nf = malloc((size_t)(LW * s) * (size_t)(LH * s) * 4);
    if (!nf) { if (g_frame) return; s = 1; nf = malloc(LW * LH * 4); }
    free(g_frame);
    g_frame = nf;
    g_S = s;
    g_W = LW * s;
    g_H = LH * s;
    clip_all();
}

int32_t trace_init(void)
{
    set_scale(1);
    // Needs TERMinator 1.1.3+; the door only offers TRACE when mouse=1.
    trace_mouse_mode(TRACE_MOUSE_POINTER);
    trace_set_tick(30);
    g_rng ^= (uint32_t)trace_time_ms() * 2654435761u;
    sky_init();
    return 0;
}

void *trace_alloc(int32_t size) { return size > 0 ? malloc((size_t)size) : NULL; }
void  trace_free(void *ptr)     { free(ptr); }

void trace_on_resize(int32_t width, int32_t height)
{
    set_display(width, height);
}

void trace_on_data(const char *data, int32_t length)
{
    if (length <= 0) return;
    if (length == 5 && !memcmp(data, "start", 5)) {
        if (!g_started) { g_started = 1; send_msg(WX_OUT_READY, NULL, 0); }
        return;
    }
    if (length == 4 && !memcmp(data, "quit", 4)) { g_quitting = 1; return; }

    // Everything else is head "msg" + '\n' + WxMsgHeader + payload.
    const char *nl = memchr(data, '\n', (size_t)length);
    if (!nl) return;
    const uint8_t *body = (const uint8_t *)nl + 1;
    int32_t bodyLen = length - (int32_t)(body - (const uint8_t *)data);
    if (bodyLen < (int32_t)sizeof(WxMsgHeader)) return;
    WxMsgHeader h;
    memcpy(&h, body, sizeof h);
    const uint8_t *p = body + sizeof h;
    if ((uint32_t)(bodyLen - (int32_t)sizeof h) < h.bytes) return;

    switch (h.type) {
    case WX_IN_CURRENT:
        if (h.bytes >= sizeof(WxCurrent)) {
            memcpy(&g_cur, p, sizeof g_cur);
            g_cur.place[WX_NAME_LEN - 1] = 0;
            g_recvMs = trace_time_ms();
            g_haveCur = 1;
        }
        break;
    case WX_IN_HOURLY:
        if (h.bytes >= 8) {
            memset(&g_hr, 0, sizeof g_hr);
            memcpy(&g_hr, p, h.bytes < sizeof g_hr ? h.bytes : sizeof g_hr);
            if (g_hr.count > WX_HOURS) g_hr.count = WX_HOURS;
            g_haveHr = g_hr.count >= CHART_HOURS;
            if (g_followNow) select_day(0);
            else set_chart_start(g_chartStart);
        }
        break;
    case WX_IN_DAILY:
        if (h.bytes >= 4) {
            memset(&g_dy, 0, sizeof g_dy);
            memcpy(&g_dy, p, h.bytes < sizeof g_dy ? h.bytes : sizeof g_dy);
            if (g_dy.count > WX_DAYS) g_dy.count = WX_DAYS;
            g_haveDy = g_dy.count > 0;
        }
        break;
    case WX_IN_PLACES: {
        int n = h.count;
        if (n > WX_MAX_PLACES) n = WX_MAX_PLACES;
        if ((uint32_t)n * sizeof(WxPlace) > h.bytes) n = (int)(h.bytes / sizeof(WxPlace));
        memcpy(g_places, p, (size_t)n * sizeof(WxPlace));
        for (int i = 0; i < n; i++) g_places[i].name[WX_NAME_LEN - 1] = 0;
        g_placeCount = n;
        g_pickSel = 0;
        g_searching = 0;
        break;
    }
    case WX_IN_STATUS:
        if (h.bytes >= 2) {
            int n = p[1];
            if (n > (int)h.bytes - 2) n = (int)h.bytes - 2;
            char buf[128];
            if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
            memcpy(buf, p + 2, (size_t)n);
            buf[n] = 0;
            set_status(p[0], buf);
            // A failed search answers the picker too.
            if (p[0] == WX_STATUS_ERROR && g_searching) { g_searching = 0; g_placeCount = -1; }
        }
        break;
    case WX_IN_PREFS:
        if (h.bytes >= sizeof(WxPrefs)) {
            WxPrefs pr;
            memcpy(&pr, p, sizeof pr);
            g_units = pr.units == WX_UNITS_METRIC ? WX_UNITS_METRIC : WX_UNITS_IMPERIAL;
            if (pr.firstRun && !g_picker) { g_firstRun = 1; open_picker(); }
        }
        break;
    default:
        break;
    }
}

// Set-1 scancodes.
#define SC_ESC 0x01
#define SC_BACK 0x0E
#define SC_TAB 0x0F
#define SC_ENTER 0x1C
#define SC_SPACE 0x39
#define SC_UP 0x48
#define SC_DOWN 0x50
#define SC_LEFT 0x4B
#define SC_RIGHT 0x4D
#define SC_HOME 0x47
#define SC_END 0x4F
#define SC_PGUP 0x49
#define SC_PGDN 0x51

// Letters, digits and the punctuation a place name needs, straight from
// scancodes, so the module doesn't need trace_text_input (older TERMinators
// don't have it). US layout; plenty for "St. John's, NL" or "90210".
static char scancode_char(int sc, int shift)
{
    static const char lower[0x40] =
        "\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`" "\0\\" "zxcvbnm,./" "\0\0\0 ";
    static const char upper[0x40] =
        "\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~" "\0|" "ZXCVBNM<>?" "\0\0\0 ";
    if (sc < 0 || sc >= 0x40) return 0;
    char c = shift ? upper[sc] : lower[sc];
    // Only what a place name uses.
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    if (c == ' ' || c == ',' || c == '.' || c == '-' || c == '\'') return c;
    return 0;
}

static void picker_key(int sc, int shift)
{
    switch (sc) {
    case SC_ESC:
        g_picker = 0;
        g_firstRun = 0;
        return;
    case SC_ENTER:
        if (g_placeCount > 0 && !strcmp(g_query, g_lastSearch)) do_pick(g_pickSel);
        else do_search();
        return;
    case SC_UP:   if (g_placeCount > 0 && g_pickSel > 0) g_pickSel--; return;
    case SC_DOWN: if (g_placeCount > 0 && g_pickSel < g_placeCount - 1 && g_pickSel < PICK_VISIBLE - 1) g_pickSel++; return;
    case SC_BACK:
        if (g_qlen > 0) g_query[--g_qlen] = 0;
        return;
    default: {
        char c = scancode_char(sc, shift);
        if (c && g_qlen < WX_SEARCH_LEN && !(c == ' ' && g_qlen == 0)) {
            // Capitalise the start of each word, so it reads like a place name.
            if (c >= 'a' && c <= 'z' && (g_qlen == 0 || g_query[g_qlen - 1] == ' ')) c = (char)(c - 32);
            g_query[g_qlen++] = c;
            g_query[g_qlen] = 0;
        }
    }
    }
}

static void move_hover(int delta)
{
    if (!have_data()) return;
    if (g_hover < 0) g_hover = g_followNow ? 0 : CHART_HOURS / 2;
    else g_hover += delta;
    g_hoverFromKey = 1;
    // Walk off the edge and the chart scrolls with you.
    if (g_hover < 0)            { g_hover = 0; g_followNow = 0; set_chart_start(g_chartStart - 1); }
    if (g_hover >= CHART_HOURS) { g_hover = CHART_HOURS - 1; g_followNow = 0; set_chart_start(g_chartStart + 1); }
}

void trace_on_input(int32_t type, int32_t flags, int32_t a, int32_t b, int32_t c)
{
    switch (type) {
    case 1: {   // TE_IN_KEY
        if (!(flags & 1)) break;
        int shift = (flags & 4) != 0;
        if (g_picker) { picker_key(a, shift); break; }
        switch (a) {
        case SC_ESC: g_quitting = 1; break;
        case SC_LEFT:  move_hover(-1); break;
        case SC_RIGHT: move_hover(1); break;
        case SC_UP:    select_day(g_selDay - 1); break;
        case SC_DOWN:  select_day(g_selDay + 1); break;
        case SC_TAB:   select_day(shift ? (g_selDay + WX_DAYS - 1) % WX_DAYS : (g_selDay + 1) % WX_DAYS); break;
        case SC_HOME:  select_day(0); g_hover = -1; break;
        case SC_PGUP:  g_followNow = 0; set_chart_start(g_chartStart - 6); break;
        case SC_PGDN:  g_followNow = 0; set_chart_start(g_chartStart + 6); break;
        default: {
            char ch = scancode_char(a, 0);
            if (ch >= '1' && ch <= '7') select_day(ch - '1');
            else if (ch == 'l') open_picker();
            else if (ch == 'u') do_units(g_units == WX_UNITS_METRIC ? WX_UNITS_IMPERIAL : WX_UNITS_METRIC);
            else if (ch == 'f') do_units(WX_UNITS_IMPERIAL);
            else if (ch == 'c') do_units(WX_UNITS_METRIC);
            else if (ch == 'r') do_refresh();
            else if (ch == 'q') g_quitting = 1;
            break;
        }
        }
        break;
    }

    case TRACE_INPUT_MOUSE_POS:
        g_mx = mouse_lx(b);
        g_my = mouse_ly(c);
        g_mouseOver = flags & 1;
        g_haveMouse = 1;
        if (g_picker) {
            int r = picker_row_at(g_mx, g_my);
            if (r >= 0) g_pickSel = r;
        } else if (g_mouseOver && inside(&R_CHART, g_mx, g_my) && have_data() && g_my > R_CHART.y + 20) {
            g_hover = chart_hour_at(g_mx);
            g_hoverFromKey = 0;
        } else if (!g_hoverFromKey) {
            g_hover = -1;
        }
        break;

    case TRACE_INPUT_MOUSE_BUTTON: {
        int pressed = flags & 1, btn = a;
        if (btn == 4 || btn == 5) {
            int dir = btn == 4 ? -1 : 1;
            if (g_picker) {
                if (g_placeCount > 0) { g_pickSel += dir; if (g_pickSel < 0) g_pickSel = 0; if (g_pickSel >= g_placeCount) g_pickSel = g_placeCount - 1; }
            } else if (inside(&R_DAYS, g_mx, g_my)) {
                select_day(g_selDay + dir);
            } else if (have_data()) {
                g_followNow = 0;
                set_chart_start(g_chartStart + dir * 3);
            }
            break;
        }
        if (!pressed || btn != 1 || !g_mouseOver) break;
        if (g_picker) {
            int r = picker_row_at(g_mx, g_my);
            if (r >= 0) do_pick(r);
            else if (!inside(&R_PICKER, g_mx, g_my)) { g_picker = 0; g_firstRun = 0; }
            break;
        }
        if (inside(&R_CLOSE, g_mx, g_my)) { g_quitting = 1; break; }
        if (inside(&R_REFRESH, g_mx, g_my)) { do_refresh(); break; }
        if (inside(&R_UNITS, g_mx, g_my)) {
            do_units(g_mx < R_UNITS.x + R_UNITS.w / 2 ? WX_UNITS_IMPERIAL : WX_UNITS_METRIC);
            break;
        }
        if (inside(&g_placeBtn, g_mx, g_my)) { open_picker(); break; }
        if (inside(&R_DAYS, g_mx, g_my)) {
            int d = (int)((g_mx - R_DAYS.x) / (CARD_W + CARD_GAP));
            float within = g_mx - R_DAYS.x - d * (CARD_W + CARD_GAP);
            if (within <= CARD_W) select_day(d);
            break;
        }
        break;
    }

    case 6:     // TE_IN_QUIT
        g_quitting = 1;
        break;
    default:
        break;
    }
}

void trace_update(void)
{
    if (g_quitting) trace_quit(0);
    if (g_wantS != g_S) set_scale(g_wantS);
    if (!g_frame) return;
    if (g_spriteScale != g_S) bake_sprites();

    int32_t now = trace_time_ms();
    float dt = g_lastFrameMs ? (now - g_lastFrameMs) / 1000.0f : 0;
    if (dt > 0.1f) dt = 0.1f;
    g_lastFrameMs = now;

    // Keep the chart on "now" as the hours tick over.
    if (g_followNow && have_data() && g_chartStart != now_hour_index() && g_selDay == 0)
        set_chart_start(now_hour_index());

    clip_all();
    for (int y = 0; y < g_H; y++)
        fill_d(0, y, g_W, y + 1, mixf(C_BG0, C_BG1, (float)y / g_H), 255);

    draw_hero(dt);
    draw_details();
    draw_chart();
    draw_days();
    draw_bar();
    draw_footer();
    if (g_picker) draw_picker();

    present_fitted(TRACE_PRESENT_ASPECT_4_3);
}
