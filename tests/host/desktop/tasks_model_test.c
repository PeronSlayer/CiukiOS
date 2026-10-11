/* f2-33 Task Manager model against fake call-70 results.
 * SPDX-License-Identifier: MIT */
#include "model.h"
#include "protocol.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static uint8_t buffer[TM_LIST_BYTES];
static struct ciuki_proc_record records[CIUKI_PROCESS_MAX];
static unsigned record_count;
static void record(uint32_t pid, uint32_t ppid, uint32_t pgid, uint32_t state, uint32_t threads,
                   uint32_t pages, uint64_t cpu, uint64_t start, const char *name)
{
    struct ciuki_proc_record *r = &records[record_count++];
    memset(r, 0, sizeof(*r));
    *r = (struct ciuki_proc_record){ .version = CIUKI_PROC_VERSION, .size = sizeof(*r), .pid = pid, .ppid = ppid,
        .pgid = pgid, .state = state, .threads = threads, .resident_pages = pages, .cpu_ticks = cpu, .start_tick = start };
    snprintf(r->name, sizeof(r->name), "%s", name);
}
static size_t pack(uint64_t total, uint64_t idle)
{
    struct ciuki_proc_system s = { .version = CIUKI_PROC_VERSION, .size = sizeof(s), .tick_hz = 1000,
        .records = record_count, .total_ticks = total, .idle_ticks = idle, .usable_pages = 32000,
        .free_pages = 24000, .kheap_pages = 40, .kheap_bytes = 90000, .kheap_high_water = 120000,
        .processes = record_count };
    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, &s, sizeof(s));
    memcpy(buffer + sizeof(s), records, record_count * sizeof(records[0]));
    return sizeof(s) + record_count * sizeof(records[0]);
}
static const struct tm_process *find(const struct tm_model *m, uint32_t pid)
{ const struct tm_process *p = tm_find(m, pid); assert(p); return p; }
int main(void)
{
    static struct tm_model m;
    tm_init(&m);
    char text[32];
    tm_format_k(text, sizeof(text), 0); assert(!strcmp(text, "0 K"));
    tm_format_k(text, sizeof(text), 999); assert(!strcmp(text, "999 K"));
    tm_format_k(text, sizeof(text), 1000); assert(!strcmp(text, "1,000 K"));
    tm_format_k(text, sizeof(text), 1234567); assert(!strcmp(text, "1,234,567 K"));
    tm_format_uptime(text, sizeof(text), 3723456, 1000); assert(!strcmp(text, "1:02:03"));
    tm_format_uptime(text, sizeof(text), 360000000, 0); assert(!strcmp(text, "100:00:00"));

    record(1, 0, 1, CIUKI_PROC_LIVE, 1, 0, 0, 0, "supervisor");
    record(5, 1, 5, CIUKI_PROC_LIVE, 2, 400, 200, 10, "desktop");
    record(7, 5, 5, CIUKI_PROC_LIVE, 1, 120, 50, 400, "tasks");
    record(9, 1, 9, CIUKI_PROC_LIVE, 3, 300, 100, 20, "lua");
    record(11, 5, 5, CIUKI_PROC_ZOMBIE, 0, 0, 7, 30, "demo");
    record(12, 1, 1, CIUKI_PROC_LIVE, 0, 0, 0, 30, "");
    size_t n = pack(1000, 600);
    assert(tm_update(&m, buffer, n, (int)record_count) == 0);
    /* First snapshot: averages since boot. Idle 60%, kernel the remainder. */
    assert(m.count == record_count + 1 && m.cpu == 40 && m.threads == 7);
    assert(find(&m, 0)->cpu == 60 && !strcmp(find(&m, 0)->cells[TM_NAME], "System Idle Process"));
    assert(find(&m, 5)->cpu == 20 && find(&m, 7)->cpu == 5 && find(&m, 9)->cpu == 10);
    assert(find(&m, 1)->cpu == 4);  /* 1000-600-(200+50+100+7)=43 ticks */
    assert(!strcmp(find(&m, 9)->cells[TM_MEMORY], "1,200 K") && !strcmp(find(&m, 9)->cells[TM_CPU], "10"));
    assert(!strcmp(find(&m, 11)->cells[TM_NAME], "demo (exited)") && !strcmp(find(&m, 12)->cells[TM_NAME], "(kernel owner)"));
    assert(!strcmp(find(&m, 0)->cells[TM_PPID], "") && !strcmp(find(&m, 7)->cells[TM_PPID], "5"));
    for (unsigned i = 1; i < m.count; i++) assert(m.processes[i].pid > m.processes[i-1].pid); /* default PID order */

    /* Second snapshot: deltas, a restarted identity and an exited process. */
    record_count = 0;
    record(1, 0, 1, CIUKI_PROC_LIVE, 1, 0, 0, 0, "supervisor");
    record(5, 1, 5, CIUKI_PROC_LIVE, 2, 400, 250, 10, "desktop");
    record(7, 5, 5, CIUKI_PROC_LIVE, 1, 120, 50, 400, "tasks");
    record(9, 1, 9, CIUKI_PROC_LIVE, 1, 60, 30, 1500, "lua");      /* same PID, new start tick */
    record(13, 7, 13, CIUKI_PROC_LIVE, 1, 80, 20, 1500, "demo");
    n = pack(2000, 1500);
    assert(tm_update(&m, buffer, n, (int)record_count) == 0);
    assert(m.cpu == 10 && find(&m, 0)->cpu == 90 && find(&m, 5)->cpu == 5 && find(&m, 7)->cpu == 0);
    assert(find(&m, 9)->cpu == 3 && find(&m, 13)->cpu == 2 && !tm_find(&m, 11));
    assert(find(&m, 1)->cpu == 0);  /* 1000-900-(50+30+20) = 0 */

    tm_sort(&m, TM_CPU, 1);
    assert(m.processes[0].pid == 0 && m.processes[1].pid == 5 && m.sort == TM_CPU && m.descending);
    for (unsigned i = 1; i < m.count; i++) assert(m.processes[i].cpu <= m.processes[i-1].cpu);
    tm_sort(&m, TM_NAME, 0);
    assert(!strcmp(m.processes[0].cells[TM_NAME], "System Idle Process") && !strcmp(m.processes[1].name, "demo"));
    tm_sort(&m, TM_MEMORY, 1); assert(m.processes[0].pid == 5 && m.processes[1].pid == 7);
    tm_sort(&m, TM_THREADS, 0); assert(m.processes[m.count-1].pid == 5);
    tm_sort(&m, 99, 0); assert(m.sort == TM_PID && m.processes[0].pid == 0);

    /* Malformed kernel output never replaces the current model. */
    unsigned before = m.count;
    assert(tm_update(&m, buffer, 10, 0) == -EINVAL && tm_update(&m, buffer, n, -1) == -EINVAL);
    assert(tm_update(&m, buffer, n, (int)record_count - 1) == -EIO);
    assert(tm_update(&m, buffer, n - 1, (int)record_count) == -EIO);
    records[2].pid = 3; n = pack(3000, 1600); assert(tm_update(&m, buffer, n, (int)record_count) == -EIO); records[2].pid = 7;
    records[1].name[31] = 'x'; n = pack(3000, 1600); assert(tm_update(&m, buffer, n, (int)record_count) == -EIO); records[1].name[31] = 0;
    records[1].state = 4; n = pack(3000, 1600); assert(tm_update(&m, buffer, n, (int)record_count) == -EIO); records[1].state = 1;
    n = pack(3000, 3100); assert(tm_update(&m, buffer, n, (int)record_count) == -EIO);
    struct ciuki_proc_system bad; memcpy(&bad, buffer, sizeof(bad)); bad.version = 2; memcpy(buffer, &bad, sizeof(bad));
    assert(tm_update(&m, buffer, n, (int)record_count) == -EIO && m.count == before);

    /* History: one point per sampler snapshot, 60 retained, counter reset. */
    struct ciuki_proc_system s = { .version = 1, .size = 64, .tick_hz = 1000, .usable_pages = 10, .free_pages = 5 };
    for (unsigned i = 1; i <= 61; i++) {            /* second i is (i % 4) * 25 % busy */
        s.total_ticks = i * 1000; s.idle_ticks += 1000 - (i % 4) * 250;
        assert(tm_sample(&m, &s, sizeof(s)) == 1);
    }
    /* 61 points pushed: the first (i=1) dropped; i=2 is oldest, i=61 newest. */
    assert(m.history_count == TM_HISTORY && m.history[0] == 50 && m.history[TM_HISTORY - 1] == 25 && m.history[TM_HISTORY - 2] == 0);
    assert(tm_sample(&m, &s, sizeof(s)) == 0);                       /* no elapsed tick */
    s.total_ticks = 500; s.idle_ticks = 100; assert(tm_sample(&m, &s, sizeof(s)) == 1 && m.history[TM_HISTORY - 1] == 80);
    assert(tm_sample(&m, &s, 10) == -EINVAL);

    /* Window list assembly from WINDOW_INFO messages. */
    tm_windows_begin(&m, 41);
    assert(tm_window_info(&m, 40, 0, 2, 3, 13, 0, "Ciuki Demo") == -1);
    assert(tm_window_info(&m, 41, 1, 2, 2, 7, DESK_WINDOW_FOCUSED, "Task Manager") == 0);
    assert(tm_window_info(&m, 41, 0, 3, 3, 13, 0, "x") == -1);       /* count changed mid-list */
    assert(tm_window_info(&m, 41, 2, 2, 3, 13, 0, "x") == -1);
    assert(tm_window_info(&m, 41, 0, 2, 3, 13, DESK_WINDOW_UNRESPONSIVE, "Ciuki Demo") == 1);
    assert(m.window_count == 2 && m.windows[0].pid == 13 && !strcmp(m.windows[1].title, "Task Manager"));
    assert(tm_window_info(&m, 41, 0, 2, 3, 13, 0, "late") == -1);    /* completed serial is closed */
    tm_windows_begin(&m, 42);
    assert(tm_window_info(&m, 42, 0, 3, 9, 20, 0, "Same") == 0 && tm_window_info(&m, 42, 1, 3, 8, 19, 0, "Same") == 0);
    assert(tm_window_info(&m, 42, 2, 3, 4, 19, 0, "Alpha") == 1);
    assert(m.windows[0].id == 4 && m.windows[1].id == 8 && m.windows[2].id == 9); /* title, PID, id */
    tm_windows_begin(&m, 43); assert(tm_window_info(&m, 43, 0, 0, 0, 0, 0, "") == 1 && !m.window_count);

    /* End Process routing: system, the desktop, its group, others, gone. */
    tm_sort(&m, TM_PID, 0);
    assert(tm_route(&m, 0, 5) == TM_REFUSE_SYSTEM && tm_route(&m, 1, 5) == TM_REFUSE_SYSTEM);
    assert(tm_route(&m, 5, 5) == TM_REFUSE_DESKTOP && tm_route(&m, 7, 5) == TM_VIA_DESKTOP);
    assert(tm_route(&m, 9, 5) == TM_VIA_KERNEL && tm_route(&m, 13, 5) == TM_VIA_KERNEL && tm_route(&m, 99, 5) == TM_REFUSE_GONE);
    assert(tm_route(&m, 7, 0) == TM_VIA_KERNEL);
    puts("PASS tasks model: call-70 parsing/validation, CPU deltas and identity, kernel/idle rows, sorting, history, window lists, End Process routing");
    return 0;
}
