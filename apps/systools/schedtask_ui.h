/* G30 Scheduled Tasks: folder window, Add Scheduled Task wizard, task properties and Maintenance
 * Wizard on libdeskui. Everything here is testable without a desktop: the task file, the clock, the
 * /Programs listing and the launcher are injected through struct stui_host (docs/design/system-tools.md).
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_SCHEDTASK_UI_H
#define CIUKI_SCHEDTASK_UI_H
#include "systools.h"
#include "../libdeskui/deskui.h"

#define STUI_W 640
#define STUI_H 380
#define STUI_PROGRAMS 48
#define STUI_NAME 32

struct stui_host {
    void *ctx;
    int64_t (*now_minutes)(void *);                                /* local time, minutes since 1970-01-01 */
    int (*load)(void *, char *text, size_t cap, size_t *length);   /* 0 or -errno; an absent file is -ENOENT */
    int (*save)(void *, const char *text, size_t length);          /* atomic replace; 0 or -errno */
    /* Names of the installed programs of /Programs (no path). 0 or -errno. */
    int (*programs)(void *, char names[][STUI_NAME], unsigned cap, unsigned *count);
    /* Asks the desktop to start the catalogue program `program`; returns a request number (> 0) whose result
     * arrives later through stui_launch_result, or -errno. */
    int (*launch)(void *, const char *task, const char *program);
    void (*exit)(void *);                                          /* File > Exit */
};

enum stui_mode { STUI_FOLDER, STUI_WIZARD, STUI_PROPS, STUI_MAINT };
enum stui_wpage { WP_WELCOME, WP_PROGRAM, WP_NAME, WP_WHEN, WP_FINISH, WP_PAGES };
enum stui_mpage { MP_WELCOME, MP_SLOT, MP_TOOLS, MP_FINISH, MP_PAGES };

struct stui_program { char name[STUI_NAME], label[40]; };

/* Add Scheduled Task wizard state; the widgets hold the text of the page being shown. */
struct stui_wizard {
    int page;
    int program;                       /* selected row of the programs list, -1 none */
    char browse[ST_COMMAND];           /* typed absolute path (and arguments); wins over the list */
    char name[STUI_NAME], name_default[STUI_NAME];
    int kind;
    char time[8], date[12], day[4];
    unsigned weekdays;
    int open_advanced;
    char command[ST_COMMAND];          /* command resolved by the program page */
};

struct stui_props {
    int tab;
    char original[STUI_NAME];          /* the stored task being edited, by name */
    struct st_task task;               /* values being edited (applied by OK/Apply) */
    char time[8], date[12], day[4];
};

struct stui_maint { int page; struct mw_choice choice; };

struct stui {
    struct du_ui *ui;
    const struct stui_host *host;
    int mode;
    struct st_store store, trial;      /* the file as last read; scratch for validation */
    char text[SS_TEXT];
    char status[160];
    int selected;                      /* folder row: 0 is Add Scheduled Task, task i is row i + 1; -1 none */
    char selected_name[STUI_NAME];
    int64_t now;
    uint64_t list_hash;
    struct stui_wizard wiz;
    struct stui_props props;
    struct stui_maint maint;
    struct stui_program programs[STUI_PROGRAMS];
    unsigned program_count;
    int launch_request;                /* outstanding request number, 0 none */
    char launch_name[STUI_NAME];       /* the task being run by hand */
    char pending_delete[STUI_NAME];
    int tip_count;
    /* Widget-owned item storage. */
    struct du_item rows[ST_MAX + 1], program_rows[STUI_PROGRAMS];
    char cells[ST_MAX + 1][5][48];
    char program_cells[STUI_PROGRAMS][2][48];
    struct du_menu_item file_items[8], advanced_items[3], help_items[1], popup_items[4];
    struct du_menu menus[3];
    struct du_item tab_items[3];
};

void stui_init(struct stui *, struct du_ui *, const struct stui_host *);
void stui_event(struct du_ui *, const struct du_event *, void *);
/* Opens the properties of the task `name` (folder mode). */
void stui_open_props(struct stui *, const char *name);
/* The result of the DESK_LAUNCH request `request` (> 0 process id, < 0 -errno). */
void stui_launch_result(struct stui *, int request, int result);
/* Re-reads the task file and the clock and refreshes the list. */
void stui_refresh(struct stui *);
/* "2026-10-24 09:00" from minutes since 1970-01-01 (local). */
void stui_format_minutes(char *out, size_t cap, int64_t minutes);
/* "Daily at 09:00", "Weekly on Mon, Wed at 09:00", ... */
void stui_describe(const struct st_task *, char *out, size_t cap);
/* Text of the folder's Next Run Time and Status columns. */
void stui_next_text(const struct st_task *, int64_t now, char *out, size_t cap);
void stui_status_text(const struct st_task *, char *out, size_t cap);
/* The folder as a text list, for Advanced > View Log. */
void stui_log_text(const struct stui *, char *out, size_t cap);
/* Wizard parsing rules (also used by Properties): 0 or -EINVAL. */
int stui_parse_time(const char *text, unsigned *minute_of_day);
int stui_parse_date(const char *text, struct st_date *date);
#endif
