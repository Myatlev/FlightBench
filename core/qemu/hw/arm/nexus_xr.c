/*
 * FlightBench: STM32F722 (RadioMaster Nexus-XR) machine for QEMU.
 *
 * Goal: boot an unmodified RotorFlight STM32F7X2_OSD build to its scheduler
 * and talk to its CLI/MSP over USART1. Not a full SoC model:
 *
 *   - memory map: ITCM RAM 16K @0, flash 512K @0x08000000 (aliased at
 *     0x00200000, the ITCM flash interface), DTCM+SRAM1+SRAM2 256K
 *     @0x20000000, system memory/OTP 1M @0x1FF00000 (holds UID + flash size)
 *   - flash is writable through the FLASH controller: sector erase fills
 *     0xFF, programming (CR.PG) clears bits like real NOR flash
 *   - DWT CYCCNT counts the core clock derived from QEMU's virtual clock
 *   - USART1..6, UART7/8: QEMU's stm32l4x5 USART (same register layout as F7)
 *   - RCC/PWR: register storage with the "ready" bits following the "on" bits
 *   - ADC: "conversion done" always, plausible Vrefint/temperature codes
 *   - GPIO A..D (levels, BSRR), SYSCFG EXTI routing + EXTI (IRQs 6-10/23/40)
 *   - SPI1 + the DMA2 streams it uses, with the ICM-42688-P IMU model from
 *     devices/ (CS PA4, INT1 PB8) sampled at its ODR in virtual time, fed
 *     by a noisy "hover" motion source settable over the "fbmotion" chardev
 *   - I2C3 (master, the HAL's polled + interrupt paths) with the DPS310/SPL06
 *     baro model from devices/ on it, fed with the FDM altitude; on the F405 board
 *     I2C1 (the v1 IP) with the SPL06 and a wired QMC5883L compass
 *   - F405 board: a u-blox GPS on a USART (in-process wire), ADC1 + DMA2 stream 0
 *     with the battery model on the VBAT / CURRENT channels
 *   - everything else in the peripheral space: permissive storage stubs,
 *     reads return what was written (0 after reset), first access to each
 *     1K block is logged once (LOG_UNIMP, enable with -d unimp)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/host-utils.h"
#include "qemu/error-report.h"
#include "hw/core/irq.h"
#include "hw/core/cpu.h"
#include "exec/icount.h"
#include "exec/replay-core.h"
#include "target/arm/cpu.h"
#include <math.h>
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "system/cpu-timers.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "system/reset.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/clock.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "target/arm/arm-powerctl.h"
#include "hw/char/stm32l4x5_usart.h"
#include "hw/char/stm32f2xx_usart.h"
#include "hw/core/sysbus.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "qobject/qlist.h"
#include "qobject/qnum.h"
#include "qobject/qbool.h"
#include "fb-devices/fb/icm42688p.h"
#include "fb-devices/fb/dps310.h"
#include "fb-devices/fb/spl06.h"
#include "fb-devices/fb/qmc5883l.h"
#include "fb-devices/fb/ublox.h"
#include "fb-devices/fb/battery.h"
#include "fb-devices/fb/dshot.h"
#include "fb-devices/fb/esc.h"
#include "fb-devices/fb/crsf.h"
#include "fb-devices/fb/msp_osd.h"
#include "fb-devices/fb/max7456.h"
#include "fb-devices/fb/fdm.h"
#include "fb-devices/fb/plane.h"
#include "fb-devices/fb/plane_scenario.h"
#include "fb-devices/fb/device.h"
#include "fb-devices/fb/fw_image.h"
#include "fb-devices/fb/json.h"
#include "fb-devices/fb/session.h"
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif
#include "qemu/prof-hooks.h"
#include "fb_build.h"

/* host-time ledger (prof=on): rdtsc-class ticks, calibrated once against the monotonic clock */
#define FBP_TICKS()     cpu_get_host_ticks()
#define FBP_MONO_NS()   get_clock()
#include "fb_prof.c.inc"

/* SoC and board descriptions (s->soc, s->board) */
#include "stm32_soc.c.inc"
#include "fb_outputs.c.inc"

/* Permissive stub windows (storage-backed, lowest priority); the same on every STM32F4/F7. */
#define APB_AHB1_BASE   0x40000000
#define APB_AHB1_SIZE   0x00080000
#define AHB2_BASE       0x50000000
#define AHB2_SIZE       0x00070000
#define FMC_BASE        0xA0000000
#define FMC_SIZE        0x00002000

/* Modelled registers inside the stub windows (absolute addresses, the same on F4 and F7). */
#define RCC_BASE        0x40023800
#define FLASHC_BASE     0x40023C00
#define PWR_BASE        0x40007000
#define ADC_BASE        0x40012000
#define SYSCFG_BASE     0x40013800
#define RTC_BASE        0x40002800
#define BKPSRAM_BASE    0x40024000

#define MAX_SPIS        4           /* SPIs with nothing on the bus (SPI1 is modelled, see below) */
#define MAX_USARTS      8

#define SPI1_BASE       0x40013000
#define EXTI_BASE       0x40013C00
#define GPIO_BASE       0x40020000
#define NUM_GPIO        4           /* A..D modelled, E..K are stubs */
#define DMA2_BASE       0x40026400

enum { GPIO_MODER, GPIO_OTYPER, GPIO_OSPEEDR, GPIO_PUPDR, GPIO_IDR, GPIO_ODR,
       GPIO_BSRR, GPIO_LCKR, GPIO_AFRL, GPIO_AFRH, GPIO_NREGS };

/* Timers modelled beyond storage: the board outputs' (soc->tim), each counting at the timer
 * kernel clock of its bus (soc->tim_clk_hz). */
#define MAX_TIMS        8
enum { TIM_CR1 = 0x00, TIM_DIER = 0x0C, TIM_SR = 0x10, TIM_EGR = 0x14,
       TIM_CCMR1 = 0x18, TIM_CCMR2 = 0x1C, TIM_CCER = 0x20, TIM_CNT = 0x24,
       TIM_PSC = 0x28, TIM_ARR = 0x2C, TIM_CCR1 = 0x34, TIM_BDTR = 0x44, TIM_NREGS = 0x60 / 4 };

#define ESC_REPLY_NS    30000       /* bidir DShot: ESC answers ~30 us after a frame */

#define CRSF_BYTE_NS    (10 * NANOSECONDS_PER_SECOND / 420000)
/* MSP DisplayPort OSD device (HD VTX): 115200 baud, HDZero canvas */
#define OSD_BYTE_NS     (10 * NANOSECONDS_PER_SECOND / 115200)
#define OSD_ROWS        18
#define OSD_COLS        50
#define PILOT_QUEUE     512
#define PILOT_AT_MAX_S  1000000         /* latest "at" accepted, virtual seconds */
#define FDM_DT_NS       1000000     /* 1 kHz physics tick */

#define DWT_BASE        0xE0001000
#define DBGMCU_BASE     0xE0042000

#define TYPE_FB_STM32_MACHINE "fb-stm32-machine"

typedef struct StubWindow {
    MemoryRegion mr;
    hwaddr base;
    uint32_t size;
    uint8_t *regs;
    unsigned long *seen;        /* one bit per 1K block */
    struct NexusXRState *s;
} StubWindow;

typedef struct DmaStream {
    uint32_t cr, ndtr, par, m0ar, m1ar, fcr;
    bool busy;                  /* transfer done, completion pending */
    QEMUTimer *timer;
    struct NexusXRState *s;
} DmaStream;

/*
 * Output towards a socket client (MSP host, pilot, motion): never blocks the emulator. Bytes go
 * out with non-blocking writes; what the socket does not take waits in a bounded ring flushed by
 * a G_IO_OUT watch; beyond the ring the output is dropped and counted (one log line when it
 * starts, one with the count when the client catches up). No client connected: dropped silently.
 * Two drop modes: a byte stream (the USART host socket: the firmware's bytes, cut where the ring
 * ends, like a UART overrun) or whole messages (the line protocols, pilot and motion: a reply
 * that does not fit is dropped entirely, so the client never sees half a line glued to the next).
 */
typedef struct OutQ {
    CharFrontend *fe;
    const char *name;
    uint8_t *buf;
    unsigned cap, head, len;
    guint watch;
    bool whole;                 /* all or nothing per outq_write (line protocols) */
    uint64_t dropped;           /* bytes, total */
    uint64_t burst;             /* bytes dropped since the last log line */
    uint16_t acct;              /* prof: socket writes */
} OutQ;

/* I2C controller core (emulator-independent, host-tested by core/qemu/tests/test_i2c.c) */
#include "stm32_i2c.c.inc"

/* ... and its QEMU side: MMIO region, virtual-clock timer, EV/ER lines */
typedef struct I2cPort {
    MemoryRegion mr;
    QEMUTimer *timer;
    qemu_irq ev_irq, er_irq;
    I2cCtl c;
    struct NexusXRState *s;
} I2cPort;

/* the F4 I2C (v1) controller core (host-tested by core/qemu/tests/test_i2c_v1.c) ... */
#include "stm32_i2c_v1.c.inc"

/* ... and its QEMU side */
typedef struct I2cV1Port {
    MemoryRegion mr;
    QEMUTimer *timer;
    qemu_irq ev_irq, er_irq;
    I2cV1Ctl c;
    struct NexusXRState *s;
} I2cV1Port;

typedef struct SpiPort {
    MemoryRegion mr;
    int index;                  /* 0 = SPI1, 1 = SPI2 */
    uint32_t cr1, cr2, crcpr;
    uint8_t rx[4];              /* F7: RX FIFO; F4: the RX buffer (one frame, rx_n = 0/1) */
    unsigned rx_n;
    uint16_t rx16;              /* F4: the received frame (8 or 16 bit) */
    bool ovr;
    struct NexusXRState *s;
} SpiPort;

typedef struct MotionSource {
    float gyro_dps[3];          /* chip frame */
    float accel_g[3];
    float temp_c;
    float gyro_noise_dps;       /* Gaussian RMS per sample */
    float accel_noise_g;
    float accel_offset_g;       /* static error (-M imu-accel-error): the chip reads */
    float accel_gain;           /* accel * gain + offset (1 and 0: none) */
    bool accel_error;
    float temp_swing_c;         /* sinusoidal drift amplitude */
    float temp_period_s;
} MotionSource;

typedef struct NexusXRState {
    MachineState parent;
    const FbSoc *soc;           /* the chip (stm32_soc.c.inc) */
    const FbBoard *board;       /* the board on it */

    ARMv7MState armv7m;
    Clock *sysclk;
    Clock *refclk;
    Clock *pclk1;
    Clock *pclk2;

    MemoryRegion ram_low;       /* F7 ITCM RAM */
    MemoryRegion flash;
    MemoryRegion flash_alias;
    MemoryRegion ram[FB_SOC_MAX_RAM];
    MemoryRegion sysmem;
    MemoryRegion dwt;
    MemoryRegion dbgmcu;

    StubWindow apb;
    StubWindow ahb2;
    StubWindow fmc;

    Stm32l4x5UsartBaseState usart[MAX_USARTS];     /* FB_USART_L4X5 */
    STM32F2XXUsartState usart_f2[MAX_USARTS];     /* FB_USART_F2XX */

    /* FLASH controller */
    uint32_t flash_cr;
    int flash_key_state;
    bool cfg_prog;              /* the firmware erased/programmed its config sector since the unlock */

    /* DWT */
    uint32_t dwt_ctrl;
    uint32_t dwt_cyccnt_base;   /* value at dwt_cyccnt_t0 */
    int64_t dwt_cyccnt_t0;      /* virtual ns */
    uint32_t dbgmcu_cr;

    /* SPI with nothing on the bus: one byte in flight, MISO reads 0xFF */
    bool spi_rxne[MAX_SPIS];

    /* GPIO A..D */
    MemoryRegion gpio_mr;
    uint32_t gpio[NUM_GPIO][GPIO_NREGS];
    uint16_t gpio_in[NUM_GPIO];         /* levels driven from outside */
    uint16_t gpio_level[NUM_GPIO];      /* last pin levels (edge detect) */

    /* EXTI */
    MemoryRegion exti_mr;
    uint32_t exti_imr, exti_emr, exti_rtsr, exti_ftsr, exti_pr;
    qemu_irq exti_irq[7];

    /* SPI1 */
    /* SPI1 (IMU), SPI2 (F4 boards: the MAX7456) */
    SpiPort spi[2];
    fb_max7456_t *osd_chip;     /* the board's MAX7456 (board->osd_cs_port >= 0) */
    bool osd_chip_cs;

    /* DMA2 */
    MemoryRegion dma2_mr;
    uint32_t dma2_isr[2];
    DmaStream dma2[8];
    qemu_irq dma2_irq[8];

    /* IMU and its motion source */
    fb_icm42688p_t *imu;
    bool imu_present;           /* -M nexus-xr,imu=off: empty SPI1 (A/B runs) */
    bool imu_cs;                /* asserted */
    bool imu_pulse;             /* INT1 pulse in progress */
    uint32_t imu_odr;
    int64_t imu_next_ns;
    uint64_t imu_samples;
    QEMUTimer *imu_sample_timer;
    QEMUTimer *imu_pulse_timer;
    char *imu_log_path;         /* -M imu-log=<file>: per sample INT1 rise/fall, SPI DMA done */
    FILE *imu_log;
    MotionSource motion;
    uint64_t rng;
    double gauss_next;
    bool gauss_have;
    CharFrontend motion_chr;
    OutQ motion_out;
    char motion_buf[512];
    unsigned motion_len;

    /* I2C3 and the baro on it (-M baro=on|off) */
    I2cPort i2c3;               /* FB_I2C_V2 */
    I2cV1Port i2c1;             /* FB_I2C_V1 */
    fb_dps310_t *baro;          /* the Nexus-XR's (board->baro_spl06 false) */
    fb_spl06_t *spl06;          /* board->baro_spl06: the chip on the bus instead */
    QEMUTimer *spl06_timer;     /* its next conversion (fb_spl06_next_due_ns) */
    int64_t spl06_last_ns;      /* its clock, virtual ns */
    char *baro_props_str;       /* -M baro-props=noise_scale=<k> (the spl06's props) */
    bool baro_present;
    uint32_t baro_rate;
    bool baro_oneshot;          /* a one-shot measurement is converting (timer armed) */
    int64_t baro_next_ns;
    uint64_t baro_samples;
    QEMUTimer *baro_timer;
    char *baro_env_str;         /* "p0_pa:field_m:t0_c" (-M baro-env=) */
    double baro_p0, baro_field_m, baro_t0;
    double baro_pa, baro_c;     /* what the chip was given last */

    /* QMC5883L compass on the I2C controller (board->mag, -M mag=on|off, mag-props=) */
    fb_qmc5883l_t *mag;
    bool mag_present;
    char *mag_props_str;
    int mag_mount;              /* FB_MAG_CW*: how the chip sits (the firmware's align_mag) */
    double mag_ned[3];          /* the earth field, gauss, NED */
    double mag_chip[3];         /* what the chip was given last, gauss, its axes */
    int64_t mag_last_ns;        /* its clock, virtual ns */

    /* u-blox GPS on a USART (-M gps=uartN|off, gps-props=) */
    char *gps_str, *gps_props_str;
    int gps_uart;               /* USART index, -1 = none */
    Chardev *gps_wire;
    fb_ublox_t *gps;
    QEMUTimer *gps_timer;       /* the next epoch, or the next byte on the wire */
    int64_t gps_last_ns;        /* the receiver's clock, virtual ns */
    double home[3];             /* lat, lon, alt MSL of the FDM's origin (env home) */

    /* ADC1 + DMA2 stream 0 (board->adc) and the battery on VBAT / CURRENT (-M battery=on|off) */
    MemoryRegion adc_mr;
    uint32_t adc_reg[0x400 / 4];
    bool adc_running;           /* SWSTART given, continuous conversions */
    uint16_t adc_code[19];      /* per channel, what the pins read now */
    bool adc_dirty;             /* the DMA buffer is not what the codes say */
    uint64_t adc_fills;         /* DMA buffer rewrites */
    fb_battery_t *batt;
    bool battery_present;
    char *battery_props_str;

    /* Timers */
    struct NxTimer {
        MemoryRegion mr;
        uint32_t r[TIM_NREGS];
        uint32_t cnt0;          /* counter value at t0 */
        int64_t t0;
        struct NexusXRState *s;
    } tim[MAX_TIMS];

    /* DShot: motor 1 on TIM1_CH2 via a DMA2 stream */
    QEMUTimer *esc_reply_timer;
    int esc_reply_stream;
    bool esc_reply_armed;       /* a bidir frame went out, reply not sent yet */
    uint32_t dshot_period;      /* ARR + 1 of the last output frame */
    uint16_t dshot_value;
    bool dshot_bidir;
    uint64_t dshot_frames, dshot_bad, esc_replies;
    fb_esc_t *esc;
    uint32_t esc_timeout_ms;    /* esc-timeout-ms: no DShot frame this long -> motor off */
    bool esc_present;           /* -M esc=on|off: an ESC on MOTOR 1 (wiring) */

    /* CRSF receiver on a USART (-M crsf=uartN|off, default uart5), pilot socket */
    char *crsf_str;
    int crsf_uart;              /* USART index, -1 = no receiver wired */
    Chardev *crsf_wire;
    fb_crsf_t *crsf;
    QEMUTimer *crsf_timer;
    int64_t crsf_last_ns;
    int64_t crsf_frame_ns;      /* when the frame being delivered started on the wire */
    unsigned crsf_fed;          /* its bytes released into the USART so far */
    struct CrsfUsart *crsf_usart;
    uint64_t crsf_dropped;      /* receiver bytes the FC never read (at the next frame; USART off: at once) */
    /* MSP DisplayPort OSD device (HD VTX) on a USART (-M osd=uartN|off, default off) */
    char *osd_str;
    char *osd_log_path;         /* -M osd-log=: the FC's bytes to the device, raw (replay tests) */
    char *osd_poll_str;         /* -M osd-poll=on|off, default the board's (FbBoard.osd_poll) */
    bool osd_poll_on;
    FILE *osd_log;
    int osd_uart;               /* USART index, -1 = none wired */
    Chardev *osd_wire;
    fb_msp_osd_t *osd;
    QEMUTimer *osd_timer;       /* device -> FC bytes, one per 115200 byte time */
    QEMUTimer *osd_watch;       /* fires after fb_msp_osd_silence_ns() without bytes: contact loss */
    QEMUTimer *osd_poll;        /* the device's MSP_FC_VARIANT requests (fb_msp_osd_poll_in_ns()) */
    int64_t osd_last_ns;

    uint16_t pilot_ch[FB_CRSF_CHANNELS];
    struct PilotCmd {
        int64_t at_ns;
        uint16_t ch[FB_CRSF_CHANNELS];
        uint32_t ch_mask;
        int link;               /* -1 unchanged, 0 lost, 1 up */
        bool launch;            /* {"type":"launch",...}: a plane launch with these overrides */
        fb_sc_launch_t lp;
    } pilot_q[PILOT_QUEUE];
    unsigned pilot_n;
    CharFrontend pilot_chr;
    OutQ pilot_out;
    char pilot_buf[1024];
    unsigned pilot_len;

    /* FDM */
    bool fdm_on;
    char *fdm_log_path;
    uint32_t fdm_log_ms;
    FILE *fdm_log;
    fb_fdm_t *fdm;              /* the aircraft's flight dynamics model (fdm/include/fb/fdm.h) */
    fb_fdm_kind_t fdm_kind;     /* the aircraft file's "fdm": heli | plane */
    char *fdm_params_json;      /* the aircraft file's "fdm_params" as text (NULL = defaults) */
    QEMUTimer *fdm_timer;
    int64_t fdm_next_ns;
    uint64_t fdm_steps;
    uint16_t servo_us[FB_MAX_SERVOS];   /* firmware output on SERVO 1..n (last valid pulse) */
    uint32_t servo_sig;         /* bit i: SERVO i+1 had a valid pulse at the last physics tick */
    uint16_t fdm_in_us[4];      /* what the FDM got last (after the wiring map) */
    /* aircraft (-M aircraft-file=models/<id>/aircraft.json): transmission, ESC, FDM, and the
     * FDM's inputs by actuator name; the wiring map: FDM input k <- servo output fdm_src[k]
     * (-1 = nothing wired to that actuator: 1500 us, the FDM's neutral), mirrored around the
     * input's centre_us when that output is reversed */
    char *aircraft_file;
    double main_gear;           /* motor rpm / head rpm */
    char *fdm_in_names[4];
    int fdm_src[4];
    uint16_t fdm_centre_us[4];  /* aircraft.json centre_us per FDM input (default 1500) */
    uint32_t servo_mask;        /* -M servos=<mask>: bit i = a servo on SERVO i+1 */
    /* the plane FDM (fb_fdm.c.inc): with fdm=on and an aircraft file whose "fdm" is "plane";
     * NULL otherwise (the heli FDM) */
    struct FbEnv *env;          /* the environment around the FDM (fb_fdm.c.inc) */
    char *env_default_path;     /* from the aircraft file, until env_init */
    /* the aircraft's environment functions (aircraft.json `functions`, docs/protocol.md
     * `function`): what the core runs on the model and the firmware on command, and the ones
     * with auto_first_start after the config script on a blank config sector */
    struct FbFunction *fn;
    int nfn;
    char *env_scenario;         /* -M env-scenario=<name>: instead of the aircraft's env_default */
    uint64_t motor_pulse_ns;    /* the ESC's last pulse width (pulse outputs, esc_pulse) */

    bool powered_on;
    bool power_off;             /* battery off: MCU held in reset */

    /* IMU input after noise (what the chip was given last) */
    float imu_in_gyro[3], imu_in_accel[3], imu_in_temp;

    /* receiver link as set by `link` / the pilot socket (what the CRSF model reports, with
     * injected faults applied, comes from fb_crsf_link_reported()) */
    bool link_up;
    int link_rssi, link_lq, link_snr;

    /* control server (nexus_xr_ctl.c.inc) and its session options */
    struct NxCtl *ctl;
    struct HostUart *msp_uart;  /* MSP/CLI UART (board->msp_uart), shared with the server */
    char *firmware_path;        /* loaded into flash by the machine */
    char *defaults_path;        /* custom-defaults blob at 0x08002808 */
    char *ctl_addr;             /* "127.0.0.1:31250" */
    char *aircraft;
    char *actuators_str;        /* "swash_a,swash_b,swash_c,tail,motor" */
    char *act_names[FB_MAX_SERVOS + 1];    /* servo1..n, motor1 */
    uint32_t servo_rev;         /* bit i: servo i+1 linkage reversed */
    char *beeper_str;           /* "B02" or "B02:inv" from resource BEEPER */
    char *manifest_path;        /* core manifest JSON (scripts/manifest.py), sent after hello */
    char *imu_noise_str;        /* "gyro_dps:accel_g" noise RMS of the motion source */
    char *imu_accel_error_str;  /* "offset_mg:scale_pct" static accelerometer error */
    int beeper_port, beeper_pin;
    bool beeper_inverted;
    bool realtime;

    /* idle skip ("external WFI", see idle_poll) */
    bool idle_skip;
    bool io_tb;                 /* -M io-tb=on: icount_io_insn_tbs (patches/icount-io-tb.diff) */
    uint32_t idle_skip_n;       /* consecutive reads matching the read p back (~n + p reads) */
    uint32_t idle_skip_gran_us; /* longest skip, us of virtual time */
    int64_t idle_gran_insns;
    char *idle_skip_mode;       /* "budget" (default) or "halt" */
    char *idle_skip_src;        /* "cyccnt" or "systick": which reads the detector watches */
    bool idle_systick;          /* ... resolved (default: board->idle_systick) */
    bool idle_halt_mode;
    bool idle_frozen;           /* machine init done: idle-skip/-mode/-gran-us are fixed */
    int64_t idle_skipped_insns; /* instructions consumed by budget skips */
    uint64_t idle_extended;     /* budget skips that extended the budget: it was cut short by
                                 * a realtime deadline, or by a virtual timer that has since
                                 * been deleted or moved later from the main loop */
    qemu_irq cpu_irq;           /* NVIC -> CPU line, intercepted to see interrupt entry */
    QEMUTimer *idle_timer;
    uint64_t idle_ring[32];     /* IDLE_RING signatures of the last CYCCNT reads */
    unsigned idle_pos;
    uint32_t idle_run[9];       /* [p]: consecutive reads equal to the one p reads back */
    uint64_t idle_lock[8];      /* the idle cycle once found (IDLE_PMAX) */
    unsigned idle_lock_p, idle_lock_ph, idle_lock_m;
    uint64_t idle_nonce;
    int64_t idle_last_ic;
    bool idle_dirty;            /* interrupt or other peripheral access since the last read */
    bool idle_halted;
    int64_t idle_last_from, idle_last_to; /* virtual ns span of the last budget skip */
    int64_t idle_halt_t;
    uint64_t idle_skips;
    int64_t idle_skipped_ns;

    /* host-time ledger (-M prof=on, prof-period-s=N; see "Host-time ledger") */
    bool prof_on;
    uint32_t prof_period_s;

    /* bench (-M session=<bench.json>, root=<dir>, session-check=on; see "Bench") */
    char *session_path;
    char *session_root;
    bool session_check;
    bool session_applying;      /* the bench's options are being set: not "given" */
    GHashTable *given;          /* -M options given explicitly: the bench leaves them */
    uint8_t *defaults_blob;     /* the bench's custom-defaults blob (defaults= overrides it) */
    char *config_cli_path;      /* -M config-cli=<file>: the CLI config script (cli_config boards) */
    char *config_cli;           /* the script's text: that file's, else the bench's */
    char **cfg_prov;            /* per line of config_cli: "layer\tsource" (the bench's; NULL with
                                 * -M config-cli= or a blob board) */
    int cfg_prov_n;
    char *config_json;          /* the bench's layered assembly (fb_session_config_json) */
    char *session_opts;         /* the bench's -M string at start (config apply compares it) */
    char *data_root;            /* the resolver's data root (config snapshot: a relative path) */
    char *fw_hash;              /* SHA-256 of the flash image as installed (the snapshot key) */
    uint8_t *cfg_snap;          /* the config sector saved after the script (board->cfg_len) */
    char *cfg_snap_key;         /* ... for this firmware + script */
    char *cfg_snap_diff;        /* ... the firmware's `diff all` of it (NULL = not captured) */
    char *cfg_cache;            /* -M cfg-cache=<dir>|off: the snapshots on disk, across runs
                                 * (unset: <user cache dir>/flightbench/config) */
    char *bench_state;          /* -M bench-state=<file>: the bench's config sector, written after
                                 * every save of the firmware, restored at start (+ <file>.json) */
    size_t defaults_len;
    char *manifest_text;        /* the bench's core manifest (manifest= overrides it) */
} NexusXRState;

/* every FlightBench STM32 machine is this type's child; the child's class carries its board */
#define TYPE_NEXUS_XR_MACHINE TYPE_FB_STM32_MACHINE
typedef struct FbMachineClass {
    MachineClass parent;
    const FbBoard *board;
} FbMachineClass;
OBJECT_DECLARE_TYPE(NexusXRState, FbMachineClass, NEXUS_XR_MACHINE)

static const char *fb_mname = "nexus-xr";  /* the machine type: log prefix, region names */

static SysBusDevice *usart_sbd(NexusXRState *s, int i)
{
    return s->soc->usart_model == FB_USART_F2XX ? SYS_BUS_DEVICE(&s->usart_f2[i])
                                                : SYS_BUS_DEVICE(&s->usart[i]);
}

/* "<machine>.<what>": region names */
static char *mr_name(const char *what)
{
    return g_strdup_printf("%s.%s", fb_mname, what);
}

/* the same, freed with the machine (for calls that copy it) */
static const char *mr_name_tmp(NexusXRState *s, const char *what)
{
    static GPtrArray *names;
    if (!names) {
        names = g_ptr_array_new();
    }
    char *n = mr_name(what);
    g_ptr_array_add(names, n);
    return n;
}

/* "uartN" -> USART index N-1 when bit N-1 of `allowed` is set, else -1 */
static int uart_opt(const char *c, uint32_t allowed)
{
    if (strncmp(c, "uart", 4) || c[4] < '1' || c[4] > '8' || c[5]) {
        return -1;
    }
    int i = c[4] - '1';
    return allowed >> i & 1 ? i : -1;
}

/* the modelled I2C controller's region */
static MemoryRegion *i2c_mr(NexusXRState *s)
{
    return s->soc->i2c_model == FB_I2C_V1 ? &s->i2c1.mr : &s->i2c3.mr;
}

/* control server hooks (nexus_xr_ctl.c.inc) */
static void ctl_tick(NexusXRState *s, int64_t now);
static void ctl_on_reset(NexusXRState *s, const char *cause);
static void ctl_reset_flash(NexusXRState *s);
static void ctl_fc_tx(NexusXRState *s, const uint8_t *buf, int len);
static void ctl_host_rx(NexusXRState *s, const uint8_t *buf, int len);
static bool ctl_cfg_busy(NexusXRState *s);
static void ctl_cfg_written(NexusXRState *s);  /* the firmware saved its config (flash locked) */
static void ctl_dshot(NexusXRState *s, uint16_t value);
static double ctl_fc_baro_alt(NexusXRState *s);     /* MSP_ALTITUDE in m, NAN = none */
static void ctl_beeper(NexusXRState *s, bool level);
static void ctl_osd_frame(NexusXRState *s, bool full);   /* a DRAW_SCREEN reached the OSD device */
static void ctl_osd_link(NexusXRState *s);  /* the OSD lost its picture (contact, fault, CLI): link:false */
static void ctl_scenario_event(NexusXRState *s, const fb_sc_event_t *e);   /* the core's own flows */

/* an environment function (aircraft.json `functions`, the older `acc_calibration` key = a
 * calibrate_acc with auto_first_start) */
#define FB_FN_MAX 8
typedef struct FbFunction {
    char *name;                 /* calibrate_acc | calibrate_gyro | launch | calibrate_mag */
    char *label;
    bool auto_first;            /* auto_first_start: part of the first start (after the script) */
    char *scenario;             /* a scenario file relative to the aircraft, NULL = none */
    char *scenario_text;        /* ... its text (plane_init; the first-start snapshot key) */
    double hold_s;              /* calibrate_gyro: held still this long before the MCU reset */
    fb_sc_launch_t launch;      /* launch: the catalog's params (NAN / UNSET = not given) */
} FbFunction;
static void ctl_fn_launch(NexusXRState *s, bool release, double speed_mps, double height_m);

/* the environment around the FDM (fb_fdm.c.inc): scenarios, the holder, wind; the control server reads it */
typedef struct FbEnv {
    fb_scenario_t sc;           /* the running env scenario (sc.running) */
    char *dir;                  /* the aircraft file's directory: env/<name>.json */
    char *env_default;          /* aircraft.json env_default (relative to dir), NULL = none */
    char *acc_cal;              /* the calibrate_acc function's scenario (relative to dir), NULL = none */
    bool cmd_hold;              /* a `constraint` hold: its `pose` event is due */
    double wind[3];
    uint64_t launches;
    unsigned sc_gen;            /* scenarios started (env_scenario_run): a flow running its own
                                 * scenario sees it replaced (the ACC calibration) */
} FbEnv;
static char *fdm_params_text(NexusXRState *s, QDict *a);
static char *env_rel_path(NexusXRState *s, QDict *a, const char *key);
static void fdm_params_check(NexusXRState *s, const char *aircraft_file);
static void fdm_model_tick(NexusXRState *s);
static void env_init(NexusXRState *s);
static void fdm_log_header(NexusXRState *s);
static void fdm_snapshot(NexusXRState *s, double *v);
static void fdm_state_json(NexusXRState *s, GString *g);
static bool env_launch_parse(QDict *d, fb_sc_launch_t *o, Error **errp);
static void env_launch(NexusXRState *s, const fb_sc_launch_t *o);
static bool env_constraint(NexusXRState *s, QDict *d, Error **errp);
static bool env_command(NexusXRState *s, QDict *d, Error **errp);
static void env_reset(NexusXRState *s, bool fw);
static void env_default_run(NexusXRState *s);
static bool env_scenario_file(NexusXRState *s, const char *name, fb_scenario_t *sc,
                              char **text_out, Error **errp);
static void env_scenario_run(NexusXRState *s, const fb_scenario_t *sc);

/* ------------------------------------------------------------------ */
/* Host-time ledger: -M nexus-xr,prof=on (fb_prof.c.inc, docs/protocol.md `prof`)            */
/*
 * Where the host's time goes, per run, in accounts: every MMIO region (the wrapped
 * MemoryRegionOps), every timer the machine creates (nx_timer_new), the socket/chardev paths,
 * the control server, the FDM; and, through the hooks of core/qemu/patches/prof-qemu.diff, the
 * vCPU <-> main loop hand-offs: each end of a vCPU slice (tcg_cpu_exec returned) with its reason
 * and, for a timer deadline, which of the machine's timers it was, the latency from the vCPU
 * waking the main loop (aio_notify) to its poll returning, from another thread kicking the vCPU
 * to the vCPU acting on it, contended BQL acquisitions, the main loop's poll and dispatch time.
 * prof=off (default): fbprof stays NULL, nothing is wrapped, no hook is installed.
 *
 * Nothing here changes what the guest sees: wrappers call the same handlers with the same
 * arguments, the hooks only read.
 */
