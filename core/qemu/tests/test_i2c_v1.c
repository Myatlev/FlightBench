/* Host unit tests of the STM32F4 I2C (v1) controller model (hw/arm/stm32_i2c_v1.c.inc) without
 * QEMU. The master is iNav 9.1.0's drivers/bus_i2c_stm32f40x.c state machine, ported line by line
 * with the StdPeriph calls it makes (I2C_CheckEvent = SR1 then SR2, I2C_GetFlagStatus = SR1,
 * I2C_ClearFlag = SR1 rc_w0, I2C_GenerateSTART/STOP, I2C_AcknowledgeConfig,
 * I2C_NACKPositionConfig, I2C_Send7bitAddress, I2C_SendData/ReceiveData) on the register
 * interface, its busy loop stepped in 0.25 us of virtual time per pass; the slaves are a scripted
 * memory device and the SPL06 / QMC5883L models from devices/. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* the v2 controller is included for its slave table; its functions are not used here */
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../hw/arm/stm32_i2c.c.inc"
#include "../hw/arm/stm32_i2c_v1.c.inc"
#include "fb/qmc5883l.h"
#include "fb/spl06.h"

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

static I2cV1Ctl C;

/* the CPU's time passes: fire the controller's timer when it is due */
static void cpu_ns(int64_t ns)
{
    int64_t end = now_ns + ns;
    while (due_ns >= 0 && due_ns <= end) {
        now_ns = due_ns;
        due_ns = -1;
        i2c1v1_timer(&C);
    }
    now_ns = end;
}

static uint32_t rd(unsigned off) { return i2c1v1_read(&C, off, 4); }
static void wr(unsigned off, uint32_t v) { i2c1v1_write(&C, off, v, 4); }

/* ---- StdPeriph (stm32f4xx_i2c.c) on the model ------------------------------------------- */
#define I2C_EVENT_MASTER_MODE_SELECT                ((uint32_t)0x00030001)
#define I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED  ((uint32_t)0x00070082)
#define I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED     ((uint32_t)0x00030002)
#define I2C_EVENT_MASTER_BYTE_TRANSMITTED           ((uint32_t)0x00070084)
#define I2C_FLAG_AF     ((uint32_t)0x10000400)
#define I2C_FLAG_BTF    ((uint32_t)0x10000004)
#define I2C_FLAG_RXNE   ((uint32_t)0x10000040)
enum { ERROR = 0, SUCCESS = 1 };
enum { RESET = 0, SET = 1 };
enum { DISABLE = 0, ENABLE = 1 };
enum { I2C_NACKPosition_Current, I2C_NACKPosition_Next };
enum { I2C_Direction_Transmitter, I2C_Direction_Receiver };

static int I2C_CheckEvent(uint32_t event)
{
    uint32_t f1 = rd(V1_SR1), f2 = rd(V1_SR2);
    uint32_t last = (f1 | f2 << 16) & 0x00FFFFFF;
    return (last & event) == event ? SUCCESS : ERROR;
}
static int I2C_GetFlagStatus(uint32_t flag) { return (rd(V1_SR1) & (flag & 0x00FFFFFF)) ? SET : RESET; }
static void I2C_ClearFlag(uint32_t flag) { wr(V1_SR1, (uint16_t)~(flag & 0x00FFFFFF)); }
static void I2C_GenerateSTART(int en) { wr(V1_CR1, en ? rd(V1_CR1) | V1_CR1_START : rd(V1_CR1) & ~V1_CR1_START); }
static void I2C_GenerateSTOP(int en) { wr(V1_CR1, en ? rd(V1_CR1) | V1_CR1_STOP : rd(V1_CR1) & ~V1_CR1_STOP); }
static void I2C_AcknowledgeConfig(int en) { wr(V1_CR1, en ? rd(V1_CR1) | V1_CR1_ACK : rd(V1_CR1) & ~V1_CR1_ACK); }
static void I2C_NACKPositionConfig(int pos)
{
    wr(V1_CR1, pos == I2C_NACKPosition_Next ? rd(V1_CR1) | V1_CR1_POS : rd(V1_CR1) & ~V1_CR1_POS);
}
static void I2C_Send7bitAddress(uint8_t addr, int dir) { wr(V1_DR, dir ? addr | 1 : addr & ~1); }
static void I2C_SendData(uint8_t d) { wr(V1_DR, d); }
static uint8_t I2C_ReceiveData(void) { return (uint8_t)rd(V1_DR); }

