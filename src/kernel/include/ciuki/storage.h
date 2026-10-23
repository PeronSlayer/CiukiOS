/* F1 mount orchestration, also compiled by the host integration suite.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_STORAGE_H
#define CIUKI_STORAGE_H
#include "blkpart.h"
#include "../../fs/vfs.h"
#include "../../fs/cdview.h"

#define STORAGE_DISKS 16u
#define STORAGE_NTFS 8u
struct ata_device;
struct optical_binding {
    struct ata_device *device;
    uint64_t next_poll;
    uint32_t attempted_generation, observed_generation;
    int result;
};
struct storage_volume {
    struct blkpart part;
    struct blkdev io;                 /* counted forwarding view, never a snapshot copy */
    struct fat_volume fat;
    unsigned disk, partition, drive;
    int error;
    bool present, read_gate, removable, safely_removed;
    bool optical; /* f9-01: Setup C: over the boot CD's image view; disk = ATA slot */
    uint32_t read_sequence, write_sequence;
    uint64_t reads, writes, flushes, refused, writes_before_gate;
    /* Selected crash probe only; notification after synchronous completion.
     * No callback in ordinary boot, no controller fault/recovery backdoor. */
    void (*trace)(struct storage_volume *, char action, uint64_t lba, int result);
};
/* f4-04: an NTFS partition (MBR type 07h) mounted read-only through fs_ro.
 * io is a counted forwarding view of the partition: reads go to the disk;
 * write and flush are refused here and counted, so writes/flushes stay zero
 * unless a driver bypasses the cache. The view is never durable. */
struct storage_ntfs {
    struct blkpart part;
    struct blkdev io;
    struct fs_ro_volume ro;
    unsigned disk, partition, drive;  /* drive: letter index of the last mount */
    int error;
    bool present;
    uint64_t reads, writes, flushes;
};
/* f9-02: raw whole-disk access and the destination lease (disklease.c,
 * cd-setup-plan.md 5.3). Protected by the VFS namespace lock. */
#define STORAGE_LEASE_PHASE 32u
#define STORAGE_LEASE_REASON 24u
struct storage_lease {
    uint32_t holder, state, generation; /* state: CIUKI_DISK_LEASE_* */
    uint32_t drive, reasons;            /* published letter index and its mount reasons */
    uint64_t reads, writes, flushes;    /* raw node sectors and flushes */
    uint32_t io_active;                 /* raw requests in flight (holder threads) */
    char phase[STORAGE_LEASE_PHASE], reason[STORAGE_LEASE_REASON];
};
struct storage_disk {
    struct blkdev *dev;                 /* the slot's ATA device, set by storage_add_disk */
    char model[41], serial[21], firmware[9];
    bool lba48;
    struct storage_lease lease;
};
struct storage_caller { uint32_t pid; bool setup_capability; };
struct storage {
    struct block_cache cache;
    struct vfs vfs;
    struct storage_volume volumes[26];
    struct fs_ro_volume optical[STORAGE_DISKS];
    struct optical_binding optical_bindings[STORAGE_DISKS];
    struct storage_ntfs ntfs[STORAGE_NTFS];
    int disk_errors[STORAGE_DISKS];
    bool disks[STORAGE_DISKS], ready, stopped;
    bool writer_running, writer_quiescing; /* protected by vfs namespace */
    unsigned next_drive;
    uint32_t sequence, writer_ticks;
    int writer_error;
    /* f9-01 Setup mode (cd-setup-plan.md 3.2/3.3): every ATA volume stays
     * read-only; C: is the boot CD's image view, D: its ISO 9660 tree. */
    bool setup_mode;
    unsigned boot_cd_slot;
    struct cdview boot_cd;
    struct storage_disk raw[STORAGE_DISKS];
};
/* f9-01: what storage_init activates; pure, so the host suite checks it. */
struct storage_start_policy {
    bool writer, enable_write, bootlog, pagefile, optical_poller, disk0_system;
};
static inline struct storage_start_policy storage_start_policy(bool probe, bool setup)
{
    /* Probes start read-only and open gates themselves; Setup mode takes
     * the same read-only start (D2) and keeps the optical media poller. */
    bool normal = !probe && !setup;
    return (struct storage_start_policy){ .writer = normal, .enable_write = normal, .bootlog = normal,
        .pagefile = normal, .optical_poller = !probe, .disk0_system = !setup };
}
/* Setup mode: identify the boot CD on one optical device (session = last
 * session LBA) and bind C:/D:. Returns 2 (C:) or a negative FS_E*. */
int storage_setup_identify(struct storage *, unsigned slot, struct blkdev *optical, uint64_t session,
                           const char *options, size_t options_size, const struct cdview_identity *);
/* Setup mode after identification: ATA volumes take letters from E:. */
void storage_setup_finish(struct storage *);
bool storage_setup_media_ready(void);
/* Kernel Setup-mode steps of storage_init (core/setup_mode.c): whether
 * setup=1 selected Setup mode; find and bind the boot CD before the ATA
 * disks; log the per-disk write counters. */
bool storage_setup_requested(void);
void storage_setup_media(struct storage *, struct ata_device *devices[STORAGE_DISKS]);
void storage_setup_records(struct ata_device *devices[STORAGE_DISKS]);
int storage_setup(struct storage *, size_t cache_bytes);
/* disk is the IDE slot, independent of discovery order. Only MBR primary
 * entry 1 on slot 0 may become C:, all other FAT partitions start at D:. */
