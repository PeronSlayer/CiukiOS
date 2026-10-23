/* Host file helpers for libdisk_test.c. Kept apart from <ciuki/abi.h>,
 * whose O_* and errno values are the CiukiOS ABI, not the host's.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int hf_create(const char *path, uint64_t bytes)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int e = ftruncate(fd, (off_t)bytes);
    close(fd);
    return e ? -1 : 0;
}
int hf_read(const char *path, uint64_t offset, void *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = pread(fd, buf, n, (off_t)offset);
    close(fd);
    return r == (ssize_t)n ? 0 : -1;
}
int hf_write(const char *path, uint64_t offset, const void *buf, size_t n)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t r = pwrite(fd, buf, n, (off_t)offset);
    close(fd);
    return r == (ssize_t)n ? 0 : -1;
}
long hf_size(const char *path)
{
    struct stat st;
    return stat(path, &st) ? -1 : (long)(st.st_size / 512);
}
/* True when any byte at or after offset is allocated (SEEK_DATA). */
bool hf_data_beyond(const char *path, uint64_t offset)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return true;
    off_t at = lseek(fd, (off_t)offset, SEEK_DATA);
    bool data = false;
    while (at >= 0 && !data) {
        unsigned char block[4096];
        ssize_t r = pread(fd, block, sizeof(block), at);
        if (r <= 0) break;
        for (ssize_t i = 0; i < r; i++) if (block[i]) { data = true; break; }
        at += r;
        at = lseek(fd, at, SEEK_DATA);
    }
    close(fd);
    return data;
}
/* Runs a shell command; returns its exit status (or -1) and the combined
 * output, NUL-terminated. */
int hf_run(const char *command, char *out, size_t cap)
{
    char full[2048];
    snprintf(full, sizeof(full), "%s 2>&1", command);
    FILE *p = popen(full, "r");
    if (!p) return -1;
    size_t n = fread(out, 1, cap ? cap - 1 : 0, p);
    if (cap) out[n] = 0;
    int status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
int hf_remove(const char *path) { return unlink(path); }
