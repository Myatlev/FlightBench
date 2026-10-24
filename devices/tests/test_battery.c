/* LiPo battery: the OCV curve, charge integration, sag, faults, descriptor. */
#include "fb/battery.h"
#include "fb/device.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

int main(void)
{
    /* the curve: 4.20 full, 3.30 empty, monotonic, linear between the points */
    CHECK(fb_battery_ocv_cell(1.0) == 4.20 && fb_battery_ocv_cell(0.0) == 3.30);
    CHECK(fb_battery_ocv_cell(2.0) == 4.20 && fb_battery_ocv_cell(-1) == 3.30 && fb_battery_ocv_cell(NAN) == 3.30);
    CHECK(NEAR(fb_battery_ocv_cell(0.5), 3.83, 1e-12) && NEAR(fb_battery_ocv_cell(0.95), 4.13, 1e-12));
    for (double s = 0.001; s <= 1.0; s += 0.001) {
        CHECK(fb_battery_ocv_cell(s) >= fb_battery_ocv_cell(s - 0.001));
    }

    /* defaults: 4S 4200 mAh full, no sag */
    fb_battery_t *b = fb_battery_new(NULL);
    const fb_battery_config_t *c = fb_battery_config(b);
    CHECK(c->cells == 4 && c->capacity_mah == 4200 && c->start_soc == 1.0 && c->resistance_mohm == 0);
    CHECK(c->current_scale == 1.0 && c->fc_current_a == 0.3);
    CHECK(NEAR(fb_battery_voltage(b), 16.8, 1e-12) && fb_battery_sense_v(b) == fb_battery_voltage(b));
    /* 20 A for 6 min = 2000 mAh in 1 ms steps: SoC 1 - 2000/4200 */
    for (int i = 0; i < 360000; i++) fb_battery_step(b, 20.0, 0.001);
    CHECK(NEAR(fb_battery_mah_used(b), 2000, 1e-6) && NEAR(fb_battery_soc(b), 1 - 2000.0 / 4200, 1e-9));
    CHECK(NEAR(fb_battery_voltage(b), 4 * fb_battery_ocv_cell(fb_battery_soc(b)), 1e-12));
    CHECK(fb_battery_current_a(b) == 20.0 && fb_battery_sense_a(b) == 20.0);
    /* empty: SoC stops at 0, 3.30 V/cell, mAh keep counting */
    for (int i = 0; i < 1000; i++) fb_battery_step(b, 100.0, 1.0);
    CHECK(fb_battery_soc(b) == 0 && NEAR(fb_battery_voltage(b), 13.2, 1e-12) && fb_battery_mah_used(b) > 4200);
    fb_battery_reset(b);
    CHECK(fb_battery_soc(b) == 1 && fb_battery_mah_used(b) == 0 && fb_battery_current_a(b) == 0);

    /* faults: the FC's view only, the pack goes on */
    const double v[1] = { 11.1 };
    CHECK(fb_battery_set_fault(b, "voltage", true, v) && fb_battery_fault_active(b, "voltage"));
    fb_battery_step(b, 10, 1);
    CHECK(fb_battery_sense_v(b) == 11.1 && fb_battery_sense_a(b) == 10 && NEAR(fb_battery_voltage(b), 16.8, 0.01));
    const double v0[1] = { 0 };
    CHECK(fb_battery_set_fault(b, "voltage", true, v0) && fb_battery_sense_v(b) == 0);
    CHECK(fb_battery_set_fault(b, "disconnect", true, NULL));
    CHECK(fb_battery_sense_v(b) == 0 && fb_battery_sense_a(b) == 0);
    fb_battery_reset(b);
    CHECK(fb_battery_fault_active(b, "disconnect") && fb_battery_fault_active(b, "voltage"));
    CHECK(fb_battery_set_fault(b, "disconnect", false, NULL) && fb_battery_set_fault(b, "voltage", false, NULL));
    CHECK(fb_battery_sense_v(b) == fb_battery_voltage(b));
    CHECK(!fb_battery_set_fault(b, "nope", true, NULL) && !fb_battery_fault_active(b, "nope"));
    fb_battery_free(b);

    /* 3S at half charge with 50 mOhm: sag = I R */
    fb_battery_config_t cfg;
    fb_battery_default_config(&cfg);
    cfg.cells = 3;
    cfg.start_soc = 0.5;
    cfg.resistance_mohm = 50;
    b = fb_battery_new(&cfg);
    CHECK(NEAR(fb_battery_voltage(b), 3 * 3.83, 1e-12));
    fb_battery_step(b, 30, 0);
    CHECK(NEAR(fb_battery_voltage(b), 3 * 3.83 - 1.5, 1e-12));
    fb_battery_free(b);

    /* descriptor */
    const fb_device_desc_t *d = fb_device_find("battery");
    CHECK(d && !strcmp(d->category, "battery") && !strcmp(d->kind, "power"));
    CHECK(fb_device_fault(d, "voltage") && fb_fault_nparams(fb_device_fault(d, "voltage")) == 1);
    CHECK(fb_device_fault(d, "voltage")->params[0].max >= 16.8 && fb_device_fault(d, "disconnect"));
    int np = 0;
    for (const fb_param_desc_t *p = d->props; p->id; p++) np++;
    CHECK(np == 6 && !strcmp(d->props[0].id, "cells") && d->props[0].def == 4);

    printf("test_battery: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
