/* G30 Scheduled Tasks: the real widgets driven with synthetic input over a fake task file, clock,
 * /Programs listing and launcher: folder, Add Scheduled Task wizard, properties, Maintenance Wizard.
 * SPDX-License-Identifier: MIT */
#include "../../../apps/systools/schedtask_ui.h"
#include "acc_test.h"
#include <errno.h>

/* ---- fakes ---- */
static char file[SS_TEXT]; static size_t file_length; static int file_exists, saves, fail_load, fail_save, fail_launch;
static int64_t clock_now = 29861861;            /* 2026-10-11 (Sunday) 09:41 local */
static int next_request = 6; static char launched[64]; static int launches, exits;
static const char *const installed[] = { "calc", "clock", "regedit", "desktop", "sysinfo", "unknownprog" };
static int64_t f_now(void *c) { (void)c; return clock_now; }
static int f_load(void *c, char *text, size_t cap, size_t *length)
{
    (void)c;
    if (fail_load) { fail_load--; return -EIO; }
    if (!file_exists) return -ENOENT;
    assert(file_length < cap); memcpy(text, file, file_length); *length = file_length; return 0;
}
static int f_save(void *c, const char *text, size_t length)
{
    (void)c;
    if (fail_save) return -ENOSPC;
    assert(length < sizeof(file)); memcpy(file, text, length); file_length = length; file_exists = 1; saves++; return 0;
}
static int f_programs(void *c, char names[][STUI_NAME], unsigned cap, unsigned *count)
{
    (void)c; unsigned n = 0;
    for (unsigned i = 0; i < sizeof(installed) / sizeof(installed[0]) && i < cap; i++) snprintf(names[n++], STUI_NAME, "%s", installed[i]);
    *count = n; return 0;
}
static int f_launch(void *c, const char *task_name, const char *program)
{ (void)c; (void)task_name; if (fail_launch) return -EAGAIN; snprintf(launched, sizeof(launched), "%s", program); launches++; return next_request++; }
static void f_exit(void *c) { (void)c; exits++; }
static const struct stui_host host = { NULL, f_now, f_load, f_save, f_programs, f_launch, f_exit };

