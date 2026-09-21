/*
 * json.c - a small JSON reader, enough for Open-Meteo's replies. See json.h.
 */
#include "json.h"

#include <stdlib.h>
#include <string.h>

/* Every node and string of one document lives in one arena. The first block
 * is the handle json_free() walks. */
typedef struct arena_block {
    struct arena_block *next;
    size_t used, size;
    char data[];
} arena_block;

typedef struct {
    const char *p;
    arena_block *head;
    int depth;
} parser;

#define MAX_DEPTH 64

static void *arena_alloc(parser *ps, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    arena_block *b = ps->head;
    if (!b || b->used + n > b->size) {
        size_t size = n > 32768 ? n : 32768;
        arena_block *nb = malloc(sizeof *nb + size);
        if (!nb) return NULL;
        nb->used = 0;
        nb->size = size;
        /* Keep the first block first: json_free() starts from the root node,
         * which lives in it. */
        if (b) { nb->next = b->next; b->next = nb; }
        else   { nb->next = NULL; ps->head = nb; }
        b = nb;
    }
    void *out = b->data + b->used;
    b->used += n;
    return out;
}

static void skip_ws(parser *ps)
{
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') ps->p++;
}

static json_node *new_node(parser *ps, json_type t)
{
    json_node *n = arena_alloc(ps, sizeof *n);
    if (n) { memset(n, 0, sizeof *n); n->type = t; }
    return n;
}

static void put_utf8(char **out, unsigned cp)
{
    char *o = *out;
    if (cp < 0x80) *o++ = (char)cp;
    else if (cp < 0x800) { *o++ = (char)(0xC0 | (cp >> 6)); *o++ = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | (cp >> 12));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | (cp >> 18));
        *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    }
    *out = o;
}

static int hex4(const char *s, unsigned *v)
{
    *v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        *v <<= 4;
        if (c >= '0' && c <= '9') *v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') *v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') *v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    return 1;
}

/* ps->p is on the opening quote. */
static char *parse_string(parser *ps)
{
    const char *s = ++ps->p;
    size_t len = 0;
    while (s[len] && s[len] != '"') { if (s[len] == '\\' && s[len + 1]) len++; len++; }
    if (s[len] != '"') return NULL;
    /* Escapes only ever shrink, except \u which is at most 4 bytes for 6 in. */
    char *out = arena_alloc(ps, len + 1);
    if (!out) return NULL;
    char *o = out;
    while (*ps->p && *ps->p != '"') {
        char c = *ps->p++;
        if (c != '\\') { *o++ = c; continue; }
        c = *ps->p++;
        switch (c) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u': {
            unsigned cp;
            if (!hex4(ps->p, &cp)) return NULL;
            ps->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                unsigned lo;
                if (hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ps->p += 6;
                }
            }
            put_utf8(&o, cp);
            break;
        }
        case '\0': return NULL;
        default: *o++ = c; break;       /* \" \\ \/ */
        }
    }
    if (*ps->p != '"') return NULL;
    ps->p++;
    *o = 0;
    return out;
}

static json_node *parse_value(parser *ps);

static json_node *parse_container(parser *ps, int object)
{
    if (++ps->depth > MAX_DEPTH) return NULL;
    json_node *n = new_node(ps, object ? JSON_OBJECT : JSON_ARRAY);
    if (!n) return NULL;
    char close = object ? '}' : ']';
    ps->p++;
    skip_ws(ps);
    json_node **tail = &n->child;
    if (*ps->p == close) { ps->p++; ps->depth--; return n; }
    for (;;) {
        char *key = NULL;
        skip_ws(ps);
        if (object) {
            if (*ps->p != '"') return NULL;
            key = parse_string(ps);
            if (!key) return NULL;
            skip_ws(ps);
            if (*ps->p++ != ':') return NULL;
        }
        json_node *v = parse_value(ps);
        if (!v) return NULL;
        v->key = key;
        *tail = v;
        tail = &v->next;
        n->count++;
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == close) { ps->p++; break; }
        return NULL;
    }
    ps->depth--;
    return n;
}

static json_node *parse_value(parser *ps)
{
    skip_ws(ps);
    char c = *ps->p;
    if (c == '{') return parse_container(ps, 1);
    if (c == '[') return parse_container(ps, 0);
    if (c == '"') {
        json_node *n = new_node(ps, JSON_STRING);
        if (!n || !(n->str = parse_string(ps))) return NULL;
        return n;
    }
    if (!strncmp(ps->p, "true", 4))  { ps->p += 4; json_node *n = new_node(ps, JSON_BOOL); if (n) n->num = 1; return n; }
    if (!strncmp(ps->p, "false", 5)) { ps->p += 5; return new_node(ps, JSON_BOOL); }
    if (!strncmp(ps->p, "null", 4))  { ps->p += 4; return new_node(ps, JSON_NULL); }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        double v = strtod(ps->p, &end);
        if (end == ps->p) return NULL;
        ps->p = end;
        json_node *n = new_node(ps, JSON_NUMBER);
        if (n) n->num = v;
        return n;
    }
    return NULL;
}

json_node *json_parse(const char *text)
{
    parser ps = { text, NULL, 0 };
    json_node *root = parse_value(&ps);
    if (root) {
        skip_ws(&ps);
        if (*ps.p) root = NULL;          /* trailing junk */
    }
    if (!root) {
        for (arena_block *b = ps.head, *nx; b; b = nx) { nx = b->next; free(b); }
        return NULL;
    }
    return root;
}

void json_free(json_node *root)
{
    if (!root) return;
    /* The root is the first allocation, so it sits at the start of the first
     * block's data: step back to the block header. */
    arena_block *b = (arena_block *)((char *)root - offsetof(arena_block, data));
    for (arena_block *nx; b; b = nx) { nx = b->next; free(b); }
}

const json_node *json_get(const json_node *obj, const char *key)
{
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (const json_node *c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

const json_node *json_at(const json_node *arr, int i)
{
    if (!arr || arr->type != JSON_ARRAY || i < 0) return NULL;
    const json_node *c = arr->child;
    while (c && i--) c = c->next;
    return c;
}

double json_num(const json_node *n, double fallback)
{
    return (n && (n->type == JSON_NUMBER || n->type == JSON_BOOL)) ? n->num : fallback;
}

const char *json_str(const json_node *n, const char *fallback)
{
    return (n && n->type == JSON_STRING) ? n->str : fallback;
}

bool json_is_null(const json_node *n)
{
    return !n || n->type == JSON_NULL;
}
