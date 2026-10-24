/* JSON number writer (fb/json.h): integers stay integers, times keep 0.1 ms at any magnitude,
 * NaN/Inf never produce invalid JSON; the catalog JSON uses it. The reader: node layout, object
 * lookup and iteration, string decoding, errors with line/column, the catalog read back. */
#include "fb/json.h"
#include "fb/device.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void num(double v, const char *want)
{
    char b[FB_JSON_NUM_MAX];
    size_t k = fb_json_num(b, sizeof b, v);
    CHECK(!strcmp(b, want) && k == strlen(want), "num(%.17g) = '%s', want '%s'", v, b, want);
}

static void tim(double v, const char *want)
{
    char b[FB_JSON_NUM_MAX];
    size_t k = fb_json_time(b, sizeof b, v);
    CHECK(!strcmp(b, want) && k == strlen(want), "time(%.17g) = '%s', want '%s'", v, b, want);
}

static fb_json_node_t nodes[256];

static int parse(const char *s)
{
    char err[96];
    int n = fb_json_parse(s, strlen(s), nodes, 256, err, sizeof err);
    CHECK(n > 0, "parse '%s': %s", s, err);
    return n;
}

static void bad(const char *s, const char *want)
{
    char err[96];
    int n = fb_json_parse(s, strlen(s), nodes, 256, err, sizeof err);
    CHECK(n == -1 && strstr(err, want), "parse '%s' = %d '%s', want error '%s'", s, n, err, want);
}

static void reader(void)
{
    char b[64];
    /* node layout: one per value and key, skip = subtree size, count = children */
    int n = parse(" {\"a\": [1, 2.5, -3e2], \"b\": {\"c\": null}, \"d\": \"x\", \"e\": true} ");
    CHECK(n == 14, "nodes %d", n);
    CHECK(nodes[0].type == FB_JSON_OBJECT && nodes[0].count == 4 && nodes[0].skip == 14, "root");
    const fb_json_node_t *a = fb_json_get(nodes, "a");
    CHECK(a && a->type == FB_JSON_ARRAY && a->count == 3 && a->skip == 4, "a");
    CHECK(fb_json_at(a, 0)->is_int && !fb_json_at(a, 1)->is_int && !fb_json_at(a, 2)->is_int, "is_int");
    CHECK(fb_json_number(fb_json_at(a, 1)) == 2.5 && fb_json_number(fb_json_at(a, 2)) == -300, "numbers");
    CHECK(!fb_json_at(a, 3) && !fb_json_at(nodes, 0), "at out of range / not an array");
    CHECK(fb_json_get(fb_json_get(nodes, "b"), "c")->type == FB_JSON_NULL, "nested");
    CHECK(fb_json_str_eq(fb_json_get(nodes, "d"), "x") && fb_json_get(nodes, "e")->type == FB_JSON_TRUE, "d, e");
    CHECK(!fb_json_get(nodes, "z") && !fb_json_get(a, "a"), "missing key / not an object");
    /* member iteration in document order */
    const char *want[] = { "a", "b", "d", "e" };
    const fb_json_node_t *k = nodes + 1;
    for (int i = 0; i < nodes[0].count; i++, k = (k + 1) + k[1].skip) {
        CHECK(fb_json_str_eq(k, want[i]), "member %d", i);
    }
    /* a repeated key: the last value, as Python's json */
    parse("{\"k\": 1, \"k\": 2}");
    CHECK(fb_json_number(fb_json_get(nodes, "k")) == 2, "last duplicate");
    /* strings: escapes and \u (with a surrogate pair) decoded to UTF-8, lookups on decoded keys */
    parse("{\"t\\u0061b\": \"q\\\"\\\\\\/\\n\\u00e9\\ud83d\\ude00\"}");
    CHECK(fb_json_get(nodes, "tab") != NULL, "escaped key");
    size_t l = fb_json_str(fb_json_get(nodes, "tab"), b, sizeof b);
    CHECK(l == 11 && !strcmp(b, "q\"\\/\n\xc3\xa9\xf0\x9f\x98\x80"), "decoded '%s' (%zu)", b, l);
    CHECK(fb_json_str(fb_json_get(nodes, "tab"), b, 3) == 11 && !strcmp(b, "q\""), "truncated '%s'", b);
    /* truthiness as Python's */
    parse("[null, false, 0, 0.0, \"\", [], {}, true, 1, \"0\", [0], {\"a\": 0}]");
    for (int i = 0; i < 12; i++) {
        CHECK(fb_json_truthy(fb_json_at(nodes, i)) == (i >= 7), "truthy %d", i);
    }
    /* counting: max 0 gives the size, nothing written */
    CHECK(fb_json_parse("[1,[2,3]]", 9, NULL, 0, NULL, 0) == 5, "count only");
    /* errors with where */
    bad("", "line 1 column 1: expecting a value");
    bad("{\"a\": 1,\n \"b\" 2}", "line 2 column 6: expecting ':'");
    bad("[1, 2", "expecting ',' or ']'");
    bad("{\"a\": 1} x", "extra data");
    bad("\"abc", "unterminated string");
    bad("\"a\\x\"", "bad escape");
    bad("01", "extra data");
    bad("1.", "bad number");
    bad("[tru]", "expecting a value");
    bad("{1: 2}", "expecting a property name");
    /* malformed input: a BOM (Python's json rejects it too), truncation, nesting, escapes */
    bad("\xef\xbb\xbf{}", "expecting a value");
    bad("{\"a\": [1, {\"b\": ", "expecting a value");
    bad("\"\\u12\"", "bad \\u escape");
    bad("\"\\", "unterminated string");
    bad("\"a\tb\"", "control character");
    bad("-", "expecting a value");
    bad("1e", "bad number");
    {
        char deep[600];
        memset(deep, '[', 257);
        memset(deep + 257, ']', 257);
        deep[514] = 0;
        bad(deep, "nested too deep");
        deep[256] = 0;              /* 256 '[' then nothing: truncated, not too deep */
        bad(deep, "expecting a value");
        char ok[520];
        memset(ok, '[', 256);
        memset(ok + 256, ']', 256);
        ok[512] = 0;
        CHECK(fb_json_parse(ok, 512, NULL, 0, NULL, 0) == 256, "256 levels");
    }
    /* CRLF is whitespace; a huge number parses (inf/0 by strtod), its text is kept */
    n = parse("{\r\n\"a\": 1e999,\r\n\"b\": 123456789012345678901234567890}\r\n");
    CHECK(isinf(fb_json_number(fb_json_get(nodes, "a"))) && fb_json_get(nodes, "b")->is_int &&
          fb_json_get(nodes, "b")->len == 30, "huge numbers");
    /* a lone surrogate decodes (as 3 bytes) without reading past the string */
    parse("\"\\ud800x\"");
    CHECK(fb_json_str(nodes, b, sizeof b) == 4 && b[3] == 'x', "lone surrogate");
    /* the device catalog reads back */
    size_t cn = fb_device_catalog_json(NULL, 0) + 1;
    char *cat = malloc(cn);
    fb_device_catalog_json(cat, cn);
    int need = fb_json_parse(cat, strlen(cat), NULL, 0, NULL, 0);
    fb_json_node_t *cn_nodes = malloc(sizeof *cn_nodes * (size_t)need);
    CHECK(need > 0 && fb_json_parse(cat, strlen(cat), cn_nodes, need, NULL, 0) == need, "catalog parse");
    CHECK(fb_json_str_eq(fb_json_get(fb_json_at(cn_nodes, 0), "id"), "icm42688p"), "catalog[0]");
    free(cn_nodes);
    free(cat);
}

