/* SPDX-License-Identifier: MIT
 * Control Panel pages (control.h). No direct I/O: everything goes through
 * struct cp_host, so the host capture can render every page. */
#include "control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    C_TABS = 1, C_CLOSE, C_STATUS,
    C_THEME_L, C_THEME, C_WALL_L, C_WALL, C_POS_L, C_POS0, C_POS1, C_POS2, C_POS3, C_COL_L,
    C_COL0, C_COL1, C_COL2, C_COL3, C_COL4, C_COL5, C_ICONS,
    C_SPEED_L, C_SPEED, C_SPEED_V, C_SLOW, C_FAST, C_DBL_L, C_DBL, C_DBL_V, C_MOUSE_NOTE,
    C_KEY_L, C_KEY, C_KEY_NOTE1, C_KEY_NOTE2,
    C_VOL_L, C_VOL, C_VOL_V, C_MUTE, C_AUDIO,
    C_DATE, C_H24, C_SECONDS, C_TIME_NOTE1, C_TIME_NOTE2,
    C_SYS0, C_SYS1, C_SYS2, C_SYS3, C_SYS4
};
static const char *const page_names[CP_PAGES] = {"display", "mouse", "keyboard", "sounds", "datetime", "system"};
static const char *const page_labels[CP_PAGES] = {"Display", "Mouse", "Keyboard", "Sounds", "Date & Time", "System"};
static const uint32_t swatches[6] = {0x375564u, 0x2f6f6bu, 0x3a3f6bu, 0x6b4f3au, 0x808080u, 0x101820u};
static const int mode_order[4] = {CFG_WALL_CENTER, CFG_WALL_TILE, CFG_WALL_STRETCH, CFG_WALL_FIT};
/* Page of each widget id (-1: every page). */
static int page_of(int id)
{
    if (id >= C_THEME_L && id <= C_ICONS) return CP_DISPLAY;
    if (id >= C_SPEED_L && id <= C_MOUSE_NOTE) return CP_MOUSE;
    if (id >= C_KEY_L && id <= C_KEY_NOTE2) return CP_KEYBOARD;
    if (id >= C_VOL_L && id <= C_AUDIO) return CP_SOUNDS;
    if (id >= C_DATE && id <= C_TIME_NOTE2) return CP_DATETIME;
    if (id >= C_SYS0 && id <= C_SYS4) return CP_SYSTEM;
    return -1;
}
int cp_page_index(const char *name)
{
    for (int i = 0; i < CP_PAGES; i++) if (name && !strcmp(name, page_names[i])) return i;
    return CP_DISPLAY;
}
static struct du_widget *add(struct cp_app *a, enum du_kind k, int id, int x, int y, int w, int h, const char *text)
{ return du_add(a->ui, k, id, (struct du_rect){x, y, w, h}, text); }
static void text(struct cp_app *a, int id, const char *s)
{ struct du_widget *w = du_get(a->ui, id); if (w) { du_set_text(w, s); a->ui->dirty = 1; } }

/* ---- settings: merge the panel's keys into the file as it is now ---- */
static void load(struct cp_app *a, struct ciuki_settings *out)
{
    static char buffer[CFG_TEXT_MAX]; size_t n = 0;
    cfg_defaults(out);
    if (a->host.load && !a->host.load(a->host.ctx, buffer, sizeof(buffer), &n)) cfg_parse(out, buffer, n);
}
static void changed(struct cp_app *a)
{
    static struct ciuki_settings now;
    load(a, &now);            /* keep what the desktop wrote meanwhile: recent documents, unknown keys */
    memcpy(now.theme, a->cfg.theme, sizeof(now.theme));
    memcpy(now.wallpaper, a->cfg.wallpaper, sizeof(now.wallpaper));
    now.wallpaper_mode = a->cfg.wallpaper_mode; now.desktop_color = a->cfg.desktop_color;
    now.clock_24h = a->cfg.clock_24h; now.clock_seconds = a->cfg.clock_seconds; now.desktop_icons = a->cfg.desktop_icons;
    now.mouse_speed = a->cfg.mouse_speed; now.double_click_ms = a->cfg.double_click_ms; now.keyboard = a->cfg.keyboard;
    static char out[CFG_TEXT_MAX];
    size_t n = cfg_format(&now, out, sizeof(out));
    if (!n || !a->host.save || a->host.save(a->host.ctx, out, n)) { text(a, C_STATUS, "The settings could not be saved."); return; }
    int r = a->host.apply ? a->host.apply(a->host.ctx, DESK_SETTINGS_RELOAD, 0, 0) : -1;
    text(a, C_STATUS, r ? "Saved. The desktop applies it at its next start." : "Saved and applied.");
}

