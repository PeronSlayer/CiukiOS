/* G30: Device Manager actions, Add New Hardware, Performance and Hardware Profiles,
 * included by app.c (docs/design/system-tools.md, "Control Panel part").
 * Windows 98 model (Resource Kit ch. 24): Properties > General has "Disable in this hardware
 * profile", Remove and Update Driver start the Upgrade Device Driver Wizard; hardware profiles
 * select the drivers to load; the Add New Hardware Wizard finds legacy devices.
 * Pure models: apps/systools (devctl.c, profiles.c). SPDX-License-Identifier: MIT */
enum { H_DM_DISABLE = 1700, H_DM_ENABLE, H_DM_REMOVE, H_DM_UPDATE, H_DM_CONFIRM, H_DM_RESULT, H_DM_RESTART,
       H_DM_NAME, H_DM_LIST, H_DM_APPLY, H_DM_OTHER, H_DM_CANCEL, H_DM_TITLE, H_DM_HELP,
       H_AW_TITLE, H_AW_BACK, H_AW_NEXT, H_AW_CANCEL, H_AW_YES, H_AW_NO, H_AW_LIST, H_AW_PROGRESS, H_AW_RESTART,
       H_AW_TIMER, H_AW_SEP, H_AW_DONE,
       H_PF_FILE, H_PF_BTN_FILE, H_PF_GFX, H_PF_VM, H_PF_NOTE, H_PF_VERDICT,
       H_HP_LIST, H_HP_PROPS, H_HP_COPY, H_HP_RENAME, H_HP_DELETE, H_HP_UP, H_HP_DOWN, H_HP_DEFAULT,
       H_HP_WAIT_USER, H_HP_WAIT_TIME, H_HP_SECONDS, H_HP_SECONDS_L, H_HP_APPLY, H_HP_GROUP, H_HP_LABEL,
       H_HP_COPY_DLG, H_HP_RENAME_DLG, H_HP_DELETE_DLG, H_HP_SHEET, H_HP_SHEET_NAME, H_HP_SHEET_PORTABLE, H_HP_GAP,
       H_LAST_NAMED,
       H_LINE0 = 1740, H_PF_BASE = 1760, H_LAST = 1779 };
#define HW_TEXT 24576u
#define H_AW_TICKS 5u
static char hw_text[HW_TEXT];

/* ---- state file ---- */
static void hw_ready(struct cp_app *a)
{ if (!a->hp_ready) { hp_init(&a->hp); a->hp_ready = 1; } }
static struct dc_store *hw_devices(struct cp_app *a)
{ hw_ready(a); return &a->hp.p[a->hp.active < a->hp.count ? a->hp.active : 0].devices; }
static void hw_load_state(struct cp_app *a)
{
    size_t n = 0;
    hp_init(&a->hp); a->hp_ready = 1; a->hp_loaded_note[0] = 0;
    if (!a->host.hw_load) return;
    int r = a->host.hw_load(a->host.ctx, hw_text, sizeof(hw_text) - 1, &n);
    if (!r && n < sizeof(hw_text)) { hw_text[n] = 0; hp_load(&a->hp, hw_text); }
    uint32_t serial = a->host.boot_serial ? a->host.boot_serial(a->host.ctx) : 0;
    if (serial && serial != a->hp.boot_serial) {
        for (unsigned i = 0; i < a->hp.count; i++) dc_restart(&a->hp.p[i].devices);
        a->hp.boot_serial = serial;
    }
    if (a->devices_ok) {
        const char *tag = strstr(a->devices.boot_options, "hwprofile=");
        if (tag && tag[10] >= '0' && tag[10] <= '7' && (unsigned)(tag[10] - '0') < a->hp.count) a->hp.active = (unsigned)(tag[10] - '0');
    }
    if (r && r != -ENOENT) snprintf(a->hp_loaded_note, sizeof(a->hp_loaded_note), "Hardware profile settings could not be read; defaults are in use.");
}
/* 0 or -errno; without a save seam the profiles live in memory only. */
static int hw_persist(struct cp_app *a)
{
    int n = hp_save(&a->hp, hw_text, sizeof(hw_text));
    if (n < 0) return n;
    if (!a->host.hw_save) return 0;
    int r = a->host.hw_save(a->host.ctx, hw_text, (size_t)n);
    return r > 0 ? -EIO : r;
}
static struct hp_store *hw_snapshot(struct cp_app *a)
{ hw_ready(a); struct hp_store *s = malloc(sizeof(*s)); if (s) *s = a->hp; return s; }
/* Saves; on failure restores the snapshot. Frees it. */
static int hw_commit(struct cp_app *a, struct hp_store *before)
{
    int r = hw_persist(a);
    if (r) a->hp = *before;
    free(before);
    return r;
}
static void hw_message(struct cp_app *a, const char *title, const char *body, int icon)
{ du_message(a->ui, H_DM_RESULT, title, body, icon, NULL, 0, 0, 0); }

/* ---- catalogue of drivers: built-in drivers by the classes of the devices they are bound to, plus pack entries ---- */
static void hw_catalog(struct cp_app *a)
{
    a->aw_catalog_count = 0;
    memset(a->aw_from_pack, 0, sizeof(a->aw_from_pack));
    if (a->devices_ok) for (unsigned k = 0; k < a->devices.driver_count && k < CIUKI_DRIVERS_MAX; k++) {
        const struct ciuki_driver_record *dr = &a->devices.drivers[k];
        for (unsigned i = 0; i < a->devices.count; i++) {
            const struct ciuki_device_record *d = &a->devices.devices[i];
            if (!dr->name[0] || strcmp(d->driver, dr->name) || d->class_id >= CIUKI_DEVICE_CLASSES) continue;
            int dup = 0;
            for (unsigned m = 0; m < a->aw_catalog_count; m++)
                if (a->aw_catalog[m].class_id == d->class_id && !strcmp(a->aw_catalog[m].driver, dr->name)) dup = 1;
            if (dup || a->aw_catalog_count >= AW_CATALOG_MAX) continue;
            struct aw_model *m = &a->aw_catalog[a->aw_catalog_count++];
            memset(m, 0, sizeof(*m));
            m->class_id = d->class_id;
            snprintf(m->driver, sizeof(m->driver), "%s", dr->name);
            snprintf(m->name, sizeof(m->name), "%s", d->driver_display_name[0] ? d->driver_display_name : dr->name);
        }
    }
    if (a->host.driver_catalog && a->aw_catalog_count < AW_CATALOG_MAX) {
        unsigned first = a->aw_catalog_count;
        int n = a->host.driver_catalog(a->host.ctx, a->aw_catalog + first, AW_CATALOG_MAX - first);
        if (n < 0 || (unsigned)n > AW_CATALOG_MAX - first) n = 0;
        unsigned kept = 0;
        for (int i = 0; i < n; i++) {
            struct aw_model *m = &a->aw_catalog[first + (unsigned)i], *to = &a->aw_catalog[first + kept];
            m->name[sizeof(m->name) - 1] = 0; m->driver[sizeof(m->driver) - 1] = 0;
            int dup = 0;
            for (unsigned k = 0; k < first + kept; k++)
                if (a->aw_catalog[k].class_id == m->class_id && !strcmp(a->aw_catalog[k].driver, m->driver)) dup = 1;
            if (m->class_id >= CIUKI_DEVICE_CLASSES || !m->driver[0] || dup) continue;
            if (to != m) *to = *m;
            a->aw_from_pack[first + kept] = 1; kept++;
        }
        a->aw_catalog_count = first + kept;
    }
}

