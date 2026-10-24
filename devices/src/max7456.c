/* FlightBench device model: MAX7456 / AT7456E analog OSD on SPI (see include/fb/max7456.h). */
#include "fb/max7456.h"

#include <stdlib.h>
#include <string.h>

enum {
    VM0 = 0x00, VM1 = 0x01, HOS = 0x02, VOS = 0x03, DMM = 0x04, DMAH = 0x05, DMAL = 0x06,
    DMDI = 0x07, CMM = 0x08, CMAH = 0x09, CMAL = 0x0A, CMDI = 0x0B, OSDM = 0x0C, RB0 = 0x10,
    OSDBL = 0x6C,
    RD = 0x80, STAT_RD = 0xA0, DMDO_RD = 0xB0, CMDO_RD = 0xC0,
};

#define VM0_RESET       0x02
#define DMM_AUTOINC     0x01
#define DMM_CLEAR       0x04
#define DMM_8BIT        0x40
#define DMAH_ATTR       0x02
#define CMM_READ_NVR    0x50
#define CMM_WRITE_NVR   0xA0
#define STAT_LOS        0x04

struct fb_max7456 {
    uint8_t reg[0x20];
    uint8_t osdbl;
    uint8_t chars[FB_MAX7456_DISPLAY], attrs[FB_MAX7456_DISPLAY];
    uint8_t shadow[FB_MAX7456_CHAR_BYTES];
    uint8_t nvm[FB_MAX7456_CHARS][FB_MAX7456_CHAR_BYTES];
    bool cs;
    bool data_phase;            /* the next byte is the data byte of the word */
    bool autoinc;               /* auto-increment: bytes are DMDI data until 0xFF */
    uint8_t addr;
    fb_max7456_stats_t st;
};

static void regs_reset(fb_max7456_t *d)
{
    memset(d->reg, 0, sizeof(d->reg));
    d->reg[VM1] = 0x47;
    d->reg[HOS] = 0x20;
    d->reg[VOS] = 0x10;
    d->reg[OSDM] = 0x1B;
    for (int i = 0; i < 16; i++) {
        d->reg[RB0 + i] = 0x01;
    }
    d->osdbl = 0x00;
    d->autoinc = false;
    memset(d->shadow, 0, sizeof(d->shadow));
}

fb_max7456_t *fb_max7456_new(void)
{
    fb_max7456_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    memset(d->nvm, 0x55, sizeof(d->nvm));      /* every pixel transparent */
    fb_max7456_reset(d);
    return d;
}

void fb_max7456_free(fb_max7456_t *dev)
{
    free(dev);
}

void fb_max7456_reset(fb_max7456_t *d)
{
    regs_reset(d);
    memset(d->chars, 0, sizeof(d->chars));
    memset(d->attrs, 0, sizeof(d->attrs));
    d->data_phase = false;
}

void fb_max7456_cs(fb_max7456_t *d, bool asserted)
{
    d->cs = asserted;
    d->data_phase = false;          /* a frame starts with an address byte */
}

static unsigned disp_addr(const fb_max7456_t *d)
{
    return ((d->reg[DMAH] & 1u) << 8 | d->reg[DMAL]) % FB_MAX7456_DISPLAY;
}

static unsigned nvm_char(const fb_max7456_t *d)
{
    return (d->reg[CMAH] | ((d->reg[CMAL] >> 6) & 1u) << 8) % FB_MAX7456_CHARS;
}

static void disp_store(fb_max7456_t *d, unsigned a, uint8_t v, bool attr)
{
    if (attr) {
        d->attrs[a] = v;
    } else {
        d->chars[a] = v;
        if (!(d->reg[DMM] & DMM_8BIT)) {
            /* 16-bit mode: the attribute from DMM[5:3], kept as the 8-bit mode's DMDI attribute
             * byte (LBC/BLK/INV at [7:5], character bit 8 at [4] = 0) */
            d->attrs[a] = ((d->reg[DMM] >> 3) & 7) << 5;
        }
    }
    d->st.display_writes++;
}

