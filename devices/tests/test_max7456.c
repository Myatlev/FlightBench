/* MAX7456 model: the register sequences of iNav 9.1.0's drivers/max7456.c (init, NVM font read,
 * NVM font write, screen update in 16- and 8-bit modes, clear display) over the byte-level SPI API,
 * framed as its bus layer frames them (busWrite = one CS frame of address + data, busRead = address
 * with the read bit + one dummy byte, busTransfer = one CS frame of address/data pairs). */
#include "fb/max7456.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void wr(fb_max7456_t *d, uint8_t a, uint8_t v)
{
    fb_max7456_cs(d, true);
    fb_max7456_xfer(d, a);
    fb_max7456_xfer(d, v);
    fb_max7456_cs(d, false);
}

static uint8_t rd(fb_max7456_t *d, uint8_t a)
{
    fb_max7456_cs(d, true);
    fb_max7456_xfer(d, a);
    uint8_t v = fb_max7456_xfer(d, 0xFF);
    fb_max7456_cs(d, false);
    return v;
}

/* busTransfer of address/data pairs in one frame */
static void pairs(fb_max7456_t *d, const uint8_t *b, int n)
{
    fb_max7456_cs(d, true);
    for (int i = 0; i < n; i++) {
        fb_max7456_xfer(d, b[i]);
    }
    fb_max7456_cs(d, false);
}

int main(void)
{
    fb_max7456_t *d = fb_max7456_new();

    /* not selected: DOUT high impedance */
    CHECK(fb_max7456_xfer(d, 0xA0) == 0xFF);

    /* max7456Init: soft reset, VM0 polled (reads back without the reset bit), RB0..15 + VM1 */
    wr(d, 0x00, 0x02);
    CHECK(rd(d, 0x80) == 0x00);
    CHECK(fb_max7456_stats(d)->resets == 1);
    {
        uint8_t b[34];
        int n = 0;
        for (int i = 0; i < 16; i++) {
            b[n++] = 0x10 + i;
            b[n++] = 0x00;                  /* MAX7456_BWBRIGHTNESS */
        }
        b[n++] = 0x01;
        b[n++] = 0x0C;                      /* VM1: blink 50/50, time 3, background 28 % */
        pairs(d, b, n);
        CHECK(fb_max7456_peek(d, 0x1F) == 0x00 && fb_max7456_peek(d, 0x01) == 0x0C);
    }
    /* reset values: VM1 0x47 after a soft reset, HOS/VOS, RB = 0x01 */
    wr(d, 0x00, 0x02);
    CHECK(rd(d, 0x81) == 0x47 && rd(d, 0x82) == 0x20 && rd(d, 0x83) == 0x10 && rd(d, 0x90) == 0x01);

    /* STAT: loss of sync (no camera), never NVR busy: max7456WaitUntilNoBusy() returns */
    CHECK(rd(d, 0xA0) == 0x04);
    CHECK(!(rd(d, 0xA0) & 0x20));

    /* max7456WriteNvm(char 0x42): CMAH, CMAL/CMDI pairs, CMM WRITE_NVR in one frame */
    {
        uint8_t b[2 + 54 * 4 + 2];
        int n = 0;
        b[n++] = 0x09;
        b[n++] = 0x42;
        for (int x = 0; x < 54; x++) {
            b[n++] = 0x0A;
            b[n++] = x;
            b[n++] = 0x0B;
            b[n++] = (uint8_t)(0xA0 + x);
        }
        b[n++] = 0x08;
        b[n++] = 0xA0;
        pairs(d, b, n);
        CHECK(fb_max7456_stats(d)->nvm_writes == 1);
        CHECK(fb_max7456_nvm(d, 0x42)[0] == 0xA0 && fb_max7456_nvm(d, 0x42)[53] == 0xA0 + 53);
    }
    /* max7456ReadNvm(0x42): OSD off, CMAH, CMM READ_NVR, wait, CMAL/CMDO per byte */
    wr(d, 0x00, 0x00);
    wr(d, 0x09, 0x42);
    wr(d, 0x08, 0x50);
    CHECK(!(rd(d, 0xA0) & 0x20));
    {
        int ok = 1;
        for (int x = 0; x < 54; x++) {
            wr(d, 0x0A, x);
            ok &= rd(d, 0xC0) == (uint8_t)(0xA0 + x);
        }
        CHECK(ok);
    }
    /* a character never written: the blank NVM (transparent pixels) */
    wr(d, 0x09, 0xFF);
    wr(d, 0x08, 0x50);
    wr(d, 0x0A, 0x00);
    CHECK(rd(d, 0xC0) == 0x55);
    /* character 0x142 (> 255) through CMAL bit 6 (AT7456E) */
    wr(d, 0x09, 0x42);
    wr(d, 0x0A, 1 << 6);
    wr(d, 0x08, 0x50);
    wr(d, 0x0A, 0x00);
    CHECK(rd(d, 0xC0) == 0x55);
    CHECK(fb_max7456_stats(d)->nvm_reads == 3);

    /* max7456DrawScreen, 16-bit mode: DMM with the character mode, DMAH/DMAL/DMDI per cell */
    {
        const uint8_t b[] = { 0x04, 0x08 /* DMM: invert attr */, 0x05, 0x01, 0x06, 0x0E, 0x07, 'A' };
        pairs(d, b, sizeof(b));
        CHECK(fb_max7456_display(d, 0x10E) == ('A' | (1 << 5) << 8));   /* INV, the DMDI layout */
    }
    /* 8-bit mode: attribute byte through DMAH bit 1, then the character */
    {
        const uint8_t b[] = { 0x04, 0x40, 0x05, 0x02, 0x06, 0x20, 0x07, 0x5 << 2,
                              0x05, 0x00, 0x06, 0x20, 0x07, 'B' };
        pairs(d, b, sizeof(b));
        CHECK(fb_max7456_display(d, 0x20) == ('B' | (0x5 << 2) << 8));
        CHECK(rd(d, 0xB0) == 'B');
    }
    /* clear display: DMM bit 2 reads back 0 at once, the memory is blank */
    wr(d, 0x04, 0x04);
    CHECK(!(rd(d, 0x84) & 0x04));
    CHECK(fb_max7456_display(d, 0x20) == 0 && fb_max7456_stats(d)->clears == 1);
    /* auto-increment: data bytes after DMDI up to the 0xFF escape */
    {
        const uint8_t b[] = { 0x04, 0x01, 0x05, 0x00, 0x06, 0x05, 0x07, 'x', 'y', 'z', 0xFF };
        pairs(d, b, sizeof(b));
        CHECK(fb_max7456_display(d, 5) == 'x' && fb_max7456_display(d, 6) == 'y' &&
              fb_max7456_display(d, 7) == 'z' && fb_max7456_display(d, 8) == 0);
        CHECK(!(rd(d, 0x84) & 0x01));
    }
    /* OSDBL at 0x6C / 0xEC */
    wr(d, 0x6C, 0x10);
    CHECK(rd(d, 0xEC) == 0x10);
    /* power-on reset keeps the NVM */
    fb_max7456_reset(d);
    CHECK(fb_max7456_nvm(d, 0x42)[0] == 0xA0 && fb_max7456_display(d, 5) == 0);

    fb_max7456_free(d);
    printf("test_max7456: %s\n", fails ? "FAILED" : "OK");
    return fails != 0;
}
