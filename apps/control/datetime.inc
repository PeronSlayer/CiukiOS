/* G1: Date/Time Properties, included by app.c.
 * Windows 98 model (Resource Kit ch. 23, Table 23.1 "Date/Time: change
 * date, time, and time zone settings"): a "Date & Time" tab with a month
 * calendar and the clock, a "Time Zone" tab with the zone list and
 * "Automatically adjust clock for daylight saving changes". Apply sets the
 * zone first, then the local date and time (SetLocalTime semantics: the
 * kernel converts with the zone's rule and writes the RTC in local time).
 * The kernel record comes from host.time_zone (syscall 97).
 * SPDX-License-Identifier: MIT */
enum { C_DT_TABS = 1600, C_DT_DATE_GROUP, C_DT_TIME_GROUP, C_DT_PREV, C_DT_NEXT, C_DT_GRID, C_DT_TIME_L,
       C_DT_TIME, C_DT_ZONES, C_DT_AUTO, C_DT_APPLY };
#define DT_GRID_X 36
#define DT_GRID_Y 106
#define DT_CELL_W 32
#define DT_CELL_H 22
_Static_assert(CIUKI_TZ_ZONE_COUNT <= CP_TZ_MAX, "bundled zones fit the list");
static const char *const dt_months[] = {"January", "February", "March", "April", "May", "June", "July",
                                        "August", "September", "October", "November", "December"};
static const char *const dt_weekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