static void reg_write(fb_max7456_t *d, uint8_t a, uint8_t v)
{
    if (a == OSDBL) {
        d->osdbl = v;
        return;
    }
    if (a >= sizeof(d->reg)) {
        return;
    }
    switch (a) {
    case VM0:
        if (v & VM0_RESET) {
            regs_reset(d);                      /* done at once: the bit reads back 0 */
            d->reg[VM0] = v & ~VM0_RESET;
            d->st.resets++;
            return;
        }
        break;
    case DMM:
        if (v & DMM_CLEAR) {
            memset(d->chars, 0, sizeof(d->chars));
            memset(d->attrs, 0, sizeof(d->attrs));
            d->st.clears++;
            v &= ~(DMM_CLEAR | 0x02);           /* clear done: the bits read back 0 */
        }
        break;
    case DMDI: {
        bool attr = (d->reg[DMM] & DMM_8BIT) && (d->reg[DMAH] & DMAH_ATTR);
        disp_store(d, disp_addr(d), v, attr);
        if (d->reg[DMM] & DMM_AUTOINC) {
            d->autoinc = true;                  /* next bytes: data only, up to 0xFF */
            unsigned n = (disp_addr(d) + 1) % FB_MAX7456_DISPLAY;
            d->reg[DMAH] = (d->reg[DMAH] & ~1u) | (n >> 8);
            d->reg[DMAL] = n & 0xFF;
        }
        break;
    }
    case CMM:
        if (v == CMM_READ_NVR) {
            memcpy(d->shadow, d->nvm[nvm_char(d)], sizeof(d->shadow));
            d->st.nvm_reads++;
        } else if (v == CMM_WRITE_NVR) {
            memcpy(d->nvm[nvm_char(d)], d->shadow, sizeof(d->shadow));
            d->st.nvm_writes++;
        }
        break;
    case CMDI:
        d->shadow[d->reg[CMAL] & 0x3F] = v;
        break;
    }
    d->reg[a] = v;
}

static uint8_t reg_read(const fb_max7456_t *d, uint8_t a)
{
    if (a >= RD && a < RD + sizeof(d->reg)) {
        return d->reg[a - RD];
    }
    switch (a) {
    case STAT_RD:
        return STAT_LOS;                        /* no camera: loss of sync; never NVR busy */
    case DMDO_RD:
        return (d->reg[DMM] & DMM_8BIT) && (d->reg[DMAH] & DMAH_ATTR) ? d->attrs[disp_addr(d)]
                                                                        : d->chars[disp_addr(d)];
    case CMDO_RD:
        return d->shadow[d->reg[CMAL] & 0x3F];
    case OSDBL | RD:
        return d->osdbl;
    }
    return 0x00;
}

uint8_t fb_max7456_xfer(fb_max7456_t *d, uint8_t mosi)
{
    if (!d->cs) {
        return 0xFF;                            /* DOUT is high impedance */
    }
    if (d->autoinc) {
        if (mosi == 0xFF) {                     /* escape: auto-increment mode ends */
            d->autoinc = false;
            d->reg[DMM] &= ~DMM_AUTOINC;
        } else {
            reg_write(d, DMDI, mosi);
        }
        return 0x00;
    }
    if (!d->data_phase) {
        d->addr = mosi;
        d->data_phase = true;
        return 0x00;
    }
    d->data_phase = false;
    if (d->addr & RD) {
        return reg_read(d, d->addr);
    }
    reg_write(d, d->addr, mosi);
    return 0x00;
}

uint8_t fb_max7456_peek(const fb_max7456_t *d, uint8_t reg)
{
    return reg == OSDBL ? d->osdbl : reg < sizeof(d->reg) ? d->reg[reg] : 0;
}

uint16_t fb_max7456_display(const fb_max7456_t *d, unsigned cell)
{
    cell %= FB_MAX7456_DISPLAY;
    return d->chars[cell] | d->attrs[cell] << 8;
}

const uint8_t *fb_max7456_nvm(const fb_max7456_t *d, unsigned ch)
{
    return d->nvm[ch % FB_MAX7456_CHARS];
}

const fb_max7456_stats_t *fb_max7456_stats(const fb_max7456_t *d)
{
    return &d->st;
}
