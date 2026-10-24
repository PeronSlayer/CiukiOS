/* G14 boot status and the Automatic Skip Driver agent (CBS1, partition
 * sector 9; docs/design/startup-recovery.md section 2.1). The loader marks
 * every start "in progress"; this module blames the driver classes that
 * were activating when the previous start died, skips a class at its
 * second failure (Windows 98 ASD), records each activation before it runs
 * and marks the start complete once the desktop has presented a frame.
 * Raw sector I/O only, like the black box: no FAT, cache or allocation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/bootstat.h>
#include <ciuki/boot_info.h>
#include <ciuki/abi.h>
#include <ciuki/blackbox.h>
#include "../fs/blkdev.h"

#ifdef FS_HOST
extern struct ciuki_boot_info g_boot;
#endif
extern void klog(const char *, ...) __attribute__((weak));

_Static_assert(CIUKI_BOOT_SAFE_MODE == CBI_F_SAFE_MODE && CIUKI_BOOT_VERBOSE == CBI_F_VERBOSE &&
               CIUKI_BOOT_HW_CHECK == CBI_F_HW_CHECK && CIUKI_BOOT_FAILED == CBI_F_BOOT_FAILED &&
               CIUKI_BOOT_STEP == CBI_F_STEP && CIUKI_BOOT_CMD_ONLY == CBI_F_CMD_ONLY &&
               CIUKI_BOOT_SAFE_STORAGE == CBI_F_SAFE_STORAGE && CIUKI_BOOT_SCANDISK == CBI_F_SCANDISK &&
               CIUKI_BOOT_SCANDISK_SKIP == CBI_F_SCANDISK_SKIP && CIUKI_BOOT_MENU_SHOWN == CBI_F_MENU_SHOWN &&
               CIUKI_BOOT_SKIP_SHIFT == CBI_F_SKIP_SHIFT, "boot flags ABI mirror");
_Static_assert(CIUKI_RECOVERY_CLASS_MASK == BOOTSTAT_CLASS_MASK && CIUKI_RECOVERY_CLASSES == BOOTSTAT_CLASSES &&
               CIUKI_RECOVERY_SOUND == 1u << BOOTSTAT_SOUND && CIUKI_RECOVERY_STARTUP == 1u << BOOTSTAT_STARTUP &&
               CIUKI_RECOVERY_REQ_MENU == BOOTSTAT_REQ_MENU, "recovery classes ABI mirror");

static struct blkdev *device;
static uint64_t lba;
static struct bootstat_record rec;
static bool bound, presented, completed, dirty;
static uint8_t ran, newly;
static fs_lock guard;

static int store(void)
{
    uint8_t b[512];
    fs_lock_take(&guard);
    bootstat_encode(b, &rec);
    dirty = false;
    fs_lock_drop(&guard);
    int e = device->write ? device->write(device, lba, 1, b) : -FS_EROFS;
    if (!e && device->write_cache_state != BLKDEV_CACHE_DISABLED)
        e = device->flush ? device->flush(device) : -FS_EROFS;
    if (e) {
        fs_lock_take(&guard); dirty = true; fs_lock_drop(&guard);
        if (klog) klog("[bootstat] write error=%d", e);
    }
    return e;
}

void bootstat_reset(void)
{
    device = 0; lba = 0; bound = presented = completed = dirty = false; ran = newly = 0;
    memset(&rec, 0, sizeof(rec));
}

/* The boot partition, after the black box and before FAT. Refuses test
 * requests (probes own their disk evidence), foreign sector contents and a
 * device that cannot make the record durable. */
int bootstat_bind(struct blkdev *d, uint64_t start, uint64_t count)
{
    bootstat_reset();
    if (g_boot.flags & CBI_F_TEST_REQUEST) return -FS_EOPNOTSUPP;
    if (!d || d->sector_size != 512 || count <= BOOTSTAT_SECTOR || blkdev_range(d, start, 160)) return -FS_EINVAL;
    if (!blkdev_durable(d)) return -FS_EROFS;
    uint8_t b[512], desc[512];
    int e = d->read(d, start + BOOTSTAT_SECTOR, 1, b);
    if (e) return e;
    bool adopt = false;
    if (!bootstat_decode(b, &rec)) {
        /* Setup's formatter and earlier images leave sector 9 zero. Adopt it
         * only on a CiukiOS volume: the black-box descriptor proves it. */
        if (!bootstat_blank(b)) return -FS_ENOENT;
        if ((e = d->read(d, start + BLACKBOX_DESCRIPTOR, 1, desc))) return e;
        if (memcmp(desc, "CBB1", 4) || bootstat_rd32(desc + 8) != BLACKBOX_FIRST) return -FS_ENOENT;
        memset(&rec, 0, sizeof(rec));
        adopt = true;
    }
    fs_lock_init(&guard);
    device = d; lba = start + BOOTSTAT_SECTOR; bound = true;
    bool failed = (g_boot.flags & CBI_F_BOOT_FAILED) != 0;
    if (rec.boots != UINT32_MAX) rec.boots++;
    uint8_t blamed_before = rec.active;
    newly = bootstat_blame(&rec, failed, rec.boots);
    rec.state = 1;
    /* The scheduled check is this start's; the loader already turned the
     * request into CBI_F_SCANDISK. The Diagnostic startup request is once. */
    rec.request &= (uint8_t)~(BOOTSTAT_REQ_SCANDISK | BOOTSTAT_REQ_THOROUGH | BOOTSTAT_REQ_MENU);
    g_boot.flags |= (uint32_t)(rec.skipped | rec.disabled) << CBI_F_SKIP_SHIFT;
    if (klog) {
        klog("[bootstat] bound adopt=%u previous_failed=%u failures=%u blamed=0x%x disabled=0x%x skipped=0x%x boot=%u",
             adopt, failed, rec.failures, blamed_before, rec.disabled, rec.skipped, rec.boots);
        for (unsigned c = 0; c < BOOTSTAT_CLASSES; c++) if (newly & (1u << c))
            klog("[asd] skip class=%s failures=%u boot=%u", bootstat_class_name(c), rec.fail_count[c], rec.boots);
    }
    return store();
}

