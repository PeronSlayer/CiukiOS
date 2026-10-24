/* libdisk enumeration, whole-disk plan, raw phases P1/P2/P5 and checks.
 * Contract: docs/design/cd-setup-plan.md 5 and 6, directive f9-02. MBR and
 * EBR conventions as src/kernel/fs/partition.c (logical starts relative to
 * their EBR, links relative to the extended container). One session: the
 * node of the destination stays open and leased from P1 to disk_release.
 * SPDX-License-Identifier: MIT */
#include "internal.h"
#include <string.h>
#include <stdlib.h>

/* A strong reference extracts native.o when applications link -ldisk.
 * Tests replace this backend at the same link boundary. */
const struct disk_backend *disk_native_backend(void);

#define CD_SIGNATURE 0x4B554943u /* reserved for the Setup CD image (lead decision O4) */
#define MIB (1024ull * 1024ull)
#define BAD_REASONS (CIUKI_FAT_RO_CORRUPT | CIUKI_FAT_RO_COPIES | CIUKI_FAT_RO_LOST | CIUKI_FAT_RO_IO_FLAG | \
                     CIUKI_FAT_RO_WRITE_ERROR | CIUKI_FAT_RO_ACTIVE_ONLY)

static const struct disk_backend *installed;
static struct disk_error_info last;
static struct {
    bool open, leased, published;
    int handle;
    unsigned disk;
    char letter;
} session;
static uint8_t io_buffer[DISK_IO_SECTORS * DISK_SECTOR];
static uint8_t check_buffer[DISK_IO_SECTORS * DISK_SECTOR];
static struct disk_entry scratch;
static struct disk_plan p5_plan;

static const struct disk_backend *backend(void)
{
    if (installed) return installed;
    return disk_native_backend();
}
void disk_set_backend(const struct disk_backend *b) { installed = b; }
const struct disk_error_info *disk_last_error(void) { return &last; }

static void copy_text(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}
static int fail(int code, int err, unsigned disk, uint64_t lba, uint64_t count, const char *what)
{
    last.code = code; last.err = err < 0 ? -err : err; last.disk = disk;
    last.lba = lba; last.count = count;
    copy_text(last.what, sizeof(last.what), what);
    return code;
}
static void clear_error(void) { memset(&last, 0, sizeof(last)); }
static int lease_code(int e) { return e == -ENOMEM ? DISK_ENOMEM : DISK_ELEASE; }

const char *disk_refusal_text(enum disk_refusal r)
{
    switch (r) {
    case DISK_INSTALLABLE: return "installable";
    case DISK_REFUSE_ABSENT: return "no ATA disk in this slot";
    case DISK_REFUSE_NOT_DISK0: return "only ATA disk 0 (primary master) in this Beta";
    case DISK_REFUSE_SECTOR_SIZE: return "sectors are not 512 bytes";
    case DISK_REFUSE_TOO_SMALL: return "smaller than 1 GiB";
    case DISK_REFUSE_GPT: return "GPT (non-MBR) partition scheme";
    case DISK_REFUSE_OVERLAY: return "disk overlay software installed";
    case DISK_REFUSE_UNREADABLE: return "sector 0 cannot be read";
    case DISK_REFUSE_DURABILITY: return "the disk cannot confirm writes";
    }
    return "unknown";
}

static int raw_read(int h, unsigned disk, uint64_t lba, uint32_t count, void *buf)
{
    const struct disk_backend *b = backend();
    uint8_t *p = buf;
    while (count) {
        uint32_t n = count < DISK_IO_SECTORS ? count : DISK_IO_SECTORS;
        int r = b->pread(b->ctx, h, p, n * DISK_SECTOR, lba * DISK_SECTOR);
        if (r != (int)(n * DISK_SECTOR))
            return fail(r == -ENOMEM ? DISK_ENOMEM : DISK_EIO, r < 0 ? r : EIO, disk, lba, n, "read");
        lba += n; p += (size_t)n * DISK_SECTOR; count -= n;
    }
    return 0;
}
static int raw_write(int h, unsigned disk, uint64_t lba, uint32_t count, const void *buf)
{
    const struct disk_backend *b = backend();
    const uint8_t *p = buf;
    while (count) {
        uint32_t n = count < DISK_IO_SECTORS ? count : DISK_IO_SECTORS;
        int r = b->pwrite(b->ctx, h, p, n * DISK_SECTOR, lba * DISK_SECTOR);
        if (r != (int)(n * DISK_SECTOR)) {
            int code = r == -EPERM || r == -EACCES ? DISK_ELEASE : r == -EROFS ? DISK_EFLUSH :
                       r == -ENOMEM ? DISK_ENOMEM : DISK_EIO;
            return fail(code, r < 0 ? r : EIO, disk, lba, n, "write");
        }
        lba += n; p += (size_t)n * DISK_SECTOR; count -= n;
    }
    return 0;
}
static int control(uint32_t op, void *record, uint32_t size)
{
    const struct disk_backend *b = backend();
    return b->devctl(b->ctx, session.handle, op, record, size);
}
static int flush_disk(unsigned disk)
{
    struct ciuki_disk_request q = { CIUKI_DISK_VERSION, sizeof(q), 0, 0 };
    int r = control(DISK_FLUSH, &q, sizeof(q));
    if (r) return fail(r == -EPERM || r == -EACCES ? DISK_ELEASE : DISK_EFLUSH, r, disk, 0, 0, "flush");
    return 0;
}

