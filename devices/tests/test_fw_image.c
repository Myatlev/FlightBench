/* Firmware file parsing (fb/fw_image.h): good ELF/HEX/raw images land where they belong, broken
 * ones (truncated or overflowing headers, segments outside the file or the flash, bad records)
 * are refused without reading or writing out of bounds (run under -fsanitize=address to see). */
#include "fb/fw_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define BASE 0x08000000u
#define SIZE (512u * 1024)
#define ALIAS 0x00200000u

static uint8_t img[SIZE];
static const fb_fw_flash_t fl = { img, BASE, SIZE, ALIAS };
static char err[200];

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* ELF header + nph program headers at phoff, a 16-byte payload at 0x100 */
static size_t mk_elf(uint8_t *d, uint32_t phoff, uint16_t phentsize, uint16_t phnum, uint32_t paddr)
{
    memset(d, 0, 0x200);
    memcpy(d, "\x7f" "ELF", 4);
    d[4] = 1; d[5] = 1; d[6] = 1;
    w16(d + 16, 2);             /* ET_EXEC */
    w16(d + 18, 40);            /* EM_ARM */
    w32(d + 28, phoff);
    w16(d + 42, phentsize);
    w16(d + 44, phnum);
    if (phoff < 0x100 && phentsize >= 32) {
        uint8_t *ph = d + phoff;
        w32(ph, 1);             /* PT_LOAD */
        w32(ph + 4, 0x100);     /* offset */
        w32(ph + 8, paddr);
        w32(ph + 12, paddr);
        w32(ph + 16, 16);       /* filesz */
    }
    for (int i = 0; i < 16; i++) d[0x100 + i] = (uint8_t)(0xA0 + i);
    return 0x110;
}

static bool parse(const uint8_t *d, size_t n)
{
    memset(img, 0xFF, SIZE);
    err[0] = 0;
    return fb_fw_parse(&fl, d, n, err, sizeof err);
}

static void elf_tests(void)
{
    uint8_t d[0x200];
    size_t n = mk_elf(d, 52, 32, 1, BASE + 0x1000);
    CHECK(parse(d, n) && img[0x1000] == 0xA0 && img[0x100F] == 0xAF, "good ELF: %s", err);

    /* the ITCM alias maps onto the same flash */
    n = mk_elf(d, 52, 32, 1, ALIAS + 0x2000);
    CHECK(parse(d, n) && img[0x2000] == 0xA0, "alias ELF: %s", err);

    /* ELF header itself truncated */
    CHECK(!parse(d, 40), "truncated ehdr accepted");

    /* program headers past the end of the file */
    n = mk_elf(d, 52, 32, 1, BASE);
    CHECK(!parse(d, 60) && strstr(err, "truncated"), "truncated phdr: %s", err);

    /* phoff near 4 GiB: phoff + i * phentsize overflowed 32 bits before */
    n = mk_elf(d, 0xFFFFFFF0u, 32, 2, BASE);
    CHECK(!parse(d, n) && strstr(err, "truncated"), "overflowing phoff: %s", err);

    /* phnum * phentsize beyond the file */
    n = mk_elf(d, 52, 32, 0xFFFF, BASE);
    CHECK(!parse(d, n) && strstr(err, "truncated"), "huge phnum: %s", err);

    /* program header entries smaller than Elf32_Phdr would read past each entry */
    n = mk_elf(d, 52, 8, 1, BASE);
    CHECK(!parse(d, n) && strstr(err, "entry size"), "small phentsize: %s", err);
    n = mk_elf(d, 52, 0, 3, BASE);
    CHECK(!parse(d, n) && strstr(err, "entry size"), "zero phentsize: %s", err);

    /* segment data outside the file */
    n = mk_elf(d, 52, 32, 1, BASE);
    w32(d + 52 + 4, 0xFFFFFFF8u);
    CHECK(!parse(d, n) && strstr(err, "outside the file"), "segment offset: %s", err);
    n = mk_elf(d, 52, 32, 1, BASE);
    w32(d + 52 + 16, 0xFFFFFFFFu);
    CHECK(!parse(d, n) && strstr(err, "outside the file"), "segment size: %s", err);

    /* segment outside the flash, and one crossing its end */
    n = mk_elf(d, 52, 32, 1, 0x20000000u);
    CHECK(!parse(d, n) && strstr(err, "outside the flash"), "segment in RAM: %s", err);
    n = mk_elf(d, 52, 32, 1, BASE + SIZE - 8);
    CHECK(!parse(d, n) && strstr(err, "outside the flash"), "segment across the end: %s", err);
    n = mk_elf(d, 52, 32, 1, 0xFFFFFFF8u);
    CHECK(!parse(d, n), "segment at the top of the address space");

    /* no PT_LOAD at all */
    n = mk_elf(d, 52, 32, 1, BASE);
    w32(d + 52, 4);
    CHECK(!parse(d, n) && strstr(err, "no loadable"), "no PT_LOAD: %s", err);

    /* wrong class/machine */
    n = mk_elf(d, 52, 32, 1, BASE);
    d[4] = 2;
    CHECK(!parse(d, n) && strstr(err, "32-bit"), "ELF64 accepted");
}

