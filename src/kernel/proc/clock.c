/* F2 clocks. Provider selection consumes a qualified F1 sample, never ports.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/clock.h>
#include <ciuki/signal.h>

static struct clock_seed clock_source;
/* Provider extension required because fs_timestamp hides invalid samples. */
extern bool fs_calendar_sample(uint16_t *, uint16_t *, uint8_t *) __attribute__((weak));
extern int file_clock_rtc_offset(void) __attribute__((weak));
/* G1: the system time zone (proc/timezone.c); absent in host harnesses
 * that link no zone, where local time is UTC as before. */
extern int32_t timezone_offset_at(int64_t utc) __attribute__((weak));
extern int64_t timezone_local_utc(int64_t local) __attribute__((weak));
typedef bool (*fs_stamp_clock)(uint16_t *, uint16_t *, uint8_t *);
extern void fs_set_timestamp_clock(fs_stamp_clock) __attribute__((weak));
static bool clock_fat_now(uint16_t *date, uint16_t *time, uint8_t *tenths);

void file_clock_start(int64_t build_epoch, bool rtc_qualified)
{
    uint16_t date = 0, time = 0; uint8_t tenths = 0;
    bool valid = fs_calendar_sample && fs_calendar_sample(&date, &time, &tenths);
    uint32_t flags = irq_save();
    uint64_t tick = g_ticks;
    irq_restore(flags);
    file_clock_init(build_epoch, rtc_qualified, valid, date, time, tenths, tick);
    if (clock_source.source && file_clock_rtc_offset) {
        clock_source.utc-=file_clock_rtc_offset();
        proc_clock_seed(clock_source.utc,clock_source.tick);
    }
    /* G1: FAT stamps local time from the running clock, not the RTC. */
    if (clock_source.source && fs_set_timestamp_clock) fs_set_timestamp_clock(clock_fat_now);
}

void file_clock_set_utc(int64_t seconds)
{
    uint32_t flags=irq_save();
    clock_source.utc=seconds; clock_source.tick=g_ticks;
    clock_source.source=1; clock_source.valid=true; clock_source.qualified=true;
    irq_restore(flags);
    proc_clock_seed(seconds,clock_source.tick);
}

/* After a firmware suspend the PIT stopped: reseed the realtime clock from
 * the RTC, only when boot qualified that RTC as the clock source. */
bool file_clock_resync(void)
{
    uint16_t date = 0, time = 0; uint8_t tenths = 0;
    int64_t seconds;
    if (!clock_source.source || !clock_source.qualified || !fs_calendar_sample ||
        !fs_calendar_sample(&date, &time, &tenths) || tenths >= 200 || !clock_fat_civil(date, time, &seconds))
        return false;
    seconds += tenths / 100;
    if (file_clock_rtc_offset) seconds -= file_clock_rtc_offset();
    uint32_t flags = irq_save();
    clock_source.utc = seconds; clock_source.tick = g_ticks;
    uint64_t tick = clock_source.tick;
    irq_restore(flags);
    proc_clock_seed(seconds, tick);
    return true;
}

static bool leap(unsigned y) { return !(y % 4) && (y % 100 || !(y % 400)); }