bool bootstat_bound(void) { return bound; }
bool bootstat_skip(enum bootstat_class c)
{
    return (g_boot.flags >> CBI_F_SKIP_SHIFT) & (1u << c);
}
/* Before a class activation runs: the record names it on disk. */
void bootstat_begin(enum bootstat_class c)
{
    if (!bound) return;
    fs_lock_take(&guard);
    rec.active |= (uint8_t)(1u << c);
    rec.state = 1;
    fs_lock_drop(&guard);
    (void)store();
}
static void complete_locked(void)
{
    if (completed) return;
    bootstat_complete(&rec, ran);
    completed = true;
    dirty = true;
    if (klog) klog("[bootstat] start complete boot=%u completed=%u", rec.boots, rec.completed);
}
/* Memory only, no I/O in the caller: the storage writer task (or the next
 * begin) persists it within one writer turn, so a later failure is not
 * blamed on a class that already finished. */
void bootstat_end(enum bootstat_class c)
{
    if (!bound) return;
    fs_lock_take(&guard);
    rec.active &= (uint8_t)~(1u << c);
    ran |= (uint8_t)(1u << c);
    dirty = true;
    if (completed) rec.state = rec.active ? 1 : 0;
    else if (presented && !rec.active) complete_locked();
    fs_lock_drop(&guard);
}
/* The desktop presented its first full frame (grants.c) or the hardware
 * check page is shown. Called on every frame: no I/O here. */
void bootstat_presented(void)
{
    if (!bound || presented) return;
    fs_lock_take(&guard);
    presented = true;
    if (!rec.active) complete_locked();
    fs_lock_drop(&guard);
}
/* Storage writer task (P_DEVICE): persist ends and the completion. */
int bootstat_service(void)
{
    if (!bound) return 0;
    fs_lock_take(&guard);
    bool due = dirty;
    fs_lock_drop(&guard);
    return due ? store() : 0;
}

void bootstat_status(struct ciuki_recovery_status *out)
{
    memset(out, 0, sizeof(*out));
    out->version = CIUKI_RECOVERY_VERSION;
    out->size = sizeof(*out);
    out->boot_flags = g_boot.flags;
    if (!bound) return;
    fs_lock_take(&guard);
    out->flags = CIUKI_RECOVERY_VALID | (completed ? CIUKI_RECOVERY_COMPLETE : 0);
    out->disabled = rec.disabled; out->skipped = rec.skipped; out->newly_skipped = newly;
    out->request = rec.request; out->failures = rec.failures; out->blamed = rec.blamed;
    out->active = rec.active;
    memcpy(out->fail_count, rec.fail_count, sizeof(out->fail_count));
    out->boots = rec.boots; out->completed = rec.completed;
    memcpy(out->skipped_at, rec.skipped_at, sizeof(out->skipped_at));
    fs_lock_drop(&guard);
}
/* System Configuration: selective startup and next-start requests. */
int bootstat_configure(uint8_t disabled, uint8_t request)
{
    if (!bound) return -FS_ENOENT;
    if ((disabled & ~BOOTSTAT_CLASS_MASK) || (request & ~BOOTSTAT_REQ_MASK)) return -FS_EINVAL;
    fs_lock_take(&guard);
    rec.disabled = disabled; rec.request = request;
    fs_lock_drop(&guard);
    if (klog) klog("[bootstat] configure disabled=0x%x request=0x%x", disabled, request);
    return store();
}
/* The ASD dialog's Enable: load the class again, forget its failures. */
int bootstat_enable(uint8_t classes)
{
    if (!bound) return -FS_ENOENT;
    if (classes & ~BOOTSTAT_CLASS_MASK) return -FS_EINVAL;
    fs_lock_take(&guard);
    rec.skipped &= (uint8_t)~classes;
    for (unsigned c = 0; c < BOOTSTAT_CLASSES; c++) if (classes & (1u << c)) { rec.fail_count[c] = 0; rec.skipped_at[c] = 0; }
    fs_lock_drop(&guard);
    if (klog) klog("[asd] enable classes=0x%x", classes);
    return store();
}
/* ScanDisk's Check Now on a volume in use: run at the next start. */
int bootstat_schedule_scan(bool thorough)
{
    if (!bound) return -FS_ENOENT;
    fs_lock_take(&guard);
    rec.request |= (uint8_t)(BOOTSTAT_REQ_SCANDISK | (thorough ? BOOTSTAT_REQ_THOROUGH : 0));
    fs_lock_drop(&guard);
    return store();
}
