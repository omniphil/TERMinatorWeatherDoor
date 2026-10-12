/*
 * main.c -- TERMinator Weather, as a BBS door.
 *
 * Three ways to see it, picked by the caller on the start page (the same flow as
 * the DOOM door):
 *   TRACE  the door sends a small module and TERMinator draws the weather on the
 *          caller's own machine: an animated sky, an hourly chart, mouse and all.
 *          The door only fetches the forecast and passes it down.
 *   JPEG XL the TRACE screen drawn here on the BBS and sent as JPEG XL
 *          pictures, for CTerm terminals that show them (pix_weather.c).
 *   ANSI   a 24-bit ANSI screen for every other terminal (ansi_view.c).
 *
 * The forecast comes from Open-Meteo (free, no key), fetched and cached here on
 * the BBS (wx.c). Each caller's place and units are remembered.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ansi_view.h"
#include "apc.h"
#include "door.h"
#include "jxl_enc.h"
#include "pix_weather.h"
#include "trace_door.h"
#include "wx.h"

#define CSI "\033["
#define MODULE_ID "weather"
#define SOURCE_URL "https://github.com/omniphil/TERMinatorWeatherDoor"

/* As kept in the prefs (display =): JPEG XL came later, so it's 3 there,
 * though the menu shows it second. */
enum { CHOICE_TRACE = 1, CHOICE_ANSI = 2, CHOICE_JXL = 3 };

static WxUserPrefs g_prefs;

/* ------------------------------------------------------------- screens --- */

static void cls(void)
{
    door_write(CSI "0m" CSI "2J" CSI "H");
}

static void title(void)
{
    cls();
    door_write(CSI "1;36m"
               "        ===============================================\r\n"
               "           " CSI "1;35m" "T E R M" CSI "1;36m" " i n a t o r   " CSI "1;37m" "W E A T H E R\r\n"
               CSI "1;36m"
               "        ===============================================\r\n" CSI "0m");
    door_write(CSI "1;34m" "                    BBS door by JSONBourne\r\n" CSI "0m" "\r\n");
}

static void write_terminator(void)
{
    door_write(CSI "1;35m" "TERM" CSI "1;36m" "inator" CSI "0m");
}

static void press_any_key(void)
{
    door_write(CSI "0;37m\r\n  Press any key to return to the BBS...\r\n" CSI "0m");
    door_read_char();
}

/* ---------------------------------------------------------- the module --- */

static tdoor_blob_t g_module;
static bool g_haveModule = false;

/* What the caller's terminal turned out to be. The module uses the mouse, and
 * WASM imports are static: on a TERMinator without mouse support (before 1.1.3)
 * it wouldn't start at all, so TRACE is only offered with mouse=1. */
enum { TERM_PLAIN = 0, TERM_TRACE_OLD, TERM_TRACE };

static void on_module_message(const unsigned char *data, size_t len);

static void load_module_files(void)
{
    tdoor_init(MODULE_ID, on_module_message);
    g_haveModule = tdoor_load_blob("weather.wasm", &g_module, 8 * 1024 * 1024);
}

/* Asks the terminal what it is. TERMinator answers in milliseconds; a terminal
 * without TRACE never answers, and the question gives up after half a second.
 * The start menu shows the result, so this needs no screen of its own. */
static int detect_terminal(void)
{
    if (!g_haveModule) return TERM_PLAIN;
    static const char *const need[] = { "send=1", NULL };
    if (!tdoor_detect(need)) return TERM_PLAIN;
    return tdoor_has("mouse=1") ? TERM_TRACE : TERM_TRACE_OLD;
}

/* ---------------------------------------------------- the JPEG XL mode --- */

static struct {
    bool cterm, jxl;
    bool old;           /* draws JPEG XL, but only through its cache (CTerm before 1.329): too slow here */
    int  major, minor;
    int  px_w, px_h;
} g_cterm;

/*
 * A CTerm terminal answers "who are you" (ESC [ c) with ESC [ = 67;84;101;
 * 114;109;<major>;<minor> c. Then, for one, whether it draws JPEG XL
 * (Q;JXL -> ESC [ = 1 ; 1 - n) and its screen in pixels (ESC [ ? 2 ; 0 ; w ;
 * h S), finished by a status request every terminal answers.
 */
