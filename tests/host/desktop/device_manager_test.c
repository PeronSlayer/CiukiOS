/* F2-41: applets, synthetic inventory, Properties, next-boot edits and
 * review renders through the production toolkit. SPDX-License-Identifier: MIT */
#include "control.h"
#include "acc_test.h"
#include "device_fixture.h"
#include "display_fixture.h"
static struct cp_app app;
static struct du_ui ui;
static uint32_t pixels[CP_WIDTH * CP_HEIGHT], portrait[256 * 256];
static struct ciuki_devices_info sample;
static uint64_t monotonic_ns = 9000000000ull;
static uint64_t monotonic(void *ctx) { (void)ctx; return monotonic_ns; }
static int fail_query, fail_save, loads, queries, saves;
static int pack_installs, pack_removes;
static int pack_scan(void *ctx,const char *root,char names[][192],unsigned cap)
{ (void)ctx; assert(!strcmp(root,"/media") && cap>=2); strcpy(names[0],"rejected.inf"); strcpy(names[1],"synthetic.inf"); return 2; }
static int pack_preview(void *ctx,struct dp_plan *p,const char *root,const char *inf,const struct ndp_device *d,char reason[160])
{
    (void)ctx; (void)root; assert(d->vendor==0x10ec);
    if(!strcmp(inf,"rejected.inf")) { strcpy(reason,"No exact NT x86 model match."); return -EINVAL; }
    memset(p,0,sizeof(*p)); struct ndp_manifest *m=&p->manifest; m->device=*d;
    strcpy(m->name,"Synthetic Adapter"); strcpy(m->provider,"Synthetic Provider"); strcpy(m->driver_version,"1.2.3.4");
    strcpy(m->hardware_id,"PCI\\VEN_10EC&DEV_8139"); strcpy(m->service,"Synthetic"); strcpy(m->reason,NDP_DISABLED);
    m->file_count=3; strcpy(m->files[0].name,"synthetic.inf"); strcpy(m->files[1].name,"mini.sys"); strcpy(m->files[2].name,"support.dat"); return 0;
}
static int pack_install(void *ctx,const struct dp_plan *p,const char *root,char reason[160])
{ (void)ctx; assert(!p->manifest.enabled && !strcmp(root,"/media")); pack_installs++; strcpy(reason,NDP_DISABLED); return 0; }
static int pack_remove(void *ctx,const struct ndp_device *d,char reason[160])
{ (void)ctx; assert(d->vendor==0x10ec); pack_removes++; strcpy(reason,"Driver package removed. Native driver remains the default."); return 0; }

