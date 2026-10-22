/* libdisk: the CiukiOS destination-disk library (directive f9-02, contract
 * docs/design/cd-setup-plan.md sections 5 and 6). It enumerates ATA disks
 * through the raw nodes /dev/ata0../dev/ata3, plans the only Beta layout
 * (whole disk: canonical MBR, loader gap, FAT32 at LBA 2048 with the
 * geometry of scripts/build_image.py), and executes the raw phases P1, P2
 * and P5 under the kernel's exclusive destination lease. It writes nothing
 * outside disk_execute_p1/p2/p5. Not thread-safe: one Setup session.
 *
 * Every function returns 0 or a negative DISK_E* code; -code is the
 * setup_error_class of cd-setup-plan.md 6.4 (setup_txn.h), so libsetup maps
 * a failure with cls = -code. disk_last_error() adds the errno, the LBA
 * range and the step. Bad-sector policy (D7): any read, write or read-back
 * error on a planned or verified sector refuses the disk (DISK_EIO with the
 * LBA range); there is no bad-cluster marking and no surface scan.
 * SPDX-License-Identifier: MIT */
#ifndef LIBDISK_H
#define LIBDISK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum disk_error {
    DISK_OK = 0,
    DISK_EIO = -4,          /* SETUP_ERR_DEST_WRITE: read/write/read-back error or mismatch */
    DISK_EFLUSH = -5,       /* SETUP_ERR_DEST_FLUSH: the disk did not confirm a flush */
    DISK_EFULL = -6,        /* SETUP_ERR_DEST_FULL: the payload does not fit (plan still filled) */
    DISK_ECHANGED = -7,     /* SETUP_ERR_DEST_CHANGED: identity, capacity or table changed */
    DISK_ELEASE = -8,       /* SETUP_ERR_LEASE: capability missing, lease refused or lost */
    DISK_ECONSISTENCY = -9, /* SETUP_ERR_CONSISTENCY: FAT_RO_* reason, publish refused */
    DISK_ECANCELLED = -10,  /* SETUP_ERR_CANCELLED: cancel observed between ranges */
    DISK_ENOMEM = -11,      /* SETUP_ERR_MEMORY */
    DISK_EINTERNAL = -12    /* SETUP_ERR_INTERNAL: bad argument, refused disk, no session */
};

#define DISK_SECTOR 512u
#define DISK_SLOTS 4u
#define DISK_PART_LBA 2048u          /* the only start ciukldr accepts (disk.inc) */
#define DISK_LOADER_LBA 1u
#define DISK_LOADER_SECTORS 1023u    /* ciukldr lives in LBA 1..1023 */
#define DISK_RESERVED 160u           /* build_image.py FAT_RESERVED */
#define DISK_SPC 8u                  /* 4 KiB clusters, pinned by the loader */
#define DISK_FATS 2u
#define DISK_BLACKBOX_DESCRIPTOR 8u
#define DISK_BLACKBOX_FIRST 32u
#define DISK_BLACKBOX_SECTORS 128u
#define DISK_MIN_SECTORS 2097152ull  /* 1 GiB: the UI's minimum (plan 5.1) */
#define DISK_LBA28 (1ull << 28)      /* partition.c refuses partitions ending above */
#define DISK_MIN_CLUSTERS 65529u     /* mkfs.fat 4.2 MIN_CLUST_32; the loader needs 65525 */
#define DISK_IO_SECTORS 128u         /* one node request: 64 KiB */
#define DISK_TABLE_MAX 64u
#define DISK_VOLUMES_MAX 8u
#define DISK_RANGES_MAX 24u