int storage_add_disk(struct storage *, unsigned disk, struct blkdev *);
/* USB records/views are retained for the boot. Letters are E: onward and
 * never reused. Dirty removable media stay read-only; no recovery writes. */
int storage_add_removable(struct storage *, struct blkdev *);
/* Whole floppy media only, with a native drive lease held through detach. */
int storage_add_floppy(struct storage *, struct blkdev *, unsigned drive);
int storage_add_removable_bounded(struct storage *, struct blkdev *,
                                  bool (*cancel)(void *), void (*pause)(void *), void *);
int storage_remove_removable(struct storage *, struct blkdev *, bool surprise);
int storage_remove_removable_locked(struct storage *, struct blkdev *, bool surprise);
int storage_add_ro(struct storage *, unsigned slot, struct blkdev *, uint64_t session);
int storage_add_ro_locked(struct storage *, unsigned slot, struct blkdev *, uint64_t session);
void storage_refresh_ro(struct storage *);
int optical_bind(struct storage *, unsigned slot, struct ata_device *);
void optical_service(struct storage *, uint64_t now_ms);
void optical_start_service(struct storage *);
int optical_eject(struct storage *, unsigned drive, uint32_t generation);
/* NTFS slot lifecycle. Mount returns the drive letter index (>= 3) or a
 * negative FS_E*; the refusal reason is ntfs[slot].ro.diagnostic. Unmount
 * refuses (EBUSY) while files are open and issues no media write. */
int storage_ntfs_mount(struct storage *, unsigned slot);
int storage_ntfs_unmount(struct storage *, unsigned slot);
struct storage_volume *storage_volume(struct storage *, unsigned drive);
int storage_enable_write(struct storage *, unsigned drive);
/* fx-17: after a removable device's volumes are published read-only, make
 * each writable one read-write (Windows 98 model, no ownership scan).
 * Returns the number of volumes made writable; records each outcome. */
int storage_activate_removable(struct storage *, struct blkdev *);
int storage_writeback(struct storage *, uint32_t now_ms);
int storage_shutdown(struct storage *); /* no clean mark following any error */
void storage_destroy(struct storage *); /* quiescent host tests / failed init only */
struct storage *storage_get(void);
/* Retained state only, no device access, allocation or storage lock. */
bool storage_readonly_info(unsigned drive, char *text, unsigned bytes);
void storage_init(void);
int storage_sync(void);
/* Suspend flush: caller-scoped deadline in microseconds, propagated into
 * VFS/cache/controller waits. Keeps mounts and dirty flags for resume.
 * This synchronous worker entry retires in-flight references before return. */
int storage_sync_until(struct storage *, const struct vfs_request *);
/* Bounded, nonblocking system-volume adapter: absolute PIT milliseconds.
 * -EAGAIN while pending, -VFS_ETIMEOUT on expiry. Retains the worker/context
 * until transport cleanup completes; expiry never frees uncertain buffers. */
int storage_power_init(void);
int storage_sync_bounded(uint64_t deadline_ms);
/* f0-49: bind the pager's backing file on a writable fixed FAT volume. */
struct pagefile;
int storage_pagefile_bind(struct storage *, unsigned drive, const char *path);
struct pagefile *storage_pagefile(void);

/* f9-02 raw nodes /dev/ata0..3 (disklease.c). Results are 0, a byte count
 * or a negative ABI errno. Authority is checked on every mutating call:
 * the caller's Setup capability (EPERM) and the lease it holds (EACCES).
 * No call takes a path in ordinary boots: they serve the device nodes only. */
int storage_disk_open(struct storage *, unsigned disk);
int storage_disk_io(struct storage *, unsigned disk, const struct storage_caller *, bool write,
                    uint64_t offset, void *buffer, uint32_t bytes);
int storage_disk_flush(struct storage *, unsigned disk, const struct storage_caller *);
/* DISK_* requests on a record already copied into kernel memory. */
int storage_disk_control(struct storage *, unsigned disk, const struct storage_caller *,
                         uint32_t op, void *record, uint32_t size);
void storage_disk_exit(struct storage *, uint32_t pid);      /* process death: revoke */
bool storage_disk_write_denied(struct storage *, unsigned drive, uint32_t pid); /* namespace lock held */
uint64_t storage_disk_writes(struct storage *, unsigned disk); /* raw + volume sectors written */
/* mount.c helpers for disklease.c; namespace lock held, drive reserved. */
bool storage_volume_pagefile(const struct storage *, const struct fat_volume *);
int storage_publish_locked(struct storage *, unsigned disk, unsigned drive,
                           const struct partition *, uint32_t *reasons);
int storage_writer_start(struct storage *);

/* Production probe helpers: no hardware or records in these decisions. */
bool storage_probe_readonly(const char *selector, unsigned length);
int storage_file_digest(struct vfs_table *, const char *, uint8_t digest[32], uint32_t *size);
int storage_write_workload(struct storage *, struct vfs_table *, bool *reboot);
int storage_cache_fault(struct block_cache *, struct blkdev *, void (*inject)(void *, bool), void *, bool flush);
#endif
