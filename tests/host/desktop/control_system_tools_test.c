/* G30 Control Panel: Device Manager actions, Add New Hardware, System Properties Performance and
 * Hardware Profiles tabs, driven through the production widgets with synthetic input.
 * SPDX-License-Identifier: MIT */
#include "control.h"
#include "acc_test.h"
#include "device_fixture.h"
#define HW_TEXT_TEST 24576
static struct cp_app app;
static struct du_ui ui;
static uint32_t pixels[CP_WIDTH * CP_HEIGHT];
static struct ciuki_devices_info sample;
static const char *renders;

/* in-memory hwprofiles.cfg */
static char file[HW_TEXT_TEST];
static int file_exists, file_saves, fail_save, fail_load, power_calls, catalog_pack;
static uint32_t boot_count = 1;
static uint32_t boot_serial(void *ctx) { (void)ctx; return boot_count; }
static uint64_t mem_total = 512 * 1024, mem_free = 400 * 1024;
static int hw_load(void *c, char *t, size_t cap, size_t *n)
{ (void)c; if (fail_load) return -EIO; if (!file_exists) return -ENOENT; *n = strlen(file); assert(*n < cap); memcpy(t, file, *n + 1); return 0; }
static int hw_save(void *c, const char *t, size_t n)
{ (void)c; if (fail_save) return -ENOSPC; assert(n < sizeof(file)); memcpy(file, t, n); file[n] = 0; file_exists = 1; file_saves++; return 0; }
static struct system_perf perf_saved;
static int perf_save(void *c, const struct system_perf *p) { (void)c; if (fail_save) return -ENOSPC; perf_saved = *p; return 0; }
static int devices(void *c, struct ciuki_devices_info *out) { (void)c; *out = sample; return 0; }
static int power(void *c) { (void)c; power_calls++; return 0; }
static int memory(void *c, uint64_t *t, uint64_t *f) { (void)c; *t = mem_total; *f = mem_free; return 0; }
static int engine_info(void *c, struct ciuki_display_engine_info *e) { (void)c; memset(e, 0, sizeof(*e)); e->version = CIUKI_DISPLAY_ENGINE_VERSION; e->kind = CIUKI_ENGINE_SOFTWARE; return 0; }
static int catalog(void *c, struct aw_model *out, unsigned cap)
{
    (void)c;
    if (!catalog_pack || !cap) return 0;
    memset(out, 0, sizeof(*out)); out->class_id = CIUKI_DEVICE_NETWORK;
    strcpy(out->name, "Realtek RTL8139 (driver pack)"); strcpy(out->driver, "rtl8139");
    return 1;
}
static int settings_load(void *c, char *s, size_t cap, size_t *n)
{ (void)c; const char text[] = "version=1\nwallpaper=none\n"; assert(cap >= sizeof(text)); memcpy(s, text, sizeof(text)); *n = sizeof(text) - 1; return 0; }