static FbProf *fbprof;              /* NULL = prof=off */
static NexusXRState *prof_s;

static struct {
    uint16_t between, vbql, mbql, halt_wait, poll, dispatch, rt_sleep, idle_skip;
    uint16_t fdm_step, esc_step, crsf_fc_tx, osd_fc_tx, pilot_rx, motion_rx;
    uint16_t ctl_rx, ctl_tx, ctl_cmd, ctl_tick, ctl_state, ctl_samples, ctl_osd, ctl_hello;
    uint16_t ctl_fc_tx, ctl_prof;
} pa;

#define PROF_ENTER(acct) do { if (unlikely(fbprof)) { fbp_enter(fbprof, (acct)); } } while (0)
#define PROF_LEAVE(acct) do { if (unlikely(fbprof)) { fbp_leave(fbprof, (acct)); } } while (0)
#define PROF_BYTES(acct, n) do { if (unlikely(fbprof)) { fbp_bytes(fbprof, (acct), (n)); } } while (0)

static uint16_t prof_acct(const char *name, int kind)
{
    return fbprof ? fbp_acct(fbprof, name, kind) : FBP_NONE;
}

/* ---- timers: one account per name, and the list the deadline attribution looks at ---- */
typedef struct ProfTimer {
    QEMUTimerCB *cb;
    void *opaque;
    uint16_t acct;
    QEMUTimer *t;
} ProfTimer;

#define PROF_MAX_TIMERS 48
static ProfTimer *prof_timers[PROF_MAX_TIMERS];
static unsigned prof_ntimers;

static void prof_timer_cb(void *opaque)
{
    ProfTimer *pt = opaque;
    fbp_enter(fbprof, pt->acct);
    pt->cb(pt->opaque);
    fbp_leave(fbprof, pt->acct);
}

/* every QEMU_CLOCK_VIRTUAL timer of the machine is made here */
static QEMUTimer *nx_timer_new(const char *name, QEMUTimerCB *cb, void *opaque)
{
    if (!fbprof) {
        return timer_new_ns(QEMU_CLOCK_VIRTUAL, cb, opaque);
    }
    ProfTimer *pt = g_new0(ProfTimer, 1);
    pt->cb = cb;
    pt->opaque = opaque;
    pt->acct = fbp_acct(fbprof, name, FBP_TIMER);
    pt->t = timer_new_ns(QEMU_CLOCK_VIRTUAL, prof_timer_cb, pt);
    if (prof_ntimers < PROF_MAX_TIMERS) {
        prof_timers[prof_ntimers++] = pt;
    }
    return pt->t;
}

/* ---- MMIO: the region's ops and opaque are swapped for a wrapper calling the same handlers */
typedef struct ProfMr {
    const MemoryRegionOps *ops;
    void *opaque;
    uint16_t acct;
    MemoryRegionOps w;
} ProfMr;

static MemTxResult prof_mr_read(void *opaque, hwaddr addr, uint64_t *data, unsigned size,
                                MemTxAttrs attrs)
{
    ProfMr *m = opaque;
    MemTxResult r = MEMTX_OK;
    bool timed = fbp_enter_sampled(fbprof, m->acct);
    if (m->ops->read_with_attrs) {
        r = m->ops->read_with_attrs(m->opaque, addr, data, size, attrs);
    } else {
        *data = m->ops->read(m->opaque, addr, size);
    }
    if (timed) {
        fbp_leave(fbprof, m->acct);
    }
    return r;
}

static MemTxResult prof_mr_write(void *opaque, hwaddr addr, uint64_t data, unsigned size,
                                 MemTxAttrs attrs)
{
    ProfMr *m = opaque;
    MemTxResult r = MEMTX_OK;
    bool timed = fbp_enter_sampled(fbprof, m->acct);
    if (m->ops->write_with_attrs) {
        r = m->ops->write_with_attrs(m->opaque, addr, data, size, attrs);
    } else {
        m->ops->write(m->opaque, addr, data, size);
    }
    if (timed) {
        fbp_leave(fbprof, m->acct);
    }
    return r;
}

static bool prof_mr_accepts(void *opaque, hwaddr addr, unsigned size, bool is_write,
                            MemTxAttrs attrs)
{
    ProfMr *m = opaque;
    return m->ops->valid.accepts(m->opaque, addr, size, is_write, attrs);
}

/* sample > 1: time only every sample-th access (a hot region whose handler enters no other
 * account; the count stays exact, the time is scaled) */
static void prof_wrap_mr(MemoryRegion *mr, const char *name, unsigned sample)
{
    ProfMr *m = g_new0(ProfMr, 1);
    m->ops = mr->ops;
    m->opaque = mr->opaque;
    m->acct = fbp_acct(fbprof, name ? name : memory_region_name(mr), FBP_MMIO);
    m->w = *mr->ops;
    m->w.read = NULL;
    m->w.write = NULL;
    m->w.read_with_attrs = mr->ops->read || mr->ops->read_with_attrs ? prof_mr_read : NULL;
    m->w.write_with_attrs = mr->ops->write || mr->ops->write_with_attrs ? prof_mr_write : NULL;
    m->w.valid.accepts = mr->ops->valid.accepts ? prof_mr_accepts : NULL;
    mr->ops = &m->w;
    mr->opaque = m;
    fbp_set_sample(fbprof, m->acct, sample);
}

/* ---- hand-offs (hooks of patches/prof-qemu.diff) ---- */
static uint64_t prof_wake_t;        /* ticks: the vCPU woke the main loop, not yet seen */
static uint64_t prof_kick_t;        /* ticks: another thread kicked the vCPU, not yet acted on */
static bool prof_between;           /* vCPU thread: vcpu.between_slices is open */
static bool prof_rearm;             /* vCPU thread: this slice was cut by a timer it armed */
static bool prof_skip_end;          /* vCPU thread: an idle skip used up the budget */

/* ns since a tick stamp taken on another thread (0 if the TSCs disagree and it looks negative) */
static uint64_t prof_since(uint64_t t)
{
    int64_t d = (int64_t)(cpu_get_host_ticks() - t);
    return d > 0 ? fbp_ns(fbprof, d) : 0;
}

static void prof_kick_seen(void)
{
    uint64_t k = qatomic_read(&prof_kick_t) ? qatomic_xchg(&prof_kick_t, 0) : 0;
    if (k) {
        fbp_hist_add(&fbprof->kick, prof_since(k));
    }
}

static void prof_slice_start(CPUState *cpu)
{
    fbp_thread_set(FBP_T_VCPU);
    prof_kick_seen();
    /* a cpu_exit between slices (icount_notify_aio_contexts -> qemu_timer_notify_cb after every
     * due timer, timer_mod from the timers the vCPU ran) was consumed by the round-robin loop;
     * only one during the slice can end it */
    prof_rearm = false;
    if (prof_between) {
        prof_between = false;
        fbp_leave(fbprof, pa.between);
    }
}

/* after icount_process_data (the virtual clock includes the slice); left = budget not used */
static void prof_slice_end(CPUState *cpu, int ret, int64_t left)
{
    FbProf *p = fbprof;
    int r = FBP_R_OTHER;

    fbp_thread_set(FBP_T_VCPU);
    if (ret == EXCP_HALTED || ret == EXCP_HLT || cpu->halted) {
        r = prof_s->idle_halted ? FBP_R_IDLE_SKIP : FBP_R_HALT;
    } else if (icount_enabled() && left == 0) {
        /* which of the machine's timers is due (the earliest; expire_time read without the list
         * lock: this thread is the only one running them, a stale value only misattributes);
         * none: ask QEMU whether another virtual timer (its own, e.g. SysTick) or a realtime
         * one is due -- two locked list walks, only for those slices */
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        ProfTimer *due = NULL;
        int64_t due_t = INT64_MAX;
        for (unsigned i = 0; i < prof_ntimers; i++) {
            int64_t e = qatomic_read(&prof_timers[i]->t->expire_time);
            if (e >= 0 && e <= now && e < due_t) {
                due = prof_timers[i];
                due_t = e;
            }
        }
        if (due) {
            p->a[due->acct].deadlines++;
            r = prof_skip_end ? FBP_R_IDLE_SKIP : FBP_R_TIMER_DEADLINE;
        } else if (qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL, QEMU_TIMER_ATTR_ALL) == 0) {
            p->not_machine++;
            r = prof_skip_end ? FBP_R_IDLE_SKIP : FBP_R_TIMER_DEADLINE;
        } else if (qemu_clock_deadline_ns_all(QEMU_CLOCK_REALTIME, QEMU_TIMER_ATTR_ALL) == 0) {
            r = FBP_R_TIMER_REALTIME;
        }
    } else if (prof_rearm) {
        r = FBP_R_TIMER_REARM;
    } else if (qatomic_read(&prof_kick_t)) {
        r = FBP_R_IO;
    }
    prof_kick_seen();
    prof_rearm = prof_skip_end = false;
    p->reason[r]++;
    prof_between = true;
    fbp_enter(p, pa.between);
}

static void prof_vcpu_wait(bool begin)
{
    if (begin) {
        fbp_enter(fbprof, pa.halt_wait);
    } else {
        fbp_leave(fbprof, pa.halt_wait);
        prof_kick_seen();
    }
}

static void prof_kick(CPUState *cpu, bool self)
{
    if (!self) {
        qatomic_cmpxchg(&prof_kick_t, 0, cpu_get_host_ticks());
    }
}

static void prof_timer_exit(void)
{
    prof_rearm = true;
}

static void prof_notify(void)
{
    if (fbp_thread() == FBP_T_VCPU) {
        qatomic_cmpxchg(&prof_wake_t, 0, cpu_get_host_ticks());
    }
}

static void prof_poll(bool begin, unsigned nfds)
{
    if (begin) {
        if (fbp_thread() != FBP_T_MAIN) {
            fbp_thread_set(FBP_T_MAIN);
        } else {
            fbp_leave(fbprof, pa.dispatch);
        }
        fbp_bytes(fbprof, pa.poll, nfds);
        fbp_enter(fbprof, pa.poll);
    } else {
        fbp_leave(fbprof, pa.poll);
        uint64_t w = qatomic_xchg(&prof_wake_t, 0);
        if (w) {
            fbp_hist_add(&fbprof->wake, prof_since(w));
        }
        fbp_enter(fbprof, pa.dispatch);
    }
}

static void prof_bql_wait(bool begin)
{
    uint16_t a = fbp_thread() == FBP_T_VCPU ? pa.vbql : pa.mbql;
    if (begin) {
        fbp_enter(fbprof, a);
    } else {
        fbp_leave(fbprof, a);
    }
}

static const QemuProfHooks prof_hooks = {
    .slice_start = prof_slice_start,
    .slice_end = prof_slice_end,
    .vcpu_wait = prof_vcpu_wait,
    .kick = prof_kick,
    .timer_exit = prof_timer_exit,
    .notify = prof_notify,
    .poll = prof_poll,
    .bql_wait = prof_bql_wait,
};

/* at the start of machine init: before the first timer or region is made */
static void prof_init(NexusXRState *s)
{
    if (!s->prof_on) {
        return;
    }
    fbprof = fbp_new(0);
    prof_s = s;
    pa.between = prof_acct("vcpu.between_slices", FBP_HANDOFF);
    pa.vbql = prof_acct("vcpu.bql_wait", FBP_HANDOFF);
    pa.mbql = prof_acct("main.bql_wait", FBP_HANDOFF);
    pa.halt_wait = prof_acct("vcpu.halt_wait", FBP_IDLE);
    pa.rt_sleep = prof_acct("vcpu.realtime_sleep", FBP_IDLE);
    pa.idle_skip = prof_acct("idle.skip", FBP_IDLE);
    pa.poll = prof_acct("mainloop.poll", FBP_MAINLOOP);
    pa.dispatch = prof_acct("mainloop.dispatch", FBP_MAINLOOP);
    pa.fdm_step = prof_acct("fdm.step", FBP_FDM);
    pa.esc_step = prof_acct("esc.step", FBP_FDM);
    pa.crsf_fc_tx = prof_acct("crsf.fc_tx", FBP_CHARDEV);
    pa.osd_fc_tx = prof_acct("osd.fc_tx", FBP_CHARDEV);
    pa.pilot_rx = prof_acct("pilot.rx", FBP_CHARDEV);
    pa.motion_rx = prof_acct("motion.rx", FBP_CHARDEV);
    pa.ctl_rx = prof_acct("ctl.rx", FBP_CHARDEV);
    pa.ctl_tx = prof_acct("ctl.tx", FBP_CHARDEV);
    pa.ctl_cmd = prof_acct("ctl.cmd", FBP_CTL);
    pa.ctl_tick = prof_acct("ctl.tick", FBP_CTL);
    pa.ctl_state = prof_acct("ctl.state_json", FBP_CTL);
    pa.ctl_samples = prof_acct("ctl.samples_json", FBP_CTL);
    pa.ctl_osd = prof_acct("ctl.osd_json", FBP_CTL);
    pa.ctl_hello = prof_acct("ctl.hello", FBP_CTL);
    pa.ctl_fc_tx = prof_acct("ctl.fc_tx", FBP_CTL);
    pa.ctl_prof = prof_acct("ctl.prof_json", FBP_CTL);
}

/* at the end of machine init: every region exists; hooks go live */
static void prof_start(NexusXRState *s)
{
    if (!fbprof) {
        return;
    }
    prof_wrap_mr(&s->flash, NULL, 0);
    prof_wrap_mr(&s->apb.mr, NULL, 0);
    prof_wrap_mr(&s->ahb2.mr, NULL, 0);
    prof_wrap_mr(&s->fmc.mr, NULL, 0);
    /* the three hottest regions (~2 M accesses per virtual s together in the RS7 flight, the
     * firmware's idle scheduler pass): leaf handlers, time every 7th access (prof overhead) */
    prof_wrap_mr(&s->dwt, NULL, 7);
    prof_wrap_mr(&s->dbgmcu, NULL, 0);
    prof_wrap_mr(&s->gpio_mr, NULL, 0);
    prof_wrap_mr(&s->exti_mr, NULL, 0);
    prof_wrap_mr(&s->spi[0].mr, NULL, 0);
    if (s->soc->spi2_base) {
        prof_wrap_mr(&s->spi[1].mr, NULL, 0);
    }
    prof_wrap_mr(&s->dma2_mr, NULL, 0);
    prof_wrap_mr(i2c_mr(s), NULL, 0);
    if (s->board->adc) {
        prof_wrap_mr(&s->adc_mr, NULL, 0);
    }
    for (int i = 0; i < s->soc->ntim; i++) {
        prof_wrap_mr(&s->tim[i].mr, NULL, 0);
    }
    for (int i = 0; i < s->soc->nusart; i++) {
        prof_wrap_mr(sysbus_mmio_get_region(usart_sbd(s, i), 0), s->soc->usart[i].name, 0);
    }
    /* QEMU's armv7m PPB: NVIC/SCB (ICSR in micros()), SysTick, the RAZ/WI default */
    prof_wrap_mr(&s->armv7m.nvic.sysregmem, "armv7m.nvic", 7);
    prof_wrap_mr(&s->armv7m.systickmem, "armv7m.systick", 7);
    prof_wrap_mr(&s->armv7m.defaultmem, "armv7m.ppb_default", 0);
    fbp_reset(fbprof, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    qatomic_set(&qemu_prof_hooks, &prof_hooks);
    fprintf(stderr, "%s: prof=on (%u accounts, %u timers, %.4f ns per tick)\n", fb_mname,
            fbprof->n, prof_ntimers, fbprof->ns_per_tick);
}

/* ------------------------------------------------------------------ */
/* Block names for the "first access" log (the SoC's table). */

static const char *block_name(NexusXRState *s, hwaddr addr)
{
    hwaddr blk = addr & ~(hwaddr)0x3FF;
    for (int i = 0; i < s->soc->nblocks; i++) {
        if (s->soc->blocks[i].base == blk) {
            return s->soc->blocks[i].name;
        }
    }
    if (s->soc->usb_hs_base && addr >= s->soc->usb_hs_base && addr < s->soc->usb_hs_base + 0x40000) {
        return "USB_OTG_HS";
    }
    if (addr >= 0x50000000 && addr < 0x50040000) {
        return "USB_OTG_FS";
    }
    return "?";
}

/* Blocks that are modelled here and should not be reported as stubs. */
static bool block_is_modelled(hwaddr addr)
{
    hwaddr blk = addr & ~(hwaddr)0x3FF;
    return blk == RCC_BASE || blk == FLASHC_BASE || blk == PWR_BASE ||
           blk == ADC_BASE;
}

/* ------------------------------------------------------------------ */
/* Flash memory and controller */

static void flash_erase(NexusXRState *s, uint32_t off, uint32_t len)
{
    uint8_t *p = memory_region_get_ram_ptr(&s->flash);
    memset(p + off, 0xFF, len);
    memory_region_set_dirty(&s->flash, off, len);
    qemu_log_mask(LOG_UNIMP, "%s: flash erase 0x%08x +0x%x\n", fb_mname,
                  (uint32_t)s->soc->flash_base + off, len);
}

/* ------------------------------------------------------------------ */
/* Idle skip ("external WFI"): -M nexus-xr,idle_skip=on                */
/*
 * RotorFlight never executes WFI. Between tasks its main loop calls scheduler() again and
 * again: DWT CYCCNT (getCycleCounter), SysTick VAL (micros), the task checkers, and back,
 * until something is due. Under icount every one of those instructions costs host time
 * (~92 % of them at 8 % firmware CPU). The machine watches the CYCCNT reads; each gets a
 * signature: the PC of the translation block doing the read and the number of guest
 * instructions executed since the previous read (exact under icount). One scheduler() pass
 * that runs no task is a fixed cycle of p >= 2 such signatures. When idle_skip_n consecutive
 * reads each match the read p before them (so ~idle_skip_n + p reads, at least two cycles), in
 * thread mode with PRIMASK/BASEPRI/FAULTMASK clear, and no interrupt was signalled by the NVIC
 * and no reporting peripheral (below) was accessed in between, the guest is idle, and virtual
 * time is moved forward instead of executing the polls.
 *
 * Reporting = the handler calls idle_touch(): GPIO, EXTI, SPI1, DMA2, the timers, the stub
 * windows (APB/AHB1, AHB2, FMC: RCC, flash controller, the other SPIs, ...) and flash writes.
 * NOT reporting: the 8 QEMU USARTs (stm32l4x5_usart), the PPB of armv7m (NVIC, SCB incl. ICSR,
 * SysTick, MPU and the PPB's RAZ/WI default region), DBGMCU, DWT writes and CTRL/LSR reads,
 * flash/ITCM/SRAM reads. The PPB is part of the idle cycle (micros() reads SysTick VAL and
 * ICSR); what it can change that matters reaches the CPU as an NVIC exception (intercepted) or
 * a virtual timer (SysTick: a deadline the skip stops at). USART polling loops are safe without
 * reporting: TX completes synchronously in the TDR write (every USART is on an fbwire chardev,
 * which always accepts, or on none: TXE/TC are set before the next read), RX
 * bytes arrive from the main loop and set RXNE with the IRQ the firmware drains them by (an NVIC
 * edge = dirty); a pure flag spin has no CYCCNT read in it, and a spin with a CYCCNT timeout
 * would be skipped at most idle_skip_gran_us per cycle, as time would pass anyway.
 *
 * - idle-skip-mode=budget (default): the poll is "executed" by taking instructions out of the
 *   vCPU's icount budget at once. The length comes from the virtual clock alone, read at the
 *   skipping read: MIN(idle_skip_gran_us, next QEMU_CLOCK_VIRTUAL deadline), extending the
 *   budget when it is shorter (see idle_poll), so no timer and no interrupt can be jumped over:
 *   the vCPU thread leaves cpu_exec at the deadline, runs the due timers itself
 *   (icount_handle_deadline) and goes on. The read that triggered the skip already returns the
 *   advanced time.
 * - idle-skip-mode=halt: the CPU is halted as WFI halts it (cs->halted, cpu_exit); icount then
 *   warps to the next deadline, any interrupt the NVIC signals wakes it (cpu_has_work, as for
 *   WFI), a virtual-clock timer after idle_skip_gran_us at the latest. Same result, but every
 *   halt is a round trip vCPU thread -> main loop (which does the warp and runs the timers) ->
 *   vCPU thread, which costs more host time than it saves (evidence/idle-skip-2026-09-26.log).
 *
 * Either way a task that becomes due by time alone starts at most idle_skip_gran_us late. After
 * a skip or an interrupt the guest must show one full idle cycle after its first fresh time read
 * (p matching reads; p + 1 in halt mode, where the halting read returned the old time) before it
 * is skipped again, so a task that became due always gets its pass. Single-read spins (p = 1,
 * the scheduler's wait for the gyro target, <= 12 us) are left alone: their deadline is no
 * interrupt the machine could wake them for.
 */
#define IDLE_RING 32
#define IDLE_PMAX 8

static inline void idle_touch(NexusXRState *s)
{
    s->idle_dirty = true;
}

static int64_t idle_skipped_now(NexusXRState *s)
{
    return s->idle_skipped_ns +
           (s->idle_halted ? qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->idle_halt_t : 0);
}

static void idle_account(NexusXRState *s)
{
    if (s->idle_halted) {
        s->idle_skipped_ns += qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->idle_halt_t;
        s->idle_halted = false;
        timer_del(s->idle_timer);
    }
}

/* End an idle halt from outside the NVIC path: the wake timer, the realtime throttle. */
static void idle_wake(NexusXRState *s)
{
    CPUState *cs = first_cpu;

    if (!s->idle_halted) {
        return;
    }
    idle_account(s);
    if (cs->halted && ARM_CPU(cs)->power_state == PSCI_ON) {
        cpu_env(cs)->halt_reason = NOT_HALTED;
        cs->halted = 0;
        qemu_cpu_kick(cs);
    }
}

static void idle_timer_cb(void *opaque)
{
    idle_wake(opaque);
}

/* NVIC -> CPU interrupt line: a rising edge is an exception about to be taken */
static void idle_nvic_irq(void *opaque, int n, int level)
{
    NexusXRState *s = opaque;

    if (level) {
        s->idle_dirty = true;
        idle_account(s);        /* the CPU leaves the halt for it (cpu_has_work) */
    }
    qemu_set_irq(s->cpu_irq, level);
}

static void idle_forget(NexusXRState *s)
{
    idle_account(s);
    memset(s->idle_run, 0, sizeof(s->idle_run));
    s->idle_lock_p = s->idle_lock_m = 0;
    s->idle_dirty = true;
}

/* Called for every CYCCNT read (see above). */
static void idle_poll(NexusXRState *s)
{
    CPUState *cs = current_cpu;
    CPUARMState *env;
    uint64_t sig;
    int64_t ic;
    bool ok, halt = false;
    unsigned p;
    int sec;

    if (!s->idle_skip || !cs || !icount_enabled()) {
        return;
    }
    env = cpu_env(cs);
    sec = env->v7m.secure;
    ok = env->v7m.exception == 0 && !env->v7m.primask[sec] && !env->v7m.basepri[sec] &&
         !env->v7m.faultmask[sec];
    ic = icount_get_raw() - s->idle_skipped_insns;     /* instructions really executed */
    if (!ok || s->idle_dirty) {
        sig = (1ULL << 63) | ++s->idle_nonce;       /* matches nothing */
        s->idle_dirty = false;
    } else {
        sig = ((uint64_t)env->regs[15] << 32) ^ (uint64_t)(ic - s->idle_last_ic);
    }
    s->idle_last_ic = ic;
    s->idle_pos = (s->idle_pos + 1) % IDLE_RING;
    s->idle_ring[s->idle_pos] = sig;

    /* the cycle found before: one full cycle after a fresh read is enough */
    p = s->idle_lock_p;
    if (p) {
        if (s->idle_lock_m && sig == s->idle_lock[(s->idle_lock_ph + 1) % p]) {
            s->idle_lock_ph = (s->idle_lock_ph + 1) % p;
            s->idle_lock_m++;
        } else {
            s->idle_lock_m = 0;
            for (unsigned i = 0; i < p; i++) {
                if (sig == s->idle_lock[i]) {
                    s->idle_lock_ph = i;
                    s->idle_lock_m = 1;
                    break;
                }
            }
        }
        /* budget mode: the skipping read returned the advanced time, a full cycle after it
         * (p reads) shows the guest acted on it; halt mode: that read was stale, one more */
        halt = s->idle_lock_m >= p + s->idle_halt_mode;
    }
    /* a new cycle: idle_skip_n reads (at least two cycles) repeating with period p */
    for (p = 2; p <= IDLE_PMAX; p++) {
        if (sig == s->idle_ring[(s->idle_pos + IDLE_RING - p) % IDLE_RING]) {
            s->idle_run[p]++;
        } else {
            s->idle_run[p] = 0;
        }
    }
    /* (a single-read spin repeats with every period: never lock onto it) */
    for (p = 2; !halt && p <= IDLE_PMAX && sig != s->idle_ring[(s->idle_pos + IDLE_RING - 1) %
                                                            IDLE_RING]; p++) {
        if (s->idle_run[p] >= MAX(s->idle_skip_n, 2 * p)) {
            for (unsigned i = 0; i < p; i++) {
                s->idle_lock[i] = s->idle_ring[(s->idle_pos + IDLE_RING - (p - 1) + i) %
                                               IDLE_RING];
            }
            s->idle_lock_p = p;
            s->idle_lock_ph = p - 1;
            halt = true;
        }
    }
    /* an exception about to be taken: let it in first. Not cpu_has_work(): that includes
     * CPU_INTERRUPT_EXITTB, which other threads can raise at host-timed moments: with it two
     * identical runs flew different trajectories, with this test they are identical. */
    if (!halt || !ok || cpu_test_interrupt(cs, CPU_INTERRUPT_HARD | CPU_INTERRUPT_NMI)) {
        return;
    }
    s->idle_lock_m = 0;
    memset(s->idle_run, 0, sizeof(s->idle_run));
    if (!s->idle_halt_mode) {
        /* The I/O instruction is the last of its TB and already counted (icount_get_raw above
         * synced icount_budget); what is left of the budget is decr.low + extra
         * (icount_get_executed). Taking k instructions away is executing them.
         *
         * k is NOT the leftover budget: icount_get_limit() also caps the budget by the nearest
         * QEMU_CLOCK_REALTIME deadline (monitor/QMP rate limiter, chardev reconnect, ...) and by
         * replay, so the jump -- and the time the firmware reads next -- would depend on host
         * state. k comes from the virtual clock alone, read now: the next QEMU_CLOCK_VIRTUAL
         * deadline, capped at idle_skip_gran_us. When the budget is shorter (a realtime timer
         * cut it), it is extended so the jump is exactly k; that realtime timer then runs at
         * most idle_skip_gran_us of virtual time late (host time: one TB exit later).
         *
         * Reading the deadline here, not only in icount_prepare_for_run, also covers a virtual
         * timer armed from the main loop while the vCPU runs (ctl "step", the host UART inject
         * pump): the budget computed before it would overshoot it by up to idle_skip_gran_us;
         * this read sees it (timer lists are locked), so the jump ends at it at the latest. A
         * timer armed after this read is the usual icount race, the kick handles it as before. */
        int64_t left = cs->neg.icount_decr.u16.low + cs->icount_extra;
        int64_t dl = qemu_clock_deadline_ns_all(QEMU_CLOCK_VIRTUAL, QEMU_TIMER_ATTR_ALL);
        int64_t k = dl < 0 ? s->idle_gran_insns : MIN(s->idle_gran_insns, icount_round(dl));
        if (k <= 0) {
            return;
        }
        if (k > left) {
            cs->icount_budget += k - left;
            left = k;
            s->idle_extended++;
        }
        left -= k;
        cs->neg.icount_decr.u16.low = MIN(left, 0xffff);
        cs->icount_extra = left - cs->neg.icount_decr.u16.low;
        s->idle_last_to = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);   /* folds k in (ctl step check) */
        s->idle_last_from = s->idle_last_to - icount_to_ns(k);
        s->idle_skips++;
        s->idle_skipped_insns += k;
        s->idle_skipped_ns += icount_to_ns(k);
        if (unlikely(fbprof)) {
            fbp_count(fbprof, pa.idle_skip, icount_to_ns(k));
            prof_skip_end = left == 0;      /* the slice ends at the deadline the skip reached */
        }
        return;
    }
    s->idle_halted = true;
    s->idle_halt_t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->idle_skips++;
    if (unlikely(fbprof)) {
        fbp_count(fbprof, pa.idle_skip, 0);
    }
    timer_mod(s->idle_timer, s->idle_halt_t + (int64_t)s->idle_skip_gran_us * SCALE_US);
    /* what HELPER(wfi) does, after the load rather than in place of an instruction: the TB
     * ends here (an I/O instruction is the last one of its TB under icount), cpu_exec sees
     * halted */
    env->halt_reason = HALT_WFI;
    cs->halted = 1;
    cpu_exit(cs);
}

/*
 * The second trigger, idle-skip-src=systick (iNav): its scheduler never reads CYCCNT, its idle pass
 * is micros() = SCB ICSR + BASEPRI + sysTickUptime + SysTick VAL in a retry loop
 * (drivers/time.c). QEMU's armv7m SysTick region is wrapped (as the prof ledger wraps it): every
 * VAL read (offset 8 in the region at 0xE000E010) goes through idle_poll first, with the same
 * detector (signature = TB PC + instructions since the previous read, a cycle of p >= 2 reads,
 * thread mode, no interrupt, no reporting peripheral in between), then reads the advanced time.
 * The CYCCNT trigger is off then; the other way round, SysTick reads are ignored (RotorFlight reads
 * both: its detector stays on CYCCNT, unchanged).
 */
typedef struct IdleMr {
    const MemoryRegionOps *ops;
    void *opaque;
    NexusXRState *s;
    MemoryRegionOps w;
} IdleMr;

static MemTxResult idle_systick_read(void *opaque, hwaddr addr, uint64_t *data, unsigned size,
                                     MemTxAttrs attrs)
{
    IdleMr *m = opaque;
    if (addr == 0x8) {
        idle_poll(m->s);
    }
    if (m->ops->read_with_attrs) {
        return m->ops->read_with_attrs(m->opaque, addr, data, size, attrs);
    }
    *data = m->ops->read(m->opaque, addr, size);
    return MEMTX_OK;
}

static MemTxResult idle_systick_write(void *opaque, hwaddr addr, uint64_t data, unsigned size,
                                      MemTxAttrs attrs)
{
    IdleMr *m = opaque;
    if (m->ops->write_with_attrs) {
        return m->ops->write_with_attrs(m->opaque, addr, data, size, attrs);
    }
    m->ops->write(m->opaque, addr, data, size);
    return MEMTX_OK;
}

static void idle_systick_hook(NexusXRState *s, MemoryRegion *mr)
{
    IdleMr *m = g_new0(IdleMr, 1);
    assert(!mr->ops->valid.accepts);
    m->ops = mr->ops;
    m->opaque = mr->opaque;
    m->s = s;
    m->w = *mr->ops;
    m->w.read = NULL;
    m->w.write = NULL;
    m->w.read_with_attrs = idle_systick_read;
    m->w.write_with_attrs = idle_systick_write;
    mr->ops = &m->w;
    mr->opaque = m;
}

static uint64_t flash_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    NexusXRState *s = opaque;
    uint8_t *p = memory_region_get_ram_ptr(&s->flash);
    return ldn_le_p(p + addr, size);
}

static void flash_mem_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    NexusXRState *s = opaque;
    idle_touch(s);
    uint8_t *p = memory_region_get_ram_ptr(&s->flash);

    if (!(s->flash_cr & 1) || (s->flash_cr & (1u << 31))) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: flash write 0x%08" HWADDR_PRIx
                      " without PG/unlock, ignored\n", fb_mname, s->soc->flash_base + addr);
        return;
    }
    if (addr >= s->board->cfg_off && addr < s->board->cfg_off + s->board->cfg_len) {
        s->cfg_prog = true;
    }
    /* NOR programming can only clear bits */
    uint64_t old = ldn_le_p(p + addr, size);
    stn_le_p(p + addr, size, old & val);
    memory_region_set_dirty(&s->flash, addr, size);
}