/* ---- input ---- */
static struct du_ui ui; static struct stui s; static uint32_t pixels[STUI_W * STUI_H];
static const char *folder;
static int code_of(char c, int *shift)
{
    static const char rows[] = "qwertyuiop\0asdfghjkl\0zxcvbnm";
    static const int base[] = { 0x10, 0x1e, 0x2c };
    static const char digits[] = "1234567890", plain[] = "-=[];',./`\\ ", shifted[] = "_+{}:\"<>?~|";
    static const int plain_codes[] = { 0x0c, 0x0d, 0x1a, 0x1b, 0x27, 0x28, 0x33, 0x34, 0x35, 0x29, 0x2b, 0x39 };
    *shift = 0;
    if (c >= 'A' && c <= 'Z') { *shift = 1; c = (char)(c + 32); }
    if (c >= 'a' && c <= 'z') {
        const char *p = rows;
        for (int row = 0; row < 3; row++) { const char *at = strchr(p, c); if (at && (at - p) < (int)strlen(p)) return base[row] + (int)(at - p); p += strlen(p) + 1; }
    }
    const char *at = strchr(digits, c); if (c && at) return 0x02 + (int)(at - digits);
    at = strchr(plain, c); if (c && at) return plain_codes[at - plain];
    at = strchr(shifted, c); if (c && at) { *shift = 1; return plain_codes[at - shifted]; }
    return 0;
}
static void type(const char *t)
{
    for (; *t; t++) {
        int shift, code = code_of(*t, &shift); assert(code);
        if (shift) at_key(&ui, 0x2a, 1);
        at_tap(&ui, code);
        if (shift) at_key(&ui, 0x2a, 0);
    }
}
static void render(void) { du_render(&ui); }
static void snap(const char *name) { render(); at_save(folder, name, &ui); }
static void click(int id) { at_click_widget(&ui, id); }
static void put(int id, const char *text)     /* click the edit, select all, type */
{ click(id); at_chord(&ui, DU_KEY_LCTRL, 0x1e); type(text); }
static const char *text_of(int id) { struct du_widget *w = du_get(&ui, id); assert(w); return w->text; }
static void dismiss(int button)
{
    assert(ui.dialog.kind);
    struct du_rect r = du_dialog_button(&ui, (unsigned)button);
    at_click(&ui, r.x + r.w / 2, r.y + r.h / 2);
    assert(!ui.dialog.kind);
}
static void answer(int button)     /* the answer may open a follow-up dialog */
{ struct du_rect r = du_dialog_button(&ui, (unsigned)button); at_click(&ui, r.x + r.w / 2, r.y + r.h / 2); }
static void expect_dialog(const char *needle) { assert(ui.dialog.kind); assert(strstr(ui.dialog.text, needle)); }
static void warned(const char *needle) { expect_dialog(needle); dismiss(0); }
static void alt(int code) { at_key(&ui, 0x38, 1); at_tap(&ui, code); at_key(&ui, 0x38, 0); }
static void menu_pick(int mnemonic, int index) { alt(mnemonic); for (int i = 0; i < index; i++) at_tap(&ui, DU_KEY_DOWN); at_tap(&ui, DU_KEY_ENTER); }
#define FILE_MENU 0x21
#define ADVANCED_MENU 0x1e
static void select_row(int row)
{
    struct du_widget *w = du_get(&ui, 1); assert(w);
    struct du_rect r = du_list_row(w, (unsigned)row); assert(r.w > 0);
    at_click(&ui, r.x + 40, r.y + r.h / 2);
}
static void select_program(int row)
{
    struct du_widget *w = du_get(&ui, 20); assert(w);
    struct du_rect r = du_list_row(w, (unsigned)row); assert(r.w > 0);
    at_click(&ui, r.x + 40, r.y + r.h / 2);
}
static const char *cell(int row, int col) { return du_get(&ui, 1)->items[row].text[col]; }
static struct st_store read_file(void)
{
    static struct st_store st; st_init(&st);
    char text[SS_TEXT]; assert(file_length < sizeof(text)); memcpy(text, file, file_length); text[file_length] = 0;
    st_load(&st, text); return st;
}
static const struct st_task *task(const struct st_store *st, const char *name)
{ for (unsigned i = 0; i < st->count; i++) if (!strcmp(st->t[i].name, name)) return &st->t[i]; return NULL; }
static void write_store(const struct st_store *st)
{ char t[SS_TEXT]; int n = st_save(st, t, sizeof(t)); assert(n > 0); memcpy(file, t, (size_t)n); file_length = (size_t)n; file_exists = 1; }
static void next(void) { click(11); }
static void open_wizard(void)
{ du_focus(&ui, 1); select_row(0); at_tap(&ui, DU_KEY_ENTER); assert(s.mode == STUI_WIZARD && s.wiz.page == WP_WELCOME); }
/* Runs the wizard to the Name page for catalogue row `row` (sorted by label). */
static void to_name_page(int row)
{
    open_wizard(); next(); assert(s.wiz.page == WP_PROGRAM);
    select_program(row); next(); assert(s.wiz.page == WP_NAME);
}