/* ---- wrapped text in label slots ---- */
static int hw_lines(struct cp_app *a, int *slot, int x, int y, int width, const char *s)
{
    char line[200]; size_t at = 0;
    while (*s) {
        size_t w = 0;
        while (s[w] && s[w] != ' ' && s[w] != '\n') w++;
        char cand[200];
        int fits = at + w + 1 < sizeof(cand);
        if (fits) { memcpy(cand, line, at); if (at) cand[at] = ' '; memcpy(cand + at + (at ? 1 : 0), s, w); cand[at + (at ? 1 : 0) + w] = 0; }
        if (at && (!fits || du_text_width(cand) > width)) {
            if (*slot < 20) add(a, DU_LABEL, H_LINE0 + (*slot)++, x, y, width, 16, line);
            y += 16; at = 0; continue;
        }
        if (!fits) w = sizeof(cand) - 2;
        if (at) line[at++] = ' ';
        memcpy(line + at, s, w); at += w; line[at] = 0;
        s += w;
        if (*s == '\n') {
            if (*slot < 20) add(a, DU_LABEL, H_LINE0 + (*slot)++, x, y, width, 16, line);
            y += 16; at = 0; s++;
        } else if (*s == ' ') s++;
    }
    if (at) { if (*slot < 20) add(a, DU_LABEL, H_LINE0 + (*slot)++, x, y, width, 16, line); y += 16; }
    return y;
}

/* ---- Device Manager actions ---- */
static const struct ciuki_device_record *dm_device(struct cp_app *a, uint32_t id)
{
    if (!a->devices_ok || !id) return NULL;
    for (unsigned i = 0; i < a->devices.count; i++) if (a->devices.devices[i].id == id) return &a->devices.devices[i];
    return NULL;
}
static void dm_buttons(struct cp_app *a)
{
    static const int ids[4] = {H_DM_DISABLE, H_DM_ENABLE, H_DM_REMOVE, H_DM_UPDATE};
    const struct ciuki_device_record *d = dm_device(a, a->selected_device_id);
    enum dc_status s = d ? dc_status_of(hw_devices(a), d->id) : DC_ACTIVE;
    int absent = d && d->state == CIUKI_DEVICE_ABSENT;
    int off[4] = { !d || absent || s != DC_ACTIVE, !d || s == DC_ACTIVE, !d || absent || s == DC_REMOVED, !d || absent || s == DC_REMOVED };
    for (int k = 0; k < 4; k++) {
        struct du_widget *w = du_get(a->ui, ids[k]);
        if (w && w->disabled != off[k]) { w->disabled = (uint8_t)off[k]; du_invalidate_widget(a->ui, w->id); }
    }
}
static void dm_run(struct cp_app *a, uint32_t id, int action, const char *arg)
{
    const struct ciuki_device_record *d = dm_device(a, id);
    if (!d) { hw_message(a, "Device Manager", "The device is no longer in the inventory. Choose Refresh.", DU_DLG_ICON_ERROR); return; }
    struct hp_store *snap = hw_snapshot(a);
    if (!snap) { hw_message(a, "Device Manager", "Not enough memory.", DU_DLG_ICON_ERROR); return; }
    struct dc_result r;
    hp_apply(&a->hp, a->hp.active, d, action, arg, &r);
    if (!r.error) {
        int w = hw_commit(a, snap);
        if (w) { r.error = w; snprintf(r.message, sizeof(r.message), "The settings could not be saved: %s.", strerror(-w)); r.needs_restart = 0; }
    } else free(snap);
    char body[256];
    snprintf(body, sizeof(body), "%s%s", r.message, !r.error && r.needs_restart ? "\n\nRestart the computer for the change to take effect." : "");
    if (a->page == CP_DEVICES) { cp_tree_build(a); dm_buttons(a); }
    if (!r.error && r.needs_restart && a->host.power) {
        static const char *const buttons[] = {"Restart...", "Later"};
        du_message(a->ui, H_DM_RESTART, "Device Manager", body, DU_DLG_ICON_INFO, buttons, 2, 1, 1);
    } else du_message(a->ui, H_DM_RESULT, "Device Manager", body, r.error ? DU_DLG_ICON_ERROR : r.warn ? DU_DLG_ICON_WARNING : DU_DLG_ICON_INFO, NULL, 0, 0, 0);
    text(a, C_STATUS, r.error ? r.message : "Device settings saved.");
}
static void dm_candidates(struct cp_app *a, const struct ciuki_device_record *d)
{
    a->dm_candidate_count = 0; a->dm_candidate = 0;
    hw_catalog(a);
    for (unsigned i = 0; i < a->aw_catalog_count; i++) if (a->aw_catalog[i].class_id == d->class_id) a->dm_candidates[a->dm_candidate_count++] = i;
}
static void dm_update_open(struct cp_app *a)
{
    const struct ciuki_device_record *d = dm_device(a, a->selected_device_id);
    if (!d) return;
    dm_candidates(a, d);
    if (a->dm_candidate_count) { a->dm_updating = 1; build(a); }
    else du_input_box(a->ui, H_DM_NAME, "Update Driver", "No driver list is available for this device. Driver name:", dc_driver_override(hw_devices(a), d->id) ? dc_driver_override(hw_devices(a), d->id) : "");
}
static void dm_confirm(struct cp_app *a, int action)
{
    const struct ciuki_device_record *d = dm_device(a, a->selected_device_id);
    if (!d) return;
    if (dc_critical(d) || d->state == CIUKI_DEVICE_ABSENT) { dm_run(a, d->id, action, NULL); return; }   /* the model refuses with its own message */
    static const char *const buttons[] = {"Yes", "No"};
    a->dm_pending_id = d->id; a->dm_pending_action = action;
    if (action == DC_DISABLE)
        du_message(a->ui, H_DM_CONFIRM, "Confirm Device Disable", "Disabling this device will cause it to stop functioning. Do you really want to disable it?", DU_DLG_ICON_QUESTION, buttons, 2, 1, 1);
    else
        du_message(a->ui, H_DM_CONFIRM, "Confirm Device Removal", "Warning: you are about to remove this device from your system. It is detected again at the next restart. Do you want to continue?", DU_DLG_ICON_WARNING, buttons, 2, 1, 1);
}
static void dm_update_build(struct cp_app *a)
{
    const struct ciuki_device_record *d = dm_device(a, a->selected_device_id);
    char line[160], resolved[65];
    snprintf(line, sizeof(line), "Update Driver: %s", d ? product(a, d, resolved, sizeof(resolved)) : "device");
    add(a, DU_LABEL, H_DM_TITLE, 24, 44, 504, 16, line);
    add(a, DU_LABEL, H_DM_HELP, 24, 66, 504, 16, "Select the driver to use for this device at the next start:");
    for (unsigned k = 0; k < a->dm_candidate_count; k++) {
        const struct aw_model *m = &a->aw_catalog[a->dm_candidates[k]];
        memset(&a->dm_items[k], 0, sizeof(a->dm_items[k]));
        a->dm_items[k].text[0] = m->name; a->dm_items[k].text[1] = m->driver;
        a->dm_items[k].text[2] = a->aw_from_pack[a->dm_candidates[k]] ? "Driver pack" : "Built-in";
        a->dm_items[k].selected = k == a->dm_candidate;
    }
    struct du_widget *w = add(a, DU_LISTVIEW, H_DM_LIST, 24, 88, 504, 190, "");
    w->items = a->dm_items; w->count = a->dm_candidate_count; w->view = DU_DETAILS; w->inactive_selection = 1;
    w->column_count = 3; w->active = a->dm_candidate;
    w->columns[0] = (struct du_column){"Name", 220, 0}; w->columns[1] = (struct du_column){"Driver", 150, 0}; w->columns[2] = (struct du_column){"Source", 110, 0};
    add(a, DU_BUTTON, H_DM_APPLY, 24, 290, 110, 24, "Update")->def = 1;
    add(a, DU_BUTTON, H_DM_OTHER, 146, 290, 130, 24, "Other...");
    add(a, DU_BUTTON, H_DM_CANCEL, 288, 290, 110, 24, "Cancel");
}

