/* SPDX-License-Identifier: MIT
 * G11 power policy, pure functions (power_policy.h). The Windows 98
 * behaviour and the sources are in docs/design/power-policy.md. */
#include "power_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN 60u
#define HOUR 3600u
/* Windows 98's Power Schemes lists: Never, 1..45 minutes, 1..5 hours. */
const uint32_t pp_choices[] = {0, 1 * MIN, 2 * MIN, 3 * MIN, 5 * MIN, 10 * MIN, 15 * MIN, 20 * MIN, 25 * MIN,
                               30 * MIN, 45 * MIN, 1 * HOUR, 2 * HOUR, 3 * HOUR, 4 * HOUR, 5 * HOUR};
const unsigned pp_choice_count = sizeof(pp_choices) / sizeof(pp_choices[0]);
/* CiukiOS values satisfying the Resource Kit's relations (power-policy.md 2).
 * Turn off hard disks starts at Never in every scheme until the ATA standby
 * and wake path is qualified on hardware (lead decision 2026-10-24); the
 * user can still choose a time. */
static const struct pp_scheme builtin[PP_BUILTIN] = {
    {"Home/Office Desk", {{20 * MIN, 15 * MIN, 0}, {20 * MIN, 15 * MIN, 0}}},
    {"Portable/Laptop", {{20 * MIN, 15 * MIN, 0}, {5 * MIN, 2 * MIN, 0}}},
    {"Always On", {{0, 15 * MIN, 0}, {0, 15 * MIN, 0}}},
};
static const char *const button_names[] = {"shutdown", "standby"};
static const char *const lid_names[] = {"none", "standby", "shutdown"};

