/* Host unit tests of the STM32F7 I2C controller model (hw/arm/stm32_i2c.c.inc) without QEMU:
 * a fake virtual clock drives the controller's one timer, test code plays the HAL through the
 * register interface (polled like HAL_I2C_Mem_Read/Mem_Write, and interrupt-driven like
 * HAL_I2C_Mem_Write_IT/Mem_Read_IT), slaves are a scripted memory device and the DPS310 model
 * from devices/. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../hw/arm/stm32_i2c.c.inc"
#include "fb/dps310.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- environment: fake clock, one timer, the IRQ lines, the log ------------------------- */
static int64_t now_ns, due_ns = -1;
static bool ev_line, er_line;
static int unimp_lines;
static void h_mod(void *o, int64_t d) { (void)o; due_ns = now_ns + d; }
static void h_del(void *o) { (void)o; due_ns = -1; }
static void h_irq(void *o, bool ev, bool er) { (void)o; ev_line = ev; er_line = er; }
static void h_log(void *o, const char *m) { (void)o; (void)m; unimp_lines++; }

static I2cCtl C;

/* advance to the pending timer and fire it; false = nothing pending */
static bool step(void)
{
    if (due_ns < 0) return false;
    now_ns = due_ns;
    due_ns = -1;
    i2c_core_timer(&C);
    return true;
}

static uint32_t rd(unsigned off) { return i2c_core_read(&C, off, 4); }
static void wr(unsigned off, uint32_t v) { i2c_core_write(&C, off, v, 4); }
static uint32_t isr(void) { return C.isr; }         /* a peek: no side effects */

/* poll ISR until any of mask is set, running the bus meanwhile (the HAL's wait loops) */
static bool wait_isr(uint32_t mask)
{
    for (int guard = 0; guard < 100000; guard++) {
        if (rd(I2C_ISR) & mask) return true;
        if (!step()) return (rd(I2C_ISR) & mask) != 0;
    }
    return false;
}

static void run_idle(void) { while (step()) {} }

/* ---- a scripted memory slave: 512 bytes, pointer byte, NACK after nack_after data bytes --- */
typedef struct Mem {
    uint8_t mem[512];
    unsigned ptr;
    bool ptr_next;
    int nack_after;         /* data bytes accepted before a NACK, -1 = never */
    int data_written;
    int starts, stops, reads;
} Mem;
static Mem M;
static void m_start(void *o, bool read) { Mem *m = o; m->starts++; if (!read) { m->ptr_next = true; m->data_written = 0; } }
static bool m_write(void *o, uint8_t b)
{
    Mem *m = o;
    if (m->ptr_next) { m->ptr_next = false; m->ptr = b; return true; }
    if (m->nack_after >= 0 && m->data_written >= m->nack_after) return false;
    m->mem[m->ptr++ % 512] = b;
    m->data_written++;
    return true;
}
static uint8_t m_read(void *o) { Mem *m = o; m->reads++; return m->mem[m->ptr++ % 512]; }
static void m_stop(void *o) { Mem *m = o; m->stops++; }

/* ---- DPS310 glue (as nexus_xr.c's) --------------------------------------------------------- */
static fb_dps310_t *baro;
static int baro_stops;
static void b_start(void *o, bool read) { (void)o; if (!read) fb_dps310_i2c_start(baro); }
static bool b_write(void *o, uint8_t b) { (void)o; return fb_dps310_i2c_write_byte(baro, b); }
static uint8_t b_read(void *o) { (void)o; return fb_dps310_i2c_read_byte(baro); }
static void b_stop(void *o) { (void)o; baro_stops++; }

#define MEM_ADDR  0x50
#define BARO_ADDR 0x76

static void setup(void)
{
    memset(&C, 0, sizeof(C));
    C.name = "test.i2c";
    C.pclk_hz = 54000000;
    C.hooks = (I2cHooks) { .timer_mod = h_mod, .timer_del = h_del, .irq = h_irq, .log_unimp = h_log };
    C.slaves[C.nslaves++] = (I2cSlave) { .addr = MEM_ADDR, .present = true, .start = m_start, .write = m_write,
                                         .read = m_read, .stop = m_stop, .opaque = &M };
    C.slaves[C.nslaves++] = (I2cSlave) { .addr = BARO_ADDR, .present = true, .start = b_start, .write = b_write,
                                         .read = b_read, .stop = b_stop, .opaque = NULL };
    now_ns = 0; due_ns = -1;
    i2c_core_reset(&C);
    memset(&M, 0, sizeof(M));
    M.nack_after = -1;
    for (int i = 0; i < 512; i++) M.mem[i] = (uint8_t)(i * 7 + 3);
    wr(I2C_TIMINGR, 0x00B03FDB & 0x0FFFFFFF);   /* some TIMINGR (PRESC 0) */
    wr(I2C_CR1, I2C_CR1_PE);
}