static struct du_widget *find(enum du_kind k, const char *label)
{ for (unsigned i = 0; i < ui.count; i++) if (ui.widgets[i].kind == k && !strcmp(ui.widgets[i].text, label)) return &ui.widgets[i]; return NULL; }
static struct du_widget *kind(enum du_kind k)
{ for (unsigned i = 0; i < ui.count; i++) if (ui.widgets[i].kind == k) return &ui.widgets[i]; return NULL; }
static void click_at(const char *label, int line)
{ (void)line; struct du_widget *w = find(DU_BUTTON, label); if (!w || w->disabled) fprintf(stderr, "line %d click %s: %s (page %d aw %d tab %d)\n", line, label, w ? "disabled" : "missing", app.page, app.aw.page, app.system_tab); assert(w && !w->disabled); at_click_widget(&ui, w->id); }
#define click(label) click_at(label, __LINE__)
static void tab(unsigned i) { struct du_widget *w = kind(DU_TABS); assert(w); du_post(&ui, DU_EV_SELECT, w->id, (int)i); }
static void dialog_button(unsigned i)
{ struct du_rect r = du_dialog_button(&ui, i); assert(r.w); at_click(&ui, r.x + r.w / 2, r.y + r.h / 2); }
static void render(const char *name)
{ du_render(&ui); du_tick(&ui, ui.now + 1000000000ull); du_render(&ui); at_save(renders, name, &ui); }
static unsigned tree_row(unsigned device)
{ for (unsigned i = 0; i < app.tree_count; i++) if (app.tree_device[i] == (int)device) return i; assert(!"row"); return 0; }
static void select_device(unsigned device)
{ struct du_widget *t = kind(DU_TREE); t->active = tree_row(device); du_post(&ui, DU_EV_SELECT, t->id, (int)t->active); }
static const char *row_name(unsigned device) { return app.tree_items[tree_row(device)].text[0]; }
static void attach(int page)
{
    memset(&app, 0, sizeof(app));
    app.host = (struct cp_host){.load = settings_load, .devices = devices, .hw_load = hw_load, .hw_save = hw_save, .boot_serial = boot_serial, .perf_save = perf_save, .power = power,
        .memory = memory, .engine_info = engine_info, .driver_catalog = catalog, .screen_w = 1024, .screen_h = 768,
        .mem_total_k = 512 * 1024, .mem_free_k = 400 * 1024, .version = "0.9.0", .build = "host", .machine = "i686"};
    du_init(&ui, pixels, CP_WIDTH, CP_HEIGHT); ui.focused_window = 1;
    assert(!cp_attach(&app, &ui, page));
    struct du_widget *tabs=kind(DU_TABS);
    if (tabs && tabs->count==7) {
        unsigned seen=0;
        for (int x=tabs->box.x;x<tabs->box.x+tabs->box.w;x++) {
            int tab=du_tabs_hit(tabs,x,tabs->box.y+10);
            if (tab>=0) seen|=1u<<(unsigned)tab;
        }
        assert(seen==127); /* every merged System page is reachable */
        assert(du_tabs_hit(tabs,tabs->box.x+tabs->box.w-1,tabs->box.y+10)==-1);
    }
}
static void reset_file(void) { power_calls = 0; file[0] = 0; file_exists = 0; file_saves = 0; fail_save = 0; }
static void type_dialog(const char *text) { du_set_text(&ui.dialog.items[2], text); at_tap(&ui, DU_KEY_ENTER); }

static void device_actions(void)
{
    device_fixture(&sample); reset_file(); attach(CP_DEVICES);
    assert(find(DU_BUTTON, "Disable")->disabled && find(DU_BUTTON, "Enable")->disabled && find(DU_BUTTON, "Update Driver...")->disabled);
    render("g30-devices-buttons");
    /* a working device: Disable asks first; No changes nothing */
    select_device(5);
    assert(!find(DU_BUTTON, "Disable")->disabled && find(DU_BUTTON, "Enable")->disabled && !find(DU_BUTTON, "Remove")->disabled);
    click("Disable"); assert(strstr(ui.dialog.text, "Do you really want to disable it?")); render("g30-disable-confirm");
    dialog_button(1); assert(!du_dialog_id(&ui) && !file_saves && !strstr(row_name(5), "disabled"));
    click("Disable"); dialog_button(0);
    assert(file_saves == 1 && strstr(file, "device=6 disabled=1 removed=0 driver=-") && strstr(row_name(5), "(disabled)"));
    assert(strstr(ui.dialog.text, "release its resources") && strstr(ui.dialog.text, "Restart the computer")); render("g30-disable-result");
    dialog_button(0); assert(power_calls == 1);   /* Restart... opens the shutdown dialog */
    if (du_dialog_id(&ui)) at_tap(&ui, DU_KEY_ESC);
    select_device(5);
    assert(find(DU_BUTTON, "Disable")->disabled && !find(DU_BUTTON, "Enable")->disabled);
    click("Properties..."); assert(strstr(ui.dialog.text, "This device is disabled.") && strstr(ui.dialog.text, "Hardware profile: Original Configuration"));
    dialog_button(1); assert(strstr(ui.dialog.text, "No resources are held") && !strstr(ui.dialog.text, "I/O:"));
    at_tap(&ui, DU_KEY_ESC);
    /* the disabled setting survives a new Control Panel session */
    attach(CP_DEVICES); assert(strstr(row_name(5), "(disabled)") && app.hp.p[0].devices.count == 1);
    select_device(5); click("Enable");
    assert(strstr(ui.dialog.text, "Restart to start it") && !strstr(file, "device=6") && !strstr(row_name(5), "disabled"));
    dialog_button(1); if (du_dialog_id(&ui)) at_tap(&ui, DU_KEY_ESC);
    /* Remove a device that is not running: no restart needed, detected again after one */
    select_device(3); click("Remove"); assert(strstr(ui.dialog.text, "about to remove")); dialog_button(0);
    assert(strstr(file, "device=4 disabled=0 removed=1") && strstr(row_name(3), "(removed)"));
    assert(du_dialog_id(&ui)); at_tap(&ui, DU_KEY_ESC);
    click("Properties..."); assert(strstr(ui.dialog.text, "(removed, detected again after a restart)")); at_tap(&ui, DU_KEY_ESC);
    dc_restart(&app.hp.p[app.hp.active].devices); cp_tree_build(&app);
    assert(!strstr(row_name(3), "removed") && app.hp.p[0].devices.count == 0);
    /* critical devices are refused with the model's message, without confirmation and without saving */
    int saved = file_saves;
    select_device(4); click("Disable"); assert(strstr(ui.dialog.text, "required by the system and cannot be disabled")); at_tap(&ui, DU_KEY_ESC);
    select_device(0); click("Remove"); assert(strstr(ui.dialog.text, "cannot be removed")); at_tap(&ui, DU_KEY_ESC);
    assert(file_saves == saved && !strstr(row_name(4), "disabled"));
    /* a failed save rolls the change back and is reported */
    fail_save = 1; select_device(5); click("Disable"); dialog_button(0);
    assert(strstr(ui.dialog.text, "could not be saved") && !strstr(row_name(5), "disabled") && app.hp.p[0].devices.count == 0);
    at_tap(&ui, DU_KEY_ESC); fail_save = 0;
}