void pp_defaults(struct pp_config *c)
{
    memset(c, 0, sizeof(*c));
    memcpy(c->schemes, builtin, sizeof(builtin));
    c->count = PP_BUILTIN; c->active = -1;
    c->low = (struct pp_alarm){1, 10, 0, 1, PP_ACTION_NONE, 0};
    c->critical = (struct pp_alarm){1, 3, 1, 1, PP_ACTION_STANDBY, 0};
    c->show_meter = 1; c->power_button = PP_BUTTON_SHUTDOWN; c->lid = PP_LID_NONE;
}
static int number(const char *s, unsigned long max, unsigned long *out)
{
    if (!*s || *s < '0' || *s > '9') return 0;
    char *end; unsigned long v = strtoul(s, &end, 10);
    if (*end || v > max) return 0;
    *out = v; return 1;
}
/* "a,b,c" into at most n numbers; every field present and in range. */
static int numbers(const char *s, unsigned long *v, unsigned n, unsigned long max)
{
    char field[16];
    for (unsigned i = 0; i < n; i++) {
        const char *comma = strchr(s, ',');
        size_t len = comma ? (size_t)(comma - s) : strlen(s);
        if (len >= sizeof(field) || (i + 1 < n) != (comma != NULL)) return 0;
        memcpy(field, s, len); field[len] = 0;
        if (!number(field, max, &v[i])) return 0;
        s = comma ? comma + 1 : s + len;
    }
    return 1;
}
static int timer_valid(unsigned long seconds)
{
    for (unsigned i = 0; i < pp_choice_count; i++) if (pp_choices[i] == seconds) return 1;
    return 0;
}
static int name_chars(const char *name)
{
    size_t n = strlen(name);
    if (!n || n >= PP_NAME_MAX) return 0;
    for (const char *p = name; *p; p++) if ((unsigned char)*p < 32 || *p == ',' || *p == '=' || *p == 127) return 0;
    return 1;
}
static int scheme_line(struct pp_config *c, unsigned index, const char *v)
{
    const char *comma = strchr(v, ',');
    if (!comma || index >= PP_SCHEMES_MAX) return 0;
    char name[PP_NAME_MAX]; size_t len = (size_t)(comma - v);
    if (len >= sizeof(name)) return 0;
    memcpy(name, v, len); name[len] = 0;
    unsigned long t[PP_SOURCES * PP_TIMERS];
    if (!name_chars(name) || !numbers(comma + 1, t, PP_SOURCES * PP_TIMERS, PP_TIMER_MAX)) return 0;
    for (unsigned i = 0; i < PP_SOURCES * PP_TIMERS; i++) if (!timer_valid(t[i])) return 0;
    /* Built-in slots keep their names; saved ones follow without gaps. */
    if (index >= PP_BUILTIN && index != c->count) return 0;
    struct pp_scheme *s = &c->schemes[index];
    if (index >= PP_BUILTIN) { memcpy(s->name, name, len + 1); c->count = index + 1; }
    else if (strcmp(name, builtin[index].name)) return 0;
    for (unsigned i = 0; i < PP_SOURCES * PP_TIMERS; i++) s->timer[i / PP_TIMERS][i % PP_TIMERS] = (uint32_t)t[i];
    return 1;
}
static int alarm_line(struct pp_alarm *a, const char *v)
{
    unsigned long f[6];
    if (!numbers(v, f, 6, 100) || f[0] > 1 || f[2] > 1 || f[3] > 1 || f[4] > PP_ACTION_SHUTDOWN || f[5] > 1) return 0;
    *a = (struct pp_alarm){(int)f[0], (int)f[1], (int)f[2], (int)f[3], (int)f[4], (int)f[5]};
    return 1;
}
static int named(const char *v, const char *const *names, unsigned n, int *out)
{
    for (unsigned i = 0; i < n; i++) if (!strcmp(v, names[i])) { *out = (int)i; return 1; }
    return 0;
}
static int assign(struct pp_config *c, const char *key, const char *v)
{
    unsigned long n;
    if (!strcmp(key, "power_scheme")) {
        if (!strcmp(v, "auto")) { c->active = -1; return 1; }
        if (!number(v, PP_SCHEMES_MAX - 1, &n)) return 0;
        c->active = (int)n; return 1;
    }
    if (!strncmp(key, "power_scheme", 12) && number(key + 12, PP_SCHEMES_MAX - 1, &n)) return scheme_line(c, (unsigned)n, v);
    if (!strcmp(key, "power_alarm_low")) return alarm_line(&c->low, v);
    if (!strcmp(key, "power_alarm_critical")) return alarm_line(&c->critical, v);
    if (!strcmp(key, "power_meter") || !strcmp(key, "power_password")) {
        if (!number(v, 1, &n)) return 0;
        *(key[6] == 'm' ? &c->show_meter : &c->password) = (int)n; return 1;
    }
    if (!strcmp(key, "power_button")) return named(v, button_names, 2, &c->power_button);
    if (!strcmp(key, "power_lid")) return named(v, lid_names, 3, &c->lid);
    return 0;
}
int pp_load(struct pp_config *c, const char *extra)
{
    pp_defaults(c);
    int rejected = 0;
    char line[160];
    for (const char *p = extra ? extra : ""; *p;) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len >= 6 && !strncmp(p, "power_", 6)) {
            const char *eq = memchr(p, '=', len);
            if (!eq || len >= sizeof(line)) rejected++;
            else {
                memcpy(line, p, len); line[len] = 0; line[eq - p] = 0;
                if (!assign(c, line, line + (eq - p) + 1)) rejected++;
            }
        }
        p = end ? end + 1 : p + len;
    }
    if (c->active >= (int)c->count) { c->active = -1; rejected++; }
    pp_alarm_fix(c);
    return rejected;
}
int pp_store(const struct pp_config *c, char *extra, size_t size)
{
    /* Keep every other line, in order, then append the power_* lines. */
    char *kept = malloc(size);
    if (!kept) return -1;
    size_t used = 0;
    for (const char *p = extra; *p;) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len && strncmp(p, "power_", 6)) {
            if (used + len + 2 > size) { free(kept); return -1; }
            memcpy(kept + used, p, len); used += len; kept[used++] = '\n';
        }
        p = end ? end + 1 : p + len;
    }
    int n;
