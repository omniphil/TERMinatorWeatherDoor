/*
 * ansi_view.c - the weather as a 24-bit ANSI screen. See ansi_view.h.
 *
 * The screen is composed in an 80x25 cell buffer and sent as a diff against
 * what the caller already has, so a key press costs a few dozen bytes, not a
 * whole repaint. It is laid out in 79x24, one short of the screen each way,
 * so nothing can make a terminal scroll. Characters are CP437; nothing below 0x20 is ever sent (those
 * are control codes to a terminal, whatever glyph CP437 gives them).
 */
#include "ansi_view.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "door.h"
#include "wxcommon.h"

/* 79x24, not 80x25: writing the last column or row of the screen makes some
 * terminals scroll, so the layout never goes there. */
#define ROWS 24
#define COLS 79
#define CSI "\033["

/* CP437 */
#define CH_DEG    '\xF8'
#define CH_FULL   '\xDB'
#define CH_LOWER  '\xDC'
#define CH_UPPER  '\xDF'
#define CH_LIGHT  '\xB0'
#define CH_MEDIUM '\xB1'
#define CH_DARK   '\xB2'
#define CH_SQUARE '\xFE'
#define CH_DOT    '\xFA'
#define DEG "\xF8"

/* The same palette as the TRACE picture. */
#define C_BG      0x0A0F1E
#define C_BAR     0x0D1428
#define C_PANEL   0x141C38
#define C_PANEL_HI 0x1E2A52
#define C_EDGE    0x2A3862
#define C_TEXT    0xEAF0FA
#define C_TEXT2   0xA3AECB
#define C_TEXT3   0x66739A
#define C_ACCENT  0x4FD1FF
#define C_MAGENTA 0xFF5FD2
#define C_RAIN    0x4C9AFF
#define C_SUN     0xFFC53D
#define C_WARN    0xFFB547
#define C_ERR     0xFF6B6B

typedef struct { char ch; uint32_t fg, bg; } Cell;

static Cell g_scr[ROWS][COLS];
static Cell g_sent[ROWS][COLS];
static bool g_sentValid = false;

static WxForecast g_f;
static bool       g_have = false;
static int        g_units = WX_UNITS_IMPERIAL;
static int        g_selDay = 0;
static int        g_start = 0;          /* first hour in the chart */
static bool       g_followNow = true;
static char       g_status[128];
static uint32_t   g_statusCol = C_TEXT2;
static time_t     g_statusUntil = 0;

/* ------------------------------------------------------------ colour --- */

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return (uint32_t)((int)(ar + (br - ar) * t) << 16 | (int)(ag + (bg - ag) * t) << 8 | (int)(ab + (bb - ab) * t));
}

/* The TRACE picture's temperature scale, cold purple to hot red. */
static uint32_t temp_color(float c)
{
    static const struct { float t; uint32_t c; } S[] = {
        { -15, 0xA78BFA }, { -5, 0x7C9CFF }, { 3, 0x56B4FF }, { 10, 0x45D6C8 },
        { 17, 0x7ED957 }, { 23, 0xFFD447 }, { 29, 0xFF9A3D }, { 36, 0xFF5A4E }, { 45, 0xE0335E },
    };
    const int n = (int)(sizeof S / sizeof S[0]);
    if (c <= S[0].t) return S[0].c;
    for (int i = 1; i < n; i++)
        if (c <= S[i].t) return mix(S[i - 1].c, S[i].c, (c - S[i - 1].t) / (S[i].t - S[i - 1].t));
    return S[n - 1].c;
}

/* ------------------------------------------------------------ buffer --- */

static void clear(uint32_t bg)
{
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) g_scr[r][c] = (Cell){ ' ', C_TEXT, bg };
}

static void fill(int r, int c, int h, int w, uint32_t bg)
{
    for (int y = r; y < r + h && y < ROWS; y++)
        for (int x = c; x < c + w && x < COLS; x++)
            if (y >= 0 && x >= 0) g_scr[y][x] = (Cell){ ' ', C_TEXT, bg };
}

/* Writes text at (r, c), 0-based; bg 0xFFFFFFFF keeps what's there. Returns the column after it. */
#define KEEP 0xFFFFFFFFu
static int put(int r, int c, const char *s, uint32_t fg, uint32_t bg)
{
    if (r < 0 || r >= ROWS) return c;
    for (; *s; s++, c++) {
        if (c < 0) continue;
        if (c >= COLS) break;
        unsigned char ch = (unsigned char)*s;
        g_scr[r][c].ch = ch < 0x20 ? ' ' : (char)ch;
        g_scr[r][c].fg = fg;
        if (bg != KEEP) g_scr[r][c].bg = bg;
    }
    return c;
}

static int putf(int r, int c, uint32_t fg, uint32_t bg, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return put(r, c, buf, fg, bg);
}

static void put_c(int r, int c0, int w, const char *s, uint32_t fg, uint32_t bg)
{
    int len = (int)strlen(s);
    put(r, c0 + (w - len) / 2, s, fg, bg);
}

static void put_r(int r, int cEnd, const char *s, uint32_t fg, uint32_t bg)
{
    put(r, cEnd - (int)strlen(s), s, fg, bg);
}

static void setch(int r, int c, char ch, uint32_t fg, uint32_t bg)
{
    if (r < 0 || r >= ROWS || c < 0 || c >= COLS) return;
    g_scr[r][c].ch = ch;
    g_scr[r][c].fg = fg;
    if (bg != KEEP) g_scr[r][c].bg = bg;
}

/* Sends what changed since the last flush. */
static void flush(void)
{
    static char out[96 * 1024];
    size_t n = 0;
    uint32_t fg = KEEP, bg = KEEP;
    int cr = -1, cc = -1;
#define EMIT(...) (n += (size_t)snprintf(out + n, sizeof out - n, __VA_ARGS__))
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            Cell *want = &g_scr[r][c];
            if (g_sentValid && !memcmp(want, &g_sent[r][c], sizeof *want)) continue;
            if (n == 0) EMIT(CSI "?25l");
            if (cr != r || cc != c) EMIT(CSI "%d;%dH", r + 1, c + 1);
            if (want->bg != bg) { EMIT(CSI "48;2;%u;%u;%um", (want->bg >> 16) & 255, (want->bg >> 8) & 255, want->bg & 255); bg = want->bg; }
            if (want->fg != fg && want->ch != ' ') { EMIT(CSI "38;2;%u;%u;%um", (want->fg >> 16) & 255, (want->fg >> 8) & 255, want->fg & 255); fg = want->fg; }
            out[n++] = want->ch;
            g_sent[r][c] = *want;
            cr = r;
            cc = c + 1;
            if (n > sizeof out - 256) { door_write_raw(out, n); n = 0; }
        }
    g_sentValid = true;
    if (n) door_write_raw(out, n);