enum disk_refusal {
    DISK_INSTALLABLE = 0,
    DISK_REFUSE_ABSENT,       /* no ATA disk in the slot (or ATAPI) */
    DISK_REFUSE_NOT_DISK0,    /* Beta: only ATA disk 0 (primary master) */
    DISK_REFUSE_SECTOR_SIZE,  /* not 512-byte sectors */
    DISK_REFUSE_TOO_SMALL,    /* below DISK_MIN_SECTORS */
    DISK_REFUSE_GPT,          /* protective 0xEE entry (non-MBR scheme) */
    DISK_REFUSE_OVERLAY,      /* disk overlay (Ontrack 54h, EZ-Drive 55h) */
    DISK_REFUSE_UNREADABLE,   /* LBA 0 could not be read */
    DISK_REFUSE_DURABILITY    /* write cache on and no FLUSH CACHE: writes cannot be confirmed */
};
const char *disk_refusal_text(enum disk_refusal);

struct disk_partition {
    uint64_t start, count;
    uint8_t type;
    bool bootable, logical, extended; /* extended: a container entry */
};
struct disk_volume {
    char letter;          /* 'E' ... */
    uint8_t fs;           /* CIUKI_DISK_FS_* value: 12, 16, 32, 7 (NTFS) */
    bool readonly;
    uint32_t partition;   /* kernel table order, 1-based */
    char label[12];
};
struct disk_identity {
    char model[41], serial[21], firmware[9];
    uint64_t sectors;
    uint32_t sector_size;
    uint8_t table_fingerprint[32]; /* SHA-256 of LBA 0 and every EBR, walk order */
};
struct disk_entry {
    unsigned disk;
    bool present;
    struct disk_identity id;
    uint32_t flags;          /* CIUKI_DISK_* info flags */
    uint32_t write_cache;    /* CIUKI_DISK_CACHE_* */
    uint64_t lba_limit;
    bool table_present;      /* 55AA at LBA 0 */
    bool table_error;        /* malformed or looping EBR chain (still erasable) */
    bool gpt, overlay;
    unsigned ebrs;
    unsigned partitions;
    struct disk_partition partition[DISK_TABLE_MAX];
    unsigned volumes;
    struct disk_volume volume[DISK_VOLUMES_MAX];
    bool stub_mbr;           /* the f9-02 incomplete-install MBR is at LBA 0 */
    bool setup_incomplete;   /* stub MBR, or SETUP.STATE other than phase=installed */
    char setup_state[64];    /* first line of \SYSTEM\SETUP.STATE when found */
    bool installable;
    enum disk_refusal refusal;
};
struct disk_list {
    unsigned count;          /* always DISK_SLOTS; absent slots have present=false */
    struct disk_entry disk[DISK_SLOTS];
};
/* Reads only: DISK_GET_INFO, LBA 0, the EBR chain and SETUP.STATE. */
int disk_enumerate(struct disk_list *out);
int disk_probe(unsigned disk, struct disk_entry *out);

/* Payload accounting for the size check. Add files one by one for exact
 * cluster use, or set bytes/files directly for the conservative estimate
 * (every file wastes up to one cluster). */
struct disk_payload {
    uint64_t bytes;
    uint64_t clusters;       /* exact data clusters when nonzero */
    uint32_t files, directories;
};
void disk_payload_add_file(struct disk_payload *, uint64_t size);
void disk_payload_add_directory(struct disk_payload *);

/* FAT32 geometry of mkfs.fat 4.2 `-F 32 -S 512 -s 8 -f 2 -R 160 -b 6
 * -h 2048 -a` on a partition of part_sectors (the build_image.py command). */
struct disk_geometry {
    uint64_t part_start;
    uint32_t part_sectors;
    uint32_t reserved, fat_sectors, clusters, data_sector, root_cluster;
    uint32_t fat_spec_sectors;   /* Microsoft FAT spec formula (record; may exceed the need) */
    uint16_t sectors_per_track, heads; /* BPB CHS fields as mkfs.fat 4.2 */
    uint16_t fsinfo, backup;
};
int disk_geometry_whole(uint64_t capacity, struct disk_geometry *out);

