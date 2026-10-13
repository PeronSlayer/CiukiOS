/* Reference frames of the CiukiOS look (ui-look): the production compositor
 * and libdeskui render a 1024x768 shell desktop with a window of every
 * control, an inactive CiukNote window, a windowshaded window, dialogs and
 * the Start menu, once per built-in theme. Host-only review material; never
 * guest evidence. Icons come from the pinned Tango store (icon_assets.py), as
 * on the guest. SPDX-License-Identifier: MIT
 *
 *   look_capture PORTRAIT.xrgb OUTDIR   -> OUTDIR/<theme>-{desktop,dialogs}.ppm
 *   look_capture PORTRAIT.xrgb OUTDIR WALLPAPER
 *       the desktop icons over the decoded image file (image.c, the default
 *       settings' placement), one icon selected, no window, and the same
 *       desktop with the icons turned off (the bare wallpaper):
 *       OUTDIR/<theme>-wallpaper.ppm, OUTDIR/<theme>-wallpaper-plain.ppm */
#include "fake.h"
#include "../../../apps/libdeskui/deskui.h"
#include "../../../apps/libdeskui/textarea.h"
#include "shell_icons.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 1024
#define H 768

static struct du_menu_item file_items[] = {{"&New", 1, 0x31, DU_CTRL, 0}, {"&Open...", 2, 0x18, DU_CTRL, 0},
    {"&Save", 3, 0x1f, DU_CTRL, 0}, {"Save &As...", 4, 0, 0, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"Page Set&up...", 5, 0, 0, DU_ITEM_DISABLED}, {"E&xit", 6, 0, 0, 0}};
static struct du_menu_item edit_items[] = {{"&Undo", 10, 0x2c, DU_CTRL, 0}, {NULL, 0, 0, 0, DU_ITEM_SEPARATOR},
    {"Cu&t", 11, 0x2d, DU_CTRL, 0}, {"&Copy", 12, 0x2e, DU_CTRL, 0}, {"&Paste", 13, 0x2f, DU_CTRL, 0}};
static struct du_menu_item view_items[] = {{"&Toolbar", 20, 0, 0, DU_ITEM_CHECKED}, {"&Status Bar", 21, 0, 0, DU_ITEM_CHECKED},
    {NULL, 0, 0, 0, DU_ITEM_SEPARATOR}, {"Lar&ge Icons", 22, 0, 0, DU_ITEM_RADIO}, {"&Details", 23, 0, 0, DU_ITEM_RADIO | DU_ITEM_CHECKED}};
static struct du_menu_item help_items[] = {{"&Help Topics", 30, 0x3b, 0, 0}, {"&About CiukiOS", 31, 0, 0, 0}};
static struct du_menu menus[] = {{"&File", file_items, 7}, {"&Edit", edit_items, 5}, {"&View", view_items, 5}, {"&Help", help_items, 2}};
static struct du_menu note_menus[] = {{"&File", file_items, 7}, {"&Edit", edit_items, 5}, {"F&ormat", view_items, 5},
                                      {"&View", view_items, 5}, {"&Help", help_items, 2}};

static struct du_item rows[6], tabs[3], nodes[6];
static int lister(void *ctx, const char *path, struct du_file_entry *out, unsigned capacity, unsigned *count)
{
    (void)ctx; (void)path; (void)capacity;
    static const char *const names[] = {"docs", "games", "readme.txt", "ciuki.txt", "notes.txt", "todo.txt"};
    for (unsigned i = 0; i < 6; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        snprintf(out[i].name, sizeof(out[i].name), "%s", names[i]);
        out[i].directory = i < 2; out[i].size = 1024u * (i + 1);
    }
    *count = 6;
    return 0;
}

