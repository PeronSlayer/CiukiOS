/* Task Manager model: kernel process records (call 70) and the desktop's
 * window list (protocol v2), independent of any window or syscall.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_TASKS_MODEL_H
#define CIUKI_TASKS_MODEL_H
#include <ciuki/abi.h>
#include <stddef.h>
#include <stdint.h>
#define TM_PROCESSES (CIUKI_PROCESS_MAX + 1)   /* records plus System Idle Process */
#define TM_WINDOWS 16
#define TM_HISTORY 60
#define TM_LIST_BYTES (CIUKI_PROC_LIST_MIN + CIUKI_PROCESS_MAX * sizeof(struct ciuki_proc_record))
enum tm_column { TM_NAME, TM_PID, TM_PPID, TM_THREADS, TM_MEMORY, TM_CPU, TM_COLUMNS };
enum tm_route { TM_VIA_KERNEL, TM_VIA_DESKTOP, TM_REFUSE_SYSTEM, TM_REFUSE_DESKTOP, TM_REFUSE_GONE };
struct tm_process {
    uint32_t pid, ppid, pgid, state, threads, pages;
    uint64_t cpu_ticks, start_tick;
    unsigned cpu;                      /* percent of the last interval, 0..100 */
    char name[CIUKI_PROC_NAME_MAX];
    char cells[TM_COLUMNS][32];        /* formatted column text */
};
struct tm_window { uint32_t id, pid, flags; char title[64]; };
struct tm_baseline { uint64_t total, idle; unsigned count; struct { uint32_t pid; uint64_t start, cpu; } p[CIUKI_PROCESS_MAX]; };
struct tm_model {
    struct ciuki_proc_system system;
    struct tm_process processes[TM_PROCESSES];
    unsigned count, cpu, threads;
    struct tm_baseline display, sampler;
    uint8_t history[TM_HISTORY];
    unsigned history_count;
    int sort, descending;
    struct tm_window windows[TM_WINDOWS], pending[TM_WINDOWS];
    unsigned window_count, pending_count;
    uint32_t window_serial, pending_mask;
    uint64_t performance[9];
};
void tm_init(struct tm_model *);
/* Validates and applies one call-70 result; returns 0, -EINVAL (short buffer) or -EIO (inconsistent records). */
int tm_update(struct tm_model *, const void *buffer, size_t bytes, int records);
/* One history point from a 1-second sampler snapshot (system record only). */
int tm_sample(struct tm_model *, const void *buffer, size_t bytes);
void tm_sort(struct tm_model *, int column, int descending);
const struct tm_process *tm_find(const struct tm_model *, uint32_t pid);
void tm_windows_begin(struct tm_model *, uint32_t serial);
/* 1 when the list for the current serial is complete, 0 pending, -1 ignored. */
int tm_window_info(struct tm_model *, uint32_t serial, uint32_t index, uint32_t count,
                   uint32_t id, uint32_t pid, uint32_t flags, const char *title);
enum tm_route tm_route(const struct tm_model *, uint32_t pid, uint32_t desktop_pid);
void tm_format_k(char *out, size_t size, uint64_t kib);
void tm_format_uptime(char *out, size_t size, uint64_t ticks, uint32_t hz);
#endif
