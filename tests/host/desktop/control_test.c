/* The Control Panel's Display page on the host: the five wallpaper Position
 * buttons (Center, Tile, Stretch, Fit, Fill), their layout beside the other
 * Display widgets, the desktop.cfg mode they save, and the preview of the
 * chosen mode (desk_wallpaper_render). Never guest evidence.
 *   control_test PORTRAIT.xrgb WALLPAPER_DIR [RENDER_DIR]
 * WALLPAPER_DIR holds Ciuk1..3.png (assets/wallpapers/ciuki), listed as
 * /system/wallpapers like the image installs them.
 * SPDX-License-Identifier: MIT */
#include "control.h"
#include "acc_test.h"
#include "display_fixture.h"

static int autoplay_count=2, autoplay_error;
static int autoplay_list(void *ctx,struct ap_preference *out,unsigned cap)
{
    (void)ctx; assert(cap>=2); if (autoplay_error) return -1;
    out[0]=(struct ap_preference){.id="fat-12345678-100",.label="Music stick",.action=AP_MUSIC};
    out[1]=(struct ap_preference){.id="iso-1234567890123456",.label="Games CD",.action=AP_NOTHING};
    return autoplay_count;
}
static int autoplay_forget(void *ctx,const char *id)
{ (void)ctx; assert(!strcmp(id,"iso-1234567890123456")); autoplay_count--; return 0; }
static char saved[CFG_TEXT_MAX]; static size_t saved_n; static unsigned saves, applies, decodes; static const char *folder;
static int save_error;
static int load(void *c, char *t, size_t s, size_t *n) { (void)c; if (!saved_n || saved_n > s) return -1; memcpy(t, saved, saved_n); *n = saved_n; return 0; }
static int save(void *c, const char *t, size_t n) { (void)c; if (save_error || n >= sizeof(saved)) return -1; memcpy(saved, t, n); saved[n] = 0; saved_n = n; saves++; return 0; }
static int apply(void *c, unsigned r, int v, int m) { (void)c; (void)v; (void)m; assert(r == DESK_SETTINGS_RELOAD); applies++; return 0; }
static int mixer_volume=70;
static int mixer(void *c, int *v, int *m) { (void)c; *v = mixer_volume; *m = 0; return 0; }
static unsigned master_applies;
static int master_apply(void *c,unsigned request,int volume,int muted)
{ (void)c;assert(request==DESK_SETTINGS_MASTER && volume==20 && muted==1);master_applies++;return 0; }
static uint64_t now(void *c) { (void)c; return 1791763200ull + 20 * 3600 + 46 * 60 + 12; }
static int display(void *c, struct ciuki_display_info *d) { (void)c; display_fixture(d); return 0; }
static int wallpapers(void *c, char names[][CFG_PATH_MAX], unsigned capacity)
{
    (void)c; assert(capacity >= 3);
    for (int k = 0; k < 3; k++) snprintf(names[k], CFG_PATH_MAX, "%s/Ciuk%d.png", CFG_WALLPAPER_DIR, k + 1);
    return 3;
}
static int decode(void *c, const char *path, struct desk_image *out)
{
    (void)c;
    assert(!strncmp(path, CFG_WALLPAPER_DIR "/", sizeof(CFG_WALLPAPER_DIR)));
    char host[1024]; snprintf(host, sizeof(host), "%s/%s", folder, path + sizeof(CFG_WALLPAPER_DIR));
    FILE *f = fopen(host, "rb"); assert(f && !fseek(f, 0, SEEK_END));
    long size = ftell(f); assert(size > 0 && !fseek(f, 0, SEEK_SET));
    uint8_t *data = malloc((size_t)size); assert(data && fread(data, 1, (size_t)size, f) == (size_t)size); fclose(f);
    int r = desk_image_decode(data, (size_t)size, out);
    free(data); decodes++;
    return r;
}

static uint32_t portrait[256 * 256], pixels[CP_WIDTH * CP_HEIGHT];
static struct cp_internet_options internet_saved;
static unsigned internet_saves,internet_defaulted,internet_programs_opened,internet_cleared;
static int internet_default_program(void *ctx) { (void)ctx;internet_defaulted++;return 0; }
static int internet_programs_open(void *ctx) { (void)ctx;internet_programs_opened++;return 0; }
static int internet_clear(void *ctx) { (void)ctx;internet_cleared++;return 0; }
static int internet_read(void *ctx,struct cp_internet_options *o)
{ (void)ctx;internet_defaults(o);return 0; }
static int internet_write(void *ctx,const struct cp_internet_options *o)
{ (void)ctx;assert(internet_valid(o));internet_saved=*o;internet_saves++;return 0; }
static struct cp_app a; static struct du_ui u;
static const char *const labels[5] = {"Center", "Tile", "Stretch", "Fit", "Fill"};
static const int modes[5] = {CFG_WALL_CENTER, CFG_WALL_TILE, CFG_WALL_STRETCH, CFG_WALL_FIT, CFG_WALL_FILL};

static void attach(int page)
{
    memset(&a, 0, sizeof(a));
    a.host = (struct cp_host){.load = load, .save = save, .wallpapers = wallpapers, .decode = decode, .mixer = mixer,
                              .apply = apply, .now = now, .display = display, .portrait = portrait, .screen_w = 1024, .screen_h = 768,
                              .version = "0.9.0-prealpha", .build = "host-preview", .machine = "i686",
                              .mem_total_k = 511u * 1024, .mem_free_k = 480u * 1024, .processes = 6};
    du_init(&u, pixels, CP_WIDTH, CP_HEIGHT);
    u.focused_window = 1;
    assert(!cp_attach(&a, &u, page));
}
static struct du_widget *find(enum du_kind kind, const char *text)
{
    struct du_widget *found = NULL;
    for (unsigned i = 0; i < u.count; i++)
        if (u.widgets[i].kind == kind && !strcmp(u.widgets[i].text, text)) { assert(!found); found = &u.widgets[i]; }
    return found;
}
static int overlap(struct du_rect p, struct du_rect q)
{ return p.x < q.x + q.w && q.x < p.x + p.w && p.y < q.y + q.h && q.y < p.y + p.h; }
static int inside(struct du_rect p, struct du_rect outer)
{ return p.x >= outer.x && p.y >= outer.y && p.x + p.w <= outer.x + outer.w && p.y + p.h <= outer.y + outer.h; }
/* Exactly the button of the given mode is marked as the current one. */
static void check_marked(int mode)
{
    for (int k = 0; k < 5; k++) assert(find(DU_BUTTON, labels[k])->def == (modes[k] == mode));
}
/* The preview of an image wallpaper is decoded after the first paint (timer): render, tick, render. */
static void render_settled(struct du_ui *u) { du_render(u); du_tick(u, u->now + 1000000000ull); du_render(u); }
/* An image wallpaper is rendered straight at preview size (bars 32*144/768 =
 * 6 px, decoded image cached in the app); any other spec is the full 1024x768
 * render of the chosen mode scaled to 192x144. */
