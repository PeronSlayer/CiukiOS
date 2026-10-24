/* G30 Scheduled Tasks windows (docs/design/system-tools.md, "Scheduled Tasks program").
 * One window, four modes: the folder, the Add Scheduled Task wizard, the task properties and the
 * Maintenance Wizard. The latter three are pages built into the window (a toolkit dialog holds only
 * six caller controls), the folder stays the single owner of the task list.
 * SPDX-License-Identifier: MIT */
#include "schedtask_ui.h"
#include "sysui.h"
#include "catalog.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    A_LIST = 1, A_STATUS,
    B_BACK = 10, B_NEXT, B_CANCEL, B_RULE,
    W_PROGRAMS = 20, W_BROWSE_LABEL, W_BROWSE, W_NAME_LABEL, W_NAME, W_FREQ_LABEL,
    W_FREQ0 = 30,                                  /* 30..34 */
    W_TIME_LABEL = 40, W_TIME, W_DATE_LABEL, W_DATE, W_DAY_LABEL, W_DAY, W_HINT,
    W_DAYS0 = 50,                                  /* 50..56 */
    W_ADVANCED = 60,
    M_EXPRESS = 70, M_CUSTOM, M_DESC1, M_DESC2, M_SLOT0 = 74 /* 74..76 */, M_TOOL0 = 80 /* 80..82 */, M_LABEL,
    P_TABS = 100, P_OK, P_CANCEL, P_APPLY,
    P_RUN_LABEL = 110, P_RUN, P_INFO_NAME, P_ENABLED, P_INFO_CREATED,
    P_KIND_LABEL = 120, P_KIND0 = 121 /* 121..125 */, P_TIME_LABEL = 130, P_TIME, P_DATE_LABEL, P_DATE, P_DAY_LABEL, P_DAY,
    P_DAYS0 = 140 /* 140..146 */, P_NEXT = 150,
    P_LAST = 160, P_RESULT, P_STATE, P_NOTE,
    C_OPEN = 200, C_RUN, C_DELETE, C_PROPS, C_EXIT, C_MAINT, C_LOG, C_ABOUT,
    D_INFO = 300, D_DELETE,
    T_REFRESH = 1
};
static const char *const day_names[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const kind_names[5] = { "&Daily", "&Weekly", "&Monthly", "&One time only", "When my &computer starts" };
static const struct du_column folder_columns[5] = {
    { "Name", 152, 0 }, { "Schedule", 168, 0 }, { "Next Run Time", 118, 0 }, { "Last Run Time", 118, 0 }, { "Status", 84, 0 } };

/* ---- time and date ---- */
static int leap(int y) { return (y % 4 == 0 && y % 100) || y % 400 == 0; }
static int mdays(int y, int m) { static const int n[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }; return m == 2 && leap(y) ? 29 : n[m - 1]; }
static void civil(int64_t minutes, struct st_date *d, unsigned *minute)
{
    int64_t day = minutes >= 0 ? minutes / 1440 : -((-minutes + 1439) / 1440);
    *minute = (unsigned)(minutes - day * 1440);
    int64_t z = day + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    d->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    d->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    d->year = (int)((int64_t)yoe + era * 400 + (d->month <= 2));
}
void stui_format_minutes(char *out, size_t cap, int64_t minutes)
{
    struct st_date d; unsigned m; civil(minutes, &d, &m);
    snprintf(out, cap, "%04d-%02d-%02d %02u:%02u", d.year, d.month, d.day, m / 60, m % 60);
}
static int digits(const char **p, int max, int *value)
{
    int n = 0, v = 0;
    while (**p >= '0' && **p <= '9' && n < max) { v = v * 10 + (**p - '0'); (*p)++; n++; }
    *value = v; return n;
}
static const char *skip(const char *p) { while (*p == ' ') p++; return p; }
int stui_parse_time(const char *text, unsigned *minute)
{
    const char *p = skip(text); int h, m;
    int a = digits(&p, 2, &h);
    if (!a || *p++ != ':' || digits(&p, 2, &m) != 2 || *skip(p) || h > 23 || m > 59) return -EINVAL;
    *minute = (unsigned)(h * 60 + m); return 0;
}
int stui_parse_date(const char *text, struct st_date *date)
{
    const char *p = skip(text); int y, m, d;
    if (digits(&p, 4, &y) != 4 || *p++ != '-') return -EINVAL;
    int a = digits(&p, 2, &m);
    if (!a || *p++ != '-' || !digits(&p, 2, &d) || *skip(p)) return -EINVAL;
    if (y < 1970 || y > 2999 || m < 1 || m > 12 || d < 1 || d > mdays(y, m)) return -EINVAL;
    *date = (struct st_date){ y, m, d }; return 0;
}
void stui_describe(const struct st_task *t, char *out, size_t cap)
{
    char at[16]; snprintf(at, sizeof(at), "%02u:%02u", t->minute_of_day / 60, t->minute_of_day % 60);
    switch (t->kind) {
    case ST_DAILY: snprintf(out, cap, "Daily at %s", at); break;
    case ST_WEEKLY: {
        char days[48] = ""; size_t n = 0;
        for (int i = 0; i < 7; i++) if (t->weekday_mask & (1u << i))
            n += (size_t)snprintf(days + n, sizeof(days) - n, "%s%.3s", n ? ", " : "", day_names[i]);
        snprintf(out, cap, "Weekly on %s at %s", days, at); break;
    }
    case ST_MONTHLY: snprintf(out, cap, "Monthly on day %u at %s", t->day_of_month, at); break;
    case ST_ONCE: snprintf(out, cap, "Once on %04d-%02d-%02d at %s", t->start.year, t->start.month, t->start.day, at); break;
    default: snprintf(out, cap, "At system startup"); break;
    }
}
void stui_next_text(const struct st_task *t, int64_t now, char *out, size_t cap)
{
    if (!t->enabled) { snprintf(out, cap, "Never"); return; }
    if (t->kind == ST_AT_START) { snprintf(out, cap, "At system startup"); return; }
    int64_t next = st_next(t, t->last_run >= 0 ? t->last_run : t->created);
    if (next < 0) snprintf(out, cap, "Never");
    else if (next <= now) snprintf(out, cap, "Due now");
    else stui_format_minutes(out, cap, next);
}
void stui_status_text(const struct st_task *t, char *out, size_t cap)
{
    if (t->last_run >= 0 && t->last_result) snprintf(out, cap, "Last run failed (error %d)", t->last_result < 0 ? -t->last_result : t->last_result);
    else if (!t->enabled) snprintf(out, cap, "Disabled");
    else snprintf(out, cap, "Ready");
}
void stui_log_text(const struct stui *s, char *out, size_t cap)
{
    unsigned order[ST_MAX], n = 0;
    for (unsigned i = 0; i < s->store.count; i++) if (s->store.t[i].last_run >= 0) order[n++] = i;
    for (unsigned i = 1; i < n; i++)
        for (unsigned j = i; j && s->store.t[order[j]].last_run > s->store.t[order[j - 1]].last_run; j--) { unsigned x = order[j]; order[j] = order[j - 1]; order[j - 1] = x; }
    size_t at = (size_t)snprintf(out, cap, n ? "Last results of the scheduled tasks, newest first:\n\n" : "No scheduled task has run yet.");
    for (unsigned i = 0; i < n && i < 12 && at + 80 < cap; i++) {
        const struct st_task *t = &s->store.t[order[i]];
        char when[24], result[40];
        stui_format_minutes(when, sizeof(when), t->last_run);
        if (t->last_result) snprintf(result, sizeof(result), "failed (error %d)", t->last_result < 0 ? -t->last_result : t->last_result);
        else snprintf(result, sizeof(result), "started");
        at += (size_t)snprintf(out + at, cap - at, "%s  %.24s  %s\n", when, t->name, result);
    }
    if (n > 12 && at + 40 < cap) snprintf(out + at, cap - at, "(%u older results not shown)", n - 12);
}

/* ---- helpers ---- */
static struct du_widget *add(struct stui *s, enum du_kind k, int id, int x, int y, int w, int h, const char *text)
{ return du_add(s->ui, k, id, (struct du_rect){ x, y, w, h }, text); }
static void reset(struct stui *s, int menus)
{
    struct du_ui *u = s->ui;
    du_menu_close(u); du_release(u);
    u->count = 0; u->focus = u->capture = u->hover = -1; u->last_click_widget = -1;
    u->menus = menus ? s->menus : NULL; u->menu_count = menus ? 3 : 0;
    u->dirty = 1;
}
static void trim(char *t)
{
    char *p = t; while (*p == ' ') p++;
    memmove(t, p, strlen(p) + 1);
    size_t n = strlen(t); while (n && t[n - 1] == ' ') t[--n] = 0;
}
static const char *widget_text(struct stui *s, int id) { struct du_widget *w = du_get(s->ui, id); return w ? w->text : ""; }
static int widget_value(struct stui *s, int id) { struct du_widget *w = du_get(s->ui, id); return w ? w->value : 0; }
static void warn(struct stui *s, const char *text)
{
    const char *title = s->mode == STUI_PROPS ? "Task Properties" : s->mode == STUI_MAINT ? "Maintenance Wizard" :
                        s->mode == STUI_WIZARD ? "Scheduled Task Wizard" : "Scheduled Tasks";
    sysui_box(s->ui, D_INFO, title, text, DU_DLG_ICON_WARNING);
}
static void set_status(struct stui *s, const char *text)
{
    snprintf(s->status, sizeof(s->status), "%s", text);
    if (s->mode == STUI_FOLDER) sysui_text(s->ui, A_STATUS, text);
}
static int64_t now_of(struct stui *s) { s->now = s->host->now_minutes(s->host->ctx); return s->now; }
static struct st_task *find_task(struct stui *s, const char *name)
{
    for (unsigned i = 0; i < s->store.count; i++) if (!strcmp(s->store.t[i].name, name)) return &s->store.t[i];
    return NULL;
}
/* ---- persistence: re-read before every edit, write the whole file back ---- */
static int store_read(struct stui *s, struct st_store *into)
{
    size_t length = 0;
    int e = s->host->load(s->host->ctx, s->text, SS_TEXT - 1, &length);
    if (e == -ENOENT) { st_init(into); return 0; }
    if (e) return e < 0 ? e : -e;
    if (length >= SS_TEXT) length = SS_TEXT - 1;
    s->text[length] = 0;
    st_load(into, s->text);
    return 0;
}
static int store_write(struct stui *s, const struct st_store *from)
{
    int w = st_save(from, s->text, SS_TEXT);
    if (w < 0) return w;
    return s->host->save(s->host->ctx, s->text, (size_t)w);
}
/* Re-reads the file into s->store; a real read error stops the edit and nothing is written. */
static int begin_edit(struct stui *s)
{
    int e = store_read(s, &s->store);
    if (e) {
        char text[160]; snprintf(text, sizeof(text), "The list of scheduled tasks could not be read (error %d). Nothing was changed.", -e);
        warn(s, text);
    }
    return e;
}
static int end_edit(struct stui *s)
{
    int e = store_write(s, &s->store);
    if (e) {
        char text[160]; snprintf(text, sizeof(text), "The list of scheduled tasks could not be saved (error %d). The change was not made.", e < 0 ? -e : e);
        warn(s, text);
    }
    return e;
}

/* ---- programs ---- */
static void load_programs(struct stui *s)
{
    static char names[STUI_PROGRAMS][STUI_NAME];
    unsigned n = 0;
    s->program_count = 0;
    if (!s->host->programs || s->host->programs(s->host->ctx, names, STUI_PROGRAMS, &n)) return;
    for (unsigned i = 0; i < n && i < STUI_PROGRAMS; i++) {
        struct stui_program *p = &s->programs[s->program_count];
        if (!names[i][0] || names[i][0] == '.' || !strcmp(names[i], "desktop")) continue;
        snprintf(p->name, sizeof(p->name), "%s", names[i]);
        const struct desk_catalog_entry *c = desk_catalog_find(names[i]);
        const char *label = c ? c->label : names[i];
        for (unsigned k = 0; k < DESK_APPLICATION_COUNT && !c; k++) if (!strcmp(desk_applications[k].name, names[i])) label = desk_applications[k].label;
        snprintf(p->label, sizeof(p->label), "%s", label);
        s->program_count++;
    }
    for (unsigned i = 1; i < s->program_count; i++)       /* by label */
        for (unsigned j = i; j && strcmp(s->programs[j].label, s->programs[j - 1].label) < 0; j--) {
            struct stui_program t = s->programs[j]; s->programs[j] = s->programs[j - 1]; s->programs[j - 1] = t;
        }
}

/* ---- folder ---- */
static uint64_t fnv(uint64_t h, const void *p, size_t n) { const unsigned char *c = p; while (n--) h = (h ^ *c++) * 1099511628211ull; return h; }
static void update_menus(struct stui *s)
{
    int task = s->selected >= 1;
    s->file_items[0].flags = DU_ITEM_DEFAULT | (s->selected >= 0 ? 0 : DU_ITEM_DISABLED);
    s->file_items[1].flags = task ? 0 : DU_ITEM_DISABLED;
    s->file_items[2].flags = task ? 0 : DU_ITEM_DISABLED;
    s->file_items[4].flags = task ? 0 : DU_ITEM_DISABLED;
}
static void fill_rows(struct stui *s)
{
    struct du_widget *w = du_get(s->ui, A_LIST);
    if (!w) return;
    now_of(s);
    s->selected = -1;
    for (unsigned i = 0; i < s->store.count; i++) if (!strcmp(s->store.t[i].name, s->selected_name)) s->selected = (int)i + 1;
    if (s->selected < 0 && s->selected_name[0] == 1) s->selected = 0;     /* the Add row, marked by a 1 byte */
    if (s->selected < 0) s->selected_name[0] = 0;
    memset(s->rows, 0, sizeof(s->rows));
    snprintf(s->cells[0][0], sizeof(s->cells[0][0]), "Add Scheduled Task");
    s->rows[0].text[0] = s->cells[0][0];
    s->rows[0].icon16 = du_icon_argb(du_icon_name("action:add"), 16);
    for (unsigned i = 0; i < s->store.count; i++) {
        const struct st_task *t = &s->store.t[i];
        char (*c)[48] = s->cells[i + 1];
        char program[48], argument[161], when[24];
        snprintf(c[0], 48, "%s", t->name);
        stui_describe(t, c[1], 48);
        stui_next_text(t, s->now, c[2], 48);
        if (t->last_run >= 0) { stui_format_minutes(when, sizeof(when), t->last_run); snprintf(c[3], 48, "%s", when); }
        else snprintf(c[3], 48, "Never");
        stui_status_text(t, c[4], 48);
        for (unsigned k = 0; k < 5; k++) s->rows[i + 1].text[k] = c[k];
        s->rows[i + 1].icon16 = NULL;
        if (!ss_split(t->command, program, sizeof(program), argument, sizeof(argument))) s->rows[i + 1].icon16 = du_icon_argb(du_icon_file(program, 0, 1), 16);
        if (!s->rows[i + 1].icon16) s->rows[i + 1].icon16 = du_icon_argb(du_icon_name("program:schedtask"), 16);
    }
    w->count = s->store.count + 1;
    if (s->selected >= 0) { w->active = (unsigned)s->selected; w->selection_active = 1; s->rows[s->selected].selected = 1; }
    else w->selection_active = 0;
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, &s->selected, sizeof(s->selected));
    for (unsigned i = 0; i < w->count; i++) for (unsigned k = 0; k < 5; k++) if (s->rows[i].text[k]) h = fnv(h, s->rows[i].text[k], strlen(s->rows[i].text[k]) + 1);
    if (h != s->list_hash) { s->list_hash = h; du_invalidate_widget(s->ui, A_LIST); }
    update_menus(s);
    if (s->status[0]) sysui_text(s->ui, A_STATUS, s->status);
    else { char t[48]; snprintf(t, sizeof(t), "%u scheduled task%s", s->store.count, s->store.count == 1 ? "" : "s"); sysui_text(s->ui, A_STATUS, t); }
}
static void build_folder(struct stui *s)
{
    reset(s, 1); s->mode = STUI_FOLDER; s->list_hash = 0;
    struct du_widget *w = add(s, DU_LISTVIEW, A_LIST, 0, 22, STUI_W, 336, "");
    w->view = DU_DETAILS; w->items = s->rows; w->column_count = 5;
    memcpy(w->columns, folder_columns, sizeof(folder_columns));
    add(s, DU_STATUS, A_STATUS, 0, STUI_H - 22, STUI_W, 22, "");
    fill_rows(s);
    du_focus(s->ui, A_LIST);
}
void stui_refresh(struct stui *s)
{
    now_of(s);
    if (s->mode != STUI_FOLDER) return;
    struct st_store *fresh = &s->trial;
    if (!store_read(s, fresh)) s->store = *fresh;
    fill_rows(s);
}
static void select_row(struct stui *s, int row)
{
    s->selected = row;
    if (row >= 1 && (unsigned)row <= s->store.count) snprintf(s->selected_name, sizeof(s->selected_name), "%s", s->store.t[row - 1].name);
    else { s->selected_name[0] = row == 0 ? 1 : 0; s->selected_name[1] = 0; }
    s->status[0] = 0;
    fill_rows(s);
}
static const char *selected_task_name(struct stui *s)
{ return s->selected >= 1 && (unsigned)s->selected <= s->store.count ? s->store.t[s->selected - 1].name : NULL; }

