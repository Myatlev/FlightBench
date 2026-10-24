/* FlightBench: firmware files -> an MCU flash image, emulator-independent (the QEMU machine's
 * `fw=` option and the control server's `firmware` command use it; tests/test_fw_image.c feeds
 * it broken files).
 *
 *   fb_fw_parse()        sniffs the format: ELF (PT_LOAD segments at their physical address),
 *                        Intel HEX (':' records, types 00/01/02/04), else a raw binary at the base;
 *                        files above FB_FW_MAX_FILE are refused
 *   fb_fw_parse_elf/hex  the two parsers: every length and offset from the file is checked in
 *                        64-bit arithmetic before it is used, nothing is read or written outside
 *                        the file and the image
 *   fb_fw_custom_defaults_area()  the unified-target custom-defaults area: two little-endian
 *                        pointers (start, end) at base + 0x2800 of the image (tools/mkdefaults.py),
 *                        validated: inside the flash, after the pointers, start < end, <= 64 KiB
 *   fb_fw_erased()       start..end (absolute addresses) is inside the flash and all 0xFF: the
 *                        custom-defaults blob only goes where the firmware image left erased flash
 *   A HEX file without data records is refused (nothing to flash).
 *
 * The image is `size` bytes for the flash at `base`; the caller fills it with 0xFF (erased)
 * first. Addresses in the alias window (alias_base .. alias_base + size; 0 = no alias) are the
 * same flash. Errors: a NUL-terminated message in err (may be NULL), return false. */
#ifndef FB_FW_IMAGE_H
#define FB_FW_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *img;
    uint32_t base;          /* 0x08000000 on the STM32F7 */
    uint32_t size;          /* 512 KiB on the F722 */
    uint32_t alias_base;    /* 0x00200000 (flash on the ITCM bus), 0 = none */
} fb_fw_flash_t;

/* the largest firmware file accepted (a HEX of a full 512K flash is ~1.4 MiB) */
#define FB_FW_MAX_FILE            (2u * 1024 * 1024)
#define FB_FW_CUSTOM_DEFAULTS_PTR 0x2800u   /* offset in the flash of the two pointers */
#define FB_FW_CUSTOM_DEFAULTS_MAX (64u * 1024)

bool fb_fw_parse(const fb_fw_flash_t *f, const uint8_t *data, size_t n, char *err, size_t errlen);
bool fb_fw_parse_elf(const fb_fw_flash_t *f, const uint8_t *data, size_t n, char *err, size_t errlen);
bool fb_fw_parse_hex(const fb_fw_flash_t *f, const char *text, size_t n, char *err, size_t errlen);
bool fb_fw_put(const fb_fw_flash_t *f, uint32_t addr, const uint8_t *data, size_t n,
               char *err, size_t errlen);
bool fb_fw_custom_defaults_area(const fb_fw_flash_t *f, uint32_t *start, uint32_t *end,
                                char *err, size_t errlen);
bool fb_fw_erased(const fb_fw_flash_t *f, uint32_t start, uint32_t end);

#ifdef __cplusplus
}
#endif
#endif
