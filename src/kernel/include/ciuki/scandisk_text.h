/* sec-sysvc2: the ScanDisk report text (Windows 98 "ScanDisk Results" and
 * SCANDISK.LOG), pure code shared by the kernel's startup-report job
 * (src/kernel/fs/sysvc_jobs.c, a SYSTEM job) and the recovery tools
 * (apps/sysrecovery/scanreport.c), so the at-start entries the kernel appends
 * and the entries of an administrator's check read the same. The includer
 * defines SCANDISK_TEXT_VFORMAT(buffer, size, format, va_list) first
 * (vsnprintf, or the kernel's kvsnprintf: only %s %c %d %u %llu and widths).
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_SCANDISK_TEXT_H
#define CIUKI_SCANDISK_TEXT_H
#include "abi.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* 1234567 -> "1,234,567" */
static inline const char *scandisk_grouped(uint64_t v, char out[32])
{
    char digits[24]; int n = 0, o = 0;
    do { digits[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 20);
    for (int i = n - 1; i >= 0; i--) {
        if (o && (i + 1) % 3 == 0) out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = 0;
    return out;
}
struct scandisk_sink { char *out; size_t cap, used; };
static inline void scandisk_put(struct scandisk_sink *s, const char *f, ...)
{
    va_list a; va_start(a, f);
    char line[160]; int n = SCANDISK_TEXT_VFORMAT(line, sizeof(line), f, a);
    va_end(a);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = sizeof(line) - 1;
    if (s->cap && s->used + 1 < s->cap) {
        size_t take = (size_t)n < s->cap - 1 - s->used ? (size_t)n : s->cap - 1 - s->used;
        for (size_t i = 0; i < take; i++) s->out[s->used + i] = line[i];
        s->out[s->used + take] = 0;
    }
    s->used += (size_t)n;
}
/* The summary paragraph and the statistics, as text with '\n'. Returns the
 * full length (the text is cut at cap - 1). */
static inline size_t scandisk_report_text(const struct ciuki_scandisk *r, char *out, size_t cap)
{
    struct scandisk_sink s = { out, cap, 0 };
    char a[32], b[32];
    if (cap) out[0] = 0;
    char drive = (char)('A' + (r->drive < 26 ? r->drive : 25));
    if (!(r->flags & CIUKI_SCANDISK_RAN)) { scandisk_put(&s, "ScanDisk has not checked drive %c: at this start.\n", drive); return s.used; }
    if (r->result) {
        scandisk_put(&s, "ScanDisk could not check drive %c: (error %d).\n", drive, -r->result);
        return s.used;
    }
    if (r->flags & CIUKI_SCANDISK_AT_START)
        scandisk_put(&s, "ScanDisk checked drive %c: because CiukiOS was not shut down properly.\n", drive);
    if (!(r->flags & CIUKI_SCANDISK_ERRORS)) scandisk_put(&s, "ScanDisk did not find any errors on this drive.\n");
    else if (r->flags & CIUKI_SCANDISK_FIXED) scandisk_put(&s, "ScanDisk found errors on this drive and fixed them all.\n");
    else if (r->flags & CIUKI_SCANDISK_SCHEDULED)
        scandisk_put(&s, "ScanDisk found errors on this drive. They will be fixed the next time CiukiOS starts.\n");
    else if (r->flags & CIUKI_SCANDISK_FIX) scandisk_put(&s, "ScanDisk found errors on this drive. Some of them could not be fixed.\n");
    else scandisk_put(&s, "ScanDisk found errors on this drive.\n");
    if (r->lost_chains) scandisk_put(&s, "  %u lost file fragments (%u allocation units)%s\n", r->lost_chains, r->lost_clusters,
                                     r->salvaged_files ? " saved in" : r->flags & CIUKI_SCANDISK_FIX ? " freed" : "");
    if (r->salvaged_files) {
        char found[sizeof(r->found) + 1]; size_t n = 0;
        while (n < sizeof(r->found) && r->found[n]) { found[n] = r->found[n]; n++; }
        found[n] = 0;
        scandisk_put(&s, "    %u files in \\%s\n", r->salvaged_files, found);
    }
    if (r->crosslinked_files) scandisk_put(&s, "  %u cross-linked files, %u copied\n", r->crosslinked_files, r->crosslinks_fixed);
    if (r->invalid_chains) scandisk_put(&s, "  %u invalid allocation chains ended\n", r->invalid_chains);
    if (r->size_mismatches) scandisk_put(&s, "  %u file sizes corrected\n", r->size_mismatches);
    if (r->invalid_entries) scandisk_put(&s, "  %u invalid directory entries corrected\n", r->invalid_entries);
    if (r->fat_mismatches) scandisk_put(&s, "  %u sectors of the file allocation table copies did not match\n", r->fat_mismatches);
    if (r->bad_sectors) scandisk_put(&s, "  %u bad sectors: %u free units marked bad, %u units moved\n", r->bad_sectors, r->bad_marked, r->relocated);
    if (r->unreadable_system) scandisk_put(&s, "  %u unreadable sectors in the system area\n", r->unreadable_system);
    if (r->first_problem[0]) {
        char first[sizeof(r->first_problem) + 1]; size_t n = 0;
        while (n < sizeof(r->first_problem) && r->first_problem[n]) { first[n] = r->first_problem[n]; n++; }
        first[n] = 0;
        scandisk_put(&s, "  First damaged entry: %s\n", first);
    }
    uint64_t unit = r->cluster_bytes;
    scandisk_put(&s, "\n%14s bytes total disk space\n", scandisk_grouped((uint64_t)r->clusters * unit, a));
    scandisk_put(&s, "%14s bytes in bad sectors\n", scandisk_grouped((uint64_t)r->bad_clusters * unit, a));
    scandisk_put(&s, "%14s bytes in %s folders\n", scandisk_grouped((uint64_t)r->directory_clusters * unit, a),
                 scandisk_grouped((uint64_t)r->directories + 1, b));
    scandisk_put(&s, "%14s bytes in %s user files\n", scandisk_grouped((uint64_t)r->file_clusters * unit, a), scandisk_grouped(r->files, b));
    scandisk_put(&s, "%14s bytes available on disk\n", scandisk_grouped((uint64_t)r->free_clusters * unit, a));
    scandisk_put(&s, "%14s bytes in each allocation unit\n", scandisk_grouped(unit, a));
    scandisk_put(&s, "%14s total allocation units on disk\n", scandisk_grouped(r->clusters, a));
    scandisk_put(&s, "%14s available allocation units\n", scandisk_grouped(r->free_clusters, a));
    if (r->flags & CIUKI_SCANDISK_THOROUGH) scandisk_put(&s, "%14s sectors read by the surface scan\n", scandisk_grouped(r->surface_sectors, a));
    return s.used;
}
/* One SCANDISK.LOG entry: "ScanDisk, <when>, drive C:, Standard (at start)",
 * the report text and an empty line. `text` is scratch of text_cap bytes.
 * Returns the entry length, cut at cap - 1. */
static inline size_t scandisk_log_entry(const struct ciuki_scandisk *r, const char *when, char *text, size_t text_cap,
                                        char *out, size_t cap)
{
    scandisk_report_text(r, text, text_cap);
    struct scandisk_sink s = { out, cap, 0 };
    if (cap) out[0] = 0;
    scandisk_put(&s, "ScanDisk, %s, drive %c:, %s%s\n", when, 'A' + (int)(r->drive < 26 ? r->drive : 25),
                 r->flags & CIUKI_SCANDISK_THOROUGH ? "Thorough" : "Standard", r->flags & CIUKI_SCANDISK_AT_START ? " (at start)" : "");
    /* The text may pass one line's 160 bytes: copy it whole. */
    for (size_t i = 0; text[i] && i + 1 < text_cap; i++) {
        if (s.cap && s.used + 1 < s.cap) { s.out[s.used] = text[i]; s.out[s.used + 1] = 0; }
        s.used++;
    }
    scandisk_put(&s, "\n");
    return s.used < cap ? s.used : (cap ? cap - 1 : 0);
}
#endif
