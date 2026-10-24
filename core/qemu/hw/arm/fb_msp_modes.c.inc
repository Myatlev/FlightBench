/*
 * FlightBench: decoders of the firmware's mode messages, iNav 9.1 and RotorFlight 4.6 (the
 * Betaflight layout). Plain C, no QEMU or glib: the control server (nexus_xr_ctl.c.inc) and the
 * host test core/qemu/tests/test_msp.c include it.
 *
 *   MSP_BOXNAMES 116   "NAME;NAME;..." in box-index order. RotorFlight: pages of 32 boxes (the
 *                      request's optional u8 page, msp_box.c serializeBoxReply); iNav: all at once.
 *   MSP_BOXIDS 119     u8 permanent id per box, same order and paging.
 *   MSP_MODE_RANGES 34 per activation slot: u8 permanent id, u8 aux channel index, u8 start step,
 *                      u8 end step. iNav (fc/rc_modes.h): 40 slots, us = 900 + 25 * step (u8);
 *                      RotorFlight (fc/rc_modes.h): 20 slots, us = 1500 + 5 * step (int8). A slot
 *                      is in use when start < end (RotorFlight's isRangeUsable; an unused slot reads
 *                      box 0 = ARM, 0..0).
 *   MSP_MODE_RANGES_EXTRA 238  RotorFlight only (iNav answers an error): u8 slot count, then per
 *                      slot u8 permanent id, u8 logic (0 OR, 1 AND), u8 linked box permanent id
 *                      (0 = none: ARM cannot be linked).
 *   MSP_RX_MAP 64      u8 per mapped firmware channel: the receiver channel it reads (iNav 4,
 *                      RotorFlight 8; channels past the map are read 1:1).
 * The aux channel is the firmware's rc channel aux + 4 (iNav NON_AUX_CHANNEL_COUNT) or aux + 5
 * (RotorFlight CONTROL_CHANNEL_COUNT: roll, pitch, yaw, collective, throttle).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define FBM_MAX_BOXES   64
#define FBM_NAME        24
#define FBM_MAX_SLOTS   40
#define FBM_MAX_MAP     8

typedef struct {
    int slot;                   /* activation slot (the configurator's row) */
    uint8_t box;                /* permanent id */
    uint8_t aux;                /* aux channel index, 0-based */
    int start_us, end_us;       /* active while start_us <= channel < end_us */
    bool and_logic;             /* RotorFlight MODE_RANGES_EXTRA: AND with the box's other slots */
    uint8_t linked;             /* ... linked to this box's state (permanent id), 0 = none */
} fbm_range_t;

typedef struct {
    int nbox;
    char name[FBM_MAX_BOXES][FBM_NAME];
    int nid;
    uint8_t id[FBM_MAX_BOXES];
    int nrange;
    fbm_range_t range[FBM_MAX_SLOTS];
    int nmap;
    uint8_t map[FBM_MAX_MAP];
} fbm_modes_t;

/* "A;B;" appended to names[*n..max): the number of names in this reply */
static int fbm_names(const uint8_t *p, unsigned n, char (*names)[FBM_NAME], int *count, int max)
{
    int got = 0;
    unsigned st = 0;
    for (unsigned i = 0; i <= n && *count < max; i++) {
        if (i == n || p[i] == ';') {
            if (i > st) {
                unsigned l = i - st < FBM_NAME - 1 ? i - st : FBM_NAME - 1;
                memcpy(names[*count], p + st, l);
                names[(*count)++][l] = 0;
                got++;
            }
            st = i + 1;
        }
    }
    return got;
}

/* permanent ids appended to ids[*n..max): the number in this reply */
static int fbm_ids(const uint8_t *p, unsigned n, uint8_t *ids, int *count, int max)
{
    int got = 0;
    for (unsigned i = 0; i < n && *count < max; i++, got++) {
        ids[(*count)++] = p[i];
    }
    return got;
}

/* iNav 9.1.0's box names by permanent id (fc/fc_msp_box.c boxes[]), for when it refuses
 * MSP_BOXNAMES: serializeBoxNamesReply returns an error once the names of the active boxes do not
 * fit the MSP reply (with GPS and a compass: ~47 boxes); MSP_BOXIDS still answers. NULL = unknown */