static bool extended_type(uint8_t t) { return t == 0x05 || t == 0x0f || t == 0x85; }
static void add_partition(struct disk_entry *e, uint64_t start, uint32_t count, const uint8_t *entry, bool logical)
{
    if (e->partitions == DISK_TABLE_MAX) { e->table_error = true; return; }
    struct disk_partition *p = &e->partition[e->partitions++];
    p->start = start; p->count = count; p->type = entry[4];
    p->bootable = entry[0] == 0x80; p->logical = logical; p->extended = extended_type(entry[4]);
    if (p->type == 0xee) e->gpt = true;
    if (p->type == 0x54 || p->type == 0x55) e->overlay = true;
}
/* Reads LBA 0 and the EBR chain. The fingerprint hashes every sector read,
 * in walk order; a malformed chain stops the walk and sets table_error. */
static int table_scan(int h, struct disk_entry *e, uint8_t fingerprint[32])
{
    uint8_t s[DISK_SECTOR];
    struct disk_sha256 sha;
    disk_sha256_init(&sha);
    int r = raw_read(h, e->disk, 0, 1, s);
    if (r) return r;
    disk_sha256_update(&sha, s, sizeof(s));
    e->stub_mbr = !memcmp(s, disk_stub_mbr_code, sizeof(disk_stub_mbr_code));
    e->table_present = s[510] == 0x55 && s[511] == 0xaa;
    uint8_t primary[64];
    memcpy(primary, s + 446, sizeof(primary));
    for (unsigned i = 0; e->table_present && i < 4; i++) {
        const uint8_t *q = primary + 16 * i;
        if (q[4]) add_partition(e, disk_rd32(q + 8), disk_rd32(q + 12), q, false);
    }
    for (unsigned i = 0; e->table_present && i < 4; i++) {
        const uint8_t *q = primary + 16 * i;
        if (!extended_type(q[4])) continue;
        uint64_t base = disk_rd32(q + 8), end = base + disk_rd32(q + 12), cur = base;
        uint64_t seen[DISK_TABLE_MAX];
        unsigned nseen = 0;
        for (;;) {
            bool loop = false;
            for (unsigned j = 0; j < nseen; j++) loop |= seen[j] == cur;
            if (loop || nseen == DISK_TABLE_MAX || !cur || cur < base || cur >= end || cur >= e->id.sectors) {
                e->table_error = true; break;
            }
            seen[nseen++] = cur;
            if (raw_read(h, e->disk, cur, 1, s)) { e->table_error = true; clear_error(); break; }
            disk_sha256_update(&sha, s, sizeof(s));
            e->ebrs++;
            if (s[510] != 0x55 || s[511] != 0xaa) { e->table_error = true; break; }
            uint64_t next = 0;
            for (unsigned k = 0; k < 4; k++) {
                const uint8_t *l = s + 446 + 16 * k;
                if (!l[4]) continue;
                if (extended_type(l[4])) { if (!next) next = base + disk_rd32(l + 8); }
                else add_partition(e, cur + disk_rd32(l + 8), disk_rd32(l + 12), l, true);
            }
            if (!next) break;
            cur = next;
        }
    }
    disk_sha256_final(&sha, fingerprint);
    return 0;
}