/* I2C_DeInit (RCC reset) + I2C_Init(400 kHz, DutyCycle_2, ack, 7-bit) + StretchClock + Cmd */
static void i2c_init(void)
{
    i2c1v1_reset(&C);
    uint32_t cr2 = rd(V1_CR2) & ~0x3Fu;
    wr(V1_CR2, cr2 | 42);                               /* PCLK1 42 MHz */
    wr(V1_CR1, rd(V1_CR1) & ~V1_CR1_PE);
    uint32_t r = 42000000 / (400000 * 3);               /* 35 */
    wr(V1_TRISE, 42 * 300 / 1000 + 1);
    wr(V1_CCR, r | (1u << 15));
    wr(V1_CR1, rd(V1_CR1) | V1_CR1_PE);
    wr(V1_CR1, (rd(V1_CR1) & 0xFBF5) | V1_CR1_ACK);
    wr(V1_OAR1, 0x4000);
    wr(V1_CR1, rd(V1_CR1) & ~V1_CR1_NOSTRETCH);
    wr(V1_CR1, rd(V1_CR1) | V1_CR1_PE);
}

/* ---- iNav's drivers/bus_i2c_stm32f40x.c state machine, verbatim in structure ------------- */
#define I2C_TIMEOUT 10000                               /* us (drivers/bus_i2c.h) */
typedef enum {
    I2C_STATE_STOPPED = 0, I2C_STATE_STOPPING, I2C_STATE_STARTING, I2C_STATE_STARTING_WAIT,
    I2C_STATE_R_ADDR, I2C_STATE_R_ADDR_WAIT, I2C_STATE_R_REGISTER, I2C_STATE_R_REGISTER_WAIT,
    I2C_STATE_R_RESTARTING, I2C_STATE_R_RESTARTING_WAIT, I2C_STATE_R_RESTART_ADDR,
    I2C_STATE_R_RESTART_ADDR_WAIT, I2C_STATE_R_TRANSFER_EQ1, I2C_STATE_R_TRANSFER_EQ2,
    I2C_STATE_R_TRANSFER_GE2, I2C_STATE_W_ADDR, I2C_STATE_W_ADDR_WAIT, I2C_STATE_W_REGISTER,
    I2C_STATE_W_TRANSFER_WAIT, I2C_STATE_W_TRANSFER, I2C_STATE_NACK, I2C_STATE_BUS_ERROR,
} i2cState_t;
typedef enum { I2C_TXN_READ, I2C_TXN_WRITE } i2cTransferDirection_t;
static struct {
    i2cState_t state;
    uint32_t timeout;
    bool allowRawAccess;
    uint8_t addr;
    i2cTransferDirection_t rw;
    uint8_t reg;
    uint32_t len;
    uint8_t *buf;
    bool txnOk;
} B;
static int bus_errors, loops;

static uint32_t micros(void) { return (uint32_t)(now_ns / 1000); }