static void check_preview(void)
{
    assert(a.preview_valid);
    static uint32_t full[1024 * 768], expected[192 * 144];
    struct desk_image image;
    assert(!decode(NULL, a.cfg.wallpaper, &image));
    uint32_t colour = desk_theme_named(a.cfg.theme)->desktop;
    if (a.cfg.wallpaper[0] == '/') {
        assert(!desk_wallpaper_render(expected, 192, 144, 6, 6, a.cfg.wallpaper, a.cfg.wallpaper_mode, colour, &image, portrait));
        assert(a.image.pixels && !strcmp(a.image_path, a.cfg.wallpaper));
    } else {
        assert(!desk_wallpaper_render(full, 1024, 768, 32, 32, a.cfg.wallpaper, a.cfg.wallpaper_mode, colour, &image, portrait));
        for (size_t i = 0; i < 1024 * 768; i++) full[i] |= 0xff000000u;
        struct desk_image src = {full, 1024, 768};
        desk_image_scale(&src, expected, 192, 144, CFG_WALL_STRETCH, colour);
    }
    desk_image_free(&image);
    assert(!memcmp(expected, a.preview, sizeof(expected)));
}
/* Preview column 0 at mid-height: Fit leaves desktop-colour bands beside a
 * 4:3 scene in the 1024x704 work area, Fill covers it. */
static int preview_band(void) { return a.preview[72 * 192] == desk_theme_named(a.cfg.theme)->desktop; }

static unsigned program_removes,program_changes,program_installs,program_overflow;
static int installed_programs(void *ctx,struct desk_uninstall_entry *out,unsigned cap)
{
    (void)ctx;assert(cap>=2);
    memset(out,0,2*sizeof(*out));strcpy(out[0].name,"DOOM (shareware)");out[0].kind=DESK_PROGRAM_GAME;out[0].game=2;
    strcpy(out[0].version,"1.9");strcpy(out[0].publisher,"id Software");out[0].disk_bytes=8192;out[0].inventory_more=program_overflow;
    strcpy(out[1].name,"Desktop");out[1].no_remove=1;out[1].system_component=1;return 2;
}
static int remove_program(void *ctx,const struct desk_uninstall_entry *r)
{(void)ctx;assert(r->game==2);program_removes++;return 0;}
static int install_program(void *ctx,const char *path)
{(void)ctx;assert(!strcmp(path,"/mnt/d/SETUP.EXE"));program_installs++;return 0;}
static void programs_changed(void *ctx){(void)ctx;program_changes++;}
static void programs_page(void)
{
    attach(CP_HOME);
    a.host.uninstall_list=installed_programs;a.host.program_remove=remove_program;
    a.host.program_install=install_program;a.host.programs_changed=programs_changed;
    cp_open(&a,CP_UNINSTALL);assert(a.uninstall_count==2 && du_get(&u,982)->column_count==4);
    assert(!strcmp(a.uninstall_items[0].text[1],"1.9") && !strcmp(a.uninstall_items[0].text[2],"id Software"));
    at_click_widget(&u,983);
    assert(!program_removes && du_dialog_id(&u)==987 && u.dialog.cancel==1);
    assert(strstr(u.dialog.text,"DOOM") && strstr(u.dialog.text,"cannot be restored without reinstalling the system"));
    at_tap(&u,DU_KEY_ESC);assert(!program_removes);
    at_click_widget(&u,983);
    struct du_rect b=du_dialog_button(&u,0);at_click(&u,b.x+b.w/2,b.y+b.h/2);
    assert(program_removes==1 && program_changes==1 && strstr(du_get(&u,3)->text,"Program removed"));
    struct du_event select={.type=DU_EV_SELECT,.id=982,.index=1};u.event(&u,&select,u.user);
    assert(du_get(&u,983)->disabled);at_click_widget(&u,983);assert(!du_dialog_id(&u) && program_removes==1);
    at_click_widget(&u,986);assert(du_dialog_id(&u)==988);
    du_dialog_close(&u);struct du_event chosen={.type=DU_EV_DIALOG,.id=988,.index=0,.text="/mnt/d/SETUP.EXE"};u.event(&u,&chosen,u.user);
    assert(program_installs==1);
    program_overflow=1;cp_open(&a,CP_UNINSTALL);
    assert(a.uninstall_count==2 && strstr(du_get(&u,985)->text,"More programs exist"));
    program_overflow=0;
    cp_detach(&a);du_release(&u);
}
static struct ciuki_joystick_control joystick_pending;
static unsigned joystick_sent, joystick_game_saved;
static struct ciuki_joystick_control joystick_game_record;
static int joystick_save(void *ctx,unsigned game,const struct ciuki_joystick_control *c)
{ (void)ctx;joystick_game_saved=game;joystick_game_record=*c;return 0; }
static int joystick_submit(void *ctx,const struct ciuki_joystick_control *c)
{ (void)ctx;joystick_pending=*c;joystick_sent++;return 0; }
static void joystick_page(void)
{
    attach(CP_HOME);a.host.joystick=joystick_submit;a.host.joystick_save_game=joystick_save;cp_open(&a,CP_DEVICES);
    a.devices_ok=1;a.devices.count=1;a.properties_device=0;
    a.devices.devices[0]=(struct ciuki_device_record){.class_id=CIUKI_DEVICE_PORT,.state=CIUKI_DEVICE_READY};
    strcpy(a.devices.devices[0].driver,"gameport");
    struct du_event open={.type=DU_EV_DIALOG,.id=200,.index=0};u.event(&u,&open,u.user);
    assert(a.page==CP_JOYSTICK);
    assert(joystick_pending.operation==CIUKI_JOY_NEXT && a.joystick_busy);
    struct ciuki_joystick_control reply=joystick_pending;
    reply.device=257;reply.generation=42;reply.session=123456;reply.axes=3;reply.attached=1;
    cp_joystick_result(&a,&reply,0);assert(!a.joystick_busy && strstr(du_get(&u,1121)->text,"Analog"));
    at_click_widget(&u,1101);assert(a.joystick_calibrating==1);
    for (unsigned phase=0;phase<3;phase++) {
        unsigned before=joystick_sent;at_click_widget(&u,1102);
        assert(joystick_sent==before+1 && joystick_pending.operation==CIUKI_JOY_QUERY && a.joystick_capture_pending);
        reply=joystick_pending;reply.axes=3;reply.attached=1;reply.session=123456;
        reply.raw[0]=10+90*(int)phase;reply.raw[1]=20+180*(int)phase;
        cp_joystick_result(&a,&reply,0);
    }
    assert(joystick_pending.operation==CIUKI_JOY_CALIBRATE && joystick_pending.axis==0 &&
        joystick_pending.minimum==10 && joystick_pending.centre==100 && joystick_pending.maximum==190);
    reply=joystick_pending;cp_joystick_result(&a,&reply,0);
    assert(joystick_pending.axis==1 && joystick_pending.minimum==20 && joystick_pending.maximum==380);
    reply=joystick_pending;cp_joystick_result(&a,&reply,0);assert(!a.joystick_calibrating);
    at_click_widget(&u,1103);assert(joystick_pending.operation==CIUKI_JOY_ROUTE && joystick_pending.device==257);
    reply=joystick_pending;cp_joystick_result(&a,&reply,0);
    at_click_widget(&u,1100);reply=joystick_pending;reply.device=258;reply.generation=43;reply.session=123456;reply.axes=3;reply.attached=1;
    cp_joystick_result(&a,&reply,0);at_click_widget(&u,1124);
    assert(joystick_pending.mode==1 && joystick_pending.device==257 && joystick_pending.second_device==258);
    reply=joystick_pending;cp_joystick_result(&a,&reply,-ENODEV);assert(!a.joystick_busy);
    at_click_widget(&u,1104);assert(!joystick_pending.device && !joystick_pending.generation);
    reply=joystick_pending;cp_joystick_result(&a,&reply,0);
    /* f2-125: per-game joystick/COM/LPT choices moved to the shared Game
     * properties view (game_properties_test.c covers its saves and frames). */
    assert(!du_get(&u,1107) && du_get(&u,1106));
    if (a.joystick_busy) { reply=joystick_pending;cp_joystick_result(&a,&reply,0); } /* the page's own query */
    /* The DOS route persists by controller identity; the panel waits for it. */
    at_click_widget(&u,1120);assert(!a.game_open && a.joystick_save_pending);
    cp_tick(&a);assert(joystick_pending.operation==CIUKI_JOY_SAVE_STATUS && a.joystick_busy);
    reply=joystick_pending;reply.buttons=1;cp_joystick_result(&a,&reply,0);assert(a.joystick_save_pending);
    cp_tick(&a);reply=joystick_pending;cp_joystick_result(&a,&reply,0);
    assert(!a.joystick_save_pending && !a.joystick_busy && strstr(du_get(&u,3)->text,"saved for future sessions"));
    at_click_widget(&u,1120);
    assert(a.game_open && joystick_pending.operation==CIUKI_JOY_GET_GAME && joystick_pending.axis==1 && a.joystick_busy);
    reply=joystick_pending;reply.attached=0;cp_joystick_result(&a,&reply,0);
    assert(!a.joystick_busy && a.game_joy_ready && strstr(du_get(&u,2200)->text,"Game: ") && strstr(du_get(&u,2201)->text,"DOS default"));
    at_click_widget(&u,2210);assert(!a.game_open && du_get(&u,1121) && !joystick_game_saved && !joystick_game_record.device);
    assert(cp_page_index("joystick")==CP_JOYSTICK);
    cp_detach(&a);du_release(&u);
    puts("PASS joystick Control Panel: discovery, fresh three-point capture, per-axis calibration, A/B/off, stale refusal and the per-game properties entry");
}
static int power_fixture(void *ctx, struct ciuki_power_status *p)
{
    (void)ctx; memset(p,0xff,sizeof(*p)); p->version=1; p->size=sizeof(*p);
    p->ownership=CIUKI_POWER_FIRMWARE; p->error=0; p->battery_count=1; p->zone_count=0;
    p->batteries[0].id=7; p->batteries[0].present=1; return 0;
}
/* APM 1.2 provider record (power_source.c apm_record): one percent battery. */
static int apm_fixture(void *ctx, struct ciuki_power_status *p)
{
    (void)ctx; memset(p,0xff,sizeof(*p)); p->version=1; p->size=sizeof(*p);
    p->provider=CIUKI_POWER_PROVIDER_APM; p->ownership=CIUKI_POWER_OWNED; p->error=0; p->reserved=0;
    p->ac_online=CIUKI_POWER_AC_BACKUP; p->battery_count=1; p->zone_count=0;
    p->batteries[0].id=1; p->batteries[0].present=1; p->batteries[0].unit=CIUKI_POWER_UNIT_PERCENT;
    p->batteries[0].full=100; p->batteries[0].remaining=40; p->batteries[0].seconds=3720; p->batteries[0].state=5; return 0;
}
/* fx-18: the T23 under APM: main battery discharging, UltraBay empty. */
static int apm_t23_fixture(void *ctx, struct ciuki_power_status *p)
{
    (void)apm_fixture(ctx,p); p->ac_online=0; p->battery_count=2;
    p->batteries[0].remaining=75; p->batteries[0].seconds=5400; p->batteries[0].state=1;
    memset(&p->batteries[1],0xff,sizeof(p->batteries[1])); p->batteries[1].id=2; p->batteries[1].present=0; return 0;
}
/* G11 Power Management (apps/control/power.inc ids). */
enum { PW_TABS=1850, PW_SCHEME=1852, PW_SAVE_AS, PW_DELETE, PW_GROUP, PW_HEAD_AC, PW_HEAD_DC, PW_NOTE=1861, PW_SOURCE,
       PW_CELL=1870, PW_LOW=1881, PW_LOW_LEVEL, PW_LOW_VALUE, PW_LOW_ACTION=1887, PW_CRIT_LEVEL=1892, PW_CRIT_ACTION=1897,
       PW_METER_SOURCE=1900, PW_METER_TOTAL, PW_DETAILS, PW_SHOW_METER=1905, PW_PASSWORD, PW_BUTTON=1910, PW_LID=1912,
       PW_ADV_NOTE, PW_SAVE_NAME=1915, PW_DELETE_CONFIRM, PW_ITEM=1920 };
