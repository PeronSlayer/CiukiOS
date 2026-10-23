/* G1: Control Panel Date/Time Properties through the production toolkit:
 * the Date & Time tab (calendar, clock, time edit), the Time Zone tab (zone
 * list, "Automatically adjust clock for daylight saving changes"), Apply
 * with and without the time authority, refusals, and the host fallback.
 * The model kernel evaluates the zone rule with src/kernel/lib/tzrule.c.
 * usage: datetime_control_test portrait.xrgb renders-dir
 * SPDX-License-Identifier: MIT */
#include "control.h"
#include "acc_test.h"
#include <ciuki/tzrule.h>
#include <errno.h>
/* app.c's private widget ids (enum at the top of apps/control/app.c). */
#define C_STATUS 3
#define C_TIME_NOTE1 47
static struct cp_app app;
static struct du_ui ui;
static uint32_t pixels[CP_WIDTH * CP_HEIGHT], portrait[256 * 256];

/* Model kernel (proc/timezone.c semantics). */
static int64_t utc = 1782900000;   /* 2026-07-01 10:00 UTC = 12:00 CEST */
static char zone[48] = "Europe/Rome", rule[64] = "CET-1CEST,M3.5.0,M10.5.0/3";
static uint32_t flags = CIUKI_TIME_AUTO_DST | CIUKI_TIME_RTC_LOCAL | CIUKI_TIME_AUTHORITY;
static int refuse, gets, set_local, set_zone;
static int64_t last_local;
static int time_zone(void *ctx, struct ciuki_time_zone *q, uint32_t op)
{
    (void)ctx;
    struct tzr r;
    if (op == CIUKI_TIME_SET_LOCAL || op == CIUKI_TIME_SET_ZONE) {
        if (!(flags & CIUKI_TIME_AUTHORITY)) return -EPERM;
        if (refuse) return refuse;
    }
    if (op == CIUKI_TIME_SET_ZONE) {
        assert(tzr_parse(q->rule, &r) && !(q->flags & ~CIUKI_TIME_AUTO_DST));
        strcpy(zone, q->zone); strcpy(rule, q->rule);
        flags = (flags & ~CIUKI_TIME_AUTO_DST) | q->flags; set_zone++;
    }
    assert(tzr_parse(rule, &r));
    if (!(flags & CIUKI_TIME_AUTO_DST)) r.has_dst = false;
    if (op == CIUKI_TIME_SET_LOCAL) {
        last_local = q->local_seconds;
        if (q->local_seconds < 315532800 || q->local_seconds > 3471292799LL) return -EINVAL;
        utc = tzr_local_to_utc(&r, q->local_seconds, -1, 0); set_local++;
    }
    if (op == CIUKI_TIME_GET) gets++;
    struct tzr full; assert(tzr_parse(rule, &full));
    bool dst; int32_t off = tzr_offset(&r, utc, &dst);
    memset(q, 0, sizeof(*q));
    q->version = 1; q->size = sizeof(*q); q->flags = flags | (dst ? CIUKI_TIME_DAYLIGHT : 0);
    q->utc_seconds = utc; q->local_seconds = utc + off; q->offset_minutes = off / 60;
    q->standard_minutes = full.std_offset / 60; q->daylight_minutes = (r.has_dst ? r.dst_offset : r.std_offset) / 60;
    strcpy(q->zone, zone); strcpy(q->rule, rule); strcpy(q->standard_name, full.std); strcpy(q->daylight_name, full.dst);
    return 0;
}
static uint64_t host_now(void *ctx) { (void)ctx; return 1782900000ull; }

