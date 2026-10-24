/* Prints the device catalog (fb/device.h) as the JSON array that goes into the core manifest's
 * "devices" (core/qemu/scripts/manifest.py adds "implemented" per machine).
 *   make tools && build/fb_manifest */
#include "fb/device.h"

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    size_t n = fb_device_catalog_json(NULL, 0) + 1;
    char *buf = malloc(n);
    if (!buf) return 1;
    fb_device_catalog_json(buf, n);
    fputs(buf, stdout);
    free(buf);
    return 0;
}