#define APPEND(...) do { n = snprintf(kept + used, size - used, __VA_ARGS__); \
                         if (n < 0 || (size_t)n >= size - used) { free(kept); return -1; } used += (size_t)n; } while (0)
    if (c->active < 0) APPEND("power_scheme=auto\n"); else APPEND("power_scheme=%d\n", c->active);
    for (unsigned i = 0; i < c->count && i < PP_SCHEMES_MAX; i++) {
        const struct pp_scheme *s = &c->schemes[i];
        APPEND("power_scheme%u=%s,%u,%u,%u,%u,%u,%u\n", i, s->name, s->timer[0][0], s->timer[0][1], s->timer[0][2],
               s->timer[1][0], s->timer[1][1], s->timer[1][2]);
    }
    const struct pp_alarm *a[2] = {&c->low, &c->critical};
    for (unsigned i = 0; i < 2; i++)
        APPEND("power_alarm_%s=%d,%d,%d,%d,%d,%d\n", i ? "critical" : "low", a[i]->enabled, a[i]->level,
               a[i]->sound, a[i]->message, a[i]->action, a[i]->force);
    APPEND("power_meter=%d\npower_password=%d\npower_button=%s\npower_lid=%s\n", !!c->show_meter, !!c->password,
           button_names[c->power_button == PP_BUTTON_STANDBY], lid_names[c->lid >= 0 && c->lid <= PP_LID_SHUTDOWN ? c->lid : 0]);
#undef APPEND
    kept[used] = 0;   /* settings.c ends every extra line with a newline */
    memcpy(extra, kept, used + 1);
    free(kept);
    return 0;
}
int pp_active(const struct pp_config *c, const struct pp_summary *s)
{
    if (c->active >= 0 && (unsigned)c->active < c->count) return c->active;
    if (s && s->valid && s->bays) return PP_PORTABLE;
    if (s && s->valid && s->apm) return PP_ALWAYS_ON;
    return PP_HOME;
}
int pp_source(const struct pp_summary *s) { return s && s->valid && s->on_battery ? PP_BATTERY : PP_AC; }
uint32_t pp_timer(const struct pp_config *c, const struct pp_summary *s, int which)
{
    if (which < 0 || which >= PP_TIMERS) return 0;
    return c->schemes[pp_active(c, s)].timer[pp_source(s)][which];
}
int pp_choice_index(uint32_t seconds)
{
    if (!seconds) return 0;
    for (unsigned i = 1; i < pp_choice_count; i++) if (pp_choices[i] >= seconds) return (int)i;
    return (int)pp_choice_count - 1;
}
void pp_choice_label(uint32_t seconds, char *out, size_t size)
{
    if (!seconds) snprintf(out, size, "Never");
    else if (seconds >= HOUR && !(seconds % HOUR)) snprintf(out, size, "After %u hour%s", seconds / HOUR, seconds == HOUR ? "" : "s");
    else snprintf(out, size, "After %u min%s", (seconds + MIN - 1) / MIN, seconds <= MIN ? "" : "s");
}
int pp_name_valid(const struct pp_config *c, const char *name)
{
    if (!name || !name_chars(name)) return 0;
    for (unsigned i = 0; i < c->count; i++) if (!strcmp(c->schemes[i].name, name)) return 0;
    return 1;
}
int pp_scheme_save_as(struct pp_config *c, const char *name, int from)
{
    if (c->count >= PP_SCHEMES_MAX || from < 0 || (unsigned)from >= c->count || !pp_name_valid(c, name)) return -1;
    struct pp_scheme *s = &c->schemes[c->count];
    *s = c->schemes[from];
    memset(s->name, 0, sizeof(s->name));
    memcpy(s->name, name, strlen(name));
    return (int)c->count++;
}
int pp_scheme_delete(struct pp_config *c, int index)
{
    if (index < PP_BUILTIN || (unsigned)index >= c->count) return -1;
    memmove(&c->schemes[index], &c->schemes[index + 1], (c->count - (unsigned)index - 1) * sizeof(c->schemes[0]));
    c->count--;
    if (c->active == index) c->active = -1;
    else if (c->active > index) c->active--;
    return 0;
}
void pp_alarm_fix(struct pp_config *c)
{
    if (c->low.level < 0) c->low.level = 0;
    if (c->low.level > 100) c->low.level = 100;
    if (c->critical.level < 0) c->critical.level = 0;
    if (c->critical.level >= c->low.level && c->low.enabled) {
        if (c->low.level) c->critical.level = c->low.level - 1;
        else c->critical.enabled = 0;
    }
}
int pp_action_effective(int action, int standby_available)
{
    if (action == PP_ACTION_STANDBY && !standby_available) return PP_ACTION_SHUTDOWN;
    return action >= PP_ACTION_NONE && action <= PP_ACTION_SHUTDOWN ? action : PP_ACTION_NONE;
}