int disk_probe(unsigned disk, struct disk_entry *e)
{
    clear_error();
    memset(e, 0, sizeof(*e));
    e->disk = disk; e->refusal = DISK_REFUSE_ABSENT;
    const struct disk_backend *b = backend();
    if (!b) return fail(DISK_EINTERNAL, 0, disk, 0, 0, "no backend");
    if (disk >= DISK_SLOTS) return fail(DISK_EINTERNAL, EINVAL, disk, 0, 0, "disk slot");
    bool own = !(session.open && session.disk == disk);
    int h = own ? b->open(b->ctx, disk, false) : session.handle;
    if (h == -ENXIO || h == -ENODEV || h == -ENOENT) return 0;
    if (h < 0) return fail(DISK_EINTERNAL, h, disk, 0, 0, "open");
    struct ciuki_disk_info info;
    memset(&info, 0, sizeof(info));
    info.version = CIUKI_DISK_VERSION; info.size = sizeof(info);
    int r = b->devctl(b->ctx, h, DISK_GET_INFO, &info, sizeof(info));
    if (r) { if (own) b->close(b->ctx, h); return fail(DISK_EINTERNAL, r, disk, 0, 0, "DISK_GET_INFO"); }
    e->present = info.flags & CIUKI_DISK_PRESENT;
    info.model[40] = info.serial[20] = info.firmware[8] = 0;
    memcpy(e->id.model, info.model, sizeof(e->id.model));
    memcpy(e->id.serial, info.serial, sizeof(e->id.serial));
    memcpy(e->id.firmware, info.firmware, sizeof(e->id.firmware));
    e->id.sectors = info.sectors; e->id.sector_size = info.sector_size;
    e->flags = info.flags; e->write_cache = info.write_cache; e->lba_limit = info.lba_limit;
    for (unsigned i = 0; i < info.volume_count && i < CIUKI_DISK_VOLUMES_MAX; i++) {
        const struct ciuki_disk_volume *v = &info.volumes[i];
        if (v->drive >= 26) continue;
        struct disk_volume *o = &e->volume[e->volumes++];
        o->letter = (char)('A' + v->drive); o->fs = (uint8_t)v->fs; o->partition = v->partition;
        o->readonly = v->flags & CIUKI_DISK_VOLUME_READONLY;
        memcpy(o->label, v->label, sizeof(o->label)); o->label[11] = 0;
    }
    bool readable = false;
    if (e->present && info.sector_size == DISK_SECTOR) {
        readable = !table_scan(h, e, e->id.table_fingerprint);
        clear_error(); /* an unreadable sector 0 is a refusal, not a probe error */
    }
    if (own) b->close(b->ctx, h);
    e->setup_incomplete = e->stub_mbr;
    for (unsigned i = 0; i < e->volumes && b->read_file; i++) {
        if (e->volume[i].fs == CIUKI_DISK_FS_NTFS) continue;
        char text[64];
        int n = b->read_file(b->ctx, e->volume[i].letter, "SYSTEM/SETUP.STATE", text, sizeof(text) - 1);
        if (n <= 0) continue;
        text[n] = 0;
        unsigned k = 0;
        while (text[k] && text[k] != '\n' && text[k] != '\r' && k + 1 < sizeof(e->setup_state)) {
            e->setup_state[k] = text[k] >= 32 && text[k] <= 126 ? text[k] : '?'; k++;
        }
        e->setup_state[k] = 0;
        if (strncmp(text, "phase=installed", 15)) e->setup_incomplete = true;
        break;
    }
    bool durable = e->write_cache == CIUKI_DISK_CACHE_DISABLED ||
                   (e->write_cache == CIUKI_DISK_CACHE_ENABLED && (e->flags & CIUKI_DISK_FLUSH_CACHE));
    e->refusal = !e->present ? DISK_REFUSE_ABSENT : disk ? DISK_REFUSE_NOT_DISK0 :
        e->id.sector_size != DISK_SECTOR ? DISK_REFUSE_SECTOR_SIZE : !readable ? DISK_REFUSE_UNREADABLE :
        e->gpt ? DISK_REFUSE_GPT : e->overlay ? DISK_REFUSE_OVERLAY :
        e->id.sectors < DISK_MIN_SECTORS ? DISK_REFUSE_TOO_SMALL : !durable ? DISK_REFUSE_DURABILITY : DISK_INSTALLABLE;
    e->installable = e->refusal == DISK_INSTALLABLE;
    return 0;
}

int disk_enumerate(struct disk_list *out)
{
    memset(out, 0, sizeof(*out));
    out->count = DISK_SLOTS;
    int first = 0;
    struct disk_error_info error = { 0 };
    for (unsigned d = 0; d < DISK_SLOTS; d++) {
        int r = disk_probe(d, &out->disk[d]);
        if (r && !first) { first = r; error = last; }
    }
    if (first) last = error; else clear_error();
    return first;
}

void disk_payload_add_file(struct disk_payload *p, uint64_t size)
{
    uint64_t cluster = (uint64_t)DISK_SPC * DISK_SECTOR;
    p->bytes += size; p->files++;
    p->clusters += (size + cluster - 1) / cluster;
}
void disk_payload_add_directory(struct disk_payload *p) { p->directories++; }

/* Size check of plan 4 ("Ready to install"). Overhead: the partition space
 * outside data clusters (reserved, FATs, tail), the unused end of every
 * file's last cluster (exact with disk_payload_add_file, else one cluster
 * per file) and directories (one cluster each plus three 32-byte entries,
 * short name and two long-name slots, per file or directory). Reserve: the
 * pagefile's 16 MiB initial extent + max(16 MiB, 5 % of the volume) +
 * 16 MiB (boot-memory.md). */
static void sizes(struct disk_plan *p, const struct disk_payload *payload)
{
    uint64_t cluster = (uint64_t)DISK_SPC * DISK_SECTOR;
    if (payload) p->payload = *payload;
    p->partition_bytes = (uint64_t)p->geo.part_sectors * DISK_SECTOR;
    p->data_bytes = (uint64_t)p->geo.clusters * cluster;
    uint64_t used = p->payload.clusters ? p->payload.clusters * cluster :
        ((p->payload.bytes + cluster - 1) / cluster + p->payload.files) * cluster;
    uint64_t slack = used > p->payload.bytes ? used - p->payload.bytes : 0;
    uint64_t entries = 3ull * ((uint64_t)p->payload.files + p->payload.directories);
    uint64_t directories = ((uint64_t)p->payload.directories + 1) * cluster +
        (entries * 32 + cluster - 1) / cluster * cluster;
    p->overhead_bytes = (p->partition_bytes - p->data_bytes) + slack + directories;
    uint64_t five = p->partition_bytes / 20;
    p->reserve_bytes = 16 * MIB + (five > 16 * MIB ? five : 16 * MIB) + 16 * MIB;
    p->required_bytes = p->payload.bytes + p->overhead_bytes + p->reserve_bytes;
    p->fits = p->required_bytes <= p->partition_bytes;
    p->free_after_bytes = p->fits ? p->partition_bytes - p->required_bytes : 0;
}

