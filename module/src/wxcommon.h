// Things the TRACE module and the door's ANSI view both need, so the two
// always name the weather the same way. Pure C, no host calls.
#pragma once
#include <stdint.h>

#define WXC_GOOD 0x6EDC8C

typedef enum {
    K_CLEAR, K_MOSTLY, K_PARTLY, K_CLOUDY, K_FOG, K_DRIZZLE, K_RAIN, K_HEAVY,
    K_FREEZE, K_SNOW, K_HEAVYSNOW, K_THUNDER,
} Kind;

static inline Kind kind_of(int code)
{
    switch (code) {
    case 0: return K_CLEAR;
    case 1: return K_MOSTLY;
    case 2: return K_PARTLY;
    case 3: return K_CLOUDY;
    case 45: case 48: return K_FOG;
    case 51: case 53: case 55: return K_DRIZZLE;
    case 56: case 57: case 66: case 67: return K_FREEZE;
    case 61: case 63: case 80: case 81: return K_RAIN;
    case 65: case 82: return K_HEAVY;
    case 71: case 73: case 77: case 85: return K_SNOW;
    case 75: case 86: return K_HEAVYSNOW;
    case 95: case 96: case 99: return K_THUNDER;
    default: return code > 3 ? K_CLOUDY : K_CLEAR;
    }
}

static inline const char *desc_of(int code, int day)
{
    switch (code) {
    case 0:  return day ? "Sunny" : "Clear";
    case 1:  return day ? "Mostly sunny" : "Mostly clear";
    case 2:  return "Partly cloudy";
    case 3:  return "Overcast";
    case 45: return "Fog";
    case 48: return "Freezing fog";
    case 51: return "Light drizzle";
    case 53: return "Drizzle";
    case 55: return "Heavy drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: return "Snow showers";
    case 86: return "Heavy snow showers";
    case 95: return "Thunderstorms";
    case 96: case 99: return "Storms with hail";
    default: return "Unknown";
    }
}

static inline const char *compass(int deg)
{
    static const char *const P[16] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                       "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
    return P[((deg % 360 + 360) % 360 * 16 + 180) / 360 % 16];
}

static inline const char *uv_level(int uv10, uint32_t *col)
{
    int uv = (uv10 + 5) / 10;
    if (uv <= 2)  { *col = WXC_GOOD;   return "LOW"; }
    if (uv <= 5)  { *col = 0xFFD84D; return "MODERATE"; }
    if (uv <= 7)  { *col = 0xFF9A3D; return "HIGH"; }
    if (uv <= 10) { *col = 0xFF5C5C; return "VERY HIGH"; }
    *col = 0xC77DFF;
    return "EXTREME";
}