static const MemoryRegionOps flash_mem_ops = {
    .read = flash_mem_read,
    .write = flash_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* FLASH controller registers, called from the stub window */
static bool flashc_read(NexusXRState *s, hwaddr off, uint32_t *val)
{
    switch (off) {
    case 0x0C:              /* SR: never busy, no errors */
        *val = 0;
        return true;
    case 0x10:              /* CR */
        *val = s->flash_cr;
        return true;
    }
    return false;           /* ACR, OPTCR...: plain storage */
}

static void flashc_write(NexusXRState *s, hwaddr off, uint32_t val)
{
    switch (off) {
    case 0x04:              /* KEYR */
        if (val == 0x45670123) {
            s->flash_key_state = 1;
        } else if (val == 0xCDEF89AB && s->flash_key_state == 1) {
            s->flash_cr &= ~(1u << 31);
            s->flash_key_state = 0;
        } else {
            s->flash_key_state = 0;
        }
        break;
    case 0x10:              /* CR */
        if (s->flash_cr & (1u << 31)) {
            /* locked: only the LOCK bit itself can be written */
            break;
        }
        s->flash_cr = val & ~(1u << 16);
        if ((val & (1u << 31)) && s->cfg_prog) {
            /* locked again after writing the config sector: the firmware's save is complete
             * (config_streamer_finish -> FLASH_Lock) */
            s->cfg_prog = false;
            ctl_cfg_written(s);
        }
        if (val & (1u << 16)) {                     /* STRT */
            if (val & (1u << 2)) {                  /* MER */
                flash_erase(s, 0, s->soc->flash_size);
            } else if (val & (1u << 1)) {           /* SER */
                unsigned snb = (val >> 3) & 0xF;
                const uint32_t *so = s->soc->flash_sector;
                if (snb < s->soc->nsectors) {
                    flash_erase(s, so[snb], so[snb + 1] - so[snb]);
                    s->cfg_prog |= so[snb] <= s->board->cfg_off && s->board->cfg_off < so[snb + 1];
                }
            }
        }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Stub windows with the small register models folded in */

static uint32_t rcc_fixup(NexusXRState *s, hwaddr off, uint32_t v)
{
    switch (off) {
    case 0x00: {            /* CR: xxxRDY follows xxxON */
        uint32_t on = v & ((1u << 0) | (1u << 16) | (1u << 24) |
                           (1u << 26) | (1u << 28));
        v &= ~((1u << 1) | (1u << 17) | (1u << 25) | (1u << 27) | (1u << 29));
        return v | (on << 1);
    }
    case 0x08:              /* CFGR: SWS = SW */
        return (v & ~0xCu) | ((v & 3u) << 2);
    case 0x70:              /* BDCR: LSERDY = LSEON */
        return (v & ~2u) | ((v & 1u) << 1);
    case 0x74:              /* CSR: LSIRDY = LSION */
        return (v & ~2u) | ((v & 1u) << 1);
    }
    return v;
}

static int spi_index(NexusXRState *s, hwaddr blk)
{
    for (int i = 0; i < s->soc->nspi_empty; i++) {
        if (s->soc->spi_empty[i] == blk) {
            return i;
        }
    }
    return -1;
}

static uint32_t stub_read_fixup(NexusXRState *s, hwaddr addr, uint32_t v)
{
    if ((addr & ~0x3FFull) == RCC_BASE) {
        return rcc_fixup(s, addr - RCC_BASE, v);
    }
    int spi = spi_index(s, addr & ~0x3FFull);
    if (spi >= 0) {
        switch (addr & 0x3FF) {
        case 0x08:          /* SR: TXE, RXNE/FRLVL follow the byte in flight */
            return (1u << 1) | (s->spi_rxne[spi] ? (1u << 0) | (1u << 9) : 0);
        case 0x0C:          /* DR: nothing drives MISO */
            s->spi_rxne[spi] = false;
            return 0xFFFF;
        }
    }
    switch (addr) {
    case PWR_BASE + 0x04:   /* CSR1: VOSRDY, ODRDY, ODSWRDY */
        return v | (1u << 14) | (1u << 16) | (1u << 17);
    case SYSCFG_BASE + 0x20: /* CMPCR: READY */
        return v | (1u << 8);
    case ADC_BASE + 0x00:   /* ADC1 SR: AWD|EOC|JEOC|JSTRT|STRT */
        return 0x1E;
    case ADC_BASE + 0x3C: case ADC_BASE + 0x40:
    case ADC_BASE + 0x44: case ADC_BASE + 0x48:
        return 1489;        /* ~Vrefint at 3.3 V, 12 bit */
    case ADC_BASE + 0x4C:
        return 2048;
    case RTC_BASE + 0x0C:   /* RTC ISR: INITF, RSF, INITS */
        return v | (1u << 6) | (1u << 5) | (1u << 4);
    }
    return v;
}

static uint64_t stub_read(void *opaque, hwaddr off, unsigned size)
{
    StubWindow *w = opaque;
    NexusXRState *s = w->s;

    idle_touch(s);
    hwaddr addr = w->base + off;
    hwaddr aligned = addr & ~3ull;
    uint32_t v;

    if (!test_and_set_bit(off >> 10, w->seen) && !block_is_modelled(addr)) {
        qemu_log_mask(LOG_UNIMP, "%s: stub %s @0x%08" HWADDR_PRIx
                      " first access (read 0x%08" HWADDR_PRIx ")\n",
                      fb_mname, block_name(s, addr), (hwaddr)(addr & ~0x3FFull), addr);
    }
    if ((aligned & ~0x3FFull) == FLASHC_BASE &&
        flashc_read(s, aligned - FLASHC_BASE, &v)) {
        /* v set */
    } else {
        v = stub_read_fixup(s, aligned, ldl_le_p(w->regs + (off & ~3ull)));
    }
    return extract64(v, (addr & 3) * 8, size * 8);
}

static void stub_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    StubWindow *w = opaque;
    NexusXRState *s = w->s;

    idle_touch(s);
    hwaddr addr = w->base + off;
    hwaddr aligned = addr & ~3ull;
    uint8_t *reg = w->regs + (off & ~3ull);
    uint32_t v = ldl_le_p(reg);

    if (!test_and_set_bit(off >> 10, w->seen) && !block_is_modelled(addr)) {
        qemu_log_mask(LOG_UNIMP, "%s: stub %s @0x%08" HWADDR_PRIx
                      " first access (write 0x%08" HWADDR_PRIx ")\n",
                      fb_mname, block_name(s, addr), (hwaddr)(addr & ~0x3FFull), addr);
    }
    v = deposit32(v, (addr & 3) * 8, size * 8, val);

    if ((aligned & ~0x3FFull) == FLASHC_BASE) {
        flashc_write(s, aligned - FLASHC_BASE, v);
    }
    int spi = spi_index(s, aligned & ~0x3FFull);
    if (spi >= 0 && (aligned & 0x3FF) == 0x0C) {
        s->spi_rxne[spi] = true;    /* a written byte clocks one in */
    }
    if (aligned == RCC_BASE + 0x74 && (v & (1u << 24))) {
        v &= 0x00FFFFFF;    /* RMVF clears the reset flags */
    }
    stl_le_p(reg, v);
}

static const MemoryRegionOps stub_ops = {
    .read = stub_read,
    .write = stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void stub_window_init(NexusXRState *s, StubWindow *w, const char *name,
                             hwaddr base, uint32_t size)
{
    g_autofree char *n = mr_name(name);
    w->s = s;
    w->base = base;
    w->size = size;
    w->regs = g_malloc0(size);
    w->seen = bitmap_new(size >> 10);
    memory_region_init_io(&w->mr, OBJECT(s), &stub_ops, w, n, size);
    memory_region_add_subregion_overlap(get_system_memory(), base, &w->mr,
                                        -1000);
}

static void stub_set32(StubWindow *w, hwaddr addr, uint32_t v)
{
    stl_le_p(w->regs + (addr - w->base), v);
}

static uint32_t stub_get32(StubWindow *w, hwaddr addr)
{
    return ldl_le_p(w->regs + (addr - w->base));
}

/* ------------------------------------------------------------------ */
/* DWT (cycle counter) and DBGMCU */

static uint32_t dwt_now(NexusXRState *s)
{
    if (!(s->dwt_ctrl & 1)) {
        return s->dwt_cyccnt_base;
    }
    int64_t dt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->dwt_cyccnt_t0;
    return s->dwt_cyccnt_base +
           (uint32_t)muldiv64(dt, s->soc->sysclk_hz, NANOSECONDS_PER_SECOND);
}

static void dwt_rebase(NexusXRState *s, uint32_t value)
{
    s->dwt_cyccnt_base = value;
    s->dwt_cyccnt_t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static uint64_t dwt_read(void *opaque, hwaddr off, unsigned size)
{
    NexusXRState *s = opaque;
    switch (off) {
    case 0x000:
        return s->dwt_ctrl;
    case 0x004:
        if (!s->idle_systick) {
            idle_poll(s);
        }
        return dwt_now(s);
    case 0xFB4:             /* LSR: lock implemented, unlocked */
        return 1;
    }
    return 0;
}

static void dwt_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    NexusXRState *s = opaque;
    switch (off) {
    case 0x000: {
        uint32_t cur = dwt_now(s);
        s->dwt_ctrl = (s->dwt_ctrl & 0xF0000000) | (val & 0x0FFFFFFF);
        dwt_rebase(s, cur);
        break;
    }
    case 0x004:
        dwt_rebase(s, val);
        break;
    }
}

static const MemoryRegionOps dwt_ops = {
    .read = dwt_read,
    .write = dwt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t dbgmcu_read(void *opaque, hwaddr off, unsigned size)
{
    NexusXRState *s = opaque;
    switch (off) {
    case 0x00:
        return s->soc->dbgmcu_idcode;
    case 0x04:
        return s->dbgmcu_cr;
    }
    return 0;
}

static void dbgmcu_write(void *opaque, hwaddr off, uint64_t val,
                         unsigned size)
{
    NexusXRState *s = opaque;
    if (off == 0x04) {
        s->dbgmcu_cr = val;
    }
}

static const MemoryRegionOps dbgmcu_ops = {
    .read = dbgmcu_read,
    .write = dbgmcu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* GPIO A..D, SYSCFG EXTI routing, EXTI                               */
/*
 * Pins only carry levels: an output pin's level is its ODR bit, any other
 * pin reads what the outside drives (gpio_in, idle high = board pull-up
 * for undriven pins that matter, e.g. the gyro CS). Level changes feed
 * the IMU chip select (PA4) and the EXTI edge detectors.
 */

static bool gpio_pin_is_output(NexusXRState *s, int port, int pin)
{
    return ((s->gpio[port][GPIO_MODER] >> (2 * pin)) & 3) == 1;
}

static uint16_t gpio_levels(NexusXRState *s, int port)
{
    uint16_t v = 0;
    for (int pin = 0; pin < 16; pin++) {
        bool hi = gpio_pin_is_output(s, port, pin)
                  ? (s->gpio[port][GPIO_ODR] >> pin) & 1
                  : (s->gpio_in[port] >> pin) & 1;
        v |= (uint16_t)hi << pin;
    }
    return v;
}

static void exti_update_irq(NexusXRState *s)
{
    uint32_t pend = s->exti_pr & s->exti_imr;
    for (int i = 0; i < 5; i++) {
        qemu_set_irq(s->exti_irq[i], (pend >> i) & 1);
    }
    qemu_set_irq(s->exti_irq[5], (pend & 0x03E0) != 0);    /* EXTI9_5 */
    qemu_set_irq(s->exti_irq[6], (pend & 0xFC00) != 0);    /* EXTI15_10 */
}

static void imu_cs_changed(NexusXRState *s, bool level);

static void gpio_update(NexusXRState *s, int port)
{
    uint16_t now = gpio_levels(s, port);
    uint16_t changed = now ^ s->gpio_level[port];
    s->gpio_level[port] = now;

    for (int pin = 0; changed && pin < 16; pin++) {
        if (!((changed >> pin) & 1)) {
            continue;
        }
        bool hi = (now >> pin) & 1;
        if (port == s->board->imu_cs_port && pin == s->board->imu_cs_pin) {
            imu_cs_changed(s, hi);
        }
        if (s->osd_chip && port == s->board->osd_cs_port && pin == s->board->osd_cs_pin &&
            s->osd_chip_cs != !hi) {
            s->osd_chip_cs = !hi;       /* active low */
            fb_max7456_cs(s->osd_chip, !hi);
        }
        if (port == s->beeper_port && pin == s->beeper_pin) {
            ctl_beeper(s, hi);
        }
        /* EXTI line <pin> listens to the port selected in SYSCFG_EXTICRx */
        uint32_t cr = stub_get32(&s->apb, SYSCFG_BASE + 0x08 + 4 * (pin / 4));
        if (((cr >> (4 * (pin % 4))) & 0xF) != port) {
            continue;
        }
        if ((hi && (s->exti_rtsr >> pin) & 1) ||
            (!hi && (s->exti_ftsr >> pin) & 1)) {
            s->exti_pr |= 1u << pin;
        }
    }
    exti_update_irq(s);
}

/* Drive an input pin from outside (device line). */
static void gpio_set_input(NexusXRState *s, int port, int pin, bool hi)
{
    s->gpio_in[port] = deposit32(s->gpio_in[port], pin, 1, hi);
    gpio_update(s, port);
}

static uint64_t gpio_read(void *opaque, hwaddr off, unsigned size)
{
    NexusXRState *s = opaque;
    idle_touch(s);
    int port = off / 0x400;
    int reg = (off & 0x3FF) / 4;
    uint32_t v;

    if (reg >= GPIO_NREGS) {
        return 0;
    }
    switch (reg) {
    case GPIO_IDR:
        v = gpio_levels(s, port);
        break;
    case GPIO_BSRR:
        v = 0;              /* write-only */
        break;
    default:
        v = s->gpio[port][reg];
    }
    return extract32(v, (off & 3) * 8, size * 8);
}

static void gpio_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    NexusXRState *s = opaque;
    idle_touch(s);
    int port = off / 0x400;
    int reg = (off & 0x3FF) / 4;

    if (reg >= GPIO_NREGS || reg == GPIO_IDR) {
        return;
    }
    if (reg == GPIO_BSRR) {
        uint32_t v = deposit32(0, (off & 3) * 8, size * 8, val);
        uint32_t odr = s->gpio[port][GPIO_ODR];
        odr &= ~(v >> 16);
        odr |= v & 0xFFFF;              /* set wins over reset */
        s->gpio[port][GPIO_ODR] = odr & 0xFFFF;
    } else {
        s->gpio[port][reg] = deposit32(s->gpio[port][reg], (off & 3) * 8,
                                       size * 8, val);
    }
    if (reg == GPIO_MODER || reg == GPIO_ODR || reg == GPIO_BSRR) {
        gpio_update(s, port);
    }
}

static const MemoryRegionOps gpio_ops = {
    .read = gpio_read,
    .write = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static uint64_t exti_read(void *opaque, hwaddr off, unsigned size)
{
    NexusXRState *s = opaque;
    idle_touch(s);
    uint32_t v = 0;

    switch (off & ~3) {
    case 0x00: v = s->exti_imr; break;
    case 0x04: v = s->exti_emr; break;
    case 0x08: v = s->exti_rtsr; break;
    case 0x0C: v = s->exti_ftsr; break;
    case 0x14: v = s->exti_pr; break;
    }
    return extract32(v, (off & 3) * 8, size * 8);
}

static void exti_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    NexusXRState *s = opaque;
    idle_touch(s);
    uint32_t *r = NULL;
    uint32_t v;

    switch (off & ~3) {
    case 0x00: r = &s->exti_imr; break;
    case 0x04: r = &s->exti_emr; break;
    case 0x08: r = &s->exti_rtsr; break;
    case 0x0C: r = &s->exti_ftsr; break;
    case 0x10:              /* SWIER: software trigger */
        v = deposit32(0, (off & 3) * 8, size * 8, val);
        s->exti_pr |= v & s->exti_imr;
        exti_update_irq(s);
        return;
    case 0x14:              /* PR: write 1 to clear */
        v = deposit32(0, (off & 3) * 8, size * 8, val);
        s->exti_pr &= ~v;
        exti_update_irq(s);
        return;
    default:
        return;
    }
    *r = deposit32(*r, (off & 3) * 8, size * 8, val) & 0x00FFFFFF;
    exti_update_irq(s);
}

static const MemoryRegionOps exti_ops = {
    .read = exti_read,
    .write = exti_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* SPI1: STM32F7 SPI with the gyro on it                              */
/*
 * Transfers are instantaneous for polled access: a DR write clocks each
 * frame through the selected device (the IMU when PA4 is low, 0xFF
 * otherwise) and its MISO byte lands in a 4-byte RX FIFO. 8-bit frames
 * with data packing: a 16-bit DR write sends the low byte first, a 16-bit
 * DR read pops two bytes. SR.RXNE follows CR2.FRXTH (1 or 2 bytes).
 */

/* SPI1 carries the IMU; SPI2 (F4 boards) the board's MAX7456 OSD chip */
static uint8_t spi_exchange(SpiPort *p, uint8_t mosi)
{
    NexusXRState *s = p->s;
    if (p->index == 0) {
        return s->imu_cs && s->imu_present ? fb_icm42688p_xfer(s->imu, mosi) : 0xFF;
    }
    return s->osd_chip && s->osd_chip_cs ? fb_max7456_xfer(s->osd_chip, mosi) : 0xFF;
}

static void spi1_rx_push(SpiPort *p, uint8_t b)
{
    if (p->rx_n < sizeof(p->rx)) {
        p->rx[p->rx_n++] = b;
    } else {
        p->ovr = true;
    }
}

static uint8_t spi1_rx_pop(SpiPort *p)
{
    if (!p->rx_n) {
        return 0;
    }
    uint8_t b = p->rx[0];
    memmove(p->rx, p->rx + 1, --p->rx_n);
    return b;
}

static uint32_t spi1_sr(SpiPort *p)
{
    bool frxth = p->cr2 & (1u << 12);
    unsigned n = p->rx_n;
    uint32_t sr = 1u << 1;                                /* TXE */
    if (n >= (frxth ? 1u : 2u)) {
        sr |= 1u << 0;                                    /* RXNE */
    }
    sr |= (uint32_t)MIN(n, 3u) << 9;                      /* FRLVL */
    if (p->ovr) {
        sr |= 1u << 6;
    }
    return sr;
}

static void dma2_try_start(NexusXRState *s);
static void dma2_timer_check(NexusXRState *s);
static void imu_trace(NexusXRState *s, char ev, int64_t t, int arg);

/*
 * F4 SPI (FB_SPI_F4, RM0090 28.5): no FIFO. A DR write while SPE clocks one frame through the
 * selected device at once, 8 bit or with CR1.DFF 16 bit (MSB first: high byte on the wire
 * first); the frame lands in the RX buffer and sets RXNE; a DR read returns it and clears RXNE.
 * A frame received while RXNE is still set is lost and sets OVR (cleared by a DR read followed
 * by an SR read). TXE always set, never BSY. The polled path of iNav's bus_spi.c (wait TXE,
 * write DR, wait RXNE, read DR) is all that is needed; DMA works as on the F7.
 */
static bool spi1_f4(NexusXRState *s)
{
    return s->soc->spi_model == FB_SPI_F4;
}

static void spi1_f4_rx(SpiPort *p, uint16_t frame)
{
    if (p->rx_n) {
        p->ovr = true;
        return;
    }
    p->rx16 = frame;
    p->rx_n = 1;
}

static void spi1_f4_frame(SpiPort *p, uint16_t mosi)
{
    if (p->cr1 & (1u << 11)) {                              /* DFF: 16-bit frame */
        uint16_t hi = spi_exchange(p, mosi >> 8);
        spi1_f4_rx(p, hi << 8 | spi_exchange(p, mosi & 0xFF));
    } else {
        spi1_f4_rx(p, spi_exchange(p, mosi & 0xFF));
    }
}

static uint64_t spi1_read(void *opaque, hwaddr off, unsigned size)
{
    SpiPort *p = opaque;
    NexusXRState *s = p->s;
    idle_touch(s);
    uint32_t v;

    switch (off & ~3) {
    case 0x00: v = p->cr1; break;
    case 0x04: v = p->cr2; break;
    case 0x08:
        if (spi1_f4(s)) {
            v = (1u << 1) | (p->rx_n ? 1u << 0 : 0) | (p->ovr ? 1u << 6 : 0);
        } else {
            v = spi1_sr(p);
        }
        p->ovr = false;             /* OVR: cleared by DR read + SR read */
        break;
    case 0x0C:
        if (off & 3) {
            return 0;
        }
        if (spi1_f4(s)) {
            v = p->rx_n ? p->rx16 : 0;
            p->rx_n = 0;
            return v;
        }
        v = spi1_rx_pop(p);
        if (size >= 2) {
            v |= spi1_rx_pop(p) << 8;
        }
        return v;
    case 0x10: v = p->crcpr; break;
    default: v = 0;
    }
    return extract32(v, (off & 3) * 8, size * 8);
}

static void spi1_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    SpiPort *p = opaque;
    NexusXRState *s = p->s;
    idle_touch(s);

    switch (off & ~3) {
    case 0x00:
        p->cr1 = deposit32(p->cr1, (off & 3) * 8, size * 8, val);
        break;
    case 0x04:
        p->cr2 = deposit32(p->cr2, (off & 3) * 8, size * 8, val);
        if (p->index == 0) {
            dma2_try_start(s);
        }
        break;
    case 0x0C:
        if ((off & 3) || !(p->cr1 & (1u << 6))) {           /* SPE */
            break;
        }
        if (spi1_f4(s)) {
            spi1_f4_frame(p, val);
            break;
        }
        if (((p->cr2 >> 8) & 0xF) > 7) {
            qemu_log_mask(LOG_UNIMP, "%s: SPI1 frames > 8 bit\n", fb_mname);
        }
        spi1_rx_push(p, spi_exchange(p, val & 0xFF));
        if (size >= 2) {
            spi1_rx_push(p, spi_exchange(p, (val >> 8) & 0xFF));
        }
        break;
    case 0x10:
        p->crcpr = deposit32(p->crcpr, (off & 3) * 8, size * 8, val);
        break;
    }
}

static const MemoryRegionOps spi1_ops = {
    .read = spi1_read,
    .write = spi1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* I2C3: STM32F7 I2C (v2 IP) master with the onboard baro on its bus   */
/*
 * The register interface and bus state machine are stm32_i2c.c.inc (what is modelled and how:
 * its header comment); here only the QEMU side: MMIO, the virtual-clock timer, the NVIC lines,
 * -d unimp logging, the idle-skip touch.
 */

static void i2c_port_timer_mod(void *opaque, int64_t delay_ns)
{
    I2cPort *p = opaque;
    timer_mod(p->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
}

static void i2c_port_timer_del(void *opaque)
{
    timer_del(((I2cPort *)opaque)->timer);
}

static void i2c_port_irq(void *opaque, bool ev, bool er)
{
    I2cPort *p = opaque;
    qemu_set_irq(p->ev_irq, ev);
    qemu_set_irq(p->er_irq, er);
}

static void i2c_port_log(void *opaque, const char *msg)
{
    qemu_log_mask(LOG_UNIMP, "%s\n", msg);
}

static void i2c_port_timer_cb(void *opaque)
{
    i2c_core_timer(&((I2cPort *)opaque)->c);
}

static uint64_t i2c_port_read(void *opaque, hwaddr off, unsigned size)
{
    I2cPort *p = opaque;
    idle_touch(p->s);
    return i2c_core_read(&p->c, off, size);
}

static void i2c_port_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    I2cPort *p = opaque;
    idle_touch(p->s);
    i2c_core_write(&p->c, off, val, size);
}

static const MemoryRegionOps i2c_ops = {
    .read = i2c_port_read,
    .write = i2c_port_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void i2c_port_init(NexusXRState *s, I2cPort *p, const char *name, hwaddr base,
                          int ev_irq, int er_irq)
{
    p->s = s;
    p->c.name = name;
    p->c.pclk_hz = s->soc->pclk1_hz;
    p->c.hooks = (I2cHooks) {
        .timer_mod = i2c_port_timer_mod, .timer_del = i2c_port_timer_del,
        .irq = i2c_port_irq, .log_unimp = i2c_port_log, .opaque = p,
    };
    memory_region_init_io(&p->mr, OBJECT(s), &i2c_ops, p, name, 0x400);
    memory_region_add_subregion_overlap(get_system_memory(), base, &p->mr, 0);
    p->ev_irq = qdev_get_gpio_in(DEVICE(&s->armv7m), ev_irq);
    p->er_irq = qdev_get_gpio_in(DEVICE(&s->armv7m), er_irq);
    p->timer = nx_timer_new("i2c3.timer", i2c_port_timer_cb, p);
}

/* ------------------------------------------------------------------ */
/* I2C1: STM32F4 I2C (v1 IP) master: the SPL06 baro and a wired compass */
/*
 * The register interface and bus state machine are stm32_i2c_v1.c.inc (what is modelled and how:
 * its header comment); here only the QEMU side: MMIO, the virtual-clock timer, the NVIC lines,
 * -d unimp logging, the idle-skip touch. Counters: i2c_xfers (STARTs), i2c_nacks, i2c_bytes.
 */

static void i2c_v1_timer_mod(void *opaque, int64_t delay_ns)
{
    I2cV1Port *p = opaque;
    timer_mod(p->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
}

static void i2c_v1_timer_del(void *opaque)
{
    timer_del(((I2cV1Port *)opaque)->timer);
}

static void i2c_v1_irq(void *opaque, bool ev, bool er)
{
    I2cV1Port *p = opaque;
    qemu_set_irq(p->ev_irq, ev);
    qemu_set_irq(p->er_irq, er);
}

static void i2c_v1_log(void *opaque, const char *msg)
{
    qemu_log_mask(LOG_UNIMP, "%s: %s\n", fb_mname, msg);
}

static void i2c_v1_timer_cb(void *opaque)
{
    i2c1v1_timer(&((I2cV1Port *)opaque)->c);
}

static uint64_t i2c_v1_port_read(void *opaque, hwaddr off, unsigned size)
{
    I2cV1Port *p = opaque;
    idle_touch(p->s);
    return i2c1v1_read(&p->c, off, size);
}

static void i2c_v1_port_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    I2cV1Port *p = opaque;
    idle_touch(p->s);
    i2c1v1_write(&p->c, off, val, size);
}

static const MemoryRegionOps i2c_v1_ops = {
    .read = i2c_v1_port_read,
    .write = i2c_v1_port_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void i2c_v1_port_init(NexusXRState *s, I2cV1Port *p, const char *name, hwaddr base,
                             int ev_irq, int er_irq)
{
    p->s = s;
    p->c.name = s->soc->i2c_name;
    p->c.pclk_hz = s->soc->pclk1_hz;
    p->c.hooks = (I2cHooks) {
        .timer_mod = i2c_v1_timer_mod, .timer_del = i2c_v1_timer_del,
        .irq = i2c_v1_irq, .log_unimp = i2c_v1_log, .opaque = p,
    };
    memory_region_init_io(&p->mr, OBJECT(s), &i2c_v1_ops, p, name, 0x400);
    memory_region_add_subregion_overlap(get_system_memory(), base, &p->mr, 0);
    p->ev_irq = qdev_get_gpio_in(DEVICE(&s->armv7m), ev_irq);
    p->er_irq = qdev_get_gpio_in(DEVICE(&s->armv7m), er_irq);
    p->timer = nx_timer_new("i2c1.timer", i2c_v1_timer_cb, p);
}

/* the machine's I2C slave table (the v2 or the v1 controller's) */
static void i2c_add_slave(NexusXRState *s, const I2cSlave *sl)
{
    if (s->soc->i2c_model == FB_I2C_V1) {
        assert(s->i2c1.c.nslaves < I2C_MAX_SLAVES);
        s->i2c1.c.slaves[s->i2c1.c.nslaves++] = *sl;
    } else {
        assert(s->i2c3.c.nslaves < I2C_MAX_SLAVES);
        s->i2c3.c.slaves[s->i2c3.c.nslaves++] = *sl;
    }
}

/* ---- the DPS310 (SPL06-001) baro on I2C3 ------------------------------------------------ */
/*
 * fb_dps310 from devices/, only through its public API: byte-level slave callbacks, a
 * one-shot measurement (MEAS_CTRL 1/2) sampled once after fb_dps310_oneshot_us(), a
 * virtual-clock measurement timer at fb_dps310_rate_hz() (re-evaluated at every STOP, as the
 * IMU's ODR is at every CS release; 32 Hz as this driver programs it), and the environment
 * from the FDM every physics tick: altitude = field elevation + FDM altitude, pressure from the
 * ICAO standard atmosphere p = p0 (1 - 2.25577e-5 h)^5.25588 (p0 = sea-level pressure,
 * 101325 Pa unless -M baro-env= says otherwise), temperature t0 - 6.5 K/km * h (t0 = the
 * SEA-LEVEL temperature, default 22 C: at a 1000 m field it is 15.5 C);
 * handed to the chip at each of its samples, which adds the datasheet's pressure noise.
 */

static void baro_start(void *opaque, bool read)
{
    NexusXRState *s = opaque;
    if (!read) {
        fb_dps310_i2c_start(s->baro);
    }
}

static bool baro_write(void *opaque, uint8_t b)
{
    NexusXRState *s = opaque;
    return fb_dps310_i2c_write_byte(s->baro, b);
}

static uint8_t baro_read(void *opaque)
{
    NexusXRState *s = opaque;
    return fb_dps310_i2c_read_byte(s->baro);
}

static void baro_rate_update(NexusXRState *s)
{
    uint32_t hz = fb_dps310_rate_hz(s->baro);
    uint32_t os_us = hz ? 0 : fb_dps310_oneshot_us(s->baro);
    if (os_us) {
        /* MEAS_CTRL 1/2: one measurement, sampled once after its conversion time */
        s->baro_rate = 0;
        if (!s->baro_oneshot) {
            s->baro_oneshot = true;
            timer_mod(s->baro_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + os_us * SCALE_US);
        }
        return;
    }
    if (s->baro_oneshot) {
        s->baro_oneshot = false;        /* the one-shot was cancelled (MEAS_CTRL rewritten) */
        timer_del(s->baro_timer);
    }
    if (hz == s->baro_rate) {
        return;
    }
    s->baro_rate = hz;
    if (!hz) {
        timer_del(s->baro_timer);
        return;
    }
    s->baro_next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND / hz;
    timer_mod(s->baro_timer, s->baro_next_ns);
}

/* end of a transaction: the measurement config may have changed */
static void baro_stop(void *opaque)
{
    baro_rate_update(opaque);
}

static void baro_sample(void *opaque)
{
    NexusXRState *s = opaque;
    /* the chip measures what the FDM says now (plus its own noise, in the model) */
    fb_dps310_set_environment(s->baro, (float)s->baro_pa, (float)s->baro_c);
    fb_dps310_sample(s->baro);
    s->baro_samples++;
    if (s->baro_oneshot) {
        s->baro_oneshot = false;        /* done: the chip is idle again (unless frozen) */
        return;
    }
    if (s->baro_rate) {
        s->baro_next_ns += NANOSECONDS_PER_SECOND / s->baro_rate;
        timer_mod(s->baro_timer, s->baro_next_ns);
    }
}

/* the environment at FDM altitude alt_m (above the field) */
static void baro_environment(NexusXRState *s, double alt_m)
{
    double h = MIN(s->baro_field_m + alt_m, 40000.0);
    double p = s->baro_p0 * pow(1.0 - 2.25577e-5 * h, 5.25588);
    double t = s->baro_t0 - 0.0065 * h;
    s->baro_pa = p;
    s->baro_c = t;
}

/* ---- the SPL06-001 baro on I2C1 (the F405 board) -------------------------------------------- */
/*
 * fb_spl06 keeps its own clock: before every transaction and when its next conversion is due
 * (one timer at fb_spl06_next_due_ns()) it is ticked to now with the environment the FDM gives
 * (baro_environment, the same formulas as the DPS310's: devices/include/fb/atmosphere.h). iNav
 * programs command mode at 8x: a pressure or a temperature conversion per 16 ms.
 */
static void spl06_sync(NexusXRState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    fb_spl06_set_environment(s->spl06, s->baro_pa, s->baro_c);
    if (now > s->spl06_last_ns) {
        fb_spl06_tick(s->spl06, now - s->spl06_last_ns);
        s->spl06_last_ns = now;
    }
    uint64_t due = fb_spl06_next_due_ns(s->spl06);
    if (due == UINT64_MAX) {
        timer_del(s->spl06_timer);
    } else {
        timer_mod(s->spl06_timer, now + (int64_t)MAX(due, 1));
    }
}

static void spl06_timer_cb(void *opaque)
{
    spl06_sync(opaque);
}

static bool spl06_start(void *opaque, bool read)
{
    NexusXRState *s = opaque;
    spl06_sync(s);
    return fb_spl06_i2c_start(s->spl06, read);
}

static bool spl06_write(void *opaque, uint8_t b)
{
    return fb_spl06_i2c_write_byte(((NexusXRState *)opaque)->spl06, b);
}

static uint8_t spl06_read(void *opaque)
{
    return fb_spl06_i2c_read_byte(((NexusXRState *)opaque)->spl06);
}

static void spl06_stop(void *opaque)
{
    spl06_sync(opaque);                 /* a MEAS_CTRL write starts a conversion: its deadline */
}

/* ---- the QMC5883L compass on the I2C controller (wired: the GPS module's) ------------------- */
/*
 * The earth field (mag_ned: declination, inclination, intensity from the wiring's props) turned
 * into the body frame by the FDM's attitude and into the chip's axes by its mount (the inverse of
 * the firmware's align_mag), handed to the chip at each transaction; the chip's own clock (its
 * ODR, DRDY) is ticked then too: nothing is observable between transactions, so no timer.
 */
static void fdm_quat(NexusXRState *s, double q[4])
{
    fb_fdm_state_t st;
    fb_fdm_state(s->fdm, &st);
    memcpy(q, st.quat, 4 * sizeof(double));
}

static void mag_sync(NexusXRState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    double q[4], frd[3];
    fdm_quat(s, q);
    fb_mag_ned_to_body(q, s->mag_ned, frd);
    fb_mag_body_to_chip(s->mag_mount, frd, s->mag_chip);
    fb_qmc5883l_set_field(s->mag, s->mag_chip);
    fb_qmc5883l_set_temp(s->mag, s->baro_c);
    if (now > s->mag_last_ns) {
        fb_qmc5883l_tick(s->mag, now - s->mag_last_ns);
        s->mag_last_ns = now;
    }
}

static bool mag_start(void *opaque, bool read)
{
    NexusXRState *s = opaque;
    mag_sync(s);
    return fb_qmc5883l_i2c_start(s->mag, read);
}

static bool mag_write(void *opaque, uint8_t b)
{
    return fb_qmc5883l_i2c_write_byte(((NexusXRState *)opaque)->mag, b);
}

static uint8_t mag_read(void *opaque)
{
    return fb_qmc5883l_i2c_read_byte(((NexusXRState *)opaque)->mag);
}

/* ---- device props on the command line: "<id>=<value>:<id>=<value>" (-M <dev>-props=) ------ */
/*
 * Checked against the device's descriptor (devices/, the manifest's props): known id, type,
 * range, enum option. The bench resolver writes these from a wiring entry's props. Returns
 * false with errp set.
 */
typedef struct {
    const fb_param_desc_t *d;
    double v;                           /* number / bool */
    char str[24];                       /* enum */
    bool given;
} DevProp;

static bool dev_props_parse(const char *dev, const char *text, DevProp *out, int max,
                            Error **errp)
{
    const fb_device_desc_t *desc = fb_device_find(dev);
    int n = 0;
    for (const fb_param_desc_t *p = desc ? desc->props : NULL; p && p->id && n < max; p++, n++) {
        out[n] = (DevProp) { .d = p, .v = p->def };
        if (p->def_str) {
            g_strlcpy(out[n].str, p->def_str, sizeof(out[n].str));
        }
    }
    if (n < max) {
        out[n].d = NULL;
    }
    if (!text || !*text) {
        return true;
    }
    g_auto(GStrv) kv = g_strsplit(text, ":", -1);
    for (int i = 0; kv[i]; i++) {
        char *eq = strchr(kv[i], '=');
        int k;
        if (!eq) {
            error_setg(errp, "%s-props: %s: need <prop>=<value>", dev, kv[i]);
            return false;
        }
        *eq = 0;
        for (k = 0; k < n && strcmp(out[k].d->id, kv[i]); k++) {
        }
        if (k == n) {
            error_setg(errp, "%s-props: %s has no prop %s", dev, dev, kv[i]);
            return false;
        }
        const fb_param_desc_t *p = out[k].d;
        const char *v = eq + 1;
        if (p->type == FB_PARAM_ENUM) {
            bool ok = false;
            for (int o = 0; p->options && p->options[o]; o++) {
                ok |= !strcmp(p->options[o], v);
            }
            if (!ok || strlen(v) >= sizeof(out[k].str)) {
                error_setg(errp, "%s-props: %s=%s: not one of its options", dev, p->id, v);
                return false;
            }
            g_strlcpy(out[k].str, v, sizeof(out[k].str));
        } else if (p->type == FB_PARAM_BOOL) {
            if (strcmp(v, "true") && strcmp(v, "false")) {
                error_setg(errp, "%s-props: %s=%s: need true|false", dev, p->id, v);
                return false;
            }
            out[k].v = !strcmp(v, "true");
        } else {
            char *end;
            double x = g_ascii_strtod(v, &end);
            if (!*v || *end || !isfinite(x) || (p->type == FB_PARAM_INT && x != floor(x)) ||
                (p->has_range && (x < p->min || x > p->max))) {
                error_setg(errp, "%s-props: %s=%s: need %s %g..%g", dev, p->id, v,
                           p->type == FB_PARAM_INT ? "an integer" : "a number", p->min, p->max);
                return false;
            }
            out[k].v = x;
        }
        out[k].given = true;
    }
    return true;
}

static const DevProp *dev_prop(const DevProp *list, const char *id)
{
    for (; list->d; list++) {
        if (!strcmp(list->d->id, id)) {
            return list;
        }
    }
    g_assert_not_reached();
}

static void dev_props_or_exit(const char *dev, const char *text, DevProp *out, int max)
{
    Error *err = NULL;
    if (!dev_props_parse(dev, text, out, max, &err)) {
        error_report("%s: %s", fb_mname, error_get_pretty(err));
        exit(1);
    }
}

/* ---- the u-blox GPS on a USART (an in-process wire, as the receiver's) ----------------------- */
/*
 * The bytes the FC transmits go to fb_ublox_rx_byte(); the receiver's own bytes enter the USART
 * at its baud, one per byte time when RDR is free (else the next byte time: the firmware's RX
 * interrupt drains RDR well within one); between bursts one deadline per navigation epoch
 * (fb_ublox_next_due_ns). Every BRR write tells the receiver the FC's baud (iNav's autobaud
 * reaches it only at its own baud). The FDM's NED position and velocity go in every physics
 * tick, around the env `home` (default Yerevan, 40.1792 N 44.4991 E, the baro's field elevation).
 */
static void gps_arm(NexusXRState *s, int64_t now)
{
    uint64_t d = fb_ublox_tx_pending(s->gps) ? fb_ublox_byte_ns(s->gps) : fb_ublox_next_due_ns(s->gps);
    timer_mod(s->gps_timer, now + (int64_t)MAX(MIN(d, (uint64_t)NANOSECONDS_PER_SECOND), 1000));
}

static void gps_tick(void *opaque)
{
    NexusXRState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now > s->gps_last_ns) {
        fb_ublox_tick(s->gps, now - s->gps_last_ns);
        s->gps_last_ns = now;
    }
    if (fb_ublox_tx_pending(s->gps) && qemu_chr_be_can_write(s->gps_wire)) {
        uint8_t b;
        if (fb_ublox_tx_pop(s->gps, &b, 1) == 1) {
            qemu_chr_be_write(s->gps_wire, &b, 1);
        }
    }
    gps_arm(s, now);
}

static void gps_wire_tx(void *opaque, const uint8_t *buf, int len)
{
    NexusXRState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now > s->gps_last_ns) {
        fb_ublox_tick(s->gps, now - s->gps_last_ns);
        s->gps_last_ns = now;
    }
    for (int i = 0; i < len; i++) {
        fb_ublox_rx_byte(s->gps, buf[i]);
    }
    if (fb_ublox_tx_pending(s->gps) && !timer_pending(s->gps_timer)) {
        gps_arm(s, now);
    } else if (fb_ublox_tx_pending(s->gps) &&
               timer_expire_time_ns(s->gps_timer) > now + (int64_t)fb_ublox_byte_ns(s->gps)) {
        gps_arm(s, now);                /* an answer: its first byte one byte time from now */
    }
}

/* the GPS's USART: BRR writes tell the receiver the FC's baud */
typedef struct GpsUsart {
    NexusXRState *s;
    const MemoryRegionOps *ops;
    void *opaque;
    MemoryRegionOps w;
    uint64_t pclk_hz;
} GpsUsart;

static void gps_usart_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    GpsUsart *u = opaque;
    u->ops->write(u->opaque, addr, val, size);
    if (u->s->soc->usart_model == FB_USART_F2XX && addr == USART_BRR) {
        /* OVER8 = 0: baud = PCLK / (16 x USARTDIV) = PCLK / BRR */
        uint32_t brr = val & 0xFFFF;
        fb_ublox_set_fc_baud(u->s->gps, brr ? (uint32_t)(u->pclk_hz / brr) : 0);
    }
}

static uint64_t gps_usart_read(void *opaque, hwaddr addr, unsigned size)
{
    GpsUsart *u = opaque;
    return u->ops->read(u->opaque, addr, size);
}

static void gps_usart_hook(NexusXRState *s, MemoryRegion *mr, uint64_t pclk_hz)
{
    GpsUsart *u = g_new0(GpsUsart, 1);
    assert(mr->ops->read && mr->ops->write && !mr->ops->valid.accepts);
    if (s->soc->usart_model != FB_USART_F2XX) {
        warn_report("%s: gps: the BRR tap knows the F2xx USART only: every baud passes", fb_mname);
    }
    u->s = s;
    u->ops = mr->ops;
    u->opaque = mr->opaque;
    u->pclk_hz = pclk_hz;
    u->w = *mr->ops;
    u->w.read = gps_usart_read;
    u->w.write = gps_usart_write;
    mr->ops = &u->w;
    mr->opaque = u;
}

/* the FDM's position and velocity (NED, m around home) for the receiver: every physics tick */
static void gps_motion(NexusXRState *s, const double pos[3], const double vel[3])
{
    if (s->gps_uart >= 0) {
        fb_ublox_set_motion(s->gps, pos, vel);
    }
}

/* ---- ADC1 + DMA2 stream 0: the battery meter (the F405 board) -------------------------------- */
/*
 * RM0090 13, as iNav's drivers/adc_stm32f4xx.c uses it: the regular sequence (SQR1..3, length
 * SQR1.L + 1) in scan + continuous mode with DMA (CR2.DMA, DDS), started by CR2.SWSTART; DMA2
 * stream 0 (channel 0) moves DR to the firmware's buffer circularly: NDTR half-words = the
 * channels x ADC_AVERAGE_N_SAMPLES (20). The conversions are not run one by one in virtual time:
 * whenever what a pin reads changes (the battery, every physics tick) or the stream / sequence is
 * (re)programmed, the whole buffer is rewritten with every channel's code, which is what the
 * circular transfer converges to (the firmware averages the 20 samples of each channel). Codes:
 * 12 bit of 3.3 V; VBAT = the pack through the board's divider, CURRENT = the sensor's mV/A;
 * everything else (RSSI, AIRSPEED, internal channels) 0 V. No battery wired: 0 V everywhere
 * (the FC then reports no battery). SR reads EOC | STRT while running; DR the last rank's code;
 * ADC2/ADC3 and the common registers are storage.
 */
#define ADC_SR      0x00
#define ADC_CR2     0x08
#define ADC_SQR1    0x2C
#define ADC_SQR2    0x30
#define ADC_SQR3    0x34
#define ADC_DR      0x4C
#define ADC_CR2_ADON    (1u << 0)
#define ADC_CR2_DMA     (1u << 8)
#define ADC_CR2_SWSTART (1u << 30)

static int adc_seq_len(NexusXRState *s)
{
    return ((s->adc_reg[ADC_SQR1 / 4] >> 20) & 0xF) + 1;
}

/* the channel converted at rank r (0-based) */
static int adc_rank_ch(NexusXRState *s, int r)
{
    uint32_t sqr = r < 6 ? s->adc_reg[ADC_SQR3 / 4] : r < 12 ? s->adc_reg[ADC_SQR2 / 4]
                                                             : s->adc_reg[ADC_SQR1 / 4];
    return (sqr >> (5 * (r % 6))) & 0x1F;
}

static uint16_t adc_ch_code(NexusXRState *s, int ch)
{
    return ch < (int)ARRAY_SIZE(s->adc_code) ? s->adc_code[ch] : 0;
}

/* the pins' codes now; true when one changed */
static bool adc_update_codes(NexusXRState *s)
{
    double vb = 0, ia = 0;
    if (s->battery_present) {
        vb = fb_battery_sense_v(s->batt);
        ia = fb_battery_sense_a(s->batt);
    }
    double pin_v = vb / s->board->vbat_divider, pin_i = ia * s->board->curr_mv_per_a / 1000.0;
    uint16_t cv = (uint16_t)MIN(MAX(lround(pin_v / 3.3 * 4095), 0), 4095);
    uint16_t ci = (uint16_t)MIN(MAX(lround(pin_i / 3.3 * 4095), 0), 4095);
    bool chg = s->adc_code[s->board->adc_vbat_ch] != cv || s->adc_code[s->board->adc_curr_ch] != ci;
    s->adc_code[s->board->adc_vbat_ch] = cv;
    s->adc_code[s->board->adc_curr_ch] = ci;
    return chg;
}

/* the DMA stream moving ADC1->DR, enabled; -1 = none */
static int adc_dma_stream(NexusXRState *s)
{
    for (int n = 0; n < 8; n++) {
        DmaStream *st = &s->dma2[n];
        if ((st->cr & 1) && st->par == ADC_BASE + ADC_DR && ((st->cr >> 6) & 3) == 0) {
            return n;
        }
    }
    return -1;
}

/* rewrite the firmware's buffer when the codes or the configuration changed */
static void adc_refresh(NexusXRState *s)
{
    if (!s->board->adc) {
        return;
    }
    s->adc_dirty |= adc_update_codes(s);
    uint32_t cr2 = s->adc_reg[ADC_CR2 / 4];
    int n = adc_dma_stream(s);
    if (!s->adc_dirty || !s->adc_running || !(cr2 & ADC_CR2_ADON) || !(cr2 & ADC_CR2_DMA) || n < 0) {
        return;
    }
    DmaStream *st = &s->dma2[n];
    unsigned msize = 1u << ((st->cr >> 13) & 3), cnt = st->ndtr & 0xFFFF, len = adc_seq_len(s);
    bool minc = st->cr & (1u << 10);
    if (!cnt || msize > 4) {
        return;
    }
    g_autofree uint8_t *buf = g_malloc0((size_t)cnt * msize);
    for (unsigned i = 0; i < cnt; i++) {
        uint32_t v = adc_ch_code(s, adc_rank_ch(s, i % len));
        memcpy(buf + (size_t)i * msize, &v, msize);     /* little-endian host and target */
    }
    address_space_write(&address_space_memory, st->m0ar, MEMTXATTRS_UNSPECIFIED, buf,
                        minc ? (size_t)cnt * msize : msize);
    s->adc_dirty = false;
    s->adc_fills++;
}

static uint64_t adc_read(void *opaque, hwaddr off, unsigned size)
{
    NexusXRState *s = opaque;
    idle_touch(s);
    hwaddr a = off & ~3;
    uint32_t v = s->adc_reg[a / 4];
    if (a == ADC_SR && s->adc_running) {
        v |= (1u << 1) | (1u << 4);                     /* EOC, STRT */
    } else if (a == ADC_DR) {
        v = s->adc_running ? adc_ch_code(s, adc_rank_ch(s, adc_seq_len(s) - 1)) : 0;
    }
    return extract32(v, (off & 3) * 8, size * 8);
}

static void adc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    NexusXRState *s = opaque;
    idle_touch(s);
    hwaddr a = off & ~3;
    uint32_t v = deposit32(s->adc_reg[a / 4], (off & 3) * 8, size * 8, val);
    if (a == ADC_SR) {
        v = s->adc_reg[a / 4] & deposit32(0xFFFFFFFF, (off & 3) * 8, size * 8, val);   /* rc_w0 */
    }
    if (a == ADC_CR2) {
        if (v & ADC_CR2_SWSTART) {
            s->adc_running = v & ADC_CR2_ADON;          /* continuous: runs until ADON = 0 */
            v &= ~ADC_CR2_SWSTART;                      /* cleared by hardware at the start */
        }
        if (!(v & ADC_CR2_ADON)) {
            s->adc_running = false;
        }
    }
    if (a == ADC_DR) {
        return;                                         /* read-only */
    }
    s->adc_reg[a / 4] = v;
    if (a < 0x100) {
        s->adc_dirty = true;                            /* ADC1 reprogrammed */
        adc_refresh(s);
    }
}

static const MemoryRegionOps adc_ops = {
    .read = adc_read,
    .write = adc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void adc_reset(NexusXRState *s)
{
    memset(s->adc_reg, 0, sizeof(s->adc_reg));
    s->adc_running = false;
    s->adc_dirty = true;
}

/* the battery: the aircraft's current (the ESC's, scaled, + the electronics), every physics tick */
static void battery_tick(NexusXRState *s, double dt)
{
    if (s->battery_present) {
        const fb_battery_config_t *c = fb_battery_config(s->batt);
        double i = c->current_scale * fb_esc_current_a(s->esc) + (s->power_off ? 0 : c->fc_current_a);
        fb_battery_step(s->batt, i, dt);
    }
    adc_refresh(s);
}

/* ------------------------------------------------------------------ */
/* DMA2: streams move bytes between memory and SPI1                   */
/*
 * RotorFlight always allocates SPI DMA when a stream is free (dmaopt -1
 * means "try every option", there is no config switch to turn it off) and
 * after 100 gyro interrupts reads the IMU by EXTI -> DMA. Only what that
 * needs is modelled: a stream pair with PAR = SPI1_DR and the SPI's
 * TXDMAEN/RXDMAEN set moves NDTR bytes through spi1_exchange() at once;
 * completion (EN cleared, HTIF|TCIF, IRQ) comes after the SPI bit time.
 * Other streams (the ADC's) are plain registers, as before.
 */

static const unsigned dma_flag_shift[4] = { 0, 6, 16, 22 };

static uint32_t dma2_flags(NexusXRState *s, int n)
{
    return (s->dma2_isr[n / 4] >> dma_flag_shift[n % 4]) & 0x3F;
}

static void dma2_update_irq(NexusXRState *s, int n)
{
    uint32_t f = dma2_flags(s, n);
    uint32_t cr = s->dma2[n].cr;
    bool lvl = ((f & (1u << 5)) && (cr & (1u << 4))) ||   /* TC */
               ((f & (1u << 4)) && (cr & (1u << 3))) ||   /* HT */
               ((f & (1u << 3)) && (cr & (1u << 2))) ||   /* TE */
               ((f & (1u << 2)) && (cr & (1u << 1))) ||   /* DME */
               ((f & (1u << 0)) && (s->dma2[n].fcr & (1u << 7)));
    qemu_set_irq(s->dma2_irq[n], lvl);
}

static int dma2_find(NexusXRState *s, unsigned dir)
{
    for (int n = 0; n < 8; n++) {
        DmaStream *st = &s->dma2[n];
        if ((st->cr & 1) && !st->busy && ((st->cr >> 6) & 3) == dir &&
            st->par == SPI1_BASE + 0x0C) {
            return n;
        }
    }
    return -1;
}

static void dma2_done(void *opaque)
{
    DmaStream *st = opaque;
    NexusXRState *s = st->s;
    int n = st - s->dma2;

    st->busy = false;
    st->cr &= ~1u;
    st->ndtr = 0;
    s->dma2_isr[n / 4] |= ((1u << 5) | (1u << 4)) << dma_flag_shift[n % 4];
    dma2_update_irq(s, n);
    if (st->par == SPI1_BASE + 0x0C) {
        imu_trace(s, 'D', qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), n);
    }
}

static void dma2_try_start(NexusXRState *s)
{
    bool txen = s->spi[0].cr2 & (1u << 1), rxen = s->spi[0].cr2 & (1u << 0);
    if (!txen || !(s->spi[0].cr1 & (1u << 6))) {
        return;
    }
    int tx = dma2_find(s, 1);                   /* memory -> peripheral */
    int rx = rxen ? dma2_find(s, 0) : -1;       /* peripheral -> memory */
    if (tx < 0 || (rxen && rx < 0)) {
        return;                                 /* wait for the other stream */
    }
    DmaStream *t = &s->dma2[tx], *r = rx >= 0 ? &s->dma2[rx] : NULL;
    uint32_t len = t->ndtr & 0xFFFF;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t mosi, miso;
        hwaddr ta = t->m0ar + ((t->cr & (1u << 10)) ? i : 0);
        address_space_read(&address_space_memory, ta, MEMTXATTRS_UNSPECIFIED,
                           &mosi, 1);
        miso = spi_exchange(&s->spi[0], mosi);
        if (r) {
            hwaddr ra = r->m0ar + ((r->cr & (1u << 10)) ? i : 0);
            address_space_write(&address_space_memory, ra,
                                MEMTXATTRS_UNSPECIFIED, &miso, 1);
        } else if (spi1_f4(s)) {
            spi1_f4_rx(&s->spi[0], miso);
        } else {
            spi1_rx_push(&s->spi[0], miso);
        }
    }

    /* SPI1 on APB2: f = PCLK2 / 2^(BR+1) */
    uint64_t hz = s->soc->pclk2_hz >> (((s->spi[0].cr1 >> 3) & 7) + 1);
    int64_t ns = muldiv64(len * 8, NANOSECONDS_PER_SECOND, hz);
    int64_t done = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns;
    t->busy = true;
    timer_mod(t->timer, done);
    if (r) {
        r->busy = true;
        timer_mod(r->timer, done);
    }
}

static uint64_t dma2_read(void *opaque, hwaddr off, unsigned size)
{
    NexusXRState *s = opaque;
    idle_touch(s);
    hwaddr a = off & ~3;
    uint32_t v = 0;

    if (a < 0x10) {
        v = a < 0x08 ? s->dma2_isr[a / 4] : 0;
    } else if (a < 0x10 + 8 * 0x18) {
        DmaStream *st = &s->dma2[(a - 0x10) / 0x18];
        switch ((a - 0x10) % 0x18) {
        case 0x00: v = st->cr; break;
        case 0x04: v = st->ndtr; break;
        case 0x08: v = st->par; break;
        case 0x0C: v = st->m0ar; break;
        case 0x10: v = st->m1ar; break;
        case 0x14: v = st->fcr; break;
        }
    }
    return extract32(v, (off & 3) * 8, size * 8);
}

static void dma2_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    NexusXRState *s = opaque;
    idle_touch(s);
    hwaddr a = off & ~3;
    int sh = (off & 3) * 8, bits = size * 8;

    if (a == 0x08 || a == 0x0C) {               /* LIFCR / HIFCR */
        int hi = a == 0x0C;
        s->dma2_isr[hi] &= ~deposit32(0, sh, bits, val);
        for (int n = hi * 4; n < hi * 4 + 4; n++) {
            dma2_update_irq(s, n);
        }
        return;
    }
    if (a < 0x10 || a >= 0x10 + 8 * 0x18) {
        return;
    }
    int n = (a - 0x10) / 0x18;
    DmaStream *st = &s->dma2[n];
    switch ((a - 0x10) % 0x18) {
    case 0x00: {
        uint32_t old = st->cr;
        st->cr = deposit32(st->cr, sh, bits, val);
        if ((old & 1) && !(st->cr & 1) && st->busy) {
            timer_del(st->timer);               /* disabled mid-transfer */
            st->busy = false;
        }
        if (!(old & 1) && (st->cr & 1)) {
            dma2_try_start(s);
            dma2_timer_check(s);
            if (st->par == ADC_BASE + ADC_DR) {
                s->adc_dirty = true;            /* the ADC's circular stream (re)started */
                adc_refresh(s);
            }
        }
        break;
    }
    case 0x04: st->ndtr = deposit32(st->ndtr, sh, bits, val) & 0xFFFF; break;
    case 0x08: st->par = deposit32(st->par, sh, bits, val); break;
    case 0x0C: st->m0ar = deposit32(st->m0ar, sh, bits, val); break;
    case 0x10: st->m1ar = deposit32(st->m1ar, sh, bits, val); break;
    case 0x14: st->fcr = deposit32(st->fcr, sh, bits, val); break;
    }
    dma2_update_irq(s, n);
}

static const MemoryRegionOps dma2_ops = {
    .read = dma2_read,
    .write = dma2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* ------------------------------------------------------------------ */
/* IMU: ICM-42688-P (devices/src/icm42688p.c) on SPI1, CS PA4, INT1 PB8 */
/*
 * The register logic lives in the device library; here are only the
 * wires: chip select edges, the sample clock at the programmed ODR (QEMU
 * virtual time), the INT1 pulse, and the motion source feeding the chip.
 *
 * Motion source (chip frame): a base rate/specific force/temperature, set
 * at runtime through the "fbmotion" chardev (one JSON object per line),
 * plus Gaussian noise per sample and a slow sinusoidal temperature drift.
 * Default = hover: 0 deg/s, +1 g on chip Z, 30 degC +-1 over 10 minutes.
 * Noise matters: the firmware's calibration needs realistic data.
 */

static uint64_t rng_next(NexusXRState *s)
{
    /* xorshift64*: deterministic, so icount runs are reproducible */
    s->rng ^= s->rng >> 12;
    s->rng ^= s->rng << 25;
    s->rng ^= s->rng >> 27;
    return s->rng * 0x2545F4914F6CDD1DULL;
}

static double rng_gauss(NexusXRState *s)
{
    if (s->gauss_have) {
        s->gauss_have = false;
        return s->gauss_next;
    }
    double u1 = ((rng_next(s) >> 11) + 1.0) / 9007199254740993.0;
    double u2 = (rng_next(s) >> 11) / 9007199254740992.0;
    double r = sqrt(-2.0 * log(u1));
    s->gauss_next = r * sin(2 * G_PI * u2);
    s->gauss_have = true;
    return r * cos(2 * G_PI * u2);
}

static void motion_apply(NexusXRState *s)
{
    MotionSource *m = &s->motion;
    float g[3], a[3];
    double t = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9;

    for (int i = 0; i < 3; i++) {
        g[i] = m->gyro_dps[i] + m->gyro_noise_dps * rng_gauss(s);
        a[i] = m->accel_g[i] + m->accel_noise_g * rng_gauss(s);
        if (m->accel_error) {
            a[i] = a[i] * m->accel_gain + m->accel_offset_g;
        }
    }
    float temp = m->temp_c +
                 m->temp_swing_c * sin(2 * G_PI * t / m->temp_period_s);
    fb_icm42688p_set_motion(s->imu, g, a, temp);
    memcpy(s->imu_in_gyro, g, sizeof(g));
    memcpy(s->imu_in_accel, a, sizeof(a));
    s->imu_in_temp = temp;
}

static void imu_int1_update(NexusXRState *s)
{
    if (!s->imu_present || s->board->imu_int_port < 0) {
        return;                 /* no IMU (imu=off) or INT1 not wired: nobody drives it */
    }
    bool ah = fb_icm42688p_int1_active_high(s->imu);
    gpio_set_input(s, s->board->imu_int_port, s->board->imu_int_pin, s->imu_pulse ? ah : !ah);
}

static void imu_rate_update(NexusXRState *s)
{
    uint32_t odr = fb_icm42688p_odr_hz(s->imu);
    if (odr == s->imu_odr) {
        return;
    }
    s->imu_odr = odr;
    if (!odr) {
        timer_del(s->imu_sample_timer);
        return;
    }
    s->imu_next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                     NANOSECONDS_PER_SECOND / odr;
    timer_mod(s->imu_sample_timer, s->imu_next_ns);
}

static void imu_cs_changed(NexusXRState *s, bool level)
{
    bool asserted = !level;                     /* active low */
    if (!s->imu_present) {
        return;                 /* no IMU (imu=off): the CS line goes nowhere */
    }
    if (asserted == s->imu_cs) {
        return;
    }
    s->imu_cs = asserted;
    fb_icm42688p_cs(s->imu, asserted);
    if (!asserted) {
        /* end of a transaction: rate or INT1 config may have changed */
        imu_rate_update(s);
        if (!s->imu_pulse) {
            imu_int1_update(s);
        }
    }
}

/* IMU trace (imu-log=): "S <ns>" INT1 rises (a sample), "F <ns>" INT1 falls, "D <ns> <stream>"
 * an SPI1 DMA stream completes; virtual ns at which the firmware can see it */
static void imu_trace(NexusXRState *s, char ev, int64_t t, int arg)
{
    if (s->imu_log) {
        if (arg >= 0) {
            fprintf(s->imu_log, "%c %" PRId64 " %d\n", ev, t, arg);
        } else {
            fprintf(s->imu_log, "%c %" PRId64 "\n", ev, t);
        }
    }
}

static void imu_pulse_end(void *opaque)
{
    NexusXRState *s = opaque;
    s->imu_pulse = false;
    imu_int1_update(s);
    imu_trace(s, 'F', qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), -1);
}

static void imu_sample(void *opaque)
{
    NexusXRState *s = opaque;

    motion_apply(s);
    if (fb_icm42688p_sample(s->imu)) {
        s->imu_pulse = true;
        imu_int1_update(s);
        imu_trace(s, 'S', qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), -1);
        timer_mod(s->imu_pulse_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  fb_icm42688p_int1_pulse_ns(s->imu));
    }
    s->imu_samples++;
    if (s->imu_odr) {
        s->imu_next_ns += NANOSECONDS_PER_SECOND / s->imu_odr;
        timer_mod(s->imu_sample_timer, s->imu_next_ns);
    }
}

