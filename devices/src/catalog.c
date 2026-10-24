/* Device catalog: descriptors of devices without a model file, the list, and its JSON.
 * See include/fb/device.h. */
#include "fb/device.h"
#include "fb/json.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
/* mingw's <stdio.h> names the archetype of the printf in effect (gnu_printf with
 * __USE_MINGW_ANSI_STDIO, see Makefile; printf under clang, which has no gnu_printf) */
#if defined(__MINGW_PRINTF_FORMAT)
#define FB_PRINTF_ARCHETYPE __MINGW_PRINTF_FORMAT
#else
#define FB_PRINTF_ARCHETYPE printf
#endif

/* ---- devices without a model file of their own ------------------------------------------ */

/* PWM servo output: the machine decodes the timer compare values; nothing to inject yet */
static const fb_param_desc_t servo_props[] = {
    { .id = "actuator", .label = "aircraft actuator", .type = FB_PARAM_ACTUATOR },
    { .id = "reversed", .label = "linkage reversed", .type = FB_PARAM_BOOL, .has_default = true, .def = 0 },
    { 0 },
};
static const char *const servo_signals[] = { "out.servo{n}_us", NULL };
static const fb_port_desc_t servo_ports[] = {
    { .id = "pwm", .kind = "servo", .dir = "in", .label = "PWM signal" },
    { 0 },
};
static const fb_device_desc_t servo_desc = {
    .id = "servo", .label = "Servo", .category = "servo", .kind = "servo",
    .props = servo_props, .signals = servo_signals, .ports = servo_ports,
};

/* generic GPS / compass (no model: the models are `ublox` and `qmc5883l`), kept for the benches
 * and apps that name them */
static const fb_port_desc_t uart_port[] = {
    { .id = "uart", .kind = "uart", .dir = "both", .label = "serial" }, { 0 },
};
static const fb_port_desc_t i2c_port[] = {
    { .id = "i2c", .kind = "i2c", .dir = "both", .label = "I2C" }, { 0 },
};
static const fb_port_desc_t spi_port[] = {
    { .id = "spi", .kind = "spi", .dir = "both", .label = "SPI" }, { 0 },
};
static const fb_device_desc_t gps_desc = {
    .id = "gps", .label = "GPS", .category = "gps", .kind = "uart", .ports = uart_port,
};
static const fb_device_desc_t compass_desc = {
    .id = "compass", .label = "Compass", .category = "compass", .kind = "i2c", .ports = i2c_port,
};
/* blackbox flash soldered on boards (boards/<id>/board.json "onboard"); no model yet */
static const fb_device_desc_t spiflash_desc = {
    .id = "spiflash", .label = "SPI flash (blackbox)", .category = "storage", .kind = "spi",
    .ports = spi_port,
};

/* onboard on the SpeedyBee F405 WING (boards/speedybee-f405-wing/board.json): the MAX7456 model
 * (src/max7456.c) has no descriptor of its own; the SD card has no model yet */
static const fb_device_desc_t max7456_desc = {
    .id = "max7456", .label = "MAX7456 analog OSD", .category = "osd", .kind = "spi", .ports = spi_port,
};
static const fb_device_desc_t sdcard_desc = {
    .id = "sdcard", .label = "SD card (blackbox)", .category = "storage", .kind = "spi", .ports = spi_port,
};

const fb_device_desc_t *const *fb_device_catalog(void)
{
    static const fb_device_desc_t *list[24];
    if (!list[0]) {
        int i = 0;
        list[i++] = fb_icm42688p_desc();
        list[i++] = fb_dps310_desc();
        list[i++] = fb_spl06_desc();
        list[i++] = fb_crsf_desc();
        list[i++] = fb_esc_desc();
        list[i++] = fb_msp_osd_desc();
        list[i++] = &servo_desc;
        list[i++] = fb_ublox_desc();
        list[i++] = &gps_desc;
        list[i++] = fb_qmc5883l_desc();
        list[i++] = &compass_desc;
        list[i++] = fb_battery_desc();
        list[i++] = &spiflash_desc;
        list[i++] = &max7456_desc;
        list[i++] = &sdcard_desc;
        list[i] = NULL;
    }
    return list;
}

const fb_device_desc_t *fb_device_find(const char *id)
{
    for (const fb_device_desc_t *const *d = fb_device_catalog(); id && *d; d++) {
        if (!strcmp((*d)->id, id)) return *d;
    }
    return NULL;
}

const fb_fault_desc_t *fb_device_fault(const fb_device_desc_t *d, const char *fault_id)
{
    for (const fb_fault_desc_t *f = d ? d->faults : NULL; f && f->id && fault_id; f++) {
        if (!strcmp(f->id, fault_id)) return f;
    }
    return NULL;
}

const fb_port_desc_t *fb_device_port(const fb_device_desc_t *d, const char *port_id)
{
    for (const fb_port_desc_t *p = d ? d->ports : NULL; p && p->id && port_id; p++) {
        if (!strcmp(p->id, port_id)) return p;
    }
    return NULL;
}

int fb_fault_nparams(const fb_fault_desc_t *f)
{
    int n = 0;
    while (f->params && f->params[n].id) n++;
    return n;
}

/* ---- JSON ----------------------------------------------------------------------------- */

typedef struct { char *out; size_t n, len; } jbuf_t;

