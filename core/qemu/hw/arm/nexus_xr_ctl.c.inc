/*
 * FlightBench nexus-xr machine: the control server of docs/protocol.md.
 *
 * Included by nexus_xr.c (needs its state and helpers). Runs inside QEMU's
 * event loop: a TCP listener (QIONetListener) for any number of clients,
 * newline-delimited JSON both ways. Everything periodic is driven by the
 * machine's 1 kHz physics tick in virtual time (ctl_tick), so `state` and
 * `samples` are stamped and paced by the same clock as the FDM; the only
 * host-time things are the realtime throttle, the realtime-factor
 * measurement and the firmware file watch.
 *
 * The server only marshals: device and FDM logic stays in devices/ and fdm/.
 * It also owns the firmware's MSP UART (USART1) as a bus master: it polls
 * MSP_STATUS / MSP_ATTITUDE in the gaps of the host's traffic (replies to its
 * own requests are not forwarded to the host socket) and drives CLI mode.
 * Its own requests are MSP v2 ($X<): the replies come back as v2 ($X>), so
 * they are told apart from the host's v1 traffic by the version, not only
 * by the command.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "io/net-listener.h"
#include "io/channel-socket.h"
#include "qemu/sockets.h"
#include "qemu/main-loop.h"
#include "system/runstate.h"
#include "system/cpus.h"
#include "exec/icount.h"
#include "qemu/config-file.h"
#include "qemu/option.h"
#include "hw/core/cpu.h"
#ifndef _WIN32
#include <sys/utsname.h>
#endif

#include "qobject/qstring.h"
#include "qobject/qbool.h"

#include "fb_msp_modes.c.inc"
#include "fb_bench_state.c.inc"
#include "fb-devices/fb/cli_norm.h"

#define CTL_STATE_HZ        100
#define CTL_MAX_SIGS        128
#define CTL_OUT_SOFT        (64 * KiB)  /* above this, state/samples are dropped */
#define CTL_OUT_HARD        (4 * MiB)   /* above this, the client is dropped */
#define CTL_LINE_MAX        (64 * KiB)
#define CTL_RT_SLACK_NS     (200 * SCALE_US)  /* realtime: ahead by less is not worth a vCPU exit */

#define MSP_FC_VARIANT_CMD  2
#define MSP_FC_VERSION_CMD  3
#define MSP_BOARD_INFO_CMD  4
#define MSP_BOXNAMES_CMD    116
#define MSP_BOXIDS_CMD      119
#define MSP_RX_MAP_CMD      64
#define MSP_MODE_RANGES_CMD 34
#define MSP_MODE_RANGES_EXTRA_CMD 238           /* Betaflight/RotorFlight; iNav answers an error */
#define MSP_SET_MODE_RANGE_CMD 35               /* host commands the core watches for: the modes */
#define MSP_EEPROM_WRITE_CMD 250                /* ... and the saved config may have changed */
#define MODES_HOST_NS       (300 * SCALE_MS)    /* re-read the modes this long after the host's last */
#define MSP_STATUS_CMD      101
#define MSP_ATTITUDE_CMD    108
#define MSP_RC_CMD          105
#define MSP_ALTITUDE_CMD    109
#define MSP_CALIBRATION_DATA_CMD 14             /* iNav: u8 positions collected, accZero, accGain */
#define MSP_ACC_CALIBRATION_CMD  205            /* iNav: start sampling the current position */
#define MSP_MAG_CALIBRATION_CMD  206            /* iNav: CALIBRATE_MAG (fc_msp.c), mag_calibration_time */
#define MSP_RAW_GPS_CMD     106                 /* fix, sats, lat, lon (1e-7 deg), alt m, speed, course */
#define MSP_SENSOR_GPS      (1u << 3)           /* iNav packSensorStatus: sensors(SENSOR_GPS) << 3 */
#define MSP_SENSOR_MAG      (1u << 2)           /* iNav packSensorStatus: sensors(SENSOR_MAG) << 2 */
#define CAL_POSE_NS         (5 * NANOSECONDS_PER_SECOND / 2)   /* a position not collected this long: failed */
#define MSP2_INAV_STATUS_CMD 0x2000             /* iNav msp_protocol_v2_inav.h (MSP_STATUS is
                                                 * deprecated in iNav 9 and gone in 10) */
#define MSP_SENSOR_BARO     (1u << 1)           /* msp.c MSP_STATUS: sensors(SENSOR_BARO) << 1 */
#define MSP_POLL_NS         (33 * SCALE_MS)     /* one request per 33 ms: ~10 Hz each of 3 */
#define MSP_POLL4_NS        (25 * SCALE_MS)     /* with a baro, 4 in the cycle: still ~10 Hz */
#define MSP_POLL5_NS        (20 * SCALE_MS)     /* ... and the GPS, 5 */
#define MSP_TIMEOUT_NS      (200 * SCALE_MS)
#define MSP_GAP_NS          (10 * SCALE_MS)     /* an extra request only this far from the next poll */
#define MSP_STALE_NS        (2000 * SCALE_MS)   /* a timed-out request's late reply is still ours */
#define MSP_HOST_FRAME_NS   (100 * SCALE_MS)    /* a host frame stalled this long is abandoned */
#define CLI_ENTER_NS        (1000 * SCALE_MS)
#define CFG_REPLY_NS        (10 * NANOSECONDS_PER_SECOND)  /* config script: no prompt = failed */
#define CFG_ENTER_TRIES     3                   /* config script: CLI entries without a prompt */
#define CFG_LINE_MAX        63                  /* iNav fc/cli.c cliBuffer[64] */
#define DIFF_REPLY_NS       (10 * NANOSECONDS_PER_SECOND)  /* `diff all` ends with "save" by then */
#define FN_GYRO_NS          (20 * NANOSECONDS_PER_SECOND)  /* calibrate_gyro: calibrated after the reboot */
#define FN_ACC_NS           (5 * NANOSECONDS_PER_SECOND)   /* RotorFlight's level ACC calibration */
#define FN_SAVE_NS          (10 * NANOSECONDS_PER_SECOND)  /* a function's CLI save: the reboot */
#define FN_MAG_NS           (130 * NANOSECONDS_PER_SECOND) /* calibrate_mag: the firmware's result
                                                            * (mag_calibration_time is 20..120 s) */

/* RotorFlight fc/runtime_config.h armingDisableFlags_e order (bits 5/6 are GOVERNOR and
 * RPM_SIGNAL there, not Betaflight's RUNAWAY/CRASH) */
static const char *const arming_flag_names[] = {
    "NOGYRO", "FAILSAFE", "RXLOSS", "BADRX", "BOXFAILSAFE", "GOVERNOR", "RPM_SIGNAL",
    "THROTTLE", "ANGLE", "BOOTGRACE", "NOPREARM", "LOAD", "CALIB", "CLI", "CMS",
    "BST", "MSP", "PARALYZE", "GPS", "RESCUE_SW", "RPMFILTER", "REBOOT_REQD",
    "DSHOT_BBANG", "NO_ACC_CAL", "MOTOR_PROTO", "OVERRIDE", "ARMSWITCH",
};
#define ARM_FLAG_FAILSAFE   (1u << 1)
#define ARM_FLAG_RXLOSS     (1u << 2)
#define ARM_FLAG_CALIB      (1u << 12)          /* ARMING_DISABLED_CALIBRATING (gyro, acc) */
#define ARM_FLAG_NO_ACC_CAL (1u << 23)          /* ARMING_DISABLED_ACC_CALIBRATION */

/* iNav fc/runtime_config.h armingFlag_e: bit 2 ARMED, 3 WAS_EVER_ARMED, 4/5 simulator; the
 * blockers from bit 6 on, named as its CLI `status` names them (runtime_config.c
 * armingDisableFlagNames: bit b = name b - 6) */
#define INAV_ARMED          (1u << 2)
#define INAV_BLOCKER_BIT0   6
static const char *const inav_blocker_names[] = {
    "GEOZONE", "FS", "ANGLE", "CAL", "OVRLD", "NAV", "COMPASS",
    "ACC", "ARMSW", "HWFAIL", "BOXFS", "PLACEHOLDER", "RX",
    "THR", "CLI", "CMS", "OSD", "ROLL/PITCH", "AUTOTRIM", "OOM",
    "SETTINGFAIL", "PWMOUT", "NOPREARM", "DSHOTBEEPER", "LANDED",
};
#define INAV_FLAG_FS        (1u << 7)           /* ARMING_DISABLED_FAILSAFE_SYSTEM */
#define INAV_FLAG_RX        (1u << 18)          /* ARMING_DISABLED_RC_LINK */
#define INAV_FLAG_CAL       (1u << 9)           /* ARMING_DISABLED_SENSORS_CALIBRATING */
#define INAV_FLAG_ACC       (1u << 13)          /* ARMING_DISABLED_ACCELEROMETER_NOT_CALIBRATED */
#define INAV_FLAG_COMPASS   (1u << 12)          /* ARMING_DISABLED_COMPASS_NOT_CALIBRATED */
#define MAX_BOXES           64

/* ------------------------------------------------------------------ */
/* Signals: every value the core knows, by stable id                   */

#define SIGNAL_LIST(X) \
    X(FDM_POS_N, "fdm.pos_n_m", "m") X(FDM_POS_E, "fdm.pos_e_m", "m") \
    X(FDM_POS_D, "fdm.pos_d_m", "m") X(FDM_ALT, "fdm.alt_m", "m") \
    X(FDM_VEL_N, "fdm.vel_n_mps", "m/s") X(FDM_VEL_E, "fdm.vel_e_mps", "m/s") \
    X(FDM_VEL_D, "fdm.vel_d_mps", "m/s") \
    X(FDM_QW, "fdm.quat_w", "") X(FDM_QX, "fdm.quat_x", "") \
    X(FDM_QY, "fdm.quat_y", "") X(FDM_QZ, "fdm.quat_z", "") \
    X(FDM_ROLL, "fdm.roll_deg", "deg") X(FDM_PITCH, "fdm.pitch_deg", "deg") \
    X(FDM_YAW, "fdm.yaw_deg", "deg") \
    X(FDM_P, "fdm.rate_p_dps", "deg/s") X(FDM_Q, "fdm.rate_q_dps", "deg/s") \
    X(FDM_R, "fdm.rate_r_dps", "deg/s") \
    X(FDM_AX, "fdm.accel_x_mps2", "m/s2") X(FDM_AY, "fdm.accel_y_mps2", "m/s2") \
    X(FDM_AZ, "fdm.accel_z_mps2", "m/s2") \
    X(FDM_COLL, "fdm.collective_deg", "deg") X(FDM_THRUST, "fdm.thrust_n", "N") \
    X(FDM_ON_GROUND, "fdm.on_ground", "bool") X(HEAD_RPM, "fdm.head_rpm", "rpm") \
    X(IMU_GX, "imu.gyro_x_dps", "deg/s") X(IMU_GY, "imu.gyro_y_dps", "deg/s") \
    X(IMU_GZ, "imu.gyro_z_dps", "deg/s") \
    X(IMU_AX, "imu.accel_x_g", "g") X(IMU_AY, "imu.accel_y_g", "g") \
    X(IMU_AZ, "imu.accel_z_g", "g") \
    X(IMU_NGX, "imu.noisy.gyro_x_dps", "deg/s") X(IMU_NGY, "imu.noisy.gyro_y_dps", "deg/s") \
    X(IMU_NGZ, "imu.noisy.gyro_z_dps", "deg/s") \
    X(IMU_NAX, "imu.noisy.accel_x_g", "g") X(IMU_NAY, "imu.noisy.accel_y_g", "g") \
    X(IMU_NAZ, "imu.noisy.accel_z_g", "g") X(IMU_TEMP, "imu.noisy.temp_c", "degC") \
    X(IMU_ODR, "imu.odr_hz", "Hz") X(IMU_SAMPLES, "imu.samples", "") \
    X(OUT_S1, "out.servo1_us", "us") X(OUT_S2, "out.servo2_us", "us") \
    X(OUT_S3, "out.servo3_us", "us") X(OUT_S4, "out.servo4_us", "us") \
    X(OUT_DSHOT, "out.motor1_dshot", "") X(OUT_BIDIR, "out.motor1_bidir", "bool") \
    X(ESC_THR, "esc.throttle", "") X(ESC_RPM, "esc.motor_rpm", "rpm") \
    X(ESC_ERPM, "esc.erpm", "rpm") X(ESC_CURRENT, "esc.current_a", "A") \
    X(ESC_LOAD, "esc.load", "") \
    X(RC1, "rc.ch1_us", "us") X(RC2, "rc.ch2_us", "us") X(RC3, "rc.ch3_us", "us") \
    X(RC4, "rc.ch4_us", "us") X(RC5, "rc.ch5_us", "us") X(RC6, "rc.ch6_us", "us") \
    X(RC7, "rc.ch7_us", "us") X(RC8, "rc.ch8_us", "us") X(RC9, "rc.ch9_us", "us") \
    X(RC10, "rc.ch10_us", "us") X(RC11, "rc.ch11_us", "us") X(RC12, "rc.ch12_us", "us") \
    X(RC13, "rc.ch13_us", "us") X(RC14, "rc.ch14_us", "us") X(RC15, "rc.ch15_us", "us") \
    X(RC16, "rc.ch16_us", "us") \
    X(LINK_UP, "link.up", "bool") X(LINK_RSSI, "link.rssi_dbm", "dBm") \
    X(LINK_LQ, "link.lq", "%") X(LINK_SNR, "link.snr_db", "dB") \
    X(CRSF_TLM, "crsf.telemetry_frames", "") \
    X(OSD_FRAMES, "osd.frames", "") \
    X(BARO_PA, "baro.pressure_pa", "Pa") X(BARO_C, "baro.temp_c", "degC") \
    X(FC_BARO_ALT, "fc.baro_alt_m", "m") \
    X(FC_ARMED, "fc.armed", "bool") X(FC_CPU, "fc.cpu_pct", "%") \
    X(FC_FLAGS, "fc.arming_flags", "bits") X(FC_ROLL, "fc.attitude.roll_deg", "deg") \
    X(FC_PITCH, "fc.attitude.pitch_deg", "deg") X(FC_YAW, "fc.attitude.yaw_deg", "deg") \
    X(FC_CLI, "fc.cli", "bool") X(FC_POWER, "fc.power", "bool") \
    X(RT_FACTOR, "timing.realtime_factor", "") \
    X(FDM_AIRSPEED, "fdm.airspeed_mps", "m/s") X(FDM_ALPHA, "fdm.alpha_deg", "deg") \
    X(FDM_BETA, "fdm.beta_deg", "deg") \
    X(MAG_X, "mag.x_g", "G") X(MAG_Y, "mag.y_g", "G") X(MAG_Z, "mag.z_g", "G") \
    X(GPS_LAT, "gps.lat_deg", "deg") X(GPS_LON, "gps.lon_deg", "deg") X(GPS_ALT, "gps.alt_m", "m") \
    X(GPS_FIX, "gps.fix", "") X(GPS_SATS, "gps.num_sat", "") \
    X(GPS_SPEED, "gps.ground_speed_mps", "m/s") X(GPS_COURSE, "gps.course_deg", "deg") \
    X(BAT_V, "battery.voltage_v", "V") X(BAT_A, "battery.current_a", "A") \
    X(BAT_MAH, "battery.mah_used", "mAh") X(BAT_SOC, "battery.soc", "") \
    X(FC_GPS_FIX, "fc.gps.fix", "") X(FC_GPS_SATS, "fc.gps.num_sat", "") \
    X(FC_GPS_LAT, "fc.gps.lat_deg", "deg") X(FC_GPS_LON, "fc.gps.lon_deg", "deg")

#define SIG_ENUM(e, id, unit) SIG_##e,
enum { SIGNAL_LIST(SIG_ENUM) SIG_COUNT };
#define SIG_ID(e, id, unit) id,
static const char *const sig_id[] = { SIGNAL_LIST(SIG_ID) };
#define SIG_UNIT(e, id, unit) unit,
static const char *const sig_unit[] = { SIGNAL_LIST(SIG_UNIT) };

/* ------------------------------------------------------------------ */

typedef struct CtlClient {
    struct NxCtl *ctl;
    QIOChannelSocket *sioc;
    guint in_src, out_src;
    GString *in, *out;
    int64_t period_ns, next_ns;     /* state/samples stream, virtual time */
    bool log, osd, dead;
    bool modes;                     /* subscribe modes:true: the core reads the modes by itself */
    bool modes_ask;                 /* asked {"type":"modes"}: gets the next result */
    bool osd_sync;                  /* has the OSD canvas: gets deltas (else the next is full) */
    int nsig;
    uint16_t sig[CTL_MAX_SIGS];
    uint64_t dropped;
    char name[64];
    char *snap_path;                /* {"type":"config","snapshot":"bench","path":..}: the file to
                                     * write when the `diff all` it started ends */
    bool diff_script;               /* {"type":"diff","against":"script"}: gets the comparison */
    bool apply_wait;                /* its `config apply` waits on a `diff all`: an error to it */
} CtlClient;

typedef struct NxCtl {
    NexusXRState *s;
    QIONetListener *listener;
    GPtrArray *clients;
    QEMUBH *reap_bh;

    /* time */
    bool paused;
    bool realtime;
    double speed;
    int64_t rt_v0, rt_h0;           /* realtime anchor: virtual ns <-> host ns */
    QEMUTimer *step_timer;
    int64_t step_deadline;          /* virtual ns the running step ends at */
    int64_t host_start;
    int64_t win_v[11], win_h[11];   /* realtime factor over the last ~1 s */
    int64_t win_s[11];              /* idle-skipped virtual ns at each window sample */
    double idle_pct;                /* share of virtual time skipped over the window */
    int win_n, win_i;
    int64_t win_next;
    double rtf;
    bool rt_sleep_queued;           /* a throttle sleep is queued on the vCPU */
    bool rt_warned;

    /* firmware + watch */
    char *fw_path;
    bool watch;
    guint watch_src;
    int64_t watch_mtime, watch_size, watch_seen_mtime, watch_seen_size;
    uint8_t *fw_pending;            /* flash image waiting for the reset */
    bool erase_cfg;

    /* USART1 as MSP master / CLI */
    bool cli, cli_entering;
    bool osd_link;                  /* the last `osd` message had link:true (none sent: false) */
    bool cli_core_sent;             /* the core owns this entry ('#' sent or pending) */
    bool cli_need_entry;            /* the core's '#' waits for a gap between host frames */
    int64_t cli_enter_ns;
    GQueue cli_q;
    GString *line;
    uint8_t fr[1100];
    unsigned fr_len, fr_need;
    int poll_cmd;                   /* outstanding request, 0 = none */
    int64_t poll_ns, next_poll_ns;
    unsigned poll_i;
    int stale_cmd;                  /* timed out, its reply may still come: swallow it */
    int64_t stale_ns;
    uint64_t poll_bad, poll_late;   /* replies with a bad checksum / after the timeout */
    int64_t host_ns;                /* last byte from the host socket */
    /* host -> FC MSP framing ($M< / $X< header, length, payload, checksum): the core only
     * injects between complete host frames, and '#' is a CLI entry only between frames */
    uint8_t hf[8];
    unsigned hf_len, hf_need;

    /* what MSP told us */
    bool have_variant, have_version, have_board, have_boxes, have_status, have_att;
    bool boxnames_err;              /* iNav refused MSP_BOXNAMES (too many boxes): names by id */
    char variant[8];
    uint8_t version[3];
    char target[40], board[40];
    char boxes[MAX_BOXES][24];
    int nbox;
    uint64_t mode_bits;             /* bit i = boxes[i] active */
    uint32_t arm_flags;             /* the firmware's word (RotorFlight or iNav layout, `inav`) */
    bool fs;                        /* failsafe as last reported (the `failsafe` event's state) */
    bool inav;                      /* FC_VARIANT "INAV": MSP2_INAV_STATUS, iNav's flag names */
    unsigned cpu_pct;
    bool armed;
    int16_t att[3];

    uint16_t last_dshot_cmd;
    unsigned crashes_seen;          /* FDM hard contacts already reported as `crash` */

    /* MSP_STATUS sensors: bit 1 = SENSOR_BARO (the firmware detected a baro) */
    uint16_t sensors;
    /* MSP_ALTITUDE: the firmware's altitude estimate (baro only on this board), cm; polled
     * only while MSP_STATUS reports the baro */
    bool have_alt;
    int32_t alt_cm;

    /* MSP_RAW_GPS: the firmware's GPS solution; polled only while MSP_STATUS reports the GPS */
    bool have_gps;
    uint8_t gps_fix, gps_sats;
    int32_t gps_lat, gps_lon;       /* 1e-7 deg */
    int16_t gps_alt_m;
    uint16_t gps_speed_cms, gps_course_dd;

    /* MSP_RC: the channels the firmware sees (after its map) */
    bool have_rc;
    int nrc;
    uint16_t rc[16];

    /* The board's CLI config script (board->cli_config; s->config_cli): after a reset that left
     * the config sector blank, sent over the MSP UART once the firmware answers MSP, one line per
     * CLI prompt, then `save` (the firmware writes the sector and reboots); the saved sector is
     * kept as a snapshot keyed by firmware + script hash and written back instead of running
     * the script again (reset all, the same firmware reloaded). */
    enum { CFG_NONE, CFG_PENDING, CFG_ENTER, CFG_SEND, CFG_DIFF, CFG_SAVE, CFG_DONE, CFG_FAILED,
           CFG_FN, CFG_CHECK } cfg;
    char **cfg_lines;               /* the whole script */
    int cfg_n, cfg_i;
    char **cfg_run;                 /* the lines this run sends: cfg_lines, or the delta */
    int cfg_run_n;
    uint32_t fs_mask;               /* first-start functions to run (bit i = s->fn[i]) */

    /* The bench state (-M bench-state=<file>; bs_*): the config sector written to <file> after
     * every save of the firmware (the flash locked after writes into the sector), + <file>.json
     * (firmware, the script applied, the functions' inputs, the diff); restored at start instead
     * of the snapshot, then only the script's changed lines (the delta) are applied and only the
     * automatic functions whose results are missing run. */
    char *bs_script;                /* the script the sector has; NULL = not ours yet (not kept) */
    GHashTable *bs_fn;              /* function name -> the key of its inputs, results in the sector */
    bool bs_from_bench;             /* the sector came from the bench state: the snapshot is left */
    bool cfg_delta;                 /* the run is the delta on the bench state */
    bool bs_skip;                   /* the next blank-sector reset ignores the bench state */
    const char *reset_first;        /* config reset in progress: "first_start" | "defaults" */
    bool cfg_apply;                 /* the run is a `config apply` (the bench's script now) */
    bool apply_wait;                /* a `config apply` waits on its `diff all` (the firmware's
                                     * config now: the settings it changed get the script's value) */
    GString *bs_extra;              /* ,"removed":[...] of the delta (the events' data) */
    int64_t bs_check_t0;
    uint8_t *bs_pending;            /* the sector as the firmware locked the flash: bs_bh writes it */
    QEMUBH *bs_bh;
    unsigned bs_saves;
    bool bs_warned;
    bool bs_off;                    /* a newer core's file: not restored, not overwritten this run */
    const char *cli_by;             /* who opened the CLI: "core" | "client" */
    const char *cli_via;            /* ... over "control" (a client's cli command) | "msp" */
    int cfg_tries;                  /* CLI entries tried for this run of the script */
    bool cfg_err;                   /* the firmware printed "### ERROR" for the current line */
    bool cfg_saved;                 /* the firmware printed "Saving" (cli.c cliSave: after writeEEPROM,
                                     * flushed with "Rebooting") */
    GString *cfg_errtext;
    /* iNav's 6-position ACC calibration (the calibrate_acc function with a poses scenario;
     * CFG_FN when the first start runs it after the script's save and reboot): the scenario holds
     * the model in each pose; at each pose event with an iNav position `index` the core sends
     * MSP_ACC_CALIBRATION and polls MSP_CALIBRATION_DATA until the firmware has that position;
     * after the scenario the CLI `save` (the first start: its final save, and the sector with the
     * calibration becomes the snapshot; the key includes the scenario). */
    int cal_pose;                   /* iNav position being sampled, -1 = none */
    char cal_label[FB_SCENARIO_NAME];
    uint16_t cal_cmd;               /* the calibration's MSP request due, 0 = none */
    int64_t cal_t0;                 /* virtual ns the position's sampling was asked for */
    uint8_t cal_flags;              /* MSP_CALIBRATION_DATA byte 0 (iNav's configurator bit order) */
    int16_t cal_zero[3], cal_gain[3];
    int16_t cal_mag_zero[3], cal_mag_gain[3];  /* MSP_CALIBRATION_DATA (iNav): magZero, magGain */
    int cal_n;                      /* positions collected */
    unsigned cal_gen;               /* FbEnv.sc_gen of the calibration's scenario */
    int64_t cfg_t0;                 /* virtual ns of the last line / save sent */

    char *cfg_last;                 /* the last `config` event's data (sent again to a new client) */

    /* `modes`: MSP_BOXNAMES (pages of 32 on RotorFlight), MSP_BOXIDS, MSP_RX_MAP,
     * MSP_MODE_RANGES (+ MSP_MODE_RANGES_EXTRA on RotorFlight), read on request, and — only while
     * a client is subscribed (`subscribe` modes:true) — after every boot once the firmware answers
     * and after the host changed modes or saved. The extra MSP traffic shifts the firmware's
     * timing, so with no subscriber the board sees exactly the regular polls (the flight hashes).
     * The result goes to the subscribers and the clients that asked (a new subscriber the last). */
    enum { MS_NONE, MS_NAMES, MS_NAMES_P1, MS_IDS, MS_IDS_P1, MS_MAP, MS_RANGES, MS_EXTRA } ms;
    bool modes_want;                /* a read is due */
    bool modes_dirty;               /* a boot / the host may have changed them since the last read */
    bool modes_names;               /* ... with MSP_BOXNAMES again (the boot read has them) */
    int ms_tries;                   /* timeouts of the current step */
    bool have_modes;
    int64_t modes_host_ns;          /* the host changed modes / saved: re-read then (0 = no) */
    bool modes_host_eeprom;         /* ... and it saved: the diff is stale */
    fbm_modes_t modes, mnext;       /* the last complete read, the one in progress */
    uint8_t mranges[256];           /* MSP_MODE_RANGES of the read in progress (for _EXTRA) */
    unsigned nmranges;
    char *modes_msg;                /* the last `modes` message */

    /* `diff`: the firmware's `diff all`, captured in the config script's CLI session before its
     * final save (CFG_DIFF), or on request in a CLI session of its own (diff_st) that ends with
     * `exit` (the firmware reboots); kept with the config snapshot */
    enum { DIFF_IDLE, DIFF_ENTER, DIFF_RUN, DIFF_EXIT } diff_st;
    bool diff_capture;              /* CLI lines go into diff_buf, not to the log */
    bool diff_echo;                 /* the next line is the echo of `diff all` */
    bool diff_saw_save;             /* the dump's last line ("save") came: the next prompt ends it */
    int64_t diff_t0;
    GString *diff_buf;
    char *diff_text;                /* the last complete dump, NULL = none */
    bool diff_stale;                /* the firmware's config may have changed since */
    const char *diff_source;        /* first_start | snapshot | request */
    double diff_at;                 /* virtual s it was captured */

    /* environment functions (s->fn): one calibration at a time; `launch` alongside */
    int fn_cur;                     /* index into s->fn, -1 = none */
    bool fn_first;                  /* ... run by the first start (the config flow saves) */
    enum { FN_WAIT, FN_RUN, FN_HOLD, FN_REBOOT, FN_CHECK, FN_SAVE } fn_st;
    int fn_step, fn_steps;
    int64_t fn_t0;
    double fn_hold_s;               /* calibrate_gyro: the catalog's hold_s or the command's */
    bool fn_seen;                   /* calibrate_*: the firmware reported calibrating */
    bool fn_saved;                  /* FN_SAVE: the firmware printed "Saving" */
    int fs_i;                       /* first start: the auto function running */
    bool fs_done;                   /* first start: the functions ran, the save that follows is the last */
    bool diff_cfg_fresh;            /* the config flow's CFG_DIFF captured the diff of this save */
    char *fn_last;                  /* the last `function` message, without its closing "}\n" */

    char *manifest;                 /* one JSON line (scripts/manifest.py), NULL = none */
    int64_t prof_next;              /* virtual ns of the next periodic `prof` (prof-period-s) */
    uint32_t faults_seen;           /* active-fault bits already reported as `fault` events */
    bool host_held;                 /* ctl_cfg_busy() at the last tick: the MSP host socket is not read */
} NxCtl;

/* ------------------------------------------------------------------ */
/* JSON writing                                                        */

static void js_str(GString *g, const char *str)
{
    g_string_append_c(g, '"');
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        switch (*p) {
        case '"': g_string_append(g, "\\\""); break;
        case '\\': g_string_append(g, "\\\\"); break;
        case '\n': g_string_append(g, "\\n"); break;
        case '\r': g_string_append(g, "\\r"); break;
        case '\t': g_string_append(g, "\\t"); break;
        default:
            if (*p < 0x20 || *p == 0x7F) {
                g_string_append_printf(g, "\\u%04x", *p);
            } else {
                g_string_append_c(g, *p);
            }
        }
    }
    g_string_append_c(g, '"');
}

/* numbers through devices/src/json.c: integers as integers, else %.9g, NaN/Inf -> null */
static void js_num(GString *g, double v)
{
    char b[FB_JSON_NUM_MAX];
    fb_json_num(b, sizeof(b), v);
    g_string_append(g, b);
}

/* times (virtual/host seconds): fixed 4 decimals, 0.1 ms at any t */
static void js_time(GString *g, double t)
{
    char b[FB_JSON_NUM_MAX];
    fb_json_time(b, sizeof(b), t);
    g_string_append(g, b);
}

/* ",\"key\":<number>" */
static void js_kv_num(GString *g, const char *key, double v)
{
    g_string_append_printf(g, ",\"%s\":", key);
    js_num(g, v);
}

static void js_vec(GString *g, const double *v, int n)
{
    g_string_append_c(g, '[');
    for (int i = 0; i < n; i++) {
        if (i) {
            g_string_append_c(g, ',');
        }
        js_num(g, v[i]);
    }
    g_string_append_c(g, ']');
}

static double vnow_s(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9;
}

/* ------------------------------------------------------------------ */
/* Clients                                                             */

static gboolean ctl_client_out(QIOChannel *ioc, GIOCondition cond, gpointer opaque);

static void ctl_client_kill(CtlClient *c)
{
    if (!c->dead) {
        c->dead = true;
        qemu_bh_schedule(c->ctl->reap_bh);
    }
}

static void ctl_flush_body(CtlClient *c);

static void ctl_flush(CtlClient *c)
{
    PROF_ENTER(pa.ctl_tx);
    ctl_flush_body(c);
    PROF_LEAVE(pa.ctl_tx);
}

static void ctl_flush_body(CtlClient *c)
{
    while (!c->dead && c->out->len) {
        ssize_t n = qio_channel_write(QIO_CHANNEL(c->sioc), c->out->str,
                                      c->out->len, NULL);
        if (n == QIO_CHANNEL_ERR_BLOCK) {
            if (!c->out_src) {
                c->out_src = qio_channel_add_watch(QIO_CHANNEL(c->sioc), G_IO_OUT,
                                                   ctl_client_out, c, NULL);
            }
            return;
        }
        if (n <= 0) {
            ctl_client_kill(c);
            return;
        }
        g_string_erase(c->out, 0, n);
        PROF_BYTES(pa.ctl_tx, n);
    }
}

static gboolean ctl_client_out(QIOChannel *ioc, GIOCondition cond, gpointer opaque)
{
    CtlClient *c = opaque;
    c->out_src = 0;
    ctl_flush(c);
    return G_SOURCE_REMOVE;
}

/* droppable: state/samples, skipped for a client that does not keep up */
static void ctl_send(CtlClient *c, const GString *msg, bool droppable)
{
    if (c->dead) {
        return;
    }
    if (droppable && c->out->len > CTL_OUT_SOFT) {
        c->dropped++;
        return;
    }
    if (c->out->len + msg->len > CTL_OUT_HARD) {
        ctl_client_kill(c);
        return;
    }
    g_string_append_len(c->out, msg->str, msg->len);
    ctl_flush(c);
}

static void ctl_broadcast(NxCtl *ctl, const GString *msg, bool log_only)
{
    for (guint i = 0; i < ctl->clients->len; i++) {
        CtlClient *c = g_ptr_array_index(ctl->clients, i);
        if (!log_only || c->log) {
            ctl_send(c, msg, false);
        }
    }
}

static void G_GNUC_PRINTF(3, 4)
ctl_log(NexusXRState *s, const char *src, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_autofree char *text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    if (!strcmp(src, "core")) {
        fprintf(stderr, "fbctl t=%.3f: %s\n", vnow_s(), text);
    }
    if (!s->ctl) {
        return;
    }
    g_autoptr(GString) g = g_string_new("{\"type\":\"log\",\"src\":");
    js_str(g, src);
    g_string_append(g, ",\"t\":");
    js_time(g, vnow_s());
    g_string_append(g, ",\"text\":");
    js_str(g, text);
    g_string_append(g, "}\n");
    ctl_broadcast(s->ctl, g, true);
}

