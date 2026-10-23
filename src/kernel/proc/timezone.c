/* G1: system time zone, local time and the RTC kept in local time.
 *
 * Windows model (docs/design/local-time.md): the kernel keeps UTC
 * (Microsoft, SetLocalTime: "The system uses UTC internally"); the
 * battery-backed RTC holds LOCAL time on machines set up for Windows; FAT
 * stores local time (Microsoft, "File Times"); the time-zone setting has a
 * daylight-saving rule applied automatically unless the user turns it off
 * (Date/Time Properties, "Automatically adjust clock for daylight saving
 * changes"). When the offset changes the RTC is rewritten so that it keeps
 * holding local time, and the change is reported once (CIUKI_TIME_ADJUSTED)
 * so the desktop can tell the user, as Windows does.
 *
 * clock.cfg records rtc_bias_minutes, the offset of the local time the RTC
 * holds. Boot reads RTC - bias, so a daylight-saving change that happened
 * while the machine was off is still read correctly and corrected at the
 * first poll.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/clock.h>
#include <ciuki/process.h>
#include <ciuki/uaddr.h>
#include <ciuki/tzrule.h>
#include <ciuki/systime.h>

extern bool fs_rtc_store(uint16_t, uint16_t, uint8_t) __attribute__((weak));

/* Host tests substitute the store; NULL selects setup_clock.c's file. */
static int (*tz_store_read)(char *, size_t, size_t *);
static int (*tz_store_write)(const char *, size_t);
void timezone_set_store(int (*r)(char *, size_t, size_t *), int (*w)(const char *, size_t))
{ tz_store_read = r; tz_store_write = w; }
static int tz_read_store(char *text, size_t cap, size_t *n)
{
    if (tz_store_read) return tz_store_read(text, cap, n);
    return timezone_store_read(text, cap, n);   /* proc/setup_clock.c */
}
static int tz_write_store(const char *text, size_t n)
{
    if (tz_store_write) return tz_store_write(text, n);
    return timezone_store_write(text, n);
}

static struct tzr_config tz_config;
static struct tzr tz_rule;
static bool tz_loaded, tz_adjusted, tz_busy;
static uint32_t tz_generation;

void timezone_reset(void)
{
    uint32_t f = irq_save();
    tz_loaded = tz_adjusted = tz_busy = false; tz_generation = 0;
    irq_restore(f);
}

static void tz_load(void)
{
    if (tz_loaded) return;
    struct tzr_config c;
    struct tzr r;
    char text[512];
    size_t n = 0;
    tzr_config_default(&c);
    if (!tz_read_store(text, sizeof(text), &n) &&
        tzr_config_parse(text, n, &c) && tzr_config_rule(&c, &r)) {
        /* parsed */
    } else {
        tzr_config_default(&c);
        tzr_config_rule(&c, &r);
    }
    uint32_t f = irq_save();
    if (!tz_loaded) { tz_config = c; tz_rule = r; tz_loaded = true; }
    irq_restore(f);
}

static void tz_snapshot(struct tzr *r, struct tzr_config *c)
{
    tz_load();
    uint32_t f = irq_save();
    if (r) *r = tz_rule;
    if (c) *c = tz_config;
    irq_restore(f);
}

int file_clock_rtc_offset(void)
{
    struct tzr_config c;
    tz_snapshot(0, &c);
    return c.rtc_local ? c.rtc_bias_minutes * 60 : 0;
}

int32_t timezone_offset_at(int64_t utc)
{
    struct tzr r;
    tz_snapshot(&r, 0);
    return tzr_offset(&r, utc, 0);
}

int64_t timezone_local_utc(int64_t local)
{
    struct tzr r;
    tz_snapshot(&r, 0);
    return tzr_local_to_utc(&r, local, -1, 0);
}

static int64_t tz_now_utc(uint32_t *ms)
{
    struct ciuki_timespec t = {0};
    file_clock_now(CLOCK_REALTIME, 0, &t);
    if (ms) *ms = (uint32_t)t.tv_nsec / 1000000u;
    return t.tv_sec;
}

/* The RTC's local calendar, 1980..2079 (the reader's two-digit pivot). */
static int tz_rtc_write(int64_t local)
{
    if (local < 315532800LL || local > 3471292799LL) return -EINVAL;
    if (!fs_rtc_store) return -EIO;
    int64_t y; unsigned m, d;
    int64_t days = tzr_floor_div(local, 86400), rem = local - days * 86400;
    tzr_civil_from_days(days, &y, &m, &d);
    uint16_t date = (uint16_t)((y - 1980) << 9 | m << 5 | d);
    uint16_t time = (uint16_t)((rem / 3600) << 11 | ((rem / 60) % 60) << 5 | (rem % 60) / 2);
    return fs_rtc_store(date, time, (uint8_t)((rem % 2) * 100)) ? 0 : -EIO;
}