enum disk_range_kind {
    DISK_R_STUB_MBR, DISK_R_GAP, DISK_R_BOOT, DISK_R_FSINFO, DISK_R_RESERVED,
    DISK_R_BOOT_BACKUP, DISK_R_FSINFO_BACKUP, DISK_R_BLACKBOX, DISK_R_RING,
    DISK_R_FAT1, DISK_R_FAT1_ZERO, DISK_R_FAT2, DISK_R_FAT2_ZERO,
    DISK_R_ROOT, DISK_R_ROOT_ZERO, DISK_R_LOADER, DISK_R_MBR
};
/* One raw write of the dry run: absolute LBA, sector count, the phase that
 * writes it (1, 2 or 5) and SHA-256 of exactly the bytes written. */
struct disk_range {
    uint64_t lba;
    uint32_t count;
    uint8_t phase, kind;
    bool zero;
    uint8_t sha256[32];
};
struct disk_plan {
    unsigned disk;
    struct disk_identity id;     /* revalidated after the lease is acquired */
    struct disk_geometry geo;
    uint32_t disk_signature, volume_id; /* fresh, from getentropy */
    uint16_t label_date, label_time;    /* FAT date/time of the CIUKIOS label */
    uint8_t table[64];           /* MBR bytes 446..509 */
    unsigned erased;             /* every partition found, containers included */
    struct disk_partition erased_list[DISK_TABLE_MAX];
    unsigned erased_volumes;
    struct disk_volume erased_volume[DISK_VOLUMES_MAX];
    uint64_t unallocated_sectors; /* beyond the 28-bit cap */
    struct disk_payload payload;
    uint64_t partition_bytes, data_bytes;
    uint64_t overhead_bytes;     /* reserved + FATs + tail + slack + directories */
    uint64_t reserve_bytes;      /* pagefile: 16 MiB + max(16 MiB, 5 %) + 16 MiB */
    uint64_t required_bytes;     /* payload + overhead + reserve */
    uint64_t free_after_bytes;
    bool fits;
    bool loader_planned;
    unsigned ranges;
    struct disk_range range[DISK_RANGES_MAX];
};
/* No writes. Fills the plan even when it returns DISK_EFULL. */
int disk_plan_whole(unsigned disk, const struct disk_payload *, struct disk_plan *out);
/* Adds the P5 raw ranges (loader at LBA 1, canonical MBR) to the dry run. */
int disk_plan_publish(struct disk_plan *, const void *loader, uint32_t loader_bytes);

struct disk_progress {
    unsigned phase;           /* 1, 2 or 5 */
    const char *step;
    uint64_t done, total;     /* sectors written and read back in this phase */
};
struct disk_callbacks {
    void (*progress)(void *ctx, const struct disk_progress *);
    bool (*cancel)(void *ctx); /* polled between requests of at most 128 sectors */
    void *ctx;
};
/* P1: lease, revalidation (DISK_ECHANGED, lease released), stub MBR with
 * the planned table and signature, zero LBA 1..2047, flush, raw read-back. */
int disk_execute_p1(const struct disk_plan *, const struct disk_callbacks *);
/* P2: reserved sectors, FAT copy 1, flush, FAT copy 2, root, flush, raw
 * read-back of every written sector; DISK_PUBLISH; \SYSTEM and SETUP.STATE
 * ("phase=format") through the VFS with fsync; DISK_FLUSH. FAT[1]'s clean
 * bit is written clear. *letter receives the published drive letter. */
int disk_execute_p2(const struct disk_plan *, const struct disk_callbacks *, char *letter);
struct disk_publish {
    const void *loader;       /* ciukldr image bytes, whole sectors, 1..1023 */
    uint32_t loader_bytes;
    const char *boot_cfg;     /* \SYSTEM\BOOT.CFG, <=127 ASCII bytes, no setup=1 */
    const char *setup_state;  /* \SYSTEM\SETUP.STATE, "phase=installed ..." */
};
/* P5: loader + read-back, BOOT.CFG, SETUP.STATE, the kernel's sealing
 * recheck (clean unmount sets FAT[1]'s clean bit, copy 1 -> barrier -> copy
 * 2; read-only remount with the full scan), flush, FAT[1] read-back, then
 * the canonical MBR code over the stub, flush, raw read-back. */