static void controls(struct du_ui *u, uint32_t *pixels, int w, int h)
{
    du_init(u, pixels, w, h);
    u->menus = menus; u->menu_count = 4;
    du_add(u, DU_TOOLBAR, 100, (struct du_rect){0, 20, w, 44}, "");
    static const struct { const char *label; const uint32_t *icon; int sep; } tools[] = {
        {"Back", desk_icon_back22, 0}, {"Forward", desk_icon_forward22, 0}, {"Up", desk_icon_up22, 1},
        {"Cut", desk_icon_cut22, 0}, {"Copy", desk_icon_copy22, 0}, {"Paste", desk_icon_paste22, 1},
        {"Delete", desk_icon_delete22, 0}, {"Properties", desk_icon_properties22, 0}};
    int x = 4;
    for (int i = 0; i < 8; i++) {
        int bw = du_text_width(tools[i].label) + 16; if (bw < 46) bw = 46;
        struct du_widget *b = du_add(u, DU_TOOLBUTTON, 110 + i, (struct du_rect){x, 22, bw, 40}, tools[i].label);
        b->icon = tools[i].icon; b->icon_size = 22; b->disabled = i == 1 || i == 5;
        if (i == 6) b->hot = 1;
        x += bw + 1;
        if (tools[i].sep) { du_add(u, DU_SEPARATOR, 130 + i, (struct du_rect){x + 2, 26, 2, 32}, ""); x += 7; }
    }
    du_add(u, DU_LABEL, 140, (struct du_rect){8, 68, 52, 22}, "A&ddress");
    du_add(u, DU_EDIT, 141, (struct du_rect){62, 68, w - 70, 22}, "/Documents/ciuki")->icon = desk_icon_folder;
    struct du_widget *t = du_add(u, DU_TABS, 150, (struct du_rect){8, 98, w - 16, 196}, "");
    tabs[0].text[0] = "General"; tabs[1].text[0] = "Appearance"; tabs[2].text[0] = "About";
    t->items = tabs; t->count = 3; t->active = 1;
    du_add(u, DU_GROUP, 151, (struct du_rect){18, 126, 232, 94}, "&Theme");
    static const char *const names[] = {"&Platinum", "Ciuki &Blue", "Classic &98"};
    for (int i = 0; i < 3; i++) {
        struct du_widget *r = du_add(u, DU_RADIO, 152 + i, (struct du_rect){30, 144 + i * 22, 200, 20}, names[i]);
        r->total = 1; r->value = !strcmp(desk_theme->name, i == 0 ? "Platinum" : i == 1 ? "Ciuki Blue" : "Classic 98");
    }
    du_add(u, DU_GROUP, 156, (struct du_rect){260, 126, w - 278, 94}, "&Options");
    struct du_widget *c1 = du_add(u, DU_CHECKBOX, 157, (struct du_rect){272, 144, 220, 20}, "&Smooth fonts");
    c1->value = desk_theme->text_aa;
    du_add(u, DU_CHECKBOX, 158, (struct du_rect){272, 166, 220, 20}, "Window s&hadows")->value = desk_theme->shadow_size > 0;
    du_add(u, DU_CHECKBOX, 159, (struct du_rect){272, 188, 220, 20}, "Startup sound")->disabled = 1;
    struct du_widget *pr = du_add(u, DU_PROGRESS, 160, (struct du_rect){18, 230, 232, 18}, "");
    pr->total = 100; pr->value = 62;
    struct du_widget *e = du_add(u, DU_EDIT, 161, (struct du_rect){260, 228, w - 278, 22}, "Ciuki's desktop");
    e->anchor = 0; e->cursor = 7;
    du_add(u, DU_BUTTON, 162, (struct du_rect){w - 18 - 3 * 81, 262, 75, 24}, "&Apply")->def = 1;
    du_add(u, DU_BUTTON, 163, (struct du_rect){w - 18 - 2 * 81, 262, 75, 24}, "Cancel");
    du_add(u, DU_BUTTON, 164, (struct du_rect){w - 18 - 81, 262, 75, 24}, "&Help")->disabled = 1;
    struct du_widget *l = du_add(u, DU_LISTVIEW, 170, (struct du_rect){8, 302, 330, h - 302 - 26}, "");
    static const char *const files[6][3] = {{"docs", "", "File Folder"}, {"games", "", "File Folder"},
        {"ciuki.txt", "2 KB", "Text Document"}, {"readme.txt", "1 KB", "Text Document"},
        {"photo.png", "84 KB", "PNG Image"}, {"theme.wav", "12 KB", "Sound"}};
    for (int i = 0; i < 6; i++) {
        rows[i] = (struct du_item){{files[i][0], files[i][1], files[i][2]}, i < 2 ? desk_icon_folder : i < 4 ? desk_icon_text :
                                   i == 4 ? desk_icon_image : desk_icon_audio, NULL, (uint8_t)(i == 2), 0, 0, 0, 0};
    }
    l->items = rows; l->count = 6; l->active = 2; l->column_count = 3; l->sort = 0;
    l->columns[0] = (struct du_column){"Name", 120, 0}; l->columns[1] = (struct du_column){"Size", 60, 1};
    l->columns[2] = (struct du_column){"Type", 130, 0};
    struct du_widget *tr = du_add(u, DU_TREE, 171, (struct du_rect){344, 302, w - 352, h - 302 - 26}, "");
    static const char *const tree[6] = {"CiukiOS", "Programs", "Documents", "ciuki", "System", "tmp"};
    static const uint8_t depth[6] = {0, 1, 1, 2, 1, 1}, kids[6] = {1, 0, 1, 0, 1, 0}, open[6] = {1, 0, 1, 0, 0, 0};
    for (int i = 0; i < 6; i++)
        nodes[i] = (struct du_item){{tree[i]}, i ? desk_icon_folder : desk_icon_harddisk, NULL, (uint8_t)(i == 3), 0, depth[i], open[i], kids[i]};
    tr->items = nodes; tr->count = 6; tr->active = 3;
    struct du_widget *s = du_add(u, DU_STATUS, 172, (struct du_rect){0, h - 22, w, 22}, "6 object(s)\t99 KB\tCiukiOS");
    s->panels[0] = 200; s->panels[1] = 120;
    du_focus(u, 161);
}

