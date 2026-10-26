/* Universal synchronous display adapters. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DISPLAY_DRIVER_H
#define CIUKI_DISPLAY_DRIVER_H
#include <ciuki/accel.h>
#include <ciuki/abi.h>
#include <ciuki/registry.h>

struct display_match { uint16_t vendor, device; uint8_t class_code, class_mask; };
/* Kernel-only export, valid until provider end; no user ABI or RAM ownership.
 * cache_flags contains PTE_PCD/PTE_PWT only, matching the retained alias. */
struct display_lfb {
    uint32_t physical, bytes, pitch;
    uint16_t width, height;
    uint8_t bpp;
    uint32_t cache_flags;
    volatile uint8_t *mapped;
};
/* display.c owns the transaction, without holding the presenter mutex.
 * supported/prepare are software preflight before grants/2D state change.
 * begin/write/end run with the mode lease pinned and the 2D engine stopped;
 * complete follows desktop commit/mediated recovery, before lease release. */
struct display_mode_provider {
    bool (*supported)(const struct fb_device *, uint16_t);
    int (*prepare)(const struct fb_device *, uint16_t);
    int (*begin)(uint16_t);
    int (*write)(uint32_t, const void *, uint32_t);
    int (*end)(void);
    void (*complete)(bool);
    int (*lfb)(struct display_lfb *);
};
struct display_selftest_result {
    unsigned tried, passed, skipped, timeouts, completed;
    int first, error;
    bool restored;
};
#define DISPLAY_ATTEMPT_FILES 2
struct render3d_family;
struct display_driver {
    const char *id, *name;
    const struct display_match *matches;
    unsigned match_count, framebuffer_bar;
    /* Read-only, outside presenter: no resource claims, commands or file I/O.
     * probe nominates; eligible retains all driver qualification restrictions. */
    bool (*probe)(const struct pci_func *);
    bool (*eligible)(const struct fb_device *);
    /* Outside presenter. All activation pixel probes MUST use fbdev_accel_probe.
     * NULL means failure; fallback MUST retain an unsafe rejecting guard.
     * No other adapter is attempted after start. */
    const struct accel_ops *(*start)(const struct fb_device *, struct vfs *);
    /* Under presenter + drawing claim during a failed pixel probe, or outside
     * presenter immediately after failed start. Bounded idle proof only;
     * no VFS/ATA. The returned guard's cpu_ready is cached and nonwaiting. */
    const struct accel_ops *(*fallback)(void);
    /* Worker context outside presenter: VFS/ATA and deferred logging allowed. */
    int (*attempt_begin)(struct vfs *);
    int (*attempt_end)(struct vfs *);
    int (*attempt_failed)(struct vfs *);
    void (*record)(void);
    /* Notification only, outside presenter, after a completed desktop frame. */
    void (*desktop_presented)(uint32_t generation);
    /* Presenter held: no VFS/ATA, summary/snapshot are cached, nonwaiting.
     * quarantine disables submissions and offscreen spans; never proves idle.
     * ops->stop is the sole stop authority; retain ops if it fails. */
    void (*quarantine)(const char *);
    /* Cached, nonwaiting summary also admits the UP inventory reader. */
    bool (*summary)(bool *, bool *, const char **);
    void (*snapshot)(struct ciuki_accel_info *);
    /* Provider binding is independent of 2D policy/activation. Immutable
     * provider pointers are pinned by display.c for the whole transaction. */
    const struct display_mode_provider *modes;
    bool (*mode_adapter)(const struct pci_func *);
    /* Diagnostic worker, outside presenter. Own marker and disjoint target;
     * bounded deadline, driver preserves same-boot quarantine/retained marker. */
    int (*check_begin)(struct vfs *, bool *);
    int (*selftest)(uint64_t, void (*)(void *, const char *), void *, struct display_selftest_result *);
    /* Fresh H-page activation only, presenter + exclusive drawing held.
     * Evidence comes from driver register checks, not LFB comparisons.
     * Leaves the cursor hidden, including failure/cancellation cleanup. */
    int (*cursor_test)(const struct accel_ops *, uint64_t, bool (*)(void *), void *);
    /* Optional read-only per-device BAR selection. The registry still proves
     * the full active framebuffer lease and extent; this hook grants nothing. */
    unsigned (*framebuffer_bar_for)(const struct pci_func *);
    /* Declarative (DM8): every durable attempt file the driver itself reads
     * at attempt_begin, besides the registry keyed marker, as literal
     * C:/system/settings paths; one "%04x" expands to the PCI device id.
     * The driver keeps its own I/O. The registry only stats these for its
     * retained-attempt status and truncates them for an explicit retry. */
    const char *attempt_files[DISPLAY_ATTEMPT_FILES];
    /* Immutable kernel-only 3D nomination. NULL means software-only.
     * Potential features never authorize hardware without boot qualification. */
    const struct render3d_family *render3d;
};
#define DISPLAY_CHECK_CURSOR (1u << 4)
/* Selection is read-only, before any activation. Ambiguous PCI ownership
 * chooses software. Tables may be supplied by integrated host fixtures. */