static int hwid_queries;
static int hwid_lookup(void *ctx, unsigned vendor, unsigned device, char *out, size_t size)
{ (void)ctx; hwid_queries++; return cp_hwid_lookup_json("{\"8086:2415\": \"Pack AC97 device\", \"8086:2485\": \"Intel ICH3 AC97 audio\", \"5333:8c2e\": \"S3 SuperSavage IX/C SDR\"}", vendor, device, out, size); }
static int usb_hwid_lookup(void *ctx, unsigned vendor, unsigned device, char *out, size_t size)
{ (void)ctx; return cp_hwid_lookup_json("{\"1234:0001\": \"USB Pack Keyboard\"}", vendor, device, out, size); }
static int display(void *ctx,struct ciuki_display_info *out) { (void)ctx; display_fixture(out); return 0; }
static int engine(void *ctx,struct ciuki_accel_info *out)
{ (void)ctx; *out=(struct ciuki_accel_info){.driver=CIUKI_ACCEL_DRIVER_LEGACY_NATIVE,.capabilities=3}; return 0; }
static int network(void *ctx,struct ciuki_net_state *out)
{ (void)ctx; *out=(struct ciuki_net_state){.version=CIUKI_NET_VERSION,.size=sizeof(*out),.present=1,.link=CIUKI_NET_LINK_UP}; return 0; }
static int mixer(void *ctx,int *volume,int *muted) { (void)ctx; *volume=70; *muted=0; return 0; }
static int log_error;
static int log_read(void *ctx,uint32_t offset,char *out,size_t cap,size_t *length,uint32_t *total)
{
    (void)ctx; if (log_error) return -ENOENT;
    static const char line[]="Ciuki boot log: device ready\n";
    *total=320u*1024; *length=offset<*total ? *total-offset : 0;
    if (*length>cap) *length=cap;
    for (size_t k=0;k<*length;k++) out[k]=line[(offset+k)%(sizeof(line)-1)];
    return 0;
}
static int action(void *ctx) { (void)ctx; return 0; }
static int autoplay(void *ctx,struct ap_preference *out,unsigned capacity) { (void)ctx; (void)out; (void)capacity; return 0; }
static char boot[128] = "safe=0 serial=1 video=1024x768 input=firmware\n";
static int settings_load(void *c, char *s, size_t cap, size_t *n)
{ (void)c; const char text[] = "version=1\nwallpaper=none\n"; assert(cap >= sizeof(text)); memcpy(s, text, sizeof(text)); *n = sizeof(text) - 1; return 0; }
static int devices(void *ctx, struct ciuki_devices_info *out)
{ (void)ctx; queries++; assert(out->version == CIUKI_DEVICES_VERSION && out->size == sizeof(*out)); *out = sample; return fail_query; }
static int firmware_error, firmware_queries;
static int firmware(void *ctx, struct ciuki_firmware_page *p)
{
    (void)ctx; firmware_queries++;
    assert(p->version == 2 && p->size == sizeof(*p) && !p->reserved && p->cursor <= 4);
    if (firmware_error) return -EAGAIN;
    uint32_t cursor = p->cursor;
    memset(p, 0, sizeof(*p)); p->version = 2; p->size = sizeof(*p); p->cursor = cursor;
    p->generation = 7; p->total = 5; p->count = cursor ? 1 : 4; p->next = cursor ? 0 : 4;
    for (unsigned i = 0; i < p->count; i++) {
        struct ciuki_firmware_record *r = &p->records[i];
        r->id = cursor + i + 1; r->parent = 1; r->origin = CIUKI_FIRMWARE_ACPI;
        r->state = CIUKI_FIRMWARE_DISCOVERED; r->capabilities = CIUKI_FW_CAP_BATTERY;
        snprintf(r->path, sizeof(r->path), "\\_SB_.BAT%u", cursor + i);
        strcpy(r->hid, "PNP0C0A"); strcpy(r->reason, "Firmware description only.");
    }
    return 0;
}
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
    app.host = (struct cp_host){.pack_scan=pack_scan, .pack_preview=pack_preview, .pack_install=pack_install, .pack_remove=pack_remove, .load = settings_load, .devices = devices, .firmware = firmware, .display=display, .engine=engine, .network=network, .mixer=mixer, .power=action, .log_read=log_read, .autoplay_list=autoplay, .hwid_name = hwid_lookup, .usb_hwid_name = usb_hwid_lookup, .boot_load = boot_load, .boot_save = boot_save,
        .portrait = portrait, .screen_w = 1024, .screen_h = 768, .mem_total_k = 512 * 1024, .mem_free_k = 480 * 1024,
        .version = "0.9.0 pre-alpha", .build = "host-preview", .machine = "i686", .processes = 6, .fonts_path = argv[3]};
    du_init(&ui, pixels, CP_WIDTH, CP_HEIGHT); ui.focused_window = 1;
    assert(!cp_attach(&app, &ui, CP_HOME));
    assert(app.page==CP_HOME && kind(DU_TREE)->count==CP_CATEGORIES);
    render(renders,"control-folder");
    unsigned seen[CP_PAGES]={0}, count=0;
    static const char *const page_files[]={"display","mouse","keyboard","sounds","datetime","system","devices","drivers","network","power","adapter","fonts","autoplay","programs","joystick","access"};
    _Static_assert(sizeof(page_files)/sizeof(*page_files)==CP_PAGES,"every applet has a render filename");
    for (int cat=0;cat<CP_CATEGORIES;cat++) {
        du_focus(&ui,CP_CATEGORY); at_tap(&ui,DU_KEY_HOME);
        for (int k=0;k<cat;k++) at_tap(&ui,DU_KEY_DOWN);
        assert(app.category==cat);
        char category_name[64]; snprintf(category_name,sizeof(category_name),"control-category-%d",cat); render(renders,category_name);
        unsigned n=kind(DU_LISTVIEW)->count;
        for (unsigned row=0;row<n;row++) {
            int page=app.applet_pages[row]; assert(!seen[page]++); count++;
            assert(app.applet_items[row].icon16);
            du_focus(&ui,kind(DU_LISTVIEW)->id); at_tap(&ui,DU_KEY_HOME);
            for (unsigned k=0;k<row;k++) at_tap(&ui,DU_KEY_DOWN);
            at_tap(&ui,DU_KEY_ENTER); assert(app.page==page);
            char name[64]; snprintf(name,sizeof(name),"control-%s",page_files[page]); render(renders,name);
            assert(ui.count<DU_WIDGETS); click("Control Panel"); assert(app.page==CP_HOME);
        }
    }
    assert(count==CP_APPLETS && !seen[CP_ADAPTER] && !seen[CP_FONTS] && !seen[CP_DEVICES] && !seen[CP_DRIVERS]);
    const int inspectors[]={CP_DEVICES,CP_DRIVERS,CP_ADAPTER,CP_FONTS};
    for (unsigned k=0;k<4;k++) {
        cp_open(&app,inspectors[k]); char name[64]; snprintf(name,sizeof(name),"control-%s",page_files[inspectors[k]]); render(renders,name);
        click("Control Panel");
    }
    cp_open(&app, CP_DEVICES);
    assert(app.tree_count == 19 && !strcmp(app.tree_items[0].text[0], "Computer"));
    click("Firmware..."); assert(strstr(ui.dialog.text, "PNP0C0A") && strstr(ui.dialog.text, "battery") && strstr(ui.dialog.text, "unknown"));
    render(renders, "firmware-devices");
    for (unsigned i = 0; i < 4; i++) {
        struct du_rect next = du_dialog_button(&ui, 1); at_click(&ui, next.x + next.w / 2, next.y + next.h / 2);
    }
    assert(app.firmware_index == 4 && strstr(ui.dialog.text, "5 of 5") && strstr(ui.dialog.text, "Parent 1"));
    struct du_rect previous = du_dialog_button(&ui, 0); at_click(&ui, previous.x + previous.w / 2, previous.y + previous.h / 2);
    assert(app.firmware_index == 3 && firmware_queries == 6); at_tap(&ui, DU_KEY_ESC);
    firmware_error = 1; click("Firmware..."); assert(strstr(ui.dialog.text, "invalid") || strstr(ui.dialog.text, "No firmware"));
    at_tap(&ui, DU_KEY_ESC); firmware_error = 0;
    for (unsigned k = 0; k < sample.count; k++) assert(app.tree_items[tree_row(k)].depth == 2 && app.tree_items[tree_row(k)].icon16);
    struct ciuki_device_record usb_sample = sample.devices[0];
    usb_sample.bus_type = CIUKI_BUS_USB; usb_sample.vendor = 0x1234; usb_sample.device = 1;
    struct ciuki_device_record original_usb_sample = sample.devices[0];
    sample.devices[0] = usb_sample; click("Refresh");
    assert(strstr(app.tree_items[tree_row(0)].text[0], "USB Pack Keyboard"));
    sample.devices[0] = original_usb_sample; click("Refresh");
    /* Driver-pack dialogs use the production controls and keyboard path. */
    assert(du_get(&ui,CP_PACK_INSTALL)->disabled);
    du_post(&ui,DU_EV_SELECT,kind(DU_TREE)->id,(int)tree_row(7));
    assert(!du_get(&ui,CP_PACK_INSTALL)->disabled);
    click("Install driver from pack..."); assert(du_dialog_id(&ui)==CP_PACK_LOCATION);
    render(renders,"driver-pack-location");
    at_tap(&ui,DU_KEY_ENTER); assert(du_dialog_id(&ui)==CP_PACK_PREVIEW && strstr(ui.dialog.text,"Cannot install"));
    render(renders,"driver-pack-rejected");
    at_tap(&ui,DU_KEY_ENTER); assert(strstr(ui.dialog.text,"Synthetic Adapter") && strstr(ui.dialog.text,"support.dat"));
    assert(strstr(ui.dialog.text,"No lead qualification record")); render(renders,"driver-pack-preview");
    struct du_rect files_button=du_dialog_button(&ui,1); at_click(&ui,files_button.x+files_button.w/2,files_button.y+files_button.h/2);
    assert(du_dialog_id(&ui)==CP_PACK_FILES && strstr(ui.dialog.text,"mini.sys")); render(renders,"driver-pack-files"); at_tap(&ui,DU_KEY_ESC);
    struct du_rect install_button=du_dialog_button(&ui,0); at_click(&ui,install_button.x+install_button.w/2,install_button.y+install_button.h/2);
    assert(pack_installs==1 && strstr(ui.dialog.text,"installed disabled")); render(renders,"driver-pack-installed"); at_tap(&ui,DU_KEY_ESC);
    click("Remove driver package..."); render(renders,"driver-pack-remove");
    struct du_rect remove_button=du_dialog_button(&ui,0); at_click(&ui,remove_button.x+remove_button.w/2,remove_button.y+remove_button.h/2);
    assert(pack_removes==1 && strstr(ui.dialog.text,"removed")); render(renders,"driver-pack-removed"); at_tap(&ui,DU_KEY_ESC);
    sample.devices[7].state=CIUKI_DEVICE_QUARANTINED; click("Refresh"); assert(du_get(&ui,CP_PACK_INSTALL)->disabled); sample.devices[7].state=CIUKI_DEVICE_FIRMWARE_FIRST; click("Refresh");
    unsigned audio_row = tree_row(6); assert(strstr(app.tree_items[audio_row].text[0], "Pack AC97 device") && hwid_queries > 0);
    assert(strstr(app.tree_items[audio_row].text[0], "quarantined"));
    sample.devices[6].device = 0x2485; click("Refresh");
    audio_row = tree_row(6); assert(strstr(app.tree_items[audio_row].text[0], "Intel ICH3 AC97 audio"));
    sample.devices[6].device = 0x2415; click("Refresh");
    audio_row = tree_row(6);
    struct du_widget *tree = kind(DU_TREE); tree->active = audio_row; du_post(&ui, DU_EV_SELECT, tree->id, (int)audio_row);
    click("Properties..."); assert(du_dialog_id(&ui) && strstr(ui.dialog.text, "Quarantined") && strstr(ui.dialog.text, "8086:2415"));
    assert(strstr(ui.dialog.text, "ac97") && strstr(ui.dialog.text, "Generation: 16")); render(renders, "device-properties-general");
    at_tap(&ui, DU_KEY_ESC);
    strcpy(sample.devices[6].driver_display_name,"Recording adapter driver"); click("Refresh");
    click("Properties..."); assert(strstr(ui.dialog.text,"Driver: Recording adapter driver"));
    sample.devices[6].driver_display_name[0]=0; /* keep the existing render fixtures */
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
    tab(1); render(renders, "system-hardware"); assert(app.page==CP_DEVICES && kind(DU_TREE) && app.tree_count==19);
    tab(2); render(renders, "system-advanced"); assert(kind(DU_CHECKBOX)->value == 1);
    at_click_widget(&ui, kind(DU_CHECKBOX)->id); assert(app.serial_next == 0);
    /* A different writer updated the boot file while the page was open. */
    strcpy(boot, "safe=1 serial=1 video=800x600 extra=keep\n");
    click("Apply to BOOT.CFG"); assert(saves == 1 && strstr(boot, "serial=0") && strstr(boot, "video=800x600 extra=keep"));
    assert(sample.boot_flags == 2 && find(DU_LABEL, "Saved. Serial logging changes at the next boot."));
    fail_save = 1; click("Apply to BOOT.CFG"); assert(find(DU_LABEL, "BOOT.CFG could not be saved."));
    strcpy(boot, "serial=0 serial=1"); tab(2); assert(!kind(DU_CHECKBOX)->value);
    strcpy(boot, "safe=0"); tab(2); assert(kind(DU_CHECKBOX)->value);
    assert(app.log.readonly && app.log.len==CP_LOG_BYTES && app.log_total==320u*1024 && !app.log_offset);
    at_click_widget(&ui,CP_LOG_NEXT); assert(app.log_offset==CP_LOG_BYTES);
    at_click_widget(&ui,CP_LOG_PREV); assert(!app.log_offset);
    du_focus(&ui,CP_LOG_VIEW); size_t length=app.log.len;
    at_tap(&ui,0x1e); assert(app.log.len==length && !app.log.modified);
    log_error=1; at_click_widget(&ui,CP_OPEN_LOG); assert(!app.log.len && strstr(app.log_status,"No such file"));
    log_error=0; at_click_widget(&ui,CP_OPEN_LOG); assert(app.log.len==CP_LOG_BYTES);
    assert(loads >= 3); cp_detach(&app);
    /* Driver names and readiness are metadata, including software-only boots. */
    device_fixture(&sample);
    sample.devices[0].id=CIUKI_DEVICE_ACCELERATOR_ID;
    strcpy(sample.devices[0].driver_display_name,"Recording display driver");
    strcpy(sample.devices[0].reason,"none"); sample.devices[0].accel_ready=1;
    cp_engine_refresh(&app); assert(strstr(app.engine_line,"Recording display driver: active"));
    sample.devices[0].state=CIUKI_DEVICE_QUARANTINED; strcpy(sample.devices[0].reason,"engine_timeout");
    cp_engine_refresh(&app); assert(strstr(app.engine_line,"quarantined (engine_timeout)"));
    sample.devices[0].state=CIUKI_DEVICE_READY; sample.devices[0].accel_ready=0;
    cp_engine_refresh(&app); assert(strstr(app.engine_line,"Recording display driver: software"));
    puts("PASS device manager: nine categorized applets, keyboard navigation, bounded class tree, refresh, Properties, drivers/leases, system and preserving BOOT.CFG edits");
    return 0;
}
