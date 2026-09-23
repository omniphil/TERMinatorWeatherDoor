// Compiles the TRACE module natively with stub host functions, feeds it a real
// forecast (fetched through the door's own code, so it exercises that too), and
// writes a frame to a .ppm -- so the picture can be checked without Windows,
// TERMinator or a BBS in the loop.
//
//   make -C tools preview && ./tools/preview [options] out.ppm
//     --scale 1|2       render scale (default 2)
//     --window W H      the window size TERMinator reports (device pixels)
//     --code N          force the WMO weather code (sky and icons)
//     --hour H          pretend it is H:00 local time today
//     --hover I         hover chart hour I
//     --day D           select day D
//     --picker          open the location picker (with sample results)
//     --theme N         0 smooth, 1 retro tech, 2 cyberpunk, 3 hacker
//     --themes          the theme popup open
//     --loading         before any data arrives
//     --metric          metric units
//     --t SECONDS       animation time (clouds, rain) to simulate first
//     --lat/--lon/--place   where
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../module/src/wxmodule.c"

// The door's data code, to fetch a real forecast.
#include "../door/json.c"
#include "../door/wx.c"

// ---- the host side, stubbed -------------------------------------------------

static uint32_t *g_shot;
static int g_shotW, g_shotH;
static int32_t g_clock = 0;

void trace_present(const uint32_t *pixels, int32_t w, int32_t h, int32_t flags)
{
    (void)flags;
    free(g_shot);
    g_shot = malloc((size_t)w * h * 4);
    memcpy(g_shot, pixels, (size_t)w * h * 4);
    g_shotW = w; g_shotH = h;
}
int32_t trace_input_pending(void) { return 0; }
int32_t trace_cpu_count(void) { return 1; }
void    trace_frame_capacity(int32_t *w, int32_t *h) { *w = 3840; *h = 2400; }
void    trace_log(const char *t, int32_t n) { fwrite(t, 1, (size_t)n, stderr); fputc('\n', stderr); }
void    trace_quit(int32_t code) { (void)code; }
int32_t trace_time_ms(void) { return g_clock; }
void    trace_set_tick(int32_t hz) { (void)hz; }
void    trace_text_input(int32_t on) { (void)on; }
void    trace_mouse_mode(int32_t mode) { (void)mode; }
void    trace_pad_rumble(int32_t a, int32_t b, int32_t c, int32_t d) { (void)a;(void)b;(void)c;(void)d; }
int32_t trace_send(const void *d, int32_t n) { (void)d; return n; }
int32_t trace_send_room(void) { return 4096; }
int32_t trace_audio_write(const int16_t *f, int32_t n) { (void)f; return n; }
int32_t trace_audio_room(void) { return 4096; }
int32_t trace_store_read(void *b, int32_t n) { (void)b; (void)n; return 0; }
int32_t trace_store_write(const void *d, int32_t n) { (void)d; (void)n; return 0; }
int32_t trace_asset_size(const char *sha) { (void)sha; return 0; }
int32_t trace_asset_read(const char *sha, int32_t off, void *buf, int32_t len) { (void)sha;(void)off;(void)buf;(void)len; return 0; }

// Wrap a payload the way the door does: head "msg" + '\n' + header + body.
static void feed(uint8_t type, uint16_t count, const void *body, uint32_t len)
{
    size_t total = 4 + sizeof(WxMsgHeader) + len;
    uint8_t *buf = malloc(total);
    memcpy(buf, "msg\n", 4);
    WxMsgHeader h = { type, 0, count, len };
    memcpy(buf + 4, &h, sizeof h);
    if (body && len) memcpy(buf + 4 + sizeof h, body, len);
    trace_on_data((const char *)buf, (int32_t)total);
    free(buf);
}

