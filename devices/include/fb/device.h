/* FlightBench device descriptors: what a device model announces about itself.
 *
 * Every model in devices/ ships one descriptor (fb_<dev>_desc()), devices without a model file
 * of their own (servo output, the generic GPS/compass) are described in src/catalog.c. The catalog
 * (fb_device_catalog()) is the core manifest's device list (docs/protocol.md `manifest`):
 * devices/tools/fb_manifest prints it as JSON, and the emulator checks `fault` commands against it.
 *
 *   id        stable name used in the session wiring and in `fault` messages ("crsf")
 *   category  what it is for the app (imu, baro, receiver, esc, servo, osd, gps, compass, battery,
 *             storage)
 *   kind      the board pad kind of its primary port (= ports[0].kind; kept for the old app)
 *   ports     every connection the device has (ESC: signal + tlm + rpm): id, the pad kind it
 *             plugs into (spi, i2c, uart, motor, servo, capture, gpio, power; a board's `output` pad
 *             takes a motor or a servo port), direction seen from the
 *             device (in / out / both); optional = works without it; bundled = rides on the
 *             primary port's pad (the IMU's INT line comes with its SPI pad), never wired alone
 *   props     options of a wiring entry (the session's wiring carries them)
 *   faults    what can be injected; each with optional numeric params
 *   signals   control-server signal ids the device feeds; "{n}" = the slot number
 *   config    firmware CLI lines the core appends to the custom defaults (a CLI-configured board:
 *             to its CLI script) when the device is wired
 *             (the device needs the firmware configured for it: a serial function, a driver
 *             setting); "{serial}" = the serial index of the UART pad it is wired to
 *   config_firmware  more such lines for one firmware family only (the board's
 *             `firmware_family` in board.json: "rotorflight", "inav"), after `config`; a
 *             family without an entry gets `config` alone (manifest: "config_firmware":
 *             {"<family>":[lines]})
 *
 * Whether the emulated machine has the device wired in is the machine's knowledge, not the
 * model's: the manifest composer adds "implemented".
 */
#ifndef FB_DEVICE_H
#define FB_DEVICE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_FAULT_MAX_PARAMS 4

typedef enum {
    FB_PARAM_INT, FB_PARAM_FLOAT, FB_PARAM_BOOL, FB_PARAM_ENUM, FB_PARAM_ACTUATOR,
} fb_param_type_t;

typedef struct {
    const char *id;                 /* NULL terminates a list */
    const char *label;
    fb_param_type_t type;
    bool has_range;
    double min, max;
    bool has_default;               /* no default and not optional = required (fault params) */
    double def;                     /* int/float/bool default */
    const char *def_str;            /* enum default */
    const char *const *options;     /* enum options, NULL-terminated */
    bool optional;                  /* fault param: absent = keep the current value */
} fb_param_desc_t;

typedef struct {
    const char *id;                 /* NULL terminates a list */
    const char *label;
    const fb_param_desc_t *params;  /* NULL or a list ended by id == NULL */
} fb_fault_desc_t;

typedef struct {
    const char *id;                 /* NULL terminates a list */
    const char *kind;               /* pad kind */
    const char *dir;                /* "in", "out", "both" (seen from the device) */
    const char *label;
    bool optional;
    bool bundled;
} fb_port_desc_t;

/* CLI lines for one firmware family */
typedef struct {
    const char *firmware;           /* "rotorflight", "inav"; NULL terminates a list */
    const char *const *lines;       /* NULL-terminated, "{serial}" substituted */
} fb_config_fw_t;

typedef struct {
    const char *id;
    const char *label;
    const char *category;
    const char *kind;               /* = ports[0].kind */
    const fb_param_desc_t *props;   /* NULL or a list ended by id == NULL */
    const fb_fault_desc_t *faults;  /* NULL or a list ended by id == NULL */
    const char *const *signals;     /* NULL or NULL-terminated */
    const fb_port_desc_t *ports;    /* at least one; list ended by id == NULL */
    const char *const *config;      /* NULL or NULL-terminated CLI lines ("{serial}" substituted) */
    const fb_config_fw_t *config_fw;    /* NULL or a list ended by firmware == NULL */
} fb_device_desc_t;

/* Descriptors of the models in this library */
const fb_device_desc_t *fb_crsf_desc(void);
const fb_device_desc_t *fb_icm42688p_desc(void);
const fb_device_desc_t *fb_esc_desc(void);
const fb_device_desc_t *fb_dps310_desc(void);
const fb_device_desc_t *fb_spl06_desc(void);
const fb_device_desc_t *fb_ublox_desc(void);
const fb_device_desc_t *fb_msp_osd_desc(void);
const fb_device_desc_t *fb_qmc5883l_desc(void);
const fb_device_desc_t *fb_battery_desc(void);

/* All devices, NULL-terminated, in a stable order */
const fb_device_desc_t *const *fb_device_catalog(void);
const fb_device_desc_t *fb_device_find(const char *id);
const fb_fault_desc_t *fb_device_fault(const fb_device_desc_t *d, const char *fault_id);
int fb_fault_nparams(const fb_fault_desc_t *f);
const fb_port_desc_t *fb_device_port(const fb_device_desc_t *d, const char *port_id);

/* The catalog as a JSON array (devices part of the manifest), one device per line.
 * Returns the length it needs (like snprintf); out may be NULL with n == 0. */
size_t fb_device_catalog_json(char *out, size_t n);

#ifdef __cplusplus
}
#endif
#endif