void disk_range_sector(const struct disk_plan *p, const struct disk_range *r, uint32_t index,
                       const void *loader, uint8_t out[512])
{
    switch (r->kind) {
    case DISK_R_STUB_MBR: disk_fmt_mbr(out, disk_stub_mbr_code, p->disk_signature, p->table); return;
    case DISK_R_MBR: disk_fmt_mbr(out, disk_canonical_mbr_code, p->disk_signature, p->table); return;
    case DISK_R_BOOT: case DISK_R_BOOT_BACKUP: disk_fmt_boot(out, &p->geo, p->volume_id); return;
    case DISK_R_FSINFO: case DISK_R_FSINFO_BACKUP: disk_fmt_fsinfo(out, &p->geo); return;
    case DISK_R_BLACKBOX: disk_fmt_blackbox(out); return;
    case DISK_R_FAT1: case DISK_R_FAT2: disk_fmt_fat_first(out, false); return;
    case DISK_R_ROOT: disk_fmt_root_first(out, p->label_date, p->label_time); return;
    case DISK_R_LOADER: memcpy(out, (const uint8_t *)loader + (size_t)index * DISK_SECTOR, DISK_SECTOR); return;
    default: memset(out, 0, DISK_SECTOR); return;
    }
}
static void zero_hash(uint64_t count, uint8_t out[32])
{
    static struct { uint64_t count; uint8_t sha[32]; } cache[6];
    for (unsigned i = 0; i < 6; i++) if (cache[i].count == count) { memcpy(out, cache[i].sha, 32); return; }
    struct disk_sha256 s;
    disk_sha256_init(&s);
    memset(io_buffer, 0, sizeof(io_buffer));
    for (uint64_t left = count; left;) {
        uint64_t n = left < DISK_IO_SECTORS ? left : DISK_IO_SECTORS;
        disk_sha256_update(&s, io_buffer, (size_t)n * DISK_SECTOR);
        left -= n;
    }
    disk_sha256_final(&s, out);
    static unsigned next;
    cache[next].count = count; memcpy(cache[next].sha, out, 32); next = (next + 1) % 6;
}
static void hash_range(const struct disk_plan *p, struct disk_range *r, const void *loader)
{
    if (r->zero) { zero_hash(r->count, r->sha256); return; }
    struct disk_sha256 s;
    uint8_t sector[DISK_SECTOR];
    disk_sha256_init(&s);
    for (uint32_t i = 0; i < r->count; i++) {
        disk_range_sector(p, r, i, loader, sector);
        disk_sha256_update(&s, sector, sizeof(sector));
    }
    disk_sha256_final(&s, r->sha256);
}
static struct disk_range *add_range(struct disk_plan *p, uint64_t lba, uint32_t count, unsigned phase,
                                    enum disk_range_kind kind, bool zero)
{
    struct disk_range *r = &p->range[p->ranges++];
    memset(r, 0, sizeof(*r));
    r->lba = lba; r->count = count; r->phase = (uint8_t)phase; r->kind = (uint8_t)kind; r->zero = zero;
    return r;
}
static void build_ranges(struct disk_plan *p)
{
    uint64_t v = p->geo.part_start, fat = p->geo.fat_sectors;
    add_range(p, 0, 1, 1, DISK_R_STUB_MBR, false);
    add_range(p, 1, DISK_PART_LBA - 1, 1, DISK_R_GAP, true);
    add_range(p, v, 1, 2, DISK_R_BOOT, false);
    add_range(p, v + 1, 1, 2, DISK_R_FSINFO, false);
    add_range(p, v + 2, 4, 2, DISK_R_RESERVED, true);
    add_range(p, v + 6, 1, 2, DISK_R_BOOT_BACKUP, false);
    add_range(p, v + 7, 1, 2, DISK_R_FSINFO_BACKUP, false);
    add_range(p, v + DISK_BLACKBOX_DESCRIPTOR, 1, 2, DISK_R_BLACKBOX, false);
    add_range(p, v + 9, DISK_BLACKBOX_FIRST - 9, 2, DISK_R_RESERVED, true);
    add_range(p, v + DISK_BLACKBOX_FIRST, DISK_BLACKBOX_SECTORS, 2, DISK_R_RING, true);
    add_range(p, v + DISK_RESERVED, 1, 2, DISK_R_FAT1, false);
    add_range(p, v + DISK_RESERVED + 1, (uint32_t)fat - 1, 2, DISK_R_FAT1_ZERO, true);
    add_range(p, v + DISK_RESERVED + fat, 1, 2, DISK_R_FAT2, false);
    add_range(p, v + DISK_RESERVED + fat + 1, (uint32_t)fat - 1, 2, DISK_R_FAT2_ZERO, true);
    add_range(p, v + p->geo.data_sector, 1, 2, DISK_R_ROOT, false);
    add_range(p, v + p->geo.data_sector + 1, DISK_SPC - 1, 2, DISK_R_ROOT_ZERO, true);
    for (unsigned i = 0; i < p->ranges; i++) hash_range(p, &p->range[i], 0);
}