static uint32_t cr2(uint8_t a7, unsigned nbytes, uint32_t flags)
{
    return ((uint32_t)a7 << 1) | ((uint32_t)nbytes << 16) | flags;
}

static void clear_flags(void) { wr(I2C_ICR, 0x3F38u); }

/* HAL_I2C_Mem_Read, polled: address (1 byte, SOFTEND) -> TC -> repeated START read, RELOAD
 * chunks of 255 -> TCR ... last chunk AUTOEND -> STOPF. n bytes into buf; false on any error. */
static bool hal_mem_read(uint8_t a7, uint8_t reg, uint8_t *buf, unsigned n, bool nbytes_before_rxdr)
{
    wr(I2C_CR2, cr2(a7, 1, I2C_CR2_START));
    if (!wait_isr(I2C_ISR_TXIS | I2C_ISR_NACKF) || (isr() & I2C_ISR_NACKF)) return false;
    wr(I2C_TXDR, reg);
    if (!wait_isr(I2C_ISR_TC)) return false;
    unsigned chunk = n > 255 ? 255 : n;
    wr(I2C_CR2, cr2(a7, chunk, I2C_CR2_RD_WRN | I2C_CR2_START | (n > 255 ? I2C_CR2_RELOAD : I2C_CR2_AUTOEND)));
    unsigned got = 0, left = n - chunk;
    while (got < n) {
        if (!wait_isr(I2C_ISR_RXNE)) return false;
        if (nbytes_before_rxdr && (isr() & I2C_ISR_TCR) && left) {
            /* reprogram NBYTES before reading the chunk's last byte: the bus must stretch */
            unsigned c2 = left > 255 ? 255 : left;
            wr(I2C_CR2, cr2(a7, c2, I2C_CR2_RD_WRN | (left > 255 ? I2C_CR2_RELOAD : I2C_CR2_AUTOEND)));
            left -= c2;
            run_idle();
            CHECK(C.phase == I2C_PH_RXWAIT && (isr() & I2C_ISR_RXNE), "stretched across RELOAD, RXDR kept");
        }
        buf[got++] = (uint8_t)rd(I2C_RXDR);
        if (!nbytes_before_rxdr && got < n && (got % 255) == 0) {
            if (!wait_isr(I2C_ISR_TCR)) return false;
            unsigned c2 = left > 255 ? 255 : left;
            wr(I2C_CR2, cr2(a7, c2, I2C_CR2_RD_WRN | (left > 255 ? I2C_CR2_RELOAD : I2C_CR2_AUTOEND)));
            left -= c2;
        }
    }
    if (!wait_isr(I2C_ISR_STOPF)) return false;
    clear_flags();
    return true;
}

/* ---- tests -------------------------------------------------------------------------------- */

static void t_nbytes0_probe(void)
{
    setup();
    int64_t t0 = now_ns;
    wr(I2C_CR2, cr2(MEM_ADDR, 0, I2C_CR2_START | I2C_CR2_AUTOEND));    /* HAL_I2C_IsDeviceReady */
    CHECK(rd(I2C_ISR) & I2C_ISR_BUSY, "BUSY after START");
    CHECK(wait_isr(I2C_ISR_STOPF), "probe of a present address ends in STOPF");
    CHECK(!(isr() & I2C_ISR_NACKF) && !(isr() & I2C_ISR_BUSY), "ACK, bus free: isr=0x%x", isr());
    CHECK(M.starts == 1 && M.stops == 1 && M.reads == 0, "slave addressed once, no data");
    CHECK(now_ns - t0 == 11 * i2c_bit_ns(&C), "10 bit times address + 1 STOP: %lld ns", (long long)(now_ns - t0));
    CHECK(!(rd(I2C_CR2) & (I2C_CR2_START | I2C_CR2_STOP)), "START/STOP cleared");
    clear_flags();
    wr(I2C_CR2, cr2(0x33, 0, I2C_CR2_START | I2C_CR2_AUTOEND));
    CHECK(wait_isr(I2C_ISR_STOPF), "probe of an absent address ends in STOPF");
    CHECK((isr() & I2C_ISR_NACKF) && C.nacks == 1 && unimp_lines == 1, "NACKF, counted, logged once");
    clear_flags();
    wr(I2C_CR2, cr2(0x33, 0, I2C_CR2_START | I2C_CR2_AUTOEND));
    wait_isr(I2C_ISR_STOPF);
    CHECK(unimp_lines == 1, "the same absent address is logged only once");
    /* NBYTES = 0 without AUTOEND: TC, then software STOP */
    clear_flags();
    wr(I2C_CR2, cr2(MEM_ADDR, 0, I2C_CR2_START));
    CHECK(wait_isr(I2C_ISR_TC), "NBYTES 0 SOFTEND: TC");
    wr(I2C_CR2, rd(I2C_CR2) | I2C_CR2_STOP);
    CHECK(wait_isr(I2C_ISR_STOPF) && !(isr() & (I2C_ISR_TC | I2C_ISR_BUSY)), "software STOP after TC");
}