/* FAT/RTC calendar fields as seconds of the same calendar since 1970. */
bool clock_fat_civil(uint16_t date, uint16_t time, int64_t *seconds)
{
    unsigned y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    unsigned h = time >> 11, min = (time >> 5) & 63, s = (time & 31) * 2;
    static const uint8_t days[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (!m || m > 12 || !d || d > (unsigned)days[m - 1] + (m == 2 && leap(y)) ||
        h > 23 || min > 59 || s > 59)
        return false;
    uint32_t count = 0;
    for (unsigned year = 1970; year < y; year++) count += 365 + leap(year);
    for (unsigned month = 1; month < m; month++) count += days[month - 1] + (month == 2 && leap(y));
    *seconds = ((int64_t)count + d - 1) * 86400 + h * 3600 + min * 60 + s;
    return true;
}

/* G1: FAT stores local time (Microsoft, "File Times"): the stamp is read
 * back through the zone rule of its own instant, so a summer stamp keeps
 * its wall-clock value in winter. */
bool clock_fat_utc(uint16_t date, uint16_t time, int64_t *seconds)
{
    int64_t local;
    if (!clock_fat_civil(date, time, &local)) return false;
    *seconds = timezone_local_utc ? timezone_local_utc(local) : local;
    return true;
}

static bool clock_fat_now(uint16_t *date, uint16_t *time, uint8_t *tenths)
{
    struct ciuki_timespec now;
    if (file_clock_now(CLOCK_REALTIME, 0, &now)) return false;
    int64_t local = now.tv_sec + (timezone_offset_at ? timezone_offset_at(now.tv_sec) : 0);
    if (local < 315532800LL || local > 4354819199LL) return false;      /* FAT: 1980..2107 */
    int64_t days = local / 86400, rem = local % 86400;
    unsigned y = 1970, m = 1;
    static const uint8_t md[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    while (days >= 365 + leap(y)) { days -= 365 + leap(y); y++; }
    while (days >= md[m - 1] + (m == 2 && leap(y))) { days -= md[m - 1] + (m == 2 && leap(y)); m++; }
    *date = (uint16_t)((y - 1980) << 9 | m << 5 | (unsigned)(days + 1));
    *time = (uint16_t)((rem / 3600) << 11 | ((rem / 60) % 60) << 5 | (rem % 60) / 2);
    *tenths = (uint8_t)((rem % 2) * 100 + now.tv_nsec / 10000000);
    return true;
}

void file_clock_init(int64_t build_epoch, bool qualified, bool valid,
                     uint16_t date, uint16_t time, uint8_t tenths, uint64_t tick)
{
    int64_t seconds;
    valid = valid && tenths < 200 && clock_fat_civil(date, time, &seconds);
    clock_source = (struct clock_seed){ .utc = build_epoch, .qualified = qualified, .valid = valid };
    if (qualified && valid) {
        clock_source.utc = seconds + tenths / 100;
        clock_source.tick = tick;
        clock_source.source = 1;
    }
    proc_clock_seed(clock_source.utc, clock_source.tick);
}

void file_clock_snapshot(struct clock_seed *out) { *out = clock_source; }

void file_clock_tick(void)
{
    struct proc_thread *t = proc_thread_for(g_current);
    if (t && g_current->state == T_RUNNING && t->process->cpu_ticks != UINT64_MAX)
        t->process->cpu_ticks++;
}

int file_clock_now(uint32_t clock, const struct process *p, struct ciuki_timespec *out)
{
    if (clock > CLOCK_PROCESS_CPUTIME_ID) return -EINVAL;
    uint32_t flags = irq_save();
    uint64_t ticks = clock == CLOCK_PROCESS_CPUTIME_ID ? (p ? p->cpu_ticks : 0) : g_ticks;
    struct clock_seed seed = clock_source;
    irq_restore(flags);
    int64_t seconds = 0;
    if (clock == CLOCK_REALTIME) {
        ticks -= seed.tick;
        seconds = seed.utc;
    }
    int64_t elapsed = (int64_t)(ticks / 1000);
    if (seconds > INT64_MAX - elapsed) {
        *out = (struct ciuki_timespec){ .tv_sec = INT64_MAX, .tv_nsec = 999000000 };
        return 0;
    }
    *out = (struct ciuki_timespec){ .tv_sec = seconds + elapsed,
        .tv_nsec = (int32_t)(ticks % 1000) * 1000000 };
    return 0;
}

int file_clock_res(uint32_t clock, struct ciuki_timespec *out)
{
    if (clock > CLOCK_PROCESS_CPUTIME_ID) return -EINVAL;
    *out = (struct ciuki_timespec){ .tv_nsec = 1000000 };
    return 0;
}

int file_sleep_deadline(const struct ciuki_timespec *request, uint64_t now, uint64_t *deadline)
{
    if (request->reserved || request->tv_sec < 0 || request->tv_nsec < 0 || request->tv_nsec >= 1000000000)
        return -EINVAL;
    uint64_t seconds = (uint64_t)request->tv_sec;
    uint32_t fraction = ((uint32_t)request->tv_nsec + 999999) / 1000000;
    if (seconds > (UINT64_MAX - fraction) / 1000) return -EOVERFLOW;
    uint64_t duration = seconds * 1000 + fraction;
    if (duration > UINT64_MAX - now) return -EOVERFLOW;
    *deadline = now + duration;
    return 0;
}

int file_nanosleep(uint32_t request_va, uint32_t remaining_va)
{
    struct proc_thread *t = proc_thread_for(g_current);
    struct ciuki_timespec request, remaining = { 0 };
    int err = copy_from_user(&request, request_va, sizeof(request));
    uint64_t deadline;
    uint32_t flags = irq_save();
    if (!err) err = file_sleep_deadline(&request, g_ticks, &deadline);
    irq_restore(flags);
    if (err) return err;
    struct ua_pin pin;
    if (remaining_va && (err = ua_pin(t->process->memory, &pin, remaining_va, sizeof(remaining), true)))
        return err;
    flags = irq_save();
    while (g_ticks < deadline) {
        if (t->process->state == PROC_STOPPING ||
            proc_signal_wait(t, SIGNAL_WAIT_I, false, false, 0) == SIGNAL_WAIT_EINTR) {
            uint64_t ticks = deadline - g_ticks;
            remaining.tv_sec = (int64_t)(ticks / 1000);
            remaining.tv_nsec = (int32_t)(ticks % 1000) * 1000000;
            err = -EINTR;
            break;
        }
        ktimer_wait_task(g_current, deadline);
        g_current->state = T_BLOCKED;
        irq_restore(flags);
        schedule();
        flags = irq_save();
    }
    g_current->wake_tick = 0;
    irq_restore(flags);
    if (remaining_va) {
        int copy = copy_to_user(remaining_va, &remaining, sizeof(remaining));
        ua_unpin(t->process->memory, &pin);
        if (copy) err = copy;
    }
    return err;
}
