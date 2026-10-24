/* FlightBench device model: Maxim MAX7456 (and the AT7456E clone) monochrome analog OSD on SPI.
 *
 * Emulator-independent: the emulator owns the SPI controller and the chip-select GPIO; this model
 * only knows bytes and registers. There is no video: the chip sees no camera (STAT reports loss of
 * sync), nothing is overlaid. What it has is what a firmware driver talks to:
 *
 *   SPI          fb_max7456_cs()      chip select asserted / released (frames a transaction)
 *                fb_max7456_xfer()    full-duplex byte exchange, returns the MISO byte
 *
 * Protocol (datasheet 19-0576, "Serial Interface"): 16-bit words, the address byte first, then
 * the data byte; any number of words per CS frame. Write addresses 0x00..0x1F and 0x6C (OSDBL);
 * read addresses are the write address | 0x80, plus STAT 0xA0, DMDO 0xB0, CMDO 0xC0. In display
 * memory auto-increment mode (DMM bit 0) the bytes after a DMDI write are character data with no
 * address byte, up to the escape 0xFF.
 *
 * Modelled: the register file with its reset values; VM0 software reset (done at once); DMM clear
 * display (done at once, the bit reads back 0); display memory 512 characters + attributes
 * (16-bit mode: the attribute from DMM[5:3]; 8-bit mode: DMAH bit 1 selects the attribute byte;
 * both kept in the attribute byte's layout: LBC/BLK/INV at [7:5], character bit 8 at [4]);
 * character memory: CMM READ_NVR (0x50) copies the NVM character CMAH (+ CMAL bit 6 = bit 8 on the
 * 512-character AT7456E) into the 64-byte shadow read through CMDO at CMAL[5:0], CMDI writes the
 * shadow, CMM WRITE_NVR (0xA0) stores it (at once: STAT NVR busy is never seen set); STAT = LOS
 * (no video input). The NVM starts with every pixel transparent (0x55): which font the real board
 * ships with is not known here. Not modelled: video timing, the overlay, VSYNC/HSYNC/LOS outputs,
 * the NVM write time (~12 ms).
 */
#ifndef FB_MAX7456_H
#define FB_MAX7456_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_MAX7456_CHARS        512     /* AT7456E; the MAX7456 has 256 */
#define FB_MAX7456_CHAR_BYTES   64      /* 54 used (12 x 18 pixels, 2 bit each), 10 unused */
#define FB_MAX7456_DISPLAY      512     /* display memory cells (PAL 16 x 30 = 480 used) */

typedef struct fb_max7456 fb_max7456_t;

typedef struct {
    uint32_t nvm_reads, nvm_writes;     /* CMM READ_NVR / WRITE_NVR commands */
    uint32_t display_writes;            /* DMDI bytes stored (characters and attributes) */
    uint32_t clears;                    /* DMM clear display */
    uint32_t resets;                    /* VM0 software resets */
} fb_max7456_stats_t;

fb_max7456_t *fb_max7456_new(void);
void fb_max7456_free(fb_max7456_t *dev);
void fb_max7456_reset(fb_max7456_t *dev);  /* power-on: registers and display memory; NVM kept */

void    fb_max7456_cs(fb_max7456_t *dev, bool asserted);
uint8_t fb_max7456_xfer(fb_max7456_t *dev, uint8_t mosi);

/* Introspection (tests, a future OSD view): register (write address 0x00..0x1F, 0x6C),
 * display memory cell, one NVM character */
uint8_t fb_max7456_peek(const fb_max7456_t *dev, uint8_t reg);
uint16_t fb_max7456_display(const fb_max7456_t *dev, unsigned cell);  /* char | attr << 8 */
const uint8_t *fb_max7456_nvm(const fb_max7456_t *dev, unsigned ch);
const fb_max7456_stats_t *fb_max7456_stats(const fb_max7456_t *dev);

#ifdef __cplusplus
}
#endif
#endif
