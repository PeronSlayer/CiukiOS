/* Register transport also used by the host engine model.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SAVAGE2D_H
#define CIUKI_SAVAGE2D_H
#include <ciuki/accel.h>
struct pci_func;
#define SAVAGE_MMIO_BYTES 0x80000u
#define SAVAGE_BCI 0x10000u
#define SAVAGE_STATUS 0x48c60u
#define SAVAGE_EVENT_STATUS 0x48c64u
struct savage_io {
    void *ctx;
    uint32_t (*read32)(void *, uint32_t);
    void (*write32)(void *, uint32_t, uint32_t);
    uint64_t (*now_us)(void *);
    int (*delay_us)(void *, uint32_t);
    void (*drain)(void *);
    void (*fault)(void *);
    /* Optional MMIO VGA alias diagnostic, never the operational transport. */
    uint8_t (*read8)(void *, uint32_t);
    void (*write8)(void *, uint32_t, uint8_t);
    /* Byte VGA I/O only: inclusive 0x3b0, exclusive 0x3e0. */
    uint8_t (*read_port)(void *, uint16_t);
    void (*write_port)(void *, uint16_t, uint8_t);
    /* RE046-B section 7: 2D readback supports halfword bus cycles only. */
    uint16_t (*read16)(void *, uint32_t);
    void (*write16)(void *, uint32_t, uint16_t);
};
struct savage2d {
    struct savage_io io;
    struct accel_ops ops;
    uint32_t width, height, descriptor, offset, cursor_offset;
    uint8_t *cursor;
    uint8_t saved40, saved31, saved50, saved45, saved55;
    uint32_t saved_bci, saved_gbd[2];
    uint32_t idle_mask, idle_value, status;
    uint32_t event_before, event_status;
    uint16_t event_tag;
    const char *reason;
    uint64_t operations, idle_waits, fallbacks, timeouts;
    uint64_t reset_us, poll_us;
    bool active, quarantined, cpu_safe, touched, supersavage, cursor_loaded, status_logged;
    /* submitted includes BCI register setup, before drawing is enabled. */
    bool mmio_saved, status_ready, submitted, final_idle_checked;
    bool event_pending;
};
bool savage2d_supported(const struct pci_func *);
/* No register/file access: decide before scheduling marker persistence. */
bool savage2d_eligible(const struct fb_device *);
/* Attempt marker file I/O for kernel callers, outside presenter locks. */
struct vfs;
int savage2d_attempt_begin(struct vfs *);
int savage2d_attempt_end(struct vfs *);
int savage2d_attempt_failed(struct vfs *);
int savage2d_check_begin(struct vfs *, bool *retained_marker);
void savage2d_quarantine(const char *reason);
/* Final idle proof once; return a rejecting guard only for an unsafe writer. */
const struct accel_ops *savage2d_fallback_ops(void);
int savage2d_start(struct savage2d *, const struct savage_io *, const struct fb_device *,
                   bool supersavage, uint8_t *cursor, uint32_t cursor_offset);
#define SAVAGE_CONFIGS 96u
#define SAVAGE_SWEEP_BYTES (256u * 1024u)
struct savage_config { uint32_t descriptor; uint8_t depth; };
bool savage2d_config(unsigned, const struct fb_device *, struct savage_config *);
struct savage_sweep_result {
    unsigned tried, passed, skipped, timeouts, completed;
    int first, error;
    bool restored;
};
typedef void (*savage_sweep_line)(void *, const char *);
typedef int (*savage_sweep_checkpoint)(void *);
int savage2d_sweep_model(struct savage2d *, const struct savage_io *, const struct fb_device *,
    bool supersavage, uint8_t *scratch, uint32_t offset, uint64_t deadline_us,
    savage_sweep_line, savage_sweep_checkpoint, void *, struct savage_sweep_result *);
int savage2d_hardware_sweep(uint64_t deadline_ms, savage_sweep_line, void *, struct savage_sweep_result *);
#endif
