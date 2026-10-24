/* Descriptor catalog and the injected faults of the CRSF receiver, the IMU and the ESC. */
#include "fb/crsf.h"
#include "fb/device.h"
#include "fb/esc.h"
#include "fb/icm42688p.h"
#include "fb/spl06.h"
#include "fb/ublox.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* run the receiver for `ns` in 1 ms steps, draining its TX queue; count frames by type,
 * remember the last RC ch1 raw and LQ */
static void run(fb_crsf_t *d, uint64_t ns, int *rc, int *ls, int *ch1, int *lq)
{
    *rc = *ls = 0;
    for (uint64_t t = 0; t < ns; t += 1000000ull) {
        uint8_t b[1024];
        fb_crsf_tick(d, 1000000ull);
        int n = fb_crsf_tx_pop(d, b, sizeof(b));
        for (int i = 0; i + 3 < n; i += b[i + 1] + 2) {
            if (b[i + 2] == FB_CRSF_RC_CHANNELS) { (*rc)++; *ch1 = b[i + 3] | (b[i + 4] & 7) << 8; }
            if (b[i + 2] == FB_CRSF_LINK_STATISTICS) { (*ls)++; *lq = b[i + 5]; }
        }
    }
}

int main(void)
{
    /* catalog */
    const char *ids[] = { "icm42688p", "dps310", "spl06", "crsf", "esc", "msp_osd", "servo", "ublox", "gps", "qmc5883l", "compass", "battery", "spiflash" };
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) CHECK(fb_device_find(ids[i]) != NULL);
    CHECK(fb_device_find("nope") == NULL);
    const fb_fault_desc_t *f = fb_device_fault(fb_device_find("crsf"), "lq");
    CHECK(f && fb_fault_nparams(f) == 3 && !strcmp(f->params[0].id, "lq"));
    CHECK(fb_device_fault(fb_device_find("crsf"), "nope") == NULL);
    size_t need = fb_device_catalog_json(NULL, 0);
    char *js = malloc(need + 1);
    CHECK(fb_device_catalog_json(js, need + 1) == need && strstr(js, "\"telemetry_lost\""));
    CHECK(strstr(js, "\"ports\":[{\"id\":\"signal\",\"kind\":\"motor\",\"dir\":\"in\"") != NULL);
    free(js);

    /* ports: every device has at least one, the primary port's kind is the device's kind */
    for (const fb_device_desc_t *const *d = fb_device_catalog(); *d; d++) {
        CHECK((*d)->ports && (*d)->ports[0].id && !strcmp((*d)->ports[0].kind, (*d)->kind));
        CHECK(!(*d)->ports[0].bundled && !(*d)->ports[0].optional);
        for (const fb_port_desc_t *p = (*d)->ports; p->id; p++) {
            CHECK(p->kind && (!strcmp(p->dir, "in") || !strcmp(p->dir, "out") || !strcmp(p->dir, "both")));
        }
    }
    const fb_device_desc_t *esc = fb_device_find("esc");
    CHECK(fb_device_port(esc, "signal") && fb_device_port(esc, "tlm") && fb_device_port(esc, "rpm"));
    CHECK(!strcmp(fb_device_port(esc, "tlm")->kind, "uart") && !strcmp(fb_device_port(esc, "tlm")->dir, "out"));
    CHECK(!strcmp(fb_device_port(esc, "rpm")->kind, "capture") && fb_device_port(esc, "nope") == NULL);
    CHECK(fb_device_port(fb_device_find("icm42688p"), "int")->bundled);

    /* spl06 and ublox: every declared fault is accepted by the model, props and CLI lines listed */
    fb_spl06_t *baro = fb_spl06_new();
    for (const fb_fault_desc_t *f = fb_spl06_desc()->faults; f->id; f++) {
        double prm[FB_FAULT_MAX_PARAMS] = { 10, 0, 0, 0 };
        CHECK(fb_spl06_set_fault(baro, f->id, true, prm) && fb_spl06_fault_active(baro, f->id));
        CHECK(fb_spl06_set_fault(baro, f->id, false, NULL) && !fb_spl06_fault_active(baro, f->id));
    }
    fb_spl06_free(baro);
    const fb_fault_desc_t *off = fb_device_fault(fb_device_find("spl06"), "offset");
    CHECK(off && fb_fault_nparams(off) == 1 && !off->params[0].has_default);   /* required */
    fb_ublox_t *gps = fb_ublox_new(NULL);
    for (const fb_fault_desc_t *f = fb_ublox_desc()->faults; f->id; f++) {
        CHECK(fb_ublox_set_fault(gps, f->id, true, NULL) && fb_ublox_fault_active(gps, f->id));
        CHECK(fb_ublox_set_fault(gps, f->id, false, NULL) && !fb_ublox_fault_active(gps, f->id));
    }
    fb_ublox_free(gps);
    const fb_device_desc_t *ub = fb_device_find("ublox");
    CHECK(ub && !strcmp(ub->category, "gps") && ub->config && !strcmp(ub->config[0], "feature GPS"));
    CHECK(fb_fault_nparams(fb_device_fault(ub, "spoof")) == 3 && fb_device_fault(ub, "silence"));
    js = malloc(need + 1);
    fb_device_catalog_json(js, need + 1);
    CHECK(strstr(js, "{\"id\":\"spl06\",\"label\":\"SPL06-001 baro\",\"category\":\"baro\"") != NULL);
    CHECK(strstr(js, "\"serial {serial} +1\"") && strstr(js, "{\"id\":\"generation\",\"label\":\"receiver generation\",\"type\":\"enum\",\"options\":[\"m10\",\"m8\"],\"default\":\"m10\"}"));
    free(js);

    /* CRSF: freeze repeats the frame of the fault moment, link stats stay */
    fb_crsf_t *c = fb_crsf_new();
    uint16_t us[16];
    for (int i = 0; i < 16; i++) us[i] = 1500;
    fb_crsf_set_channels_us(c, us);
    int rc, ls, ch1 = 0, lq = -1;
    run(c, 1000000000ull, &rc, &ls, &ch1, &lq);
    CHECK(rc >= 200 && ls >= 4 && ch1 == fb_crsf_us_to_raw(1500) && lq == 100);
    CHECK(fb_crsf_set_fault(c, "freeze", true, NULL) && fb_crsf_fault_active(c, "freeze"));
    us[0] = 2000;
    fb_crsf_set_channels_us(c, us);
    run(c, 500000000ull, &rc, &ls, &ch1, &lq);
    CHECK(rc >= 100 && ls >= 2 && ch1 == fb_crsf_us_to_raw(1500));
    fb_crsf_set_fault(c, "freeze", false, NULL);
    run(c, 100000000ull, &rc, &ls, &ch1, &lq);
    CHECK(ch1 == fb_crsf_us_to_raw(2000));
    /* lq: the statistics carry the fault's LQ/RSSI, NaN keeps SNR */
    double p[3] = { 30, -95, NAN };
    fb_crsf_set_fault(c, "lq", true, p);
    run(c, 500000000ull, &rc, &ls, &ch1, &lq);
    int r, q, s;
    bool up;
    fb_crsf_link_reported(c, &r, &q, &s, &up);
    CHECK(lq == 30 && q == 30 && r == -95 && s == 10 && up);
    fb_crsf_set_fault(c, "lq", false, NULL);
    /* link_lost: silence, alias of set_link(up=false), survives reset */
    fb_crsf_set_fault(c, "link_lost", true, NULL);
    fb_crsf_reset(c);
    run(c, 500000000ull, &rc, &ls, &ch1, &lq);
    CHECK(rc == 0 && ls == 0 && fb_crsf_fault_active(c, "link_lost"));
    fb_crsf_set_link(c, -60, 100, 10, true);
    CHECK(!fb_crsf_fault_active(c, "link_lost"));
    CHECK(!fb_crsf_set_fault(c, "nope", true, NULL));
    fb_crsf_free(c);

    /* IMU: frozen data registers, DATA_RDY/INT1 continue */
    fb_icm42688p_t *m = fb_icm42688p_new();
    uint8_t wr[] = { 0x4E, 0x0F };                        /* PWR_MGMT0: gyro+accel LN */
    fb_icm42688p_cs(m, true); fb_icm42688p_xfer(m, wr[0]); fb_icm42688p_xfer(m, wr[1]); fb_icm42688p_cs(m, false);
    uint8_t wi[] = { 0x65, 0x08 };                        /* INT_SOURCE0: DRDY -> INT1 */
    fb_icm42688p_cs(m, true); fb_icm42688p_xfer(m, wi[0]); fb_icm42688p_xfer(m, wi[1]); fb_icm42688p_cs(m, false);
    float g[3] = { 100, 0, 0 }, a[3] = { 0, 0, 1 };
    fb_icm42688p_set_motion(m, g, a, 30);
    fb_icm42688p_sample(m);
    uint8_t gx = fb_icm42688p_peek(m, 0, 0x25);
    fb_icm42688p_set_fault(m, "freeze", true, NULL);
    g[0] = -100;
    fb_icm42688p_set_motion(m, g, a, 30);
    CHECK(fb_icm42688p_sample(m) && fb_icm42688p_peek(m, 0, 0x25) == gx);
    fb_icm42688p_set_fault(m, "freeze", false, NULL);
    fb_icm42688p_sample(m);
    CHECK(fb_icm42688p_peek(m, 0, 0x25) != gx);
    fb_icm42688p_free(m);

    /* ESC: telemetry lost, motor keeps running */
    fb_esc_t *e = fb_esc_new(NULL);
    fb_esc_set_dshot(e, 1000, true);
    for (int i = 0; i < 100; i++) { fb_esc_set_dshot(e, 1000, false); fb_esc_step(e, 0.01f); }
    uint8_t fr[10];
    CHECK(fb_esc_telemetry_ok(e) && fb_esc_telemetry_frame(e, fr) == 10);
    fb_esc_set_fault(e, "telemetry_lost", true, NULL);
    fb_esc_reset(e);
    fb_esc_set_dshot(e, 1000, true);
    for (int i = 0; i < 100; i++) { fb_esc_set_dshot(e, 1000, false); fb_esc_step(e, 0.01f); }
    CHECK(!fb_esc_telemetry_ok(e) && !fb_esc_telemetry_due(e) && fb_esc_telemetry_frame(e, fr) == 0 && fb_esc_rpm(e) > 1000);
    fb_esc_free(e);

    printf(fails ? "test_faults: %d FAILED\n" : "test_faults: all passed\n", fails);
    return fails != 0;
}
