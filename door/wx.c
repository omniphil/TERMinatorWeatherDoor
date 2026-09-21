/*
 * wx.c - the door's weather data. See wx.h.
 *
 * HTTP goes through the curl command (every BBS box has it), run with fork and
 * exec so the caller's search text never passes through a shell.
 */
#include "wx.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "door.h"
#include "json.h"

#define USER_AGENT   "TERMinator-Weather-Door/1.0 (+https://github.com/omniphil)"
#define MAX_REPLY    (512 * 1024)
#define HTTP_TIMEOUT "12"

WxConfig wx_config;

/* ------------------------------------------------------------- paths --- */

const char *wx_beside_exe(const char *name, char *out, size_t size)
{
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, sizeof path - 1);
    if (len > 0) {
        path[len] = '\0';
        char *slash = strrchr(path, '/');
        if (slash) {
            slash[1] = '\0';
            snprintf(out, size, "%s%s", path, name);
            return out;
        }
    }
    snprintf(out, size, "%s", name);    /* last resort: relative to the cwd */
    return out;
}

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

/* key = value lines, '#' comments. Calls fn for each pair. */
static void read_kv(const char *path, void (*fn)(const char *k, const char *v, void *ctx), void *ctx)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        trim(line);
        trim(eq + 1);
        if (line[0]) fn(line, eq + 1, ctx);
    }
    fclose(f);
}

static void copy_name(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n > WX_NAME_LEN - 1) n = WX_NAME_LEN - 1;
    memcpy(dst, src, n);
    memset(dst + n, 0, WX_NAME_LEN - n);
}

/* ------------------------------------------------------------ config --- */

static void config_kv(const char *k, const char *v, void *ctx)
{
    (void)ctx;
    if (!strcmp(k, "default_place"))      copy_name(wx_config.place, v);
    else if (!strcmp(k, "default_lat"))   wx_config.lat = atof(v);
    else if (!strcmp(k, "default_lon"))   wx_config.lon = atof(v);
    else if (!strcmp(k, "units"))         wx_config.units = (v[0] == 'm' || v[0] == 'M') ? WX_UNITS_METRIC : WX_UNITS_IMPERIAL;
    else if (!strcmp(k, "cache_minutes")) wx_config.cacheMinutes = atoi(v);
}

void wx_load_config(void)
{
    /* A sysop who never writes weather.ini still gets a working door. */
    copy_name(wx_config.place, "New York, New York");
    wx_config.lat = 40.7128;
    wx_config.lon = -74.0060;
    wx_config.units = WX_UNITS_IMPERIAL;
    wx_config.cacheMinutes = 15;

    char path[1100];
    read_kv(wx_beside_exe("weather.ini", path, sizeof path), config_kv, NULL);
    if (wx_config.cacheMinutes < 5) wx_config.cacheMinutes = 5;    /* be kind to the API */
}

/* ------------------------------------------------------------- prefs --- */

static const char *prefs_path(char *out, size_t size)
{
    char dir[1100];
    wx_beside_exe("saves", dir, sizeof dir);
    mkdir(dir, 0755);
    char who[MAX_USERNAME];
    snprintf(who, sizeof who, "%s", door_info.handle[0] ? door_info.handle : "Player");
    for (char *c = who; *c; c++)
        if (*c == '/' || *c == '\\' || *c == ' ' || *c == '.') *c = '_';
    char sub[1300];
    snprintf(sub, sizeof sub, "%s/%s-%d", dir, who, door_info.user_record);
    mkdir(sub, 0755);
    snprintf(out, size, "%s/prefs", sub);
    return out;
}

static void prefs_kv(const char *k, const char *v, void *ctx)
{
    WxUserPrefs *p = ctx;
    if (!strcmp(k, "place"))      { copy_name(p->place, v); p->havePlace = true; }
    else if (!strcmp(k, "lat"))   p->lat = atof(v);
    else if (!strcmp(k, "lon"))   p->lon = atof(v);
    else if (!strcmp(k, "units")) p->units = atoi(v) == WX_UNITS_METRIC ? WX_UNITS_METRIC : WX_UNITS_IMPERIAL;
    else if (!strcmp(k, "display")) p->display = atoi(v);
}