static void update_driver(void)
{
    device_fixture(&sample); reset_file(); catalog_pack = 0; attach(CP_DEVICES);
    /* built-in list for the class of the selected device */
    select_device(6); click("Update Driver...");
    struct du_widget *list = kind(DU_LISTVIEW); assert(list && list->count == 1 && !strcmp(app.dm_items[0].text[1], "ac97") && !strcmp(app.dm_items[0].text[2], "Built-in"));
    render("g30-update-driver");
    click("Cancel"); assert(kind(DU_TREE));
    select_device(6); click("Update Driver..."); click("Update");
    assert(strstr(file, "device=7 disabled=0 removed=0 driver=ac97") && strstr(ui.dialog.text, "Restart to load it"));
    dialog_button(1); if (du_dialog_id(&ui)) at_tap(&ui, DU_KEY_ESC);
    click("Properties..."); assert(strstr(ui.dialog.text, "Driver update pending: ac97")); at_tap(&ui, DU_KEY_ESC);
    /* the driver pack contributes network entries only when present */
    select_device(7); click("Update Driver..."); assert(du_dialog_id(&ui) && !kind(DU_LISTVIEW));
    at_tap(&ui, DU_KEY_ESC);
    catalog_pack = 1; select_device(7); click("Update Driver...");
    list = kind(DU_LISTVIEW); assert(list && list->count == 1 && !strcmp(app.dm_items[0].text[1], "rtl8139") && !strcmp(app.dm_items[0].text[2], "Driver pack"));
    click("Other..."); assert(du_dialog_id(&ui)); type_dialog("not a name"); assert(strstr(ui.dialog.text, "driver name is not valid"));
    assert(!strstr(file, "device=8")); at_tap(&ui, DU_KEY_ESC);
    select_device(7); click("Update Driver..."); click("Other..."); type_dialog("ne2000");
    assert(strstr(file, "device=8 disabled=0 removed=0 driver=ne2000")); at_tap(&ui, DU_KEY_ESC);
    /* no catalogue entry at all for the class: a text entry, validated by the model */
    catalog_pack = 0; select_device(8); click("Update Driver..."); type_dialog("uart16550");
    assert(strstr(file, "device=9") && strstr(file, "driver=uart16550")); at_tap(&ui, DU_KEY_ESC);
}