static void note(struct du_ui *u, struct du_textarea *ta, uint32_t *pixels, int w, int h)
{
    du_init(u, pixels, w, h);
    u->menus = note_menus; u->menu_count = 5;
    static const char text[] = "CiukiOS - A modern Retro OS\n\nDedicated to Ciuki.\n\n"
                               "Inconsolata 12 for text, Inter 12 for the UI.\nCaf\xc3\xa9, perch\xc3\xa9, citt\xc3\xa0 \xe2\x80\xa2 \xc2\xa9 2026\n\n"
                               "int main(void) { return 0; }\n";
    assert(!du_ta_init(ta, 4096));
    du_ta_set_text(ta, text, sizeof(text) - 1);
    du_ta_select(ta, 29, 48);
    struct du_widget *t = du_add(u, DU_TEXTAREA, 1, (struct du_rect){0, 20, w, h - 20 - 22}, "");
    t->data = ta;
    struct du_widget *s = du_add(u, DU_STATUS, 2, (struct du_rect){0, h - 22, w, 22}, "notes.txt\tLn 3, Col 1");
    s->panels[0] = w - 110;
    du_focus(u, 1);
}

/* The desktop's icon hook (desktop.c store_icon): libdeskui's Tango store. */
static const uint32_t *store_icon(const char *object, unsigned size) { return du_icon_argb(du_icon_name(object), size); }

static void window(struct desktop *d, int i, const char *title, uint32_t *pixels, int w, int h, int x, int y, int program)
{
    struct desk_client *c = &d->clients[i];
    c->window = 1; c->info.width = (uint32_t)w; c->info.height = (uint32_t)h; c->pixels = pixels;
    c->frame = desk_frame((uint32_t)w, (uint32_t)h, x, y); c->order = ++d->order; c->program = program;
    snprintf(c->title, sizeof(c->title), "%s", title);
}

static void save(const struct desktop *d, const char *dir, const char *theme, const char *what)
{
    char path[512], slug[32]; size_t n = 0;
    for (const char *s = theme; *s && n + 1 < sizeof(slug); s++) slug[n++] = *s == ' ' ? '-' : (char)(*s | 0x20);
    slug[n] = 0;
    snprintf(path, sizeof(path), "%s/%s-%s.ppm", dir, slug, what);
    FILE *f = fopen(path, "wb"); assert(f);
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int k = 0; k < W * H; k++) {
        uint32_t v = d->pixels[k];
        unsigned char rgb[3] = {(unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v};
        fwrite(rgb, 1, 3, f);
    }
    assert(!fclose(f));
    printf("%s\n", path);
}