#if defined(__GNUC__)
__attribute__((format(FB_PRINTF_ARCHETYPE, 2, 3)))
#endif
static void jp(jbuf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t room = b->len < b->n ? b->n - b->len : 0;
    int k = vsnprintf(room ? b->out + b->len : NULL, room, fmt, ap);
    va_end(ap);
    if (k > 0) b->len += (size_t)k;
}

static void jstr(jbuf_t *b, const char *s)
{
    jp(b, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') jp(b, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) jp(b, "\\u%04x", (unsigned char)*s);
        else jp(b, "%c", *s);
    }
    jp(b, "\"");
}

static void jnum(jbuf_t *b, double v)
{
    char t[FB_JSON_NUM_MAX];
    fb_json_num(t, sizeof t, v);
    jp(b, "%s", t);
}

static const char *type_name(fb_param_type_t t)
{
    switch (t) {
    case FB_PARAM_INT: return "int";
    case FB_PARAM_FLOAT: return "float";
    case FB_PARAM_BOOL: return "bool";
    case FB_PARAM_ENUM: return "enum";
    default: return "actuator";
    }
}

static void jparams(jbuf_t *b, const fb_param_desc_t *p)
{
    jp(b, "[");
    for (int i = 0; p && p[i].id; i++) {
        jp(b, i ? ",{\"id\":" : "{\"id\":");
        jstr(b, p[i].id);
        if (p[i].label) { jp(b, ",\"label\":"); jstr(b, p[i].label); }
        jp(b, ",\"type\":\"%s\"", type_name(p[i].type));
        if (p[i].has_range) {
            jp(b, ",\"min\":"); jnum(b, p[i].min);
            jp(b, ",\"max\":"); jnum(b, p[i].max);
        }
        if (p[i].options) {
            jp(b, ",\"options\":[");
            for (int k = 0; p[i].options[k]; k++) {
                if (k) jp(b, ",");
                jstr(b, p[i].options[k]);
            }
            jp(b, "]");
        }
        if (p[i].def_str) {
            jp(b, ",\"default\":"); jstr(b, p[i].def_str);
        } else if (p[i].has_default) {
            jp(b, ",\"default\":");
            if (p[i].type == FB_PARAM_BOOL) jp(b, "%s", p[i].def ? "true" : "false");
            else jnum(b, p[i].def);
        }
        if (p[i].optional) jp(b, ",\"optional\":true");
        jp(b, "}");
    }
    jp(b, "]");
}

size_t fb_device_catalog_json(char *out, size_t n)
{
    jbuf_t b = { out, n, 0 };
    if (out && n) out[0] = 0;
    jp(&b, "[");
    int i = 0;
    for (const fb_device_desc_t *const *dp = fb_device_catalog(); *dp; dp++, i++) {
        const fb_device_desc_t *d = *dp;
        jp(&b, i ? ",\n{\"id\":" : "\n{\"id\":");
        jstr(&b, d->id);
        jp(&b, ",\"label\":"); jstr(&b, d->label);
        jp(&b, ",\"category\":"); jstr(&b, d->category);
        jp(&b, ",\"kind\":"); jstr(&b, d->kind);
        jp(&b, ",\"props\":");
        jparams(&b, d->props);
        jp(&b, ",\"faults\":[");
        for (int k = 0; d->faults && d->faults[k].id; k++) {
            jp(&b, k ? ",{\"id\":" : "{\"id\":");
            jstr(&b, d->faults[k].id);
            jp(&b, ",\"label\":"); jstr(&b, d->faults[k].label);
            if (d->faults[k].params) {
                jp(&b, ",\"params\":");
                jparams(&b, d->faults[k].params);
            }
            jp(&b, "}");
        }
        jp(&b, "],\"signals\":[");
        for (int k = 0; d->signals && d->signals[k]; k++) {
            if (k) jp(&b, ",");
            jstr(&b, d->signals[k]);
        }
        jp(&b, "],\"ports\":[");
        for (int k = 0; d->ports && d->ports[k].id; k++) {
            const fb_port_desc_t *p = &d->ports[k];
            jp(&b, k ? ",{\"id\":" : "{\"id\":");
            jstr(&b, p->id);
            jp(&b, ",\"kind\":"); jstr(&b, p->kind);
            jp(&b, ",\"dir\":"); jstr(&b, p->dir);
            if (p->label) { jp(&b, ",\"label\":"); jstr(&b, p->label); }
            if (p->optional) jp(&b, ",\"optional\":true");
            if (p->bundled) jp(&b, ",\"bundled\":true");
            jp(&b, "}");
        }
        jp(&b, "]");
        if (d->config) {
            jp(&b, ",\"config\":[");
            for (int k = 0; d->config[k]; k++) {
                if (k) jp(&b, ",");
                jstr(&b, d->config[k]);
            }
            jp(&b, "]");
        }
        if (d->config_fw) {
            jp(&b, ",\"config_firmware\":{");
            for (int f = 0; d->config_fw[f].firmware; f++) {
                if (f) jp(&b, ",");
                jstr(&b, d->config_fw[f].firmware);
                jp(&b, ":[");
                for (int k = 0; d->config_fw[f].lines[k]; k++) {
                    if (k) jp(&b, ",");
                    jstr(&b, d->config_fw[f].lines[k]);
                }
                jp(&b, "]");
            }
            jp(&b, "}");
        }
        jp(&b, "}");
    }
    jp(&b, "\n]\n");
    return b.len;
}
