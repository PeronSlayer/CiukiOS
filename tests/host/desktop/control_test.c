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
static int load(void *c, char *t, size_t s, size_t *n) { (void)c; if (!saved_n || saved_n > s) return -1; memcpy(t, saved, saved_n); *n = saved_n; return 0; }
static int save(void *c, const char *t, size_t n) { (void)c; if (n >= sizeof(saved)) return -1; memcpy(saved, t, n); saved[n] = 0; saved_n = n; saves++; return 0; }
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

int main(int argc, char **argv)
{
    assert(argc == 3 || argc == 4);
    FILE *f = fopen(argv[1], "rb"); assert(f && fread(portrait, 4, 256 * 256, f) == 256 * 256); fclose(f);
    folder = argv[2];
    const char *renders = argc == 4 ? argv[3] : NULL;

    /* No desktop.cfg yet: the defaults, Ciuk2 filled. */
    attach(CP_DISPLAY);
    assert(!strcmp(a.cfg.theme, "Ciuki Blue") && !strcmp(desk_theme->name, "Ciuki Blue"));
    for (int k = 0; k < CFG_THEMES; k++) assert(a.theme_items[k].selected == (k == cfg_theme_index("Ciuki Blue")));
    assert(a.cfg.wallpaper_mode == CFG_WALL_FILL && !strcmp(a.cfg.wallpaper, "/system/wallpapers/Ciuk2.png"));
    struct du_widget *list = NULL;
    for (unsigned i = 0; i < u.count; i++) if (u.widgets[i].kind == DU_LISTVIEW && u.widgets[i].count == 2 + DESK_BUILTIN_WALLPAPERS + 3) list = &u.widgets[i];
    assert(list && !strcmp(list->items[list->active].text[0], "Ciuk2.png") && list->items[list->active].selected);
    check_marked(CFG_WALL_FILL);

    /* Layout: the label, then two rows of three 68 px buttons under the 208 px
     * monitor (x 24..232, y 44..215), clear of every other Display widget. */
    struct du_widget *label = find(DU_LABEL, "Position:"), *tabs = NULL;
    assert(label && !label->hidden);
    for (unsigned i = 0; i < u.count; i++) if (u.widgets[i].kind == DU_TABS) tabs = &u.widgets[i];
    assert(tabs);
    struct du_rect monitor = {24, 44, 208, 172}, column = {24, 216, 208, CP_HEIGHT - 52 + 8 - 216};
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
    struct ciuki_settings parsed;
    cfg_defaults(&parsed); parsed.wallpaper_mode = CFG_WALL_CENTER;
    assert(!cfg_parse(&parsed, saved, saved_n) && parsed.wallpaper_mode == CFG_WALL_FILL);

    cp_detach(&a);
    /* Other pages hide the buttons. */
    attach(CP_MOUSE);
    for (int k = 0; k < 5; k++) assert(!find(DU_BUTTON, labels[k]));
    cp_detach(&a);

    /* A file written before fill keeps its mode; Ciuk1 is selected. */
    static const char old[] = "version=1\ntheme=Classic 98\nwallpaper=/system/wallpapers/Ciuk1.png\nwallpaper_mode=center\n";
    memcpy(saved, old, sizeof(old)); saved_n = sizeof(old) - 1;
    attach(CP_DISPLAY);
    assert(a.cfg.wallpaper_mode == CFG_WALL_CENTER && !strcmp(a.cfg.theme, "Classic 98"));
    for (int k = 0; k < CFG_THEMES; k++) assert(a.theme_items[k].selected == (k == cfg_theme_index("Classic 98")));
    check_marked(CFG_WALL_CENTER);
    for (int k = 0; k < 5; k++) assert(du_text_width(labels[k]) + 8 <= find(DU_BUTTON, labels[k])->box.w);
    render_settled(&u);
    check_preview();
    at_click_widget(&u, find(DU_BUTTON, "Fill")->id);
    at_click_widget(&u,CP_APPLY);
    assert(a.cfg.wallpaper_mode == CFG_WALL_FILL && strstr(saved, "\nwallpaper=/system/wallpapers/Ciuk1.png\nwallpaper_mode=fill\n"));
    render_settled(&u);
    check_preview();
    at_save(renders, "control-display-classic-fill", &u);
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
    for (unsigned i=0;i<2;i++) at_tap(&u,DU_KEY_TAB);
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
    puts("PASS Display Properties: staged edits, preview/Cancel merge, durable OK, pending-close rollback and failed save");
    puts("PASS F1-63 Control Panel: two Tab presses and Space apply master preference 20/20/muted");
    printf("PASS control display: five positions, layout, desktop.cfg modes, preview\n");
    return 0;
}
