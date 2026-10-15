/* Native mobile scanout state, independent of the firmware VM and BCI.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SUPERSAVAGE_MODE_H
#define CIUKI_SUPERSAVAGE_MODE_H
#include "../vm/vga_hardware.h"
#define SUPERSAVAGE_STREAM_REGS 40u
struct supersavage_mode_io {
    struct vga_hardware_bus vga;
    uint32_t (*read32)(void *, unsigned);
    void (*write32)(void *, unsigned, uint32_t);
    int (*delay_us)(void *, unsigned);
    uint64_t (*now_us)(void *);
    int (*checkpoint)(void *, const char *, int);
};
struct supersavage_mode {
    cvga_state vga;
    uint8_t cr[256], sr[256];
    uint32_t streams[SUPERSAVAGE_STREAM_REGS];
    uint8_t lock38, lock39, lock08, iga, misc, enable, cr_index, sr_index;
    bool saved, planes_saved, touched;
    const char *stage;
    unsigned bad_reg;
    uint32_t expected, actual, mask;
    uint64_t restore_us;
};
bool supersavage_mode_supported(uint16_t);
int supersavage_mode_save(struct supersavage_mode *, const struct supersavage_mode_io *);
int supersavage_mode_enter(struct supersavage_mode *, const struct supersavage_mode_io *, uint16_t);
/* Recovery keeps going after readback/checkpoint errors and retains the
 * snapshot on failure. Caller keeps/quarantines ownership until verified. */
int supersavage_mode_restore(struct supersavage_mode *, const struct supersavage_mode_io *);
#endif
