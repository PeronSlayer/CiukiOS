/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FS_FAKE_H
#define FS_FAKE_H
#include "blkdev.h"
#define FAKE_LIMIT 32768u
#define FAKE_TRACE_LIMIT 131072u
struct fake_sector { uint64_t lba; uint8_t data[512]; };
struct fake_event { uint64_t lba; char kind; };
struct fake {
    struct blkdev dev;
    int fd;
    uint64_t fail_read, fail_write, torn_lba;
    size_t torn_bytes;
    unsigned events, writes, reads, flushes, cut_at, cut_mode;
    bool cut, reorder, fail_flush, undo_enabled;
    struct fake_sector *undo, *pending;
    unsigned undo_count, pending_count;
    struct fake_event *trace;
};
int fake_open(struct fake *, const char *);
void fake_close(struct fake *);
void fake_reset(struct fake *); /* restore baseline independently of driver */
void fake_power_loss(struct fake *);
int fake_raw_read(struct fake *, uint64_t, void *);
int fake_raw_write(struct fake *, uint64_t, const void *);
#endif
