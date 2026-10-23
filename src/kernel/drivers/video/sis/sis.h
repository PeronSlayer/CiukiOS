/* Native private SiS transport; no userspace ABI.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VIDEO_SIS_H
#define CIUKI_VIDEO_SIS_H
#include <ciuki/display_driver.h>
#define SIS_MMIO_BYTES 0x10000u
#define SIS_QUEUE_BYTES (512u * 1024u)
#define SIS_CURSOR_BYTES 4096u
#define SIS_POLL_LIMIT 100000u
#define SIS_WAIT_US 20000u
enum sis_generation { SIS_300, SIS_315 };
struct sis_io {
    void *ctx;
    uint32_t (*read)(void *, unsigned, unsigned);
    void (*write)(void *, unsigned, unsigned, uint32_t);
    uint8_t (*indexed_read)(void *, bool, uint8_t);
    void (*indexed_write)(void *, bool, uint8_t, uint8_t);
    uint64_t (*now)(void *);
    void (*drain)(void *);
};
struct sis_engine {
    struct sis_io io;
    struct fb_device fb;
    enum sis_generation generation;
    bool active, quarantined, cpu_safe, touched, cursor_loaded, cursor_enabled;
    bool cursor_ok, configured, queue_changed;
    uint8_t saved05, saved1e, saved26, saved27;
    uint32_t saved_queue, cursor_offset, queue_offset, cursor_regs[5];
    uint8_t *cursor;
    const char *reason;
    uint64_t operations, waits, timeouts;
    int (*pause)(void *);
    void *pause_arg;
};
extern const struct display_driver sis300_driver, sis315_driver;
bool sis_mode_supported(const struct fb_device *);
bool sis_pci_supported(const struct pci_func *, enum sis_generation);
int sis_engine_start(struct sis_engine *);
int sis_sync(void *);
int sis_fill(void *, const struct fb_rect *, uint32_t);
int sis_copy(void *, const struct fb_rect *, int32_t, int32_t);
/* Private future part C hook: RGB bits only, reserved bits preserved. */
int sis_invert(void *, const struct fb_rect *);
int sis_pattern(void *, const struct fb_rect *, const uint8_t[8], uint32_t, uint32_t);
int sis_cursor_set(void *, const struct accel_cursor *);
int sis_cursor_move(void *, int32_t, int32_t);
int sis_cursor_show(void *, bool);
bool sis_cursor_hide(struct sis_engine *);
int sis_stop(void *);
void sis_quarantine(struct sis_engine *, const char *);
unsigned sis_capabilities(void *);
bool sis_cpu_ready(void *);
void sis_pause(void *, int (*)(void *), void *);
#endif