int disk_plan_whole(unsigned disk, const struct disk_payload *payload, struct disk_plan *p)
{
    memset(p, 0, sizeof(*p));
    int r = disk_probe(disk, &scratch);
    if (r) return r;
    if (!scratch.installable) return fail(DISK_EINTERNAL, 0, disk, 0, 0, disk_refusal_text(scratch.refusal));
    const struct disk_backend *b = backend();
    p->disk = disk; p->id = scratch.id;
    if (disk_geometry_whole(scratch.id.sectors, &p->geo)) return fail(DISK_EINTERNAL, 0, disk, 0, 0, "geometry");
    uint32_t random[2] = { 0, 0 };
    for (unsigned tries = 0; tries < 8 && (!random[0] || random[0] == CD_SIGNATURE || !random[1]); tries++) {
        r = b->entropy ? b->entropy(b->ctx, random, sizeof(random)) : -ENOSYS;
        if (r) return fail(DISK_EINTERNAL, r, disk, 0, 0, "entropy");
    }
    if (!random[0] || random[0] == CD_SIGNATURE || !random[1]) return fail(DISK_EINTERNAL, 0, disk, 0, 0, "entropy");
    p->disk_signature = random[0]; p->volume_id = random[1];
    p->label_date = 0x0021; p->label_time = 0; /* 1980-01-01 00:00 without a clock */
    if (b->fat_now) b->fat_now(b->ctx, &p->label_date, &p->label_time);
    disk_fmt_table(p->table, DISK_PART_LBA, p->geo.part_sectors);
    p->erased = scratch.partitions;
    memcpy(p->erased_list, scratch.partition, sizeof(p->erased_list));
    p->erased_volumes = scratch.volumes;
    memcpy(p->erased_volume, scratch.volume, sizeof(p->erased_volume));
    p->unallocated_sectors = scratch.id.sectors - (p->geo.part_start + p->geo.part_sectors);
    sizes(p, payload);
    build_ranges(p);
    clear_error();
    return p->fits ? 0 : fail(DISK_EFULL, 0, disk, 0, 0, "payload does not fit");
}

static int loader_check(const void *loader, uint32_t bytes)
{
    const uint8_t *l = loader;
    if (!l || !bytes || bytes % DISK_SECTOR || bytes / DISK_SECTOR > DISK_LOADER_SECTORS ||
        memcmp(l, "CLDR", 4) || disk_rd16(l + 4) != 1 || disk_rd16(l + 6) != bytes / DISK_SECTOR)
        return DISK_EINTERNAL;
    return 0;
}
int disk_plan_publish(struct disk_plan *p, const void *loader, uint32_t bytes)
{
    clear_error();
    if (!p || loader_check(loader, bytes)) return fail(DISK_EINTERNAL, EINVAL, p ? p->disk : 0, 0, 0, "loader image");
    unsigned kept = 0;
    for (unsigned i = 0; i < p->ranges; i++) if (p->range[i].phase != 5) p->range[kept++] = p->range[i];
    p->ranges = kept;
    hash_range(p, add_range(p, DISK_LOADER_LBA, bytes / DISK_SECTOR, 5, DISK_R_LOADER, false), loader);
    hash_range(p, add_range(p, 0, 1, 5, DISK_R_MBR, false), 0);
    p->loader_planned = true;
    return 0;
}

struct run {
    const struct disk_plan *p;
    const struct disk_callbacks *cb;
    const void *loader;
    const char *step;
    unsigned phase;
    uint64_t done, total;
};
static void report(struct run *x)
{
    if (!x->cb || !x->cb->progress) return;
    struct disk_progress g = { x->phase, x->step, x->done, x->total };
    x->cb->progress(x->cb->ctx, &g);
}
static void fill(struct run *x, const struct disk_range *r, uint32_t first, uint32_t n, uint8_t *buf)
{
    if (r->zero) { memset(buf, 0, (size_t)n * DISK_SECTOR); return; }
    for (uint32_t i = 0; i < n; i++) disk_range_sector(x->p, r, first + i, x->loader, buf + (size_t)i * DISK_SECTOR);
}
static int write_range(struct run *x, const struct disk_range *r)
{
    for (uint32_t i = 0; i < r->count;) {
        if (x->cb && x->cb->cancel && x->cb->cancel(x->cb->ctx))
            return fail(DISK_ECANCELLED, 0, x->p->disk, r->lba + i, 0, x->step);
        uint32_t n = r->count - i < DISK_IO_SECTORS ? r->count - i : DISK_IO_SECTORS;
        fill(x, r, i, n, io_buffer);
        int e = raw_write(session.handle, x->p->disk, r->lba + i, n, io_buffer);
        if (e) return e;
        i += n; x->done += n; report(x);
    }
    return 0;
}
static int verify_range(struct run *x, const struct disk_range *r)
{
    for (uint32_t i = 0; i < r->count;) {
        if (x->cb && x->cb->cancel && x->cb->cancel(x->cb->ctx))
            return fail(DISK_ECANCELLED, 0, x->p->disk, r->lba + i, 0, x->step);
        uint32_t n = r->count - i < DISK_IO_SECTORS ? r->count - i : DISK_IO_SECTORS;
        int e = raw_read(session.handle, x->p->disk, r->lba + i, n, check_buffer);
        if (e) return e;
        fill(x, r, i, n, io_buffer);
        for (uint32_t k = 0; k < n; k++)
            if (memcmp(io_buffer + (size_t)k * DISK_SECTOR, check_buffer + (size_t)k * DISK_SECTOR, DISK_SECTOR))
                return fail(DISK_EIO, 0, x->p->disk, r->lba + i + k, 1, "read-back mismatch");
        i += n; x->done += n; report(x);
    }
    return 0;
}
static uint64_t phase_sectors(const struct disk_plan *p, unsigned phase)
{
    uint64_t n = 0;
    for (unsigned i = 0; i < p->ranges; i++) if (p->range[i].phase == phase) n += p->range[i].count;
    return n;
}
static int verify_phase(struct run *x)
{
    x->step = "read-back";
    for (unsigned i = 0; i < x->p->ranges; i++)
        if (x->p->range[i].phase == x->phase) { int e = verify_range(x, &x->p->range[i]); if (e) return e; }
    return 0;
}
static void set_phase(const char *text)
{
    struct ciuki_disk_lease l;
    memset(&l, 0, sizeof(l));
    l.version = CIUKI_DISK_VERSION; l.size = sizeof(l);
    copy_text(l.phase, sizeof(l.phase), text);
    (void)control(DISK_SET_PHASE, &l, sizeof(l)); /* diagnostics only */
}
static void end_session(void)
{
    const struct disk_backend *b = backend();
    if (session.leased) {
        struct ciuki_disk_request q = { CIUKI_DISK_VERSION, sizeof(q), 0, 0 };
        (void)control(DISK_LEASE_RELEASE, &q, sizeof(q));
    }
    if (session.open && b) b->close(b->ctx, session.handle);
    memset(&session, 0, sizeof(session));
}
static int same_identity(const struct disk_identity *a, const struct ciuki_disk_info *b)
{
    return !strncmp(a->model, b->model, sizeof(a->model)) && !strncmp(a->serial, b->serial, sizeof(a->serial)) &&
           !strncmp(a->firmware, b->firmware, sizeof(a->firmware));
}
/* Plan 5.3: open, acquire, then revalidate identity, capacity and table
 * fingerprint immediately; any difference releases the lease. */
