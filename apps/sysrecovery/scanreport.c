/* SPDX-License-Identifier: MIT
 * G14 ScanDisk report text (docs/design/startup-recovery.md 2.4): the
 * summary and statistics of Windows 98's "ScanDisk Results" window, the
 * details of what was found, and SCANDISK.LOG. */
#define _POSIX_C_SOURCE 200809L
/* System headers first: abi.h then leaves the C library's macros alone. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "recovery.h"

/* 1234567 -> "1,234,567" */
static const char *grouped(uint64_t v, char out[32])
{
    char digits[24]; int n = snprintf(digits, sizeof(digits), "%llu", (unsigned long long)v), o = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = 0;
    return out;
}
struct sink { char *out; size_t cap, used; };
static void put(struct sink *s, const char *f, ...)
{
    va_list a; va_start(a, f);
    char line[160]; int n = vsnprintf(line, sizeof(line), f, a);
    va_end(a);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = sizeof(line) - 1;
    if (s->cap && s->used + 1 < s->cap) {
        size_t take = (size_t)n < s->cap - 1 - s->used ? (size_t)n : s->cap - 1 - s->used;
        memcpy(s->out + s->used, line, take); s->out[s->used + take] = 0;
    }
    s->used += (size_t)n;
}
size_t scandisk_text(const struct ciuki_scandisk *r, char *out, size_t cap)
{
    struct sink s = { out, cap, 0 };
    char a[32], b[32];
    if (cap) out[0] = 0;
    char drive = (char)('A' + (r->drive < 26 ? r->drive : 25));
    if (!(r->flags & CIUKI_SCANDISK_RAN)) { put(&s, "ScanDisk has not checked drive %c: at this start.\n", drive); return s.used; }
    if (r->result) {
        put(&s, "ScanDisk could not check drive %c: (error %d).\n", drive, -r->result);
        return s.used;
    }
    if (r->flags & CIUKI_SCANDISK_AT_START)
        put(&s, "ScanDisk checked drive %c: because CiukiOS was not shut down properly.\n", drive);
    if (!(r->flags & CIUKI_SCANDISK_ERRORS)) put(&s, "ScanDisk did not find any errors on this drive.\n");
    else if (r->flags & CIUKI_SCANDISK_FIXED) put(&s, "ScanDisk found errors on this drive and fixed them all.\n");
    else if (r->flags & CIUKI_SCANDISK_SCHEDULED)
        put(&s, "ScanDisk found errors on this drive. They will be fixed the next time CiukiOS starts.\n");
    else if (r->flags & CIUKI_SCANDISK_FIX) put(&s, "ScanDisk found errors on this drive. Some of them could not be fixed.\n");
    else put(&s, "ScanDisk found errors on this drive.\n");
    if (r->lost_chains) put(&s, "  %u lost file fragments (%u allocation units)%s\n", r->lost_chains, r->lost_clusters,
                            r->salvaged_files ? " saved in" : r->flags & CIUKI_SCANDISK_FIX ? " freed" : "");
    if (r->salvaged_files) put(&s, "    %u files in \\%s\n", r->salvaged_files, r->found);
    if (r->crosslinked_files) put(&s, "  %u cross-linked files, %u copied\n", r->crosslinked_files, r->crosslinks_fixed);
    if (r->invalid_chains) put(&s, "  %u invalid allocation chains ended\n", r->invalid_chains);
    if (r->size_mismatches) put(&s, "  %u file sizes corrected\n", r->size_mismatches);
    if (r->invalid_entries) put(&s, "  %u invalid directory entries corrected\n", r->invalid_entries);
    if (r->fat_mismatches) put(&s, "  %u sectors of the file allocation table copies did not match\n", r->fat_mismatches);
    if (r->bad_sectors) put(&s, "  %u bad sectors: %u free units marked bad, %u units moved\n", r->bad_sectors, r->bad_marked, r->relocated);
    if (r->unreadable_system) put(&s, "  %u unreadable sectors in the system area\n", r->unreadable_system);
    if (r->first_problem[0]) put(&s, "  First damaged entry: %s\n", r->first_problem);
    uint64_t unit = r->cluster_bytes;
    put(&s, "\n%14s bytes total disk space\n", grouped((uint64_t)r->clusters * unit, a));
    put(&s, "%14s bytes in bad sectors\n", grouped((uint64_t)r->bad_clusters * unit, a));
    put(&s, "%14s bytes in %s folders\n", grouped((uint64_t)r->directory_clusters * unit, a), grouped(r->directories + 1, b));
    put(&s, "%14s bytes in %s user files\n", grouped((uint64_t)r->file_clusters * unit, a), grouped(r->files, b));
    put(&s, "%14s bytes available on disk\n", grouped((uint64_t)r->free_clusters * unit, a));
    put(&s, "%14s bytes in each allocation unit\n", grouped(unit, a));
    put(&s, "%14s total allocation units on disk\n", grouped(r->clusters, a));
    put(&s, "%14s available allocation units\n", grouped(r->free_clusters, a));
    if (r->flags & CIUKI_SCANDISK_THOROUGH) put(&s, "%14s sectors read by the surface scan\n", grouped(r->surface_sectors, a));
    return s.used;
}
int scandisk_log(const char *root, const struct ciuki_scandisk *r, int64_t now)
{
    time_t t = (time_t)now; struct tm tm; char when[32];
    if (localtime_r(&t, &tm)) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm); else snprintf(when, sizeof(when), "?");
    enum { TEXT = 2048, ENTRY = 2304 };
    char *text = malloc(TEXT + ENTRY);
    if (!text) return -ENOMEM;
    char *entry = text + TEXT;
    scandisk_text(r, text, TEXT);
    int n = snprintf(entry, ENTRY, "ScanDisk, %s, drive %c:, %s%s\n%s\n", when, 'A' + (r->drive < 26 ? r->drive : 25),
                     r->flags & CIUKI_SCANDISK_THOROUGH ? "Thorough" : "Standard", r->flags & CIUKI_SCANDISK_AT_START ? " (at start)" : "", text);
    /* sec-sysvc: a system log, appended through the SYSTEM log service. */
    int e = n < 0 ? -EIO : sr_log_append(root, "SCANDISK.LOG", entry, (size_t)n < ENTRY ? (size_t)n : ENTRY - 1);
    free(text);
    return e;
}