void wx_load_prefs(WxUserPrefs *p)
{
    memset(p, 0, sizeof *p);
    copy_name(p->place, wx_config.place);
    p->lat = wx_config.lat;
    p->lon = wx_config.lon;
    p->units = wx_config.units;
    char path[1400];
    read_kv(prefs_path(path, sizeof path), prefs_kv, p);
}

void wx_save_prefs(const WxUserPrefs *p)
{
    char path[1400], tmp[1410];
    prefs_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    if (p->havePlace)
        fprintf(f, "place = %s\nlat = %.4f\nlon = %.4f\n", p->place, p->lat, p->lon);
    fprintf(f, "units = %d\ndisplay = %d\n", p->units, p->display);
    fclose(f);
    rename(tmp, path);      /* never leave a half-written file behind */
}

/* -------------------------------------------------------------- http --- */

/* GETs url into a malloc'd, NUL-terminated buffer. NULL on any failure. */
static char *http_get(const char *url)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return NULL; }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", 1);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        /* stdin is the caller's line: curl must never read it. */
        int devin = open("/dev/null", 0);
        if (devin >= 0) dup2(devin, STDIN_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("curl", "curl", "-sSfL", "--compressed", "--max-time", HTTP_TIMEOUT,
               "-A", USER_AGENT, url, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    size_t cap = 16384, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (!buf) break;
        if (len + 4096 + 1 > cap) {
            if (cap >= MAX_REPLY) break;
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
        ssize_t n = read(pipefd[0], buf + len, cap - len - 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!buf) return NULL;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || len == 0) { free(buf); return NULL; }
    buf[len] = 0;
    return buf;
}

static void url_encode(const char *in, char *out, size_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < size; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

/* ------------------------------------------------------------ search --- */

static const struct { const char *abbr, *name; } US_STATES[] = {
    {"AL","Alabama"},{"AK","Alaska"},{"AZ","Arizona"},{"AR","Arkansas"},{"CA","California"},
    {"CO","Colorado"},{"CT","Connecticut"},{"DE","Delaware"},{"FL","Florida"},{"GA","Georgia"},
    {"HI","Hawaii"},{"ID","Idaho"},{"IL","Illinois"},{"IN","Indiana"},{"IA","Iowa"},
    {"KS","Kansas"},{"KY","Kentucky"},{"LA","Louisiana"},{"ME","Maine"},{"MD","Maryland"},
    {"MA","Massachusetts"},{"MI","Michigan"},{"MN","Minnesota"},{"MS","Mississippi"},
    {"MO","Missouri"},{"MT","Montana"},{"NE","Nebraska"},{"NV","Nevada"},{"NH","New Hampshire"},
    {"NJ","New Jersey"},{"NM","New Mexico"},{"NY","New York"},{"NC","North Carolina"},
    {"ND","North Dakota"},{"OH","Ohio"},{"OK","Oklahoma"},{"OR","Oregon"},{"PA","Pennsylvania"},
    {"RI","Rhode Island"},{"SC","South Carolina"},{"SD","South Dakota"},{"TN","Tennessee"},
    {"TX","Texas"},{"UT","Utah"},{"VT","Vermont"},{"VA","Virginia"},{"WA","Washington"},
    {"WV","West Virginia"},{"WI","Wisconsin"},{"WY","Wyoming"},{"DC","District of Columbia"},
};

static const char *state_abbr(const char *name)
{
    for (size_t i = 0; i < sizeof US_STATES / sizeof US_STATES[0]; i++)
        if (!strcasecmp(US_STATES[i].name, name)) return US_STATES[i].abbr;
    return NULL;
}

/* Does the part after the comma ("OR", "Oregon", "UK", "France") fit this result? */
static bool qualifier_matches(const json_node *r, const char *q)
{
    if (!q[0]) return true;
    const char *admin1  = json_str(json_get(r, "admin1"), "");
    const char *country = json_str(json_get(r, "country"), "");
    const char *cc      = json_str(json_get(r, "country_code"), "");
    const char *ab      = state_abbr(admin1);
    if (!strcasecmp(q, admin1) || !strcasecmp(q, country) || !strcasecmp(q, cc)) return true;
    if (ab && !strcasecmp(q, ab)) return true;
    if (!strcasecmp(q, "UK") && !strcasecmp(cc, "GB")) return true;
    if ((!strcasecmp(q, "USA") || !strcasecmp(q, "America")) && !strcasecmp(cc, "US")) return true;
    return false;
}

/* "Portland, Oregon" in the US, "Hamburg, DE" abroad; never "Hamburg, Hamburg". */
static void place_name(const json_node *r, char *out)
{
    const char *name   = json_str(json_get(r, "name"), "?");
    const char *admin1 = json_str(json_get(r, "admin1"), "");
    const char *cc     = json_str(json_get(r, "country_code"), "");
    const char *country = json_str(json_get(r, "country"), "");
    bool us = !strcmp(cc, "US");
    char buf[160];
    if (us)
        snprintf(buf, sizeof buf, "%s%s%s", name, admin1[0] ? ", " : "", admin1);
    /* Skip a region that repeats the town ("Free and Hanseatic City of Hamburg")
     * or would push the name past what the screen shows. */
    else if (admin1[0] && !strstr(admin1, name) && strlen(name) + strlen(admin1) < 30)
        snprintf(buf, sizeof buf, "%s, %s, %s", name, admin1, cc[0] ? cc : country);
    else
        snprintf(buf, sizeof buf, "%s, %s", name, country[0] ? country : cc);
    copy_name(out, buf);
}

int wx_search(const char *query, WxPlace *out, int max)
{
    char name[WX_SEARCH_LEN + 1], qual[WX_SEARCH_LEN + 1] = "";
    snprintf(name, sizeof name, "%s", query);
    char *comma = strchr(name, ',');
    if (comma) {
        *comma = 0;
        snprintf(qual, sizeof qual, "%s", comma + 1);
        trim(qual);
    }
    trim(name);
    if (!name[0]) return 0;

    char enc[160], url[400];
    url_encode(name, enc, sizeof enc);
    /* Ask for plenty, so a qualifier can pick Springfield, IL out of the pile. */
    snprintf(url, sizeof url,
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=%d&language=en&format=json",
             enc, qual[0] ? 30 : max);
    char *body = http_get(url);
    if (!body) return -1;
    json_node *root = json_parse(body);
    free(body);
    if (!root) return -1;

    const json_node *results = json_get(root, "results");
    int n = 0;
    /* Two passes: the ones that fit the qualifier, then (if there's room and
     * nothing fitted) the rest, so a typo in the state still finds the town. */
    for (int pass = 0; pass < 2 && n < max; pass++) {
        if (pass == 1 && (n > 0 || !qual[0])) break;
        for (const json_node *r = results ? results->child : NULL; r && n < max; r = r->next) {
            if (pass == 0 && !qualifier_matches(r, qual)) continue;
            out[n].lat1e4 = (int32_t)lround(json_num(json_get(r, "latitude"), 0) * 1e4);
            out[n].lon1e4 = (int32_t)lround(json_num(json_get(r, "longitude"), 0) * 1e4);
            place_name(r, out[n].name);
            n++;
        }
    }
    json_free(root);
    return n;
}

/* ---------------------------------------------------------- forecast --- */

#define CURRENT_FIELDS "temperature_2m,apparent_temperature,relative_humidity_2m,dew_point_2m,is_day," \
                       "weather_code,cloud_cover,wind_speed_10m,wind_direction_10m,wind_gusts_10m," \
                       "pressure_msl,precipitation,visibility,uv_index"
#define HOURLY_FIELDS  "temperature_2m,precipitation_probability,precipitation,weather_code," \
                       "wind_speed_10m,is_day,relative_humidity_2m"
#define DAILY_FIELDS   "weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max," \
                       "precipitation_sum,wind_speed_10m_max,sunrise,sunset,uv_index_max"

static int16_t t10(const json_node *n)
{
    if (json_is_null(n)) return WX_NONE;
    double v = json_num(n, 0) * 10.0;
    if (v < -32000) v = -32000;
    if (v > 32000) v = 32000;
    return (int16_t)lround(v);
}

static uint16_t u16x(const json_node *n, double scale)
{
    double v = json_num(n, 0) * scale;
    if (v < 0) v = 0;
    if (v > 65535) v = 65535;
    return (uint16_t)lround(v);
}

static uint8_t u8x(const json_node *n, double scale)
{
    double v = json_num(n, 0) * scale;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)lround(v);
}

static bool parse_forecast(const char *body, WxForecast *f)
{
    json_node *root = json_parse(body);
    if (!root) return false;
    const json_node *cur = json_get(root, "current");
    const json_node *hr  = json_get(root, "hourly");
    const json_node *dy  = json_get(root, "daily");
    if (!cur || !hr || !dy) { json_free(root); return false; }

    memset(f, 0, sizeof *f);
    WxCurrent *c = &f->cur;
    c->obsTime    = (uint32_t)json_num(json_get(cur, "time"), 0);
    c->utcOffset  = (int32_t)json_num(json_get(root, "utc_offset_seconds"), 0);
    c->lat1e4     = (int32_t)lround(json_num(json_get(root, "latitude"), 0) * 1e4);
    c->lon1e4     = (int32_t)lround(json_num(json_get(root, "longitude"), 0) * 1e4);
    c->temp10     = t10(json_get(cur, "temperature_2m"));
    c->feels10    = t10(json_get(cur, "apparent_temperature"));
    c->dew10      = t10(json_get(cur, "dew_point_2m"));
    c->pressure10 = u16x(json_get(cur, "pressure_msl"), 10);
    c->wind10     = u16x(json_get(cur, "wind_speed_10m"), 10);
    c->gust10     = u16x(json_get(cur, "wind_gusts_10m"), 10);
    c->windDir    = u16x(json_get(cur, "wind_direction_10m"), 1);
    c->precip10   = u16x(json_get(cur, "precipitation"), 10);
    c->visibility = u16x(json_get(cur, "visibility"), 1);
    c->humidity   = u8x(json_get(cur, "relative_humidity_2m"), 1);
    c->cloud      = u8x(json_get(cur, "cloud_cover"), 1);
    c->code       = u8x(json_get(cur, "weather_code"), 1);
    c->isDay      = u8x(json_get(cur, "is_day"), 1);
    c->uv10       = u8x(json_get(cur, "uv_index"), 10);

    const json_node *ht = json_get(hr, "time");
    const json_node *h_temp = json_get(hr, "temperature_2m");
    const json_node *h_pp   = json_get(hr, "precipitation_probability");
    const json_node *h_pr   = json_get(hr, "precipitation");
    const json_node *h_code = json_get(hr, "weather_code");
    const json_node *h_wind = json_get(hr, "wind_speed_10m");
    const json_node *h_day  = json_get(hr, "is_day");
    const json_node *h_hum  = json_get(hr, "relative_humidity_2m");
    int nh = ht ? ht->count : 0;
    if (nh > WX_HOURS) nh = WX_HOURS;
    f->hourly.start = (uint32_t)json_num(json_at(ht, 0), 0);
    f->hourly.count = (uint16_t)nh;
    /* Walk the arrays in step rather than indexing each one (json_at is a list walk). */
    const json_node *a = h_temp ? h_temp->child : NULL, *b = h_pp ? h_pp->child : NULL,
                    *cc = h_pr ? h_pr->child : NULL, *d = h_code ? h_code->child : NULL,
                    *e = h_wind ? h_wind->child : NULL, *g = h_day ? h_day->child : NULL,
                    *hh = h_hum ? h_hum->child : NULL;
    for (int i = 0; i < nh; i++) {
        WxHour *h = &f->hourly.h[i];
        h->temp10     = t10(a);
        h->precipProb = u8x(b, 1);
        h->precip10   = u16x(cc, 10);
        h->code       = u8x(d, 1);
        h->wind10     = u16x(e, 10);
        h->isDay      = u8x(g, 1);
        h->humidity   = u8x(hh, 1);
        a = a ? a->next : NULL; b = b ? b->next : NULL; cc = cc ? cc->next : NULL;
        d = d ? d->next : NULL; e = e ? e->next : NULL; g = g ? g->next : NULL;
        hh = hh ? hh->next : NULL;
    }

    const json_node *dt = json_get(dy, "time");
    int nd = dt ? dt->count : 0;
    if (nd > WX_DAYS) nd = WX_DAYS;
    f->daily.count = (uint16_t)nd;
    for (int i = 0; i < nd; i++) {
        WxDay *w = &f->daily.d[i];
        w->date        = (uint32_t)json_num(json_at(dt, i), 0);
        w->sunrise     = (uint32_t)json_num(json_at(json_get(dy, "sunrise"), i), 0);
        w->sunset      = (uint32_t)json_num(json_at(json_get(dy, "sunset"), i), 0);
        w->hi10        = t10(json_at(json_get(dy, "temperature_2m_max"), i));
        w->lo10        = t10(json_at(json_get(dy, "temperature_2m_min"), i));
        w->precipSum10 = u16x(json_at(json_get(dy, "precipitation_sum"), i), 10);
        w->windMax10   = u16x(json_at(json_get(dy, "wind_speed_10m_max"), i), 10);
        w->code        = u8x(json_at(json_get(dy, "weather_code"), i), 1);
        w->precipProb  = u8x(json_at(json_get(dy, "precipitation_probability_max"), i), 1);
        w->uvMax10     = u8x(json_at(json_get(dy, "uv_index_max"), i), 10);
    }
    json_free(root);
    return c->obsTime != 0 && nh > 0 && nd > 0;
}

static const char *cache_path(double lat, double lon, char *out, size_t size)
{
    char dir[1100];
    wx_beside_exe("cache", dir, sizeof dir);
    mkdir(dir, 0755);
    /* Two decimals is about a kilometre: close enough to share between callers. */
    snprintf(out, size, "%s/fc_%.2f_%.2f.json", dir, lat, lon);
    return out;
}

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0 || n > MAX_REPLY) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); buf = NULL; }
    fclose(fp);
    if (buf) buf[n] = 0;
    return buf;
}