int main(int argc, char **argv)
{
    int winW = 0, winH = 0;
    int scale = 2, code = -1, hour = -1, hover = -1, day = -1, picker = 0, loading = 0, metric = 0, themes = 0;
    float t = 3;
    double lat = 45.5235, lon = -122.6762;
    const char *place = "Portland, Oregon", *out = "preview.ppm";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--scale")) scale = atoi(argv[++i]);
        else if (!strcmp(a, "--window")) { winW = atoi(argv[++i]); winH = atoi(argv[++i]); }
        else if (!strcmp(a, "--code")) code = atoi(argv[++i]);
        else if (!strcmp(a, "--hour")) hour = atoi(argv[++i]);
        else if (!strcmp(a, "--hover")) hover = atoi(argv[++i]);
        else if (!strcmp(a, "--day")) day = atoi(argv[++i]);
        else if (!strcmp(a, "--picker")) picker = 1;
        else if (!strcmp(a, "--theme")) g_theme = atoi(argv[++i]);
        else if (!strcmp(a, "--themes")) themes = 1;
        else if (!strcmp(a, "--loading")) loading = 1;
        else if (!strcmp(a, "--metric")) metric = 1;
        else if (!strcmp(a, "--t")) t = (float)atof(argv[++i]);
        else if (!strcmp(a, "--lat")) lat = atof(argv[++i]);
        else if (!strcmp(a, "--lon")) lon = atof(argv[++i]);
        else if (!strcmp(a, "--place")) place = argv[++i];
        else out = a;
    }

    trace_init();
    if (winW) trace_on_resize(winW, winH);
    else trace_on_resize(scale == 2 ? 1400 : 640, scale == 2 ? 1050 : 480);
    trace_on_data("start", 5);

    if (!loading) {
        wx_load_config();
        WxForecast f;
        char err[128];
        if (!wx_forecast(lat, lon, place, -1, &f, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
        f.cur.doorNow = f.cur.obsTime;
        if (hour >= 0) {
            // Local midnight today, plus the hour asked for.
            f.cur.doorNow = f.hourly.start + (uint32_t)hour * 3600 + 600;
            int hi = hour < f.hourly.count ? hour : 0;
            f.cur.isDay = f.hourly.h[hi].isDay;
        }
        if (code >= 0) { f.cur.code = (uint8_t)code; f.cur.cloud = code <= 1 ? 10 : code == 2 ? 50 : 100; }
        WxPrefs pr = { (uint8_t)(metric ? WX_UNITS_METRIC : WX_UNITS_IMPERIAL), 0, (uint8_t)g_theme, 0 };
        feed(WX_IN_PREFS, 1, &pr, sizeof pr);
        feed(WX_IN_CURRENT, 1, &f.cur, sizeof f.cur);
        feed(WX_IN_HOURLY, 1, &f.hourly, sizeof f.hourly);
        feed(WX_IN_DAILY, 1, &f.daily, sizeof f.daily);
        uint8_t st[64] = { WX_STATUS_INFO, 0 };
        (void)st;
    }

    // Run the animation forward so clouds and rain are spread out, then draw.
    int frames = (int)(t * 30);
    for (int i = 0; i <= frames; i++) {
        g_clock = 10000 + i * 33;
        if (i == frames) {
            if (day >= 0) select_day(day);
            if (hover >= 0) { g_hover = hover; g_hoverFromKey = 1; }
            if (themes) g_themesOpen = 1;
            if (picker) {
                open_picker();
                strcpy(g_query, "Springfield"); g_qlen = (int)strlen(g_query);
                strcpy(g_lastSearch, g_query);
                const char *names[] = { "Springfield, Illinois", "Springfield, Missouri", "Springfield, Massachusetts",
                                        "Springfield, Oregon", "Springfield, Ohio" };
                for (int k = 0; k < 5; k++) {
                    memset(&g_places[k], 0, sizeof g_places[k]);
                    strcpy(g_places[k].name, names[k]);
                    g_places[k].lat1e4 = 398017 - k * 12000; g_places[k].lon1e4 = -896437 + k * 30000;
                }
                g_placeCount = 5; g_pickSel = 1;
                g_haveMouse = 1;
            }
        }
        trace_update();
    }
    if (!g_shot) { fprintf(stderr, "module never presented a frame\n"); return 1; }

    FILE *o = fopen(out, "wb");
    fprintf(o, "P6\n%d %d\n255\n", g_shotW, g_shotH);
    for (int i = 0; i < g_shotW * g_shotH; i++) {
        uint32_t p = g_shot[i];
        uint8_t rgb[3] = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p };
        fwrite(rgb, 1, 3, o);
    }
    fclose(o);
    fprintf(stderr, "wrote %s (%dx%d)\n", out, g_shotW, g_shotH);
    return 0;
}