static struct du_widget *kind(enum du_kind k)
{ for (unsigned i = 0; i < ui.count; i++) if (ui.widgets[i].kind == k) return &ui.widgets[i]; return NULL; }
static const char *label(int id) { struct du_widget *w = du_get(&ui, id); assert(w); return w->text; }
static void render(const char *folder, const char *name) { du_render(&ui); at_save(folder, name, &ui); }
static int64_t civil(int y, unsigned m, unsigned d, unsigned h, unsigned mi, unsigned s)
{ return tzr_days_from_civil(y, m, d) * 86400 + h * 3600 + mi * 60 + s; }
/* The calendar cell of a day in the shown month. */
static void click_day(int day)
{
    int64_t first = tzr_days_from_civil(app.dt_year, (unsigned)app.dt_month, 1);
    int cell = (int)tzr_weekday(first) + day - 1;
    at_click(&ui, 36 + cell % 7 * 32 + 16, 106 + cell / 7 * 22 + 11);
}
static void type_time(const char *s)
{
    struct du_widget *w = du_get(&ui, 1607); assert(w && w->kind == DU_EDIT);
    du_set_text(w, s); du_post(&ui, DU_EV_CHANGE, 1607, 0);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    FILE *f = fopen(argv[1], "rb"); assert(f && fread(portrait, 4, 256 * 256, f) == 256 * 256); fclose(f);
    const char *renders = argv[2];
    app.host = (struct cp_host){.now = host_now, .time_zone = time_zone, .portrait = portrait, .screen_w = 1024, .screen_h = 768};
    du_init(&ui, pixels, CP_WIDTH, CP_HEIGHT); ui.focused_window = 1;
    assert(!cp_attach(&app, &ui, cp_page_index("datetime")));
    assert(app.page == CP_DATETIME && gets);
    struct du_widget *tabs = kind(DU_TABS);
    assert(tabs && tabs->count == 2 && !strcmp(tabs->items[0].text[0], "Date & Time") && !strcmp(tabs->items[1].text[0], "Time Zone"));
    /* The clock, the date and the zone show local time (12:00 CEST). */
    assert(!strcmp(label(1607), "12:00:00") && !strcmp(app.date_line, "Wednesday, 1 July 2026"));
    assert(!strcmp(label(C_TIME_NOTE1), "Current time zone: W. Europe Daylight Time"));
    assert(strstr(app.clock_line, "12:00") && app.dt_year == 2026 && app.dt_month == 7 && app.dt_day == 1);
    assert(!du_get(&ui, 1610)->disabled);
    render(renders, "control-datetime-date");
    /* A second later the clock follows; an unedited time edit follows too. */
    utc++; cp_tick(&app);
    assert(!strcmp(label(1607), "12:00:01"));
    /* Calendar: next month, the 15th; time 14:30; Apply = SetLocalTime. */
    at_click_widget(&ui, 1604);
    assert(app.dt_month == 8 && app.dt_day == 1 && app.dt_date_dirty);
    click_day(15); assert(app.dt_day == 15);
    type_time("14:30");
    utc += 5; cp_tick(&app);
    assert(!strcmp(label(1607), "14:30") && app.dt_month == 8);   /* edits survive the tick */
    render(renders, "control-datetime-edited");
    at_click_widget(&ui, 1610);
    assert(set_local == 1 && last_local == civil(2026, 8, 15, 14, 30, 0) && utc == civil(2026, 8, 15, 12, 30, 0));
    assert(!strcmp(label(C_STATUS), "The date, time and time zone were applied.") && !app.dt_date_dirty && !app.dt_time_dirty);
    assert(!strcmp(label(1607), "14:30:00"));
    /* Winter date through the 12-hour form: the kernel picks CET. */
    at_click_widget(&ui, 1604); at_click_widget(&ui, 1604); at_click_widget(&ui, 1604); at_click_widget(&ui, 1604);
    assert(app.dt_month == 12); click_day(24); type_time("6:15 PM");
    at_click_widget(&ui, 1610);
    assert(set_local == 2 && last_local == civil(2026, 12, 24, 18, 15, 0) && utc == civil(2026, 12, 24, 17, 15, 0));
    assert(!strcmp(label(C_TIME_NOTE1), "Current time zone: W. Europe Standard Time"));
    /* Malformed time: refused before any call. */
    type_time("25:00"); at_click_widget(&ui, 1610);
    assert(set_local == 2 && strstr(label(C_STATUS), "hh:mm:ss"));
    type_time("10:00");
    for (int k = 0; k < 600; k++) at_click_widget(&ui, 1603);     /* the calendar stops at 1980 */
    assert(app.dt_year == 1980 && app.dt_month == 1);
    /* Time Zone tab. */
    du_post(&ui, DU_EV_SELECT, 1600, 1);
    assert(app.dt_tab == 1 && kind(DU_LISTVIEW) && kind(DU_LISTVIEW)->count == CIUKI_TZ_ZONE_COUNT);
    int rome = ciuki_tz_find("Europe/Rome"), ny = ciuki_tz_find("America/New_York"), phoenix = ciuki_tz_find("America/Phoenix");
    assert(kind(DU_LISTVIEW)->active == (unsigned)rome && du_get(&ui, 1609)->value == 1 && !du_get(&ui, 1609)->disabled);
    assert(!strcmp(label(C_TIME_NOTE1), "Daylight time: Last Sunday of March 02:00 to Last Sunday of October 03:00"));
    assert(kind(DU_LISTVIEW)->scroll <= rome && kind(DU_LISTVIEW)->scroll + 8 > rome);   /* the current zone is in view */
    render(renders, "control-datetime-zone");
    du_post(&ui, DU_EV_SELECT, 1608, phoenix);
    assert(du_get(&ui, 1609)->disabled && !du_get(&ui, 1609)->value && !strcmp(label(C_TIME_NOTE1), "This time zone does not observe daylight saving time."));
    du_post(&ui, DU_EV_SELECT, 1608, ny);
    assert(!du_get(&ui, 1609)->disabled && du_get(&ui, 1609)->value);
    assert(!strcmp(label(C_TIME_NOTE1), "Daylight time: Second Sunday of March 02:00 to First Sunday of November 02:00"));
    at_click_widget(&ui, 1609);   /* off */
    assert(!app.dt_auto);
    /* The month edit on the other tab was abandoned (dirty date): Apply sets the zone and that date. */
    app.dt_date_dirty = 0; app.dt_time_dirty = 0;
    at_click_widget(&ui, 1610);
    assert(set_zone == 1 && !strcmp(zone, "America/New_York") && !strcmp(rule, "EST5EDT,M3.2.0,M11.1.0") && !(flags & CIUKI_TIME_AUTO_DST));
    assert(set_local == 2 && app.dt_tab == 1 && !app.dt_auto && !du_get(&ui, 1609)->value);
    du_post(&ui, DU_EV_SELECT, 1608, rome); at_click_widget(&ui, 1609);   /* back to Rome, adjust on */
    at_click_widget(&ui, 1610);
    assert(set_zone == 2 && !strcmp(zone, "Europe/Rome") && (flags & CIUKI_TIME_AUTO_DST));
    /* Refusals keep the edits and explain. */
    du_post(&ui, DU_EV_SELECT, 1608, ny); refuse = -EBUSY;
    at_click_widget(&ui, 1610);
    assert(set_zone == 2 && app.dt_zone_dirty && strstr(label(C_STATUS), "another program"));
    refuse = 0;
    /* Without the time authority: read-only, Apply disabled. */
    flags &= ~CIUKI_TIME_AUTHORITY;
    cp_open(&app, CP_DATETIME);
    assert(du_get(&ui, 1610)->disabled && du_get(&ui, 1608)->disabled && !strcmp(label(C_STATUS), "Only the desktop and Control Panel may change the clock."));
    du_post(&ui, DU_EV_SELECT, 1600, 0);
    assert(du_get(&ui, 1607)->disabled);
    flags |= CIUKI_TIME_AUTHORITY;
    /* A kernel EPERM (the authority withdrawn meanwhile) is reported. */
    cp_open(&app, CP_DATETIME); type_time("09:00"); flags &= ~CIUKI_TIME_AUTHORITY;
    at_click_widget(&ui, 1610);
    assert(!strcmp(label(C_STATUS), "You do not have permission to change the system time."));
    flags |= CIUKI_TIME_AUTHORITY;
    cp_detach(&app);
    /* No time service: the host clock, read-only, in UTC. */
    static struct cp_app plain; static struct du_ui plain_ui;
    plain.host = (struct cp_host){.now = host_now, .portrait = portrait};
    du_init(&plain_ui, pixels, CP_WIDTH, CP_HEIGHT);
    assert(!cp_attach(&plain, &plain_ui, CP_DATETIME));
    struct du_widget *apply = du_get(&plain_ui, 1610), *status = du_get(&plain_ui, C_STATUS);
    assert(apply->disabled && strstr(status->text, "not available") && strstr(plain.clock_line, "10:00"));
    cp_detach(&plain);
    printf("PASS date/time properties: calendar, time edit, SetLocalTime on both seasons, zone list with DST rules, auto-adjust, authority and refusals, UTC fallback\n");
    return 0;
}
