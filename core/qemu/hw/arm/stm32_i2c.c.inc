/*
 * STM32F7 I2C (v2 IP) master: register interface and bus state machine, emulator-independent.
 *
 * Included by nexus_xr.c (QEMU: MemoryRegion, QEMUTimer and qemu_irq live in its I2cPort
 * wrapper) and by core/qemu/tests/test_i2c.c (host unit tests, a fake clock). Plain C99: the
 * includer has <stdbool.h>, <stdint.h>, <stdio.h>, <string.h> in scope; the environment comes
 * in through I2cHooks (one timer, the EV/ER lines, a log line for things not modelled).
 *
 * RotorFlight drives the F7's I2C through the ST HAL (drivers/bus_i2c_hal.c): blocking
 * HAL_I2C_Mem_Read/Mem_Write poll ISR (TXIS, TC, TCR, RXNE, STOPF, NACKF) with HAL_GetTick
 * timeouts; the baro's cyclic read (dps310ReadUP -> i2cReadBuffer) is HAL_I2C_Mem_Read_IT:
 * the register address is sent polled (SOFTEND, wait TC), then a repeated START for reading
 * with AUTOEND and the EV interrupt (RXIE/STOPIE/NACKIE/TCIE/ERRIE) moves each byte from RXDR
 * and completes on STOPF. No DMA (TXDMAEN/RXDMAEN are never set), 7-bit addresses, master only.
 *
 * Modelled: CR1/CR2/OAR1/OAR2/TIMINGR/TIMEOUTR storage, ISR/ICR, RXDR/TXDR; START (address
 * phase, then TXIS or the first received byte), NBYTES counting with RELOAD (TCR) / AUTOEND
 * (STOP) / neither (TC, the master then writes START again or STOP), STOP, NACK from an absent
 * address or on a data byte (NACKF, then the automatic STOP -> STOPF, BUSY released), clock
 * stretching (the next byte is received only after RXDR is read, also across a RELOAD),
 * CR1.PE = 0 as the peripheral reset, the EV/ER interrupt lines.
 * START/STOP (RM0410 32.7.8): set by software, cleared by hardware, a write of 0 has no effect;
 * both are ignored while PE = 0. START while the bus is idle or after TC/TCR starts at once;
 * START during a STOP is generated after that STOP; START during a transfer is a repeated START
 * at the end of NBYTES (instead of TC; RELOAD = 0, AUTOEND = 0) - with AUTOEND after its STOP.
 * STOP while waiting for software (TC/TCR, TXIS, RXDR full) is generated at once; STOP while a
 * byte (or the address) is on the wire is generated after it; STOP on an idle bus is ignored.
 * Neither bit stays set once its condition has been generated (or PE cleared).
 * Bus timing in virtual time from TIMINGR: one SCL period =
 * (PRESC + 1) * (SCLL + 1 + SCLH + 1) / PCLK1 (400 kHz if TIMINGR is 0); address phase 10 bit
 * times (START + 8 + ACK), a byte 9, STOP 1. Not modelled (first use logged): DMA requests,
 * 10-bit addressing, slave mode (OAR1 is stored), SMBus/PEC, timeouts.
 */

#define I2C_MAX_SLAVES  4

/* A device on an I2C bus: 7-bit address, byte-level callbacks */
typedef struct I2cSlave {
    uint8_t addr;
    bool present;
    void (*start)(void *opaque, bool read);     /* addressed (ACK) */
    bool (*write)(void *opaque, uint8_t b);     /* false = NACK */
    uint8_t (*read)(void *opaque);
    void (*stop)(void *opaque);                 /* STOP (end of the transaction) */
    void *opaque;
    /* optional, the F4 (v1) controller: the address phase instead of start(); false = NACK (a
     * device that can leave the bus: a fault). NULL = start() and always ACK */
    bool (*start_ack)(void *opaque, bool read);
} I2cSlave;

/* What the controller needs from its environment */
typedef struct I2cHooks {
    void (*timer_mod)(void *opaque, int64_t delay_ns);   /* (re)arm the one timer: i2c_core_timer */
    void (*timer_del)(void *opaque);
    void (*irq)(void *opaque, bool ev, bool er);         /* level of the EV / ER lines */
    void (*log_unimp)(void *opaque, const char *msg);    /* one line, not modelled / no device */
    void *opaque;
} I2cHooks;

enum { I2C_PH_IDLE, I2C_PH_ADDR, I2C_PH_TX, I2C_PH_TXBYTE, I2C_PH_RX, I2C_PH_RXWAIT,
       I2C_PH_WAIT, I2C_PH_STOP };

