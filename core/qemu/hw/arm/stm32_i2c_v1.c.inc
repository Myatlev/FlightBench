/*
 * STM32F4 I2C (v1 IP, RM0090 27) master: register interface and bus state machine,
 * emulator-independent.
 *
 * Included by nexus_xr.c after stm32_i2c.c.inc (it reuses its I2cSlave table, I2cHooks and
 * I2C_MAX_SLAVES; QEMU's MemoryRegion, QEMUTimer and qemu_irq live in the machine's wrapper) and
 * by core/qemu/tests/test_i2c_v1.c (host unit tests, a fake clock).
 *
 * iNav 9.1.0 drives the F4's I2C with drivers/bus_i2c_stm32f40x.c: a polled state machine stepped
 * in a busy loop (i2cWaitForCompletion -> i2cStateMachine(micros())), StdPeriph I2C_CheckEvent =
 * SR1 then SR2 read, (SR1 | SR2 << 16) & event == event; no interrupts, no DMA, 7-bit addresses,
 * master only. That driver is the spec here:
 *   write:  START; EV5 MASTER_MODE_SELECT (SB|MSL|BUSY); DR = addr|W; EV6 TRANSMITTER_MODE_SELECTED
 *           (ADDR|TXE|TRA|MSL|BUSY; the CheckEvent's SR1+SR2 read clears ADDR); DR = reg; EV8_2
 *           BYTE_TRANSMITTED (BTF|TXE|TRA|...); DR = data ... ; STOP; wait CR1.STOP == 0.
 *   read:   as write up to the register byte, then a repeated START, EV5, DR = addr|R, EV6
 *           RECEIVER_MODE_SELECTED (ADDR|MSL|BUSY; cleared by that read), then by length
 *           1 byte: ACK = 0, STOP, wait RXNE, read DR;
 *           2 bytes: POS = 1 (before the START), ACK = 0 right after ADDR, wait BTF (data 1 in DR,
 *                    data 2 in the shift register), STOP, read DR twice;
 *           N > 2:   wait BTF, read DR, ... ; with 3 left: ACK = 0, read DR (N-2), STOP, read DR
 *                    (N-1), wait RXNE, read DR (N).
 *   NACK:   SR1.AF after the address or a data byte: STOP, AF cleared by writing SR1 (rc_w0).
 *
 * Modelled: CR1 (PE, START, STOP, ACK, POS, SWRST), CR2 (FREQ, ITERREN/ITEVTEN/ITBUFEN), OAR1/2,
 * CCR, TRISE, FLTR storage; SR1 (SB, ADDR, BTF, RXNE, TXE, AF; the error bits rc_w0), SR2 (MSL,
 * BUSY, TRA); DR with a TX buffer and the RX shift register.
 * Events in virtual time from CCR: one SCL period = 2 CCR (standard), 3 CCR (fast, DUTY 0) or
 * 25 CCR (fast, DUTY 1) PCLK1 periods (CR2.FREQ MHz, else the machine's PCLK1; 400 kHz while CCR
 * is 0); START 1 period, the address byte 9 (8 + ACK), a data byte 9, STOP 1.
 * - START (CR1.START with PE): generated at once on an idle bus or while the bus waits for
 *   software (TXE/BTF after a byte, a received byte NACKed, after an address/data NACK): a
 *   repeated START; while a byte is on the wire, after it. When done: SB, MSL, BUSY; START
 *   cleared; BTF and TXE cleared.
 * - DR written while SB is set: the address byte (SB cleared; RM0090 wants an SR1 read before,
 *   which every driver does). At its end the slave with that 7-bit address (present, and its
 *   start_ack if any says yes) ACKs: ADDR, TRA = write, TXE with a write; none: AF (NACK), the
 *   master then waits for software (STOP or START). ADDR is cleared by an SR1 read followed by an
 *   SR2 read; only then does the data phase start (SCL is stretched while ADDR is set).
 * - Transmitter: DR written while nothing is being shifted (after ADDR is cleared) goes on the
 *   wire at once (TXE stays set, BTF cleared); written while a byte is on the wire, it waits in DR
 *   (TXE clear) and follows that byte. A byte done with DR empty: BTF (+ TXE); a NACKed data
 *   byte: AF, the transfer stops.
 * - Receiver: bytes are clocked in one after another once ADDR is cleared; a byte done goes to DR
 *   (RXNE) or, DR still full, waits in the shift register (BTF, SCL stretched until DR is read).
 *   Its ACK: CR1.ACK at the end of the byte (POS = 0), or at its start (POS = 1: ACK = 0 then
 *   NACKs the next byte, RM0090 27.6.8, the 2-byte procedure: the first byte after ADDR is ACKed
 *   then). A NACKed byte is the last one; a
 *   STOP written during a byte is sent after it.
 * - STOP (CR1.STOP): at once when nothing is on the wire, else after the current byte (also
 *   after the current START). Done: STOP cleared, MSL, BUSY, TRA, TXE, BTF cleared (RXNE and a
 *   byte still in the shift register stay for DR reads); a START written meanwhile follows.
 * - PE = 0 and CR1.SWRST: the communication is released and the flags cleared (SWRST: every
 *   register back to its reset value while it is set); START, STOP, ACK, POS cleared by PE = 0.
 *   i2c1v1_reset() is the system reset; RCC APB1RSTR.I2C1RST (I2C_DeInit) is not wired (the RCC
 *   is a stub): iNav's re-init clears PE first, which releases the bus the same way.
 * - EV line: ITEVTEN and (SB|ADDR|BTF|STOPF), or ITEVTEN + ITBUFEN and (TXE|RXNE); ER line:
 *   ITERREN and an error bit.
 * Not modelled (first use logged): DMA requests (CR2.DMAEN), slave mode (addressed by another
 * master; OAR1 is stored), SMBus, PEC, 10-bit addressing, NOSTRETCH, bus errors / arbitration
 * loss (no other master), OVR (the clock is always stretched), the analog/digital noise filters.
 */