static int64_t dt_days(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
static void dt_civil(int64_t days, int *y, int *m, int *d)
{
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1); *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}
static int dt_month_days(int y, int m)
{
    static const int n[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return n[m - 1] + (m == 2 && !(y % 4) && (y % 100 || !(y % 400)));
}
static int64_t dt_floor_day(int64_t s) { return (s >= 0 ? s : s - 86399) / 86400; }

/* The kernel record, or the host clock as UTC when the service is absent. */
static void dt_refresh(struct cp_app *a)
{
    struct ciuki_time_zone q;
    memset(&q, 0, sizeof(q));
    a->time_ok = a->host.time_zone && !a->host.time_zone(a->host.ctx, &q, CIUKI_TIME_GET);
    if (!a->time_ok) {
        uint64_t now = a->host.now ? a->host.now(a->host.ctx) : 0;
        memset(&q, 0, sizeof(q));
        q.utc_seconds = q.local_seconds = (int64_t)now;
        strcpy(q.rule, "UTC0"); strcpy(q.standard_name, "UTC");
    }
    q.zone[sizeof(q.zone) - 1] = q.rule[sizeof(q.rule) - 1] = 0;
    q.standard_name[sizeof(q.standard_name) - 1] = q.daylight_name[sizeof(q.daylight_name) - 1] = 0;
    a->time = q;
}
static int dt_zone_of(const struct ciuki_time_zone *q)
{
    int i = ciuki_tz_find(q->zone);
    if (i < 0) for (unsigned k = 0; k < CIUKI_TZ_ZONE_COUNT; k++)
        if (!strcmp(ciuki_tz_zones[k].tz, q->rule)) { i = (int)k; break; }
    if (i < 0) i = ciuki_tz_find_minutes(q->standard_minutes);
    return i < 0 ? ciuki_tz_find(CIUKI_TZ_UTC) : i;
}
static void dt_selection(struct cp_app *a, int force)
{
    int y, m, d;
    dt_civil(dt_floor_day(a->time.local_seconds), &y, &m, &d);
    if (force || !a->dt_date_dirty) { a->dt_year = y; a->dt_month = m; a->dt_day = d; }
    if (force || !a->dt_zone_dirty) {
        a->dt_zone = dt_zone_of(&a->time);
        a->dt_auto = !a->time_ok || !!(a->time.flags & CIUKI_TIME_AUTO_DST);
    }
    if (force) a->dt_time_dirty = 0;
}
static int dt_has_dst(int zone) { return zone >= 0 && ciuki_tz_zones[zone].daylight[0]; }

/* "Mm.w.d[/time]" as words; 0 when the rule uses another date form. */
static int dt_change(const char *s, char *out, size_t n)
{
    unsigned m = 0, w = 0, d = 0, h = 2, mi = 0;
    if (*s++ != 'M') return 0;
    while (*s >= '0' && *s <= '9') m = m * 10 + (unsigned)(*s++ - '0');
    if (*s++ != '.' || *s < '1' || *s > '5') return 0;
    w = (unsigned)(*s++ - '0');
    if (*s++ != '.' || *s < '0' || *s > '6') return 0;
    d = (unsigned)(*s++ - '0');
    if (*s == '/') {
        s++; h = 0;
        if (*s == '-') return 0;
        while (*s >= '0' && *s <= '9') h = h * 10 + (unsigned)(*s++ - '0');
        if (*s == ':') { s++; while (*s >= '0' && *s <= '9') mi = mi * 10 + (unsigned)(*s++ - '0'); }
    }
    if (!m || m > 12) return 0;
    static const char *const nth[] = {"", "First", "Second", "Third", "Fourth", "Last"};
    snprintf(out, n, "%s %s of %s %02u:%02u", nth[w], dt_weekdays[d], dt_months[m - 1], h % 24, mi % 60);
    return 1;
}
static void dt_texts(struct cp_app *a)
{
    const struct ciuki_tz_zone *z = a->dt_zone >= 0 ? &ciuki_tz_zones[a->dt_zone] : NULL;
    const char *rule = z ? z->tz : a->time.rule;
    const char *comma = strchr(rule, ',');
    char start[96], end[96];
    if (!z || !z->daylight[0]) snprintf(a->dt_dst_line, sizeof(a->dt_dst_line), "This time zone does not observe daylight saving time.");
    else if (comma && dt_change(comma + 1, start, sizeof(start)) && strchr(comma + 1, ',') &&
             dt_change(strchr(comma + 1, ',') + 1, end, sizeof(end)))
        snprintf(a->dt_dst_line, sizeof(a->dt_dst_line), "Daylight time: %s to %s", start, end);
    else snprintf(a->dt_dst_line, sizeof(a->dt_dst_line), "Daylight saving time applies (rule %s).", rule);
    /* Windows 98 names the zone in force: "Current time zone: W. Europe
     * Daylight Time" (GetTimeZoneInformation names). */
    int daylight = !!(a->time.flags & CIUKI_TIME_DAYLIGHT);
    int current = dt_zone_of(&a->time);
    const struct ciuki_tz_zone *c = current >= 0 ? &ciuki_tz_zones[current] : NULL;
    const char *name = !c ? (daylight ? a->time.daylight_name : a->time.standard_name) :
                       daylight && c->daylight[0] ? c->daylight : c->standard;
    snprintf(a->dt_zone_line, sizeof(a->dt_zone_line), "Current time zone: %s", name);
}
static void dt_time_text(const struct cp_app *a, char *out, size_t n)
{
    int64_t s = a->time.local_seconds - dt_floor_day(a->time.local_seconds) * 86400;
    snprintf(out, n, "%02d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
}

/* The clock and date lines; called every second by cp_tick. */
static void dt_tick(struct cp_app *a)
{
    dt_refresh(a);
    dt_selection(a, 0);
    int64_t t = a->time.local_seconds, day = dt_floor_day(t), s = t - day * 86400;
    unsigned h = (unsigned)(s / 3600), m = (unsigned)(s / 60 % 60), sec = (unsigned)(s % 60);
    char previous[sizeof(a->clock_line)]; memcpy(previous, a->clock_line, sizeof(previous));
    if (a->cfg.clock_24h) snprintf(a->clock_line, sizeof(a->clock_line), a->cfg.clock_seconds ? "%02u:%02u:%02u" : "%02u:%02u", h, m, sec);
    else if (a->cfg.clock_seconds) snprintf(a->clock_line, sizeof(a->clock_line), "%u:%02u:%02u %s", h % 12 ? h % 12 : 12, m, sec, h < 12 ? "AM" : "PM");
    else snprintf(a->clock_line, sizeof(a->clock_line), "%u:%02u %s", h % 12 ? h % 12 : 12, m, h < 12 ? "AM" : "PM");
    int y, mo, d; dt_civil(day, &y, &mo, &d);
    int64_t w = (day + 4) % 7;
    snprintf(a->date_line, sizeof(a->date_line), "%s, %d %s %d", dt_weekdays[w < 0 ? w + 7 : w], d, dt_months[mo - 1], y);
    text(a, C_DATE, a->date_line);
    dt_texts(a);
    text(a, C_TIME_NOTE1, a->dt_tab ? a->dt_dst_line : a->dt_zone_line);
    struct du_widget *e = du_get(a->ui, C_DT_TIME);
    if (e && !a->dt_time_dirty && a->ui->focus != (int)(e - a->ui->widgets)) {
        dt_time_text(a, a->dt_time_text, sizeof(a->dt_time_text));
        if (strcmp(e->text, a->dt_time_text)) { du_set_text(e, a->dt_time_text); du_invalidate_widget(a->ui, C_DT_TIME); }
    }
    if (a->page == CP_DATETIME && !a->dt_tab) {
        if (strcmp(previous, a->clock_line)) du_invalidate(a->ui, (struct du_rect){300, 62, 228, 54});
        if (!a->dt_date_dirty) du_invalidate(a->ui, (struct du_rect){24, 56, 248, 184});
    }
}

static void dt_build(struct cp_app *a)
{
    struct du_ui *u = a->ui;
    struct du_widget *w;
    dt_refresh(a); dt_selection(a, 0);
    a->dt_tabs[0].text[0] = "Date & Time"; a->dt_tabs[1].text[0] = "Time Zone";
    w = du_get(u, C_TABS); w->id = C_DT_TABS; w->items = a->dt_tabs; w->count = 2; w->active = (unsigned)a->dt_tab;
    int can = a->time_ok && (a->time.flags & CIUKI_TIME_AUTHORITY);
    if (!a->dt_tab) {
        add(a, DU_GROUP, C_DT_DATE_GROUP, 20, 40, 260, 250, "Date");
        add(a, DU_BUTTON, C_DT_PREV, 32, 60, 26, 22, "<");
        add(a, DU_BUTTON, C_DT_NEXT, 242, 60, 26, 22, ">");
        /* One hit target under the painted grid: the pointer picks the day. */
        add(a, DU_TOOLBUTTON, C_DT_GRID, DT_GRID_X, DT_GRID_Y, 7 * DT_CELL_W, 6 * DT_CELL_H, "");
        add(a, DU_LABEL, C_DATE, 32, 266, 240, 14, a->date_line);
        add(a, DU_GROUP, C_DT_TIME_GROUP, 288, 40, 252, 250, "Time");
        add(a, DU_LABEL, C_DT_TIME_L, 300, 132, 80, 16, "&Time:");
        if (!a->dt_time_dirty) dt_time_text(a, a->dt_time_text, sizeof(a->dt_time_text));
        w = add(a, DU_EDIT, C_DT_TIME, 380, 128, 100, 22, a->dt_time_text); w->disabled = !can;
        add(a, DU_BUTTON, C_H24, 300, 166, 228, 24, "");
        add(a, DU_BUTTON, C_SECONDS, 300, 196, 228, 24, "");
        add(a, DU_LABEL, C_TIME_NOTE2, 300, 230, 236, 14, (a->time.flags & CIUKI_TIME_RTC_LOCAL) ? "The battery clock keeps local time." : "The battery clock keeps UTC.");
    } else {
        for (unsigned k = 0; k < CIUKI_TZ_ZONE_COUNT; k++)
            a->dt_zone_items[k] = (struct du_item){.text = {ciuki_tz_zones[k].label}, .selected = (int)k == a->dt_zone};
        w = add(a, DU_LISTVIEW, C_DT_ZONES, 20, 40, 520, 206, "");
        w->items = a->dt_zone_items; w->count = CIUKI_TZ_ZONE_COUNT; w->view = DU_DETAILS;
        w->column_count = 1; w->columns[0] = (struct du_column){"Time zone", 496, 0};
        w->active = a->dt_zone >= 0 ? (unsigned)a->dt_zone : 0; w->disabled = !can; w->inactive_selection = 1;
        du_list_ensure(w, w->active);
        if (w->scroll < (int)w->active - 4) { w->scroll = (int)w->active - 4; du_list_ensure(w, w->active); }
        w = add(a, DU_CHECKBOX, C_DT_AUTO, 20, 254, 520, 24, "&Automatically adjust clock for daylight saving changes");
        w->value = a->dt_auto; w->disabled = !can || !dt_has_dst(a->dt_zone);
        add(a, DU_LABEL, C_TIME_NOTE2, 20, 300, 520, 14, (a->time.flags & CIUKI_TIME_RTC_LOCAL) ?
            "The battery clock keeps local time; CiukiOS moves it at each daylight saving change." : "The battery clock keeps UTC.");
    }
    dt_texts(a);
    add(a, DU_LABEL, C_TIME_NOTE1, 20, 284, 520, 14, a->dt_tab ? a->dt_dst_line : a->dt_zone_line);
    if (!a->dt_tab) du_get(u, C_TIME_NOTE1)->box = (struct du_rect){20, 298, 520, 14};
    w = add(a, DU_BUTTON, C_DT_APPLY, CP_WIDTH - 184, CP_HEIGHT - 34, 88, 24, "Apply");
    w->anchors = DU_ANCHOR_RIGHT | DU_ANCHOR_BOTTOM; w->disabled = !can;
    if (!a->time_ok) text(a, C_STATUS, "The time zone service is not available: the clock is shown in UTC.");
    else if (!can) text(a, C_STATUS, "Only the desktop and Control Panel may change the clock.");
}

static void dt_paint(struct cp_app *a, struct du_canvas *c)
{
    if (a->dt_tab) return;
    char text[48];
    du_well(c, (struct du_rect){300, 62, 228, 54}, DU_WINDOW);
    int font = desk_theme->font_large;
    du_text_font(c, font, 300 + (228 - du_text_width_font(font, a->clock_line)) / 2, 89 - desk_font_cap(font) / 2, a->clock_line, DU_SELECT);
    snprintf(text, sizeof(text), "%s %d", dt_months[a->dt_month - 1], a->dt_year);
    du_text(c, 150 - du_text_width(text) / 2, 66, text, DU_INK);
    struct du_rect frame = {DT_GRID_X - 2, DT_GRID_Y - 18, 7 * DT_CELL_W + 4, 6 * DT_CELL_H + 20};
    du_fill(c, frame, DU_WINDOW); du_edge(c, frame, DU_SUNKEN);
    static const char *const heads[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    du_fill(c, (struct du_rect){DT_GRID_X, DT_GRID_Y - 16, 7 * DT_CELL_W, 14}, DU_SELECT);
    for (int i = 0; i < 7; i++) du_text(c, DT_GRID_X + i * DT_CELL_W + (DT_CELL_W - du_text_width(heads[i])) / 2, DT_GRID_Y - 12, heads[i], DU_SELECT_INK);
    int64_t first = dt_days(a->dt_year, a->dt_month, 1);
    int64_t lead = (first + 4) % 7; if (lead < 0) lead += 7;
    int days = dt_month_days(a->dt_year, a->dt_month);
    for (int k = 0; k < days; k++) {
        int cell = (int)lead + k;
        struct du_rect r = {DT_GRID_X + cell % 7 * DT_CELL_W, DT_GRID_Y + cell / 7 * DT_CELL_H, DT_CELL_W, DT_CELL_H};
        uint32_t ink = DU_INK;
        if (k + 1 == a->dt_day) { du_fill(c, (struct du_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, DU_SELECT); ink = DU_SELECT_INK; }
        snprintf(text, sizeof(text), "%d", k + 1);
        du_text(c, r.x + (r.w - du_text_width(text)) / 2, r.y + (r.h - DU_GLYPH_H) / 2, text, ink);
    }
}

/* "H:MM", "H:MM:SS", optional AM/PM. */
static int dt_parse_time(const char *s, int *seconds)
{
    int part[3] = {0, 0, 0}, n = 0, digits = 0;
    while (*s == ' ') s++;
    for (; *s && *s != ' ' && *s != 'A' && *s != 'P' && *s != 'a' && *s != 'p'; s++) {
        if (*s == ':') { if (!digits || ++n > 2) return -1; digits = 0; continue; }
        if (*s < '0' || *s > '9' || ++digits > 2) return -1;
        part[n] = part[n] * 10 + (*s - '0');
    }
    if (!digits || n < 1) return -1;
    while (*s == ' ') s++;
    if (*s) {
        int pm = *s == 'P' || *s == 'p';
        if ((s[1] != 'M' && s[1] != 'm') || s[2] || part[0] < 1 || part[0] > 12) return -1;
        part[0] = part[0] % 12 + (pm ? 12 : 0);
    }
    if (part[0] > 23 || part[1] > 59 || part[2] > 59) return -1;
    *seconds = part[0] * 3600 + part[1] * 60 + part[2];
    return 0;
}
static const char *dt_error(int e)
{
    return e == -EPERM ? "You do not have permission to change the system time." :
           e == -EINVAL ? "Choose a date from 1980 to 2079 and type the time as hh:mm:ss." :
           e == -EBUSY ? "The clock is being changed by another program; try again." : "The clock could not be set.";
}
static void dt_apply(struct cp_app *a)
{
    struct ciuki_time_zone q;
    int e = 0, changed = 0;
    struct du_widget *w = du_get(a->ui, C_DT_TIME);
    if (w) snprintf(a->dt_time_text, sizeof(a->dt_time_text), "%s", w->text);
    int seconds = 0;
    if ((a->dt_time_dirty || a->dt_date_dirty) && dt_parse_time(a->dt_time_text, &seconds)) {
        text(a, C_STATUS, "Type the time as hh:mm:ss (24-hour) or h:mm AM/PM."); return;
    }
    if (!a->host.time_zone) { text(a, C_STATUS, dt_error(-ENOSYS)); return; }
    if (a->dt_zone_dirty && a->dt_zone >= 0) {
        memset(&q, 0, sizeof(q));
        snprintf(q.zone, sizeof(q.zone), "%s", ciuki_tz_zones[a->dt_zone].id);
        snprintf(q.rule, sizeof(q.rule), "%s", ciuki_tz_zones[a->dt_zone].tz);
        q.flags = a->dt_auto && dt_has_dst(a->dt_zone) ? CIUKI_TIME_AUTO_DST : 0;
        e = a->host.time_zone(a->host.ctx, &q, CIUKI_TIME_SET_ZONE);
        changed |= !e;
    }
    if (!e && (a->dt_time_dirty || a->dt_date_dirty)) {
        memset(&q, 0, sizeof(q));
        q.local_seconds = dt_days(a->dt_year, a->dt_month, a->dt_day) * 86400 + seconds;
        e = a->host.time_zone(a->host.ctx, &q, CIUKI_TIME_SET_LOCAL);
        changed |= !e;
    }
    if (e) { text(a, C_STATUS, dt_error(e)); return; }
    a->dt_zone_dirty = a->dt_date_dirty = a->dt_time_dirty = 0;
    snprintf(a->status, sizeof(a->status), "%s", changed ? "The date, time and time zone were applied." : "No changes to apply.");
    dt_refresh(a); dt_selection(a, 1);
    build(a);
    text(a, C_STATUS, a->status);
}

/* 1 when the event belongs to Date/Time Properties. */
static int dt_event(struct cp_app *a, const struct du_event *e)
{
    if (a->page != CP_DATETIME) return 0;
    struct du_ui *u = a->ui;
    switch (e->id) {
    case C_DT_TABS:
        if (e->type == DU_EV_SELECT && e->index >= 0 && e->index < 2 && e->index != a->dt_tab) {
            struct du_widget *w = du_get(u, C_DT_TIME);
            if (w) snprintf(a->dt_time_text, sizeof(a->dt_time_text), "%s", w->text);
            a->dt_tab = e->index; build(a);
        }
        return 1;
    case C_DT_PREV: case C_DT_NEXT:
        if (e->type == DU_EV_ACTIVATE) {
            int m = a->dt_month + (e->id == C_DT_NEXT ? 1 : -1), y = a->dt_year;
            if (m < 1) { m = 12; y--; } else if (m > 12) { m = 1; y++; }
            if (y < 1980 || y > 2079) return 1;
            a->dt_year = y; a->dt_month = m;
            if (a->dt_day > dt_month_days(y, m)) a->dt_day = dt_month_days(y, m);
            a->dt_date_dirty = 1;
            du_invalidate(u, (struct du_rect){24, 56, 248, 184});
        }
        return 1;
    case C_DT_GRID:
        if (e->type == DU_EV_ACTIVATE) {
            int col = (u->mouse_x - DT_GRID_X) / DT_CELL_W, row = (u->mouse_y - DT_GRID_Y) / DT_CELL_H;
            int64_t lead = (dt_days(a->dt_year, a->dt_month, 1) + 4) % 7; if (lead < 0) lead += 7;
            int day = row * 7 + col - (int)lead + 1;
            if (u->mouse_x >= DT_GRID_X && u->mouse_y >= DT_GRID_Y && col < 7 && row < 6 && day >= 1 && day <= dt_month_days(a->dt_year, a->dt_month)) {
                a->dt_day = day; a->dt_date_dirty = 1;
                du_invalidate(u, (struct du_rect){24, 56, 248, 184});
            }
        }
        return 1;
    case C_DT_TIME:
        if (e->type == DU_EV_CHANGE) a->dt_time_dirty = 1;
        return 1;
    case C_DT_ZONES:
        if (e->type == DU_EV_SELECT && e->index >= 0 && (unsigned)e->index < CIUKI_TZ_ZONE_COUNT) {
            int had = dt_has_dst(a->dt_zone);
            a->dt_zone = e->index; a->dt_zone_dirty = 1;
            a->dt_auto = dt_has_dst(a->dt_zone) ? (had ? a->dt_auto : 1) : 0;
            struct du_widget *w = du_get(u, C_DT_AUTO);
            w->disabled = !dt_has_dst(a->dt_zone) || !(a->time.flags & CIUKI_TIME_AUTHORITY); w->value = a->dt_auto;
            du_invalidate_widget(u, C_DT_AUTO);
            dt_texts(a); text(a, C_TIME_NOTE1, a->dt_dst_line);
        }
        return 1;
    case C_DT_AUTO:
        if (e->type == DU_EV_CHANGE || e->type == DU_EV_ACTIVATE) { a->dt_auto = du_get(u, C_DT_AUTO)->value != 0; a->dt_zone_dirty = 1; }
        return 1;
    case C_DT_APPLY:
        if (e->type == DU_EV_ACTIVATE) dt_apply(a);
        return 1;
    default: return 0;
    }
}