static void detect_cterm(void)
{
    char reply[512];
    int n = 0, c;
    door_write(CSI "c");
    c = door_read_char_timeout(500);
    while (c >= 0 && n < 63) {
        reply[n++] = (char)c;
        if (c == 'c' && n > 2) break;
        c = door_read_char_timeout(200);
    }
    reply[n] = 0;
    const char *p = strstr(reply, "[=67;84;101;114;109;");
    if (!p || sscanf(p + 20, "%d;%d", &g_cterm.major, &g_cterm.minor) != 2) return;
    g_cterm.cterm = true;

    door_write(APC_PREFIX "Q;JXL" APC_END CSI "?2;1S" CSI "5n");
    n = 0;
    c = door_read_char_timeout(1500);
    while (c >= 0 && n < (int)sizeof reply - 1) {
        reply[n++] = (char)c;
        reply[n] = 0;
        if (strstr(reply, "\033[0n")) break;
        c = door_read_char_timeout(500);
    }
    reply[n] = 0;
    g_cterm.jxl = strstr(reply, "\033[=1;1-n") != NULL && jxl_enc_available();
    /* A terminal before CTerm 1.329 takes pictures only through its cache, a file written and read back for every
     * frame: SyncTERM 1.8b managed 9 a second with stalls, and keys typed during them reached the BBS afterwards
     * (2026-10-05). Those get ANSI. */
    if (g_cterm.jxl && !(g_cterm.major > 1 || (g_cterm.major == 1 && g_cterm.minor >= 329))) {
        g_cterm.jxl = false;
        g_cterm.old = true;
    }
    p = strstr(reply, "\033[?2;0;");
    if (!p || sscanf(p + 7, "%d;%d", &g_cterm.px_w, &g_cterm.px_h) != 2) {
        g_cterm.px_w = 640;     /* 80x25 at 8x16, what nearly every CTerm terminal is */
        g_cterm.px_h = 400;
    }
}

static bool cterm_at_least(int major, int minor)
{
    return g_cterm.cterm && (g_cterm.major > major || (g_cterm.major == major && g_cterm.minor >= minor));
}

/* ------------------------------------------------------- the start menu --- */

static void draw_menu(int term, int recommended)
{
    char line[32];
    title();
    door_write(CSI "0;37m  Choose how to view the weather:\r\n\r\n");
    if (term == TERM_TRACE)
        door_write(CSI "1;37m  [1] " CSI "1;32mTRACE" CSI "1;37m graphics   " CSI "0;37m(animated sky, mouse)  "
                   CSI "1;32mDETECTED\r\n");
    else if (term == TERM_TRACE_OLD)
        door_write(CSI "1;30m  [1] TRACE graphics   (animated sky, mouse)  " CSI "1;33mUPDATE " CSI "1;35mTERM"
                   CSI "1;36minator" CSI "1;33m to 1.1.3+\r\n");
    else
        door_write(CSI "1;30m  [1] TRACE graphics   (animated sky, mouse)  NOT FOUND (needs " CSI "1;35mTERM"
                   CSI "1;36minator" CSI "1;30m)\r\n");
    if (g_cterm.jxl)
        door_write(CSI "1;37m  [2] " CSI "1;32mJPEG XL" CSI "1;37m graphics " CSI "0;37m(animated sky, mouse)  "
                   CSI "1;32mDETECTED\r\n");
    else
        door_write(g_cterm.old ? CSI "1;30m  [2] JPEG XL graphics (animated sky, mouse)  " CSI "1;33mUPDATE TERMINAL\r\n"
                               : CSI "1;30m  [2] JPEG XL graphics (animated sky, mouse)  NOT FOUND\r\n");
    door_write(CSI "1;37m  [3] ANSI 24-bit      " CSI "0;37m(any modern terminal)  " CSI "1;32mSUPPORTED\r\n\r\n");

    snprintf(line, sizeof(line), "[%d]", recommended);
    door_write(CSI "0;37m  Enter = " CSI "1;37m");
    door_write(line);
    door_write(CSI "0;37m     Q = back to the BBS " CSI "0m");
}