/* ---- Add New Hardware ---- */
static void aw_start(struct cp_app *a)
{
    hw_ready(a);
    hw_catalog(a);
    aw_init(&a->aw, a->aw_catalog, a->aw_catalog_count, hw_devices(a), a->devices_ok ? &a->devices : NULL);
    a->aw_active = 1; a->aw_searching = 0; a->aw_progress = 0;
}
static void aw_stop(struct cp_app *a)
{ if (a->ui) du_timer_stop(a->ui, H_AW_TIMER); a->aw_searching = 0; }
static void aw_build(struct cp_app *a)
{
    struct aw_wizard *wz = &a->aw;
    enum aw_page page = a->aw_searching ? AW_DETECT : wz->page;
    int slot = 0, y;
    struct du_widget *w;
    add(a, DU_LABEL, H_AW_TITLE, 24, 44, 504, 18, aw_title(page));
    y = 72;
    int back = wz->depth > 0 && page != AW_DONE && !a->aw_searching, next = !a->aw_searching;
    const char *next_label = "Next >";
    switch (page) {
    case AW_WELCOME:
        y = hw_lines(a, &slot, 24, y, 504, "This wizard helps you install a driver for hardware that is not working yet.");
        y = hw_lines(a, &slot, 24, y + 8, 504, "The system can search for devices that have no working driver, or you can choose the hardware from a list.");
        hw_lines(a, &slot, 24, y + 8, 504, "To continue, click Next.");
        break;
    case AW_SEARCH:
        y = hw_lines(a, &slot, 24, y, 504, "When you click Next, the system looks for devices that have no working driver. This can take a moment.");
        w = add(a, DU_RADIO, H_AW_YES, 40, y + 12, 440, 22, "&Yes (Recommended)"); w->total = 91; w->value = wz->automatic;
        w = add(a, DU_RADIO, H_AW_NO, 40, y + 38, 440, 22, "&No, I want to select the hardware from a list"); w->total = 91; w->value = !wz->automatic;
        break;
    case AW_DETECT:
        y = hw_lines(a, &slot, 24, y, 504, "The system is searching for new hardware. Please wait...");
        w = add(a, DU_PROGRESS, H_AW_PROGRESS, 24, y + 16, 504, 18, ""); w->total = (int)H_AW_TICKS; w->value = (int)a->aw_progress;
        break;
    case AW_FOUND:
        y = hw_lines(a, &slot, 24, y, 504, "The system found the following devices that have no working driver:");
        for (unsigned k = 0; k < wz->found_count; k++) {
            const struct ciuki_device_record *d = dm_device(a, wz->found[k]);
            char resolved[65];
            snprintf(a->aw_names[k], sizeof(a->aw_names[k]), "%s", d ? product(a, d, resolved, sizeof(resolved)) : "Unknown device");
            memset(&a->aw_items[k], 0, sizeof(a->aw_items[k]));
            a->aw_items[k].text[0] = a->aw_names[k]; a->aw_items[k].text[1] = d ? cp_device_class(d->class_id) : "";
        }
        w = add(a, DU_LISTVIEW, H_AW_LIST, 24, y + 6, 504, 150, "");
        w->items = a->aw_items; w->count = wz->found_count; w->view = DU_DETAILS; w->inactive_selection = 1; w->column_count = 2;
        w->columns[0] = (struct du_column){"Device", 340, 0}; w->columns[1] = (struct du_column){"Type", 140, 0};
        hw_lines(a, &slot, 24, y + 164, 504, "Click Next to choose a driver for the first device.");
        break;
    case AW_NONE_FOUND:
        y = hw_lines(a, &slot, 24, y, 504, "The system did not find any new hardware that needs a driver.");
        hw_lines(a, &slot, 24, y + 8, 504, "You can still choose the hardware from a list. Click Next to see the hardware types.");
        break;
    case AW_CLASS:
        y = hw_lines(a, &slot, 24, y, 504, "Select the type of hardware you want to install:");
        for (unsigned k = 0; k < CIUKI_DEVICE_CLASSES; k++) {
            memset(&a->aw_items[k], 0, sizeof(a->aw_items[k]));
            a->aw_items[k].text[0] = cp_device_class(k);
            a->aw_items[k].icon16 = du_icon_argb(du_icon_name(class_icon(k)), 16);
            a->aw_items[k].selected = k == wz->class_id;
        }
        w = add(a, DU_LISTVIEW, H_AW_LIST, 24, y + 6, 504, 190, "");
        w->items = a->aw_items; w->count = CIUKI_DEVICE_CLASSES; list_style(w, "Hardware types"); w->active = wz->class_id < CIUKI_DEVICE_CLASSES ? wz->class_id : 0;
        w->columns[0].width = 480;
        next = wz->class_id < CIUKI_DEVICE_CLASSES;
        break;
    case AW_MODEL: {
        a->aw_list_count = aw_models(wz, a->aw_list, AW_CATALOG_MAX);
        char line[128];
        snprintf(line, sizeof(line), "Select the driver for this %s device:", wz->class_id < CIUKI_DEVICE_CLASSES ? cp_device_class(wz->class_id) : "hardware");
        y = hw_lines(a, &slot, 24, y, 504, line);
        for (unsigned k = 0; k < a->aw_list_count; k++) {
            const struct aw_model *m = &wz->catalog[a->aw_list[k]];
            memset(&a->aw_items[k], 0, sizeof(a->aw_items[k]));
            a->aw_items[k].text[0] = m->name; a->aw_items[k].text[1] = m->driver;
            a->aw_items[k].text[2] = a->aw_from_pack[a->aw_list[k]] ? "Driver pack" : "Built-in";
            a->aw_items[k].selected = (int)a->aw_list[k] == wz->model;
        }
        w = add(a, DU_LISTVIEW, H_AW_LIST, 24, y + 6, 504, 150, "");
        w->items = a->aw_items; w->count = a->aw_list_count; w->view = DU_DETAILS; w->inactive_selection = 1; w->column_count = 3;
        w->columns[0] = (struct du_column){"Name", 220, 0}; w->columns[1] = (struct du_column){"Driver", 150, 0}; w->columns[2] = (struct du_column){"Source", 110, 0};
        for (unsigned k = 0; k < a->aw_list_count; k++) if ((int)a->aw_list[k] == wz->model) w->active = k;
        if (!a->aw_list_count)
            hw_lines(a, &slot, 24, y + 164, 504, "No driver is available for this type of hardware. Install a driver package first (Device Manager, Install driver from pack).");
        next = wz->model >= 0;
        break;
    }
    case AW_FINISH: {
        char line[200];
        const struct aw_model *m = wz->model >= 0 ? &wz->catalog[wz->model] : NULL;
        y = hw_lines(a, &slot, 24, y, 504, "The system is ready to record the driver for your hardware.");
        snprintf(line, sizeof(line), "Driver: %s (%s)", m ? m->name : "-", m ? m->driver : "-");
        y = hw_lines(a, &slot, 24, y + 8, 504, line);
        hw_lines(a, &slot, 24, y + 8, 504, "To record the driver, click Finish. You must restart the computer for it to take effect.");
        next_label = "Finish";
        break;
    }
    case AW_DONE:
        y = hw_lines(a, &slot, 24, y, 504, wz->summary);
        if (wz->restart) hw_lines(a, &slot, 24, y + 8, 504, "Restart the computer for the change to take effect.");
        next_label = "Finish";
        break;
    }
    add(a, DU_SEPARATOR, H_AW_SEP, 24, 316, 504, 2, "")->anchors = DU_ANCHOR_BOTTOM;
    w = add(a, DU_BUTTON, H_AW_BACK, 268, 326, 84, 24, "< Back"); w->anchors = DU_ANCHOR_RIGHT | DU_ANCHOR_BOTTOM; w->disabled = !back;
    w = add(a, DU_BUTTON, H_AW_NEXT, 360, 326, 84, 24, next_label); w->anchors = DU_ANCHOR_RIGHT | DU_ANCHOR_BOTTOM; w->disabled = !next; w->def = 1;
    if (page == AW_DONE) {
        w = add(a, DU_BUTTON, H_AW_RESTART, 160, 326, 100, 24, "Restart..."); w->anchors = DU_ANCHOR_RIGHT | DU_ANCHOR_BOTTOM;
        w->disabled = !wz->restart || !a->host.power;
    } else {
        w = add(a, DU_BUTTON, H_AW_CANCEL, 452, 326, 76, 24, "Cancel"); w->anchors = DU_ANCHOR_RIGHT | DU_ANCHOR_BOTTOM;
    }
}
static void aw_search_done(struct cp_app *a)
{
    du_timer_stop(a->ui, H_AW_TIMER);
    a->aw_searching = 0;
    refresh(a);
    hw_catalog(a);
    a->aw.catalog = a->aw_catalog; a->aw.catalog_count = a->aw_catalog_count;
    a->aw.devices = a->devices_ok ? &a->devices : NULL;
    aw_next(&a->aw);                  /* detects and moves to the found or the none-found page */
    build(a);
    du_focus(a->ui, H_AW_NEXT);
}
static void aw_next_click(struct cp_app *a)
{
    struct aw_wizard *wz = &a->aw;
    if (a->aw_searching) return;
    if (wz->page == AW_SEARCH && wz->automatic) {
        a->aw_searching = 1; a->aw_progress = 0;
        du_timer_start(a->ui, H_AW_TIMER, 150000000ull, 1);
        build(a);
        return;
    }
    if (wz->page == AW_DONE) { cp_open(a, CP_HOME); return; }
    if (wz->page == AW_FINISH) {
        struct hp_store *snap = hw_snapshot(a);
        if (!snap) { hw_message(a, "Add New Hardware", "Not enough memory.", DU_DLG_ICON_ERROR); return; }
        int e = aw_next(wz);
        if (e) {
            free(snap);
            hw_message(a, "Add New Hardware", wz->summary[0] ? wz->summary : "The driver could not be recorded.", DU_DLG_ICON_ERROR);
            return;
        }
        int w = hw_commit(a, snap);
        if (w) {
            wz->page = AW_FINISH;
            snprintf(wz->summary, sizeof(wz->summary), "The settings could not be saved: %s.", strerror(-w));
            build(a);
            hw_message(a, "Add New Hardware", wz->summary, DU_DLG_ICON_ERROR);
            return;
        }
        build(a);
        text(a, C_STATUS, "Driver recorded. Restart to take effect.");
        return;
    }
    if (aw_next(wz)) return;
    build(a);
    du_focus(a->ui, H_AW_NEXT);
}
static int aw_event(struct cp_app *a, const struct du_event *e)
{
    struct aw_wizard *wz = &a->aw;
    switch (e->id) {
    case H_AW_YES: case H_AW_NO:
        if (e->type == DU_EV_CHANGE && e->index) aw_choose_search(wz, e->id == H_AW_YES);
        return 1;
    case H_AW_NEXT: if (e->type == DU_EV_ACTIVATE) aw_next_click(a); return 1;
    case H_AW_BACK:
        if (e->type == DU_EV_ACTIVATE && !a->aw_searching && !aw_back(wz)) build(a);
        return 1;
    case H_AW_CANCEL: if (e->type == DU_EV_ACTIVATE) { aw_stop(a); cp_open(a, CP_HOME); } return 1;
    case H_AW_RESTART:
        if (e->type == DU_EV_ACTIVATE && a->host.power && a->host.power(a->host.ctx)) text(a, C_STATUS, "The shutdown dialog could not be opened.");
        return 1;
    case H_AW_LIST:
        if (e->type == DU_EV_SELECT || e->type == DU_EV_OPEN) {
            if (e->index < 0) return 1;
            if (wz->page == AW_CLASS && aw_select_class(wz, (unsigned)e->index) == 0) {
                for (unsigned k = 0; k < CIUKI_DEVICE_CLASSES; k++) a->aw_items[k].selected = k == (unsigned)e->index;
                du_get(a->ui, H_AW_NEXT)->disabled = 0; du_invalidate_widget(a->ui, H_AW_NEXT);
                if (e->type == DU_EV_OPEN) aw_next_click(a);
            } else if (wz->page == AW_MODEL && (unsigned)e->index < a->aw_list_count &&
                       aw_select_model(wz, (int)a->aw_list[e->index]) == 0) {
                for (unsigned k = 0; k < a->aw_list_count; k++) a->aw_items[k].selected = k == (unsigned)e->index;
                du_get(a->ui, H_AW_NEXT)->disabled = 0; du_invalidate_widget(a->ui, H_AW_NEXT);
                if (e->type == DU_EV_OPEN) aw_next_click(a);
            }
        }
        return 1;
    default: return 0;
    }
}