static void record_run(struct stui *s, const char *name, int result)
{
    if (store_read(s, &s->store)) return;
    struct st_task *t = find_task(s, name);
    if (!t) return;
    t->last_run = now_of(s); t->last_result = result;
    (void)store_write(s, &s->store);
}
static void run_failed(struct stui *s, const char *name, int error)
{
    char text[200];
    snprintf(text, sizeof(text), "\"%.31s\" could not be started.\n%s (error %d)", name, strerror(error < 0 ? -error : error), error < 0 ? -error : error);
    sysui_box(s->ui, D_INFO, "Scheduled Tasks", text, DU_DLG_ICON_ERROR);
    set_status(s, "The task could not be started.");
}
static void run_selected(struct stui *s)
{
    const char *name = selected_task_name(s);
    if (!name) return;
    char task[STUI_NAME], program[48], argument[161];
    snprintf(task, sizeof(task), "%s", name);
    if (ss_split(s->store.t[s->selected - 1].command, program, sizeof(program), argument, sizeof(argument))) {
        run_failed(s, task, -EINVAL); record_run(s, task, -EINVAL); fill_rows(s); return;
    }
    snprintf(s->launch_name, sizeof(s->launch_name), "%s", task);
    int r = s->host->launch(s->host->ctx, program);
    if (r < 0) { run_failed(s, task, r); record_run(s, task, r); fill_rows(s); return; }
    s->launch_request = r;
    char text[96]; snprintf(text, sizeof(text), "Starting \"%.31s\"...", task);
    set_status(s, text);
}
void stui_launch_result(struct stui *s, int request, int result)
{
    if (!request || request != s->launch_request) return;
    s->launch_request = 0;
    char task[STUI_NAME]; snprintf(task, sizeof(task), "%s", s->launch_name);
    if (result > 0) {
        record_run(s, task, 0);
        char text[96]; snprintf(text, sizeof(text), "Started \"%.31s\".", task);
        set_status(s, text);
    } else { record_run(s, task, result); run_failed(s, task, result ? result : -EIO); }
    if (s->mode == STUI_FOLDER) { store_read(s, &s->store); fill_rows(s); }
}
static void ask_delete(struct stui *s)
{
    const char *name = selected_task_name(s);
    if (!name) return;
    snprintf(s->pending_delete, sizeof(s->pending_delete), "%s", name);
    char text[160]; static const char *const buttons[] = { "&Yes", "&No" };
    snprintf(text, sizeof(text), "Are you sure you want to delete the scheduled task \"%.31s\"?", name);
    du_message(s->ui, D_DELETE, "Confirm Task Delete", text, DU_DLG_ICON_QUESTION, buttons, 2, 1, 1);
}
static void do_delete(struct stui *s)
{
    if (begin_edit(s)) return;
    int e = st_remove(&s->store, s->pending_delete);
    char text[96];
    if (!e && end_edit(s)) { fill_rows(s); return; }
    snprintf(text, sizeof(text), e ? "\"%.31s\" had already been deleted." : "Deleted \"%.31s\".", s->pending_delete);
    s->selected_name[0] = 0; s->selected = -1;
    set_status(s, text); fill_rows(s);
}