static void i2cStateMachine(uint32_t currentTicksUs)
{
    switch (B.state) {
    case I2C_STATE_BUS_ERROR:
        bus_errors++;
        i2c_init();                                     /* i2cResetInterface: unstick + init */
        B.state = I2C_STATE_STOPPED;
        break;
    case I2C_STATE_STOPPING:
        if ((rd(V1_CR1) & V1_CR1_STOP) == 0) B.state = I2C_STATE_STOPPED;
        else if ((currentTicksUs - B.timeout) >= I2C_TIMEOUT) B.state = I2C_STATE_BUS_ERROR;
        break;
    case I2C_STATE_STOPPED:
        break;
    case I2C_STATE_STARTING:
        I2C_NACKPositionConfig(I2C_NACKPosition_Current);
        I2C_AcknowledgeConfig(ENABLE);
        I2C_GenerateSTART(ENABLE);
        B.state = I2C_STATE_STARTING_WAIT;
        B.timeout = currentTicksUs;
        /* fall through */
    case I2C_STATE_STARTING_WAIT:
        if (I2C_CheckEvent(I2C_EVENT_MASTER_MODE_SELECT) != ERROR) {
            if (B.rw == I2C_TXN_READ) {
                B.state = (B.reg == 0xFF && B.allowRawAccess) ? I2C_STATE_R_RESTART_ADDR : I2C_STATE_R_ADDR;
            } else {
                B.state = I2C_STATE_W_ADDR;
            }
        } else if ((currentTicksUs - B.timeout) >= I2C_TIMEOUT) B.state = I2C_STATE_BUS_ERROR;
        break;
    case I2C_STATE_R_ADDR:
        I2C_Send7bitAddress(B.addr, I2C_Direction_Transmitter);
        B.state = I2C_STATE_R_ADDR_WAIT;
        B.timeout = currentTicksUs;
        /* fall through */
    case I2C_STATE_R_ADDR_WAIT:
        if (I2C_CheckEvent(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED) != ERROR) B.state = I2C_STATE_R_REGISTER;
        else if (I2C_GetFlagStatus(I2C_FLAG_AF) != RESET) B.state = I2C_STATE_NACK;
        else if ((currentTicksUs - B.timeout) >= I2C_TIMEOUT) B.state = I2C_STATE_BUS_ERROR;
        break;
    case I2C_STATE_R_REGISTER:
        I2C_SendData(B.reg);
        B.state = I2C_STATE_R_REGISTER_WAIT;
        B.timeout = currentTicksUs;
        /* fall through */
    case I2C_STATE_R_REGISTER_WAIT:
        if (I2C_CheckEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED) != ERROR) {
            if (B.len == 0) {
                I2C_GenerateSTOP(ENABLE);
                B.timeout = currentTicksUs;
                B.state = I2C_STATE_STOPPING;
            } else {
                B.state = I2C_STATE_R_RESTARTING;
            }
        } else if (I2C_GetFlagStatus(I2C_FLAG_AF) != RESET) B.state = I2C_STATE_NACK;
        else if ((currentTicksUs - B.timeout) >= I2C_TIMEOUT) B.state = I2C_STATE_BUS_ERROR;
        break;
    case I2C_STATE_R_RESTARTING:
        I2C_GenerateSTART(ENABLE);
        B.state = I2C_STATE_R_RESTARTING_WAIT;
        B.timeout = currentTicksUs;
        /* fall through */
    case I2C_STATE_R_RESTARTING_WAIT:
        if (I2C_CheckEvent(I2C_EVENT_MASTER_MODE_SELECT) != ERROR) B.state = I2C_STATE_R_RESTART_ADDR;
        else if ((currentTicksUs - B.timeout) >= I2C_TIMEOUT) B.state = I2C_STATE_BUS_ERROR;
        break;
    case I2C_STATE_R_RESTART_ADDR:
        I2C_Send7bitAddress(B.addr, I2C_Direction_Receiver);
        B.state = I2C_STATE_R_RESTART_ADDR_WAIT;
        B.timeout = currentTicksUs;
        /* fall through */
    case I2C_STATE_R_RESTART_ADDR_WAIT:
        if (I2C_CheckEvent(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED) != ERROR) {
            if (B.len == 1) {
                I2C_AcknowledgeConfig(DISABLE);
                (void)rd(V1_SR2);
                I2C_GenerateSTOP(ENABLE);
                B.state = I2C_STATE_R_TRANSFER_EQ1;
            } else if (B.len == 2) {
                I2C_NACKPositionConfig(I2C_NACKPosition_Next);
                (void)rd(V1_SR2);
                I2C_AcknowledgeConfig(DISABLE);
                B.state = I2C_STATE_R_TRANSFER_EQ2;
            } else {
                (void)rd(V1_SR2);
                B.state = I2C_STATE_R_TRANSFER_GE2;
            }
            B.timeout = currentTicksUs;
        } else if (I2C_GetFlagStatus(I2C_FLAG_AF) != RESET) B.state = I2C_STATE_NACK;
        else if ((currentTicksUs - B.timeout) >= I2C_TIMEOUT) B.state = I2C_STATE_BUS_ERROR;
        break;
    case I2C_STATE_R_TRANSFER_EQ1:
        if (I2C_GetFlagStatus(I2C_FLAG_RXNE) != RESET) {
            *B.buf++ = I2C_ReceiveData();
            B.len--;
            B.txnOk = true;
            B.timeout = currentTicksUs;
            B.state = I2C_STATE_STOPPING;
        } else if ((currentTicksUs - B.timeout) >= I2C_TIMEOUT) B.state = I2C_STATE_BUS_ERROR;
        break;
    case I2C_STATE_R_TRANSFER_EQ2:
        if (I2C_GetFlagStatus(I2C_FLAG_BTF) != RESET) {
            I2C_GenerateSTOP(ENABLE);
            *B.buf++ = I2C_ReceiveData();
            *B.buf++ = I2C_ReceiveData();
            B.len = -2;                                  /* iNav's `len =- 2` */
            B.txnOk = true;
            B.timeout = currentTicksUs;
            B.state = I2C_STATE_STOPPING;
        } else if ((currentTicksUs - B.timeout) >= I2C_TIMEOUT) B.state = I2C_STATE_BUS_ERROR;
        break;
    case I2C_STATE_R_TRANSFER_GE2:
        if (I2C_GetFlagStatus(I2C_FLAG_BTF) != RESET) {
            if (B.len == 3) {
                I2C_AcknowledgeConfig(DISABLE);
                *B.buf++ = I2C_ReceiveData();
                I2C_GenerateSTOP(ENABLE);
                *B.buf++ = I2C_ReceiveData();
                B.len -= 2;
                B.state = I2C_STATE_R_TRANSFER_EQ1;
                B.timeout = currentTicksUs;
            } else if (B.len < 3) {
                I2C_AcknowledgeConfig(DISABLE);
                I2C_GenerateSTOP(ENABLE);
                I2C_ReceiveData();
                B.txnOk = false;
                B.timeout = currentTicksUs;
                B.state = I2C_STATE_STOPPING;
            } else {
                *B.buf++ = I2C_ReceiveData();
                B.len--;
                B.timeout = currentTicksUs;
            }
        } else if ((currentTicksUs - B.timeout) >= I2C_TIMEOUT) B.state = I2C_STATE_BUS_ERROR;
        break;
    case I2C_STATE_W_ADDR:
        I2C_AcknowledgeConfig(DISABLE);
        I2C_Send7bitAddress(B.addr, I2C_Direction_Transmitter);
        B.state = I2C_STATE_W_ADDR_WAIT;
        B.timeout = currentTicksUs;
        /* fall through */
    case I2C_STATE_W_ADDR_WAIT:
        if (I2C_CheckEvent(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED) != ERROR) {
            B.state = (B.reg == 0xFF && B.allowRawAccess) ? I2C_STATE_W_TRANSFER : I2C_STATE_W_REGISTER;
        } else if (I2C_GetFlagStatus(I2C_FLAG_AF) != RESET) B.state = I2C_STATE_NACK;
        else if ((currentTicksUs - B.timeout) >= I2C_TIMEOUT) B.state = I2C_STATE_BUS_ERROR;
        break;
    case I2C_STATE_W_REGISTER:
        I2C_SendData(B.reg);
        B.state = I2C_STATE_W_TRANSFER_WAIT;
        B.timeout = currentTicksUs;
        /* fall through */
    case I2C_STATE_W_TRANSFER_WAIT:
        if (I2C_CheckEvent(I2C_EVENT_MASTER_BYTE_TRANSMITTED) != ERROR) B.state = I2C_STATE_W_TRANSFER;
        else if (I2C_GetFlagStatus(I2C_FLAG_AF) != RESET) B.state = I2C_STATE_NACK;
        else if ((currentTicksUs - B.timeout) >= I2C_TIMEOUT) B.state = I2C_STATE_BUS_ERROR;
        break;
    case I2C_STATE_W_TRANSFER:
        if (B.len > 0) {
            I2C_SendData(*B.buf);
            B.buf++;
            B.len--;
            B.timeout = currentTicksUs;
            B.state = I2C_STATE_W_TRANSFER_WAIT;
        } else {
            I2C_GenerateSTOP(ENABLE);
            B.timeout = currentTicksUs;
            B.txnOk = true;
            B.state = I2C_STATE_STOPPING;
        }
        break;
    case I2C_STATE_NACK:
        I2C_GenerateSTOP(ENABLE);
        I2C_ClearFlag(I2C_FLAG_AF);
        B.timeout = currentTicksUs;
        B.state = I2C_STATE_STOPPING;
        break;
    }
}