static int begin_session(const struct disk_plan *p)
{
    const struct disk_backend *b = backend();
    if (!b) return fail(DISK_EINTERNAL, 0, p->disk, 0, 0, "no backend");
    if (session.open && session.disk != p->disk) return fail(DISK_EINTERNAL, EBUSY, p->disk, 0, 0, "another disk in use");
    if (!session.open) {
        int h = b->open(b->ctx, p->disk, true);
        if (h < 0) return fail(DISK_EINTERNAL, h, p->disk, 0, 0, "open");
        session.open = true; session.handle = h; session.disk = p->disk;
    }
    if (session.leased) return 0;
    struct ciuki_disk_info info;
    memset(&info, 0, sizeof(info));
    info.version = CIUKI_DISK_VERSION; info.size = sizeof(info);
    int r = control(DISK_LEASE_ACQUIRE, &info, sizeof(info));
    if (r) { end_session(); return fail(lease_code(r), r, p->disk, 0, 0, "DISK_LEASE_ACQUIRE"); }
    session.leased = true;
    const char *changed = !same_identity(&p->id, &info) ? "identity" :
        info.sectors != p->id.sectors || info.sector_size != p->id.sector_size ? "capacity" : 0;
    if (!changed) {
        memset(&scratch, 0, sizeof(scratch));
        scratch.disk = p->disk; scratch.id.sectors = info.sectors;
        uint8_t fingerprint[32];
        r = table_scan(session.handle, &scratch, fingerprint);
        if (r) { int code = last.code; struct disk_error_info keep = last; end_session(); last = keep; return code; }
        if (memcmp(fingerprint, p->id.table_fingerprint, sizeof(fingerprint))) changed = "partition table";
    }
    if (changed) {
        end_session();
        return fail(DISK_ECHANGED, 0, p->disk, 0, 0, changed);
    }
    return 0;
}
static int need_session(const struct disk_plan *p, bool published)
{
    if (!p || p->disk >= DISK_SLOTS) return fail(DISK_EINTERNAL, EINVAL, 0, 0, 0, "plan");
    if (!session.leased || session.disk != p->disk) return fail(DISK_ELEASE, 0, p->disk, 0, 0, "no lease");
    if (session.published != published) return fail(DISK_EINTERNAL, 0, p->disk, 0, 0, published ? "not published" : "already published");
    return 0;
}

int disk_execute_p1(const struct disk_plan *p, const struct disk_callbacks *cb)
{
    clear_error();
    if (!p || p->disk >= DISK_SLOTS || !p->ranges) return fail(DISK_EINTERNAL, EINVAL, 0, 0, 0, "plan");
    if (session.published) return fail(DISK_EINTERNAL, 0, p->disk, 0, 0, "already published");
    int e = begin_session(p);
    if (e) return e;
    set_phase("p1 partition");
    struct run x = { p, cb, 0, "partition", 1, 0, 2 * phase_sectors(p, 1) };
    for (unsigned i = 0; i < p->ranges; i++)
        if (p->range[i].phase == 1 && (e = write_range(&x, &p->range[i]))) return e;
    if ((e = flush_disk(p->disk))) return e;
    return verify_phase(&x);
}

static int bad_reasons(uint32_t reasons, uint32_t bad, unsigned disk, const char *what)
{
    if (!(reasons & bad)) return 0;
    fail(DISK_ECONSISTENCY, 0, disk, 0, 0, what);
    last.reasons = reasons;
    return DISK_ECONSISTENCY;
}