/* "fbmotion" chardev: {"gyro_dps":[x,y,z],"accel_g":[x,y,z],"temp_c":t,
 *  "gyro_noise_dps":n,"accel_noise_g":n,"temp_swing_c":n}, any subset,
 *  one object per line; replies "ok" or "error: ...". */
static bool json_vec3(QDict *d, const char *key, float out[3], Error **errp)
{
    QList *l = qdict_get_qlist(d, key);
    if (!qdict_haskey(d, key)) {
        return true;
    }
    if (!l || qlist_size(l) != 3) {
        error_setg(errp, "%s: need 3 numbers", key);
        return false;
    }
    float v[3];
    int i = 0;
    for (const QListEntry *e = qlist_first(l); e; e = qlist_next(e), i++) {
        QNum *n = qobject_to(QNum, qlist_entry_obj(e));
        if (!n) {
            error_setg(errp, "%s: need 3 numbers", key);
            return false;
        }
        v[i] = qnum_get_double(n);
    }
    memcpy(out, v, sizeof(v));
    return true;
}

static bool json_num(QDict *d, const char *key, float *out, Error **errp)
{
    if (!qdict_haskey(d, key)) {
        return true;
    }
    QNum *n = qobject_to(QNum, qdict_get(d, key));
    if (!n) {
        error_setg(errp, "%s: need a number", key);
        return false;
    }
    *out = qnum_get_double(n);
    return true;
}

/* ------------------------------------------------------------------ */
/* Non-blocking output to socket clients                               */

static void outq_init(OutQ *q, CharFrontend *fe, const char *name, unsigned cap, bool whole)
{
    q->fe = fe;
    q->name = name;
    q->cap = cap;
    q->whole = whole;
    q->buf = g_malloc(cap);
    g_autofree char *an = g_ascii_strdown(name, -1);
    g_strdelimit(an, " ", '_');
    g_autofree char *acct = g_strdup_printf("%s.out", an);
    q->acct = prof_acct(acct, FBP_CHARDEV);
}

static void outq_clear(OutQ *q)
{
    q->head = q->len = 0;
    if (q->watch) {
        g_source_remove(q->watch);
        q->watch = 0;
    }
}

static gboolean outq_writable(void *do_not_use, GIOCondition cond, void *opaque);

static void outq_flush_body(OutQ *q);

static void outq_flush(OutQ *q)
{
    PROF_ENTER(q->acct);
    outq_flush_body(q);
    PROF_LEAVE(q->acct);
}

static void outq_flush_body(OutQ *q)
{
    while (q->len) {
        unsigned chunk = MIN(q->len, q->cap - q->head);
        int w = qemu_chr_fe_write(q->fe, q->buf + q->head, chunk);
        if (w < 0 && errno != EAGAIN) {
            outq_clear(q);                  /* client gone: nothing to deliver to */
            return;
        }
        if (w <= 0) {
            break;                          /* socket full: wait for G_IO_OUT */
        }
        q->head = (q->head + w) % q->cap;
        q->len -= w;
        PROF_BYTES(q->acct, w);
    }
    if (q->len && !q->watch) {
        q->watch = qemu_chr_fe_add_watch(q->fe, G_IO_OUT | G_IO_HUP, outq_writable, q);
        if (!q->watch) {
            outq_clear(q);                  /* disconnected */
        }
    }
    if (!q->len && q->burst) {
        fprintf(stderr, "%s: %s: client reads again; %" PRIu64 " bytes were dropped "
                "(%" PRIu64 " in total)\n", fb_mname, q->name, q->burst, q->dropped);
        q->burst = 0;
    }
}

static gboolean outq_writable(void *do_not_use, GIOCondition cond, void *opaque)
{
    OutQ *q = opaque;
    q->watch = 0;
    outq_flush(q);
    return G_SOURCE_REMOVE;
}

