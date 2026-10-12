/* SPDX-License-Identifier: MIT
 * f1-23 / f2-37 shared SDK contract, kernel call 74. */
#ifndef CIUKI_SDK_DISPLAY_H
#define CIUKI_SDK_DISPLAY_H
#include <stdint.h>
#define CIUKI_DISPLAY_VERSION 1
struct ciuki_display_mode { uint16_t id, width, height, bpp; uint16_t pitch; uint8_t linear, current; };
struct ciuki_display_info {
    uint32_t version, size;
    uint16_t width, height, bpp, pitch;
    uint32_t vram_bytes, vbe_version;
    char oem[64], vendor[64], product[64];
    uint16_t pci_vendor, pci_device, pci_class, pci_subclass;
    uint32_t mode_count;
    struct ciuki_display_mode modes[32];
};
int ciuki_display_query(struct ciuki_display_info *);
int ciuki_display_set(uint16_t mode_id);
#endif