/* ---- Performance ---- */
static const struct du_item perf_pages[] = {{.text = {"Settings"}}};
static int perf_accept(struct du_ui *u, int button, void *ctx)
{
    struct cp_app *a = ctx;
    if (button == 1) return 0;
    struct system_perf p = a->perf_settings;
    struct du_dialog *d = &u->dialog;
    if (a->perf_dialog == H_PF_BTN_FILE) {
        for (unsigned i = 0; i < 3; i++) if (d->items[4 + i].value) p.role = i;
        p.read_ahead = (unsigned)d->items[7].value;
    } else if (a->perf_dialog == H_PF_GFX) p.graphics = !!d->items[4].value;
    else {
        for (unsigned i = 0; i < 3; i++) if (d->items[4 + i].value) p.vm = i;
        uint32_t min, max;
        if (!system_uint(d->items[7].text, strlen(d->items[7].text), 10, &min) ||
            !system_uint(d->items[8].text, strlen(d->items[8].text), 10, &max)) {
            snprintf(d->text, sizeof(d->text), "Enter minimum and maximum sizes in MiB (multiples of 16)."); return 1;
        }
        p.min_mb = min; p.max_mb = max;
    }
    if (!system_perf_valid(&p)) { snprintf(d->text, sizeof(d->text), "Sizes must be multiples of 16 MiB, from 16 to 1024, minimum <= maximum."); return 1; }
    int e = a->host.perf_save ? a->host.perf_save(a->host.ctx, &p) : -ENOSYS;
    if (e) { snprintf(d->text, sizeof(d->text), "Cannot save performance settings (error %d).", e < 0 ? -e : e); return 1; }
    a->perf_settings = p;
    text(a, H_PF_NOTE, "Settings saved. Restart the computer for the changes to take effect.");
    snprintf(d->text, sizeof(d->text), "Settings saved. Restart the computer to apply them.");
    return button == 2;
}
static void perf_open(struct cp_app *a, int kind)
{
    a->perf_dialog = kind;
    const char *title = kind == H_PF_BTN_FILE ? "File System Properties" : kind == H_PF_GFX ? "Graphics" : "Virtual Memory";
    du_property_sheet(a->ui, kind, title, 450, 300, perf_pages, 1);
    struct du_dialog *d = &a->ui->dialog;
    d->accept = perf_accept; d->accept_ctx = a;
    const struct system_perf *p = &a->perf_settings;
    if (kind == H_PF_BTN_FILE) {
        snprintf(d->text, sizeof(d->text), "Typical role of this computer; read-ahead: None, 16, 32 or 64 KiB.");
        const char *names[] = {"Desktop computer", "Mobile or docking system", "Network server"};
        for (unsigned i = 0; i < 3; i++) {
            struct du_widget *w = du_property_control(a->ui, 4 + i, DU_RADIO, 1780 + (int)i, (struct du_rect){24, 74 + (int)i * 28, 360, 22}, names[i]);
            if (w) { w->total = 1780; w->value = p->role == i; }
        }
        struct du_widget *w = du_property_control(a->ui, 7, DU_TRACKBAR, 1783, (struct du_rect){24, 180, 360, 26}, "Read-ahead");
        if (w) { w->lo = 0; w->hi = 3; w->value = (int)p->read_ahead; }
    } else if (kind == H_PF_GFX) {
        snprintf(d->text, sizeof(d->text), "Full enables the qualified driver. None uses software drawing.\nIntermediate Windows driver levels are not supported.");
        struct du_widget *w = du_property_control(a->ui, 4, DU_CHECKBOX, 1784, (struct du_rect){24, 110, 380, 24}, "Full hardware acceleration (unchecked: None)");
        if (w) w->value = (int)p->graphics;
    } else {
        snprintf(d->text, sizeof(d->text), "Backing file: C:/SYSTEM/PAGEFILE.SWP. Restart required.\nDisabling virtual memory reduces available program memory.");
        const char *names[] = {"Let the system manage virtual memory", "Let me specify my own settings", "Disable virtual memory"};
        for (unsigned i = 0; i < 3; i++) {
            struct du_widget *w = du_property_control(a->ui, 4 + i, DU_RADIO, 1785 + (int)i, (struct du_rect){24, 84 + (int)i * 28, 390, 22}, names[i]);
            if (w) { w->total = 1785; w->value = p->vm == i; }
        }
        char min[16], max[16]; snprintf(min, sizeof(min), "%u", p->min_mb); snprintf(max, sizeof(max), "%u", p->max_mb);
        du_property_control(a->ui, 7, DU_EDIT, 1788, (struct du_rect){24, 200, 90, 23}, min);
        du_property_control(a->ui, 8, DU_EDIT, 1789, (struct du_rect){180, 200, 90, 23}, max);
        du_property_control(a->ui, 9, DU_LABEL, 1790, (struct du_rect){24, 176, 390, 18}, "Minimum (MiB)          Maximum (MiB)");
    }
}