/* data: a JSON object body without braces, e.g. "\"on\":true" (may be "") */
static void ctl_event(NexusXRState *s, const char *name, const char *data)
{
    fprintf(stderr, "fbctl t=%.3f: event %s {%s}\n", vnow_s(), name, data);
    if (!s->ctl) {
        return;
    }
    g_autoptr(GString) g = g_string_new("{\"type\":\"event\",\"t\":");
    js_time(g, vnow_s());
    g_string_append(g, ",\"name\":");
    js_str(g, name);
    g_string_append_printf(g, ",\"data\":{%s}}\n", data);
    ctl_broadcast(s->ctl, g, false);
}

static void ctl_error(CtlClient *c, const char *what)
{
    g_autoptr(GString) g = g_string_new("{\"type\":\"error\",\"what\":");
    js_str(g, what);
    g_string_append(g, "}\n");
    ctl_send(c, g, false);
}

/* ------------------------------------------------------------------ */
/* Snapshot of all signals                                             */


static void ctl_snapshot(NexusXRState *s, double *v)
{
    NxCtl *ctl = s->ctl;
    for (int i = 0; i < 3; i++) {
        v[SIG_IMU_GX + i] = s->motion.gyro_dps[i];
        v[SIG_IMU_AX + i] = s->motion.accel_g[i];
        v[SIG_IMU_NGX + i] = s->imu_in_gyro[i];
        v[SIG_IMU_NAX + i] = s->imu_in_accel[i];
    }
    for (int i = 0; i < 4; i++) {
        v[SIG_OUT_S1 + i] = s->servo_us[i];
    }
    v[SIG_IMU_TEMP] = s->imu_in_temp;
    v[SIG_IMU_ODR] = s->imu_odr;
    v[SIG_IMU_SAMPLES] = s->imu_samples;
    v[SIG_OUT_DSHOT] = s->dshot_value;
    v[SIG_OUT_BIDIR] = s->dshot_bidir;
    v[SIG_ESC_THR] = fb_esc_throttle(s->esc);
    v[SIG_ESC_RPM] = fb_esc_rpm(s->esc);
    v[SIG_ESC_ERPM] = fb_esc_erpm(s->esc);
    v[SIG_ESC_CURRENT] = fb_esc_current_a(s->esc);
    for (int i = 0; i < 16; i++) {
        v[SIG_RC1 + i] = s->pilot_ch[i];
    }
    {
        int rssi, lq, snr;
        bool up;
        fb_crsf_link_reported(s->crsf, &rssi, &lq, &snr, &up);
        if (s->crsf_uart < 0) {
            up = false;             /* no receiver wired: nothing links */
            lq = 0;
        }
        v[SIG_LINK_UP] = up;
        v[SIG_LINK_RSSI] = rssi;
        v[SIG_LINK_LQ] = lq;
        v[SIG_LINK_SNR] = snr;
    }
    v[SIG_CRSF_TLM] = fb_crsf_rx_frames(s->crsf);
    v[SIG_OSD_FRAMES] = s->osd_uart >= 0 ? fb_msp_osd_stats(s->osd)->frames : NAN;
    v[SIG_BARO_PA] = s->baro_present ? s->baro_pa : NAN;
    v[SIG_BARO_C] = s->baro_present ? s->baro_c : NAN;
    v[SIG_FC_BARO_ALT] = ctl->have_alt ? ctl->alt_cm / 100.0 : NAN;
    v[SIG_FC_ARMED] = ctl->armed;
    v[SIG_FC_CPU] = ctl->cpu_pct;
    v[SIG_FC_FLAGS] = ctl->arm_flags;
    v[SIG_FC_ROLL] = ctl->have_att ? ctl->att[0] / 10.0 : NAN;
    v[SIG_FC_PITCH] = ctl->have_att ? ctl->att[1] / 10.0 : NAN;
    v[SIG_FC_YAW] = ctl->have_att ? ctl->att[2] : NAN;
    v[SIG_FC_CLI] = ctl->cli;
    v[SIG_FC_POWER] = !s->power_off;
    v[SIG_RT_FACTOR] = ctl->rtf;
    v[SIG_FDM_AIRSPEED] = v[SIG_FDM_ALPHA] = v[SIG_FDM_BETA] = NAN;
    for (int i = 0; i < 3; i++) {
        v[SIG_MAG_X + i] = s->mag_present ? s->mag_chip[i] : NAN;
    }
    {
        const fb_ublox_solution_t *g = s->gps_uart >= 0 ? fb_ublox_solution(s->gps) : NULL;
        v[SIG_GPS_LAT] = g ? g->lat_deg : NAN;
        v[SIG_GPS_LON] = g ? g->lon_deg : NAN;
        v[SIG_GPS_ALT] = g ? g->alt_msl_m : NAN;
        v[SIG_GPS_FIX] = g ? g->fix_type : NAN;
        v[SIG_GPS_SATS] = g ? g->num_sv : NAN;
        v[SIG_GPS_SPEED] = g ? g->ground_speed_mps : NAN;
        v[SIG_GPS_COURSE] = g ? g->course_deg : NAN;
    }
    v[SIG_BAT_V] = s->battery_present ? fb_battery_voltage(s->batt) : NAN;
    v[SIG_BAT_A] = s->battery_present ? fb_battery_current_a(s->batt) : NAN;
    v[SIG_BAT_MAH] = s->battery_present ? fb_battery_mah_used(s->batt) : NAN;
    v[SIG_BAT_SOC] = s->battery_present ? fb_battery_soc(s->batt) : NAN;
    v[SIG_FC_GPS_FIX] = ctl->have_gps ? ctl->gps_fix : NAN;
    v[SIG_FC_GPS_SATS] = ctl->have_gps ? ctl->gps_sats : NAN;
    v[SIG_FC_GPS_LAT] = ctl->have_gps ? ctl->gps_lat / 1e7 : NAN;
    v[SIG_FC_GPS_LON] = ctl->have_gps ? ctl->gps_lon / 1e7 : NAN;
    fdm_snapshot(s, v);             /* the FDM's part */
}

static const char *ctl_actuator(NexusXRState *s, int i)
{
    return s->act_names[i];     /* servo1..n, then the motor (filled at init) */
}

static void ctl_mode_str(NxCtl *ctl, char *buf, size_t len)
{
    buf[0] = 0;
    for (int i = 0; i < ctl->nbox && i < MAX_BOXES; i++) {
        if ((ctl->mode_bits >> i & 1) && strcmp(ctl->boxes[i], "ARM")) {
            if (buf[0]) {
                g_strlcat(buf, "+", len);
            }
            g_strlcat(buf, ctl->boxes[i], len);
        }
    }
    if (!buf[0]) {
        g_strlcpy(buf, ctl->have_status ? "ACRO" : "", len);
    }
}

/* ------------------------------------------------------------------ */
/* Faults: declared by the device descriptors (devices/include/fb/device.h), applied to the
 * device models this machine has wired in                             */

#define CTL_MAX_FAULTS 32

typedef struct {
    const fb_device_desc_t *dev;
    const fb_fault_desc_t *fault;
} CtlFault;

/* the fault-taking models this session has wired in (scripts/wiring.py resolves the session's
 * wiring into -M imu=, crsf=, esc=) */
static bool ctl_dev_wired(NexusXRState *s, const char *id)
{
    if (!id) {
        return false;
    }
    if (!strcmp(id, "crsf")) {
        return s->crsf_uart >= 0;
    }
    if (!strcmp(id, "icm42688p")) {
        return s->imu_present;
    }
    if (!strcmp(id, "esc")) {
        return s->esc_present;
    }
    if (!strcmp(id, "dps310")) {
        return s->baro_present && !s->spl06;
    }
    if (!strcmp(id, "spl06")) {
        return s->baro_present && s->spl06;
    }
    if (!strcmp(id, "msp_osd")) {
        return s->osd_uart >= 0;
    }
    if (!strcmp(id, "qmc5883l")) {
        return s->mag_present;
    }
    if (!strcmp(id, "ublox")) {
        return s->gps_uart >= 0;
    }
    if (!strcmp(id, "battery")) {
        return s->battery_present;
    }
    return false;
}

/* every (device, fault) this machine can inject, in catalog order */
static int ctl_fault_list(NexusXRState *s, CtlFault *out)
{
    int n = 0;
    for (const fb_device_desc_t *const *d = fb_device_catalog(); *d; d++) {
        if (!ctl_dev_wired(s, (*d)->id)) {
            continue;
        }
        for (const fb_fault_desc_t *f = (*d)->faults; f && f->id && n < CTL_MAX_FAULTS; f++) {
            out[n].dev = *d;
            out[n++].fault = f;
        }
    }
    return n;
}

static bool ctl_fault_set(NexusXRState *s, const char *dev, const char *kind, bool on,
                          const double *p)
{
    if (!strcmp(dev, "crsf")) {
        bool ok = fb_crsf_set_fault(s->crsf, kind, on, p);
        if (ok && !strcmp(kind, "link_lost")) {
            s->link_up = !on;
        }
        return ok;
    }
    if (!strcmp(dev, "icm42688p")) {
        return fb_icm42688p_set_fault(s->imu, kind, on, p);
    }
    if (!strcmp(dev, "esc")) {
        return fb_esc_set_fault(s->esc, kind, on, p);
    }
    if (!strcmp(dev, "dps310")) {
        return fb_dps310_set_fault(s->baro, kind, on, p);
    }
    if (!strcmp(dev, "msp_osd")) {
        bool ok = fb_msp_osd_set_fault(s->osd, kind, on, p);
        ctl_osd_link(s);                /* link_lost on: the picture is gone */
        return ok;
    }
    if (!strcmp(dev, "spl06")) {
        return fb_spl06_set_fault(s->spl06, kind, on, p);
    }
    if (!strcmp(dev, "qmc5883l")) {
        return fb_qmc5883l_set_fault(s->mag, kind, on, p);
    }
    if (!strcmp(dev, "ublox")) {
        return fb_ublox_set_fault(s->gps, kind, on, p);
    }
    if (!strcmp(dev, "battery")) {
        return fb_battery_set_fault(s->batt, kind, on, p);
    }
    return false;
}

static bool ctl_fault_on(NexusXRState *s, const char *dev, const char *kind)
{
    if (!strcmp(dev, "crsf")) {
        return fb_crsf_fault_active(s->crsf, kind);
    }
    if (!strcmp(dev, "icm42688p")) {
        return fb_icm42688p_fault_active(s->imu, kind);
    }
    if (!strcmp(dev, "esc")) {
        return fb_esc_fault_active(s->esc, kind);
    }
    if (!strcmp(dev, "dps310")) {
        return fb_dps310_fault_active(s->baro, kind);
    }
    if (!strcmp(dev, "msp_osd")) {
        return fb_msp_osd_fault_active(s->osd, kind);
    }
    if (!strcmp(dev, "spl06")) {
        return fb_spl06_fault_active(s->spl06, kind);
    }
    if (!strcmp(dev, "qmc5883l")) {
        return fb_qmc5883l_fault_active(s->mag, kind);
    }
    if (!strcmp(dev, "ublox")) {
        return fb_ublox_fault_active(s->gps, kind);
    }
    if (!strcmp(dev, "battery")) {
        return fb_battery_fault_active(s->batt, kind);
    }
    return false;
}

static uint32_t ctl_fault_bits(NexusXRState *s, CtlFault *list, int *n)
{
    uint32_t bits = 0;
    *n = ctl_fault_list(s, list);
    for (int i = 0; i < *n; i++) {
        if (ctl_fault_on(s, list[i].dev->id, list[i].fault->id)) {
            bits |= 1u << i;
        }
    }
    return bits;
}

/* one `fault` event per change, whatever the path (fault, link, pilot socket) */
static void ctl_faults_check(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    CtlFault list[CTL_MAX_FAULTS];
    int n;
    uint32_t bits = ctl_fault_bits(s, list, &n);
    uint32_t chg = bits ^ ctl->faults_seen;
    for (int i = 0; chg && i < n; i++) {
        if (chg >> i & 1) {
            g_autofree char *d = g_strdup_printf("\"device\":\"%s\",\"kind\":\"%s\",\"on\":%s",
                                                 list[i].dev->id, list[i].fault->id,
                                                 bits >> i & 1 ? "true" : "false");
            ctl_event(s, "fault", d);
        }
    }
    ctl->faults_seen = bits;
}

/* An optional boolean field: absent -> def; present, it must be a JSON true/false (a string
 * "false" or a number is an error, never silently the default). False on a bad type. */
static bool ctl_opt_bool(QDict *d, const char *key, bool def, bool *out)
{
    QObject *o = qdict_get(d, key);
    QBool *b = o ? qobject_to(QBool, o) : NULL;
    if (o && !b) {
        return false;
    }
    *out = b ? qbool_get_bool(b) : def;
    return true;
}

static void ctl_cmd_fault(CtlClient *c, QDict *d)
{
    NexusXRState *s = c->ctl->s;
    const char *dev = qdict_get_try_str(d, "device");
    const char *kind = qdict_get_try_str(d, "kind");
    bool on;
    const fb_device_desc_t *dd = fb_device_find(dev);
    const fb_fault_desc_t *fd = fb_device_fault(dd, kind);
    double p[FB_FAULT_MAX_PARAMS];
    QObject *po = qdict_get(d, "params");
    QDict *pd = po ? qobject_to(QDict, po) : NULL;

    if (!ctl_opt_bool(d, "on", true, &on)) {
        ctl_error(c, "fault: on must be true|false");
        return;
    }
    if (po && !pd) {
        ctl_error(c, "fault: params must be an object");
        return;
    }
    if (!dd) {
        g_autofree char *m = g_strdup_printf("fault: unknown device %s", dev ? dev : "(none)");
        ctl_error(c, m);
        return;
    }
    if (!fd) {
        g_autofree char *m = g_strdup_printf("fault: device %s has no fault %s", dev,
                                             kind ? kind : "(none)");
        ctl_error(c, m);
        return;
    }
    if (!ctl_dev_wired(s, dev)) {
        g_autofree char *m = g_strdup_printf("fault: device %s is not wired in this session", dev);
        ctl_error(c, m);
        return;
    }
    int np = MIN(fb_fault_nparams(fd), FB_FAULT_MAX_PARAMS);
    /* every key of params is one of the fault's parameters (a typo is not a silent default) */
    for (const QDictEntry *e = pd ? qdict_first(pd) : NULL; e; e = qdict_next(pd, e)) {
        int k = -1;
        for (int i = 0; i < np; i++) {
            if (!strcmp(fd->params[i].id, qdict_entry_key(e))) {
                k = i;
            }
        }
        if (k < 0) {
            g_autofree char *m = g_strdup_printf("fault %s.%s: unknown param %s", dev, kind,
                                                 qdict_entry_key(e));
            ctl_error(c, m);
            return;
        }
    }
    for (int i = 0; i < np; i++) {
        const fb_param_desc_t *pp = &fd->params[i];
        QObject *qo = pd ? qdict_get(pd, pp->id) : NULL;
        QNum *q = qo ? qobject_to(QNum, qo) : NULL;
        const char *bad = NULL;
        if (qo && !q) {
            bad = "not a number";
        } else if (!q) {
            if (pp->has_default) {
                p[i] = pp->def;
            } else if (pp->optional || !on) {
                p[i] = NAN;
            } else {
                bad = "missing";
            }
        } else {
            p[i] = qnum_get_double(q);
            if (pp->type == FB_PARAM_INT && p[i] != floor(p[i])) {
                bad = "not an integer";
            } else if (pp->has_range && (p[i] < pp->min || p[i] > pp->max)) {
                bad = "out of range";
            }
        }
        if (bad) {
            g_autofree char *m = g_strdup_printf("fault %s.%s: params.%s %s (%g..%g)", dev, kind,
                                                 pp->id, bad, pp->min, pp->max);
            ctl_error(c, m);
            return;
        }
    }
    if (!ctl_fault_set(s, dev, kind, on, np ? p : NULL)) {
        /* the descriptor lists it but the model refused it: descriptor and model disagree */
        g_autofree char *m = g_strdup_printf("fault %s.%s: the device model did not accept it",
                                             dev, kind);
        ctl_error(c, m);
        return;
    }
    ctl_log(s, "core", "fault %s.%s %s", dev, kind, on ? "ON" : "off");
    ctl_faults_check(s);
}

/* ,"osd":{...} the OSD this machine models, or null: an MSP DisplayPort device wired to a UART
 * (the core streams what the firmware draws: `osd` messages), the board's MAX7456 (the chip is
 * modelled so the firmware's driver runs; no picture comes out). The MAX7456's rows follow its
 * VM0 register: PAL 16, NTSC 13 (PAL, iNav's default, until the firmware set the mode). */
static void ctl_osd_info(NexusXRState *s, GString *g)
{
    if (s->osd_uart >= 0) {
        g_string_append_printf(g, ",\"osd\":{\"kind\":\"msp_displayport\",\"cols\":%d,\"rows\":%d,"
                               "\"video\":true,\"uart\":%d}", OSD_COLS, OSD_ROWS, s->osd_uart + 1);
    } else if (s->osd_chip) {
        uint8_t vm0 = fb_max7456_peek(s->osd_chip, 0x00);
        bool ntsc = vm0 && !(vm0 & 0x40);
        g_string_append_printf(g, ",\"osd\":{\"kind\":\"max7456\",\"cols\":30,\"rows\":%d,"
                               "\"video_system\":%s,\"video\":false}", ntsc ? 13 : 16,
                               !vm0 ? "null" : ntsc ? "\"NTSC\"" : "\"PAL\"");
    } else {
        g_string_append(g, ",\"osd\":null");
    }
}

static void ctl_state_json(NexusXRState *s, const double *v, GString *g)
{
    NxCtl *ctl = s->ctl;
    double t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9;
    char mode[128];

    g_string_append(g, "{\"type\":\"state\",\"t\":");
    js_time(g, t);
    g_string_append(g, ",\"pos_ned\":");
    js_vec(g, &v[SIG_FDM_POS_N], 3);
    g_string_append(g, ",\"vel_ned\":");
    js_vec(g, &v[SIG_FDM_VEL_N], 3);
    g_string_append(g, ",\"quat\":");
    js_vec(g, &v[SIG_FDM_QW], 4);
    g_string_append(g, ",\"euler_deg\":");
    js_vec(g, &v[SIG_FDM_ROLL], 3);
    g_string_append(g, ",\"gyro_dps\":");
    js_vec(g, &v[SIG_IMU_GX], 3);
    g_string_append(g, ",\"accel_g\":");
    js_vec(g, &v[SIG_IMU_AX], 3);
    g_string_append(g, ",\"rpm\":");
    js_num(g, round(v[SIG_HEAD_RPM] * 10) / 10);
    g_string_append(g, ",\"motor_rpm\":");
    js_num(g, round(v[SIG_ESC_RPM]));
    /* an output with nothing wired (servo not in servos=, motor with esc=off): null; a wired
     * output without a signal (no pulse on the pad at the last physics tick; the ESC past its
     * signal-loss timeout): null in pwm_us, the actuator holds its last value, named in no_signal */
    const int ns = s->board->nservo;
    bool wired[FB_MAX_SERVOS + 1], sig[FB_MAX_SERVOS + 1];
    for (int i = 0; i <= ns; i++) {
        wired[i] = i < ns ? (s->servo_mask >> i & 1) : s->esc_present;
        sig[i] = i < ns ? (s->servo_sig >> i & 1) : !fb_esc_signal_lost(s->esc);
    }
    g_string_append(g, ",\"actuators\":{");
    for (int i = 0; i <= ns; i++) {
        double a;
        if (!wired[i]) {
            a = 0;
        } else if (i < ns) {
            a = (s->servo_us[i] - 1500.0) / 500.0;
            if (s->servo_rev >> i & 1) {
                a = -a;
            }
            a = MAX(-1.0, MIN(1.0, a));
        } else {
            a = fb_esc_throttle(s->esc);
        }
        if (i) {
            g_string_append_c(g, ',');
        }
        js_str(g, ctl_actuator(s, i));
        g_string_append_c(g, ':');
        if (wired[i]) {
            js_num(g, round(a * 1e4) / 1e4);
        } else {
            g_string_append(g, "null");
        }
    }
    g_string_append(g, "},\"pwm_us\":[");
    for (int i = 0; i <= ns; i++) {
        if (i) {
            g_string_append_c(g, ',');
        }
        if (!wired[i] || !sig[i]) {
            g_string_append(g, "null");
        } else {
            g_string_append_printf(g, "%u", i < ns ? s->servo_us[i]
                                   : 1000 + (unsigned)lrintf(1000 * fb_esc_throttle(s->esc)));
        }
    }
    g_string_append(g, "],\"no_signal\":[");
    for (int i = 0, n = 0; i <= ns; i++) {
        if (wired[i] && !sig[i]) {
            if (n++) {
                g_string_append_c(g, ',');
            }
            js_str(g, ctl_actuator(s, i));
        }
    }
    g_string_append_printf(g, "],\"dshot\":%u", s->dshot_value);
    g_string_append_printf(g, ",\"on_ground\":%s",
                           v[SIG_FDM_ON_GROUND] ? "true" : "false");
    if (s->fdm_on) {
        fdm_state_json(s, g);
    }

    ctl_osd_info(s, g);
    ctl_mode_str(ctl, mode, sizeof(mode));
    g_string_append_printf(g, ",\"fc\":{\"msp\":%s,\"armed\":%s,\"mode\":",
                           ctl->have_status ? "true" : "false",
                           ctl->armed ? "true" : "false");
    js_str(g, mode);
    if (ctl->have_modes && ctl->have_status) {
        /* the active boxes by permanent id (`modes` boxes[].id): the status bits by box index */
        uint8_t ids[FBM_MAX_BOXES];
        int n = fbm_active_ids(&ctl->modes, ctl->mode_bits, ctl->inav ? 64 : 32, ids);
        g_string_append(g, ",\"boxes\":[");
        for (int i = 0; i < n; i++) {
            g_string_append_printf(g, i ? ",%u" : "%u", ids[i]);
        }
        g_string_append_c(g, ']');
    }
    g_string_append(g, ",\"arming_flags\":[");
    if (ctl->inav) {
        for (unsigned i = 0, n = 0; i < ARRAY_SIZE(inav_blocker_names); i++) {
            if (ctl->arm_flags >> (INAV_BLOCKER_BIT0 + i) & 1) {
                if (n++) {
                    g_string_append_c(g, ',');
                }
                js_str(g, inav_blocker_names[i]);
            }
        }
    }
    for (unsigned i = 0, n = 0; !ctl->inav && i < ARRAY_SIZE(arming_flag_names); i++) {
        if (ctl->arm_flags >> i & 1) {
            if (n++) {
                g_string_append_c(g, ',');
            }
            js_str(g, arming_flag_names[i]);
        }
    }
    g_string_append_printf(g, "],\"cpu_pct\":%u,\"cli\":%s,\"power\":%s",
                           ctl->cpu_pct, ctl->cli ? "true" : "false",
                           s->power_off ? "false" : "true");
    if (ctl->have_att) {
        double a[3] = { ctl->att[0] / 10.0, ctl->att[1] / 10.0, ctl->att[2] };
        g_string_append(g, ",\"attitude_deg\":");
        js_vec(g, a, 3);
    }
    if (ctl->have_alt) {
        g_string_append(g, ",\"baro_alt_m\":");
        js_num(g, ctl->alt_cm / 100.0);
    }
    if (ctl->have_gps) {
        g_string_append_printf(g, ",\"gps\":{\"fix\":%u,\"sats\":%u,\"lat\":", ctl->gps_fix,
                               ctl->gps_sats);
        js_num(g, ctl->gps_lat / 1e7);
        g_string_append(g, ",\"lon\":");
        js_num(g, ctl->gps_lon / 1e7);
        g_string_append_printf(g, ",\"alt_m\":%d,\"speed_mps\":", ctl->gps_alt_m);
        js_num(g, ctl->gps_speed_cms / 100.0);
        g_string_append(g, ",\"course_deg\":");
        js_num(g, ctl->gps_course_dd / 10.0);
        g_string_append_c(g, '}');
    }
    if (ctl->have_rc) {
        g_string_append(g, ",\"rc_us\":[");
        for (int i = 0; i < ctl->nrc; i++) {
            g_string_append_printf(g, i ? ",%u" : "%u", ctl->rc[i]);
        }
        g_string_append_c(g, ']');
    }
    g_string_append_printf(g, ",\"link\":{\"up\":%s,\"rssi_dbm\":%d,\"lq\":%d,"
                           "\"snr_db\":%d}}", v[SIG_LINK_UP] ? "true" : "false",
                           (int)v[SIG_LINK_RSSI], (int)v[SIG_LINK_LQ], (int)v[SIG_LINK_SNR]);
    {
        CtlFault list[CTL_MAX_FAULTS];
        int n;
        uint32_t bits = ctl_fault_bits(s, list, &n);
        g_string_append(g, ",\"faults\":[");
        for (int i = 0, k = 0; i < n; i++) {
            if (bits >> i & 1) {
                g_string_append_printf(g, "%s\"%s.%s\"", k++ ? "," : "", list[i].dev->id,
                                       list[i].fault->id);
            }
        }
        g_string_append_c(g, ']');
    }
    if (s->battery_present) {
        g_string_append(g, ",\"battery\":{\"voltage_v\":");
        js_num(g, round(fb_battery_voltage(s->batt) * 1000) / 1000);
        g_string_append(g, ",\"current_a\":");
        js_num(g, round(fb_battery_current_a(s->batt) * 1000) / 1000);
        g_string_append(g, ",\"mah_used\":");
        js_num(g, round(fb_battery_mah_used(s->batt) * 10) / 10);
        g_string_append(g, ",\"soc\":");
        js_num(g, round(fb_battery_soc(s->batt) * 10000) / 10000);
        g_string_append_printf(g, ",\"cells\":%d}", fb_battery_config(s->batt)->cells);
    }
    g_string_append(g, ",\"rc_us\":[");
    for (int i = 0; i < 16; i++) {
        g_string_append_printf(g, i ? ",%u" : "%u", s->pilot_ch[i]);
    }
    g_string_append(g, "],\"timing\":{\"realtime_factor\":");
    js_num(g, round(ctl->rtf * 1000) / 1000);
    g_string_append(g, ",\"host_t\":");
    js_time(g, (g_get_monotonic_time() * 1000 - ctl->host_start) / 1e9);
    g_string_append_printf(g, ",\"realtime\":%s,\"speed\":",
                           ctl->realtime ? "true" : "false");
    js_num(g, ctl->speed);
    g_string_append_printf(g, ",\"idle_skip\":%s,\"idle_skip_pct\":",
                           s->idle_skip ? "true" : "false");
    js_num(g, round(ctl->idle_pct * 10) / 10);
    g_string_append(g, "}}\n");
}

static void ctl_samples_json(CtlClient *c, const double *v, GString *g)
{
    g_string_append(g, "{\"type\":\"samples\",\"t\":");
    js_time(g, vnow_s());
    g_string_append(g, ",\"v\":{");
    for (int i = 0; i < c->nsig; i++) {
        if (i) {
            g_string_append_c(g, ',');
        }
        js_str(g, sig_id[c->sig[i]]);
        g_string_append_c(g, ':');
        js_num(g, v[c->sig[i]]);
    }
    g_string_append(g, "}}\n");
}

/* ------------------------------------------------------------------ */
/* Firmware images: ELF / Intel HEX / raw binary into a flash buffer    */

/* A regular file of at most `max` bytes (a FIFO, a device or a huge file is not read).
 * g_stat (not stat): path is a -M/fw property value, UTF-8; g_stat converts to UTF-16 on Windows. */
static bool fw_read_file(const char *path, size_t max, gchar **d, gsize *n, Error **errp)
{
    GStatBuf st;
    GError *ge = NULL;
    if (g_stat(path, &st) < 0) {
        error_setg_errno(errp, errno, "Failed to open file");
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        error_setg(errp, "not a regular file");
        return false;
    }
    if ((uint64_t)st.st_size > max) {
        error_setg(errp, "%" PRIu64 " bytes: larger than %zu", (uint64_t)st.st_size, max);
        return false;
    }
    if (!g_file_get_contents(path, d, n, &ge)) {
        error_setg(errp, "%s", ge->message);
        g_error_free(ge);
        return false;
    }
    if (*n > max) {                 /* grew between stat and read */
        g_free(*d);
        *d = NULL;
        error_setg(errp, "%zu bytes: larger than %zu", (size_t)*n, max);
        return false;
    }
    return true;
}

/* The firmware file plus the custom-defaults blob, as a full flash image. The parsers are in
 * devices/src/fw_image.c (tests/test_fw_image.c feeds them broken files). */
static uint8_t *fw_build_image(NexusXRState *s, const char *path, Error **errp)
{
    g_autofree gchar *d = NULL;
    gsize n;
    const FbSoc *soc = s->soc;
    g_autofree uint8_t *img = g_malloc(soc->flash_size);
    const fb_fw_flash_t fl = { img, soc->flash_base, soc->flash_size, soc->flash_alias };
    char err[160];

    memset(img, 0xFF, soc->flash_size);
    if (!fw_read_file(path, FB_FW_MAX_FILE, &d, &n, errp)) {
        error_prepend(errp, "%s: ", path);
        return NULL;
    }
    if (!fb_fw_parse(&fl, (const uint8_t *)d, n, err, sizeof(err))) {
        error_setg(errp, "%s: %s", path, err);
        return NULL;
    }
    /* the blob: defaults=<file>, else the bench's (in memory, -M session=) */
    g_autofree gchar *dd = NULL;
    gsize dn = 0;
    const uint8_t *blob = NULL;
    if (s->defaults_path && *s->defaults_path) {
        if (!fw_read_file(s->defaults_path, FB_FW_CUSTOM_DEFAULTS_MAX, &dd, &dn, errp)) {
            error_prepend(errp, "defaults %s: ", s->defaults_path);
            return NULL;
        }
        blob = (const uint8_t *)dd;
    } else if (s->defaults_blob) {
        blob = s->defaults_blob;
        dn = s->defaults_len;
    }
    if (blob) {
        uint32_t start, end;
        /* where the image says the area is (tools/mkdefaults.py reads the same pointers).
         * Without valid pointers the F7 default area is used only when the image leaves it
         * erased; anything else there is firmware code/data the blob would overwrite. */
        if (!fb_fw_custom_defaults_area(&fl, &start, &end, err, sizeof(err))) {
            uint32_t ds = soc->flash_base + 0x2808, de = soc->flash_base + 0x4000;
            if (!fb_fw_erased(&fl, ds, de)) {
                error_setg(errp, "firmware %s: %s, and the F7 default area 0x%08x..0x%08x is "
                           "not erased: refusing to place the custom defaults", path, err,
                           ds, de);
                return NULL;
            }
            start = ds;
            end = de;
            ctl_log(s, "core", "firmware %s: %s; custom defaults go to the erased F7 default "
                    "area 0x%08x..0x%08x", path, err, start, end);
        } else if (!fb_fw_erased(&fl, start, end)) {
            error_setg(errp, "firmware %s: the custom-defaults area 0x%08x..0x%08x is not "
                       "erased (the image already has data there): refusing to overwrite it",
                       path, start, end);
            return NULL;
        }
        if (dn > end - start) {
            error_setg(errp, "defaults: %zu bytes do not fit 0x%08x..0x%08x", (size_t)dn,
                       start, end);
            return NULL;
        }
        memcpy(img + (start - soc->flash_base), blob, dn);
    }
    return g_steal_pointer(&img);
}

static void fw_install(NexusXRState *s, const uint8_t *img)
{
    g_free(s->fw_hash);
    s->fw_hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, img, s->soc->flash_size);
    memcpy(memory_region_get_ram_ptr(&s->flash), img, s->soc->flash_size);
    memory_region_flush_rom_device(&s->flash, 0, s->soc->flash_size);
}

/* hello's firmware string: from MSP once the firmware answered, else the file name */
static char *fw_describe(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    if (ctl && ctl->have_variant && ctl->have_version && ctl->have_board) {
        const char *v = !strcmp(ctl->variant, "RTFL") ? "rotorflight"
                      : !strcmp(ctl->variant, "INAV") ? "inav" : ctl->variant;
        return g_strdup_printf("%s %u.%u.%u %s%s%s%s", v, ctl->version[0],
                               ctl->version[1], ctl->version[2], ctl->target,
                               ctl->board[0] ? " (" : "", ctl->board,
                               ctl->board[0] ? ")" : "");
    }
    return s->firmware_path ? g_path_get_basename(s->firmware_path) : g_strdup("unknown");
}

/* ------------------------------------------------------------------ */
/* USART1: MSP master and CLI                                          */

