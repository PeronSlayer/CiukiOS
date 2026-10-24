/* SPDX-License-Identifier: MIT
 * Production Control Panel display model, widgets and protocol; host evidence. */
#include "control.h"
#include "acc_test.h"
#include "display_fixture.h"
#include "../../../apps/libdeskui/internal.h"
#include <errno.h>
#include <stddef.h>
_Static_assert(sizeof(struct ciuki_display_mode) == 12, "f1-23 mode ABI");
_Static_assert(offsetof(struct ciuki_display_info, modes) == 228, "f1-23 info ABI");
_Static_assert(sizeof(struct ciuki_display_info) == 612, "f1-23 info size");
static struct cp_app a;
static struct du_ui u;
static struct ciuki_display_info info;
static uint32_t pixels[CP_WIDTH * CP_HEIGHT];
static uint32_t portrait[256 * 256];
static char saved[CFG_TEXT_MAX]; static size_t saved_n;
static int query_error, save_error, queue_error, requests, saves;
static uint16_t requested;
static struct ciuki_message wire;
/* The SDK query symbol is supplied only by this host harness. */
int ciuki_display_query(struct ciuki_display_info *d)
{
    assert(d->version == CIUKI_DISPLAY_VERSION && d->size == sizeof(*d));
    if (query_error) return -ENOSYS;
    *d = info; return 0;
}
static int query(void *ctx, struct ciuki_display_info *d) { (void)ctx; return ciuki_display_query(d); }
static int load(void *ctx, char *out, size_t cap, size_t *n)
{ (void)ctx; assert(saved_n < cap); memcpy(out, saved, saved_n); *n = saved_n; return 0; }
static int save(void *ctx, const char *s, size_t n)
{ (void)ctx; if (save_error) return -1; assert(n < sizeof(saved)); memcpy(saved, s, n); saved[n] = 0; saved_n = n; saves++; return 0; }
static int apply(void *ctx, unsigned request, int value, int flag)
{
    (void)ctx; if (queue_error) return -1;
    cp_settings_message(&wire, (unsigned)++requests, request, value, flag);
    struct desk_packet p;
    assert(desk_parse(&wire, 1, NULL, &p) && p.request == request && p.serial == (unsigned)requests);
    if (request == DESK_SETTINGS_DISPLAY || request == DESK_SETTINGS_DISPLAY_PREVIEW) { assert(!flag && !p.values[1] && !p.values[2]); requested = (uint16_t)p.values[0]; }
    return 0;
}
static void changed_mode(uint16_t id)
{
    int found = 0;
    for (unsigned i = 0; i < info.mode_count; i++) {
        struct ciuki_display_mode *m = &info.modes[i]; m->current = m->id == id;
        if (m->current) { info.width = m->width; info.height = m->height; info.bpp = m->bpp; info.pitch = m->pitch; found = 1; }
    }
    assert(found);
}
static void select(int id, int index)
{ struct du_event e = {.type = DU_EV_SELECT, .id = id, .index = index}; u.event(&u, &e, u.user); }
static unsigned hwid_calls;
static int hwid_lookup(void *ctx,unsigned vendor,unsigned device,char *out,size_t size)
{
    (void)ctx; hwid_calls++;
    return cp_hwid_lookup_json("{\"5333:8c2e\":\"S3 SuperSavage IX/C SDR\",\"1002:4c52\":\"ATI Rage Mobility M/P\"}",vendor,device,out,size);
}
static void attach(int page)
{
    memset(&a, 0, sizeof(a));
    a.host = (struct cp_host){.load = load, .save = save, .apply = apply, .display = query, .portrait = portrait, .hwid_name=hwid_lookup,
                             .screen_w = 1024, .screen_h = 768, .machine = "i686"};
    du_init(&u, pixels, CP_WIDTH, CP_HEIGHT); u.focused_window = 1;
    assert(!cp_attach(&a, &u, page) && u.count < DU_WIDGETS);
}
static void init(void)
{
    display_fixture(&info); query_error = save_error = queue_error = requests = saves = 0; requested = 0;
    strcpy(saved, "wallpaper=none\ndisplay_mode=280\nfuture_key=kept\nrecent=/Documents/note.txt\n"); saved_n = strlen(saved);
    attach(CP_ADAPTER);
}
static void start(void)
{
    select(CP_AREA, 0); /* 640 x 480 only has 16-bit */
    assert(a.depth_count == 1 && a.depths[0] == 16);
    at_click_widget(&u, CP_APPLY);
    assert(a.mode_state == CP_MODE_WAIT && requested == 0x111 && saves == 0 && a.previous_mode == 0x118);
    assert(!u.dialog.kind && du_get(&u, CP_APPLY)->disabled);
}
static void ack(void)
{ changed_mode(requested); cp_display_result(&a, 0); assert(a.mode_state == CP_MODE_CONFIRM && du_dialog_id(&u) == CP_CONFIRM); }
static void choose(int button)
{ struct du_rect b = du_dialog_button(&u, (unsigned)button); at_click(&u, b.x + b.w/2, b.y + b.h/2); }
static void check_layout(void)
{
    for (unsigned i = 0; i < u.count; i++) {
        const struct du_widget *p = &u.widgets[i];
        if (p->hidden || p->kind == DU_TABS) continue;
        assert(p->box.x >= 8 && p->box.x + p->box.w <= CP_WIDTH - 8 && p->box.y + p->box.h <= CP_HEIGHT);
        for (unsigned j = i + 1; j < u.count; j++) {
            const struct du_widget *q = &u.widgets[j];
            if (q->hidden || q->kind == DU_TABS) continue;
            if(p->box.x < q->box.x + q->box.w && q->box.x < p->box.x + p->box.w && p->box.y < q->box.y + q->box.h && q->box.y < p->box.y + p->box.h)
                fprintf(stderr,"overlapping widgets: %d and %d\n",p->id,q->id);
            assert(!(p->box.x < q->box.x + q->box.w && q->box.x < p->box.x + p->box.w && p->box.y < q->box.y + q->box.h && q->box.y < p->box.y + p->box.h));
        }
    }
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    FILE *f = fopen(argv[2], "rb"); assert(f && fread(portrait, 4, 256 * 256, f) == 256 * 256); fclose(f);
    init(); cp_open(&a,CP_DISPLAY); select(CP_DISPLAY_TABS_ID,CP_SCREEN_SAVER);
    assert(du_get(&u,CP_SAVER_KIND) && du_get(&u,CP_SAVER_WAIT)); check_layout();
    select(CP_SAVER_KIND,2); assert(a.cfg.saver_kind==2 && a.display_dirty);
    at_click_widget(&u,CP_SAVER_ENABLE); assert(a.cfg.saver_enabled);
    struct du_event wait={.type=DU_EV_CHANGE,.id=CP_SAVER_WAIT,.index=3};
    u.event(&u,&wait,u.user); assert(a.cfg.saver_seconds==180);
    du_render(&u); at_save(argv[1],"control-screen-saver",&u);
    at_click_widget(&u,CP_SAVER_SETTINGS); assert(du_dialog_id(&u)==CP_SAVER_SETTINGS); du_dialog_close(&u);
    at_click_widget(&u,CP_SAVER_PREVIEW);
    struct desk_packet preview; assert(desk_parse(&wire,1,NULL,&preview));
    assert(preview.request==DESK_SETTINGS_SAVER_PREVIEW && preview.values[0]==2);
    at_click_widget(&u,CP_APPLY);
    assert(strstr(saved,"saver_kind=2\n") && strstr(saved,"saver_seconds=180\n"));
    assert(cp_close(&a) && a.cfg.saver_kind==1 && !a.cfg.saver_enabled);
    cp_detach(&a);
    init(); cp_open(&a,CP_POWER);
    wait.id=CP_MONITOR_WAIT; wait.index=5; u.event(&u,&wait,u.user);
    assert(strstr(saved,"monitor_seconds=300\n"));
    du_render(&u); at_save(argv[1],"control-monitor-power",&u); cp_detach(&a);
    puts("PASS G10 Control Panel: saver choice, enable, minutes, preview wire, Apply/Cancel rollback, Power monitor timeout");
    /* G23: real controls, all items, save/delete, effects and Advanced DPI. */
    init(); cp_open(&a,CP_DISPLAY);
    for(int tab=0;tab<CP_DISPLAY_TABS;tab++) {
        select(CP_DISPLAY_TABS_ID,tab); check_layout(); du_render(&u);
        if(tab==CP_APPEARANCE) at_save(argv[1],"control-appearance",&u);
        if(tab==CP_EFFECTS) at_save(argv[1],"control-effects",&u);
        if(tab==CP_THEMES) at_save(argv[1],"control-desktop-themes",&u);
    }
    select(CP_DISPLAY_TABS_ID,CP_APPEARANCE); select(4100,0);
    assert(a.cfg.appearance_set && !strcmp(a.cfg.scheme,"Windows Standard"));
    for(int item=0;item<AP_ITEMS;item++) {
        select(4104,item); assert(a.appearance_item==item);
        struct du_event size={.type=DU_EV_CHANGE,.id=4108,.index=2}; u.event(&u,&size,u.user);
        assert(a.cfg.appearance.item[item].size==ap_size_min(item)+2);
    }
    select(4104,AP_SCROLLBAR);
    struct du_widget *slider=du_get(&u,4108);
    struct du_sb sb={slider->box,slider->vertical,slider->value,slider->total,slider->page};
    int grab=slider->box.x;
    while(grab<slider->box.x+slider->box.w && du_sb_part(&sb,grab,slider->box.y+8)!=3) grab++;
    assert(grab<slider->box.x+slider->box.w);
    at_move(&u,grab,slider->box.y+8);
    struct ciuki_input_event down={.type=CIUKI_INPUT_BUTTON,.code=1,.value=1}; du_input(&u,&down);
    unsigned old_size=a.cfg.appearance.item[AP_SCROLLBAR].size;
    at_move(&u,grab+30,slider->box.y+8);
    assert(a.cfg.appearance.item[AP_SCROLLBAR].size>old_size && u.capture>=0 && u.widgets[u.capture].id==4108);
    down.value=0; du_input(&u,&down);
    struct du_event save_scheme={.type=DU_EV_DIALOG,.id=4120,.index=0,.text="My Appearance"};
    u.event(&u,&save_scheme,u.user);
    assert(!strcmp(a.cfg.schemes[0].name,"My Appearance"));
    at_click_widget(&u,4102); assert(!a.cfg.schemes[0].name[0]);
    /* Save As uses a modal response through its own ID. */
    select(CP_DISPLAY_TABS_ID,CP_EFFECTS);
    for(int i=0;i<4;i++) { unsigned old=a.cfg.appearance.effects; at_click_widget(&u,4113+i); assert((a.cfg.appearance.effects^old)==(1u<<i)); }
    select(CP_DISPLAY_TABS_ID,CP_SCREEN_SETTINGS);
    at_click_widget(&u,4133); assert(du_dialog_id(&u)==4134);
    struct du_rect large_font=u.dialog.items[6].box;
    at_click(&u,large_font.x+10,large_font.y+10); assert(a.cfg.appearance.dpi==120 && du_dialog_id(&u)==4134);
    du_render(&u); at_save(argv[1],"control-advanced-fonts",&u);
    struct du_rect cancel_font=du_dialog_button(&u,1); at_click(&u,cancel_font.x+5,cancel_font.y+5);
    assert(!du_dialog_id(&u) && a.cfg.appearance.dpi==96);
    at_click_widget(&u,4133);
    large_font=u.dialog.items[6].box; at_click(&u,large_font.x+10,large_font.y+10);
    save_error=1; choose(2); assert(du_dialog_id(&u)==4134 && a.cfg.appearance.dpi==120);
    save_error=0; choose(2); assert(du_dialog_id(&u)==4134 && a.cfg.appearance.dpi==120);
    choose(1); assert(!du_dialog_id(&u) && a.cfg.appearance.dpi==120);
    at_click_widget(&u,4133); choose(0); assert(!du_dialog_id(&u));
    at_click_widget(&u,4118); assert(a.cfg.appearance.dpi==120);
    at_click_widget(&u,4117); assert(a.cfg.appearance.dpi==96);
    struct du_event custom={.type=DU_EV_CHANGE,.id=4119,.index=96}; u.event(&u,&custom,u.user);
    assert(a.cfg.appearance.dpi==144);
    assert(cp_close(&a)); cp_detach(&a);
    puts("PASS G23 Control Panel: every Appearance item, Effects toggles, Advanced 96/120/custom DPI and tab geometry");
    init(); check_layout();
    assert(a.area_count == 4 && !strcmp(a.area_items[2].text[0], "1024 x 768") && a.area == 2);
    assert(a.depth_count == 2 && a.depths[0] == 16 && a.depths[1] == 32 && a.depth == 1);
    struct du_widget *area = du_get(&u, CP_AREA);
    struct du_rect row = du_list_label(area, area->active), body = du_list_body(area);
    assert(row.y >= body.y && row.y + row.h <= body.y + body.h);
    du_focus(&u, CP_AREA); at_tap(&u, DU_KEY_UP);
    assert(a.area == 1 && a.depth_count == 1 && a.depths[0] == 24);
    at_tap(&u, DU_KEY_DOWN); assert(a.area == 2);
    du_focus(&u, CP_DEPTH); at_tap(&u, DU_KEY_DOWN);
    assert(a.depths[a.depth] == 32 && du_get(&u, CP_APPLY)->disabled);
    select(CP_AREA, 1); assert(a.depth_count == 1 && a.depths[0] == 24);
    select(CP_AREA, 2); select(CP_DEPTH, 0); assert(!du_get(&u, CP_APPLY)->disabled);
    du_render(&u); at_save(argv[1], "control-display-modes", &u);
    start(); assert(desk_get32(wire.data + 12) == 0x111 && wire.length == 24);
    ack(); assert(strstr(u.dialog.text, "15 seconds"));
    du_render(&u); at_save(argv[1], "control-display-confirm", &u);
    du_tick(&u, u.now + 14000000000ull); assert(strstr(u.dialog.text, "1 second.") && a.mode_state == CP_MODE_CONFIRM);
    du_tick(&u, u.now + 1000000000ull); assert(a.mode_state == CP_MODE_REVERT && requested == 0x118 && saves == 0 && !u.dialog.kind);
    changed_mode(requested); cp_display_result(&a, 0); assert(a.mode_state == CP_MODE_IDLE && !strcmp(saved, "wallpaper=none\ndisplay_mode=280\nfuture_key=kept\nrecent=/Documents/note.txt\n"));
    cp_detach(&a);
    init(); start(); ack(); choose(0); assert(a.mode_state == CP_MODE_IDLE && saves == 1);
    assert(strstr(saved, "display_mode=273\n") && strstr(saved, "future_key=kept\n") && strstr(saved, "recent=/Documents/note.txt\n"));
    assert(desk_get32(wire.data + 8) == DESK_SETTINGS_RELOAD);
    struct ciuki_settings cfg; cfg_defaults(&cfg); assert(!cfg_parse(&cfg, saved, saved_n) && cfg.display_mode == 0x111);
    assert(cfg_parse(&cfg, "display_mode=65536\n", 19) == 1 && cfg.display_mode == 0x111);
    cp_detach(&a);
    /* f1-67: the existing 1024x768 RGB565 alternative must be selectable,
     * previewed and persisted without changing resolution. */
    init(); select(CP_DEPTH, 0);
    assert(a.area_w[a.area] == 1024 && a.area_h[a.area] == 768 && a.depths[a.depth] == 16);
    at_click_widget(&u, CP_APPLY);
    assert(a.mode_state == CP_MODE_WAIT && requested == 0x117 && a.previous_mode == 0x118 && !saves);
    ack(); assert(info.width == 1024 && info.height == 768 && info.bpp == 16 && info.pitch == 2048);
    choose(0); assert(a.mode_state == CP_MODE_IDLE && saves == 1);
    cfg_defaults(&cfg); assert(!cfg_parse(&cfg, saved, saved_n) && cfg.display_mode == 0x117);
    assert(strstr(saved, "future_key=kept\n") && strstr(saved, "recent=/Documents/note.txt\n"));
    cp_detach(&a);
    init(); start(); ack(); choose(1); assert(a.mode_state == CP_MODE_REVERT && requested == 0x118 && !saves);
    changed_mode(requested); cp_display_result(&a, 0); cp_detach(&a);
    init(); start(); ack(); at_tap(&u, DU_KEY_ESC); assert(a.mode_state == CP_MODE_REVERT && !saves); cp_display_result(&a, -EIO); cp_detach(&a);
    init(); start(); cp_display_result(&a, -EBUSY);
    assert(a.mode_state == CP_MODE_WAIT && a.mode_retry && !saves && !u.dialog.kind);
    int retries=requests; du_tick(&u,u.now+1000000000ull);
    assert(requests==retries+1 && !a.mode_retry); ack(); choose(1);
    cp_display_result(&a,-EBUSY); assert(a.mode_state==CP_MODE_REVERT && a.mode_retry);
    du_tick(&u,u.now+1000000000ull); changed_mode(requested); cp_display_result(&a,0);
    assert(a.mode_state==CP_MODE_IDLE && !saves); cp_detach(&a);
    init(); start(); cp_display_result(&a,-EBUSY); du_tick(&u,u.now+16000000000ull);
    assert(a.mode_state==CP_MODE_REVERT); cp_display_result(&a,-EBUSY);
    du_tick(&u,u.now+16000000000ull); assert(a.mode_state==CP_MODE_IDLE); cp_detach(&a);
    init(); a.host.save_async=1; start(); ack(); choose(0);
    assert(a.mode_state==CP_MODE_SAVE); cp_settings_saved(&a,0); assert(a.mode_state==CP_MODE_IDLE); cp_detach(&a);
    init(); a.host.save_async=1; start(); ack(); choose(0); cp_settings_saved(&a,EIO);
    assert(a.mode_state==CP_MODE_REVERT && requested==0x118); cp_detach(&a);
    init(); start(); ack(); save_error = 1; choose(0); assert(a.mode_state == CP_MODE_REVERT && !saves && requested == 0x118); changed_mode(requested); cp_display_result(&a, 0); cp_detach(&a);
    init(); start(); du_tick(&u, u.now + 15000000000ull); assert(a.mode_state == CP_MODE_REVERT && requested == 0x118); du_tick(&u, u.now + 15000000000ull); assert(a.mode_state == CP_MODE_IDLE && !saves); cp_detach(&a);
    init(); queue_error = 1; select(CP_AREA, 0); at_click_widget(&u, CP_APPLY); assert(a.mode_state == CP_MODE_IDLE && !requests && !saves); cp_detach(&a);
    init(); queue_error=1; select(CP_AREA,0); at_click_widget(&u,CP_OK);
    assert(!a.quit && !a.display_accept && a.mode_state==CP_MODE_IDLE); cp_detach(&a);
    init(); start(); ack(); cp_open(&a, CP_SOUNDS);
    assert(a.page == CP_DISPLAY && a.navigation_pending && a.mode_state == CP_MODE_REVERT && requested == 0x118 && !saves);
    changed_mode(requested); cp_display_result(&a, 0); cp_tick(&a); assert(a.page==CP_SOUNDS);
    cp_open(&a, CP_DISPLAY); assert(a.mode_state == CP_MODE_IDLE && !u.dialog.kind); cp_detach(&a);
    init(); start(); ack(); cp_detach(&a); assert(a.mode_state == CP_MODE_REVERT && requested == 0x118 && !saves);
    init(); start(); ack(); du_tick(&u, u.now + 16000000000ull); assert(a.mode_state == CP_MODE_REVERT && !saves); changed_mode(requested); cp_display_result(&a, 0); cp_detach(&a);
    init(); start(); /* changed query despite successful acknowledgement must revert */
    cp_display_result(&a, 0); assert(a.mode_state == CP_MODE_REVERT && !saves); cp_display_result(&a, 0); cp_detach(&a);
    init(); info.modes[0].current = 0; cp_display_refresh(&a); assert(du_get(&u, CP_APPLY)->disabled); cp_detach(&a);
    init(); info.mode_count = 33; cp_display_refresh(&a); assert(!a.display_ok && !a.area_count && du_get(&u, CP_APPLY)->disabled); cp_detach(&a);
    init(); info.mode_count = 0; cp_display_refresh(&a); assert(!a.area_count && du_get(&u, CP_APPLY)->disabled); cp_detach(&a);
    init(); info.version = 2; cp_display_refresh(&a); assert(!a.display_ok); cp_detach(&a);
    init(); info.size = 48; cp_display_refresh(&a); assert(!a.display_ok); cp_detach(&a);
    init(); query_error = 1; cp_display_refresh(&a); assert(!a.display_ok && du_get(&u, CP_APPLY)->disabled); cp_detach(&a);
    init(); info.mode_count = 32; /* maximum table: remaining entries have zero IDs */
    cp_display_refresh(&a); assert(a.area_count == 4); cp_detach(&a);
    init(); cp_open(&a, CP_ADAPTER); check_layout(); assert(!du_get(&u, CP_REFRESH)->hidden && du_get(&u, CP_AREA));
    assert(strstr(a.adapter_lines[0], "1024 x 768") && strstr(a.adapter_lines[1], "4096") && strstr(a.adapter_lines[2], "2.00") && strstr(a.adapter_lines[2], "16384"));
    assert(strstr(a.adapter_lines[4], "SuperSavage") && strstr(a.adapter_lines[9], "no EDID yet"));
    du_render(&u); at_save(argv[1], "control-settings-adapter", &u);
    info.pci_vendor = 0x1002; info.pci_device = 0x4c52; at_click_widget(&u, CP_REFRESH); assert(strstr(a.adapter_lines[4], "ATI Rage Mobility"));
    cp_open(&a, CP_SYSTEM); assert(strstr(du_get(&u, CP_SUMMARY)->text, "32-bit")); du_render(&u); at_save(argv[1], "control-system-display", &u);
    int system_tabs = -1;
    for (unsigned i = 0; i < u.count; i++) if (u.widgets[i].kind == DU_TABS) system_tabs = u.widgets[i].id;
    assert(system_tabs >= 0); select(system_tabs, 1); assert(!du_get(&u, CP_SUMMARY));
    select(system_tabs, 0); assert(strstr(du_get(&u, CP_SUMMARY)->text, "32-bit"));
    char name[64];
    assert(hwid_calls && !strcmp(cp_adapter_name(0x5333,0x8c2e,name,sizeof(name)),"Unknown adapter (5333:8c2e)"));
    assert(!strcmp(cp_adapter_name(0xdead, 0xbeef, name, sizeof(name)), "Unknown adapter (dead:beef)"));
    info.pci_vendor = 0xdead; info.pci_device = 0xbeef; cp_open(&a, CP_ADAPTER); du_render(&u); at_save(argv[1], "control-adapter-unknown", &u);
    query_error = 1; at_click_widget(&u, CP_REFRESH); du_render(&u); at_save(argv[1], "control-adapter-unavailable", &u); cp_detach(&a);
    /* Multiple mode previews still roll back to the entry mode on Cancel. */
    init(); start(); ack(); choose(0); assert(info.width==640 && a.display_preview);
    select(CP_AREA,1); at_click_widget(&u,CP_APPLY); ack(); choose(0); assert(info.width==800);
    assert(!cp_close(&a) && a.mode_state==CP_MODE_REVERT && requested==0x118 && !a.quit);
    changed_mode(requested); cp_display_result(&a,0); cp_tick(&a);
    assert(a.quit && !a.display_session && info.width==1024); cp_detach(&a);
    init(); start(); ack(); choose(0); queue_error=1;
    assert(!cp_close(&a) && !a.quit && a.display_session && a.display_preview && !a.close_pending);
    queue_error=0; assert(!cp_close(&a)); changed_mode(requested); cp_display_result(&a,0); cp_tick(&a);
    assert(a.quit && !a.display_session); cp_detach(&a);
    init(); a.host.save_async=1; start(); ack(); choose(0); cp_settings_saved(&a,0);
    uint16_t preview_mode=requested;
    assert(!cp_close(&a) && a.display_write && a.mode_state==CP_MODE_IDLE && requested==preview_mode);
    cp_settings_saved(&a,0); cp_tick(&a);
    assert(a.mode_state==CP_MODE_REVERT && requested==0x118);
    changed_mode(requested); cp_display_result(&a,0); cp_tick(&a); assert(a.quit); cp_detach(&a);
    struct desk_packet p;
    cp_settings_message(&wire, 42, DESK_SETTINGS_DISPLAY, 0x118, 0); assert(desk_parse(&wire, 1, NULL, &p));
    assert(!desk_parse(&wire, 0, NULL, &p)); desk_put32(wire.data + 16, 1); assert(!desk_parse(&wire, 1, NULL, &p));
    cp_settings_message(&wire, 42, DESK_SETTINGS_DISPLAY, 0x118, 0); wire.data[0] = DESK_VERSION; assert(!desk_parse(&wire, 1, NULL, &p));
    cp_settings_message(&wire, 42, DESK_SETTINGS_DISPLAY, 0x118, 0); wire.length = 20; assert(!desk_parse(&wire, 1, NULL, &p));
    cp_settings_message(&wire, 42, DESK_SETTINGS_DISPLAY, 65536, 0); assert(!desk_parse(&wire, 1, NULL, &p));
    cp_settings_message(&wire, 42, DESK_SETTINGS_DISPLAY, 0, 0); assert(desk_parse(&wire, 1, NULL, &p) && !p.values[0]); /* main delegates id validation to the kernel */
    cp_settings_message(&wire, 42, DESK_SETTINGS_MASTER, 70, 1); assert(desk_parse(&wire, 1, NULL, &p) && p.values[0] == 70 && p.values[1] == 70 && p.values[2] == 1);
    cp_settings_message(&wire, 43, DESK_SETTINGS_SOUND_RELOAD, 0, 0);
    assert(desk_parse(&wire, 1, NULL, &p) && p.request == DESK_SETTINGS_SOUND_RELOAD && !p.values[0]);
    desk_put32(wire.data + 12, 1); assert(!desk_parse(&wire, 1, NULL, &p));
    cp_settings_message(&wire, 44, DESK_SETTINGS_SOUND_TEST, SOUND_ERROR, 0);
    assert(desk_parse(&wire, 1, NULL, &p) && p.request == DESK_SETTINGS_SOUND_TEST && p.values[0] == SOUND_ERROR);
    cp_settings_message(&wire, 44, DESK_SETTINGS_SOUND_TEST, SOUND_EVENTS, 0); assert(!desk_parse(&wire, 1, NULL, &p));
    cp_settings_message(&wire, 45, DESK_SETTINGS_AUDIO_TEST, 0, 0);
    assert(desk_parse(&wire, 1, NULL, &p) && p.request == DESK_SETTINGS_AUDIO_TEST && !p.values[0]);
    desk_put32(wire.data + 20, 1); assert(!desk_parse(&wire, 1, NULL, &p));
    cp_settings_message(&wire,46,DESK_SETTINGS_OPEN_POWER,0,0);
    assert(desk_parse(&wire,1,NULL,&p) && p.request==DESK_SETTINGS_OPEN_POWER);
    desk_put32(wire.data+12,1);assert(!desk_parse(&wire,1,NULL,&p));
    init(); cp_open(&a,CP_DISPLAY); select(CP_DISPLAY_TABS_ID,CP_BACKGROUND);
    struct du_widget *wall=NULL,*red=NULL;
    for (unsigned i=0;i<u.count;i++) {
        struct du_widget *w=&u.widgets[i];
        if (w->kind==DU_LISTVIEW && w->box.x==248 && w->box.y==64) wall=w;
        if (w->kind==DU_SCROLLBAR && w->box.x==292 && w->box.y==256) red=w;
    }
    assert(wall && wall->count==16 && red && red->total==256);
    select(wall->id,8); assert(!strcmp(a.cfg.wallpaper,"pattern:0") && a.display_dirty);
    struct du_event colour={.type=DU_EV_CHANGE,.id=red->id,.index=18}; u.event(&u,&colour,u.user);
    assert(a.cfg.desktop_color==0x125564u && a.display_dirty);
    at_click_widget(&u,CP_APPLY);
    assert(strstr(saved,"wallpaper=pattern:0\n") && strstr(saved,"desktop_color=125564\n"));
    cp_detach(&a);
    puts("PASS Control Panel wire coexistence: display, sound reload/preview, hardware test, reserved fields");
    puts("PASS control display modes: ABI, synthetic mode lists, filtering, layout, Apply/ack/Keep/Revert/Esc, monotonic timeout, save/queue/query failures, close rollback, settings merge, wire validation, adapter lookup/render/refresh, System summary");
    return 0;
}