/* Menu keys 1-3 against the choices as the prefs keep them */
static const int MENU_CHOICE[4] = { 0, CHOICE_TRACE, CHOICE_JXL, CHOICE_ANSI };

/* Returns the choice, or 0 to go back to the BBS. */
/* A terminal's answer that arrives after detection gave up (a phone on a slow link) starts with Esc, which the menu
 * would take as "leave": the door quit before the caller saw it (2026-10-11). A lone Esc still leaves; Esc followed at
 * once by [ (CSI), _ (APC), P (DCS) or ] (OSC) is a late reply and is read to its end and ignored. */
static int menu_esc_is_key(void)
{
    int c = door_read_char_timeout(80);
    if (c < 0) return 1;                        /* nothing followed: the Esc key */
    if (c == '[') {                             /* CSI: up to its final byte */
        for (int i = 0; i < 64 && (c = door_read_char_timeout(300)) >= 0; i++)
            if (c >= 0x40 && c <= 0x7E) break;
    } else if (c == '_' || c == 'P' || c == ']') {   /* string: up to ST (Esc \\) or BEL */
        int prev = 0;
        for (int i = 0; i < 4096 && (c = door_read_char_timeout(300)) >= 0; i++) {
            if (c == 7 || (prev == 27 && c == '\\')) break;
            prev = c;
        }
    }
    return 0;
}

static int choose_display(int term)
{
    bool trace = term == TERM_TRACE, jxl = g_cterm.jxl;
    int recommended = trace ? 1 : jxl ? 2 : 3;
    if (g_prefs.display == CHOICE_ANSI) recommended = 3;
    else if (g_prefs.display == CHOICE_TRACE && trace) recommended = 1;
    else if (g_prefs.display == CHOICE_JXL && jxl) recommended = 2;
    for (;;) {
        draw_menu(term, recommended);
        int c = door_read_char();
        if (c == 27 && !menu_esc_is_key()) continue;    /* a late terminal answer, not the Esc key */
        if (c < 0 || c == 'q' || c == 'Q' || c == 27) return 0;
        if (c == '\r' || c == '\n') c = '0' + recommended;
        if (c == '1' && !trace) continue;
        if (c == '2' && !jxl) continue;
        if (c >= '1' && c <= '3') {
            g_prefs.display = MENU_CHOICE[c - '0'];
            wx_save_prefs(&g_prefs);
            return g_prefs.display;
        }
    }
}

static void goodbye(void)
{
    title();
    door_write(CSI "0;37m  Thanks for checking the weather.\r\n\r\n");
    door_write("  Weather data by " CSI "1;37mOpen-Meteo.com" CSI "0;37m, licensed CC BY 4.0.\r\n");
    door_write("  Source: " CSI "1;37m" SOURCE_URL CSI "0m\r\n");
    press_any_key();
}

/* ------------------------------------------------------------ TRACE link --- */

/* What the module asked for; handled after tdoor_wait_reply() returns, so a
 * slow fetch never runs inside the library's message callback. */
static struct {
    bool ready, refresh, search, pick, units;
    char query[WX_SEARCH_LEN + 1];
    int pickIndex, unitsValue;
} g_req;

static WxPlace g_places[WX_MAX_PLACES];
static int g_placeCount = 0;

