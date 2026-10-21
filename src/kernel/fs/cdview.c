/* f9-01: bounded read-only 512-byte view of the Setup CD's El Torito
 * hard-disk-emulation image, and the checks that identify the boot CD
 * (cd-setup-plan.md 3.1, 3.3; directive f9-01 item 6).
 * El Torito 1.0: 2.0 and figure 7 (boot record at sector 17 of the last
 * session, catalogue pointer at 47h), 2.1/figure 2 (validation entry,
 * word checksum, 55h AAh), 2.2/figure 3 (initial entry: 88h, media type 4 =
 * hard disk, load segment 0 = 7C0h, system type = partition type, sector
 * count, load RBA), 4.1 (hard-disk images: one partition, first entry).
 * The translation equals SeaBIOS 1.16.3 cdemu_read: block = image + lba/4,
 * slice (lba mod 4) x 512. The view streams; the image is never copied.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "cdview.h"
#include "fat.h"

static int cdview_read(struct blkdev *, uint64_t, uint32_t, void *);

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int block_read(struct blkdev *optical, uint64_t block, void *buffer)
{
    int e = blkdev_range(optical, block, 1);
    return e ? e : optical->read(optical, block, 1, buffer);
}

int cdview_probe(struct blkdev *optical, uint64_t session, void *buffer,
                 struct cdview_boot_record *out, const char **reason)
{
    static const char system_id[] = "EL TORITO SPECIFICATION";
    const char *why = "argument";
    uint8_t *b = buffer;
    int e = -FS_EINVAL;
    if (!optical || !buffer || !out || optical->sector_size != CDVIEW_BLOCK || !optical->read) goto done;
    memset(out, 0, sizeof(*out));
    why = "boot_record_read";
    if (session > UINT64_MAX - 17 || (e = block_read(optical, session + 17, b))) goto done;
    e = -FS_EINVAL; why = "boot_record";
    if (b[0] || memcmp(b + 1, "CD001", 5) || b[6] != 1) goto done;
    for (unsigned i = 0; i < 32; i++)
        if (b[7 + i] != (i < sizeof(system_id) - 1 ? (uint8_t)system_id[i] : 0)) goto done;
    out->catalog_block = rd32(b + 0x47);
    why = "catalog_read";
    if ((e = block_read(optical, out->catalog_block, b))) goto done;
    e = -FS_EINVAL; why = "validation_entry";
    uint16_t sum = 0;
    for (unsigned i = 0; i < 32; i += 2) sum = (uint16_t)(sum + rd16(b + i));
    out->platform = b[1]; out->checksum = rd16(b + 0x1c);
    if (b[0] != 1 || b[1] != 0 || rd16(b + 2) || sum || b[0x1e] != 0x55 || b[0x1f] != 0xaa) goto done;
    const uint8_t *entry = b + 32;
    out->indicator = entry[0]; out->media = entry[1]; out->load_segment = rd16(entry + 2);
    out->system_type = entry[4]; out->sector_count = rd16(entry + 6); out->load_rba = rd32(entry + 8);
    why = "initial_entry";
    if (entry[0] != 0x88 || entry[1] != 4 || out->load_segment || entry[4] != CDVIEW_PART_TYPE ||
        entry[5] || out->sector_count != 1) goto done;
    why = "image_read";
    if ((e = block_read(optical, out->load_rba, b))) goto done;
    e = -FS_EINVAL; why = "image_mbr";
    const uint8_t *part = b + 446;
    out->signature = rd32(b + 440);
    out->part_start = rd32(part + 8); out->part_count = rd32(part + 12);
    if (rd16(b + 510) != 0xaa55 || part[0] != 0x80 || part[4] != CDVIEW_PART_TYPE) goto done;
    for (unsigned i = 16; i < 64; i++) if (part[i]) goto done;
    why = "signature";
    if (out->signature != CDVIEW_SETUP_SIGNATURE) goto done;
    why = "partition";
    if (out->part_start != CDVIEW_PART_LBA || !out->part_count ||
        out->part_count > UINT32_MAX - CDVIEW_PART_LBA) goto done;
    out->image_sectors = out->part_start + out->part_count;
    why = "image_extent";
    if (out->load_rba >= optical->capacity ||
        out->image_sectors > (optical->capacity - out->load_rba) * 4) goto done;
    e = 0; why = "ok";
done:
    if (reason) *reason = why;
    return e;
}

int cdview_bind(struct cdview *v, struct blkdev *optical, uint64_t image_block, uint64_t image_sectors)
{
    if (!v || v->bound || !optical || optical->sector_size != CDVIEW_BLOCK || !optical->read || !image_sectors ||
        image_block >= optical->capacity || image_sectors > (optical->capacity - image_block) * 4)
        return -FS_EINVAL;
    uint8_t *bounce = fs_contig_alloc(CDVIEW_BOUNCE_BYTES);
    if (!bounce) return -FS_ENOMEM;
    memset(v, 0, sizeof(*v));
    fs_lock_init(&v->lock);
    v->parent = optical; v->image_block = image_block; v->image_sectors = image_sectors;
    v->generation = optical->media_generation; v->bounce = bounce; v->bound = true;
    /* No write or flush: fat_mount can only be read-only (not durable). */
    v->dev = (struct blkdev){ .read = cdview_read, .capacity = image_sectors, .sector_size = BLKDEV_SECTOR_SIZE,
        .media_generation = v->generation, .write_cache_state = BLKDEV_CACHE_UNKNOWN, .ctx = v };
    return 0;
}

