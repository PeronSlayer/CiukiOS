/* SY-7 Task Scheduler and Maintenance Wizard. SPDX-License-Identifier: MIT */
#include "systools.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400), doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}
static int month_days(int y, int m)
{
    static const int n[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 29 : n[m - 1];
}
static int date_ok(struct st_date d) { return d.month >= 1 && d.month <= 12 && d.year >= 1970 && d.year < 3000 && d.day >= 1 && d.day <= month_days(d.year, d.month); }
int64_t st_minutes(struct st_date d, unsigned minute) { return days_from_civil(d.year, d.month, d.day) * 1440 + minute; }
int st_weekday(struct st_date d) { int64_t z = days_from_civil(d.year, d.month, d.day); return (int)((z % 7 + 11) % 7); }  /* 1970-01-01 was Thursday */

void st_init(struct st_store *s) { memset(s, 0, sizeof(*s)); }
static int matches(const struct st_task *t, int64_t day)
{
    int y, m, d;
    civil_from_days(day, &y, &m, &d);
    switch (t->kind) {
    case ST_DAILY: return 1;
    case ST_WEEKLY: return (t->weekday_mask >> ((day % 7 + 11) % 7)) & 1;
    case ST_MONTHLY: { int dm = month_days(y, m); return d == (int)(t->day_of_month > (unsigned)dm ? (unsigned)dm : t->day_of_month); }
    case ST_ONCE: return day == days_from_civil(t->start.year, t->start.month, t->start.day);
    }
    return 0;
}
int64_t st_next(const struct st_task *t, int64_t after)
{
    if (!t->enabled || t->kind == ST_AT_START || !date_ok(t->start)) return -1;
    int64_t first = days_from_civil(t->start.year, t->start.month, t->start.day);
    int64_t day = after / 1440 - (after < 0 && after % 1440 ? 1 : 0);
    if (day < first) day = first;
    for (int i = 0; i < 800; i++, day++) {
        if (!matches(t, day)) continue;
        int64_t at = day * 1440 + t->minute_of_day;
        if (at > after) return at;
        if (t->kind == ST_ONCE) return -1;
    }
    return -1;
}
unsigned st_due(const struct st_store *s, int64_t now, int startup, unsigned *out, unsigned cap)
{
    unsigned n = 0;
    for (unsigned i = 0; i < s->count && n < cap; i++) {
        const struct st_task *t = &s->t[i];
        if (!t->enabled) continue;
        if (t->kind == ST_AT_START) { if (startup) out[n++] = i; continue; }
        int64_t base = t->last_run >= 0 ? t->last_run : t->created, next = st_next(t, base);
        if (next >= 0 && next <= now) out[n++] = i;      /* a missed run happens once, not once per missed day */
    }
    return n;
}
void st_mark_run(struct st_store *s, unsigned i, int64_t now, int result)
{
    if (i >= s->count) return;
    s->t[i].last_run = now; s->t[i].last_result = result;
    if (s->t[i].kind == ST_ONCE) s->t[i].enabled = 0;
}
static int valid(const struct st_task *t)
{
    if (!t->name[0] || !t->command[0] || memchr(t->name, 0, sizeof(t->name)) == NULL || memchr(t->command, 0, sizeof(t->command)) == NULL) return 0;
    for (const char *p = t->name; *p; p++) if (*p == '\n' || *p == '\r' || *p == '=') return 0;
    for (const char *p = t->command; *p; p++) if (*p == '\n' || *p == '\r') return 0;
    if (t->kind < ST_DAILY || t->kind > ST_AT_START || t->minute_of_day > 1439) return 0;
    if (t->kind != ST_AT_START && !date_ok(t->start)) return 0;
    if (t->kind == ST_WEEKLY && !(t->weekday_mask & 0x7f)) return 0;
    if (t->kind == ST_MONTHLY && (t->day_of_month < 1 || t->day_of_month > 31)) return 0;
    return 1;
}
int st_add(struct st_store *s, const struct st_task *t)
{
    if (!valid(t) || t->created < 0) return -EINVAL;
    for (unsigned i = 0; i < s->count; i++) if (!strcmp(s->t[i].name, t->name)) return -EEXIST;
    if (s->count >= ST_MAX) return -ENOSPC;
    s->t[s->count] = *t;
    s->t[s->count].enabled = 1;
    if (!t->last_run) s->t[s->count].last_run = -1;
    s->count++;
    return 0;
}
int st_remove(struct st_store *s, const char *name)
{
    for (unsigned i = 0; i < s->count; i++)
        if (!strcmp(s->t[i].name, name)) { memmove(&s->t[i], &s->t[i + 1], (s->count - i - 1) * sizeof(s->t[0])); s->count--; return 0; }
    return -ENOENT;
}
int st_save(const struct st_store *s, char *out, size_t cap)
{
    size_t at = 0;
    int w = snprintf(out, cap, "version=1\n");
    if (w < 0 || (size_t)w >= cap) return -ENOSPC;
    at = (size_t)w;
    for (unsigned i = 0; i < s->count; i++) {
        const struct st_task *t = &s->t[i];
        w = snprintf(out + at, cap - at, "task=%s|%d|%d|%u|%u|%u|%04d-%02d-%02d|%lld|%lld|%d|%s\n", t->name, t->kind, t->enabled, t->minute_of_day,
                     t->weekday_mask, t->day_of_month, t->start.year, t->start.month, t->start.day, (long long)t->created, (long long)t->last_run,
                     t->last_result, t->command);
        if (w < 0 || (size_t)w >= cap - at) return -ENOSPC;
        at += (size_t)w;
    }
    return (int)at;
}
int st_load(struct st_store *s, const char *text)
{
    st_init(s);
    while (*text) {
        const char *eol = strchr(text, '\n');
        size_t len = eol ? (size_t)(eol - text) : strlen(text);
        char line[256];
        if (len < sizeof(line) && !strncmp(text, "task=", 5)) {
            memcpy(line, text + 5, len - 5); line[len - 5] = 0;
            struct st_task t; memset(&t, 0, sizeof(t));
            char *f[11], *p = line; int n = 0;
            while (n < 10) { f[n++] = p; p = strchr(p, '|'); if (!p) break; *p++ = 0; }
            if (n == 10 && p) {
                f[10] = p;
                if (strlen(f[0]) < sizeof(t.name) && strlen(f[10]) < sizeof(t.command)) {
                    strcpy(t.name, f[0]); strcpy(t.command, f[10]);
                    t.kind = atoi(f[1]); t.enabled = atoi(f[2]) != 0; t.minute_of_day = (unsigned)atoi(f[3]);
                    t.weekday_mask = (unsigned)atoi(f[4]); t.day_of_month = (unsigned)atoi(f[5]);
                    if (sscanf(f[6], "%d-%d-%d", &t.start.year, &t.start.month, &t.start.day) == 3) {
                        t.created = atoll(f[7]); t.last_run = atoll(f[8]); t.last_result = atoi(f[9]);
                        int enabled = t.enabled;
                        if (st_add(s, &t) == 0) { s->t[s->count - 1].enabled = enabled; s->t[s->count - 1].last_run = t.last_run; }
                    } else if (t.kind == ST_AT_START) {
                        t.start = (struct st_date){ 1970, 1, 1 }; t.created = atoll(f[7]); t.last_run = atoll(f[8]);
                        if (st_add(s, &t) == 0) s->t[s->count - 1].last_run = t.last_run;
                    }
                }
            }
        }
        text += len + (eol ? 1 : 0);
        if (!eol) break;
    }
    return (int)s->count;
}

static const struct { const char *name, *command; } tool[MW_TOOLS] = {
    [MW_DEFRAG] = { "Disk Defragmenter", "/bin/defrag /all /silent" },        /* G29 supplies these tools */
    [MW_SCANDISK] = { "ScanDisk", "/bin/scandisk /all /autofix /silent" },
    [MW_CLEANUP] = { "Disk Cleanup", "/bin/cleanmgr /autoclean" },
};
const char *mw_tool_name(int t) { return t >= 0 && t < MW_TOOLS ? tool[t].name : ""; }
int mw_apply(struct st_store *s, const struct mw_choice *c, struct st_date start, int64_t now)
{
    static const unsigned slot_minute[] = { 0, 12 * 60, 20 * 60 };
    unsigned tools = c->express ? (1u << MW_DEFRAG) | (1u << MW_SCANDISK) | (1u << MW_CLEANUP) : c->tools;
    if (c->slot < MW_NIGHT || c->slot > MW_EVENING || !(tools & ((1u << MW_TOOLS) - 1)) || !date_ok(start)) return -EINVAL;
    unsigned want = 0;
    for (int k = 0; k < MW_TOOLS; k++) if (tools & (1u << k)) want++;
    if (s->count + want > ST_MAX) return -ENOSPC;
    int added = 0;
    for (int k = 0; k < MW_TOOLS; k++) {
        if (!(tools & (1u << k))) continue;
        struct st_task t; memset(&t, 0, sizeof(t));
        snprintf(t.name, sizeof(t.name), "%s", tool[k].name); snprintf(t.command, sizeof(t.command), "%s", tool[k].command);
        t.kind = ST_WEEKLY; t.weekday_mask = 1u << 0;       /* Sunday */
        t.minute_of_day = slot_minute[c->slot] + (unsigned)added * 30; t.start = start; t.created = now; t.last_run = -1;
        int e = st_remove(s, t.name); (void)e;              /* the wizard replaces its own earlier schedule */
        e = st_add(s, &t);
        if (e) return e;
        added++;
    }
    return added;
}