/* ---- display preview ---- */
/* The desktop colour as the desktop paints it: the unchanged default
 * follows the theme's desktop (shell.c desktop_colour). */
static uint32_t desktop_colour(const struct cp_app *a, const struct desk_theme *t)
{
    struct ciuki_settings defaults;
    cfg_defaults(&defaults);
    return a->cfg.desktop_color == defaults.desktop_color ? t->desktop : a->cfg.desktop_color;
}
static const struct desk_theme *chosen(const struct cp_app *a)
{ const struct desk_theme *t = desk_theme_named(a->cfg.theme); return t ? t : desk_theme; }
static void preview_build(struct cp_app *a)
{
    uint32_t sw = a->host.screen_w ? a->host.screen_w : 1024, sh = a->host.screen_h ? a->host.screen_h : 768;
    uint32_t *full = malloc((size_t)sw * sh * 4u);
    desk_image_free(&a->image);
    if (a->cfg.wallpaper[0] == '/' && a->host.decode) a->host.decode(a->host.ctx, a->cfg.wallpaper, &a->image);
    if (full) {
        desk_wallpaper_render(full, sw, sh, 32, 32, a->cfg.wallpaper, a->cfg.wallpaper_mode, desktop_colour(a, chosen(a)), &a->image, a->host.portrait);
        struct desk_image src = {full, sw, sh};
        for (size_t i = 0; i < (size_t)sw * sh; i++) full[i] |= 0xff000000u;
        desk_image_scale(&src, a->preview, 192, 144, CFG_WALL_STRETCH, desktop_colour(a, chosen(a)));
        free(full);
    } else for (size_t i = 0; i < 192 * 144; i++) a->preview[i] = desktop_colour(a, chosen(a));
    desk_image_free(&a->image);
    a->preview_valid = 1;
}
static void put(struct du_canvas *c, int x, int y, int w, int h, uint32_t colour) { du_fill(c, (struct du_rect){x, y, w, h}, colour); }
static void paint(struct du_ui *u, void *user)
{
    struct cp_app *a = user;
    struct du_canvas *c = &u->canvas;
    if (a->page == CP_DISPLAY) {
        if (!a->preview_valid) preview_build(a);
        /* A small monitor: bezel, screen, stand. */
        put(c, 24, 44, 208, 160, DU_FACE);
        du_edge(c, (struct du_rect){24, 44, 208, 160}, DU_RAISED);
        du_edge(c, (struct du_rect){30, 50, 196, 148}, DU_SUNKEN);
        for (int y = 0; y < 144; y++) for (int x = 0; x < 192; x++) put(c, 32 + x, 52 + y, 1, 1, a->preview[y * 192 + x]);
        put(c, 108, 204, 40, 6, DU_SHADOW); put(c, 92, 210, 72, 4, DU_FACE); du_edge(c, (struct du_rect){92, 210, 72, 5}, DU_RAISED_THIN);
        /* The chosen theme in miniature: top bar with the Ciuki button, taskbar and a window. */
        const struct desk_theme *t = chosen(a);
        struct desk_paint pp = {c->pixels, c->width, c->height, c->clip.x, c->clip.y, c->clip.w, c->clip.h};
        put(c, 32, 52, 192, 6, t->topbar); put(c, 32, 57, 192, 1, t->topbar_edge); put(c, 33, 53, 16, 4, t->start);
        put(c, 32, 190, 192, 6, t->taskbar);
        put(c, 40, 132, 70, 50, t->face); du_outline(c, (struct du_rect){40, 132, 70, 50}, t->frame);
        if (t->title_style == DESK_TITLE_PINSTRIPE) {
            desk_paint_vgradient(&pp, 41, 133, 68, 8, t->title[0], t->title[1]);
            desk_paint_pinstripes(&pp, 43, 134, 64, 6, t->stripe_light, t->stripe_dark);
        } else if (t->control_style == DESK_CONTROLS_CLASSIC) desk_paint_hgradient(&pp, 42, 134, 66, 7, t->title[0], t->title[1]);
        else desk_paint_vgradient(&pp, 41, 133, 68, 8, t->title[0], t->title[1]);
        put(c, 44, 143, 62, 37, t->window);
        for (int k = 0; k < 6; k++) {
            struct du_widget *w = du_get(u, C_COL0 + k);
            if (!w || w->hidden) continue;
            put(c, w->box.x + 4, w->box.y + 4, w->box.w - 8, w->box.h - 8, k ? swatches[k] : chosen(a)->desktop);
            du_outline(c, (struct du_rect){w->box.x + 3, w->box.y + 3, w->box.w - 6, w->box.h - 6}, DU_DARK);
            if (swatches[k] == a->cfg.desktop_color) du_outline(c, (struct du_rect){w->box.x - 1, w->box.y - 1, w->box.w + 2, w->box.h + 2}, DU_SELECT);
        }
    } else if (a->page == CP_DATETIME) {
        du_well(c, (struct du_rect){24, 44, 452, 54}, DU_WINDOW);
        int font = desk_theme->font_large;
        du_text_font(c, font, 24 + (452 - du_text_width_font(font, a->clock_line)) / 2, 71 - desk_font_cap(font) / 2, a->clock_line, DU_SELECT);
    } else if (a->page == CP_SYSTEM && a->host.portrait) {
        du_edge(c, (struct du_rect){22, 46, 100, 100}, DU_SUNKEN);
        for (int y = 0; y < 96; y++) for (int x = 0; x < 96; x++) put(c, 24 + x, 48 + y, 1, 1, a->host.portrait[(y * 256 / 96) * 256 + x * 256 / 96]);
        du_text_font(c, desk_theme->font_large, 140, 50, "CiukiOS", DU_SELECT);
    }
}