static void t_data_nack(void)
{
    setup();
    M.nack_after = 2;
    wr(I2C_CR2, cr2(MEM_ADDR, 5, I2C_CR2_START | I2C_CR2_AUTOEND));
    int sent = 0;
    while (wait_isr(I2C_ISR_TXIS | I2C_ISR_NACKF | I2C_ISR_STOPF)) {
        if (isr() & (I2C_ISR_NACKF | I2C_ISR_STOPF)) break;
        wr(I2C_TXDR, (uint32_t)(0x10 + sent));
        sent++;
    }
    CHECK(sent == 4, "pointer + 2 data ACKed, the third data byte written then NACKed: sent %d", sent);
    CHECK(isr() & I2C_ISR_NACKF, "NACKF on the data byte");
    CHECK(wait_isr(I2C_ISR_STOPF), "automatic STOP after the NACK");
    CHECK(!(isr() & (I2C_ISR_BUSY | I2C_ISR_TXIS | I2C_ISR_TC)), "bus free, no TXIS/TC: 0x%x", isr());
    CHECK(M.data_written == 2 && M.mem[0x10] == 0x11 && M.stops == 1, "slave got 2 data bytes at ptr 0x10");
    CHECK(C.left == 1, "one byte of NBYTES never sent: %u", C.left);
    clear_flags();
    /* the controller is usable again */
    M.nack_after = -1;
    uint8_t b[3];
    CHECK(hal_mem_read(MEM_ADDR, 0x10, b, 3, false) && b[0] == 0x11 && b[1] == 0x12, "next transfer fine");
}

static void t_rx_reload_chain(void)
{
    setup();
    uint8_t buf[300];
    CHECK(hal_mem_read(MEM_ADDR, 0x00, buf, 300, false), "300-byte read: 255 RELOAD + 45 AUTOEND");
    bool same = true;
    for (int i = 0; i < 300; i++) same &= buf[i] == (uint8_t)(i * 7 + 3);
    CHECK(same, "300 bytes in order");
    CHECK(M.starts == 2 && M.stops == 1 && M.reads == 300, "starts %d stops %d reads %d", M.starts, M.stops, M.reads);
    CHECK(!(rd(I2C_CR2) & (I2C_CR2_START | I2C_CR2_STOP)) && !(isr() & I2C_ISR_BUSY), "clean end");
    /* 600 bytes: two reloads, NBYTES reprogrammed before the chunk's last RXDR read (stretching) */
    setup();
    uint8_t big[600];
    CHECK(hal_mem_read(MEM_ADDR, 0x00, big, 600, true), "600-byte read with NBYTES before RXDR");
    same = true;
    for (int i = 0; i < 600; i++) same &= big[i] == M.mem[i % 512];
    CHECK(same, "600 bytes in order, none overwritten across the reloads");
    CHECK(M.reads == 600, "600 bytes clocked: %d", M.reads);
}

