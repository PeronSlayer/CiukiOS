/* F2-41: applets, synthetic inventory, Properties, next-boot edits and
 * review renders through the production toolkit. SPDX-License-Identifier: MIT */
#include "control.h"
#include "acc_test.h"
#include "device_fixture.h"
static struct cp_app app;
static struct du_ui ui;
static uint32_t pixels[CP_WIDTH * CP_HEIGHT], portrait[256 * 256];
static struct ciuki_devices_info sample;
static uint64_t monotonic_ns = 9000000000ull;
static uint64_t monotonic(void *ctx) { (void)ctx; return monotonic_ns; }
static int fail_query, fail_save, loads, queries, saves;
static int hwid_queries;
static int hwid_lookup(void *ctx, unsigned vendor, unsigned device, char *out, size_t size)
{ (void)ctx; hwid_queries++; return cp_hwid_lookup_json("{\"8086:2415\": \"Pack AC97 device\"}", vendor, device, out, size); }
static int usb_hwid_lookup(void *ctx, unsigned vendor, unsigned device, char *out, size_t size)
{ (void)ctx; return cp_hwid_lookup_json("{\"1234:0001\": \"USB Pack Keyboard\"}", vendor, device, out, size); }
static char boot[128] = "safe=0 serial=1 video=1024x768 input=firmware\n";
static int settings_load(void *c, char *s, size_t cap, size_t *n)
{ (void)c; const char text[] = "version=1\nwallpaper=none\n"; assert(cap >= sizeof(text)); memcpy(s, text, sizeof(text)); *n = sizeof(text) - 1; return 0; }
static int devices(void *ctx, struct ciuki_devices_info *out)
{ (void)ctx; queries++; assert(out->version == 1 && out->size == sizeof(*out)); *out = sample; return fail_query; }
static int boot_load(void *ctx, char *out, size_t cap, size_t *n)
{ (void)ctx; loads++; *n = strlen(boot); if (*n >= cap) return -1; memcpy(out, boot, *n + 1); return 0; }
static int boot_save(void *ctx, const char *in, size_t n)
{ (void)ctx; saves++; if (fail_save) return -1; assert(n < sizeof(boot)); memcpy(boot, in, n); boot[n] = 0; return 0; }
static struct du_widget *find(enum du_kind kind, const char *label)
{
    for (unsigned i = 0; i < ui.count; i++) if (ui.widgets[i].kind == kind && !strcmp(ui.widgets[i].text, label)) return &ui.widgets[i];
    return NULL;
}
static struct du_widget *kind(enum du_kind k)
{ for (unsigned i = 0; i < ui.count; i++) if (ui.widgets[i].kind == k) return &ui.widgets[i]; return NULL; }
static void click(const char *label)
{ struct du_widget *w = find(DU_BUTTON, label); assert(w); at_click_widget(&ui, w->id); }
static void tab(unsigned index)
{ struct du_widget *w = kind(DU_TABS); assert(w); du_post(&ui, DU_EV_SELECT, w->id, (int)index); }
static void render(const char *folder, const char *name)
{ du_render(&ui); du_tick(&ui, ui.now + 1000000000ull); du_render(&ui); at_save(folder, name, &ui); }
static unsigned tree_row(unsigned device)
{
    for (unsigned i = 0; i < app.tree_count; i++) if (app.tree_device[i] == (int)device) return i;
    assert(!"missing tree row"); return 0;
}
int main(int argc, char **argv)
{
    assert(argc == 4); FILE *f = fopen(argv[1], "rb"); assert(f && fread(portrait, 4, 256 * 256, f) == 256 * 256); fclose(f);
    const char *renders = argv[2];
    device_fixture(&sample);
    assert(cp_snapshot_valid(&sample));
    sample.count = 65; assert(!cp_snapshot_valid(&sample)); sample.count = 9;
    sample.devices[0].resource_count = 9; assert(!cp_snapshot_valid(&sample)); sample.devices[0].resource_count = 1;
    sample.driver_count = 17; assert(!cp_snapshot_valid(&sample)); sample.driver_count = 4;
    memset(sample.cpu_brand, 'X', sizeof(sample.cpu_brand)); assert(!cp_snapshot_valid(&sample)); device_fixture(&sample);
    sample.devices[0].class_id = 9; assert(!cp_snapshot_valid(&sample)); device_fixture(&sample);
    char out[128];
    const char duplicate[] = "safe=1\tserial=0 serial=1\r\nprobe=f2:app video=1024x768 extra=yes";
    assert(!cp_boot_serial(duplicate, sizeof(duplicate) - 1, 0, out, sizeof(out)));
    assert(!strcmp(out, "safe=1\tserial=0 serial=0\r\nprobe=f2:app video=1024x768 extra=yes"));
    assert(!cp_boot_serial("serial=", 7, 1, out, sizeof(out)) && !strcmp(out, "serial=1"));
    assert(!cp_boot_serial("safe=0", 6, 1, out, sizeof(out)) && !strcmp(out, "safe=0 serial=1"));
    assert(!cp_boot_serial("", 0, 0, out, sizeof(out)) && !strcmp(out, "serial=0"));
    assert(cp_boot_serial("a\0b", 3, 0, out, sizeof(out)) && cp_boot_serial("\xff", 1, 0, out, sizeof(out)));
    memset(out, 'x', 127); char tiny[8]; assert(cp_boot_serial(out, 127, 1, tiny, sizeof(tiny)));
    char max[128]; memset(max, 'x', 127); assert(cp_boot_serial(max, 127, 1, out, sizeof(out)));
    assert(cp_page_index(NULL) == CP_HOME && cp_page_index("devices") == CP_DEVICES && cp_page_index("drivers") == CP_DRIVERS);
    char hwid[65];
    assert(!cp_hwid_lookup_json("{\"8086:2485\": \"INF Audio Name\"}", 0x8086, 0x2485, hwid, sizeof(hwid)) && !strcmp(hwid, "INF Audio Name"));
    assert(cp_hwid_lookup_json("{}", 0x8086, 0x2485, hwid, sizeof(hwid)));
    app.host = (struct cp_host){.load = settings_load, .devices = devices, .hwid_name = hwid_lookup, .usb_hwid_name = usb_hwid_lookup, .boot_load = boot_load, .boot_save = boot_save,
        .portrait = portrait, .screen_w = 1024, .screen_h = 768, .mem_total_k = 512 * 1024, .mem_free_k = 480 * 1024,
        .version = "0.9.0 pre-alpha", .build = "host-preview", .machine = "i686", .processes = 6, .fonts_path = argv[3]};
    du_init(&ui, pixels, CP_WIDTH, CP_HEIGHT); ui.focused_window = 1;
    assert(!cp_attach(&app, &ui, CP_HOME));
    assert(app.page == CP_HOME && kind(DU_LISTVIEW)->view == DU_ICONS && kind(DU_LISTVIEW)->count == CP_PAGES);
    for (unsigned k = 0; k < CP_PAGES; k++) assert(app.applet_items[k].icon16 && app.applet_items[k].icon32);
    render(renders, "control-folder");
    static const char *const page_files[] = {"display", "mouse", "keyboard", "sounds", "datetime", "system", "devices", "drivers", "network", "power", "adapter", "fonts", "autoplay"};
    _Static_assert(sizeof(page_files) / sizeof(page_files[0]) == CP_PAGES, "every Control Panel page needs a capture");
    for (int k = 0; k < CP_PAGES; k++) {
        du_focus(&ui, kind(DU_LISTVIEW)->id); at_tap(&ui, DU_KEY_HOME);
        for (int right = 0; right < k; right++) at_tap(&ui, DU_KEY_RIGHT);
        at_tap(&ui, DU_KEY_ENTER); assert(app.page == k);
        char name[64]; snprintf(name, sizeof(name), "control-%s", page_files[k]); render(renders, name);
        assert(ui.count <= DU_WIDGETS);
        click("Control Panel"); assert(app.page == CP_HOME);
    }
    cp_open(&app, CP_DEVICES);
    assert(app.tree_count == 19 && !strcmp(app.tree_items[0].text[0], "Computer"));
    for (unsigned k = 0; k < sample.count; k++) assert(app.tree_items[tree_row(k)].depth == 2 && app.tree_items[tree_row(k)].icon16);
    struct ciuki_device_record usb_sample = sample.devices[0];
    usb_sample.bus_type = CIUKI_BUS_USB; usb_sample.vendor = 0x1234; usb_sample.device = 1;
    struct ciuki_device_record original_usb_sample = sample.devices[0];
    sample.devices[0] = usb_sample; click("Refresh");
    assert(strstr(app.tree_items[tree_row(0)].text[0], "USB Pack Keyboard"));
    sample.devices[0] = original_usb_sample; click("Refresh");
    unsigned audio_row = tree_row(6); assert(strstr(app.tree_items[audio_row].text[0], "Pack AC97 device") && hwid_queries > 0);
    assert(strstr(app.tree_items[audio_row].text[0], "quarantined"));
    sample.devices[6].device = 0x2485; click("Refresh");
    audio_row = tree_row(6); assert(strstr(app.tree_items[audio_row].text[0], "Intel ICH3 AC97 audio"));
    sample.devices[6].device = 0x2415; click("Refresh");
    audio_row = tree_row(6);
    struct du_widget *tree = kind(DU_TREE); tree->active = audio_row; du_post(&ui, DU_EV_SELECT, tree->id, (int)audio_row);
    click("Properties..."); assert(du_dialog_id(&ui) && strstr(ui.dialog.text, "Quarantined") && strstr(ui.dialog.text, "8086:2415"));
    assert(strstr(ui.dialog.text, "ac97") && strstr(ui.dialog.text, "Generation: 16")); render(renders, "device-properties-general");
    struct du_rect b = du_dialog_button(&ui, 1); at_click(&ui, b.x + b.w / 2, b.y + b.h / 2);
    assert(strstr(ui.dialog.text, "I/O: 0xC000 - 0xC0FF") && strstr(ui.dialog.text, "IRQ: 11") && strstr(ui.dialog.text, "size unknown"));
    render(renders, "device-properties-resources"); at_tap(&ui, DU_KEY_ESC); assert(!du_dialog_id(&ui));
    int q0 = queries; click("Refresh"); assert(queries == q0 + 1 && kind(DU_TREE)->active == tree_row(6));
    tree = kind(DU_TREE); du_post(&ui, DU_EV_OPEN, tree->id, 1); assert(app.tree_count == 18); /* collapse System */
    du_post(&ui, DU_EV_OPEN, tree->id, 0); assert(app.tree_count == 1);
    du_post(&ui, DU_EV_OPEN, tree->id, 0); assert(app.tree_count == 18);
    app.collapsed = 0;
    for (unsigned k = 0; k < 64; k++) {
        sample.devices[k] = sample.devices[k % 9]; sample.devices[k].id = k + 1;
    }
    sample.count = 64; click("Refresh"); assert(app.tree_count == CP_TREE_MAX);
    fail_query = 1; click("Refresh"); assert(!app.devices_ok && app.tree_count == 10);
    assert(find(DU_LABEL, "Device information is unavailable.")); render(renders, "devices-unavailable");
    fail_query = 0; device_fixture(&sample);
    cp_open(&app, CP_DRIVERS); assert(du_get(&ui, 105)->count == 4 && du_get(&ui, 106)->count == 4);
    assert(!strcmp(app.driver_items[1].text[4], "Sole firmware-first PS/2 lease"));
    du_post(&ui, DU_EV_SELECT, 105, 1); click("Details...");
    assert(strstr(ui.dialog.text, "Sole firmware-first PS/2 lease") && strstr(ui.dialog.text, "Kernel build: host-preview"));
    render(renders, "driver-details"); at_tap(&ui, DU_KEY_ESC);
    du_post(&ui, DU_EV_SELECT, 106, 3); click("Details...");
    assert(strstr(ui.dialog.text, "Generation: 13") && strstr(ui.dialog.text, "Quarantined"));
    render(renders, "lease-details"); at_tap(&ui, DU_KEY_ESC);
    app.host.monotonic = monotonic;
    cp_open(&app, CP_SYSTEM); assert(find(DU_LABEL, "CPU: Intel Pentium III"));
    assert(find(DU_LABEL, "Uptime: 0 days, 02:02:03"));
    monotonic_ns += 5000000000ull; cp_tick(&app);
    assert(find(DU_LABEL, "Uptime: 0 days, 02:02:08"));
    tab(1); render(renders, "system-hardware"); assert(find(DU_LABEL, "Sound: 1 listed, 0 ready"));
    tab(2); render(renders, "system-advanced"); assert(kind(DU_CHECKBOX)->value == 1);
    at_click_widget(&ui, kind(DU_CHECKBOX)->id); assert(app.serial_next == 0);
    /* A different writer updated the boot file while the page was open. */
    strcpy(boot, "safe=1 serial=1 video=800x600 extra=keep\n");
    click("Apply to BOOT.CFG"); assert(saves == 1 && strstr(boot, "serial=0") && strstr(boot, "video=800x600 extra=keep"));
    assert(sample.boot_flags == 2 && find(DU_LABEL, "Saved. Serial logging changes at the next boot."));
    fail_save = 1; click("Apply to BOOT.CFG"); assert(find(DU_LABEL, "BOOT.CFG could not be saved."));
    strcpy(boot, "serial=0 serial=1"); tab(2); assert(!kind(DU_CHECKBOX)->value);
    strcpy(boot, "safe=0"); tab(2); assert(kind(DU_CHECKBOX)->value);
    assert(loads >= 3); cp_detach(&app);
    puts("PASS device manager: twelve applets, keyboard navigation, bounded class tree, refresh, Properties, drivers/leases, system and preserving BOOT.CFG edits");
    return 0;
}