static void add_hardware(void)
{
    device_fixture(&sample); reset_file(); catalog_pack = 0; attach(CP_HOME);
    assert(cp_page_index("addhw") == CP_ADDHW);
    cp_open(&app, CP_ADDHW); assert(find(DU_LABEL, "Add New Hardware Wizard") && find(DU_BUTTON, "< Back")->disabled);
    render("g30-addhw-welcome");
    click("Next >"); assert(find(DU_LABEL, "Do you want Windows to search for your new hardware?") && find(DU_RADIO, "&Yes (Recommended)")->value);
    render("g30-addhw-search");
    click("Next >"); assert(app.aw_searching && kind(DU_PROGRESS) && find(DU_BUTTON, "Next >")->disabled); render("g30-addhw-searching");
    for (unsigned i = 0; i < 5; i++) du_tick(&ui, ui.now + 150000001ull);
    assert(!app.aw_searching && app.aw.page == AW_FOUND && app.aw.found_count == 4);   /* keyboard, mouse, audio, network */
    assert(kind(DU_LISTVIEW)->count == 4); render("g30-addhw-found");
    click("Next >"); assert(app.aw.page == AW_MODEL && app.aw_list_count == 1 && !strcmp(app.aw_items[0].text[1], "firmware-input"));
    assert(find(DU_BUTTON, "Next >")->disabled);
    du_post(&ui, DU_EV_SELECT, kind(DU_LISTVIEW)->id, 0); assert(!find(DU_BUTTON, "Next >")->disabled); render("g30-addhw-model");
    click("Next >"); assert(app.aw.page == AW_FINISH && find(DU_BUTTON, "Finish")); render("g30-addhw-finish");
    click("Finish"); assert(app.aw.page == AW_DONE && strstr(file, "device=3 disabled=0 removed=0 driver=firmware-input"));
    assert(file_saves == 1 && !find(DU_BUTTON, "Restart...")->disabled); render("g30-addhw-done");
    click("Restart..."); assert(power_calls == 1);
    click("Finish"); assert(app.page == CP_HOME);
    /* Back walks the history */
    cp_open(&app, CP_ADDHW); click("Next >"); assert(app.aw.page == AW_SEARCH); click("< Back"); assert(app.aw.page == AW_WELCOME);
}
static void add_hardware_more(void)
{
    /* no new hardware: the wizard offers the hardware types */
    device_fixture(&sample); reset_file();
    for (unsigned i = 0; i < sample.count; i++) sample.devices[i].state = CIUKI_DEVICE_READY;
    attach(CP_ADDHW); click("Next >"); click("Next >");
    for (unsigned i = 0; i < 5; i++) du_tick(&ui, ui.now + 150000001ull);
    assert(app.aw.page == AW_NONE_FOUND && find(DU_LABEL, "No new hardware found")); render("g30-addhw-none");
    click("Next >"); assert(app.aw.page == AW_CLASS && kind(DU_LISTVIEW)->count == CIUKI_DEVICE_CLASSES && find(DU_BUTTON, "Next >")->disabled);
    render("g30-addhw-classes");
    du_post(&ui, DU_EV_SELECT, kind(DU_LISTVIEW)->id, CIUKI_DEVICE_SOUND); click("Next >");
    assert(app.aw.page == AW_MODEL && app.aw_list_count == 1);
    du_post(&ui, DU_EV_SELECT, kind(DU_LISTVIEW)->id, 0); click("Next >"); click("Finish");
    assert(du_dialog_id(&ui) && strstr(ui.dialog.text, "waiting for a driver") && app.aw.page == AW_FINISH && !file_saves);   /* nothing to install: never invent */
    at_tap(&ui, DU_KEY_ESC);
    /* manual choice with a quarantined device that waits for a driver */
    sample.devices[6].state = CIUKI_DEVICE_QUARANTINED;
    attach(CP_ADDHW); click("Next >"); at_click_widget(&ui, find(DU_RADIO, "&No, I want to select the hardware from a list")->id); click("Next >");
    assert(app.aw.page == AW_CLASS && !app.aw_searching);
    du_post(&ui, DU_EV_SELECT, kind(DU_LISTVIEW)->id, CIUKI_DEVICE_SOUND); click("Next >");
    du_post(&ui, DU_EV_SELECT, kind(DU_LISTVIEW)->id, 0); click("Next >"); click("Finish");
    assert(app.aw.page == AW_DONE && strstr(file, "device=7 disabled=0 removed=0 driver=ac97"));
    /* the pack's network entry is offered only when the package manifest is present */
    device_fixture(&sample); reset_file(); catalog_pack = 1; attach(CP_ADDHW); click("Next >"); click("Next >");
    for (unsigned i = 0; i < 5; i++) du_tick(&ui, ui.now + 150000001ull);
    click("Next >"); click("< Back"); assert(app.aw.page == AW_FOUND);
    /* Cancel leaves the wizard without recording anything */
    click("Cancel"); assert(app.page == CP_HOME && !file_saves);
    /* a failed save keeps the wizard on Finish and records nothing */
    attach(CP_ADDHW); click("Next >"); click("Next >");
    for (unsigned i = 0; i < 5; i++) du_tick(&ui, ui.now + 150000001ull);
    click("Next >"); du_post(&ui, DU_EV_SELECT, kind(DU_LISTVIEW)->id, 0); click("Next >");
    fail_save = 1; click("Finish");
    assert(app.aw.page == AW_FINISH && strstr(ui.dialog.text, "could not be saved") && app.hp.p[0].devices.count == 0);
    at_tap(&ui, DU_KEY_ESC); fail_save = 0; catalog_pack = 0;
}

