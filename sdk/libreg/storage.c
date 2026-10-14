/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int reg_file_read(const char *path, char **out, size_t *size)
{
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    char *text = malloc(REG_TEXT_MAX + 2); if (!text) { fclose(f); return -1; }
    size_t n = fread(text, 1, REG_TEXT_MAX + 1, f);
    int error = ferror(f) ? (errno ? errno : EIO) : n > REG_TEXT_MAX ? E2BIG : 0;
    if (fclose(f) && !error) error = errno;
    if (error) { free(text); return reg_fail(error); }
    text[n] = 0; *out = text; *size = n; return 0;
}
static char *suffix(const char *path, const char *extra)
{
    size_t n = strlen(path) + strlen(extra) + 1; char *s = malloc(n);
    if (s) snprintf(s, n, "%s%s", path, extra); return s;
}
static uint32_t checksum(const char *s, size_t n)
{
    uint32_t v = 2166136261u; for (size_t i = 0; i < n; i++) v = (v ^ (unsigned char)s[i]) * 16777619u; return v;
}
static int durable(const char *path, const char *text, size_t n)
{
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    int error = 0;
    if (fwrite(text, 1, n, f) != n || fflush(f) || fsync(fileno(f))) error = errno ? errno : EIO;
    if (fclose(f) && !error) error = errno;
    return error ? reg_fail(error) : 0;
}
static int sync_directory(const char *path)
{
#ifndef __CIUKIOS__
    char *directory = strdup(path); if (!directory) return -1;
    char *slash = strrchr(directory, '/'); if (slash == directory) slash[1] = 0; else if (slash) *slash = 0;
    int fd = open(directory, O_RDONLY | O_CLOEXEC); free(directory); if (fd < 0) return -1;
    int result = fsync(fd), error = errno; if (close(fd) && !result) { result = -1; error = errno; }
    if (result) return reg_fail(error);
#else
    (void)path; /* Native file FSYNC already flushes volume metadata. */
#endif
    return 0;
}
static int sync_installed(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    int result = fsync(fd), error = errno; if (close(fd) && !result) { result = -1; error = errno; }
    return result ? reg_fail(error) : sync_directory(path);
}
static int exists(const char *path)
{
    struct stat s; if (!stat(path, &s)) return 1; return errno == ENOENT || errno == E2BIG ? 0 : -1;
}
static int remove_if_present(const char *path) { return !unlink(path) || errno == ENOENT ? 0 : -1; }
static int install(const char *path, const char *temp, const char *backup)
{
    if (!rename(temp, path)) return sync_installed(path);
    if (errno != EEXIST) return -1;
    int old = exists(backup); if (old < 0) return -1; if (old) return reg_fail(EIO);
    if (rename(path, backup)) return -1;
    if (rename(temp, path)) return -1; /* Intent and backup make this recoverable. */
    return sync_installed(path);
}
static int candidate(const char *path, const char *root, uint64_t gen, size_t length, uint32_t hash, int exact)
{
    char *text; size_t n;
    if (reg_file_read(path, &text, &n)) return errno == ENOENT || errno == E2BIG ? 0 : -1;
    struct rt *t = reg_parse(root, text, n);
    int valid = t && (!exact || (t->generation == gen && n == length && checksum(text, n) == hash));
    reg_tree_free(t); free(text); return valid;
}
int reg_recover(const char *path, const char *root)
{
    char *temp = suffix(path, ".new"), *backup = suffix(path, ".bak"), *journal = suffix(path, ".txn");
    if (!temp || !backup || !journal) { free(temp); free(backup); free(journal); return -1; }
    int result = 0; char *intent = NULL; size_t n;
    if (reg_file_read(journal, &intent, &n)) {
        if (errno != ENOENT) { result = -1; goto done; }
        int old = exists(backup); if (old < 0) { result = -1; goto done; }
        if (old) {
            int current = candidate(path, root, 0, 0, 0, 0);
            if (current < 0) { result = -1; goto done; }
            if (!current) {
                int saved = candidate(backup, root, 0, 0, 0, 0);
                if (saved <= 0 || exists(path) != 0) { result = reg_fail(EIO); goto done; }
                if (rename(backup, path) || sync_installed(path)) { result = -1; goto done; }
            }
        }
        /* An intent-free partial temp is never a commit. */
        if (remove_if_present(temp) || remove_if_present(backup)) result = -1;
        goto done;
    }
    unsigned long long generation, length; unsigned hash; char extra;
    if (memchr(intent, 0, n) || sscanf(intent, "ciuki-registry %llu %llu %x %c", &generation, &length, &hash, &extra) != 3 ||
        generation == UINT64_MAX || length > REG_TEXT_MAX) { result = reg_fail(EIO); goto done; }
    int current = candidate(path, root, generation, (size_t)length, hash, 1);
    if (current < 0) { result = -1; goto done; }
    if (current) { if (sync_installed(path)) { result = -1; goto done; } }
    else {
        int pending = candidate(temp, root, generation, (size_t)length, hash, 1);
        if (pending < 0) { result = -1; goto done; }
        if (pending) {
            if (install(path, temp, backup)) { result = -1; goto done; }
        } else {
            /* No valid new snapshot: preserve canonical or restore backup. */
            int canonical = candidate(path, root, 0, 0, 0, 0);
            if (canonical < 0) { result = -1; goto done; }
            if (!canonical) {
                int saved = candidate(backup, root, 0, 0, 0, 0);
                if (saved <= 0 || exists(path) != 0) { result = reg_fail(EIO); goto done; }
                if (rename(backup, path) || sync_installed(path)) { result = -1; goto done; }
            }
        }
    }
    if (remove_if_present(journal) || remove_if_present(temp) || remove_if_present(backup)) result = -1;
done: { int error = errno; free(intent); free(temp); free(backup); free(journal); errno = error; return result; }
}
int reg_file_write(const char *path, const char *root, const char *text, size_t n, uint64_t generation)
{
    if (reg_recover(path, root)) return -1;
    char *temp = suffix(path, ".new"), *backup = suffix(path, ".bak"), *journal = suffix(path, ".txn");
    if (!temp || !backup || !journal) { free(temp); free(backup); free(journal); return -1; }
    char intent[128]; int count = snprintf(intent, sizeof(intent), "ciuki-registry %llu %llu %08x\n",
                                          (unsigned long long)generation, (unsigned long long)n, checksum(text, n));
    int result = durable(temp, text, n);
    if (!result) {
        /* Intent write/sync failure precedes any rename: discard the staging
         * files while retaining the old canonical root. Later installation
         * failures keep the durable intent and snapshot for recovery. */
        result = durable(journal, intent, (size_t)count);
        if (result) { int error = errno; remove_if_present(journal); remove_if_present(temp); errno = error; }
    } else { int error = errno; remove_if_present(temp); errno = error; }
    if (!result) result = sync_directory(path); /* Durable intent entry before moving the old root. */
    if (!result) result = install(path, temp, backup);
    if (!result && (remove_if_present(journal) || remove_if_present(backup))) result = -1;
    int error = errno; free(temp); free(backup); free(journal); errno = error; return result;
}
