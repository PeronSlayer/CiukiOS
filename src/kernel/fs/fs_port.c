/* SPDX-License-Identifier: GPL-2.0-only */
#include "fs_port.h"
#ifndef FS_HOST
#include <ciuki/sync.h>
#include <ciuki/work.h>
#endif
static fs_calendar_clock calendar;
void fs_set_calendar_clock(fs_calendar_clock clock) { calendar=clock; }
void fs_timestamp(uint16_t *date, uint16_t *time, uint8_t *tenths) {
    if (calendar && calendar(date,time,tenths)) return;
    *date=0x21; *time=0; *tenths=0;
}

/* Behavioral references, not copied implementation:
 * https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/rtc/rtc-mc146818-lib.c
 * Seconds are sampled BEFORE UIP and checked again AFTER the fields/UIP.
 * https://raw.githubusercontent.com/torvalds/linux/v6.12/include/linux/mc146818rtc.h
 * Register B selects binary/BCD and 24/12 hours; bit 7 of a 12h hour is PM.
 * https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf
 * Section 6.3: local calendar, 1980..2107, two-second packed time. The
 * creation fraction holds hundredths within that two-second interval, so
 * an odd RTC second contributes 100. No subsecond precision is invented.
 * CBI1 does not identify a century register: do not assume CMOS 32h. The
 * explicit pivot is 80: years 80..99 => 1980..1999, 00..79 => 2000..2079. */
static bool rtc_number(uint8_t raw, bool binary, unsigned *out)
{
    if (!binary && ((raw & 15) > 9 || (raw >> 4) > 9)) return false;
    *out = binary ? raw : (raw >> 4) * 10u + (raw & 15);
    return true;
}

bool fs_rtc_decode(const struct fs_rtc_sample *s, uint16_t *date, uint16_t *time, uint8_t *tenths)
{
    if (!s || !date || !time || !tenths || !(s->valid & 0x80) || (s->control & 0x80)) return false;
    bool binary = !!(s->control & 4), mode24 = !!(s->control & 2);
    unsigned sec, min, hour, day, month, year;
    if (!rtc_number(s->second, binary, &sec) || !rtc_number(s->minute, binary, &min) ||
        !rtc_number(mode24 ? s->hour : s->hour & 0x7f, binary, &hour) ||
        !rtc_number(s->day, binary, &day) || !rtc_number(s->month, binary, &month) ||
        !rtc_number(s->year, binary, &year)) return false;
    if (sec > 59 || min > 59 || year > 99 || !month || month > 12) return false;
    if (mode24) { if (hour > 23) return false; }
    else {
        if (!hour || hour > 12) return false;
        hour = hour % 12 + ((s->hour & 0x80) ? 12 : 0);
    }
    year += year >= 80 ? 1900 : 2000;
    static const uint8_t days[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    unsigned limit = days[month - 1];
    if (month == 2 && !(year % 4) && (year % 100 || !(year % 400))) limit++;
    if (!day || day > limit) return false;
    *date = (uint16_t)((year - 1980) << 9 | month << 5 | day);
    *time = (uint16_t)(hour << 11 | min << 5 | sec / 2);
    *tenths = (uint8_t)((sec & 1) * 100);
    return true;
}

bool fs_rtc_read(const struct fs_rtc_ops *ops, void *ctx, uint16_t *date, uint16_t *time, uint8_t *tenths)
{
    if (!ops || !ops->begin || !ops->end || !ops->read || !ops->now_ms || !ops->pause) return false;
    uint32_t start = ops->now_ms(ctx);
    /* 100 ms plus a finite stalled-clock escape; waits are outside IF=0. */
    for (unsigned attempt = 0; attempt < 1024; attempt++) {
        if ((uint32_t)(ops->now_ms(ctx) - start) >= 100) break;
        struct fs_rtc_sample s = { 0 };
        ops->begin(ctx);
        s.second = ops->read(ctx, 0);
        bool stable = !(ops->read(ctx, 0x0a) & 0x80) && s.second == ops->read(ctx, 0);
        if (stable) {
            s.control = ops->read(ctx, 0x0b);
            s.minute = ops->read(ctx, 2); s.hour = ops->read(ctx, 4);
            s.day = ops->read(ctx, 7); s.month = ops->read(ctx, 8); s.year = ops->read(ctx, 9);
            s.valid = ops->read(ctx, 0x0d);
            stable = !(ops->read(ctx, 0x0a) & 0x80) && s.second == ops->read(ctx, 0) &&
                     s.control == ops->read(ctx, 0x0b);
        }
        ops->end(ctx);
        if (stable) return fs_rtc_decode(&s, date, time, tenths);
        if (!ops->pause(ctx)) break;
    }
    return false;
}

#ifndef FS_HOST
/* UP platform service, sole native CMOS accessor. Software policy leaves NMI
 * enabled (shadow bit 7 = 0); never read port 70h to infer this write-only
 * policy. IRQ8 remains masked, register C is never read, port 71h never
 * written. NMI diagnostics do not access CMOS. Each complete sample is one
 * short irq-save section, not the UIP wait. */
static uint32_t rtc_flags;
static const uint8_t rtc_nmi_shadow = 0;
static void rtc_begin(void *ctx) { (void)ctx; rtc_flags = irq_save(); }
static void rtc_end(void *ctx) { (void)ctx; irq_restore(rtc_flags); }
static uint8_t rtc_read(void *ctx, uint8_t reg)
{
    (void)ctx; outb(0x70, rtc_nmi_shadow | reg); return inb(0x71);
}
static uint32_t rtc_now(void *ctx) { (void)ctx; return fs_now_ms(); }
static bool rtc_pause(void *ctx) { (void)ctx; return !udelay(100); }
static bool rtc_calendar(uint16_t *date, uint16_t *time, uint8_t *tenths)
{
    static const struct fs_rtc_ops ops = { rtc_begin, rtc_end, rtc_read, rtc_now, rtc_pause };
    return fs_rtc_read(&ops, 0, date, time, tenths);
}
static struct task *fs_worker;
static uint32_t service_tick;
void fs_calendar_init(void) { fs_set_calendar_clock(rtc_calendar); }
void fs_worker_enter(void) { fs_worker = g_current; service_tick = fs_now_ms(); }
void fs_worker_leave(void) { fs_worker = 0; }
void fs_service(void)
{
    if (g_current && fs_now_ms() != service_tick) {
        if (g_current == fs_worker) kwork_yield(); else task_yield();
        service_tick = fs_now_ms();
    }
}
#else
void fs_calendar_init(void) { }
void fs_worker_enter(void) { }
void fs_worker_leave(void) { }
void fs_service(void) { }
#endif
