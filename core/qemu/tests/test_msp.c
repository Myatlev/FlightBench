/* Host test of the mode-message decoders (hw/arm/fb_msp_modes.c.inc) on MSP replies captured
 * from both firmwares (fixtures/msp-*.json, scripts/msp_capture.py): RotorFlight 4.6.0 on the
 * RS7 bench, iNav 9.1.0 on the trainer bench (after its first start). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../hw/arm/fb_msp_modes.c.inc"
#include "fb/json.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct {
    char *text;
    fb_json_node_t nodes[256];
} fixture_t;

static void load(fixture_t *f, const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURES, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("FAIL: %s: cannot open\n", path);
        exit(1);
    }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    rewind(fp);
    f->text = malloc(n + 1);
    if (fread(f->text, 1, n, fp) != (size_t)n) {
        exit(1);
    }
    fclose(fp);
    char err[128];
    if (fb_json_parse(f->text, n, f->nodes, 256, err, sizeof(err)) < 0) {
        printf("FAIL: %s: %s\n", path, err);
        exit(1);
    }
}

/* the reply payload of fixture entry `key`: its length, -1 when absent or an error reply */
static int payload(const fixture_t *f, const char *key, uint8_t *out, int max)
{
    const fb_json_node_t *e = fb_json_get(f->nodes, key);
    if (!e || fb_json_get(e, "error")) {
        return -1;
    }
    char hex[4096];
    fb_json_str(fb_json_get(e, "hex"), hex, sizeof(hex));
    int n = 0;
    for (const char *p = hex; p[0] && p[1] && n < max; p += 2) {
        unsigned v;
        sscanf(p, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

static int box_index(const fbm_modes_t *m, const char *name)
{
    for (int i = 0; i < m->nbox; i++) {
        if (!strcmp(m->name[i], name)) {
            return i;
        }
    }
    return -1;
}

/* the decode as the control server does it (modes_reply) */
static void decode(const fixture_t *f, bool inav, fbm_modes_t *m)
{
    uint8_t p[1024], r[256];
    int n, nr;
    memset(m, 0, sizeof(*m));
    n = payload(f, "boxnames", p, sizeof(p));
    int got = fbm_names(p, n, m->name, &m->nbox, FBM_MAX_BOXES);
    if (!inav && got == 32 && (n = payload(f, "boxnames:1", p, sizeof(p))) >= 0) {
        fbm_names(p, n, m->name, &m->nbox, FBM_MAX_BOXES);
    }
    n = payload(f, "boxids", p, sizeof(p));
    got = fbm_ids(p, n, m->id, &m->nid, FBM_MAX_BOXES);
    if (!inav && got == 32 && (n = payload(f, "boxids:1", p, sizeof(p))) >= 0) {
        fbm_ids(p, n, m->id, &m->nid, FBM_MAX_BOXES);
    }
    n = payload(f, "rx_map", p, sizeof(p));
    m->nmap = n > 0 ? fbm_rx_map(p, n, m->map, FBM_MAX_MAP) : 0;
    nr = payload(f, "mode_ranges", r, sizeof(r));
    m->nrange = fbm_ranges(inav, r, nr, m->range, FBM_MAX_SLOTS);
    n = payload(f, "mode_ranges_extra", p, sizeof(p));
    if (n >= 0) {
        m->nrange = fbm_ranges_extra(r, nr, p, n, m->range, m->nrange, FBM_MAX_SLOTS);
    }
}

static void test_rotorflight(void)
{
    fixture_t f;
    fbm_modes_t m;
    load(&f, "msp-rotorflight-rs7.json");
    decode(&f, false, &m);
    uint8_t p[64];
    CHECK(payload(&f, "fc_variant", p, 64) == 4 && !memcmp(p, "RTFL", 4), "variant RTFL");
    CHECK(m.nbox == 18 && m.nid == 18, "RotorFlight: 18 boxes, 18 ids (%d, %d)", m.nbox, m.nid);
    CHECK(payload(&f, "boxnames:1", p, 64) == 0, "one page: page 1 is empty");
    CHECK(!strcmp(m.name[0], "ARM") && m.id[0] == 0, "box 0 ARM, id 0");
    int i = box_index(&m, "RESCUE");
    CHECK(i == 4 && m.id[i] == 53, "RESCUE: index 4, permanent id 53 (%d, %u)", i, i >= 0 ? m.id[i] : 0);
    i = box_index(&m, "GOVERNOR BYPASS");
    CHECK(i == 17 && m.id[i] == 57, "GOVERNOR BYPASS: index 17, id 57");
    /* the RS7 profile: aux 0 0 0 1700 2100 (ARM on AUX1), map AECR1T23 */
    CHECK(m.nrange == 1, "one range in use (%d)", m.nrange);
    const fbm_range_t *r = &m.range[0];
    CHECK(r->slot == 0 && r->box == 0 && r->aux == 0, "slot 0: ARM on aux 0");
    CHECK(r->start_us == 1700 && r->end_us == 2100, "1700..2100 us (%d..%d): steps of 5 us around 1500",
          r->start_us, r->end_us);
    CHECK(!r->and_logic && !r->linked, "OR, not linked");
    CHECK(m.nmap == 8 && m.map[2] == 3 && m.map[3] == 2 && m.map[4] == 5 && m.map[5] == 4,
          "rx map AECR1T23 = 0 1 3 2 5 4 6 7");
    int rc = fbm_rc_channel(false, r->aux);
    CHECK(rc == 5 && fbm_rx_channel(&m, rc) == 4, "aux 0 = rc channel 5 (after roll, pitch, yaw, "
          "collective, throttle) = receiver channel 5 (index 4) through the map (%d, %d)", rc,
          fbm_rx_channel(&m, rc));
    /* MSP_STATUS @6: the box bits by index (disarmed in the capture); ARM + RESCUE -> ids */
    int n = payload(&f, "status", p, 64);
    uint32_t bits = n >= 10 ? (uint32_t)p[6] | p[7] << 8 | p[8] << 16 | (uint32_t)p[9] << 24 : 0xFFFFFFFF;
    uint8_t ids[64];
    CHECK(n >= 10 && fbm_active_ids(&m, bits, 32, ids) == 0, "status: no box active on the pad");
    CHECK(fbm_active_ids(&m, 1u | 1u << 4, 32, ids) == 2 && ids[0] == 0 && ids[1] == 53,
          "bits 0 and 4 -> ARM (0), RESCUE (53)");
}

static void test_inav(void)
{
    fixture_t f;
    fbm_modes_t m;
    load(&f, "msp-inav-trainer.json");
    decode(&f, true, &m);
    uint8_t p[64];
    CHECK(payload(&f, "fc_variant", p, 64) == 4 && !memcmp(p, "INAV", 4), "variant INAV");
    CHECK(payload(&f, "mode_ranges_extra", p, 64) < 0, "iNav: MSP_MODE_RANGES_EXTRA is an error reply");
    CHECK(m.nbox > 0 && m.nbox == m.nid, "names and ids pair up (%d, %d)", m.nbox, m.nid);
    int arm = box_index(&m, "ARM"), angle = box_index(&m, "ANGLE"), launch = box_index(&m, "NAV LAUNCH");
    CHECK(arm == 0 && m.id[arm] == 0, "ARM: index 0, id 0");
    CHECK(angle > 0 && m.id[angle] == 1, "ANGLE: id 1");
    CHECK(launch > 0 && m.id[launch] == 36, "NAV LAUNCH: id 36");
    /* models/trainer/inav.cli: aux 0 0 0 / aux 1 1 1 / aux 2 36 2, 1700..2100 */
    CHECK(m.nrange == 3, "three ranges in use (%d)", m.nrange);
    static const uint8_t box[3] = { 0, 1, 36 };
    for (int i = 0; i < 3 && i < m.nrange; i++) {
        const fbm_range_t *r = &m.range[i];
        CHECK(r->slot == i && r->box == box[i] && r->aux == i, "slot %d: box %u on aux %d", i, box[i], i);
        CHECK(r->start_us == 1700 && r->end_us == 2100, "slot %d: 1700..2100 us (%d..%d): 900 + 25 * step",
              i, r->start_us, r->end_us);
        int rc = fbm_rc_channel(true, r->aux);
        CHECK(rc == 4 + i && fbm_rx_channel(&m, rc) == 4 + i, "aux %d = rc channel %d = receiver "
              "channel %d (iNav maps the first 4 only)", i, 4 + i, 5 + i);
    }
    CHECK(m.nmap == 4, "iNav's rx map has 4 entries (%d)", m.nmap);
    /* MSP2_INAV_STATUS @13: the box bits by index */
    uint8_t ids[64];
    uint64_t bits = 1ull << arm | 1ull << angle | 1ull << launch;
    CHECK(fbm_active_ids(&m, bits, 64, ids) == 3 && ids[0] == 0 && ids[1] == 1 && ids[2] == 36,
          "ARM + ANGLE + NAV LAUNCH -> 0, 1, 36");
}

static void test_synthetic(void)
{
    /* RotorFlight's second page, a linked slot, AND logic */
    fbm_modes_t m = { 0 };
    char names[40 * 4 + 1] = "";
    for (int i = 0; i < 32; i++) {
        strcat(names, "B;");
    }
    CHECK(fbm_names((const uint8_t *)names, strlen(names), m.name, &m.nbox, FBM_MAX_BOXES) == 32,
          "a full page: 32");
    CHECK(fbm_names((const uint8_t *)"X;Y;", 4, m.name, &m.nbox, FBM_MAX_BOXES) == 2 && m.nbox == 34 &&
          !strcmp(m.name[33], "Y"), "page 1 appends");
    {
        /* iNav refuses MSP_BOXNAMES with GPS + a compass (47 boxes): the names from the ids (the
         * MSP_BOXIDS reply of stock iNav 9.1.0 on the trainer bench with the GPS module and the
         * compass, captured 2026-09-29) */
        static const uint8_t ids[47] = { 0, 51, 61, 1, 2, 35, 5, 8, 11, 49, 10, 28, 53, 45, 30, 31, 55,
                                         59, 56, 3, 12, 36, 37, 21, 54, 64, 13, 60, 19, 26, 27, 39, 40, 41,
                                         47, 48, 57, 58, 42, 43, 44, 50, 62, 63, 65, 66, 67 };
        fbm_modes_t b = { 0 };
        fbm_inav_names(ids, 47, b.name, &b.nbox, FBM_MAX_BOXES);
        CHECK(b.nbox == 47 && !strcmp(b.name[0], "ARM") && !strcmp(b.name[1], "PREARM") &&
              !strcmp(b.name[3], "ANGLE") && !strcmp(b.name[21], "NAV LAUNCH") && !strcmp(b.name[30], "FAILSAFE") &&
              !strcmp(b.name[46], "GIMBAL CENTER"), "iNav names by permanent id");
        static const uint8_t odd[2] = { 4, 200 };
        fbm_inav_names(odd, 2, b.name, &b.nbox, FBM_MAX_BOXES);
        CHECK(b.nbox == 2 && !strcmp(b.name[0], "BOX4") && !strcmp(b.name[1], "BOX200"), "unknown ids");
    }
    static const uint8_t ranges[12] = { 1, 2, 0x28, 0x78, 13, 0, 0, 0, 5, 1, 0x9C, 0x00 };
    int n = fbm_ranges(false, ranges, 12, m.range, FBM_MAX_SLOTS);
    CHECK(n == 2 && m.range[1].start_us == 1000 && m.range[1].end_us == 1500,
          "int8 steps: -100..0 = 1000..1500 us");
    static const uint8_t extra[10] = { 3, 1, 1, 0, 13, 0, 1, 5, 0, 0 };
    n = fbm_ranges_extra(ranges, 12, extra, 10, m.range, n, FBM_MAX_SLOTS);
    CHECK(n == 3 && m.range[0].and_logic && m.range[2].slot == 1 && m.range[2].linked == 1 &&
          m.range[2].start_us == 0, "slot 0 AND; slot 1 (no range) linked to box 1");
    CHECK(fbm_ranges(true, (const uint8_t *)"\x00\x00\x00\x00", 4, m.range, FBM_MAX_SLOTS) == 0,
          "an unused iNav slot (ARM 0..0) is left out");
}

int main(void)
{
    test_rotorflight();
    test_inav();
    test_synthetic();
    printf("test_msp: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
