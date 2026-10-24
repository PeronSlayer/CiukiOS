/* SPDX-License-Identifier: MIT
 * G11 power policy (docs/design/power-policy.md) against apps/desktop/
 * power_policy.c: Windows 98 schemes and timers, desktop.cfg persistence,
 * the power meter summary and tooltip, SYSTEM_POWER_STATUS flags, alarms
 * and the PBT_APMPOWERSTATUSCHANGE rule. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "power_policy.h"

static struct ciuki_power_status record(uint32_t provider, uint32_t ac)
{
    struct ciuki_power_status p;
    memset(&p, 0xff, sizeof(p));
    p.version = CIUKI_POWER_VERSION; p.size = sizeof(p); p.provider = provider; p.ac_online = ac;
    p.battery_count = p.zone_count = 0; p.reserved = 0;
    return p;
}
static void apm_battery(struct ciuki_power_status *p, unsigned i, uint32_t present, uint32_t percent,
                        uint32_t seconds, uint32_t state)
{
    struct ciuki_power_battery *b = &p->batteries[i];
    memset(b, 0xff, sizeof(*b));
    b->id = i + 1; b->present = present;
    if (present == 1) { b->unit = CIUKI_POWER_UNIT_PERCENT; b->full = 100; b->remaining = percent; b->seconds = seconds; b->state = state; }
    if (p->battery_count < i + 1) p->battery_count = i + 1;
}
static void test_schemes(void)
{
    struct pp_config c; pp_defaults(&c);
    assert(c.count == PP_BUILTIN && c.active == -1 && c.show_meter && c.power_button == PP_BUTTON_SHUTDOWN && c.lid == PP_LID_NONE);
    assert(!strcmp(c.schemes[PP_HOME].name, "Home/Office Desk") && !strcmp(c.schemes[PP_PORTABLE].name, "Portable/Laptop") &&
           !strcmp(c.schemes[PP_ALWAYS_ON].name, "Always On"));
    /* Resource Kit relations: Portable/Laptop's AC settings are the
     * desktop's; Always On has no standby. Turn off hard disks is Never
     * in every built-in scheme until ATA standby is qualified (lead). */
    assert(!memcmp(c.schemes[PP_PORTABLE].timer[PP_AC], c.schemes[PP_HOME].timer[PP_AC], sizeof(c.schemes[0].timer[0])));
    for (int i = 0; i < PP_DISKS; i++) assert(c.schemes[PP_PORTABLE].timer[PP_BATTERY][i] < c.schemes[PP_HOME].timer[PP_BATTERY][i]);
    for (int k = 0; k < PP_BUILTIN; k++) assert(!c.schemes[k].timer[PP_AC][PP_DISKS] && !c.schemes[k].timer[PP_BATTERY][PP_DISKS]);
    assert(!c.schemes[PP_ALWAYS_ON].timer[PP_AC][PP_STANDBY] && !c.schemes[PP_ALWAYS_ON].timer[PP_BATTERY][PP_STANDBY]);
    /* Default scheme: battery bay, APM desktop, otherwise. */
    struct pp_summary s;
    struct ciuki_power_status p = record(CIUKI_POWER_PROVIDER_ACPI, 1); pp_summarize(&p, &s);
    assert(pp_active(&c, &s) == PP_HOME);
    p = record(CIUKI_POWER_PROVIDER_APM, 1); pp_summarize(&p, &s); assert(pp_active(&c, &s) == PP_ALWAYS_ON);
    apm_battery(&p, 0, 0, 0, 0, 0); pp_summarize(&p, &s); assert(pp_active(&c, &s) == PP_PORTABLE);
    assert(pp_active(&c, NULL) == PP_HOME);
    c.active = PP_ALWAYS_ON; assert(pp_active(&c, &s) == PP_ALWAYS_ON);
    /* The timer of the current power source. */
    c.active = PP_PORTABLE;
    p = record(CIUKI_POWER_PROVIDER_APM, 1); apm_battery(&p, 0, 1, 80, CIUKI_POWER_UNKNOWN, 2); pp_summarize(&p, &s);
    assert(pp_source(&s) == PP_AC && pp_timer(&c, &s, PP_MONITOR) == 15 * 60 && pp_timer(&c, &s, PP_DISKS) == 0);
    p.ac_online = CIUKI_POWER_AC_BACKUP; p.batteries[0].state = 1; pp_summarize(&p, &s);
    assert(pp_source(&s) == PP_BATTERY && pp_timer(&c, &s, PP_STANDBY) == 5 * 60 && pp_timer(&c, &s, PP_MONITOR) == 120);
    p.ac_online = CIUKI_POWER_UNKNOWN; pp_summarize(&p, &s); assert(pp_source(&s) == PP_BATTERY); /* discharging */
    p.batteries[0].state = CIUKI_POWER_UNKNOWN; pp_summarize(&p, &s); assert(pp_source(&s) == PP_AC); /* unknown counts as AC */
    assert(pp_timer(&c, &s, 7) == 0);
    /* The Windows 98 lists and their labels. */
    char label[32];
    assert(pp_choice_count == 16 && pp_choices[0] == 0 && pp_choices[15] == 5 * 3600);
    pp_choice_label(0, label, sizeof(label)); assert(!strcmp(label, "Never"));
    pp_choice_label(60, label, sizeof(label)); assert(!strcmp(label, "After 1 min"));
    pp_choice_label(25 * 60, label, sizeof(label)); assert(!strcmp(label, "After 25 mins"));
    pp_choice_label(3600, label, sizeof(label)); assert(!strcmp(label, "After 1 hour"));
    pp_choice_label(4 * 3600, label, sizeof(label)); assert(!strcmp(label, "After 4 hours"));
    assert(pp_choice_index(0) == 0 && pp_choice_index(61) == 2 && pp_choice_index(45 * 60) == 10 && pp_choice_index(99999) == 15);
    /* Save As, names, delete. */
    assert(!pp_name_valid(&c, "") && !pp_name_valid(&c, "Always On") && !pp_name_valid(&c, "a,b") && !pp_name_valid(&c, "a=b") &&
           !pp_name_valid(&c, "bad\nname") && !pp_name_valid(&c, "0123456789012345678901234567890123"));
    assert(pp_name_valid(&c, "Presentation"));
    int k = pp_scheme_save_as(&c, "Presentation", PP_HOME);
    assert(k == 3 && c.count == 4 && !memcmp(c.schemes[3].timer, c.schemes[PP_HOME].timer, sizeof(c.schemes[3].timer)));
    assert(pp_scheme_save_as(&c, "Presentation", PP_HOME) == -1 && pp_scheme_save_as(&c, "X", 9) == -1);
    for (int i = 0; i < 7; i++) { char n[8]; snprintf(n, sizeof(n), "S%d", i); assert(pp_scheme_save_as(&c, n, 0) == 4 + i); }
    assert(c.count == PP_SCHEMES_MAX && pp_scheme_save_as(&c, "Full", 0) == -1);
    c.active = 5; assert(!pp_scheme_delete(&c, 4) && c.active == 4 && c.count == PP_SCHEMES_MAX - 1 && !strcmp(c.schemes[4].name, "S1"));
    assert(!pp_scheme_delete(&c, 4) && c.active == -1);
    assert(pp_scheme_delete(&c, PP_HOME) == -1 && pp_scheme_delete(&c, 99) == -1);
    /* Alarm levels: critical below low; actions fall back from Stand by. */
    c.low.level = 5; c.critical.level = 9; pp_alarm_fix(&c); assert(c.critical.level == 4 && c.critical.enabled);
    c.low.level = 0; c.critical.level = 0; pp_alarm_fix(&c); assert(!c.critical.enabled);
    assert(pp_action_effective(PP_ACTION_STANDBY, 0) == PP_ACTION_SHUTDOWN && pp_action_effective(PP_ACTION_STANDBY, 1) == PP_ACTION_STANDBY);
    assert(pp_action_effective(PP_ACTION_NONE, 0) == PP_ACTION_NONE && pp_action_effective(9, 1) == PP_ACTION_NONE);
    puts("PASS power policy: Windows 98 schemes and relations, default scheme, AC/battery timers, timer lists, Save As/Delete, alarm levels, action fallback");
}
static void test_persistence(void)
{
    struct pp_config c, back; pp_defaults(&c);
    c.active = 3; assert(pp_scheme_save_as(&c, "Presentation", PP_PORTABLE) == 3);
    c.schemes[3].timer[PP_BATTERY][PP_MONITOR] = 0; c.schemes[PP_HOME].timer[PP_AC][PP_DISKS] = 3600;
    c.low = (struct pp_alarm){1, 12, 1, 0, PP_ACTION_NONE, 0}; c.critical = (struct pp_alarm){1, 4, 1, 1, PP_ACTION_SHUTDOWN, 1};
    c.show_meter = 0; c.password = 1; c.power_button = PP_BUTTON_STANDBY; c.lid = PP_LID_SHUTDOWN;
    /* Other lines of the block are kept in order; stale power_* lines go. */
    char extra[4096] = "icon=Files,10,20\npower_scheme=1\ntheme_font=x\npower_lid=none\n";
    assert(!pp_store(&c, extra, sizeof(extra)));
    assert(!strncmp(extra, "icon=Files,10,20\ntheme_font=x\npower_scheme=3\n", 44) && extra[strlen(extra) - 1] == '\n');
    assert(strstr(extra, "power_scheme3=Presentation,1200,900,0,300,0,0\n") && strstr(extra, "power_lid=shutdown\n"));
    assert(strstr(extra, "power_alarm_critical=1,4,1,1,2,1\n") && strstr(extra, "power_button=standby\n"));
    assert(!pp_load(&back, extra));
    assert(!memcmp(&back, &c, sizeof(c)));
    /* The block is unchanged when the result does not fit. */
    char small[64] = "icon=Files,10,20\n";
    assert(pp_store(&c, small, sizeof(small)) == -1 && !strcmp(small, "icon=Files,10,20\n"));
    /* Rejected lines keep their defaults. */
    struct pp_config d;
    assert(pp_load(&d, "power_scheme=9\npower_scheme0=Wrong,1,2,3,4,5,6\npower_scheme1=Portable/Laptop,1200,900,1800,300,120,7\n"
                       "power_scheme4=Gap,0,0,0,0,0,0\npower_alarm_low=1,101,0,1,0,0\npower_meter=2\npower_button=hibernate\n"
                       "power_lid\npower_unknown=1\nother=kept\n") == 9);
    pp_defaults(&back); assert(!memcmp(&d, &back, sizeof(d)));
    assert(!pp_load(&d, NULL) && !memcmp(&d, &back, sizeof(d)));
    assert(pp_load(&d, "power_alarm_low=1,10,0,1,0,0\npower_alarm_critical=1,20,0,1,1,0\n") == 0 && d.critical.level == 9);
    puts("PASS power policy: desktop.cfg power_* lines round-trip, other lines kept, overflow leaves the block, rejected lines keep defaults");
}
static void test_summary_and_tooltip(void)
{
    struct pp_summary s; char tip[160];
    pp_summarize(NULL, &s); assert(!s.valid && pp_battery_flag(&s) == 255);
    pp_tooltip(&s, NULL, tip, sizeof(tip)); assert(!strcmp(tip, "Battery status unknown"));
    /* T23 shape (fx-18): APM, unit 1 present, UltraBay empty. */
    struct ciuki_power_status p = record(CIUKI_POWER_PROVIDER_APM, 0);
    apm_battery(&p, 0, 1, 78, 5100, 1); apm_battery(&p, 1, 0, 0, 0, 0);
    pp_summarize(&p, &s);
    assert(s.valid && s.apm && s.on_battery && s.bays == 2 && s.batteries == 1 && s.percent == 78 && s.seconds == 5100);
    pp_tooltip(&s, "Portable/Laptop", tip, sizeof(tip));
    assert(!strcmp(tip, "1 hr 25 min (78%) remaining\nPower scheme: Portable/Laptop"));
    assert(pp_battery_flag(&s) == 1);
    p.batteries[0].seconds = CIUKI_POWER_UNKNOWN; pp_summarize(&p, &s);
    pp_tooltip(&s, NULL, tip, sizeof(tip)); assert(!strcmp(tip, "78% remaining"));
    p.batteries[0].remaining = CIUKI_POWER_UNKNOWN; pp_summarize(&p, &s);
    pp_tooltip(&s, NULL, tip, sizeof(tip)); assert(!strcmp(tip, "On battery power") && pp_battery_flag(&s) == 255);
    /* On AC: charging, full, idle, no facts. */
    p = record(CIUKI_POWER_PROVIDER_APM, 1); apm_battery(&p, 0, 1, 42, 9000, 2); pp_summarize(&p, &s);
    pp_tooltip(&s, NULL, tip, sizeof(tip)); assert(!strcmp(tip, "Charging (42%)") && s.seconds == -1 && pp_battery_flag(&s) == 8);
    p.batteries[0].remaining = 100; p.batteries[0].state = 0; pp_summarize(&p, &s);
    pp_tooltip(&s, NULL, tip, sizeof(tip)); assert(!strcmp(tip, "Fully charged (100%)") && pp_battery_flag(&s) == 1);
    p.batteries[0].remaining = 30; pp_summarize(&p, &s);
    pp_tooltip(&s, NULL, tip, sizeof(tip)); assert(!strcmp(tip, "AC power (30%)") && pp_battery_flag(&s) == 2);
    p.batteries[0].remaining = 4; pp_summarize(&p, &s); assert(pp_battery_flag(&s) == 6);
    p = record(CIUKI_POWER_PROVIDER_ACPI, 1); pp_summarize(&p, &s);
    pp_tooltip(&s, NULL, tip, sizeof(tip)); assert(!strcmp(tip, "AC power") && pp_battery_flag(&s) == 128);
    /* Two batteries: capacity-weighted total, largest time; ACPI time from
     * charge over rate; different units give no total. */
    p = record(CIUKI_POWER_PROVIDER_ACPI, 0); p.battery_count = 2;
    for (unsigned i = 0; i < 2; i++) {
        memset(&p.batteries[i], 0xff, sizeof(p.batteries[i]));
        p.batteries[i] = (struct ciuki_power_battery){.id = i, .present = 1, .unit = 0, .state = 1, .design = CIUKI_POWER_UNKNOWN,
            .full = i ? 20000 : 40000, .remaining = i ? 5000 : 20000, .rate = 10000, .voltage = CIUKI_POWER_UNKNOWN,
            .seconds = CIUKI_POWER_UNKNOWN};
    }
    pp_summarize(&p, &s); assert(s.percent == 41 && s.seconds == 7200 && pp_battery_flag(&s) == 0);
    p.batteries[1].unit = 1; pp_summarize(&p, &s); assert(s.percent == -1 && s.seconds == 7200);
    struct ciuki_power_status bad = p; bad.battery_count = 9; pp_summarize(&bad, &s); assert(!s.valid);
    puts("PASS power policy: meter summary (APM T23 bays, ACPI rate time, mixed units), tooltip texts, SYSTEM_POWER_STATUS battery flag");
}
static void test_alarms(void)
{
    struct pp_config c; pp_defaults(&c);
    struct pp_alarm_state a = {0};
    struct pp_summary s;
    struct ciuki_power_status p = record(CIUKI_POWER_PROVIDER_APM, 1); apm_battery(&p, 0, 1, 8, 600, 2);
    pp_summarize(&p, &s); assert(pp_alarm_step(&a, &c, &s, 0) == PP_ALARM_NONE); /* on AC: never */
    p.ac_online = 0; p.batteries[0].state = 1;
    unsigned percent[] = {50, 11, 10, 9, 10, 12, 10, 3, 2, 50};
    int expect[] = {PP_ALARM_NONE, PP_ALARM_NONE, PP_ALARM_LOW, PP_ALARM_NONE, PP_ALARM_NONE, PP_ALARM_NONE, PP_ALARM_LOW,
                    PP_ALARM_CRITICAL, PP_ALARM_NONE, PP_ALARM_NONE};
    for (unsigned i = 0; i < 10; i++) {
        p.batteries[0].remaining = percent[i]; pp_summarize(&p, &s);
        assert(pp_alarm_step(&a, &c, &s, 0) == expect[i]);
    }
    /* A jump past both levels: only the critical alarm, the low one latched. */
    a = (struct pp_alarm_state){0}; p.batteries[0].remaining = 40; pp_summarize(&p, &s); assert(!pp_alarm_step(&a, &c, &s, 0));
    p.batteries[0].remaining = 2; pp_summarize(&p, &s); assert(pp_alarm_step(&a, &c, &s, 0) == PP_ALARM_CRITICAL);
    p.batteries[0].remaining = 5; pp_summarize(&p, &s); assert(!pp_alarm_step(&a, &c, &s, 0));
    /* AC rearms both. */
    p.ac_online = 1; pp_summarize(&p, &s); assert(!pp_alarm_step(&a, &c, &s, 0));
    p.ac_online = 0; pp_summarize(&p, &s); assert(pp_alarm_step(&a, &c, &s, 0) == PP_ALARM_LOW);
    /* APM Battery Low (0005h) without a percentage, once per event. */
    a = (struct pp_alarm_state){0}; p.batteries[0].remaining = CIUKI_POWER_UNKNOWN; pp_summarize(&p, &s);
    assert(!pp_alarm_step(&a, &c, &s, 4) && pp_alarm_step(&a, &c, &s, 5) == PP_ALARM_LOW && !pp_alarm_step(&a, &c, &s, 5));
    assert(!pp_alarm_step(&a, &c, &s, 6));   /* still latched for this discharge */
    /* Disabled alarms never go off. */
    c.low.enabled = c.critical.enabled = 0; a = (struct pp_alarm_state){0};
    p.batteries[0].remaining = 1; pp_summarize(&p, &s); assert(!pp_alarm_step(&a, &c, &s, 9));
    puts("PASS power policy: alarms on battery only, once per discharge, rearm above the level and on AC, critical wins a jump, APM Battery Low");
}
static void test_broadcast_rule(void)
{
    struct pp_broadcast_state b = {0};
    struct pp_summary s;
    struct ciuki_power_status p = record(CIUKI_POWER_PROVIDER_APM, 1); apm_battery(&p, 0, 1, 80, CIUKI_POWER_UNKNOWN, 2);
    pp_summarize(&p, &s); assert(!pp_status_changed(&b, &s));          /* primes */
    assert(!pp_status_changed(&b, &s));
    p.ac_online = 0; p.batteries[0].state = 1; p.batteries[0].seconds = 4000; pp_summarize(&p, &s);
    assert(pp_status_changed(&b, &s));                                  /* AC and flag */
    p.batteries[0].remaining = 79; pp_summarize(&p, &s); assert(!pp_status_changed(&b, &s));
    p.batteries[0].remaining = 78; pp_summarize(&p, &s); assert(!pp_status_changed(&b, &s));
    p.batteries[0].remaining = 77; pp_summarize(&p, &s); assert(pp_status_changed(&b, &s)); /* 3 % since the last */
    p.batteries[0].remaining = 67; pp_summarize(&p, &s); assert(pp_status_changed(&b, &s));
    p.batteries[0].remaining = 66; pp_summarize(&p, &s); assert(pp_status_changed(&b, &s)); /* flag high -> 0 */
    p.batteries[0].remaining = 11; pp_summarize(&p, &s); assert(pp_status_changed(&b, &s));
    p.batteries[0].remaining = 10; pp_summarize(&p, &s); assert(!pp_status_changed(&b, &s));
    p.batteries[0].remaining = 9; pp_summarize(&p, &s); assert(pp_status_changed(&b, &s));  /* below 10 % */
    p.batteries[0].seconds = 400; pp_summarize(&p, &s); assert(!pp_status_changed(&b, &s));
    p.batteries[0].seconds = 299; pp_summarize(&p, &s); assert(pp_status_changed(&b, &s)); /* below 5 min */
    p.batteries[0].seconds = 200; pp_summarize(&p, &s); assert(!pp_status_changed(&b, &s));
    p.batteries[0].remaining = CIUKI_POWER_UNKNOWN; pp_summarize(&p, &s); assert(pp_status_changed(&b, &s));
    puts("PASS power policy: PBT_APMPOWERSTATUSCHANGE on AC, battery flag, 3 % steps, below 10 %, below 5 minutes, unknown charge");
}
int main(void)
{
    test_schemes();
    test_persistence();
    test_summary_and_tooltip();
    test_alarms();
    test_broadcast_rule();
    return 0;
}