typedef struct I2cCtl {
    const char *name;
    uint64_t pclk_hz;
    I2cHooks hooks;
    uint32_t cr1, cr2, oar1, oar2, timingr, timeoutr, isr;
    uint8_t rxdr, txdr;
    int phase;                  /* I2C_PH_*: what the bus is doing / what the timer ends */
    unsigned left;              /* bytes left of NBYTES */
    bool read;
    I2cSlave *cur;              /* addressed slave, NULL = none */
    I2cSlave slaves[I2C_MAX_SLAVES];
    int nslaves;
    uint64_t xfers, nacks, bytes;
    uint8_t nack_seen[128 / 8];
    bool warned_dma, warned_slave, warned_add10, warned_smbus;
} I2cCtl;

enum { I2C_CR1 = 0x00, I2C_CR2 = 0x04, I2C_OAR1 = 0x08, I2C_OAR2 = 0x0C, I2C_TIMINGR = 0x10,
       I2C_TIMEOUTR = 0x14, I2C_ISR = 0x18, I2C_ICR = 0x1C, I2C_PECR = 0x20, I2C_RXDR = 0x24,
       I2C_TXDR = 0x28 };
#define I2C_ISR_TXE     (1u << 0)
#define I2C_ISR_TXIS    (1u << 1)
#define I2C_ISR_RXNE    (1u << 2)
#define I2C_ISR_NACKF   (1u << 4)
#define I2C_ISR_STOPF   (1u << 5)
#define I2C_ISR_TC      (1u << 6)
#define I2C_ISR_TCR     (1u << 7)
#define I2C_ISR_ERRS    (0x3Fu << 8)        /* BERR ARLO OVR PECERR TIMEOUT ALERT */
#define I2C_ISR_BUSY    (1u << 15)
#define I2C_ISR_DIR     (1u << 16)
#define I2C_CR1_PE      (1u << 0)
#define I2C_CR1_TXIE    (1u << 1)
#define I2C_CR1_RXIE    (1u << 2)
#define I2C_CR1_ADDRIE  (1u << 3)
#define I2C_CR1_NACKIE  (1u << 4)
#define I2C_CR1_STOPIE  (1u << 5)
#define I2C_CR1_TCIE    (1u << 6)
#define I2C_CR1_ERRIE   (1u << 7)
#define I2C_CR1_DMAEN   ((1u << 14) | (1u << 15))
#define I2C_CR2_RD_WRN  (1u << 10)
#define I2C_CR2_ADD10   (1u << 11)
#define I2C_CR2_START   (1u << 13)
#define I2C_CR2_STOP    (1u << 14)
#define I2C_CR2_RELOAD  (1u << 24)
#define I2C_CR2_AUTOEND (1u << 25)
#define I2C_CR2_NBYTES(v) (((v) >> 16) & 0xFF)

/* byte-lane access to a 32-bit register (off & 3, size 1..4) */
static uint32_t i2c_lane_get(uint32_t reg, unsigned off, unsigned size)
{
    uint32_t v = reg >> ((off & 3) * 8);
    return size >= 4 ? v : v & ((1u << (size * 8)) - 1);
}

static uint32_t i2c_lane_put(uint32_t reg, unsigned off, unsigned size, uint32_t val)
{
    unsigned sh = (off & 3) * 8;
    uint32_t m = size >= 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1);
    return (reg & ~(m << sh)) | ((val & m) << sh);
}

static void i2c_unimp(I2cCtl *c, bool *once, const char *what)
{
    char line[160];
    if (once) {
        if (*once) {
            return;
        }
        *once = true;
    }
    snprintf(line, sizeof(line), "nexus-xr: %s %s", c->name, what);
    if (c->hooks.log_unimp) {
        c->hooks.log_unimp(c->hooks.opaque, line);
    }
}

static I2cSlave *i2c_find(I2cCtl *c, uint8_t addr7)
{
    for (int i = 0; i < c->nslaves; i++) {
        if (c->slaves[i].addr == addr7 && c->slaves[i].present) {
            return &c->slaves[i];
        }
    }
    return NULL;
}

