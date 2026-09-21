/*
 * wx.h - the door's weather data: config, per-caller prefs, place search and
 * the forecast (fetched from Open-Meteo, cached on disk).
 *
 * Everything is kept metric, in the protocol structs (protocol.h), so the TRACE
 * module and the ANSI view share one forecast shape.
 */
#ifndef WX_H
#define WX_H

#include <stdbool.h>
#include <stddef.h>
#include "protocol.h"

typedef struct {
    char   place[WX_NAME_LEN];     /* where callers start before they pick */
    double lat, lon;
    int    units;                  /* WX_UNITS_* default for new callers */
    int    cacheMinutes;
} WxConfig;

typedef struct {
    bool   havePlace;              /* false until the caller picks one */
    char   place[WX_NAME_LEN];
    double lat, lon;
    int    units;
    int    display;                /* last choice on the start page: 1 TRACE, 2 ANSI, 0 none */
} WxUserPrefs;

extern WxConfig wx_config;

/* A path beside the door binary: a BBS starts doors with some other cwd. */
const char *wx_beside_exe(const char *name, char *out, size_t size);

/* Reads weather.ini beside the binary; defaults if it's missing. */
void wx_load_config(void);

/* This caller's saved place and units (saves/<handle>-<usernum>/prefs). */
void wx_load_prefs(WxUserPrefs *p);
void wx_save_prefs(const WxUserPrefs *p);

/* Looks a place up by name, "City, ST" or postcode. Returns the number found
 * (0..WX_MAX_PLACES), or -1 when the lookup itself failed (network). */
int wx_search(const char *query, WxPlace *out, int max);

/*
 * Fills f with the forecast for lat/lon, from the cache when it is younger than
 * wx_config.cacheMinutes (or than maxAgeSec when that is >= 0, for a refresh).
 * place names it on screen. False with a message in err when it can't.
 */
bool wx_forecast(double lat, double lon, const char *place, int maxAgeSec,
                 WxForecast *f, char *err, size_t errSize);

#endif
