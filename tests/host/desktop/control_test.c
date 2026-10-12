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

static char saved[CFG_TEXT_MAX]; static size_t saved_n; static unsigned saves, applies, decodes; static const char *folder;
static int load(void *c, char *t, size_t s, size_t *n) { (void)c; if (!saved_n || saved_n > s) return -1; memcpy(t, saved, saved_n); *n = saved_n; return 0; }
static int save(void *c, const char *t, size_t n) { (void)c; if (n >= sizeof(saved)) return -1; memcpy(saved, t, n); saved[n] = 0; saved_n = n; saves++; return 0; }
static int apply(void *c, unsigned r, int v, int m) { (void)c; (void)v; (void)m; assert(r == DESK_SETTINGS_RELOAD); applies++; return 0; }
static int mixer(void *c, int *v, int *m) { (void)c; *v = 70; *m = 0; return 0; }
static uint64_t now(void *c) { (void)c; return 1791763200ull + 20 * 3600 + 46 * 60 + 12; }
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
                              .apply = apply, .now = now, .portrait = portrait, .screen_w = 1024, .screen_h = 768,
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
        if (p->box.y + p->box.h <= CP_HEIGHT - 52 + 8) {   /* inside the tab sheet (the status line and Close are below it) */
            assert(inside(p->box, tabs->box) && !overlap(p->box, monitor));
        }
        for (unsigned j = i + 1; j < u.count; j++) {
            struct du_widget *q = &u.widgets[j];
            if (!q->hidden && q->kind != DU_TABS) assert(!overlap(p->box, q->box));
        }
    }
    assert(visible >= 5 + 6 + 8);

    du_render(&u);
    check_preview();
    assert(!preview_band());
    at_save(renders, "control-display-fill", &u);

    /* Each button saves its desktop.cfg name, applies, marks itself and
     * rebuilds the preview with that mode. */
    for (int k = 0; k < 5; k++) {
        unsigned s0 = saves, a0 = applies, d0 = decodes;
        at_click_widget(&u, find(DU_BUTTON, labels[k])->id);
        assert(a.cfg.wallpaper_mode == modes[k] && saves == s0 + 1 && applies == a0 + 1);
        char line[64]; snprintf(line, sizeof(line), "\nwallpaper_mode=%s\n", cfg_mode_names[modes[k]]);
        assert(strstr(saved, line) && strstr(saved, "\nwallpaper=/system/wallpapers/Ciuk2.png\n"));
        assert(find(DU_LABEL, "Saved and applied."));
        check_marked(modes[k]);
        assert(!a.preview_valid);
        du_render(&u);
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
    for (int k = 0; k < 5; k++) assert(find(DU_BUTTON, labels[k])->hidden);
    cp_detach(&a);

    /* A file written before fill keeps its mode; Ciuk1 is selected. */
    static const char old[] = "version=1\ntheme=Classic 98\nwallpaper=/system/wallpapers/Ciuk1.png\nwallpaper_mode=center\n";
    memcpy(saved, old, sizeof(old)); saved_n = sizeof(old) - 1;
    attach(CP_DISPLAY);
    assert(a.cfg.wallpaper_mode == CFG_WALL_CENTER && !strcmp(a.cfg.theme, "Classic 98"));
    check_marked(CFG_WALL_CENTER);
    for (int k = 0; k < 5; k++) assert(du_text_width(labels[k]) + 8 <= find(DU_BUTTON, labels[k])->box.w);
    du_render(&u);
    check_preview();
    at_click_widget(&u, find(DU_BUTTON, "Fill")->id);
    assert(a.cfg.wallpaper_mode == CFG_WALL_FILL && strstr(saved, "\nwallpaper=/system/wallpapers/Ciuk1.png\nwallpaper_mode=fill\n"));
    du_render(&u);
    check_preview();
    at_save(renders, "control-display-classic-fill", &u);
    cp_detach(&a);
    printf("PASS control display: five positions, layout, desktop.cfg modes, preview\n");
    return 0;
}