const struct display_driver *display_driver_select(const struct fb_device *,
    const struct display_driver *const *, unsigned, const struct pci_func **);
const struct display_driver *display_driver_bind(const struct fb_device *);
const struct display_driver *display_driver_bound(void);
const struct pci_func *display_driver_owner(void);
const struct display_mode_provider *display_driver_modes(const struct fb_device *);
/* No provider: success with NULL. A native-managed adapter with unproved
 * ownership fails closed, rather than selecting firmware implicitly. */
int display_driver_mode_bind(const struct fb_device *, const struct display_mode_provider **);
bool display_accel_disabled(const char **reason);
void display_driver_software(const char *reason);
void display_driver_record(void);
void display_driver_desktop_presented(uint32_t generation);
/* Refresh only under presenter, after deferred initialization completes.
 * info reads this cache and never calls a driver snapshot without its lock. */
bool display_driver_refresh(void);
void display_driver_info(struct ciuki_display_engine_info *);
void display_driver_count(unsigned operation, bool native);
void display_driver_command_count(unsigned opcode, bool native);
void display_driver_command_counters(struct ciuki_command_info *);
bool display_driver_summary(bool *, bool *, const char **);
/* Durable keyed storage, worker context only (no presenter/device-work lock).
 * id is 1..15 lowercase ASCII letters/digits/underscore/hyphen; PCI identity
 * is copied by the adapter at BEGIN and pinned through END/failure. BEGIN
 * sets previous for any nonempty record, including a migrated legacy one.
 * legacy_path is private adapter data, NULL for new drivers. Migration copies
 * and commits the old bytes before truncating the legacy file. Errors never
 * authorize hardware. clear/diagnostic require a successful fresh BEGIN;
 * adapters must never clear or annotate a retained diagnostic attempt. */
int display_attempt_marker(struct vfs *, const char *id, uint16_t vendor,
    uint16_t device, const char *legacy_path, bool clear, bool *previous,
    const char *diagnostic);
/* Registry status before every activation attempt (DM8), worker context only.
 * The attempt files of a bound driver are its keyed marker plus its declared
 * attempt_files. A user retry request (DISPLAY_RETRY_PATH, exactly
 * "driver=<id>\npci=<vvvv>:<dddd>\n", lowercase hex) keyed to this driver
 * and PCI function first truncates every nonempty attempt file, then is
 * consumed; a request for another key is consumed without clearing anything.
 * *retained then reports any nonempty attempt file. Errors fail closed. */
#define DISPLAY_ATTEMPT_RETAINED "attempt_retained"
#define DISPLAY_RETRY_PATH "C:/system/settings/accel-retry.cfg"
int display_attempt_prepare(struct vfs *, const struct display_driver *,
    const struct pci_func *, bool *retained);
#endif