static void outq_write(OutQ *q, const uint8_t *buf, int len)
{
    if (!q->buf || len <= 0 || !qemu_chr_fe_backend_connected(q->fe)) {
        return;
    }
    unsigned n = MIN((unsigned)len, q->cap - q->len);
    if (q->whole && n < (unsigned)len) {
        n = 0;                              /* a message is delivered whole or not at all */
    }
    for (unsigned i = 0; i < n; i++) {
        q->buf[(q->head + q->len + i) % q->cap] = buf[i];
    }
    q->len += n;
    if (n < (unsigned)len) {
        if (!q->burst) {
            fprintf(stderr, "%s: %s: client does not read, output buffer (%u bytes) "
                    "full: dropping\n", fb_mname, q->name, q->cap);
        }
        q->burst += len - n;
        q->dropped += len - n;
    }
    outq_flush(q);
}

/* a new client does not get what was queued for the previous one */
static void outq_chr_event(OutQ *q, QEMUChrEvent ev)
{
    if (ev == CHR_EVENT_OPENED || ev == CHR_EVENT_CLOSED) {
        if (q->burst) {
            fprintf(stderr, "%s: %s: client gone; %" PRIu64 " bytes were dropped "
                    "(%" PRIu64 " in total)\n", fb_mname, q->name, q->burst, q->dropped);
            q->burst = 0;
        }
        outq_clear(q);
    }
}

static void motion_event(void *opaque, QEMUChrEvent ev)
{
    outq_chr_event(&((NexusXRState *)opaque)->motion_out, ev);
}

static void motion_line(NexusXRState *s, const char *line)
{
    Error *err = NULL;
    MotionSource m = s->motion;
    QObject *o = qobject_from_json(line, &err);
    QDict *d = o ? qobject_to(QDict, o) : NULL;

    if (!err && !d) {
        error_setg(&err, "need a JSON object");
    }
    if (!err && json_vec3(d, "gyro_dps", m.gyro_dps, &err) &&
        json_vec3(d, "accel_g", m.accel_g, &err) &&
        json_num(d, "temp_c", &m.temp_c, &err) &&
        json_num(d, "gyro_noise_dps", &m.gyro_noise_dps, &err) &&
        json_num(d, "accel_noise_g", &m.accel_noise_g, &err) &&
        json_num(d, "temp_swing_c", &m.temp_swing_c, &err)) {
        s->motion = m;
    }
    qobject_unref(o);
    if (err) {
        g_autofree char *msg = g_strdup_printf("error: %s\n",
                                               error_get_pretty(err));
        outq_write(&s->motion_out, (uint8_t *)msg, strlen(msg));
        error_free(err);
    } else {
        outq_write(&s->motion_out, (uint8_t *)"ok\n", 3);
    }
}

static int motion_can_read(void *opaque)
{
    return 256;
}

static void motion_read(void *opaque, const uint8_t *buf, int size)
{
    NexusXRState *s = opaque;
    PROF_ENTER(pa.motion_rx);
    PROF_BYTES(pa.motion_rx, size);
    for (int i = 0; i < size; i++) {
        char c = buf[i];
        if (c == '\n' || c == '\r') {
            s->motion_buf[s->motion_len] = 0;
            if (s->motion_len) {
                motion_line(s, s->motion_buf);
            }
            s->motion_len = 0;
        } else if (s->motion_len < sizeof(s->motion_buf) - 1) {
            s->motion_buf[s->motion_len++] = c;
        }
    }
    PROF_LEAVE(pa.motion_rx);
}

/* ------------------------------------------------------------------ */
/* Timers TIM1/2/3: registers + a counter running in virtual time      */
/*
 * Enough for what the RS7 outputs need: the servo PWM compare values and
 * prescalers (pulse width = CCR * (PSC+1) / 216 MHz), TIM1 CH2 driving
 * DShot through DMA (CCx DMA requests), and the counter value the input
 * capture of the bidirectional-DShot reply timestamps edges with. No
 * interrupts, no pin output, ARR/PSC preload is not modelled (writes take
 * effect at once; the firmware's configuration sequences don't care).
 */

typedef struct NxTimer NxTimer;

static uint32_t tim_reg(NxTimer *t, unsigned off)
{
    return t->r[off / 4];
}

/* the timer's description (soc->tim) and its kernel clock */
static const FbSocTim *tim_desc(NxTimer *t)
{
    return &t->s->soc->tim[t - t->s->tim];
}

static uint64_t tim_clk(NxTimer *t)
{
    return t->s->soc->tim_clk_hz[tim_desc(t)->bus];
}

static uint32_t tim_cnt(NxTimer *t)
{
    uint32_t top = tim_reg(t, TIM_ARR);
    if (!tim_desc(t)->is32) {
        top &= 0xFFFF;
    }
    if (!(tim_reg(t, TIM_CR1) & 1) || !top) {
        return t->cnt0;
    }
    uint64_t hz = tim_clk(t) / ((tim_reg(t, TIM_PSC) & 0xFFFF) + 1);
    int64_t dt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - t->t0;
    uint64_t ticks = t->cnt0 + muldiv64(dt, hz, NANOSECONDS_PER_SECOND);
    return ticks % ((uint64_t)top + 1);
}

static void tim_rebase(NxTimer *t, uint32_t cnt)
{
    t->cnt0 = cnt;
    t->t0 = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

/* Timer tick rate in Hz */
static uint64_t tim_hz(NxTimer *t)
{
    return tim_clk(t) / ((tim_reg(t, TIM_PSC) & 0xFFFF) + 1);
}

/* Channel ch (1..4) configured as input capture (CCxS != 0) */
static bool tim_ch_input(NxTimer *t, int ch)
{
    uint32_t ccmr = tim_reg(t, ch <= 2 ? TIM_CCMR1 : TIM_CCMR2);
    return ((ccmr >> (ch & 1 ? 0 : 8)) & 3) != 0;
}

static bool tim_ch_dma(NxTimer *t, int ch)
{
    return (tim_reg(t, TIM_DIER) >> (8 + ch)) & 1;      /* CCxDE */
}

/* Output o (a board pad) carries pulses: the timer drives the channel and the pad is in the
 * timer's alternate function (fb_outputs.c.inc) */
static bool tim_out_on(NexusXRState *s, const FbOutput *o)
{
    NxTimer *t = &s->tim[o->tim];
    const uint32_t *g = s->gpio[o->port];
    return fb_out_tim_on(tim_reg(t, TIM_CR1), tim_reg(t, TIM_CCER), tim_reg(t, TIM_BDTR),
                         s->soc->tim[o->tim].adv, o->ch, o->n) &&
           fb_out_pin_af(g[GPIO_MODER], g[GPIO_AFRL], g[GPIO_AFRH], o->pin, o->af);
}

/* Output pulse in nanoseconds, 0 when no pulse reaches the pad */
static uint64_t tim_pulse_ns(NexusXRState *s, const FbOutput *o)
{
    NxTimer *t = &s->tim[o->tim];
    if (!tim_out_on(s, o)) {
        return 0;
    }
    uint64_t ccr = tim_reg(t, TIM_CCR1 + 4 * (o->ch - 1));
    return muldiv64(ccr * ((tim_reg(t, TIM_PSC) & 0xFFFF) + 1), NANOSECONDS_PER_SECOND, tim_clk(t));
}

/* Servo output pulse in microseconds, 0 when no pulse reaches the pad */
static unsigned tim_pulse_us(NexusXRState *s, const FbOutput *o)
{
    NxTimer *t = &s->tim[o->tim];
    if (!tim_out_on(s, o)) {
        return 0;
    }
    uint64_t ccr = tim_reg(t, TIM_CCR1 + 4 * (o->ch - 1));
    return muldiv64(ccr, (tim_reg(t, TIM_PSC) & 0xFFFF) + 1, tim_clk(t) / 1000000);
}

static uint64_t tim_read(void *opaque, hwaddr off, unsigned size)
{
    NxTimer *t = opaque;
    idle_touch(t->s);
    uint32_t v = off / 4 < TIM_NREGS ? t->r[off / 4] : 0;
    if ((off & ~3) == TIM_CNT) {
        v = tim_cnt(t);
    } else if ((off & ~3) == TIM_EGR) {
        v = 0;
    }
    return extract32(v, (off & 3) * 8, size * 8);
}

static void tim_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    NxTimer *t = opaque;
    idle_touch(t->s);
    unsigned reg = off & ~3;
    if (reg / 4 >= TIM_NREGS) {
        return;
    }
    uint32_t v = deposit32(t->r[reg / 4], (off & 3) * 8, size * 8, val);
    uint32_t cur = tim_cnt(t);

    switch (reg) {
    case TIM_SR:            /* rc_w0 */
        t->r[reg / 4] &= deposit32(0xFFFFFFFF, (off & 3) * 8, size * 8, val);
        return;
    case TIM_EGR:
        if (v & 1) {        /* UG: counter restarts */
            tim_rebase(t, 0);
            t->r[TIM_SR / 4] |= 1;
        }
        return;
    case TIM_CNT:
        t->r[reg / 4] = v;
        tim_rebase(t, v);
        return;
    }
    t->r[reg / 4] = v;
    if (reg == TIM_CR1 || reg == TIM_PSC || reg == TIM_ARR) {
        tim_rebase(t, cur);
    }
    if (reg == TIM_DIER || reg == TIM_CCMR1 || reg == TIM_CCMR2 ||
        reg == TIM_CCER) {
        dma2_timer_check(t->s);
    }
}

static const MemoryRegionOps tim_ops = {
    .read = tim_read,
    .write = tim_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

/* PAR -> (timer, channel) when it points at a modelled timer's CCRx */
static bool tim_ccr_lookup(NexusXRState *s, uint32_t par, int *idx, int *ch)
{
    for (int i = 0; i < s->soc->ntim; i++) {
        hwaddr b = s->soc->tim[i].base;
        if (par >= b + TIM_CCR1 && par < b + TIM_CCR1 + 16 && !(par & 3)) {
            *idx = i;
            *ch = (par - b - TIM_CCR1) / 4 + 1;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* DShot on TIM1_CH2 through DMA2, bidirectional eRPM reply, ESC       */
/*
 * Output: a stream memory->TIMx_CCRy runs when the timer's CCyDE is set;
 * the whole buffer (16 bits + 2 reset slots) is taken at once, decoded
 * with fb_dshot_decode_ccr() against the timer period (ARR+1), handed to
 * the ESC model, and the stream completes after len bit periods (TC IRQ).
 * The firmware's TC handler turns the pin into an input capture with a
 * peripheral->memory stream on the same CCR; ESC_REPLY_NS later the ESC
 * model's eRPM goes out as GCR edges (fb_dshot_* chain, 5/4 of the DShot
 * bit rate = 4/5 of the output bit period in timer ticks), written as
 * capture timestamps, NDTR counting down per edge like the real DMA.
 */

/* Memory data size of a stream in bytes. MSIZE=3 is reserved: a misprogrammed guest gets a
 * guest error and word accesses (the buffers are uint32_t: 8 would overflow them). */
static unsigned dma2_msize(DmaStream *st, int n)
{
    unsigned m = (st->cr >> 13) & 3;
    if (m == 3) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DMA2 stream %d: MSIZE=3 is reserved, "
                      "using 32-bit\n", fb_mname, n);
        m = 2;
    }
    return 1u << m;
}

static void esc_reply(void *opaque)
{
    NexusXRState *s = opaque;
    int n = s->esc_reply_stream, idx, ch;
    DmaStream *st = &s->dma2[n];

    if (!(st->cr & 1) || ((st->cr >> 6) & 3) != 0 ||
        !tim_ccr_lookup(s, st->par, &idx, &ch)) {
        return;                         /* direction switched back already */
    }
    if (!fb_esc_telemetry_ok(s->esc)) {
        return;                         /* fault esc.telemetry_lost: the ESC stays silent */
    }
    NxTimer *t = &s->tim[idx];
    if (!tim_ch_input(t, ch) || !tim_ch_dma(t, ch) || !s->dshot_period) {
        return;
    }
    uint32_t edges[21];
    uint32_t bit_ticks = s->dshot_period * 4 / 5;
    uint32_t mask = s->soc->tim[idx].is32 ? 0xFFFFFFFF : 0xFFFF;
    uint16_t v16 = fb_dshot_erpm_to_value16(fb_esc_erpm(s->esc));
    int ne = fb_dshot_stream21_to_edges(fb_dshot_value16_to_stream21(v16),
                                        tim_cnt(t), bit_ticks, edges, 21);
    unsigned width = dma2_msize(st, n);
    for (int k = 0; k < ne && (st->ndtr & 0xFFFF); k++) {
        uint32_t v = edges[k] & mask;
        hwaddr a = st->m0ar + ((st->cr & (1u << 10)) ? (hwaddr)k * width : 0);
        address_space_write(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                            &v, width);
        t->r[(TIM_CCR1 + 4 * (ch - 1)) / 4] = v;
        st->ndtr--;
    }
    s->esc_replies++;
}

static void dshot_frame(NexusXRState *s, DmaStream *st, NxTimer *t, int n)
{
    uint32_t len = st->ndtr & 0xFFFF;
    uint32_t v[32] = { 0 };
    unsigned width = dma2_msize(st, n);
    uint32_t period = (tim_reg(t, TIM_ARR) & 0xFFFF) + 1;

    for (uint32_t i = 0; i < len && i < ARRAY_SIZE(v); i++) {
        hwaddr a = st->m0ar + ((st->cr & (1u << 10)) ? i * width : 0);
        address_space_read(&address_space_memory, a, MEMTXATTRS_UNSPECIFIED,
                           &v[i], width);
    }
    if (len) {
        int idx, ch;
        tim_ccr_lookup(s, st->par, &idx, &ch);
        t->r[(TIM_CCR1 + 4 * (ch - 1)) / 4] = v[MIN(len, ARRAY_SIZE(v)) - 1];
    }

    uint16_t value;
    bool tlm;
    if (!s->esc_present) {
        goto clocked;               /* no ESC on the pad: nothing decodes, nothing answers */
    }
    int fidx, fch;
    tim_ccr_lookup(s, st->par, &fidx, &fch);
    if (fidx != s->board->motor.tim || fch != s->board->motor.ch ||
        !tim_out_on(s, &s->board->motor)) {
        goto clocked;               /* the frame never reaches the ESC's pad: no signal */
    }
    if (len >= 16 && fb_dshot_decode_ccr(v, period, true, &value, &tlm)) {
        s->dshot_bidir = true;
    } else if (len >= 16 && fb_dshot_decode_ccr(v, period, false, &value, &tlm)) {
        s->dshot_bidir = false;
    } else {
        s->dshot_bad++;
        value = 0;
        tlm = false;
    }
    s->dshot_value = value;
    s->dshot_period = period;
    s->esc_reply_armed = s->dshot_bidir;        /* one reply per frame */
    s->dshot_frames++;
    fb_esc_set_dshot(s->esc, value, tlm);
    ctl_dshot(s, value);

clocked:;
    /* the stream completes after the frame has been clocked out */
    int64_t ns = muldiv64((uint64_t)len * period, NANOSECONDS_PER_SECOND, tim_hz(t));
    st->busy = true;
    timer_mod(st->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
}

/* Start whatever timer-driven stream became ready */
static void dma2_timer_check(NexusXRState *s)
{
    for (int n = 0; n < 8; n++) {
        DmaStream *st = &s->dma2[n];
        int idx, ch;
        if (!(st->cr & 1) || st->busy || !tim_ccr_lookup(s, st->par, &idx, &ch)) {
            continue;
        }
        NxTimer *t = &s->tim[idx];
        if (!tim_ch_dma(t, ch)) {
            continue;
        }
        unsigned dir = (st->cr >> 6) & 3;
        if (dir == 1 && !tim_ch_input(t, ch)) {
            dshot_frame(s, st, t, n);
        } else if (dir == 0 && tim_ch_input(t, ch) &&
                   idx == s->board->motor.tim && ch == s->board->motor.ch && s->esc_reply_armed) {
            s->esc_reply_armed = false;
            s->esc_reply_stream = n;
            timer_mod(s->esc_reply_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ESC_REPLY_NS);
        }
    }
}

/*
 * An ESC on a pulse output (board->motor_dshot false: iNav's default ONESHOT125 on the F405 WING's
 * S1, no DMA): every physics tick the pulse width is read from the timer's CCR and handed to the
 * ESC model as the equivalent DShot throttle (48..2047; 0 = stop at or below the protocol's
 * minimum pulse). The protocol is told by the pulse width: 1000..2000 us standard PWM, 125..250 us
 * OneShot125, 5..25 us Multishot. A stopped timer (MCU reset, power off) or a pad not in the
 * timer's alternate function (iNav without PWM_OUTPUT_ENABLE) sends nothing: the
 * ESC's signal-loss timeout stops the motor, as with DShot.
 */
static void esc_pulse(NexusXRState *s)
{
    uint64_t ns = tim_pulse_ns(s, &s->board->motor);
    double lo, span;

    if (!ns) {
        return;
    }
    if (ns >= 500000) {
        lo = 1000000, span = 1000000;       /* PWM */
    } else if (ns >= 60000) {
        lo = 125000, span = 125000;         /* OneShot125 */
    } else {
        lo = 5000, span = 20000;            /* Multishot */
    }
    s->motor_pulse_ns = ns;
    double f = MIN(((double)ns - lo) / span, 1.0);
    uint16_t value = f <= 0 ? 0 : 48 + (uint16_t)lrint(f * 1999);
    s->dshot_value = value;
    s->dshot_frames++;
    fb_esc_set_dshot(s->esc, value, false);
}

/* ------------------------------------------------------------------ */
/* CRSF receiver (devices/src/crsf.c) on UART5 and the pilot socket    */
/*
 * The USART's chardev is an in-process "wire": bytes the FC transmits go
 * to fb_crsf_rx_byte() (telemetry). The receiver's frames reach the USART as
 * a burst, with one virtual-time deadline per frame (crsf.frame, at the frame
 * time on the model's packet-rate grid), not one per 420 kbaud byte: the
 * first byte enters RDR at the frame time, each next one right after the
 * firmware's RX interrupt has consumed the previous one -- its handler
 * (serial_uart_hal.c uartIrqHandler) reads RDR and then writes RQR.RXFRQ,
 * and that write (crsf_usart_write) releases the next byte, never earlier,
 * so the flush cannot eat it. The RXFRQ write is only an early trigger: a
 * driver that never writes RXFRQ (polling, another HAL) gets each byte at
 * its 420 kbaud arrival time (frame time + n byte times; RDR still full then:
 * overrun, see crsf_arm), from the same timer -- it is armed at the next byte's
 * arrival while bytes are queued, and moved to the next frame once the
 * RXFRQ path has drained the queue, so an RXFRQ driver still costs one
 * deadline per frame. Bytes still queued at the next frame time were never
 * read (USART off, RX interrupt masked): dropped, as overrun would on the
 * wire. The firmware's CRSF parser only needs a frame to complete within
 * CRSF_TIME_NEEDED_PER_FRAME_US; the frame rate and content are unchanged.
 * The pilot sets the 16 channel values (microseconds) over
 * the "fbpilot" chardev, one JSON object per line:
 *   {"ch":[1500,1500,1000,...]}        first N channels, now
 *   {"at":12.5,"ch":[...]}             at 12.5 s virtual time (queued)
 *   {"set":{"6":2000}}                 single channels (1-based)
 *   {"link":false}                     receiver stops sending (failsafe)
 * Reply: "ok t=<virtual seconds>" or "error: ...".
 */

#define TYPE_CHARDEV_FBWIRE "chardev-fbwire"

typedef struct FbWireChardev {
    Chardev parent;
    void (*tx)(void *opaque, const uint8_t *buf, int len);  /* bytes the USART sent */
    void *opaque;
} FbWireChardev;

OBJECT_DECLARE_SIMPLE_TYPE(FbWireChardev, CHARDEV_FBWIRE)

static int fbwire_write(Chardev *chr, const uint8_t *buf, int len)
{
    FbWireChardev *w = CHARDEV_FBWIRE(chr);
    if (w->tx) {
        w->tx(w->opaque, buf, len);
    }
    return len;
}

static Chardev *fbwire_new(const char *id,
                           void (*tx)(void *, const uint8_t *, int), void *opaque)
{
    Chardev *chr = qemu_chardev_new(id, TYPE_CHARDEV_FBWIRE, NULL, NULL,
                                    &error_fatal);
    CHARDEV_FBWIRE(chr)->tx = tx;
    CHARDEV_FBWIRE(chr)->opaque = opaque;
    /* the machine pushes the input (a virtual timer checks qemu_chr_be_can_write): a USART's
     * accept_input has no main-loop watch to wake (patches/usart-accept-input.diff) */
    qemu_chr_set_feature(chr, QEMU_CHAR_FEATURE_INPUT_PUSHED);
    return chr;
}

static void crsf_wire_tx(void *opaque, const uint8_t *buf, int len)
{
    NexusXRState *s = opaque;
    PROF_ENTER(pa.crsf_fc_tx);
    PROF_BYTES(pa.crsf_fc_tx, len);
    for (int i = 0; i < len; i++) {
        fb_crsf_rx_byte(s->crsf, buf[i]);
    }
    PROF_LEAVE(pa.crsf_fc_tx);
}

/*
 * Host serial ports (-serial ...) reach their USART through a wire too, paced
 * at the byte time of 115200 baud. Without pacing a socket chardev refills
 * RDR the instant the firmware reads it, and the firmware's ISR, which
 * reads RDR and then writes RQR.RXFRQ, flushed the byte that had just
 * arrived: lost characters on the CLI, MSP out of sync for seconds. On the
 * chip the next byte is ~87 us away.
 */
#define HOST_UART_BYTE_NS   (10 * NANOSECONDS_PER_SECOND / 115200)

typedef struct HostUart {
    NexusXRState *s;
    int index;
    bool msp;                   /* the MSP/CLI host UART: the control server listens in */
    CharFrontend host;
    OutQ out;                   /* FC -> host */
    char out_name[24];
    Chardev *wire;
    uint8_t fifo[4096];         /* host (and control server) -> FC, paced */
    unsigned head, len;
    uint64_t in_dropped;
    QEMUTimer *timer;
    uint16_t acct_rx, acct_tx;  /* prof: host -> FIFO, FC -> host */
} HostUart;

static void host_uart_forward(HostUart *u, const uint8_t *buf, int len)
{
    outq_write(&u->out, buf, len);
}

static void host_uart_tx(void *opaque, const uint8_t *buf, int len)
{
    HostUart *u = opaque;
    PROF_ENTER(u->acct_tx);
    PROF_BYTES(u->acct_tx, len);
    if (u->msp && u->s->ctl) {
        ctl_fc_tx(u->s, buf, len);      /* the server filters its own MSP replies */
    } else {
        host_uart_forward(u, buf, len);
    }
    PROF_LEAVE(u->acct_tx);
}

static void host_uart_pump(void *opaque)
{
    HostUart *u = opaque;
    if (u->len && qemu_chr_be_can_write(u->wire)) {
        uint8_t b = u->fifo[u->head];
        u->head = (u->head + 1) % sizeof(u->fifo);
        u->len--;
        qemu_chr_be_write(u->wire, &b, 1);
        qemu_chr_fe_accept_input(&u->host);
    }
    if (u->len) {
        timer_mod(u->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + HOST_UART_BYTE_NS);
    }
}

static int host_uart_can_read(void *opaque)
{
    HostUart *u = opaque;
    if (u->msp && ctl_cfg_busy(u->s)) {
        return 0;               /* the config script owns the CLI: host bytes wait in the socket */
    }
    return sizeof(u->fifo) - u->len;
}

static unsigned host_uart_room(HostUart *u)
{
    return sizeof(u->fifo) - u->len;
}

/* bytes towards the firmware: from the host socket or the control server; returns how many
 * were accepted (the rest did not fit the FIFO) */
static int host_uart_inject(HostUart *u, const uint8_t *buf, int size)
{
    int i = 0;
    for (; i < size && u->len < sizeof(u->fifo); i++) {
        u->fifo[(u->head + u->len++) % sizeof(u->fifo)] = buf[i];
    }
    if (u->len && !timer_pending(u->timer)) {
        timer_mod(u->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + HOST_UART_BYTE_NS);
    }
    return i;
}

static void host_uart_read(void *opaque, const uint8_t *buf, int size)
{
    HostUart *u = opaque;
    PROF_ENTER(u->acct_rx);
    PROF_BYTES(u->acct_rx, size);
    /* can_read offered the free space, but the control server may have injected since */
    int k = host_uart_inject(u, buf, size);
    if (u->msp) {
        ctl_host_rx(u->s, buf, k);
    }
    if (k < size) {
        u->in_dropped += size - k;
        fprintf(stderr, "%s: USART%d: %d host bytes dropped, FIFO full (%" PRIu64
                " in total)\n", fb_mname, u->index + 1, size - k, u->in_dropped);
    }
    PROF_LEAVE(u->acct_rx);
}

static void host_uart_event(void *opaque, QEMUChrEvent ev)
{
    HostUart *u = opaque;
    outq_chr_event(&u->out, ev);
    if (u->msp && (ev == CHR_EVENT_OPENED || ev == CHR_EVENT_CLOSED)) {
        ctl_host_rx(u->s, NULL, -1);    /* no half host frame survives a (re)connect */
    }
}

/* All or nothing: a frame from the control server is never cut. False if it did not fit. */
static bool fc_inject(NexusXRState *s, const void *buf, int len)
{
    if (host_uart_room(s->msp_uart) < (unsigned)len) {
        return false;
    }
    return host_uart_inject(s->msp_uart, buf, len) == len;
}

/* bytes in the USART1 FIFO not yet delivered to the firmware */
static unsigned host_uart_pending(NexusXRState *s)
{
    return s->msp_uart->len;
}

static void fc_forward(NexusXRState *s, const uint8_t *buf, int len)
{
    host_uart_forward(s->msp_uart, buf, len);
}

static HostUart *host_uart_new(NexusXRState *s, int i, Chardev *host)
{
    HostUart *u = g_new0(HostUart, 1);
    g_autofree char *id = g_strdup_printf("fbuart%d-wire", i + 1);
    u->s = s;
    u->index = i;
    u->msp = i == s->board->msp_uart;
    u->wire = fbwire_new(id, host_uart_tx, u);
    g_autofree char *pump = g_strdup_printf("usart%d.pump", i + 1);
    g_autofree char *rx = g_strdup_printf("usart%d.host_rx", i + 1);
    g_autofree char *tx = g_strdup_printf("usart%d.fc_tx", i + 1);
    u->timer = nx_timer_new(pump, host_uart_pump, u);
    u->acct_rx = prof_acct(rx, FBP_CHARDEV);
    u->acct_tx = prof_acct(tx, FBP_CHARDEV);
    snprintf(u->out_name, sizeof(u->out_name), "USART%d host", i + 1);
    outq_init(&u->out, &u->host, u->out_name, 64 * KiB, false);
    if (host) {
        qemu_chr_fe_init(&u->host, host, &error_fatal);
        qemu_chr_fe_set_handlers(&u->host, host_uart_can_read, host_uart_read,
                                 host_uart_event, NULL, u, NULL, true);
    }
    return u;
}

static void fbwire_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);
    cc->chr_write = fbwire_write;
}

static void pilot_apply(NexusXRState *s, const struct PilotCmd *c)
{
    for (int i = 0; i < FB_CRSF_CHANNELS; i++) {
        if (c->ch_mask & (1u << i)) {
            s->pilot_ch[i] = c->ch[i];
        }
    }
    fb_crsf_set_channels_us(s->crsf, s->pilot_ch);
    if (c->link >= 0) {
        s->link_up = c->link;
        fb_crsf_set_link(s->crsf, s->link_rssi, s->link_lq, s->link_snr, c->link);
    }
    if (c->launch && s->env) {
        env_launch(s, &c->lp);
    }
}

/* the receiver's USART: its own handlers, plus the next byte after an RQR.RXFRQ write */
typedef struct CrsfUsart {
    NexusXRState *s;
    const MemoryRegionOps *ops;
    void *opaque;
    MemoryRegionOps w;
} CrsfUsart;

/* L4X5 (F7) layout: CR1 0x00 UE bit 0, RE bit 2; RQR 0x18 RXFRQ bit 3. F2XX (F4) layout: CR1 0x0C
 * UE bit 13, RE bit 2; DR 0x04 (stm32f2xx_usart.h) */
#define L4_RQR          0x18
#define L4_RQR_RXFRQ    (1u << 3)
#define L4_CR1          0x00
#define L4_CR1_UE_RE    ((1u << 0) | (1u << 2))
#define F2_CR1_UE_RE    (USART_CR1_UE | USART_CR1_RE)

/* the receiver's USART is enabled for reception */
static bool crsf_usart_rx_on(NexusXRState *s, CrsfUsart *u)
{
    if (s->soc->usart_model == FB_USART_F2XX) {
        return (u->ops->read(u->opaque, USART_CR1, 4) & F2_CR1_UE_RE) == F2_CR1_UE_RE;
    }
    return (u->ops->read(u->opaque, L4_CR1, 4) & L4_CR1_UE_RE) == L4_CR1_UE_RE;
}

/* one queued receiver byte into the USART, if RDR is free */
static void crsf_rx_feed(NexusXRState *s)
{
    if (fb_crsf_tx_pending(s->crsf) && qemu_chr_be_can_write(s->crsf_wire)) {
        uint8_t b;
        fb_crsf_tx_pop(s->crsf, &b, 1);
        qemu_chr_be_write(s->crsf_wire, &b, 1);
        s->crsf_fed++;
    }
}

/*
 * crsf.frame deadline: the next frame, a queued pilot command, or (bytes queued) the next byte's
 * arrival on the wire. A byte that arrives while RDR is still full would overrun on the wire: the
 * rest of the frame then waits for an RXFRQ write or is dropped at the next frame, and with the
 * receiver disabled (CR1 UE/RE) or not wired (crsf=off, pilot commands still tick the model) it is
 * dropped at once -- no deadline per byte for a USART that does not listen (the firmware's boot).
 */
static void crsf_arm(NexusXRState *s, int64_t now)
{
    int64_t next = MAX(s->crsf_last_ns + (int64_t)fb_crsf_next_due_ns(s->crsf), now + 1000);
    if (fb_crsf_tx_pending(s->crsf)) {
        CrsfUsart *u = s->crsf_usart;
        int64_t at = s->crsf_frame_ns + (int64_t)s->crsf_fed * CRSF_BYTE_NS;
        if (!u || !crsf_usart_rx_on(s, u)) {
            s->crsf_dropped += fb_crsf_tx_discard(s->crsf);
        } else if (at > now) {
            next = MIN(next, at);
        }
    }
    if (s->pilot_n && s->pilot_q[0].at_ns < next) {
        next = MAX(s->pilot_q[0].at_ns, now + 1000);
    }
    timer_mod(s->crsf_timer, next);
}

/* the next byte after the firmware's RX interrupt consumed one: its handler writes RQR.RXFRQ
 * after reading RDR (F7 HAL), or just reads DR (F4 StdPeriph: the DR read clears RXNE) */
static void crsf_usart_consumed(CrsfUsart *u)
{
    if (fb_crsf_tx_pending(u->s->crsf)) {
        crsf_rx_feed(u->s);
        if (!fb_crsf_tx_pending(u->s->crsf)) {
            crsf_arm(u->s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));   /* drained: next frame */
        }
    }
}

static void crsf_usart_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    CrsfUsart *u = opaque;
    u->ops->write(u->opaque, addr, val, size);
    if (u->s->soc->usart_model == FB_USART_L4X5 && addr == L4_RQR && (val & L4_RQR_RXFRQ)) {
        crsf_usart_consumed(u);
    }
}

static uint64_t crsf_usart_read(void *opaque, hwaddr addr, unsigned size)
{
    CrsfUsart *u = opaque;
    uint64_t v = u->ops->read(u->opaque, addr, size);
    if (u->s->soc->usart_model == FB_USART_F2XX && addr == USART_DR) {
        crsf_usart_consumed(u);
    }
    return v;
}

static void crsf_usart_hook(NexusXRState *s, MemoryRegion *mr)
{
    CrsfUsart *u = g_new0(CrsfUsart, 1);
    assert(mr->ops->read && mr->ops->write && !mr->ops->valid.accepts);
    u->s = s;
    u->ops = mr->ops;
    u->opaque = mr->opaque;
    u->w = *mr->ops;
    u->w.read = crsf_usart_read;
    u->w.write = crsf_usart_write;
    mr->ops = &u->w;
    mr->opaque = u;
    s->crsf_usart = u;
}

