/* Bounded RAM producer; VFS work only from ordinary task context.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_BOOTLOG_H
#define CIUKI_BOOTLOG_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define BOOTLOG_BUDGET (1024u * 1024u)
/* Three boots share the original budget, including cluster rounding. */
#define BOOTLOG_LIMIT (320u * 1024u)
#define BOOTLOG_PROTECTED (128u * 1024u)
#define BOOTLOG_RING (BOOTLOG_LIMIT - BOOTLOG_PROTECTED)
#define BOOTLOG_RAM 65536u
#define BOOTLOG_BATCH 32768u
#define BOOTLOG_HIGH_WATER (BOOTLOG_RAM * 3u / 4u)
#define BOOTLOG_DRAIN_MS 1000u
#define BOOTLOG_TURN_MS 10u
#define BOOTLOG_TURN_BLOCKS 4u
#define BOOTLOG_RECORD_BYTES 256u
#define BOOTLOG_RECORDS (BOOTLOG_RAM / BOOTLOG_RECORD_BYTES)
#define BOOTLOG_METADATA_MS 5000u
#define BOOTLOG_PATH "C:/SYSTEM/LOGS/BOOT.LOG"
#define BOOTLOG_PREVIOUS "C:/SYSTEM/LOGS/BOOT1.LOG"
#define BOOTLOG_OLDEST "C:/SYSTEM/LOGS/BOOT2.LOG"
#define BOOTLOG_SYNC_TIMEOUT_MS 2000u
/* Mount-crash's runner deadline is 300 seconds, including the cut workload. */
#define BOOTLOG_PAUSE_MAX_MS 300000u
struct vfs;
struct bootlog_stats {
    uint32_t size, queued, dropped, rotations, qualification_sequence, first_write_sequence;
    uint64_t storage_calls, writes, captured;
    int error;
    bool active;
    uint32_t boot_size, boot_queued, ring_queued;
    uint64_t metadata_writes, bytes_written;
    /* Loss is bytes (legacy dropped), plus evicted/refused publication slices. */
    uint32_t dropped_protected, dropped_verbose;
    uint32_t records_dropped_protected, records_dropped_verbose;
};
/* No allocation, no VFS, no logging. Ordinary output only; never call from
 * kputs_raw/rec_emit_panicsafe/panic. May collect before storage_init. */
void bootlog_capture(const char *, size_t);
/* Periodic summaries bypass the protected prefix, including before mount. */
void bootlog_capture_ring(const char *, size_t);
/* Droppable diagnostics never occupy the immutable boot prefix. */
void bootlog_capture_verbose(const char *, size_t);
int bootlog_activate(struct vfs *, bool qualified, uint32_t sequence);
int bootlog_drain(void); /* storage task: bounded device-paced turn */
bool bootlog_due(uint32_t now); /* RAM only; includes pending metadata/sync */
/* Single-owner, non-nesting drain exclusion; capture continues unchanged.
 * RAM only. -EBUSY means a drain/shutdown or another pause owns the sink;
 * success guarantees no in-flight drain. 0 or >MAX durations are invalid.
 * due/drain/shutdown automatically release an expired pause (clock wrap safe).
 * The owner must resume on every returning path, with no VFS locks held
 * while retrying acquisition. */
int bootlog_pause_drain(uint32_t timeout_ms);
void bootlog_resume_drain(void);
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