static void i2c_update_irq(I2cCtl *c)
{
    uint32_t isr = c->isr, cr1 = c->cr1;
    bool ev = ((cr1 & I2C_CR1_TXIE) && (isr & I2C_ISR_TXIS)) ||
              ((cr1 & I2C_CR1_RXIE) && (isr & I2C_ISR_RXNE)) ||
              ((cr1 & I2C_CR1_NACKIE) && (isr & I2C_ISR_NACKF)) ||
              ((cr1 & I2C_CR1_STOPIE) && (isr & I2C_ISR_STOPF)) ||
              ((cr1 & I2C_CR1_TCIE) && (isr & (I2C_ISR_TC | I2C_ISR_TCR)));
    bool er = (cr1 & I2C_CR1_ERRIE) && (isr & I2C_ISR_ERRS);
    c->hooks.irq(c->hooks.opaque, ev, er);
}

/* one SCL period in ns, from TIMINGR (PRESC 31:28, SCLH 15:8, SCLL 7:0) and PCLK1 */
static int64_t i2c_bit_ns(I2cCtl *c)
{
    uint32_t t = c->timingr;
    if (!t) {
        return 1000000000 / 400000;
    }
    uint64_t ticks = (uint64_t)((t >> 28) + 1) * (((t >> 8) & 0xFF) + 1 + (t & 0xFF) + 1);
    int64_t ns = (int64_t)(ticks * 1000000000ULL / c->pclk_hz);
    return ns < 100 ? 100 : ns;
}

static void i2c_schedule(I2cCtl *c, int phase, unsigned bits)
{
    c->phase = phase;
    c->hooks.timer_mod(c->hooks.opaque, (int64_t)bits * i2c_bit_ns(c));
}

static void i2c_stop_cond(I2cCtl *c)
{
    c->isr &= ~(I2C_ISR_TC | I2C_ISR_TCR | I2C_ISR_TXIS);
    i2c_schedule(c, I2C_PH_STOP, 1);
}

static void i2c_start_cond(I2cCtl *c)
{
    c->isr &= ~(I2C_ISR_TC | I2C_ISR_TCR);
    c->isr |= I2C_ISR_BUSY;
    c->read = c->cr2 & I2C_CR2_RD_WRN;
    c->left = I2C_CR2_NBYTES(c->cr2);
    if (c->cr2 & I2C_CR2_ADD10) {
        i2c_unimp(c, &c->warned_add10, "10-bit addressing not modelled");
    }
    i2c_schedule(c, I2C_PH_ADDR, 10);   /* START (or repeated START) + address + ACK */
}

/* NBYTES transferred: reload, automatic STOP, a latched repeated START, or wait (TC) */
static void i2c_end_of_count(I2cCtl *c)
{
    if (c->cr2 & I2C_CR2_RELOAD) {
        c->isr |= I2C_ISR_TCR;
        c->phase = I2C_PH_WAIT;
    } else if (c->cr2 & I2C_CR2_AUTOEND) {
        i2c_stop_cond(c);               /* a latched START follows the STOP */
    } else if (c->cr2 & I2C_CR2_START) {
        i2c_start_cond(c);              /* START written during the transfer: repeated START */
    } else {
        c->isr |= I2C_ISR_TC;
        c->phase = I2C_PH_WAIT;
    }
}

