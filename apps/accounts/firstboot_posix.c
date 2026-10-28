/* SPDX-License-Identifier: MIT
 * POSIX binding of the first-boot adapter's G32 settings access
 * (/System/Settings/users.dat, users.new, users.lock). The ACCOUNT image
 * reads the shared G32 files once to protect them as migration evidence and
 * then removes them; no other path is reachable through this table.
 */
#define _POSIX_C_SOURCE 200809L
#include "firstboot.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LEGACY_DIRECTORY "/System/Settings"

static int legacy_path(const char *name, char out[96])
{
    if (strcmp(name, "users.dat") && strcmp(name, "users.new") && strcmp(name, "users.lock")) return -EINVAL;
    int n = snprintf(out, 96, "%s/%s", LEGACY_DIRECTORY, name);
    return n < 0 || n >= 96 ? -ENAMETOOLONG : 0;
}
static int legacy_read(void *ctx, const char *name, void *buffer, size_t capacity, size_t *bytes)
{
    (void)ctx; char path[96]; int e = legacy_path(name, path); if (e) return e;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    size_t total = 0;
    while (total < capacity) {
        ssize_t n = read(fd, (char *)buffer + total, capacity - total);
        if (n < 0) { if (errno == EINTR) continue; e = -errno; break; }
        if (!n) break;
        total += (size_t)n;
    }
    if (close(fd) && !e) e = -errno;
    if (!e) *bytes = total;
    return e;
}
static int legacy_remove(void *ctx, const char *name)
{
    (void)ctx; char path[96]; int e = legacy_path(name, path); if (e) return e;
    return unlink(path) ? -errno : 0;
}
static int legacy_barrier(void *ctx)
{
    (void)ctx;
    int fd = open(LEGACY_DIRECTORY, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -errno;
    int e = fsync(fd) ? -errno : 0;
    if (close(fd) && !e) e = -errno;
    return e;
}
int acct_legacy_io_posix(struct acct_legacy_io *out)
{
    if (!out) return -EINVAL;
    *out = (struct acct_legacy_io){ .ctx = NULL, .read = legacy_read, .remove = legacy_remove, .barrier = legacy_barrier };
    return 0;
}