static void crsf_tick(void *opaque)
{
    NexusXRState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /* due pilot commands (queue is kept sorted by time) */
    unsigned done = 0;
    while (done < s->pilot_n && s->pilot_q[done].at_ns <= now) {
        pilot_apply(s, &s->pilot_q[done++]);
    }
    if (done) {
        memmove(s->pilot_q, s->pilot_q + done,
                (s->pilot_n - done) * sizeof(s->pilot_q[0]));
        s->pilot_n -= done;
    }

    if (now - s->crsf_last_ns >= (int64_t)fb_crsf_next_due_ns(s->crsf)) {
        /* frame time: the previous frame's unread bytes are lost, the new one starts */
        s->crsf_dropped += fb_crsf_tx_discard(s->crsf);
        fb_crsf_tick(s->crsf, now - s->crsf_last_ns);
        s->crsf_last_ns = now;
        s->crsf_frame_ns = now;
        s->crsf_fed = 0;
        crsf_rx_feed(s);
    } else if (now >= s->crsf_frame_ns + (int64_t)s->crsf_fed * CRSF_BYTE_NS) {
        crsf_rx_feed(s);                /* the next byte has arrived on the wire */
    }
    crsf_arm(s, now);
}

/* ------------------------------------------------------------------ */
/* MSP DisplayPort OSD device (devices/src/msp_osd.c) on a USART        */
/*
 * Same in-process wire as the receiver: the bytes the FC transmits go to fb_msp_osd_rx_byte();
 * each DRAW_SCREEN is handed to the control server (ctl_osd_frame -> `osd` message); the device's
 * own bytes (MSP_SET_OSD_CANVAS on a new contact, MSP_FC_VARIANT every 125 ms from power-on: osd_poll,
 * -M osd-poll=)
 * enter the USART one per 115200 byte time when RDR is free; not while the power is off. osd_watch fires when the wire has been silent for the device's threshold
 * (max(250 ms, 3 x its measured frame interval)); an MCU reset tells the device at once
 * (fb_msp_osd_contact_lost). Either way the control server sends `osd` link:false.
 */

static void osd_clock(NexusXRState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    fb_msp_osd_tick(s->osd, now - s->osd_last_ns);
    s->osd_last_ns = now;
}

static void osd_wire_tx_body(NexusXRState *s, const uint8_t *buf, int len);

static void osd_wire_tx(void *opaque, const uint8_t *buf, int len)
{
    PROF_ENTER(pa.osd_fc_tx);
    PROF_BYTES(pa.osd_fc_tx, len);
    osd_wire_tx_body(opaque, buf, len);
    PROF_LEAVE(pa.osd_fc_tx);
}

static void osd_wire_tx_body(NexusXRState *s, const uint8_t *buf, int len)
{
    bool full;
    osd_clock(s);
    if (s->osd_log && len > 0) {
        fwrite(buf, 1, (size_t)len, s->osd_log);
    }
    for (int i = 0; i < len; i++) {
        fb_msp_osd_rx_byte(s->osd, buf[i]);
        if (fb_msp_osd_take_frame(s->osd, &full)) {
            ctl_osd_frame(s, full);
            if (s->osd_log) {
                fflush(s->osd_log);     /* whole frames on disk: the process is stopped by a signal */
            }
        }
    }
    if (fb_msp_osd_tx_pending(s->osd) && !timer_pending(s->osd_timer)) {
        timer_mod(s->osd_timer, s->osd_last_ns + OSD_BYTE_NS);
    }
    if (fb_msp_osd_link_up(s->osd)) {
        timer_mod(s->osd_watch, s->osd_last_ns + fb_msp_osd_silence_ns(s->osd) + 1);
    }
}

static void osd_watch_tick(void *opaque)
{
    NexusXRState *s = opaque;
    osd_clock(s);
    ctl_osd_link(s);
    if (fb_msp_osd_link_up(s->osd)) {       /* the threshold grew with the last frame: look again */
        timer_mod(s->osd_watch, s->osd_last_ns + fb_msp_osd_silence_ns(s->osd) / 4 + 1);
    }
}

static void osd_poll_tick(void *opaque)
{
    NexusXRState *s = opaque;
    osd_clock(s);                       /* queues the request when due */
    if (s->power_off) {
        uint8_t drop[64];
        while (fb_msp_osd_tx_pop(s->osd, drop, sizeof drop)) {
        }
    } else if (fb_msp_osd_tx_pending(s->osd) && !timer_pending(s->osd_timer)) {
        timer_mod(s->osd_timer, s->osd_last_ns + OSD_BYTE_NS);
    }
    timer_mod(s->osd_poll, s->osd_last_ns + fb_msp_osd_poll_in_ns(s->osd) + 1);
}

static void osd_tx_tick(void *opaque)
{
    NexusXRState *s = opaque;
    osd_clock(s);
    if (fb_msp_osd_tx_pending(s->osd) && qemu_chr_be_can_write(s->osd_wire)) {
        uint8_t b;
        fb_msp_osd_tx_pop(s->osd, &b, 1);
        qemu_chr_be_write(s->osd_wire, &b, 1);
    }
    if (fb_msp_osd_tx_pending(s->osd)) {
        timer_mod(s->osd_timer, s->osd_last_ns + OSD_BYTE_NS);
    }
}

static void pilot_reply(NexusXRState *s, Error *err)
{
    g_autofree char *msg = err
        ? g_strdup_printf("error: %s\n", error_get_pretty(err))
        : g_strdup_printf("ok t=%.6f\n",
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9);
    outq_write(&s->pilot_out, (uint8_t *)msg, strlen(msg));
}

static void pilot_event(void *opaque, QEMUChrEvent ev)
{
    outq_chr_event(&((NexusXRState *)opaque)->pilot_out, ev);
}

static bool json_us(QObject *o, uint16_t *out)
{
    QNum *n = qobject_to(QNum, o);
    if (!n) {
        return false;
    }
    double v = qnum_get_double(n);
    if (v < 800 || v > 2200) {
        return false;
    }
    *out = (uint16_t)lrint(v);
    return true;
}

static void pilot_line(NexusXRState *s, const char *line)
{
    Error *err = NULL;
    QObject *o = qobject_from_json(line, &err);
    QDict *d = o ? qobject_to(QDict, o) : NULL;
    struct PilotCmd c = { .at_ns = 0, .link = -1 };

    if (!err && !d) {
        error_setg(&err, "need a JSON object");
    }
    if (!err && qdict_haskey(d, "type")) {
        /* {"type":"launch", speed_kmh, duration_s, pitch_deg, heading_deg[, "at"]} */
        if (g_strcmp0(qdict_get_try_str(d, "type"), "launch")) {
            error_setg(&err, "type: only launch (channel lines have no type)");
        } else if (!s->env || s->fdm_kind != FB_FDM_PLANE) {
            error_setg(&err, "launch: the aircraft has no plane FDM");
        } else if (env_launch_parse(d, &c.lp, &err)) {
            c.launch = true;
        }
    }
    if (!err && qdict_haskey(d, "ch")) {
        QList *l = qdict_get_qlist(d, "ch");
        int i = 0;
        for (const QListEntry *e = l ? qlist_first(l) : NULL;
             e && i < FB_CRSF_CHANNELS; e = qlist_next(e), i++) {
            if (!json_us(qlist_entry_obj(e), &c.ch[i])) {
                error_setg(&err, "ch[%d]: need 800..2200 us", i);
                break;
            }
            c.ch_mask |= 1u << i;
        }
        if (!l && !err) {
            error_setg(&err, "ch: need a list");
        }
    }
    if (!err && qdict_haskey(d, "set")) {
        QDict *m = qdict_get_qdict(d, "set");
        for (const QDictEntry *e = m ? qdict_first(m) : NULL; e && !err;
             e = qdict_next(m, e)) {
            int i = atoi(qdict_entry_key(e)) - 1;
            if (i < 0 || i >= FB_CRSF_CHANNELS ||
                !json_us(qdict_entry_value(e), &c.ch[i])) {
                error_setg(&err, "set: channel 1..16 -> 800..2200 us");
            } else {
                c.ch_mask |= 1u << i;
            }
        }
        if (!m && !err) {
            error_setg(&err, "set: need an object");
        }
    }
    if (!err && qdict_haskey(d, "link")) {
        c.link = qdict_get_try_bool(d, "link", true);
    }
    if (!err && qdict_haskey(d, "at")) {
        QNum *n = qobject_to(QNum, qdict_get(d, "at"));
        double at = n ? qnum_get_double(n) : NAN;
        /* checked before the conversion: a huge/NaN double -> int64 is undefined */
        if (!(at >= 0 && at <= PILOT_AT_MAX_S)) {
            error_setg(&err, "at: need virtual seconds 0..%d", PILOT_AT_MAX_S);
        } else {
            c.at_ns = (int64_t)(at * 1e9);
        }
    }
    if (!err) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (c.at_ns <= now) {
            pilot_apply(s, &c);
        } else if (s->pilot_n >= PILOT_QUEUE) {
            error_setg(&err, "queue full");
        } else {
            unsigned i = s->pilot_n;
            while (i && s->pilot_q[i - 1].at_ns > c.at_ns) {
                s->pilot_q[i] = s->pilot_q[i - 1];
                i--;
            }
            s->pilot_q[i] = c;
            s->pilot_n++;
            uint64_t exp = timer_expire_time_ns(s->crsf_timer);
            if (i == 0 && (exp == UINT64_MAX || (int64_t)exp > c.at_ns)) {
                timer_mod(s->crsf_timer, c.at_ns);
            }
        }
    }
    qobject_unref(o);
    pilot_reply(s, err);
    error_free(err);
}

static int pilot_can_read(void *opaque)
{
    return 256;
}

static void pilot_read(void *opaque, const uint8_t *buf, int size)
{
    NexusXRState *s = opaque;
    PROF_ENTER(pa.pilot_rx);
    PROF_BYTES(pa.pilot_rx, size);
    for (int i = 0; i < size; i++) {
        char ch = buf[i];
        if (ch == '\n' || ch == '\r') {
            s->pilot_buf[s->pilot_len] = 0;
            if (s->pilot_len) {
                pilot_line(s, s->pilot_buf);
            }
            s->pilot_len = 0;
        } else if (s->pilot_len < sizeof(s->pilot_buf) - 1) {
            s->pilot_buf[s->pilot_len++] = ch;
        }
    }
    PROF_LEAVE(pa.pilot_rx);
}

/* ------------------------------------------------------------------ */
/* Physics tick: ESC + motor, helicopter FDM, IMU feed, CSV log         */
/*
 * Every 1 ms of virtual time: the ESC model steps with the last DShot
 * command; with fdm=on the head speed is the motor's rpm through the main
 * gear (one source of truth: fb_esc; the FDM's own run-up model is
 * bypassed), the FDM steps with the servo pulses read from the timers and
 * feeds the rotor load back to the ESC, and its body rates / specific
 * force become the IMU motion source (the noise and sampling at the ODR
 * stay as in step A).
 *
 * Axes: the FDM body frame is FRD. The firmware's board frame on the real
 * board is the chip frame turned by the alignment (CW180), right-handed,
 * +Z up; forward is +X: board = (x, -y, -z) of FRD. The chip sees board
 * turned back by CW180: chip = (-bx, -by, bz).
 */