static int tz_persist(const struct tzr_config *c)
{
    char text[512];
    size_t n = tzr_config_format(c, text, sizeof(text));
    if (!n) return -EINVAL;
    return tz_write_store(text, n);
}

static bool tz_enter(void)
{
    tz_load();
    uint32_t f = irq_save();
    bool ok = !tz_busy;
    if (ok) tz_busy = true;
    irq_restore(f);
    return ok;
}
static void tz_leave(const struct tzr_config *c, const struct tzr *r, bool changed)
{
    uint32_t f = irq_save();
    if (c) tz_config = *c;
    if (r) tz_rule = *r;
    if (changed) tz_generation++;
    tz_busy = false;
    irq_restore(f);
}

/* Moves the RTC to utc's local time under c/r and records the new bias.
 * The configuration is written first: if the RTC write then fails, the old
 * record is restored so that boot never applies a bias the RTC lacks. */
static int tz_commit(struct tzr_config *c, const struct tzr *r, int64_t utc, const struct tzr_config *old)
{
    int32_t offset = tzr_offset(r, utc, 0);
    if (c->rtc_local) c->rtc_bias_minutes = offset / 60;
    else c->rtc_bias_minutes = 0;
    int e = tz_persist(c);
    if (e) return e;
    e = tz_rtc_write(utc + (c->rtc_local ? offset : 0));
    if (e) tz_persist(old);
    return e;
}

bool timezone_poll(void)
{
    if (!tz_enter()) return false;
    struct tzr_config c = tz_config, old = tz_config;
    struct tzr r = tz_rule;
    bool moved = false;
    if (c.rtc_local) {
        int64_t utc = tz_now_utc(0);
        int32_t offset = tzr_offset(&r, utc, 0) / 60;
        if (offset != c.rtc_bias_minutes && !tz_commit(&c, &r, utc, &old)) {
            moved = true;
            uint32_t f = irq_save(); tz_adjusted = true; irq_restore(f);
        }
    }
    tz_leave(moved ? &c : 0, 0, moved);
    if (moved)
        klog("[time] step=dst_adjust bias=%d zone=%s", c.rtc_bias_minutes, c.zone[0] ? c.zone : "-");
    return moved;
}

size_t timezone_env(char *out, size_t capacity)
{
    struct tzr_config c;
    tz_snapshot(0, &c);
    char text[TZR_TEXT];
    const char *value = c.tz;
    if (!c.auto_dst && tzr_standard_only(c.tz, text, sizeof(text))) value = text;
    size_t n = 3, i = 0;
    while (value[i]) i++;
    if (!out || capacity < n + i + 1) return 0;
    out[0] = 'T'; out[1] = 'Z'; out[2] = '=';
    for (size_t k = 0; k < i; k++) out[n + k] = value[k];
    out[n + i] = 0;
    return n + i;
}

static void tz_copy_name(char *out, size_t cap, const char *in)
{
    size_t i = 0;
    for (; in && in[i] && i + 1 < cap; i++) out[i] = in[i];
    for (; i < cap; i++) out[i] = 0;
}

static void tz_fill(struct process *p, struct ciuki_time_zone *q, uint32_t operation)
{
    struct tzr r;
    struct tzr_config c;
    uint32_t f = irq_save();
    r = tz_rule; c = tz_config;
    bool adj = tz_adjusted;
    uint32_t gen = tz_generation;
    irq_restore(f);
    uint32_t ms;
    int64_t utc = tz_now_utc(&ms), next = 0;
    bool daylight;
    int32_t offset = tzr_offset(&r, utc, &daylight);
    *q = (struct ciuki_time_zone){.version = CIUKI_TIME_VERSION, .size = sizeof(*q), .operation = operation,
        .utc_seconds = utc, .local_seconds = utc + offset, .milliseconds = ms,
        .offset_minutes = offset / 60, .standard_minutes = r.std_offset / 60,
        .daylight_minutes = (r.has_dst ? r.dst_offset : r.std_offset) / 60,
        .rtc_bias_minutes = c.rtc_local ? c.rtc_bias_minutes : 0, .generation = gen};
    q->flags = (c.auto_dst ? CIUKI_TIME_AUTO_DST : 0) | (c.rtc_local ? CIUKI_TIME_RTC_LOCAL : 0) |
               (daylight ? CIUKI_TIME_DAYLIGHT : 0) | (adj ? CIUKI_TIME_ADJUSTED : 0) |
               (p && p->time_capability ? CIUKI_TIME_AUTHORITY : 0);
    if (tzr_next_change(&r, utc, &next)) q->next_change = next;
    tz_copy_name(q->zone, sizeof(q->zone), c.zone);
    tz_copy_name(q->rule, sizeof(q->rule), c.tz);
    tz_copy_name(q->standard_name, sizeof(q->standard_name), r.std);
    tz_copy_name(q->daylight_name, sizeof(q->daylight_name), r.has_dst ? r.dst : "");
}