#undef EMIT
}

static void invalidate(void) { g_sentValid = false; }

/* ------------------------------------------------------------- units --- */

static int temp_i(int16_t c10)
{
    float c = c10 / 10.0f;
    return (int)lroundf(g_units == WX_UNITS_IMPERIAL ? c * 9 / 5 + 32 : c);
}

static void fmt_temp(char *o, size_t n, int16_t c10)
{
    if (c10 == WX_NONE) snprintf(o, n, "--");
    else snprintf(o, n, "%d" DEG, temp_i(c10));
}

static void fmt_wind(char *o, size_t n, uint16_t k10)
{
    float k = k10 / 10.0f;
    if (g_units == WX_UNITS_IMPERIAL) snprintf(o, n, "%d mph", (int)lroundf(k * 0.621371f));
    else snprintf(o, n, "%d km/h", (int)lroundf(k));
}

static void fmt_pressure(char *o, size_t n, uint16_t h10)
{
    float h = h10 / 10.0f;
    if (g_units == WX_UNITS_IMPERIAL) snprintf(o, n, "%.2f in", h * 0.02953f);
    else snprintf(o, n, "%d hPa", (int)lroundf(h));
}

static void fmt_vis(char *o, size_t n, uint16_t m)
{
    float v = g_units == WX_UNITS_IMPERIAL ? m / 1609.34f : m / 1000.0f;
    const char *u = g_units == WX_UNITS_IMPERIAL ? "mi" : "km";
    if (v >= 9.95f) snprintf(o, n, "%d %s", (int)lroundf(v), u);
    else snprintf(o, n, "%.1f %s", v, u);
}

typedef struct { int year, mon, mday, wday, hour, min; } LocalTime;

static long long floordiv(long long a, long long b) { long long q = a / b; return (a % b < 0) ? q - 1 : q; }

static LocalTime local_time(long long utc)
{
    LocalTime lt;
    long long t = utc + g_f.cur.utcOffset;
    long long days = floordiv(t, 86400), secs = t - days * 86400;
    lt.hour = (int)(secs / 3600);
    lt.min = (int)(secs % 3600 / 60);
    lt.wday = (int)(((days + 4) % 7 + 7) % 7);
    long long z = days + 719468, era = floordiv(z, 146097);
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    lt.mday = (int)(doy - (153 * mp + 2) / 5 + 1);
    lt.mon = (int)(mp < 10 ? mp + 3 : mp - 9);
    lt.year = (int)(yoe + era * 400 + (lt.mon <= 2));
    return lt;
}

