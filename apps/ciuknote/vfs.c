/* SPDX-License-Identifier: MIT
 * CiukNote's POSIX file boundary. Saving writes a sibling temporary file,
 * flushes it with fsync and replaces the target by rename, so a failure never
 * leaves a half-written document. */
#include "doc.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int file_stat(void *ctx, const char *path, struct cn_stat *st)
{
    (void)ctx; struct stat s;
    if (stat(path, &s)) return -errno;
    *st = (struct cn_stat){S_ISDIR(s.st_mode) != 0, (uint64_t)s.st_size, (int64_t)s.st_mtime};
    return 0;
}
static int file_list(void *ctx, const char *path, cn_emit emit, void *arg)
{
    DIR *dir = opendir(path);
    if (!dir) return -errno;
    int r = 0; struct dirent *entry;
    for (;;) {
        errno = 0; entry = readdir(dir);
        if (!entry) { if (errno) r = -errno; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char full[CN_PATH]; struct cn_stat st;
        r = cn_join(full, path, entry->d_name);
        if (!r) r = file_stat(ctx, full, &st);
        if (r == -ENOENT) { r = 0; continue; } /* removed while listing */
        if (!r) r = emit(entry->d_name, &st, arg);
        if (r) break;
    }
    if (closedir(dir) && !r) r = -errno;
    return r;
}
static int file_load(void *ctx, const char *path, char *buf, size_t cap, size_t *len)
{
    (void)ctx;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    size_t n = 0; int r = 0;
    for (;;) {
        ssize_t k = read(fd, buf + n, cap - n);
        if (k < 0) { if (errno == EINTR) continue; r = -errno; break; }
        if (!k) break;
        n += (size_t)k;
        if (n == cap) { /* exactly full: any further byte means the file is too large */
            char extra; ssize_t more;
            do more = read(fd, &extra, 1); while (more < 0 && errno == EINTR);
            if (more < 0) r = -errno; else if (more > 0) r = -EFBIG;
            break;
        }
    }
    if (close(fd) && !r) r = -errno;
    if (!r) *len = n;
    return r;
}
static int write_all(int fd, const char *data, size_t n)
{
    while (n) {
        ssize_t k = write(fd, data, n);
        if (k < 0) { if (errno == EINTR) continue; return -errno; }
        if (!k) return -EIO;
        data += k; n -= (size_t)k;
    }
    return 0;
}
static int file_save(void *ctx, const char *path, const char *data, size_t len)
{
    (void)ctx;
    char temporary[CN_PATH + 8];
    if (strlen(path) + 5 >= sizeof(temporary) || strlen(path) + 5 >= CN_PATH) return -ENAMETOOLONG;
    snprintf(temporary, sizeof(temporary), "%s.cn~", path);
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -errno;
    int r = write_all(fd, data, len);
    if (!r && fsync(fd)) r = -errno;
    if (close(fd) && !r) r = -errno;
    if (!r && rename(temporary, path)) r = -errno;
    if (r) unlink(temporary);
    return r;
}
const struct cn_fs *cn_native_fs(void)
{
    static const struct cn_fs fs = {NULL, file_stat, file_list, file_load, file_save};
    return &fs;
}