/* MSP v2 checksum: CRC-8/DVB-S2 over flag, cmd, size and payload */
static uint8_t msp_crc8_dvb_s2(uint8_t crc, const uint8_t *p, unsigned n)
{
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = crc & 0x80 ? (uint8_t)(crc << 1) ^ 0xD5 : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* the core's own requests are MSP v2 ($X<, flag 0, u16 cmd, u16 size 0): the firmware answers
 * in the version it was asked in, so a v1 reply is always the host's */
static bool fc_send_msp_p(NexusXRState *s, uint16_t cmd, const uint8_t *pl, unsigned n)
{
    uint8_t f[16] = { '$', 'X', '<', 0, cmd & 0xFF, cmd >> 8, n & 0xFF, n >> 8 };
    assert(n <= sizeof(f) - 9);
    memcpy(f + 8, pl, n);
    f[8 + n] = msp_crc8_dvb_s2(0, f + 3, 5 + n);
    return fc_inject(s, f, 9 + n);
}


/* one CLI line, whole or not at all */
static bool fc_cli_line(NexusXRState *s, const char *text)
{
    g_autofree char *l = g_strdup_printf("%s\r", text);
    if (!fc_inject(s, l, strlen(l))) {
        ctl_log(s, "core", "cli: '%s' dropped: the MSP UART FIFO is full", text);
        return false;
    }
    return true;
}

static void fc_cli_flush_queue(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    char *cmd;
    while ((cmd = g_queue_pop_head(&ctl->cli_q))) {
        fc_cli_line(s, cmd);
        g_free(cmd);
    }
}

static void diff_mark_stale(NexusXRState *s, const char *why);

static void fc_line(NexusXRState *s, const char *text)
{
    NxCtl *ctl = s->ctl;
    if (ctl->diff_capture && ctl->cli) {
        /* `diff all`: the dump goes into the `diff` message, not line by line into the log */
        if (ctl->diff_echo) {
            ctl->diff_echo = false;
            if (g_str_has_suffix(text, "diff all")) {
                return;                 /* the prompt with the command's echo */
            }
        }
        g_string_append(ctl->diff_buf, text);
        g_string_append_c(ctl->diff_buf, '\n');
        if (!strcmp(text, "save")) {
            ctl->diff_saw_save = true;  /* the dump's last line; its prompt ends it (cfg_prompt) */
        }
        return;
    }
    if (strstr(text, "Entering CLI Mode")) {
        bool was = ctl->cli;
        ctl->cli = true;
        ctl->cli_entering = ctl->cli_core_sent = ctl->cli_need_entry = false;
        ctl->poll_cmd = 0;
        if (!was) {
            ctl_log(s, "core", "firmware in CLI mode (MSP polling paused; "
                    "'exit' leaves it and reboots the board)");
            ctl_osd_link(s);            /* the firmware stops drawing in CLI mode */
            if (!ctl->cli_by) {
                ctl->cli_by = "client";     /* no entry seen: a host tool's '#' inside its stream */
                ctl->cli_via = "msp";
            }
            g_autofree char *d = g_strdup_printf("\"open\":true,\"by\":\"%s\",\"via\":\"%s\"",
                                                 ctl->cli_by, ctl->cli_via);
            ctl_event(s, "cli", d);
        }
        fc_cli_flush_queue(s);
    }
    if (!strncmp(text, "Saving", 6)) {
        if (ctl->cfg == CFG_SAVE) {
            ctl->cfg_saved = true;      /* the config flow's: its diff was captured just before */
        } else {
            if (ctl->fn_cur >= 0 && ctl->fn_st == FN_SAVE) {
                ctl->fn_saved = true;
            }
            diff_mark_stale(s, "a CLI save");
        }
    }
    if (ctl->cfg == CFG_SEND && !strncmp(text, "### ERROR", 9)) {
        ctl->cfg_err = true;
        g_string_assign(ctl->cfg_errtext, text);
    }
    ctl_log(s, ctl->cli ? "cli" : "fc", "%s", text);
}

static void cfg_prompt(NexusXRState *s);

/* the config script is sending lines or saving: the MSP UART's host socket is not read (a
 * configurator's MSP bytes would land in the CLI line buffer) */
static bool ctl_cfg_busy(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    return ctl && (ctl->cfg == CFG_SEND || ctl->cfg == CFG_DIFF || ctl->cfg == CFG_SAVE ||
                   ctl->diff_st == DIFF_RUN || ctl->diff_st == DIFF_EXIT);
}

static void fc_text(NexusXRState *s, const uint8_t *buf, int len)
{
    NxCtl *ctl = s->ctl;
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            fc_line(s, ctl->line->str);
            g_string_truncate(ctl->line, 0);
        } else if (buf[i] != '\r' && ctl->line->len < 4096) {
            g_string_append_c(ctl->line, buf[i]);
            if (ctl->cli && buf[i] == ' ' && !strcmp(ctl->line->str, "# ")) {
                cfg_prompt(s);          /* the CLI prompt ("\r\n# "): ready for a line */
            }
        }
    }
}

static void modes_reply(NexusXRState *s, unsigned cmd, const uint8_t *p, unsigned n, bool err);

/* the status every firmware reports (MSP_STATUS / MSP2_INAV_STATUS, decoded): events on change */
static void fc_status(NexusXRState *s, bool armed, uint32_t flags, bool fs_new, bool rxloss,
                      uint64_t modes, unsigned cpu, uint16_t sensors)
{
    NxCtl *ctl = s->ctl;
    bool fs_old = ctl->fs;
    if (ctl->have_status && armed != ctl->armed) {
        ctl_event(s, armed ? "armed" : "disarmed", "");
    }
    if (ctl->have_status && fs_old != fs_new) {
        g_autofree char *d = g_strdup_printf("\"on\":%s,\"rxloss\":%s", fs_new ? "true" : "false",
                                             rxloss ? "true" : "false");
        ctl_event(s, "failsafe", d);
    }
    ctl->armed = armed;
    ctl->mode_bits = modes;
    ctl->arm_flags = flags;
    ctl->fs = fs_new;
    ctl->cpu_pct = cpu;
    ctl->sensors = sensors;
    if (!(ctl->sensors & MSP_SENSOR_BARO)) {
        ctl->have_alt = false;      /* no baro detected: no altitude to report */
    }
    ctl->have_status = true;
}

static void mag_cal_started(NexusXRState *s);
static void mag_cal_result(NexusXRState *s);
static bool fn_is(NexusXRState *s, int i, const char *name);
static char *fn_inputs(NexusXRState *s, int i);
static void bs_keep(NexusXRState *s);
static GPtrArray *cfg_split(const char *text);
static void cfg_prov_get(NexusXRState *s, int i, char **layer, char **src);
static fb_cli_line_t *cfg_script_lines(NexusXRState *s, char ***own);
static void js_cfg_run(GString *g, NexusXRState *s);