static void write_file(const char *path, const char *data)
{
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return;
    fputs(data, fp);
    fclose(fp);
    rename(tmp, path);      /* two nodes fetching at once can't tear the file */
}

bool wx_forecast(double lat, double lon, const char *place, int maxAgeSec,
                 WxForecast *f, char *err, size_t errSize)
{
    char path[1200];
    cache_path(lat, lon, path, sizeof path);
    if (maxAgeSec < 0) maxAgeSec = wx_config.cacheMinutes * 60;

    struct stat st;
    time_t now = time(NULL);
    time_t fetched = 0;
    char *body = NULL;
    if (stat(path, &st) == 0 && now - st.st_mtime < maxAgeSec) {
        body = read_file(path);
        fetched = st.st_mtime;
    }
    if (!body) {
        char url[1024];
        snprintf(url, sizeof url,
                 "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                 "&current=" CURRENT_FIELDS "&hourly=" HOURLY_FIELDS "&daily=" DAILY_FIELDS
                 "&timezone=auto&timeformat=unixtime&forecast_days=7",
                 lat, lon);
        body = http_get(url);
        fetched = now;
        if (body) {
            write_file(path, body);
        } else if (stat(path, &st) == 0) {
            /* The network is down: an older forecast beats none. */
            body = read_file(path);
            fetched = st.st_mtime;
        }
    }
    if (!body) {
        snprintf(err, errSize, "Couldn't reach the weather service. Try again in a minute.");
        return false;
    }
    bool ok = parse_forecast(body, f);
    free(body);
    if (!ok) {
        unlink(path);       /* don't keep serving a reply we can't read */
        snprintf(err, errSize, "The weather service sent something unexpected.");
        return false;
    }
    f->cur.fetchedAt = (uint32_t)fetched;
    copy_name(f->cur.place, place);
    return true;
}