static void on_module_message(const unsigned char *data, size_t len)
{
    if (len < sizeof(WxMsgHeader)) return;
    WxMsgHeader h;
    memcpy(&h, data, sizeof h);
    const unsigned char *body = data + sizeof h;
    if (len - sizeof h < h.bytes) return;
    switch (h.type) {
    case WX_OUT_READY:   g_req.ready = true; break;
    case WX_OUT_REFRESH: g_req.refresh = true; break;
    case WX_OUT_SEARCH:
        if (h.bytes >= 1) {
            size_t n = body[0];
            if (n > h.bytes - 1) n = h.bytes - 1;
            if (n > WX_SEARCH_LEN) n = WX_SEARCH_LEN;
            memcpy(g_req.query, body + 1, n);
            g_req.query[n] = 0;
            g_req.search = true;
        }
        break;
    case WX_OUT_PICK:
        if (h.bytes >= 1) { g_req.pickIndex = body[0]; g_req.pick = true; }
        break;
    case WX_OUT_UNITS:
        if (h.bytes >= 1) { g_req.unitsValue = body[0]; g_req.units = true; }
        break;
    case WX_OUT_THEME:
        /* Only a prefs write: done right here. */
        if (h.bytes >= 1 && body[0] < WX_THEMES) { g_prefs.theme = body[0]; wx_save_prefs(&g_prefs); }
        break;
    default:
        break;
    }
}

static void send_packet(uint8_t type, const void *payload, size_t len, uint16_t count)
{
    unsigned char buf[sizeof(WxMsgHeader) + WX_MAX_PAYLOAD];
    if (len > WX_MAX_PAYLOAD) return;
    WxMsgHeader h = { type, 0, count, (uint32_t)len };
    memcpy(buf, &h, sizeof h);
    if (payload && len) memcpy(buf + sizeof h, payload, len);
    tdoor_send("msg", buf, sizeof h + len);
}

static void send_status(int kind, const char *text)
{
    unsigned char buf[2 + 120];
    size_t n = strlen(text);
    if (n > 120) n = 120;
    buf[0] = (unsigned char)kind;
    buf[1] = (unsigned char)n;
    memcpy(buf + 2, text, n);
    send_packet(WX_IN_STATUS, buf, 2 + n, 1);
}

static void send_prefs(void)
{
    WxPrefs p = { (uint8_t)g_prefs.units, (uint8_t)(g_prefs.havePlace ? 0 : 1), (uint8_t)g_prefs.theme, 0 };
    send_packet(WX_IN_PREFS, &p, sizeof p, 1);
}

static void send_forecast(int maxAge)
{
    WxForecast f;
    char err[128];
    send_status(WX_STATUS_BUSY, "Fetching the forecast...");
    if (!wx_forecast(g_prefs.lat, g_prefs.lon, g_prefs.place, maxAge, &f, err, sizeof err)) {
        send_status(WX_STATUS_ERROR, err);
        return;
    }
    f.cur.doorNow = (uint32_t)time(NULL);
    send_packet(WX_IN_CURRENT, &f.cur, sizeof f.cur, 1);
    send_packet(WX_IN_HOURLY, &f.hourly, sizeof f.hourly, f.hourly.count);
    send_packet(WX_IN_DAILY, &f.daily, sizeof f.daily, f.daily.count);
    send_status(WX_STATUS_INFO, "Forecast updated");
}

static void handle_requests(time_t *lastFetch)
{
    if (g_req.ready) {
        g_req.ready = false;
        send_prefs();
        send_forecast(-1);
        *lastFetch = time(NULL);
    }
    if (g_req.search) {
        g_req.search = false;
        int n = wx_search(g_req.query, g_places, WX_MAX_PLACES);
        if (n < 0) {
            g_placeCount = 0;
            send_status(WX_STATUS_ERROR, "Couldn't reach the place search. Try again.");
        } else {
            g_placeCount = n;
            send_packet(WX_IN_PLACES, g_places, (size_t)n * sizeof(WxPlace), (uint16_t)n);
        }
    }
    if (g_req.pick) {
        g_req.pick = false;
        if (g_req.pickIndex >= 0 && g_req.pickIndex < g_placeCount) {
            const WxPlace *p = &g_places[g_req.pickIndex];
            snprintf(g_prefs.place, sizeof g_prefs.place, "%s", p->name);
            g_prefs.lat = p->lat1e4 / 1e4;
            g_prefs.lon = p->lon1e4 / 1e4;
            g_prefs.havePlace = true;
            wx_save_prefs(&g_prefs);
            send_forecast(-1);
            *lastFetch = time(NULL);
        }
    }
    if (g_req.units) {
        g_req.units = false;
        g_prefs.units = g_req.unitsValue == WX_UNITS_METRIC ? WX_UNITS_METRIC : WX_UNITS_IMPERIAL;
        wx_save_prefs(&g_prefs);
    }
    if (g_req.refresh) {
        g_req.refresh = false;
        /* A refresh within two minutes of the last fetch uses the cache: the
         * forecast doesn't change that fast, and the API is shared. */
        send_forecast(120);
        *lastFetch = time(NULL);
    }
}

