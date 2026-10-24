/* G14 host harness for the ScanDisk engine: a file-backed partition device
 * with injectable unreadable sectors. Usage:
 *   scandisk_tool IMAGE [fix] [thorough] [free] [nocopy] [bad=LBA ...]
 * Prints one "key=value" line per report field.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../../../src/kernel/fs/scandisk.h"

extern unsigned fs_host_contig_live; /* fs_port.c */
static int fd;
static uint64_t bad[64]; static unsigned nbad;
static unsigned writes;
static int rd(struct blkdev *d, uint64_t lba, uint32_t n, void *b)
{
    if (blkdev_range(d, lba, n)) return -FS_EINVAL;
    for (unsigned i = 0; i < nbad; i++) if (bad[i] >= lba && bad[i] < lba + n) return -FS_EIO;
    return pread(fd, b, (size_t)n * 512, (off_t)(lba * 512)) == (ssize_t)n * 512 ? 0 : -FS_EIO;
}
static int wr(struct blkdev *d, uint64_t lba, uint32_t n, const void *b)
{
    if (blkdev_range(d, lba, n)) return -FS_EINVAL;
    writes++;
    return pwrite(fd, b, (size_t)n * 512, (off_t)(lba * 512)) == (ssize_t)n * 512 ? 0 : -FS_EIO;
}
static int fl(struct blkdev *d) { (void)d; return fsync(fd) ? -FS_EIO : 0; }

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    fd = open(argv[1], O_RDWR);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { perror(argv[1]); return 2; }
    struct blkdev dev = { .read = rd, .write = wr, .flush = fl, .capacity = (uint64_t)st.st_size / 512,
                          .sector_size = 512, .write_cache_state = BLKDEV_CACHE_ENABLED };
    struct ciuki_scandisk r; memset(&r, 0, sizeof(r));
    r.drive = 3;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "fix")) r.flags |= CIUKI_SCANDISK_FIX;
        else if (!strcmp(argv[i], "thorough")) r.flags |= CIUKI_SCANDISK_THOROUGH;
        else if (!strcmp(argv[i], "free")) r.flags |= CIUKI_SCANDISK_FREE_LOST;
        else if (!strcmp(argv[i], "nocopy")) r.flags |= CIUKI_SCANDISK_NO_COPY;
        else if (!strncmp(argv[i], "bad=", 4) && nbad < 64) bad[nbad++] = strtoull(argv[i] + 4, 0, 0);
        else return 2;
    }
    int e = scandisk_run(&dev, &r);
#define F(name) printf(#name "=%u\n", (unsigned)r.name)
    printf("result=%d\nwrites=%u\nleak=%u\n", e, writes, fs_host_contig_live);
    F(flags); F(fat_type); F(cluster_bytes); F(clusters); F(free_clusters); F(bad_clusters);
    F(directories); F(files); F(directory_clusters); F(file_clusters); F(lost_chains); F(lost_clusters);
    F(salvaged_files); F(crosslinked_files); F(crosslinks_fixed); F(invalid_chains); F(size_mismatches);
    F(invalid_entries); F(fat_mismatches); F(excess_tails); F(surface_sectors); F(bad_sectors);
    F(bad_marked); F(relocated); F(unreadable_system); F(version); F(size);
    printf("found=%s\nfirst_problem=%s\n", r.found, r.first_problem);
    close(fd);
    return 0;
}