/* ---- state to widgets ---- */
static void toggle_label(struct cp_app *a, int id, const char *name, int on)
{ char s[64]; snprintf(s, sizeof(s), "%s: %s", name, on ? "On" : "Off"); text(a, id, s); }
static void sync(struct cp_app *a)
{
    struct du_ui *u = a->ui;
    for (unsigned i = 0; i < u->count; i++) {
        int p = page_of(u->widgets[i].id);
        u->widgets[i].hidden = p >= 0 && p != a->page;
    }
    struct du_widget *w = du_get(u, C_TABS); if (w) w->active = (unsigned)a->page;
    int t = cfg_theme_index(a->cfg.theme);
    for (int k = 0; k < CFG_THEMES; k++) a->theme_items[k].selected = k == t;
    if ((w = du_get(u, C_THEME))) w->active = t < 0 ? 0 : (unsigned)t;
    unsigned wall = 0;
    for (unsigned k = 0; k < 2 + DESK_BUILTIN_WALLPAPERS + a->wall_count; k++) {
        const char *spec = k == 0 ? "none" : k == 1 ? "ciuki" : NULL;
        char built[48];
        if (!spec && k < 2 + DESK_BUILTIN_WALLPAPERS) { snprintf(built, sizeof(built), "builtin:%s", desk_builtin_wallpapers[k - 2].id); spec = built; }
        if (!spec) spec = a->wall_names[k - 2 - DESK_BUILTIN_WALLPAPERS];
        a->wall_items[k].selected = !strcmp(spec, a->cfg.wallpaper);
        if (a->wall_items[k].selected) wall = k;
    }
    if ((w = du_get(u, C_WALL))) w->active = wall;
    for (int k = 0; k < 4; k++) if ((w = du_get(u, C_POS0 + k))) w->def = mode_order[k] == a->cfg.wallpaper_mode;
    toggle_label(a, C_ICONS, "Desktop icons", a->cfg.desktop_icons);
    char s[64];
    if ((w = du_get(u, C_SPEED))) w->value = a->cfg.mouse_speed - CFG_MOUSE_MIN;
    snprintf(s, sizeof(s), "%d of %d%s", a->cfg.mouse_speed, CFG_MOUSE_MAX, a->cfg.mouse_speed == CFG_MOUSE_NORMAL ? " (normal)" : ""); text(a, C_SPEED_V, s);
    if ((w = du_get(u, C_DBL))) w->value = (a->cfg.double_click_ms - 200) / 100;
    snprintf(s, sizeof(s), "%d ms", a->cfg.double_click_ms); text(a, C_DBL_V, s);
    for (int k = 0; k < CFG_KEYS_COUNT; k++) a->key_items[k].selected = k == a->cfg.keyboard;
    if ((w = du_get(u, C_KEY))) w->active = (unsigned)a->cfg.keyboard;
    if ((w = du_get(u, C_VOL))) { w->value = a->volume; w->disabled = !a->mixer_ok; }
    snprintf(s, sizeof(s), "%d%%", a->volume); text(a, C_VOL_V, s);
    toggle_label(a, C_MUTE, "Mute", a->muted);
    if ((w = du_get(u, C_MUTE))) w->disabled = !a->mixer_ok;
    text(a, C_AUDIO, a->mixer_ok ? "Master output of the sound card (/dev/mixer)." : "No audio device: /dev/mixer is unavailable.");
    toggle_label(a, C_H24, "24-hour clock", a->cfg.clock_24h);
    toggle_label(a, C_SECONDS, "Show seconds", a->cfg.clock_seconds);
    u->dirty = 1;
}
void cp_tick(struct cp_app *a)
{
    uint64_t t = a->host.now ? a->host.now(a->host.ctx) : 0;
    static const char *const wd[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const mo[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    unsigned h = (unsigned)(t / 3600 % 24), m = (unsigned)(t / 60 % 60), sec = (unsigned)(t % 60);
    if (a->cfg.clock_24h) snprintf(a->clock_line, sizeof(a->clock_line), a->cfg.clock_seconds ? "%02u:%02u:%02u" : "%02u:%02u", h, m, sec);
    else if (a->cfg.clock_seconds) snprintf(a->clock_line, sizeof(a->clock_line), "%u:%02u:%02u %s", h % 12 ? h % 12 : 12, m, sec, h < 12 ? "AM" : "PM");
    else snprintf(a->clock_line, sizeof(a->clock_line), "%u:%02u %s", h % 12 ? h % 12 : 12, m, h < 12 ? "AM" : "PM");
    int64_t z = (int64_t)(t / 86400) + 719468, era = z / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int day = (int)(doy - (153 * mp + 2) / 5 + 1), month = (int)(mp < 10 ? mp + 3 : mp - 9), year = (int)(yoe + era * 400 + (month <= 2));
    snprintf(a->date_line, sizeof(a->date_line), "%s, %d %s %d (UTC)", wd[(t / 86400 + 4) % 7], day, mo[month - 1], year);
    text(a, C_DATE, a->date_line);
}
static void event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct cp_app *a = user;
    (void)u;
    int save = 0;
    switch (e->id) {
    case C_TABS: if (e->type == DU_EV_SELECT && e->index >= 0 && e->index < CP_PAGES) { a->page = e->index; cp_tick(a); } break;
    case C_CLOSE: if (e->type == DU_EV_ACTIVATE) a->quit = 1; break;
    case C_THEME:
        if (e->type == DU_EV_SELECT && e->index >= 0 && e->index < CFG_THEMES) {
            snprintf(a->cfg.theme, sizeof(a->cfg.theme), "%s", cfg_theme_names[e->index]); save = 1; a->preview_valid = 0;
            du_theme_use(a->ui, a->cfg.theme);   /* this window at once; the desktop on DESK_SETTINGS reload, others by poll */
        }
        break;
    case C_WALL:
        if (e->type == DU_EV_SELECT && e->index >= 0) {
            unsigned k = (unsigned)e->index;
            if (k == 0) strcpy(a->cfg.wallpaper, "none");
            else if (k == 1) strcpy(a->cfg.wallpaper, "ciuki");
            else if (k < 2 + DESK_BUILTIN_WALLPAPERS) snprintf(a->cfg.wallpaper, sizeof(a->cfg.wallpaper), "builtin:%s", desk_builtin_wallpapers[k - 2].id);
            else if (k - 2 - DESK_BUILTIN_WALLPAPERS < a->wall_count) {
                snprintf(a->cfg.wallpaper, sizeof(a->cfg.wallpaper), "%s", a->wall_names[k - 2 - DESK_BUILTIN_WALLPAPERS]);
                if (a->cfg.wallpaper_mode == CFG_WALL_CENTER) a->cfg.wallpaper_mode = CFG_WALL_TILE;
            } else break;
            save = 1; a->preview_valid = 0;
        }
        break;
    case C_POS0: case C_POS1: case C_POS2: case C_POS3:
        if (e->type == DU_EV_ACTIVATE) { a->cfg.wallpaper_mode = mode_order[e->id - C_POS0]; save = 1; a->preview_valid = 0; }
        break;
    case C_COL0: case C_COL1: case C_COL2: case C_COL3: case C_COL4: case C_COL5:
        if (e->type == DU_EV_ACTIVATE) { a->cfg.desktop_color = swatches[e->id - C_COL0]; save = 1; a->preview_valid = 0; }
        break;
    case C_ICONS: if (e->type == DU_EV_ACTIVATE) { a->cfg.desktop_icons = !a->cfg.desktop_icons; save = 1; } break;
    case C_SPEED: if (e->type == DU_EV_CHANGE) { a->cfg.mouse_speed = CFG_MOUSE_MIN + e->index; save = 1; } break;
    case C_DBL: if (e->type == DU_EV_CHANGE) { a->cfg.double_click_ms = 200 + e->index * 100; save = 1; } break;
    case C_KEY: if (e->type == DU_EV_SELECT && e->index >= 0 && e->index < CFG_KEYS_COUNT) { a->cfg.keyboard = e->index; save = 1; } break;
    case C_VOL: case C_MUTE:
        if ((e->id == C_VOL && e->type == DU_EV_CHANGE) || (e->id == C_MUTE && e->type == DU_EV_ACTIVATE)) {
            if (e->id == C_VOL) a->volume = e->index; else a->muted = !a->muted;
            int r = a->host.apply ? a->host.apply(a->host.ctx, DESK_SETTINGS_MASTER, a->volume, a->muted) : -1;
            text(a, C_STATUS, r ? "The volume could not be changed." : "Volume applied.");
        }
        break;
    case C_H24: if (e->type == DU_EV_ACTIVATE) { a->cfg.clock_24h = !a->cfg.clock_24h; save = 1; cp_tick(a); } break;
    case C_SECONDS: if (e->type == DU_EV_ACTIVATE) { a->cfg.clock_seconds = !a->cfg.clock_seconds; save = 1; cp_tick(a); } break;
    default: break;
    }
    if (save) changed(a);
    sync(a);
}
static void items(struct du_item *out, char labels[][48], unsigned count, const char *(*name)(struct cp_app *, unsigned), struct cp_app *a)
{
    for (unsigned k = 0; k < count; k++) {
        snprintf(labels[k], 48, "%s", name(a, k));
        memset(&out[k], 0, sizeof(out[k]));
        out[k].text[0] = labels[k];
    }
}
static void list_style(struct du_widget *w, const char *title)
{
    w->view = DU_DETAILS; w->column_count = 1; w->inactive_selection = 1;
    w->columns[0] = (struct du_column){title, w->box.w - 22, 0};
}
static const char *wall_label(struct cp_app *a, unsigned k)
{
    if (k == 0) return "(None)";
    if (k == 1) return "Ciuki portrait";
    if (k < 2 + DESK_BUILTIN_WALLPAPERS) return desk_builtin_wallpapers[k - 2].label;
    const char *path = a->wall_names[k - 2 - DESK_BUILTIN_WALLPAPERS], *base = strrchr(path, '/');
    return base ? base + 1 : path;
}
int cp_attach(struct cp_app *a, struct du_ui *u, int page)
{
    a->ui = u; a->page = page >= 0 && page < CP_PAGES ? page : CP_DISPLAY;
    load(a, &a->cfg);
    du_theme_use(u, a->cfg.theme);
    a->wall_count = a->host.wallpapers ? (unsigned)a->host.wallpapers(a->host.ctx, a->wall_names, CP_WALLPAPERS) : 0;
    if (a->wall_count > CP_WALLPAPERS) a->wall_count = 0;
    a->mixer_ok = a->host.mixer && !a->host.mixer(a->host.ctx, &a->volume, &a->muted);
    u->event = event; u->paint = paint; u->user = a;
    for (int k = 0; k < CP_PAGES; k++) { memset(&a->tab_items[k], 0, sizeof(a->tab_items[k])); a->tab_items[k].text[0] = page_labels[k]; }
    struct du_widget *w = add(a, DU_TABS, C_TABS, 8, 8, CP_WIDTH - 16, CP_HEIGHT - 52, "");
    if (!w) return -1;
    w->items = a->tab_items; w->count = CP_PAGES;
    add(a, DU_BUTTON, C_CLOSE, CP_WIDTH - 88, CP_HEIGHT - 34, 80, 24, "Close");
    add(a, DU_LABEL, C_STATUS, 12, CP_HEIGHT - 30, 380, 16, "");
    /* Display */
    add(a, DU_LABEL, C_THEME_L, 248, 40, 200, 14, "&Theme:");
    for (int k = 0; k < CFG_THEMES; k++) { memset(&a->theme_items[k], 0, sizeof(a->theme_items[k])); a->theme_items[k].text[0] = cfg_theme_names[k]; }
    w = add(a, DU_LISTVIEW, C_THEME, 248, 56, 228, 72, ""); w->items = a->theme_items; w->count = CFG_THEMES; list_style(w, "Theme");
    add(a, DU_LABEL, C_WALL_L, 248, 136, 200, 14, "&Wallpaper:");
    unsigned walls = 2 + DESK_BUILTIN_WALLPAPERS + a->wall_count;
    items(a->wall_items, a->wall_labels, walls, wall_label, a);
    for (unsigned k = 0; k < walls; k++) a->wall_items[k].icon16 = NULL;
    w = add(a, DU_LISTVIEW, C_WALL, 248, 152, 228, 126, ""); w->items = a->wall_items; w->count = walls; list_style(w, "Wallpaper");
    add(a, DU_LABEL, C_POS_L, 24, 224, 100, 14, "Position:");
    static const char *const pos[4] = {"Center", "Tile", "Stretch", "Fit"};
    for (int k = 0; k < 4; k++) add(a, DU_BUTTON, C_POS0 + k, 24 + k * 52, 240, 50, 22, pos[k]);
    add(a, DU_LABEL, C_COL_L, 24, 272, 100, 14, "Desktop colour:");
    for (int k = 0; k < 6; k++) add(a, DU_BUTTON, C_COL0 + k, 24 + k * 34, 288, 30, 24, "");
    add(a, DU_BUTTON, C_ICONS, 248, 290, 228, 24, "");
    /* Mouse */
    add(a, DU_LABEL, C_SPEED_L, 24, 44, 200, 14, "Pointer &speed:");
    w = add(a, DU_SCROLLBAR, C_SPEED, 24, 62, 300, 17, ""); w->vertical = 0; w->total = CFG_MOUSE_MAX - CFG_MOUSE_MIN + 1; w->page = 1;
    add(a, DU_LABEL, C_SPEED_V, 336, 62, 140, 17, "");
    add(a, DU_LABEL, C_SLOW, 24, 82, 60, 14, "Slow");
    add(a, DU_LABEL, C_FAST, 300, 82, 40, 14, "Fast");
    add(a, DU_LABEL, C_DBL_L, 24, 116, 200, 14, "&Double-click speed:");
    w = add(a, DU_SCROLLBAR, C_DBL, 24, 134, 300, 17, ""); w->vertical = 0; w->total = 8; w->page = 1;
    add(a, DU_LABEL, C_DBL_V, 336, 134, 140, 17, "");
    add(a, DU_LABEL, C_MOUSE_NOTE, 24, 172, 452, 14, "The desktop applies both at once; 5 moves one pixel per mouse count.");
    /* Keyboard */
    add(a, DU_LABEL, C_KEY_L, 24, 44, 200, 14, "Keyboard &layout:");
    for (int k = 0; k < CFG_KEYS_COUNT; k++) { memset(&a->key_items[k], 0, sizeof(a->key_items[k])); a->key_items[k].text[0] = cfg_keyboard_labels[k]; }
    w = add(a, DU_LISTVIEW, C_KEY, 24, 60, 220, 56, ""); w->items = a->key_items; w->count = CFG_KEYS_COUNT; list_style(w, "Layout");
    add(a, DU_LABEL, C_KEY_NOTE1, 24, 126, 452, 14, "The Run dialog uses the new layout at once; programs started");
    add(a, DU_LABEL, C_KEY_NOTE2, 24, 140, 452, 14, "afterwards receive it in CIUKI_KEYMAP.");
    /* Sounds */
    add(a, DU_LABEL, C_VOL_L, 24, 44, 200, 14, "Master &volume:");
    w = add(a, DU_SCROLLBAR, C_VOL, 24, 62, 300, 17, ""); w->vertical = 0; w->total = 101; w->page = 1;
    add(a, DU_LABEL, C_VOL_V, 336, 62, 80, 17, "");
    add(a, DU_BUTTON, C_MUTE, 24, 94, 140, 24, "");
    add(a, DU_LABEL, C_AUDIO, 24, 132, 452, 14, "");
    /* Date & Time */
    add(a, DU_LABEL, C_DATE, 24, 106, 452, 14, "");
    add(a, DU_BUTTON, C_H24, 24, 134, 200, 24, "");
    add(a, DU_BUTTON, C_SECONDS, 24, 164, 200, 24, "");
    add(a, DU_LABEL, C_TIME_NOTE1, 24, 202, 452, 14, "The clock reads the computer's real-time clock (UTC).");
    add(a, DU_LABEL, C_TIME_NOTE2, 24, 216, 452, 14, "Setting it is not available yet: the kernel has no call for it.");
    /* System */
    char line[160];
    snprintf(line, sizeof(line), "Version %s", a->host.version[0] ? a->host.version : "unknown"); add(a, DU_LABEL, C_SYS0, 140, 74, 336, 14, line);
    snprintf(line, sizeof(line), "Build %s", a->host.build[0] ? a->host.build : "unknown"); add(a, DU_LABEL, C_SYS1, 140, 90, 336, 14, line);
    snprintf(line, sizeof(line), "Machine %s, display %u x %u", a->host.machine[0] ? a->host.machine : "i686", (unsigned)a->host.screen_w, (unsigned)a->host.screen_h);
    add(a, DU_LABEL, C_SYS2, 140, 106, 336, 14, line);
    snprintf(line, sizeof(line), "Memory %llu MB, %llu MB available, %u processes", (unsigned long long)(a->host.mem_total_k / 1024),
             (unsigned long long)(a->host.mem_free_k / 1024), a->host.processes);
    add(a, DU_LABEL, C_SYS3, 140, 122, 336, 14, line);
    add(a, DU_LABEL, C_SYS4, 24, 160, 452, 14, "Classic computing, one woof at a time. About CiukiOS is in the Start menu, under Help.");
    cp_tick(a);
    sync(a);
    /* Keep the preview and controls at their existing minimum size while
     * widening the lists and anchoring the page/footer to the new surface. */
    du_get(u, C_TABS)->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_GROW_Y;
    du_get(u, C_CLOSE)->anchors = DU_ANCHOR_RIGHT | DU_ANCHOR_BOTTOM;
    du_get(u, C_STATUS)->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_BOTTOM;
    du_get(u, C_THEME)->anchors = du_get(u, C_WALL)->anchors = DU_ANCHOR_GROW_X;
    du_get(u, C_ICONS)->anchors = DU_ANCHOR_GROW_X;
    du_focus(u, C_TABS);
    return 0;
}
void cp_detach(struct cp_app *a) { desk_image_free(&a->image); }