static void t_stop_mid_transfer(void)
{
    /* write: STOP while the 2nd data byte is on the wire -> STOP after it, no TC */
    setup();
    wr(I2C_CR2, cr2(MEM_ADDR, 5, I2C_CR2_START));
    wait_isr(I2C_ISR_TXIS); wr(I2C_TXDR, 0x20);         /* pointer */
    wait_isr(I2C_ISR_TXIS); wr(I2C_TXDR, 0xA1);
    wait_isr(I2C_ISR_TXIS); wr(I2C_TXDR, 0xA2);         /* on the wire now */
    CHECK(C.phase == I2C_PH_TXBYTE, "byte in flight");
    wr(I2C_CR2, rd(I2C_CR2) | I2C_CR2_STOP);
    CHECK(rd(I2C_CR2) & I2C_CR2_STOP, "STOP latched while the byte is on the wire");
    CHECK(wait_isr(I2C_ISR_STOPF), "STOPF");
    CHECK(M.data_written == 2 && M.mem[0x21] == 0xA2 && M.stops == 1, "the byte in flight completed, then STOP");
    CHECK(!(isr() & (I2C_ISR_TC | I2C_ISR_TXIS | I2C_ISR_BUSY)), "no TC/TXIS, bus free: 0x%x", isr());
    CHECK(!(rd(I2C_CR2) & I2C_CR2_STOP), "STOP cleared by hardware");
    /* write: STOP while waiting for TXDR (nothing on the wire) -> STOP now */
    setup();
    wr(I2C_CR2, cr2(MEM_ADDR, 5, I2C_CR2_START));
    wait_isr(I2C_ISR_TXIS); wr(I2C_TXDR, 0x30);
    wait_isr(I2C_ISR_TXIS);
    wr(I2C_CR2, rd(I2C_CR2) | I2C_CR2_STOP);
    CHECK(wait_isr(I2C_ISR_STOPF) && !(isr() & I2C_ISR_TXIS), "STOP at once while TXIS pending");
    CHECK(M.data_written == 0, "no data byte sent");
    /* read: STOP while a byte is being received -> that byte lands in RXDR, then STOP */
    setup();
    wr(I2C_CR2, cr2(MEM_ADDR, 1, I2C_CR2_START)); wait_isr(I2C_ISR_TXIS); wr(I2C_TXDR, 0x00); wait_isr(I2C_ISR_TC);
    wr(I2C_CR2, cr2(MEM_ADDR, 10, I2C_CR2_START | I2C_CR2_RD_WRN));
    wait_isr(I2C_ISR_RXNE); rd(I2C_RXDR);                /* byte 0; byte 1 now on the wire */
    CHECK(C.phase == I2C_PH_RX, "receiving");
    wr(I2C_CR2, rd(I2C_CR2) | I2C_CR2_STOP);
    CHECK(wait_isr(I2C_ISR_STOPF), "STOPF after the byte");
    CHECK((isr() & I2C_ISR_RXNE) && rd(I2C_RXDR) == M.mem[1] && M.reads == 2, "byte 1 received, nothing more");
    CHECK(!(rd(I2C_CR2) & (I2C_CR2_STOP | I2C_CR2_START)), "bits clear");
    /* STOP on an idle bus: ignored, does not stick */
    setup();
    wr(I2C_CR2, I2C_CR2_STOP);
    run_idle();
    CHECK(!(rd(I2C_CR2) & I2C_CR2_STOP) && !(isr() & I2C_ISR_STOPF), "STOP on an idle bus ignored");
}

static void t_start_while_busy(void)
{
    /* START written during a write transfer (SOFTEND): repeated START at the end of NBYTES, no TC */
    setup();
    wr(I2C_CR2, cr2(MEM_ADDR, 2, I2C_CR2_START));
    wait_isr(I2C_ISR_TXIS); wr(I2C_TXDR, 0x40);
    wait_isr(I2C_ISR_TXIS);
    wr(I2C_CR2, cr2(MEM_ADDR, 3, I2C_CR2_START | I2C_CR2_RD_WRN | I2C_CR2_AUTOEND));   /* next: read 3 */
    CHECK(rd(I2C_CR2) & I2C_CR2_START, "START latched while BUSY");
    CHECK(M.starts == 1, "no second START yet");
    wr(I2C_TXDR, 0x77);
    uint8_t b[3]; int n = 0;
    while (n < 3 && wait_isr(I2C_ISR_RXNE)) b[n++] = (uint8_t)rd(I2C_RXDR);
    CHECK(wait_isr(I2C_ISR_STOPF), "read ends in STOPF");
    CHECK(M.starts == 2 && M.stops == 1, "repeated START, one STOP: starts %d stops %d", M.starts, M.stops);
    CHECK(n == 3 && b[0] == M.mem[0x41] && b[2] == M.mem[0x43], "read after the latched START from ptr 0x41");
    CHECK(!(isr() & I2C_ISR_TC) && !(rd(I2C_CR2) & I2C_CR2_START), "no TC, START cleared");
    /* START written during a STOP (AUTOEND): generated after that STOP */
    setup();
    clear_flags();
    wr(I2C_CR2, cr2(MEM_ADDR, 0, I2C_CR2_START | I2C_CR2_AUTOEND));
    while (C.phase != I2C_PH_STOP && step()) {}
    CHECK(C.phase == I2C_PH_STOP, "in the STOP phase");
    wr(I2C_CR2, cr2(MEM_ADDR, 0, I2C_CR2_START | I2C_CR2_AUTOEND));
    CHECK(wait_isr(I2C_ISR_STOPF), "first STOP");
    CHECK(C.phase == I2C_PH_ADDR && (isr() & I2C_ISR_BUSY), "the pending START follows the STOP");
    clear_flags();
    CHECK(wait_isr(I2C_ISR_STOPF) && M.starts == 2 && M.stops == 2, "second probe done");
    CHECK(!(rd(I2C_CR2) & (I2C_CR2_START | I2C_CR2_STOP)), "no bits stick");
    /* START after a NACK's automatic STOP: nothing sticks from the NACKed transfer */
    setup();
    wr(I2C_CR2, cr2(0x33, 1, I2C_CR2_START));
    wait_isr(I2C_ISR_NACKF);
    wr(I2C_CR2, rd(I2C_CR2) | I2C_CR2_STOP);             /* the HAL's STOP after NACKF */
    CHECK(wait_isr(I2C_ISR_STOPF), "STOPF after NACK");
    run_idle();
    CHECK(!(rd(I2C_CR2) & (I2C_CR2_START | I2C_CR2_STOP)) && C.phase == I2C_PH_IDLE, "idle, no bits");
}

