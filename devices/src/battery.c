/* LiPo battery model. See include/fb/battery.h. */
#include "fb/battery.h"
#include "fb/device.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct fb_battery {
    fb_battery_config_t cfg;
    double soc, mah, current;
    bool v_fault, disconnected;
    double v_fault_v;
};

static const double ocv_soc[] = { 0.00, 0.05, 0.10, 0.20, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90, 1.00 };
static const double ocv_v[] = { 3.30, 3.50, 3.60, 3.70, 3.75, 3.79, 3.83, 3.87, 3.92, 3.98, 4.06, 4.20 };
#define OCV_N ((int)(sizeof(ocv_soc) / sizeof(ocv_soc[0])))

void fb_battery_default_config(fb_battery_config_t *c)
{
    c->cells = 4;
    c->capacity_mah = 4200;
    c->start_soc = 1.0;
    c->resistance_mohm = 0;
    c->current_scale = 1.0;
    c->fc_current_a = 0.3;
}

fb_battery_t *fb_battery_new(const fb_battery_config_t *cfg)
{
    fb_battery_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    if (cfg) {
        d->cfg = *cfg;
    } else {
        fb_battery_default_config(&d->cfg);
    }
    fb_battery_reset(d);
    return d;
}

void fb_battery_free(fb_battery_t *d) { free(d); }

void fb_battery_reset(fb_battery_t *d)
{
    d->soc = d->cfg.start_soc < 0 ? 0 : d->cfg.start_soc > 1 ? 1 : d->cfg.start_soc;
    d->mah = 0;
    d->current = 0;
}

const fb_battery_config_t *fb_battery_config(const fb_battery_t *d) { return &d->cfg; }

double fb_battery_ocv_cell(double soc)
{
    if (!(soc > 0)) return ocv_v[0];
    if (soc >= 1) return ocv_v[OCV_N - 1];
    int i = 1;
    while (ocv_soc[i] < soc) i++;
    double f = (soc - ocv_soc[i - 1]) / (ocv_soc[i] - ocv_soc[i - 1]);
    return ocv_v[i - 1] + f * (ocv_v[i] - ocv_v[i - 1]);
}

void fb_battery_step(fb_battery_t *d, double current_a, double dt_s)
{
    d->current = current_a;
    double mah = current_a * dt_s / 3.6;
    d->mah += mah;
    if (d->cfg.capacity_mah > 0) {
        d->soc -= mah / d->cfg.capacity_mah;
    }
    if (d->soc < 0) d->soc = 0;
    if (d->soc > 1) d->soc = 1;         /* a negative current (charging) stops at full */
}

double fb_battery_voltage(const fb_battery_t *d)
{
    double v = d->cfg.cells * fb_battery_ocv_cell(d->soc) - d->current * d->cfg.resistance_mohm / 1000.0;
    return v > 0 ? v : 0;
}

double fb_battery_current_a(const fb_battery_t *d) { return d->current; }
double fb_battery_mah_used(const fb_battery_t *d) { return d->mah; }
double fb_battery_soc(const fb_battery_t *d) { return d->soc; }

double fb_battery_sense_v(const fb_battery_t *d)
{
    return d->disconnected ? 0 : d->v_fault ? d->v_fault_v : fb_battery_voltage(d);
}

double fb_battery_sense_a(const fb_battery_t *d)
{
    return d->disconnected ? 0 : d->current;
}

/* ---- descriptor ------------------------------------------------------------------------- */

static const char *const battery_signals[] = {
    "battery.voltage_v", "battery.current_a", "battery.mah_used", "battery.soc", NULL,
};
static const fb_param_desc_t battery_props[] = {
    { .id = "cells", .label = "cells in series", .type = FB_PARAM_INT,
      .has_range = true, .min = 1, .max = 12, .has_default = true, .def = 4 },
    { .id = "capacity_mah", .label = "capacity mAh", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = 1, .max = 100000, .has_default = true, .def = 4200 },
    { .id = "start_soc", .label = "state of charge at start 0..1", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = 0, .max = 1, .has_default = true, .def = 1.0 },
    { .id = "resistance_mohm", .label = "internal resistance mOhm (0 = no sag)", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = 0, .max = 1000, .has_default = true, .def = 0 },
    { .id = "current_scale", .label = "current = this x the ESC's current", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = 0, .max = 10, .has_default = true, .def = 1.0 },
    { .id = "fc_current_a", .label = "electronics current A (FC, receiver, GPS)", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = 0, .max = 10, .has_default = true, .def = 0.3 },
    { 0 },
};
static const fb_param_desc_t voltage_params[] = {
    { .id = "volts", .label = "voltage the FC reads, V", .type = FB_PARAM_FLOAT,
      .has_range = true, .min = 0, .max = 60 },
    { 0 },
};
static const fb_fault_desc_t battery_faults[] = {
    { "voltage", "voltage sensor reads a fixed value", voltage_params },
    { "disconnect", "sense leads off (the FC reads 0 V, 0 A)", NULL },
    { 0 },
};
static const fb_port_desc_t battery_ports[] = {
    { .id = "power", .kind = "power", .dir = "out", .label = "VBAT + current sensor" },
    { 0 },
};
static const fb_device_desc_t battery_desc = {
    .id = "battery", .label = "LiPo battery", .category = "battery", .kind = "power",
    .props = battery_props, .faults = battery_faults, .signals = battery_signals, .ports = battery_ports,
};
const fb_device_desc_t *fb_battery_desc(void) { return &battery_desc; }

bool fb_battery_set_fault(fb_battery_t *d, const char *fault, bool on, const double *params)
{
    if (!strcmp(fault, "voltage")) {
        if (on && params && !isnan(params[0])) d->v_fault_v = params[0];
        d->v_fault = on;
    } else if (!strcmp(fault, "disconnect")) {
        d->disconnected = on;
    } else {
        return false;
    }
    return true;
}

bool fb_battery_fault_active(const fb_battery_t *d, const char *fault)
{
    if (!strcmp(fault, "voltage")) return d->v_fault;
    if (!strcmp(fault, "disconnect")) return d->disconnected;
    return false;
}
