/* SPDX-License-Identifier: MIT */
#include "model.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void tm_init(struct tm_model *m)
{
    memset(m, 0, sizeof(*m));
    m->sort = TM_PID;
}
void tm_format_k(char *out, size_t size, uint64_t kib)
{
    char digits[24], grouped[32];
    int n = snprintf(digits, sizeof(digits), "%llu", (unsigned long long)kib), at = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0) grouped[at++] = ',';
        grouped[at++] = digits[i];
    }
    grouped[at] = 0;
    snprintf(out, size, "%s K", grouped);
}
void tm_format_uptime(char *out, size_t size, uint64_t ticks, uint32_t hz)
{
    uint64_t s = ticks / (hz ? hz : 1000);
    snprintf(out, size, "%llu:%02u:%02u", (unsigned long long)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}
static unsigned percent(uint64_t part, uint64_t whole)
{
    if (!whole) return 0;
    uint64_t p = (part * 100 + whole / 2) / whole;
    return p > 100 ? 100 : (unsigned)p;
}
static int system_valid(const struct ciuki_proc_system *s)
{
    return s->version == CIUKI_PROC_VERSION && s->size == sizeof(*s) && s->records <= CIUKI_PROCESS_MAX &&
           s->processes >= s->records && s->idle_ticks <= s->total_ticks && s->free_pages <= s->usable_pages;
}
static void cells(struct tm_process *p)
{
    const char *suffix = p->state == CIUKI_PROC_ZOMBIE ? " (exited)" : p->state == CIUKI_PROC_STOPPING ? " (ending)" : "";
    snprintf(p->cells[TM_NAME], sizeof(p->cells[TM_NAME]), "%s%s", p->name[0] ? p->name : "(kernel owner)", suffix);
    snprintf(p->cells[TM_PID], sizeof(p->cells[TM_PID]), "%u", p->pid);
    if (p->pid) snprintf(p->cells[TM_PPID], sizeof(p->cells[TM_PPID]), "%u", p->ppid);
    else p->cells[TM_PPID][0] = 0;
    snprintf(p->cells[TM_THREADS], sizeof(p->cells[TM_THREADS]), "%u", p->threads);
    tm_format_k(p->cells[TM_MEMORY], sizeof(p->cells[TM_MEMORY]), (uint64_t)p->pages * 4);
    snprintf(p->cells[TM_CPU], sizeof(p->cells[TM_CPU]), "%02u", p->cpu);
}
static uint64_t previous_cpu(const struct tm_baseline *b, uint32_t pid, uint64_t start)
{
    for (unsigned i = 0; i < b->count; i++) if (b->p[i].pid == pid && b->p[i].start == start) return b->p[i].cpu;
    return 0;
}
int tm_update(struct tm_model *m, const void *buffer, size_t bytes, int records)
{
    struct ciuki_proc_system s;
    if (records < 0 || bytes < sizeof(s)) return -EINVAL;
    memcpy(&s, buffer, sizeof(s));
    if (!system_valid(&s) || s.records != (uint32_t)records ||
        bytes < sizeof(s) + (size_t)records * sizeof(struct ciuki_proc_record)) return -EIO;
    /* One record at a time: the whole list would exceed the 4 KiB frame rule. */
    const uint8_t *records_at = (const uint8_t *)buffer + sizeof(s);
    struct ciuki_proc_record r;
    uint32_t last = 0;
    for (int i = 0; i < records; i++) {
        memcpy(&r, records_at + (size_t)i * sizeof(r), sizeof(r));
        if (r.version != CIUKI_PROC_VERSION || r.size != sizeof(r) || r.pid <= last ||
            r.state < CIUKI_PROC_LIVE || r.state > CIUKI_PROC_ZOMBIE || r.name[CIUKI_PROC_NAME_MAX-1]) return -EIO;
        last = r.pid;
    }
    struct tm_baseline *b = &m->display;
    if (s.total_ticks < b->total || s.idle_ticks < b->idle) memset(b, 0, sizeof(*b));
    uint64_t total = s.total_ticks - b->total, idle = s.idle_ticks - b->idle, busy = 0;
    if (idle > total) idle = total;
    m->system = s;
    m->count = 0;
    m->threads = 0;
    struct tm_process *idle_row = &m->processes[m->count++];
    memset(idle_row, 0, sizeof(*idle_row));
    strcpy(idle_row->name, "System Idle Process");
    idle_row->state = CIUKI_PROC_LIVE;
    idle_row->threads = 1;
    idle_row->cpu = percent(idle, total);
    idle_row->cpu_ticks = s.idle_ticks;
    struct tm_process *kernel = 0;
    for (int i = 0; i < records; i++) {
        memcpy(&r, records_at + (size_t)i * sizeof(r), sizeof(r));
        struct tm_process *p = &m->processes[m->count++];
        memset(p, 0, sizeof(*p));
        p->pid = r.pid; p->ppid = r.ppid; p->pgid = r.pgid; p->state = r.state;
        p->threads = r.threads; p->pages = r.resident_pages;
        p->cpu_ticks = r.cpu_ticks; p->start_tick = r.start_tick;
        memcpy(p->name, r.name, sizeof(p->name));
        uint64_t before = previous_cpu(b, p->pid, p->start_tick);
        uint64_t used = p->cpu_ticks >= before ? p->cpu_ticks - before : 0;
        if (p->pid == 1) kernel = p;
        else { busy += used; p->cpu = percent(used, total); }
        m->threads += p->threads;
    }
    /* PID 1 carries kernel and interrupt time: busy ticks not charged to a
     * native process (the Windows "System" row has the same role). */
    if (kernel) kernel->cpu = percent(total > idle + busy ? total - idle - busy : 0, total);
    m->cpu = percent(total - idle, total);
    b->total = s.total_ticks; b->idle = s.idle_ticks; b->count = 0;
    for (unsigned i = 0; i < m->count; i++) {
        const struct tm_process *p = &m->processes[i];
        if (!p->pid) continue;
        b->p[b->count].pid = p->pid; b->p[b->count].start = p->start_tick; b->p[b->count].cpu = p->cpu_ticks;
        b->count++;
    }
    for (unsigned i = 0; i < m->count; i++) cells(&m->processes[i]);
    tm_sort(m, m->sort, m->descending);
    return 0;
}
int tm_sample(struct tm_model *m, const void *buffer, size_t bytes)
{
    struct ciuki_proc_system s;
    if (bytes < sizeof(s)) return -EINVAL;
    memcpy(&s, buffer, sizeof(s));
    if (!system_valid(&s)) return -EIO;
    struct tm_baseline *b = &m->sampler;
    if (s.total_ticks < b->total || s.idle_ticks < b->idle) memset(b, 0, sizeof(*b));
    uint64_t total = s.total_ticks - b->total, idle = s.idle_ticks - b->idle;
    b->total = s.total_ticks; b->idle = s.idle_ticks;
    if (!total) return 0;
    if (idle > total) idle = total;
    if (m->history_count == TM_HISTORY) memmove(m->history, m->history + 1, TM_HISTORY - 1);
    else m->history_count++;
    m->history[m->history_count - 1] = (uint8_t)percent(total - idle, total);
    return 1;
}
static int sort_column, sort_descending;
static int compare(const void *a, const void *b)
{
    const struct tm_process *x = a, *y = b;
    int r = 0;
    switch (sort_column) {
    case TM_NAME: r = strcmp(x->cells[TM_NAME], y->cells[TM_NAME]); break;
    case TM_PPID: r = (x->ppid > y->ppid) - (x->ppid < y->ppid); break;
    case TM_THREADS: r = (x->threads > y->threads) - (x->threads < y->threads); break;
    case TM_MEMORY: r = (x->pages > y->pages) - (x->pages < y->pages); break;
    case TM_CPU: r = (x->cpu > y->cpu) - (x->cpu < y->cpu); break;
    default: break;
    }
    if (!r) r = (x->pid > y->pid) - (x->pid < y->pid);
    return sort_descending ? -r : r;
}
void tm_sort(struct tm_model *m, int column, int descending)
{
    if (column < 0 || column >= TM_COLUMNS) column = TM_PID;
    m->sort = column; m->descending = descending != 0;
    sort_column = column; sort_descending = m->descending;
    qsort(m->processes, m->count, sizeof(m->processes[0]), compare);
}
const struct tm_process *tm_find(const struct tm_model *m, uint32_t pid)
{
    for (unsigned i = 0; i < m->count; i++) if (m->processes[i].pid == pid) return &m->processes[i];
    return 0;
}
void tm_windows_begin(struct tm_model *m, uint32_t serial)
{
    m->window_serial = serial; m->pending_count = 0; m->pending_mask = 0;
}
static int window_order(const void *a, const void *b)
{
    const struct tm_window *x = a, *y = b;
    int r = strcmp(x->title, y->title);
    if (!r) r = (x->pid > y->pid) - (x->pid < y->pid);
    return r ? r : (x->id > y->id) - (x->id < y->id);
}
int tm_window_info(struct tm_model *m, uint32_t serial, uint32_t index, uint32_t count,
                   uint32_t id, uint32_t pid, uint32_t flags, const char *title)
{
    if (serial != m->window_serial || count > TM_WINDOWS || (count && index >= count)) return -1;
    if (m->pending_mask && count != m->pending_count) return -1;
    if (!count) {
        m->window_count = 0; m->window_serial = 0; return 1;
    }
    m->pending_count = count;
    struct tm_window *w = &m->pending[index];
    w->id = id; w->pid = pid; w->flags = flags;
    snprintf(w->title, sizeof(w->title), "%s", title ? title : "");
    m->pending_mask |= 1u << index;
    if (m->pending_mask != (count == 32 ? UINT32_MAX : (1u << count) - 1)) return 0;
    memcpy(m->windows, m->pending, count * sizeof(m->windows[0]));
    m->window_count = count;
    qsort(m->windows, count, sizeof(m->windows[0]), window_order);
    m->window_serial = 0; m->pending_mask = 0;
    return 1;
}
enum tm_route tm_route(const struct tm_model *m, uint32_t pid, uint32_t desktop_pid)
{
    if (pid <= 1) return TM_REFUSE_SYSTEM;
    if (desktop_pid && pid == desktop_pid) return TM_REFUSE_DESKTOP;
    const struct tm_process *target = tm_find(m, pid), *desktop = desktop_pid ? tm_find(m, desktop_pid) : 0;
    if (!target || target->state != CIUKI_PROC_LIVE) return TM_REFUSE_GONE;
    return desktop && target->pgid == desktop->pgid ? TM_VIA_DESKTOP : TM_VIA_KERNEL;
}
