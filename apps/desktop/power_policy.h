/* SPDX-License-Identifier: MIT
 * G11 power policy (docs/design/power-policy.md): Windows 98 power schemes,
 * battery alarms, power-button and lid choices, the power meter summary and
 * the WM_POWERBROADCAST status-change rule. Pure functions over memory,
 * shared by the desktop shell and the Control Panel. The settings are
 * `power_*` lines of /system/settings/desktop.cfg, kept verbatim by
 * settings.c among its unknown keys (ciuki_settings.extra). */
#ifndef CIUKI_DESKTOP_POWER_POLICY_H
#define CIUKI_DESKTOP_POWER_POLICY_H
#include <stddef.h>
#include <stdint.h>
#include <ciuki/abi.h>

#define PP_BUILTIN 3            /* Home/Office Desk, Portable/Laptop, Always On */
#define PP_SCHEMES_MAX (PP_BUILTIN + 8)
#define PP_NAME_MAX 32          /* 1..31 characters */
#define PP_TIMER_MAX (5u * 3600u)
enum { PP_HOME, PP_PORTABLE, PP_ALWAYS_ON };
enum { PP_AC, PP_BATTERY, PP_SOURCES };
enum { PP_STANDBY, PP_MONITOR, PP_DISKS, PP_TIMERS };
enum { PP_ACTION_NONE, PP_ACTION_STANDBY, PP_ACTION_SHUTDOWN };
enum { PP_BUTTON_SHUTDOWN, PP_BUTTON_STANDBY };
enum { PP_LID_NONE, PP_LID_STANDBY, PP_LID_SHUTDOWN };
enum { PP_ALARM_NONE, PP_ALARM_LOW, PP_ALARM_CRITICAL };
/* Windows 98 WM_POWERBROADCAST (winuser.h) event values. */
#define PP_WM_POWERBROADCAST 0x0218u
#define PP_PBT_QUERYSUSPEND 0u
#define PP_PBT_QUERYSUSPENDFAILED 2u
#define PP_PBT_SUSPEND 4u
#define PP_PBT_RESUMECRITICAL 6u
#define PP_PBT_RESUMESUSPEND 7u
#define PP_PBT_BATTERYLOW 9u
#define PP_PBT_POWERSTATUSCHANGE 10u
#define PP_PBT_RESUMEAUTOMATIC 18u
#define PP_PBTF_RESUMEFROMFAILURE 1u
#define PP_BROADCAST_QUERY_DENY 0x424D5144u

struct pp_scheme {
    char name[PP_NAME_MAX];
    uint32_t timer[PP_SOURCES][PP_TIMERS];   /* seconds; 0 = Never */
};
struct pp_alarm {
    int enabled, level;                       /* level 0..100 % */
    int sound, message, action, force;        /* PP_ACTION_* */
};
struct pp_config {
    struct pp_scheme schemes[PP_SCHEMES_MAX];
    unsigned count;
    int active;                               /* scheme index; -1 until the user chose */
    struct pp_alarm low, critical;
    int show_meter, password, power_button, lid;
};
/* One reading of the call-73 record, as Windows 98's power meter shows it. */
struct pp_summary {
    int valid;
    int ac;                                   /* 0 off line (or APM backup), 1 on line, 2 unknown */
    int on_battery;
    unsigned bays, batteries;                 /* reported bays, installed batteries */
    int percent;                              /* combined charge, -1 unknown */
    int seconds;                              /* remaining time on battery, -1 unknown */
    int charging, critical, apm;
};

extern const uint32_t pp_choices[];           /* the Windows 98 timer list, seconds, Never first */
extern const unsigned pp_choice_count;
void pp_defaults(struct pp_config *);
/* Parses the power_* lines of a settings `extra` block over the defaults;
 * returns the number of rejected lines (they keep their defaults). */
int pp_load(struct pp_config *, const char *extra);
/* Replaces every power_* line of `extra` ("key=value\n" lines, as
 * settings.c keeps them) with the configuration; 0, or -1 when it does not
 * fit in `size` bytes (the block is then unchanged). */
int pp_store(const struct pp_config *, char *extra, size_t size);
/* Active scheme: the user's choice, else Portable/Laptop with a battery bay,
 * Always On on an APM machine without one, Home/Office Desk otherwise. */
int pp_active(const struct pp_config *, const struct pp_summary *);
int pp_source(const struct pp_summary *);    /* PP_BATTERY on battery, else PP_AC */
uint32_t pp_timer(const struct pp_config *, const struct pp_summary *, int which);
int pp_choice_index(uint32_t seconds);       /* nearest list entry not shorter */
void pp_choice_label(uint32_t seconds, char *out, size_t size);
int pp_name_valid(const struct pp_config *, const char *name);
int pp_scheme_save_as(struct pp_config *, const char *name, int from); /* new index or -1 */
int pp_scheme_delete(struct pp_config *, int index);                    /* built-ins: -1 */
/* Critical below low (Resource Kit): critical = low - 1, off with low 0. */
void pp_alarm_fix(struct pp_config *);
/* An action that is not available (standby) falls back to Shut down. */
int pp_action_effective(int action, int standby_available);

void pp_summarize(const struct ciuki_power_status *, struct pp_summary *);
/* Tray tooltip, one or two lines (the scheme name second when given). */
void pp_tooltip(const struct pp_summary *, const char *scheme, char *out, size_t size);
/* SYSTEM_POWER_STATUS.BatteryFlag: 1 high (>66 %), 2 low (<33 %), 4
 * critical (<5 %), 8 charging, 128 no system battery, 255 unknown. */
unsigned pp_battery_flag(const struct pp_summary *);

/* Alarms: only on battery, once per discharge; critical wins a jump past
 * both levels; the APM Battery Low count sets off the low alarm. */
struct pp_alarm_state { int primed, low_latched, critical_latched; uint32_t battery_low_seen; };
int pp_alarm_step(struct pp_alarm_state *, const struct pp_config *, const struct pp_summary *,
                  uint32_t battery_low_count);
/* PBT_APMPOWERSTATUSCHANGE is due: AC or battery flag changed, the time
 * fell below 5 min, the charge fell below 10 % or moved by 3 % or more. */
struct pp_broadcast_state { int primed, ac, flag, percent, seconds; };
int pp_status_changed(struct pp_broadcast_state *, const struct pp_summary *);
#endif
