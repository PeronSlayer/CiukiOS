/* SPDX-License-Identifier: MIT
 * G11 Power Management Properties (docs/design/power-policy.md), included
 * by app.c: the Windows 98 tabs Power Schemes, Alarms (with a battery bay),
 * Power Meter and Advanced. The settings are desktop.cfg's power_* lines
 * (apps/desktop/power_policy.c); each change is saved and the desktop
 * reloads it (DESK_SETTINGS_RELOAD), like the other pages. Status comes
 * from the cached call-73 record and EVENTS, once per second. */
enum { PW_TABS = 1850, PW_SCHEME_L, PW_SCHEME, PW_SAVE_AS, PW_DELETE, PW_GROUP, PW_HEAD_AC, PW_HEAD_DC,
       PW_ROW_L0, PW_ROW_L1, PW_ROW_L2, PW_NOTE, PW_SOURCE,
       PW_CELL = 1870, /* + source * PP_TIMERS + timer */
       PW_LOW_GROUP = 1880, PW_LOW, PW_LOW_LEVEL, PW_LOW_VALUE, PW_LOW_SOUND, PW_LOW_MESSAGE, PW_LOW_ACTION_L, PW_LOW_ACTION, PW_LOW_FORCE,
       PW_CRIT_GROUP = 1890, PW_CRIT, PW_CRIT_LEVEL, PW_CRIT_VALUE, PW_CRIT_SOUND, PW_CRIT_MESSAGE, PW_CRIT_ACTION_L, PW_CRIT_ACTION, PW_CRIT_FORCE,
       PW_METER_SOURCE = 1900, PW_METER_TOTAL, PW_DETAILS,
       PW_SHOW_METER = 1905, PW_PASSWORD, PW_PASSWORD_NOTE, PW_BUTTONS_GROUP, PW_BUTTON_L, PW_BUTTON, PW_LID_L, PW_LID, PW_ADV_NOTE,
       PW_SAVE_NAME = 1915, PW_DELETE_CONFIRM, PW_ITEM = 1920 /* popup item k: PW_ITEM + k */ };
#define PW_LINES 16
static int pw_standby(const struct cp_app *a)
{ return a->pw_events_ok && a->pw_events.standby_reason == CIUKI_POWER_STANDBY_AVAILABLE; }
static const char *pw_standby_text(const struct cp_app *a)
{
    if (!a->pw_events_ok) return "Stand by is not available: the kernel does not report it.";
    switch (a->pw_events.standby_reason) {
    case CIUKI_POWER_STANDBY_AVAILABLE: return "This computer can stand by.";
    case CIUKI_POWER_STANDBY_NO_PROVIDER: return "Stand by is not available: no power management firmware is in use.";
    case CIUKI_POWER_STANDBY_NO_FIRMWARE: return "Stand by is not available: the firmware reports no standby state.";
    default: return "Stand by is not available: network, audio or USB drivers cannot resume yet.";
    }
}
/* Read the record and the events; 1 when what the page shows changed shape. */
static int pw_refresh(struct cp_app *a)
{
    struct pp_summary before = a->pw_summary; int standby = pw_standby(a);
    struct ciuki_power_status p;
    if (!a->host.power_status || a->host.power_status(a->host.ctx, &p)) memset(&p, 0, sizeof(p));
    pp_summarize(&p, &a->pw_summary);
    a->pw_lid_known = a->pw_summary.valid && p.lid_open <= 1;
    a->pw_events_ok = a->host.power_events && !a->host.power_events(a->host.ctx, &a->pw_events) &&
                      a->pw_events.version == CIUKI_POWER_EVENTS_VERSION;
    return (before.bays != 0) != (a->pw_summary.bays != 0) || standby != pw_standby(a);
}
/* --- sec-k08 (lead decision U4-6): in a LOGON session the power-button
 * and lid actions are the machine's, read from power.cfg and saved through
 * the SYSTEM setting service; the page shows the machine's values to
 * everyone and the service refuses Standard users and Guest. */
