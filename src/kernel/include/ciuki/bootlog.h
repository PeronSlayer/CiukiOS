/* Bounded RAM producer; VFS work only from ordinary task context.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_BOOTLOG_H
#define CIUKI_BOOTLOG_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define BOOTLOG_LIMIT (64u * 1024u)
#define BOOTLOG_RAM 8192u
#define BOOTLOG_PATH "C:/SYSTEM/BOOT.LOG"
struct vfs;
struct bootlog_stats {
    uint32_t size, queued, dropped, rotations, qualification_sequence, first_write_sequence;
    uint64_t storage_calls, writes, captured;
    int error;
    bool active;
};
/* No allocation, no VFS, no logging. Ordinary output only; never call from
 * kputs_raw/rec_emit_panicsafe/panic. May collect before storage_init. */
void bootlog_capture(const char *, size_t);
int bootlog_activate(struct vfs *, bool qualified, uint32_t sequence);
int bootlog_drain(void);
int bootlog_shutdown(void);
void bootlog_snapshot(struct bootlog_stats *);
void bootlog_reset(void); /* quiescent, detached only; host reboot fixture */
#endif