static void performance(void)
{
    device_fixture(&sample); reset_file(); mem_free = 400 * 1024; attach(CP_SYSTEM);
    tab(4); assert(app.page == CP_SYSTEM && app.system_tab == 4);
    assert(find(DU_LABEL, "Memory:") && find(DU_LABEL, "512.0 MB of RAM") && find(DU_LABEL, "78% free") && find(DU_LABEL, "32-bit"));
    assert(find(DU_LABEL, "Managed automatically by the system") && find(DU_LABEL, "Not installed"));
    assert(find(DU_LABEL, "Hardware acceleration: software drawing") && find(DU_LABEL, "Your system is configured for optimal performance."));
    assert(find(DU_LABEL, "No PC Card sockets are installed."));
    assert(!find(DU_BUTTON, "File System...")->disabled && !find(DU_BUTTON, "Graphics...")->disabled && !find(DU_BUTTON, "Virtual Memory...")->disabled);
    assert(find(DU_LABEL, "Changes to these settings take effect after restarting the computer."));
    render("g30-performance");
    click("File System..."); assert(ui.dialog.kind != DU_DLG_NONE && ui.dialog.sheet); render("g30-performance-filesystem");
    ui.dialog.items[4].value = 0; ui.dialog.items[5].value = 1; ui.dialog.items[7].value = 0;
    assert(ui.dialog.accept(&ui, 0, ui.dialog.accept_ctx) == 0 && perf_saved.role == 1 && !perf_saved.read_ahead);
    at_tap(&ui, DU_KEY_ESC);
    click("Graphics..."); render("g30-performance-graphics"); ui.dialog.items[4].value = 0;
    assert(ui.dialog.accept(&ui, 0, ui.dialog.accept_ctx) == 0 && !perf_saved.graphics);
    at_tap(&ui, DU_KEY_ESC);
    click("Virtual Memory..."); render("g30-performance-vm"); ui.dialog.items[4].value = 0; ui.dialog.items[5].value = 1;
    strcpy(ui.dialog.items[7].text, "32"); strcpy(ui.dialog.items[8].text, "128");
    assert(ui.dialog.accept(&ui, 0, ui.dialog.accept_ctx) == 0 && perf_saved.vm == 1 && perf_saved.min_mb == 32 && perf_saved.max_mb == 128);
    strcpy(ui.dialog.items[7].text, "129");
    assert(ui.dialog.accept(&ui, 0, ui.dialog.accept_ctx) == 1 && perf_saved.min_mb == 32);
    strcpy(ui.dialog.items[7].text, "64"); fail_save = 1;
    assert(ui.dialog.accept(&ui, 0, ui.dialog.accept_ctx) == 1 && app.perf_settings.min_mb == 32);
    fail_save = 0; at_tap(&ui, DU_KEY_ESC);

    /* acceleration from the accelerator record, a CardBus bridge counts as a PC Card socket */
    sample.devices[0].id = CIUKI_DEVICE_ACCELERATOR_ID; sample.devices[0].accel_ready = 1;
    strcpy(sample.devices[0].driver_display_name, "Savage display driver");
    sample.devices[1].bus_type = CIUKI_BUS_PCI; sample.devices[1].pci_class = 6; sample.devices[1].subclass = 7;
    tab(0); tab(4);
    assert(find(DU_LABEL, "Hardware acceleration: Savage display driver") && find(DU_LABEL, "1 PC Card socket(s)"));
    /* low resources change the verdict */
    mem_free = 40 * 1024; tab(0); tab(4);
    assert(find(DU_LABEL, "System resources are low, which reduces performance.") && find(DU_LABEL, "7% free")); render("g30-performance-low");
    mem_free = 400 * 1024;
    /* the other tabs stay reachable */
    tab(1); assert(app.page == CP_DEVICES); tab(3); assert(app.page == CP_DRIVERS); tab(5); assert(app.page == CP_SYSTEM && app.system_tab == 5);
}