static void fc_decode(NexusXRState *s, unsigned cmd, const uint8_t *p, unsigned n)
{
    NxCtl *ctl = s->ctl;
    switch (cmd) {
    case MSP_FC_VARIANT_CMD:
        if (n >= 4) {
            memcpy(ctl->variant, p, 4);
            ctl->variant[4] = 0;
            ctl->have_variant = true;
            ctl->inav = !strcmp(ctl->variant, "INAV");
        }
        break;
    case MSP_FC_VERSION_CMD:
        if (n >= 3) {
            memcpy(ctl->version, p, 3);
            ctl->have_version = true;
        }
        break;
    case MSP_BOARD_INFO_CMD:
        ctl->target[0] = ctl->board[0] = 0;
        if (n >= 9 && 9u + p[8] <= n) {
            unsigned l = MIN(p[8], sizeof(ctl->target) - 1), o = 9 + p[8];
            memcpy(ctl->target, p + 9, l);
            ctl->target[l] = 0;
            if (o < n && o + 1u + p[o] <= n) {
                l = MIN(p[o], sizeof(ctl->board) - 1);
                memcpy(ctl->board, p + o + 1, l);
                ctl->board[l] = 0;
            }
        }
        ctl->have_board = true;
        {
            g_autofree char *fw = fw_describe(s);
            ctl_log(s, "core", "firmware says: %s", fw);
        }
        break;
    case MSP_BOXNAMES_CMD: {
        if (ctl->ms == MS_NAMES || ctl->ms == MS_NAMES_P1) {
            modes_reply(s, cmd, p, n, false);
            break;
        }
        ctl->nbox = 0;
        unsigned st = 0;
        for (unsigned i = 0; i <= n && ctl->nbox < MAX_BOXES; i++) {
            if (i == n || p[i] == ';') {
                if (i > st) {
                    unsigned l = MIN(i - st, sizeof(ctl->boxes[0]) - 1);
                    memcpy(ctl->boxes[ctl->nbox], p + st, l);
                    ctl->boxes[ctl->nbox++][l] = 0;
                }
                st = i + 1;
            }
        }
        ctl->have_boxes = true;
        break;
    }
    case MSP_STATUS_CMD:
        /* msp.c MSP_STATUS: u16 pid dt, u16 gyro dt, u16 sensors, u32 flight modes @6, u8
         * profile, u16 max load, u16 avg cpu @13, u8 extra flight-mode byte count @15 (+ that
         * many bytes; RotorFlight writes 0), u8 arming-flag count, u32 arming flags @17 + extra */
        if (n >= 16 && n >= 21u + p[15]) {
            uint32_t modes = ldl_le_p(p + 6), flags = ldl_le_p(p + 17 + p[15]);
            bool armed = false;
            for (int i = 0; i < ctl->nbox && i < 32; i++) {
                if (!strcmp(ctl->boxes[i], "ARM")) {
                    armed = modes >> i & 1;
                }
            }
            if (!ctl->have_boxes) {
                armed = modes & 1;
            }
            fc_status(s, armed, flags, flags & (ARM_FLAG_FAILSAFE | ARM_FLAG_RXLOSS),
                      flags & ARM_FLAG_RXLOSS, modes, lduw_le_p(p + 13) / 10, lduw_le_p(p + 4));
        }
        break;
    case MSP2_INAV_STATUS_CMD:
        /* fc_msp.c MSP2_INAV_STATUS: u16 cycle time, u16 I2C errors, u16 sensors @4, u16 system
         * load % @6, u8 battery/config profile, u32 arming flags @9 (bit 2 = ARMED), the box
         * mode bitmask @13 (bit i = the i-th name of MSP_BOXNAMES; sizeof(boxBitmask_t) bytes),
         * u8 mixer profile */
        if (n >= 14) {
            uint32_t flags = ldl_le_p(p + 9);
            uint64_t modes = 0;
            for (unsigned i = 0; i < 8 && 13 + i < n - 1; i++) {
                modes |= (uint64_t)p[13 + i] << (8 * i);
            }
            /* iNav sets FS/RC_LINK only while disarmed (fc_core.c updateArmingStatus, failsafe.c
             * FAILSAFE_LANDED): armed, failsafe shows only as the FAILSAFE box */
            bool fs_box = false;
            for (int i = 0; ctl->have_boxes && i < ctl->nbox && i < MAX_BOXES; i++) {
                fs_box |= (modes >> i & 1) && !strcmp(ctl->boxes[i], "FAILSAFE");
            }
            fc_status(s, flags & INAV_ARMED, flags, fs_box || (flags & (INAV_FLAG_FS | INAV_FLAG_RX)),
                      flags & INAV_FLAG_RX, modes, lduw_le_p(p + 6), lduw_le_p(p + 4));
        }
        break;
    case MSP_RC_CMD:
        ctl->nrc = MIN(n / 2, 16u);
        for (int i = 0; i < ctl->nrc; i++) {
            ctl->rc[i] = lduw_le_p(p + 2 * i);
        }
        ctl->have_rc = ctl->nrc > 0;
        break;
    case MSP_ALTITUDE_CMD:
        if (n >= 4) {
            ctl->alt_cm = (int32_t)ldl_le_p(p);     /* getEstimatedAltitudeCm() */
            ctl->have_alt = true;
        }
        break;
    case MSP_RAW_GPS_CMD:
        if (n >= 16) {
            ctl->gps_fix = p[0];
            ctl->gps_sats = p[1];
            ctl->gps_lat = (int32_t)ldl_le_p(p + 2);
            ctl->gps_lon = (int32_t)ldl_le_p(p + 6);
            ctl->gps_alt_m = (int16_t)lduw_le_p(p + 10);
            ctl->gps_speed_cms = lduw_le_p(p + 12);
            ctl->gps_course_dd = lduw_le_p(p + 14);
            ctl->have_gps = true;
        }
        break;
    case MSP_MAG_CALIBRATION_CMD:
        if (ctl->cal_cmd == cmd) {
            ctl->cal_cmd = 0;
            mag_cal_started(s);
        }
        break;
    case MSP_ATTITUDE_CMD:
        if (n >= 6) {
            for (int i = 0; i < 3; i++) {
                ctl->att[i] = (int16_t)lduw_le_p(p + 2 * i);
            }
            ctl->have_att = true;
        }
        break;
    case MSP_BOXIDS_CMD:
        if (!ctl->have_boxes && ctl->boxnames_err && !ctl->ms) {
            char nm[MAX_BOXES][FBM_NAME];
            int nb;
            fbm_inav_names(p, MIN(n, MAX_BOXES), nm, &nb, MAX_BOXES);
            for (int i = 0; i < nb; i++) {
                g_strlcpy(ctl->boxes[i], nm[i], sizeof(ctl->boxes[0]));
            }
            ctl->nbox = nb;
            ctl->have_boxes = true;
            break;
        }
        modes_reply(s, cmd, p, n, false);
        break;
    case MSP_RX_MAP_CMD:
    case MSP_MODE_RANGES_CMD:
    case MSP_MODE_RANGES_EXTRA_CMD:
        modes_reply(s, cmd, p, n, false);
        break;
    case MSP_ACC_CALIBRATION_CMD:
        if (ctl->cal_cmd == cmd && ctl->inav) {
            ctl->cal_cmd = MSP_CALIBRATION_DATA_CMD;    /* started: poll until it has the position */
        } else if (ctl->cal_cmd == cmd) {
            ctl->cal_cmd = 0;                           /* RotorFlight: level, then it saves */
            ctl->fn_st = FN_CHECK;
            ctl->fn_t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        break;
    case MSP_CALIBRATION_DATA_CMD:
        if (n >= 13) {
            ctl->cal_flags = p[0];
            for (int i = 0; i < 3; i++) {
                ctl->cal_zero[i] = (int16_t)lduw_le_p(p + 1 + 2 * i);
                ctl->cal_gain[i] = (int16_t)lduw_le_p(p + 7 + 2 * i);
            }
        }
        if (n >= 27) {
            for (int i = 0; i < 3; i++) {
                ctl->cal_mag_zero[i] = (int16_t)lduw_le_p(p + 13 + 2 * i);
                ctl->cal_mag_gain[i] = (int16_t)lduw_le_p(p + 21 + 2 * i);
            }
        }
        if (ctl->cal_cmd == cmd && fn_is(s, ctl->fn_cur, "calibrate_mag")) {
            ctl->cal_cmd = 0;
            mag_cal_result(s);
        }
        break;
    }
}

/* Complete frame in ctl->fr: a reply to the core's request (the current one, or one that
 * timed out and answers late) -> decoded / swallowed; anything else -> host */
static void fc_frame_done(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    const uint8_t *f = ctl->fr;
    unsigned len = ctl->fr_len;
    bool v2 = f[1] == 'X', reply = f[2] == '>' || f[2] == '!';
    /* v2: flag @3, u16 cmd @4, u16 size @6, payload @8, crc8 */
    unsigned cmd = v2 ? lduw_le_p(f + 4) : 0;

    if (!v2 || !reply || !cmd || (cmd != ctl->poll_cmd && cmd != ctl->stale_cmd)) {
        fc_forward(s, f, len);
        return;
    }
    if (cmd != ctl->poll_cmd) {                 /* the late reply to a timed-out request */
        ctl->stale_cmd = 0;
        ctl->poll_late++;
        return;
    }
    ctl->poll_cmd = 0;
    if (msp_crc8_dvb_s2(0, f + 3, len - 4) != f[len - 1]) {
        if (!ctl->poll_bad++ || !(ctl->poll_bad & 63)) {
            ctl_log(s, "core", "msp: reply to %u with a bad checksum dropped (%" PRIu64
                    " so far)", cmd, ctl->poll_bad);
        }
        return;
    }
    if (f[2] == '>') {
        fc_decode(s, cmd, f + 8, lduw_le_p(f + 6));
    } else if (ctl->ms) {
        modes_reply(s, cmd, NULL, 0, true);     /* e.g. iNav has no MSP_MODE_RANGES_EXTRA */
    } else if (cmd == MSP_BOXNAMES_CMD && !ctl->have_boxes && ctl->inav) {
        /* iNav with many boxes (GPS, compass) cannot fit their names: MSP_BOXIDS + its table */
        if (!ctl->boxnames_err) {
            ctl_log(s, "core", "msp: iNav refused MSP_BOXNAMES (its boxes' names do not fit the "
                    "reply); the box names come from MSP_BOXIDS and iNav's permanent ids");
        }
        ctl->boxnames_err = true;
    }                                           /* else '!': unsupported, dropped */
}

/* Bytes the firmware transmitted on USART1 */
static void ctl_fc_tx_body(NexusXRState *s, const uint8_t *buf, int len);

static void ctl_fc_tx(NexusXRState *s, const uint8_t *buf, int len)
{
    PROF_ENTER(pa.ctl_fc_tx);
    ctl_fc_tx_body(s, buf, len);
    PROF_LEAVE(pa.ctl_fc_tx);
}

static void ctl_fc_tx_body(NexusXRState *s, const uint8_t *buf, int len)
{
    NxCtl *ctl = s->ctl;
    if (ctl->cli) {
        fc_forward(s, buf, len);
        fc_text(s, buf, len);
        return;
    }
    for (int i = 0; i < len; i++) {
        uint8_t b = buf[i];
        if (!ctl->fr_len) {
            if (b == '$') {
                ctl->fr[ctl->fr_len++] = b;
                ctl->fr_need = 0;
            } else {
                fc_forward(s, &b, 1);
                fc_text(s, &b, 1);
            }
            continue;
        }
        ctl->fr[ctl->fr_len++] = b;
        unsigned n = ctl->fr_len;
        bool bad = (n == 2 && b != 'M' && b != 'X') ||
                   (n == 3 && b != '>' && b != '!' && b != '<');
        if (!bad && !ctl->fr_need) {
            if (ctl->fr[1] == 'M' && n == 5 && ctl->fr[3] != 255) {
                ctl->fr_need = 6 + ctl->fr[3];
            } else if (ctl->fr[1] == 'M' && n == 7 && ctl->fr[3] == 255) {
                ctl->fr_need = 8 + lduw_le_p(ctl->fr + 5);
            } else if (ctl->fr[1] == 'X' && n == 8) {
                ctl->fr_need = 9 + lduw_le_p(ctl->fr + 6);
            }
            if (ctl->fr_need > sizeof(ctl->fr)) {
                bad = true;
            }
        }
        if (bad) {
            fc_forward(s, ctl->fr, n);
            fc_text(s, ctl->fr, n);
            ctl->fr_len = 0;
        } else if (ctl->fr_need && n >= ctl->fr_need) {
            fc_frame_done(s);
            ctl->fr_len = 0;
        }
    }
}

/* One host byte through the host -> FC MSP framing. True if it is outside any frame (text). */
static bool host_frame_byte(NxCtl *ctl, uint8_t b)
{
    unsigned n = ctl->hf_len;
    if (!n) {
        if (b == '$') {
            ctl->hf[0] = b;
            ctl->hf_len = 1;
            ctl->hf_need = 0;
            return false;
        }
        return true;
    }
    if (n < sizeof(ctl->hf)) {
        ctl->hf[n] = b;
    }
    ctl->hf_len = ++n;
    if ((n == 2 && b != 'M' && b != 'X') || (n == 3 && b != '<' && b != '>' && b != '!')) {
        ctl->hf_len = 0;                        /* not a frame after all: this byte anew */
        return host_frame_byte(ctl, b);
    }
    if (!ctl->hf_need) {
        if (ctl->hf[1] == 'M' && n == 5 && ctl->hf[3] != 255) {
            ctl->hf_need = 6 + ctl->hf[3];
        } else if (ctl->hf[1] == 'M' && n == 7 && ctl->hf[3] == 255) {
            ctl->hf_need = 8 + lduw_le_p(ctl->hf + 5);
        } else if (ctl->hf[1] == 'X' && n == 8) {
            ctl->hf_need = 9 + lduw_le_p(ctl->hf + 6);
        }
    }
    if (ctl->hf_need && n >= ctl->hf_need) {
        ctl->hf_len = 0;                        /* complete */
        unsigned cmd = ctl->hf[1] == 'X' ? lduw_le_p(ctl->hf + 4) : ctl->hf[4];
        if (ctl->hf[2] == '<' && (cmd == MSP_SET_MODE_RANGE_CMD || cmd == MSP_EEPROM_WRITE_CMD)) {
            /* a configurator changed modes / saved: the modes again once it is done */
            ctl->modes_host_ns = ctl->host_ns + MODES_HOST_NS;
            ctl->modes_host_eeprom |= cmd == MSP_EEPROM_WRITE_CMD;
        }
    }
    return false;
}

/* Bytes from the host socket (configurator / script), as accepted into the USART1 FIFO;
 * len < 0: the host (re)connected, a half frame of the old connection is gone */
static void ctl_host_rx(NexusXRState *s, const uint8_t *buf, int len)
{
    NxCtl *ctl = s->ctl;
    if (!ctl) {
        return;
    }
    if (len < 0) {
        ctl->hf_len = 0;
        return;
    }
    ctl->host_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (int i = 0; i < len; i++) {
        /* '#' enters the CLI only between frames; 0x23 inside a binary frame is data */
        if (host_frame_byte(ctl, buf[i]) && buf[i] == '#' && !ctl->cli && !ctl->cli_entering) {
            ctl->cli_entering = true;           /* a host tool may be entering the CLI */
            ctl->cli_core_sent = false;
            ctl->cli_by = "client";
            ctl->cli_via = "msp";
            ctl->cli_enter_ns = ctl->host_ns;
        }
    }
}

/* ---- the CLI config script (boards with cli_config) --------------------------------------- */

static void cfg_event(NexusXRState *s, const char *state, const char *extra)
{
    g_autofree char *d = g_strdup_printf("\"state\":\"%s\",\"lines\":%d%s", state, s->ctl->cfg_run_n,
                                         extra ? extra : "");
    g_free(s->ctl->cfg_last);
    s->ctl->cfg_last = g_strdup(d);     /* a client that connects later gets it (replay) */
    ctl_event(s, "config", d);
}

static void ctl_request_reset(NexusXRState *s);
static const char *ctl_cli(NexusXRState *s, const char *text);

/* ---- modes ------------------------------------------------------------------------------ */

static void modes_json(NexusXRState *s, GString *g)
{
    NxCtl *ctl = s->ctl;
    const fbm_modes_t *m = &ctl->modes;
    g_string_append(g, "{\"type\":\"modes\",\"t\":");
    js_time(g, vnow_s());
    g_string_append(g, ",\"variant\":");
    js_str(g, ctl->variant);
    g_string_append(g, ",\"boxes\":[");
    for (int i = 0; i < m->nbox; i++) {
        g_string_append(g, i ? ",{\"id\":" : "{\"id\":");
        if (i < m->nid) {
            g_string_append_printf(g, "%u", m->id[i]);
        } else {
            g_string_append(g, "null");
        }
        g_string_append_printf(g, ",\"index\":%d,\"name\":", i);
        js_str(g, m->name[i]);
        g_string_append_c(g, '}');
    }
    g_string_append(g, "],\"ranges\":[");
    for (int i = 0; i < m->nrange; i++) {
        const fbm_range_t *r = &m->range[i];
        int rc = fbm_rc_channel(ctl->inav, r->aux);
        g_string_append_printf(g, "%s{\"slot\":%d,\"channel\":%u,\"rc_channel\":%d,\"rx_channel\":%d,"
                               "\"box\":%u,", i ? "," : "", r->slot, r->aux, rc, fbm_rx_channel(m, rc),
                               r->box);
        if (r->start_us < r->end_us) {
            g_string_append_printf(g, "\"start_us\":%d,\"end_us\":%d", r->start_us, r->end_us);
        } else {
            g_string_append(g, "\"start_us\":null,\"end_us\":null");
        }
        if (!ctl->inav) {
            g_string_append_printf(g, ",\"logic\":\"%s\"", r->and_logic ? "and" : "or");
        }
        if (r->linked) {
            g_string_append_printf(g, ",\"linked\":%u", r->linked);
        }
        g_string_append_c(g, '}');
    }
    g_string_append(g, "],\"rx_map\":[");
    for (int i = 0; i < m->nmap; i++) {
        g_string_append_printf(g, i ? ",%u" : "%u", m->map[i]);
    }
    g_string_append(g, "]}\n");
}

static void modes_done(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    ctl->ms = MS_NONE;
    ctl->modes = ctl->mnext;
    ctl->have_modes = true;
    for (int i = 0; i < ctl->modes.nbox && i < MAX_BOXES; i++) {
        g_strlcpy(ctl->boxes[i], ctl->modes.name[i], sizeof(ctl->boxes[i]));
    }
    ctl->nbox = MIN(ctl->modes.nbox, MAX_BOXES);
    g_autoptr(GString) g = g_string_new(NULL);
    modes_json(s, g);
    g_free(ctl->modes_msg);
    ctl->modes_msg = g_strdup(g->str);
    ctl_log(s, "core", "modes: %d boxes, %d ranges in use (MSP_BOXNAMES, BOXIDS, RX_MAP, "
            "MODE_RANGES%s)", ctl->modes.nbox, ctl->modes.nrange, ctl->inav ? "" : ", _EXTRA");
    for (guint i = 0; i < ctl->clients->len; i++) {
        CtlClient *c = g_ptr_array_index(ctl->clients, i);
        if (c->modes || c->modes_ask) {
            ctl_send(c, g, false);
        }
        c->modes_ask = false;
    }
}

/* a client subscribed to the modes, or one waiting for its request's result */
static bool modes_listened(NxCtl *ctl)
{
    for (guint i = 0; i < ctl->clients->len; i++) {
        CtlClient *c = g_ptr_array_index(ctl->clients, i);
        if (!c->dead && (c->modes || c->modes_ask)) {
            return true;
        }
    }
    return false;
}

/* the firmware's modes may have changed (a boot, the host): read them again if anyone listens */
static void modes_changed(NxCtl *ctl)
{
    ctl->modes_dirty = true;
    ctl->modes_want = modes_listened(ctl);
}

static void modes_start(NxCtl *ctl)
{
    memset(&ctl->mnext, 0, sizeof(ctl->mnext));
    /* RotorFlight pages its names by 32: a full first page may have a second (the boot read
     * asked for the first only) */
    if (!ctl->modes_names && (ctl->inav || ctl->nbox < 32)) {
        for (int i = 0; i < ctl->nbox && i < FBM_MAX_BOXES; i++) {
            g_strlcpy(ctl->mnext.name[i], ctl->boxes[i], FBM_NAME);
        }
        ctl->mnext.nbox = MIN(ctl->nbox, FBM_MAX_BOXES);
        ctl->ms = MS_IDS;
    } else {
        ctl->ms = MS_NAMES;
    }
    ctl->modes_want = ctl->modes_names = ctl->modes_dirty = false;
}

/* the request of the read in progress; page = 1 for RotorFlight's second page */
static uint16_t modes_cmd(NxCtl *ctl, bool *page)
{
    *page = ctl->ms == MS_NAMES_P1 || ctl->ms == MS_IDS_P1;
    switch (ctl->ms) {
    case MS_NAMES: case MS_NAMES_P1: return MSP_BOXNAMES_CMD;
    case MS_IDS: case MS_IDS_P1: return MSP_BOXIDS_CMD;
    case MS_MAP: return MSP_RX_MAP_CMD;
    case MS_RANGES: return MSP_MODE_RANGES_CMD;
    case MS_EXTRA: return MSP_MODE_RANGES_EXTRA_CMD;
    default: return 0;
    }
}

/* a reply (or an error reply, err) to the read in progress: the next step */
static void modes_reply(NexusXRState *s, unsigned cmd, const uint8_t *p, unsigned n, bool err)
{
    NxCtl *ctl = s->ctl;
    fbm_modes_t *m = &ctl->mnext;
    bool page;
    if (!ctl->ms || cmd != modes_cmd(ctl, &page)) {
        return;
    }
    ctl->ms_tries = 0;
    switch (ctl->ms) {
    case MS_NAMES:
    case MS_NAMES_P1: {
        int got = err ? 0 : fbm_names(p, n, m->name, &m->nbox, FBM_MAX_BOXES);
        ctl->ms = ctl->ms == MS_NAMES && !ctl->inav && got == 32 ? MS_NAMES_P1 : MS_IDS;
        break;
    }
    case MS_IDS:
    case MS_IDS_P1: {
        int got = err ? 0 : fbm_ids(p, n, m->id, &m->nid, FBM_MAX_BOXES);
        if (ctl->inav && !m->nbox && m->nid) {
            fbm_inav_names(m->id, m->nid, m->name, &m->nbox, FBM_MAX_BOXES);   /* names refused */
        }
        ctl->ms = ctl->ms == MS_IDS && !ctl->inav && got == 32 ? MS_IDS_P1 : MS_MAP;
        break;
    }
    case MS_MAP:
        m->nmap = err ? 0 : fbm_rx_map(p, n, m->map, FBM_MAX_MAP);
        ctl->ms = MS_RANGES;
        break;
    case MS_RANGES:
        ctl->nmranges = err ? 0 : MIN(n, sizeof(ctl->mranges));
        memcpy(ctl->mranges, p, ctl->nmranges);
        m->nrange = fbm_ranges(ctl->inav, ctl->mranges, ctl->nmranges, m->range, FBM_MAX_SLOTS);
        if (ctl->inav) {
            modes_done(s);              /* iNav has no MSP_MODE_RANGES_EXTRA */
        } else {
            ctl->ms = MS_EXTRA;
        }
        break;
    case MS_EXTRA:
        if (!err) {
            m->nrange = fbm_ranges_extra(ctl->mranges, ctl->nmranges, p, n, m->range, m->nrange,
                                         FBM_MAX_SLOTS);
        }
        modes_done(s);
        break;
    default:
        break;
    }
}

static void modes_error(CtlClient *c, const char *what)
{
    g_autoptr(GString) g = g_string_new("{\"type\":\"modes\",\"t\":");
    js_time(g, vnow_s());
    g_string_append(g, ",\"error\":");
    js_str(g, what);
    g_string_append(g, "}\n");
    ctl_send(c, g, false);
}

/* a client that just subscribed: the last read if still current (replay), else a read now */
static void modes_subscribed(CtlClient *c)
{
    NxCtl *ctl = c->ctl;
    if (ctl->modes_msg && !ctl->modes_dirty) {
        g_autoptr(GString) g = g_string_new(ctl->modes_msg);
        g_string_truncate(g, g->len - 2);                   /* "}\n" */
        g_string_append(g, ",\"replay\":true}\n");
        ctl_send(c, g, false);
    } else {
        ctl->modes_want = true;     /* read once the firmware answers MSP */
    }
}

/* ---- diff all --------------------------------------------------------------------------- */

/* the `diff` message to c (NULL: every client); error: why it could not be refreshed */
static void diff_send(NexusXRState *s, CtlClient *c, const char *error, bool replay)
{
    NxCtl *ctl = s->ctl;
    g_autoptr(GString) g = g_string_new("{\"type\":\"diff\",\"t\":");
    js_time(g, vnow_s());
    g_string_append(g, ",\"text\":");
    if (ctl->diff_text) {
        js_str(g, ctl->diff_text);
    } else {
        g_string_append(g, "null");
    }
    g_string_append_printf(g, ",\"stale\":%s,\"source\":", ctl->diff_stale || error ? "true" : "false");
    if (ctl->diff_text) {
        js_str(g, ctl->diff_source);
        g_string_append(g, ",\"at\":");
        js_time(g, ctl->diff_at);
    } else {
        g_string_append(g, "null,\"at\":null");
    }
    if (error) {
        g_string_append(g, ",\"error\":");
        js_str(g, error);
    }
    g_string_append(g, replay ? ",\"replay\":true}\n" : "}\n");
    if (c) {
        ctl_send(c, g, false);
    } else {
        ctl_broadcast(ctl, g, false);
    }
}

static void diff_mark_stale(NexusXRState *s, const char *why)
{
    NxCtl *ctl = s->ctl;
    if (ctl->diff_text && !ctl->diff_stale) {
        ctl->diff_stale = true;
        ctl_log(s, "core", "diff: stale (%s changed the firmware's config)", why);
        diff_send(s, NULL, NULL, false);
    }
}

/* {"type":"diff","against":"script"}: the script the core sends (the running assembly, each line
 * with its layer and source) against the firmware's last `diff all`, setting by setting
 * (devices/src/cli_norm.c); to c. error: why the diff could not be refreshed (the comparison is then
 * against the last text, when there is one) */
static void diff_script_send(NexusXRState *s, CtlClient *c, const char *error)
{
    NxCtl *ctl = s->ctl;
    g_autoptr(GString) g = g_string_new("{\"type\":\"diff\",\"t\":");
    js_time(g, vnow_s());
    g_string_append(g, ",\"against\":\"script\",\"source\":");
    if (ctl->diff_text) {
        js_str(g, ctl->diff_source);
        g_string_append(g, ",\"at\":");
        js_time(g, ctl->diff_at);
    } else {
        g_string_append(g, "null,\"at\":null");
    }
    g_string_append_printf(g, ",\"stale\":%s", ctl->diff_stale || error ? "true" : "false");
    if (error) {
        g_string_append(g, ",\"error\":");
        js_str(g, error);
    }
    if (ctl->diff_text) {
        int n = ctl->cfg_lines ? ctl->cfg_n : 0;
        char **own;
        fb_cli_line_t *l = cfg_script_lines(s, &own);
        char *j = fb_cli_diff_json(l, n, ctl->diff_text);
        g_string_append_printf(g, ",%s", j);
        /* the counts for the log */
        g_autofree char *obj = g_strdup_printf("{%s}", j);
        QObject *o = qobject_from_json(obj, NULL);
        QDict *od = o ? qobject_to(QDict, o) : NULL;
        int cnt[3] = { 0, 0, 0 };
        static const char *const k[3] = { "changed", "only_firmware", "only_script" };
        for (int i = 0; od && i < 3; i++) {
            QList *ql = qdict_get_qlist(od, k[i]);
            cnt[i] = ql ? (int)qlist_size(ql) : 0;
        }
        qobject_unref(o);
        ctl_log(s, "core", "diff: against the script (%d lines): %d settings changed by the firmware's "
                "config, %d only in the firmware's diff, %d only in the script (diff %s at t=%.2f%s)", n,
                cnt[0], cnt[1], cnt[2], ctl->diff_source, ctl->diff_at, ctl->diff_stale ? ", stale" : "");
        free(j);
        g_strfreev(own);
        g_free(l);
    } else {
        ctl_log(s, "core", "diff: against the script: no `diff all` yet (%s)", error ? error : "none");
    }
    g_string_append(g, "}\n");
    ctl_send(c, g, false);
}

static void cfg_snap_write(NexusXRState *s, CtlClient *c);
static void cfg_apply_go(NexusXRState *s, bool full);

/* a requested `diff all` ended (error NULL: captured): the clients waiting on it - a snapshot to
 * write, a comparison against the script */
static void diff_waiters(NexusXRState *s, const char *error)
{
    NxCtl *ctl = s->ctl;
    for (guint i = 0; i < ctl->clients->len; i++) {
        CtlClient *c = g_ptr_array_index(ctl->clients, i);
        if (c->snap_path) {
            if (error) {
                ctl_log(s, "core", "config snapshot: %s not written: `diff all` failed: %s", c->snap_path, error);
                g_autoptr(GString) g = g_string_new("{\"type\":\"config\",\"t\":");
                js_time(g, vnow_s());
                g_string_append(g, ",\"snapshot\":\"failed\",\"path\":");
                js_str(g, c->snap_path);
                g_string_append(g, ",\"error\":");
                js_str(g, error);
                g_string_append(g, "}\n");
                ctl_send(c, g, false);
            } else {
                cfg_snap_write(s, c);
            }
            g_clear_pointer(&c->snap_path, g_free);
        }
        if (c->diff_script) {
            c->diff_script = false;
            diff_script_send(s, c, error);
        }
        if (c->apply_wait && error) {
            g_autofree char *e = g_strdup_printf("config apply: `diff all` failed (%s): nothing typed", error);
            ctl_error(c, e);
        }
        c->apply_wait = false;
    }
    if (ctl->apply_wait) {
        ctl->apply_wait = false;
        if (error) {
            ctl_log(s, "core", "config apply: `diff all` failed (%s): nothing typed", error);
        } else {
            cfg_apply_go(s, false);     /* the firmware reboots from the diff's `exit` first */
        }
    }
}

static void diff_begin(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    ctl->diff_capture = ctl->diff_echo = true;
    ctl->diff_saw_save = false;
    g_string_truncate(ctl->diff_buf, 0);
    ctl->diff_t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void diff_finish(NexusXRState *s, bool ok, const char *source)
{
    NxCtl *ctl = s->ctl;
    ctl->diff_capture = false;
    if (!ok) {
        ctl_log(s, "core", "diff: `diff all` did not end within %.0f s (%zu bytes); the previous "
                "dump is kept, stale", DIFF_REPLY_NS / 1e9, ctl->diff_buf->len);
        ctl->diff_stale = true;
        diff_send(s, NULL, "timeout", false);
        return;
    }
    g_free(ctl->diff_text);
    ctl->diff_text = g_strdup(ctl->diff_buf->str);
    ctl->diff_stale = false;
    ctl->diff_source = source;
    ctl->diff_at = vnow_s();
    unsigned lines = 0;
    for (const char *q = ctl->diff_text; *q; q++) {
        lines += *q == '\n';
    }
    ctl_log(s, "core", "diff: `diff all` captured (%u lines, %zu bytes, %s)", lines,
            strlen(ctl->diff_text), source);
    diff_send(s, NULL, NULL, false);
}

/* {"type":"diff"}: NULL = started (the result goes to every client), else why not */
static const char *diff_request(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    if (ctl->diff_st != DIFF_IDLE) {
        return NULL;                    /* one is running: its result comes */
    }
    if (s->power_off) {
        return "power_off";
    }
    if (ctl->armed) {
        return "armed";
    }
    if (ctl->have_status && (ctl->arm_flags & (ctl->inav ? INAV_FLAG_CAL : ARM_FLAG_CALIB))) {
        return "calibrating";           /* the firmware's own (boot) calibration: not mid-way */
    }
    if (ctl->cli || ctl->cli_entering) {
        return "cli";
    }
    if ((ctl->cfg >= CFG_PENDING && ctl->cfg <= CFG_SAVE) || ctl->cfg == CFG_FN || ctl->fn_cur >= 0) {
        return "busy";
    }
    if (!ctl->have_board) {
        return "not_ready";
    }
    ctl->diff_st = DIFF_ENTER;
    ctl->diff_t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ctl->cli_entering = ctl->cli_core_sent = ctl->cli_need_entry = true;
    ctl->cli_by = "core";
    ctl->cli_via = "control";
    ctl->cli_enter_ns = ctl->diff_t0;
    ctl_log(s, "core", "diff: entering the CLI for `diff all`, then `exit` (the firmware reboots)");
    return NULL;
}

static void diff_poll(NexusXRState *s, int64_t now)
{
    NxCtl *ctl = s->ctl;
    switch (ctl->diff_st) {
    case DIFF_ENTER:
        if (!ctl->cli && !ctl->cli_entering) {
            ctl->diff_st = DIFF_IDLE;
            ctl_log(s, "core", "diff: the firmware did not enter the CLI (armed?)");
            diff_send(s, NULL, "no_cli", false);
            diff_waiters(s, "no_cli");
        }
        break;
    case DIFF_RUN:
        if (now - ctl->diff_t0 > DIFF_REPLY_NS) {
            diff_finish(s, false, "request");
            diff_waiters(s, "timeout");
            ctl->diff_st = DIFF_EXIT;
            ctl->diff_t0 = now;
            fc_cli_line(s, "exit");
        }
        break;
    case DIFF_EXIT:
        if (now - ctl->diff_t0 > FN_SAVE_NS) {
            ctl->diff_st = DIFF_IDLE;
            ctl_log(s, "core", "diff: no reboot within %.0f s after `exit`", FN_SAVE_NS / 1e9);
        }
        break;
    default:
        break;
    }
}

/* ---- environment functions (s->fn) ------------------------------------------------------ */

static int fn_find(NexusXRState *s, const char *name)
{
    for (int i = 0; i < s->nfn; i++) {
        if (!strcmp(s->fn[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static bool fn_is(NexusXRState *s, int i, const char *name)
{
    return i >= 0 && !strcmp(s->fn[i].name, name);
}

/* the poses of iNav's 6-position ACC calibration (else the firmware's level calibration) */
static bool fn_poses(NexusXRState *s, int i)
{
    return fn_is(s, i, "calibrate_acc") && s->fn[i].scenario && s->ctl->inav;
}

/* the `function` progress message to every client; extra: ",\"key\":value..." or NULL */
static void fn_msg(NexusXRState *s, const char *name, const char *phase, int step, int steps,
                   const char *detail, const char *extra)
{
    NxCtl *ctl = s->ctl;
    g_autoptr(GString) g = g_string_new("{\"type\":\"function\",\"t\":");
    js_time(g, vnow_s());
    g_string_append(g, ",\"name\":");
    js_str(g, name);
    g_string_append_printf(g, ",\"phase\":\"%s\",\"step\":%d,\"steps\":%d,\"detail\":", phase, step,
                           steps);
    js_str(g, detail);
    if (extra) {
        g_string_append(g, extra);
    }
    fprintf(stderr, "fbctl t=%.3f: function %s %s %d/%d: %s\n", vnow_s(), name, phase, step, steps,
            detail);
    g_free(ctl->fn_last);
    ctl->fn_last = g_strdup(g->str);    /* a client that connects later gets it (replay) */
    g_string_append(g, "}\n");
    ctl_broadcast(ctl, g, false);
}

/* part of the first start: auto_first_start, and runnable here (the poses need the plane FDM) */
static bool fn_auto(NexusXRState *s, int i)
{
    const FbFunction *f = &s->fn[i];
    return f->auto_first && strcmp(f->name, "launch") && (!f->scenario || s->env) &&
           (strcmp(f->name, "calibrate_mag") || (s->env && s->mag_present));
}

static int fs_next(NexusXRState *s, int from)
{
    for (int i = from; i < s->nfn; i++) {
        if (fn_auto(s, i) && (s->ctl->fs_mask >> i & 1)) {
            return i;
        }
    }
    return -1;
}

static void fn_begin(NexusXRState *s, int i, bool first)
{
    NxCtl *ctl = s->ctl;
    const FbFunction *f = &s->fn[i];
    ctl->fn_cur = i;
    ctl->fn_first = first;
    ctl->fs_i = first ? i : ctl->fs_i;
    ctl->fn_st = FN_WAIT;
    ctl->fn_step = 0;
    ctl->fn_steps = fn_is(s, i, "calibrate_gyro") ? 2 : fn_is(s, i, "calibrate_mag") ? 3
                  : f->scenario ? 6 : 1;
    ctl->fn_t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ctl->fn_seen = ctl->fn_saved = false;
    ctl->fn_hold_s = f->hold_s;
    ctl->cal_cmd = 0;
    ctl->cal_pose = -1;
    g_autofree char *d = g_strdup_printf("%s%s", f->label, first ? " (first start)" : "");
    fn_msg(s, f->name, "start", 0, ctl->fn_steps, d, first ? ",\"first_start\":true" : NULL);
}

/* the running function ended; the first start goes on with the next one or its final save */
static void fn_end(NexusXRState *s, bool ok, const char *detail, const char *extra)
{
    NxCtl *ctl = s->ctl;
    int i = ctl->fn_cur;
    bool first = ctl->fn_first;
    ctl->fn_cur = -1;
    ctl->cal_cmd = 0;
    ctl->cal_pose = -1;
    fn_msg(s, s->fn[i].name, ok ? "done" : "failed", ok ? ctl->fn_steps : ctl->fn_step,
           ctl->fn_steps, detail, extra);
    if (ok && g_str_has_prefix(s->fn[i].name, "calibrate_") && ctl->bs_fn) {
        /* the bench state's record of it (a first start saves next; otherwise the firmware saved) */
        g_hash_table_insert(ctl->bs_fn, g_strdup(s->fn[i].name), fn_inputs(s, i));
        if (!first) {
            bs_keep(s);
        }
    }
    if (s->env && (s->fn[i].scenario || fn_is(s, i, "calibrate_gyro") ||
                     fn_is(s, i, "calibrate_mag"))) {
        env_default_run(s);           /* the model is let go: the aircraft's default env */
    }
    if (!first) {
        return;
    }
    if (!ok) {
        ctl_log(s, "core", "config: %s failed: %s; the script's configuration stays saved without "
                "it", s->fn[i].name, detail);
        g_autoptr(GString) x = g_string_new(",\"error\":");
        js_str(x, detail);
        g_string_append(x, ",\"function\":");
        js_str(x, s->fn[i].name);
        cfg_event(s, "failed", x->str);
        ctl->cfg = CFG_FAILED;
        return;
    }
    int nx = fs_next(s, i + 1);
    if (nx >= 0) {
        fn_begin(s, nx, true);
        return;
    }
    /* the save goes like the script's: CLI, `diff all`, `save`, the reboot keeps the sector */
    ctl->fs_done = true;
    ctl->cfg = CFG_PENDING;
    ctl->cfg_i = ctl->cfg_run_n;
    ctl->cfg_tries = 0;
}

/* ---- the CLI config script -------------------------------------------------------------- */

static void cfg_fail(NexusXRState *s, const char *why)
{
    NxCtl *ctl = s->ctl;
    const char *line = ctl->cfg_i > 0 && ctl->cfg_i <= ctl->cfg_run_n ? ctl->cfg_run[ctl->cfg_i - 1]
                     : "";
    ctl_log(s, "core", "config: error: line %d '%s': %s; the configuration is not saved "
            "(exit: the firmware reboots with its defaults)", ctl->cfg_i, line, why);
    g_autoptr(GString) x = g_string_new(",\"line\":");
    g_string_append_printf(x, "%d,\"text\":", ctl->cfg_i);
    js_str(x, line);
    g_string_append(x, ",\"error\":");
    js_str(x, why);
    if (ctl->cfg_apply) {
        g_string_append(x, ",\"apply\":true");
        ctl->cfg_apply = false;
    }
    cfg_event(s, "failed", x->str);
    ctl->cfg = CFG_FAILED;
    ctl->diff_capture = false;
    if (ctl->cli) {
        fc_cli_line(s, "exit");
    }
}

/* the diff captured before the config flow's save: whose */
static const char *cfg_diff_source(NxCtl *ctl)
{
    return ctl->cfg_apply ? "apply" : ctl->bs_from_bench ? "bench" : "first_start";
}

static void cfg_save(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    ctl->cfg = CFG_SAVE;
    ctl->cfg_saved = false;
    ctl->cfg_t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (ctl->fs_done) {
        ctl_log(s, "core", "config: save (the first-start functions; the firmware writes its config "
                "and reboots)");
    } else {
        ctl_log(s, "core", "config: save (%d lines applied; the firmware writes its config and "
                "reboots)", ctl->cfg_run_n);
    }
    fc_cli_line(s, "save");
}

/* a CLI prompt: the previous line is done; send the next, or `diff all` before the final save,
 * or save. The core's own `diff` session (diff_st) goes through here too. */
static void cfg_prompt(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (ctl->diff_st == DIFF_ENTER) {
        ctl->diff_st = DIFF_RUN;
        diff_begin(s);
        fc_cli_line(s, "diff all");
        return;
    }
    if (ctl->diff_st == DIFF_RUN) {
        if (ctl->diff_saw_save) {       /* comment lines ("# ...") look like prompts too */
            diff_finish(s, true, "request");
            diff_waiters(s, NULL);
            ctl->diff_st = DIFF_EXIT;
            ctl->diff_t0 = now;
            fc_cli_line(s, "exit");
        }
        return;
    }
    if (ctl->cfg == CFG_DIFF) {
        if (ctl->diff_saw_save) {
            diff_finish(s, true, cfg_diff_source(ctl));
            ctl->diff_cfg_fresh = true;
            cfg_save(s);
        }
        return;
    }
    if (ctl->cfg == CFG_ENTER) {
        ctl->cfg = CFG_SEND;
        ctl->cfg_tries = 0;
    } else if (ctl->cfg != CFG_SEND) {
        return;
    } else if (ctl->cfg_err) {
        cfg_fail(s, ctl->cfg_errtext->str);
        return;
    }
    ctl->cfg_t0 = now;
    ctl->cfg_err = false;
    if (ctl->cfg_i < ctl->cfg_run_n) {
        const char *line = ctl->cfg_run[ctl->cfg_i++];
        ctl_log(s, "core", "config: %s", line);
        fc_cli_line(s, line);
    } else if (ctl->fs_done || fs_next(s, 0) < 0) {
        /* the final save: the firmware's `diff all` of what it saves, kept with the snapshot */
        ctl->cfg = CFG_DIFF;
        diff_begin(s);
        ctl_log(s, "core", "config: diff all (the configuration being saved; kept with the snapshot)");
        fc_cli_line(s, "diff all");
    } else {
        cfg_save(s);
    }
}

/* the snapshot's key: firmware + script + the first-start functions (their scenarios and the
 * sensor error the calibration measures) */
static char *cfg_key(NexusXRState *s)
{
    g_autoptr(GChecksum) c = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(c, (const guchar *)s->config_cli, strlen(s->config_cli));
    bool any = false;
    for (int i = 0; i < s->nfn; i++) {
        if (!fn_auto(s, i)) {
            continue;
        }
        g_autofree char *sep = g_strdup_printf("\n# function %s\n", s->fn[i].name);
        g_checksum_update(c, (const guchar *)sep, strlen(sep));
        if (s->fn[i].scenario_text) {
            g_checksum_update(c, (const guchar *)s->fn[i].scenario_text,
                              strlen(s->fn[i].scenario_text));
        }
        any = true;
    }
    if (any) {
        /* ... for this sensor: its static error is what a calibration measures */
        g_autofree char *err = g_strdup_printf("\n# imu-accel-error %g:%g\n", s->motion.accel_offset_g,
                                               s->motion.accel_gain);
        g_checksum_update(c, (const guchar *)err, strlen(err));
        int mag = fn_find(s, "calibrate_mag");
        if (mag >= 0 && fn_auto(s, mag)) {
            /* ... and the compass's: its field and mount are what the compass calibration sees */
            g_autofree char *m = g_strdup_printf("# mag %d %.6g %.6g %.6g\n", s->mag_mount, s->mag_ned[0],
                                                 s->mag_ned[1], s->mag_ned[2]);
            g_checksum_update(c, (const guchar *)m, strlen(m));
        }
    }
    return g_strdup_printf("fw %.12s script %.12s", s->fw_hash ? s->fw_hash : "-",
                           g_checksum_get_string(c));
}

/* the snapshot's file in the cfg-cache directory, NULL when off */
static char *cfg_cache_file(NexusXRState *s, const char *key)
{
    if (s->cfg_cache && (!*s->cfg_cache || !strcmp(s->cfg_cache, "off"))) {
        return NULL;
    }
#ifdef _WIN32
    /* glib's user cache dir on Windows is the Temporary Internet Files folder: LocalAppData */
    const char *base = g_getenv("XDG_CACHE_HOME") ? g_get_user_cache_dir() : g_get_user_data_dir();
#else
    const char *base = g_get_user_cache_dir();
#endif
    g_autofree char *dir = s->cfg_cache ? g_strdup(s->cfg_cache)
                         : g_build_filename(base, "flightbench", "config", NULL);
    g_autofree char *name = g_strdup_printf("%s-%s.bin", s->board->machine, key);
    g_strdelimit(name, " ", '-');
    return g_build_filename(dir, name, NULL);
}

/* ... and its `diff all` next to it: <name>.diff.txt */
static char *cfg_diff_file(const char *bin)
{
    return g_strdup_printf("%.*s.diff.txt", (int)(strlen(bin) - 4), bin);
}

/* ---- the bench state (-M bench-state=<file>) ------------------------------------------------ */

#define BS_CHECK_NS     (30 * NANOSECONDS_PER_SECOND)  /* restored: the firmware's status by then */

static bool bs_on(NexusXRState *s)
{
    return s->bench_state && *s->bench_state && s->board->cli_config && !s->ctl->bs_off;
}

/* <file>.json next to the sector: X.state.bin -> X.state.json, else <file>.json */
static char *bs_json_file(const char *bin)
{
    size_t cap = strlen(bin) + 6;
    char *j = g_malloc(cap);
    fbbs_json_name(bin, j, cap);        /* fits: at most bin + ".json" */
    return j;
}

/* what a function's result depends on (its scenario, the sensor's error it measures): 12 hex */
static char *fn_inputs(NexusXRState *s, int i)
{
    const FbFunction *f = &s->fn[i];
    g_autoptr(GChecksum) c = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(c, (const guchar *)f->name, strlen(f->name));
    if (f->scenario_text) {
        g_checksum_update(c, (const guchar *)f->scenario_text, strlen(f->scenario_text));
    }
    g_autofree char *x = NULL;
    if (fn_is(s, i, "calibrate_acc")) {
        x = g_strdup_printf("\n# imu-accel-error %g:%g\n", s->motion.accel_offset_g, s->motion.accel_gain);
    } else if (fn_is(s, i, "calibrate_mag")) {
        x = g_strdup_printf("\n# mag %d %.6g %.6g %.6g\n", s->mag_mount, s->mag_ned[0], s->mag_ned[1],
                            s->mag_ned[2]);
    }
    if (x) {
        g_checksum_update(c, (const guchar *)x, strlen(x));
    }
    return g_strndup(g_checksum_get_string(c), 12);
}

/* the metadata of the sector sec */
static GString *bs_json(NexusXRState *s, const uint8_t *sec)
{
    NxCtl *ctl = s->ctl;
    QDict *d = qdict_new();
    qdict_put_int(d, "format", FBBS_FORMAT);
    qdict_put_str(d, "machine", s->board->machine);
    qdict_put_str(d, "firmware", s->fw_hash ? s->fw_hash : "");
    g_autofree char *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, sec, s->board->cfg_len);
    qdict_put_str(d, "sector_sha256", sha);
    qdict_put_str(d, "script", ctl->bs_script);
    QDict *fn = qdict_new();
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, ctl->bs_fn);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        qdict_put_str(fn, k, v);
    }
    qdict_put(d, "functions", fn);
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *ts = g_date_time_format_iso8601(now);
    qdict_put_str(d, "saved", ts);
    qdict_put(d, "t", qnum_from_double(vnow_s()));
    qdict_put_int(d, "saves", ctl->bs_saves);
    if (ctl->diff_text) {
        qdict_put_str(d, "diff", ctl->diff_text);
        qdict_put_bool(d, "diff_stale", ctl->diff_stale);
    } else {
        qdict_put_null(d, "diff");
    }
    GString *g = qobject_to_json_pretty(QOBJECT(d), true);
    qobject_unref(d);
    g_string_append_c(g, '\n');
    return g;
}

/* the sector, then its metadata; each through a temporary file renamed over the old one */
static void bs_write(NexusXRState *s, const uint8_t *sec)
{
    g_autoptr(GError) ge = NULL;
    g_autofree char *dir = g_path_get_dirname(s->bench_state);
    g_autofree char *jf = bs_json_file(s->bench_state);
    g_autoptr(GString) j = bs_json(s, sec);
    if (g_mkdir_with_parents(dir, 0755)) {
        ctl_log(s, "core", "bench state: not written: %s: %s", dir, g_strerror(errno));
        return;
    }
    if (!g_file_set_contents_full(s->bench_state, (const char *)sec, s->board->cfg_len,
                                  G_FILE_SET_CONTENTS_CONSISTENT, 0644, &ge) ||
        !g_file_set_contents_full(jf, j->str, j->len, G_FILE_SET_CONTENTS_CONSISTENT, 0644, &ge)) {
        ctl_log(s, "core", "bench state: not written: %s", ge->message);
        return;
    }
    ctl_log(s, "core", "bench state: the config sector written to %s (+ .json)", s->bench_state);
}

static void bs_bh(void *opaque)
{
    NexusXRState *s = opaque;
    NxCtl *ctl = s->ctl;
    g_autofree uint8_t *sec = g_steal_pointer(&ctl->bs_pending);
    if (sec) {
        bs_write(s, sec);
    }
}

/* keep the config sector as it is in flash now (written from the main loop) */
static void bs_keep(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    if (!bs_on(s)) {
        return;
    }
    if (!ctl->bs_script) {
        if (!ctl->bs_warned) {
            ctl->bs_warned = true;
            ctl_log(s, "core", "bench state: the sector is not kept yet (the first start has not "
                    "applied the script)");
        }
        return;
    }
    const uint8_t *sec = (uint8_t *)memory_region_get_ram_ptr(&s->flash) + s->board->cfg_off;
    g_free(ctl->bs_pending);
    ctl->bs_pending = g_memdup2(sec, s->board->cfg_len);
    qemu_bh_schedule(ctl->bs_bh);
}

/* the flash locked after writes into the config sector: every save of the firmware (CLI `save`,
 * MSP_EEPROM_WRITE, the calibrations' own saves) */
static void ctl_cfg_written(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    if (!ctl || (!ctl->cfg_lines && !bs_on(s))) {
        return;
    }
    if (ctl->cfg_lines && ctl->cfg == CFG_SAVE) {
        g_free(ctl->bs_script);         /* the core's save: the sector has the script's lines */
        ctl->bs_script = g_strdup(s->config_cli);
    } else {
        /* any other save (MSP_EEPROM_WRITE, a CLI save, a calibration's) may change what the last
         * diff says: stale before the bench state keeps the diff with the sector (the MSP decoder
         * marks it too, but after the flash write, so a restart would restore it as fresh) */
        diff_mark_stale(s, "a save not by the core's script");
    }
    ctl->bs_saves++;
    if (bs_on(s)) {
        ctl_log(s, "core", "config: the firmware saved its config (save %u of this run)",
                ctl->bs_saves);
    }
    bs_keep(s);
}

/* the bench state into sec when it is there and for this firmware; false (logged) otherwise */
static bool bs_load(NexusXRState *s, uint8_t *sec)
{
    NxCtl *ctl = s->ctl;
    uint32_t len = s->board->cfg_len;
    g_autofree char *jf = bs_json_file(s->bench_state);
    g_autofree char *data = NULL, *json = NULL;
    g_autoptr(GError) ge = NULL;
    gsize n = 0;
    if (!g_file_get_contents(s->bench_state, &data, &n, &ge)) {
        ctl_log(s, "core", "bench state: none (%s)", ge->message);
        return false;
    }
    if (n != len) {
        ctl_log(s, "core", "bench state: %s has %" G_GSIZE_FORMAT " bytes, not %u: ignored",
                s->bench_state, n, len);
        return false;
    }
    if (!g_file_get_contents(jf, &json, NULL, &ge)) {
        ctl_log(s, "core", "bench state: no metadata (%s): ignored", ge->message);
        return false;
    }
    Error *err = NULL;
    QObject *o = qobject_from_json(json, &err);
    QDict *d = o ? qobject_to(QDict, o) : NULL;
    FbbsMeta m = { .parsed = d != NULL };
    if (d) {
        m.format = qdict_get_try_int(d, "format", 0);
        m.machine = qdict_get_try_str(d, "machine");
        m.firmware = qdict_get_try_str(d, "firmware");
        m.script = qdict_get_try_str(d, "script");
        m.sector_sha256 = qdict_get_try_str(d, "sector_sha256");
    }
    g_autofree char *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *)data, len);
    const char *why;
    switch (fbbs_verdict(&m, n, len, sha, s->board->machine, s->fw_hash, &why)) {
    case FBBS_FIRMWARE:
        ctl_log(s, "core", "bench state: %s for firmware %.12s, this is %.12s: ignored (the snapshot or "
                "the first start instead)", jf, m.firmware ?: "-", s->fw_hash ?: "-");
        qobject_unref(o);
        return false;
    case FBBS_UNREADABLE:
        ctl_log(s, "core", "bench state: %s unreadable (%s): ignored", jf,
                !d && err ? error_get_pretty(err) : why);
        error_free(err);
        qobject_unref(o);
        return false;
    case FBBS_NEWER:
        ctl->bs_off = true;
        ctl_log(s, "core", "bench state: %s is format %" PRId64 ", a newer core's: not restored, and "
                "not overwritten in this run (the bench state is off)", jf, m.format);
        qobject_unref(o);
        return false;
    case FBBS_SIZE:                     /* checked above; the verdict agrees */
        qobject_unref(o);
        return false;
    case FBBS_RESTORE_OTHER_SHA:
        ctl_log(s, "core", "bench state: the metadata describes another sector (a write "
                "interrupted between the two files?): the sector is used, its metadata may be older");
        break;
    case FBBS_RESTORE:
        break;
    }
    memcpy(sec, data, len);
    g_free(ctl->bs_script);
    ctl->bs_script = g_strdup(qdict_get_try_str(d, "script"));
    g_hash_table_remove_all(ctl->bs_fn);
    QDict *fn = qdict_get_qdict(d, "functions");
    for (const QDictEntry *e = fn ? qdict_first(fn) : NULL; e; e = qdict_next(fn, e)) {
        QString *v = qobject_to(QString, qdict_entry_value(e));
        if (v) {
            g_hash_table_insert(ctl->bs_fn, g_strdup(qdict_entry_key(e)),
                                g_strdup(qstring_get_str(v)));
        }
    }
    const char *diff = qdict_get_try_str(d, "diff");
    g_free(ctl->diff_text);
    ctl->diff_text = diff ? g_strdup(diff) : NULL;
    ctl->diff_stale = qdict_get_try_bool(d, "diff_stale", false);
    ctl->diff_source = "bench";
    ctl->diff_at = vnow_s();
    ctl_log(s, "core", "bench state: %s (saved %s, %" PRId64 " saves in that run)", s->bench_state,
            qdict_get_try_str(d, "saved") ?: "?", qdict_get_try_int(d, "saves", 0));
    qobject_unref(o);
    return true;
}

static void cfg_run_set(NxCtl *ctl, char **run, int n)
{
    if (ctl->cfg_run != ctl->cfg_lines) {
        g_free(ctl->cfg_run);           /* a delta: the array only, its lines are cfg_lines' */
    }
    ctl->cfg_run = run;
    ctl->cfg_run_n = n;
}

/* a delta's run: the script's lines the sector's script (bs_script) does not have (added or
 * changed) and those with reassert[i] (a setting the firmware's config changed: the script's value
 * again), each with the context lines in force at it (control_profile N ...: fb_cli_with_context);
 * the lines the sector had that the script no longer has go to bs_extra's "removed" (not reverted).
 * what: the log lines' prefix. Returns the run's length. */
static int cfg_delta_run(NexusXRState *s, const unsigned char *reassert, const char *what)
{
    NxCtl *ctl = s->ctl;
    g_autoptr(GPtrArray) old = cfg_split(ctl->bs_script);
    int n = ctl->cfg_n;
    g_autofree int *add = g_new(int, n + 1);
    g_autofree int *rm = g_new(int, old->len + 1);
    g_autofree unsigned char *want = g_new0(unsigned char, n + 1);
    g_autofree int *out = g_new(int, n * 6 + 6);
    int nrm;
    int na = fbbs_delta((const char *const *)ctl->cfg_lines, n, (const char *const *)old->pdata, old->len,
                        add, rm, &nrm);
    for (int i = 0; i < na; i++) {
        want[add[i]] = 1;
    }
    for (int i = 0; reassert && i < n; i++) {
        if (reassert[i] && !want[i]) {
            want[i] = 2;
        }
    }
    int m = fb_cli_with_context((const char *const *)ctl->cfg_lines, n, want, out);
    char **run = g_new(char *, m + 1);
    for (int k = 0; k < m; k++) {
        run[k] = ctl->cfg_lines[out[k]];
        ctl_log(s, "core", "%s: + %s%s", what, run[k],
                !want[out[k]] ? " (the context in force at the next line)"
                : want[out[k]] == 2 ? " (the firmware's config has another value: the script's again)" : "");
    }
    run[m] = NULL;
    cfg_run_set(ctl, run, m);
    for (int i = 0; i < nrm; i++) {
        const char *l = g_ptr_array_index(old, rm[i]);
        g_string_append(ctl->bs_extra, i ? "," : ",\"removed\":[");
        js_str(ctl->bs_extra, l);
        ctl_log(s, "core", "%s: '%s' is no longer in the script; left as the firmware has it (config "
                "reset for a clean start)", what, l);
    }
    if (nrm) {
        g_string_append_c(ctl->bs_extra, ']');
    }
    return m;
}

/* the script's lines with their layer and source (cfg_prov_get): *own holds the strings */
static fb_cli_line_t *cfg_script_lines(NexusXRState *s, char ***own)
{
    NxCtl *ctl = s->ctl;
    int n = ctl->cfg_lines ? ctl->cfg_n : 0;
    fb_cli_line_t *l = g_new0(fb_cli_line_t, n + 1);
    *own = g_new0(char *, 2 * n + 1);
    for (int i = 0; i < n; i++) {
        cfg_prov_get(s, i, &(*own)[2 * i], &(*own)[2 * i + 1]);
        l[i] = (fb_cli_line_t) { ctl->cfg_lines[i], (*own)[2 * i], (*own)[2 * i + 1] };
    }
    return l;
}

/* the script's lines the bench state's script does not have (added or changed: sent), and the
 * ones it had that the script no longer has (removed: reported in bs_extra, not reverted) */
static void bs_delta(NexusXRState *s)
{
    g_string_assign(s->ctl->bs_extra, ",\"source\":\"bench\"");
    cfg_delta_run(s, NULL, "config: delta");
}

/* restored from the bench state (and its delta saved): the automatic functions whose results
 * the sector does not have (the firmware's calibration flags) or whose inputs changed run now */
static void bs_check(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    uint32_t mask = 0;
    g_autoptr(GString) why = g_string_new(NULL), ok = g_string_new(NULL);
    for (int i = 0; i < s->nfn; i++) {
        if (!fn_auto(s, i)) {
            continue;
        }
        g_autofree char *key = fn_inputs(s, i);
        const char *had = g_hash_table_lookup(ctl->bs_fn, s->fn[i].name);
        bool missing = fn_is(s, i, "calibrate_acc")
                     ? (ctl->arm_flags & (ctl->inav ? INAV_FLAG_ACC : ARM_FLAG_NO_ACC_CAL)) != 0
                     : fn_is(s, i, "calibrate_mag") ? (ctl->arm_flags & INAV_FLAG_COMPASS) != 0
                     : false;
        const char *r = missing ? "the firmware reports it not calibrated"
                      : !had ? "not run on this bench" : strcmp(had, key) ? "its inputs changed" : NULL;
        GString *g = r ? why : ok;
        g_string_append_printf(g, "%s%s%s%s%s", g->len ? ", " : "", s->fn[i].name, r ? " (" : "",
                               r ?: "", r ? ")" : "");
        mask |= r ? 1u << i : 0;
    }
    if (!mask) {
        ctl->cfg = CFG_DONE;
        ctl_log(s, "core", "config: bench state: the automatic functions' results are in the sector%s%s",
                ok->len ? ": " : " (none)", ok->str);
        return;
    }
    ctl->fs_mask = mask;
    ctl->fs_done = false;
    int nx = fs_next(s, 0);
    ctl->cfg = CFG_FN;
    ctl_log(s, "core", "config: bench state: %s run now%s%s", why->str, ok->len ? "; kept: " : "",
            ok->str);
    g_autofree char *x = g_strdup_printf(",\"function\":\"%s\",\"source\":\"bench\"", s->fn[nx].name);
    cfg_event(s, "calibrating", x);
    fn_begin(s, nx, true);
}

/* Reset handler, a CLI board without a script (a bare firmware on the bench): no first start;
 * every save of the firmware is the bench's (bs_script ""), a blank sector gets it back */
static void bs_at_reset_bare(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    uint8_t *sec = (uint8_t *)memory_region_get_ram_ptr(&s->flash) + s->board->cfg_off;
    uint32_t len = s->board->cfg_len;
    bool blank = true;
    for (uint32_t i = 0; i < len && blank; i++) {
        blank = sec[i] == 0xFF;
    }
    if (!blank || !bs_on(s)) {
        return;
    }
    bool skip = ctl->bs_skip || ctl->reset_first;
    ctl->bs_skip = false;
    ctl->reset_first = NULL;
    ctl->bs_from_bench = false;
    if (skip || !bs_load(s, sec)) {
        g_free(ctl->bs_script);
        ctl->bs_script = g_strdup("");  /* the firmware's defaults on: all its saves are kept */
        return;
    }
    memory_region_set_dirty(&s->flash, s->board->cfg_off, len);
    memory_region_flush_rom_device(&s->flash, s->board->cfg_off, len);
    ctl->bs_from_bench = true;
    ctl_log(s, "core", "config: blank config sector: the bench state written back (no script)");
    cfg_event(s, "restored", ",\"source\":\"bench\"");
    if (ctl->diff_text) {
        diff_send(s, NULL, NULL, false);
    }
}

/* Reset handler (CPU stopped): after a save the sector becomes the snapshot; a blank sector gets
 * the snapshot back or the script again */
static void cfg_at_reset(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    if (!ctl->cfg_lines) {
        bs_at_reset_bare(s);
        return;
    }
    uint8_t *sec = (uint8_t *)memory_region_get_ram_ptr(&s->flash) + s->board->cfg_off;
    uint32_t len = s->board->cfg_len;
    bool blank = true;
    for (uint32_t i = 0; i < len && blank; i++) {
        blank = sec[i] == 0xFF;
    }
    g_autofree char *key = cfg_key(s);
    if (ctl->cfg == CFG_SAVE) {
        /* not `blank` alone: iNav writes its defaults at the first boot on a blank sector
         * (config.c ensureEEPROMContainsValidData), so a reset before the save would keep those */
        if (blank || !ctl->cfg_saved) {
            ctl->cli = false;           /* the MCU is being reset: no `exit` to send */
            cfg_fail(s, blank ? "save rebooted without writing the config sector"
                              : "reset before the firmware confirmed the save");
            return;
        }
        if (ctl->cfg_apply) {
            /* config apply: the bench's script is the sector's now (ctl_cfg_written); the
             * first-start snapshot stays the baseline of the script it was made with */
            ctl->cfg_apply = false;
            ctl->cfg = CFG_DONE;
            ctl->diff_cfg_fresh = false;
            g_autofree char *x = g_strdup_printf("%s%s", ctl->cfg_delta ? ",\"delta\":true" : "",
                                                 ctl->bs_extra->str);
            ctl_log(s, "core", "config: apply: saved (%d lines typed%s); the firmware's config has the "
                    "bench's script as assembled now", ctl->cfg_run_n, ctl->cfg_delta ? ", the delta" : "");
            cfg_event(s, "applied", x);
            return;
        }
        if (ctl->bs_from_bench) {
            /* on the bench state: the snapshot (the first start's baseline) is left as it is */
            g_autoptr(GString) x = g_string_new(NULL);
            if (!ctl->fs_done) {
                g_string_printf(x, ",\"delta\":true%s", ctl->bs_extra->str);
                ctl_log(s, "core", "config: saved (the delta, %d lines); the calibrations are checked "
                        "once the firmware answers", ctl->cfg_run_n);
                cfg_event(s, "applied", x->str);
                ctl->cfg = CFG_CHECK;
                ctl->bs_check_t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                return;
            }
            g_string_append(x, ",\"functions\":[");
            for (int i = 0, k = 0; i < s->nfn; i++) {
                if (fn_auto(s, i) && (ctl->fs_mask >> i & 1)) {
                    g_string_append_printf(x, "%s\"%s\"", k++ ? "," : "", s->fn[i].name);
                }
            }
            g_string_append(x, "],\"source\":\"bench\"");
            ctl->cfg = CFG_DONE;
            ctl_log(s, "core", "config: saved (the functions' results, on the bench state)");
            cfg_event(s, "applied", x->str);
            return;
        }
        int nx = ctl->fs_done ? -1 : fs_next(s, 0);
        if (nx >= 0) {
            ctl->cfg = CFG_FN;
            ctl_log(s, "core", "config: saved; the first-start functions run once the firmware "
                    "answers (%s first), then save again", s->fn[nx].name);
            g_autofree char *x = g_strdup_printf(",\"function\":\"%s\"", s->fn[nx].name);
            cfg_event(s, "calibrating", x);
            fn_begin(s, nx, true);
            return;
        }
        g_free(s->cfg_snap);
        g_free(s->cfg_snap_key);
        g_free(s->cfg_snap_diff);
        s->cfg_snap = g_memdup2(sec, len);
        s->cfg_snap_key = g_strdup(key);
        s->cfg_snap_diff = ctl->diff_cfg_fresh && ctl->diff_text ? g_strdup(ctl->diff_text) : NULL;
        ctl->diff_cfg_fresh = false;
        ctl->cfg = CFG_DONE;
        g_autofree char *file = cfg_cache_file(s, key);
        if (file) {
            g_autoptr(GError) ge = NULL;
            g_autofree char *dir = g_path_get_dirname(file);
            g_autofree char *dfile = cfg_diff_file(file);
            if (g_mkdir_with_parents(dir, 0755) || !g_file_set_contents(file, (char *)sec, len, &ge) ||
                (s->cfg_snap_diff && !g_file_set_contents(dfile, s->cfg_snap_diff, -1, &ge))) {
                ctl_log(s, "core", "config: snapshot not kept on disk (%s): %s", file,
                        ge ? ge->message : g_strerror(errno));
            } else {
                ctl_log(s, "core", "config: snapshot kept in %s%s", file,
                        s->cfg_snap_diff ? " (+ .diff.txt)" : "");
            }
        }
        ctl_log(s, "core", "config: saved; the config sector (0x%08" PRIx64 ", %u KiB) is kept as the "
                "snapshot for %s", (uint64_t)s->soc->flash_base + s->board->cfg_off, len / 1024, key);
        g_autoptr(GString) x = g_string_new(NULL);
        if (ctl->fs_done) {
            g_string_append(x, ",\"functions\":[");
            for (int i = 0, k = 0; i < s->nfn; i++) {
                if (fn_auto(s, i)) {
                    g_string_append_printf(x, "%s\"%s\"", k++ ? "," : "", s->fn[i].name);
                }
            }
            g_string_append_c(x, ']');
            int acc = fn_find(s, "calibrate_acc");
            if (acc >= 0 && fn_auto(s, acc)) {
                g_string_append(x, ",\"acc_calibrated\":true");
            }
            int mag = fn_find(s, "calibrate_mag");
            if (mag >= 0 && fn_auto(s, mag)) {
                g_string_append(x, ",\"mag_calibrated\":true");
            }
        }
        cfg_event(s, "applied", x->str);
        return;
    }
    if (ctl->cfg == CFG_FN && !blank) {
        return;                         /* the function sees the reset (fn_at_reset) */
    }
    if (ctl->cfg == CFG_FN && ctl->fn_cur >= 0) {
        ctl->fn_cur = -1;               /* reset all: the first start begins again */
        fn_msg(s, s->fn[ctl->fs_i].name, "failed", ctl->fn_step, ctl->fn_steps,
               "the config sector was erased: the first start begins again", NULL);
    }
    ctl->fs_done = false;
    bool first = ctl->reset_first != NULL;     /* a config reset: the whole first start */
    bool skip = ctl->bs_skip;
    if (blank) {
        ctl->bs_skip = false;
    }
    if (blank && !first && !skip && bs_on(s) && bs_load(s, sec)) {
        memory_region_set_dirty(&s->flash, s->board->cfg_off, len);
        memory_region_flush_rom_device(&s->flash, s->board->cfg_off, len);
        ctl->bs_from_bench = true;
        ctl->fs_mask = 0;
        ctl->cfg_i = 0;
        ctl->cfg_tries = 0;
        bs_delta(s);
        ctl->cfg_delta = ctl->cfg_run_n > 0;
        if (ctl->cfg_delta) {
            ctl->cfg = CFG_PENDING;
            ctl_log(s, "core", "config: blank config sector: the bench state written back; the "
                    "script's %d changed/added lines (the delta) go over the MSP UART once the "
                    "firmware answers, then save", ctl->cfg_run_n);
        } else {
            ctl->cfg = CFG_CHECK;
            ctl->bs_check_t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            ctl_log(s, "core", "config: blank config sector: the bench state written back (the "
                    "script unchanged); the calibrations are checked once the firmware answers");
        }
        cfg_event(s, "restored", ctl->bs_extra->str);
        if (ctl->diff_text) {
            diff_send(s, NULL, NULL, false);
        }
        return;
    }
    if (blank) {
        ctl->bs_from_bench = ctl->cfg_delta = false;
        ctl->fs_mask = ~0u;
        cfg_run_set(ctl, ctl->cfg_lines, ctl->cfg_n);
    }
    if (blank && !first && !(s->cfg_snap && !strcmp(s->cfg_snap_key, key))) {
        /* a snapshot of an earlier run (cfg-cache) */
        g_autofree char *file = cfg_cache_file(s, key);
        g_autofree char *data = NULL;
        gsize n = 0;
        if (file && g_file_get_contents(file, &data, &n, NULL)) {
            if (n == len) {
                g_autofree char *dfile = cfg_diff_file(file);
                g_free(s->cfg_snap);
                g_free(s->cfg_snap_key);
                g_free(s->cfg_snap_diff);
                s->cfg_snap = (uint8_t *)g_steal_pointer(&data);
                s->cfg_snap_key = g_strdup(key);
                s->cfg_snap_diff = NULL;
                g_file_get_contents(dfile, &s->cfg_snap_diff, NULL, NULL);
                ctl_log(s, "core", "config: the snapshot for %s from %s%s", key, file,
                        s->cfg_snap_diff ? " (+ its diff)" : "");
            } else {
                ctl_log(s, "core", "config: %s has %" G_GSIZE_FORMAT " bytes, not %u: ignored",
                        file, n, len);
            }
        }
    }
    if (blank && !first && s->cfg_snap && !strcmp(s->cfg_snap_key, key)) {
        memcpy(sec, s->cfg_snap, len);
        memory_region_set_dirty(&s->flash, s->board->cfg_off, len);
        memory_region_flush_rom_device(&s->flash, s->board->cfg_off, len);
        ctl->cfg = CFG_DONE;
        ctl_log(s, "core", "config: blank config sector: the snapshot for %s written back (the "
                "script is not run again)", key);
        if (s->cfg_snap_diff) {
            g_free(ctl->diff_text);
            ctl->diff_text = g_strdup(s->cfg_snap_diff);
            ctl->diff_stale = false;
            ctl->diff_source = "snapshot";
            ctl->diff_at = vnow_s();
        }
        cfg_event(s, "restored", ",\"source\":\"snapshot\"");
        if (s->cfg_snap_diff) {
            diff_send(s, NULL, NULL, false);
        }
        /* the bench's config is now the snapshot's: the script and the automatic functions */
        g_free(ctl->bs_script);
        ctl->bs_script = g_strdup(s->config_cli);
        g_hash_table_remove_all(ctl->bs_fn);
        for (int i = 0; i < s->nfn; i++) {
            if (fn_auto(s, i)) {
                g_hash_table_insert(ctl->bs_fn, g_strdup(s->fn[i].name), fn_inputs(s, i));
            }
        }
        bs_keep(s);
    } else if (blank) {
        ctl->cfg = CFG_PENDING;
        ctl->cfg_i = 0;
        ctl->cfg_tries = 0;
        g_clear_pointer(&ctl->bs_script, g_free);   /* not ours until the script's save */
        g_hash_table_remove_all(ctl->bs_fn);
        if (first) {
            ctl_log(s, "core", "config: reset (%s): the first start runs again", ctl->reset_first);
            ctl->reset_first = NULL;
        }
        ctl_log(s, "core", "config: blank config sector: the CLI script (%d lines) goes over the MSP "
                "UART once the firmware answers", ctl->cfg_run_n);
    } else if (ctl->cfg == CFG_ENTER || ctl->cfg == CFG_SEND || ctl->cfg == CFG_DIFF) {
        ctl->cfg = CFG_PENDING;
        ctl->cfg_i = 0;
        ctl->cfg_tries = 0;
        ctl->diff_capture = false;
        ctl_log(s, "core", "config: the script was interrupted by a reset; it starts over");
    }
}

/* ---- the functions' steps --------------------------------------------------------------- */

/* iNav position index -> its bit in MSP_CALIBRATION_DATA (acceleration.c accGetCalibrationAxisFlags) */
static const uint8_t cal_bit[6] = { 0, 1, 3, 5, 2, 4 };

static void fn_fail(NexusXRState *s, const char *why)
{
    fn_end(s, false, why, NULL);
}

/* iNav's 6 positions: the pose events (ctl_scenario_event) ask, MSP_CALIBRATION_DATA answers */
static void cal_poll(NexusXRState *s, int64_t now)
{
    NxCtl *ctl = s->ctl;
    if (!s->env->sc.running || s->env->sc_gen != ctl->cal_gen) {
        /* an `env` scenario or `reset aircraft` took the model: the poses start over */
        ctl->fn_st = FN_WAIT;
        ctl->fn_step = 0;
        ctl->cal_cmd = 0;
        ctl->cal_pose = -1;
        ctl_log(s, "core", "calibrate_acc: its scenario was stopped or replaced (env, reset "
                "aircraft); it starts over");
        fn_msg(s, "calibrate_acc", "step", 0, ctl->fn_steps,
               "the poses scenario was stopped or replaced: it starts over", NULL);
        return;
    }
    if (ctl->cal_pose < 0) {
        return;
    }
    if (ctl->cal_flags >> cal_bit[ctl->cal_pose] & 1) {
        ctl->cal_n++;
        ctl_log(s, "core", "calibrate_acc: %s (iNav position %d) collected after %.2f s "
                "(positions 0x%02x)", ctl->cal_label, ctl->cal_pose, (now - ctl->cal_t0) / 1e9,
                ctl->cal_flags);
        g_autofree char *d = g_strdup_printf("%s (iNav position %d) collected", ctl->cal_label,
                                             ctl->cal_pose);
        ctl->fn_step = ctl->cal_n;
        fn_msg(s, "calibrate_acc", "step", ctl->cal_n, ctl->fn_steps, d, NULL);
        ctl->cal_pose = -1;
        ctl->cal_cmd = 0;
    } else if (now - ctl->cal_t0 > CAL_POSE_NS) {
        g_autofree char *why = g_strdup_printf("%s (iNav position %d) not collected within %.1f s "
                                               "(positions 0x%02x)", ctl->cal_label, ctl->cal_pose,
                                               CAL_POSE_NS / 1e9, ctl->cal_flags);
        fn_fail(s, why);
    }
}

/* ---- calibrate_mag: iNav's compass calibration (sensors/compass.c compassUpdate) --------------- */
/*
 * MSP_MAG_CALIBRATION starts it: for mag_calibration_time (20..120 s, default 30) iNav pushes a
 * sample into its sphere fit whenever the field turned by more than ~8 deg since the last one,
 * and keeps, per chip axis, the reading of the largest magnitude; at the end magZero = the
 * sphere's centre, magGain = |that reading - magZero| per axis, and it saves. The heading comes
 * out right only if every axis has seen the whole field (|B| along it): with a gain short on one
 * axis the field is scaled unevenly. So the sweep the core runs on the model turns it until
 * each chip axis, both ways, points along the field (the poses computed from the compass's field
 * and mount: the shortest rotation taking that axis onto B), with level and inverted yaw turns
 * between them for the sphere fit; all within 19 s (the shortest calibration time is 20 s), then level.
 * The firmware's result: the COMPASS arming blocker clears; MSP_CALIBRATION_DATA has magZero and
 * magGain.
 */
static void mag_pose(NexusXRState *s, int axis, int sign, double q[4])
{
    double b[3], n = 0, d[3];
    for (int i = 0; i < 3; i++) {
        n += s->mag_ned[i] * s->mag_ned[i];
    }
    n = sqrt(n) > 0 ? sqrt(n) : 1;
    for (int i = 0; i < 3; i++) {
        b[i] = s->mag_ned[i] / n;
    }
    /* the chip axis in body axes: row `axis` of FRD -> chip (an orthogonal map, its transpose back) */
    for (int j = 0; j < 3; j++) {
        double e[3] = { j == 0, j == 1, j == 2 }, c[3];
        fb_mag_body_to_chip(s->mag_mount, e, c);
        d[j] = sign * c[axis];
    }
    /* the shortest rotation d -> b: q = [1 + d.b, d x b], normalised; opposite: 180 deg about an
     * axis perpendicular to d */
    double dot = d[0] * b[0] + d[1] * b[1] + d[2] * b[2];
    double x[3] = { d[1] * b[2] - d[2] * b[1], d[2] * b[0] - d[0] * b[2], d[0] * b[1] - d[1] * b[0] };
    if (dot < -0.999999) {
        double t[3] = { fabs(d[0]) < 0.9, fabs(d[0]) >= 0.9, 0 };
        x[0] = d[1] * t[2] - d[2] * t[1];
        x[1] = d[2] * t[0] - d[0] * t[2];
        x[2] = d[0] * t[1] - d[1] * t[0];
        dot = -1;
    }
    q[0] = 1 + dot;
    q[1] = x[0], q[2] = x[1], q[3] = x[2];
    double qn = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) {
        q[i] /= qn;
    }
}

/* the sweep as a scenario (its steps: hold quats, 1.2 s turns, the axis poses held 0.5 s) */
static bool mag_sweep(NexusXRState *s, fb_scenario_t *sc, Error **errp)
{
    g_autoptr(GString) j = g_string_new("{\"name\":\"mag-calibration\",\"description\":\"the core's "
                                        "compass sweep (calibrate_mag)\",\"steps\":[");
    double t = 0;
    int n = 0;
#define STEP(q, rot, hold, label) do { \
        g_string_append_printf(j, "%s{\"at_s\":%.3f,\"action\":\"hold\",\"quat\":[%.6f,%.6f,%.6f,%.6f]," \
                               "\"rotate_s\":%.3f,\"label\":\"%s\"}", n++ ? "," : "", t, (q)[0], (q)[1], \
                               (q)[2], (q)[3], (rot), (label)); \
        t += (rot) + (hold); \
    } while (0)
    const double level[4] = { 1, 0, 0, 0 };
    static const char *const names[3][2] = { { "chip +X along the field", "chip -X along the field" },
                                            { "chip +Y along the field", "chip -Y along the field" },
                                            { "chip +Z along the field", "chip -Z along the field" } };
    STEP(level, 1.0, 0.2, "level");
    for (int axis = 0; axis < 3; axis++) {
        for (int k = 0; k < 2; k++) {
            double q[4];
            mag_pose(s, axis, k ? -1 : 1, q);
            STEP(q, 1.2, 0.5, names[axis][k]);
        }
    }
    for (int k = 1; k <= 3; k++) {          /* level, yaw 120 / 240 / 360 */
        double h = k * G_PI / 3;
        const double q[4] = { cos(h), 0, 0, sin(h) };
        STEP(q, 1.0, 0.0, "level yaw turn");
    }
    for (int k = 0; k < 3; k++) {           /* inverted, yaw 0 / 120 / 240 */
        double h = k * G_PI / 3;
        const double q[4] = { 0, cos(h), sin(h), 0 };     /* roll 180 then yaw */
        STEP(q, 1.0, 0.0, "inverted yaw turn");
    }
    STEP(level, 1.2, 0.3, "level");
#undef STEP
    g_string_append(j, "]}");
    char e[160];
    if (fb_scenario_parse(sc, j->str, j->len, e, sizeof e)) {
        error_setg(errp, "calibrate_mag: the sweep: %s", e);
        return false;
    }
    return true;
}

