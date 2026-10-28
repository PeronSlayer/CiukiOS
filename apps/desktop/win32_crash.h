/* Text-only crash persistence. SPDX-License-Identifier: MIT */
#ifndef DESK_W32_CRASH_H
#define DESK_W32_CRASH_H
#include <stddef.h>
#include <ciuki/w32_diagnostics.h>
/* The eight files <prefix>0.TXT..<prefix>7.TXT: the oldest (or a missing)
 * one takes the next sequence. head reads a file's first bytes. */
struct desk_crash_io {
    void *ctx;
    int (*head)(void *ctx, const char *name, char *out, size_t cap, size_t *got);
    int (*replace)(void *ctx, const char *name, const char *text, size_t n);
};
int desk_w32_crash_write(const struct desk_crash_io *, const char *prefix, const struct ciuki_w32_fault *);
/* CRASHn.TXT in directory (created when missing). */
int desk_w32_crash_store(const char *, const struct ciuki_w32_fault *);
/* sec-sysvc2: <prefix>n.TXT in directory, the service's names (CRASH native,
 * WCRASH Win32) for a profile's log folder without the log service. */
int desk_w32_crash_store_as(const char *directory, const char *prefix, const struct ciuki_w32_fault *);
/* sec-sysvc: the faulting user's own logs through the SYSTEM log service:
 * CRASHn.TXT (native), WCRASHn.TXT (Win32). -ENOSYS: an older kernel. */
int desk_w32_crash_log(const struct ciuki_w32_fault *);
#endif