static void t_pe0_mid_transfer(void)
{
    setup();
    wr(I2C_CR2, cr2(MEM_ADDR, 1, I2C_CR2_START)); wait_isr(I2C_ISR_TXIS); wr(I2C_TXDR, 0x00); wait_isr(I2C_ISR_TC);
    wr(I2C_CR2, cr2(MEM_ADDR, 20, I2C_CR2_START | I2C_CR2_RD_WRN | I2C_CR2_AUTOEND));
    wait_isr(I2C_ISR_RXNE); rd(I2C_RXDR);
    wr(I2C_CR1, 0);                                      /* PE = 0 mid-transfer */
    CHECK(isr() == I2C_ISR_TXE && C.phase == I2C_PH_IDLE && due_ns < 0, "released: isr 0x%x", isr());
    CHECK(M.stops == 1, "slave told (stop callback)");
    wr(I2C_CR2, cr2(MEM_ADDR, 1, I2C_CR2_START | I2C_CR2_STOP));   /* while PE = 0: ignored */
    CHECK(!(rd(I2C_CR2) & (I2C_CR2_START | I2C_CR2_STOP)), "START/STOP not latched while PE = 0");
    run_idle();
    wr(I2C_CR1, I2C_CR1_PE);
    run_idle();
    CHECK(C.phase == I2C_PH_IDLE && M.starts == 2 && !(isr() & I2C_ISR_BUSY), "PE = 1 again: nothing starts by itself");
    uint8_t b[2];
    CHECK(hal_mem_read(MEM_ADDR, 0x05, b, 2, false) && b[0] == M.mem[5], "works after the reset");
}

/* HAL_I2C_Mem_Write_IT-style: interrupt-driven, the register address in a 1-byte RELOAD chunk,
 * TCR -> NBYTES = data length with AUTOEND, TXIS per byte, completion on STOPF. The ISR runs
 * whenever the EV line is high (the NVIC). Target: the DPS310 (PRS_CFG, TMP_CFG, MEAS_CFG). */