int main(void)
{
    reader();

    /* integers as integers, including the edges of the integer range */
    num(0, "0");
    num(-0.0, "0");
    num(42, "42");
    num(-7, "-7");
    num(999999999999999.0, "999999999999999");
    num(-999999999999999.0, "-999999999999999");
    num(1e15, "1e+15");                 /* beyond the guard: %.9g, never an integer cast */
    num(1e300, "1e+300");
    num(-1e300, "-1e+300");
    num(9.3e18, "9.3e+18");             /* > INT64_MAX: the old (int64_t)v cast was UB here */
    /* other doubles with 9 significant digits */
    num(0.5, "0.5");
    num(1799.25, "1799.25");
    num(123456.789, "123456.789");
    num(1.0 / 3.0, "0.333333333");
    num(-1e-7, "-1e-07");
    /* not finite -> null */
    num(NAN, "null");
    num(-NAN, "null");
    num(INFINITY, "null");
    num(-INFINITY, "null");

    /* times: fixed 4 decimals, the 0.1 ms survives past 100 s and 1000 s */
    tim(0, "0.0000");
    tim(-0.0, "0.0000");
    tim(2.35, "2.3500");
    tim(100.0001, "100.0001");
    tim(1234.5678, "1234.5678");
    tim(98765.4321, "98765.4321");
    tim(123.45678, "123.4568");
    tim(NAN, "null");
    tim(INFINITY, "null");
    tim(1e20, "1e+20");

    /* every length fits the documented buffer */
    const double worst[] = { -999999999999999.0, -1.23456789e-300, -1.23456789e+300, -99999999999999.9999 };
    for (unsigned i = 0; i < sizeof worst / sizeof worst[0]; i++) {
        char b[FB_JSON_NUM_MAX];
        CHECK(fb_json_num(b, sizeof b, worst[i]) < FB_JSON_NUM_MAX, "num %g too long", worst[i]);
        CHECK(fb_json_time(b, sizeof b, worst[i]) < FB_JSON_NUM_MAX, "time %g too long", worst[i]);
    }
    /* a short buffer truncates but reports the length it needed, like snprintf */
    {
        char b[4];
        CHECK(fb_json_num(b, sizeof b, 123456) == 6 && !strcmp(b, "123"), "truncation '%s'", b);
    }

    /* the catalog JSON goes through the same writer: no nan/inf, ranges as integers */
    size_t n = fb_device_catalog_json(NULL, 0) + 1;
    char *cat = malloc(n);
    fb_device_catalog_json(cat, n);
    CHECK(!strstr(cat, "nan") && !strstr(cat, "inf"), "catalog has nan/inf");
    CHECK(strstr(cat, "\"min\":-130,\"max\":0") != NULL, "crsf rssi range not integer");
    CHECK(strstr(cat, "\"default\":0.05") != NULL, "icm gyro noise default");
    free(cat);

    if (fails) { printf("test_json: %d failure(s)\n", fails); return 1; }
    printf("test_json: all passed\n");
    return 0;
}