enum { V1_CR1 = 0x00, V1_CR2 = 0x04, V1_OAR1 = 0x08, V1_OAR2 = 0x0C, V1_DR = 0x10, V1_SR1 = 0x14,
       V1_SR2 = 0x18, V1_CCR = 0x1C, V1_TRISE = 0x20, V1_FLTR = 0x24 };
#define V1_CR1_PE       (1u << 0)
#define V1_CR1_SMBUS    (1u << 1)
#define V1_CR1_ENPEC    (1u << 5)
#define V1_CR1_NOSTRETCH (1u << 7)
#define V1_CR1_START    (1u << 8)
#define V1_CR1_STOP     (1u << 9)
#define V1_CR1_ACK      (1u << 10)
#define V1_CR1_POS      (1u << 11)
#define V1_CR1_PEC      (1u << 12)
#define V1_CR1_SWRST    (1u << 15)
#define V1_CR2_FREQ(v)  ((v) & 0x3F)
#define V1_CR2_ITERREN  (1u << 8)
#define V1_CR2_ITEVTEN  (1u << 9)
#define V1_CR2_ITBUFEN  (1u << 10)
#define V1_CR2_DMAEN    (1u << 11)
#define V1_SR1_SB       (1u << 0)
#define V1_SR1_ADDR     (1u << 1)
#define V1_SR1_BTF      (1u << 2)
#define V1_SR1_ADD10    (1u << 3)
#define V1_SR1_STOPF    (1u << 4)
#define V1_SR1_RXNE     (1u << 6)
#define V1_SR1_TXE      (1u << 7)
#define V1_SR1_AF       (1u << 10)
#define V1_SR1_ERR      0xDF00u             /* BERR ARLO AF OVR PECERR TIMEOUT SMBALERT (rc_w0) */
#define V1_SR2_MSL      (1u << 0)
#define V1_SR2_BUSY     (1u << 1)
#define V1_SR2_TRA      (1u << 2)

