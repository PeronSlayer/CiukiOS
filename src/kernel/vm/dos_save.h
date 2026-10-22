/* Internal persistent sector store; no guest or public SDK layout.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOS_SAVE_H
#define CIUKI_DOS_SAVE_H
#include "../fs/fs_port.h"
#include <ciuki/sha256.h>
#define DOS_SAVE_QUOTA (64u * 1024u * 1024u)
#define DOS_SAVE_SECTORS 262144u
#define DOS_SAVE_MISMATCH 1
struct dos_save_identity {
    uint32_t game, kernel;
    char base[65]; /* full base content SHA-256; includes kernel and filesystem */
};
struct dos_save_io {
    int (*read)(void *, uint32_t, void *, unsigned);
    int (*write)(void *, uint32_t, const void *, unsigned);
    int (*flush)(void *);
    int (*resize)(void *, uint32_t);
    void *arg;
};
struct dos_save {
    struct dos_save_io io;
    struct sha256_ctx hash;
    uint32_t *index[DOS_SAVE_SECTORS / 1024u];
    void *(*alloc)(void *);
    void (*release)(void *, void *);
    void *memory;
    uint32_t end, committed, rows, pending, quota, recovered_bytes, index_pages;
    bool readonly, failed, sealed, full;
};
/* Zero-initialized; caller supplies alloc/release/memory and quota (zero =
 * default). Release the index before opening another source in this object.
 * A mismatch never writes, truncates or frees the caller's source file. */
int dos_save_open(struct dos_save *, const struct dos_save_io *,
                  const struct dos_save_identity *, uint32_t file_size);
int dos_save_read(struct dos_save *, uint32_t lba, void *); /* ENOENT = base */
int dos_save_write(struct dos_save *, uint32_t lba, const void *);
int dos_save_commit(struct dos_save *, bool seal);
void dos_save_release(struct dos_save *);
#endif