static void pw_machine_load(struct cp_app *a)
{
    if (!a->host.power_machine_load) return;
    char buf[PP_MACHINE_MAX + 1]; size_t n = 0; struct pp_machine m;
    if (a->host.power_machine_load(a->host.ctx, buf, sizeof(buf), &n) || pp_machine_parse(&m, buf, n)) pp_machine_defaults(&m);
    pp_machine_apply(&a->pw_cfg, &m);
}
static void pw_machine_save(struct cp_app *a)
{
    struct pp_machine m; pp_machine_take(&m, &a->pw_cfg);
    char buf[PP_MACHINE_MAX + 1]; size_t n = pp_machine_format(&m, buf, sizeof(buf));
    int e = n ? a->host.power_machine_save(a->host.ctx, buf, n) : -E2BIG;
    if (e) {
        pw_machine_load(a);   /* show what the machine keeps */
        text(a, C_STATUS, e == -EPERM ? "You must be an administrator to change the power buttons." :
                          "The power settings could not be saved.");
        return;
    }
    int r = a->host.apply ? a->host.apply(a->host.ctx, DESK_SETTINGS_RELOAD, 0, 0) : -1;
    text(a, C_STATUS, r ? "Saved. The desktop applies it at its next start." : "Saved and applied.");
}
/* --- end sec-k08 --- */
static void pw_open(struct cp_app *a)
{
    static struct ciuki_settings now;
    if (load(a,&now)) now=a->cfg;
    (void)pp_load(&a->pw_cfg, now.extra);
    /* sec-user-04: the session's own resume preference, and the machine
     * policy fields a Standard user or Guest sees but cannot change. */
    a->secure_resume = desk_secure_pref(now.extra, DESK_SECURE_RESUME);
    a->machine_locked = a->host.self_admin && !a->host.self_admin(a->host.ctx);
    pw_machine_load(a);   /* sec-k08 (U4-6) */
    a->pw_tab = power_meter_requested ? 2 : 0; a->pw_details = 1;
    power_meter_requested = 0;
    (void)pw_refresh(a);
}
static void pw_save(struct cp_app *a)
{
    static struct ciuki_settings now; static char out[CFG_TEXT_MAX];
    if (load(a,&now)) return; /* keep what the desktop wrote meanwhile */
    pp_alarm_fix(&a->pw_cfg);
    size_t n = pp_store(&a->pw_cfg, now.extra, sizeof(now.extra)) ? 0 : cfg_format(&now, out, sizeof(out));
    if (!n || !a->host.save || a->host.save(a->host.ctx, out, n)) { text(a, C_STATUS, "The power settings could not be saved."); return; }
    a->cfg_loaded=1;
    int r = a->host.apply ? a->host.apply(a->host.ctx, DESK_SETTINGS_RELOAD, 0, 0) : -1;
    text(a, C_STATUS, a->host.save_async ? "Saving settings..." : r ? "Saved. The desktop applies it at its next start." : "Saved and applied.");
}
static int pw_active(const struct cp_app *a) { return pp_active(&a->pw_cfg, &a->pw_summary); }
static void pw_choice_text(char *out, size_t size, uint32_t seconds) { pp_choice_label(seconds, out, size); }
static const char *pw_action_name(int action)
{ return action == PP_ACTION_STANDBY ? "Stand by" : action == PP_ACTION_SHUTDOWN ? "Shut down" : "No action"; }
static void pw_lines(struct cp_app *a)
{
    const struct pp_summary *s = &a->pw_summary;
    snprintf(a->pw_text[0], sizeof(a->pw_text[0]), "Power source: %s",
             !s->valid || s->ac == 2 ? (s->on_battery ? "Batteries" : "Unknown") : s->ac ? "AC power" : "Batteries");
    if (!s->bays) snprintf(a->pw_text[1], sizeof(a->pw_text[1]), "No battery is installed.");
    else if (s->percent < 0) snprintf(a->pw_text[1], sizeof(a->pw_text[1]), "Total battery power remaining: Unknown");
    else if (s->seconds >= 0)
        snprintf(a->pw_text[1], sizeof(a->pw_text[1]), "Total battery power remaining: %d%% (%d hr %02d min)%s", s->percent,
                 s->seconds / 3600, s->seconds / 60 % 60, s->charging ? ", charging" : "");
    else snprintf(a->pw_text[1], sizeof(a->pw_text[1]), "Total battery power remaining: %d%%%s", s->percent, s->charging ? ", charging" : "");
    snprintf(a->pw_text[2], sizeof(a->pw_text[2]), "The %s values apply now: the computer runs on %s.",
             pp_source(s) == PP_BATTERY ? "battery" : "plugged-in", pp_source(s) == PP_BATTERY ? "batteries" : "AC power");
}
static void pw_alarm_widgets(struct cp_app *a, const struct pp_alarm *m, int base, int y, const char *title, const char *check)
{
    struct du_widget *w = add(a, DU_GROUP, base, 16, y, CP_WIDTH - 32, 150, title); w->anchors = DU_ANCHOR_GROW_X;
    w = add(a, DU_CHECKBOX, base + 1, 28, y + 16, 500, 22, check); w->value = m->enabled;
    w = slider(a, base + 2, 28, y + 40, 300, 100, 0); w->value = m->level;
    w->disabled = !m->enabled;
    char v[16]; snprintf(v, sizeof(v), "%d%%", m->level);
    add(a, DU_LABEL, base + 3, 340, y + 46, 60, 16, v);
    w = add(a, DU_CHECKBOX, base + 4, 28, y + 68, 150, 22, "&Sound alarm"); w->value = m->sound; w->disabled = !m->enabled;
    w = add(a, DU_CHECKBOX, base + 5, 190, y + 68, 180, 22, "&Display message"); w->value = m->message; w->disabled = !m->enabled;
    add(a, DU_LABEL, base + 6, 28, y + 98, 270, 16, "When the alarm goes off, the computer will:");
    w = add(a, DU_BUTTON, base + 7, 300, y + 94, 150, 24, pw_action_name(pp_action_effective(m->action, pw_standby(a))));
    w->disabled = !m->enabled;
    w = add(a, DU_CHECKBOX, base + 8, 28, y + 122, 500, 22, "&Force stand by or shutdown even if a program stops responding");
    w->value = m->force; w->disabled = !m->enabled || m->action == PP_ACTION_NONE;
}
static void pw_build(struct cp_app *a)
{
    struct du_ui *u = a->ui; struct du_widget *w;
    static const char *const names[] = {"Power Schemes", "Alarms", "Power Meter", "Advanced"};
    a->pw_tab_count = 0;
    for (int k = 0; k < 4; k++) if (k != 1 || a->pw_summary.bays) {
        a->pw_tabs_map[a->pw_tab_count] = k;
        a->pw_tabs[a->pw_tab_count] = (struct du_item){.text = {names[k]}};
        a->pw_tab_count++;
    }
    if (a->pw_tab == 1 && !a->pw_summary.bays) a->pw_tab = 0;
    unsigned active = 0;
    for (int k = 0; k < a->pw_tab_count; k++) if (a->pw_tabs_map[k] == a->pw_tab) active = (unsigned)k;
    w = du_get(u, C_TABS); w->id = PW_TABS; w->items = a->pw_tabs; w->count = (unsigned)a->pw_tab_count; w->active = active;
    pw_lines(a);
    const struct pp_config *c = &a->pw_cfg;
    int scheme = pw_active(a), battery = a->pw_summary.bays != 0, standby = pw_standby(a);
    if (a->pw_tab == 0) {
        add(a, DU_LABEL, PW_SCHEME_L, 24, 44, 200, 16, "Power &schemes:");
        add(a, DU_BUTTON, PW_SCHEME, 24, 64, 300, 24, c->schemes[scheme].name);
        add(a, DU_BUTTON, PW_SAVE_AS, 336, 64, 96, 24, "Sa&ve As...");
        add(a, DU_BUTTON, PW_DELETE, 440, 64, 96, 24, "&Delete")->disabled = scheme < PP_BUILTIN;
        char title[80]; snprintf(title, sizeof(title), "Settings for %s power scheme", c->schemes[scheme].name);
        w = add(a, DU_GROUP, PW_GROUP, 16, 100, CP_WIDTH - 32, 172, title); w->anchors = DU_ANCHOR_GROW_X;
        add(a, DU_LABEL, PW_HEAD_AC, 224, 120, 150, 16, "Plugged in");
        if (battery) add(a, DU_LABEL, PW_HEAD_DC, 384, 120, 150, 16, "Running on batteries");
        static const char *const rows[] = {"System stand&by:", "Turn off &monitor:", "Turn off hard &disks:"};
        for (int t = 0; t < PP_TIMERS; t++) {
            add(a, DU_LABEL, PW_ROW_L0 + t, 32, 146 + t * 32, 180, 16, rows[t]);
            for (int src = 0; src < 1 + battery; src++) {
                pw_choice_text(a->pw_cells[src][t], sizeof(a->pw_cells[src][t]), c->schemes[scheme].timer[src][t]);
                w = add(a, DU_BUTTON, PW_CELL + src * PP_TIMERS + t, 224 + src * 160, 142 + t * 32, 150, 24, a->pw_cells[src][t]);
                w->disabled = t == PP_STANDBY && !standby;   /* sec-k08b: per-user scheme timers */
            }
        }
        add(a, DU_LABEL, PW_NOTE, 32, 244, CP_WIDTH - 64, 16, standby ? "" : pw_standby_text(a));
        add(a, DU_LABEL, PW_SOURCE, 24, 284, CP_WIDTH - 48, 16, a->pw_text[2]);
    } else if (a->pw_tab == 1) {
        pw_alarm_widgets(a, &c->low, PW_LOW_GROUP, 40, "Low battery alarm", "Set off &low battery alarm when power level reaches:");
        pw_alarm_widgets(a, &c->critical, PW_CRIT_GROUP, 196, "Critical battery alarm", "Set off &critical battery alarm when power level reaches:");
    } else if (a->pw_tab == 2) {
        add(a, DU_LABEL, PW_METER_SOURCE, 24, 44, CP_WIDTH - 48, 16, a->pw_text[0]);
        add(a, DU_LABEL, PW_METER_TOTAL, 24, 66, CP_WIDTH - 48, 16, a->pw_text[1]);
        w = add(a, DU_CHECKBOX, PW_DETAILS, 24, 88, 400, 22, "Show &details for each battery"); w->value = a->pw_details;
        unsigned lines = power_read(a);
        for (unsigned i = 0; i < PW_LINES; i++)
            add(a, DU_LABEL, 1800 + (int)i, 24, 118 + (int)i * 14, CP_WIDTH - 48, 14, a->pw_details && i < lines ? a->power_lines[i] : "");
    } else {
        w = add(a, DU_CHECKBOX, PW_SHOW_METER, 24, 44, 400, 22, "Show &power meter on taskbar"); w->value = c->show_meter;
        w = add(a, DU_CHECKBOX, PW_PASSWORD, 24, 70, 500, 22, "Prompt for pass&word when computer goes off stand by");
        w->value = accounts_mode(a) ? a->secure_resume : c->password;   /* sec-user-04: per-user in a LOGON session */
        add(a, DU_LABEL, PW_PASSWORD_NOTE, 48, 94, 480, 16, accounts_mode(a) ? "Uses your account password; locked before the computer stands by." :
                                                                   "Uses the logged-on profile password. FAT files remain shared.");
        w = add(a, DU_GROUP, PW_BUTTONS_GROUP, 16, 116, CP_WIDTH - 32, 124, "Power buttons"); w->anchors = DU_ANCHOR_GROW_X;
        add(a, DU_LABEL, PW_BUTTON_L, 32, 136, 480, 16, "When I press the power &button on my computer:");
        add(a, DU_BUTTON, PW_BUTTON, 32, 156, 200, 24,
            c->power_button == PP_BUTTON_STANDBY && standby ? "Stand by" : "Shut down")->disabled = a->machine_locked;   /* sec-user-04 */
        if (battery || a->pw_lid_known) {
            add(a, DU_LABEL, PW_LID_L, 32, 188, 480, 16, "When I close the &lid of my portable computer:");
            int lid = c->lid == PP_LID_STANDBY && !standby ? PP_LID_SHUTDOWN : c->lid;
            add(a, DU_BUTTON, PW_LID, 32, 208, 200, 24, lid == PP_LID_STANDBY ? "Stand by" : lid == PP_LID_SHUTDOWN ? "Shut down" : "None")->disabled = a->machine_locked;
        }
        add(a, DU_LABEL, PW_ADV_NOTE, 24, 250, CP_WIDTH - 48, 16, a->machine_locked ? "You must be an administrator to change the power buttons." :
                                                                   pw_standby_text(a));   /* sec-user-04 */
        add(a, DU_BUTTON, CP_POWER_ACTION, 24, 300, 160, 24, "Shut Down...")->disabled = !a->host.power;
    }
}
/* A dropdown: the popup under the button that owns it. */
static void pw_popup(struct cp_app *a, int owner, unsigned count)
{
    struct du_widget *w = du_get(a->ui, owner); if (!w) return;
    a->pw_popup_owner = owner;
    du_popup(a->ui, a->pw_menu, count, w->box.x, w->box.y + w->box.h, owner);
}
static unsigned pw_menu_item(struct cp_app *a, unsigned k, const char *label, int value, int selected)
{
    snprintf(a->pw_menu_labels[k], sizeof(a->pw_menu_labels[k]), "%s", label);
    a->pw_menu[k] = (struct du_menu_item){.label = a->pw_menu_labels[k], .id = PW_ITEM + (int)k,
                                          .flags = selected ? DU_ITEM_CHECKED : 0};
    a->pw_menu_value[k] = value;
    return k + 1;
}
static void pw_open_menu(struct cp_app *a, int owner)
{
    const struct pp_config *c = &a->pw_cfg;
    int scheme = pw_active(a), standby = pw_standby(a);
    unsigned n = 0;
    if (owner == PW_SCHEME) for (unsigned k = 0; k < c->count; k++) n = pw_menu_item(a, n, c->schemes[k].name, (int)k, (int)k == scheme);
    else if (owner >= PW_CELL && owner < PW_CELL + PP_SOURCES * PP_TIMERS) {
        uint32_t now = c->schemes[scheme].timer[(owner - PW_CELL) / PP_TIMERS][(owner - PW_CELL) % PP_TIMERS];
        for (unsigned k = 0; k < pp_choice_count; k++) {
            char label[24]; pp_choice_label(pp_choices[k], label, sizeof(label));
            n = pw_menu_item(a, n, label, (int)pp_choices[k], pp_choices[k] == now);
        }
    } else if (owner == PW_LOW_ACTION || owner == PW_CRIT_ACTION) {
        int now = pp_action_effective((owner == PW_LOW_ACTION ? &c->low : &c->critical)->action, standby);
        n = pw_menu_item(a, n, "No action", PP_ACTION_NONE, now == PP_ACTION_NONE);
        if (standby) n = pw_menu_item(a, n, "Stand by", PP_ACTION_STANDBY, now == PP_ACTION_STANDBY);
        n = pw_menu_item(a, n, "Shut down", PP_ACTION_SHUTDOWN, now == PP_ACTION_SHUTDOWN);
    } else if (owner == PW_BUTTON) {
        n = pw_menu_item(a, n, "Shut down", PP_BUTTON_SHUTDOWN, c->power_button != PP_BUTTON_STANDBY || !standby);
        if (standby) n = pw_menu_item(a, n, "Stand by", PP_BUTTON_STANDBY, c->power_button == PP_BUTTON_STANDBY);
    } else if (owner == PW_LID) {
        n = pw_menu_item(a, n, "None", PP_LID_NONE, c->lid == PP_LID_NONE);
        if (standby) n = pw_menu_item(a, n, "Stand by", PP_LID_STANDBY, c->lid == PP_LID_STANDBY);
        n = pw_menu_item(a, n, "Shut down", PP_LID_SHUTDOWN, c->lid == PP_LID_SHUTDOWN || (c->lid == PP_LID_STANDBY && !standby));
    }
    if (n) pw_popup(a, owner, n);
}
static void pw_chosen(struct cp_app *a, int owner, int value)
{
    struct pp_config *c = &a->pw_cfg;
    int scheme = pw_active(a);
    if (owner == PW_SCHEME) c->active = value;
    else if (owner >= PW_CELL && owner < PW_CELL + PP_SOURCES * PP_TIMERS) {
        /* A built-in scheme picked automatically becomes the user's choice. */
        c->active = scheme;
        c->schemes[scheme].timer[(owner - PW_CELL) / PP_TIMERS][(owner - PW_CELL) % PP_TIMERS] = (uint32_t)value;
    } else if (owner == PW_LOW_ACTION) c->low.action = value;
    else if (owner == PW_CRIT_ACTION) c->critical.action = value;
    else if (owner == PW_BUTTON) c->power_button = value;
    else if (owner == PW_LID) c->lid = value;
    else return;
    if ((owner == PW_BUTTON || owner == PW_LID) && a->host.power_machine_save) pw_machine_save(a);   /* sec-k08 */
    else pw_save(a);
    build(a);
}
static int pw_event(struct cp_app *a, const struct du_event *e)
{
    if (a->page != CP_POWER) return 0;
    struct du_ui *u = a->ui;
    struct pp_config *c = &a->pw_cfg;
    if (e->type == DU_EV_ACTIVATE && e->id >= PW_ITEM && e->id < PW_ITEM + (int)(sizeof(a->pw_menu) / sizeof(a->pw_menu[0]))) {
        pw_chosen(a, e->index, a->pw_menu_value[e->id - PW_ITEM]); return 1;
    }
    struct pp_alarm *alarm = e->id > PW_LOW_GROUP && e->id <= PW_LOW_FORCE ? &c->low :
                             e->id > PW_CRIT_GROUP && e->id <= PW_CRIT_FORCE ? &c->critical : NULL;
    int field = alarm ? e->id - (alarm == &c->low ? PW_LOW_GROUP : PW_CRIT_GROUP) : 0;
    switch (e->id) {
    case PW_TABS:
        if (e->type == DU_EV_SELECT && e->index >= 0 && e->index < a->pw_tab_count && a->pw_tabs_map[e->index] != a->pw_tab) {
            a->pw_tab = a->pw_tabs_map[e->index]; build(a); du_focus(u, PW_TABS);
        }
        return 1;
    case PW_SCHEME: case PW_LOW_ACTION: case PW_CRIT_ACTION: case PW_BUTTON: case PW_LID:
        if (e->type == DU_EV_ACTIVATE && !(a->machine_locked && (e->id == PW_BUTTON || e->id == PW_LID))) pw_open_menu(a, e->id);
        return 1;
    case PW_SAVE_AS:
        if (e->type == DU_EV_ACTIVATE) du_input_box(u, PW_SAVE_NAME, "Save Scheme", "Save this power scheme as:", "");
        return 1;
    case PW_SAVE_NAME:
        if (e->type == DU_EV_DIALOG && !e->index && e->text) {
            int k = pp_scheme_save_as(c, e->text, pw_active(a));
            if (k < 0) { text(a, C_STATUS, c->count >= PP_SCHEMES_MAX ? "No more power schemes can be saved." :
                              "Type a new name of 1 to 31 characters, without , or =."); return 1; }
            c->active = k; pw_save(a); build(a);
        }
        return 1;
    case PW_DELETE:
        if (e->type == DU_EV_ACTIVATE && pw_active(a) >= PP_BUILTIN) {
            char prompt[128]; static const char *const buttons[] = {"Yes", "No"};
            snprintf(prompt, sizeof(prompt), "Are you sure you want to delete the '%s' power scheme?", c->schemes[pw_active(a)].name);
            du_message(u, PW_DELETE_CONFIRM, "Confirm Scheme Removal", prompt, DU_DLG_ICON_WARNING, buttons, 2, 1, 1);
        }
        return 1;
    case PW_DELETE_CONFIRM:
        if (e->type == DU_EV_DIALOG && !e->index && !pp_scheme_delete(c, pw_active(a))) { pw_save(a); build(a); }
        return 1;
    case PW_DETAILS:
        if (e->type == DU_EV_CHANGE) { a->pw_details = e->index != 0; build(a); }
        return 1;
    case PW_PASSWORD:
        if (e->type == DU_EV_CHANGE && accounts_mode(a)) {   /* sec-user-04 */
            if (secure_pref_save(a, DESK_SECURE_RESUME, e->index != 0)) text(a, C_STATUS, "The power settings could not be saved.");
            else { a->secure_resume = e->index != 0; text(a, C_STATUS, "Saved and applied."); }
        } else if (e->type == DU_EV_CHANGE) { c->password = e->index != 0; pw_save(a); }
        return 1;
    case PW_SHOW_METER:
        if (e->type == DU_EV_CHANGE) { c->show_meter = e->index != 0; pw_save(a); }
        return 1;
    default: break;
    }
    if (e->id >= PW_CELL && e->id < PW_CELL + PP_SOURCES * PP_TIMERS) {
        if (e->type == DU_EV_ACTIVATE) pw_open_menu(a, e->id);
        return 1;
    }
    if (!alarm) return 0;
    if (field == 2) {   /* the level slider: save when the drag ends */
        if (e->type == DU_EV_CHANGE) {
            struct du_widget *w = du_get(u, e->id);
            int level = w ? w->value : alarm->level;
            if (level >= 0 && level <= 100 && level != alarm->level) {
                struct pp_alarm critical = c->critical;
                alarm->level = level;
                char v[16]; snprintf(v, sizeof(v), "%d%%", level); text(a, e->id + 1, v);
                pw_save(a);   /* pp_alarm_fix keeps critical below low */
                if (memcmp(&critical, &c->critical, sizeof(critical))) build(a);
            }
        }
        return 1;
    }
    if (e->type != DU_EV_CHANGE) return 1;
    if (field == 1) alarm->enabled = e->index != 0;
    else if (field == 4) alarm->sound = e->index != 0;
    else if (field == 5) alarm->message = e->index != 0;
    else if (field == 8) alarm->force = e->index != 0;
    else return 1;
    pw_save(a);
    if (field == 1) build(a);
    return 1;
}
static void pw_tick(struct cp_app *a)
{
    if (pw_refresh(a)) { build(a); return; }
    pw_lines(a);
    if (a->pw_tab == 2) {
        text(a, PW_METER_SOURCE, a->pw_text[0]); text(a, PW_METER_TOTAL, a->pw_text[1]);
        unsigned lines = power_read(a);
        for (unsigned i = 0; i < PW_LINES; i++) text(a, 1800 + (int)i, a->pw_details && i < lines ? a->power_lines[i] : "");
    } else if (a->pw_tab == 0) text(a, PW_SOURCE, a->pw_text[2]);
}