static bool hex(const char *t)
{
    return parse((const uint8_t *)t, strlen(t));
}

static void hex_tests(void)
{
    /* :02 0000 04 0800 F2 (base 0x08000000), 4 data bytes at 0x0010, EOF */
    CHECK(hex(":020000040800F2\r\n:0400100001020304E2\n:00000001FF\n") &&
          img[0x10] == 1 && img[0x13] == 4, "good hex: %s", err);
    /* no EOF record, trailing blanks */
    CHECK(hex(":020000040800F2\n:0400100001020304E2  \n\n") && img[0x13] == 4, "hex without EOF: %s", err);
    /* bad checksum */
    CHECK(!hex(":020000040800F2\n:0400100001020304E3\n") && strstr(err, "checksum"), "bad checksum: %s", err);
    /* length byte disagrees with the record */
    CHECK(!hex(":0300100001020304E3\n"), "length mismatch accepted");
    /* odd length, no colon, not hex, too short */
    CHECK(!hex(":0400100001020304E\n"), "odd length accepted");
    {   /* fb_fw_parse() takes this for a raw binary; the HEX parser itself must refuse it */
        const char *t = "0400100001020304E2\n:00000001FF\n";
        CHECK(!fb_fw_parse_hex(&fl, t, strlen(t), err, sizeof err) && strstr(err, "line 1"), "no colon: %s", err);
    }
    CHECK(!hex(":04001000010203G4E2\n") && strstr(err, "not hex"), "non-hex: %s", err);
    CHECK(!hex(":0000\n"), "short record accepted");
    /* a line longer than any record */
    {
        static char big[1200];
        big[0] = ':';
        memset(big + 1, '0', 1100);
        big[1101] = 0;
        CHECK(!hex(big) && strstr(err, "bad record"), "long line: %s", err);
    }
    /* data outside the flash: no base record (address 0x10), and crossing the end */
    CHECK(!hex(":0400100001020304E2\n") && strstr(err, "outside the flash"), "hex at 0: %s", err);
    CHECK(!hex(":020000040807EB\n:04FFFE0001020304F5\n") && strstr(err, "outside"), "hex across the end: %s", err);
    /* malformed address record */
    CHECK(!hex(":0100000408F3\n") && strstr(err, "address record"), "short type-04: %s", err);
    /* no data records at all: nothing to flash (only EOF; only an address record; empty) */
    CHECK(!hex(":00000001FF\n") && strstr(err, "no data"), "hex with only EOF: %s", err);
    CHECK(!hex(":020000040800F2\n") && strstr(err, "no data"), "hex with only a base record: %s", err);
    {
        const char *t = "\n  \r\n";
        CHECK(!fb_fw_parse_hex(&fl, t, strlen(t), err, sizeof err) && strstr(err, "no data"),
              "blank hex: %s", err);
    }
}