/* the timer: the address / byte / STOP on the wire is complete */
static void i2c_core_timer(I2cCtl *c)
{
    switch (c->phase) {
    case I2C_PH_ADDR: {
        uint8_t a7 = (c->cr2 >> 1) & 0x7F;
        c->cr2 &= ~I2C_CR2_START;
        c->xfers++;
        c->cur = i2c_find(c, a7);
        if (!c->cur) {
            c->nacks++;
            if (!(c->nack_seen[a7 / 8] & (1u << (a7 % 8)))) {
                char what[48];
                c->nack_seen[a7 / 8] |= 1u << (a7 % 8);
                snprintf(what, sizeof(what), "address 0x%02x: no device, NACK", a7);
                i2c_unimp(c, NULL, what);
            }
            c->isr |= I2C_ISR_NACKF;
            i2c_stop_cond(c);               /* a NACK makes the master send STOP */
            break;
        }
        c->cur->start(c->cur->opaque, c->read);
        if (c->read) {
            c->isr |= I2C_ISR_DIR;
        } else {
            c->isr &= ~I2C_ISR_DIR;
        }
        if (c->cr2 & I2C_CR2_STOP) {
            i2c_stop_cond(c);               /* STOP written during the address phase */
        } else if (!c->left) {
            i2c_end_of_count(c);            /* NBYTES = 0: an address probe */
        } else if (c->read) {
            i2c_schedule(c, I2C_PH_RX, 9);
        } else {
            c->phase = I2C_PH_TX;
            if (c->isr & I2C_ISR_TXE) {
                c->isr |= I2C_ISR_TXIS;
            } else {
                i2c_schedule(c, I2C_PH_TXBYTE, 9);  /* TXDR was preloaded */
            }
        }
        break;
    }
    case I2C_PH_TXBYTE: {
        bool ack = c->cur && c->cur->write(c->cur->opaque, c->txdr);
        c->bytes++;
        c->isr |= I2C_ISR_TXE;
        c->left--;
        if (!ack) {
            c->nacks++;
            c->isr |= I2C_ISR_NACKF;
            i2c_stop_cond(c);
        } else if (c->cr2 & I2C_CR2_STOP) {
            i2c_stop_cond(c);               /* STOP written while this byte was on the wire */
        } else if (c->left) {
            c->phase = I2C_PH_TX;
            c->isr |= I2C_ISR_TXIS;
        } else {
            i2c_end_of_count(c);
        }
        break;
    }
    case I2C_PH_RX:
        if (c->isr & I2C_ISR_RXNE) {
            c->phase = I2C_PH_RXWAIT;       /* RXDR still full: stretch, never overwrite it */
            break;
        }
        c->rxdr = c->cur ? c->cur->read(c->cur->opaque) : 0xFF;
        c->bytes++;
        c->isr |= I2C_ISR_RXNE;
        c->left--;
        if (c->cr2 & I2C_CR2_STOP) {
            i2c_stop_cond(c);               /* STOP written while this byte was on the wire */
        } else if (!c->left) {
            i2c_end_of_count(c);            /* the last byte is NACKed by the master */
        } else {
            c->phase = I2C_PH_RXWAIT;       /* stretched until RXDR is read */
        }
        break;
    case I2C_PH_STOP:
        c->cr2 &= ~I2C_CR2_STOP;
        c->isr |= I2C_ISR_STOPF;
        c->isr &= ~I2C_ISR_BUSY;
        if (c->cur && c->cur->stop) {
            c->cur->stop(c->cur->opaque);
        }
        c->cur = NULL;
        c->phase = I2C_PH_IDLE;
        if (c->cr2 & I2C_CR2_START) {
            i2c_start_cond(c);              /* START written during the STOP */
        }
        break;
    default:
        break;
    }
    i2c_update_irq(c);
}

/* the communication is released: slave told, timer off, START/STOP and the flags cleared */
static void i2c_release(I2cCtl *c)
{
    c->hooks.timer_del(c->hooks.opaque);
    if (c->cur && c->cur->stop) {
        c->cur->stop(c->cur->opaque);
    }
    c->cur = NULL;
    c->isr = I2C_ISR_TXE;
    c->cr2 &= ~(I2C_CR2_START | I2C_CR2_STOP);
    c->phase = I2C_PH_IDLE;
    c->left = 0;
}

/* system reset */
static void i2c_core_reset(I2cCtl *c)
{
    i2c_release(c);
    c->cr1 = c->cr2 = c->oar1 = c->oar2 = c->timingr = c->timeoutr = 0;
    c->rxdr = c->txdr = 0;
    i2c_update_irq(c);
}

static uint32_t i2c_core_read(I2cCtl *c, unsigned off, unsigned size)
{
    uint32_t v;

    switch (off & ~3u) {
    case I2C_CR1: v = c->cr1; break;
    case I2C_CR2: v = c->cr2; break;
    case I2C_OAR1: v = c->oar1; break;
    case I2C_OAR2: v = c->oar2; break;
    case I2C_TIMINGR: v = c->timingr; break;
    case I2C_TIMEOUTR: v = c->timeoutr; break;
    case I2C_ISR: v = c->isr; break;
    case I2C_RXDR:
        v = c->rxdr;
        if (c->isr & I2C_ISR_RXNE) {
            c->isr &= ~I2C_ISR_RXNE;
            if (c->phase == I2C_PH_RXWAIT) {
                i2c_schedule(c, I2C_PH_RX, 9);
            }
            i2c_update_irq(c);
        }
        break;
    case I2C_TXDR: v = c->txdr; break;
    default: v = 0;                 /* ICR is write-only, PECR: no PEC */
    }
    return i2c_lane_get(v, off, size);
}

