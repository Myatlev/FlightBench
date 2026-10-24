/* Firmware files -> flash image. See include/fb/fw_image.h. */
#include "fb/fw_image.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
/* mingw's <stdio.h> names the archetype of the printf in effect (gnu_printf with
 * __USE_MINGW_ANSI_STDIO, see Makefile; printf under clang, which has no gnu_printf) */
#if defined(__MINGW_PRINTF_FORMAT)
#define FB_PRINTF_ARCHETYPE __MINGW_PRINTF_FORMAT
#else
#define FB_PRINTF_ARCHETYPE printf
#endif

#if defined(__GNUC__)
__attribute__((format(FB_PRINTF_ARCHETYPE, 3, 4)))
#endif
static bool fail(char *err, size_t errlen, const char *fmt, ...)
{
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return false;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

bool fb_fw_put(const fb_fw_flash_t *f, uint32_t addr, const uint8_t *d, size_t n, char *err, size_t errlen)
{
    uint64_t a = addr;
    if (f->alias_base && a >= f->alias_base && a < (uint64_t)f->alias_base + f->size) {
        a = a - f->alias_base + f->base;
    }
    if (a < f->base || (uint64_t)n > f->size || a - f->base > f->size - (uint64_t)n) {
        return fail(err, errlen, "data at 0x%08x+0x%zx is outside the flash", addr, n);
    }
    memcpy(f->img + (a - f->base), d, n);
    return true;
}

/* ELF32 little-endian ARM: e_phoff @28, e_phentsize @42, e_phnum @44; program header:
 * p_type @0, p_offset @4, p_paddr @12, p_filesz @16 (Elf32_Phdr is 32 bytes) */
#define EHDR_SIZE 52u
#define PHDR_SIZE 32u

bool fb_fw_parse_elf(const fb_fw_flash_t *f, const uint8_t *d, size_t n, char *err, size_t errlen)
{
    if (n < EHDR_SIZE || memcmp(d, "\x7f" "ELF", 4) || d[4] != 1 /* ELFCLASS32 */ ||
        d[5] != 1 /* ELFDATA2LSB */ || le16(d + 18) != 40 /* EM_ARM */) {
        return fail(err, errlen, "not a 32-bit little-endian ARM ELF");
    }
    uint64_t phoff = le32(d + 28);
    uint64_t phentsize = le16(d + 42), phnum = le16(d + 44);
    if (phnum && phentsize < PHDR_SIZE) {
        return fail(err, errlen, "program header entry size %u < %u", (unsigned)phentsize, PHDR_SIZE);
    }
    /* all in 64 bits and before any pointer into the headers is formed */
    if (phoff > n || phnum * phentsize > n - phoff) {
        return fail(err, errlen, "truncated program headers (%u x %u at 0x%x, file %zu bytes)",
                    (unsigned)phnum, (unsigned)phentsize, (unsigned)phoff, n);
    }
    int loaded = 0;
    for (uint64_t i = 0; i < phnum; i++) {
        const uint8_t *ph = d + phoff + i * phentsize;
        uint32_t type = le32(ph), paddr = le32(ph + 12);
        uint64_t off = le32(ph + 4), filesz = le32(ph + 16);
        if (type != 1 /* PT_LOAD */ || !filesz) continue;
        if (off > n || filesz > n - off) {
            return fail(err, errlen, "segment %u outside the file", (unsigned)i);
        }
        if (!fb_fw_put(f, paddr, d + off, (size_t)filesz, err, errlen)) return false;
        loaded++;
    }
    if (!loaded) return fail(err, errlen, "no loadable segments");
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\0'; }

bool fb_fw_parse_hex(const fb_fw_flash_t *f, const char *txt, size_t n, char *err, size_t errlen)
{
    uint32_t base = 0;
    size_t pos = 0;
    unsigned data = 0;                              /* data records seen */
    for (int ln = 1; pos < n; ln++) {
        size_t end = pos;
        while (end < n && txt[end] != '\n') end++;
        size_t a = pos, b = end;                    /* the line without surrounding blanks */
        pos = end + 1;
        while (a < b && blank(txt[a])) a++;
        while (b > a && blank(txt[b - 1])) b--;
        size_t len = b - a;
        if (!len) continue;
        uint8_t rec[5 + 255];
        if (txt[a] != ':' || len < 11 || !(len & 1) || (len - 1) / 2 > sizeof rec) {
            return fail(err, errlen, "hex line %d: bad record", ln);
        }
        unsigned nb = (unsigned)((len - 1) / 2), sum = 0;
        for (unsigned i = 0; i < nb; i++) {
            int hi = hexval(txt[a + 1 + 2 * i]), lo = hexval(txt[a + 2 + 2 * i]);
            if (hi < 0 || lo < 0) return fail(err, errlen, "hex line %d: not hex", ln);
            rec[i] = (uint8_t)(hi << 4 | lo);
            sum += rec[i];
        }
        if ((sum & 0xFF) || rec[0] + 5u != nb) {
            return fail(err, errlen, "hex line %d: checksum/length", ln);
        }
        uint32_t off = (uint32_t)rec[1] << 8 | rec[2];
        switch (rec[3]) {
        case 0:
            if (!fb_fw_put(f, base + off, rec + 4, rec[0], err, errlen)) return false;
            data++;
            break;
        case 1:
            if (!data) return fail(err, errlen, "hex: no data records");
            return true;
        case 2:
            if (rec[0] != 2) return fail(err, errlen, "hex line %d: bad segment record", ln);
            base = ((uint32_t)rec[4] << 8 | rec[5]) << 4;
            break;
        case 4:
            if (rec[0] != 2) return fail(err, errlen, "hex line %d: bad address record", ln);
            base = ((uint32_t)rec[4] << 8 | rec[5]) << 16;
            break;
        default:
            break;          /* 03/05: start address */
        }
    }
    if (!data) return fail(err, errlen, "hex: no data records");
    return true;
}

bool fb_fw_parse(const fb_fw_flash_t *f, const uint8_t *d, size_t n, char *err, size_t errlen)
{
    if (n > FB_FW_MAX_FILE) return fail(err, errlen, "%zu bytes: larger than %u", n, FB_FW_MAX_FILE);
    if (n >= 4 && !memcmp(d, "\x7f" "ELF", 4)) return fb_fw_parse_elf(f, d, n, err, errlen);
    if (n && d[0] == ':') return fb_fw_parse_hex(f, (const char *)d, n, err, errlen);
    return fb_fw_put(f, f->base, d, n, err, errlen);
}

bool fb_fw_erased(const fb_fw_flash_t *f, uint32_t start, uint32_t end)
{
    if (start < f->base || end < start || (uint64_t)end - f->base > f->size) return false;
    for (uint32_t a = start; a < end; a++) {
        if (f->img[a - f->base] != 0xFF) return false;
    }
    return true;
}

bool fb_fw_custom_defaults_area(const fb_fw_flash_t *f, uint32_t *start, uint32_t *end, char *err, size_t errlen)
{
    if (f->size < FB_FW_CUSTOM_DEFAULTS_PTR + 8) return fail(err, errlen, "flash too small");
    uint32_t s = le32(f->img + FB_FW_CUSTOM_DEFAULTS_PTR), e = le32(f->img + FB_FW_CUSTOM_DEFAULTS_PTR + 4);
    uint64_t lo = (uint64_t)f->base + FB_FW_CUSTOM_DEFAULTS_PTR + 8, hi = (uint64_t)f->base + f->size;
    if (s < lo || e > hi || s >= e || e - s > FB_FW_CUSTOM_DEFAULTS_MAX) {
        return fail(err, errlen, "custom-defaults pointers 0x%08x..0x%08x at 0x%08x are not a valid area",
                    s, e, f->base + FB_FW_CUSTOM_DEFAULTS_PTR);
    }
    *start = s;
    *end = e;
    return true;
}