/* ---- shared schedule fields ---- */
/* Fills the schedule of `t` from edit texts; shows the reason and returns -1 when a rule is broken. */
static int fill_schedule(struct stui *s, struct st_task *t, int kind, const char *time, const char *date, const char *day, unsigned mask, int check_past)
{
    t->kind = kind; t->weekday_mask = 0; t->day_of_month = 0; t->minute_of_day = 0;
    t->start = (struct st_date){ 1970, 1, 1 };
    if (kind == ST_AT_START) return 0;
    if (stui_parse_time(time, &t->minute_of_day)) { warn(s, "The time must be typed as HH:MM on the 24-hour clock, from 00:00 to 23:59."); return -1; }
    if (stui_parse_date(date, &t->start)) {
        warn(s, "The date must be typed as YYYY-MM-DD, for example 2026-10-24, and must exist in the calendar (years 1970 to 2999)."); return -1;
    }
    if (kind == ST_WEEKLY) {
        t->weekday_mask = mask & 0x7f;
        if (!t->weekday_mask) { warn(s, "Select at least one day of the week."); return -1; }
    }
    if (kind == ST_MONTHLY) {
        char *end; long v = strtol(day, &end, 10);
        if (!*day || *end || v < 1 || v > 31) { warn(s, "The day of the month must be a number from 1 to 31. Months with fewer days run on their last day."); return -1; }
        t->day_of_month = (unsigned)v;
    }
    if (kind == ST_ONCE && check_past && st_minutes(t->start, t->minute_of_day) <= now_of(s)) {
        warn(s, "That date and time have already passed. A task that runs one time only needs a start in the future."); return -1;
    }
    return 0;
}
static void put_hhmm(char *out, unsigned minute) { snprintf(out, 8, "%02u:%02u", minute / 60, minute % 60); }
static void put_date(char *out, struct st_date d) { snprintf(out, 12, "%04d-%02d-%02d", d.year, d.month, d.day); }

/* ---- Add Scheduled Task wizard ---- */
static const char *const wizard_heading[WP_PAGES] = {
    "Scheduled Task Wizard", "Select the program you want CiukiOS to run", "Name the task and choose how often it runs",
    "Select the time and day you want this task to start", "You have successfully scheduled the following task" };
