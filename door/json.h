/*
 * json.h - a small JSON reader, enough for Open-Meteo's replies.
 *
 * Parses a whole document into a tree of nodes allocated from one arena, so a
 * reply is freed with a single json_free(). No library to install on the BBS box.
 */
#ifndef WX_JSON_H
#define WX_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } json_type;

typedef struct json_node {
    json_type type;
    char *key;                  /* set when this node is a member of an object */
    char *str;                  /* JSON_STRING */
    double num;                 /* JSON_NUMBER, and JSON_BOOL as 0/1 */
    struct json_node *child;    /* first element / member */
    struct json_node *next;     /* next sibling */
    int count;                  /* elements / members */
} json_node;

/* Parses text (NUL-terminated). NULL on malformed input. */
json_node *json_parse(const char *text);
void json_free(json_node *root);

/* Member of an object by key, or NULL. */
const json_node *json_get(const json_node *obj, const char *key);
/* Element i of an array, or NULL. */
const json_node *json_at(const json_node *arr, int i);

/* Convenience readers: the fallback when the node is missing or null. */
double json_num(const json_node *n, double fallback);
const char *json_str(const json_node *n, const char *fallback);
bool json_is_null(const json_node *n);

#endif