static void t_mem_write_it(void)
{
    setup();
    baro = fb_dps310_new();
    const uint8_t data[3] = { 0x54, 0xD4, 0x07 };
    int idx = -1, isr_calls = 0;
    bool done = false;
    wr(I2C_CR1, I2C_CR1_PE | I2C_CR1_TXIE | I2C_CR1_TCIE | I2C_CR1_STOPIE | I2C_CR1_NACKIE | I2C_CR1_ERRIE);
    wr(I2C_CR2, cr2(BARO_ADDR, 1, I2C_CR2_START | I2C_CR2_RELOAD));
    CHECK(!ev_line, "no event before the address phase");
    for (int guard = 0; guard < 1000 && !done; guard++) {
        while (ev_line && !done) {
            isr_calls++;
            uint32_t f = rd(I2C_ISR);
            if (f & I2C_ISR_NACKF) { done = true; break; }
            if (f & I2C_ISR_TXIS) {
                wr(I2C_TXDR, idx < 0 ? 0x06 : data[idx]);   /* memory address first */
                idx++;
            } else if (f & I2C_ISR_TCR) {
                wr(I2C_CR2, cr2(BARO_ADDR, 3, I2C_CR2_AUTOEND));    /* RELOAD off, NBYTES 3 */
            } else if (f & I2C_ISR_STOPF) {
                wr(I2C_ICR, I2C_ISR_STOPF);
                wr(I2C_CR1, I2C_CR1_PE);                    /* disable the sources */
                done = true;
            } else {
                break;
            }
        }
        if (!done && !step()) break;
    }
    CHECK(done && !(isr() & I2C_ISR_NACKF), "Mem_Write_IT completed on STOPF");
    CHECK(idx == 3 && isr_calls == 6, "4 TXIS + 1 TCR + 1 STOPF interrupts: idx %d calls %d", idx, isr_calls);
    CHECK(fb_dps310_peek(baro, 0x06) == 0x54 && fb_dps310_peek(baro, 0x07) == 0xD4 && (fb_dps310_peek(baro, 0x08) & 7) == 7,
          "DPS310 PRS_CFG/TMP_CFG/MEAS_CFG written (auto-increment)");
    CHECK(!ev_line && baro_stops == 1 && !(isr() & I2C_ISR_BUSY), "EV low, one STOP, bus free");
    /* and read back the ID + a measurement polled (Mem_Read), MEAS_CTRL written first */
    uint8_t w[2] = { 0x08, 0x07 };
    fb_dps310_i2c_write(baro, w, 2);
    fb_dps310_set_noise_scale(baro, 0);
    fb_dps310_set_environment(baro, 95000.f, 20.f);
    fb_dps310_sample(baro);
    uint8_t id, raw[6];
    CHECK(hal_mem_read(BARO_ADDR, 0x0D, &id, 1, false) && id == 0x10, "DPS310 ID over the controller: 0x%02x", id);
    CHECK(hal_mem_read(BARO_ADDR, 0x00, raw, 6, false), "6-byte result read");
    CHECK((fb_dps310_peek(baro, 0x08) & 0x30) == 0, "RDY cleared by the read through the controller");
    CHECK(raw[0] == fb_dps310_peek(baro, 0) && raw[5] == fb_dps310_peek(baro, 5), "bytes = registers");
    fb_dps310_free(baro);
    baro = NULL;
}

/* HAL_I2C_Mem_Read_IT as RotorFlight's baro read: the address polled (SOFTEND, TC), then read
 * with AUTOEND, RXIE: RXNE per byte, STOPF. */
static void t_mem_read_it(void)
{
    setup();
    wr(I2C_CR2, cr2(MEM_ADDR, 1, I2C_CR2_START)); wait_isr(I2C_ISR_TXIS); wr(I2C_TXDR, 0x80); wait_isr(I2C_ISR_TC);
    wr(I2C_CR2, cr2(MEM_ADDR, 6, I2C_CR2_START | I2C_CR2_RD_WRN | I2C_CR2_AUTOEND));
    wr(I2C_CR1, I2C_CR1_PE | I2C_CR1_RXIE | I2C_CR1_TCIE | I2C_CR1_STOPIE | I2C_CR1_NACKIE | I2C_CR1_ERRIE);
    uint8_t b[6]; int n = 0; bool done = false;
    for (int guard = 0; guard < 1000 && !done; guard++) {
        while (ev_line && !done) {
            uint32_t f = rd(I2C_ISR);
            if (f & I2C_ISR_RXNE) b[n++] = (uint8_t)rd(I2C_RXDR);
            else if (f & I2C_ISR_STOPF) { wr(I2C_ICR, I2C_ISR_STOPF); wr(I2C_CR1, I2C_CR1_PE); done = true; }
            else break;
        }
        if (!done && !step()) break;
    }
    CHECK(done && n == 6 && b[0] == M.mem[0x80] && b[5] == M.mem[0x85], "Mem_Read_IT: 6 bytes, STOPF");
}

int main(void)
{
    t_nbytes0_probe();
    t_data_nack();
    t_rx_reload_chain();
    t_stop_mid_transfer();
    t_start_while_busy();
    t_pe0_mid_transfer();
    t_mem_write_it();
    t_mem_read_it();
    printf("test_i2c: %s, %d checks, %d failure(s)\n", fails ? "FAILED" : "OK", checks, fails);
    return fails ? 1 : 0;
}
