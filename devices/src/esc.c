/* ESC + motor model. See include/fb/esc.h. */
#include "fb/esc.h"
#include "fb/device.h"
#include "fb/dshot.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct fb_esc {
    fb_esc_params_t p;
    float volts;
    float load;
    float throttle;
    float rpm;
    float current;
    float mah;
    float temp_c;
    bool armed;
    bool telemetry_req;
    bool telemetry_lost;    /* fault (kept across reset) */
    float signal_timeout_s; /* no DShot frame for this long -> throttle 0 (0 = never; kept) */
    float since_frame_s;
};

static const fb_esc_params_t defaults = { 520.0f, 10, 0.4f, 200.0f, 0.5f, 50.0f };

void fb_esc_default_params(fb_esc_params_t *p) { *p = defaults; }

fb_esc_t *fb_esc_new(const fb_esc_params_t *p)
{
    fb_esc_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->p = p ? *p : defaults;
    d->signal_timeout_s = FB_ESC_SIGNAL_TIMEOUT_MS_DEFAULT / 1000.0f;
    if (d->p.poles < 2) d->p.poles = 2;
    if (d->p.tau_s <= 0) d->p.tau_s = 0.4f;
    fb_esc_reset(d);
    return d;
}

void fb_esc_free(fb_esc_t *d) { free(d); }

void fb_esc_reset(fb_esc_t *d)
{
    fb_esc_params_t p = d->p;
    bool lost = d->telemetry_lost;
    float timeout = d->signal_timeout_s;
    memset(d, 0, sizeof(*d));
    d->p = p;
    d->telemetry_lost = lost;
    d->signal_timeout_s = timeout;
    d->volts = p.voltage_v > 0 ? p.voltage_v : 50.0f;  /* 12S nominal-ish by default */
    d->temp_c = 35.0f;
}

void fb_esc_set_voltage(fb_esc_t *d, float volts) { d->volts = volts; }
void fb_esc_set_load(fb_esc_t *d, float load) { d->load = load < 0 ? 0 : (load > 1 ? 1 : load); }

void fb_esc_set_signal_timeout(fb_esc_t *d, float seconds)
{
    d->signal_timeout_s = seconds > 0 ? seconds : 0;
}

bool fb_esc_signal_lost(const fb_esc_t *d)
{
    return d->signal_timeout_s > 0 && d->since_frame_s >= d->signal_timeout_s;
}

void fb_esc_set_dshot(fb_esc_t *d, uint16_t value, bool request_telemetry)
{
    d->since_frame_s = 0;
    d->throttle = fb_dshot_value_to_throttle(value);
    if (d->throttle > 0) d->armed = true;
    if (request_telemetry) d->telemetry_req = true;
}

void fb_esc_step(fb_esc_t *d, float dt_s)
{
    /* signal loss: like a real ESC, no frames for the timeout = motor off (it spins down) */
    if (d->signal_timeout_s > 0 && d->since_frame_s < d->signal_timeout_s) {
        d->since_frame_s += dt_s;
    }
    if (fb_esc_signal_lost(d)) d->throttle = 0;
    /* target speed: unloaded kv*V*throttle, drooping up to 15 % under full load */
    float target = d->throttle * d->p.kv * d->volts * (1.0f - 0.15f * d->load);
    float a = dt_s / (d->p.tau_s + dt_s);
    d->rpm += (target - d->rpm) * a;
    if (d->rpm < 0) d->rpm = 0;
    /* current: idle + quadratic in throttle scaled by load, plus spin-up surge */
    float steady = d->p.idle_current_a + d->p.max_current_a * d->throttle * d->throttle * (0.3f + 0.7f * d->load);
    float surge = (target > d->rpm) ? (target - d->rpm) / (d->p.kv * d->volts + 1e-3f) * d->p.max_current_a * 0.5f : 0.0f;
    d->current = d->throttle > 0 ? steady + surge : 0.0f;
    d->mah += d->current * dt_s / 3.6f;
    d->temp_c += (d->current * 0.1f + 25.0f - d->temp_c) * dt_s / 30.0f;
}

