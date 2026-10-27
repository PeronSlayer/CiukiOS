/* Register transport also used by the host engine model.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SAVAGE2D_H
#define CIUKI_SAVAGE2D_H
#include <ciuki/accel.h>
/* Private source compatibility for the frozen legacy snapshot. */
#define CIUKI_ACCEL_DRIVER_SAVAGE CIUKI_ACCEL_DRIVER_LEGACY_NATIVE
const struct accel_ops *savage2d_init(const struct fb_device *, struct vfs *);
void savage2d_record(void);
struct ciuki_accel_info;
void savage2d_snapshot(struct ciuki_accel_info *);
struct pci_func;
#define SAVAGE_MMIO_BYTES 0x80000u
#define SAVAGE_BCI 0x10000u
#define SAVAGE_STATUS 0x48c60u
#define SAVAGE_EVENT_STATUS 0x48c64u
#define SAVAGE_EARLY_STATUS 0x48c00u
#define SAVAGE_EARLY_MMIO 0x1000000u
#define SAVAGE_COB_BYTES 0x20000u
#define SAVAGE_COB_LIMIT 0x800000u /* MM48C14 offset bits 22:11 */
enum savage_chip {
    SAVAGE_UNKNOWN, SAVAGE_3D, SAVAGE_MX, SAVAGE_4, SAVAGE_PRO,
    SAVAGE_TWISTER, SAVAGE_DDR, SAVAGE_SUPER
};
enum savage_chip savage2d_chip(uint16_t device);
unsigned savage2d_framebuffer_bar(const struct pci_func *);
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
    /* Byte VGA colour-range I/O only: 0x3c0 inclusive, 0x3e0 exclusive. */
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
    /* f2-92: engine-only rows [offscreen_first, rows) beyond the visible
     * height (rows == height without an offscreen lease), service counters
     * and the display service's slice hook. */
    uint32_t rows, offscreen_first;
    uint64_t writes, write_words, pauses, fifo_waits;
    int (*pause)(void *);
    void *pause_arg;
    /* Write self-test: 0 untested, 1 passed, -1 failed (writes and offscreen
     * rows unavailable; fills and copies unaffected). */
    const struct fb_device *fb;
    int write_state;
    bool offscreen_ok;
    bool active, quarantined, cpu_safe, touched, supersavage, cursor_loaded, status_logged;
    /* submitted includes BCI register setup, before drawing is enabled. */
    bool mmio_saved, status_ready, submitted, final_idle_checked;
    bool event_pending;
    /* Never set by normal 2D activation. Only a completed guarded offscreen
     * 3D qualification may enable this shared-owner transport. */
    bool render3d_qualified;
    enum savage_chip chip;
    uint32_t status_reg, fifo_mask, cob_offset, saved_cob;
};
/* Idle presenter/drawing exclusion is required for a cache transfer. */
int savage2d_cache_transfer(void *, bool);
bool savage2d_supported(const struct pci_func *);
/* f2-92: engine-only rows [first, end) for a mode, VRAM size and cursor
 * page offset (0: none); false when nothing fits. */
bool savage2d_offscreen_span(const struct fb_device *, uint32_t vram, uint32_t cursor_offset,
                             uint32_t *first, uint32_t *end);
/* No register/file access: decide before scheduling marker persistence. */
bool savage2d_eligible(const struct fb_device *);
/* Part-B marker export boundary: BEGIN preserves eligibility/EngineConfig
 * and grants one durable activation only after keyed commit + close. END
 * clears that pinned key only after fresh BEGIN; failure annotates it without
 * truncation. Retained diagnostic markers grant neither permission. All three
 * run in worker context outside presenter/device-work locks. */
struct vfs;
int savage2d_attempt_begin(struct vfs *);
int savage2d_attempt_end(struct vfs *);
int savage2d_attempt_failed(struct vfs *);
int savage2d_check_begin(struct vfs *, bool *retained_marker);
void savage2d_quarantine(const char *reason);
/* f1-97: the activation decision (false before it exists); the reason is the
 * quarantine token, "none" when accelerated. f2-92: used is true once the
 * engine has executed a drawing request of a presenter (not the self-test). */
bool savage2d_summary(bool *accelerated, bool *used, const char **reason);
void savage2d_summary_safe_mode(void);
/* Final idle proof once; return a rejecting guard only for an unsafe writer. */
const struct accel_ops *savage2d_fallback_ops(void);
int savage2d_start(struct savage2d *, const struct savage_io *, const struct fb_device *,
                   bool supersavage, uint8_t *cursor, uint32_t cursor_offset);
/* Internal packet transport, caller holds presenter then engine ownership.
 * Uses this exact 2D owner/status/quarantine; creates no mapping or reset. */
int savage2d_render3d_packet(struct savage2d *, const uint32_t *, unsigned);
int savage2d_render3d_batch(struct savage2d *, const uint32_t *, unsigned);
/* Early chips require a disjoint driver-owned 128 KiB VRAM COB. Zero means
 * unavailable. No register access on invalid family/geometry/reservation. */
int savage2d_start_chip(struct savage2d *, const struct savage_io *, const struct fb_device *,
    enum savage_chip, uint8_t *cursor, uint32_t cursor_offset, uint32_t cob_offset);
bool savage2d_cob_span(const struct fb_device *, uint32_t vram, uint32_t *offset);
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