static const char *wizard_body(struct stui *s)
{
    switch (s->wiz.page) {
    case WP_WELCOME: return "This wizard helps you schedule a program to run automatically, at the time and as often as you choose.\n\nScheduled tasks start while CiukiOS is running. A task that was due while the computer was off runs once at the next check.\n\nClick Next to continue.";
    case WP_PROGRAM: return "Choose a program installed in /Programs, or type the full path of another program below the list.";
    case WP_NAME: return "Type a name for this task, then choose how often it should be performed.";
    case WP_WHEN: return s->wiz.kind == ST_WEEKLY ? "Choose the time, the first day, and the days of the week." :
                         s->wiz.kind == ST_MONTHLY ? "Choose the time, the first day, and the day of the month." :
                         s->wiz.kind == ST_ONCE ? "Choose the time and the date on which the task runs." : "Choose the time and the first day.";
    default: return "";
    }
}
static void wizard_task(struct stui *s, struct st_task *t)
{
    memset(t, 0, sizeof(*t));
    snprintf(t->name, sizeof(t->name), "%s", s->wiz.name);
    snprintf(t->command, sizeof(t->command), "%s", s->wiz.command);
    t->created = now_of(s); t->last_run = -1; t->enabled = 1;
    t->kind = s->wiz.kind;
    (void)stui_parse_time(s->wiz.time, &t->minute_of_day);
    if (stui_parse_date(s->wiz.date, &t->start)) t->start = (struct st_date){ 1970, 1, 1 };
    t->weekday_mask = s->wiz.weekdays;
    t->day_of_month = (unsigned)atoi(s->wiz.day);
    if (t->kind == ST_AT_START) { t->minute_of_day = 0; t->start = (struct st_date){ 1970, 1, 1 }; t->weekday_mask = 0; t->day_of_month = 0; }
    if (t->kind != ST_WEEKLY) t->weekday_mask = 0;
    if (t->kind != ST_MONTHLY) t->day_of_month = 0;
}
static void wizard_summary(struct stui *s, char *out, size_t cap)
{
    struct st_task t; wizard_task(s, &t);
    char when[64], next[40];
    stui_describe(&t, when, sizeof(when));
    if (t.kind == ST_AT_START) snprintf(next, sizeof(next), "At system startup");
    else { int64_t n = st_next(&t, t.created); if (n < 0) snprintf(next, sizeof(next), "Never"); else stui_format_minutes(next, sizeof(next), n); }
    snprintf(out, cap, "Name: %s\nProgram: %s\nSchedule: %s\n%s%s\nNext run: %s", t.name, t.command, when,
             t.kind == ST_AT_START ? "" : "Starting: ", t.kind == ST_AT_START ? "" : s->wiz.date, next);
}
static void build_buttons(struct stui *s, const char *next, int back_enabled)
{
    add(s, DU_SEPARATOR, B_RULE, 0, 338, STUI_W, 2, "");
    add(s, DU_BUTTON, B_BACK, STUI_W - 262, 346, 80, 24, "< &Back")->disabled = !back_enabled;
    add(s, DU_BUTTON, B_NEXT, STUI_W - 180, 346, 80, 24, next);
    add(s, DU_BUTTON, B_CANCEL, STUI_W - 88, 346, 80, 24, "Cancel");
}
static void build_wizard(struct stui *s)
{
    struct stui_wizard *w = &s->wiz;
    reset(s, 0); s->mode = STUI_WIZARD;
    build_buttons(s, w->page == WP_FINISH ? "Finish" : "&Next >", w->page != WP_WELCOME);
    int x = 156;
    switch (w->page) {
    case WP_PROGRAM: {
        struct du_widget *l = add(s, DU_LISTVIEW, W_PROGRAMS, x, 100, 392, 170, "");
        l->view = DU_DETAILS; l->items = s->program_rows; l->column_count = 2; l->count = s->program_count;
        l->columns[0] = (struct du_column){ "Application", 220, 0 }; l->columns[1] = (struct du_column){ "Program", 150, 0 };
        memset(s->program_rows, 0, sizeof(s->program_rows));
        for (unsigned i = 0; i < s->program_count; i++) {
            snprintf(s->program_cells[i][0], 48, "%s", s->programs[i].label); snprintf(s->program_cells[i][1], 48, "%s", s->programs[i].name);
            s->program_rows[i].text[0] = s->program_cells[i][0]; s->program_rows[i].text[1] = s->program_cells[i][1];
            s->program_rows[i].icon16 = du_icon_argb(du_icon_file(s->programs[i].name, 0, 1), 16);
            s->program_rows[i].selected = (int)i == w->program;
        }
        if (w->program >= 0) { l->active = (unsigned)w->program; l->selection_active = 1; du_list_ensure(l, l->active); }
        add(s, DU_LABEL, W_BROWSE_LABEL, x, 280, 392, 16, "Or &browse: type the full path of another program");
        add(s, DU_EDIT, W_BROWSE, x, 300, 392, 22, w->browse);
        du_focus(s->ui, W_PROGRAMS);
        break;
    }
    case WP_NAME:
        add(s, DU_LABEL, W_NAME_LABEL, x, 100, 200, 16, "&Name:");
        add(s, DU_EDIT, W_NAME, x, 118, 300, 22, w->name);
        add(s, DU_LABEL, W_FREQ_LABEL, x, 156, 300, 16, "Perform this task:");
        for (int k = 0; k < 5; k++) {
            struct du_widget *r = add(s, DU_RADIO, W_FREQ0 + k, x, 178 + k * 24, 300, 20, kind_names[k]);
            r->total = 1; r->value = k == w->kind;
        }
        du_focus(s->ui, W_NAME);
        break;
    case WP_WHEN:
        add(s, DU_LABEL, W_TIME_LABEL, x, 108, 110, 16, "Start &time:");
        add(s, DU_EDIT, W_TIME, x + 120, 104, 90, 22, w->time);
        add(s, DU_LABEL, W_DATE_LABEL, x, 140, 110, 16, w->kind == ST_ONCE ? "Run &date:" : "Start &date:");
        add(s, DU_EDIT, W_DATE, x + 120, 136, 110, 22, w->date);
        if (w->kind == ST_WEEKLY)
            for (int d = 0; d < 7; d++) {
                struct du_widget *c = add(s, DU_CHECKBOX, W_DAYS0 + d, x + (d % 4) * 98, 182 + (d / 4) * 26, 94, 20, day_names[d]);
                c->value = (w->weekdays >> d) & 1;
            }
        if (w->kind == ST_MONTHLY) {
            add(s, DU_LABEL, W_DAY_LABEL, x, 182, 130, 16, "Day of the &month:");
            add(s, DU_EDIT, W_DAY, x + 120, 178, 50, 22, w->day);
        }
        add(s, DU_LABEL, W_HINT, x, 290, 392, 16, "Times use the 24-hour clock (HH:MM); dates are YYYY-MM-DD.");
        du_focus(s->ui, W_TIME);
        break;
    case WP_FINISH:
        add(s, DU_CHECKBOX, W_ADVANCED, x, 270, 392, 20, "&Open advanced properties for this task when I click Finish")->value = w->open_advanced;
        du_focus(s->ui, B_NEXT);
        break;
    default: du_focus(s->ui, B_NEXT); break;
    }
}
static void wizard_start(struct stui *s)
{
    struct stui_wizard *w = &s->wiz;
    memset(w, 0, sizeof(*w));
    w->program = -1; w->kind = ST_DAILY;
    snprintf(w->time, sizeof(w->time), "09:00");
    struct st_date d; unsigned m; civil(now_of(s), &d, &m); put_date(w->date, d);
    snprintf(w->day, sizeof(w->day), "%d", d.day);
    w->weekdays = 1u << st_weekday(d);
    load_programs(s);
    build_wizard(s);
}
static void wizard_collect(struct stui *s)
{
    struct stui_wizard *w = &s->wiz;
    switch (w->page) {
    case WP_PROGRAM: snprintf(w->browse, sizeof(w->browse), "%s", widget_text(s, W_BROWSE)); trim(w->browse); break;
    case WP_NAME: snprintf(w->name, sizeof(w->name), "%s", widget_text(s, W_NAME)); break;   /* validated below */
    case WP_WHEN:
        snprintf(w->time, sizeof(w->time), "%s", widget_text(s, W_TIME)); snprintf(w->date, sizeof(w->date), "%s", widget_text(s, W_DATE));
        if (w->kind == ST_WEEKLY) { w->weekdays = 0; for (int d = 0; d < 7; d++) if (widget_value(s, W_DAYS0 + d)) w->weekdays |= 1u << d; }
        if (w->kind == ST_MONTHLY) snprintf(w->day, sizeof(w->day), "%s", widget_text(s, W_DAY));
        break;
    case WP_FINISH: w->open_advanced = widget_value(s, W_ADVANCED); break;
    default: break;
    }
}
static int wizard_validate(struct stui *s)
{
    struct stui_wizard *w = &s->wiz;
    if (w->page == WP_PROGRAM) {
        if (w->browse[0]) {
            int bad = w->browse[0] != '/' || strlen(w->browse) >= ST_COMMAND;
            for (const char *p = w->browse; *p; p++) if ((unsigned char)*p < 0x20) bad = 1;
            if (bad) { warn(s, "The path of a program must be absolute (start with '/') and fit on one line (at most 95 characters)."); return -1; }
            snprintf(w->command, sizeof(w->command), "%s", w->browse); w->program = -1;
        } else if (w->program >= 0 && (unsigned)w->program < s->program_count) {
            snprintf(w->command, sizeof(w->command), "/Programs/%s", s->programs[w->program].name);
        } else { warn(s, "Select a program from the list, or type the full path of a program."); return -1; }
        char def[STUI_NAME];
        if (w->program >= 0) snprintf(def, sizeof(def), "%.31s", s->programs[w->program].label);
        else {
            char path[ST_COMMAND]; snprintf(path, sizeof(path), "%s", w->command);
            char *sp = strchr(path, ' '); if (sp) *sp = 0;
            const char *slash = strrchr(path, '/');
            snprintf(def, sizeof(def), "%.31s", slash ? slash + 1 : path);
        }
        for (char *p = def; *p; p++) { if ((unsigned char)*p < 0x20) *p = '_'; else if (*p == '=') *p = '-'; }
        if (!w->name[0] || !strcmp(w->name, w->name_default)) snprintf(w->name, sizeof(w->name), "%s", def);
        snprintf(w->name_default, sizeof(w->name_default), "%s", def);
    } else if (w->page == WP_NAME) {
        char name[DU_TEXT_MAX]; snprintf(name, sizeof(name), "%s", widget_text(s, W_NAME)); trim(name);
        if (!name[0]) { warn(s, "Type a name for the task."); return -1; }
        if (strlen(name) >= STUI_NAME) { warn(s, "The task name can have at most 31 characters."); return -1; }
        for (const char *p = name; *p; p++) if (*p == '=' || (unsigned char)*p < 0x20) { warn(s, "The task name cannot contain '=' or line breaks."); return -1; }
        if (begin_edit(s)) return -1;
        if (find_task(s, name)) {
            char text[120]; snprintf(text, sizeof(text), "A task named \"%s\" already exists. Choose a different name.", name); warn(s, text); return -1;
        }
        snprintf(w->name, sizeof(w->name), "%s", name);
    } else if (w->page == WP_WHEN) {
        struct st_task t; memset(&t, 0, sizeof(t));
        if (fill_schedule(s, &t, w->kind, w->time, w->date, w->day, w->weekdays, 1)) return -1;
    }
    return 0;
}
static void wizard_finish(struct stui *s)
{
    struct st_task t; wizard_task(s, &t);
    if (begin_edit(s)) return;
    int e = st_add(&s->store, &t);
    char text[160];
    if (e == -EEXIST) { snprintf(text, sizeof(text), "A task named \"%s\" already exists. Go back and choose a different name.", t.name); warn(s, text); return; }
    if (e == -ENOSPC) { snprintf(text, sizeof(text), "The Scheduled Tasks folder is full (%d tasks). Delete a task before adding another.", ST_MAX); warn(s, text); return; }
    if (e) { warn(s, "The task could not be created because one of its settings is not valid (error 22). Go back and check the program, name and schedule."); return; }
    if (end_edit(s)) return;
    int open = s->wiz.open_advanced;
    snprintf(s->selected_name, sizeof(s->selected_name), "%s", t.name);
    snprintf(text, sizeof(text), "Created \"%.31s\".", t.name);
    s->status[0] = 0;
    build_folder(s);
    set_status(s, text);
    if (open) stui_open_props(s, t.name);
}
static void wizard_next(struct stui *s)
{
    struct stui_wizard *w = &s->wiz;
    wizard_collect(s);
    if (wizard_validate(s)) return;
    if (w->page == WP_FINISH) { wizard_finish(s); return; }
    w->page++;
    if (w->page == WP_WHEN && w->kind == ST_AT_START) w->page = WP_FINISH;
    build_wizard(s);
}
static void wizard_back(struct stui *s)
{
    struct stui_wizard *w = &s->wiz;
    if (w->page == WP_WELCOME) return;
    wizard_collect(s);
    w->page--;
    if (w->page == WP_WHEN && w->kind == ST_AT_START) w->page = WP_NAME;
    build_wizard(s);
}

