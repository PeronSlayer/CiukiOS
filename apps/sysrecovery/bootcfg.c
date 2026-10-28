/* SPDX-License-Identifier: MIT
 * G14 System Configuration's Advanced options in BOOT.CFG (the loader's
 * exact tokens, docs/boot.md and docs/design/startup-recovery.md 2.2): menu=1
 * ("Enable Startup menu"), autoscan=0/2 (Msdos.sys AutoScan) and serial=0/1.
 * Every other token and line (probe=, mode=, accel=0, setup=1) is kept in
 * order; the file stays within the loader's 127 bytes. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "recovery.h"

static int ours(const char *t, size_t n)
{
    static const char *const tokens[] = { "menu=1", "autoscan=0", "autoscan=2", "serial=0", "serial=1" };
    for (unsigned i = 0; i < 5; i++) if (strlen(tokens[i]) == n && !memcmp(t, tokens[i], n)) return 1;
    return 0;
}
void bootcfg_read(const char *in, size_t n, struct bootcfg_options *o)
{
    o->menu = 0; o->autoscan = 1; o->serial = 1;
    for (size_t i = 0; i < n;) {
        while (i < n && (in[i] == ' ' || in[i] == '\r' || in[i] == '\n')) i++;
        size_t s = i;
        while (i < n && in[i] != ' ' && in[i] != '\r' && in[i] != '\n') i++;
        size_t len = i - s;
        if (len == 6 && !memcmp(in + s, "menu=1", 6)) o->menu = 1;
        else if (len == 10 && !memcmp(in + s, "autoscan=", 9)) o->autoscan = in[s + 9] == '0' ? 0 : in[s + 9] == '2' ? 2 : o->autoscan;
        else if (len == 8 && !memcmp(in + s, "serial=", 7)) o->serial = in[s + 7] == '1';
    }
}
int bootcfg_apply(const char *in, size_t n, const struct bootcfg_options *o, char out[BOOTCFG_MAX + 1], size_t *outn)
{
    size_t used = 0;
    for (size_t i = 0; i < n;) {
        size_t s = i;
        while (i < n && in[i] != ' ' && in[i] != '\r' && in[i] != '\n') i++;
        size_t len = i - s;
        while (i < n && (in[i] == ' ' || in[i] == '\r' || in[i] == '\n')) i++;
        if (len && ours(in + s, len)) continue;     /* its separator goes with it */
        size_t take = i - s;
        if (used + take > BOOTCFG_MAX) return -EFBIG;
        memcpy(out + used, in + s, take); used += take;
    }
    if (used && out[used - 1] != '\n' && out[used - 1] != ' ' && out[used - 1] != '\r') {
        if (used + 1 > BOOTCFG_MAX) return -EFBIG;
        out[used++] = ' ';
    }
    char extra[48];
    int k = snprintf(extra, sizeof(extra), "%s%s%s\n", o->serial ? "serial=1" : "serial=0",
                     o->menu ? " menu=1" : "", o->autoscan == 0 ? " autoscan=0" : o->autoscan == 2 ? " autoscan=2" : "");
    if (k < 0 || used + (size_t)k > BOOTCFG_MAX) return -EFBIG;
    memcpy(out + used, extra, (size_t)k); used += (size_t)k;
    out[used] = 0; *outn = used;
    return 0;
}
int bootcfg_update(const char *root, const struct bootcfg_options *o)
{
    char path[512], temp[530], old[530], in[BOOTCFG_MAX + 2], out[BOOTCFG_MAX + 1];
    int n = snprintf(path, sizeof(path), "%s/system/BOOT.CFG", root ? root : "");
    if (n < 0 || (size_t)n >= sizeof(path)) return -ENAMETOOLONG;
    FILE *f = fopen(path, "rb");
    size_t size = 0;
    if (f) { size = fread(in, 1, sizeof(in), f); fclose(f); }
    else if (errno != ENOENT) return -errno;
    if (size > BOOTCFG_MAX) return -EFBIG;
    size_t length;
    int e = bootcfg_apply(in, size, o, out, &length);
    if (e) return e;
    /* sec-sysvc: BOOT.CFG is sealed; the SYSTEM service saves it. */
    if (!sr_boot_config(root, out, length, &e)) return e;
    snprintf(temp, sizeof(temp), "%s.new", path); snprintf(old, sizeof(old), "%s.old", path);
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    if (write(fd, out, length) != (ssize_t)length) e = -EIO;
    if (!e && fsync(fd)) e = -errno;
    if (close(fd) && !e) e = -errno;
    if (e) { unlink(temp); return e; }
    unlink(old);
    if (rename(path, old) && errno != ENOENT) { e = -errno; unlink(temp); return e; }
    if (rename(temp, path)) { e = -errno; rename(old, path); return e; }
    unlink(old);
    return 0;
}
