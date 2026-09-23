// Wire protocol between the Weather door (on the BBS) and the TRACE module (on
// the caller's machine). Shared verbatim by both sides, and the door's ANSI view
// reads the same structs, so there is one forecast shape in the whole project.
//
// Division of labour:
//   - The DOOR owns the data. It looks places up, fetches the forecast from
//     Open-Meteo, caches it, and remembers each caller's place and units.
//   - The MODULE owns the picture. It draws everything itself (there is no asset
//     to upload), converts units for display, and sends back what the player
//     asked for: a search, a pick, a unit change, a refresh.
//
// Every value on the wire is METRIC (degrees C, km/h, hPa, mm, metres); the
// module converts. Changing units never needs a refetch.
//
// Everything is little-endian. Every message starts with a WxMsgHeader.
#pragma once
#include <stdint.h>

#define WX_PROTO_VERSION 1

// Keep every packet under TERMinator's 8 KB text-command limit once base64'd
// (4 KB -> ~5.5 KB), so the door works with or without binary frames.
// tdoor_send() sends a payload in ONE command -- it never splits.
#define WX_MAX_PAYLOAD 4096

// The module renders exactly this, and TERMinator scales it to the 80x25 rect.
// 80x25 is exactly 4:3, so 640x480 maps in with no distortion.
#define WX_SCREEN_W 640
#define WX_SCREEN_H 480

#define WX_HOURS      168     // 7 days of hourly data, starting at local midnight today
#define WX_DAYS       7
#define WX_MAX_PLACES 8
#define WX_NAME_LEN   48
#define WX_SEARCH_LEN 40

// Door -> module
enum WxMsgIn {
    WX_IN_CURRENT = 1,   // WxCurrent
    WX_IN_HOURLY  = 2,   // WxHourly
    WX_IN_DAILY   = 3,   // WxDaily
    WX_IN_PLACES  = 4,   // count * WxPlace: search results (count 0 = no matches)
    WX_IN_STATUS  = 5,   // uint8 kind (WX_STATUS_*) + uint8 len + text
    WX_IN_PREFS   = 6,   // WxPrefs
};

// Module -> door (via trace_send)
enum WxMsgOut {
    WX_OUT_READY   = 1,  // no payload - module is up, send prefs and the forecast
    WX_OUT_SEARCH  = 2,  // uint8 len + text typed into the location box
    WX_OUT_PICK    = 3,  // uint8 index into the last WX_IN_PLACES list
    WX_OUT_UNITS   = 4,  // uint8 WX_UNITS_*
    WX_OUT_REFRESH = 5,  // no payload - fetch again
    WX_OUT_THEME   = 6,  // uint8 theme (0 smooth, 1 retro tech, 2 cyberpunk, 3 hacker): remember it
};

#define WX_THEMES 4          // the module's fonts.h THEME_*

enum { WX_STATUS_INFO = 0, WX_STATUS_BUSY = 1, WX_STATUS_ERROR = 2 };
enum { WX_UNITS_IMPERIAL = 0, WX_UNITS_METRIC = 1 };

typedef struct {
    uint8_t  type;          // WxMsgIn / WxMsgOut
    uint8_t  flags;
    uint16_t count;         // element count, where the message carries an array
    uint32_t bytes;         // payload length following this header
} WxMsgHeader;

// A missing reading (Open-Meteo sent null) is WX_NONE in any int16 field.
#define WX_NONE ((int16_t)-32768)

typedef struct {
    uint32_t obsTime;       // unix UTC of the observation
    uint32_t fetchedAt;     // unix UTC when the door fetched it (the "updated" line)
    uint32_t doorNow;       // the door's clock when it sent this, so the module can keep time
    int32_t  utcOffset;     // seconds east of UTC at the place
    int32_t  lat1e4, lon1e4;
    int16_t  temp10;        // deg C x10
    int16_t  feels10;
    int16_t  dew10;
    uint16_t pressure10;    // hPa x10 (mean sea level)
    uint16_t wind10;        // km/h x10
    uint16_t gust10;
    uint16_t windDir;       // degrees the wind blows FROM
    uint16_t precip10;      // mm x10, the last hour
    uint16_t visibility;    // metres, capped at 65535
    uint8_t  humidity;      // %
    uint8_t  cloud;         // %
    uint8_t  code;          // WMO weather code
    uint8_t  isDay;
    uint8_t  uv10;          // UV index x10
    uint8_t  reserved[3];
    char     place[WX_NAME_LEN];   // "Portland, Oregon, US", NUL-padded
} WxCurrent;

typedef struct {
    int16_t  temp10;        // deg C x10
    uint16_t precip10;      // mm x10
    uint16_t wind10;        // km/h x10
    uint8_t  precipProb;    // %
    uint8_t  code;          // WMO
    uint8_t  isDay;
    uint8_t  humidity;
} WxHour;                   // 10 bytes; 168 of them = 1680

typedef struct {
    uint32_t start;         // unix UTC of hour 0 (local midnight today)
    uint16_t count;         // <= WX_HOURS
    uint16_t reserved;
    WxHour   h[WX_HOURS];
} WxHourly;

typedef struct {
    uint32_t date;          // unix UTC of local midnight that day
    uint32_t sunrise, sunset;
    int16_t  hi10, lo10;    // deg C x10
    uint16_t precipSum10;   // mm x10
    uint16_t windMax10;     // km/h x10
    uint8_t  code;          // WMO
    uint8_t  precipProb;    // %
    uint8_t  uvMax10;
    uint8_t  reserved;
} WxDay;                    // 24 bytes

typedef struct {
    uint16_t count;
    uint16_t reserved;
    WxDay    d[WX_DAYS];
} WxDaily;

typedef struct {
    int32_t lat1e4, lon1e4;
    char    name[WX_NAME_LEN];
} WxPlace;

typedef struct {
    uint8_t units;          // WX_UNITS_*
    uint8_t firstRun;       // 1 = this caller has never picked a place: open the picker
    uint8_t theme;          // the TRACE theme, 0..WX_THEMES-1
    uint8_t reserved;
} WxPrefs;

// The whole forecast as the door holds it. Never sent in one piece (it is
// bigger than one packet): WxCurrent, WxHourly and WxDaily go separately.
typedef struct {
    WxCurrent cur;
    WxHourly  hourly;
    WxDaily   daily;
} WxForecast;
