/* f9-01: read-only, bounded 512-byte block view of an El Torito
 * hard-disk-emulation image over a 2,048-byte optical device, and the
 * boot-CD identification of cd-setup-plan.md sections 3.1 and 3.3.
 * El Torito 1.0 (Phoenix/IBM 1995) 2.1-2.4: boot record volume descriptor
 * at sector 17 of the session, validation entry (header 01h, platform,
 * 16-bit word checksum, key bytes 55h AAh), initial/default entry (88h,
 * media type, load segment, system type, sector count, load RBA).
 * SeaBIOS 1.16.3 src/cdrom.c (cdrom_boot, cdemu_read): each emulated
 * 512-byte sector is read as image_lba + sector/4 with its 512-byte slice.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_CDVIEW_H
#define CIUKI_CDVIEW_H
#include "blkdev.h"

#define CDVIEW_BLOCK 2048u
#define CDVIEW_BOUNCE_BYTES 65536u          /* plan 3.3: bounce <= 64 KiB */
#define CDVIEW_SETUP_SIGNATURE 0x4B554943u   /* reserved Setup CD disk signature */
#define CDVIEW_PART_LBA 2048u                /* ciukldr/disk.inc: partition 1 start */
#define CDVIEW_PART_TYPE 0x0Cu

/* Fields read back from the disc (all little-endian on the medium). */
struct cdview_boot_record {
    uint32_t catalog_block, load_rba;
    uint32_t part_start, part_count, image_sectors, signature;
    uint16_t load_segment, sector_count, checksum;
    uint8_t platform, media, system_type, indicator;
};
/* Reasons are static strings for the [setup] candidate record. */
int cdview_probe(struct blkdev *optical, uint64_t session, void *block,
                 struct cdview_boot_record *out, const char **reason);

struct cdview {
    struct blkdev dev;          /* 512-byte sectors; write/flush NULL;
                                * media_check compares generations without I/O */
    struct blkdev *parent;      /* optical device, 2,048-byte sectors */
    uint64_t image_block;       /* load RBA: first ISO block of the image */
    uint64_t image_sectors;     /* emulated 512-byte sectors (capacity) */
    uint32_t generation;        /* parent media_generation captured at bind */
    uint8_t *bounce;            /* CDVIEW_BOUNCE_BYTES, owned by the view */
    fs_lock lock;
    bool bound, detached;
    uint64_t reads, blocks;     /* diagnostics: parent requests and blocks */
};
int cdview_bind(struct cdview *, struct blkdev *optical, uint64_t image_block, uint64_t image_sectors);
void cdview_release(struct cdview *);

/* The running kernel's build identity: bytes at a virtual address of the
 * loaded image. VMM.ELF on the boot image matches when the same bytes are at
 * the file offset of that address (its PT_LOAD program headers). */
struct cdview_identity { uint32_t address; const void *bytes; uint32_t length; };
struct fat_volume;
/* \SYSTEM\BOOT.CFG equal to options byte for byte (options: the loader's
 * NUL-terminated boot-info copy) and \SYSTEM\VMM.ELF's identity equal. */
int cdview_verify_volume(struct fat_volume *, const char *options, size_t options_size,
                         const struct cdview_identity *, const char **reason);
#endif
