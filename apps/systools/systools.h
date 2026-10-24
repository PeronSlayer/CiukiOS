/* G30 system tools: System Information tree, System Monitor counters,
 * Resource Meter, Device Manager actions, Add New Hardware wizard, Task
 * Scheduler and Maintenance Wizard. Pure models over caller-owned storage
 * and injected data, independent of windows and syscalls (docs/design/system-tools.md).
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_SYSTOOLS_H
#define CIUKI_SYSTOOLS_H
#include <ciuki/abi.h>
#include <stddef.h>
#include <stdint.h>

/* ---- SY-1 System Information ---- */
#define SI_NODES 192
#define SI_LABEL 64
#define SI_VALUE 96
enum si_kind { SI_CATEGORY, SI_ITEM };
struct si_node { int parent; unsigned kind, depth; char label[SI_LABEL], value[SI_VALUE]; };
struct si_input {
    const struct ciuki_devices_info *devices;
    uint64_t memory_total_kib, memory_free_kib;
    unsigned processes, user_free_pct, gdi_free_pct;
    char os_build[48];
};
struct si_tree { struct si_node n[SI_NODES]; unsigned count, conflicts; int overflow; };
void si_build(struct si_tree *t, const struct si_input *in);
/* First node whose label equals `label` (-1 when absent). */
int si_find(const struct si_tree *t, const char *label);
/* Number of children of a node. */
unsigned si_children(const struct si_tree *t, int parent);
/* Plain-text export of the tree (Save as Text File); returns bytes written, 0 on overflow. */
size_t si_export(const struct si_tree *t, char *out, size_t cap);

/* ---- SY-3 Resource Meter ---- */
struct rm_sample { unsigned user_used, user_cap, gdi_used, gdi_cap; };
enum rm_level { RM_OK, RM_LOW, RM_CRITICAL };
struct rm_state { unsigned system_free, user_free, gdi_free; enum rm_level level; };
#define RM_LOW_PCT 30u
#define RM_CRITICAL_PCT 15u
void rm_compute(struct rm_state *out, const struct rm_sample *s);
/* Tray tooltip, as Windows 98's Resource Meter shows the three values. */
void rm_tooltip(char *out, size_t cap, const struct rm_state *s);
/* Win32 GetFreeSystemResources(GFSR_*) value for the three selectors. */
unsigned rm_gfsr(const struct rm_state *s, unsigned selector);

/* ---- SY-2 System Monitor ---- */
enum sm_counter {
    SM_CPU_USAGE, SM_MEM_FREE, SM_MEM_USED, SM_KHEAP, SM_PROCESSES, SM_THREADS,
    SM_DISK_READ, SM_DISK_WRITE, SM_NET_RX, SM_NET_TX, SM_NET_ERRORS, SM_COUNTERS
};
struct sm_raw {                     /* one snapshot; cumulative fields grow */
    uint64_t total_ticks, idle_ticks, disk_read_bytes, disk_write_bytes, net_rx_packets, net_tx_packets, net_errors;
    uint32_t usable_pages, free_pages, kheap_bytes, processes, threads, tick_hz;
    unsigned have;                  /* bit per sm_counter the source can provide */
};
#define SM_HISTORY 100
#define SM_CHART_ITEMS 8
enum sm_chart { SM_LINE, SM_BAR, SM_NUMERIC };
struct sm_item { int counter; uint32_t color; uint64_t scale; uint8_t history[SM_HISTORY]; unsigned count; uint64_t value, peak; };
struct sm_monitor {
    struct sm_item items[SM_CHART_ITEMS];
    unsigned count, interval_ms;
    int chart, logging;
    struct sm_raw last; int have_last;
    unsigned log_rows;
};
const char *sm_name(int counter);
const char *sm_category(int counter);
const char *sm_explain(int counter);
void sm_init(struct sm_monitor *m);
/* Add Item: -EINVAL bad counter, -EEXIST duplicate, -ENOSPC chart full, -ENODEV the source has no such counter. */
int sm_add(struct sm_monitor *m, int counter, const struct sm_raw *source);
int sm_remove(struct sm_monitor *m, int counter);
int sm_edit(struct sm_monitor *m, int counter, uint32_t color, uint64_t scale);
int sm_set_chart(struct sm_monitor *m, int chart);
/* Interval is Options > Chart > Update Interval, 1..300 seconds. */
int sm_set_interval(struct sm_monitor *m, unsigned seconds);
/* Feeds one snapshot, updating every chart item; appends a CSV row to log when logging. */
int sm_sample(struct sm_monitor *m, const struct sm_raw *raw, char *log, size_t cap, size_t *used);
int sm_log_header(struct sm_monitor *m, char *log, size_t cap, size_t *used);
/* Formatted "name: value unit" for the numeric chart. */
void sm_format(const struct sm_monitor *m, unsigned item, char *out, size_t cap);