static void raw_and_limits(void)
{
    static uint8_t d[SIZE + 16];
    memset(d, 0x5A, sizeof d);
    CHECK(parse(d, 1000) && img[0] == 0x5A && img[999] == 0x5A && img[1000] == 0xFF, "raw: %s", err);
    CHECK(parse(d, SIZE) && img[SIZE - 1] == 0x5A, "raw full flash: %s", err);
    CHECK(!parse(d, SIZE + 1), "raw larger than the flash accepted");
    CHECK(!parse(d, FB_FW_MAX_FILE + 1) && strstr(err, "larger"), "file above the cap: %s", err);
}

static void defaults_area(void)
{
    uint32_t s = 0, e = 0;
    memset(img, 0xFF, SIZE);
    /* an image without the pointers (erased) */
    CHECK(!fb_fw_custom_defaults_area(&fl, &s, &e, err, sizeof err), "erased pointers accepted");
    /* the build's own pointers */
    w32(img + 0x2800, 0x08002808u);
    w32(img + 0x2804, 0x08004000u);
    CHECK(fb_fw_custom_defaults_area(&fl, &s, &e, err, sizeof err) && s == 0x08002808u && e == 0x08004000u,
          "good pointers: %s", err);
    /* start before the pointers, end beyond the flash, reversed, too large */
    w32(img + 0x2800, 0x08002800u);
    CHECK(!fb_fw_custom_defaults_area(&fl, &s, &e, err, sizeof err), "start over the pointers accepted");
    w32(img + 0x2800, 0x08002808u);
    w32(img + 0x2804, BASE + SIZE + 4);
    CHECK(!fb_fw_custom_defaults_area(&fl, &s, &e, err, sizeof err), "end beyond the flash accepted");
    w32(img + 0x2804, 0x08002000u);
    CHECK(!fb_fw_custom_defaults_area(&fl, &s, &e, err, sizeof err), "reversed accepted");
    w32(img + 0x2804, 0x08002808u + FB_FW_CUSTOM_DEFAULTS_MAX + 4);
    CHECK(!fb_fw_custom_defaults_area(&fl, &s, &e, err, sizeof err), "oversized area accepted");

    /* the blob only goes into erased flash */
    memset(img, 0xFF, SIZE);
    CHECK(fb_fw_erased(&fl, 0x08002808u, 0x08004000u), "erased area not seen as erased");
    img[0x3FFF] = 0x00;
    CHECK(!fb_fw_erased(&fl, 0x08002808u, 0x08004000u), "last byte written, still 'erased'");
    CHECK(fb_fw_erased(&fl, 0x08002808u, 0x08003FFFu), "end is exclusive");
    img[0x3FFF] = 0xFF;
    img[0x2808] = 0x12;
    CHECK(!fb_fw_erased(&fl, 0x08002808u, 0x08004000u), "first byte written, still 'erased'");
    CHECK(!fb_fw_erased(&fl, BASE - 4, BASE + 4), "below the flash accepted");
    CHECK(!fb_fw_erased(&fl, BASE + SIZE - 4, BASE + SIZE + 4), "beyond the flash accepted");
    CHECK(fb_fw_erased(&fl, BASE + SIZE - 4, BASE + SIZE), "up to the end of the flash");
    CHECK(!fb_fw_erased(&fl, 0x08003000u, 0x08002000u), "reversed range accepted");
}

int main(void)
{
    elf_tests();
    hex_tests();
    raw_and_limits();
    defaults_area();
    if (fails) { printf("test_fw_image: %d failure(s)\n", fails); return 1; }
    printf("test_fw_image: all passed\n");
    return 0;
}