/* MSP_MAG_CALIBRATION answered: the firmware calibrates; the sweep */
static void mag_cal_started(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    fb_scenario_t sc;
    Error *err = NULL;
    if (ctl->fn_st != FN_RUN || !fn_is(s, ctl->fn_cur, "calibrate_mag")) {
        return;
    }
    if (!mag_sweep(s, &sc, &err)) {
        fn_fail(s, error_get_pretty(err));
        error_free(err);
        return;
    }
    env_scenario_run(s, &sc);
    ctl->cal_gen = s->env->sc_gen;
    ctl->fn_step = 1;
    ctl_log(s, "core", "calibrate_mag: the firmware calibrates (mag_calibration_time); the sweep "
            "(%d steps, every chip axis both ways along the field)", sc.n);
    fn_msg(s, "calibrate_mag", "step", 1, ctl->fn_steps,
           "MSP_MAG_CALIBRATION: the firmware samples; turning the model through the field", NULL);
}

/* the firmware's result (MSP_CALIBRATION_DATA after the COMPASS blocker cleared) */
static void mag_cal_result(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    g_autofree char *d = g_strdup_printf("magzero %d %d %d, maggain %d %d %d; %s",
                                         ctl->cal_mag_zero[0], ctl->cal_mag_zero[1],
                                         ctl->cal_mag_zero[2], ctl->cal_mag_gain[0],
                                         ctl->cal_mag_gain[1], ctl->cal_mag_gain[2],
                                         ctl->fn_first ? "the first start saves it"
                                                       : "the firmware saved it");
    ctl_log(s, "core", "calibrate_mag: %s (%.1f s after the sweep)", d,
            (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - ctl->fn_t0) / 1e9);
    if (!ctl->cal_mag_zero[0] && !ctl->cal_mag_zero[1] && !ctl->cal_mag_zero[2] &&
        ctl->cal_mag_gain[0] == 1024 && ctl->cal_mag_gain[1] == 1024 && ctl->cal_mag_gain[2] == 1024) {
        /* iNav's "uncalibrated" values (sensors/compass.c): the calibration did not run */
        fn_fail(s, "the firmware kept the uncalibrated values (magzero 0, maggain 1024)");
        return;
    }
    bool first = ctl->fn_first;
    fn_end(s, true, d, ",\"mag_calibrated\":true");
    if (!first) {
        diff_mark_stale(s, "calibrate_mag");
    }
}

/* the running function, per ctl tick */
static void fn_poll(NexusXRState *s, int64_t now)
{
    NxCtl *ctl = s->ctl;
    int i = ctl->fn_cur;
    if (i < 0) {
        return;
    }
    const FbFunction *f = &s->fn[i];
    bool gyro = fn_is(s, i, "calibrate_gyro");
    bool mag = fn_is(s, i, "calibrate_mag");
    uint32_t calflag = ctl->inav ? INAV_FLAG_CAL : ARM_FLAG_CALIB;
    switch (ctl->fn_st) {
    case FN_WAIT:
        if (!ctl->have_boxes || ctl->cli || ctl->cli_entering || s->power_off ||
            ctl->diff_st != DIFF_IDLE) {
            return;                     /* the firmware answers MSP (after a boot, a save) */
        }
        if (ctl->have_status && ctl->armed) {
            fn_fail(s, "the firmware is armed");
        } else if (gyro) {
            if (s->env) {
                fb_scenario_stop(&s->env->sc);
                fb_fdm_hold(s->fdm, 0, 0, NAN, 1.0);
            }
            ctl->fn_st = FN_HOLD;
            ctl->fn_t0 = now;
            ctl->fn_step = 1;
            g_autofree char *d = g_strdup_printf("%s still for %g s, then an MCU reset: the firmware "
                                                 "calibrates the gyro at boot",
                                                 s->env ? "held level and" : "left", ctl->fn_hold_s);
            fn_msg(s, f->name, "step", 1, ctl->fn_steps, d, NULL);
        } else if (mag) {
            if (s->fdm_kind != FB_FDM_PLANE) {
                fn_fail(s, "the sweep needs the plane FDM");
            } else if (!s->mag_present) {
                fn_fail(s, "no compass is wired (a qmc5883l)");
            } else if (!ctl->inav) {
                fn_fail(s, "the compass calibration is iNav's (MSP_MAG_CALIBRATION)");
            } else if (!ctl->have_status) {
                return;                 /* the firmware's sensors: its next status */
            } else if (!(ctl->sensors & MSP_SENSOR_MAG)) {
                /* iNav accepts MSP_MAG_CALIBRATION without a compass and then never raises the
                 * COMPASS blocker (fc/fc_core.c: only with sensors(SENSOR_MAG)) */
                fn_fail(s, "the firmware has not detected the compass (no MAG in its sensors: "
                        "mag_hardware?)");
            } else {
                ctl->fn_st = FN_RUN;
                ctl->fn_t0 = now;
                ctl->cal_cmd = MSP_MAG_CALIBRATION_CMD;
                ctl_log(s, "core", "calibrate_mag: MSP_MAG_CALIBRATION");
            }
        } else if (fn_poses(s, i)) {
            fb_scenario_t sc;
            Error *err = NULL;
            if (!s->env) {
                fn_fail(s, "the poses need the FDM");
            } else if (!env_scenario_file(s, f->scenario, &sc, NULL, &err)) {
                fn_fail(s, error_get_pretty(err));
                error_free(err);
            } else {
                ctl->fn_st = FN_RUN;
                ctl->cal_pose = -1;
                ctl->cal_cmd = 0;
                ctl->cal_n = 0;
                ctl->cal_flags = 0;
                ctl_log(s, "core", "calibrate_acc: scenario %s; MSP_ACC_CALIBRATION at each pose, "
                        "MSP_CALIBRATION_DATA until the firmware has it", sc.name);
                env_scenario_run(s, &sc);
                ctl->cal_gen = s->env->sc_gen;
            }
        } else if (ctl->inav) {
            fn_fail(s, "iNav calibrates the accelerometer in 6 positions: the function needs a "
                    "poses scenario");
        } else {
            /* RotorFlight: level, as the model sits; the firmware saves the result itself */
            ctl->fn_st = FN_RUN;
            ctl->fn_t0 = now;
            ctl->cal_cmd = MSP_ACC_CALIBRATION_CMD;
            ctl_log(s, "core", "calibrate_acc: MSP_ACC_CALIBRATION (level, as the model sits)");
        }
        break;
    case FN_RUN:
        if (mag) {
            if (ctl->cal_cmd == MSP_MAG_CALIBRATION_CMD && now - ctl->fn_t0 > FN_ACC_NS) {
                fn_fail(s, "MSP_MAG_CALIBRATION not answered");
            } else if (!ctl->cal_cmd && (!s->env->sc.running || s->env->sc_gen != ctl->cal_gen)) {
                fn_fail(s, "the sweep was stopped or replaced (env, reset aircraft)");
            }
        } else if (fn_poses(s, i)) {
            cal_poll(s, now);
        } else if (now - ctl->fn_t0 > FN_ACC_NS) {
            fn_fail(s, "MSP_ACC_CALIBRATION not answered");
        }
        break;
    case FN_HOLD:
        if (s->power_off) {
            fn_fail(s, "powered off");
        } else if (now - ctl->fn_t0 >= (int64_t)((1 + ctl->fn_hold_s) * NANOSECONDS_PER_SECOND)) {
            ctl->fn_st = FN_REBOOT;     /* 1 s to rotate level, then hold_s still */
            ctl->fn_t0 = now;
            ctl_log(s, "core", "calibrate_gyro: MCU reset (flash kept): the firmware calibrates the "
                    "gyro at boot");
            ctl_request_reset(s);
        }
        break;
    case FN_REBOOT:
        if (now - ctl->fn_t0 > NANOSECONDS_PER_SECOND) {
            fn_fail(s, "the MCU reset did not happen");
        }
        break;
    case FN_CHECK:
        if (mag) {
            if (!ctl->cal_cmd && ctl->have_status && !(ctl->arm_flags & INAV_FLAG_COMPASS)) {
                ctl->cal_cmd = MSP_CALIBRATION_DATA_CMD;    /* the result: mag_cal_result */
            } else if (now - ctl->fn_t0 > FN_MAG_NS) {
                fn_fail(s, "the firmware's COMPASS blocker did not clear within 130 s after the sweep");
            }
            break;
        }
        if (gyro && ctl->have_status && !(ctl->arm_flags & calflag)) {
            g_autofree char *d = g_strdup_printf("the firmware reports the sensors calibrated (%s "
                                                 "clear) %.2f s after the reset%s",
                                                 ctl->inav ? "CAL" : "CALIB", (now - ctl->fn_t0) / 1e9,
                                                 ctl->fn_seen ? "" : " (clear at its first status)");
            fn_end(s, true, d, ",\"gyro_calibrated\":true");
        } else if (gyro && ctl->have_status) {
            ctl->fn_seen = true;
        } else if (!gyro && ctl->have_status && (ctl->arm_flags & ARM_FLAG_CALIB)) {
            ctl->fn_seen = true;        /* RotorFlight calibrates (400 samples), then saves */
        } else if (!gyro && ctl->have_status && (ctl->fn_seen ||
                                                 now - ctl->fn_t0 >= NANOSECONDS_PER_SECOND)) {
            if (!ctl->fn_seen) {
                fn_fail(s, "the firmware never reported CALIB after MSP_ACC_CALIBRATION");
            } else if (ctl->arm_flags & ARM_FLAG_NO_ACC_CAL) {
                fn_fail(s, "the firmware still reports NO_ACC_CAL");
            } else {
                fn_end(s, true, "level calibration done; the firmware saved it (acc_calibration)",
                       ",\"acc_calibrated\":true");
                diff_mark_stale(s, "calibrate_acc");
            }
            break;
        }
        if (ctl->fn_cur >= 0 && now - ctl->fn_t0 > (gyro ? FN_GYRO_NS : FN_ACC_NS)) {
            fn_fail(s, gyro ? "the firmware still calibrates 20 s after the reset (the model moving?)"
                            : "the firmware did not finish the calibration within 5 s");
        }
        break;
    case FN_SAVE:
        if (now - ctl->fn_t0 > FN_SAVE_NS) {
            fn_fail(s, "no reboot within 10 s after the CLI save (armed? a host tool in the CLI?)");
        }
        break;
    }
}

/* every MCU reset (after cfg_at_reset): the running function's view of it */
static void fn_at_reset(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    int i = ctl->fn_cur;
    if (i < 0) {
        return;
    }
    switch (ctl->fn_st) {
    case FN_WAIT:
        break;                          /* it waits for the firmware anyway */
    case FN_REBOOT:
        ctl->fn_st = FN_CHECK;
        ctl->fn_t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        ctl->fn_seen = false;
        ctl->fn_step = 2;
        fn_msg(s, s->fn[i].name, "step", 2, ctl->fn_steps,
               "rebooting: waiting for the firmware's calibration flag", NULL);
        break;
    case FN_SAVE:
        if (ctl->fn_saved) {
            g_autofree char *d = g_strdup_printf("all 6 positions; acczero %d %d %d, accgain %d %d "
                                                 "%d; saved", ctl->cal_zero[0], ctl->cal_zero[1],
                                                 ctl->cal_zero[2], ctl->cal_gain[0], ctl->cal_gain[1],
                                                 ctl->cal_gain[2]);
            fn_end(s, true, d, ",\"acc_calibrated\":true");
        } else {
            fn_fail(s, "reset before the firmware confirmed the save");
        }
        break;
    default:
        if (fn_poses(s, i)) {
            ctl->fn_st = FN_WAIT;       /* the poses again once the firmware is back */
            ctl->fn_step = 0;
            ctl->cal_cmd = 0;
            ctl->cal_pose = -1;
            ctl_log(s, "core", "calibrate_acc: interrupted by a reset; it starts over");
            fn_msg(s, s->fn[i].name, "step", 0, ctl->fn_steps,
                   "interrupted by a reset: it starts over", NULL);
        } else {
            fn_fail(s, "interrupted by an MCU reset");
        }
        break;
    }
}