static void perf_build(struct cp_app *a)
{
    snprintf(a->status, sizeof(a->status), "Performance settings apply after restarting the computer.");
    if (!a->perf_ready) {
        a->perf_settings = system_perf_default(); a->perf_ready = 1;
        if (a->host.perf_load) (void)a->host.perf_load(a->host.ctx, &a->perf_settings);
    }
    struct pf_input in;
    uint64_t total = a->host.mem_total_k, freek = a->host.mem_free_k;
    if (a->host.memory) a->host.memory(a->host.ctx, &total, &freek);
    memset(&in, 0, sizeof(in));
    in.memory_kib = total;
    in.system_free_pct = total ? (unsigned)(freek * 100 / total) : 100;
    in.pagefile_present = 1;              /* virtual memory has no query: its line is replaced below */
    const struct ciuki_device_record *acc = NULL;
    for (unsigned i = 0; a->devices_ok && i < a->devices.count; i++) {
        const struct ciuki_device_record *d = &a->devices.devices[i];
        if (d->id == CIUKI_DEVICE_ACCELERATOR_ID) acc = d;
        if (d->bus_type == CIUKI_BUS_PCI && d->pci_class == 6 && d->subclass == 7) in.pccard_sockets++;   /* CardBus bridge */
    }
    struct ciuki_display_engine_info eng = {0};
    int have_eng = a->host.engine_info && !a->host.engine_info(a->host.ctx, &eng);
    if (acc) {
        in.accel_active = acc->accel_ready && acc->state == CIUKI_DEVICE_READY;
        snprintf(in.accel_name, sizeof(in.accel_name), "%s", acc->driver_display_name);
    } else if (have_eng) {
        in.accel_active = eng.kind == CIUKI_ENGINE_NATIVE && eng.ready && !eng.quarantined;
        snprintf(in.accel_name, sizeof(in.accel_name), "%s", eng.display_name);
    }
    pf_summarize(&a->perf, &in);
    if (a->perf_settings.vm == 2) snprintf(a->perf.virtual_memory, sizeof(a->perf.virtual_memory), "Configured: virtual memory disabled");
    else if (a->perf_settings.vm == 1) snprintf(a->perf.virtual_memory, sizeof(a->perf.virtual_memory), "Configured: %u..%u MiB", a->perf_settings.min_mb, a->perf_settings.max_mb);
    else snprintf(a->perf.virtual_memory, sizeof(a->perf.virtual_memory), "Managed automatically by the system");
    if (!total) snprintf(a->perf.resources, sizeof(a->perf.resources), "Unavailable");
    static const char *const names[7] = {"Memory:", "System Resources:", "File System:", "Virtual Memory:", "Disk Compression:", "PC Cards (PCMCIA):", "Graphics:"};
    const char *values[7] = {a->perf.memory, a->perf.resources, a->perf.file_system, a->perf.virtual_memory, a->perf.disk_compression, a->perf.pc_cards, a->perf.graphics};
    add(a, DU_LABEL, H_PF_FILE, 24, 44, 504, 16, "Performance status");
    for (int k = 0; k < 7; k++) {
        add(a, DU_LABEL, H_PF_BASE + k, 24, 68 + k * 20, 140, 16, names[k]);
        add(a, DU_LABEL, H_PF_BASE + 10 + k, 168, 68 + k * 20, 360, 16, values[k]);
    }
    add(a, DU_LABEL, H_PF_VERDICT, 24, 220, 504, 16, a->perf.verdict);
    add(a, DU_BUTTON, H_PF_BTN_FILE, 24, 252, 120, 24, "File System...");
    add(a, DU_BUTTON, H_PF_GFX, 152, 252, 120, 24, "Graphics...");
    add(a, DU_BUTTON, H_PF_VM, 280, 252, 140, 24, "Virtual Memory...");
    add(a, DU_LABEL, H_PF_NOTE, 24, 286, 504, 16, "Changes to these settings take effect after restarting the computer.");
}