static void profiles(void)
{
    device_fixture(&sample); reset_file(); attach(CP_SYSTEM);
    tab(5); assert(app.system_tab == 5 && kind(DU_LISTVIEW)->count == 1 && !strcmp(app.profile_notes[0], "Current, Default"));
    render("g30-profiles");
    /* the last profile and the profile in use cannot be deleted */
    click("Delete"); assert(strstr(ui.dialog.text, "last hardware profile")); at_tap(&ui, DU_KEY_ESC);
    click("Copy..."); type_dialog("Docked"); assert(app.hp.count == 2 && strstr(file, "profile=Docked|0"));
    click("Copy..."); type_dialog("docked"); assert(strstr(ui.dialog.text, "already exists") && app.hp.count == 2); at_tap(&ui, DU_KEY_ESC);
    click("Copy..."); type_dialog("a=b"); assert(strstr(ui.dialog.text, "name is not valid")); at_tap(&ui, DU_KEY_ESC);
    click("Delete"); assert(strstr(ui.dialog.text, "in use")); at_tap(&ui, DU_KEY_ESC);   /* selection 0 is the current profile */
    struct du_widget *list = kind(DU_LISTVIEW);
    du_post(&ui, DU_EV_SELECT, list->id, 1);
    click("Rename..."); type_dialog("Docking station"); assert(!strcmp(app.hp.p[1].name, "Docking station") && strstr(file, "profile=Docking station|0"));
    click("Move Up"); assert(!strcmp(app.hp.p[0].name, "Docking station") && app.hp.active == 1 && app.hp_selected == 0);
    assert(!strcmp(app.profile_notes[1], "Current, Default") || !strcmp(app.profile_notes[1], "Current"));
    click("Move Down"); assert(!strcmp(app.hp.p[1].name, "Docking station") && app.hp_selected == 1);
    click("Set Default"); assert(app.hp.default_index == 1 && strstr(file, "default=1"));
    /* Properties: rename and portable computer */
    click("Properties..."); assert(du_dialog_id(&ui) && ui.dialog.sheet); render("g30-profile-properties");
    du_set_text(&ui.dialog.items[4], "Dock"); ui.dialog.items[5].value = 1; dialog_button(0);
    assert(!du_dialog_id(&ui) && !strcmp(app.hp.p[1].name, "Dock") && app.hp.p[1].portable && strstr(file, "profile=Dock|1"));
    /* start-up wait */
    struct du_widget *seconds = find(DU_EDIT, "30"); assert(seconds);
    du_set_text(seconds, "45"); click("Apply"); assert(app.hp.wait_seconds == 45 && strstr(file, "wait=45"));
    du_set_text(seconds, "abc"); click("Apply"); assert(strstr(ui.dialog.text, "0 to 999") && app.hp.wait_seconds == 45); at_tap(&ui, DU_KEY_ESC);
    du_set_text(seconds, "1000"); click("Apply"); assert(strstr(ui.dialog.text, "0 to 999")); at_tap(&ui, DU_KEY_ESC);
    at_click_widget(&ui, find(DU_RADIO, "Wait until I select a hardware profile")->id); click("Apply");
    assert(app.hp.wait_seconds == -1 && strstr(file, "wait=-1")); render("g30-profiles-wait");
    /* delete the copy (confirmed), then round trip through a new session */
    du_post(&ui, DU_EV_SELECT, list->id, 1); click("Delete"); assert(strstr(ui.dialog.text, "Are you sure")); dialog_button(1); assert(app.hp.count == 2);
    click("Delete"); dialog_button(0); assert(app.hp.count == 1 && !strstr(file, "Dock"));
    click("Copy..."); type_dialog("Travel");
    char before[HW_TEXT_TEST]; strcpy(before, file);
    attach(CP_SYSTEM); tab(5);
    assert(app.hp.count == 2 && !strcmp(app.hp.p[1].name, "Travel") && app.hp.wait_seconds == -1);
    char again[HW_TEXT_TEST]; assert(hp_save(&app.hp, again, sizeof(again)) > 0 && !strcmp(again, before));
    /* a failing save is rolled back */
    fail_save = 1; click("Copy..."); type_dialog("Third"); assert(strstr(ui.dialog.text, "could not be saved") && app.hp.count == 2);
    at_tap(&ui, DU_KEY_ESC); fail_save = 0;
    /* an unreadable file keeps defaults and says so; an absent file is silent */
    fail_load = 1; attach(CP_SYSTEM); assert(app.hp.count == 1 && strstr(app.hp_loaded_note, "could not be read")); fail_load = 0;
    /* device actions go to the current profile only */
    device_fixture(&sample); attach(CP_DEVICES);
    app.hp.active = 1; select_device(5); click("Disable"); dialog_button(0);
    assert(app.hp.p[1].devices.count == 1 && app.hp.p[0].devices.count == 0 && strstr(file, "1:device=6 disabled=1"));
    if (du_dialog_id(&ui)) at_tap(&ui, DU_KEY_ESC);
    /* 32 stored device settings still save: the profile writer must hold DC_MAX entries */
    device_fixture(&sample); reset_file(); attach(CP_DEVICES);
    struct dc_store *s = &app.hp.p[app.hp.active].devices;
    for (unsigned i = 0; i < DC_MAX; i++) { s->e[i].id = 0x10000 + i; s->e[i].disabled = 1; strcpy(s->e[i].driver, "abcdefghijklmnopqrstuvwxyz12345"); }
    s->count = DC_MAX;
    assert(hp_save(&app.hp, again, sizeof(again)) > 0);
}