static struct ciuki_power_events power_events_record;
static int power_events_ok;
static int power_events(void *ctx, struct ciuki_power_events *e)
{ (void)ctx; if (!power_events_ok) return -ENOSYS; *e=power_events_record; return 0; }
static void power_emit(int type, int id, int index, const char *text)
{ struct du_event e={.type=type,.id=id,.index=index,.text=text}; u.event(&u,&e,u.user); }
/* The open dropdown's item whose label is `label`, chosen by the user. */
static void power_pick(const char *label)
{
    assert(u.popup && u.popup_count);
    for (unsigned k=0;k<u.popup_count;k++) if (!strcmp(u.popup[k].label,label)) {
        int id=u.popup[k].id, owner=u.popup_owner; du_menu_close(&u); power_emit(DU_EV_ACTIVATE,id,owner,NULL); return;
    }
    assert(!"popup item");
}
static int popup_has(const char *label)
{ for (unsigned k=0;k<u.popup_count;k++) if (!strcmp(u.popup[k].label,label)) return 1; return 0; }
static void power_policy_page(void)
{
    saved_n=0; saves=applies=0;
    power_events_record=(struct ciuki_power_events){.version=1,.size=sizeof(power_events_record),.standby_reason=CIUKI_POWER_STANDBY_NO_RESUME};
    power_events_ok=1;
    /* A desktop (no battery bay): Power Schemes, Power Meter, Advanced. */
    attach(CP_HOME); a.host.power_status=power_fixture; a.host.power_events=power_events;
    struct ciuki_power_status none; (void)power_fixture(NULL,&none);
    a.host.power_status=NULL; cp_open(&a,CP_POWER);
    struct du_widget *tabs=du_get(&u,PW_TABS);
    assert(tabs && tabs->count==3 && !strcmp(tabs->items[0].text[0],"Power Schemes") && !strcmp(tabs->items[1].text[0],"Power Meter") &&
           !strcmp(tabs->items[2].text[0],"Advanced") && !tabs->active);
    assert(!strcmp(du_get(&u,PW_SCHEME)->text,"Home/Office Desk") && du_get(&u,PW_DELETE)->disabled && !du_get(&u,PW_HEAD_DC));
    assert(!strcmp(du_get(&u,PW_CELL+PP_MONITOR)->text,"After 15 mins") && du_get(&u,PW_CELL+PP_STANDBY)->disabled);
    assert(strstr(du_get(&u,PW_NOTE)->text,"cannot resume yet"));
    /* Turn off monitor: After 25 mins, saved as the user's scheme. */
    at_click_widget(&u,PW_CELL+PP_MONITOR); assert(u.popup_count==16 && popup_has("Never") && popup_has("After 5 hours"));
    power_pick("After 25 mins");
    assert(saves==1 && applies==1 && strstr(saved,"power_scheme=0\n") && strstr(saved,"power_scheme0=Home/Office Desk,1200,1500,0,1200,900,0\n"));
    assert(!strcmp(du_get(&u,PW_CELL+PP_MONITOR)->text,"After 25 mins"));
    /* Save As, then Delete (built-ins cannot be deleted). */
    at_click_widget(&u,PW_SAVE_AS); assert(du_dialog_id(&u)==PW_SAVE_NAME);
    du_dialog_close(&u); power_emit(DU_EV_DIALOG,PW_SAVE_NAME,0,"Always On");   /* OK closes the box */
    assert(saves==1 && strstr(du_get(&u,3)->text,"Type a new name"));
    power_emit(DU_EV_DIALOG,PW_SAVE_NAME,0,"Presentation");
    assert(saves==2 && !strcmp(du_get(&u,PW_SCHEME)->text,"Presentation") && !du_get(&u,PW_DELETE)->disabled);
    assert(strstr(saved,"power_scheme=3\n") && strstr(saved,"power_scheme3=Presentation,1200,1500,0,1200,900,0\n"));
    at_click_widget(&u,PW_SCHEME); assert(u.popup_count==4); power_pick("Always On");
    assert(saves==3 && !strcmp(du_get(&u,PW_SCHEME)->text,"Always On") && !strcmp(du_get(&u,PW_CELL+PP_DISKS)->text,"Never"));
    at_click_widget(&u,PW_SCHEME); power_pick("Presentation");
    at_click_widget(&u,PW_DELETE); assert(du_dialog_id(&u)==PW_DELETE_CONFIRM);
    du_dialog_close(&u); power_emit(DU_EV_DIALOG,PW_DELETE_CONFIRM,0,NULL);
    assert(saves==5 && !strstr(saved,"Presentation") && strstr(saved,"power_scheme=auto\n"));
    /* Advanced: the meter, the password (G32), the button; no lid on a desktop with unknown lid. */
    power_emit(DU_EV_SELECT,PW_TABS,2,NULL);
    assert(du_get(&u,PW_SHOW_METER)->value && !du_get(&u,PW_PASSWORD)->disabled && !du_get(&u,PW_LID));
    at_click_widget(&u,PW_BUTTON); assert(u.popup_count==1 && popup_has("Shut down")); du_menu_close(&u);
    at_click_widget(&u,PW_SHOW_METER); assert(saves==6 && strstr(saved,"power_meter=0\n"));
    cp_detach(&a);
    /* The T23 shape: battery bays add Alarms; Stand by where qualified. */
    power_events_record.standby_reason=CIUKI_POWER_STANDBY_AVAILABLE;
    attach(CP_HOME); a.host.power_status=apm_t23_fixture; a.host.power_events=power_events; cp_open(&a,CP_POWER);
    tabs=du_get(&u,PW_TABS); assert(tabs->count==4 && !strcmp(tabs->items[1].text[0],"Alarms"));
    assert(!strcmp(du_get(&u,PW_SCHEME)->text,"Portable/Laptop") && du_get(&u,PW_HEAD_DC) && !du_get(&u,PW_CELL+PP_STANDBY)->disabled);
    assert(!strcmp(du_get(&u,PW_CELL+PP_TIMERS+PP_MONITOR)->text,"After 2 mins") && strstr(du_get(&u,PW_SOURCE)->text,"runs on batteries"));
    power_emit(DU_EV_SELECT,PW_TABS,1,NULL);
    assert(du_get(&u,PW_LOW)->value && du_get(&u,PW_LOW_LEVEL)->value==10 && !strcmp(du_get(&u,PW_LOW_VALUE)->text,"10%"));
    at_click_widget(&u,PW_CRIT_ACTION); assert(u.popup_count==3 && popup_has("Stand by")); power_pick("Shut down");
    assert(strstr(saved,"power_alarm_critical=1,3,1,1,2,0\n"));
    du_get(&u,PW_LOW_LEVEL)->value=2; power_emit(DU_EV_CHANGE,PW_LOW_LEVEL,1,NULL);
    assert(strstr(saved,"power_alarm_low=1,2,0,1,0,0\n") && strstr(saved,"power_alarm_critical=1,1,1,1,2,0\n"));
    assert(du_get(&u,PW_CRIT_LEVEL)->value==1);   /* critical moved below low */
    power_emit(DU_EV_SELECT,PW_TABS,3,NULL);
    assert(!du_get(&u,PW_PASSWORD)->disabled);
    at_click_widget(&u,PW_PASSWORD); assert(strstr(saved,"power_password=1\n"));
    at_click_widget(&u,PW_PASSWORD); assert(strstr(saved,"power_password=0\n"));
    at_click_widget(&u,PW_BUTTON); assert(u.popup_count==2); power_pick("Stand by"); assert(strstr(saved,"power_button=standby\n"));
    at_click_widget(&u,PW_LID); assert(u.popup_count==3); power_pick("Stand by"); assert(strstr(saved,"power_lid=standby\n"));
    assert(!strcmp(du_get(&u,PW_ADV_NOTE)->text,"This computer can stand by."));
    /* Stand by withdrawn: the choices fall back to Shut down on screen. */
    power_events_record.standby_reason=CIUKI_POWER_STANDBY_NO_FIRMWARE; cp_tick(&a);
    assert(!strcmp(du_get(&u,PW_BUTTON)->text,"Shut down") && !strcmp(du_get(&u,PW_LID)->text,"Shut down"));
    assert(strstr(du_get(&u,PW_ADV_NOTE)->text,"no standby state"));
    /* Power Meter: the source and the total. */
    power_emit(DU_EV_SELECT,PW_TABS,2,NULL);
    assert(!strcmp(du_get(&u,PW_METER_SOURCE)->text,"Power source: Batteries"));
    assert(!strcmp(du_get(&u,PW_METER_TOTAL)->text,"Total battery power remaining: 75% (1 hr 30 min)"));
    at_click_widget(&u,PW_DETAILS); assert(!du_get(&u,PW_DETAILS)->value && !du_get(&u,1800)->text[0]);
    cp_detach(&a);
    /* The tray's Open Power Meter. */
    assert(cp_page_index("power-meter")==CP_POWER);
    attach(CP_HOME); a.host.power_status=apm_t23_fixture; cp_open(&a,CP_POWER);
    assert(du_get(&u,PW_METER_SOURCE) && du_get(&u,PW_TABS)->active==2);
    power_events_ok=0; cp_tick(&a); power_emit(DU_EV_SELECT,PW_TABS,3,NULL);
    assert(strstr(du_get(&u,PW_ADV_NOTE)->text,"does not report"));
    cp_detach(&a);
    puts("PASS power Control Panel G11: Windows 98 tabs (Alarms with a battery bay), scheme dropdown, timers saved to desktop.cfg, Save As/Delete, alarms and levels, button and lid choices, standby availability, Power Meter, Open Power Meter");
}
static void power_page(void)
{
    assert(cp_page_index("power-meter")==CP_POWER);   /* the status lines are on the Power Meter tab */
    attach(CP_HOME); a.host.power_status=power_fixture; cp_open(&a,CP_POWER);
    assert(strstr(du_get(&u,1800)->text,"AC: Unknown") && strstr(du_get(&u,1800)->text,"Lid: Unknown"));
    assert(strstr(du_get(&u,1801)->text,"Firmware retained") && !strstr(du_get(&u,1801)->text,"APM"));
    assert(strstr(du_get(&u,1802)->text,"Unknown / Unknown") && strstr(du_get(&u,1802)->text,"units unknown"));
    a.host.power_status=apm_fixture; cp_tick(&a);
    assert(strstr(du_get(&u,1800)->text,"AC: Backup power") && strstr(du_get(&u,1800)->text,"Lid: Unknown"));
    assert(strstr(du_get(&u,1801)->text,"CiukiOS (APM 1.2)"));
    assert(strstr(du_get(&u,1802)->text,"Discharging; capacity 40 / 100 %"));
    assert(!strcmp(du_get(&u,1803)->text,"Remaining life: 1 h 02 min; critical"));
    a.host.power_status=apm_t23_fixture; cp_tick(&a);
    assert(strstr(du_get(&u,1800)->text,"AC: Disconnected") && strstr(du_get(&u,1802)->text,"Battery 1: Discharging; capacity 75 / 100 %"));
    assert(!strcmp(du_get(&u,1803)->text,"Remaining life: 1 h 30 min") && !strcmp(du_get(&u,1804)->text,"Battery 2: Not installed"));
    a.host.power_status=NULL; (void)cp_page_index("power-meter"); cp_open(&a,CP_POWER);
    assert(!strcmp(du_get(&u,1800)->text,"Power status: Unknown"));
    assert(!du_get(&u,1802)->text[0]); cp_detach(&a);
    puts("PASS power Control Panel: firmware ownership, AC/lid/battery unknowns, APM percent/life refresh, T23 empty bay, stale-value clearing");
}
/* G32: actual applet actions and secret-only controller fields. */
static struct users_request user_pending;
static struct users_view user_view;
static int user_ready, user_error;
static int user_begin(void *ctx,const struct users_request *q)
{ (void)ctx; user_pending=*q; user_ready=1; return 0; }
static int user_poll(void *ctx,struct users_view *v,int *error)
{ (void)ctx; if (!user_ready) return 0; user_ready=0; *v=user_view; *error=user_error; return 1; }
static void user_key(int id,unsigned ch)
{ struct du_event e={.type=DU_EV_KEY,.id=id,.ch=ch}; u.event(&u,&e,u.user); }
static void users_controls(void)
{
    attach(CP_HOME); a.host.user_begin=user_begin; a.host.user_poll=user_poll;
    user_view.count=1; strcpy(user_view.names[0],"Alice"); user_view.ids[0]=1; user_view.flags=USERS_FLAGS;
    cp_open(&a,CP_USERS); assert(user_pending.operation==USERS_READ && a.user_busy);
    cp_tick(&a); assert(!a.user_busy && du_get(&u,1600)->count==1);
    du_set_text(du_get(&u,1601),"Bob");
    user_key(1603,'s'); user_key(1604,'s'); du_render(&u);
    assert(!du_get(&u,1603)->text[0] && !du_get(&u,1604)->text[0]);
    at_click_widget(&u,1605); assert(user_pending.operation==USERS_CREATE);
    assert(!strcmp(user_pending.name,"Bob") && !strcmp(user_pending.password,"s") && !strcmp(user_pending.confirm,"s"));
    assert(!a.user_request.password[0] && !a.user_request.confirm[0]);
    user_view.count=2; strcpy(user_view.names[1],"Bob"); user_view.ids[1]=2;
    cp_tick(&a); assert(du_get(&u,1600)->count==2);
    du_get(&u,1608)->value=0; du_get(&u,1609)->value=1; du_get(&u,1610)->value=1;
    at_click_widget(&u,1611); assert(user_pending.operation==USERS_POLICY && user_pending.flags==(USERS_PROFILES|USERS_LOCK)); cp_tick(&a);
    du_set_text(du_get(&u,1601),"Bob"); at_click_widget(&u,1606); assert(u.dialog.kind!=DU_DLG_NONE);
    struct du_event cancel={.type=DU_EV_DIALOG,.id=1613,.index=1}; u.event(&u,&cancel,u.user); du_dialog_close(&u);
    assert(!a.user_busy); cp_detach(&a);
    const char *old=getenv("USER"); char *previous=old ? strdup(old) : NULL;
    assert(!setenv("USER","Alice",1));
    attach(CP_HOME); a.host.user_begin=user_begin; a.host.user_poll=user_poll;
    cp_open(&a,CP_PASSWORDS); cp_tick(&a);
    user_key(1602,'o'); user_key(1603,'n'); user_key(1604,'n');
    at_click_widget(&u,1607);
    assert(user_pending.operation==USERS_CHANGE && !strcmp(user_pending.name,"Alice") && !strcmp(user_pending.current,"Alice"));
    assert(!strcmp(user_pending.old_password,"o") && !strcmp(user_pending.password,"n"));
    user_error=-EACCES; cp_tick(&a); assert(!a.user_busy && strstr(du_get(&u,1612)->text,"incorrect")); user_error=0;
    at_click_widget(&u,1614); assert(user_pending.operation==USERS_READ); cp_tick(&a); assert(!a.user_busy);
    cp_detach(&a);
    attach(CP_HOME); a.host.user_begin=user_begin; a.host.user_poll=user_poll;
    cp_open(&a,CP_DISPLAY); a.display_tab=CP_SCREEN_SAVER;
    struct du_event tab={.type=DU_EV_SELECT,.id=CP_DISPLAY_TABS_ID,.index=CP_SCREEN_SAVER};
    u.event(&u,&tab,u.user); cp_tick(&a);
    assert(du_get(&u,CP_SAVER_PASSWORD) && !du_get(&u,CP_SAVER_PASSWORD)->disabled);
    at_click_widget(&u,CP_SAVER_PASSWORD);
    assert(user_pending.operation==USERS_POLICY && user_pending.flags==(USERS_LOGON|USERS_PROFILES));
    cp_tick(&a); cp_detach(&a);
    if (previous) { assert(!setenv("USER",previous,1)); free(previous); } else unsetenv("USER");
    puts("PASS G32 Users and Passwords: list, masked secret canvases, create, delete Cancel, options, old/new/confirm, asynchronous failure");
}