/* ---- task properties ---- */
static void props_visibility(struct stui *s)
{
    struct stui_props *p = &s->props;
    for (unsigned i = 0; i < s->ui->count; i++) {
        struct du_widget *w = &s->ui->widgets[i];
        int tab = w->id >= P_LAST ? 2 : w->id >= P_KIND_LABEL ? 1 : w->id >= P_RUN_LABEL ? 0 : -1;
        if (tab >= 0) w->hidden = tab != p->tab;
    }
    int k = p->task.kind;
    struct { int id, show; } rules[] = {
        { P_TIME_LABEL, k != ST_AT_START }, { P_TIME, k != ST_AT_START }, { P_DATE_LABEL, k != ST_AT_START }, { P_DATE, k != ST_AT_START },
        { P_DAY_LABEL, k == ST_MONTHLY }, { P_DAY, k == ST_MONTHLY } };
    for (unsigned i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) { struct du_widget *w = du_get(s->ui, rules[i].id); if (w && p->tab == 1) w->hidden = !rules[i].show; }
    for (int d = 0; d < 7; d++) { struct du_widget *w = du_get(s->ui, P_DAYS0 + d); if (w && p->tab == 1) w->hidden = k != ST_WEEKLY; }
    struct du_widget *dl = du_get(s->ui, P_DATE_LABEL);
    if (dl) du_set_text(dl, k == ST_ONCE ? "Run &date:" : "Start &date:");
    s->ui->dirty = 1;
}
static void props_texts(struct stui *s)
{
    struct stui_props *p = &s->props; const struct st_task *t = &p->task;
    char text[200], when[24];
    snprintf(text, sizeof(text), "Task: %s", t->name); sysui_text(s->ui, P_INFO_NAME, text);
    stui_format_minutes(when, sizeof(when), t->created);
    snprintf(text, sizeof(text), "Created: %s", when); sysui_text(s->ui, P_INFO_CREATED, text);
    char next[40]; stui_next_text(t, now_of(s), next, sizeof(next));
    snprintf(text, sizeof(text), "Next run: %s", next); sysui_text(s->ui, P_NEXT, text);
    if (t->last_run >= 0) { stui_format_minutes(when, sizeof(when), t->last_run); snprintf(text, sizeof(text), "Last run: %s", when); }
    else snprintf(text, sizeof(text), "Last run: Never");
    sysui_text(s->ui, P_LAST, text);
    if (t->last_run < 0) snprintf(text, sizeof(text), "Last result: not run yet");
    else if (t->last_result) snprintf(text, sizeof(text), "Last result: failed to start (error %d)", t->last_result < 0 ? -t->last_result : t->last_result);
    else snprintf(text, sizeof(text), "Last result: started");
    sysui_text(s->ui, P_RESULT, text);
    char status[48]; stui_status_text(t, status, sizeof(status));
    snprintf(text, sizeof(text), "Status: %s", status); sysui_text(s->ui, P_STATE, text);
}
static void build_props(struct stui *s)
{
    struct stui_props *p = &s->props;
    reset(s, 0); s->mode = STUI_PROPS;
    memcpy(s->tab_items, (struct du_item[3]){ { .text = { "Task" } }, { .text = { "Schedule" } }, { .text = { "Settings" } } }, sizeof(s->tab_items));
    struct du_widget *tabs = add(s, DU_TABS, P_TABS, 6, 6, STUI_W - 12, 326, "");
    tabs->items = s->tab_items; tabs->count = 3; tabs->active = (unsigned)p->tab;
    add(s, DU_BUTTON, P_OK, STUI_W - 262, 346, 80, 24, "OK");
    add(s, DU_BUTTON, P_CANCEL, STUI_W - 176, 346, 80, 24, "Cancel");
    add(s, DU_BUTTON, P_APPLY, STUI_W - 88, 346, 80, 24, "&Apply");
    add(s, DU_LABEL, P_RUN_LABEL, 24, 48, 200, 16, "&Run:");
    add(s, DU_EDIT, P_RUN, 24, 66, STUI_W - 48, 22, p->task.command);
    add(s, DU_LABEL, P_INFO_NAME, 24, 106, 512, 16, "");
    add(s, DU_CHECKBOX, P_ENABLED, 24, 134, 512, 20, "&Enabled (the task runs at its scheduled times)")->value = p->task.enabled;
    add(s, DU_LABEL, P_INFO_CREATED, 24, 166, 512, 16, "");
    add(s, DU_LABEL, P_KIND_LABEL, 24, 48, 200, 16, "Schedule task:");
    for (int k = 0; k < 5; k++) {
        struct du_widget *r = add(s, DU_RADIO, P_KIND0 + k, 24, 70 + k * 24, 210, 20, kind_names[k]);
        r->total = 1; r->value = k == p->task.kind;
    }
    add(s, DU_LABEL, P_TIME_LABEL, 260, 74, 100, 16, "Start &time:");
    add(s, DU_EDIT, P_TIME, 370, 70, 90, 22, p->time);
    add(s, DU_LABEL, P_DATE_LABEL, 260, 106, 100, 16, "Start &date:");
    add(s, DU_EDIT, P_DATE, 370, 102, 110, 22, p->date);
    add(s, DU_LABEL, P_DAY_LABEL, 260, 138, 110, 16, "Day of the &month:");
    add(s, DU_EDIT, P_DAY, 370, 134, 50, 22, p->day);
    for (int d = 0; d < 7; d++) {
        struct du_widget *c = add(s, DU_CHECKBOX, P_DAYS0 + d, 260 + (d % 2) * 140, 138 + (d / 2) * 24, 130, 20, day_names[d]);
        c->value = (p->task.weekday_mask >> d) & 1;
    }
    add(s, DU_LABEL, P_NEXT, 24, 250, 512, 16, "");
    add(s, DU_LABEL, P_LAST, 24, 52, 512, 16, "");
    add(s, DU_LABEL, P_RESULT, 24, 76, 512, 16, "");
    add(s, DU_LABEL, P_STATE, 24, 100, 512, 16, "");
    add(s, DU_LABEL, P_NOTE, 24, 140, 512, 16, "One time only tasks are disabled after they run and stay here until deleted.");
    props_texts(s); props_visibility(s);
    du_focus(s->ui, p->tab == 0 ? P_RUN : p->tab == 1 ? P_KIND0 + p->task.kind : P_TABS);
}
void stui_open_props(struct stui *s, const char *name)
{
    if (begin_edit(s)) return;
    struct st_task *t = find_task(s, name);
    if (!t) { warn(s, "That task no longer exists."); stui_refresh(s); return; }
    struct stui_props *p = &s->props;
    memset(p, 0, sizeof(*p));
    snprintf(p->original, sizeof(p->original), "%s", name);
    p->task = *t;
    put_hhmm(p->time, t->minute_of_day); put_date(p->date, t->start);
    snprintf(p->day, sizeof(p->day), "%u", t->day_of_month ? t->day_of_month : 1);
    build_props(s);
}
static void props_collect(struct stui *s)
{
    struct stui_props *p = &s->props;
    if (du_get(s->ui, P_RUN)) {
        snprintf(p->task.command, sizeof(p->task.command), "%s", widget_text(s, P_RUN));
        p->task.enabled = widget_value(s, P_ENABLED);
        snprintf(p->time, sizeof(p->time), "%s", widget_text(s, P_TIME)); snprintf(p->date, sizeof(p->date), "%s", widget_text(s, P_DATE));
        snprintf(p->day, sizeof(p->day), "%s", widget_text(s, P_DAY));
        p->task.weekday_mask = 0;
        for (int d = 0; d < 7; d++) if (widget_value(s, P_DAYS0 + d)) p->task.weekday_mask |= 1u << d;
    }
}
/* 1 applied, 0 refused (message shown or task gone). */
static int props_apply(struct stui *s)
{
    struct stui_props *p = &s->props;
    props_collect(s);
    struct st_task t = p->task;
    trim(t.command);
    char program[48], argument[161];
    if (!t.command[0] || strlen(t.command) >= ST_COMMAND || ss_split(t.command, program, sizeof(program), argument, sizeof(argument))) {
        warn(s, "The Run box needs a program: an absolute path or a name in /Programs, with optional arguments on one line."); return 0;
    }
    for (const char *c = t.command; *c; c++) if ((unsigned char)*c < 0x20) { warn(s, "The command line must fit on one line."); return 0; }
    if (fill_schedule(s, &t, p->task.kind, p->time, p->date, p->day, p->task.weekday_mask, t.enabled && t.last_run < 0)) return 0;
    if (begin_edit(s)) return 0;
    struct st_task *old = find_task(s, p->original);
    if (!old) { warn(s, "That task no longer exists."); build_folder(s); return 0; }
    int changed = old->kind != t.kind || old->minute_of_day != t.minute_of_day || old->weekday_mask != t.weekday_mask ||
                  old->day_of_month != t.day_of_month || memcmp(&old->start, &t.start, sizeof(t.start));
    t.created = changed ? now_of(s) : old->created;
    s->trial = s->store;
    st_remove(&s->trial, p->original);
    struct st_task probe = t; probe.last_run = -1;
    int e = st_add(&s->trial, &probe);
    if (e) { warn(s, "One of the settings is not valid (error 22). Check the program, the time and the date."); return 0; }
    int enabled = t.enabled;
    *old = t; old->enabled = enabled;
    if (end_edit(s)) return 0;
    p->task = *old;
    return 1;
}
static void props_close(struct stui *s) { s->status[0] = 0; build_folder(s); }

