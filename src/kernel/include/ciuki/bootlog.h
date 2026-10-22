/* Bounded RAM producer; VFS work only from ordinary task context.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_BOOTLOG_H
#define CIUKI_BOOTLOG_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/* f0-56: three boot logs of 2.75 MiB (8.25 MiB, >= the 8 MiB the T23 trial
 * needs) share the budget, including cluster rounding. Before f0-56 each was
 * 320 KiB (960 KiB in all), which held ~1,975 of the 13,588 lines of a 12
 * minute boot. */
#define BOOTLOG_BUDGET (9u * 1024u * 1024u)
#define BOOTLOG_LIMIT (2816u * 1024u)
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
/* Periodic statistics: at most LINES per window overall and KIND_LINES per
 * kind (e.g. "cpu/task"); the rest is summarized by one line per window. The
 * last STATISTIC_RESERVE free RAM slots are kept for protected records. */
#define BOOTLOG_BUDGET_WINDOW_MS 2000u
#define BOOTLOG_BUDGET_LINES 40u
#define BOOTLOG_BUDGET_KIND_LINES 8u
#define BOOTLOG_STATISTIC_RESERVE 32u
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
    /* f0-56: involuntary loss of budgeted statistics (queue pressure), and the
     * deliberate per-window suppression, which is not part of `dropped`. */
    uint32_t dropped_statistic, records_dropped_statistic;
    uint32_t budget_dropped_lines, budget_windows;
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
/* Read-only report snapshot. At most 4096 pending bytes, oldest first; no
 * allocation, VFS or producer changes. Position is the current disk write
 * point (ring-aware), not the preallocated file's physical end. */
bool bootlog_report_snapshot(uint32_t *position, uint32_t *dropped,
                             char *pending, size_t capacity, size_t *length);
void bootlog_reset(void); /* quiescent, detached only; host reboot fixture */
#endif