static const char *fbm_inav_box_name(uint8_t id)
{
    static const struct { uint8_t id; const char *name; } t[] = {
        { 0, "ARM" }, { 1, "ANGLE" }, { 2, "HORIZON" }, { 3, "NAV ALTHOLD" }, { 5, "HEADING HOLD" },
        { 6, "HEADFREE" }, { 7, "HEADADJ" }, { 8, "CAMSTAB" }, { 10, "NAV RTH" }, { 11, "NAV POSHOLD" },
        { 12, "MANUAL" }, { 13, "BEEPER" }, { 15, "LEDS OFF" }, { 16, "LIGHTS" }, { 19, "OSD OFF" },
        { 20, "TELEMETRY" }, { 21, "AUTO TUNE" }, { 26, "BLACKBOX" }, { 27, "FAILSAFE" },
        { 28, "NAV WP" }, { 29, "AIR MODE" }, { 30, "HOME RESET" }, { 31, "GCS NAV" },
        { 32, "FPV ANGLE MIX" }, { 33, "SURFACE" }, { 34, "FLAPERON" }, { 35, "TURN ASSIST" },
        { 36, "NAV LAUNCH" }, { 37, "SERVO AUTOTRIM" }, { 39, "CAMERA CONTROL 1" },
        { 40, "CAMERA CONTROL 2" }, { 41, "CAMERA CONTROL 3" }, { 42, "OSD ALT 1" }, { 43, "OSD ALT 2" },
        { 44, "OSD ALT 3" }, { 45, "NAV COURSE HOLD" }, { 46, "MC BRAKING" }, { 47, "USER1" },
        { 48, "USER2" }, { 49, "LOITER CHANGE" }, { 50, "MSP RC OVERRIDE" }, { 51, "PREARM" },
        { 52, "TURTLE" }, { 53, "NAV CRUISE" }, { 54, "AUTO LEVEL TRIM" }, { 55, "WP PLANNER" },
        { 56, "SOARING" }, { 57, "USER3" }, { 58, "USER4" }, { 59, "MISSION CHANGE" },
        { 60, "BEEPER MUTE" }, { 61, "MULTI FUNCTION" }, { 62, "MIXER PROFILE 2" },
        { 63, "MIXER TRANSITION" }, { 64, "ANGLE HOLD" }, { 65, "GIMBAL LEVEL TILT" },
        { 66, "GIMBAL LEVEL ROLL" }, { 67, "GIMBAL CENTER" }, { 68, "GIMBAL HEADTRACKER" },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (t[i].id == id) {
            return t[i].name;
        }
    }
    return NULL;
}

/* the names of ids[0..n) from that table (an unknown id: "BOX<id>") */
static void fbm_inav_names(const uint8_t *ids, int n, char (*names)[FBM_NAME], int *count, int max)
{
    *count = 0;
    for (int i = 0; i < n && *count < max; i++) {
        const char *nm = fbm_inav_box_name(ids[i]);
        if (nm) {
            snprintf(names[*count], FBM_NAME, "%s", nm);
        } else {
            snprintf(names[*count], FBM_NAME, "BOX%u", ids[i]);
        }
        (*count)++;
    }
}

/* MSP_MODE_RANGES -> the slots in use; inav selects the step scale */
static int fbm_ranges(bool inav, const uint8_t *p, unsigned n, fbm_range_t *out, int max)
{
    int k = 0;
    for (unsigned i = 0; i + 4 <= n && k < max; i += 4) {
        int a, b;
        if (inav) {
            a = 900 + 25 * p[i + 2];
            b = 900 + 25 * p[i + 3];
            if (p[i + 2] >= p[i + 3]) {
                continue;
            }
        } else {
            int8_t sa = (int8_t)p[i + 2], sb = (int8_t)p[i + 3];
            a = 1500 + 5 * sa;
            b = 1500 + 5 * sb;
            /* RotorFlight's isRangeUsable (fc/rc_modes.h): sa/sb must also fall within
             * MIN_MODE_RANGE_STEP/MAX_MODE_RANGE_STEP (-125..125); MSP_SET_MODE_RANGE (msp.c)
             * writes the raw wire byte into the int8_t field with no clamp, so a client that
             * is not the stock configurator can put a step outside that window on the wire */
            if (sa < -125 || sb > 125 || sa >= sb) {
                continue;
            }
        }
        out[k] = (fbm_range_t){ .slot = (int)(i / 4), .box = p[i], .aux = p[i + 1],
                                .start_us = a, .end_us = b };
        k++;
    }
    return k;
}

/* RotorFlight MSP_MODE_RANGES_EXTRA onto the slots of MSP_MODE_RANGES p/n (raw, all slots):
 * logic and links; a linked slot has no usable range, so it is added here. Returns the count. */
static int fbm_ranges_extra(const uint8_t *ranges, unsigned nranges, const uint8_t *p, unsigned n,
                            fbm_range_t *out, int k, int max)
{
    if (n < 1) {
        return k;
    }
    unsigned slots = p[0];
    for (unsigned s = 0; s < slots && 1 + 3 * s + 3 <= n; s++) {
        const uint8_t *e = p + 1 + 3 * s;
        int j = 0;
        while (j < k && out[j].slot != (int)s) {
            j++;
        }
        if (j == k) {
            if (!e[2] || k >= max || 4 * s + 4 > nranges) {
                continue;               /* an unused slot, not linked */
            }
            out[k++] = (fbm_range_t){ .slot = (int)s, .box = e[0], .aux = ranges[4 * s + 1],
                                      .start_us = 0, .end_us = 0 };
        }
        out[j].and_logic = e[1] == 1;
        out[j].linked = e[2];
    }
    return k;
}

/* MSP_RX_MAP */
static int fbm_rx_map(const uint8_t *p, unsigned n, uint8_t *map, int max)
{
    int k = 0;
    for (; k < (int)n && k < max; k++) {
        map[k] = p[k];
    }
    return k;
}

/* the firmware's rc channel (0-based) of an aux index, and the receiver channel it reads */
static int fbm_rc_channel(bool inav, int aux)
{
    return aux + (inav ? 4 : 5);
}

static int fbm_rx_channel(const fbm_modes_t *m, int rc)
{
    return rc >= 0 && rc < m->nmap ? m->map[rc] : rc;
}

/* the status message's box bits (bit i = box index i) as permanent ids; returns the count */
static int fbm_active_ids(const fbm_modes_t *m, uint64_t bits, int nbits, uint8_t *out)
{
    int k = 0;
    for (int i = 0; i < m->nid && i < nbits && i < 64; i++) {
        if (bits >> i & 1) {
            out[k++] = m->id[i];
        }
    }
    return k;
}