/* i2cWaitForCompletion: the busy loop, each pass 0.25 us of CPU time */
static void wait_completion(void)
{
    do {
        i2cStateMachine(micros());
        cpu_ns(250);
        loops++;
    } while (B.state != I2C_STATE_STOPPED);
}

static bool i2cWriteBuffer(uint8_t addr, uint8_t reg, uint8_t len, const uint8_t *data, bool raw)
{
    B.addr = addr << 1; B.reg = reg; B.rw = I2C_TXN_WRITE; B.len = len; B.buf = (uint8_t *)data;
    B.txnOk = false; B.state = I2C_STATE_STARTING; B.allowRawAccess = raw;
    wait_completion();
    return B.txnOk;
}

static bool i2cRead(uint8_t addr, uint8_t reg, uint8_t len, uint8_t *buf, bool raw)
{
    B.addr = addr << 1; B.reg = reg; B.rw = I2C_TXN_READ; B.len = len; B.buf = buf;
    B.txnOk = false; B.state = I2C_STATE_STARTING; B.allowRawAccess = raw;
    wait_completion();
    return B.txnOk;
}

/* ---- a scripted memory slave: 256 bytes, pointer byte, NACK after nack_after data bytes --- */
typedef struct Mem {
    uint8_t mem[256];
    uint8_t ptr;
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
    m->mem[m->ptr++] = b;
    m->data_written++;
    return true;
}
static uint8_t m_read(void *o) { Mem *m = o; m->reads++; return m->mem[m->ptr++]; }
static void m_stop(void *o) { Mem *m = o; m->stops++; }