/* ---- Maintenance Wizard ---- */
static const char *maint_title(int page)
{ return page == MP_WELCOME ? "Maintenance Wizard" : page == MP_SLOT ? "When do you want these tasks to run?" :
         page == MP_TOOLS ? "Which tools do you want to schedule?" : "Maintenance Wizard is ready to schedule your tasks"; }
static void maint_choice(struct stui *s, struct mw_choice *c)
{ *c = s->maint.choice; }
static void build_maint(struct stui *s)
{
    struct stui_maint *m = &s->maint;
    reset(s, 0); s->mode = STUI_MAINT;
    build_buttons(s, m->page == MP_FINISH ? "Finish" : "&Next >", m->page != MP_WELCOME);
    int x = 156;
    switch (m->page) {
    case MP_WELCOME:
        add(s, DU_RADIO, M_EXPRESS, x, 170, 392, 20, "&Express (recommended)")->total = 2;
        add(s, DU_LABEL, M_DESC1, x + 22, 192, 370, 16, "Schedules all of the tools below with the recommended settings.");
        add(s, DU_RADIO, M_CUSTOM, x, 224, 392, 20, "&Custom")->total = 2;
        add(s, DU_LABEL, M_DESC2, x + 22, 246, 370, 16, "Lets you choose which tools to schedule.");
        du_get(s->ui, M_EXPRESS)->value = m->choice.express; du_get(s->ui, M_CUSTOM)->value = !m->choice.express;
        du_focus(s->ui, m->choice.express ? M_EXPRESS : M_CUSTOM);
        break;
    case MP_SLOT: {
        static const char *const slots[3] = { "&Nights: the tasks start at 00:00", "&Days: the tasks start at 12:00", "&Evenings: the tasks start at 20:00" };
        for (int k = 0; k < 3; k++) { struct du_widget *r = add(s, DU_RADIO, M_SLOT0 + k, x, 130 + k * 30, 392, 20, slots[k]); r->total = 3; r->value = k == m->choice.slot; }
        add(s, DU_LABEL, M_LABEL, x, 240, 392, 16, "The tasks run weekly, on Sundays, 30 minutes apart.");
        du_focus(s->ui, M_SLOT0 + m->choice.slot);
        break;
    }
    case MP_TOOLS:
        for (int k = 0; k < MW_TOOLS; k++) {
            char label[48]; snprintf(label, sizeof(label), "%s", mw_tool_name(k));
            add(s, DU_CHECKBOX, M_TOOL0 + k, x, 130 + k * 30, 392, 20, label)->value = (m->choice.tools >> k) & 1;
        }
        du_focus(s->ui, M_TOOL0);
        break;
    default: du_focus(s->ui, B_NEXT); break;
    }
}
static void maint_start(struct stui *s)
{
    memset(&s->maint, 0, sizeof(s->maint));
    s->maint.choice = (struct mw_choice){ .express = 1, .slot = MW_NIGHT, .tools = 0 };
    build_maint(s);
}
static void maint_collect(struct stui *s)
{
    struct stui_maint *m = &s->maint;
    if (m->page == MP_WELCOME) m->choice.express = widget_value(s, M_EXPRESS);
    else if (m->page == MP_SLOT) { for (int k = 0; k < 3; k++) if (widget_value(s, M_SLOT0 + k)) m->choice.slot = k; }
    else if (m->page == MP_TOOLS) { m->choice.tools = 0; for (int k = 0; k < MW_TOOLS; k++) if (widget_value(s, M_TOOL0 + k)) m->choice.tools |= 1u << k; }
}
static void maint_summary(struct stui *s, char *out, size_t cap)
{
    struct mw_choice c = s->maint.choice;
    unsigned tools = c.express ? (1u << MW_TOOLS) - 1 : c.tools;
    static const unsigned start[3] = { 0, 720, 1200 };
    size_t at = (size_t)snprintf(out, cap, "Weekly, on Sundays:\n");
    unsigned n = 0;
    for (int k = 0; k < MW_TOOLS; k++) if (tools & (1u << k)) {
        unsigned m = start[c.slot] + n * 30;
        at += (size_t)snprintf(out + at, cap - at, "%s at %02u:%02u\n", mw_tool_name(k), m / 60, m % 60);
        n++;
    }
    snprintf(out + at, cap - at, "\nRunning the wizard again replaces these tasks.");
}
static int maint_apply(struct stui *s, struct st_store *store, int *count)
{
    struct mw_choice c; maint_choice(s, &c);
    struct st_date d; unsigned m; civil(now_of(s), &d, &m);
    int r = mw_apply(store, &c, d, s->now);
    if (r < 0) {
        warn(s, r == -ENOSPC ? "The Scheduled Tasks folder does not have room for these tasks. Delete some tasks first." :
                "Select at least one tool to schedule.");
        return -1;
    }
    *count = r; return 0;
}
static void maint_next(struct stui *s)
{
    struct stui_maint *m = &s->maint; int n;
    maint_collect(s);
    if (m->page == MP_FINISH) {
        if (begin_edit(s)) return;
        if (maint_apply(s, &s->store, &n)) return;
        if (end_edit(s)) return;
        char text[256];
        snprintf(text, sizeof(text), "Maintenance Wizard created %d scheduled task%s. They are in the Scheduled Tasks folder.\n\nThe tools come with the disk tools of CiukiOS; a task whose program is not installed yet reports an error when it runs.", n, n == 1 ? "" : "s");
        s->status[0] = 0; build_folder(s);
        char shortt[64]; snprintf(shortt, sizeof(shortt), "Maintenance Wizard created %d task%s.", n, n == 1 ? "" : "s");
        set_status(s, shortt);
        sysui_box(s->ui, D_INFO, "Maintenance Wizard", text, DU_DLG_ICON_INFO);
        return;
    }
    int next = m->page + 1;
    if (next == MP_TOOLS && m->choice.express) next = MP_FINISH;
    if (next == MP_FINISH) {
        s->trial = s->store; if (store_read(s, &s->trial)) { begin_edit(s); return; }
        if (maint_apply(s, &s->trial, &n)) return;
    }
    m->page = next;
    build_maint(s);
}
static void maint_back(struct stui *s)
{
    struct stui_maint *m = &s->maint;
    if (m->page == MP_WELCOME) return;
    maint_collect(s);
    m->page--;
    if (m->page == MP_TOOLS && m->choice.express) m->page = MP_SLOT;
    build_maint(s);
}