/* what the bus is doing: V1_P_* with a timer are on the wire, the others wait for software */
enum {
    V1_P_IDLE,          /* no transfer */
    V1_P_START,         /* timer: the (repeated) START condition */
    V1_P_SB,            /* SB set, waiting for the address in DR */
    V1_P_ADDR,          /* timer: the address byte and its ACK */
    V1_P_ADDR_WAIT,     /* ADDR set: stretched until SR1 + SR2 are read */
    V1_P_TX,            /* timer: a data byte out */
    V1_P_TX_WAIT,       /* transmitter, nothing on the wire: TXE (+ BTF after a byte) */
    V1_P_RX,            /* timer: a data byte in */
    V1_P_RX_WAIT,       /* receiver, stretched (a byte in the shift register) or done (NACKed) */
    V1_P_NACK,          /* the slave NACKed (AF): waiting for STOP / START */
    V1_P_STOP,          /* timer: the STOP condition */
};

typedef struct I2cV1Ctl {
    const char *name;
    uint64_t pclk_hz;
    I2cHooks hooks;
    uint32_t cr1, cr2, oar1, oar2, ccr, trise, fltr;
    uint32_t sr1, sr2;
    uint8_t dr;                 /* RX: the byte for the CPU; TX: the byte waiting (dr_full) */
    uint8_t shift;              /* RX: a received byte waiting behind DR (shift_full) */
    bool dr_full, shift_full;
    int phase;                  /* V1_P_* */
    bool read;                  /* the transfer's direction (address R/W bit) */
    bool sr1_seen;              /* SR1 read while ADDR was set: the next SR2 read clears ADDR */
    bool start_pending, stop_pending;   /* written while a byte was on the wire */
    bool rx_ack;                /* the byte in the shift register / last received was ACKed */
    bool ack_at_start;          /* POS = 1: the ACK bit when the byte on the wire started */
    bool first_rx;              /* the first byte after ADDR: with POS = 1 it is ACKed (below) */
    I2cSlave *cur;              /* addressed slave, NULL = none */
    I2cSlave slaves[I2C_MAX_SLAVES];
    int nslaves;
    uint64_t xfers, nacks, bytes;
    uint8_t nack_seen[128 / 8];
    bool warned_dma, warned_smbus, warned_pec, warned_add10, warned_nostretch;
} I2cV1Ctl;

static void i2c1v1_unimp(I2cV1Ctl *c, bool *once, const char *what)
{
    char line[160];
    if (once) {
        if (*once) {
            return;
        }
        *once = true;
    }
    snprintf(line, sizeof(line), "%s: %s", c->name, what);
    if (c->hooks.log_unimp) {
        c->hooks.log_unimp(c->hooks.opaque, line);
    }
}

static void i2c1v1_irq(I2cV1Ctl *c)
{
    uint32_t sr1 = c->sr1, cr2 = c->cr2;
    bool ev = ((cr2 & V1_CR2_ITEVTEN) &&
               (sr1 & (V1_SR1_SB | V1_SR1_ADDR | V1_SR1_BTF | V1_SR1_ADD10 | V1_SR1_STOPF))) ||
              ((cr2 & V1_CR2_ITEVTEN) && (cr2 & V1_CR2_ITBUFEN) && (sr1 & (V1_SR1_TXE | V1_SR1_RXNE)));
    bool er = (cr2 & V1_CR2_ITERREN) && (sr1 & V1_SR1_ERR);
    c->hooks.irq(c->hooks.opaque, ev, er);
}

