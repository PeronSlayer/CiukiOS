/* SPDX-License-Identifier: MIT
 * OLE2 compound file reader (G28b): the container of Word 6, 95 and 97 .doc files.
 *
 * Source: Microsoft, [MS-CFB] Compound File Binary File Format (header, FAT, DIFAT, mini FAT, directory entries and the
 * mini stream of streams below the 4096-byte cutoff). Read-only, bounded (sector chains are followed at most once
 * per sector, counts are checked against the file length) and independent of the toolkit: the host tests run the
 * production code. */
#ifndef CIUKI_WORDPAD_OLE_H
#define CIUKI_WORDPAD_OLE_H
#include <stddef.h>
#include <stdint.h>

#define WP_OLE_MAX (64u << 20)      /* largest file accepted */

struct wp_ole {
    const uint8_t *buf; size_t len;
    unsigned sector_shift, mini_shift;
    uint32_t cutoff;
    uint32_t *fat; uint32_t nfat;           /* entries */
    uint32_t *minifat; uint32_t nminifat;
    uint8_t *dir; uint32_t ndir;            /* 128-byte directory entries */
    uint8_t *mini; size_t mini_len;         /* the mini stream (the root entry's data) */
};

int wp_is_ole(const void *buf, size_t n);                         /* the 8-byte signature */
/* 0, -EINVAL (not a valid compound file) or -ENOMEM. The buffer must outlive the handle. */
int wp_ole_open(struct wp_ole *, const uint8_t *buf, size_t len);
void wp_ole_close(struct wp_ole *);
/* Copies the stream called `name` (compared case-insensitively on its ASCII form) of the root storage: malloc'd, *len
 * its size. -ENOENT when absent, -EINVAL when its chains are damaged, -ENOMEM. */
int wp_ole_read(const struct wp_ole *, const char *name, uint8_t **data, size_t *len);
#endif