/* ---- painting of the wizard pages ---- */
static void banner(struct du_ui *u, const char *title)
{
    struct du_canvas *c = &u->canvas;
    du_fill(c, (struct du_rect){ 0, 0, 140, 338 }, DU_SELECT);
    const struct du_icon *icon = du_icon_for("program:schedtask", 48);
    if (icon) du_icon_draw(c, 46, 24, icon, DU_ICON_NORMAL);
    du_text_wrap(c, (struct du_rect){ 10, 90, 120, 60 }, title, DU_SELECT_INK);
}
static void paint(struct du_ui *u, void *ctx)
{
    struct stui *s = ctx; struct du_canvas *c = &u->canvas;
    if (s->mode != STUI_WIZARD && s->mode != STUI_MAINT) return;
    const char *heading, *body = ""; char summary[320];
    if (s->mode == STUI_WIZARD) {
        heading = wizard_heading[s->wiz.page];
        body = wizard_body(s);
        if (s->wiz.page == WP_FINISH) { wizard_summary(s, summary, sizeof(summary)); body = summary; }
    } else {
        heading = maint_title(s->maint.page);
        if (s->maint.page == MP_WELCOME) body = "Maintenance Wizard schedules the tools that keep the disk in good shape: Disk Defragmenter, ScanDisk and Disk Cleanup, once a week at the time you choose.\n\nThe tools come with the disk tools of CiukiOS.";
        else if (s->maint.page == MP_SLOT) body = "Choose the time of day for the weekly tune-up.";
        else if (s->maint.page == MP_TOOLS) body = "Check the tools to run every week.";
        else { maint_summary(s, summary, sizeof(summary)); body = summary; }
    }
    banner(u, s->mode == STUI_WIZARD ? "Scheduled Tasks" : "Maintenance Wizard");
    du_text_font(c, desk_theme->font_bold, 156, 14, heading, DU_INK);
    du_text_wrap(c, (struct du_rect){ 156, 42, 392, s->mode == STUI_WIZARD && s->wiz.page == WP_FINISH ? 200 : 120 }, body, DU_INK);
}

