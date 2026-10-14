/* Bounded RAM producer; VFS work only from ordinary task context.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_BOOTLOG_H
#define CIUKI_BOOTLOG_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define BOOTLOG_LIMIT (1024u * 1024u)
#define BOOTLOG_PROTECTED (128u * 1024u)
#define BOOTLOG_RING (BOOTLOG_LIMIT - BOOTLOG_PROTECTED)
#define BOOTLOG_RAM 8192u
#define BOOTLOG_PATH "C:/SYSTEM/LOGS/BOOT.LOG"
#define BOOTLOG_SYNC_TIMEOUT_MS 2000u
struct vfs;
struct bootlog_stats {
    uint32_t size, queued, dropped, rotations, qualification_sequence, first_write_sequence;
    uint64_t storage_calls, writes, captured;
    int error;
    bool active;
    uint32_t boot_size, boot_queued, ring_queued;
};
/* No allocation, no VFS, no logging. Ordinary output only; never call from
 * kputs_raw/rec_emit_panicsafe/panic. May collect before storage_init. */
void bootlog_capture(const char *, size_t);
/* Periodic summaries bypass the protected prefix, including before mount. */
void bootlog_capture_ring(const char *, size_t);
int bootlog_activate(struct vfs *, bool qualified, uint32_t sequence);
int bootlog_drain(void); /* storage task: one 4 KiB batch, then a requested commit */
/* Thread context, with no storage or USB reservation held. Request a captured
 * byte watermark from the storage task and wait with cancellation/deadline;
 * the caller never performs disk I/O. queued includes a pending commit token. */
int bootlog_sync(bool (*cancel)(void *), void (*pause)(void *), void *);
int bootlog_shutdown(void);
void bootlog_snapshot(struct bootlog_stats *);
void bootlog_reset(void); /* quiescent, detached only; host reboot fixture */
#endif
