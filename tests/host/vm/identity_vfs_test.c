/* NETID publication/reopen through production FAT/VFS on in-memory sectors.
 * No boot image, file-backed disk copy or guest execution.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/net_bridge_port.h>
#include "../../../src/kernel/fs/vfs.h"
#include <stdio.h>
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %s:%u: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned checks, disk_writes, disk_flushes;
static uint8_t *sectors;
static struct block_cache cache;
static struct fat_volume volume;
static struct vfs space;
static struct vfs_table table;
static int read_blocks(struct blkdev *d, uint64_t lba, uint32_t n, void *out)
{
    CHECK(!blkdev_range(d, lba, n)); memcpy(out, sectors + lba * 512, n * 512); return 0;
}
static int write_blocks(struct blkdev *d, uint64_t lba, uint32_t n, const void *data)
{
    CHECK(!blkdev_range(d, lba, n)); memcpy(sectors + lba * 512, data, n * 512); disk_writes++; return 0;
}
static int flush_blocks(struct blkdev *d) { (void)d; disk_flushes++; return 0; }
static struct blkdev disk = {.read = read_blocks, .write = write_blocks, .flush = flush_blocks,
    .capacity = 8192, .sector_size = 512, .write_cache_state = BLKDEV_CACHE_ENABLED};
static void mount_disk(bool writable)
{
    CHECK(!cache_init(&cache, 16384));
    CHECK(!fat_mount(&volume, &cache, &disk, 0, disk.capacity, writable));
    CHECK(volume.type == 16 && volume.readonly != writable);
    vfs_init(&space); CHECK(!vfs_attach(&space, 2, &volume)); CHECK(!vfs_table_init(&space, &table, 2));
}
static void unmount_disk(void)
{
    vfs_table_destroy(&table); CHECK(!vfs_detach(&space, 2)); vfs_destroy(&space); cache_destroy(&cache);
}
int main(void)
{
    sectors = calloc(8192, 512); CHECK(sectors);
    sectors[0] = 0xeb; sectors[2] = 0x90; fs_wr16(sectors + 11, 512); sectors[13] = 1;
    fs_wr16(sectors + 14, 1); sectors[16] = 2; fs_wr16(sectors + 17, 64);
    fs_wr16(sectors + 19, 8192); sectors[21] = 0xf8; fs_wr16(sectors + 22, 32);
    fs_wr16(sectors + 510, 0xaa55);
    for (unsigned i = 0; i < 2; i++) {
        uint8_t *fat = sectors + (1 + i * 32) * 512;
        fs_wr16(fat, 0xfff8); fs_wr16(fat + 2, 0xffff);
    }
    const uint8_t id[16] = {0x42,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    uint8_t out[16], zero[16] = {0}; struct fat_entry entry;
    mount_disk(true); CHECK(!vfs_mkdir(&table, "C:\\System", 0));
    CHECK(net_bridge_identity_read_file(&table, out) == -FS_ENOENT);
    CHECK(!net_bridge_identity_write_file(&table, id)); CHECK(disk_flushes && disk_writes);
    CHECK(!vfs_stat(&table, "C:\\System\\NETID.BIN", &entry) && entry.size == 16);
    CHECK(vfs_stat(&table, "C:\\System\\NETID.TMP", &entry) == -FS_ENOENT);
    CHECK(!net_bridge_identity_read_file(&table, out) && !memcmp(id, out, 16));
    CHECK(net_bridge_identity_write_file(&table, id) == -FS_EEXIST);
    CHECK(!net_bridge_identity_read_file(&table, out) && !memcmp(id, out, 16));
    unmount_disk(); mount_disk(false);
    unsigned writes = disk_writes, flushes = disk_flushes;
    CHECK(!net_bridge_identity_read_file(&table, out) && !memcmp(id, out, 16));
    CHECK(disk_writes == writes && disk_flushes == flushes);
    unmount_disk(); mount_disk(true);
    CHECK(!vfs_delete(&table, "C:\\System\\NETID.BIN"));
    /* A committed candidate left by interrupted provisioning is reused. */
    int h = vfs_open(&table, "C:\\System\\NETID.TMP", VFS_WRITE | VFS_TRUNCATE, VFS_DENY_ALL, 0);
    CHECK(h >= 0); size_t done;
    CHECK(!vfs_write(&table, h, id, 16, &done) && done == 16);
    CHECK(!vfs_commit(&table, h) && !vfs_close(&table, h));
    unmount_disk(); mount_disk(true);
    CHECK(!net_bridge_identity_read_file(&table, out) && !memcmp(id, out, 16));
    CHECK(vfs_stat(&table, "C:\\System\\NETID.TMP", &entry) == -FS_ENOENT);
    h = vfs_open(&table, "C:\\System\\NETID.BIN", VFS_WRITE | VFS_TRUNCATE, VFS_DENY_ALL, 0);
    CHECK(h >= 0); CHECK(!vfs_write(&table, h, zero, 16, &done) && done == 16);
    CHECK(!vfs_commit(&table, h) && !vfs_close(&table, h));
    writes = disk_writes;
    CHECK(net_bridge_identity_read_file(&table, out) == -FS_EINVAL && !memcmp(out, zero, 16));
    CHECK(disk_writes == writes);
    unmount_disk(); free(sectors);
    CHECK(!fs_host_contig_live);
    printf("NETID production FAT/VFS: %u checks passed\n", checks); return 0;
}