/* ---- CP-19 Device Manager actions ---- */
#define DC_MAX 32
enum dc_action { DC_DISABLE, DC_ENABLE, DC_REMOVE, DC_UPDATE_DRIVER };
enum dc_status { DC_ACTIVE, DC_DISABLED, DC_REMOVED };
struct dc_entry { uint32_t id; uint8_t disabled, removed; char driver[32]; };
struct dc_store { struct dc_entry e[DC_MAX]; unsigned count, generation; int restart_needed; };
struct dc_result { int error; int needs_restart; int warn; char message[96]; };
void dc_init(struct dc_store *s);
/* Parses the persisted text; unknown lines are ignored, malformed ids reject the line. */
int dc_load(struct dc_store *s, const char *text);
int dc_save(const struct dc_store *s, char *out, size_t cap);
enum dc_status dc_status_of(const struct dc_store *s, uint32_t id);
const char *dc_driver_override(const struct dc_store *s, uint32_t id);
/* `arg` is the driver name for DC_UPDATE_DRIVER. Critical devices refuse Disable and Remove. */
void dc_apply(struct dc_store *s, const struct ciuki_device_record *d, int action, const char *arg, struct dc_result *r);
/* A restart re-detects removed Plug and Play devices; disabled ones stay disabled. */
void dc_restart(struct dc_store *s);
/* Resources a device holds now: none while disabled or removed (frees conflicts). */
unsigned dc_resource_count(const struct dc_store *s, const struct ciuki_device_record *d);
int dc_critical(const struct ciuki_device_record *d);

/* ---- CP-2 Add New Hardware wizard ---- */
#define AW_FOUND_MAX 16
#define AW_CATALOG_MAX 32
enum aw_page { AW_WELCOME, AW_SEARCH, AW_DETECT, AW_FOUND, AW_NONE_FOUND, AW_CLASS, AW_MODEL, AW_FINISH, AW_DONE };
struct aw_model { unsigned class_id; char name[48], driver[32]; };
struct aw_wizard {
    enum aw_page page, history[12]; unsigned depth;
    int automatic;                    /* 1 search, 0 choose from a list */
    uint32_t found[AW_FOUND_MAX]; unsigned found_count;
    unsigned class_id; int model;     /* selection; -1 none */
    const struct aw_model *catalog; unsigned catalog_count;
    struct dc_store *store; const struct ciuki_devices_info *devices;
    char summary[96]; int restart;
};
void aw_init(struct aw_wizard *w, const struct aw_model *catalog, unsigned count,
             struct dc_store *store, const struct ciuki_devices_info *devices);
void aw_choose_search(struct aw_wizard *w, int automatic);
/* Detection scans the retained inventory for devices without a working driver. */
int aw_detect(struct aw_wizard *w);
int aw_select_class(struct aw_wizard *w, unsigned class_id);
/* Models of the chosen class: fills indexes into the catalog; returns count. */
unsigned aw_models(const struct aw_wizard *w, unsigned *out, unsigned cap);
int aw_select_model(struct aw_wizard *w, int catalog_index);
int aw_next(struct aw_wizard *w);
int aw_back(struct aw_wizard *w);
const char *aw_title(enum aw_page page);

/* ---- SY-7 Scheduled Tasks and Maintenance Wizard ---- */
#define ST_MAX 24
#define ST_COMMAND 96
enum st_kind { ST_DAILY, ST_WEEKLY, ST_MONTHLY, ST_ONCE, ST_AT_START };
struct st_date { int year, month, day; };      /* proleptic Gregorian */
struct st_task {
    char name[32], command[ST_COMMAND];
    int kind, enabled;
    unsigned minute_of_day;           /* 0..1439 */
    unsigned weekday_mask;            /* bit 0 Sunday .. bit 6 Saturday */
    unsigned day_of_month;            /* 1..31; clipped to the month's length */
    struct st_date start;             /* first day the schedule is in force */
    int64_t created, last_run;        /* minutes since 1970-01-01; last_run -1 never */
    int last_result;
};
struct st_store { struct st_task t[ST_MAX]; unsigned count; };
void st_init(struct st_store *s);
int64_t st_minutes(struct st_date d, unsigned minute_of_day);   /* since 1970-01-01 00:00 */
int st_weekday(struct st_date d);                                /* 0 Sunday */
/* Next occurrence strictly after `after` (minutes), or -1 (never / at start / finished once). */
int64_t st_next(const struct st_task *t, int64_t after);
/* Tasks to launch at `now`: due occurrences since last_run (a missed one runs once), plus At Start tasks when `startup`. Returns count, indexes in out. */
unsigned st_due(const struct st_store *s, int64_t now, int startup, unsigned *out, unsigned cap);
void st_mark_run(struct st_store *s, unsigned index, int64_t now, int result);
int st_add(struct st_store *s, const struct st_task *t);       /* -EINVAL, -EEXIST name, -ENOSPC; created must be set */
int st_remove(struct st_store *s, const char *name);
int st_save(const struct st_store *s, char *out, size_t cap);
int st_load(struct st_store *s, const char *text);