/* ctl tick: start the script once the firmware answers MSP; timeouts */
static void cfg_poll(NexusXRState *s, int64_t now)
{
    NxCtl *ctl = s->ctl;
    switch (ctl->cfg) {
    case CFG_PENDING:
        if (ctl->have_variant && ctl->have_version && ctl->have_board && !ctl->cli &&
            !ctl->cli_entering && !s->power_off && ctl->diff_st == DIFF_IDLE) {
            if (++ctl->cfg_tries > CFG_ENTER_TRIES) {
                cfg_fail(s, "the firmware did not enter the CLI");
                break;
            }
            g_autofree char *fw = fw_describe(s);
            ctl->cfg = CFG_ENTER;
            ctl->cfg_t0 = now;
            if (ctl->fs_done) {
                ctl_log(s, "core", "config: entering the CLI on the MSP UART to save what the "
                        "first-start functions did");
            } else {
                ctl_log(s, "core", "config: %s answers; entering the CLI on the MSP UART (%d lines%s, "
                        "then save)", fw, ctl->cfg_run_n, ctl->cfg_apply && ctl->cfg_delta
                        ? " of the bench's script not applied yet (config apply)"
                        : ctl->cfg_apply ? " of the bench's script, all (config apply full)"
                        : ctl->cfg_delta ? " of the delta" : "");
                g_autoptr(GString) x = g_string_new(ctl->cfg_delta ? ",\"delta\":true" : "");
                if (ctl->cfg_delta || ctl->cfg_apply) {
                    g_string_append(x, ctl->bs_extra->str);
                }
                js_cfg_run(x, s);       /* each line with its layer and source */
                cfg_event(s, "applying", x->str);
            }
            ctl->cli_entering = ctl->cli_core_sent = ctl->cli_need_entry = true;
            ctl->cli_enter_ns = now;
            ctl->cli_by = "core";
            ctl->cli_via = "control";
        }
        break;
    case CFG_ENTER:
        if (!ctl->cli && !ctl->cli_entering) {
            ctl->cfg = CFG_PENDING;         /* the entry timed out: again at the next tick */
        }
        break;
    case CFG_SEND:
    case CFG_SAVE:
        if (now - ctl->cfg_t0 > CFG_REPLY_NS) {
            cfg_fail(s, ctl->cfg == CFG_SAVE ? "no reboot within 10 s after save"
                                             : "no CLI prompt within 10 s");
        }
        break;
    case CFG_DIFF:
        if (now - ctl->cfg_t0 > DIFF_REPLY_NS) {
            diff_finish(s, false, cfg_diff_source(ctl));   /* the save goes on */
            cfg_save(s);
        }
        break;
    case CFG_CHECK: {
        /* the firmware's calibration flags once it answers and its boot calibration is over */
        uint32_t calflag = ctl->inav ? INAV_FLAG_CAL : ARM_FLAG_CALIB;
        bool late = now - ctl->bs_check_t0 > BS_CHECK_NS;
        if (!ctl->have_status || !ctl->have_boxes || ctl->cli || ctl->cli_entering || s->power_off ||
            ctl->fn_cur >= 0 || ctl->diff_st != DIFF_IDLE || (!late && (ctl->arm_flags & calflag))) {
            if (late && !ctl->have_status) {
                ctl->cfg = CFG_DONE;
                ctl_log(s, "core", "config: no status from the firmware within %.0f s after the "
                        "restore: the calibrations are not checked", BS_CHECK_NS / 1e9);
            }
            break;
        }
        bs_check(s);
        break;
    }
    default:
        break;
    }
}

/* ---- the script's layers (fb_session: provenance per line) -------------------------------- */

/* the resolver's provenance of s->config_cli's lines and its layered assembly (config_json) */
static void cfg_prov_take(NexusXRState *s, fb_session_t *fs)
{
    if (s->cfg_prov) {
        g_strfreev(s->cfg_prov);
    }
    int n = fb_session_config_nlines(fs);
    s->cfg_prov = g_new0(char *, n + 1);
    for (int i = 0; i < n; i++) {
        const char *layer = "", *src = "";
        fb_session_config_line(fs, i, &layer, &src);
        s->cfg_prov[i] = g_strdup_printf("%s\t%s", layer, src);
    }
    s->cfg_prov_n = n;
    g_free(s->config_json);
    const char *j = fb_session_config_json(fs);
    s->config_json = j ? g_strdup(j) : NULL;
}

/* line i of the script: its layer and source ("file" + "<config-cli file>:<n>" without them) */
static void cfg_prov_get(NexusXRState *s, int i, char **layer, char **src)
{
    const char *p = s->cfg_prov && i < s->cfg_prov_n ? s->cfg_prov[i] : NULL;
    const char *tab = p ? strchr(p, '\t') : NULL;
    if (tab) {
        *layer = g_strndup(p, tab - p);
        *src = g_strdup(tab + 1);
    } else {
        *layer = g_strdup("file");
        *src = g_strdup_printf("%s:%d", s->config_cli_path && *s->config_cli_path ? s->config_cli_path
                               : "config", i + 1);
    }
}

/* {"text":..,"layer":..,"source":..} of cfg_lines[i] */
static void js_cfg_line(GString *g, NexusXRState *s, int i)
{
    g_autofree char *layer = NULL, *src = NULL;
    cfg_prov_get(s, i, &layer, &src);
    g_string_append(g, "{\"text\":");
    js_str(g, s->ctl->cfg_lines[i]);
    g_string_append(g, ",\"layer\":");
    js_str(g, layer);
    g_string_append(g, ",\"source\":");
    js_str(g, src);
    g_string_append_c(g, '}');
}

/* ,"sent":[...]: the lines this run types, each with its layer and source */
static void js_cfg_run(GString *g, NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    g_string_append(g, ",\"sent\":[");
    for (int k = 0; k < ctl->cfg_run_n; k++) {
        int i = 0;
        while (i < ctl->cfg_n && ctl->cfg_lines[i] != ctl->cfg_run[k]) {
            i++;
        }
        if (k) {
            g_string_append_c(g, ',');
        }
        js_cfg_line(g, s, i < ctl->cfg_n ? i : k);
    }
    g_string_append_c(g, ']');
}

/* the script's lines: comments, blank lines, save and exit dropped (the resolver does the same;
 * this is for -M config-cli=<file>); false + errp when a line is too long for the CLI */
static GPtrArray *cfg_split(const char *text)
{
    g_auto(GStrv) l = g_strsplit(text, "\n", -1);
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0; l[i]; i++) {
        char *x = g_strstrip(l[i]);
        if (!*x || *x == '#' || !g_ascii_strcasecmp(x, "save") || !g_ascii_strcasecmp(x, "exit")) {
            continue;
        }
        g_ptr_array_add(a, g_strdup(x));
    }
    return a;
}

static bool cfg_load(NexusXRState *s, Error **errp)
{
    NxCtl *ctl = s->ctl;
    GPtrArray *a = cfg_split(s->config_cli);
    for (guint i = 0; i < a->len; i++) {
        const char *x = g_ptr_array_index(a, i);
        if (strlen(x) > CFG_LINE_MAX) {
            error_setg(errp, "config: line '%s' is longer than the firmware's CLI line (%d "
                       "characters)", x, CFG_LINE_MAX);
            g_ptr_array_free(a, true);
            return false;
        }
    }
    if (s->cfg_prov && (int)a->len != s->cfg_prov_n) {
        /* not the resolver's lines (cannot happen with a bench): no provenance rather than a wrong one */
        warn_report("config: %u script lines, %d with provenance: the layers are not reported",
                    a->len, s->cfg_prov_n);
        g_clear_pointer(&s->cfg_prov, g_strfreev);
        s->cfg_prov_n = 0;
    }
    if (ctl->cfg_run != ctl->cfg_lines) {
        g_free(ctl->cfg_run);           /* a delta: the array only */
    }
    if (ctl->cfg_lines) {
        g_strfreev(ctl->cfg_lines);     /* config apply: the bench's script again */
    }
    ctl->cfg_n = a->len;
    g_ptr_array_set_free_func(a, NULL);
    g_ptr_array_add(a, NULL);
    ctl->cfg_lines = (char **)g_ptr_array_free(a, false);
    ctl->cfg_run = ctl->cfg_lines;
    ctl->cfg_run_n = ctl->cfg_n;
    ctl->fs_mask = ~0u;
    if (!ctl->cfg_errtext) {
        ctl->cfg_errtext = g_string_new(NULL);
    }
    return true;
}

static void fc_poll(NexusXRState *s, int64_t now)
{
    NxCtl *ctl = s->ctl;
    /* MSP_ALTITUDE only while the firmware reports the baro in MSP_STATUS (a wired chip it did
     * not detect gives no altitude either; without one the poll stays what it was), MSP_RAW_GPS
     * only while iNav reports the GPS: ~10 Hz each */
    uint16_t cycle[5] = { ctl->inav ? MSP2_INAV_STATUS_CMD : MSP_STATUS_CMD,
                          MSP_ATTITUDE_CMD, MSP_RC_CMD };
    unsigned ncycle = 3;
    if (ctl->have_status && (ctl->sensors & MSP_SENSOR_BARO)) {
        cycle[ncycle++] = MSP_ALTITUDE_CMD;
    }
    if (ctl->have_status && ctl->inav && (ctl->sensors & MSP_SENSOR_GPS)) {
        cycle[ncycle++] = MSP_RAW_GPS_CMD;
    }

    /* a host frame still in the FIFO has not stalled: its bytes are on their way to the
     * firmware (and the host is back-pressured while the FIFO is full), so the stall clock
     * runs only once the FIFO has drained */
    if (!ctl->cli && ctl->hf_len && host_uart_pending(s)) {
        ctl->host_ns = now;
    }
    if (!ctl->cli && ctl->hf_len && now - ctl->host_ns > MSP_HOST_FRAME_NS) {
        ctl_log(s, "core", "msp: host frame stalled after %u bytes, abandoned", ctl->hf_len);
        ctl->hf_len = 0;
    }
    /* the core's own CLI entry goes in like its polls: between complete host frames, so the
     * '#' never lands inside a host MSP frame */
    if (ctl->cli_need_entry && !ctl->cli && !ctl->hf_len && !s->power_off) {
        if (fc_inject(s, "#\r", 2)) {
            ctl->cli_need_entry = false;
            ctl->cli_enter_ns = now;
            ctl_log(s, "core", "cli: entering CLI mode on the MSP UART");
        }                                       /* FIFO full: next tick */
    }
    if (ctl->cli_entering && now - ctl->cli_enter_ns > CLI_ENTER_NS) {
        bool unsent = ctl->cli_need_entry;
        ctl->cli_entering = ctl->cli_core_sent = ctl->cli_need_entry = false;
        if (!g_queue_is_empty(&ctl->cli_q)) {
            if (unsent) {
                ctl_log(s, "core", "cli: CLI entry not sent within 1 s (host frame in "
                        "progress or MSP UART FIFO full); %u command(s) dropped",
                        g_queue_get_length(&ctl->cli_q));
            } else {
                ctl_log(s, "core", "cli: no CLI banner from the firmware within 1 s "
                        "(it ignores CLI entry while armed); %u command(s) dropped",
                        g_queue_get_length(&ctl->cli_q));
            }
            g_queue_clear_full(&ctl->cli_q, g_free);
        }
    }
    if (ctl->cli || ctl->cli_entering || s->power_off) {
        return;
    }
    if (ctl->poll_cmd && now - ctl->poll_ns > MSP_TIMEOUT_NS) {
        bool page;
        ctl->stale_cmd = ctl->poll_cmd;         /* a late reply is swallowed, not forwarded */
        ctl->stale_ns = now;
        if (ctl->ms && ctl->poll_cmd == modes_cmd(ctl, &page) && ++ctl->ms_tries >= 3) {
            modes_reply(s, ctl->poll_cmd, NULL, 0, true);  /* 3 timeouts: on without it */
        }
        ctl->poll_cmd = 0;
    }
    if (ctl->modes_host_ns && now >= ctl->modes_host_ns) {
        ctl->modes_host_ns = 0;
        modes_changed(ctl);                     /* the host changed modes / saved */
        if (ctl->modes_host_eeprom) {
            ctl->modes_host_eeprom = false;
            diff_mark_stale(s, "the host's MSP_EEPROM_WRITE");
        }
    }
    if (ctl->stale_cmd && now - ctl->stale_ns > MSP_STALE_NS) {
        ctl->stale_cmd = 0;
    }
    /* our request goes in only between complete host frames (the FIFO keeps the order), and
     * never while the firmware is sending a frame to the host */
    if (ctl->poll_cmd || ctl->fr_len || ctl->hf_len) {
        return;
    }
    /* the modes read goes between the periodic polls (right after a reply, far from the next
     * one), so the status/attitude/rc cadence stays exactly what it was */
    if ((ctl->ms || ctl->modes_want) && ctl->have_boxes && ctl->next_poll_ns - now > MSP_GAP_NS) {
        bool page;
        if (!ctl->ms) {
            modes_start(ctl);
        }
        uint16_t mc = modes_cmd(ctl, &page);
        static const uint8_t page1 = 1;
        if (fc_send_msp_p(s, mc, &page1, page)) {
            ctl->poll_cmd = mc;
            ctl->poll_ns = now;
            if (ctl->stale_cmd == mc) {
                ctl->stale_cmd = 0;
            }
        }
        return;
    }
    if (now < ctl->next_poll_ns) {
        return;
    }
    uint16_t cmd;
    if (!ctl->have_variant) {
        cmd = MSP_FC_VARIANT_CMD;
    } else if (!ctl->have_version) {
        cmd = MSP_FC_VERSION_CMD;
    } else if (!ctl->have_board) {
        cmd = MSP_BOARD_INFO_CMD;
    } else if (!ctl->have_boxes) {
        cmd = ctl->boxnames_err ? MSP_BOXIDS_CMD : MSP_BOXNAMES_CMD;
    } else if (ctl->cal_cmd && ctl->fn_cur >= 0) {
        cmd = ctl->cal_cmd;
    } else {
        cmd = cycle[ctl->poll_i++ % ncycle];
    }
    ctl->next_poll_ns = now + (ncycle == 5 ? MSP_POLL5_NS : ncycle == 4 ? MSP_POLL4_NS : MSP_POLL_NS);
    if (!fc_send_msp_p(s, cmd, NULL, 0)) {
        return;                                 /* FIFO full (host flood): next time */
    }
    ctl->poll_cmd = cmd;
    ctl->poll_ns = now;
    if (ctl->stale_cmd == cmd) {
        ctl->stale_cmd = 0;                     /* the next reply with this cmd is the new one */
    }
}