int disk_execute_p2(const struct disk_plan *p, const struct disk_callbacks *cb, char *letter)
{
    clear_error();
    int e = need_session(p, false);
    if (e) return e;
    const struct disk_backend *b = backend();
    set_phase("p2 format");
    struct run x = { p, cb, 0, "format", 2, 0, 2 * phase_sectors(p, 2) };
    for (unsigned i = 0; i < p->ranges; i++) {
        if (p->range[i].phase != 2) continue;
        if ((e = write_range(&x, &p->range[i]))) return e;
        /* f1-26 order: FAT copy 1 complete and durable before copy 2. */
        if (p->range[i].kind == DISK_R_FAT1_ZERO && (e = flush_disk(p->disk))) return e;
    }
    if ((e = flush_disk(p->disk)) || (e = verify_phase(&x))) return e;
    struct ciuki_disk_lease l;
    memset(&l, 0, sizeof(l));
    l.version = CIUKI_DISK_VERSION; l.size = sizeof(l);
    int r = control(DISK_PUBLISH, &l, sizeof(l));
    if (r) return fail(r == -EPERM || r == -EACCES ? DISK_ELEASE : r == -ENOMEM ? DISK_ENOMEM : DISK_ECONSISTENCY,
                       r, p->disk, 0, 0, "DISK_PUBLISH");
    session.published = true;
    session.letter = (char)('A' + l.drive);
    if ((e = bad_reasons(l.reasons, BAD_REASONS, p->disk, "publication mount"))) return e;
    r = b->mkdir(b->ctx, session.letter, "System");
    if (r && r != -EEXIST) return fail(DISK_EIO, r, p->disk, 0, 0, "mkdir SYSTEM");
    static const char state[] = "phase=format\n";
    r = b->write_file(b->ctx, session.letter, "SYSTEM/SETUP.STATE", state, sizeof(state) - 1);
    if (r) return fail(r == -ENOSPC ? DISK_EFULL : DISK_EIO, r, p->disk, 0, 0, "SETUP.STATE");
    if ((e = flush_disk(p->disk))) return e;
    if (letter) *letter = session.letter;
    return 0;
}

static int boot_cfg_check(const char *cfg)
{
    if (!cfg) return DISK_EINTERNAL;
    size_t n = strlen(cfg);
    if (n > 127) return DISK_EINTERNAL;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)cfg[i];
        if (c > 126 || (c < 32 && c != '\n' && c != '\r' && c != '\t')) return DISK_EINTERNAL;
        bool start = !i || cfg[i - 1] == ' ' || cfg[i - 1] == '\n' || cfg[i - 1] == '\r' || cfg[i - 1] == '\t';
        if (start && !strncmp(cfg + i, "setup=", 6)) return DISK_EINTERNAL; /* never on an installed disk */
    }
    return 0;
}