static bool tz_terminated(const char *s, size_t cap)
{
    for (size_t i = 0; i < cap; i++) if (!s[i]) return true;
    return false;
}

static int tz_set_zone(const struct ciuki_time_zone *q, struct tzr_config *c, struct tzr *r)
{
    if (!tz_terminated(q->zone, sizeof(q->zone)) || !tz_terminated(q->rule, sizeof(q->rule)) ||
        (q->flags & ~(uint32_t)CIUKI_TIME_AUTO_DST)) return -EINVAL;
    struct tzr_config old = *c;
    tz_copy_name(c->zone, sizeof(c->zone), q->zone);
    tz_copy_name(c->tz, sizeof(c->tz), q->rule);
    for (size_t i = 0; c->zone[i]; i++) {
        char ch = c->zone[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '/' || ch == '_' || ch == '-' || ch == '+')) return -EINVAL;
    }
    c->auto_dst = !!(q->flags & CIUKI_TIME_AUTO_DST);
    struct tzr full;
    if (!tzr_parse(c->tz, &full) || !tzr_config_rule(c, r)) return -EINVAL;
    c->offset_minutes = full.std_offset / 60;
    if (c->offset_minutes < -720 || c->offset_minutes > 840) return -EINVAL;
    int e = tz_commit(c, r, tz_now_utc(0), &old);
    if (!e) klog("[time] step=zone zone=%s tz_rule=%s auto_dst=%u", c->zone[0] ? c->zone : "-", c->tz, c->auto_dst);
    return e;
}

static int tz_set_utc(int64_t utc, struct tzr_config *c, const struct tzr *r)
{
    struct tzr_config old = *c;
    if (utc < 0) return -EINVAL;
    int64_t local = utc + tzr_offset(r, utc, 0);
    if (local < 315532800LL || local > 3471292799LL) return -EINVAL;
    int e = tz_commit(c, r, utc, &old);
    if (e) return e;
    file_clock_set_utc(utc);
    klog("[time] step=set utc=%lld bias=%d", (long long)utc, c->rtc_bias_minutes);
    return 0;
}

int timezone_call(struct process *p, struct ciuki_time_zone *q)
{
    if (q->version != CIUKI_TIME_VERSION || q->size != sizeof(*q)) return -EINVAL;
    uint32_t op = q->operation;
    if (op > CIUKI_TIME_ACKNOWLEDGE) return -EINVAL;
    if (op == CIUKI_TIME_GET) timezone_poll();
    else {
        if (!p || !p->time_capability) return -EPERM;
        if (!tz_enter()) return -EBUSY;
        struct tzr_config c = tz_config;
        struct tzr r = tz_rule;
        int e = 0;
        if (op == CIUKI_TIME_SET_ZONE) e = tz_set_zone(q, &c, &r);
        else if (op == CIUKI_TIME_SET_UTC) e = tz_set_utc(q->utc_seconds, &c, &r);
        else if (op == CIUKI_TIME_SET_LOCAL)
            e = q->local_seconds < 0 ? -EINVAL : tz_set_utc(tzr_local_to_utc(&r, q->local_seconds, -1, 0), &c, &r);
        else { uint32_t f = irq_save(); tz_adjusted = false; irq_restore(f); }
        bool changed = !e && op != CIUKI_TIME_ACKNOWLEDGE;
        tz_leave(changed ? &c : 0, changed ? &r : 0, changed);
        if (e) return e;
    }
    tz_fill(p, q, op);
    return 0;
}

int32_t time_syscall(struct trap_frame *tf)
{
    struct ciuki_time_zone q;
    if (tf->ecx != sizeof(q)) return -EINVAL;
    if (copy_from_user(&q, tf->ebx, sizeof(q))) return -EFAULT;
    int e = timezone_call(proc_current(), &q);
    if (e) return e;
    return copy_to_user(tf->ebx, &q, sizeof(q)) ? -EFAULT : 0;
}