int main(int argc, char **argv)
{
    folder = argc > 1 ? argv[1] : NULL;
    du_init(&ui, pixels, STUI_W, STUI_H);
    stui_init(&s, &ui, &host);
    /* Pure helpers. */
    unsigned m; struct st_date d;
    assert(!stui_parse_time("9:05", &m) && m == 545 && !stui_parse_time(" 23:59 ", &m) && m == 1439 && !stui_parse_time("00:00", &m));
    assert(stui_parse_time("24:00", &m) && stui_parse_time("9:5", &m) && stui_parse_time("0900", &m) && stui_parse_time("", &m) && stui_parse_time("12:60", &m));
    assert(!stui_parse_date("2028-02-29", &d) && d.month == 2 && d.day == 29);
    assert(stui_parse_date("2026-02-29", &d) && stui_parse_date("2026-13-01", &d) && stui_parse_date("1969-12-31", &d) && stui_parse_date("26-1-1", &d) && stui_parse_date("2026-1", &d));
    char buf[64]; stui_format_minutes(buf, sizeof(buf), 29861861); assert(!strcmp(buf, "2026-10-11 09:41"));
    stui_format_minutes(buf, sizeof(buf), 0); assert(!strcmp(buf, "1970-01-01 00:00"));

    /* Empty folder: only the Add Scheduled Task row; file menu items need a selection. */
    struct du_widget *list = du_get(&ui, 1);
    assert(s.mode == STUI_FOLDER && list->count == 1 && !strcmp(cell(0, 0), "Add Scheduled Task") && list->column_count == 5);
    assert(!strcmp(list->columns[0].title, "Name") && !strcmp(list->columns[2].title, "Next Run Time") && !strcmp(list->columns[4].title, "Status"));
    assert(ui.menu_count == 3 && !strcmp(ui.menus[0].label, "&File") && !strcmp(ui.menus[1].label, "&Advanced") && !strcmp(ui.menus[2].label, "&Help"));
    assert(strstr(text_of(2), "0 scheduled tasks"));
    snap("folder-empty");

    /* Daily task through the wizard, Back keeps the entered values. */
    open_wizard();
    assert(du_get(&ui, 10)->disabled && !strcmp(du_get(&ui, 11)->text, "&Next >"));
    snap("wizard-welcome");
    next(); assert(s.program_count == 5 && !strcmp(s.programs[0].label, "Calculator") && !strcmp(s.programs[2].label, "Registry Editor"));
    next(); warned("Select a program");                        /* nothing chosen */
    assert(s.wiz.page == WP_PROGRAM);
    put(22, "calc"); next(); warned("must be absolute");        /* browse text needs a slash */
    put(22, ""); select_program(0);
    snap("wizard-program");
    next(); assert(s.wiz.page == WP_NAME && !strcmp(text_of(24), "Calculator") && !strcmp(s.wiz.command, "/Programs/calc"));
    snap("wizard-name");
    click(10); assert(s.wiz.page == WP_PROGRAM && s.wiz.program == 0);     /* Back */
    next(); assert(s.wiz.page == WP_NAME);
    next(); assert(s.wiz.page == WP_WHEN && !strcmp(text_of(41), "09:00") && !strcmp(text_of(43), "2026-10-11"));
    snap("wizard-when-daily");
    put(41, "25:00"); next(); warned("HH:MM"); assert(s.wiz.page == WP_WHEN);
    put(41, "7:7"); next(); warned("HH:MM");
    put(41, "10:30"); put(43, "2026-02-30"); next(); warned("YYYY-MM-DD");
    put(43, "2026-10-11"); next(); assert(s.wiz.page == WP_FINISH);
    snap("wizard-finish");
    assert(strstr(s.status, "") && !file_exists);                          /* nothing written before Finish */
    next(); assert(s.mode == STUI_FOLDER && file_exists && saves == 1);
    struct st_store st = read_file(); assert(st.count == 1);
    const struct st_task *t = task(&st, "Calculator");
    assert(t && t->kind == ST_DAILY && t->minute_of_day == 630 && !strcmp(t->command, "/Programs/calc") && t->enabled && t->last_run == -1 && t->created == clock_now);
    assert(list->count == 2 && !strcmp(cell(1, 0), "Calculator") && !strcmp(cell(1, 1), "Daily at 10:30") && !strcmp(cell(1, 2), "2026-10-11 10:30") &&
           !strcmp(cell(1, 3), "Never") && !strcmp(cell(1, 4), "Ready") && s.selected == 2 - 1);
    assert(strstr(text_of(2), "Created \"Calculator\""));

    /* Duplicate name, weekly with weekday check boxes. */
    to_name_page(1);                                            /* Clock */
    put(24, "Calculator"); next(); warned("already exists");
    assert(s.wiz.page == WP_NAME);
    put(24, "Clock weekly"); click(31); next(); assert(s.wiz.page == WP_WHEN && s.wiz.kind == ST_WEEKLY);
    assert(du_get(&ui, 50)->value == 1 && du_get(&ui, 54)->value == 0 && !du_get(&ui, 56)->hidden);      /* today's weekday (Sunday) is pre-checked */
    click(50); next(); warned("at least one day");
    click(51); click(53); put(41, "07:15");
    snap("wizard-when-weekly");
    next(); next();
    st = read_file(); t = task(&st, "Clock weekly");
    assert(st.count == 2 && t && t->kind == ST_WEEKLY && t->weekday_mask == ((1u << 1) | (1u << 3)) && t->minute_of_day == 435);
    assert(!strcmp(cell(2, 1), "Weekly on Mon, Wed at 07:15") && !strcmp(cell(2, 2), "2026-10-12 07:15"));

    /* Monthly on day 31 runs on the last day of shorter months. */
    to_name_page(2);                                            /* Registry Editor */
    put(24, "Registry monthly"); click(32); next();
    assert(du_get(&ui, 45) && !du_get(&ui, 50));
    put(41, "00:30"); put(45, "32"); next(); warned("1 to 31");
    put(45, "31"); next(); next();
    st = read_file(); t = task(&st, "Registry monthly");
    assert(t && t->kind == ST_MONTHLY && t->day_of_month == 31 && !strcmp(cell(3, 1), "Monthly on day 31 at 00:30") && !strcmp(cell(3, 2), "2026-10-31 00:30"));
    assert(st_next(t, st_minutes((struct st_date){ 2026, 10, 31 }, 60)) == st_minutes((struct st_date){ 2026, 11, 30 }, 30));

    /* One time only: past date refused. */
    to_name_page(3);                                            /* System Information */
    put(24, "Once task"); click(33); next();
    put(41, "08:00"); put(43, "2026-10-01"); next(); warned("already passed");
    put(41, "09:00"); put(43, "2026-10-11"); next(); warned("already passed");        /* earlier today */
    put(41, "08:00"); put(43, "2026-12-25"); next(); next();
    st = read_file(); t = task(&st, "Once task");
    assert(st.count == 4 && t && t->kind == ST_ONCE && t->start.month == 12 && t->minute_of_day == 480);
    assert(!strcmp(cell(4, 1), "Once on 2026-12-25 at 08:00") && !strcmp(cell(4, 2), "2026-12-25 08:00"));
    snap("folder-tasks");

    /* When my computer starts: no time page; a typed path is the program; advanced properties open at Finish. */
    open_wizard(); next();
    put(22, "/bin/defrag /all"); next(); assert(s.wiz.page == WP_NAME && !strcmp(text_of(24), "defrag"));
    put(24, "Startup tool"); click(34); next();
    assert(s.wiz.page == WP_FINISH);
    click(10); assert(s.wiz.page == WP_NAME); next(); assert(s.wiz.page == WP_FINISH);
    click(60); next();
    assert(s.mode == STUI_PROPS && !strcmp(s.props.original, "Startup tool"));
    st = read_file(); t = task(&st, "Startup tool");
    assert(st.count == 5 && t && t->kind == ST_AT_START && !strcmp(t->command, "/bin/defrag /all"));

    /* Properties: Task, Schedule and Settings tabs; Apply keeps the sheet open, OK closes it. */
    assert(!strcmp(text_of(111), "/bin/defrag /all") && du_get(&ui, 113)->value == 1);
    snap("props-task");
    put(111, "/bin/scandisk /all"); click(113);
    at_chord(&ui, DU_KEY_LCTRL, DU_KEY_TAB); assert(s.props.tab == 1);
    assert(du_get(&ui, 131)->hidden && du_get(&ui, 140)->hidden && du_get(&ui, 125)->value == 1);   /* At startup: no time, no days */
    click(121); assert(s.props.task.kind == ST_DAILY && !du_get(&ui, 131)->hidden);
    put(131, "6pm"); click(103); warned("HH:MM");
    st = read_file(); assert(task(&st, "Startup tool")->kind == ST_AT_START && task(&st, "Startup tool")->enabled);
    put(131, "06:00"); put(133, "2026-10-12");
    click(122); assert(!du_get(&ui, 140)->hidden && du_get(&ui, 135)->hidden);
    click(142); click(146);                                                   /* Tuesday and Saturday */
    snap("props-schedule");
    click(103); assert(s.mode == STUI_PROPS);
    st = read_file(); t = task(&st, "Startup tool");
    assert(t->kind == ST_WEEKLY && t->weekday_mask == ((1u << 2) | (1u << 6)) && t->minute_of_day == 360 && !t->enabled &&
           !strcmp(t->command, "/bin/scandisk /all") && t->start.day == 12);
    assert(strstr(text_of(150), "Next run: Never"));                          /* disabled */
    at_chord(&ui, DU_KEY_LCTRL, DU_KEY_TAB); assert(s.props.tab == 2 && strstr(text_of(160), "Never") && strstr(text_of(183), "One time only"));
    click(164); put(165, "5"); click(166); click(167); click(168); put(169, "30");
    snap("props-settings");
    click(103); st = read_file(); t = task(&st, "Startup tool");
    assert(t->conditions == (ST_IDLE | ST_AC_ONLY | ST_STOP_IDLE | ST_STOP_BATTERY) && t->idle_minutes == 5 && t->idle_deadline == 30);

    at_chord(&ui, DU_KEY_LCTRL, DU_KEY_TAB);                                  /* wraps to Task */
    click(113); click(101);                                                   /* Enabled, OK */
    assert(s.mode == STUI_FOLDER);
    st = read_file(); t = task(&st, "Startup tool");
    assert(t->enabled && t->kind == ST_WEEKLY && !strcmp(cell(5, 1), "Weekly on Tue, Sat at 06:00") && !strcmp(cell(5, 2), "2026-10-13 06:00"));
    /* Cancel discards. */
    select_row(5); at_tap(&ui, DU_KEY_ENTER); assert(s.mode == STUI_PROPS);
    put(111, "/bin/other"); click(102); assert(s.mode == STUI_FOLDER);
    st = read_file(); assert(!strcmp(task(&st, "Startup tool")->command, "/bin/scandisk /all"));

    /* Run: the desktop launcher by catalogue name; the result arrives later. */
    select_row(1); menu_pick(FILE_MENU, 1);
    assert(launches == 1 && !strcmp(launched, "calc") && s.launch_request == 6 && strstr(text_of(2), "Starting"));
    stui_launch_result(&s, 99, 5); assert(s.launch_request == 6);            /* another request: ignored */
    stui_launch_result(&s, 6, 1234);
    st = read_file(); t = task(&st, "Calculator"); assert(t->last_run == -1);
    assert(strstr(text_of(2), "Run requested"));
    /* The service records an executable's actual completion; refresh reads it. */
    st_mark_run(&st, (unsigned)(t - st.t), clock_now, 23); write_store(&st); at_tap(&ui, DU_KEY_F5);
    assert(!strcmp(cell(1, 3), "2026-10-11 09:41") && strstr(cell(1, 4), "23"));
    menu_pick(FILE_MENU, 1); stui_launch_result(&s, 7, -ENOENT);
    expect_dialog("error 2"); dismiss(0);
    assert(!strcmp(cell(1, 4), "Last run failed (error 2)"));
    fail_launch = 1; menu_pick(FILE_MENU, 1); expect_dialog("error 11"); dismiss(0); fail_launch = 0;
    assert(!strcmp(cell(1, 4), "Last run failed (error 11)"));
    select_row(5); menu_pick(FILE_MENU, 1); assert(!strcmp(launched, "/bin/scandisk"));       /* arguments are not passed */
    stui_launch_result(&s, s.launch_request, 55);
    select_row(0); assert(ui.menus[0].items[1].flags & DU_ITEM_DISABLED);                      /* Add row: no Run or Delete */
    select_row(1); assert(!(ui.menus[0].items[1].flags & DU_ITEM_DISABLED));

    /* Context menu: Run, Delete, Properties. */
    launches = 0;
    struct du_rect r1 = du_list_row(du_get(&ui, 1), 1);
    struct ciuki_input_event e = { .type = CIUKI_INPUT_MOTION, .value = r1.x + 40, .value2 = r1.y + 4 }; du_input(&ui, &e);
    e = (struct ciuki_input_event){ .type = CIUKI_INPUT_BUTTON, .code = 2, .value = 1 }; du_input(&ui, &e);
    e.value = 0; du_input(&ui, &e);
    assert(ui.popup && ui.popup_count == 3 && !strcmp(ui.popup[0].label, "&Run") && !strcmp(ui.popup[2].label, "P&roperties"));
    at_tap(&ui, DU_KEY_DOWN); at_tap(&ui, DU_KEY_DOWN); at_tap(&ui, DU_KEY_DOWN); at_tap(&ui, DU_KEY_ENTER);
    assert(s.mode == STUI_PROPS && !strcmp(s.props.original, "Calculator"));
    click(101);

    /* Delete asks first. A task changed by another writer (the scheduler service) in the meantime is kept. */
    st = read_file();
    struct st_task ext; memset(&ext, 0, sizeof(ext));
    snprintf(ext.name, sizeof(ext.name), "External"); snprintf(ext.command, sizeof(ext.command), "/Programs/clock");
    ext.kind = ST_DAILY; ext.start = (struct st_date){ 2026, 10, 11 }; ext.created = clock_now; ext.last_run = -1; ext.minute_of_day = 5;
    assert(!st_add(&st, &ext)); st.t[0].last_run = 12345; st.t[0].last_result = 0; write_store(&st);
    select_row(3); at_tap(&ui, DU_KEY_DELETE);
    expect_dialog("Registry monthly"); dismiss(1);
    st = read_file(); assert(st.count == 6 && task(&st, "Registry monthly"));
    at_tap(&ui, DU_KEY_DELETE); dismiss(0);
    st = read_file(); assert(st.count == 5 && !task(&st, "Registry monthly") && task(&st, "External") && task(&st, "Calculator")->last_run == 12345);
    assert(strstr(text_of(2), "Deleted \"Registry monthly\""));
    stui_refresh(&s);
    assert(list->count == 6 && !strcmp(cell(1, 3), "1970-01-09 13:45"));
    clock_now = st_minutes((struct st_date){ 2026, 10, 11 }, 645);            /* 10:45: the 10:30 run is overdue */
    stui_refresh(&s); assert(!strcmp(cell(1, 2), "Due now"));
    clock_now = 29861861;
    {   /* the periodic tick re-reads the file */
        st = read_file(); st.t[0].last_run = clock_now - 60; write_store(&st);
        stui_event(&ui, &(struct du_event){ .type = DU_EV_TICK, .id = 1 }, &s);
        assert(!strcmp(cell(1, 3), "2026-10-11 08:41"));
    }
    /* View Log lists the last results, newest first. */
    menu_pick(ADVANCED_MENU, 1); expect_dialog("Last results"); expect_dialog("Calculator"); dismiss(0);

    /* Read and write failures stop the edit without touching the file. */
    int before = saves; size_t before_len = file_length; char copy[SS_TEXT]; memcpy(copy, file, file_length);
    select_row(2); at_tap(&ui, DU_KEY_DELETE); fail_load = 1; answer(0);
    expect_dialog("could not be read"); dismiss(0);
    assert(saves == before && file_length == before_len && !memcmp(copy, file, file_length));
    at_tap(&ui, DU_KEY_DELETE); fail_save = 1; answer(0); expect_dialog("could not be saved"); dismiss(0); fail_save = 0;
    assert(file_length == before_len && !memcmp(copy, file, file_length));

    /* Maintenance Wizard: Express, then Custom. */
    menu_pick(ADVANCED_MENU, 0); assert(s.mode == STUI_MAINT && s.maint.page == MP_WELCOME && du_get(&ui, 70)->value == 1);
    snap("maint-welcome");
    next(); assert(s.maint.page == MP_SLOT);
    click(76); snap("maint-slot");
    next(); assert(s.maint.page == MP_FINISH);                                 /* Express skips the tool page */
    snap("maint-finish");
    click(10); assert(s.maint.page == MP_SLOT); next();
    next(); assert(s.mode == STUI_FOLDER); expect_dialog("created 3 scheduled tasks"); expect_dialog("disk tools"); dismiss(0);
    st = read_file();
    t = task(&st, "Disk Defragmenter"); assert(t && t->kind == ST_WEEKLY && t->weekday_mask == 1 && t->minute_of_day == 1200);
    assert(task(&st, "ScanDisk")->minute_of_day == 1230 && task(&st, "Disk Cleanup")->minute_of_day == 1260 && st.count == 8);
    menu_pick(ADVANCED_MENU, 0); click(71); next(); click(75); next();
    assert(s.maint.page == MP_TOOLS); next(); warned("at least one tool"); assert(s.maint.page == MP_TOOLS);
    click(81); next(); assert(s.maint.page == MP_FINISH); click(10); assert(s.maint.page == MP_TOOLS); next(); next();
    expect_dialog("created 1 scheduled task."); dismiss(0);
    st = read_file(); assert(st.count == 8 && task(&st, "ScanDisk")->minute_of_day == 720 && task(&st, "Disk Cleanup")->minute_of_day == 1260);
    menu_pick(ADVANCED_MENU, 0); click(12); assert(s.mode == STUI_FOLDER);        /* Cancel */

    /* A full folder: Finish says so, nothing is written. */
    st_init(&st);
    for (int i = 0; i < ST_MAX; i++) {
        struct st_task f; memset(&f, 0, sizeof(f)); snprintf(f.name, sizeof(f.name), "task%02d", i); snprintf(f.command, sizeof(f.command), "/Programs/calc");
        f.kind = ST_DAILY; f.start = (struct st_date){ 2026, 10, 11 }; f.created = clock_now; f.last_run = -1; assert(!st_add(&st, &f));
    }
    write_store(&st); stui_refresh(&s); assert(list->count == ST_MAX + 1);
    to_name_page(0); put(24, "Overflow"); next(); next(); assert(s.wiz.page == WP_FINISH);
    before_len = file_length; memcpy(copy, file, file_length);
    next(); expect_dialog("is full"); dismiss(0); assert(s.mode == STUI_WIZARD && file_length == before_len && !memcmp(copy, file, file_length));
    click(12); assert(s.mode == STUI_FOLDER);

    /* Esc cancels the wizard; Exit leaves the program. */
    open_wizard(); at_tap(&ui, DU_KEY_ESC); assert(s.mode == STUI_FOLDER);
    select_row(1); menu_pick(FILE_MENU, 4); assert(exits == 1);
    render();
    puts("PASS schedtask: folder, wizard, properties, maintenance wizard, persistence, run/delete, failures");
    return 0;
}