/* one SCL period in ns (CCR: F/S bit 15, DUTY bit 14, CCR 11:0; CR2.FREQ = PCLK1 in MHz) */
static int64_t i2c1v1_bit_ns(I2cV1Ctl *c)
{
    uint32_t ccr = c->ccr & 0xFFF;
    uint64_t hz = V1_CR2_FREQ(c->cr2) >= 2 ? V1_CR2_FREQ(c->cr2) * 1000000ull : c->pclk_hz;
    if (!ccr || !hz) {
        return 1000000000 / 400000;
    }
    unsigned k = !(c->ccr & (1u << 15)) ? 2 : (c->ccr & (1u << 14)) ? 25 : 3;
    int64_t ns = (int64_t)((uint64_t)k * ccr * 1000000000ull / hz);
    return ns < 100 ? 100 : ns;
}

static void i2c1v1_schedule(I2cV1Ctl *c, int phase, unsigned bits)
{
    c->phase = phase;
    c->hooks.timer_mod(c->hooks.opaque, (int64_t)bits * i2c1v1_bit_ns(c));
}

static I2cSlave *i2c1v1_find(I2cV1Ctl *c, uint8_t addr7)
{
    for (int i = 0; i < c->nslaves; i++) {
        if (c->slaves[i].addr == addr7 && c->slaves[i].present) {
            return &c->slaves[i];
        }
    }
    return NULL;
}

static void i2c1v1_stop_cond(I2cV1Ctl *c)
{
    c->stop_pending = false;
    i2c1v1_schedule(c, V1_P_STOP, 1);
}

static void i2c1v1_start_cond(I2cV1Ctl *c)
{
    c->start_pending = false;
    i2c1v1_schedule(c, V1_P_START, 1);
}

/* a byte starts on the wire towards the master */
static void i2c1v1_rx_begin(I2cV1Ctl *c)
{
    c->ack_at_start = c->cr1 & V1_CR1_ACK;
    i2c1v1_schedule(c, V1_P_RX, 9);
}

/* the transmitter's next byte from DR onto the wire */
static void i2c1v1_tx_begin(I2cV1Ctl *c)
{
    c->shift = c->dr;
    c->dr_full = false;
    c->sr1 |= V1_SR1_TXE;
    c->sr1 &= ~V1_SR1_BTF;
    i2c1v1_schedule(c, V1_P_TX, 9);
}

/* nothing on the wire any more: a pending STOP or START goes now */
static bool i2c1v1_pending(I2cV1Ctl *c)
{
    if (c->stop_pending) {
        i2c1v1_stop_cond(c);
        return true;
    }
    if (c->start_pending) {
        i2c1v1_start_cond(c);
        return true;
    }
    return false;
}

