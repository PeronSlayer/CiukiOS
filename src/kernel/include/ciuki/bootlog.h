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
#define BOOTLOG_RAM 65536u
#define BOOTLOG_BATCH 32768u
#define BOOTLOG_HIGH_WATER (BOOTLOG_RAM * 3u / 4u)
#define BOOTLOG_DRAIN_MS 1000u
#define BOOTLOG_METADATA_MS 5000u
#define BOOTLOG_PATH "C:/SYSTEM/LOGS/BOOT.LOG"
#define BOOTLOG_SYNC_TIMEOUT_MS 2000u
struct vfs;
struct bootlog_stats {
    uint32_t size, queued, dropped, rotations, qualification_sequence, first_write_sequence;
    uint64_t storage_calls, writes, captured;
    int error;
    bool active;
    uint32_t boot_size, boot_queued, ring_queued;
    uint64_t metadata_writes, bytes_written;
};
/* No allocation, no VFS, no logging. Ordinary output only; never call from
 * kputs_raw/rec_emit_panicsafe/panic. May collect before storage_init. */
void bootlog_capture(const char *, size_t);
/* Periodic summaries bypass the protected prefix, including before mount. */
void bootlog_capture_ring(const char *, size_t);
int bootlog_activate(struct vfs *, bool qualified, uint32_t sequence);
int bootlog_drain(void); /* storage task: one aligned 32 KiB block when due */
bool bootlog_due(uint32_t now); /* RAM only; includes pending metadata/sync */
/* Thread context, with no storage or USB reservation held. Request a captured
 * byte watermark from the storage task and wait with cancellation/deadline;
 * the caller never performs disk I/O. queued includes a pending commit token. */
int bootlog_sync(bool (*cancel)(void *), void (*pause)(void *), void *);
/* Request the current watermark without waiting or doing VFS work. Returns
 * -EAGAIN while durability is unconfirmed (also when no sink is active).
 * A pending request still receives an eventual storage-task commit. */
int bootlog_try_sync(void);
int bootlog_shutdown(void);
void bootlog_snapshot(struct bootlog_stats *);
void bootlog_reset(void); /* quiescent, detached only; host reboot fixture */
#endif