static int wallpaper_frames(const uint32_t *portrait, const char *dir, const char *path)
{
    FILE *f = fopen(path, "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long n = ftell(f); assert(n > 0 && !fseek(f, 0, SEEK_SET));
    uint8_t *data = malloc((size_t)n); assert(data && fread(data, 1, (size_t)n, f) == (size_t)n && !fclose(f));
    static uint32_t screen[W * H];
    for (int pass = 0; pass < 2 * desk_theme_count(); pass++) {
        int theme = pass / 2, icons = !(pass & 1);
        assert(!desk_theme_use(desk_theme_at(theme)->name));
        struct desktop *d = calloc(1, sizeof(*d)); assert(d);
        desk_init(d, &fake_ops, W, H);
        d->pixels = screen; d->portrait = portrait; d->icon = store_icon; d->realtime_second = 12 * 3600 + 34 * 60 + 56;
        cfg_defaults(&d->sh.cfg);   /* the default wallpaper placement; the image stands for its file */
        snprintf(d->sh.cfg.theme, sizeof(d->sh.cfg.theme), "%s", desk_theme_at(theme)->name);
        d->sh.cfg.desktop_icons = icons;
        assert(!desk_image_decode(data, (size_t)n, &d->sh.image));
        static const char *const programs[] = {"ciuknote", "control", "dosbox", "files"};
        for (int i = 0; i < 4; i++) {
            snprintf(d->programs[i].name, sizeof(d->programs[i].name), "%s", programs[i]);
            snprintf(d->programs[i].path, sizeof(d->programs[i].path), "/Programs/%s", programs[i]);
        }
        d->program_count = 4;
        desk_shell_enable(d);
        assert(!desk_shell_wallpaper_update(d));
        d->sh.icon_selected = 1;    /* Documents: the translucent rounded selection */
        desk_composite(d, (struct desk_box){0, 0, W, H});
        save(d, dir, desk_theme->name, icons ? "wallpaper" : "wallpaper-plain");
        desk_image_free(&d->sh.image); free(d->sh.wall); free(d);
    }
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    assert(argc == 3 || argc == 4);
    uint32_t *portrait = malloc(256 * 256 * 4); assert(portrait);
    FILE *f = fopen(argv[1], "rb"); assert(f && fread(portrait, 4, 256 * 256, f) == 256 * 256 && !fclose(f));
    if (argc == 4) { int r = wallpaper_frames(portrait, argv[2], argv[3]); free(portrait); return r; }
    static uint32_t screen[W * H], a[520 * 430], b[440 * 300], c[300 * 200];
    static struct du_ui ua, ub, uc; static struct du_textarea ta;
    for (int theme = 0; theme < desk_theme_count(); theme++) {
        assert(!desk_theme_use(desk_theme_at(theme)->name));
        for (int pass = 0; pass < 2; pass++) {
            struct desktop *d = calloc(1, sizeof(*d)); assert(d);
            desk_init(d, &fake_ops, W, H);
            d->pixels = screen; d->portrait = portrait; d->icon = store_icon; d->realtime_second = 12 * 3600 + 34 * 60 + 56;
            /* The shell applies desktop.cfg's theme: name this pass's theme there. */
            cfg_defaults(&d->sh.cfg);
            snprintf(d->sh.cfg.theme, sizeof(d->sh.cfg.theme), "%s", desk_theme_at(theme)->name);
            static const char *const programs[] = {"ciuknote", "demo", "dosbox", "files", "lua", "tasks"};
            for (int i = 0; i < 6; i++) {
                snprintf(d->programs[i].name, sizeof(d->programs[i].name), "%s", programs[i]);
                snprintf(d->programs[i].path, sizeof(d->programs[i].path), "/Programs/%s", programs[i]);
            }
            d->program_count = 6;
            desk_shell_enable(d);
            assert(desk_theme == desk_theme_at(theme));
            controls(&ua, a, 520, 430);
            note(&ub, &ta, b, 440, 300);
            du_init(&uc, c, 300, 200);
            if (!pass) {
                window(d, 2, "Task Manager", c, 300, 200, 600, 70, 0);
                window(d, 1, "notes.txt - CiukNote", b, 440, 300, 572, 340, DESK_APP_NOTE);
                window(d, 0, "Display Properties", a, 520, 430, 36, 60, DESK_APP_FILES);
                desk_collapse(d, 2);
                d->focus = 0;
                du_window_focus(&ub, 0);
            } else {
                static const char *const yes[] = {"&Yes", "&No", "Cancel"};
                du_message(&ua, 9, "Apply Theme", "Switch the desktop to this theme?\nOpen windows repaint at once.",
                           DU_DLG_ICON_QUESTION, yes, 3, 0, 2);
                du_window_focus(&ua, 0);
                assert(!du_file_dialog(&ub, 8, 0, "Open", "/Documents/ciuki", "ciuki.txt", lister, NULL));
                window(d, 0, "Display Properties", a, 520, 430, 36, 60, DESK_APP_FILES);
                window(d, 1, "notes.txt - CiukNote", b, 440, 300, 420, 200, DESK_APP_NOTE);
                du_menu_open(&ua, 2, 1);
                desk_menu_open(d, SH_MENU_START, 0, 0);   /* the shell's Start menu */
                d->focus = 1;
            }
            du_render(&ua); du_render(&ub); du_render(&uc);
            if (pass) { struct du_rect r = du_menu_title(&ua, 2); (void)r; }
            desk_composite(d, (struct desk_box){0, 0, W, H});
            save(d, argv[2], desk_theme->name, pass ? "dialogs" : "desktop");
            if (!pass) {
                d->sh.mixer_state = -1;
                d->sh.muted = 1;
                desk_composite(d, (struct desk_box){0, 0, W, H});
                save(d, argv[2], desk_theme->name, "tray-disabled");
                d->sh.mixer_state = 2;
                d->sh.muted = 0;
                d->sh.network.present = 1;
                d->sh.network.link = CIUKI_NET_LINK_UP;
                d->sh.media.removable_mask = 1u << 4;
                desk_composite(d, (struct desk_box){0, 0, W, H});
                save(d, argv[2], desk_theme->name, "tray-media");
            }
            du_dialog_close(&ua); du_dialog_close(&ub); du_ta_free(&ta);
            free(d->sh.wall); free(d);
        }
    }
    free(portrait);
    return 0;
}