/* the timer: the START / address / byte / STOP on the wire is complete */
static void i2c1v1_timer(I2cV1Ctl *c)
{
    switch (c->phase) {
    case V1_P_START:
        c->cr1 &= ~V1_CR1_START;
        c->sr1 = (c->sr1 & ~(V1_SR1_BTF | V1_SR1_TXE)) | V1_SR1_SB;
        c->sr2 |= V1_SR2_MSL | V1_SR2_BUSY;
        c->phase = V1_P_SB;
        c->xfers++;
        if (c->stop_pending) {
            i2c1v1_stop_cond(c);            /* STOP written during the START */
        }
        break;
    case V1_P_ADDR: {
        uint8_t a7 = c->shift >> 1;
        I2cSlave *sl = i2c1v1_find(c, a7);
        bool ack = sl && (!sl->start_ack || sl->start_ack(sl->opaque, c->read));
        if (ack && !sl->start_ack) {
            sl->start(sl->opaque, c->read);
        }
        if (!ack) {
            c->nacks++;
            if (!sl && !(c->nack_seen[a7 / 8] & (1u << (a7 % 8)))) {
                char what[48];
                c->nack_seen[a7 / 8] |= 1u << (a7 % 8);
                snprintf(what, sizeof(what), "address 0x%02x: no device, NACK", a7);
                i2c1v1_unimp(c, NULL, what);
            }
            c->cur = NULL;
            c->sr1 |= V1_SR1_AF;
            c->phase = V1_P_NACK;
            i2c1v1_pending(c);
            break;
        }
        c->cur = sl;
        c->sr1 |= V1_SR1_ADDR;
        c->sr1_seen = false;
        if (c->read) {
            c->sr2 &= ~V1_SR2_TRA;
        } else {
            c->sr2 |= V1_SR2_TRA;
            if (!c->dr_full) {
                c->sr1 |= V1_SR1_TXE;
            }
        }
        c->phase = V1_P_ADDR_WAIT;
        if (c->stop_pending) {
            i2c1v1_stop_cond(c);            /* STOP written during the address byte */
        }
        break;
    }
    case V1_P_TX: {
        bool ack = c->cur && c->cur->write(c->cur->opaque, c->shift);
        c->bytes++;
        if (!ack) {
            c->nacks++;
            c->sr1 |= V1_SR1_AF;
            c->phase = V1_P_NACK;
            i2c1v1_pending(c);
        } else if (i2c1v1_pending(c)) {
            /* STOP / START after this byte */
        } else if (c->dr_full) {
            i2c1v1_tx_begin(c);
        } else {
            c->sr1 |= V1_SR1_BTF | V1_SR1_TXE;
            c->phase = V1_P_TX_WAIT;
        }
        break;
    }
    case V1_P_RX: {
        uint8_t b = c->cur ? c->cur->read(c->cur->opaque) : 0xFF;
        c->bytes++;
        /* POS = 1: ACK decides the byte after the one on the wire, so the first byte after ADDR
         * is ACKed even when ACK was cleared before ADDR (RM0090's 2-byte procedure) */
        c->rx_ack = (c->cr1 & V1_CR1_POS) ? (c->first_rx || c->ack_at_start)
                                          : (c->cr1 & V1_CR1_ACK) != 0;
        c->first_rx = false;
        if (c->sr1 & V1_SR1_RXNE) {
            c->shift = b;                   /* DR still full: stretched, BTF */
            c->shift_full = true;
            c->sr1 |= V1_SR1_BTF;
        } else {
            c->dr = b;
            c->sr1 |= V1_SR1_RXNE;
        }
        if (i2c1v1_pending(c)) {
            /* STOP / START after this byte */
        } else if (c->rx_ack && !c->shift_full) {
            i2c1v1_rx_begin(c);             /* the slave sends the next byte */
        } else {
            c->phase = V1_P_RX_WAIT;        /* stretched, or NACKed: the master waits */
        }
        break;
    }
    case V1_P_STOP:
        c->cr1 &= ~V1_CR1_STOP;
        c->sr2 &= ~(V1_SR2_MSL | V1_SR2_BUSY | V1_SR2_TRA);
        c->sr1 &= ~(V1_SR1_TXE | V1_SR1_BTF | V1_SR1_SB | V1_SR1_ADDR);
        c->dr_full = false;
        if (c->cur && c->cur->stop) {
            c->cur->stop(c->cur->opaque);
        }
        c->cur = NULL;
        c->phase = V1_P_IDLE;
        if (c->start_pending || (c->cr1 & V1_CR1_START)) {
            i2c1v1_start_cond(c);           /* START written during the STOP */
        }
        break;
    default:
        break;
    }
    i2c1v1_irq(c);
}

/* the communication is released: slave told, timer off, the flags cleared */
static void i2c1v1_release(I2cV1Ctl *c)
{
    c->hooks.timer_del(c->hooks.opaque);
    if (c->cur && c->cur->stop) {
        c->cur->stop(c->cur->opaque);
    }
    c->cur = NULL;
    c->sr1 = c->sr2 = 0;                /* the error flags too: cleared by hardware when PE = 0 */
    c->dr_full = c->shift_full = false;
    c->start_pending = c->stop_pending = false;
    c->sr1_seen = false;
    c->first_rx = false;
    c->phase = V1_P_IDLE;
}