int disk_execute_p5(const struct disk_plan *, const struct disk_publish *, const struct disk_callbacks *);

struct disk_check {
    uint32_t reasons;          /* CIUKI_FAT_RO_* (REQUEST masked out) */
    uint32_t fat_type, clusters, free_clusters, lost_clusters;
    uint32_t scan_passes, scan_ms, lfn_orphans, lfn_invalid, lfn_bad_checksum;
};
/* Read-only remount and full ownership scan of the published volume; the
 * volume is writable again afterwards. CORRUPT, COPIES, LOST or IO_FLAG ->
 * DISK_ECONSISTENCY. Every file on the volume must be closed. */
int disk_consistency_check(char letter, struct disk_check *out);

/* Raw reads through the node (no FAT cache). Call disk_flush first so the
 * published volume's cached writes are on the medium. */
int disk_read_raw(unsigned disk, uint64_t lba, uint32_t count, void *buf);
int disk_flush(unsigned disk);
int disk_set_phase(unsigned disk, const char *text);
/* Ends the session: DISK_LEASE_RELEASE (no rescan) and close. */
int disk_release(unsigned disk);

struct disk_error_info {
    int code;                  /* DISK_E* */
    int err;                   /* positive errno from the kernel, 0 if none */
    unsigned disk;
    uint64_t lba, count;       /* failing range, count 0 if none */
    uint32_t reasons;          /* CIUKI_FAT_RO_* behind DISK_ECONSISTENCY */
    char what[48];
};
const struct disk_error_info *disk_last_error(void);

/* Platform boundary. The default is the native CiukiOS backend (open,
 * pread, pwrite, ciuki_devctl, fsync on /mnt/<letter>/...); host tests
 * install a file-backed model. Paths are relative to the drive root with
 * '/' separators. Errors are negative errno values. */
struct disk_backend {
    void *ctx;
    int (*open)(void *ctx, unsigned disk, bool write);
    void (*close)(void *ctx, int handle);
    int (*pread)(void *ctx, int handle, void *buf, uint32_t bytes, uint64_t offset);
    int (*pwrite)(void *ctx, int handle, const void *buf, uint32_t bytes, uint64_t offset);
    int (*devctl)(void *ctx, int handle, uint32_t op, void *record, uint32_t size);
    int (*mkdir)(void *ctx, char letter, const char *path);
    int (*write_file)(void *ctx, char letter, const char *path, const void *data, uint32_t bytes);
    int (*read_file)(void *ctx, char letter, const char *path, void *data, uint32_t capacity);
    int (*entropy)(void *ctx, void *buf, uint32_t bytes);
    void (*fat_now)(void *ctx, uint16_t *date, uint16_t *time);
};
void disk_set_backend(const struct disk_backend *); /* NULL: native */
/* The incomplete-install MBR code (src/boot/setup_stub_mbr.asm) and the
 * canonical MBR code (src/boot/mbr.asm), 440 bytes each, from the build. */
extern const uint8_t disk_stub_mbr_code[440];
extern const uint8_t disk_canonical_mbr_code[440];
/* SHA-256 (FIPS 180-4), exported for libsetup's verification. */
struct disk_sha256 { uint32_t h[8]; uint64_t bytes; uint8_t block[64]; uint32_t used; };
void disk_sha256_init(struct disk_sha256 *);
void disk_sha256_update(struct disk_sha256 *, const void *, size_t);
void disk_sha256_final(struct disk_sha256 *, uint8_t out[32]);
#endif