void pp_summarize(const struct ciuki_power_status *p, struct pp_summary *s)
{
    *s = (struct pp_summary){.ac = 2, .percent = -1, .seconds = -1};
    if (!p || p->version != CIUKI_POWER_VERSION || p->size != sizeof(*p) ||
        p->battery_count > CIUKI_POWER_BATTERIES || p->zone_count > CIUKI_POWER_ZONES) return;
    s->valid = 1;
    s->apm = p->provider == CIUKI_POWER_PROVIDER_APM;
    s->ac = p->ac_online == 1 ? 1 : p->ac_online == 0 || p->ac_online == CIUKI_POWER_AC_BACKUP ? 0 : 2;
    s->bays = p->battery_count;
    uint64_t full = 0, remaining = 0;
    unsigned unit = CIUKI_POWER_UNKNOWN, discharging = 0;
    int known = 1;
    for (unsigned i = 0; i < p->battery_count; i++) {
        const struct ciuki_power_battery *b = &p->batteries[i];
        if (b->present == 0) continue;   /* fx-18: an empty bay is not unknown charge */
        s->batteries++;
        if (b->state != CIUKI_POWER_UNKNOWN) {
            if (b->state & 2) s->charging = 1;
            if (b->state & 4) s->critical = 1;
            if (b->state & 1) discharging++;
        }
        if (b->present != 1 || b->unit > CIUKI_POWER_UNIT_PERCENT || !b->full || b->full == CIUKI_POWER_UNKNOWN ||
            b->remaining == CIUKI_POWER_UNKNOWN || (unit != CIUKI_POWER_UNKNOWN && unit != b->unit)) known = 0;
        else { unit = b->unit; full += b->full; remaining += b->remaining; }
        /* Firmware time (APM), else charge over rate (ACPI _BST, same unit). */
        uint32_t t = b->seconds;
        if (t == CIUKI_POWER_UNKNOWN && b->state != CIUKI_POWER_UNKNOWN && (b->state & 1) && b->unit <= 1 &&
            b->rate && b->rate != CIUKI_POWER_UNKNOWN && b->remaining != CIUKI_POWER_UNKNOWN)
            t = (uint32_t)((uint64_t)b->remaining * 3600u / b->rate);
        if (t != CIUKI_POWER_UNKNOWN && b->state != CIUKI_POWER_UNKNOWN && (b->state & 1) && (int64_t)t > s->seconds)
            s->seconds = t > INT32_MAX ? INT32_MAX : (int)t;
    }
    if (known && full && s->batteries) s->percent = remaining >= full ? 100 : (int)(remaining * 100 / full);
    s->on_battery = s->ac == 0 || (s->ac == 2 && s->batteries && discharging == s->batteries);
    if (!s->on_battery) s->seconds = -1;
}
void pp_tooltip(const struct pp_summary *s, const char *scheme, char *out, size_t size)
{
    char first[96];
    if (!s->valid || (!s->batteries && s->ac == 2)) snprintf(first, sizeof(first), "Battery status unknown");
    else if (s->on_battery) {
        if (s->percent < 0) snprintf(first, sizeof(first), "On battery power");
        else if (s->seconds >= 0)
            snprintf(first, sizeof(first), "%d hr %02d min (%d%%) remaining", s->seconds / 3600, s->seconds / 60 % 60, s->percent);
        else snprintf(first, sizeof(first), "%d%% remaining", s->percent);
    } else if (s->percent < 0) snprintf(first, sizeof(first), "AC power");
    else if (s->charging) snprintf(first, sizeof(first), "Charging (%d%%)", s->percent);
    else if (s->percent >= 100) snprintf(first, sizeof(first), "Fully charged (100%%)");
    else snprintf(first, sizeof(first), "AC power (%d%%)", s->percent);
    if (scheme && *scheme) snprintf(out, size, "%s\nPower scheme: %s", first, scheme);
    else snprintf(out, size, "%s", first);
}
unsigned pp_battery_flag(const struct pp_summary *s)
{
    if (!s->valid) return 255;
    if (s->bays && !s->batteries) return 128;
    if (!s->bays) return s->ac == 2 ? 255 : 128;
    if (s->percent < 0) return s->charging ? 8 : 255;
    unsigned f = s->percent > 66 ? 1u : s->percent < 33 ? 2u : 0u;
    if (s->percent < 5 || s->critical) f |= 4u;
    if (s->charging) f |= 8u;
    return f;
}
int pp_alarm_step(struct pp_alarm_state *a, const struct pp_config *c, const struct pp_summary *s,
                  uint32_t battery_low_count)
{
    if (!a->primed) { a->primed = 1; a->battery_low_seen = battery_low_count; }
    int apm_low = battery_low_count != a->battery_low_seen;
    a->battery_low_seen = battery_low_count;
    if (!s->valid) return PP_ALARM_NONE;
    if (!s->on_battery) { a->low_latched = a->critical_latched = 0; return PP_ALARM_NONE; }
    int result = PP_ALARM_NONE;
    if (s->percent >= 0) {
        if (c->critical.enabled && s->percent <= c->critical.level) {
            if (!a->critical_latched) { a->critical_latched = a->low_latched = 1; return PP_ALARM_CRITICAL; }
        } else a->critical_latched = 0;
        if (c->low.enabled && s->percent <= c->low.level) {
            if (!a->low_latched) { a->low_latched = 1; result = PP_ALARM_LOW; }
        } else a->low_latched = 0;
    }
    if (apm_low && c->low.enabled && !a->low_latched && !a->critical_latched) { a->low_latched = 1; result = PP_ALARM_LOW; }
    return result;
}
int pp_status_changed(struct pp_broadcast_state *b, const struct pp_summary *s)
{
    int flag = (int)pp_battery_flag(s), percent = s->valid ? s->percent : -1, seconds = s->valid ? s->seconds : -1;
    int ac = s->valid ? s->ac : 2;
    if (!b->primed) { *b = (struct pp_broadcast_state){1, ac, flag, percent, seconds}; return 0; }
    int due = ac != b->ac || flag != b->flag || (percent < 0) != (b->percent < 0) ||
              (seconds >= 0 && seconds < 300 && (b->seconds < 0 || b->seconds >= 300)) ||
              (percent >= 0 && percent < 10 && b->percent >= 10) ||
              (percent >= 0 && b->percent >= 0 && abs(percent - b->percent) >= 3);
    b->ac = ac; b->flag = flag; b->seconds = seconds;
    if (due) b->percent = percent;   /* a slow drift adds up to the 3 % step */
    return due;
}