enum mw_slot { MW_NIGHT, MW_DAY, MW_EVENING };     /* 00:00, 12:00, 20:00 */
enum mw_tool { MW_DEFRAG, MW_SCANDISK, MW_CLEANUP, MW_TOOLS };
struct mw_choice { int express; int slot; unsigned tools; };   /* tools: bit per mw_tool */
const char *mw_tool_name(int tool);
/* Registers one weekly task per chosen tool in the slot, staggered 30 minutes apart; returns count or -errno. */
int mw_apply(struct st_store *s, const struct mw_choice *c, struct st_date start, int64_t now);

/* ---- Sources ---- */
/* Call-70 cumulative KiB counters are 32 bits and wrap; accumulate the modular difference. */
struct sm_source { uint32_t read_kib, write_kib; uint64_t read_total, write_total; int primed; };
/* Fills `out` from a call-70 system record, the sum of process threads and the network frame counters
 * (rx/tx; `have_net` 0 when no adapter). Errors are not counted by the kernel, so SM_NET_ERRORS stays unavailable. */
void sm_raw_from_proc(struct sm_raw *out, struct sm_source *src, const struct ciuki_proc_system *sys,
                      unsigned threads, int have_net, uint64_t rx_frames, uint64_t tx_frames);
/* Desktop-wide meter input: client slots in use of the desktop's table, and physical pages. */
struct rm_desktop { unsigned clients_used, clients_cap; uint32_t free_pages, usable_pages; };
void rm_from_desktop(struct rm_sample *out, const struct rm_desktop *in);

/* ---- CP-19 Hardware Profiles ---- */
#define HP_MAX 8
#define HP_NAME 32
struct hp_profile { char name[HP_NAME]; int portable; struct dc_store devices; };
struct hp_store { struct hp_profile p[HP_MAX]; unsigned count, active, default_index; int wait_seconds; };
#define HP_ORIGINAL "Original Configuration"
void hp_init(struct hp_store *s);
/* Copy: the new profile starts with the source's device settings. -EINVAL name, -EEXIST, -ENOSPC, -ENOENT. */
int hp_copy(struct hp_store *s, unsigned from, const char *name);
int hp_rename(struct hp_store *s, unsigned index, const char *name);
/* The profile in use at this boot, and the last one, cannot be deleted (-EBUSY, -EPERM). */
int hp_delete(struct hp_store *s, unsigned index);
/* Moves a profile in the start-up menu (and with it the default, which is entry 0 unless chosen). */
int hp_move(struct hp_store *s, unsigned index, int delta);
/* Seconds the start-up menu waits for a choice, 0..999; -1 waits indefinitely. */
int hp_set_wait(struct hp_store *s, int seconds);
int hp_set_default(struct hp_store *s, unsigned index);
/* The profile to load: -1 while the menu must keep waiting. `chosen` is -1 or the user's selection. */
int hp_startup(const struct hp_store *s, int chosen, unsigned elapsed_seconds);
int hp_apply(struct hp_store *s, unsigned profile, const struct ciuki_device_record *d, int action, const char *arg, struct dc_result *r);
int hp_save(const struct hp_store *s, char *out, size_t cap);
int hp_load(struct hp_store *s, const char *text);

/* ---- CP-19 Performance ---- */
struct pf_input {
    uint64_t memory_kib; unsigned system_free_pct;
    int pagefile_present; uint64_t pagefile_kib, pagefile_used_kib;
    int accel_active; char accel_name[48];
    unsigned compressed_drives, pccard_sockets;
};
struct pf_status {
    char memory[48], resources[48], file_system[48], virtual_memory[64], graphics[80], disk_compression[48], pc_cards[64], verdict[96];
    int degraded;
};
void pf_summarize(struct pf_status *out, const struct pf_input *in);

/* ---- SY-7 Scheduler service ---- */
#define SS_PATH "/system/settings/schedule.cfg"
#define SS_TEXT 8192
struct ss_host {
    void *ctx;
    int64_t (*now_minutes)(void *);                                   /* local time, minutes since 1970-01-01 */
    int (*load)(void *, char *text, size_t cap, size_t *length);     /* 0 or -errno; absent file is -ENOENT */
    int (*save)(void *, const char *text, size_t length);
    /* Starts one task. >0 pid, <0 -errno. `program` has any /Programs/ prefix removed, `argument` may be empty. */
    int (*launch)(void *, const char *program, const char *argument);
};
struct ss_service {
    struct st_store store;
    char text[SS_TEXT], saved[SS_TEXT];
    size_t saved_length;
    int started, loaded;
    unsigned launched, failed, deferred;
};
/* Splits "/Programs/name arg..." into program and argument; 0, or -EINVAL for empty or oversize pieces. */
int ss_split(const char *command, char *program, size_t program_cap, char *argument, size_t argument_cap);
/* One service step: reloads the file when it changed, launches due tasks (At Start tasks on the first step),
 * records the results and writes the file back when a run was recorded. A launcher answer of -EAGAIN (busy)
 * leaves the task due for the next step. Returns tasks launched, or -errno. */
int ss_poll(struct ss_service *s, const struct ss_host *h);
#endif