/* ---- Hardware Profiles ---- */
static void hp_items(struct cp_app *a)
{
    struct hp_store *s = &a->hp;
    for (unsigned i = 0; i < s->count; i++) {
        char *n = a->profile_notes[i]; n[0] = 0;
        if (i == s->active) strcat(n, "Current");
        if (i == s->default_index) strcat(n, n[0] ? ", Default" : "Default");
        if (s->p[i].portable) strcat(n, n[0] ? ", Portable" : "Portable");
        memset(&a->profile_items[i], 0, sizeof(a->profile_items[i]));
        a->profile_items[i].text[0] = s->p[i].name; a->profile_items[i].text[1] = n;
        a->profile_items[i].selected = i == a->hp_selected;
    }
}
static void hp_sync(struct cp_app *a)
{
    if (a->hp_selected >= a->hp.count) a->hp_selected = a->hp.count ? a->hp.count - 1 : 0;
    hp_items(a);
    struct du_widget *w = du_get(a->ui, H_HP_LIST);
    if (w) { w->count = a->hp.count; w->active = a->hp_selected; du_invalidate_widget(a->ui, H_HP_LIST); }
}
static void hp_build(struct cp_app *a)
{
    hw_ready(a);
    if (a->hp_selected >= a->hp.count) a->hp_selected = 0;
    a->hp_wait_mode = a->hp.wait_seconds >= 0;
    snprintf(a->hp_wait_text, sizeof(a->hp_wait_text), "%d", a->hp.wait_seconds >= 0 ? a->hp.wait_seconds : 30);
    add(a, DU_LABEL, H_HP_LABEL, 24, 44, 380, 16, "Available hardware profiles:");
    hp_items(a);
    struct du_widget *w = add(a, DU_LISTVIEW, H_HP_LIST, 24, 62, 372, 108, "");
    w->items = a->profile_items; w->count = a->hp.count; w->view = DU_DETAILS; w->inactive_selection = 1; w->column_count = 2;
    w->columns[0] = (struct du_column){"Profile", 230, 0}; w->columns[1] = (struct du_column){"Status", 120, 0}; w->active = a->hp_selected;
    add(a, DU_BUTTON, H_HP_PROPS, 408, 62, 120, 24, "Properties...");
    add(a, DU_BUTTON, H_HP_COPY, 408, 90, 120, 24, "Copy...");
    add(a, DU_BUTTON, H_HP_RENAME, 408, 118, 120, 24, "Rename...");
    add(a, DU_BUTTON, H_HP_DELETE, 408, 146, 120, 24, "Delete");
    add(a, DU_BUTTON, H_HP_UP, 24, 178, 90, 24, "Move Up");
    add(a, DU_BUTTON, H_HP_DOWN, 122, 178, 100, 24, "Move Down");
    add(a, DU_BUTTON, H_HP_DEFAULT, 230, 178, 110, 24, "Set Default");
    add(a, DU_GROUP, H_HP_GROUP, 24, 212, 504, 92, "When CiukiOS starts");
    w = add(a, DU_RADIO, H_HP_WAIT_USER, 36, 232, 480, 20, "Wait until I select a hardware profile"); w->total = 92; w->value = !a->hp_wait_mode;
    w = add(a, DU_RADIO, H_HP_WAIT_TIME, 36, 256, 330, 20, "Select the default profile if I do not choose in"); w->total = 92; w->value = a->hp_wait_mode;
    w = add(a, DU_EDIT, H_HP_SECONDS, 374, 254, 44, 22, a->hp_wait_text);
    add(a, DU_LABEL, H_HP_SECONDS_L, 424, 258, 60, 16, "seconds");
    add(a, DU_BUTTON, H_HP_APPLY, 436, 276, 80, 24, "Apply")->anchors = 0;
    int slot = 0;
    hw_lines(a, &slot, 24, 312, 504, "The start-up menu needs boot loader support that this release does not have yet: profiles are stored, and the current profile is used.");
}
static void hp_error(struct cp_app *a, int e)
{
    const char *m = e == -EBUSY ? "This hardware profile is in use and cannot be deleted." :
        e == -EPERM ? "The last hardware profile cannot be deleted." :
        e == -EEXIST ? "A hardware profile with this name already exists." :
        e == -ENOSPC ? "At most 8 hardware profiles are supported." :
        e == -EINVAL ? "The name is not valid. Use 1 to 31 printable characters, no \"=\", and no leading or trailing space." :
        e == -ENOENT ? "Select a hardware profile first." : strerror(e < 0 ? -e : e);
    hw_message(a, "Hardware Profiles", m, DU_DLG_ICON_ERROR);
}
/* Runs one profile operation transactionally and saves; returns 0 or -errno (already reported). */
static int hp_commit_op(struct cp_app *a, int e, struct hp_store *snap, const char *done)
{
    if (e) { a->hp = *snap; free(snap); hp_error(a, e); return e; }
    int w = hw_commit(a, snap);
    if (w) { char m[96]; snprintf(m, sizeof(m), "The settings could not be saved: %s.", strerror(-w)); hw_message(a, "Hardware Profiles", m, DU_DLG_ICON_ERROR); return w; }
    hp_sync(a);
    text(a, C_STATUS, done);
    return 0;
}
static const struct du_item hp_sheet_pages[] = {{.text = {"General"}}};
static int hp_sheet_accept(struct du_ui *u, int button, void *ctx)
{
    struct cp_app *a = ctx;
    if (button == 1) return 0;
    struct du_dialog *d = &u->dialog;
    unsigned i = a->hp_selected;
    if (i >= a->hp.count) return 0;
    struct hp_store *snap = hw_snapshot(a);
    if (!snap) return 1;
    int e = 0;
    if (strcmp(d->items[4].text, a->hp.p[i].name)) e = hp_rename(&a->hp, i, d->items[4].text);
    if (!e) a->hp.p[i].portable = d->items[5].value != 0;
    if (e) { a->hp = *snap; free(snap); snprintf(d->text, sizeof(d->text), "Cannot apply: %s", e == -EEXIST ? "a profile with this name already exists." : "the name is not valid."); return 1; }
    int w = hw_commit(a, snap);
    if (w) { snprintf(d->text, sizeof(d->text), "Cannot save: %s.", strerror(-w)); return 1; }
    snprintf(d->text, sizeof(d->text), "Hardware profile \"%s\".\nA portable computer can start with different hardware.", a->hp.p[i].name);
    hp_sync(a);
    return button == 2;
}
static void hp_properties(struct cp_app *a)
{
    if (a->hp_selected >= a->hp.count) return;
    struct hp_profile *p = &a->hp.p[a->hp_selected];
    du_property_sheet(a->ui, H_HP_SHEET, "Hardware Profile Properties", 380, 230, hp_sheet_pages, 1);
    struct du_dialog *d = &a->ui->dialog;
    d->accept = hp_sheet_accept; d->accept_ctx = a;
    snprintf(d->text, sizeof(d->text), "Hardware profile \"%s\".\nA portable computer can start with different hardware.", p->name);
    du_property_control(a->ui, 4, DU_EDIT, H_HP_SHEET_NAME, (struct du_rect){24, 70, 300, 23}, p->name);
    struct du_widget *w = du_property_control(a->ui, 5, DU_CHECKBOX, H_HP_SHEET_PORTABLE, (struct du_rect){24, 108, 300, 22}, "This is a &portable computer");
    if (w) w->value = p->portable;
}
static int hp_event(struct cp_app *a, const struct du_event *e)
{
    struct hp_store *snap;
    unsigned sel = a->hp_selected;
    switch (e->id) {
    case H_HP_LIST:
        if ((e->type == DU_EV_SELECT || e->type == DU_EV_OPEN) && e->index >= 0 && (unsigned)e->index < a->hp.count) {
            a->hp_selected = (unsigned)e->index; hp_items(a);
            if (e->type == DU_EV_OPEN) hp_properties(a);
        }
        return 1;
    case H_HP_PROPS: if (e->type == DU_EV_ACTIVATE) hp_properties(a); return 1;
    case H_HP_COPY:
        if (e->type == DU_EV_ACTIVATE && sel < a->hp.count) {
            char prompt[96]; snprintf(prompt, sizeof(prompt), "Copy \"%s\" to:", a->hp.p[sel].name);
            du_input_box(a->ui, H_HP_COPY_DLG, "Copy Profile", prompt, a->hp.p[sel].name);
        }
        return 1;
    case H_HP_RENAME:
        if (e->type == DU_EV_ACTIVATE && sel < a->hp.count) du_input_box(a->ui, H_HP_RENAME_DLG, "Rename Profile", "New name for the hardware profile:", a->hp.p[sel].name);
        return 1;
    case H_HP_COPY_DLG: case H_HP_RENAME_DLG:
        if (e->type == DU_EV_DIALOG && !e->index && e->text && (snap = hw_snapshot(a))) {
            int r = e->id == H_HP_COPY_DLG ? hp_copy(&a->hp, sel, e->text) : hp_rename(&a->hp, sel, e->text);
            hp_commit_op(a, r, snap, e->id == H_HP_COPY_DLG ? "Hardware profile copied." : "Hardware profile renamed.");
        }
        return 1;
    case H_HP_DELETE:
        if (e->type == DU_EV_ACTIVATE && sel < a->hp.count) {
            /* The model's rules (in use, last one) are reported without asking. */
            if (a->hp.count == 1 || sel == a->hp.active) {
                snap = hw_snapshot(a);
                if (snap) hp_commit_op(a, hp_delete(&a->hp, sel), snap, "");
            } else {
                static const char *const buttons[] = {"Yes", "No"};
                char m[120]; snprintf(m, sizeof(m), "Are you sure you want to delete the hardware profile \"%s\"?", a->hp.p[sel].name);
                du_message(a->ui, H_HP_DELETE_DLG, "Confirm Profile Delete", m, DU_DLG_ICON_QUESTION, buttons, 2, 1, 1);
            }
        }
        return 1;
    case H_HP_DELETE_DLG:
        if (e->type == DU_EV_DIALOG && !e->index && sel < a->hp.count && (snap = hw_snapshot(a))) hp_commit_op(a, hp_delete(&a->hp, sel), snap, "Hardware profile deleted.");
        return 1;
    case H_HP_UP: case H_HP_DOWN: case H_HP_DEFAULT:
        if (e->type == DU_EV_ACTIVATE && sel < a->hp.count && (snap = hw_snapshot(a))) {
            int r = e->id == H_HP_DEFAULT ? hp_set_default(&a->hp, sel) : hp_move(&a->hp, sel, e->id == H_HP_UP ? -1 : 1);
            if (!r && e->id != H_HP_DEFAULT) a->hp_selected = (unsigned)((int)sel + (e->id == H_HP_UP ? -1 : 1));
            if (r == -EINVAL && e->id != H_HP_DEFAULT) { free(snap); return 1; }   /* already at the end */
            hp_commit_op(a, r, snap, e->id == H_HP_DEFAULT ? "Default profile set." : "Hardware profile moved.");
        }
        return 1;
    case H_HP_WAIT_USER: case H_HP_WAIT_TIME:
        if (e->type == DU_EV_CHANGE && e->index) a->hp_wait_mode = e->id == H_HP_WAIT_TIME;
        return 1;
    case H_HP_APPLY:
        if (e->type == DU_EV_ACTIVATE) {
            int seconds = -1;
            if (a->hp_wait_mode) {
                const char *t = du_get(a->ui, H_HP_SECONDS)->text; seconds = 0;
                if (!*t || strlen(t) > 3) seconds = -2;
                for (; *t && seconds >= 0; t++) { if (*t < '0' || *t > '9') seconds = -2; else seconds = seconds * 10 + (*t - '0'); }
            }
            if (seconds == -2) { hw_message(a, "Hardware Profiles", "Enter a number of seconds from 0 to 999.", DU_DLG_ICON_ERROR); return 1; }
            if ((snap = hw_snapshot(a))) hp_commit_op(a, hp_set_wait(&a->hp, seconds), snap, "Start-up settings saved.");
        }
        return 1;
    default: return 0;
    }
}