#include "regional_control_test.h"
static void internet_page(void)
{
    attach(CP_INTERNET);a.host.internet_load=internet_read;a.host.internet_save=internet_write;a.host.internet_default=internet_default_program;
    a.host.internet_programs=internet_programs_open;a.host.internet_clear_history=internet_clear;cp_open(&a,CP_INTERNET);
    assert(cp_page_index("internet")==CP_INTERNET && du_get(&u,1700) && du_get(&u,1704));
    du_set_text(du_get(&u,1700),"https://example.test/home");du_set_text(du_get(&u,1701),"proxy.example.test");
    du_set_text(du_get(&u,1702),"3128");du_set_text(du_get(&u,1703),"4");at_click_widget(&u,1704);
    assert(internet_saves==1 && internet_saved.port==3128 && internet_saved.cache_mb==4 && !strcmp(internet_saved.home,"https://example.test/home"));
    du_set_text(du_get(&u,1703),"17");at_click_widget(&u,1704);assert(internet_saves==1);
    du_set_text(du_get(&u,1703),"2");du_set_text(du_get(&u,1700),"javascript:bad");at_click_widget(&u,1704);assert(internet_saves==1);
    du_set_text(du_get(&u,1700),"https://example.test/home");
    at_click_widget(&u,1721);assert(a.internet_tab==1 && du_get(&u,1705));
    du_set_text(du_get(&u,1705),"30");at_click_widget(&u,1704);assert(internet_saves==2 && internet_saved.history_days==30);
    at_click_widget(&u,1706);assert(internet_cleared==1);
    at_click_widget(&u,1722);assert(a.internet_tab==2 && du_get(&u,1707));
    du_set_text(du_get(&u,1707),"trusted.example");du_set_text(du_get(&u,1708),"blocked.example");
    at_click_widget(&u,1704);assert(internet_saves==3 && !strcmp(internet_saved.restricted,"blocked.example"));
    at_click_widget(&u,1723);assert(a.internet_tab==3 && du_get(&u,1713));
    du_set_text(du_get(&u,1713),"user");du_set_text(du_get(&u,1714),"password");
    at_click_widget(&u,1704);assert(internet_saves==4 && !strcmp(internet_saved.proxy_password,"password"));
    at_click_widget(&u,1724);assert(a.internet_tab==4 && !du_get(&u,1716)->disabled && !du_get(&u,1717)->disabled);
    at_click_widget(&u,1716);at_click_widget(&u,1717);assert(internet_defaulted==1 && internet_programs_opened==1);
    cp_detach(&a);puts("PASS G26 Internet Properties: page/catalog, home/proxy/cache Apply, invalid values preserve saved settings");
}
int main(int argc, char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--internet-only")) { setvbuf(stdout,NULL,_IONBF,0);internet_page();return 0; }
    users_controls();
    assert(argc == 3 || argc == 4);
    FILE *f = fopen(argv[1], "rb"); assert(f && fread(portrait, 4, 256 * 256, f) == 256 * 256); fclose(f);
    folder = argv[2];
    const char *renders = argc == 4 ? argv[3] : NULL;

    joystick_page();
    programs_page();
    power_page();
    power_policy_page();
    /* No desktop.cfg yet: the defaults, Ciuk2 filled. */
    attach(CP_DISPLAY);
    assert(!strcmp(a.cfg.theme, "Ciuki Blue") && !strcmp(desk_theme->name, "Ciuki Blue"));
    for (int k = 0; k < CFG_THEMES; k++) assert(a.theme_items[k].selected == (k == cfg_theme_index("Ciuki Blue")));
    assert(a.cfg.wallpaper_mode == CFG_WALL_FILL && !strcmp(a.cfg.wallpaper, "/system/wallpapers/Ciuk2.png"));
    struct du_widget *list = NULL;
    for (unsigned i = 0; i < u.count; i++) if (u.widgets[i].kind == DU_LISTVIEW && u.widgets[i].count == 2 + DESK_BUILTIN_WALLPAPERS + 3 + 8) list = &u.widgets[i];
    assert(list && !strcmp(list->items[list->active].text[0], "Ciuk2.png") && list->items[list->active].selected);
    check_marked(CFG_WALL_FILL);

    /* Layout: the label, then two rows of three 68 px buttons under the 208 px
     * monitor (x 24..232, y 44..215), clear of every other Display widget. */
    struct du_widget *label = find(DU_LABEL, "Position:"), *tabs = NULL;
    assert(label && !label->hidden);
    for (unsigned i = 0; i < u.count; i++) if (u.widgets[i].kind == DU_TABS) tabs = &u.widgets[i];
    assert(tabs);
    struct du_rect monitor = {24, 44, 208, 172}, column = {24, 216, 208, CP_HEIGHT - 62 + 8 - 216};
    for (int k = 0; k < 5; k++) {
        struct du_widget *b = find(DU_BUTTON, labels[k]);
        assert(b && !b->hidden && !b->disabled && b->box.w == 68 && b->box.h == 22);
        assert(b->box.x == 24 + k % 3 * 70 && b->box.y == 240 + k / 3 * 24);
        assert(inside(b->box, column) && du_text_width(labels[k]) + 8 <= b->box.w);
        assert(b->box.y > label->box.y + label->box.h - 1);
        if (k) assert(b->id == find(DU_BUTTON, labels[k - 1])->id + 1);
    }
    unsigned visible = 0;
    for (unsigned i = 0; i < u.count; i++) {
        struct du_widget *p = &u.widgets[i];
        if (p->hidden || p->kind == DU_TABS) continue;
        visible++;
        if (p->box.y + p->box.h <= tabs->box.y + tabs->box.h) {   /* inside the tab sheet (the status line and Close are below it) */
            assert(inside(p->box, tabs->box) && !overlap(p->box, monitor));
        }
        for (unsigned j = i + 1; j < u.count; j++) {
            struct du_widget *q = &u.widgets[j];
            if (!q->hidden && q->kind != DU_TABS) assert(!overlap(p->box, q->box));
        }
    }
    assert(visible >= 5 + 6 + 7);

    render_settled(&u);
    check_preview();
    assert(!preview_band());
    at_save(renders, "control-display-fill", &u);

    /* Each button saves its desktop.cfg name, applies, marks itself and
     * rebuilds the preview with that mode. */
    for (int k = 0; k < 5; k++) {
        unsigned s0 = saves, a0 = applies, d0 = decodes;
        at_click_widget(&u, find(DU_BUTTON, labels[k])->id);
        assert(a.cfg.wallpaper_mode == modes[k] && saves == s0 && applies == a0);
        at_click_widget(&u,CP_APPLY);
        assert(saves==s0+1 && applies==a0+1);
        char line[64]; snprintf(line, sizeof(line), "\nwallpaper_mode=%s\n", cfg_mode_names[modes[k]]);
        assert(strstr(saved, line) && strstr(saved, "\nwallpaper=/system/wallpapers/Ciuk2.png\n"));
        assert(find(DU_LABEL,"Preview applied. Cancel restores your previous settings."));
        check_marked(modes[k]);
        assert(!a.preview_valid);
        render_settled(&u);
        assert(decodes == d0);   /* a mode change reuses the decoded image cached per path */
        check_preview();
        assert(preview_band() == (modes[k] == CFG_WALL_FIT));
        if (modes[k] == CFG_WALL_FIT) at_save(renders, "control-display-fit", &u);
    }
    static struct ciuki_settings parsed;
    cfg_defaults(&parsed); parsed.wallpaper_mode = CFG_WALL_CENTER;
    assert(!cfg_parse(&parsed, saved, saved_n) && parsed.wallpaper_mode == CFG_WALL_FILL);

    cp_detach(&a);
    /* Other pages hide the buttons. */
    attach(CP_MOUSE);
    for (int k = 0; k < 5; k++) assert(!find(DU_BUTTON, labels[k]));
    /* Win98 look: value sliders are trackbars, the one-page applet has no tab strip of its own. */
    {
        unsigned tracks = 0, scrolls = 0;
        for (unsigned i = 0; i < u.count; i++) { tracks += u.widgets[i].kind == DU_TRACKBAR; scrolls += u.widgets[i].kind == DU_SCROLLBAR; }
        assert(tracks == 4 && !scrolls);
        for (unsigned i = 0; i < u.count; i++) if (u.widgets[i].kind == DU_TABS) assert(u.widgets[i].hidden);
    }
    render_settled(&u); at_save(renders, "control-mouse", &u);
    cp_detach(&a);
    attach(CP_KEYBOARD); render_settled(&u); at_save(renders, "control-keyboard", &u); cp_detach(&a);
    attach(CP_TASKBAR);
    {
        unsigned tabs = 0, visible = 0;
        for (unsigned i = 0; i < u.count; i++) if (u.widgets[i].kind == DU_TABS) { tabs++; visible += !u.widgets[i].hidden && u.widgets[i].count == 2; }
        assert(tabs == 1 && visible == 1);   /* the two taskbar tabs sit on the sheet, not in a second tab control */
    }
    render_settled(&u); at_save(renders, "control-taskbar", &u); cp_detach(&a);
    /* The home is a folder window: large icons, an "N object(s)" status bar and no Close button. */
    attach(CP_HOME);
    {
        struct du_widget *grid = NULL, *status = NULL;
        for (unsigned i = 0; i < u.count; i++) {
            if (u.widgets[i].kind == DU_LISTVIEW) grid = &u.widgets[i];
            if (u.widgets[i].kind == DU_STATUS) status = &u.widgets[i];
        }
        assert(grid && grid->view == DU_ICONS && grid->count && grid->items[0].icon32 && status);
        char want[32]; snprintf(want, sizeof(want), "%u object(s)", grid->count);
        assert(!strcmp(status->text, want) && !find(DU_BUTTON, "Close"));
    }
    render_settled(&u); at_save(renders, "control-home", &u);
    cp_detach(&a);

    /* A file written before fill keeps its mode; Ciuk1 is selected. */
    static const char old[] = "version=1\ntheme=Classic 98\nwallpaper=/system/wallpapers/Ciuk1.png\nwallpaper_mode=center\n";
    memcpy(saved, old, sizeof(old)); saved_n = sizeof(old) - 1;
    attach(CP_DISPLAY);
    assert(a.cfg.wallpaper_mode == CFG_WALL_CENTER && !strcmp(a.cfg.theme, "Ciuki Blue"));
    for (int k = 0; k < CFG_THEMES; k++) assert(a.theme_items[k].selected == (k == cfg_theme_index("Ciuki Blue")));
    check_marked(CFG_WALL_CENTER);
    for (int k = 0; k < 5; k++) assert(du_text_width(labels[k]) + 8 <= find(DU_BUTTON, labels[k])->box.w);
    render_settled(&u);
    check_preview();
    at_click_widget(&u, find(DU_BUTTON, "Fill")->id);
    at_click_widget(&u,CP_APPLY);
    assert(a.cfg.wallpaper_mode == CFG_WALL_FILL && strstr(saved, "\nwallpaper=/system/wallpapers/Ciuk1.png\nwallpaper_mode=fill\n"));
    render_settled(&u);
    check_preview();
    at_save(renders, "control-display-migrated-fill", &u);
    cp_detach(&a);
    /* Platinum remains a valid persisted choice, including its selection. */
    cfg_defaults(&parsed); strcpy(parsed.theme, "Platinum");
    saved_n = cfg_format(&parsed, saved, sizeof(saved)); assert(saved_n);
    attach(CP_DISPLAY);
    assert(!strcmp(a.cfg.theme, "Platinum") && !strcmp(desk_theme->name, "Platinum"));
    for (int k = 0; k < CFG_THEMES; k++) assert(a.theme_items[k].selected == (k == cfg_theme_index("Platinum")));
    cp_detach(&a);
    attach(CP_HOME);
    a.host.autoplay_list=autoplay_list; a.host.autoplay_forget=autoplay_forget;
    assert(cp_page_index("autoplay")==CP_AUTOPLAY); cp_open(&a,CP_AUTOPLAY);
    struct du_widget *choices=du_get(&u,950);
    assert(choices && choices->count==2 && !strcmp(choices->items[0].text[0],"Music stick") && !strcmp(choices->items[0].text[1],"Play music"));
    assert(!strcmp(choices->items[1].text[1],"Do nothing"));
    struct du_event selected={.type=DU_EV_SELECT,.id=950,.index=1}; u.event(&u,&selected,u.user);
    at_click_widget(&u,951); assert(a.autoplay_count==1 && strstr(du_get(&u,3)->text,"ask every time"));
    autoplay_count=0; at_click_widget(&u,952); assert(!a.autoplay_count && du_get(&u,951)->disabled);
    autoplay_error=1; at_click_widget(&u,952); assert(strstr(du_get(&u,950)->text,"could not be read"));
    cp_detach(&a);
    /* F1-63's QMP case opens --panel=sounds, presses Tab twice and
     * Space. Verify the actual toolkit targets master Mute, not UI sounds. */
    mixer_volume=20;attach(CP_SOUNDS);a.host.apply=master_apply;
    assert(u.focus>=0 && !strcmp(u.widgets[u.focus].text,"Control Panel") && a.volume==20 && !a.muted);
    /* The two volume trackbars are now tab stops (Back, Close, Master volume, UI sounds volume, Mute). */
    for (unsigned i=0;i<4;i++) {
        at_tap(&u,DU_KEY_TAB);
        if (i==1) assert(u.focus>=0 && u.widgets[u.focus].kind==DU_TRACKBAR);
    }
    assert(u.focus>=0 && u.widgets[u.focus].kind==DU_BUTTON && !strcmp(u.widgets[u.focus].text,"Mute: Off"));
    at_tap(&u,DU_KEY_SPACE);assert(master_applies==1 && a.muted);
    cp_detach(&a);mixer_volume=70;
    attach(CP_HOME); cp_settings_saved(&a,EROFS);
    assert(u.dialog.kind==DU_DLG_MESSAGE && u.dialog.icon==DU_DLG_ICON_ERROR);
    assert(!strcmp(u.dialog.text,"Read-only file system (30)"));
    assert(!strcmp(du_get(&u,3)->text,"Read-only file system (30)"));
    du_dialog_close(&u); cp_detach(&a);
    /* Apply is a preview across all tabs. Cancel restores only owned keys. */
    cfg_defaults(&parsed); strcpy(parsed.wallpaper,"none"); parsed.wallpaper_mode=CFG_WALL_CENTER;
    saved_n=cfg_format(&parsed,saved,sizeof(saved)); attach(CP_DISPLAY);
    unsigned before=saves;
    at_click_widget(&u,find(DU_BUTTON,"Fill")->id); assert(saves==before && a.display_dirty);
    at_click_widget(&u,CP_APPLY); assert(saves==before+1 && a.display_preview);
    static struct ciuki_settings concurrent;
    cfg_defaults(&concurrent); assert(!cfg_parse(&concurrent,saved,saved_n));
    concurrent.mouse_speed=9; concurrent.clock_seconds=1; strcpy(concurrent.extra,"future_setting=preserved\n");
    saved_n=cfg_format(&concurrent,saved,sizeof(saved));
    assert(cp_close(&a) && a.quit && !a.display_session);
    cfg_defaults(&parsed); assert(!cfg_parse(&parsed,saved,saved_n));
    assert(parsed.wallpaper_mode==CFG_WALL_CENTER && parsed.mouse_speed==9 && parsed.clock_seconds==1 && strstr(parsed.extra,"future_setting=preserved"));
    cp_detach(&a);
    /* OK waits for durable completion; detach must keep the accepted values. */
    attach(CP_DISPLAY); a.host.save_async=1; before=saves;
    at_click_widget(&u,find(DU_BUTTON,"Fill")->id); at_click_widget(&u,CP_OK);
    assert(a.display_write && !a.quit && saves==before+1);
    cp_settings_saved(&a,0); assert(a.quit && !a.display_session);
    cp_detach(&a); assert(saves==before+1);
    /* Caption close while a preview is saving waits, then durably rolls back. */
    attach(CP_DISPLAY); a.host.save_async=1;
    at_click_widget(&u,find(DU_BUTTON,"Fit")->id); at_click_widget(&u,CP_APPLY);
    at_click_widget(&u,CP_CANCEL); assert(a.close_pending && !a.quit);
    cp_settings_saved(&a,0); cp_tick(&a);
    assert(a.display_write && !a.quit);
    cp_settings_saved(&a,0); cp_tick(&a); assert(a.quit && !a.display_session);
    cfg_defaults(&parsed); assert(!cfg_parse(&parsed,saved,saved_n) && parsed.wallpaper_mode==CFG_WALL_FILL); cp_detach(&a);
    /* An asynchronous failure keeps the window open with a retryable draft. */
    attach(CP_DISPLAY); a.host.save_async=1;
    at_click_widget(&u,find(DU_BUTTON,"Fit")->id); at_click_widget(&u,CP_OK);
    cp_settings_saved(&a,EROFS); assert(!a.quit && !a.display_accept && a.display_dirty && a.display_session);
    du_dialog_close(&u); a.host.save_async=0; cp_close(&a); cp_detach(&a);
    regional_page();
    internet_page();
    attach(CP_DISPLAY);
    struct du_widget *browse=find(DU_BUTTON,"Browse..."); assert(browse);
    struct du_event choose={.type=DU_EV_ACTIVATE,.id=browse->id}; u.event(&u,&choose,u.user);
    struct du_event selected_wall={.type=DU_EV_DIALOG,.id=browse->id+1,.index=0,.text="/Documents/Desktop/Scene.png"};
    u.event(&u,&selected_wall,u.user);
    assert(!strcmp(a.cfg.wallpaper,"/Documents/Desktop/Scene.png") && a.display_dirty);
    a.display_dirty=0;
    static const char *const invalid_wall[]={"/Documents/Desktop/not-an-image.txt","/Documents/../Scene.png",
                                           "/Documents/./Scene.png","Scene.png"};
    for (unsigned k=0;k<sizeof(invalid_wall)/sizeof(*invalid_wall);k++) {
        selected_wall.text=invalid_wall[k]; u.event(&u,&selected_wall,u.user);
        assert(!a.display_dirty && !strcmp(a.cfg.wallpaper,"/Documents/Desktop/Scene.png"));
        assert(strstr(du_get(&u,3)->text,"Choose a PNG, JPEG or BMP"));
    }
    selected_wall.text="/Documents/Desktop/Scene.JPG"; u.event(&u,&selected_wall,u.user);
    assert(a.display_dirty && !strcmp(a.cfg.wallpaper,selected_wall.text));
    assert(!cp_wallpaper_set(&a,"/Documents/Desktop/Paint.bmp") && !strcmp(a.cfg.wallpaper,"/Documents/Desktop/Paint.bmp"));
    assert(cp_wallpaper_set(&a,"/Documents/Desktop/not-an-image.txt")==-EINVAL);
    cp_close(&a); cp_detach(&a);
    cfg_defaults(&parsed); strcpy(parsed.wallpaper,"none"); saved_n=cfg_format(&parsed,saved,sizeof(saved));
    attach(CP_DISPLAY); assert(du_get(&u,CP_WALL_PATTERN)->count==DESK_WALL_PATTERNS);
    render_settled(&u); static uint32_t plain_preview[192*144]; memcpy(plain_preview,a.preview,sizeof(plain_preview));
    struct du_event pattern={.type=DU_EV_SELECT,.id=CP_WALL_PATTERN,.index=3}; u.event(&u,&pattern,u.user);
    assert(a.display_dirty && a.cfg.wallpaper_pattern==3); render_settled(&u);
    assert(a.preview_valid && memcmp(plain_preview,a.preview,sizeof(plain_preview))); at_click_widget(&u,CP_APPLY);
    cfg_defaults(&parsed); assert(!cfg_parse(&parsed,saved,saved_n) && parsed.wallpaper_pattern==3);
    assert(cp_close(&a)); cp_detach(&a);
    cfg_defaults(&parsed); assert(!cfg_parse(&parsed,saved,saved_n) && !parsed.wallpaper_pattern);
    attach(CP_DISPLAY); u.event(&u,&pattern,u.user); at_click_widget(&u,CP_OK); assert(a.quit); cp_detach(&a);
    cfg_defaults(&parsed); assert(!cfg_parse(&parsed,saved,saved_n) && parsed.wallpaper_pattern==3);
    puts("PASS Background Pattern: chooser, preview, Apply, Cancel restore and OK persistence");
    puts("PASS Display Properties: staged edits, preview/Cancel merge, durable OK, pending-close rollback and failed save");
    puts("PASS F1-63 Control Panel: two Tab presses and Space apply master preference 20/20/muted");
    printf("PASS control display: five positions, layout, desktop.cfg modes, preview\n");
    return 0;
}