/* system reset, SWRST (not RCC APB1RSTR.I2CxRST: the RCC is a stub) */
static void i2c1v1_reset(I2cV1Ctl *c)
{
    i2c1v1_release(c);
    c->cr1 = c->cr2 = c->oar1 = c->oar2 = c->ccr = c->fltr = 0;
    c->sr1 = c->sr2 = 0;
    c->trise = 2;
    c->dr = c->shift = 0;
    i2c1v1_irq(c);
}

static uint32_t i2c1v1_read(I2cV1Ctl *c, unsigned off, unsigned size)
{
    uint32_t v;

    switch (off & ~3u) {
    case V1_CR1: v = c->cr1; break;
    case V1_CR2: v = c->cr2; break;
    case V1_OAR1: v = c->oar1; break;
    case V1_OAR2: v = c->oar2; break;
    case V1_DR:
        v = c->dr;
        if (!c->read || !(c->sr1 & V1_SR1_RXNE)) {
            break;
        }
        c->sr1 &= ~V1_SR1_RXNE;
        if (c->shift_full) {
            /* the byte behind moves up, SCL is released: the next byte if this one was ACKed */
            c->dr = c->shift;
            c->shift_full = false;
            c->sr1 = (c->sr1 & ~V1_SR1_BTF) | V1_SR1_RXNE;
            if (c->phase == V1_P_RX_WAIT && c->rx_ack && !i2c1v1_pending(c)) {
                i2c1v1_rx_begin(c);
            }
        }
        i2c1v1_irq(c);
        break;
    case V1_SR1:
        v = c->sr1;
        if (c->sr1 & V1_SR1_ADDR) {
            c->sr1_seen = true;
        }
        break;
    case V1_SR2:
        v = c->sr2;
        if ((c->sr1 & V1_SR1_ADDR) && c->sr1_seen) {
            /* ADDR cleared by SR1 then SR2: the data phase starts */
            c->sr1 &= ~V1_SR1_ADDR;
            c->sr1_seen = false;
            if (c->phase == V1_P_ADDR_WAIT) {
                if (c->read) {
                    c->first_rx = true;
                    i2c1v1_rx_begin(c);
                } else if (c->dr_full) {
                    i2c1v1_tx_begin(c);
                } else {
                    c->phase = V1_P_TX_WAIT;
                }
            }
            i2c1v1_irq(c);
        }
        break;
    case V1_CCR: v = c->ccr; break;
    case V1_TRISE: v = c->trise; break;
    case V1_FLTR: v = c->fltr; break;
    default: v = 0;
    }
    return i2c_lane_get(v, off, size);
}