static const char *const WDAY[7]  = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const WDAY3[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char *const MON3[13] = { "", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void fmt_clock(char *o, size_t n, long long utc, int withMin)
{
    LocalTime lt = local_time(utc);
    if (g_units == WX_UNITS_METRIC) { snprintf(o, n, "%02d:%02d", lt.hour, withMin ? lt.min : 0); return; }
    int h = lt.hour % 12 ? lt.hour % 12 : 12;
    if (withMin) snprintf(o, n, "%d:%02d %s", h, lt.min, lt.hour < 12 ? "AM" : "PM");
    else snprintf(o, n, "%d %s", h, lt.hour < 12 ? "AM" : "PM");
}

static int now_hour(void)
{
    long long i = floordiv((long long)time(NULL) - g_f.hourly.start, 3600);
    if (i < 0) i = 0;
    if (i >= g_f.hourly.count) i = g_f.hourly.count - 1;
    return (int)i;
}

static int today_index(void)
{
    long long t = time(NULL);
    for (int i = 0; i < g_f.daily.count; i++)
        if (t >= g_f.daily.d[i].date && t < (long long)g_f.daily.d[i].date + 86400) return i;
    return 0;
}

/* --------------------------------------------------------------- art --- */

/* Five rows of 13, coloured per character by a map: y sun, w light cloud,
 * g grey cloud, d dark cloud, b rain, s snow, Y bolt, m moon, f fog. A map row
 * shorter than its art row repeats its last letter. */
typedef struct { const char *art[5]; const char *map[5]; } Art;

static const Art ART_SUN = {{
    "    \\   /    ",
    "     .-.     ",
    "  - (   ) -  ",
    "     `-'     ",
    "    /   \\    " }, { "y", "y", "y", "y", "y" }};
static const Art ART_MOON = {{
    "     .--.    ",
    "    /  .'    ",
    "   |  (      ",
    "    \\  '.    ",
    "     '--'    " }, { "m", "m", "m", "m", "m" }};
static const Art ART_PARTLY = {{
    "   \\  /      ",
    " _ /\"\".-.    ",
    "   \\_(   ).  ",
    "   /(___(__) ",
    "             " }, { "y", "yyyyyyw", "yyyyw", "yyyw", "w" }};
static const Art ART_PARTLY_NIGHT = {{
    "    .-.      ",
    "   ( (.-.    ",
    "    `(   ).  ",
    "    (___(__) ",
    "             " }, { "m", "mmmmmw", "mmmmw", "w", "w" }};
static const Art ART_CLOUDY = {{
    "             ",
    "     .--.    ",
    "  .-(    ).  ",
    " (___.__)__) ",
    "             " }, { "g", "g", "g", "g", "g" }};
static const Art ART_FOG = {{
    "             ",
    " _ - _ - _ - ",
    "  _ - _ - _  ",
    " _ - _ - _ - ",
    "             " }, { "f", "f", "f", "f", "f" }};
static const Art ART_DRIZZLE = {{
    "     .-.     ",
    "    (   ).   ",
    "   (___(__)  ",
    "    ' ' ' '  ",
    "   ' ' ' '   " }, { "g", "g", "g", "b", "b" }};
static const Art ART_RAIN = {{
    "     .-.     ",
    "    (   ).   ",
    "   (___(__)  ",
    "    ,','.','  ",
    "   ,',',','  " }, { "g", "g", "g", "b", "b" }};
static const Art ART_HEAVY = {{
    "     .-.     ",
    "    (   ).   ",
    "   (___(__)  ",
    "  ,',',',',' ",
    "  ,',',',',  " }, { "d", "d", "d", "b", "b" }};
static const Art ART_FREEZE = {{
    "     .-.     ",
    "    (   ).   ",
    "   (___(__)  ",
    "    ,*,*,*,  ",
    "   *,*,*,*   " }, { "g", "g", "g", "bbbbsbbbsbbbs", "bbbbsbbbsbbbs" }};
static const Art ART_SNOW = {{
    "     .-.     ",
    "    (   ).   ",
    "   (___(__)  ",
    "    *  *  *  ",
    "   *  *  *   " }, { "g", "g", "g", "s", "s" }};
static const Art ART_HEAVYSNOW = {{
    "     .-.     ",
    "    (   ).   ",
    "   (___(__)  ",
    "   * * * * * ",
    "  * * * * *  " }, { "g", "g", "g", "s", "s" }};
static const Art ART_THUNDER = {{
    "     .-.     ",
    "    (   ).   ",
    "   (___(__)  ",
    "    ,'/_,'   ",
    "   ,' /,'    " }, { "d", "d", "d", "bbbbbbYYbb", "bbbbbbYbb" }};

static uint32_t art_color(char k)
{
    switch (k) {
    case 'y': return C_SUN;
    case 'w': return 0xEEF2F8;
    case 'g': return 0xB6BFCE;
    case 'd': return 0x8791A6;
    case 'b': return 0x6AB2FF;
    case 's': return 0xFFFFFF;
    case 'Y': return 0xFFE14D;
    case 'm': return 0xF2E9C9;
    case 'f': return 0xAEB7C6;
    default:  return C_TEXT;
    }
}

static const Art *art_for(int code, int day)
{
    switch (kind_of(code)) {
    case K_CLEAR:     return day ? &ART_SUN : &ART_MOON;
    case K_MOSTLY:
    case K_PARTLY:    return day ? &ART_PARTLY : &ART_PARTLY_NIGHT;
    case K_CLOUDY:    return &ART_CLOUDY;
    case K_FOG:       return &ART_FOG;
    case K_DRIZZLE:   return &ART_DRIZZLE;
    case K_RAIN:      return &ART_RAIN;
    case K_HEAVY:     return &ART_HEAVY;
    case K_FREEZE:    return &ART_FREEZE;
    case K_SNOW:      return &ART_SNOW;
    case K_HEAVYSNOW: return &ART_HEAVYSNOW;
    case K_THUNDER:   return &ART_THUNDER;
    }
    return &ART_CLOUDY;
}

/* ---------------------------------------------------------- animation --- */

/* The current-conditions art moves: rain and snow fall, fog drifts, the sun's
 * rays shimmer, a storm flashes and stars twinkle. Only the cells that change
 * go down the line (flush() sends a diff), so a frame costs a few hundred
 * bytes. ANIM_MS is one frame. */
#define ANIM_MS 330

static long anim_frame(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000L + ts.tv_nsec / 1000000L) / ANIM_MS;
}

static unsigned hash3(int a, int b, int c)
{
    unsigned h = (unsigned)a * 73856093u ^ (unsigned)b * 19349663u ^ (unsigned)c * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return h ^ (h >> 15);
}

static const Art ART_SUN2 = {{
    "      |      ",
    "  `. .-. .'  ",
    " -- (   ) -- ",
    "  .' `-' `.  ",
    "      |      " }, { "y", "y", "y", "y", "y" }};

/* The art, animated. f is the frame number. */
static void draw_weather_art(int r, int c, int code, int day, long f)
{
    Kind k = kind_of(code);
    const Art *a = art_for(code, day);
    if (k == K_CLEAR && day && (f / 2) % 2) a = &ART_SUN2;

    int precip = k == K_DRIZZLE || k == K_RAIN || k == K_HEAVY || k == K_FREEZE ||
                 k == K_SNOW || k == K_HEAVYSNOW || k == K_THUNDER;
    /* A storm flashes for two frames out of every twelve or so. */
    int flash = k == K_THUNDER && (hash3((int)(f / 12), 7, 7) % 12 == f % 12 || hash3((int)(f / 12), 7, 7) % 12 + 1 == f % 12);

    /* Clouds drift a column or two and back, about once a second. */
    static const int DRIFT[4] = { 0, 1, 2, 1 };
    int drift = DRIFT[(f / 3) % 4];
    for (int i = 0; i < (precip ? 3 : 5); i++) {
        const char *row = a->art[i], *m = a->map[i];
        size_t ml = strlen(m);
        /* Fog slides: each band drifts its own way. */
        int shift = k == K_FOG ? (int)((f / 2 + i) % 2) * (i % 2 ? 1 : -1) : 0;
        for (int j = 0; row[j]; j++) {
            if (row[j] == ' ') continue;
            char key = m[(size_t)j < ml ? (size_t)j : ml - 1];
            int cloud = key == 'w' || key == 'g' || key == 'd';
            uint32_t col = art_color(key);
            if (flash) col = mix(col, 0xFFFFFF, 0.7f);
            /* The sun's rays shimmer between yellow and white. */
            if (key == 'y' && row[j] != '(' && row[j] != ')')
                col = mix(C_SUN, 0xFFF4C8, (float)((hash3(i, j, (int)f) % 100)) / 160.0f);
            /* The moon glows a little brighter and dimmer. */
            if (key == 'm')
                col = mix(art_color('m'), 0xFFFFFF, 0.25f + 0.25f * (float)((hash3(1, 2, (int)(f / 3)) % 100)) / 100.0f);
            setch(r + i, c + j + shift + (cloud ? drift : 0), row[j], col, KEEP);
        }
    }
    if (!precip) return;

    /* Rain and snow: a drop at (row, x) this frame is at (row+1, x-1) the next,
     * so the key (x + row, frame - row) is the same drop all the way down. */
    for (int i = 3; i < 5; i++)
        for (int x = 2; x < 12; x++) {
            char ch = 0;
            uint32_t col = art_color('b');
            if (k == K_SNOW || k == K_HEAVYSNOW) {
                long g = f / 2;                               /* snow falls slower */
                int sway = (int)((g + x) % 3 == 0);
                unsigned h = hash3(x + sway, (int)(g - i), 3);
                if (h % 100 < (k == K_HEAVYSNOW ? 38u : 24u)) { ch = h & 1 ? '*' : '.'; col = art_color('s'); }
            } else {
                unsigned h = hash3(x + i, (int)(f - i), 1);
                unsigned density = k == K_DRIZZLE ? 20 : k == K_HEAVY ? 48 : k == K_THUNDER ? 40 : 32;
                if (h % 100 < density) {
                    ch = k == K_DRIZZLE ? '.' : i == 3 ? '\'' : ',';
                    if (k == K_FREEZE && h % 3 == 0) { ch = '*'; col = art_color('s'); }
                }
            }
            if (ch) setch(r + i, c + x, ch, col, KEEP);
        }
    if (k == K_THUNDER) {
        /* The bolt, bright in a flash and a dim glow otherwise. */
        uint32_t bc = flash ? 0xFFFFFF : mix(0xFFE14D, 0x3A3A20, 0.55f);
        setch(r + 3, c + 6, '/', bc, KEEP);
        setch(r + 3, c + 7, '_', bc, KEEP);
        setch(r + 4, c + 6, '/', bc, KEEP);
    }
}

/* Twinkling stars in the conditions panel on a clear night. Kept to the top
 * row and the right-hand edge, clear of the words and the numbers. */
static void draw_stars(int r0, int c0, long f)
{
    static const int SPOTS[][2] = {
        { 0, 3 }, { 0, 11 }, { 0, 19 }, { 0, 27 }, { 0, 33 }, { 0, 41 }, { 1, 14 }, { 2, 42 },
        { 3, 38 }, { 4, 43 }, { 5, 40 }, { 6, 36 }, { 6, 43 }, { 1, 37 }, { 6, 2 },
    };
    for (size_t i = 0; i < sizeof SPOTS / sizeof SPOTS[0]; i++) {
        unsigned h = hash3((int)i, (int)(f / 2), 5);
        int bright = h % 7 == 0;
        uint32_t col = mix(0x6A78A8, 0xFFFFFF, bright ? 1.0f : (float)(h % 40) / 100.0f);
        setch(r0 + SPOTS[i][0], c0 + SPOTS[i][1], bright ? '*' : CH_DOT, col, KEEP);
    }
}

/* A short word for a day card (they are 11 columns wide). */
static const char *short_desc(int code)
{
    switch (kind_of(code)) {
    case K_CLEAR:     return "Sunny";
    case K_MOSTLY:    return "Fair";
    case K_PARTLY:    return "Partly";
    case K_CLOUDY:    return "Cloudy";
    case K_FOG:       return "Fog";
    case K_DRIZZLE:   return "Drizzle";
    case K_RAIN:      return "Rain";
    case K_HEAVY:     return "Downpour";
    case K_FREEZE:    return "Ice";
    case K_SNOW:      return "Snow";
    case K_HEAVYSNOW: return "Heavy snow";
    case K_THUNDER:   return "Storms";
    }
    return "";
}

static uint32_t kind_color(int code)
{
    switch (kind_of(code)) {
    case K_CLEAR: case K_MOSTLY: return C_SUN;
    case K_PARTLY: return 0xE2D8A8;
    case K_CLOUDY: case K_FOG: return 0xB6BFCE;
    case K_SNOW: case K_HEAVYSNOW: case K_FREEZE: return 0xDDEBFF;
    case K_THUNDER: return 0xFFE14D;
    default: return 0x6AB2FF;
    }
}

/* ------------------------------------------------------- big numbers --- */

/* 5x6 pixel digits, drawn two pixel-rows to a text row with half blocks. */
static const char *const BIG[11][6] = {
    { " ### ", "#   #", "#   #", "#   #", "#   #", " ### " },
    { "  #  ", " ##  ", "  #  ", "  #  ", "  #  ", " ### " },
    { " ### ", "#   #", "   # ", "  #  ", " #   ", "#####" },
    { "#### ", "    #", " ### ", "    #", "    #", "#### " },
    { "#   #", "#   #", "#####", "    #", "    #", "    #" },
    { "#####", "#    ", "#### ", "    #", "    #", "#### " },
    { " ### ", "#    ", "#### ", "#   #", "#   #", " ### " },
    { "#####", "    #", "   # ", "  #  ", "  #  ", "  #  " },
    { " ### ", "#   #", " ### ", "#   #", "#   #", " ### " },
    { " ### ", "#   #", "#   #", " ####", "    #", " ### " },
    { "    ", "    ", "####", "    ", "    ", "    " },      /* minus */
};

/* Draws s (digits and '-'); returns the column after it. */
static int draw_big(int r, int c, const char *s, uint32_t top, uint32_t bot)
{
    for (; *s; s++) {
        int g = *s == '-' ? 10 : *s - '0';
        if (g < 0 || g > 10) continue;
        int w = (int)strlen(BIG[g][0]);
        for (int row = 0; row < 3; row++) {
            uint32_t col = mix(top, bot, row / 2.0f);
            for (int x = 0; x < w; x++) {
                int up = BIG[g][row * 2][x] == '#', dn = BIG[g][row * 2 + 1][x] == '#';
                if (up || dn) setch(r + row, c + x, up && dn ? CH_FULL : up ? CH_UPPER : CH_LOWER, col, KEEP);
            }
        }
        c += w + 1;
    }
    return c;
}

/* ------------------------------------------------------------- panels --- */

/* The current-conditions panel gets a sky of its own, like the TRACE picture. */
static void sky_colors(uint32_t *top, uint32_t *bot)
{
    long long t = time(NULL);
    int di = today_index();
    float rise = (float)((long long)g_f.daily.d[di].sunrise - t), set = (float)((long long)g_f.daily.d[di].sunset - t);
    float day = 0;
    if (-rise > -2400 && set > -2400) {
        float a = (-rise + 2400) / 4800, b = (set + 2400) / 4800;
        day = (a > 1 ? 1 : a) * (b > 1 ? 1 : b);
        if (day < 0) day = 0;
    }
    float twi = 1 - fminf(fabsf(rise), fabsf(set)) / 3600;
    if (twi < 0) twi = 0;
    Kind k = kind_of(g_f.cur.code);
    float grey = k == K_CLEAR || k == K_MOSTLY ? 0 : k == K_PARTLY ? 0.3f : 0.85f;
    *top = mix(0x070C20, 0x2A5DB5, day);
    *bot = mix(0x1B2652, 0x5B93D8, day);
    *top = mix(*top, 0x2B2A6B, twi * 0.7f * (1 - grey));
    *bot = mix(*bot, 0xB85A45, twi * 0.8f * (1 - grey));
    *top = mix(*top, mix(0x0E1118, 0x485262, day), grey);
    *bot = mix(*bot, mix(0x232835, 0x69737F, day), grey);
}

static void draw_bar(void)
{
    fill(0, 0, 1, COLS, C_BAR);
    int c = put(0, 1, "TERM", C_MAGENTA, C_BAR);
    c = put(0, c, "inator", C_ACCENT, C_BAR);
    c = put(0, c + 1, "Weather", C_TEXT, C_BAR);
    const char *place = g_have && g_f.cur.place[0] ? g_f.cur.place : "";
    if (place[0]) put(0, c + 2, "\xB3", C_EDGE, C_BAR);
    char name[40];
    snprintf(name, sizeof name, "%.38s", place);
    put(0, c + 4, name, C_TEXT, C_BAR);
    if (g_have) {
        char tm[16], buf[32];
        fmt_clock(tm, sizeof tm, time(NULL), 1);
        LocalTime lt = local_time(time(NULL));
        snprintf(buf, sizeof buf, "%s %s", WDAY3[lt.wday], tm);
        put_r(0, COLS - 1, buf, C_TEXT2, C_BAR);
    }
}

static void draw_status_line(void)
{
    fill(1, 0, 1, COLS, C_BG);
    if (g_status[0] && time(NULL) < g_statusUntil) {
        put(1, 2, g_status, g_statusCol, C_BG);
    }
    /* Open-Meteo's licence (CC BY 4.0) asks for the credit. */
    int c = COLS - 2 - 14;
    put(1, c, "Open-Meteo.com", C_TEXT3, C_BG);
    if (g_have && g_f.cur.fetchedAt && !(g_status[0] && time(NULL) < g_statusUntil)) {
        char tm[16], buf[40];
        fmt_clock(tm, sizeof tm, g_f.cur.fetchedAt, 1);
        snprintf(buf, sizeof buf, "Updated %s", tm);
        setch(1, c - 2, CH_DOT, C_TEXT3, C_BG);
        put_r(1, c - 3, buf, C_TEXT3, C_BG);
    }
}

static void draw_current(void)
{
    const int r0 = 2, h = 7, c0 = 1, w = 45;
    uint32_t top, bot;
    sky_colors(&top, &bot);
    for (int i = 0; i < h; i++) fill(r0 + i, c0, 1, w, mix(top, bot, i / (float)(h - 1)));

    const WxCurrent *cu = &g_f.cur;
    long f = anim_frame();
    Kind k = kind_of(cu->code);
    if (!cu->isDay && (k == K_CLEAR || k == K_MOSTLY || k == K_PARTLY)) draw_stars(r0, c0, f);
    draw_weather_art(r0 + 1, c0 + 1, cu->code, cu->isDay, f);

    char num[16];
    snprintf(num, sizeof num, "%d", temp_i(cu->temp10));
    uint32_t tc = temp_color(cu->temp10 / 10.0f);
    int x = draw_big(r0 + 1, c0 + 17, num, mix(tc, 0xFFFFFF, 0.55f), tc);
    put(r0 + 1, x - 1, g_units == WX_UNITS_IMPERIAL ? DEG "F" : DEG "C", C_TEXT, KEEP);

    put(r0 + 4, c0 + 17, desc_of(cu->code, cu->isDay), C_TEXT, KEEP);
    char feels[16], hi[16], lo[16];
    fmt_temp(feels, sizeof feels, cu->feels10);
    int di = today_index();
    fmt_temp(hi, sizeof hi, g_f.daily.d[di].hi10);
    fmt_temp(lo, sizeof lo, g_f.daily.d[di].lo10);
    putf(r0 + 5, c0 + 17, 0xD0D8E8, KEEP, "Feels like %s", feels);
    int cx = put(r0 + 6, c0 + 17, "High ", 0xB8C2D8, KEEP);
    cx = put(r0 + 6, cx, hi, temp_color(g_f.daily.d[di].hi10 / 10.0f), KEEP);
    cx = put(r0 + 6, cx + 2, "Low ", 0xB8C2D8, KEEP);
    put(r0 + 6, cx, lo, temp_color(g_f.daily.d[di].lo10 / 10.0f), KEEP);
}

static void detail(int r, int c, const char *label, const char *value, uint32_t col)
{
    put(r, c, label, C_TEXT3, KEEP);
    put(r, c + 12, value, col, KEEP);
}

static void draw_details(void)
{
    const int r0 = 2, c0 = 47, w = 32;
    fill(r0, c0, 7, w, C_PANEL);
    const WxCurrent *cu = &g_f.cur;
    char buf[40], v[24];
    int c = c0 + 2;

    fmt_wind(v, sizeof v, cu->wind10);
    snprintf(buf, sizeof buf, "%s %s", v, compass(cu->windDir));
    detail(r0, c, "Wind", buf, C_TEXT);
    fmt_wind(v, sizeof v, cu->gust10);
    putf(r0, c + 13 + (int)strlen(buf), C_TEXT3, KEEP, "gust %d", atoi(v));

    snprintf(buf, sizeof buf, "%d%%", cu->humidity);
    detail(r0 + 1, c, "Humidity", buf, C_TEXT);
    for (int i = 0; i < 10; i++)
        setch(r0 + 1, c + 17 + i, CH_SQUARE, i < (cu->humidity + 5) / 10 ? C_RAIN : C_EDGE, KEEP);

    fmt_pressure(buf, sizeof buf, cu->pressure10);
    detail(r0 + 2, c, "Pressure", buf, C_TEXT);
    fmt_temp(buf, sizeof buf, cu->dew10);
    detail(r0 + 3, c, "Dew point", buf, C_TEXT);
    fmt_vis(buf, sizeof buf, cu->visibility);
    detail(r0 + 4, c, "Visibility", buf, C_TEXT);

    uint32_t uvc;
    const char *lvl = uv_level(cu->uv10, &uvc);
    snprintf(buf, sizeof buf, "%d", (cu->uv10 + 5) / 10);
    detail(r0 + 5, c, "UV index", buf, C_TEXT);
    put(r0 + 5, c + 12 + (int)strlen(buf) + 1, lvl, uvc, KEEP);

    /* Sunrise and sunset share a line. */
    int di = today_index();
    char set[16];
    fmt_clock(buf, sizeof buf, g_f.daily.d[di].sunrise, 1);
    fmt_clock(set, sizeof set, g_f.daily.d[di].sunset, 1);
    detail(r0 + 6, c, "Sun", buf, C_SUN);
    int x = c + 12 + (int)strlen(buf);
    x = put(r0 + 6, x + 1, "-", C_TEXT3, KEEP);
    put(r0 + 6, x + 1, set, 0xFF9F5A, KEEP);
}

#define CHART_HOURS 24

static void draw_chart(void)
{
    const int r0 = 10, c0 = 1, w = 77, h = 8;
    fill(r0, c0, h, w, C_PANEL);
    int n = g_f.hourly.count;
    int start = g_start;
    if (start > n - CHART_HOURS) start = n - CHART_HOURS;
    if (start < 0) start = 0;

    char title[48];
    long long t0 = (long long)g_f.hourly.start + (long long)start * 3600;
    LocalTime lt0 = local_time(t0);
    if (g_followNow) snprintf(title, sizeof title, "Next 24 hours");
    else if (start % 24 == 0) snprintf(title, sizeof title, "%s, %s %d", WDAY[lt0.wday], MON3[lt0.mon], lt0.mday);
    else { char tm[16]; fmt_clock(tm, sizeof tm, t0, 0); snprintf(title, sizeof title, "%s from %s", WDAY[lt0.wday], tm); }
    put(r0, c0 + 2, title, C_TEXT, KEEP);
    put(r0, c0 + w - 32, "\xDC\xDC", C_SUN, KEEP);
    put(r0, c0 + w - 29, "Temperature", C_TEXT3, KEEP);
    put(r0, c0 + w - 16, "\xB1\xB1", C_RAIN, KEEP);
    put(r0, c0 + w - 13, "Rain chance", C_TEXT3, KEEP);

    /* Each hour is a 3-column slot, from column 4. */
    const int x0 = c0 + 3, slot = 3;
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < CHART_HOURS; i++) {
        float v = (float)temp_i(g_f.hourly.h[start + i].temp10);
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (hi - lo < 4) { float m = (hi + lo) / 2; lo = m - 2; hi = m + 2; }

    /* Four text rows of bars: eight half-block levels, plus one so the
     * coldest hour still shows. */
    const int barTop = r0 + 2, barRows = 4;
    int nowI = now_hour();
    for (int i = 0; i < CHART_HOURS; i++) {
        const WxHour *hr = &g_f.hourly.h[start + i];
        float v = (float)temp_i(hr->temp10);
        int level = 1 + (int)lroundf((v - lo) / (hi - lo) * (barRows * 2 - 1));
        uint32_t col = temp_color(hr->temp10 / 10.0f);
        int x = x0 + i * slot;
        for (int rr = 0; rr < barRows; rr++) {
            int fromBottom = barRows - 1 - rr;          /* 0 = bottom text row */
            int lvLo = fromBottom * 2 + 1, lvHi = fromBottom * 2 + 2;
            char ch = level >= lvHi ? CH_FULL : level >= lvLo ? CH_LOWER : 0;
            if (!ch) continue;
            /* Darker towards the bottom, like the TRACE chart's fade. */
            uint32_t shade = mix(col, C_PANEL, fromBottom < 1 ? 0.55f : fromBottom < 2 ? 0.35f : 0.15f);
            if ((level == lvHi || level == lvLo) || rr == 0) shade = col;
            for (int k = 0; k < slot - 1; k++) setch(barTop + rr, x + k, ch, shade, KEEP);
        }
        /* Every third hour gets its temperature above the bars. */
        LocalTime lt = local_time((long long)g_f.hourly.start + (long long)(start + i) * 3600);
        if (lt.hour % 3 == 0) {
            char buf[8];
            fmt_temp(buf, sizeof buf, hr->temp10);
            put(r0 + 1, x, buf, col, KEEP);
        }
        /* Rain chance as shading. */
        int pp = hr->precipProb;
        char rc = pp < 10 ? CH_DOT : pp < 35 ? CH_LIGHT : pp < 65 ? CH_MEDIUM : CH_DARK;
        uint32_t rcol = pp < 10 ? C_EDGE : mix(0x2F5C9E, 0x7DC0FF, pp / 100.0f);
        for (int k = 0; k < slot - 1; k++) setch(r0 + 6, x + k, rc, rcol, KEEP);
        /* Labels every third hour; the day's name at midnight; "Now" at now. */
        if (start + i == nowI) {
            put(r0 + 7, x, "Now", C_BG, C_ACCENT);
        } else if (lt.hour % 3 == 0 && !(nowI >= start + i - 2 && nowI < start + i)) {
            char buf[12];
            if (lt.hour == 0) snprintf(buf, sizeof buf, "%s", WDAY3[lt.wday]);
            else fmt_clock(buf, sizeof buf, (long long)g_f.hourly.start + (long long)(start + i) * 3600, 0);
            put(r0 + 7, x, buf, lt.hour == 0 ? C_ACCENT : C_TEXT3, KEEP);
        }
    }
}

static void draw_days(void)
{
    const int r0 = 19, c0 = 1, cw = 11;
    for (int i = 0; i < g_f.daily.count && i < 7; i++) {
        const WxDay *d = &g_f.daily.d[i];
        int x = c0 + i * (cw + 0);
        bool sel = i == g_selDay;
        uint32_t bg = sel ? C_PANEL_HI : C_PANEL;
        fill(r0, x, 4, cw - 1, bg);
        LocalTime lt = local_time((long long)d->date + 43200);
        put_c(r0, x, cw - 1, i == 0 ? "TODAY" : WDAY3[lt.wday], sel ? C_ACCENT : C_TEXT2, bg);
        put_c(r0 + 1, x, cw - 1, short_desc(d->code), kind_color(d->code), bg);
        char hi[8], lo[8], both[20];
        fmt_temp(hi, sizeof hi, d->hi10);
        fmt_temp(lo, sizeof lo, d->lo10);
        snprintf(both, sizeof both, "%s %s", hi, lo);
        int bx = x + (cw - 1 - (int)strlen(both)) / 2;
        int e = put(r0 + 2, bx, hi, temp_color(d->hi10 / 10.0f), bg);
        put(r0 + 2, e + 1, lo, C_TEXT3, bg);
        char pp[12];
        snprintf(pp, sizeof pp, "%c %d%%", d->precipProb >= 30 ? CH_MEDIUM : CH_LIGHT, d->precipProb);
        put_c(r0 + 3, x, cw - 1, pp, d->precipProb >= 30 ? C_RAIN : C_TEXT3, bg);
    }
}

static int key_hint(int c, const char *key, const char *what)
{
    c = put(ROWS - 1, c, " ", C_TEXT, C_PANEL_HI);
    c = put(ROWS - 1, c, key, C_TEXT, C_PANEL_HI);
    c = put(ROWS - 1, c, " ", C_TEXT, C_PANEL_HI);
    return put(ROWS - 1, c + 1, what, C_TEXT3, C_BAR) + 2;
}

static void draw_keys(void)
{
    fill(ROWS - 1, 0, 1, COLS, C_BAR);
    int c = 1;
    c = key_hint(c, "L", "Location");
    c = key_hint(c, "U", DEG "F/" DEG "C");
    c = key_hint(c, "<>", "Hours");
    c = key_hint(c, "1-7", "Day");
    c = key_hint(c, "R", "Refresh");
    key_hint(c, "Q", "Quit");
}

static void draw_all(void)
{
    clear(C_BG);
    draw_bar();
    draw_status_line();
    if (g_have) {
        draw_current();
        draw_details();
        draw_chart();
        draw_days();
    } else {
        put_c(12, 0, COLS, "No forecast yet.  Press L to choose a location, or R to try again.", C_TEXT2, C_BG);
    }
    draw_keys();
    flush();
}

/* ------------------------------------------------------------- input --- */

enum { K_NONE = -1, K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_ENTER, K_ESC, K_BACK, K_HANGUP };

/* A key, with arrow keys decoded; K_NONE after timeout_ms. */
static int read_key(int timeout_ms)
{
    int c = door_read_char_timeout(timeout_ms);
    if (c < 0) return K_NONE;
    if (c == 27) {
        int c2 = door_read_char_timeout(60);
        if (c2 < 0) return K_ESC;
        if (c2 == '[' || c2 == 'O') {
            int c3 = door_read_char_timeout(60);
            switch (c3) {
            case 'A': return K_UP;
            case 'B': return K_DOWN;
            case 'C': return K_RIGHT;
            case 'D': return K_LEFT;
            case 'H': return K_HOME;
            default:
                /* ESC [ 1 ~ and friends: eat to the terminator. */
                while (c3 >= 0 && c3 != '~' && !(c3 >= 'A' && c3 <= 'Z') && !(c3 >= 'a' && c3 <= 'z'))
                    c3 = door_read_char_timeout(60);
                return K_NONE;
            }
        }
        return K_ESC;
    }
    if (c == '\r' || c == '\n') return K_ENTER;
    if (c == 8 || c == 127) return K_BACK;
    return c;
}

static void status(const char *s, uint32_t col, int secs)
{
    snprintf(g_status, sizeof g_status, "%s", s);
    g_statusCol = col;
    g_statusUntil = time(NULL) + secs;
}

static void set_start(int s)
{
    int max = g_f.hourly.count - CHART_HOURS;
    if (s > max) s = max;
    if (s < 0) s = 0;
    g_start = s;
    g_selDay = (s + CHART_HOURS / 2) / 24;
    if (g_selDay > 6) g_selDay = 6;
}

static void select_day(int d)
{
    if (d < 0) d = 0;
    if (d >= g_f.daily.count) d = g_f.daily.count - 1;
    g_followNow = d == 0;
    set_start(d == 0 ? now_hour() : d * 24);
    g_selDay = d;
}

static bool fetch(const WxUserPrefs *p, int maxAge)
{
    status("Fetching the forecast...", C_WARN, 60);
    draw_status_line();
    flush();
    char err[128];
    WxForecast f;
    if (!wx_forecast(p->lat, p->lon, p->place, maxAge, &f, err, sizeof err)) {
        status(err, C_ERR, 10);
        return false;
    }
    g_f = f;
    g_have = true;
    status("", C_TEXT2, 0);
    if (g_followNow) select_day(0);
    else set_start(g_start);
    return true;
}

/* ------------------------------------------------------------ picker --- */

/* The location picker: a box over the screen, typed search, arrow to choose.
 * Returns true when a place was picked (prefs updated). */
static bool pick_location(WxUserPrefs *p, bool firstRun)
{
    char query[WX_SEARCH_LEN + 1] = "", last[WX_SEARCH_LEN + 1] = "";
    int qlen = 0, sel = 0, count = -1;
    WxPlace places[WX_MAX_PLACES];
    const char *msg = firstRun ? "Where are you? Type a city, \"City, State\" or a ZIP." :
                                 "Type a city, \"City, State\" or a ZIP / postcode.";
    const int r0 = 5, c0 = 14, h = 15, w = 52;
    for (;;) {
        draw_all();
        /* A drop shadow: darken what's to the right of and below the box. */
        for (int y = r0 + 1; y <= r0 + h; y++)
            for (int x = c0 + 2; x < c0 + w + 2; x++)
                if (y == r0 + h || x >= c0 + w)
                    if (y < ROWS && x < COLS) {
                        g_scr[y][x].bg = mix(g_scr[y][x].bg, 0x000000, 0.6f);
                        g_scr[y][x].fg = mix(g_scr[y][x].fg, 0x000000, 0.6f);
                    }
        fill(r0, c0, h, w, 0x18224A);
        for (int x = c0; x < c0 + w; x++) { setch(r0, x, '\xDF', C_ACCENT, 0x18224A); }
        put(r0 + 1, c0 + 3, "Choose a location", C_TEXT, KEEP);
        put(r0 + 2, c0 + 3, msg, C_TEXT3, KEEP);
        fill(r0 + 4, c0 + 3, 1, w - 6, 0x0B1124);
        put(r0 + 4, c0 + 4, qlen ? query : "Start typing...", qlen ? C_TEXT : C_TEXT3, KEEP);
        if (count < 0) {
            put(r0 + 7, c0 + 4, "Press Enter to search.", C_TEXT3, KEEP);
        } else if (count == 0) {
            put(r0 + 7, c0 + 4, "No places found.", C_TEXT2, KEEP);
            put(r0 + 8, c0 + 4, "Check the spelling, or try a nearby city or ZIP.", C_TEXT3, KEEP);
        } else {
            for (int i = 0; i < count && i < 7; i++) {
                bool on = i == sel;
                uint32_t bg = on ? C_PANEL_HI : KEEP;
                if (on) fill(r0 + 6 + i, c0 + 2, 1, w - 4, C_PANEL_HI);
                put(r0 + 6 + i, c0 + 3, on ? "\xAF" : " ", C_ACCENT, bg);
                char nm[44];
                snprintf(nm, sizeof nm, "%.42s", places[i].name);
                put(r0 + 6 + i, c0 + 5, nm, on ? C_TEXT : C_TEXT2, bg);
            }
        }
        bool choose = count > 0 && !strcmp(query, last);
        int c = put(r0 + h - 1, c0 + 3, " Enter ", C_TEXT, C_PANEL_HI);
        c = put(r0 + h - 1, c + 1, choose ? "choose" : "search", C_TEXT3, KEEP);
        if (count > 0) {
            c = put(r0 + h - 1, c + 3, " Up/Dn ", C_TEXT, C_PANEL_HI);
            c = put(r0 + h - 1, c + 1, "move", C_TEXT3, KEEP);
        }
        c = put(r0 + h - 1, c + 3, " Esc ", C_TEXT, C_PANEL_HI);
        put(r0 + h - 1, c + 1, "cancel", C_TEXT3, KEEP);
        flush();
        /* Show a cursor in the field while typing. */
        char at[32];
        snprintf(at, sizeof at, CSI "%d;%dH" CSI "?25h", r0 + 5, c0 + 5 + qlen);
        door_write(at);

        int k = read_key(300000);
        door_write(CSI "?25l");
        if (k == K_NONE || k == K_ESC) return false;
        if (k == K_UP && count > 0 && sel > 0) sel--;
        else if (k == K_DOWN && count > 0 && sel < count - 1 && sel < 6) sel++;
        else if (k == K_BACK) { if (qlen) query[--qlen] = 0; }
        else if (k == K_ENTER) {
            if (choose) {
                snprintf(p->place, sizeof p->place, "%s", places[sel].name);
                p->lat = places[sel].lat1e4 / 1e4;
                p->lon = places[sel].lon1e4 / 1e4;
                p->havePlace = true;
                wx_save_prefs(p);
                return true;
            }
            if (!qlen) continue;
            put(r0 + 7, c0 + 4, "Searching...                         ", C_WARN, 0x18224A);
            flush();
            count = wx_search(query, places, WX_MAX_PLACES);
            snprintf(last, sizeof last, "%s", query);
            sel = 0;
            if (count < 0) { count = -1; msg = "Couldn't reach the place search. Try again."; }
        } else if (k < 127 && qlen < WX_SEARCH_LEN &&
                   (isalnum(k) || k == ' ' || k == ',' || k == '.' || k == '-' || k == '\'')) {
            /* Only what a place name uses, as in the TRACE picker. */
            char ch = (char)k;
            if (ch == ' ' && qlen == 0) continue;
            if (ch >= 'a' && ch <= 'z' && (qlen == 0 || query[qlen - 1] == ' ')) ch = (char)(ch - 32);
            query[qlen++] = ch;
            query[qlen] = 0;
        }
    }
}

/* -------------------------------------------------------------- main --- */

bool ansi_view_run(WxUserPrefs *prefs)
{
    g_units = prefs->units;
    invalidate();
    door_write(CSI "0m" CSI "2J" CSI "?25l");
    clear(C_BG);

    if (!prefs->havePlace) {
        if (pick_location(prefs, true)) g_followNow = true;
    }
    fetch(prefs, -1);
    time_t fetchedAt = time(NULL);
    int lastMinute = -1;

    for (;;) {
        draw_all();
        /* Wake for each animation frame; a key press cuts the wait short. */
        int k = read_key(ANIM_MS);
        if (k == K_NONE) {
            /* Keep the clock right, and the forecast fresh. */
            time_t now = time(NULL);
            if (now - fetchedAt > wx_config.cacheMinutes * 60) { fetch(prefs, -1); fetchedAt = now; }
            int m = (int)(now / 60);
            if (m != lastMinute) { lastMinute = m; if (g_followNow && g_have) select_day(0); }
            if (door_time_remaining() <= 0) break;
            continue;
        }
        if (k == 'q' || k == 'Q' || k == K_ESC) break;
        if (!g_have && k != 'l' && k != 'L' && k != 'r' && k != 'R') continue;
        switch (k) {
        /* The key bar says "<>", so take the characters too (and , . unshifted). */
        case K_LEFT:  case '<': case ',': g_followNow = false; set_start(g_start - 3); break;
        case K_RIGHT: case '>': case '.': g_followNow = false; set_start(g_start + 3); break;
        case K_UP:    select_day(g_selDay - 1); break;
        case K_DOWN:  select_day(g_selDay + 1); break;
        case K_HOME:  select_day(0); break;
        case '\t':    select_day((g_selDay + 1) % 7); break;
        case 'u': case 'U':
            g_units = g_units == WX_UNITS_METRIC ? WX_UNITS_IMPERIAL : WX_UNITS_METRIC;
            prefs->units = g_units;
            wx_save_prefs(prefs);
            break;
        case 'f': case 'F': g_units = prefs->units = WX_UNITS_IMPERIAL; wx_save_prefs(prefs); break;
        case 'c': case 'C': g_units = prefs->units = WX_UNITS_METRIC; wx_save_prefs(prefs); break;
        case 'r': case 'R': fetch(prefs, 120); fetchedAt = time(NULL); break;
        case 'l': case 'L':
            if (pick_location(prefs, false)) { g_followNow = true; fetch(prefs, -1); fetchedAt = time(NULL); }
            invalidate();
            door_write(CSI "0m" CSI "2J");
            break;
        default:
            if (k >= '1' && k <= '7') select_day(k - '1');
            break;
        }
    }
    door_write(CSI "0m" CSI "2J" CSI "H" CSI "?25h");
    return true;
}