void cdview_release(struct cdview *v)
{
    if (!v || !v->bound) return;
    fs_contig_free(v->bounce, CDVIEW_BOUNCE_BYTES);
    fs_lock_destroy(&v->lock);
    memset(v, 0, sizeof(*v));
}

/* A replaced or removed disc never satisfies a read of the old image: the
 * view stays quarantined for the boot (plan 7.4: reinsert the Setup CD). */
static int generation_check(struct cdview *v)
{
    if (v->detached) return -FS_EQUARANTINED;
    if (v->parent->media_generation == v->generation) return 0;
    v->detached = true; v->dev.quarantined = true;
    klog("[cdview] detached generation=%u", v->parent->media_generation);
    return -FS_EQUARANTINED;
}

static int cdview_read(struct blkdev *d, uint64_t lba, uint32_t count, void *buffer)
{
    struct cdview *v = d->ctx;
    int e = blkdev_range(d, lba, count);
    if (e || !buffer) return e ? e : -FS_EINVAL;
    uint8_t *out = buffer;
    fs_lock_take(&v->lock);
    e = generation_check(v);
    while (!e && count) {
        uint64_t block = v->image_block + lba / 4;
        uint32_t slice = (uint32_t)(lba % 4);
        uint32_t blocks = (slice + count + 3) / 4;
        if (blocks > CDVIEW_BOUNCE_BYTES / CDVIEW_BLOCK) blocks = CDVIEW_BOUNCE_BYTES / CDVIEW_BLOCK;
        uint32_t sectors = blocks * 4 - slice;
        if (sectors > count) sectors = count;
        e = blkdev_range(v->parent, block, blocks);
        if (!e) e = v->parent->read(v->parent, block, blocks, v->bounce);
        /* The generation is checked after the transfer too: a change
         * during the read discards the bytes. */
        int g = generation_check(v);
        if (!e) e = g;
        if (e) break;
        memcpy(out, v->bounce + slice * BLKDEV_SECTOR_SIZE, (size_t)sectors * BLKDEV_SECTOR_SIZE);
        out += (size_t)sectors * BLKDEV_SECTOR_SIZE; lba += sectors; count -= sectors;
        v->reads++; v->blocks += blocks;
    }
    fs_lock_drop(&v->lock);
    return e;
}

static int system_file(struct fat_volume *fat, const char *name, struct fat_entry *file)
{
    struct fat_entry directory;
    int e = fat_lookup(fat, fat->type == 32 ? fat->root : 0, "SYSTEM", &directory);
    if (!e && !(directory.attr & FAT_ATTR_DIR)) e = -FS_ENOTDIR;
    if (!e) e = fat_lookup(fat, directory.first, name, file);
    if (!e && (file->attr & FAT_ATTR_DIR)) e = -FS_EISDIR;
    return e;
}

static int exact_read(struct fat_volume *fat, const struct fat_entry *file, uint64_t at, void *buffer, size_t bytes)
{
    size_t done = 0;
    if (at > file->size || bytes > file->size - at) return -FS_EINVAL;
    int e = fat_read(fat, file, at, buffer, bytes, &done);
    return e ? e : done == bytes ? 0 : -FS_EIO;
}

int cdview_verify_volume(struct fat_volume *fat, const char *options, size_t options_size,
                         const struct cdview_identity *kid, const char **reason)
{
    const char *why = "argument";
    int e = -FS_EINVAL;
    struct fat_entry file;
    uint8_t text[128];
    if (!fat || !options || !kid || !kid->bytes || !kid->length || kid->length > 128) goto done;
    size_t length = 0;
    while (length < options_size && options[length]) length++;
    why = "boot_cfg";
    if ((e = system_file(fat, "BOOT.CFG", &file))) goto done;
    e = -FS_EINVAL; why = "boot_cfg_differs";
    if (file.size != length || length > sizeof(text)) goto done;
    if ((e = exact_read(fat, &file, 0, text, length))) { why = "boot_cfg_read"; goto done; }
    e = -FS_EINVAL;
    if (memcmp(text, options, length)) goto done;
    why = "kernel";
    if ((e = system_file(fat, "VMM.ELF", &file))) goto done;
    uint8_t header[52];
    why = "kernel_header";
    if ((e = exact_read(fat, &file, 0, header, sizeof(header)))) goto done;
    e = -FS_EINVAL;
    if (memcmp(header, "\177ELF\001\001\001", 7) || rd16(header + 42) != 32 || !rd16(header + 44) || rd16(header + 44) > 16)
        goto done;
    uint32_t phoff = rd32(header + 28), phnum = rd16(header + 44), found = 0;
    for (uint32_t i = 0; i < phnum && !found; i++) {
        uint8_t ph[32];
        if ((e = exact_read(fat, &file, (uint64_t)phoff + i * 32u, ph, sizeof(ph)))) goto done;
        uint32_t type = rd32(ph), offset = rd32(ph + 4), vaddr = rd32(ph + 8), filesz = rd32(ph + 16);
        if (type == 1 && kid->address >= vaddr && kid->address - vaddr <= filesz &&
            kid->length <= filesz - (kid->address - vaddr))
            found = offset + (kid->address - vaddr);
    }
    e = -FS_EINVAL; why = "kernel_identity";
    if (!found) goto done;
    if ((e = exact_read(fat, &file, found, text, kid->length))) goto done;
    e = memcmp(text, kid->bytes, kid->length) ? -FS_EINVAL : 0;
    if (!e) why = "ok";
done:
    if (reason) *reason = why;
    return e;
}