static void i2c_cr2_write(I2cCtl *c, uint32_t v)
{
    uint32_t old = c->cr2;
    bool set_start = (v & I2C_CR2_START) && !(old & I2C_CR2_START);
    bool set_stop = (v & I2C_CR2_STOP) && !(old & I2C_CR2_STOP);

    /* START/STOP are set by software and cleared by hardware: a write of 0 keeps them */
    c->cr2 = (v & ~(I2C_CR2_START | I2C_CR2_STOP)) | (old & (I2C_CR2_START | I2C_CR2_STOP));
    if (!(c->cr1 & I2C_CR1_PE)) {
        return;                         /* START/STOP ignored while the peripheral is off */
    }
    if (set_start) {
        c->cr2 |= I2C_CR2_START;
        if (c->phase == I2C_PH_IDLE || c->phase == I2C_PH_WAIT) {
            i2c_start_cond(c);          /* START, or repeated START after TC (TCR) */
        }                               /* else latched: after the STOP / at the end of NBYTES */
    }
    if (set_stop) {
        switch (c->phase) {
        case I2C_PH_WAIT: case I2C_PH_TX: case I2C_PH_RXWAIT:
            i2c_stop_cond(c);           /* nothing on the wire: STOP now */
            break;
        case I2C_PH_ADDR: case I2C_PH_TXBYTE: case I2C_PH_RX:
            c->cr2 |= I2C_CR2_STOP;     /* after the current byte */
            break;
        default:
            break;                      /* idle, or a STOP already in progress: ignored */
        }
    }
    if (!set_start && !set_stop && c->phase == I2C_PH_WAIT && (c->isr & I2C_ISR_TCR) &&
        I2C_CR2_NBYTES(c->cr2)) {
        /* reload: the next NBYTES chunk of the same transfer */
        c->isr &= ~I2C_ISR_TCR;
        c->left = I2C_CR2_NBYTES(c->cr2);
        if (!c->read) {
            c->phase = I2C_PH_TX;
            c->isr |= I2C_ISR_TXIS;
        } else if (c->isr & I2C_ISR_RXNE) {
            c->phase = I2C_PH_RXWAIT;   /* the chunk's last byte not read yet: stretch */
        } else {
            i2c_schedule(c, I2C_PH_RX, 9);
        }
    }
}

static void i2c_core_write(I2cCtl *c, unsigned off, uint32_t val, unsigned size)
{
    uint32_t old, lane = val << ((off & 3) * 8);

    switch (off & ~3u) {
    case I2C_CR1:
        old = c->cr1;
        c->cr1 = i2c_lane_put(c->cr1, off, size, val);
        if ((old & I2C_CR1_PE) && !(c->cr1 & I2C_CR1_PE)) {
            i2c_release(c);             /* PE = 0: the peripheral's software reset */
        }
        if (c->cr1 & I2C_CR1_DMAEN) {
            i2c_unimp(c, &c->warned_dma, "DMA requests not modelled");
        }
        if (c->cr1 & I2C_CR1_ADDRIE) {
            i2c_unimp(c, &c->warned_slave, "slave mode not modelled");
        }
        break;
    case I2C_CR2:
        i2c_cr2_write(c, i2c_lane_put(c->cr2, off, size, val));
        break;
    case I2C_OAR1:
        c->oar1 = i2c_lane_put(c->oar1, off, size, val);
        break;
    case I2C_OAR2:
        c->oar2 = i2c_lane_put(c->oar2, off, size, val);
        break;
    case I2C_TIMINGR:
        c->timingr = i2c_lane_put(c->timingr, off, size, val);
        break;
    case I2C_TIMEOUTR:
        c->timeoutr = i2c_lane_put(c->timeoutr, off, size, val);
        if (c->timeoutr & ((1u << 15) | (1u << 31))) {
            i2c_unimp(c, &c->warned_smbus, "SMBus timeouts not modelled");
        }
        break;
    case I2C_ISR:
        /* TXE written 1: flush TXDR (TXIS is writable only with NOSTRETCH, not modelled) */
        if (lane & I2C_ISR_TXE) {
            c->isr |= I2C_ISR_TXE;
        }
        break;
    case I2C_ICR:
        /* ADDRCF NACKCF STOPCF BERRCF ARLOCF OVRCF PECCF TIMOUTCF ALERTCF */
        c->isr &= ~(lane & 0x3F38u);
        break;
    case I2C_TXDR:
        if (off & 3) {
            break;
        }
        c->txdr = val & 0xFF;
        c->isr &= ~I2C_ISR_TXE;
        if (c->phase == I2C_PH_TX && (c->isr & I2C_ISR_TXIS)) {
            c->isr &= ~I2C_ISR_TXIS;
            i2c_schedule(c, I2C_PH_TXBYTE, 9);
        }
        break;
    default:
        break;
    }
    i2c_update_irq(c);                  /* the HAL enables the EV/ER sources after a START */
}