static void fdm_tick(void *opaque)
{
    NexusXRState *s = opaque;
    const float dt = FDM_DT_NS / 1e9f;

    s->servo_sig = 0;
    for (int i = 0; i < s->board->nservo; i++) {
        unsigned us = tim_pulse_us(s, &s->board->servo[i]);
        if (us >= 500 && us <= 2500) {
            s->servo_us[i] = us;        /* no pulse: the servo holds */
            s->servo_sig |= 1u << i;
        }
    }
    if (!s->board->motor_dshot && s->esc_present) {
        esc_pulse(s);
    }
    PROF_ENTER(pa.esc_step);
    fb_esc_step(s->esc, dt);
    PROF_LEAVE(pa.esc_step);
    battery_tick(s, dt);

    if (s->fdm_on) {
        fdm_model_tick(s);
    }
    s->fdm_next_ns += FDM_DT_NS;
    timer_mod(s->fdm_timer, s->fdm_next_ns);
    ctl_tick(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

/* ------------------------------------------------------------------ */
/* Reset: peripherals back to reset values; flash, backup domain kept */

static void nexus_xr_reset(void *opaque)
{
    NexusXRState *s = opaque;
    uint32_t csr = stub_get32(&s->apb, RCC_BASE + 0x74);
    uint32_t bdcr = stub_get32(&s->apb, RCC_BASE + 0x70);
    g_autofree uint8_t *rtc = g_memdup2(s->apb.regs + (RTC_BASE - APB_AHB1_BASE),
                                        0x400);
    g_autofree uint8_t *bkp = g_memdup2(s->apb.regs +
                                        (BKPSRAM_BASE - APB_AHB1_BASE),
                                        0x1000);

    memset(s->apb.regs, 0, s->apb.size);
    memset(s->ahb2.regs, 0, s->ahb2.size);
    memset(s->fmc.regs, 0, s->fmc.size);
    memcpy(s->apb.regs + (RTC_BASE - APB_AHB1_BASE), rtc, 0x400);
    memcpy(s->apb.regs + (BKPSRAM_BASE - APB_AHB1_BASE), bkp, 0x1000);

    stub_set32(&s->apb, RCC_BASE + 0x00, 0x00000083);   /* CR: HSI on */
    stub_set32(&s->apb, RCC_BASE + 0x04, 0x24003010);   /* PLLCFGR */
    stub_set32(&s->apb, RCC_BASE + 0x70, bdcr);
    ctl_reset_flash(s);
    const char *cause = s->power_off ? "power-off"
                      : !s->powered_on ? "power-on" : "reset";
    if (!s->powered_on && !s->power_off && s->imu) {
        fb_icm42688p_reset(s->imu);     /* the IMU lost power with the board */
    }
    if (!s->powered_on && !s->power_off && s->baro) {
        fb_dps310_reset(s->baro);       /* so did the baro */
        s->baro_rate = 0;
        s->baro_oneshot = false;
        timer_del(s->baro_timer);
    }
    if (!s->powered_on && !s->power_off && s->spl06) {
        fb_spl06_reset(s->spl06);       /* the F405's baro, likewise */
        timer_del(s->spl06_timer);
        s->spl06_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    if (!s->powered_on && !s->power_off && s->mag) {
        fb_qmc5883l_reset(s->mag);      /* the GPS module hangs on the FC's 5 V */
        s->mag_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    if (!s->powered_on && !s->power_off && s->gps_uart >= 0) {
        fb_ublox_reset(s->gps);         /* factory baud, no UBX output, the fix lost */
        s->gps_last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        gps_arm(s, s->gps_last_ns);
    }
    if (s->gps_uart >= 0) {
        fb_ublox_set_fc_baud(s->gps, 0);    /* the USART's BRR is back at 0: set again at init */
    }
    if (s->board->adc) {
        adc_reset(s);
    }
    if (!s->powered_on && !s->power_off && s->osd_chip) {
        fb_max7456_reset(s->osd_chip);  /* the OSD chip is on the board: power-on reset */
    }
    if (!s->powered_on && !s->power_off && s->osd) {
        osd_clock(s);
        fb_msp_osd_reset(s->osd);       /* the VTX is on the same battery */
        timer_del(s->osd_timer);
        if (s->osd_uart >= 0 && s->osd_poll_on) {
            timer_mod(s->osd_poll, s->osd_last_ns + fb_msp_osd_poll_in_ns(s->osd) + 1);
        }
    }
    if (s->osd_uart >= 0) {
        fb_msp_osd_contact_lost(s->osd);    /* any MCU reset: the next frame is a new contact */
        timer_del(s->osd_watch);
    }
    if (!s->powered_on || s->power_off) {
        csr = 0x0E000000;   /* BORRSTF | PINRSTF | PORRSTF */
        s->powered_on = !s->power_off;
    } else {
        csr |= 0x14000000;  /* SFTRSTF | PINRSTF */
    }
    stub_set32(&s->apb, RCC_BASE + 0x74, csr);
    stub_set32(&s->apb, PWR_BASE + 0x00, s->soc->pwr_cr_reset);
    stub_set32(&s->apb, FLASHC_BASE + 0x14, s->soc->optcr_reset);

    s->flash_cr = 1u << 31;
    s->flash_key_state = 0;
    s->cfg_prog = false;
    s->dwt_ctrl = 0x40000000;
    dwt_rebase(s, 0);
    idle_forget(s);
    s->dbgmcu_cr = 0;
    memset(s->spi_rxne, 0, sizeof(s->spi_rxne));

    /* GPIO/EXTI/SPI1/DMA2 back to reset values. The IMU is a separate
     * chip: it keeps its state across an MCU reset, only its CS goes idle. */
    memset(s->gpio, 0, sizeof(s->gpio));
    s->gpio[0][GPIO_MODER] = 0xA8000000;
    s->gpio[0][GPIO_OSPEEDR] = 0x0C000000;
    s->gpio[0][GPIO_PUPDR] = 0x64000000;
    s->gpio[1][GPIO_MODER] = 0x00000280;
    s->gpio[1][GPIO_OSPEEDR] = 0x000000C0;
    s->gpio[1][GPIO_PUPDR] = 0x00000100;
    s->gpio_in[s->board->imu_cs_port] |= 1u << s->board->imu_cs_pin;    /* CS pulled up */
    if (s->osd_chip) {
        s->gpio_in[s->board->osd_cs_port] |= 1u << s->board->osd_cs_pin;
    }
    s->exti_imr = s->exti_emr = s->exti_rtsr = s->exti_ftsr = s->exti_pr = 0;
    for (int p = 0; p < NUM_GPIO; p++) {
        s->gpio_level[p] = gpio_levels(s, p);
    }
    exti_update_irq(s);
    imu_cs_changed(s, true);
    if (s->osd_chip && s->osd_chip_cs) {
        s->osd_chip_cs = false;         /* its CS goes idle too: the next frame starts clean */
        fb_max7456_cs(s->osd_chip, false);
    }

    for (int i = 0; i < 2; i++) {
        s->spi[i].cr1 = 0;
        s->spi[i].cr2 = spi1_f4(s) ? 0 : 0x0700;     /* F7: DS = 8 bit */
        s->spi[i].crcpr = 7;
        s->spi[i].rx_n = 0;
        s->spi[i].ovr = false;
    }

    if (s->soc->i2c_model == FB_I2C_V1) {
        i2c1v1_reset(&s->i2c1.c);       /* the baro and the compass keep their state */
    } else {
        i2c_core_reset(&s->i2c3.c);     /* the baro keeps its state (separate chip) */
    }

    s->dma2_isr[0] = s->dma2_isr[1] = 0;
    for (int n = 0; n < 8; n++) {
        DmaStream *st = &s->dma2[n];
        timer_del(st->timer);
        st->cr = st->ndtr = st->par = st->m0ar = st->m1ar = 0;
        st->fcr = 0x21;
        st->busy = false;
        dma2_update_irq(s, n);
    }

    /* Timers back to reset values; the ESC loses its signal (it is a
     * separate device and keeps spinning down on its own), the receiver,
     * the pilot and the FDM are outside the MCU and carry on. */
    for (int i = 0; i < s->soc->ntim; i++) {
        memset(s->tim[i].r, 0, sizeof(s->tim[i].r));
        s->tim[i].r[TIM_ARR / 4] = s->soc->tim[i].is32 ? 0xFFFFFFFF : 0xFFFF;
        tim_rebase(&s->tim[i], 0);
    }
    timer_del(s->esc_reply_timer);
    s->esc_reply_armed = false;
    s->dshot_value = 0;
    s->dshot_bidir = false;
    s->dshot_period = 0;
    fb_esc_set_dshot(s->esc, 0, false);
    ctl_on_reset(s, cause);
    ctl_osd_link(s);                    /* after the reboot event: the picture is gone */
}

/* After the CPU's own reset handler: battery off keeps the core halted */
static void nexus_xr_post_reset(void *opaque)
{
    NexusXRState *s = opaque;
    if (s->power_off) {
        arm_set_cpu_off(0);         /* async: runs before the next instruction */
    }
}

#include "nexus_xr_ctl.c.inc"

/* ------------------------------------------------------------------ */
/* Aircraft file and the wiring's actuator map                         */
/*
 * models/<id>/aircraft.json (-M aircraft-file=, the session passes it): "transmission"
 * {"main_rotor_gear_ratio":[pinion, main]}, "esc" {kv, poles, tau_s, max_current_a,
 * idle_current_a, voltage_v}, "fdm_params" (the model's parameters, docs/fdm.md: handed to the FDM
 * as they are; it names a wrong one), "fdm_inputs" (the actuator names of the FDM's inputs 0..3:
 * the heli's swash servos rear, front-left, front-right and the tail; the plane's aileron_l,
 * aileron_r, elevator, rudder; they must be the model's own input names), "env_default".
 * "centre_us" {input: us} (the servo centre a reversed output is mirrored around, default 1500).
 * Missing keys keep the library defaults; a wrong type or value exits naming the key (the same
 * checks as core/qemu/scripts/wiring.py load_aircraft); fdm=on needs the transmission. No file:
 * library defaults and a 1:1 gear.
 */

static G_NORETURN void aj_fail(NexusXRState *s, const char *key, const char *need)
{
    error_report("%s: aircraft-file %s: %s: need %s", fb_mname, s->aircraft_file, key, need);
    exit(1);
}

/* the section `key` of a (an object or absent) */
static QDict *aj_dict(NexusXRState *s, QDict *a, const char *key)
{
    QObject *o = qdict_get(a, key);
    QDict *d = o ? qobject_to(QDict, o) : NULL;
    if (o && !d) {
        aj_fail(s, key, "an object");
    }
    return d;
}

/* number sect.key in lo..hi (lo excluded when open_lo): false when absent, exits when wrong */
static bool aj_num(NexusXRState *s, QDict *d, const char *sect, const char *key,
                   double lo, double hi, bool open_lo, double *out)
{
    QObject *o = d ? qdict_get(d, key) : NULL;
    if (!o) {
        return false;
    }
    QNum *n = qobject_to(QNum, o);
    double v = n ? qnum_get_double(n) : NAN;
    if (!n || !isfinite(v) || v < lo || v > hi || (open_lo && v == lo)) {
        g_autofree char *k = g_strdup_printf("%s.%s", sect, key);
        g_autofree char *need = isinf(hi) ? (isinf(lo) ? g_strdup("a number")
                                             : g_strdup_printf("a number %s %g", open_lo ? ">" : ">=", lo))
                                          : g_strdup_printf("a number %g..%g", lo, hi);
        aj_fail(s, k, need);
    }
    *out = v;
    return true;
}

static FbFunction *fn_add(NexusXRState *s, const char *name, const char *label)
{
    if (s->nfn == FB_FN_MAX) {
        aj_fail(s, "functions", "at most 8 functions");
    }
    FbFunction *f = &s->fn[s->nfn++];
    *f = (FbFunction){ .name = g_strdup(name), .label = g_strdup(label), .hold_s = 2 };
    f->launch.speed_kmh = f->launch.duration_s = f->launch.pitch_deg = f->launch.heading_deg = NAN;
    f->launch.heading = FB_SC_HEADING_UNSET;
    return f;
}

/* aircraft.json `functions` [{name, label, auto_first_start, scenario, params}] (the resolver
 * checks them in full: devices/src/session.c, scripts/wiring.py); without it the older
 * `acc_calibration` scenario is a calibrate_acc with auto_first_start */
static void functions_load(NexusXRState *s, QDict *a)
{
    static const char *const known[] = { "calibrate_acc", "calibrate_gyro", "launch", "calibrate_mag" };
    s->fn = g_new0(FbFunction, FB_FN_MAX);
    QObject *o = qdict_get(a, "functions");
    if (!o || qobject_type(o) == QTYPE_QNULL) {
        const char *acc = qdict_get_try_str(a, "acc_calibration");
        if (acc) {
            FbFunction *f = fn_add(s, "calibrate_acc", "Calibrate accelerometer (6 positions)");
            f->scenario = g_strdup(acc);
            f->auto_first = true;
        }
        return;
    }
    QList *l = qobject_to(QList, o);
    if (!l) {
        aj_fail(s, "functions", "a list of {name, label, auto_first_start, scenario, params}");
    }
    for (const QListEntry *e = qlist_first(l); e; e = qlist_next(e)) {
        QDict *d = qobject_to(QDict, qlist_entry_obj(e));
        const char *name = d ? qdict_get_try_str(d, "name") : NULL;
        unsigned k = 0;
        while (name && k < ARRAY_SIZE(known) && strcmp(name, known[k])) {
            k++;
        }
        if (!name || k == ARRAY_SIZE(known)) {
            aj_fail(s, "functions[].name", "calibrate_acc|calibrate_gyro|launch|calibrate_mag");
        }
        for (int i = 0; i < s->nfn; i++) {
            if (!strcmp(s->fn[i].name, name)) {
                aj_fail(s, "functions[].name", "each function once");
            }
        }
        const char *label = qdict_get_try_str(d, "label");
        FbFunction *f = fn_add(s, name, label ? label : name);
        QObject *au = qdict_get(d, "auto_first_start");
        if (au && !qobject_to(QBool, au)) {
            aj_fail(s, "functions[].auto_first_start", "true|false");
        }
        f->auto_first = au && qbool_get_bool(qobject_to(QBool, au));
        const char *sc = qdict_get_try_str(d, "scenario");
        f->scenario = sc ? g_strdup(sc) : NULL;
        QDict *pd = aj_dict(s, d, "params");
        g_autofree char *sect = g_strdup_printf("functions.%s.params", name);
        if (!strcmp(name, "calibrate_gyro")) {
            aj_num(s, pd, sect, "hold_s", 0, 60, false, &f->hold_s);
        } else if (!strcmp(name, "launch") && pd) {
            Error *err = NULL;
            if (!env_launch_parse(pd, &f->launch, &err)) {
                aj_fail(s, sect, error_get_pretty(err));
            }
        }
    }
}

static void aircraft_load(NexusXRState *s, fb_esc_params_t *ep)
{
    static const char *const def_in[4] = { "swash_a", "swash_b", "swash_c", "tail" };
    static const char *const plane_in[4] = { "aileron_l", "aileron_r", "elevator", "rudder" };
    for (int k = 0; k < 4; k++) {
        s->fdm_in_names[k] = g_strdup(def_in[k]);
        s->fdm_centre_us[k] = 1500;
    }
    if (!s->aircraft_file || !*s->aircraft_file) {
        if (s->fdm_on) {
            warn_report("%s: fdm=on without aircraft-file: library defaults, gear 1:1", fb_mname);
        }
        return;
    }
    g_autofree char *text = NULL;
    g_autoptr(GError) gerr = NULL;
    Error *err = NULL;
    if (!g_file_get_contents(s->aircraft_file, &text, NULL, &gerr)) {
        error_report("%s: aircraft-file %s: %s", fb_mname, s->aircraft_file, gerr->message);
        exit(1);
    }
    QObject *o = qobject_from_json(text, &err);
    QDict *a = o ? qobject_to(QDict, o) : NULL;
    if (!a) {
        error_report("%s: aircraft-file %s: %s", fb_mname, s->aircraft_file,
                     err ? error_get_pretty(err) : "not a JSON object");
        exit(1);
    }
    const double inf = INFINITY;
    double v;
    s->fdm_kind = !g_strcmp0(qdict_get_try_str(a, "fdm"), "plane") ? FB_FDM_PLANE : FB_FDM_HELI;
    for (int k = 0; s->fdm_kind == FB_FDM_PLANE && k < 4; k++) {
        g_free(s->fdm_in_names[k]);
        s->fdm_in_names[k] = g_strdup(plane_in[k]);
    }
    QDict *t = aj_dict(s, a, "transmission");
    QObject *go = t ? qdict_get(t, "main_rotor_gear_ratio") : NULL;
    if (go) {
        QList *g = qobject_to(QList, go);
        QNum *p = g && qlist_size(g) == 2 ? qobject_to(QNum, qlist_peek(g)) : NULL;
        QNum *m = p ? qobject_to(QNum, qlist_entry_obj(qlist_next(qlist_first(g)))) : NULL;
        double pv = p ? qnum_get_double(p) : 0, mv = m ? qnum_get_double(m) : 0;
        if (!m || !isfinite(pv) || !isfinite(mv) || pv <= 0 || mv <= 0) {
            aj_fail(s, "transmission.main_rotor_gear_ratio", "[pinion, main], 2 numbers > 0");
        }
        s->main_gear = mv / pv;
    } else if (s->fdm_on && s->fdm_kind != FB_FDM_PLANE) {
        aj_fail(s, "transmission.main_rotor_gear_ratio", "[pinion, main] (the heli FDM needs the gear)");
    }
    QDict *e = aj_dict(s, a, "esc");
    if (aj_num(s, e, "esc", "kv", 0, inf, true, &v)) {
        ep->kv = v;
    }
    if (aj_num(s, e, "esc", "poles", 2, 64, false, &v)) {
        if (v != floor(v) || (int)v % 2) {
            aj_fail(s, "esc.poles", "an even integer 2..64");
        }
        ep->poles = (uint8_t)v;
    }
    if (aj_num(s, e, "esc", "tau_s", 0, inf, true, &v)) {
        ep->tau_s = v;
    }
    if (aj_num(s, e, "esc", "max_current_a", 0, inf, true, &v)) {
        ep->max_current_a = v;
    }
    if (aj_num(s, e, "esc", "idle_current_a", 0, inf, false, &v)) {
        ep->idle_current_a = v;
    }
    if (aj_num(s, e, "esc", "voltage_v", 0, inf, true, &v)) {
        ep->voltage_v = v;
    }
    g_free(s->fdm_params_json);
    s->fdm_params_json = fdm_params_text(s, a);
    QObject *ino = qdict_get(a, "fdm_inputs");
    if (ino) {
        QList *in = qobject_to(QList, ino);
        if (!in || (s->fdm_on && qlist_size(in) != 4)) {
            aj_fail(s, "fdm_inputs", "a list of the FDM's 4 input names");
        }
        int k = 0;
        for (const QListEntry *le = qlist_first(in); le && k < 4; le = qlist_next(le), k++) {
            QString *qs = qobject_to(QString, qlist_entry_obj(le));
            if (!qs) {
                aj_fail(s, "fdm_inputs", "a list of actuator names");
            }
            g_free(s->fdm_in_names[k]);
            s->fdm_in_names[k] = g_strdup(qstring_get_str(qs));
        }
    }
    QDict *c = aj_dict(s, a, "centre_us");
    for (const QDictEntry *de = c ? qdict_first(c) : NULL; de; de = qdict_next(c, de)) {
        const char *name = qdict_entry_key(de);
        g_autofree char *key = g_strdup_printf("centre_us.%s", name);
        int k = 0;
        while (k < 4 && strcmp(s->fdm_in_names[k], name)) {
            k++;
        }
        if (k == 4) {
            aj_fail(s, key, "one of the fdm_inputs");
        }
        aj_num(s, c, "centre_us", name, -inf, inf, false, &v);
        if (v != floor(v) || v < 1000 || v > 2000) {
            aj_fail(s, key, "an integer 1000..2000");
        }
        s->fdm_centre_us[k] = (uint16_t)v;
    }
    g_free(s->env_default_path);
    s->env_default_path = env_rel_path(s, a, "env_default");
    functions_load(s, a);
    qobject_unref(o);
}

#include "fb_fdm.c.inc"

/* FDM input k <- the wired servo output whose actuator name is fdm_in_names[k] */
static void fdm_map_init(NexusXRState *s)
{
    for (int k = 0; k < 4; k++) {
        s->fdm_src[k] = -1;
        for (int i = 0; i < s->board->nservo; i++) {
            if ((s->servo_mask >> i & 1) && !strcmp(ctl_actuator(s, i), s->fdm_in_names[k])) {
                s->fdm_src[k] = i;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Bench: -M nexus-xr,session=<bench.json>[,root=<dir>][,session-check=on]                    */
/*
 * The machine resolves the bench itself (devices/src/session.c: wiring, machine options, the
 * custom-defaults blob, the manifest; what core/qemu/run.sh did through scripts/session.py,
 * mkdefaults.sh and manifest.py) before anything else is built, and sets the options it
 * resolves as if they were given with -M - except the ones that were: an explicit -M option
 * wins over the bench (debugging). The blob and the manifest stay in memory (a firmware reload
 * places the same blob again). A rejected bench prints "session: <error>" lines and exits 2.
 * session-check=on prints the report (fb_session_report: the assembly, options, icount, ports,
 * manifest, the blob) and exits 0; without session= it prints the manifest line only.
 * -icount and the chardev sockets are QEMU's command line, not machine options: the bench's
 * time.icount_shift is checked against -icount here (a warning when they differ).
 */
static void session_print(const char *what, bool err, const char *text)
{
    for (const char *p = text; p && *p;) {
        const char *e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p);
        fprintf(err ? stderr : stdout, "%s%.*s\n", what, n, p);
        p = e ? e + 1 : NULL;
    }
}

static void session_apply(NexusXRState *s)
{
    fb_session_opts_t o = { .root = s->session_root };
    fb_session_t *fs;
    bool have = s->session_path && *s->session_path;

    if (!have && !s->session_check) {
        return;
    }
    fs = have ? fb_session_load(s->session_path, &o) : fb_session_manifest_only(&o);
    for (int i = 0; i < fb_session_nwarnings(fs); i++) {
        session_print("session: ", true, fb_session_warning(fs, i));
    }
    if (!fb_session_ok(fs)) {
        for (int i = 0; i < fb_session_nerrors(fs); i++) {
            session_print("session: ", true, fb_session_error(fs, i));
        }
        exit(2);
    }
    if (s->session_check) {
        size_t n = 0;
        for (int i = 0; i < fb_session_noptions(fs); i++) {
            if (!strcmp(fb_session_option_key(fs, i), "aircraft-file")) {
                fdm_params_check(s, fb_session_option_value(fs, i));
            }
        }
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);  /* "defaults: N bytes" + exactly N bytes, no CRLF */
#endif
        const char *r = have ? fb_session_report(fs, &n) : fb_session_manifest(fs);
        if (!have) {
            fputs("manifest: ", stdout);
            n = strlen(r);
        }
        fwrite(r, 1, n, stdout);
        fflush(stdout);
        exit(0);
    }

    {
        const char *m = fb_session_machine(fs);
        if (m && strcmp(m, fb_mname)) {
            fprintf(stderr, "session: %s: the bench's board runs on -M %s, this is -M %s "
                    "(docs/protocol.md, Launching the core: the launch line's machine=)\n",
                    s->session_path, m, fb_mname);
            exit(2);
        }
    }
    s->session_applying = true;
    for (int i = 0; i < fb_session_noptions(fs); i++) {
        const char *k = fb_session_option_key(fs, i), *v = fb_session_option_value(fs, i);
        Error *err = NULL;
        if (g_hash_table_contains(s->given, k)) {
            fprintf(stderr, "session: -M %s given: the bench's %s=%s is not applied\n", k, k, v);
            continue;
        }
        if (!object_property_parse(OBJECT(s), k, v, &err)) {
            error_prepend(&err, "session: -M %s=%s: ", k, v);
            error_report_err(err);
            exit(2);
        }
    }
    if (!g_hash_table_contains(s->given, "fw")) {
        g_free(s->firmware_path);
        s->firmware_path = g_strdup(fb_session_firmware(fs));
    }
    if (!g_hash_table_contains(s->given, "defaults")) {
        size_t n;
        const char *blob = fb_session_defaults(fs, &n);
        s->defaults_blob = blob ? g_memdup2(blob, n) : NULL;
        s->defaults_len = blob ? n : 0;
    }
    if (!g_hash_table_contains(s->given, "manifest")) {
        s->manifest_text = g_strdup(fb_session_manifest(fs));
    }
    if (!g_hash_table_contains(s->given, "config-cli")) {
        size_t n;
        const char *cli = fb_session_config_cli(fs, &n);
        s->config_cli = cli ? g_strndup(cli, n) : NULL;
        cfg_prov_take(s, fs);
    }
    s->session_opts = g_strdup(fb_session_options_str(fs));
    s->data_root = g_strdup(fb_session_root(fs));
    s->session_applying = false;

    {
        int shift = fb_session_icount_shift(fs);
        if (shift >= 0 && icount_enabled() != ICOUNT_PRECISE) {
            fprintf(stderr, "session: time.icount_shift %d but the command line has %s: add "
                    "-icount shift=%d,sleep=off (docs/protocol.md, Launching the core)\n", shift,
                    icount_enabled() ? "-icount shift=auto" : "no -icount", shift);
        } else if (shift >= 0 && icount_to_ns(1) != (1LL << shift)) {
            fprintf(stderr, "session: time.icount_shift %d but the command line has -icount "
                    "shift=%d: the command line wins\n", shift, ctz64(icount_to_ns(1)));
        } else if (shift < 0 && icount_enabled()) {
            fprintf(stderr, "session: time.icount_shift null but the command line has -icount: "
                    "the command line wins\n");
        }
    }
    fprintf(stderr, "session: %s -> %s; %s\n", s->session_path, s->firmware_path,
            fb_session_options_str(fs));
    fb_session_free(fs);
}

static void nexus_xr_init(MachineState *machine)
{
    NexusXRState *s = NEXUS_XR_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *armv7m;

    const FbSoc *soc = s->soc;

    session_apply(s);
    prof_init(s);

    s->sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(s->sysclk, soc->sysclk_hz);
    s->refclk = clock_new(OBJECT(machine), "REFCLK");
    clock_set_hz(s->refclk, soc->sysclk_hz / 8);
    s->pclk1 = clock_new(OBJECT(machine), "PCLK1");
    clock_set_hz(s->pclk1, soc->pclk1_hz);
    s->pclk2 = clock_new(OBJECT(machine), "PCLK2");
    clock_set_hz(s->pclk2, soc->pclk2_hz);

    if (soc->ram_low.size) {
        g_autofree char *n = mr_name(soc->ram_low.name);
        memory_region_init_ram(&s->ram_low, NULL, n, soc->ram_low.size, &error_fatal);
        memory_region_add_subregion(sysmem, soc->ram_low.base, &s->ram_low);
    }

    {
        g_autofree char *n = mr_name("flash");
        memory_region_init_rom_device(&s->flash, NULL, &flash_mem_ops, s, n, soc->flash_size,
                                      &error_fatal);
    }
    memset(memory_region_get_ram_ptr(&s->flash), 0xFF, soc->flash_size);
    memory_region_add_subregion(sysmem, soc->flash_base, &s->flash);
    if (soc->flash_alias_name) {
        g_autofree char *n = mr_name(soc->flash_alias_name);
        memory_region_init_alias(&s->flash_alias, OBJECT(machine), n, &s->flash, 0,
                                 soc->flash_size);
        memory_region_add_subregion(sysmem, soc->flash_alias, &s->flash_alias);
    }

    for (int i = 0; i < FB_SOC_MAX_RAM && soc->ram[i].size; i++) {
        g_autofree char *n = mr_name(soc->ram[i].name);
        memory_region_init_ram(&s->ram[i], NULL, n, soc->ram[i].size, &error_fatal);
        memory_region_add_subregion(sysmem, soc->ram[i].base, &s->ram[i]);
    }

    {
        g_autofree char *n = mr_name("sysmem");
        memory_region_init_ram(&s->sysmem, NULL, n, soc->sysmem_size, &error_fatal);
    }
    memory_region_add_subregion(sysmem, soc->sysmem_base, &s->sysmem);
    {
        uint8_t *p = memory_region_get_ram_ptr(&s->sysmem);
        static const uint32_t uid[3] = { 0x00350041, 0x3137510A, 0x32393436 };
        for (int i = 0; i < 3; i++) {
            stl_le_p(p + (soc->uid_addr - soc->sysmem_base) + 4 * i, uid[i]);
        }
        stw_le_p(p + (soc->flashsize_addr - soc->sysmem_base), soc->flash_size / KiB);
    }

    stub_window_init(s, &s->apb, "stub.apb-ahb1", APB_AHB1_BASE, APB_AHB1_SIZE);
    stub_window_init(s, &s->ahb2, "stub.ahb2", AHB2_BASE, AHB2_SIZE);
    stub_window_init(s, &s->fmc, "stub.fmc", FMC_BASE, FMC_SIZE);

    object_initialize_child(OBJECT(machine), "armv7m", &s->armv7m,
                            TYPE_ARMV7M);
    armv7m = DEVICE(&s->armv7m);
    qdev_prop_set_uint32(armv7m, "num-irq", soc->num_irq);
    qdev_prop_set_uint8(armv7m, "num-prio-bits", 4);
    qdev_prop_set_string(armv7m, "cpu-type", machine->cpu_type);
    qdev_prop_set_uint32(armv7m, "init-nsvtor", soc->flash_base);
    if (soc->bitband) {
        qdev_prop_set_bit(armv7m, "enable-bitband", true);
    }
    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
    qdev_connect_clock_in(armv7m, "refclk", s->refclk);
    object_property_set_link(OBJECT(armv7m), "memory", OBJECT(sysmem),
                             &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(armv7m), &error_fatal);
    s->idle_timer = nx_timer_new("idle.wake", idle_timer_cb, s);
    s->idle_frozen = true;      /* the setters refuse changes from here on */
    /* one TB per device-I/O instruction (patches/icount-io-tb.diff): the same instruction counts
     * and interrupt points, but the idle detector's signature (the TB PC in env->regs[15]) sees
     * other values, so its skips -- and the guest's timing -- differ from io-tb=off */
    icount_io_insn_tbs = s->io_tb && icount_enabled();
    if (s->idle_skip && !icount_enabled()) {
        warn_report("%s: idle-skip needs -icount (virtual time = instruction count); off", fb_mname);
        s->idle_skip = false;
    } else if (s->idle_skip && icount_enabled() == ICOUNT_ADAPTATIVE) {
        warn_report("%s: idle-skip needs a fixed -icount shift (not shift=auto: the "
                    "ns per instruction would change under it); off", fb_mname);
        s->idle_skip = false;
    } else if (s->idle_skip && replay_mode != REPLAY_MODE_NONE) {
        warn_report("%s: idle-skip does not work with record/replay (the replay log "
                    "owns the icount budget); off", fb_mname);
        s->idle_skip = false;
    }
    if (s->idle_skip) {
        const char *m = s->idle_skip_mode ? s->idle_skip_mode : "budget";
        if (strcmp(m, "budget") && strcmp(m, "halt")) {
            error_report("%s: idle-skip-mode=%s: budget|halt", fb_mname, m);
            exit(1);
        }
        s->idle_halt_mode = !strcmp(m, "halt");
        const char *src = s->idle_skip_src && *s->idle_skip_src ? s->idle_skip_src
                        : s->board->idle_systick ? "systick" : "cyccnt";
        if (strcmp(src, "cyccnt") && strcmp(src, "systick")) {
            error_report("%s: idle-skip-src=%s: cyccnt|systick", fb_mname, src);
            exit(1);
        }
        s->idle_systick = !strcmp(src, "systick");
        if (s->idle_systick) {
            idle_systick_hook(s, &s->armv7m.systickmem);
        }
        s->idle_gran_insns = MAX((int64_t)s->idle_skip_gran_us * SCALE_US / icount_to_ns(1), 1);
        s->cpu_irq = qdev_intercept_gpio_out(DEVICE(&s->armv7m.nvic),
                                             qemu_allocate_irq(idle_nvic_irq, s, 0),
                                             SYSBUS_DEVICE_GPIO_IRQ, 0);
    }

    /* PPB extras: above the armv7m RAZ/WI default region (priority -1) */
    memory_region_init_io(&s->dwt, OBJECT(machine), &dwt_ops, s,
                          mr_name_tmp(s, "dwt"), 0x1000);
    memory_region_add_subregion_overlap(&s->armv7m.container, DWT_BASE,
                                        &s->dwt, 0);
    memory_region_init_io(&s->dbgmcu, OBJECT(machine), &dbgmcu_ops, s,
                          mr_name_tmp(s, "dbgmcu"), 0x400);
    memory_region_add_subregion_overlap(&s->armv7m.container, DBGMCU_BASE,
                                        &s->dbgmcu, 0);

    /* CRSF receiver (when wired) on its USART through an in-process chardev */
    s->crsf_uart = s->board->crsf_default;
    if (s->crsf_str && *s->crsf_str) {
        const char *c = s->crsf_str;
        if (!strcmp(c, "off")) {
            s->crsf_uart = -1;
        } else if ((s->crsf_uart = uart_opt(c, s->board->crsf_uarts)) < 0) {
            error_report("%s: crsf=%s: off or %s", fb_mname, c, s->board->uart_range);
            exit(1);
        }
    }
    s->crsf = fb_crsf_new();
    s->link_up = true;
    s->link_rssi = -60;
    s->link_lq = 100;
    s->link_snr = 10;
    fb_crsf_set_link(s->crsf, s->link_rssi, s->link_lq, s->link_snr, true);
    s->crsf_wire = fbwire_new("fbcrsf-wire", crsf_wire_tx, s);
    for (int i = 0; i < FB_CRSF_CHANNELS; i++) {
        s->pilot_ch[i] = 1500;
    }
    s->pilot_ch[4] = 1000;              /* AUX1 = arm switch: off */
    s->pilot_ch[5] = 1000;              /* throttle (switch): low */
    fb_crsf_set_channels_us(s->crsf, s->pilot_ch);
    s->crsf_timer = nx_timer_new("crsf.frame", crsf_tick, s);
    if (s->crsf_uart >= 0) {
        timer_mod(s->crsf_timer, 1000000);  /* no receiver: the USART stays silent (RXLOSS) */
    }

    /* MSP DisplayPort OSD device (when wired) on its USART, same kind of wire */
    s->osd_uart = -1;
    if (s->osd_str && *s->osd_str && strcmp(s->osd_str, "off")) {
        const char *c = s->osd_str;
        if ((s->osd_uart = uart_opt(c, s->board->osd_uarts)) < 0) {
            if (!s->board->osd_uarts) {
                error_report("%s: osd=%s: the OSD device is not modelled on this board (off)",
                             fb_mname, c);
            } else {
                error_report("%s: osd=%s: off or %s", fb_mname, c, s->board->uart_range);
            }
            exit(1);
        }
        if (s->osd_uart == s->crsf_uart) {
            error_report("%s: osd=%s: the receiver is on that USART", fb_mname, c);
            exit(1);
        }
    }
    s->osd_poll_on = s->board->osd_poll;
    if (s->osd_poll_str && *s->osd_poll_str) {
        if (strcmp(s->osd_poll_str, "on") && strcmp(s->osd_poll_str, "off")) {
            error_report("%s: osd-poll=%s: on or off", fb_mname, s->osd_poll_str);
            exit(1);
        }
        s->osd_poll_on = !strcmp(s->osd_poll_str, "on");
    }
    /* a u-blox GPS (when wired) on its USART, same kind of wire */
    s->gps_uart = -1;
    if (s->gps_str && *s->gps_str && strcmp(s->gps_str, "off")) {
        const char *c = s->gps_str;
        if ((s->gps_uart = uart_opt(c, s->board->gps_uarts)) < 0) {
            if (!s->board->gps_uarts) {
                error_report("%s: gps=%s: the GPS is not modelled on this board (off)", fb_mname, c);
            } else {
                error_report("%s: gps=%s: off or %s", fb_mname, c, s->board->uart_range);
            }
            exit(1);
        }
        if (s->gps_uart == s->crsf_uart || s->gps_uart == s->osd_uart) {
            error_report("%s: gps=%s: another device is on that USART", fb_mname, c);
            exit(1);
        }
    }
    s->home[0] = 40.1792;
    s->home[1] = 44.4991;
    s->home[2] = 0;                     /* the baro's field elevation, once it is known */
    if (s->gps_uart >= 0) {
        DevProp gp[12];
        fb_ublox_config_t gc;
        dev_props_or_exit("ublox", s->gps_props_str, gp, ARRAY_SIZE(gp));
        fb_ublox_default_config(&gc);
        gc.generation = strcmp(dev_prop(gp, "generation")->str, "m8") ? FB_UBLOX_M10 : FB_UBLOX_M8;
        gc.baud = (uint32_t)dev_prop(gp, "baud")->v;
        gc.rate_hz = (uint32_t)dev_prop(gp, "rate_hz")->v;
        gc.fix_delay_s = dev_prop(gp, "fix_delay_s")->v;
        gc.noise_m = dev_prop(gp, "noise_m")->v;
        gc.sats = (int)dev_prop(gp, "sats")->v;
        gc.pdop = dev_prop(gp, "pdop")->v;
        gc.h_acc_m = dev_prop(gp, "h_acc_m")->v;
        gc.seed = (uint64_t)dev_prop(gp, "seed")->v;
        gc.epoch_unix = (int64_t)dev_prop(gp, "epoch_unix")->v;
        s->gps = fb_ublox_new(&gc);
        s->gps_wire = fbwire_new("fbgps-wire", gps_wire_tx, s);
        s->gps_timer = nx_timer_new("gps.tick", gps_tick, s);
    }
    s->osd = fb_msp_osd_new(OSD_ROWS, OSD_COLS);
    s->osd_wire = fbwire_new("fbosd-wire", osd_wire_tx, s);
    s->osd_timer = nx_timer_new("osd.tx", osd_tx_tick, s);
    s->osd_watch = nx_timer_new("osd.watch", osd_watch_tick, s);
    s->osd_poll = nx_timer_new("osd.poll", osd_poll_tick, s);
    fb_msp_osd_set_poll(s->osd, s->osd_poll_on);
    if (s->osd_uart >= 0 && s->osd_log_path && *s->osd_log_path) {
        /* g_fopen (not fopen): the path is a -M property value, UTF-8 (system/main.c re-derives
         * argv from the wide command line on Windows); g_fopen converts to UTF-16 there. Same for
         * imu-log/fdm-log below. */
        s->osd_log = g_fopen(s->osd_log_path, "wb");
        if (!s->osd_log) {
            error_report("%s: osd-log=%s: %s", fb_mname, s->osd_log_path, strerror(errno));
            exit(1);
        }
        setvbuf(s->osd_log, NULL, _IOFBF, 64 * KiB);
    }
    if (s->osd_uart >= 0 && s->osd_poll_on) {
        timer_mod(s->osd_poll, fb_msp_osd_poll_in_ns(s->osd) + 1);
    }
    {
        Chardev *chr = qemu_chr_find("fbpilot");
        if (chr) {
            qemu_chr_fe_init(&s->pilot_chr, chr, &error_fatal);
            outq_init(&s->pilot_out, &s->pilot_chr, "pilot socket", 16 * KiB, true);
            qemu_chr_fe_set_handlers(&s->pilot_chr, pilot_can_read,
                                     pilot_read, pilot_event, NULL, s, NULL, true);
        }
    }

    /* USARTs: serial_hd(0) is the MSP/CLI host UART (board->msp_uart), serial_hd(1..) the
     * others in order */
    for (int i = 0, hd = 1; i < soc->nusart; i++) {
        const FbSocUsart *u = &soc->usart[i];
        bool msp = i == s->board->msp_uart;
        DeviceState *dev;
        if (soc->usart_model == FB_USART_F2XX) {
            object_initialize_child(OBJECT(machine), "usart[*]", &s->usart_f2[i],
                                    TYPE_STM32F2XX_USART);
        } else {
            object_initialize_child(OBJECT(machine), "usart[*]", &s->usart[i],
                                    u->uart ? TYPE_STM32L4X5_UART : TYPE_STM32L4X5_USART);
        }
        dev = DEVICE(usart_sbd(s, i));
        Chardev *chr = i == s->crsf_uart ? s->crsf_wire
                     : i == s->osd_uart ? s->osd_wire
                     : i == s->gps_uart ? s->gps_wire : serial_hd(msp ? 0 : hd);
        if (!msp) {
            hd++;
        }
        if (msp) {
            s->msp_uart = host_uart_new(s, i, chr);     /* also the server's MSP port */
            chr = s->msp_uart->wire;
        } else if (chr && i != s->crsf_uart && i != s->osd_uart && i != s->gps_uart) {
            chr = host_uart_new(s, i, chr)->wire;
        }
        qdev_prop_set_chr(dev, "chardev", chr);
        if (soc->usart_model == FB_USART_L4X5) {
            qdev_connect_clock_in(dev, "clk", u->bus == FB_APB2 ? s->pclk2 : s->pclk1);
        }
        sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
        memory_region_add_subregion_overlap(sysmem, u->base,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0), 0);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(armv7m, u->irq));
        if (i == s->crsf_uart) {
            crsf_usart_hook(s, sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0));
        }
        if (i == s->gps_uart) {
            gps_usart_hook(s, sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0),
                           u->bus == FB_APB2 ? soc->pclk2_hz : soc->pclk1_hz);
        }
    }

    /* GPIO A..D, EXTI, SPI1, DMA2 above the stub window */
    memory_region_init_io(&s->gpio_mr, OBJECT(machine), &gpio_ops, s,
                          mr_name_tmp(s, "gpio"), NUM_GPIO * 0x400);
    memory_region_add_subregion_overlap(sysmem, GPIO_BASE, &s->gpio_mr, 0);
    memory_region_init_io(&s->exti_mr, OBJECT(machine), &exti_ops, s,
                          mr_name_tmp(s, "exti"), 0x400);
    memory_region_add_subregion_overlap(sysmem, EXTI_BASE, &s->exti_mr, 0);
    for (int i = 0; i < 2; i++) {
        hwaddr base = i ? soc->spi2_base : SPI1_BASE;
        s->spi[i].s = s;
        s->spi[i].index = i;
        if (!base) {
            continue;                   /* SPI2 of the F722: an empty bus in the stub window */
        }
        memory_region_init_io(&s->spi[i].mr, OBJECT(machine), &spi1_ops, &s->spi[i],
                              mr_name_tmp(s, i ? "spi2" : "spi1"), 0x400);
        memory_region_add_subregion_overlap(sysmem, base, &s->spi[i].mr, 0);
    }
    if (s->board->osd_cs_port >= 0) {
        s->osd_chip = fb_max7456_new();
    }
    memory_region_init_io(&s->dma2_mr, OBJECT(machine), &dma2_ops, s,
                          mr_name_tmp(s, "dma2"), 0x400);
    memory_region_add_subregion_overlap(sysmem, DMA2_BASE, &s->dma2_mr, 0);
    for (int i = 0; i < 7; i++) {
        s->exti_irq[i] = qdev_get_gpio_in(armv7m, soc->exti_irq[i]);
    }
    for (int n = 0; n < 8; n++) {
        s->dma2_irq[n] = qdev_get_gpio_in(armv7m, soc->dma2_irq[n]);
        s->dma2[n].s = s;
        s->dma2[n].timer = nx_timer_new("dma2.done", dma2_done, &s->dma2[n]);
    }

    /* the I2C controller (I2C3 on the F722, I2C1 on the F405) + the onboard baro at 0x76 (the
     * driver's default address: DPS310 on the Nexus-XR, SPL06-001 on the F405), a wired compass */
    if (soc->i2c_model == FB_I2C_V1) {
        i2c_v1_port_init(s, &s->i2c1, mr_name_tmp(s, soc->i2c_name), soc->i2c_base,
                         soc->i2c_ev_irq, soc->i2c_er_irq);
    } else {
        i2c_port_init(s, &s->i2c3, mr_name_tmp(s, soc->i2c_name), soc->i2c_base,
                      soc->i2c_ev_irq, soc->i2c_er_irq);
    }
    s->baro = fb_dps310_new();
    s->baro_timer = nx_timer_new("baro.sample", baro_sample, s);
    s->baro_p0 = 101325.0;
    s->baro_field_m = 0.0;
    s->baro_t0 = 22.0;
    if (s->baro_env_str && *s->baro_env_str) {
        double p0, h0, t0;
        if (sscanf(s->baro_env_str, "%lf:%lf:%lf", &p0, &h0, &t0) == 3 &&
            p0 >= 30000 && p0 <= 120000 && h0 >= -500 && h0 <= 9000 && t0 >= -60 && t0 <= 60) {
            s->baro_p0 = p0;
            s->baro_field_m = h0;
            s->baro_t0 = t0;
        } else {
            error_report("%s: baro-env=%s: need sea_level_pa:field_elevation_m:temp_c "
                         "(30000..120000, -500..9000, -60..60)", fb_mname, s->baro_env_str);
            exit(1);
        }
    }
    baro_environment(s, 0.0);
    if (s->baro_present && !s->board->baro) {
        warn_report("%s: baro=on: no baro model on this board (the I2C bus stays empty)", fb_mname);
        s->baro_present = false;
    }
    if (s->board->baro_spl06) {
        DevProp bp[4];
        dev_props_or_exit("spl06", s->baro_props_str, bp, ARRAY_SIZE(bp));
        s->spl06 = fb_spl06_new();
        fb_spl06_set_noise(s->spl06, dev_prop(bp, "noise_scale")->v, 1);
        s->spl06_timer = nx_timer_new("spl06.conversion", spl06_timer_cb, s);
        i2c_add_slave(s, &(I2cSlave) {
            .addr = FB_SPL06_I2C_ADDR, .present = s->baro_present, .start_ack = spl06_start,
            .write = spl06_write, .read = spl06_read, .stop = spl06_stop, .opaque = s,
        });
    } else {
        i2c_add_slave(s, &(I2cSlave) {
            .addr = FB_DPS310_I2C_ADDR, .present = s->baro_present, .start = baro_start,
            .write = baro_write, .read = baro_read, .stop = baro_stop, .opaque = s,
        });
    }
    s->home[2] = s->baro_field_m;       /* the GPS's origin: the field the baro stands on */
    if (s->gps) {
        fb_ublox_set_home(s->gps, s->home[0], s->home[1], s->home[2]);
    }
    if (s->mag_present && !s->board->mag) {
        warn_report("%s: mag=on: no compass on this board's I2C bus", fb_mname);
        s->mag_present = false;
    }
    if (s->mag_present) {
        DevProp mp[8];
        dev_props_or_exit("qmc5883l", s->mag_props_str, mp, ARRAY_SIZE(mp));
        s->mag = fb_qmc5883l_new();
        s->mag_mount = fb_mag_align_parse(dev_prop(mp, "mount")->str);
        fb_mag_field_ned(dev_prop(mp, "declination_deg")->v, dev_prop(mp, "inclination_deg")->v,
                         dev_prop(mp, "intensity_ut")->v, s->mag_ned);
        fb_qmc5883l_set_noise(s->mag, dev_prop(mp, "noise_mg")->v, 1);
        i2c_add_slave(s, &(I2cSlave) {
            .addr = FB_QMC5883L_I2C_ADDR, .present = true, .start_ack = mag_start,
            .write = mag_write, .read = mag_read, .opaque = s,
        });
    }
    if (s->board->adc) {
        memory_region_init_io(&s->adc_mr, OBJECT(machine), &adc_ops, s, mr_name_tmp(s, "adc"),
                              0x400);
        memory_region_add_subregion_overlap(sysmem, ADC_BASE, &s->adc_mr, 0);
    }
    if (s->battery_present && !s->board->adc) {
        warn_report("%s: battery=on: this board's ADC is not modelled (no battery meter)", fb_mname);
        s->battery_present = false;
    }
    if (s->battery_present) {
        DevProp bp[8];
        fb_battery_config_t bc;
        dev_props_or_exit("battery", s->battery_props_str, bp, ARRAY_SIZE(bp));
        fb_battery_default_config(&bc);
        bc.cells = (int)dev_prop(bp, "cells")->v;
        bc.capacity_mah = dev_prop(bp, "capacity_mah")->v;
        bc.start_soc = dev_prop(bp, "start_soc")->v;
        bc.resistance_mohm = dev_prop(bp, "resistance_mohm")->v;
        bc.current_scale = dev_prop(bp, "current_scale")->v;
        bc.fc_current_a = dev_prop(bp, "fc_current_a")->v;
        s->batt = fb_battery_new(&bc);
    }

    /* Timers, DShot/ESC, physics tick */
    for (int i = 0; i < soc->ntim; i++) {
        s->tim[i].s = s;
        memory_region_init_io(&s->tim[i].mr, OBJECT(machine), &tim_ops,
                              &s->tim[i], soc->tim[i].name, 0x400);
        memory_region_add_subregion_overlap(sysmem, soc->tim[i].base,
                                            &s->tim[i].mr, 0);
    }
    {
        fb_esc_params_t ep;
        fb_esc_default_params(&ep);
        s->main_gear = 1.0;
        aircraft_load(s, &ep);
        s->esc = fb_esc_new(&ep);
        char e[200];
        size_t pl = s->fdm_params_json ? strlen(s->fdm_params_json) : 0;
        s->fdm = fb_fdm_new(s->fdm_kind, s->fdm_params_json, pl, FDM_DT_NS / 1e9, e, sizeof e);
        if (!s->fdm) {
            error_report("%s: aircraft-file %s: fdm_params.%s", fb_mname,
                         s->aircraft_file ? s->aircraft_file : "(none)", e);
            exit(1);
        }
        for (int k = 0; k < fb_fdm_input_count(s->fdm) && k < 4; k++) {
            if (strcmp(fb_fdm_input_name(s->fdm, k), s->fdm_in_names[k])) {
                error_report("%s: aircraft-file %s: fdm_inputs[%d] is %s, the model's input %d is %s",
                             fb_mname, s->aircraft_file ? s->aircraft_file : "(none)", k,
                             s->fdm_in_names[k], k, fb_fdm_input_name(s->fdm, k));
                exit(1);
            }
        }
        env_init(s);
    }
    fb_esc_set_signal_timeout(s->esc, s->esc_timeout_ms / 1000.0f);
    s->esc_reply_timer = nx_timer_new("esc.reply", esc_reply, s);
    for (int i = 0; i < FB_MAX_SERVOS; i++) {
        s->servo_us[i] = 1500;
    }
    for (int i = 0; i < 4; i++) {
        s->fdm_in_us[i] = 1500;
    }
    if (s->imu_log_path && *s->imu_log_path) {
        s->imu_log = g_fopen(s->imu_log_path, "w");
        if (!s->imu_log) {
            error_report("%s: cannot open imu-log %s", fb_mname, s->imu_log_path);
            exit(1);
        }
        setvbuf(s->imu_log, NULL, _IOFBF, 1 << 20);
    }
    if (s->fdm_log_path && *s->fdm_log_path) {
        s->fdm_log = g_fopen(s->fdm_log_path, "w");
        if (!s->fdm_log) {
            error_report("%s: cannot open fdm-log %s", fb_mname, s->fdm_log_path);
            exit(1);
        }
        setvbuf(s->fdm_log, NULL, _IOLBF, 0);
        fdm_log_header(s);
    }
    if (!s->fdm_log_ms) {
        s->fdm_log_ms = 10;
    }
    s->fdm_timer = nx_timer_new("fdm.tick", fdm_tick, s);
    s->fdm_next_ns = FDM_DT_NS;
    timer_mod(s->fdm_timer, s->fdm_next_ns);

    /* IMU on SPI1 + motion source */
    s->imu = fb_icm42688p_new();
    s->imu_sample_timer = nx_timer_new("imu.sample", imu_sample, s);
    s->imu_pulse_timer = nx_timer_new("imu.pulse_end", imu_pulse_end, s);
    s->rng = 0x9E3779B97F4A7C15ULL;
    s->motion = (MotionSource) {
        .accel_g = { 0, 0, 1.0f },      /* see core/qemu/README.md: axes */
        .temp_c = 30.0f,
        .gyro_noise_dps = 0.05f,
        .accel_noise_g = 0.002f,
        .temp_swing_c = 1.0f,
        .temp_period_s = 600.0f,
    };
    {
        Chardev *chr = qemu_chr_find("fbmotion");
        if (chr) {
            qemu_chr_fe_init(&s->motion_chr, chr, &error_fatal);
            outq_init(&s->motion_out, &s->motion_chr, "motion socket", 16 * KiB, true);
            qemu_chr_fe_set_handlers(&s->motion_chr, motion_can_read,
                                     motion_read, motion_event, NULL, s, NULL, true);
        }
    }

    /* session options */
    s->beeper_port = s->beeper_pin = -1;
    if (s->beeper_str && *s->beeper_str) {
        const char *b = s->beeper_str;
        if (b[0] >= 'A' && b[0] < 'A' + NUM_GPIO && g_ascii_isdigit(b[1])) {
            s->beeper_port = b[0] - 'A';
            s->beeper_pin = atoi(b + 1) & 15;
            s->beeper_inverted = strstr(b, ":inv") != NULL;
        } else {
            warn_report("%s: beeper=%s: only ports A..D are modelled", fb_mname, b);
        }
    }
    if (s->imu_noise_str && *s->imu_noise_str) {
        float g, a;
        if (sscanf(s->imu_noise_str, "%f:%f", &g, &a) == 2 && g >= 0 && a >= 0) {
            s->motion.gyro_noise_dps = g;
            s->motion.accel_noise_g = a;
        } else {
            warn_report("%s: imu-noise=%s: need gyro_dps:accel_g", fb_mname, s->imu_noise_str);
        }
    }
    if (s->imu_accel_error_str && *s->imu_accel_error_str) {
        float mg, pct;
        if (sscanf(s->imu_accel_error_str, "%f:%f", &mg, &pct) == 2 && fabsf(mg) <= 500 &&
            fabsf(pct) <= 10) {
            s->motion.accel_offset_g = mg / 1000;
            s->motion.accel_gain = 1 + pct / 100;
            s->motion.accel_error = mg != 0 || pct != 0;
        } else {
            error_report("%s: imu-accel-error=%s: need offset_mg:scale_pct (|mg| <= 500, "
                         "|pct| <= 10)", fb_mname, s->imu_accel_error_str);
            exit(1);
        }
    }
    if (s->actuators_str && *s->actuators_str) {
        int n = s->board->nservo + 1;
        g_auto(GStrv) a = g_strsplit_set(s->actuators_str, ",:", n);
        for (int i = 0; a[i] && i < n; i++) {
            s->act_names[i] = g_strdup(g_strstrip(a[i]));
        }
    }
    for (int i = 0; i <= s->board->nservo; i++) {
        if (!s->act_names[i]) {
            const char *const *d = s->board->actuators;
            s->act_names[i] = d ? g_strdup(d[i]) : i < s->board->nservo
                            ? g_strdup_printf("servo%d", i + 1) : g_strdup("motor");
        }
    }
    fdm_map_init(s);
    if (s->config_cli_path && *s->config_cli_path) {
        g_autoptr(GError) ge = NULL;
        g_free(s->config_cli);
        s->config_cli = NULL;
        if (!g_file_get_contents(s->config_cli_path, &s->config_cli, NULL, &ge)) {
            error_report("%s: config-cli %s: %s", fb_mname, s->config_cli_path, ge->message);
            exit(1);
        }
    }
    ctl_init(s);
    if (s->env) {
        env_default_run(s);
    }
    prof_start(s);

    qemu_register_reset(nexus_xr_reset, s);
    if (s->firmware_path && *s->firmware_path) {
        /* The machine owns the flash image: no ROM blobs that QEMU would
         * re-apply at every reset, so a firmware reload survives resets. */
        Error *err = NULL;
        g_autofree uint8_t *img = fw_build_image(s, s->firmware_path, &err);
        if (!img) {
            error_report_err(err);
            exit(1);
        }
        fw_install(s, img);
        if (machine->kernel_filename) {
            error_report("%s: use either -kernel or fw=, not both", fb_mname);
            exit(1);
        }
    }
    nexus_xr_reset(s);

    armv7m_load_kernel(s->armv7m.cpu, machine->kernel_filename,
                       soc->flash_base, soc->flash_size);
    qemu_register_reset(nexus_xr_post_reset, s);
}

/* Counters for evidence/debug: HMP "qom-get /machine imu-samples" */
static void nexus_xr_get_imu_samples(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    NexusXRState *s = NEXUS_XR_MACHINE(obj);
    uint64_t n = s->imu_samples;
    visit_type_uint64(v, name, &n, errp);
}

static bool nexus_xr_get_imu(Object *obj, Error **errp)
{
    return NEXUS_XR_MACHINE(obj)->imu_present;
}

static void nexus_xr_set_imu(Object *obj, bool value, Error **errp)
{
    NEXUS_XR_MACHINE(obj)->imu_present = value;
}

static bool nexus_xr_get_esc(Object *obj, Error **errp)
{
    return NEXUS_XR_MACHINE(obj)->esc_present;
}

static void nexus_xr_set_esc(Object *obj, bool value, Error **errp)
{
    NEXUS_XR_MACHINE(obj)->esc_present = value;
}

static void nexus_xr_get_servos(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    visit_type_uint32(v, name, &NEXUS_XR_MACHINE(obj)->servo_mask, errp);
}

static void nexus_xr_set_servos(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    uint32_t m;
    if (visit_type_uint32(v, name, &m, errp)) {
        NexusXRState *s = NEXUS_XR_MACHINE(obj);
        s->servo_mask = m & ((1u << s->board->nservo) - 1);
    }
}

static bool nexus_xr_get_baro(Object *obj, Error **errp)
{
    return NEXUS_XR_MACHINE(obj)->baro_present;
}

static void nexus_xr_set_baro(Object *obj, bool value, Error **errp)
{
    NEXUS_XR_MACHINE(obj)->baro_present = value;
}

static bool nexus_xr_get_mag(Object *obj, Error **errp)
{
    return NEXUS_XR_MACHINE(obj)->mag_present;
}

static void nexus_xr_set_mag(Object *obj, bool value, Error **errp)
{
    NEXUS_XR_MACHINE(obj)->mag_present = value;
}

static bool nexus_xr_get_battery(Object *obj, Error **errp)
{
    return NEXUS_XR_MACHINE(obj)->battery_present;
}

static void nexus_xr_set_battery(Object *obj, bool value, Error **errp)
{
    NEXUS_XR_MACHINE(obj)->battery_present = value;
}

static void nexus_xr_instance_init(Object *obj)
{
    NexusXRState *s = NEXUS_XR_MACHINE(obj);
    s->board = NEXUS_XR_MACHINE_GET_CLASS(obj)->board;
    s->soc = s->board->soc;
    fb_mname = s->board->machine;
    NEXUS_XR_MACHINE(obj)->imu_present = true;
    NEXUS_XR_MACHINE(obj)->baro_present = s->board->baro;
    NEXUS_XR_MACHINE(obj)->esc_present = true;
    NEXUS_XR_MACHINE(obj)->servo_mask = (1u << s->board->nservo) - 1;     /* every servo pad */
    NEXUS_XR_MACHINE(obj)->esc_timeout_ms = FB_ESC_SIGNAL_TIMEOUT_MS_DEFAULT;
    NEXUS_XR_MACHINE(obj)->idle_skip_n = 16;
    NEXUS_XR_MACHINE(obj)->idle_skip_gran_us = 50;
    NEXUS_XR_MACHINE(obj)->given = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

static char *nexus_xr_get_idle_skip_mode(Object *obj, Error **errp)
{
    return g_strdup(NEXUS_XR_MACHINE(obj)->idle_skip_mode);
}

/*
 * idle-skip, idle-skip-mode and idle-skip-gran-us are read once in machine init (the NVIC
 * interception, the mode, the granularity in instructions), so they can only be set with -M;
 * later changes (qom-set) are refused instead of half applied. idle-skip-n is read on every
 * CYCCNT read and may be changed at any time.
 */
static bool idle_prop_frozen(NexusXRState *s, const char *name, Error **errp)
{
    if (s->idle_frozen) {
        error_setg(errp, "%s: %s can only be set with -M (machine is initialized)", fb_mname, name);
        return true;
    }
    return false;
}

static char *nexus_xr_get_idle_skip_src(Object *obj, Error **errp)
{
    return g_strdup(NEXUS_XR_MACHINE(obj)->idle_skip_src ?: "");
}

static void nexus_xr_set_idle_skip_src(Object *obj, const char *value, Error **errp)
{
    NexusXRState *s = NEXUS_XR_MACHINE(obj);
    if (idle_prop_frozen(s, "idle-skip-src", errp)) {
        return;
    }
    g_free(s->idle_skip_src);
    s->idle_skip_src = g_strdup(value);
}

static void nexus_xr_set_idle_skip_mode(Object *obj, const char *value, Error **errp)
{
    NexusXRState *s = NEXUS_XR_MACHINE(obj);
    if (idle_prop_frozen(s, "idle-skip-mode", errp)) {
        return;
    }
    g_free(s->idle_skip_mode);
    s->idle_skip_mode = g_strdup(value);
}

static bool nexus_xr_get_idle_skip(Object *obj, Error **errp)
{
    return NEXUS_XR_MACHINE(obj)->idle_skip;
}

static void nexus_xr_set_idle_skip(Object *obj, bool value, Error **errp)
{
    NexusXRState *s = NEXUS_XR_MACHINE(obj);
    if (idle_prop_frozen(s, "idle-skip", errp)) {
        return;
    }
    s->idle_skip = value;
}

static bool nexus_xr_get_io_tb(Object *obj, Error **errp)
{
    return NEXUS_XR_MACHINE(obj)->io_tb;
}

static void nexus_xr_set_io_tb(Object *obj, bool value, Error **errp)
{
    NexusXRState *s = NEXUS_XR_MACHINE(obj);
    if (idle_prop_frozen(s, "io-tb", errp)) {
        return;
    }
    s->io_tb = value;
}

static void nexus_xr_get_idle_u32(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    NexusXRState *s = NEXUS_XR_MACHINE(obj);
    uint32_t *f = !strcmp(name, "idle-skip-n") ? &s->idle_skip_n : &s->idle_skip_gran_us;
    visit_type_uint32(v, name, f, errp);
}

static void nexus_xr_set_idle_u32(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    NexusXRState *s = NEXUS_XR_MACHINE(obj);
    bool is_n = !strcmp(name, "idle-skip-n");
    uint32_t val;
    if (!visit_type_uint32(v, name, &val, errp)) {
        return;
    }
    if (!is_n && idle_prop_frozen(s, name, errp)) {
        return;
    }
    if (is_n ? (val < 2 || val > 1000) : (val < 1 || val > 100000)) {
        error_setg(errp, is_n ? "idle_skip_n: 2..1000" : "idle_skip_gran_us: 1..100000");
        return;
    }
    *(is_n ? &s->idle_skip_n : &s->idle_skip_gran_us) = val;
}

static void nexus_xr_get_esc_timeout_ms(Object *obj, Visitor *v, const char *name,
                                        void *opaque, Error **errp)
{
    visit_type_uint32(v, name, &NEXUS_XR_MACHINE(obj)->esc_timeout_ms, errp);
}

static void nexus_xr_set_esc_timeout_ms(Object *obj, Visitor *v, const char *name,
                                        void *opaque, Error **errp)
{
    uint32_t ms;
    if (!visit_type_uint32(v, name, &ms, errp)) {
        return;
    }
    if (ms > 2000) {
        error_setg(errp, "esc-timeout-ms: 0..2000");
        return;
    }
    NEXUS_XR_MACHINE(obj)->esc_timeout_ms = ms;
}

static void nexus_xr_get_imu_odr(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    NexusXRState *s = NEXUS_XR_MACHINE(obj);
    uint32_t n = s->imu_odr;
    visit_type_uint32(v, name, &n, errp);
}

static bool nexus_xr_get_fdm(Object *obj, Error **errp)
{
    return NEXUS_XR_MACHINE(obj)->fdm_on;
}

static void nexus_xr_set_fdm(Object *obj, bool value, Error **errp)
{
    NEXUS_XR_MACHINE(obj)->fdm_on = value;
}

static char *nexus_xr_get_fdm_log(Object *obj, Error **errp)
{
    return g_strdup(NEXUS_XR_MACHINE(obj)->fdm_log_path ?: "");
}

static void nexus_xr_set_fdm_log(Object *obj, const char *v, Error **errp)
{
    NexusXRState *s = NEXUS_XR_MACHINE(obj);
    g_free(s->fdm_log_path);
    s->fdm_log_path = g_strdup(v);
}

static void nexus_xr_get_fdm_log_ms(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    visit_type_uint32(v, name, &NEXUS_XR_MACHINE(obj)->fdm_log_ms, errp);
}

static void nexus_xr_set_fdm_log_ms(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    visit_type_uint32(v, name, &NEXUS_XR_MACHINE(obj)->fdm_log_ms, errp);
}

#define STR_ACCESSORS(field) \
static char *nexus_xr_get_##field(Object *obj, Error **errp) \
{ \
    return g_strdup(NEXUS_XR_MACHINE(obj)->field ?: ""); \
} \
static void nexus_xr_set_##field(Object *obj, const char *v, Error **errp) \
{ \
    NexusXRState *s = NEXUS_XR_MACHINE(obj); \
    g_free(s->field); \
    s->field = g_strdup(v); \
}
STR_ACCESSORS(firmware_path)
STR_ACCESSORS(imu_log_path)
STR_ACCESSORS(defaults_path)
STR_ACCESSORS(ctl_addr)
STR_ACCESSORS(aircraft)
STR_ACCESSORS(actuators_str)
STR_ACCESSORS(beeper_str)
STR_ACCESSORS(manifest_path)
STR_ACCESSORS(imu_noise_str)
STR_ACCESSORS(imu_accel_error_str)
STR_ACCESSORS(crsf_str)
STR_ACCESSORS(osd_str)
STR_ACCESSORS(osd_log_path)
STR_ACCESSORS(osd_poll_str)
STR_ACCESSORS(gps_str)
STR_ACCESSORS(baro_props_str)
STR_ACCESSORS(mag_props_str)
STR_ACCESSORS(gps_props_str)
STR_ACCESSORS(battery_props_str)
STR_ACCESSORS(aircraft_file)
STR_ACCESSORS(baro_env_str)
STR_ACCESSORS(session_path)
STR_ACCESSORS(session_root)
STR_ACCESSORS(config_cli_path)
STR_ACCESSORS(cfg_cache)
STR_ACCESSORS(bench_state)
STR_ACCESSORS(env_scenario)

static bool nexus_xr_get_session_check(Object *obj, Error **errp)
{
    return NEXUS_XR_MACHINE(obj)->session_check;
}

static void nexus_xr_set_session_check(Object *obj, bool value, Error **errp)
{
    NEXUS_XR_MACHINE(obj)->session_check = value;
}

/* Every option of this machine records that it was given: the bench sets only the others.
 * The class's own properties get these accessors in front of theirs (class_init). */
typedef struct {
    ObjectPropertyAccessor *get, *set;
    void *opaque;
} NxGivenProp;

static void nexus_xr_given_get(Object *obj, Visitor *v, const char *name, void *opaque,
                               Error **errp)
{
    NxGivenProp *w = opaque;
    w->get(obj, v, name, w->opaque, errp);
}

static void nexus_xr_given_set(Object *obj, Visitor *v, const char *name, void *opaque,
                               Error **errp)
{
    NexusXRState *s = NEXUS_XR_MACHINE(obj);
    NxGivenProp *w = opaque;
    Error *err = NULL;
    w->set(obj, v, name, w->opaque, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    if (!s->session_applying) {
        g_hash_table_add(s->given, g_strdup(name));
    }
}

static bool nexus_xr_get_realtime(Object *obj, Error **errp)
{
    return NEXUS_XR_MACHINE(obj)->realtime;
}

static void nexus_xr_set_realtime(Object *obj, bool value, Error **errp)
{
    NEXUS_XR_MACHINE(obj)->realtime = value;
}

static void nexus_xr_get_servo_rev(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    visit_type_uint32(v, name, &NEXUS_XR_MACHINE(obj)->servo_rev, errp);
}

static void nexus_xr_set_servo_rev(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    visit_type_uint32(v, name, &NEXUS_XR_MACHINE(obj)->servo_rev, errp);
}

static bool nexus_xr_get_prof(Object *obj, Error **errp)
{
    return NEXUS_XR_MACHINE(obj)->prof_on;
}

static void nexus_xr_set_prof(Object *obj, bool value, Error **errp)
{
    if (fbprof || prof_s) {
        error_setg(errp, "prof is fixed at machine init (-M prof=on)");
        return;
    }
    NEXUS_XR_MACHINE(obj)->prof_on = value;
}

static void nexus_xr_get_prof_period(Object *obj, Visitor *v, const char *name, void *opaque,
                                     Error **errp)
{
    visit_type_uint32(v, name, &NEXUS_XR_MACHINE(obj)->prof_period_s, errp);
}

static void nexus_xr_set_prof_period(Object *obj, Visitor *v, const char *name, void *opaque,
                                     Error **errp)
{
    visit_type_uint32(v, name, &NEXUS_XR_MACHINE(obj)->prof_period_s, errp);
}

/* One-line bench state for evidence: HMP "qom-get /machine bench" */
static char *nexus_xr_get_bench(Object *obj, Error **errp)
{
    NexusXRState *s = NEXUS_XR_MACHINE(obj);
    fb_fdm_state_t st;
    fb_fdm_state(s->fdm, &st);
    g_autoptr(GString) cyc = g_string_new(NULL);
    for (unsigned i = 0; i < s->idle_lock_p; i++) {
        g_string_append_printf(cyc, "%s%08x:%u", i ? "," : "", (uint32_t)(s->idle_lock[i] >> 32),
                               (uint32_t)s->idle_lock[i]);
    }
    int osd_mode = -1;
    fb_msp_osd_options(s->osd, NULL, &osd_mode);
    return g_strdup_printf(
        "t=%.3f servo_us=%u,%u,%u,%u dshot=%u bidir=%d frames=%" PRIu64
        " bad=%" PRIu64 " esc_replies=%" PRIu64 " esc_thr=%.3f motor_rpm=%.0f"
        " erpm=%u head_rpm=%.0f crsf_rx_frames=%u alt=%.2f fdm_steps=%" PRIu64
        " idle_skips=%" PRIu64 " idle_skipped_s=%.3f idle_extended=%" PRIu64 " idle_cycle=%s"
        " baro=%d baro_pa=%.1f baro_rate=%u baro_samples=%" PRIu64 " i2c3_xfers=%" PRIu64
        " i2c3_nacks=%" PRIu64 " i2c3_bytes=%" PRIu64 " baro_regs=%02x%02x%02x%02x%02x%02x"
        " baro_reg_pa=%.2f osd=%d osd_frames=%u osd_writes=%u osd_clipped=%u osd_contacts=%u"
        " osd_canvas_sent=%u osd_canvas_acks=%u osd_bad=%u osd_polls=%u osd_variants=%u osd_errors=%u"
        " osd_options=%u osd_mode=%d osd_fc=%s max7456_display_writes=%u max7456_clears=%u"
        " crsf_dropped=%" PRIu64 " vt_ns=%" PRId64,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e9, s->servo_us[0],
        s->servo_us[1], s->servo_us[2], s->servo_us[3], s->dshot_value,
        s->dshot_bidir, s->dshot_frames, s->dshot_bad, s->esc_replies,
        fb_esc_throttle(s->esc), fb_esc_rpm(s->esc), fb_esc_erpm(s->esc),
        fb_esc_rpm(s->esc) / s->main_gear, fb_crsf_rx_frames(s->crsf),
        -st.pos_ned[2], s->fdm_steps, s->idle_skips, idle_skipped_now(s) / 1e9, s->idle_extended,
        cyc->len ? cyc->str : "-", s->baro_present, s->baro_pa, s->baro_rate, s->baro_samples,
        s->i2c3.c.xfers, s->i2c3.c.nacks, s->i2c3.c.bytes,
        fb_dps310_peek(s->baro, 0), fb_dps310_peek(s->baro, 1), fb_dps310_peek(s->baro, 2),
        fb_dps310_peek(s->baro, 3), fb_dps310_peek(s->baro, 4), fb_dps310_peek(s->baro, 5),
        s->baro_present ? fb_dps310_expected_pressure_pa(s->baro) : NAN,
        s->osd_uart >= 0 ? s->osd_uart + 1 : 0, fb_msp_osd_stats(s->osd)->frames,
        fb_msp_osd_stats(s->osd)->writes, fb_msp_osd_stats(s->osd)->clipped,
        fb_msp_osd_stats(s->osd)->contacts, fb_msp_osd_stats(s->osd)->canvas_sent,
        fb_msp_osd_stats(s->osd)->canvas_acks, fb_msp_osd_stats(s->osd)->bad_checksum,
        fb_msp_osd_stats(s->osd)->polls, fb_msp_osd_stats(s->osd)->variants,
        fb_msp_osd_stats(s->osd)->errors, fb_msp_osd_stats(s->osd)->options, osd_mode,
        *fb_msp_osd_fc_variant(s->osd) ? fb_msp_osd_fc_variant(s->osd) : "-",
        s->osd_chip ? fb_max7456_stats(s->osd_chip)->display_writes : 0,
        s->osd_chip ? fb_max7456_stats(s->osd_chip)->clears : 0,
        s->crsf_dropped, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

/* a board's machine type: its description and CPU */
static void fb_board_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    const FbBoard *b = data;
    const char **cpus = g_new0(const char *, 2);

    cpus[0] = b->soc->cpu_type;
    NEXUS_XR_MACHINE_CLASS(oc)->board = b;
    mc->desc = b->desc;
    mc->default_cpu_type = b->soc->cpu_type;
    mc->valid_cpu_types = cpus;
}

static void nexus_xr_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = nexus_xr_init;
    mc->default_ram_size = 0;
    object_class_property_add(oc, "imu-samples", "uint64",
                              nexus_xr_get_imu_samples, NULL, NULL, NULL);
    object_class_property_add(oc, "imu-odr", "uint32",
                              nexus_xr_get_imu_odr, NULL, NULL, NULL);
    object_class_property_add_bool(oc, "imu", nexus_xr_get_imu,
                                   nexus_xr_set_imu);
    object_class_property_set_description(oc, "imu",
        "ICM-42688-P on SPI1 (default on; off = empty bus, for A/B runs)");
    object_class_property_add_bool(oc, "fdm", nexus_xr_get_fdm,
                                   nexus_xr_set_fdm);
    object_class_property_set_description(oc, "fdm",
        "helicopter FDM drives the IMU from the servo/motor outputs (default off)");
    object_class_property_add_str(oc, "imu-log", nexus_xr_get_imu_log_path,
                                  nexus_xr_set_imu_log_path);
    object_class_property_set_description(oc, "imu-log",
        "IMU trace file: per sample INT1 rise (S), INT1 fall (F), SPI1 DMA done (D), virtual ns");
    object_class_property_add_str(oc, "fdm-log", nexus_xr_get_fdm_log,
                                  nexus_xr_set_fdm_log);
    object_class_property_set_description(oc, "fdm-log",
        "CSV file for the FDM state (every fdm-log-ms of virtual time)");
    object_class_property_add(oc, "fdm-log-ms", "uint32",
                              nexus_xr_get_fdm_log_ms, nexus_xr_set_fdm_log_ms,
                              NULL, NULL);
    object_class_property_add_str(oc, "bench", nexus_xr_get_bench, NULL);

#define STR_PROP(name, field, desc) \
    object_class_property_add_str(oc, name, nexus_xr_get_##field, \
                                  nexus_xr_set_##field); \
    object_class_property_set_description(oc, name, desc);
    STR_PROP("fw", firmware_path,
             "firmware ELF/hex/bin loaded into flash by the machine (hot-reloadable)")
    STR_PROP("defaults", defaults_path,
             "custom-defaults blob (raw) placed at 0x08002808 with the firmware")
    STR_PROP("config-cli", config_cli_path,
             "CLI config script (boards configured over the CLI, e.g. iNav): sent over the MSP "
             "UART when the config sector is blank, then save; the saved sector is kept as a "
             "snapshot (firmware + script hash)")
    STR_PROP("cfg-cache", cfg_cache,
             "CLI boards: directory of the config-sector snapshots (script + ACC calibration) kept "
             "across runs, one file per firmware + script; default <user cache dir>/flightbench/config, "
             "off = in memory only")
    STR_PROP("bench-state", bench_state,
             "CLI boards: the bench's config sector on disk (+ <file>.json: firmware, the script "
             "applied, functions, diff); written after every save of the firmware, restored at "
             "start before the cfg-cache snapshot; a changed script is applied as a delta")
    STR_PROP("env-scenario", env_scenario,
             "plane FDM: the environment scenario from the start and after reset aircraft "
             "(models/<id>/env/<name>.json), instead of the aircraft's env_default")
    STR_PROP("ctl", ctl_addr, "control server address, e.g. 127.0.0.1:31250")
    STR_PROP("aircraft", aircraft, "aircraft name reported in hello")
    STR_PROP("actuators", actuators_str,
             "actuator names of servo1..4 and motor1, colon-separated")
    STR_PROP("beeper", beeper_str, "beeper pin (resource BEEPER), e.g. B02 or B02:inv")
    STR_PROP("manifest", manifest_path,
             "core manifest JSON file (core/qemu/scripts/manifest.py), sent after hello")
    STR_PROP("imu-noise", imu_noise_str,
             "IMU motion-source noise RMS, gyro_dps:accel_g (default 0.05:0.002)")
    STR_PROP("imu-accel-error", imu_accel_error_str,
             "static accelerometer error offset_mg:scale_pct (every axis): the chip reads "
             "accel * (1 + pct/100) + mg/1000 (default none)")
    STR_PROP("crsf", crsf_str,
             "CRSF receiver: the USART it is wired to, uart3..uart6, or off (default uart5)")
    STR_PROP("osd", osd_str,
             "MSP DisplayPort OSD device (HD VTX): the USART it is wired to (nexus-xr uart3..uart6, "
             "speedybee-f405-wing uart1..uart5), or off (default off)")
    STR_PROP("osd-log", osd_log_path,
             "file: every byte the firmware sends to the MSP DisplayPort OSD device, raw "
             "(devices/tests/test_msp_osd.c replays such streams)")
    STR_PROP("osd-poll", osd_poll_str,
             "the OSD device's MSP_FC_VARIANT request every 125 ms, as a VTX polls: on|off (default: on "
             "for speedybee-f405-wing, iNav pushes its OSD only while polled; off for nexus-xr, "
             "RotorFlight pushes regardless)")
    STR_PROP("baro-env", baro_env_str,
             "baro environment sea_level_pa:field_elevation_m:temp_c (default 101325:0:22; temp_c at sea "
             "level, the lapse rate applies from 0 m); "
             "the chip sees the standard atmosphere at field elevation + FDM altitude")
    STR_PROP("aircraft-file", aircraft_file,
             "models/<id>/aircraft.json: transmission, ESC, FDM parameters, FDM input names")
    object_class_property_add_bool(oc, "baro", nexus_xr_get_baro, nexus_xr_set_baro);
    object_class_property_set_description(oc, "baro",
        "the onboard baro at 0x76: DPS310 on I2C3 (nexus-xr), SPL06-001 on I2C1 "
        "(speedybee-f405-wing) (default on; off = the address NACKs, no baro)");
    STR_PROP("baro-props", baro_props_str,
             "the SPL06's props, <prop>=<value>:...: noise_scale (devices/ spl06 descriptor)")
    object_class_property_add_bool(oc, "mag", nexus_xr_get_mag, nexus_xr_set_mag);
    object_class_property_set_description(oc, "mag",
        "a QMC5883L compass (a GPS module's) on the board's I2C bus at 0x0D (default off; "
        "speedybee-f405-wing)");
    STR_PROP("mag-props", mag_props_str,
             "the compass's props, <prop>=<value>:...: mount, declination_deg, inclination_deg, "
             "intensity_ut, noise_mg (devices/ qmc5883l descriptor)")
    STR_PROP("gps", gps_str,
             "a u-blox GPS: the USART it is wired to (uart1..uart5 on speedybee-f405-wing) or off "
             "(default off)")
    STR_PROP("gps-props", gps_props_str,
             "the GPS's props, <prop>=<value>:...: generation, baud, rate_hz, fix_delay_s, noise_m, "
             "sats, pdop, h_acc_m, seed, epoch_unix (devices/ ublox descriptor)")
    object_class_property_add_bool(oc, "battery", nexus_xr_get_battery, nexus_xr_set_battery);
    object_class_property_set_description(oc, "battery",
        "a LiPo on the VBAT / CURRENT pads, read by ADC1 (default off; speedybee-f405-wing)");
    STR_PROP("battery-props", battery_props_str,
             "the battery's props, <prop>=<value>:...: cells, capacity_mah, start_soc, "
             "resistance_mohm, current_scale, fc_current_a (devices/ battery descriptor)")
    object_class_property_add_bool(oc, "esc", nexus_xr_get_esc, nexus_xr_set_esc);
    object_class_property_set_description(oc, "esc",
        "ESC on MOTOR 1 (default on; off = nothing decodes the DShot frames)");
    object_class_property_add(oc, "servos", "uint32", nexus_xr_get_servos,
                              nexus_xr_set_servos, NULL, NULL);
    object_class_property_set_description(oc, "servos",
        "servos wired to SERVO 1..4, bit i = SERVO i+1 (default 15); the FDM takes an actuator "
        "from the wired output named after it (actuators=, servo-rev=)");
    object_class_property_add_bool(oc, "realtime", nexus_xr_get_realtime,
                                   nexus_xr_set_realtime);
    object_class_property_add(oc, "servo-rev", "uint32", nexus_xr_get_servo_rev,
                              nexus_xr_set_servo_rev, NULL, NULL);
    object_class_property_add_bool(oc, "idle-skip", nexus_xr_get_idle_skip,
                                   nexus_xr_set_idle_skip);
    object_class_property_set_description(oc, "idle-skip",
        "fast-forward virtual time while the firmware idle-polls DWT CYCCNT (external WFI; "
        "needs -icount with a fixed shift, no record/replay; -M only)");
    object_class_property_add_bool(oc, "io-tb", nexus_xr_get_io_tb, nexus_xr_set_io_tb);
    object_class_property_set_description(oc, "io-tb",
        "under -icount, give each instruction found doing device I/O mid-TB a TB of its own "
        "instead of rewinding to it at every execution (faster MMIO polling; changes the idle "
        "skip's decisions, so not guest-identical to off; default off; -M only)");
    object_class_property_add_str(oc, "idle-skip-mode", nexus_xr_get_idle_skip_mode,
                                  nexus_xr_set_idle_skip_mode);
    object_class_property_set_description(oc, "idle-skip-mode",
        "idle skip: budget (take the skip out of the icount budget, default) or halt "
        "(WFI-style halt); -M only");
    object_class_property_add_str(oc, "idle-skip-src", nexus_xr_get_idle_skip_src,
                                  nexus_xr_set_idle_skip_src);
    object_class_property_set_description(oc, "idle-skip-src",
        "idle skip: the reads the detector watches, cyccnt (DWT CYCCNT; RotorFlight) or systick "
        "(SysTick VAL, micros(); iNav); default: the board's (nexus-xr cyccnt, speedybee-f405-wing "
        "systick); -M only");
    object_class_property_add(oc, "idle-skip-n", "uint32", nexus_xr_get_idle_u32,
                              nexus_xr_set_idle_u32, NULL, NULL);
    object_class_property_set_description(oc, "idle-skip-n",
        "idle skip: consecutive CYCCNT reads that must each match the read p back (period p "
        "= 2..8; ~n + p reads, at least 2 cycles) before the first skip (default 16)");
    object_class_property_add(oc, "idle-skip-gran-us", "uint32", nexus_xr_get_idle_u32,
                              nexus_xr_set_idle_u32, NULL, NULL);
    object_class_property_set_description(oc, "idle-skip-gran-us",
        "idle skip: longest single skip, us of virtual time; it also ends at the next "
        "virtual timer (default 50; -M only)");
    object_class_property_add_bool(oc, "prof", nexus_xr_get_prof, nexus_xr_set_prof);
    object_class_property_set_description(oc, "prof",
        "host-time ledger: where host time goes (MMIO regions, timers, sockets, control server, "
        "FDM, vCPU <-> main loop hand-offs), the control server's `prof` command (default off; "
        "-M only)");
    object_class_property_add(oc, "prof-period-s", "uint32", nexus_xr_get_prof_period,
                              nexus_xr_set_prof_period, NULL, NULL);
    object_class_property_set_description(oc, "prof-period-s",
        "with prof=on: send a `prof` message to every control client each N virtual seconds "
        "(default 0 = only on request)");
    object_class_property_add(oc, "esc-timeout-ms", "uint32", nexus_xr_get_esc_timeout_ms,
                              nexus_xr_set_esc_timeout_ms, NULL, NULL);
    object_class_property_set_description(oc, "esc-timeout-ms",
        "ESC signal-loss timeout: no DShot frame this long -> throttle 0 (default 100, 0 = never)");
    STR_PROP("session", session_path,
             "bench JSON (core/qemu/sessions/*.json): the machine resolves the wiring, options, "
             "custom defaults and manifest from it; explicit -M options win (docs/protocol.md, "
             "Launching the core)")
    STR_PROP("root", session_root,
             "data root of the bench (boards/, models/; relative bench paths resolve against it); "
             "default $FB_ROOT, else the nearest ancestor of the bench's directory with boards/")
    object_class_property_add_bool(oc, "session-check", nexus_xr_get_session_check,
                                   nexus_xr_set_session_check);
    object_class_property_set_description(oc, "session-check",
        "print the resolved bench (assembly, options, icount, ports, manifest, defaults blob) and "
        "exit 0, or 2 when it is rejected; without session=, print the manifest");

    {
        GHashTableIter it;
        gpointer key, val;
        g_hash_table_iter_init(&it, oc->properties);
        while (g_hash_table_iter_next(&it, &key, &val)) {
            ObjectProperty *op = val;
            NxGivenProp *w;
            if (!op->set || !op->get) {
                continue;
            }
            w = g_new(NxGivenProp, 1);
            *w = (NxGivenProp) { op->get, op->set, op->opaque };
            op->get = nexus_xr_given_get;
            op->set = nexus_xr_given_set;
            op->opaque = w;
            op->release = NULL;
        }
    }
}

static const TypeInfo nexus_xr_machine_types[] = {
    {
        .name           = TYPE_FB_STM32_MACHINE,
        .parent         = TYPE_MACHINE,
        .abstract       = true,
        .instance_size  = sizeof(NexusXRState),
        .class_size     = sizeof(FbMachineClass),
        .class_init     = nexus_xr_machine_class_init,
        .instance_init  = nexus_xr_instance_init,
        .interfaces     = arm_machine_interfaces,
    },
    {
        .name           = MACHINE_TYPE_NAME("nexus-xr"),
        .parent         = TYPE_FB_STM32_MACHINE,
        .class_init     = fb_board_class_init,
        .class_data     = &board_nexus_xr,
    },
    {
        .name           = MACHINE_TYPE_NAME("speedybee-f405-wing"),
        .parent         = TYPE_FB_STM32_MACHINE,
        .class_init     = fb_board_class_init,
        .class_data     = &board_speedybee_f405_wing,
    },
    {
        .name           = TYPE_CHARDEV_FBWIRE,
        .parent         = TYPE_CHARDEV,
        .instance_size  = sizeof(FbWireChardev),
        .class_init     = fbwire_class_init,
    },
};

DEFINE_TYPES(nexus_xr_machine_types)