/* ---- events ---- */
static void menu_init(struct stui *s)
{
    s->file_items[0] = (struct du_menu_item){ "&Open", C_OPEN, 0, 0, DU_ITEM_DEFAULT };
    s->file_items[1] = (struct du_menu_item){ "&Run", C_RUN, 0, 0, 0 };
    s->file_items[2] = (struct du_menu_item){ "&Delete", C_DELETE, DU_KEY_DELETE, 0, 0 };
    s->file_items[3] = (struct du_menu_item){ NULL, 0, 0, 0, DU_ITEM_SEPARATOR };
    s->file_items[4] = (struct du_menu_item){ "P&roperties", C_PROPS, 0, 0, 0 };
    s->file_items[5] = (struct du_menu_item){ NULL, 0, 0, 0, DU_ITEM_SEPARATOR };
    s->file_items[6] = (struct du_menu_item){ "E&xit", C_EXIT, 0, 0, 0 };
    s->advanced_items[0] = (struct du_menu_item){ "&Maintenance Wizard...", C_MAINT, 0, 0, 0 };
    s->advanced_items[1] = (struct du_menu_item){ "View &Log", C_LOG, 0, 0, 0 };
    s->help_items[0] = (struct du_menu_item){ "&About Scheduled Tasks", C_ABOUT, 0, 0, 0 };
    s->menus[0] = (struct du_menu){ "&File", s->file_items, 7 };
    s->menus[1] = (struct du_menu){ "&Advanced", s->advanced_items, 2 };
    s->menus[2] = (struct du_menu){ "&Help", s->help_items, 1 };
    s->popup_items[0] = (struct du_menu_item){ "&Run", C_RUN, 0, 0, 0 };
    s->popup_items[1] = (struct du_menu_item){ "&Delete", C_DELETE, 0, 0, 0 };
    s->popup_items[2] = (struct du_menu_item){ "P&roperties", C_PROPS, 0, 0, 0 };
}
void stui_init(struct stui *s, struct du_ui *u, const struct stui_host *host)
{
    memset(s, 0, sizeof(*s));
    s->ui = u; s->host = host; s->selected = -1;
    u->event = stui_event; u->user = s; u->paint = paint;
    menu_init(s);
    now_of(s);
    if (store_read(s, &s->store)) { st_init(&s->store); snprintf(s->status, sizeof(s->status), "The list of scheduled tasks could not be read."); }
    build_folder(s);
    du_timer_start(u, T_REFRESH, 15000000000ull, 1);
}
static void open_row(struct stui *s)
{
    if (s->selected == 0) wizard_start(s);
    else if (selected_task_name(s)) stui_open_props(s, selected_task_name(s));
}
static void folder_event(struct stui *s, const struct du_event *e)
{
    switch (e->type) {
    case DU_EV_TICK: if (e->id == T_REFRESH) stui_refresh(s); return;
    case DU_EV_SELECT: if (e->id == A_LIST && e->index >= 0 && (unsigned)e->index <= s->store.count) select_row(s, e->index); return;
    case DU_EV_OPEN: if (e->id == A_LIST && e->index >= 0 && (unsigned)e->index <= s->store.count) { select_row(s, e->index); open_row(s); } return;
    case DU_EV_CONTEXT:
        if (e->id == A_LIST && e->index >= 1 && (unsigned)e->index <= s->store.count) {
            select_row(s, e->index); du_popup(s->ui, s->popup_items, 3, e->x, e->y, A_LIST);
        }
        return;
    case DU_EV_KEY:
        if (e->key == DU_KEY_DELETE) ask_delete(s);
        else if (e->key == DU_KEY_F5) stui_refresh(s);
        return;
    case DU_EV_DIALOG:
        if (e->id == D_DELETE && e->index == 0) do_delete(s);
        return;
    case DU_EV_ACTIVATE: break;
    default: return;
    }
    switch (e->id) {
    case C_OPEN: open_row(s); break;
    case C_RUN: run_selected(s); break;
    case C_DELETE: ask_delete(s); break;
    case C_PROPS: if (selected_task_name(s)) stui_open_props(s, selected_task_name(s)); break;
    case C_EXIT: if (s->host->exit) s->host->exit(s->host->ctx); break;
    case C_MAINT: maint_start(s); break;
    case C_LOG: {
        static char text[DU_DIALOG_TEXT];
        if (!begin_edit(s)) { stui_log_text(s, text, sizeof(text)); sysui_box(s->ui, D_INFO, "Scheduled Tasks Log", text, DU_DLG_ICON_INFO); fill_rows(s); }
        break;
    }
    case C_ABOUT:
        sysui_box(s->ui, D_INFO, "About Scheduled Tasks", "CiukiOS Scheduled Tasks - Classic computing, one woof at a time.\nTasks are stored in /system/settings/schedule.cfg and run by the desktop while CiukiOS is running.", DU_DLG_ICON_INFO);
        break;
    default: break;
    }
}
static void wizard_event(struct stui *s, const struct du_event *e)
{
    struct stui_wizard *w = &s->wiz;
    if (e->type == DU_EV_SELECT && e->id == W_PROGRAMS && e->index >= 0 && (unsigned)e->index < s->program_count) {
        w->program = e->index; w->browse[0] = 0;
        struct du_widget *b = du_get(s->ui, W_BROWSE); if (b) { du_set_text(b, ""); du_invalidate_widget(s->ui, W_BROWSE); }
        for (unsigned i = 0; i < s->program_count; i++) s->program_rows[i].selected = (int)i == e->index;
    } else if (e->type == DU_EV_OPEN && e->id == W_PROGRAMS) wizard_next(s);
    else if (e->type == DU_EV_CHANGE && e->id == W_BROWSE && widget_text(s, W_BROWSE)[0]) {
        w->program = -1;
        for (unsigned i = 0; i < s->program_count; i++) s->program_rows[i].selected = 0;
        du_get(s->ui, W_PROGRAMS)->selection_active = 0; du_invalidate_widget(s->ui, W_PROGRAMS);
    } else if (e->type == DU_EV_CHANGE && e->id >= W_FREQ0 && e->id < W_FREQ0 + 5) w->kind = e->id - W_FREQ0;
    else if (e->type == DU_EV_ACTIVATE && e->id == B_NEXT) wizard_next(s);
    else if (e->type == DU_EV_ACTIVATE && e->id == B_BACK) wizard_back(s);
    else if ((e->type == DU_EV_ACTIVATE && e->id == B_CANCEL) || e->type == DU_EV_CANCEL || (e->type == DU_EV_KEY && e->key == DU_KEY_ESC)) build_folder(s);
    else if (e->type == DU_EV_COMMIT) wizard_next(s);
}
static void props_event(struct stui *s, const struct du_event *e)
{
    struct stui_props *p = &s->props;
    if (e->type == DU_EV_SELECT && e->id == P_TABS) { props_collect(s); p->tab = e->index; props_visibility(s); }
    else if (e->type == DU_EV_CHANGE && e->id >= P_KIND0 && e->id < P_KIND0 + 5) { props_collect(s); p->task.kind = e->id - P_KIND0; props_visibility(s); }
    else if (e->type == DU_EV_ACTIVATE && e->id == P_OK) { if (props_apply(s)) props_close(s); }
    else if (e->type == DU_EV_ACTIVATE && e->id == P_APPLY) { if (props_apply(s)) { props_texts(s); s->ui->dirty = 1; } }
    else if ((e->type == DU_EV_ACTIVATE && e->id == P_CANCEL) || e->type == DU_EV_CANCEL || (e->type == DU_EV_KEY && e->key == DU_KEY_ESC)) props_close(s);
    else if (e->type == DU_EV_COMMIT) { if (props_apply(s)) props_close(s); }
}
static void maint_event(struct stui *s, const struct du_event *e)
{
    if (e->type == DU_EV_ACTIVATE && e->id == B_NEXT) maint_next(s);
    else if (e->type == DU_EV_ACTIVATE && e->id == B_BACK) maint_back(s);
    else if ((e->type == DU_EV_ACTIVATE && e->id == B_CANCEL) || e->type == DU_EV_CANCEL || (e->type == DU_EV_KEY && e->key == DU_KEY_ESC)) build_folder(s);
}
void stui_event(struct du_ui *u, const struct du_event *e, void *ctx)
{
    struct stui *s = ctx; (void)u;
    if (e->type == DU_EV_TICK) { if (s->mode == STUI_FOLDER) folder_event(s, e); else now_of(s); return; }
    switch (s->mode) {
    case STUI_FOLDER: folder_event(s, e); break;
    case STUI_WIZARD: wizard_event(s, e); break;
    case STUI_PROPS: props_event(s, e); break;
    default: maint_event(s, e); break;
    }
}