/* the devices' glue, as nexus_xr.c's */
static fb_spl06_t *baro;
static fb_qmc5883l_t *mag;
static bool b_start(void *o, bool r) { (void)o; return fb_spl06_i2c_start(baro, r); }
static bool b_write(void *o, uint8_t b) { (void)o; return fb_spl06_i2c_write_byte(baro, b); }
static uint8_t b_read(void *o) { (void)o; return fb_spl06_i2c_read_byte(baro); }
static void b_stop(void *o) { (void)o; }
static bool q_start(void *o, bool r) { (void)o; return fb_qmc5883l_i2c_start(mag, r); }
static bool q_write(void *o, uint8_t b) { (void)o; return fb_qmc5883l_i2c_write_byte(mag, b); }
static uint8_t q_read(void *o) { (void)o; return fb_qmc5883l_i2c_read_byte(mag); }

static bool bus_idle(void)
{
    return C.phase == V1_P_IDLE && C.sr2 == 0 && !(C.cr1 & (V1_CR1_START | V1_CR1_STOP)) && due_ns < 0;
}

int main(void)
{
    C.name = "i2c1";
    C.pclk_hz = 42000000;
    C.hooks = (I2cHooks) { .timer_mod = h_mod, .timer_del = h_del, .irq = h_irq, .log_unimp = h_log };
    memset(&M, 0, sizeof(M));
    M.nack_after = -1;
    for (int i = 0; i < 256; i++) M.mem[i] = (uint8_t)(i * 7 + 3);
    C.slaves[C.nslaves++] = (I2cSlave) { .addr = 0x50, .present = true, .start = m_start,
        .write = m_write, .read = m_read, .stop = m_stop, .opaque = &M };
    baro = fb_spl06_new();
    mag = fb_qmc5883l_new();
    C.slaves[C.nslaves++] = (I2cSlave) { .addr = FB_SPL06_I2C_ADDR, .present = true, .start_ack = b_start,
        .write = b_write, .read = b_read, .stop = b_stop };
    C.slaves[C.nslaves++] = (I2cSlave) { .addr = FB_QMC5883L_I2C_ADDR, .present = true, .start_ack = q_start,
        .write = q_write, .read = q_read };

    i2c_init();
    CHECK(C.cr1 == (V1_CR1_PE | V1_CR1_ACK) && C.ccr == (35 | 1u << 15), "init: CR1 %x CCR %x", C.cr1, C.ccr);
    CHECK(i2c1v1_bit_ns(&C) == 2500, "400 kHz: %lld ns", (long long)i2c1v1_bit_ns(&C));
    CHECK(bus_idle() && C.sr1 == 0, "idle after init");

    /* write one byte: START, address, register, data, STOP */
    int64_t t0 = now_ns;
    CHECK(i2cWriteBuffer(0x50, 0x10, 1, (const uint8_t[]){ 0x5A }, false), "write 1");
    CHECK(M.mem[0x10] == 0x5A && M.starts == 1 && M.stops == 1, "slave got it: %02x %d %d", M.mem[0x10], M.starts, M.stops);
    /* 1 + 9 + 9 + 9 + 1 bit times = 72.5 us, plus the polling */
    CHECK(now_ns - t0 >= 72500 && now_ns - t0 < 80000, "write 1: %lld ns", (long long)(now_ns - t0));
    CHECK(bus_idle() && C.sr1 == 0 && C.xfers == 1, "idle after the write: sr1 %x", C.sr1);

    /* write 4 bytes */
    CHECK(i2cWriteBuffer(0x50, 0x20, 4, (const uint8_t[]){ 1, 2, 3, 4 }, false), "write 4");
    CHECK(!memcmp(&M.mem[0x20], (const uint8_t[]){ 1, 2, 3, 4 }, 4), "write 4 data");
    /* raw write (no register byte) */
    M.ptr_next = true;
    CHECK(i2cWriteBuffer(0x50, 0xFF, 2, (const uint8_t[]){ 0x30, 0x77 }, true), "raw write");
    CHECK(M.mem[0x30] == 0x77, "raw write: the first byte was the pointer");

    /* reads of every length the driver has a path for: 1, 2, 3, 4, 6, 20 */
    const int lens[] = { 1, 2, 3, 4, 6, 20 };
    for (unsigned k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
        uint8_t buf[32];
        int n = lens[k];
        memset(buf, 0xEE, sizeof(buf));
        int reads0 = M.reads;
        t0 = now_ns;
        CHECK(i2cRead(0x50, 0x40, n, buf, false), "read %d", n);
        CHECK(!memcmp(buf, &M.mem[0x40], n) && buf[n] == 0xEE, "read %d data", n);
        CHECK(M.reads - reads0 == n, "read %d: the slave sent %d bytes (the master NACKed the last)", n, M.reads - reads0);
        /* START + addr + reg + rSTART + addr + n bytes + STOP */
        int64_t bits = 1 + 9 + 9 + 1 + 9 + 9 * n + 1;
        CHECK(now_ns - t0 >= bits * 2500 && now_ns - t0 < bits * 2500 + 12000, "read %d: %lld ns for %lld bit times",
              n, (long long)(now_ns - t0), (long long)bits);
        CHECK(bus_idle() && !(C.sr1 & (V1_SR1_RXNE | V1_SR1_BTF)) && !C.shift_full, "read %d: idle after, sr1 %x", n, C.sr1);
    }
    /* raw read */
    {
        uint8_t buf[3];
        M.ptr = 0x60;
        CHECK(i2cRead(0x50, 0xFF, 3, buf, true) && !memcmp(buf, &M.mem[0x60], 3), "raw read");
    }

    /* an absent address: NACK, AF, the driver's STOP and ClearFlag(AF), logged once */
    {
        uint8_t buf[2];
        int lines = unimp_lines;
        uint64_t n0 = C.nacks;
        CHECK(!i2cRead(0x1E, 0x0A, 2, buf, false), "absent: read fails");
        CHECK(bus_idle() && C.sr1 == 0 && C.nacks == n0 + 1, "absent: idle, AF cleared (sr1 %x)", C.sr1);
        CHECK(!i2cWriteBuffer(0x1E, 0x00, 1, (const uint8_t[]){ 0 }, false), "absent: write fails");
        CHECK(unimp_lines == lines + 1, "absent: one log line per address");
    }
    /* a data byte NACKed */
    M.nack_after = 1;
    CHECK(!i2cWriteBuffer(0x50, 0x70, 3, (const uint8_t[]){ 9, 8, 7 }, false), "data NACK: fails");
    CHECK(M.mem[0x70] == 9 && M.mem[0x71] != 8 && bus_idle() && C.sr1 == 0, "data NACK: first byte only, idle");
    M.nack_after = -1;
    /* the probe: a read of length 0 is the register write and a STOP */
    CHECK(!i2cRead(0x50, 0x10, 0, NULL, false) && bus_idle(), "probe");
    CHECK(bus_errors == 0, "no bus errors: %d", bus_errors);

    /* the devices through the driver: SPL06 ID, QMC5883L detection, init and a read */
    {
        uint8_t id = 0, st = 0, b6[6];
        fb_spl06_tick(baro, 100000000);
        CHECK(i2cRead(FB_SPL06_I2C_ADDR, 0x0D, 1, &id, false) && id == FB_SPL06_PRODUCT_ID, "spl06 id %02x", id);
        CHECK(i2cWriteBuffer(FB_QMC5883L_I2C_ADDR, 0x0A, 1, (const uint8_t[]){ 0x80 }, false), "qmc reset");
        CHECK(i2cRead(FB_QMC5883L_I2C_ADDR, 0x0D, 1, &id, false) && id == 0xFF, "qmc id %02x", id);
        CHECK(i2cRead(FB_QMC5883L_I2C_ADDR, 0x09, 1, &id, false) && id == 0, "qmc standby");
        CHECK(i2cWriteBuffer(FB_QMC5883L_I2C_ADDR, 0x0B, 1, (const uint8_t[]){ 0x01 }, false) &&
              i2cWriteBuffer(FB_QMC5883L_I2C_ADDR, 0x09, 1, (const uint8_t[]){ 0x1D }, false), "qmc init");
        const double f[3] = { 0.1, -0.2, 0.3 };
        fb_qmc5883l_set_noise(mag, 0, 1);
        fb_qmc5883l_set_field(mag, f);
        fb_qmc5883l_tick(mag, 5000000);
        CHECK(i2cRead(FB_QMC5883L_I2C_ADDR, 0x06, 1, &st, false) && (st & 1), "qmc DRDY");
        CHECK(i2cRead(FB_QMC5883L_I2C_ADDR, 0x00, 6, b6, false), "qmc data");
        CHECK((int16_t)(b6[1] << 8 | b6[0]) == 300 && (int16_t)(b6[3] << 8 | b6[2]) == -600 &&
              (int16_t)(b6[5] << 8 | b6[4]) == 900, "qmc XYZ");
        /* detached: the address NACKs, the driver's read fails cleanly */
        fb_qmc5883l_set_fault(mag, "detached", true, NULL);
        CHECK(!i2cRead(FB_QMC5883L_I2C_ADDR, 0x06, 1, &st, false) && bus_idle() && C.sr1 == 0, "qmc detached: NACK");
        fb_qmc5883l_set_fault(mag, "detached", false, NULL);
        CHECK(i2cRead(FB_QMC5883L_I2C_ADDR, 0x0D, 1, &id, false) && id == 0xFF, "qmc back");
    }

    /* the interrupt lines: EV on SB/ADDR/BTF, TXE/RXNE with ITBUFEN, ER on AF */
    {
        wr(V1_CR2, rd(V1_CR2) | V1_CR2_ITEVTEN | V1_CR2_ITERREN);
        I2C_GenerateSTART(ENABLE);
        CHECK(!ev_line, "EV: nothing yet");
        cpu_ns(2500);
        CHECK(ev_line && (C.sr1 & V1_SR1_SB), "EV on SB");
        (void)rd(V1_SR1);
        I2C_Send7bitAddress(0x77 << 1, I2C_Direction_Transmitter);
        CHECK(!ev_line, "EV: SB cleared by the address");
        cpu_ns(9 * 2500);
        CHECK(!ev_line && er_line && (C.sr1 & V1_SR1_AF), "ER on AF (0x77 absent)");
        I2C_GenerateSTOP(ENABLE);
        I2C_ClearFlag(I2C_FLAG_AF);
        CHECK(!er_line, "ER: AF cleared");
        cpu_ns(2500);
        CHECK(bus_idle(), "idle");
        /* ADDR, then TXE only with ITBUFEN */
        I2C_GenerateSTART(ENABLE);
        cpu_ns(2500);
        (void)rd(V1_SR1);
        I2C_Send7bitAddress(0x50 << 1, I2C_Direction_Transmitter);
        cpu_ns(9 * 2500);
        CHECK(ev_line && (C.sr1 & V1_SR1_ADDR) && (C.sr1 & V1_SR1_TXE), "EV on ADDR (TXE set)");
        (void)rd(V1_SR1);
        (void)rd(V1_SR2);
        CHECK(!ev_line, "ADDR cleared, TXE without ITBUFEN: no EV");
        wr(V1_CR2, rd(V1_CR2) | V1_CR2_ITBUFEN);
        CHECK(ev_line, "TXE with ITBUFEN: EV");
        I2C_SendData(0x05);
        cpu_ns(9 * 2500);
        CHECK(ev_line && (C.sr1 & V1_SR1_BTF), "EV on BTF");
        I2C_GenerateSTOP(ENABLE);
        cpu_ns(2500);
        CHECK(bus_idle() && !ev_line && !er_line, "idle, lines low");
        wr(V1_CR2, rd(V1_CR2) & ~(V1_CR2_ITEVTEN | V1_CR2_ITERREN | V1_CR2_ITBUFEN));
    }

    /* RM0090's 2-byte read (not iNav's order): ACK = 0 and POS = 1 before ADDR is cleared; the first
     * byte is still ACKed, the second NACKed, both wait in DR + shift register (BTF) */
    {
        M.ptr = 0x40;
        int reads0 = M.reads;
        I2C_GenerateSTART(ENABLE);
        cpu_ns(2500);
        (void)rd(V1_SR1);
        I2C_Send7bitAddress(0x50 << 1, I2C_Direction_Receiver);
        cpu_ns(9 * 2500);
        CHECK(rd(V1_SR1) & V1_SR1_ADDR, "2-byte: ADDR");
        I2C_AcknowledgeConfig(DISABLE);
        wr(V1_CR1, rd(V1_CR1) | V1_CR1_POS);
        (void)rd(V1_SR2);                       /* ADDR cleared: byte 1 on the wire */
        cpu_ns(2 * 9 * 2500 + 2500);
        CHECK((rd(V1_SR1) & V1_SR1_BTF) && M.reads - reads0 == 2, "2-byte: BTF, two bytes (sr1 %x, %d)",
              C.sr1, M.reads - reads0);
        I2C_GenerateSTOP(ENABLE);
        uint8_t b0 = I2C_ReceiveData(), b1 = I2C_ReceiveData();
        cpu_ns(3 * 2500);
        CHECK(b0 == M.mem[0x40] && b1 == M.mem[0x41] && M.reads - reads0 == 2, "2-byte: data %02x %02x", b0, b1);
        CHECK(bus_idle(), "2-byte: idle after the STOP");
        wr(V1_CR1, (rd(V1_CR1) & ~V1_CR1_POS) | V1_CR1_ACK);
    }

    /* PE = 0 in the middle of a read releases the bus; SWRST resets the registers */
    {
        I2C_GenerateSTART(ENABLE);
        cpu_ns(2500);
        (void)rd(V1_SR1);
        I2C_Send7bitAddress(0x50 << 1, I2C_Direction_Receiver);
        cpu_ns(9 * 2500);
        (void)rd(V1_SR1);
        (void)rd(V1_SR2);
        cpu_ns(4 * 2500);                       /* a byte half on the wire */
        int stops = M.stops;
        wr(V1_CR1, rd(V1_CR1) & ~V1_CR1_PE);
        CHECK(C.phase == V1_P_IDLE && C.sr1 == 0 && C.sr2 == 0 && due_ns < 0 && M.stops == stops + 1,
              "PE = 0: released");
        wr(V1_CR1, V1_CR1_SWRST);
        CHECK(C.cr1 == V1_CR1_SWRST && C.ccr == 0 && C.cr2 == 0 && C.trise == 2, "SWRST held");
        wr(V1_CR1, 0);
        i2c_init();
        uint8_t b = 0;
        CHECK(i2cRead(0x50, 0x10, 1, &b, false) && b == 0x5A, "works again after SWRST + init");
    }

    printf("test_i2c_v1: %d checks, %d failed (%d busy-loop passes)\n", checks, fails, loops);
    fb_spl06_free(baro);
    fb_qmc5883l_free(mag);
    return fails ? 1 : 0;
}