static void reboot_state(void)
{
    device_fixture(&sample); reset_file();
    struct hp_store hp; hp_init(&hp); hp.boot_serial = boot_count;
    struct ciuki_device_record dev = {.id=123, .class_id=CIUKI_DEVICE_SOUND, .state=CIUKI_DEVICE_READY};
    struct dc_result result;
    dc_apply(&hp.p[0].devices, &dev, DC_REMOVE, NULL, &result); assert(!result.error);
    dev.id=124; dc_apply(&hp.p[0].devices, &dev, DC_DISABLE, NULL, &result); assert(!result.error);
    dev.id=125; dc_apply(&hp.p[0].devices, &dev, DC_UPDATE_DRIVER, "ac97", &result); assert(!result.error);
    int n=hp_save(&hp, file, sizeof(file)); assert(n>0); file_exists=1;
    attach(CP_DEVICES); assert(dc_status_of(&app.hp.p[0].devices,123)==DC_REMOVED);
    boot_count++; attach(CP_DEVICES);
    assert(dc_status_of(&app.hp.p[0].devices,123)==DC_ACTIVE && dc_status_of(&app.hp.p[0].devices,124)==DC_DISABLED);
    assert(!strcmp(dc_driver_override(&app.hp.p[0].devices,125),"ac97") && app.hp.boot_serial==boot_count);
    assert(!hp_copy(&hp,0,"Docked")); n=hp_save(&hp,file,sizeof(file)); assert(n>0);
    strcpy(sample.boot_options,"hwprofile=1"); attach(CP_DEVICES); assert(app.hp.active==1);
}
int main(int argc, char **argv)
{
    assert(argc == 2); renders = argv[1];
    device_actions(); update_driver(); add_hardware(); add_hardware_more(); performance(); profiles(); reboot_state();
    cp_detach(&app);
    puts("PASS G30 control: device actions, update driver, Add New Hardware, Performance and Hardware Profiles");
    return 0;
}