/* NULL or an error for the client; by: "core" (the core's own sessions) | "client" */
static const char *ctl_cli_by(NexusXRState *s, const char *text, const char *by)
{
    NxCtl *ctl = s->ctl;
    g_autofree char *t = g_strdup(text);
    g_strstrip(t);
    if (!*t) {
        return NULL;
    }
    if (ctl->cli) {
        return fc_cli_line(s, t) ? NULL : "cli: MSP UART FIFO full, line dropped";
    }
    g_queue_push_tail(&ctl->cli_q, g_steal_pointer(&t));
    if (!ctl->cli_core_sent) {
        /* our own '#', also when a host tool seemed to start an entry (that '#' may not have
         * been one). "\r" after it: should the firmware already be in the CLI, "#" is an empty
         * comment line there instead of turning our first command into a comment. It is not
         * injected here but by fc_poll at the next gap between host frames (the host framing
         * decides); polling stops meanwhile, and the 1 s entry timeout covers the wait. */
        if (ctl->cli_entering) {
            ctl_log(s, "core", "cli: a host tool sent '#' too; the core sends its own");
        }
        ctl->cli_entering = ctl->cli_core_sent = ctl->cli_need_entry = true;
        ctl->cli_enter_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        ctl->cli_by = by;
        ctl->cli_via = "control";
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Time                                                                */

static const char *ctl_cli(NexusXRState *s, const char *text)
{
    return ctl_cli_by(s, text, "core");
}

static void ctl_rt_anchor(NxCtl *ctl)
{
    ctl->rt_v0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    ctl->rt_h0 = g_get_monotonic_time() * 1000;
    ctl->win_n = 0;
    ctl->win_i = 0;
    ctl->win_next = 0;
}

static void ctl_step_done(void *opaque)
{
    NxCtl *ctl = opaque;
    NexusXRState *s = ctl->s;
    int64_t late = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - ctl->step_deadline;

    /* Under icount the vCPU stops at the step deadline rounded up to a whole instruction. Later
     * means it ran past it: a step armed while the vCPU runs races with the running budget until
     * the kick lands (QEMU's own icount behaviour, also with the idle skip off), or an idle skip
     * jumped over it (must not happen: idle_poll reads the virtual deadline at the skip). */
    if (icount_enabled() && late >= icount_to_ns(1)) {
        bool jumped = s->idle_last_from < ctl->step_deadline &&
                      s->idle_last_to >= ctl->step_deadline + icount_to_ns(1);
        ctl_log(s, "core", "time: step boundary %.9f s overshot by %" PRId64 " ns (%s)",
                ctl->step_deadline / 1e9, late,
                jumped ? "an idle skip jumped over it" : "armed while running");
    }
    qemu_system_vmstop_request_prepare();
    qemu_system_vmstop_request(RUN_STATE_PAUSED);
}

/* Host ns until virtual time `now` is due at wall clock x speed (<= 0: not ahead) */
static int64_t ctl_rt_ahead(NxCtl *ctl, int64_t now, int64_t h)
{
    return ctl->rt_h0 + (int64_t)((now - ctl->rt_v0) / ctl->speed) - h;
}

/*
 * The realtime throttle's sleep. It runs as queued work on the vCPU thread
 * (async_run_on_cpu), like QEMU's own cpu-throttle: the BQL is released
 * while it waits, so the main loop (control clients, sockets, monitor) keeps
 * running. Under icount the virtual clock is the instruction count, so a
 * sleeping vCPU is stopped virtual time: "never faster than wall clock x
 * speed" holds at every tick. Returns early on a pause/stop request (also one
 * already carried out: the vCPU stopped or the VM not running), when realtime
 * is switched off, and after 50 ms (the next tick queues more).
 */
static void ctl_rt_sleep_body(CPUState *cpu, run_on_cpu_data data);

static void ctl_rt_sleep(CPUState *cpu, run_on_cpu_data data)
{
    PROF_ENTER(pa.rt_sleep);
    ctl_rt_sleep_body(cpu, data);
    PROF_LEAVE(pa.rt_sleep);
}

static void ctl_rt_sleep_body(CPUState *cpu, run_on_cpu_data data)
{
    NxCtl *ctl = data.host_ptr;
    int64_t end = g_get_monotonic_time() * 1000 + 50 * SCALE_MS;

    ctl->rt_sleep_queued = false;
    /* an idle-skip halt would let icount warp virtual time on while we wait here (the main
     * loop warps whenever every vCPU is idle): wake the CPU first, the guest re-detects */
    idle_wake(ctl->s);
    for (;;) {
        int64_t h = g_get_monotonic_time() * 1000;
        int64_t ahead = ctl_rt_ahead(ctl, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), h);
        ahead = MIN(ahead, end - h);
        if (!ctl->realtime || cpu->stop || cpu->stopped || !runstate_is_running() ||
            ahead <= 0) {
            break;
        }
        if (ahead > SCALE_MS) {
            qemu_cond_timedwait_bql(cpu->halt_cond, ahead / SCALE_MS);
        } else {
            bql_unlock();
            g_usleep(MAX(ahead / SCALE_US, 1));
            bql_lock();
        }
    }
}

/* -icount with sleep=on (QEMU's default): when the vCPU idles, virtual time is warped forward
 * by host time, so a throttle sleep would leak host time into virtual time. Only sleep=off
 * (what session.py passes) keeps the virtual clock a pure instruction count. */
static bool ctl_icount_sleep_on(void)
{
    QemuOptsList *l = qemu_find_opts_err("icount", NULL);
    QemuOpts *o = l ? qemu_opts_find(l, NULL) : NULL;
    return o ? qemu_opt_get_bool(o, "sleep", true) : true;
}

static void ctl_throttle(NxCtl *ctl, int64_t now)
{
    int64_t h = g_get_monotonic_time() * 1000;

    /* realtime factor: virtual / host over the last ~1 s, sampled each 0.1 s */
    if (now >= ctl->win_next) {
        ctl->win_next = now + 100 * SCALE_MS;
        ctl->win_v[ctl->win_i] = now;
        ctl->win_h[ctl->win_i] = h;
        ctl->win_s[ctl->win_i] = idle_skipped_now(ctl->s);
        int oldest = ctl->win_n < 11 ? 0 : (ctl->win_i + 1) % 11;
        if (ctl->win_n > 0 && h > ctl->win_h[oldest]) {
            ctl->rtf = (double)(now - ctl->win_v[oldest]) / (h - ctl->win_h[oldest]);
        }
        if (ctl->win_n > 0 && now > ctl->win_v[oldest]) {
            ctl->idle_pct = 100.0 * (ctl->win_s[ctl->win_i] - ctl->win_s[oldest]) /
                            (now - ctl->win_v[oldest]);
        }
        ctl->win_i = (ctl->win_i + 1) % 11;
        ctl->win_n = MIN(ctl->win_n + 1, 11);
    }
    if (!ctl->realtime) {
        return;
    }
    /* never faster than wall clock x speed; if we are behind, re-anchor
     * instead of catching up. No sleeping here: this is a virtual-clock timer
     * callback with the BQL held (under icount on the vCPU thread itself). */
    int64_t ahead = ctl_rt_ahead(ctl, now, h);
    if (ahead < -20 * SCALE_MS) {
        ctl->rt_v0 = now;
        ctl->rt_h0 = h;
    } else if (ahead > CTL_RT_SLACK_NS && !ctl->rt_sleep_queued) {
        if (!icount_enabled()) {
            /* without icount virtual time is host time: a sleeping vCPU would only starve
             * the guest while its clock runs on */
            if (!ctl->rt_warned) {
                ctl->rt_warned = true;
                ctl_log(ctl->s, "core", "time: realtime speed < 1 needs -icount; not throttled");
            }
            return;
        }
        if (ctl_icount_sleep_on()) {
            if (!ctl->rt_warned) {
                ctl->rt_warned = true;
                ctl_log(ctl->s, "core", "time: -icount runs with sleep=on (host time warps the "
                        "virtual clock while the CPU idles); realtime throttle disabled, use "
                        "-icount shift=N,sleep=off");
            }
            return;
        }
        ctl->rt_sleep_queued = true;
        async_run_on_cpu(first_cpu, ctl_rt_sleep, RUN_ON_CPU_HOST_PTR(ctl));
    }
}

/* ------------------------------------------------------------------ */
/* Periodic work, 1 kHz of virtual time (from fdm_tick)                 */

/* The FDM counts hard ground contacts; each new one becomes one `crash` event (informational:
 * the model is uprighted by a rotation the IMU sees and can fly on). */
static void ctl_crash_check(NexusXRState *s)
{
    (void)s;                        /* every FDM reports its crashes as events (env_events) */
}

/* a scenario step's event (fb_fdm.c.inc): the core's own flows (the ACC calibration) */
static void ctl_scenario_event(NexusXRState *s, const fb_sc_event_t *e)
{
    NxCtl *ctl = s->ctl;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (ctl && fn_is(s, ctl->fn_cur, "calibrate_mag") && ctl->fn_st == FN_RUN && !ctl->cal_cmd &&
        !strcmp(e->name, "scenario_end") && s->env->sc_gen == ctl->cal_gen) {
        ctl->fn_st = FN_CHECK;
        ctl->fn_t0 = now;
        ctl->fn_step = 2;
        fb_fdm_hold(s->fdm, 0, 0, 0, 0.5);    /* level, still, while the firmware ends */
        fn_msg(s, "calibrate_mag", "step", 2, ctl->fn_steps,
               "sweep done: waiting for the firmware's result (mag_calibration_time)", NULL);
        return;
    }
    if (!ctl || !fn_poses(s, ctl->fn_cur) || ctl->fn_st != FN_RUN) {
        return;
    }
    if (e->index >= 0 && e->index < 6 && strcmp(e->name, "scenario_end")) {
        if (ctl->cal_pose >= 0) {
            g_autofree char *why = g_strdup_printf("%s (iNav position %d) not collected before the "
                                                   "next pose", ctl->cal_label, ctl->cal_pose);
            fn_fail(s, why);
            return;
        }
        ctl->cal_pose = e->index;
        g_strlcpy(ctl->cal_label, e->label, sizeof(ctl->cal_label));
        ctl->cal_t0 = now;
        ctl->cal_cmd = MSP_ACC_CALIBRATION_CMD;
        ctl_log(s, "core", "calibrate_acc: %s reached (iNav position %d): MSP_ACC_CALIBRATION",
                e->label, e->index);
    } else if (!strcmp(e->name, "scenario_end")) {
        if (ctl->cal_pose >= 0 || ctl->cal_n < 6 || ctl->cal_flags != 0x3F) {
            g_autofree char *why = g_strdup_printf("the scenario ended with %d of 6 positions "
                                                   "(0x%02x)", ctl->cal_n, ctl->cal_flags);
            fn_fail(s, why);
            return;
        }
        ctl_log(s, "core", "calibrate_acc: all 6 positions; the firmware computed acczero %d %d %d, "
                "accgain %d %d %d; save", ctl->cal_zero[0], ctl->cal_zero[1], ctl->cal_zero[2],
                ctl->cal_gain[0], ctl->cal_gain[1], ctl->cal_gain[2]);
        ctl->cal_cmd = 0;
        if (ctl->fn_first) {
            /* the first start's final save (CLI, diff all, save) follows: fn_end */
            g_autofree char *d = g_strdup_printf("all 6 positions; acczero %d %d %d, accgain %d %d "
                                                 "%d; the first start saves it", ctl->cal_zero[0],
                                                 ctl->cal_zero[1], ctl->cal_zero[2], ctl->cal_gain[0],
                                                 ctl->cal_gain[1], ctl->cal_gain[2]);
            fn_end(s, true, d, ",\"acc_calibrated\":true");
        } else {
            env_default_run(s);
            ctl->fn_st = FN_SAVE;
            ctl->fn_saved = false;
            ctl->fn_t0 = now;
            fn_msg(s, "calibrate_acc", "step", 6, ctl->fn_steps, "all 6 positions: CLI save", NULL);
            const char *err = ctl_cli(s, "save");
            if (err) {
                fn_fail(s, err);
            }
        }
    }
}

/* the FDM's launch (fb_fdm.c.inc): the `launch` function's progress, whoever asked */
static void ctl_fn_launch(NexusXRState *s, bool release, double speed_mps, double height_m)
{
    if (!s->ctl) {
        return;
    }
    g_autofree char *d = release
        ? g_strdup_printf("released at %.2f m/s, %.2f m", speed_mps, height_m)
        : g_strdup_printf("the ramp to %.2f m/s", speed_mps);
    fn_msg(s, "launch", release ? "done" : "step", release ? 1 : 0, 1, d, NULL);
}

static double ctl_fc_baro_alt(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    return ctl && ctl->have_alt ? ctl->alt_cm / 100.0 : NAN;
}

static void ctl_prof_periodic(NexusXRState *s, int64_t now);
static void ctl_tick_body(NexusXRState *s, int64_t now);

static void ctl_tick(NexusXRState *s, int64_t now)
{
    PROF_ENTER(pa.ctl_tick);
    ctl_tick_body(s, now);
    PROF_LEAVE(pa.ctl_tick);
}

static void ctl_tick_body(NexusXRState *s, int64_t now)
{
    NxCtl *ctl = s->ctl;
    if (!ctl) {
        return;
    }
    ctl_prof_periodic(s, now);
    ctl_throttle(ctl, now);
    cfg_poll(s, now);
    diff_poll(s, now);
    fn_poll(s, now);
    fc_poll(s, now);
    /* the MSP host socket's read watch is gone while the config script owns the CLI
     * (host_uart_can_read); its end must re-poll it: the USART's reads no longer wake the
     * main loop (patches/usart-accept-input.diff) */
    bool held = ctl_cfg_busy(s);
    if (ctl->host_held && !held && s->msp_uart) {
        qemu_chr_fe_accept_input(&s->msp_uart->host);
    }
    ctl->host_held = held;
    ctl_crash_check(s);
    ctl_faults_check(s);

    double v[SIG_COUNT];
    bool have = false;
    g_autoptr(GString) state = NULL;
    for (guint i = 0; i < ctl->clients->len; i++) {
        CtlClient *c = g_ptr_array_index(ctl->clients, i);
        if (c->dead || !c->period_ns || now < c->next_ns) {
            continue;
        }
        c->next_ns = MAX(c->next_ns + c->period_ns, now);
        if (!have) {
            ctl_snapshot(s, v);
            have = true;
        }
        if (!state) {
            state = g_string_sized_new(1200);
            PROF_ENTER(pa.ctl_state);
            ctl_state_json(s, v, state);
            PROF_LEAVE(pa.ctl_state);
        }
        ctl_send(c, state, true);
        if (c->nsig) {
            g_autoptr(GString) g = g_string_sized_new(64 + 32 * c->nsig);
            PROF_ENTER(pa.ctl_samples);
            ctl_samples_json(c, v, g);
            PROF_LEAVE(pa.ctl_samples);
            ctl_send(c, g, true);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Hooks from the machine                                              */

static void ctl_on_reset(NexusXRState *s, const char *cause)
{
    NxCtl *ctl = s->ctl;
    if (!ctl) {
        return;
    }
    bool was_cli = ctl->cli;
    ctl->cli = ctl->cli_entering = ctl->cli_core_sent = ctl->cli_need_entry = false;
    g_queue_clear_full(&ctl->cli_q, g_free);
    ctl->poll_cmd = ctl->stale_cmd = 0;
    ctl->fr_len = 0;
    g_string_truncate(ctl->line, 0);
    ctl->next_poll_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 500 * SCALE_MS;
    ctl->have_variant = ctl->have_version = ctl->have_board = ctl->have_boxes = false;
    ctl->boxnames_err = false;
    ctl->have_status = ctl->have_att = ctl->have_rc = ctl->have_alt = ctl->have_gps = false;
    if (ctl->armed) {
        ctl_event(s, "disarmed", "\"reason\":\"reset\"");
    }
    ctl->armed = false;
    ctl->arm_flags = 0;
    ctl->fs = false;
    ctl->mode_bits = 0;
    ctl->cpu_pct = 0;
    if (was_cli) {
        ctl_log(s, "core", "CLI mode ended by the reboot");
        g_autofree char *d = g_strdup_printf("\"open\":false,\"by\":\"%s\",\"via\":\"%s\"",
                                             ctl->cli_by ?: "client", ctl->cli_via ?: "msp");
        ctl_event(s, "cli", d);
    }
    ctl->cli_by = ctl->cli_via = NULL;
    /* the modes again once the firmware answers (a save, the configurator, a new firmware) */
    ctl->ms = MS_NONE;
    modes_changed(ctl);
    if (ctl->diff_st == DIFF_RUN) {
        diff_finish(s, false, "request");
    }
    if (ctl->diff_st == DIFF_ENTER || ctl->diff_st == DIFF_RUN) {
        diff_waiters(s, "reset");       /* a snapshot / comparison waiting on it: failed */
    }
    ctl->diff_st = DIFF_IDLE;
    ctl->diff_capture = false;
    g_autofree char *d = g_strdup_printf("\"cause\":\"%s\"", cause);
    ctl_event(s, "reboot", d);
    fn_at_reset(s);
}

static void ctl_dshot(NexusXRState *s, uint16_t value)
{
    NxCtl *ctl = s->ctl;
    if (!ctl) {
        return;
    }
    uint16_t cmd = value >= 1 && value <= 47 ? value : 0;
    if (cmd != ctl->last_dshot_cmd) {
        ctl->last_dshot_cmd = cmd;
        if (cmd >= 1 && cmd <= 5) {
            g_autofree char *d = g_strdup_printf("\"cmd\":%u,\"motor\":1", cmd);
            ctl_event(s, "motor_beep", d);
        } else if (cmd) {
            g_autofree char *d = g_strdup_printf("\"cmd\":%u,\"motor\":1", cmd);
            ctl_event(s, "dshot_command", d);
        }
    }
}

static void ctl_beeper(NexusXRState *s, bool level)
{
    bool on = level ^ s->beeper_inverted;
    ctl_event(s, "beep", on ? "\"on\":true" : "\"on\":false");
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */

static void ctl_request_reset(NexusXRState *s)
{
    qemu_system_reset_request(SHUTDOWN_CAUSE_HOST_QMP_SYSTEM_RESET);
}

static bool ctl_load_firmware(NexusXRState *s, const char *path, Error **errp)
{
    NxCtl *ctl = s->ctl;
    uint8_t *img = fw_build_image(s, path, errp);
    if (!img) {
        return false;
    }
    g_free(ctl->fw_pending);
    ctl->fw_pending = img;
    if (path != s->firmware_path) {
        g_free(s->firmware_path);
        s->firmware_path = g_strdup(path);
    }
    ctl_log(s, "core", "firmware: %s parsed, erasing flash and rebooting", path);
    ctl_request_reset(s);
    return true;
}

static void ctl_watch_stat(NxCtl *ctl, int64_t *mtime, int64_t *size)
{
    GStatBuf st;
    if (ctl->fw_path && !g_stat(ctl->fw_path, &st)) {
#ifdef _WIN32
        *mtime = (int64_t)st.st_mtime * 1000000000LL;   /* mingw: seconds only */
#else
        *mtime = st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
        *size = st.st_size;
    } else {
        *mtime = *size = -1;
    }
}

/* once per host second; reload when the file changed and then held still */
static gboolean ctl_watch_poll(gpointer opaque)
{
    NexusXRState *s = opaque;
    NxCtl *ctl = s->ctl;
    int64_t m, z;
    ctl_watch_stat(ctl, &m, &z);
    if (m < 0 || (m == ctl->watch_mtime && z == ctl->watch_size)) {
        ctl->watch_seen_mtime = m;
        return G_SOURCE_CONTINUE;
    }
    if (m != ctl->watch_seen_mtime || z != ctl->watch_seen_size) {
        ctl->watch_seen_mtime = m;         /* still being written? */
        ctl->watch_seen_size = z;
        return G_SOURCE_CONTINUE;
    }
    ctl->watch_mtime = m;
    ctl->watch_size = z;
    Error *err = NULL;
    ctl_log(s, "core", "watch: %s changed on disk, reloading", ctl->fw_path);
    if (!ctl_load_firmware(s, ctl->fw_path, &err)) {
        ctl_log(s, "core", "watch: reload failed: %s", error_get_pretty(err));
        error_free(err);
    }
    return G_SOURCE_CONTINUE;
}

static void ctl_set_watch(NexusXRState *s, const char *path, bool on)
{
    NxCtl *ctl = s->ctl;
    if (ctl->watch_src) {
        g_source_remove(ctl->watch_src);
        ctl->watch_src = 0;
    }
    g_free(ctl->fw_path);
    ctl->fw_path = g_strdup(path);
    ctl->watch = on;
    if (on) {
        ctl_watch_stat(ctl, &ctl->watch_mtime, &ctl->watch_size);
        ctl->watch_seen_mtime = ctl->watch_mtime;
        ctl->watch_seen_size = ctl->watch_size;
        ctl->watch_src = g_timeout_add_seconds(1, ctl_watch_poll, s);
        ctl_log(s, "core", "watch: polling %s once per second", path);
    }
}

static void ctl_cmd_time(CtlClient *c, QDict *d)
{
    NxCtl *ctl = c->ctl;
    NexusXRState *s = ctl->s;
    const char *mode = qdict_get_try_str(d, "mode");

    /* validate everything first: an error means nothing was applied */
    bool realtime = ctl->realtime;
    double speed = ctl->speed, step_s = 0.001;
    if (qdict_haskey(d, "realtime")) {
        QBool *b = qobject_to(QBool, qdict_get(d, "realtime"));
        if (!b) {
            ctl_error(c, "time: realtime must be true|false");
            return;
        }
        realtime = qbool_get_bool(b);
    }
    if (qdict_haskey(d, "speed")) {
        QNum *n = qobject_to(QNum, qdict_get(d, "speed"));
        speed = n ? qnum_get_double(n) : NAN;
        if (!(speed > 0.001 && speed < 1000)) {
            ctl_error(c, "time: speed must be in (0.001, 1000)");
            return;
        }
    }
    if (mode && strcmp(mode, "run") && strcmp(mode, "pause") && strcmp(mode, "step")) {
        ctl_error(c, "time: mode must be run|pause|step");
        return;
    }
    if (qdict_haskey(d, "mode") && !mode) {
        ctl_error(c, "time: mode must be run|pause|step");
        return;
    }
    if (mode && !strcmp(mode, "step") && qdict_haskey(d, "step_s")) {
        QNum *n = qobject_to(QNum, qdict_get(d, "step_s"));
        step_s = n ? qnum_get_double(n) : NAN;
        if (!(step_s > 0 && step_s <= 3600)) {
            ctl_error(c, "time: step_s must be in (0, 3600]");
            return;
        }
    }

    ctl->realtime = realtime;
    ctl->speed = speed;
    ctl_rt_anchor(ctl);
    if (!mode || !strcmp(mode, "run")) {
        if (mode) {
            timer_del(ctl->step_timer);
            ctl->paused = false;
            if (!runstate_is_running()) {
                vm_start();
            }
        }
    } else if (!strcmp(mode, "pause")) {
        timer_del(ctl->step_timer);
        ctl->paused = true;
        if (runstate_is_running()) {
            vm_stop(RUN_STATE_PAUSED);
        }
    } else {                                    /* step (validated above) */
        ctl->paused = true;
        ctl->step_deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (int64_t)(step_s * 1e9);
        timer_mod(ctl->step_timer, ctl->step_deadline);
        if (!runstate_is_running()) {
            vm_start();
        }
    }
    ctl_log(s, "core", "time: %s at t=%.4f, realtime %s, speed %g",
            ctl->paused ? (mode && !strcmp(mode, "step") ? "step" : "paused")
                        : "running",
            vnow_s(), ctl->realtime ? "on" : "off", ctl->speed);
}

static bool json_us_list(QList *l, uint16_t *out, uint32_t *mask, Error **errp)
{
    int i = 0;
    for (const QListEntry *e = qlist_first(l); e && i < FB_CRSF_CHANNELS;
         e = qlist_next(e), i++) {
        if (!json_us(qlist_entry_obj(e), &out[i])) {
            error_setg(errp, "ch_us[%d]: need 800..2200 us", i);
            return false;
        }
        *mask |= 1u << i;
    }
    return true;
}

static void ctl_cmd_reset(CtlClient *c, QDict *d)
{
    NexusXRState *s = c->ctl->s;
    const char *what = qdict_get_try_str(d, "what");
    if (!what) {
        what = "all";
    }
    if (!strcmp(what, "aircraft")) {
        /* Teleport the IMU can follow: the position jumps to the pad, the
         * attitude rotates to level and heading 0 in 0.25 s with body
         * rates the gyro reads, so the firmware's estimator follows
         * without a reboot. "firmware":true also resets the MCU (flash
         * kept); the FDM is then re-initialised at once. */
        bool fw = qdict_get_try_bool(d, "firmware", false);
        ctl_event(s, "reset", fw ? "\"what\":\"aircraft\",\"firmware\":true"
                                 : "\"what\":\"aircraft\",\"firmware\":false");
        if (s->env) {
            env_reset(s, fw);
            c->ctl->crashes_seen = 0;
            ctl_log(s, "core", "reset aircraft: FDM back on the pad%s; env_default again",
                    fw ? ", MCU reset (flash kept)" : ", rotated level/heading 0 through the gyro");
            if (fw) {
                ctl_request_reset(s);
            }
        }
    } else if (!strcmp(what, "firmware")) {
        ctl_log(s, "core", "reset firmware: MCU reset, flash kept");
        ctl_request_reset(s);
    } else if (!strcmp(what, "all")) {
        ctl_log(s, "core", "reset all: erase the config sector, reboot, "
                "custom defaults apply");
        c->ctl->erase_cfg = true;
        c->ctl->bs_skip = true;         /* CLI boards: the first-start snapshot, not the bench state */
        ctl_request_reset(s);
    } else {
        ctl_error(c, "reset: what must be aircraft|firmware|all");
    }
}

/* ------------------------------------------------------------------ */
/* OSD: the MSP DisplayPort device's canvas as `osd` messages          */
/*
 * One message per DRAW_SCREEN the device received (the firmware's OSD frame rate), to every client
 * with `osd` on. `cells` are runs of consecutive cells of one row with the same attr: in a delta
 * the cells that changed since the previous frame (blanks included), in a full frame
 * (`clear:true`: wipe, then write) every non-blank cell. A client gets a full frame first: on
 * connect / `subscribe osd:true` at once (when the device has shown a frame), otherwise with the
 * next frame; the device itself asks for one on a new contact (after silence: the firmware
 * rebooted or re-initialised its display), after RELEASE and after its link_lost fault.
 * `s`: each cell byte as the code point of the same value (U+0000..U+00FF, i.e. Latin-1), JSON-escaped.
 */

static void ctl_osd_json_body(NexusXRState *s, bool full, GString *g);

static void ctl_osd_json(NexusXRState *s, bool full, GString *g)
{
    PROF_ENTER(pa.ctl_osd);
    ctl_osd_json_body(s, full, g);
    PROF_LEAVE(pa.ctl_osd);
}

static void ctl_osd_json_body(NexusXRState *s, bool full, GString *g)
{
    fb_msp_osd_t *o = s->osd;
    size_t n = fb_msp_osd_cells_json(o, full, NULL, 0);

    g_string_append(g, "{\"type\":\"osd\",\"t\":");
    js_time(g, vnow_s());
    g_string_append_printf(g, ",\"frame\":%u,\"rows\":%d,\"cols\":%d,\"clear\":%s,\"link\":true,",
                           fb_msp_osd_stats(o)->frames, fb_msp_osd_rows(o), fb_msp_osd_cols(o),
                           full ? "true" : "false");
    /* the font the codes index: the device's MSP_FC_VARIANT answer, else the control server's own */
    const char *font = fb_msp_osd_font(o);
    if (!font && s->ctl && s->ctl->variant[0]) {
        font = !strcmp(s->ctl->variant, "INAV") ? "inav" : !strcmp(s->ctl->variant, "RTFL") ? "rotorflight" : NULL;
    }
    if (font) {
        g_string_append_printf(g, "\"font\":\"%s\",", font);
    }
    g_string_append(g, "\"cells\":");
    gsize at = g->len;
    g_string_set_size(g, at + n);
    fb_msp_osd_cells_json(o, full, g->str + at, n + 1);
    g_string_append(g, "}\n");
}

/* no picture: clear:true, no cells, link:false (the app greys the panel) */
static void ctl_osd_nolink_json(NexusXRState *s, GString *g)
{
    fb_msp_osd_t *o = s->osd;
    g_string_append(g, "{\"type\":\"osd\",\"t\":");
    js_time(g, vnow_s());
    g_string_append_printf(g, ",\"frame\":%u,\"rows\":%d,\"cols\":%d,\"clear\":true,\"link\":false,"
                           "\"cells\":[]}\n", fb_msp_osd_stats(o)->frames, fb_msp_osd_rows(o), fb_msp_osd_cols(o));
}

/* The picture is gone (power off / MCU reset / silence past the device's threshold, the
 * msp_osd.link_lost fault, the firmware in CLI mode): once per loss, every client with `osd` on
 * gets link:false and its next frame is full. */
static void ctl_osd_link(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    if (!ctl || s->osd_uart < 0 || !ctl->osd_link) {
        return;
    }
    if (fb_msp_osd_link_up(s->osd) && !ctl->cli && !s->power_off) {
        return;
    }
    ctl->osd_link = false;
    g_autoptr(GString) g = g_string_sized_new(160);
    ctl_osd_nolink_json(s, g);
    for (guint i = 0; i < ctl->clients->len; i++) {
        CtlClient *c = g_ptr_array_index(ctl->clients, i);
        if (c->dead || !c->osd) {
            continue;
        }
        ctl_send(c, g, false);
        c->osd_sync = false;
    }
}

/* a client that just turned `osd` on (connect or subscribe): the whole canvas now, if there is one */
static void ctl_osd_sync_client(CtlClient *c)
{
    NexusXRState *s = c->ctl->s;
    c->osd_sync = false;
    if (!c->osd || s->osd_uart < 0 || !fb_msp_osd_stats(s->osd)->frames) {
        return;
    }
    g_autoptr(GString) g = g_string_sized_new(2048);
    if (!c->ctl->osd_link) {
        ctl_osd_nolink_json(s, g);      /* no picture now: grey, the next frame full */
        ctl_send(c, g, false);
        return;
    }
    ctl_osd_json(s, true, g);
    ctl_send(c, g, false);
    c->osd_sync = true;
}

static void ctl_osd_frame(NexusXRState *s, bool full)
{
    NxCtl *ctl = s->ctl;
    g_autoptr(GString) delta = NULL;
    g_autoptr(GString) whole = NULL;
    if (!ctl || ctl->cli) {
        return;         /* CLI mode: the firmware draws nothing; a DRAW still in the FIFO is dropped */
    }
    ctl->osd_link = true;
    /* a DRAW that changes no cell is not sent to a client in sync: iNav redraws its horizon every
     * refresh (erased and written again with the same glyphs), ~47 DRAWs/s, most of them no-ops */
    bool empty = !full && !fb_msp_osd_changed_count(s->osd);
    for (guint i = 0; i < ctl->clients->len; i++) {
        CtlClient *c = g_ptr_array_index(ctl->clients, i);
        if (c->dead || !c->osd || (empty && c->osd_sync)) {
            continue;
        }
        /* not droppable: a lost delta would leave the client's canvas wrong */
        if (full || !c->osd_sync) {
            if (!whole) {
                whole = g_string_sized_new(2048);
                ctl_osd_json(s, true, whole);
            }
            ctl_send(c, whole, false);
            c->osd_sync = true;
        } else {
            if (!delta) {
                delta = g_string_sized_new(512);
                ctl_osd_json(s, false, delta);
            }
            ctl_send(c, delta, false);
        }
    }
}

/* Everything is checked before anything is applied: a bad field leaves the subscription as it
 * was (state rate, log, osd and the signal list). */
static void ctl_cmd_subscribe(CtlClient *c, QDict *d)
{
    double hz = 0;
    bool log, osd, modes;
    uint16_t sig[CTL_MAX_SIGS];
    int nsig = 0;
    QObject *so = qdict_get(d, "signals");
    QList *l = so ? qobject_to(QList, so) : NULL;

    if (qdict_haskey(d, "state_hz")) {
        /* 0 = off; otherwise clamped to 0.1 .. 1000 Hz (the physics tick is 1 kHz) */
        QNum *n = qobject_to(QNum, qdict_get(d, "state_hz"));
        hz = n ? qnum_get_double(n) : NAN;
        if (!(hz >= 0) || !isfinite(hz)) {
            ctl_error(c, "subscribe: state_hz must be 0 (off) or 0.1..1000");
            return;
        }
    }
    if (!ctl_opt_bool(d, "log", c->log, &log)) {
        ctl_error(c, "subscribe: log must be true|false");
        return;
    }
    if (!ctl_opt_bool(d, "osd", c->osd, &osd)) {
        ctl_error(c, "subscribe: osd must be true|false");
        return;
    }
    if (!ctl_opt_bool(d, "modes", c->modes, &modes)) {
        ctl_error(c, "subscribe: modes must be true|false");
        return;
    }
    if (so && !l) {
        ctl_error(c, "subscribe: signals must be a list of signal ids");
        return;
    }
    for (const QListEntry *e = l ? qlist_first(l) : NULL; e; e = qlist_next(e)) {
        QString *qs = qobject_to(QString, qlist_entry_obj(e));
        int k = -1;
        for (int i = 0; qs && i < SIG_COUNT; i++) {
            if (!strcmp(sig_id[i], qstring_get_str(qs))) {
                k = i;
            }
        }
        if (k < 0) {
            g_autofree char *m = g_strdup_printf("subscribe: unknown signal %s (subscription "
                                                 "unchanged)", qs ? qstring_get_str(qs) : "?");
            ctl_error(c, m);
            return;
        }
        if (nsig == CTL_MAX_SIGS) {
            g_autofree char *m = g_strdup_printf("subscribe: more than %d signals "
                                                 "(subscription unchanged)", CTL_MAX_SIGS);
            ctl_error(c, m);
            return;
        }
        sig[nsig++] = k;
    }
    if (qdict_haskey(d, "state_hz")) {
        c->period_ns = hz > 0 ? (int64_t)(1e9 / MAX(0.1, MIN(hz, 1000))) : 0;
        c->next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    c->log = log;
    if (osd != c->osd) {
        c->osd = osd;
        ctl_osd_sync_client(c);     /* on: the whole canvas first; off: resync when back on */
    }
    if (modes && !c->modes) {
        c->modes = true;
        modes_subscribed(c);
    }
    c->modes = modes;
    if (so) {
        memcpy(c->sig, sig, nsig * sizeof(sig[0]));
        c->nsig = nsig;
    }
}


/* ------------------------------------------------------------------ */
/* `prof`: the host-time ledger (-M prof=on, nexus_xr.c "Host-time ledger")                 */

static char *prof_cpu_model(void)
{
    g_autofree gchar *info = NULL;
    if (g_file_get_contents("/proc/cpuinfo", &info, NULL, NULL)) {
        for (char *l = info; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
            if (g_str_has_prefix(l, "model name")) {
                char *v = strchr(l, ':');
                char *e = strchr(l, '\n');
                if (v) {
                    return g_strstrip(g_strndup(v + 1, e ? e - v - 1 : strlen(v + 1)));
                }
            }
        }
    }
    const char *w = g_getenv("PROCESSOR_IDENTIFIER");       /* Windows */
    return g_strdup(w ? w : "unknown");
}

static void ctl_prof_msg(NexusXRState *s, GString *g)
{
    static char *cpu, *os;
    NxCtl *ctl = s->ctl;
    FbpBuf b = { 0 };

    PROF_ENTER(pa.ctl_prof);
    if (!cpu) {
        cpu = prof_cpu_model();
        g_autofree gchar *pretty = g_get_os_info(G_OS_INFO_KEY_PRETTY_NAME);
#ifndef _WIN32
        struct utsname u;
        os = uname(&u) == 0 ? g_strdup_printf("%s (%s %s)", pretty ? pretty : "?", u.sysname,
                                              u.release)
                            : g_strdup(pretty ? pretty : "unknown");
#else
        os = g_strdup(pretty ? pretty : "Windows");
#endif
    }
    g_autoptr(GString) h = g_string_new(",\"host\":{\"cpu\":");
    js_str(h, cpu);
    g_string_append(h, ",\"os\":");
    js_str(h, os);
    g_string_append(h, ",\"qemu\":");
    js_str(h, QEMU_VERSION);
    g_string_append(h, ",\"build\":");
    js_str(h, FB_BUILD ", " __VERSION__);
    g_autofree char *fw = fw_describe(s);
    g_string_append(h, "},\"session\":{\"firmware\":");
    js_str(h, fw);
    g_string_append_printf(h, ",\"icount_shift\":%d,\"idle_skip\":%s,\"idle_skip_mode\":\"%s\","
                           "\"realtime\":%s,\"speed\":",
                           icount_enabled() ? ctz64(icount_to_ns(1)) : -1,
                           s->idle_skip ? "true" : "false",
                           s->idle_skip ? (s->idle_halt_mode ? "halt" : "budget") : "off",
                           ctl->realtime ? "true" : "false");
    js_num(h, ctl->speed);
    g_string_append_printf(h, ",\"fdm\":%s,\"paused\":%s,\"power\":%s,\"prof_period_s\":%u}",
                           s->fdm_on ? "true" : "false", ctl->paused ? "true" : "false",
                           s->power_off ? "false" : "true", s->prof_period_s);
    fbp_json(fbprof, &b, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), h->str);
    g_string_append_len(g, b.p, b.len);
    free(b.p);
    PROF_LEAVE(pa.ctl_prof);
}

/* automatic `prof` every prof-period-s of virtual time (from ctl_tick) */
static void ctl_prof_periodic(NexusXRState *s, int64_t now)
{
    NxCtl *ctl = s->ctl;
    if (!fbprof || !s->prof_period_s || now < ctl->prof_next || !ctl->clients->len) {
        return;
    }
    ctl->prof_next = now + (int64_t)s->prof_period_s * NANOSECONDS_PER_SECOND;
    g_autoptr(GString) g = g_string_sized_new(16 * KiB);
    ctl_prof_msg(s, g);
    ctl_broadcast(ctl, g, false);
}

/* {"type":"prof"} or {"type":"prof","dump":true}: one `prof` message to this client;
 * {"type":"prof","reset":true}: the ledger starts again (both: dump, then reset) */
static void ctl_cmd_prof(CtlClient *c, QDict *d)
{
    NexusXRState *s = c->ctl->s;
    bool reset, dump;
    if (!ctl_opt_bool(d, "reset", false, &reset) || !ctl_opt_bool(d, "dump", !reset, &dump)) {
        ctl_error(c, "prof: dump and reset must be true|false");
        return;
    }
    if (!fbprof) {
        ctl_error(c, "prof: the core runs without the ledger (-M prof=on; PROF=1 core/qemu/run.sh)");
        return;
    }
    if (dump) {
        g_autoptr(GString) g = g_string_sized_new(16 * KiB);
        ctl_prof_msg(s, g);
        ctl_send(c, g, false);
    }
    if (reset) {
        fbp_reset(fbprof, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        ctl_log(s, "core", "prof: ledger reset");
    }
}

/* {"type":"function","name":...,"params":{...}} */
static void ctl_cmd_function(CtlClient *c, QDict *d)
{
    NxCtl *ctl = c->ctl;
    NexusXRState *s = ctl->s;
    const char *name = qdict_get_try_str(d, "name");
    int i = name ? fn_find(s, name) : -1;
    QObject *po = qdict_get(d, "params");
    QDict *pd = po ? qobject_to(QDict, po) : NULL;
    if (i < 0) {
        g_autoptr(GString) m = g_string_new("function: ");
        g_string_append_printf(m, "%s: not one of this aircraft's functions (", name ? name : "need name");
        for (int k = 0; k < s->nfn; k++) {
            g_string_append_printf(m, "%s%s", k ? ", " : "", s->fn[k].name);
        }
        g_string_append(m, s->nfn ? ")" : "none)");
        ctl_error(c, m->str);
        return;
    }
    if (po && !pd) {
        ctl_error(c, "function: params must be an object");
        return;
    }
    if (fn_is(s, i, "launch")) {
        fb_sc_launch_t o, l = s->fn[i].launch;
        Error *err = NULL;
        g_autoptr(QDict) empty = qdict_new();
        if (s->fdm_kind != FB_FDM_PLANE) {
            ctl_error(c, "function launch: the aircraft has no plane FDM");
        } else if (!env_launch_parse(pd ? pd : empty, &o, &err)) {
            ctl_error(c, error_get_pretty(err));
            error_free(err);
        } else {
            /* the catalog's params under the command's */
            l.speed_kmh = isnan(o.speed_kmh) ? l.speed_kmh : o.speed_kmh;
            l.duration_s = isnan(o.duration_s) ? l.duration_s : o.duration_s;
            l.pitch_deg = isnan(o.pitch_deg) ? l.pitch_deg : o.pitch_deg;
            if (o.heading != FB_SC_HEADING_UNSET) {
                l.heading = o.heading;
                l.heading_deg = o.heading_deg;
            }
            env_launch(s, &l);
        }
        return;
    }
    const char *busy = s->power_off ? "the board is powered off"
                     : ctl->armed ? "the firmware is armed"
                     : ctl->fn_cur >= 0 ? "another function is running"
                     : (ctl->cfg >= CFG_PENDING && ctl->cfg <= CFG_SAVE) || ctl->cfg == CFG_FN
                       ? "the first start (config script) is running"
                     : ctl->diff_st != DIFF_IDLE ? "the core is reading the firmware's diff"
                     : ctl->cli || ctl->cli_entering ? "the firmware is in its CLI" : NULL;
    if (busy) {
        g_autofree char *m = g_strdup_printf("function %s: %s", s->fn[i].name, busy);
        ctl_error(c, m);
        return;
    }
    if (fn_is(s, i, "calibrate_gyro") && pd) {
        QNum *h = qobject_to(QNum, qdict_get(pd, "hold_s"));
        if (qdict_haskey(pd, "hold_s") && (!h || !(qnum_get_double(h) >= 0 && qnum_get_double(h) <= 60))) {
            ctl_error(c, "function calibrate_gyro: params.hold_s must be a number 0..60");
            return;
        }
    }
    fn_begin(s, i, false);
    QNum *h = pd ? qobject_to(QNum, qdict_get(pd, "hold_s")) : NULL;
    if (fn_is(s, i, "calibrate_gyro") && h) {
        ctl->fn_hold_s = qnum_get_double(h);
    }
}

/* why the config flow cannot start a run now (NULL: it can) */
static const char *cfg_busy_why(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    if (s->power_off) {
        return "power_off";
    }
    if (ctl->armed) {
        return "armed";
    }
    if (ctl->cli || ctl->cli_entering) {
        return "cli";
    }
    if ((ctl->cfg >= CFG_PENDING && ctl->cfg <= CFG_SAVE) || ctl->cfg == CFG_FN || ctl->cfg == CFG_CHECK ||
        ctl->fn_cur >= 0 || ctl->diff_st != DIFF_IDLE) {
        return "busy";
    }
    if (!ctl->have_board) {
        return "not_ready";
    }
    return NULL;
}

/* the bench assembled again from its files (config apply / export); NULL + *why when it cannot be.
 * The resolver's notes and errors are logged. */
static fb_session_t *bench_reload(NexusXRState *s, const char *what, char **why)
{
    if (!s->session_path || !*s->session_path) {
        *why = g_strdup("the core was started without a bench (session=): no layers to assemble");
        return NULL;
    }
    fb_session_opts_t o = { .root = s->session_root };
    fb_session_t *fs = fb_session_load(s->session_path, &o);
    for (int i = 0; i < fb_session_nwarnings(fs); i++) {
        ctl_log(s, "core", "config %s: session: %s", what, fb_session_warning(fs, i));
    }
    if (!fb_session_ok(fs)) {
        for (int i = 0; i < fb_session_nerrors(fs); i++) {
            ctl_log(s, "core", "config %s: session: %s", what, fb_session_error(fs, i));
        }
        *why = g_strdup_printf("the bench %s is rejected: %s%s", s->session_path, fb_session_error(fs, 0),
                               fb_session_nerrors(fs) > 1 ? " (more in the log)" : "");
        fb_session_free(fs);
        return NULL;
    }
    ctl_log(s, "core", "config %s: the bench %s assembled again from its files", what, s->session_path);
    return fs;
}

/* the bench's script as its files assemble it now becomes the running script (config apply /
 * reset): s->config_cli, its provenance, cfg_lines. false + *why (nothing changed) when the bench is
 * rejected, has no script or a line too long for the CLI. A changed wiring / device option (the
 * -M string) is logged and *restart set: only the script is taken. */
static bool cfg_reload(NexusXRState *s, const char *what, bool *restart, char **why)
{
    *restart = false;
    fb_session_t *fs = bench_reload(s, what, why);
    if (!fs) {
        return false;
    }
    size_t n = 0;
    const char *t = fb_session_config_cli(fs, &n);
    g_autofree char *text = g_strndup(t ? t : "", n);
    if (!*text && !s->config_cli) {
        fb_session_free(fs);
        *why = g_strdup("the bench has no script (no board, aircraft, profile, device or bench lines)");
        return false;
    }
    g_autoptr(GPtrArray) chk = cfg_split(text);
    for (guint i = 0; i < chk->len; i++) {
        if (strlen(g_ptr_array_index(chk, i)) > CFG_LINE_MAX) {
            *why = g_strdup_printf("line '%s' is longer than the firmware's CLI line (%d characters)",
                                   (char *)g_ptr_array_index(chk, i), CFG_LINE_MAX);
            fb_session_free(fs);
            return false;
        }
    }
    *restart = strcmp(fb_session_options_str(fs), s->session_opts ? s->session_opts : "") != 0;
    if (*restart) {
        ctl_log(s, "core", "config %s: the bench's machine options changed since the start (%s -> %s): "
                "only the script is taken; restart the bench for the wiring and the devices", what,
                s->session_opts ? s->session_opts : "", fb_session_options_str(fs));
    }
    g_free(s->config_cli);
    s->config_cli = g_steal_pointer(&text);
    cfg_prov_take(s, fs);
    fb_session_free(fs);
    Error *err = NULL;
    cfg_load(s, &err);                  /* the lengths were checked above */
    error_free(err);
    return true;
}

/* {"type":"config","apply":true[,"full":true]}: the bench's script as its files assemble it now,
 * typed into the firmware (the lines the firmware's sector does not have and the settings its config
 * changed, or all), then save */
static void cfg_cmd_apply(CtlClient *c, QDict *d)
{
    NxCtl *ctl = c->ctl;
    NexusXRState *s = ctl->s;
    bool full, apply;
    if (!ctl_opt_bool(d, "apply", false, &apply) || !apply || !ctl_opt_bool(d, "full", false, &full)) {
        ctl_error(c, "config apply: apply must be true, full true|false");
        return;
    }
    if (!s->board->cli_config) {
        ctl_error(c, "config apply: this board is configured by a blob, not a CLI script (a firmware "
                  "load or reset all places the blob)");
        return;
    }
    if (s->config_cli_path && *s->config_cli_path) {
        ctl_error(c, "config apply: the script is -M config-cli='s file, not the bench's layers");
        return;
    }
    const char *busy = cfg_busy_why(s);
    if (busy) {
        g_autofree char *e = g_strdup_printf("config apply: not now: %s", busy);
        ctl_log(s, "core", "%s", e);
        ctl_error(c, e);
        return;
    }
    if (!s->session_path || !*s->session_path) {
        ctl_error(c, "config apply: the core was started without a bench (session=): no layers to assemble");
        return;
    }
    /* the delta needs the firmware's config now: a `diff all` first when the last one is stale */
    bool need_diff = !full && ctl->bs_script && (!ctl->diff_text || ctl->diff_stale);
    if (need_diff) {
        const char *e = diff_request(s);
        if (e) {
            g_autofree char *m = g_strdup_printf("config apply: the firmware's config is not known (the last "
                                                 "`diff all` is %s) and `diff all` is not possible now: %s; "
                                                 "nothing typed", ctl->diff_text ? "stale" : "none", e);
            ctl_log(s, "core", "%s", m);
            ctl_error(c, m);
            return;
        }
    }
    g_autofree char *why = NULL;
    bool restart;
    if (!cfg_reload(s, "apply", &restart, &why)) {
        g_autofree char *e = g_strdup_printf("config apply: %s", why);
        ctl_error(c, e);
        if (need_diff) {
            ctl_log(s, "core", "config apply: the `diff all` started for it runs on (the diff message)");
        }
        return;
    }
    g_string_assign(ctl->bs_extra, ",\"apply\":true");
    if (restart) {
        g_string_append(ctl->bs_extra, ",\"restart_needed\":true");
    }
    if (need_diff) {
        ctl->apply_wait = c->apply_wait = true;
        ctl_log(s, "core", "config apply: `diff all` first (the last one is %s): a setting the firmware's "
                "config changed gets the script's value again", ctl->diff_text ? "stale" : "none");
        return;
    }
    cfg_apply_go(s, full);
}

/* a `config apply` with the bench's script loaded (cfg_reload) and bs_extra begun: the run (the
 * delta + the settings the firmware's config changed, or all) and the config flow started */
static void cfg_apply_go(NexusXRState *s, bool full)
{
    NxCtl *ctl = s->ctl;
    if (s->power_off || ctl->armed) {   /* after its diff: the diff's CLI and exit the run waits on */
        ctl_log(s, "core", "config apply: not now: %s; nothing typed", s->power_off ? "power_off" : "armed");
        return;
    }
    bool delta = !full && ctl->bs_script;
    if (!full && !ctl->bs_script) {
        ctl_log(s, "core", "config apply: what the firmware's sector has is not known (no applied script): "
                "all lines");
    }
    if (delta) {
        /* the settings the firmware's config has with other values than the script's (the diff is
         * fresh here): typed again, reported as "reasserted" */
        g_autofree unsigned char *re = g_new0(unsigned char, ctl->cfg_n + 1);
        g_autoptr(GPtrArray) old = cfg_split(ctl->bs_script);
        if (ctl->diff_text && !ctl->diff_stale) {
            char **own;
            fb_cli_line_t *l = cfg_script_lines(s, &own);
            /* a setting whose script line is new is the delta's own, not a reassertion */
            if (fb_cli_changed_lines(l, ctl->cfg_n, ctl->diff_text, (const char *const *)old->pdata, old->len,
                                     re)) {
                char *j = fb_cli_diff_json(l, ctl->cfg_n, ctl->diff_text);
                g_autofree char *obj = g_strdup_printf("{%s}", j);
                QObject *o = qobject_from_json(obj, NULL);
                QDict *od = o ? qobject_to(QDict, o) : NULL;
                QList *ch = od ? qdict_get_qlist(od, "changed") : NULL;
                int k = 0;
                for (const QListEntry *e = ch ? qlist_first(ch) : NULL; e; e = qlist_next(e)) {
                    QDict *x = qobject_to(QDict, qlist_entry_obj(e));
                    const char *sc = x ? qdict_get_try_str(x, "script") : NULL;
                    /* a changed line of the script is the delta's own, not a reassertion */
                    if (!sc || !fbbs_has((const char *const *)old->pdata, old->len, sc)) {
                        continue;
                    }
                    g_autoptr(GString) js = qobject_to_json(qlist_entry_obj(e));
                    g_string_append(ctl->bs_extra, k++ ? "," : ",\"reasserted\":[");
                    g_string_append(ctl->bs_extra, js->str);
                    ctl_log(s, "core", "config apply: %s: the firmware has '%s', the script '%s' [%s:%s]: typed "
                            "again", qdict_get_try_str(x, "setting"), qdict_get_try_str(x, "firmware"), sc,
                            qdict_get_try_str(x, "layer"), qdict_get_try_str(x, "source"));
                }
                if (k) {
                    g_string_append_c(ctl->bs_extra, ']');
                }
                qobject_unref(o);
                free(j);
            }
            g_strfreev(own);
            g_free(l);
        } else {
            ctl_log(s, "core", "config apply: the firmware's config is not known (the last `diff all` is %s): "
                    "only the delta", ctl->diff_text ? "stale" : "none");
        }
        cfg_delta_run(s, re, "config apply");
    } else {
        cfg_run_set(ctl, ctl->cfg_lines, ctl->cfg_n);
    }
    ctl->cfg_delta = delta;
    if (!ctl->cfg_run_n) {
        ctl_log(s, "core", "config apply: the firmware's config has every line of the bench's script "
                "(%d lines): nothing to type, no reboot", ctl->cfg_n);
        g_autofree char *x = g_strdup_printf(",\"delta\":true%s", ctl->bs_extra->str);
        cfg_event(s, "applied", x);
        return;
    }
    ctl->cfg_apply = true;
    ctl->fs_mask = 0;                   /* no first-start functions: the lines, diff all, save */
    ctl->fs_done = false;
    ctl->cfg_i = 0;
    ctl->cfg_tries = 0;
    ctl->cfg = CFG_PENDING;
    ctl_log(s, "core", "config apply: %d of the bench's %d script lines go over the MSP UART now, then "
            "save (the firmware reboots)", ctl->cfg_run_n, ctl->cfg_n);
}

/* the absolute path of a client's path: a relative one is the data root's (as the bench's paths) */
static char *cfg_abs_path(NexusXRState *s, const char *p)
{
    if (g_path_is_absolute(p) || !s->data_root) {
        return g_strdup(p);
    }
    return g_build_filename(s->data_root, p, NULL);
}

/* {"type":"config","snapshot":"bench","path":..}: the firmware's `diff all` (the diff path: CLI,
 * diff all, exit - it reboots) written to the file the app names, as the bench's lines */
static void cfg_cmd_snapshot(CtlClient *c, QDict *d)
{
    NxCtl *ctl = c->ctl;
    NexusXRState *s = ctl->s;
    const char *what = qdict_get_try_str(d, "snapshot");
    const char *path = qdict_get_try_str(d, "path");
    if (g_strcmp0(what, "bench")) {
        ctl_error(c, "config snapshot: snapshot must be \"bench\" (the bench's lines, bench_cli)");
        return;
    }
    if (!path || !*path) {
        ctl_error(c, "config snapshot: need \"path\": the file for the bench's lines (the app names "
                  "it; the core writes no file it was not given)");
        return;
    }
    if (!s->board->cli_config) {
        ctl_error(c, "config snapshot: this board is configured by a blob, not a CLI script");
        return;
    }
    const char *e = diff_request(s);
    if (e) {
        g_autofree char *m = g_strdup_printf("config snapshot: `diff all` not possible now: %s", e);
        ctl_log(s, "core", "%s", m);
        ctl_error(c, m);
        return;
    }
    g_free(c->snap_path);
    c->snap_path = cfg_abs_path(s, path);
    ctl_log(s, "core", "config snapshot: `diff all` for %s (the firmware reboots after it)", c->snap_path);
}

/* the snapshot file: the diff's lines without blank lines and batch/defaults/save (the resolver
 * rejects `defaults`, the core saves at the end), a header naming what it is */
static void cfg_snap_write(NexusXRState *s, CtlClient *c)
{
    NxCtl *ctl = s->ctl;
    g_autoptr(GString) f = g_string_new(NULL);
    g_autofree char *fw = fw_describe(s);
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree char *when = g_date_time_format(now, "%Y-%m-%dT%H:%M:%S%z");
    g_string_append_printf(f, "# FlightBench: the bench's lines (bench_cli, layer \"bench\"): the firmware's "
                           "`diff all`\n# %s, virtual t=%.2f s, %s; written by the core on the app's "
                           "request (config snapshot)\n", fw, ctl->diff_at, when);
    int lines = 0;
    g_auto(GStrv) l = g_strsplit(ctl->diff_text, "\n", -1);
    for (int i = 0; l[i]; i++) {
        char *x = g_strstrip(l[i]);
        g_auto(GStrv) w = g_strsplit_set(x, " \t", 2);
        const char *cmd = w[0] ? w[0] : "";
        if (!*x || !g_ascii_strcasecmp(cmd, "batch") || !g_ascii_strcasecmp(cmd, "defaults") ||
            !g_ascii_strcasecmp(cmd, "save") || !g_ascii_strcasecmp(cmd, "exit")) {
            continue;
        }
        g_string_append_printf(f, "%s\n", x);
        lines += *x != '#';
    }
    g_autoptr(GError) ge = NULL;
    g_autoptr(GString) g = g_string_new("{\"type\":\"config\",\"t\":");
    js_time(g, vnow_s());
    g_string_append(g, ",\"snapshot\":");
    /* g_file_set_contents: a temporary file in the same directory, renamed over the target */
    if (g_file_set_contents(c->snap_path, f->str, f->len, &ge)) {
        ctl_log(s, "core", "config snapshot: %d lines of `diff all` written to %s: the bench's lines "
                "once the app names the file bench_cli", lines, c->snap_path);
        g_string_append(g, "\"written\",\"path\":");
        js_str(g, c->snap_path);
        g_string_append_printf(g, ",\"lines\":%d}\n", lines);
    } else {
        ctl_log(s, "core", "config snapshot: %s not written: %s", c->snap_path, ge->message);
        g_string_append(g, "\"failed\",\"path\":");
        js_str(g, c->snap_path);
        g_string_append(g, ",\"error\":");
        js_str(g, ge->message);
        g_string_append(g, "}\n");
    }
    ctl_send(c, g, false);
}

/* {"type":"config","export":true}: what a preset needs, the files are the app's to pack: the layered
 * assembly as the bench's files give it now (layers with their files, lines with their sources,
 * disabled lines, conflicts), the script, whether the firmware's sector has it, the last diff */
static void cfg_cmd_export(CtlClient *c, QDict *d)
{
    NxCtl *ctl = c->ctl;
    NexusXRState *s = ctl->s;
    bool ex;
    if (!ctl_opt_bool(d, "export", false, &ex) || !ex) {
        ctl_error(c, "config export: export must be true");
        return;
    }
    g_autofree char *why = NULL;
    fb_session_t *fs = s->session_path && *s->session_path ? bench_reload(s, "export", &why) : NULL;
    if (s->session_path && *s->session_path && !fs) {
        g_autofree char *e = g_strdup_printf("config export: %s", why);
        ctl_error(c, e);
        return;
    }
    const char *assembly = fs ? fb_session_config_json(fs) : s->config_json;
    size_t n = 0;
    const char *sc = fs ? fb_session_config_cli(fs, &n) : s->config_cli;
    g_autofree char *script = sc ? g_strndup(sc, fs ? n : strlen(sc)) : NULL;
    g_autoptr(GString) g = g_string_new("{\"type\":\"config\",\"t\":");
    js_time(g, vnow_s());
    g_string_append(g, ",\"export\":{\"bench\":");
    if (s->session_path && *s->session_path) {
        js_str(g, s->session_path);
    } else {
        g_string_append(g, "null");
    }
    g_string_append(g, ",\"root\":");
    if (s->data_root) {
        js_str(g, s->data_root);
    } else {
        g_string_append(g, "null");
    }
    g_string_append_printf(g, ",\"assembly\":%s,\"script\":", assembly ? assembly : "null");
    if (script) {
        js_str(g, script);
    } else {
        g_string_append(g, "null");
    }
    bool applied = script && ctl->bs_script && !strcmp(script, ctl->bs_script);
    g_string_append_printf(g, ",\"applied\":%s,\"diff\":{\"text\":", applied ? "true" : "false");
    if (ctl->diff_text) {
        js_str(g, ctl->diff_text);
        g_string_append_printf(g, ",\"stale\":%s,\"source\":", ctl->diff_stale ? "true" : "false");
        js_str(g, ctl->diff_source);
        g_string_append(g, ",\"at\":");
        js_time(g, ctl->diff_at);
    } else {
        g_string_append(g, "null,\"stale\":true,\"source\":null,\"at\":null");
    }
    g_string_append(g, "}}}\n");
    ctl_log(s, "core", "config export: the layered assembly (%s), the script (%s the firmware's), the last "
            "diff (%s) sent", fs ? "from the bench's files now" : "as started", applied ? "=" : "not",
            ctl->diff_text ? ctl->diff_source : "none");
    ctl_send(c, g, false);
    if (fs) {
        fb_session_free(fs);
    }
}

/* {"type":"config","reset":"first_start"|"defaults"}: the bench state discarded, the config
 * sector erased, the whole first start (script + automatic functions) again */
static void cfg_cmd_reset(CtlClient *c, QDict *d)
{
    NxCtl *ctl = c->ctl;
    NexusXRState *s = ctl->s;
    const char *r = qdict_get_try_str(d, "reset");
    if (!r || (strcmp(r, "first_start") && strcmp(r, "defaults"))) {
        ctl_error(c, "config: reset must be first_start|defaults");
        return;
    }
    if (!ctl->cfg_lines && !bs_on(s)) {
        ctl_error(c, s->board->cli_config
                  ? "config: no script and no bench state: nothing to reset (reset all erases the config)"
                  : "config: this board is configured by a blob, not a CLI script (reset all erases its config)");
        return;
    }
    if (ctl->armed) {
        ctl_error(c, "config: the firmware is armed");
        return;
    }
    if (s->board->cli_config && s->session_path && *s->session_path &&
        !(s->config_cli_path && *s->config_cli_path)) {
        /* the first start types the bench's script as its files assemble it now (a snapshot the
         * app named as bench_cli since the start included) */
        g_autofree char *why = NULL;
        bool restart;
        if (!cfg_reload(s, "reset", &restart, &why)) {
            g_autofree char *e = g_strdup_printf("config: reset: %s; nothing reset", why);
            ctl_error(c, e);
            return;
        }
    }
    if (bs_on(s)) {
        g_autofree char *jf = bs_json_file(s->bench_state);
        g_unlink(s->bench_state);
        g_unlink(jf);
        g_clear_pointer(&ctl->bs_pending, g_free);
        ctl_log(s, "core", "config: reset (%s): the bench state %s discarded", r, s->bench_state);
    }
    g_clear_pointer(&ctl->bs_script, g_free);
    g_hash_table_remove_all(ctl->bs_fn);
    if (ctl->fn_cur >= 0) {
        fn_msg(s, s->fn[ctl->fn_cur].name, "failed", ctl->fn_step, ctl->fn_steps,
               "config reset: the first start runs again", NULL);
        ctl->fn_cur = -1;
    }
    ctl->cfg = CFG_NONE;                /* whatever the config flow did: it starts over */
    ctl->reset_first = !strcmp(r, "first_start") ? "first_start" : "defaults";
    ctl->erase_cfg = true;
    g_autofree char *x = g_strdup_printf(",\"reset\":\"%s\"", r);
    cfg_event(s, "reset", x);
    ctl_log(s, "core", "config: reset (%s): erase the config sector, reboot, the first start (the "
            "script, the automatic functions) runs again", r);
    ctl_request_reset(s);
}

/* {"type":"config", one of reset | apply | snapshot | export} */
static void ctl_cmd_config(CtlClient *c, QDict *d)
{
    static const char *const ops[] = { "reset", "apply", "snapshot", "export" };
    int op = -1, nop = 0;
    for (int i = 0; i < 4; i++) {
        if (qdict_haskey(d, ops[i])) {
            op = i;
            nop++;
        }
    }
    if (nop != 1) {
        ctl_error(c, "config: need one of reset, apply, snapshot, export");
        return;
    }
    switch (op) {
    case 0:
        cfg_cmd_reset(c, d);
        break;
    case 1:
        cfg_cmd_apply(c, d);
        break;
    case 2:
        cfg_cmd_snapshot(c, d);
        break;
    default:
        cfg_cmd_export(c, d);
        break;
    }
}

static void ctl_line_body(CtlClient *c, const char *line);

static void ctl_line(CtlClient *c, const char *line)
{
    PROF_ENTER(pa.ctl_cmd);
    ctl_line_body(c, line);
    PROF_LEAVE(pa.ctl_cmd);
}

static void ctl_line_body(CtlClient *c, const char *line)
{
    NxCtl *ctl = c->ctl;
    NexusXRState *s = ctl->s;
    Error *err = NULL;
    QObject *o = qobject_from_json(line, &err);
    QDict *d = o ? qobject_to(QDict, o) : NULL;
    const char *type = d ? qdict_get_try_str(d, "type") : NULL;

    if (d && !type && !g_strcmp0(qdict_get_try_str(d, "cmd"), "prof")) {
        type = "prof";                  /* {"cmd":"prof",...} is accepted as well */
    }

    if (err || !d || !type) {
        ctl_error(c, err ? error_get_pretty(err) : "need a JSON object with \"type\"");
        error_free(err);
        qobject_unref(o);
        return;
    }
    if (!strcmp(type, "rc")) {
        QList *l = qdict_get_qlist(d, "ch_us");
        struct PilotCmd pc = { .link = -1 };
        if (!l) {
            ctl_error(c, "rc: need ch_us list");
        } else if (!json_us_list(l, pc.ch, &pc.ch_mask, &err)) {
            ctl_error(c, error_get_pretty(err));
            error_free(err);
        } else {
            pilot_apply(s, &pc);
        }
    } else if (!strcmp(type, "link")) {
        /* ranges from the crsf descriptor (the lq fault's params); all checked, then applied */
        static const char *const keys[3] = { "rssi_dbm", "lq", "snr_db" };
        int val[3] = { s->link_rssi, s->link_lq, s->link_snr };
        const fb_fault_desc_t *fd = fb_device_fault(fb_device_find("crsf"), "lq");
        for (int i = 0; i < 3; i++) {
            if (!qdict_haskey(d, keys[i])) {
                continue;
            }
            const fb_param_desc_t *pp = NULL;
            for (int k = 0; fd && fd->params && fd->params[k].id; k++) {
                if (!strcmp(fd->params[k].id, keys[i])) {
                    pp = &fd->params[k];
                }
            }
            QNum *n = qobject_to(QNum, qdict_get(d, keys[i]));
            double v = n ? qnum_get_double(n) : NAN;
            if (!pp || !isfinite(v) || v != floor(v) ||
                (pp->has_range && (v < pp->min || v > pp->max))) {
                g_autofree char *m = g_strdup_printf("link: %s must be an integer in %g..%g",
                                                     keys[i], pp ? pp->min : 0, pp ? pp->max : 0);
                ctl_error(c, m);
                qobject_unref(o);
                return;
            }
            val[i] = (int)v;
        }
        if (qdict_haskey(d, "up") && !qobject_to(QBool, qdict_get(d, "up"))) {
            ctl_error(c, "link: up must be true|false");
            qobject_unref(o);
            return;
        }
        s->link_rssi = val[0];
        s->link_lq = val[1];
        s->link_snr = val[2];
        /* alias of the crsf.link_lost fault; rssi/lq/snr are the receiver's normal values */
        bool up = qdict_get_try_bool(d, "up", !fb_crsf_fault_active(s->crsf, "link_lost"));
        struct PilotCmd pc = { .link = up };
        pilot_apply(s, &pc);
        ctl_log(s, "core", "link %s rssi %d dBm lq %d%% snr %d dB",
                up ? "up" : "DOWN", s->link_rssi, s->link_lq, s->link_snr);
        ctl_faults_check(s);
    } else if (!strcmp(type, "fault")) {
        ctl_cmd_fault(c, d);
    } else if (!strcmp(type, "time")) {
        ctl_cmd_time(c, d);
    } else if (!strcmp(type, "reset")) {
        ctl_cmd_reset(c, d);
    } else if (!strcmp(type, "cli") && qdict_get_try_bool(d, "exit", false)) {
        if (!ctl->cli) {
            ctl_error(c, ctl->cli_entering ? "cli: the firmware is entering its CLI (not in it yet)"
                                           : "cli: the firmware is not in its CLI");
        } else if (ctl->cfg >= CFG_ENTER && ctl->cfg <= CFG_SAVE) {
            ctl_error(c, "cli: the board's config script is being applied");
        } else if (ctl->diff_st != DIFF_IDLE) {
            ctl_error(c, "cli: the core is reading the firmware's diff");
        } else {
            ctl_log(s, "core", "cli: exit (asked by a client; the firmware reboots)");
            fc_cli_line(s, "exit");
        }
    } else if (!strcmp(type, "cli")) {
        const char *t = qdict_get_try_str(d, "text");
        if (!t) {
            ctl_error(c, "cli: need text");
        } else if (s->power_off) {
            ctl_error(c, "cli: the board is powered off");
        } else if (c->ctl->cfg >= CFG_ENTER && c->ctl->cfg <= CFG_SAVE) {
            ctl_error(c, "cli: the board's config script is being applied");
        } else if (c->ctl->diff_st != DIFF_IDLE) {
            ctl_error(c, "cli: the core is reading the firmware's diff");
        } else {
            const char *e = ctl_cli_by(s, t, "client");
            if (e) {
                ctl_error(c, e);
            }
        }
    } else if (!strcmp(type, "firmware")) {
        const char *path = qdict_get_try_str(d, "path");
        bool watch = qdict_get_try_bool(d, "watch", false);
        if (!path || !*path) {
            path = s->firmware_path;
        }
        if (!path) {
            ctl_error(c, "firmware: need path");
        } else if (qdict_haskey(d, "path") && !ctl_load_firmware(s, path, &err)) {
            ctl_log(s, "core", "firmware: %s", error_get_pretty(err));
            ctl_error(c, error_get_pretty(err));
            error_free(err);
        } else {
            ctl_set_watch(s, path, watch);
        }
    } else if (!strcmp(type, "power")) {
        bool on;
        if (!ctl_opt_bool(d, "on", true, &on)) {
            ctl_error(c, "power: on must be true|false");
        } else if (on == !s->power_off) {
            ctl_log(s, "core", "power: already %s", on ? "on" : "off");
        } else {
            s->power_off = !on;
            ctl_log(s, "core", "power %s: MCU %s", on ? "on" : "off",
                    on ? "released from reset, boots from its flash"
                       : "held in reset");
            ctl_event(s, "power", on ? "\"on\":true" : "\"on\":false");
            ctl_request_reset(s);
        }
    } else if (!strcmp(type, "subscribe")) {
        ctl_cmd_subscribe(c, d);
    } else if (!strcmp(type, "prof")) {
        ctl_cmd_prof(c, d);
    } else if (!strcmp(type, "env")) {
        if (!s->env) {
            ctl_error(c, "env: the core runs no FDM");
        } else if (!env_command(s, d, &err)) {
            ctl_error(c, error_get_pretty(err));
            error_free(err);
        }
    } else if (!strcmp(type, "launch") || !strcmp(type, "constraint")) {
        fb_sc_launch_t lo;
        if (!s->env || (!strcmp(type, "launch") && s->fdm_kind != FB_FDM_PLANE)) {
            g_autofree char *m = g_strdup_printf("%s: the aircraft has no plane FDM", type);
            ctl_error(c, m);
        } else if (!strcmp(type, "launch") ? !env_launch_parse(d, &lo, &err)
                                           : !env_constraint(s, d, &err)) {
            ctl_error(c, error_get_pretty(err));
            error_free(err);
        } else if (!strcmp(type, "launch")) {
            env_launch(s, &lo);
        }
    } else if (!strcmp(type, "modes")) {
        if (s->power_off) {
            modes_error(c, "power_off");
        } else if (ctl->cli || ctl->cli_entering) {
            modes_error(c, "cli");      /* no MSP while the firmware is in its CLI */
        } else {
            ctl->modes_want = ctl->modes_names = true;  /* the subscribers and the askers get it */
            c->modes_ask = true;
        }
    } else if (!strcmp(type, "diff")) {
        bool refresh;
        const char *against = qdict_get_try_str(d, "against");
        if (!ctl_opt_bool(d, "refresh", true, &refresh)) {
            ctl_error(c, "diff: refresh must be true|false");
        } else if (qdict_haskey(d, "against") && g_strcmp0(against, "script")) {
            ctl_error(c, "diff: against must be \"script\"");
        } else if (against && !refresh) {
            diff_script_send(s, c, ctl->diff_text ? NULL : "none");
        } else if (against) {
            const char *e = diff_request(s);
            if (e) {
                ctl_log(s, "core", "diff: not refreshed: %s", e);
                diff_script_send(s, c, e);
            } else {
                c->diff_script = true;  /* the comparison once `diff all` ends */
            }
        } else if (!refresh) {
            diff_send(s, c, ctl->diff_text ? NULL : "none", false);
        } else {
            const char *e = diff_request(s);
            if (e) {
                ctl_log(s, "core", "diff: not refreshed: %s", e);
                diff_send(s, c, e, false);
            }
        }
    } else if (!strcmp(type, "function")) {
        ctl_cmd_function(c, d);
    } else if (!strcmp(type, "config")) {
        ctl_cmd_config(c, d);
    } else if (!strcmp(type, "hello") || !strcmp(type, "ping")) {
        g_autoptr(GString) g = g_string_new("{\"type\":\"pong\",\"t\":");
        js_time(g, vnow_s());
        g_string_append(g, "}\n");
        ctl_send(c, g, false);
    } else {
        ctl_error(c, "unknown type");
    }
    qobject_unref(o);
}

static gboolean ctl_client_in_body(QIOChannel *ioc, GIOCondition cond, gpointer opaque);

static gboolean ctl_client_in(QIOChannel *ioc, GIOCondition cond, gpointer opaque)
{
    PROF_ENTER(pa.ctl_rx);
    gboolean r = ctl_client_in_body(ioc, cond, opaque);
    PROF_LEAVE(pa.ctl_rx);
    return r;
}

static gboolean ctl_client_in_body(QIOChannel *ioc, GIOCondition cond, gpointer opaque)
{
    CtlClient *c = opaque;
    char buf[4096];

    for (;;) {
        ssize_t n = qio_channel_read(ioc, buf, sizeof(buf), NULL);
        if (n == QIO_CHANNEL_ERR_BLOCK) {
            break;
        }
        if (n <= 0) {
            c->in_src = 0;
            ctl_client_kill(c);
            return G_SOURCE_REMOVE;
        }
        PROF_BYTES(pa.ctl_rx, n);
        for (ssize_t i = 0; i < n && !c->dead; i++) {
            if (buf[i] == '\n') {
                if (c->in->len) {
                    ctl_line(c, c->in->str);
                }
                g_string_truncate(c->in, 0);
            } else if (buf[i] != '\r' && c->in->len < CTL_LINE_MAX) {
                g_string_append_c(c->in, buf[i]);
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

/* A new client: what happened before it connected and is still true — the config flow's state
 * (a snapshot restore happens at t = 0, before anyone connects), the last function progress, the
 * diff — each marked "replay":true (the modes: on `subscribe` modes:true, modes_subscribed) */
static void ctl_replay(CtlClient *c)
{
    NxCtl *ctl = c->ctl;
    NexusXRState *s = ctl->s;
    if (ctl->cfg_last) {
        g_autoptr(GString) g = g_string_new("{\"type\":\"event\",\"t\":");
        js_time(g, vnow_s());
        g_string_append_printf(g, ",\"name\":\"config\",\"data\":{%s,\"replay\":true}}\n",
                               ctl->cfg_last);
        ctl_send(c, g, false);
    }
    if (ctl->fn_last) {
        g_autoptr(GString) g = g_string_new(ctl->fn_last);
        g_string_append(g, ",\"replay\":true}\n");
        ctl_send(c, g, false);
    }
    if (ctl->diff_text) {
        diff_send(s, c, NULL, true);
    }
}

static void ctl_send_hello_body(CtlClient *c);

static void ctl_send_hello(CtlClient *c)
{
    PROF_ENTER(pa.ctl_hello);
    ctl_send_hello_body(c);
    PROF_LEAVE(pa.ctl_hello);
}

static void ctl_send_hello_body(CtlClient *c)
{
    NexusXRState *s = c->ctl->s;
    g_autofree char *fw = fw_describe(s);
    g_autoptr(GString) g = g_string_new("{\"type\":\"hello\",\"core\":\"flightbench-qemu\","
                                        "\"version\":\"0.1\",\"board\":");
    js_str(g, s->board->machine);
    g_string_append(g, ",\"firmware\":");
    js_str(g, fw);
    g_string_append(g, ",\"aircraft\":");
    js_str(g, s->aircraft ? s->aircraft : "rs7");
    g_string_append(g, ",\"actuators\":[");
    for (int i = 0; i <= s->board->nservo; i++) {
        if (i) {
            g_string_append_c(g, ',');
        }
        js_str(g, ctl_actuator(s, i));
    }
    g_string_append_printf(g, "],\"state_hz\":%d,\"t\":", CTL_STATE_HZ);
    js_time(g, vnow_s());
    g_string_append_printf(g, ",\"fdm\":%s,\"power\":%s,\"paused\":%s",
                           s->fdm_on ? "true" : "false",
                           s->power_off ? "false" : "true",
                           c->ctl->paused ? "true" : "false");
    ctl_osd_info(s, g);
    g_string_append(g, "}\n");
    ctl_send(c, g, false);

    if (c->ctl->manifest) {
        g_autoptr(GString) mg = g_string_new(c->ctl->manifest);
        ctl_send(c, mg, false);
    }

    g_autoptr(GString) sg = g_string_new("{\"type\":\"signals\",\"items\":[");
    for (int i = 0; i < SIG_COUNT; i++) {
        g_string_append(sg, i ? ",{\"id\":" : "{\"id\":");
        js_str(sg, sig_id[i]);
        g_string_append(sg, ",\"unit\":");
        js_str(sg, sig_unit[i]);
        g_string_append_c(sg, '}');
    }
    g_string_append(sg, "]}\n");
    ctl_send(c, sg, false);
    ctl_replay(c);
}

static void ctl_accept(QIONetListener *l, QIOChannelSocket *sioc, gpointer opaque)
{
    NxCtl *ctl = opaque;
    CtlClient *c = g_new0(CtlClient, 1);

    c->ctl = ctl;
    c->sioc = sioc;
    object_ref(OBJECT(sioc));
    c->in = g_string_new(NULL);
    c->out = g_string_new(NULL);
    c->period_ns = NANOSECONDS_PER_SECOND / CTL_STATE_HZ;
    c->next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    c->log = true;
    c->osd = true;
    qio_channel_set_blocking(QIO_CHANNEL(sioc), false, NULL);
    qio_channel_set_delay(QIO_CHANNEL(sioc), false);
    if (sioc->remoteAddr.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&sioc->remoteAddr;
        snprintf(c->name, sizeof(c->name), "%s:%u", inet_ntoa(a->sin_addr),
                 ntohs(a->sin_port));
    } else {
        g_strlcpy(c->name, "client", sizeof(c->name));
    }
    g_ptr_array_add(ctl->clients, c);
    c->in_src = qio_channel_add_watch(QIO_CHANNEL(sioc), G_IO_IN | G_IO_HUP | G_IO_ERR,
                                      ctl_client_in, c, NULL);
    ctl_log(ctl->s, "core", "client %s connected (%u total)", c->name,
            ctl->clients->len);
    ctl_send_hello(c);
    ctl_osd_sync_client(c);
}

static void ctl_reap(void *opaque)
{
    NxCtl *ctl = opaque;
    for (guint i = 0; i < ctl->clients->len;) {
        CtlClient *c = g_ptr_array_index(ctl->clients, i);
        if (!c->dead) {
            i++;
            continue;
        }
        g_ptr_array_remove_index(ctl->clients, i);
        if (c->in_src) {
            g_source_remove(c->in_src);
        }
        if (c->out_src) {
            g_source_remove(c->out_src);
        }
        qio_channel_close(QIO_CHANNEL(c->sioc), NULL);
        object_unref(OBJECT(c->sioc));
        fprintf(stderr, "fbctl t=%.3f: client %s gone (%" PRIu64
                " state messages dropped for it)\n", vnow_s(), c->name, c->dropped);
        g_string_free(c->in, true);
        g_string_free(c->out, true);
        g_free(c->snap_path);
        g_free(c);
    }
}

/* Called from the reset handler (vCPUs stopped): pending flash work */
static void ctl_reset_flash(NexusXRState *s)
{
    NxCtl *ctl = s->ctl;
    if (!ctl) {
        return;
    }
    if (ctl->fw_pending) {
        fw_install(s, ctl->fw_pending);
        g_free(ctl->fw_pending);
        ctl->fw_pending = NULL;
        ctl->erase_cfg = false;
        s->powered_on = false;          /* a fresh chip: power-on reset flags */
        ctl_event(s, "firmware_loaded", "");
    }
    if (ctl->erase_cfg) {
        flash_erase(s, s->board->cfg_off, s->board->cfg_len);    /* the config sector */
        memory_region_flush_rom_device(&s->flash, s->board->cfg_off, s->board->cfg_len);
        ctl->erase_cfg = false;
    }
    cfg_at_reset(s);
}

static void ctl_init(NexusXRState *s)
{
    NxCtl *ctl = g_new0(NxCtl, 1);
    Error *err = NULL;

    ctl->s = s;
    ctl->clients = g_ptr_array_new();
    ctl->reap_bh = qemu_bh_new(ctl_reap, ctl);
    ctl->line = g_string_new(NULL);
    ctl->diff_buf = g_string_new(NULL);
    ctl->fn_cur = -1;
    ctl->bs_fn = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    ctl->bs_extra = g_string_new(NULL);
    ctl->bs_bh = qemu_bh_new(bs_bh, s);
    ctl->modes_dirty = true;            /* nothing read yet; nobody subscribed yet: no read */
    g_queue_init(&ctl->cli_q);
    ctl->speed = 1.0;
    ctl->realtime = s->realtime;
    ctl->host_start = g_get_monotonic_time() * 1000;
    ctl->step_timer = nx_timer_new("ctl.step", ctl_step_done, ctl);
    ctl->next_poll_ns = 500 * SCALE_MS;
    ctl->host_ns = INT64_MIN / 2;
    ctl->prof_next = (int64_t)s->prof_period_s * NANOSECONDS_PER_SECOND;
    ctl_rt_anchor(ctl);
    s->ctl = ctl;

    if (s->config_cli) {
        if (!s->board->cli_config) {
            error_report("%s: a CLI config script, but this board is configured by a custom-defaults "
                         "blob", fb_mname);
            exit(1);
        }
        if (!cfg_load(s, &err)) {
            error_reportf_err(err, "%s: ", fb_mname);
            exit(1);
        }
    } else if (s->bench_state && *s->bench_state && !s->board->cli_config) {
        info_report("%s: bench-state=%s: this board is configured by a custom-defaults blob, not a CLI "
                    "script; nothing is kept", fb_mname, s->bench_state);
    }

    if (s->manifest_path && *s->manifest_path) {
        gchar *m = NULL;
        GError *ge = NULL;
        if (!g_file_get_contents(s->manifest_path, &m, NULL, &ge)) {
            warn_report("%s: manifest %s: %s", fb_mname, s->manifest_path, ge->message);
            g_error_free(ge);
        } else {
            g_strstrip(m);          /* one line; the newline is the frame end */
            ctl->manifest = g_strdup_printf("%s\n", m);
            g_free(m);
        }
    } else if (s->manifest_text) {
        ctl->manifest = g_strdup(s->manifest_text);   /* the bench's: one line with "\n" */
    }

    if (s->ctl_addr && *s->ctl_addr) {
        SocketAddress *addr = socket_parse(s->ctl_addr, &err);
        ctl->listener = qio_net_listener_new();
        qio_net_listener_set_name(ctl->listener, "fbctl");
        if (!addr || qio_net_listener_open_sync(ctl->listener, addr, 4, &err) < 0) {
            error_reportf_err(err, "%s: control server on %s: ", fb_mname, s->ctl_addr);
            exit(1);
        }
        qapi_free_SocketAddress(addr);
        qio_net_listener_set_client_func(ctl->listener, ctl_accept, ctl, NULL);
        fprintf(stderr, "fbctl: control server on %s\n", s->ctl_addr);
    }
}