int disk_execute_p5(const struct disk_plan *p, const struct disk_publish *pub, const struct disk_callbacks *cb)
{
    clear_error();
    int e = need_session(p, true);
    if (e) return e;
    if (!pub || loader_check(pub->loader, pub->loader_bytes) || boot_cfg_check(pub->boot_cfg) ||
        !pub->setup_state || strncmp(pub->setup_state, "phase=installed", 15) || strlen(pub->setup_state) > 4096)
        return fail(DISK_EINTERNAL, EINVAL, p->disk, 0, 0, "publication arguments");
    const struct disk_plan *plan = p;
    if (p->loader_planned) {
        /* The dry run named these loader bytes; refuse anything else. */
        struct disk_range check = { 0 };
        for (unsigned i = 0; i < p->ranges; i++) if (p->range[i].kind == DISK_R_LOADER) check = p->range[i];
        uint8_t sha[32];
        memcpy(sha, check.sha256, sizeof(sha));
        hash_range(p, &check, pub->loader);
        if (check.count != pub->loader_bytes / DISK_SECTOR || memcmp(sha, check.sha256, sizeof(sha)))
            return fail(DISK_ECHANGED, 0, p->disk, DISK_LOADER_LBA, check.count, "loader differs from the plan");
    } else {
        p5_plan = *p;
        if ((e = disk_plan_publish(&p5_plan, pub->loader, pub->loader_bytes))) return e;
        plan = &p5_plan;
    }
    const struct disk_backend *b = backend();
    const struct disk_range *loader = 0, *mbr = 0;
    for (unsigned i = 0; i < plan->ranges; i++) {
        if (plan->range[i].kind == DISK_R_LOADER) loader = &plan->range[i];
        if (plan->range[i].kind == DISK_R_MBR) mbr = &plan->range[i];
    }
    set_phase("p5 publish");
    struct run x = { plan, cb, pub->loader, "loader", 5, 0, 2 * phase_sectors(plan, 5) };
    if ((e = write_range(&x, loader)) || (e = flush_disk(p->disk))) return e;
    x.step = "read-back";
    if ((e = verify_range(&x, loader))) return e;
    int r = b->write_file(b->ctx, session.letter, "SYSTEM/BOOT.CFG", pub->boot_cfg, (uint32_t)strlen(pub->boot_cfg));
    if (r) return fail(r == -ENOSPC ? DISK_EFULL : DISK_EIO, r, p->disk, 0, 0, "BOOT.CFG");
    r = b->write_file(b->ctx, session.letter, "SYSTEM/SETUP.STATE", pub->setup_state, (uint32_t)strlen(pub->setup_state));
    if (r) return fail(r == -ENOSPC ? DISK_EFULL : DISK_EIO, r, p->disk, 0, 0, "SETUP.STATE");
    /* Seal: the kernel's clean unmount sets FAT[1]'s clean bit (copy 1 ->
     * barrier -> copy 2), then remounts read-only with the full scan. */
    struct ciuki_disk_recheck q;
    memset(&q, 0, sizeof(q));
    q.version = CIUKI_DISK_VERSION; q.size = sizeof(q); q.flags = CIUKI_DISK_RECHECK_SEAL;
    r = control(DISK_RECHECK, &q, sizeof(q));
    if (r) return fail(r == -EPERM || r == -EACCES ? DISK_ELEASE : DISK_ECONSISTENCY, r, p->disk, 0, 0, "seal");
    /* After the clean unmount a DIRTY reason means the clean bit is absent. */
    if ((e = bad_reasons(q.reasons, BAD_REASONS | CIUKI_FAT_RO_DIRTY, p->disk, "seal scan"))) return e;
    if ((e = flush_disk(p->disk))) return e;
    for (unsigned copy = 0; copy < DISK_FATS; copy++) {
        uint64_t lba = p->geo.part_start + p->geo.reserved + (uint64_t)copy * p->geo.fat_sectors;
        uint8_t sector[DISK_SECTOR];
        if ((e = raw_read(session.handle, p->disk, lba, 1, sector))) return e;
        uint32_t one = disk_rd32(sector + 4);
        if ((one & (DISK_FAT32_CLEAN | DISK_FAT32_NO_ERROR)) != (DISK_FAT32_CLEAN | DISK_FAT32_NO_ERROR))
            return fail(DISK_ECONSISTENCY, 0, p->disk, lba, 1, "FAT[1] clean bit");
    }
    /* Last: the canonical MBR code over the stub; table and signature kept. */
    x.step = "mbr";
    if ((e = write_range(&x, mbr)) || (e = flush_disk(p->disk))) return e;
    x.step = "read-back";
    return verify_range(&x, mbr);
}

int disk_consistency_check(char letter, struct disk_check *out)
{
    clear_error();
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    if (!session.published || letter != session.letter)
        return fail(DISK_EINTERNAL, EINVAL, session.disk, 0, 0, "not the published volume");
    struct ciuki_disk_recheck q;
    memset(&q, 0, sizeof(q));
    q.version = CIUKI_DISK_VERSION; q.size = sizeof(q);
    int r = control(DISK_RECHECK, &q, sizeof(q));
    if (r) return fail(r == -EPERM || r == -EACCES ? DISK_ELEASE : DISK_ECONSISTENCY, r, session.disk, 0, 0, "DISK_RECHECK");
    if (out) {
        memset(out, 0, sizeof(*out));
        out->reasons = q.reasons; out->fat_type = q.fat_type; out->clusters = q.clusters;
        out->free_clusters = q.free_clusters; out->lost_clusters = q.lost_clusters;
        out->scan_passes = q.scan_passes; out->scan_ms = q.scan_ms; out->lfn_orphans = q.lfn_orphans;
        out->lfn_invalid = q.lfn_invalid; out->lfn_bad_checksum = q.lfn_bad_checksum;
    }
    return bad_reasons(q.reasons, BAD_REASONS, session.disk, "consistency check");
}

int disk_read_raw(unsigned disk, uint64_t lba, uint32_t count, void *buf)
{
    clear_error();
    const struct disk_backend *b = backend();
    if (!b || disk >= DISK_SLOTS || !buf) return fail(DISK_EINTERNAL, EINVAL, disk, lba, count, "raw read");
    bool own = !(session.open && session.disk == disk);
    int h = own ? b->open(b->ctx, disk, false) : session.handle;
    if (h < 0) return fail(DISK_EINTERNAL, h, disk, lba, count, "open");
    int e = raw_read(h, disk, lba, count, buf);
    if (own) b->close(b->ctx, h);
    return e;
}
int disk_flush(unsigned disk)
{
    clear_error();
    if (!session.leased || session.disk != disk) return fail(DISK_ELEASE, 0, disk, 0, 0, "no lease");
    return flush_disk(disk);
}
int disk_set_phase(unsigned disk, const char *text)
{
    clear_error();
    if (!session.leased || session.disk != disk) return fail(DISK_ELEASE, 0, disk, 0, 0, "no lease");
    set_phase(text);
    return 0;
}
int disk_release(unsigned disk)
{
    clear_error();
    if (!session.open || session.disk != disk) return 0;
    int r = 0;
    if (session.leased) {
        struct ciuki_disk_request q = { CIUKI_DISK_VERSION, sizeof(q), 0, 0 };
        r = control(DISK_LEASE_RELEASE, &q, sizeof(q));
        session.leased = false;
    }
    end_session();
    return r ? fail(DISK_ELEASE, r, disk, 0, 0, "DISK_LEASE_RELEASE") : 0;
}

#include "partition_tool.inc"