static int play_trace(void)
{
    title();
    door_write(CSI "1;32m  ");
    write_terminator();
    door_write(CSI "1;32m found. Opening the weather...\r\n" CSI "0m");

    if (!tdoor_open(&g_module, "exclusive=1", NULL)) {
        door_write(CSI "1;33m\r\n  The weather couldn't be started on your terminal.\r\n" CSI "0m");
        press_any_key();
        return 1;
    }
    /* The picture covers the screen; clear what's under it so nothing flashes
     * up when it closes. */
    cls();
    tdoor_send_text("start");

    memset(&g_req, 0, sizeof g_req);
    time_t began = time(NULL), lastFetch = time(NULL);
    for (;;) {
        int reply = tdoor_wait_reply(200);
        if (reply == TDOOR_REPLY_CLOSED || !tdoor_is_open()) break;
        handle_requests(&lastFetch);
        /* Keep an open screen fresh. */
        if (time(NULL) - lastFetch > wx_config.cacheMinutes * 60) {
            send_forecast(-1);
            lastFetch = time(NULL);
        }
        if (door_time_remaining() <= 0) break;
    }
    tdoor_close("quit", 2000);
    (void)began;

    /* A module that failed says so: a non-zero code, usually with a reason.
     * A quick quit is just a quick quit. */
    if (tdoor_last_close_code() != 0 || tdoor_last_close_reason()[0]) {
        /* The picture opened and shut straight away: the module didn't run on the
         * caller's machine. Say why instead of pretending all is well. */
        title();
        door_write(CSI "1;31m  The picture stopped with an error.\r\n" CSI "0m");
        const char *why = tdoor_last_close_reason();
        char line[200];
        snprintf(line, sizeof line, CSI "1;33m  Reason: %s\r\n" CSI "0m", (why && *why) ? why : "(none given)");
        door_write(line);
        snprintf(line, sizeof line, CSI "1;30m  Your TERMinator offers: %.100s\r\n" CSI "0m", tdoor_info());
        door_write(line);
        door_write(CSI "0;37m  Please send those two lines to the sysop, or choose ANSI next time.\r\n" CSI "0m");
        press_any_key();
        return 1;
    }
    goodbye();
    return 0;
}

static int play_jxl(void)
{
    pix_caps_t caps = { cterm_at_least(1, 329), g_cterm.px_w, g_cterm.px_h };
    title();
    door_write(CSI "0;37m  Click a day to choose it, or use Up/Down, Tab or 1-7. Left/Right walk the\r\n"
               "  hourly chart (hover it with the mouse). L picks a place, U switches F/C,\r\n"
               "  T picks a theme, R refreshes. Esc or Q goes back to the BBS.\r\n\r\n");
    door_write(CSI "1;37m  Press any key to start." CSI "0m");
    if (door_read_char() < 0) return 0;
    if (!pix_weather_run(&caps, &g_prefs)) return 0;
    goodbye();
    return 0;
}

static int play_ansi(void)
{
    ansi_view_run(&g_prefs);
    goodbye();
    return 0;
}

int main(int argc, char *argv[])
{
    door_init(argc > 1 ? argv[1] : NULL);
    wx_load_config();
    wx_load_prefs(&g_prefs);
    load_module_files();

    title();
    int term = detect_terminal();
    detect_cterm();         /* always: TERMinator speaks CTerm too */

    int choice = choose_display(term);
    int status = 0;
    if (choice == CHOICE_TRACE) status = play_trace();
    else if (choice == CHOICE_JXL) status = play_jxl();
    else if (choice == CHOICE_ANSI) status = play_ansi();

    door_cleanup();
    return status;
}