float    fb_esc_rpm(const fb_esc_t *d) { return d->rpm; }
uint32_t fb_esc_erpm(const fb_esc_t *d) { return (uint32_t)lrintf(d->rpm * d->p.poles / 2.0f); }
float    fb_esc_current_a(const fb_esc_t *d) { return d->current; }
float    fb_esc_throttle(const fb_esc_t *d) { return d->throttle; }
bool     fb_esc_armed(const fb_esc_t *d) { return d->armed; }
bool     fb_esc_telemetry_due(const fb_esc_t *d) { return d->telemetry_req && !d->telemetry_lost; }

uint8_t fb_esc_crc8_kiss(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

int fb_esc_telemetry_frame(fb_esc_t *d, uint8_t out[10])
{
    if (!d->telemetry_req) return 0;
    d->telemetry_req = false;
    if (d->telemetry_lost) return 0;
    uint16_t volt = (uint16_t)lrintf(d->volts * 100.0f);       /* 10 mV */
    uint16_t curr = (uint16_t)lrintf(d->current * 100.0f);     /* 10 mA */
    uint16_t capa = (uint16_t)lrintf(d->mah);
    uint16_t erpm = (uint16_t)(fb_esc_erpm(d) / 100);
    out[0] = (uint8_t)lrintf(d->temp_c);
    out[1] = (uint8_t)(volt >> 8); out[2] = (uint8_t)volt;
    out[3] = (uint8_t)(curr >> 8); out[4] = (uint8_t)curr;
    out[5] = (uint8_t)(capa >> 8); out[6] = (uint8_t)capa;
    out[7] = (uint8_t)(erpm >> 8); out[8] = (uint8_t)erpm;
    out[9] = fb_esc_crc8_kiss(out, 9);
    return 10;
}

/* ---- descriptor and faults ------------------------------------------------------------- */

static const char *const telemetry_options[] = { "bidir", "serial", NULL };
static const fb_param_desc_t esc_props[] = {
    { .id = "actuator", .label = "aircraft actuator", .type = FB_PARAM_ACTUATOR },
    { .id = "telemetry", .label = "RPM telemetry", .type = FB_PARAM_ENUM, .options = telemetry_options,
      .def_str = "bidir" },
    { .id = "signal_timeout_ms", .label = "signal-loss timeout ms (0 = never)", .type = FB_PARAM_INT,
      .has_range = true, .min = 0, .max = 2000, .has_default = true, .def = FB_ESC_SIGNAL_TIMEOUT_MS_DEFAULT },
    { 0 },
};
static const fb_fault_desc_t esc_faults[] = {
    { "telemetry_lost", "RPM telemetry lost", NULL },
    { 0 },
};
static const char *const esc_signals[] = {
    "out.motor{n}_dshot", "out.motor{n}_bidir", "esc.throttle", "esc.motor_rpm", "esc.erpm",
    "esc.current_a", "esc.load", NULL,
};
static const fb_port_desc_t esc_ports[] = {
    { .id = "signal", .kind = "motor", .dir = "in", .label = "throttle signal (DShot)" },
    { .id = "tlm", .kind = "uart", .dir = "out", .label = "serial telemetry", .optional = true },
    { .id = "rpm", .kind = "capture", .dir = "out", .label = "RPM (frequency) wire", .optional = true },
    { 0 },
};
static const fb_device_desc_t esc_desc = {
    .id = "esc", .label = "ESC", .category = "esc", .kind = "motor",
    .props = esc_props, .faults = esc_faults, .signals = esc_signals, .ports = esc_ports,
};
const fb_device_desc_t *fb_esc_desc(void) { return &esc_desc; }

bool fb_esc_set_fault(fb_esc_t *d, const char *fault, bool on, const double *params)
{
    (void)params;
    if (strcmp(fault, "telemetry_lost")) return false;
    d->telemetry_lost = on;
    return true;
}

bool fb_esc_fault_active(const fb_esc_t *d, const char *fault)
{
    return !strcmp(fault, "telemetry_lost") && d->telemetry_lost;
}

bool fb_esc_telemetry_ok(const fb_esc_t *d) { return !d->telemetry_lost; }
