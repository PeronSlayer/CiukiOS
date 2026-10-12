/* SPDX-License-Identifier: MIT
 * Synthetic f1-23 data for host tests and review renders, never runtime data. */
#ifndef CIUKI_HOST_DISPLAY_FIXTURE_H
#define CIUKI_HOST_DISPLAY_FIXTURE_H
#include <ciuki/display.h>
#include <string.h>
static inline void display_fixture(struct ciuki_display_info *d)
{
    *d = (struct ciuki_display_info){.version = CIUKI_DISPLAY_VERSION, .size = sizeof(*d),
        .width = 1024, .height = 768, .bpp = 32, .pitch = 4096,
        .vram_bytes = 16 * 1024 * 1024, .vbe_version = 0x0200,
        .pci_vendor = 0x5333, .pci_device = 0x8c2e, .pci_class = 3, .mode_count = 9,
        .modes = {{0x118, 1024, 768, 32, 4096, 1, 1},
                  {0x111, 640, 480, 16, 1280, 1, 0}, {0x114, 800, 600, 24, 2400, 1, 0},
                  {0x117, 1024, 768, 16, 2048, 1, 0}, {0x119, 1280, 1024, 32, 5120, 1, 0},
                  {0x120, 640, 480, 32, 2560, 0, 0}, /* banked: excluded */
                  {0x121, 800, 600, 8, 800, 1, 0},   /* unsupported depth */
                  {0x122, 800, 600, 32, 1, 1, 0},    /* bad pitch */
                  {0x123, 1024, 768, 32, 4096, 1, 0}}}; /* duplicate geometry/depth */
    strcpy(d->oem, "S3 Incorporated. SuperSavage BIOS");
    strcpy(d->vendor, "S3 Graphics"); strcpy(d->product, "SuperSavage IX/C");
}
#endif
