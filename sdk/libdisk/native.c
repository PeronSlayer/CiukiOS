/* Native CiukiOS backend of libdisk: the raw node /dev/ataN through open,
 * pread, pwrite and ciuki_devctl, and the published volume through the
 * native namespace /mnt/<letter>/... with fsync before every close.
 * SPDX-License-Identifier: MIT */
#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <ciuki/entropy.h>

int ciuki_devctl(int fd, uint32_t op, void *record, uint32_t size);

static int n_open(void *ctx, unsigned disk, bool write)
{
    (void)ctx;
    char path[] = "/dev/ata0";
    path[8] = (char)('0' + disk);
    int fd = open(path, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}
static void n_close(void *ctx, int handle) { (void)ctx; close(handle); }
static int n_pread(void *ctx, int handle, void *buf, uint32_t bytes, uint64_t offset)
{
    (void)ctx;
    ssize_t r = pread(handle, buf, bytes, (off_t)offset);
    return r < 0 ? -errno : (int)r;
}
static int n_pwrite(void *ctx, int handle, const void *buf, uint32_t bytes, uint64_t offset)
{
    (void)ctx;
    ssize_t r = pwrite(handle, buf, bytes, (off_t)offset);
    return r < 0 ? -errno : (int)r;
}
static int n_devctl(void *ctx, int handle, uint32_t op, void *record, uint32_t size)
{
    (void)ctx;
    return ciuki_devctl(handle, op, record, size) < 0 ? -errno : 0;
}
static int path_of(char *out, size_t cap, char letter, const char *rel)
{
    size_t n = strlen(rel);
    if (n + 8 > cap) return -ENAMETOOLONG;
    memcpy(out, "/mnt/X/", 7);
    out[5] = letter;
    memcpy(out + 7, rel, n + 1);
    return 0;
}
static int n_mkdir(void *ctx, char letter, const char *rel)
{
    (void)ctx;
    char path[128];
    int e = path_of(path, sizeof(path), letter, rel);
    if (e) return e;
    return mkdir(path, 0777) ? -errno : 0;
}
static int n_write_file(void *ctx, char letter, const char *rel, const void *data, uint32_t bytes)
{
    (void)ctx;
    char path[128];
    int e = path_of(path, sizeof(path), letter, rel);
    if (e) return e;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) return -errno;
    const uint8_t *p = data;
    while (!e && bytes) {
        ssize_t r = write(fd, p, bytes);
        if (r <= 0) e = r < 0 ? -errno : -EIO;
        else { p += r; bytes -= (uint32_t)r; }
    }
    if (!e && fsync(fd)) e = -errno;
    if (close(fd) && !e) e = -errno;
    return e;
}
static int n_read_file(void *ctx, char letter, const char *rel, void *data, uint32_t capacity)
{
    (void)ctx;
    char path[128];
    int e = path_of(path, sizeof(path), letter, rel);
    if (e) return e;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    ssize_t r = read(fd, data, capacity);
    e = r < 0 ? -errno : (int)r;
    close(fd);
    return e;
}
static int n_entropy(void *ctx, void *buf, uint32_t bytes)
{
    (void)ctx;
    return ciuki_getentropy_wait(buf, bytes) ? -errno : 0;
}
static void n_now(void *ctx, uint16_t *date, uint16_t *time_of_day)
{
    (void)ctx;
    time_t now = time(0);
    struct tm tm;
    if (now == (time_t)-1 || !gmtime_r(&now, &tm) || tm.tm_year < 80 || tm.tm_year > 207) return;
    *date = (uint16_t)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
    *time_of_day = (uint16_t)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
}
static const struct disk_backend native = {
    0, n_open, n_close, n_pread, n_pwrite, n_devctl, n_mkdir, n_write_file, n_read_file, n_entropy, n_now
};
const struct disk_backend *disk_native_backend(void) { return &native; }