/* 1 when the event belongs to this file. */
static int hw_event(struct cp_app *a, const struct du_event *e)
{
    if (e->type == DU_EV_TICK && e->id == H_AW_TIMER) {
        if (a->page == CP_ADDHW && a->aw_searching) {
            if (++a->aw_progress >= H_AW_TICKS) aw_search_done(a);
            else { struct du_widget *w = du_get(a->ui, H_AW_PROGRESS); if (w) { w->value = (int)a->aw_progress; du_invalidate_widget(a->ui, H_AW_PROGRESS); } }
        } else du_timer_stop(a->ui, H_AW_TIMER);
        return 1;
    }
    if (a->page == CP_SYSTEM && (e->id == H_PF_BTN_FILE || e->id == H_PF_GFX || e->id == H_PF_VM)) {
        if (e->type == DU_EV_ACTIVATE) perf_open(a, e->id);
        return 1;
    }
    if (e->id < H_DM_DISABLE || e->id > H_LAST) return 0;
    if (a->page == CP_ADDHW && aw_event(a, e)) return 1;
    if (a->page == CP_SYSTEM && a->system_tab == 5 && hp_event(a, e)) return 1;
    switch (e->id) {
    case H_DM_DISABLE: case H_DM_REMOVE:
        if (e->type == DU_EV_ACTIVATE) dm_confirm(a, e->id == H_DM_DISABLE ? DC_DISABLE : DC_REMOVE);
        return 1;
    case H_DM_ENABLE:
        if (e->type == DU_EV_ACTIVATE && a->selected_device_id) dm_run(a, a->selected_device_id, DC_ENABLE, NULL);
        return 1;
    case H_DM_UPDATE: if (e->type == DU_EV_ACTIVATE) dm_update_open(a); return 1;
    case H_DM_CONFIRM:
        if (e->type == DU_EV_DIALOG && !e->index) dm_run(a, a->dm_pending_id, a->dm_pending_action, NULL);
        return 1;
    case H_DM_RESTART:
        if (e->type == DU_EV_DIALOG && !e->index && a->host.power && a->host.power(a->host.ctx)) text(a, C_STATUS, "The shutdown dialog could not be opened.");
        return 1;
    case H_DM_NAME:
        if (e->type == DU_EV_DIALOG && !e->index && e->text) dm_run(a, a->selected_device_id, DC_UPDATE_DRIVER, e->text);
        return 1;
    case H_DM_LIST:
        if ((e->type == DU_EV_SELECT || e->type == DU_EV_OPEN) && e->index >= 0 && (unsigned)e->index < a->dm_candidate_count) {
            a->dm_candidate = (unsigned)e->index;
            for (unsigned k = 0; k < a->dm_candidate_count; k++) a->dm_items[k].selected = k == a->dm_candidate;
            if (e->type == DU_EV_OPEN) { struct du_event go = {.type = DU_EV_ACTIVATE, .id = H_DM_APPLY}; return hw_event(a, &go); }
        }
        return 1;
    case H_DM_APPLY:
        if (e->type == DU_EV_ACTIVATE && a->dm_updating && a->dm_candidate < a->dm_candidate_count) {
            char driver[sizeof(a->aw_catalog[0].driver)];
            snprintf(driver, sizeof(driver), "%s", a->aw_catalog[a->dm_candidates[a->dm_candidate]].driver);
            a->dm_updating = 0; build(a);
            dm_run(a, a->selected_device_id, DC_UPDATE_DRIVER, driver);
        }
        return 1;
    case H_DM_OTHER:
        if (e->type == DU_EV_ACTIVATE) { a->dm_updating = 0; build(a); du_input_box(a->ui, H_DM_NAME, "Update Driver", "Driver name:", ""); }
        return 1;
    case H_DM_CANCEL:
        if (e->type == DU_EV_ACTIVATE) { a->dm_updating = 0; build(a); }
        return 1;
    default: return 0;
    }
}