static void i2c1v1_cr1_write(I2cV1Ctl *c, uint32_t v)
{
    uint32_t old = c->cr1;

    if (v & V1_CR1_SWRST) {
        i2c1v1_reset(c);                /* held in reset while SWRST is set */
        c->cr1 = V1_CR1_SWRST;
        return;
    }
    /* START/STOP are set by software and cleared by hardware: a write of 0 keeps them */
    c->cr1 = v | (old & (V1_CR1_START | V1_CR1_STOP));
    if (!(v & V1_CR1_PE)) {
        if (old & V1_CR1_PE) {
            i2c1v1_release(c);
        }
        c->cr1 &= ~(V1_CR1_START | V1_CR1_STOP | V1_CR1_ACK | V1_CR1_POS);
        return;
    }
    if (v & (V1_CR1_SMBUS)) {
        i2c1v1_unimp(c, &c->warned_smbus, "SMBus mode not modelled");
    }
    if (v & (V1_CR1_ENPEC | V1_CR1_PEC)) {
        i2c1v1_unimp(c, &c->warned_pec, "PEC not modelled");
    }
    if (v & V1_CR1_NOSTRETCH) {
        i2c1v1_unimp(c, &c->warned_nostretch, "NOSTRETCH not modelled");
    }
    bool set_start = (v & V1_CR1_START) && !(old & V1_CR1_START);
    bool set_stop = (v & V1_CR1_STOP) && !(old & V1_CR1_STOP);
    if (set_stop) {
        switch (c->phase) {
        case V1_P_SB: case V1_P_ADDR_WAIT: case V1_P_TX_WAIT: case V1_P_RX_WAIT: case V1_P_NACK:
            i2c1v1_stop_cond(c);        /* nothing on the wire: STOP now */
            break;
        case V1_P_START: case V1_P_ADDR: case V1_P_TX: case V1_P_RX:
            c->stop_pending = true;     /* after the current START / byte */
            break;
        case V1_P_IDLE:
            c->cr1 &= ~V1_CR1_STOP;     /* no transfer: nothing to stop */
            break;
        default:
            break;                      /* a STOP in progress */
        }
    }
    if (set_start) {
        switch (c->phase) {
        case V1_P_IDLE: case V1_P_TX_WAIT: case V1_P_RX_WAIT: case V1_P_NACK: case V1_P_ADDR_WAIT:
            if (!c->stop_pending && c->phase != V1_P_STOP) {
                i2c1v1_start_cond(c);   /* START, or a repeated START */
            }
            break;
        case V1_P_ADDR: case V1_P_TX: case V1_P_RX:
            c->start_pending = true;    /* after the current byte */
            break;
        default:
            break;                      /* START / STOP in progress: the STOP's end takes it */
        }
    }
}

static void i2c1v1_write(I2cV1Ctl *c, unsigned off, uint32_t val, unsigned size)
{
    switch (off & ~3u) {
    case V1_CR1:
        i2c1v1_cr1_write(c, i2c_lane_put(c->cr1, off, size, val));
        break;
    case V1_CR2:
        c->cr2 = i2c_lane_put(c->cr2, off, size, val);
        if (c->cr2 & V1_CR2_DMAEN) {
            i2c1v1_unimp(c, &c->warned_dma, "DMA requests not modelled");
        }
        break;
    case V1_OAR1:
        c->oar1 = i2c_lane_put(c->oar1, off, size, val);
        break;
    case V1_OAR2:
        c->oar2 = i2c_lane_put(c->oar2, off, size, val);
        break;
    case V1_DR:
        if (off & 3) {
            break;
        }
        if (!(c->cr1 & V1_CR1_PE)) {
            break;
        }
        if (c->sr1 & V1_SR1_SB) {
            /* the address byte */
            c->sr1 &= ~V1_SR1_SB;
            c->shift = val & 0xFF;
            c->read = val & 1;
            if ((val & 0xF9) == 0xF0) {
                i2c1v1_unimp(c, &c->warned_add10, "10-bit addressing not modelled");
            }
            i2c1v1_schedule(c, V1_P_ADDR, 9);
            break;
        }
        c->sr1 &= ~V1_SR1_BTF;
        if (c->read) {
            break;                      /* a receiver's DR is read-only */
        }
        c->dr = val & 0xFF;
        c->dr_full = true;
        c->sr1 &= ~V1_SR1_TXE;
        if (c->phase == V1_P_TX_WAIT) {
            i2c1v1_tx_begin(c);         /* the shift register is free: on the wire at once */
        }
        break;
    case V1_SR1:
        /* the error flags are rc_w0; the event flags are read-only */
        c->sr1 &= i2c_lane_put(0xFFFFFFFFu, off, size, val) | ~V1_SR1_ERR;
        break;
    case V1_CCR:
        c->ccr = i2c_lane_put(c->ccr, off, size, val);
        break;
    case V1_TRISE:
        c->trise = i2c_lane_put(c->trise, off, size, val);
        break;
    case V1_FLTR:
        c->fltr = i2c_lane_put(c->fltr, off, size, val);
        break;
    default:
        break;                          /* SR2 is read-only */
    }
    i2c1v1_irq(c);
}
