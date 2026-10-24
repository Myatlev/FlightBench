/* FlightBench: JSON number formatting shared by the device catalog (src/catalog.c) and the
 * emulator's control server (core/qemu/hw/arm/nexus_xr_ctl.c.inc). Plain C, no allocation.
 *
 *   fb_json_num()   an integer value (|v| < 1e15, no fraction) as an integer, any other finite
 *                   value with %.9g, NaN/Inf as null (JSON has no NaN; a bare "nan" would make
 *                   the whole line unparsable)
 *   fb_json_time()  a time in seconds with fixed 4 decimals (0.1 ms at any magnitude: %.6g lost
 *                   the millisecond after 100 s and the 10 ms after 1000 s), null when not finite
 *
 * Both write a NUL-terminated string (FB_JSON_NUM_MAX bytes are always enough) and return its
 * length (like snprintf: the length it needed if n was too small).
 *
 * Reader (the bench resolver, src/session.c): fb_json_parse() tokenizes a whole document into a
 * flat node array (one node per value, key strings included, in document order), without
 * copying: a node points into the text. A node's subtree is the `skip` nodes starting at it, so
 * a node pointer alone is enough to walk it:
 *   array   elements at n + 1, each next one at e + e->skip, `count` of them
 *   object  `count` members, each a key node (a string) followed by its value node:
 *           for (k = n + 1, i = 0; i < n->count; i++, k = (k + 1) + k[1].skip) key k, value k + 1
 * Strings keep their escapes (fb_json_str() decodes into UTF-8); numbers keep their text
 * (fb_json_number() converts; is_int = no fraction and no exponent, as Python's json tells an
 * int from a float). No allocation: the caller gives the node array; call with max == 0 first
 * to learn the count. */
#ifndef FB_JSON_H
#define FB_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_JSON_NUM_MAX 32

size_t fb_json_num(char *buf, size_t n, double v);
size_t fb_json_time(char *buf, size_t n, double t_s);

typedef enum {
    FB_JSON_NULL, FB_JSON_FALSE, FB_JSON_TRUE, FB_JSON_NUMBER, FB_JSON_STRING, FB_JSON_ARRAY,
    FB_JSON_OBJECT,
} fb_json_type_t;

typedef struct fb_json_node {
    fb_json_type_t type;
    int is_int;                 /* number: no '.', 'e' or 'E' */
    const char *text;           /* number: its text; string: the bytes between the quotes */
    size_t len;
    int skip;                   /* nodes in this subtree, itself included */
    int count;                  /* array: elements; object: members */
} fb_json_node_t;

/* Parses text[0..n) as one JSON value (RFC 8259; surrounding whitespace allowed). Returns the
 * number of nodes the document has (only the first `max` are written; call again with a larger
 * array if it is more), or -1 on a syntax error with "line L column C: what" in err. */
int fb_json_parse(const char *text, size_t n, fb_json_node_t *nodes, int max, char *err,
                  size_t errlen);
/* The value of `key` in an object (the last one if the key repeats, as Python's json keeps),
 * NULL if obj is not an object or has no such key. */
const fb_json_node_t *fb_json_get(const fb_json_node_t *obj, const char *key);
/* Array element i, NULL when out of range or not an array */
const fb_json_node_t *fb_json_at(const fb_json_node_t *arr, int i);
/* A string node decoded to UTF-8, NUL-terminated; returns the length it needs (like snprintf) */
size_t fb_json_str(const fb_json_node_t *s, char *buf, size_t n);
/* A string node equals z (after decoding) */
int fb_json_str_eq(const fb_json_node_t *s, const char *z);
double fb_json_number(const fb_json_node_t *num);
/* Python truthiness: null, false, 0, "", [] and {} are false */
int fb_json_truthy(const fb_json_node_t *v);

#ifdef __cplusplus
}
#endif
#endif
