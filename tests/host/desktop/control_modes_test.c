/* SPDX-License-Identifier: MIT
 * Production Control Panel display model, widgets and protocol; host evidence. */
#include "control.h"
#include "acc_test.h"
#include "display_fixture.h"
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
    if (request == DESK_SETTINGS_DISPLAY) { assert(!flag && !p.values[1] && !p.values[2]); requested = (uint16_t)p.values[0]; }
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
static void attach(int page)
{
    memset(&a, 0, sizeof(a));
    a.host = (struct cp_host){.load = load, .save = save, .apply = apply, .display = query, .portrait = portrait,
                             .screen_w = 1024, .screen_h = 768, .machine = "i686"};
    du_init(&u, pixels, CP_WIDTH, CP_HEIGHT); u.focused_window = 1;
    assert(!cp_attach(&a, &u, page) && u.count < DU_WIDGETS);
}
static void init(void)
{
    display_fixture(&info); query_error = save_error = queue_error = requests = saves = 0; requested = 0;
    strcpy(saved, "wallpaper=none\ndisplay_mode=280\nfuture_key=kept\nrecent=/Documents/note.txt\n"); saved_n = strlen(saved);
    attach(CP_DISPLAY);
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
            assert(!(p->box.x < q->box.x + q->box.w && q->box.x < p->box.x + p->box.w && p->box.y < q->box.y + q->box.h && q->box.y < p->box.y + p->box.h));
        }
    }
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    FILE *f = fopen(argv[2], "rb"); assert(f && fread(portrait, 4, 256 * 256, f) == 256 * 256); fclose(f);
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
    init(); start(); ack(); choose(1); assert(a.mode_state == CP_MODE_REVERT && requested == 0x118 && !saves);
    changed_mode(requested); cp_display_result(&a, 0); cp_detach(&a);
    init(); start(); ack(); at_tap(&u, DU_KEY_ESC); assert(a.mode_state == CP_MODE_REVERT && !saves); cp_display_result(&a, -EIO); cp_detach(&a);
    init(); start(); cp_display_result(&a, -EBUSY); assert(a.mode_state == CP_MODE_IDLE && !saves && !u.dialog.kind); cp_detach(&a);
    init(); start(); ack(); save_error = 1; choose(0); assert(a.mode_state == CP_MODE_REVERT && !saves && requested == 0x118); changed_mode(requested); cp_display_result(&a, 0); cp_detach(&a);
    init(); start(); du_tick(&u, u.now + 15000000000ull); assert(a.mode_state == CP_MODE_REVERT && requested == 0x118); du_tick(&u, u.now + 15000000000ull); assert(a.mode_state == CP_MODE_IDLE && !saves); cp_detach(&a);
    init(); queue_error = 1; select(CP_AREA, 0); at_click_widget(&u, CP_APPLY); assert(a.mode_state == CP_MODE_IDLE && !requests && !saves); cp_detach(&a);
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
    init(); select(1, CP_ADAPTER); check_layout(); assert(!du_get(&u, CP_REFRESH)->hidden && du_get(&u, CP_AREA)->hidden);
    assert(strstr(a.adapter_lines[0], "1024 x 768") && strstr(a.adapter_lines[1], "4096") && strstr(a.adapter_lines[2], "2.00") && strstr(a.adapter_lines[2], "16384"));
    assert(strstr(a.adapter_lines[4], "SuperSavage") && strstr(a.adapter_lines[9], "no EDID yet"));
    du_render(&u); at_save(argv[1], "control-adapter", &u);
    info.pci_vendor = 0x1002; info.pci_device = 0x4c52; at_click_widget(&u, CP_REFRESH); assert(strstr(a.adapter_lines[4], "ATI Rage Mobility"));
    select(1, CP_SYSTEM); assert(strstr(du_get(&u, CP_SUMMARY)->text, "32-bit")); du_render(&u); at_save(argv[1], "control-system-display", &u);
    char name[64];
    assert(strstr(cp_adapter_name(0x5333, 0x8c2f, name, sizeof(name)), "IX/C"));
    assert(strstr(cp_adapter_name(0x8086, 0x1132, name, sizeof(name)), "815"));
    assert(strstr(cp_adapter_name(0x10de, 0x0110, name, sizeof(name)), "GeForce2"));
    assert(strstr(cp_adapter_name(0x5333, 0x8d04, name, sizeof(name)), "VIA"));
    assert(strstr(cp_adapter_name(0x1106, 0x3122, name, sizeof(name)), "CLE266"));
    assert(strstr(cp_adapter_name(0x1106, 0x7205, name, sizeof(name)), "UniChrome"));
    assert(strstr(cp_adapter_name(0x8086, 0x2572, name, sizeof(name)), "865G"));
    assert(strstr(cp_adapter_name(0x121a, 0x0005, name, sizeof(name)), "Voodoo3"));
    assert(!strcmp(cp_adapter_name(0xdead, 0xbeef, name, sizeof(name)), "Unknown adapter (dead:beef)"));
    info.pci_vendor = 0xdead; info.pci_device = 0xbeef; select(1, CP_ADAPTER); du_render(&u); at_save(argv[1], "control-adapter-unknown", &u);
    query_error = 1; at_click_widget(&u, CP_REFRESH); du_render(&u); at_save(argv[1], "control-adapter-unavailable", &u); cp_detach(&a);
    struct desk_packet p;
    cp_settings_message(&wire, 42, DESK_SETTINGS_DISPLAY, 0x118, 0); assert(desk_parse(&wire, 1, NULL, &p));
    assert(!desk_parse(&wire, 0, NULL, &p)); desk_put32(wire.data + 16, 1); assert(!desk_parse(&wire, 1, NULL, &p));
    cp_settings_message(&wire, 42, DESK_SETTINGS_DISPLAY, 0x118, 0); wire.data[0] = DESK_VERSION; assert(!desk_parse(&wire, 1, NULL, &p));
    cp_settings_message(&wire, 42, DESK_SETTINGS_DISPLAY, 0x118, 0); wire.length = 20; assert(!desk_parse(&wire, 1, NULL, &p));
    cp_settings_message(&wire, 42, DESK_SETTINGS_DISPLAY, 65536, 0); assert(!desk_parse(&wire, 1, NULL, &p));
    cp_settings_message(&wire, 42, DESK_SETTINGS_DISPLAY, 0, 0); assert(desk_parse(&wire, 1, NULL, &p) && !p.values[0]); /* main delegates id validation to the kernel */
    cp_settings_message(&wire, 42, DESK_SETTINGS_MASTER, 70, 1); assert(desk_parse(&wire, 1, NULL, &p) && p.values[0] == 70 && p.values[1] == 70 && p.values[2] == 1);
    puts("PASS control display modes: ABI, synthetic mode lists, filtering, layout, Apply/ack/Keep/Revert/Esc, monotonic timeout, save/queue/query failures, close rollback, settings merge, wire validation, adapter lookup/render/refresh, System summary");
    return 0;
}
