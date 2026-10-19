/* SPDX-License-Identifier: MIT
 * POSIX adapter for the Files model (CiukiOS SDK: readdir over getdents,
 * stat, mkdir, rename, unlink, rmdir, open/read/write/fsync/close). */
#define _DEFAULT_SOURCE 1
#include "model.h"
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int metadata_stat(void *ctx, const char *path, struct fm_meta *m)
{
    (void)ctx;
    struct stat s;
    if (stat(path, &s)) return -errno;
    *m = (struct fm_meta){S_ISDIR(s.st_mode) ? FM_DIR : S_ISREG(s.st_mode) ? FM_FILE : FM_OTHER, (uint64_t)s.st_size,
                          (uint64_t)s.st_ino, (uint32_t)s.st_dev, (uint32_t)s.st_mode, (int64_t)s.st_mtime, 0};
    return 0;
}

/* Files without an extension are classified by their ELF32 i386 header. */
static int sniff(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    unsigned char h[20];
    ssize_t n = read(fd, h, sizeof(h));
    close(fd);
    return n == (ssize_t)sizeof(h) && !memcmp(h, "\177ELF\1\1\1", 7) && h[16] == 2 && h[18] == 3;
}

static int list(void *ctx, const char *path, unsigned flags, fm_emit emit, void *arg)
{
    DIR *dir = opendir(path);
    if (!dir) return -errno;
    int r = 0; unsigned sniffed = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) r = -errno; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char full[FM_PATH]; struct fm_meta meta = {.type=FM_OTHER, .ino=entry->d_ino};
#if defined(CIUKI_DIRENT_METADATA)
        meta.type = entry->d_type == DT_DIR ? FM_DIR : entry->d_type == DT_REG ? FM_FILE : FM_OTHER;
        meta.size = entry->d_size; meta.modified = entry->d_mtime;
#elif defined(DT_DIR)
        /* Portable host builds lack Ciuki's metadata extension. */
        meta.type = entry->d_type == DT_DIR ? FM_DIR : entry->d_type == DT_REG ? FM_FILE : FM_OTHER;
#endif
        if (flags & FM_LIST_SNIFF) {
            r = fm_join(full, path, entry->d_name);
            if (!r) r = metadata_stat(ctx, full, &meta);
        }
#if !defined(CIUKI_DIRENT_METADATA)
        else if (flags & FM_LIST_METADATA) {
            r = fm_join(full, path, entry->d_name);
            if (!r) r = metadata_stat(ctx, full, &meta);
        }
#endif
        if (r == -ENOENT) { r = 0; continue; }   /* removed during enumeration */
        if (!r && (flags & FM_LIST_SNIFF) && meta.type == FM_FILE && meta.size >= 52 && !strchr(entry->d_name, '.') && sniffed < 128) {
            sniffed++; meta.exec = (uint32_t)sniff(full);
        }
        if (!r) r = emit(entry->d_name, &meta, arg);
        if (r) break;
    }
    if (closedir(dir) && !r) r = -errno;
    return r;
}

static int make_dir(void *ctx, const char *p) { (void)ctx; return mkdir(p, 0777) ? -errno : 0; }
static int move(void *ctx, const char *a, const char *b) { (void)ctx; return rename(a, b) ? -errno : 0; }
static int remove_file(void *ctx, const char *p) { (void)ctx; return unlink(p) ? -errno : 0; }
static int remove_dir(void *ctx, const char *p) { (void)ctx; return rmdir(p) ? -errno : 0; }
static int open_file(void *ctx, const char *p, int create)
{ (void)ctx; int fd = open(p, create ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY, 0666); return fd < 0 ? -errno : fd; }
static int read_file(void *ctx, int fd, void *p, unsigned n) { (void)ctx; int r = (int)read(fd, p, n); return r < 0 ? -errno : r; }
static int write_file(void *ctx, int fd, const void *p, unsigned n) { (void)ctx; int r = (int)write(fd, p, n); return r < 0 ? -errno : r; }
static int sync_file(void *ctx, int fd) { (void)ctx; return fsync(fd) ? -errno : 0; }
static int close_file(void *ctx, int fd) { (void)ctx; return close(fd) ? -errno : 0; }

const struct fm_vfs *fm_native_vfs(void)
{
    static const struct fm_vfs fs = {NULL, list, metadata_stat, make_dir, move, remove_file, remove_dir,
                                     open_file, read_file, write_file, sync_file, close_file, NULL};
    return &fs;
}
