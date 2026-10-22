/* SPDX-License-Identifier: MIT
 * CiukiOS desktop shell: Start menu, taskbar buttons and tray, desktop
 * icons, built-in windows (About, Run, Shut Down, messages), Alt+Tab and the
 * other window-manager shortcuts. Drawing goes through the libdeskui
 * primitives and the theme painter (theme.h) on the compositor's frame:
 * colours, bevels and the anti-aliased Inter faces follow the current
 * theme (Platinum, Ciuki Blue). No file or kernel access here
 * (shell.h). Integer-only. */
#include "desktop.h"
#include "../libdeskui/deskui.h"
#include "shell_icons.h"
#include "keymap.h"
#include "catalog.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- theme hooks ------------------------------------------------------- */
/* Every shell colour is a theme colour (desk_theme; DU_* are the same
 * fields under the toolkit's names). The tooltip keeps the classic paper. */
#define SH_FACE      DU_FACE
#define SH_INK       DU_INK
#define SH_HOT       DU_SELECT
#define SH_HOT_INK   DU_SELECT_INK
#define SH_WINDOW    DU_WINDOW
#define SH_TIP       0xffffe1u      /* tooltip paper */
static void tooltip_hide(struct desktop *d);

static struct du_canvas canvas(struct desktop *d, struct desk_box clip)
{
    struct du_canvas c = {d->pixels, (int)d->width, (int)d->height, {clip.x, clip.y, clip.w, clip.h}};
    return c;
}
static struct desk_paint painter(const struct du_canvas *c)
{
    struct du_rect v = du_intersection(c->clip, (struct du_rect){0, 0, c->width, c->height});
    return (struct desk_paint){c->pixels, c->width, c->height, v.x, v.y, v.w, v.h};
}
static struct du_rect R(struct desk_box b) { return (struct du_rect){b.x, b.y, b.w, b.h}; }
static void damage(struct desktop *d, struct desk_box b) { desk_damage_add(&d->damage, b, d->width, d->height); }
static void damage_all(struct desktop *d)
{ damage(d, (struct desk_box){0, 0, (int32_t)d->width, (int32_t)d->height}); }
/* Text: y is the top of the capitals (du_text); faces from the theme. */
static void bold(struct du_canvas *c, int x, int y, const char *s, uint32_t colour)
{ du_text_font(c, desk_theme->font_bold, x, y, s, colour); }
static int bold_width(const char *s) { return du_text_width_font(desk_theme->font_bold, s); }
static int text_h(void) { return DU_GLYPH_H; }
static int line_h(void) { return desk_font_line(desk_theme->font_ui) + 1; }
/* Menus and popups cast the theme's soft shadow (du_shadow 4) when it has one. */
#define SH_SHADOW 4
static struct desk_box outer(struct desk_box b)
{ return desk_theme->shadow_size ? (struct desk_box){b.x - SH_SHADOW, b.y - SH_SHADOW, b.w + 2 * SH_SHADOW + 2, b.h + 2 * SH_SHADOW + 2} : b; }
static void popup_face(struct desktop *d, struct du_canvas *c, struct desk_box b)
{
    /* Menus and notification cards share the bars' shallow lit surface. */
    if (desk_theme->control_style == DESK_CONTROLS_PLATINUM) {
        if (desk_theme->shadow_size) du_shadow(c, R(b), SH_SHADOW);
        struct desk_paint p=painter(c);
        desk_paint_bar(&p,b.x,b.y,b.w,b.h,desk_mix(DU_HILITE,DU_FACE,60),d->lfb_bytes_per_pixel==2);
        du_outline(c, R(b), desk_mix(desk_theme->outline, DU_SHADOW, 120));
    } else { du_fill(c, R(b), SH_FACE); du_edge(c, R(b), DU_RAISED); }
}
static void hot_row(struct du_canvas *c, struct du_rect r)
{
    if (desk_theme->control_style == DESK_CONTROLS_PLATINUM) {
        struct desk_paint p = painter(c);
        desk_paint_vgradient(&p, r.x, r.y, r.w, r.h, desk_mix(0xffffffu, DU_SELECT, 50), DU_SELECT);
    } else du_fill(c, r, SH_HOT);
}

/* ---- icons ------------------------------------------------------------- */
/* Tango 0.8.90 names (the icon family of AGENTS.md), looked up in the Tango
 * store through the desktop's icon hook (d->icon: libdeskui du_icon at 16,
 * 22, 32 and 48 pixels); other sizes are area-averaged from the next larger
 * store icon. Without the store (host fixtures) each name maps to the
 * sprites of shell_icons.h. The identity icons add the approved Ciuki
 * portrait (deterministic area scaling of the exact asset). */
struct icon_map { const char *name; const uint32_t *s16, *s22, *s32; };
static const struct icon_map icon_table[] = {
    {"applications-other", desk_icon_folder, NULL, desk_icon_folder32},
    {"applications-development", desk_icon_script, NULL, desk_icon_script32},
    {"applications-games", desk_icon_image, NULL, desk_icon_image32},
    {"document-open", desk_icon_folder_open, NULL, desk_icon_folder32},
    {"preferences-desktop", desk_icon_properties, desk_icon_properties22, NULL},
    {"preferences-system", desk_icon_properties, desk_icon_properties22, NULL},
    {"preferences-desktop-wallpaper", desk_icon_image, NULL, desk_icon_image32},
    {"preferences-desktop-keyboard-shortcuts", desk_icon_generic, NULL, desk_icon_generic32},
    {"input-mouse", desk_icon_properties, desk_icon_properties22, NULL},
    {"input-keyboard", desk_icon_generic, NULL, desk_icon_generic32},
    {"audio-card", desk_icon_audio, NULL, desk_icon_audio32},
    {"audio-volume-high", desk_icon_audio, NULL, desk_icon_audio32},
    {"audio-volume-muted", desk_icon_audio, NULL, desk_icon_audio32},
    {"network-wired", desk_icon_network_wired, NULL, NULL},
    {"network-offline", desk_icon_network_offline, NULL, NULL},
    {"network-error", desk_icon_network_error, NULL, NULL},
    {"office-calendar", desk_icon_generic, NULL, desk_icon_generic32},
    {"help-browser", NULL, NULL, desk_icon_dialog_question32},
    {"dialog-information", NULL, NULL, desk_icon_dialog_info32},
    {"dialog-warning", NULL, NULL, desk_icon_dialog_warning32},
    {"dialog-error", NULL, NULL, desk_icon_dialog_error32},
    {"application-x-executable", desk_icon_run, NULL, desk_icon_run32},
    {"system-shutdown", desk_icon_restart, NULL, NULL},
    {"accessories-text-editor", desk_icon_text, NULL, desk_icon_text32},
    {"text-x-generic", desk_icon_text, NULL, desk_icon_text32},
    {"text-x-script", desk_icon_script, NULL, desk_icon_script32},
    {"system-file-manager", desk_icon_files, NULL, desk_icon_files32},
    {"utilities-system-monitor", desk_icon_views, desk_icon_views22, NULL},
    {"utilities-terminal", desk_icon_run, NULL, desk_icon_run32},
    {"computer", desk_icon_harddisk, NULL, desk_icon_harddisk32},
    {"drive-harddisk", desk_icon_harddisk, NULL, desk_icon_harddisk32},
    {"user-home", desk_icon_folder, NULL, desk_icon_folder32},
    {"folder", desk_icon_folder, NULL, desk_icon_folder32},
    {"user-trash", NULL, desk_icon_delete22, NULL},
    {"view-refresh", desk_icon_refresh, NULL, NULL},
    {"document-properties", desk_icon_properties, desk_icon_properties22, NULL},
    {"process-stop", NULL, desk_icon_delete22, NULL},
    {"go-up", desk_icon_up, desk_icon_up22, NULL},
};
static const struct icon_map *icon_lookup(const char *name)
{
    for (size_t i = 0; i < sizeof(icon_table) / sizeof(icon_table[0]); i++) if (!strcmp(icon_table[i].name, name)) return &icon_table[i];
    return NULL;
}
static const uint32_t *portrait_src;   /* 256x256 approved array, from desk_shell_reset */
static const uint32_t *(*store_icon)(const char *object, unsigned size);   /* d->icon: the Tango store, or NULL */
static void bind(const struct desktop *d) { portrait_src = d->portrait; store_icon = d->icon; }
static int store_size(int size) { return size == 16 || size == 22 || size == 32 || size == 48; }
#define ICON_CACHE 40
static struct { const char *name; int size; uint32_t pixels[48 * 48]; } icon_cache[ICON_CACHE];
static unsigned icon_cache_count;

static void area_scale(const uint32_t *src, int ss, uint32_t *dst, int ds, int ox, int oy, int dw)
{
    /* Premultiplied area average of an ss x ss ARGB sprite into ds x ds at (ox,oy) of a dw-wide buffer. */
    for (int y = 0; y < ds; y++) for (int x = 0; x < ds; x++) {
        int x0 = x * ss / ds, x1 = (x + 1) * ss / ds, y0 = y * ss / ds, y1 = (y + 1) * ss / ds;
        if (x1 <= x0) x1 = x0 + 1;
        if (y1 <= y0) y1 = y0 + 1;
        uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;
        for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) {
            uint32_t p = src[yy * ss + xx], pa = p >> 24;
            a += pa; r += ((p >> 16) & 255) * pa; g += ((p >> 8) & 255) * pa; b += (p & 255) * pa; n++;
        }
        uint32_t out = 0;
        if (a) out = (a + n / 2) / n << 24 | (r + a / 2) / a << 16 | (g + a / 2) / a << 8 | (b + a / 2) / a;
        dst[(oy + y) * dw + ox + x] = out;
    }
}
static void portrait_badge(uint32_t *dst, int size, int badge, int ox, int oy)
{
    /* The approved portrait, area-averaged; its #375564 surround is transparent. */
    if (!portrait_src || badge <= 0) return;
    for (int y = 0; y < badge; y++) for (int x = 0; x < badge; x++) {
        int x0 = x * 256 / badge, x1 = (x + 1) * 256 / badge, y0 = y * 256 / badge, y1 = (y + 1) * 256 / badge;
        uint32_t r = 0, g = 0, b = 0, n = 0, total = 0;
        for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) {
            uint32_t p = portrait_src[yy * 256 + xx] & 0xffffffu;
            total++;
            if (p == 0x375564u) continue;
            r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++;
        }
        if (!n || ox + x >= size || oy + y >= size) continue;
        uint32_t a = n * 255 / total;
        uint32_t src = (r / n) << 16 | (g / n) << 8 | (b / n);
        uint32_t *p = &dst[(oy + y) * size + ox + x], back = *p, ba = back >> 24, out = 0;
        uint32_t oa = a + ba * (255 - a) / 255;
        if (oa) for (unsigned shift = 0; shift < 24; shift += 8) {
            uint32_t s = (src >> shift) & 255, t = (back >> shift) & 255;
            out |= ((s * a + t * ba * (255 - a) / 255) / oa) << shift;
        }
        *p = oa << 24 | out;
    }
}
const uint32_t *desk_shell_icon(const char *name, int size)
{
    if (!name || size <= 0 || size > 48) return NULL;
    for (unsigned i = 0; i < icon_cache_count; i++)
        if (icon_cache[i].size == size && !strcmp(icon_cache[i].name, name)) return icon_cache[i].pixels;
    int ciuki = !strcmp(name, "ciuki") || !strcmp(name, "ciuki-computer");
    const char *base = !strcmp(name, "ciuki-computer") ? "computer" : name;
    if (!ciuki && store_icon && store_size(size)) {
        const uint32_t *p = store_icon(name, (unsigned)size);
        if (p) return p;
    }
    /* The next store size up, area-averaged below (identity badges included). */
    static const int slots[4] = {16, 22, 32, 48};
    const uint32_t *from_store = NULL; int store_from = 0;
    if (store_icon && strcmp(name, "ciuki"))
        for (int k = 0; k < 4 && !from_store; k++)
            if (slots[k] >= size && (from_store = store_icon(base, (unsigned)slots[k]))) store_from = slots[k];
    const struct icon_map *m = icon_lookup(base);
    if (!m && !ciuki && !from_store && store_icon)   /* a name the store lacks: its generic program icon */
        for (int k = 0; k < 4 && !from_store; k++)
            if (slots[k] >= size && (from_store = store_icon("application-x-executable", (unsigned)slots[k]))) store_from = slots[k];
    if (!m && !ciuki) m = icon_lookup("application-x-executable");
    if (m && !ciuki && !from_store) {
        if (size == 16 && m->s16) return m->s16;
        if (size == 22 && m->s22) return m->s22;
        if (size == 32 && m->s32) return m->s32;
    }
    if (icon_cache_count == ICON_CACHE) icon_cache_count = 0;   /* bounded; callers redraw each frame */
    uint32_t *dst = icon_cache[icon_cache_count].pixels;
    memset(dst, 0, sizeof(icon_cache[0].pixels));
    if (from_store) area_scale(from_store, store_from, dst, size, 0, 0, size);
    else if (m) {
        const uint32_t *src = NULL; int ss = 0;
        if (m->s32 && size <= 32) { src = m->s32; ss = 32; }
        else if (m->s22 && size <= 22) { src = m->s22; ss = 22; }
        else if (m->s16 && size <= 16) { src = m->s16; ss = 16; }
        if (src) area_scale(src, ss, dst, size, 0, 0, size);
        else {
            /* Only smaller sprites exist: centre the largest one. */
            src = m->s32 ? m->s32 : m->s22 ? m->s22 : m->s16; ss = m->s32 ? 32 : m->s22 ? 22 : 16;
            if (ss > size) area_scale(src, ss, dst, size, 0, 0, size);
            else for (int y = 0; y < ss; y++) for (int x = 0; x < ss; x++)
                dst[(y + (size - ss) / 2) * size + x + (size - ss) / 2] = src[y * ss + x];
        }
    }
    if (!strcmp(name, "ciuki")) portrait_badge(dst, size, size, 0, 0);
    else if (ciuki) portrait_badge(dst, size, size * 5 / 8, size - size * 5 / 8, size - size * 5 / 8);
    icon_cache[icon_cache_count].name = name; icon_cache[icon_cache_count].size = size;
    return icon_cache[icon_cache_count++].pixels;
}

/* ---- programs ---------------------------------------------------------- */
/* target is a system PE launched through the existing Win32 service; NULL
 * uses the native /Programs inventory. Both share this menu catalogue. */
struct program_info { const char *name, *label, *icon, *category, *target; };
/* Menu order of the known programs; anything else in /Programs is "developer".
 * Icons: the Tango names of icon_map.c program:* (Files and Task Manager
 * show the same icon for the same program; test_icons.py compares them). */
static const struct program_info programs[] = {
    {"ciuknote", "CiukNote", "accessories-text-editor", "programs", NULL},
    {"files", "Files", "system-file-manager", "programs", NULL},
    /* f6-19: the browser application (catalog.h desk_applications record). */
    {"netsurf", "NetSurf", "internet-web-browser", "programs", NULL},
    {"tasks", "Task Manager", "utilities-system-monitor", "programs", NULL},
    {"dosbox", "DOS Box", "utilities-terminal", "programs", NULL},
    {"demo", "Ciuki Demo", "preferences-desktop-peripherals", "programs", NULL},   /* the keyboard/mouse input demo */
    {"lua", "Lua", "text-x-script", "programs", NULL},
    {"control", "Control Panel", "preferences-desktop", "programs", NULL},
    {"music", "Music Player", "audio-x-generic", "programs", NULL},
    {"viewer", "Image Viewer", "image-x-generic", "programs", NULL},
    {"paint", "CiukPaint", "applications-graphics", "programs", NULL},
    {"terminal", "Terminal", "utilities-terminal", "programs", NULL},
    {"cdplayer", "CD Player", "media-optical", "accessories", "C:\\WINDOWS\\CDPLAYER.EXE"},
    {"calc", "Calculator", "accessories-calculator", "accessories", NULL},
    {"gfxdemo", "Graphics Demo", "applications-graphics", "accessories", NULL},
    {"charmap", "Character Map", "accessories-character-map", "accessories", NULL},
    {"clock", "Clock", "appointment-new", "accessories", NULL},
    {"help", "CiukiOS Help", "help-browser", "accessories", NULL},
    {"mixer", "Volume Control", "audio-volume-high", "accessories", NULL},
    {"hwreport", "Hardware Report", "preferences-system", "system-tools", NULL},
    {"regedit", "Registry Editor", "preferences-system", "accessories", NULL},
    {"viewer", "Image Viewer", "image-x-generic", "accessories", NULL},
    {"hello", "Hello", "face-smile", "developer", NULL},
    {"ciukide", "CiukIDE", "applications-development", "developer", NULL},
    {"tcc", "Tiny C Compiler", "text-x-script", "developer", NULL},
    {"libc_smoke", "libc smoke test", "applications-system", "developer", NULL},
};
static const struct program_info *program_find(const char *name)
{
    for (size_t i = 0; i < sizeof(programs) / sizeof(programs[0]); i++) if (!strcmp(programs[i].name, name)) return &programs[i];
    return NULL;
}
const char *desk_program_icon_name(const char *name)
{ const struct program_info *p = program_find(name); return p ? p->icon : "application-x-executable"; }
const char *desk_program_category(const char *name)
{ const struct program_info *p = program_find(name); return p ? p->category : "developer"; }
static int program_index(const struct desktop *d, const char *name)
{
    for (unsigned i = 0; i < d->program_count; i++) if (!strcmp(d->programs[i].name, name)) return (int)i;
    return -1;
}
static const char *client_icon(const struct desktop *d, int i)
{
    const struct desk_client *c = &d->clients[i];
    if (c->internal == DESK_INTERNAL_ABOUT) return "ciuki";
    if (c->internal == DESK_INTERNAL_RUN) return "go-jump";
    if (c->internal == DESK_INTERNAL_TRANSFER || c->internal==DESK_INTERNAL_PROGRAMS) return "folder";
    if (c->internal == DESK_INTERNAL_SHUTDOWN || c->internal == DESK_INTERNAL_POWER) return "system-shutdown";
    if (c->internal) return du_message_icon_name(d->sh.message_kind) ? du_message_icon_name(d->sh.message_kind) : "dialog-information";
    if (c->program == DESK_APP_TERMINAL) return "utilities-terminal";
    if (c->program == DESK_APP_FILES) return "system-file-manager";
    if (c->program == DESK_APP_NOTE) return "accessories-text-editor";
    if (c->program == DESK_APP_CONTROL) {
        if (!strcmp(c->title,"Display Properties")) return cp_applets[0].icon;
        for (unsigned k=0;k<CP_APPLETS;k++) if (!strcmp(c->title,cp_applets[k].label)) return cp_applets[k].icon;
        return "preferences-desktop";
    }
    if (c->taskman) return "utilities-system-monitor";
    /* Applications by their launch-bound identity: the page title replaces the window title. */
    const struct desk_application_entry *app = desk_application_find(c->application);
    if (app) return app->tango;
    for (unsigned k = 0; k < DESK_CATALOG_COUNT; k++)   /* accessories: the launcher's Tango name by window title */
        if (strstr(c->title, desk_catalog[k].label)) return desk_catalog[k].tango;
    for (unsigned k = 0; k < DESK_GAME_COUNT; k++)   /* Programs > Games: the DOS Box window carries the game's title */
        if (!strcmp(c->title, desk_games[k].title)) return desk_games[k].tango;
    if (!strncmp(c->title, "DOS", 3)) return "utilities-terminal";
    if (strstr(c->title, "Demo") || strstr(c->title, "demo")) return desk_program_icon_name("demo");
    return "application-x-executable";
}

/* ---- menus ------------------------------------------------------------- */
static void item(struct sh_menu *m, const char *label, const char *icon, int action, int arg, unsigned flags)
{
    if (m->count >= SH_ITEMS) return;
    struct sh_item *it = &m->items[m->count++];
    snprintf(it->label, sizeof(it->label), "%s", label);
    it->icon = icon; it->action = (uint8_t)action; it->arg = (int16_t)arg; it->flags = (uint8_t)flags;
}
static void separator(struct sh_menu *m) { item(m, "", NULL, SH_ACT_NONE, 0, SH_ITEM_SEPARATOR); }

/* Only projected Desktop entries may receive filesystem operations. */
static int icon_is_entry(const struct desktop *d, int index)
{
    return d->sh.folder && index >= 3 && index < d->sh.icon_count &&
        (unsigned)(index - 3) < d->sh.folder->model.count;
}
static const char *icon_position_name(const struct desktop *d, int index)
{
    if (index < 3) return index == 0 ? "/Computer" : index == 1 ? "/Documents" : "/Trash";
    return icon_is_entry(d, index) ? d->sh.icons[index].label : "/Pictures";
}
static void build(struct desktop *d, struct sh_menu *m, int kind)
{
    memset(m, 0, sizeof(*m));
    m->kind = kind; m->hot = -1;
    m->row_h = kind == SH_MENU_START ? SH_START_ROW : SH_SUB_ROW;
    m->icon = 16;   /* every menu: the Tango 16 px artwork (the Start menu too) */
    switch (kind) {
    case SH_MENU_START:
        item(m, "Programs", "applications-other", SH_ACT_SUBMENU, SH_MENU_PROGRAMS, 0);
        item(m, "Documents", "document-open", SH_ACT_SUBMENU, SH_MENU_DOCUMENTS, 0);
        item(m, "Settings", "preferences-system", SH_ACT_SUBMENU, SH_MENU_SETTINGS, 0);
        item(m, "Help", "help-browser", SH_ACT_SUBMENU, SH_MENU_HELP, 0);
        item(m, "Run...", "go-jump", SH_ACT_RUN, 0, 0);
        separator(m);
        item(m, "Shut Down...", "system-shutdown", SH_ACT_SHUTDOWN, 0, 0);
        break;
    case SH_MENU_PROGRAMS: {
        /* f2-95: opening the menu asks for a fresh scan; the menu shows the
         * current snapshot and the next refresh publishes any change. */
        d->v3_links_force|=DESK_LINKS_SCAN;
        desk_links_refresh(d);
        if (d->v3_link_count) item(m,"Programs folder","folder",SH_ACT_PROGRAMS_FOLDER,0,0);
        unsigned preview=0;
        for (unsigned k=0;k<d->v3_link_count && preview<8;k++) if (!d->v3_links[k].desktop) {
            item(m,d->v3_links[k].launch.label,"application-x-executable",SH_ACT_WIN32_LINK,(int)k,0); preview++;
        }
        int developer = 0, accessories = 0, system_tools = 0;
        for (size_t k = 0; k < sizeof(programs) / sizeof(programs[0]); k++) {
            int at = program_index(d, programs[k].name);
            if (at < 0 && !(programs[k].target && d->v3_launch)) continue;
            if (!strcmp(programs[k].category, "system-tools")) { system_tools = 1; continue; }
            if (!strcmp(programs[k].category, "accessories")) { accessories = 1; continue; }
            if (strcmp(programs[k].category, "programs")) { developer = 1; continue; }
            item(m, programs[k].label, programs[k].icon, SH_ACT_LAUNCH, at, 0);
        }
        for (unsigned k = 0; k < d->program_count; k++) if (!program_find(d->programs[k].name)) developer = 1;
        if (!m->count) item(m, "(No programs)", NULL, SH_ACT_NONE, 0, SH_ITEM_DISABLED);
        if (system_tools) item(m, "System Tools", "applications-system", SH_ACT_SUBMENU, SH_MENU_SYSTEM_TOOLS, 0);
        if (accessories) item(m, "Accessories", "applications-accessories", SH_ACT_SUBMENU, SH_MENU_ACCESSORIES, 0);
        /* The bundled shareware games run in a DOS Box window (catalog.h desk_games). */
        if (program_index(d, "dosbox") >= 0) item(m, "Games", "applications-games", SH_ACT_SUBMENU, SH_MENU_GAMES, 0);
        if (developer) { separator(m); item(m, "Developer", "applications-development", SH_ACT_SUBMENU, SH_MENU_DEVELOPER, 0); }
        break;
    }
    case SH_MENU_SYSTEM_TOOLS:
        for (size_t k = 0; k < sizeof(programs) / sizeof(programs[0]); k++) {
            int at = program_index(d, programs[k].name);
            if (at >= 0 && !strcmp(programs[k].category, "system-tools"))
                item(m, programs[k].label, programs[k].icon, SH_ACT_LAUNCH, at, 0);
        }
        break;
    case SH_MENU_ACCESSORIES:
        for (size_t k = 0; k < sizeof(programs) / sizeof(programs[0]); k++) {
            int at = program_index(d, programs[k].name);
            if (strcmp(programs[k].category, "accessories")) continue;
            if (programs[k].target && d->v3_launch)
                item(m, programs[k].label, programs[k].icon, SH_ACT_WIN32_PROGRAM, (int)k, 0);
            else if (at >= 0)
                item(m, programs[k].label, programs[k].icon, SH_ACT_LAUNCH, at, 0);
        }
        break;
    case SH_MENU_GAMES:
        for (unsigned k = 0; k < DESK_GAME_COUNT; k++) if(!(d->games_removed&(1u<<k))) item(m, desk_games[k].label, desk_games[k].tango, SH_ACT_GAME, (int)k, 0);
        break;
    case SH_MENU_DEVELOPER:
        for (unsigned k = 0; k < d->program_count && m->count < SH_ITEMS; k++) {
            const struct program_info *p = program_find(d->programs[k].name);
            if (p && strcmp(p->category, "developer")) continue;   /* Programs and Accessories list theirs */
            item(m, p ? p->label : d->programs[k].name, p ? p->icon : "application-x-executable", SH_ACT_LAUNCH, (int)k, 0);
        }
        break;
    case SH_MENU_DOCUMENTS:
        for (unsigned k = 0; k < d->sh.cfg.recent_count && k < CFG_RECENT_MAX; k++) {
            const char *path = d->sh.cfg.recent[k], *base = strrchr(path, '/');
            item(m, base && base[1] ? base + 1 : path, "text-x-generic", SH_ACT_DOCUMENT, (int)k, 0);
        }
        if (!d->sh.cfg.recent_count) item(m, "(Empty)", NULL, SH_ACT_NONE, 0, SH_ITEM_DISABLED);
        separator(m);
        item(m, "My Documents", "user-home", SH_ACT_ICON_OPEN, 1, 0);
        item(m, "Clear Recent Documents", NULL, SH_ACT_CLEAR_RECENT, 0, d->sh.cfg.recent_count ? 0 : SH_ITEM_DISABLED);
        break;
    case SH_MENU_SETTINGS: {
        unsigned off = program_index(d, "control") < 0 ? SH_ITEM_DISABLED : 0;
        item(m, "Control Panel", "preferences-desktop", SH_ACT_CONTROL, 0, off);
        separator(m);
        for (int k=0;k<CP_CATEGORIES;k++)
            item(m,cp_categories[k].label,cp_categories[k].icon,SH_ACT_SUBMENU,SH_MENU_SETTINGS_CATEGORY+k,off);
        separator(m);
        item(m, "Task Manager", "utilities-system-monitor", SH_ACT_TASKMAN, 0, program_index(d, "tasks") < 0 ? SH_ITEM_DISABLED : 0);
        break;
    }
    case SH_MENU_HELP:
        item(m, "Keyboard Shortcuts", "preferences-desktop-keyboard-shortcuts", SH_ACT_KEYS, 0, 0);
        separator(m);
        item(m, "About CiukiOS", "ciuki", SH_ACT_ABOUT, 0, 0);
        break;
    case SH_MENU_NEW:
        item(m,"Folder","folder-new",SH_ACT_FILE,DF_FOLDER,0);
        item(m,"Text file","text-x-generic",SH_ACT_FILE,DF_TEXT,0);
        break;
    case SH_MENU_OPEN_WITH:
        if (icon_is_entry(d, d->sh.menu_owner)) {
            const char *path=d->sh.icons[d->sh.menu_owner].argument;
            for (unsigned i=0;i<DESK_CATALOG_COUNT;i++) if (desk_catalog_supports(&desk_catalog[i],path))
                item(m,desk_catalog[i].label,desk_catalog[i].tango,SH_ACT_OPEN_WITH,(int)i,0);
            item(m,"CiukNote","accessories-text-editor",SH_ACT_OPEN_WITH,-1,0);
        }
        break;
    case SH_MENU_DESKTOP:
        item(m,"Arrange icons",NULL,SH_ACT_ARRANGE,0,0);
        item(m,"Line up",NULL,SH_ACT_ARRANGE,1,0);
        item(m,"Refresh","view-refresh",SH_ACT_REFRESH,0,0);
        item(m,"New","document-new",SH_ACT_SUBMENU,SH_MENU_NEW,d->sh.folder ? 0 : SH_ITEM_DISABLED);
        item(m,"Paste","edit-paste",SH_ACT_FILE,DF_PASTE,d->sh.folder ? 0 : SH_ITEM_DISABLED);
        separator(m);
        item(m,"Properties","document-properties",SH_ACT_CONTROL,CP_DISPLAY+1,program_index(d,"control")<0 ? SH_ITEM_DISABLED : 0);
        break;
    case SH_MENU_TRAY_SETTINGS:
        for (unsigned k=0;k<CP_APPLETS;k++) if (cp_applets[k].page==d->sh.menu_owner) {
            const struct cp_applet *a=&cp_applets[k];
            item(m,cp_categories[a->category].label,cp_categories[a->category].icon,SH_ACT_NONE,0,SH_ITEM_DISABLED);
            separator(m);
            item(m,a->label,a->icon,SH_ACT_CONTROL,a->page+1,program_index(d,"control")<0 ? SH_ITEM_DISABLED : 0);
        }
        break;
    case SH_MENU_ICON:
        item(m, "Open", NULL, SH_ACT_ICON_OPEN, d->sh.menu_owner, SH_ITEM_DEFAULT);
        if (icon_is_entry(d, d->sh.menu_owner)) {
            if (d->sh.folder->model.entries[d->sh.menu_owner-3].meta.type != FM_DIR)
                item(m,"Open With",NULL,SH_ACT_SUBMENU,SH_MENU_OPEN_WITH,0);
            item(m,"Cut", "edit-cut", SH_ACT_FILE, DF_CUT,0);
            item(m,"Copy", "edit-copy", SH_ACT_FILE, DF_COPY,0);
            item(m,"Rename    F2", NULL, SH_ACT_RENAME,d->sh.menu_owner,0);
            item(m,"Delete", "edit-delete", SH_ACT_FILE, DF_DELETE,0);
        }
        separator(m);
        item(m, "Properties", "document-properties", SH_ACT_ICON_PROPERTIES, d->sh.menu_owner, 0);
        break;
    case SH_MENU_TASK: {
        int owner = d->sh.menu_owner, minimized = owner >= 0 && owner < DESK_CLIENTS && d->clients[owner].minimized;
        struct desk_client *c=owner>=0 && owner<DESK_CLIENTS ? &d->clients[owner] : NULL;
        int blocked=!c || desk_modal_target(d,owner)!=owner;
        item(m, "Restore", NULL, SH_ACT_TASK_RESTORE, owner, !blocked && c && (minimized || c->zoomed) ? SH_ITEM_DEFAULT : SH_ITEM_DISABLED);
        item(m, "Move", NULL, SH_ACT_TASK_MOVE, owner, blocked || minimized || (c && c->zoomed) ? SH_ITEM_DISABLED : 0);
        item(m, "Size", NULL, SH_ACT_TASK_SIZE, owner, blocked || minimized || !c || !c->resizable || c->zoomed ? SH_ITEM_DISABLED : 0);
        item(m, "Minimize", NULL, SH_ACT_TASK_MINIMIZE, owner, blocked || minimized || (c && c->owner_id) ? SH_ITEM_DISABLED : 0);
        item(m, "Maximize", NULL, SH_ACT_TASK_MAXIMIZE, owner, blocked || !c || !c->resizable || c->zoomed ? SH_ITEM_DISABLED : 0);
        if (owner>=0 && owner<DESK_CLIENTS && d->clients[owner].dosbox)
            item(m,"Full screen    Alt+Enter",NULL,SH_ACT_DOS_FULLSCREEN,owner,0);
        separator(m);
        item(m, "Close    Alt+F4", "process-stop", SH_ACT_TASK_CLOSE, owner, minimized ? 0 : SH_ITEM_DEFAULT);
        break;
    }
    default:
        if (kind>=SH_MENU_SETTINGS_CATEGORY && kind<=SH_MENU_SETTINGS_LAST)
            for (unsigned k=0;k<CP_APPLETS;k++) if (cp_applets[k].category==kind-SH_MENU_SETTINGS_CATEGORY)
                item(m,cp_applets[k].label,cp_applets[k].icon,SH_ACT_CONTROL,cp_applets[k].page+1,program_index(d,"control")<0 ? SH_ITEM_DISABLED : 0);
        break;
    }
}
static int32_t item_y(const struct sh_menu *m, int index)
{
    int32_t y = m->box.y + 3;
    for (int k = 0; k < index && k < m->count; k++) y += m->items[k].flags & SH_ITEM_SEPARATOR ? SH_SEPARATOR : m->row_h;
    return y;
}
static int32_t item_h(const struct sh_menu *m, int index)
{ return m->items[index].flags & SH_ITEM_SEPARATOR ? SH_SEPARATOR : m->row_h; }
static void layout(struct desktop *d, struct sh_menu *m, int32_t x, int32_t y, int above)
{
    int32_t widest = 0;
    for (int k = 0; k < m->count; k++) {
        int32_t w = du_text_width(m->items[k].label);
        if (w > widest) widest = w;
    }
    int32_t banner = m->kind == SH_MENU_START ? SH_BANNER : 0;
    int32_t w = 3 + banner + 6 + m->icon + 10 + widest + 28 + 3, h = 6;
    if (m->kind == SH_MENU_START && w < 184) w = 184;
    if (w < 150) w = 150;
    for (int k = 0; k < m->count; k++) h += item_h(m, k);
    int32_t limit = (int32_t)d->height - (d->shell ? DESK_BAR : 0);
    if (above) y -= h;
    if (x + w > (int32_t)d->width) x = (int32_t)d->width - w;
    if (y + h > limit) y = limit - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    m->box = (struct desk_box){x, y, w, h};
}
static void menus_damage(struct desktop *d)
{
    for (int l = 0; l < d->sh.levels; l++) damage(d, outer(d->sh.menus[l].box));
}
void desk_menu_open(struct desktop *d, int kind, int32_t x, int32_t y)
{
    desk_v3_revoke(d); desk_v3_pointer_release(d);
    tooltip_hide(d);
    if (d->sh.icon_selected>=0) damage(d,d->sh.icons[d->sh.icon_selected].box);
    desk_sound(d,SOUND_MENU_OPEN);
    menus_damage(d);
    d->sh.levels = 1;
    build(d, &d->sh.menus[0], kind);
    if (kind == SH_MENU_START) {
        /* Drops from the Ciuki button at the left end of the top bar. */
        layout(d, &d->sh.menus[0], desk_start_button(d).x, DESK_BAR, 0);
        if (!d->menu_open) { d->menu_focus = d->focus; desk_focus(d, -1); }
        d->menu_open = 1;
        if (d->shell_test && d->shell_stage == 0) d->shell_stage = 1;
        damage(d, desk_start_bounds(d));
    } else layout(d, &d->sh.menus[0], x, y, 0);
    damage(d, outer(d->sh.menus[0].box));
}
void desk_menu_close(struct desktop *d)
{
    if (!d->sh.levels) return;
    if (d->sh.icon_selected>=0) damage(d,d->sh.icons[d->sh.icon_selected].box);
    menus_damage(d);
    int start = d->sh.menus[0].kind == SH_MENU_START;
    d->sh.levels = 0;
    if (start) {
        d->menu_open = 0;
        desk_focus(d, d->menu_focus);
        damage(d, desk_start_bounds(d));
    }
}
static void submenu(struct desktop *d, int level, int index)
{
    /* Opens (or keeps) the submenu of menus[level].items[index]. */
    struct sh_menu *m = &d->sh.menus[level];
    const struct sh_item *it = &m->items[index];
    if (level + 1 < d->sh.levels && d->sh.menus[level + 1].kind == it->arg) return;
    for (int l = level + 1; l < d->sh.levels; l++) damage(d, outer(d->sh.menus[l].box));
    d->sh.levels = level + 1;
    if (level + 1 >= SH_LEVELS) return;
    desk_sound(d,SOUND_MENU_OPEN);
    struct sh_menu *s = &d->sh.menus[level + 1];
    build(d, s, it->arg);
    int32_t y = item_y(m, index) - 3;
    layout(d, s, m->box.x + m->box.w - 3, y, 0);
    if (s->box.x < m->box.x + m->box.w - 3) layout(d, s, m->box.x - s->box.w + 3, y, 0);
    d->sh.levels = level + 2;
    damage(d, outer(s->box));
}
static int menu_hit(const struct sh_menu *m, int32_t x, int32_t y)
{
    if (!desk_contains(m->box, x, y)) return -1;
    for (int k = 0; k < m->count; k++) {
        int32_t top = item_y(m, k);
        if (y >= top && y < top + item_h(m, k))
            return m->items[k].flags & (SH_ITEM_SEPARATOR | SH_ITEM_DISABLED) ? -2 : k;
    }
    return -2;
}
static void menu_highlight(struct desktop *d,struct sh_menu *m,int hot)
{
    if(hot==m->hot)return;
    int rows[]={m->hot,hot};
    for(unsigned i=0;i<2;i++)if(rows[i]>=0 && rows[i]<m->count)
        damage(d,(struct desk_box){m->box.x+2,item_y(m,rows[i]),m->box.w-4,item_h(m,rows[i])});
    m->hot=hot;
}
static void icon_open(struct desktop *d, int index);
static void launch_named(struct desktop *d, const char *name, const char *argument)
{
    int r = desk_task_launch(d, name, argument);
    if (r < 0) {
        char text[160];
        snprintf(text, sizeof(text), r == -ENOENT ? "CiukiOS cannot find '%s' in /Programs." :
                 "'%s' could not be started (error %d).", name, -r);
        desk_shell_message(d, "Programs", text, 1);
    }
}
/* Programs > Games: a DOS Box window titled after the game, typing its command.
 * f3-61: kernel, when given (the vm-game probe only), is the DOS Box's own
 * unified-only runtime assertion; no configuration is written. */
int desk_launch_game(struct desktop *d, int index, const char *kernel)
{
    if (index < 0 || (unsigned)index >= DESK_GAME_COUNT) return -EINVAL;
    if(d->games_removed&(1u<<index)){
        desk_shell_message(d,"Games","This bundled game has been removed. Reinstall CiukiOS to restore it.",1);return -ENOENT;
    }
    if (kernel && strcmp(kernel,"unified")) return -EINVAL;
    char name[48], choice[32];
    snprintf(name, sizeof(name), "--name=%s", desk_games[index].title);
    snprintf(choice, sizeof(choice), "--dos-kernel=%s", kernel ? kernel : "");
    const char *args[] = {name, choice, desk_games[index].command};
    int r = kernel ? desk_task_launch_args(d, "dosbox", args, 3) :
                     desk_task_launch_args(d, "dosbox", (const char *const[]){name, desk_games[index].command}, 2);
    if (r < 0) {
        char text[160];
        snprintf(text, sizeof(text), r == -ENOENT ? "CiukiOS cannot find 'dosbox' in /Programs." :
                 "'%s' could not be started (error %d).", desk_games[index].title, -r);
        desk_shell_message(d, "Games", text, 1);
    }
    return r;
}
static void launch_game(struct desktop *d, int index) { (void)desk_launch_game(d, index, NULL); }
void desk_game_properties(struct desktop *d,unsigned game)
{
    if (!game || game>DESK_GAME_COUNT || d->sh.save_busy) return;
    desk_menu_close(d);
    d->sh.save_game=game; d->sh.save_confirm=0; d->sh.save_focus=2;
    d->sh.save_bytes=0; d->sh.save_quota=64u*1024u*1024u;
    d->sh.save_op=CIUKI_DOSVM_SAVE_QUERY; d->sh.save_busy=1;
    snprintf(d->sh.save_status,sizeof(d->sh.save_status),"Loading game data...");
    int i=desk_internal_open(d,DESK_INTERNAL_GAME_DATA);
    if (i>=0) {
        snprintf(d->clients[i].title,sizeof(d->clients[i].title),"%.32s Properties",desk_games[game-1].title);
        damage_all(d);
    }
}
void desk_game_data_result(struct desktop *d,unsigned game,unsigned op,int error,const struct ciuki_dosvm_command *c)
{
    if (game!=d->sh.save_game) return;
    d->sh.save_busy=0;
    if (error) snprintf(d->sh.save_status,sizeof(d->sh.save_status),"%s",
        error==-EBUSY ? "Close the game before changing or exporting its data." :
        error==-ENOENT ? "No current game data is available to export." : "Game data could not be accessed. Please try again.");
    else if (op==CIUKI_DOSVM_SAVE_QUERY) {
        d->sh.save_bytes=c->changed; d->sh.save_quota=c->reason;
        snprintf(d->sh.save_status,sizeof(d->sh.save_status),"Saved games, high scores and setup choices are kept between sessions.");
    } else if (op==CIUKI_DOSVM_SAVE_RESET) {
        d->sh.save_bytes=0;
        snprintf(d->sh.save_status,sizeof(d->sh.save_status),"Game data has been reset.");
    } else snprintf(d->sh.save_status,sizeof(d->sh.save_status),"Exported %u changed file(s) to %s",c->changed,c->name);
    damage_all(d);
}
static void game_data_action(struct desktop *d,int index,unsigned button)
{
    if (d->sh.save_busy) { if (button==2) desk_request_close(d,index); return; }
    if (d->sh.save_confirm) {
        d->sh.save_confirm=0;
        if (button==0) d->sh.save_op=CIUKI_DOSVM_SAVE_RESET;
        d->sh.save_focus=2;
    } else if (button==0) { d->sh.save_confirm=1; d->sh.save_focus=1; }
    else if (button==1) d->sh.save_op=CIUKI_DOSVM_SAVE_EXPORT;
    else desk_request_close(d,index);
    if (d->sh.save_op) {
        d->sh.save_busy=1;
        snprintf(d->sh.save_status,sizeof(d->sh.save_status),"Working...");
    }
    damage_all(d);
}
static void act(struct desktop *d, const struct sh_item *it)
{
    char argument[24];
    switch (it->action) {
    case SH_ACT_LAUNCH: d->action = it->arg; break;
    case SH_ACT_ABOUT: desk_about(d); break;
    case SH_ACT_EMPTY_RECYCLE: desk_sound(d,SOUND_EMPTY_RECYCLE); break;
    case SH_ACT_RUN: desk_internal_open(d, DESK_INTERNAL_RUN); break;
    case SH_ACT_SHUTDOWN: desk_internal_open(d, DESK_INTERNAL_SHUTDOWN); break;
    case SH_ACT_DOCUMENT:
        if (it->arg >= 0 && (unsigned)it->arg < d->sh.cfg.recent_count) {
            const char *path = d->sh.cfg.recent[it->arg];
            const struct desk_catalog_entry *program = desk_catalog_file(path);
            launch_named(d, program ? program->name : "ciuknote", path);
        }
        break;
    case SH_ACT_CONTROL:
        if (it->arg <= 0) launch_named(d, "control", NULL);
        else for (unsigned k=0;k<CP_APPLETS;k++) if (cp_applets[k].page==it->arg-1) {
            snprintf(argument,sizeof(argument),"--panel=%s",cp_applets[k].name); launch_named(d,"control",argument); break;
        }
        break;
    case SH_ACT_WIN32_PROGRAM: {
        int result=-ENOENT;
        if (it->arg>=0 && (size_t)it->arg<sizeof(programs)/sizeof(programs[0]) &&
            programs[it->arg].target && d->v3_launch) {
            struct desk_launch_record *record=calloc(1,sizeof(*record));
            if (record) {
                snprintf(record->label,sizeof(record->label),"%s",programs[it->arg].label);
                snprintf(record->target,sizeof(record->target),"%s",programs[it->arg].target);
                record->show=1;
                result=d->v3_launch(d,record);
                free(record);
            } else result=-ENOMEM;
        }
        if (result<0) desk_shell_message(d,"Accessories","The selected program could not be started.",1);
        break;
    }
    case SH_ACT_WIN32_LINK:
        if (desk_link_launch(d,(unsigned)it->arg)<0) desk_shell_message(d,"Programs","This shortcut is unavailable or its target could not be started.",1);
        break;
    case SH_ACT_PROGRAMS_FOLDER: desk_internal_open(d,DESK_INTERNAL_PROGRAMS); break;
    case SH_ACT_TASKMAN: launch_named(d, "tasks", NULL); break;
    case SH_ACT_GAME: launch_game(d, it->arg); break;
    case SH_ACT_KEYS:
        desk_shell_message(d, "Keyboard Shortcuts",
            "Win or Ctrl+Esc   Open the Start menu\nAlt+Tab           Switch between windows\n"
            "Alt+F4            Close the active window\nEnter             Open the selected icon\n"
            "Arrows            Move in menus and icons\nEsc               Close a menu", 0);
        break;
    case SH_ACT_ARRANGE: {
        if (!it->arg) cfg_icons_arrange(&d->sh.cfg);
        else {
            uint8_t occupied[512]={0};
            int cw=d->sh.icon_count ? d->sh.icons[0].box.w : 96;
            int ch=d->sh.icon_count ? d->sh.icons[0].box.h : 94;
            int columns=((int)d->width-8)/cw,rows=((int)d->height-2*DESK_BAR-14)/ch;
            if (columns<1) columns=1;
            if (rows<1) rows=1;
            int cells=columns*rows;
            if (cells>4096) cells=4096;
            for (int k=0;k<d->sh.icon_count && k<cells;k++) {
                struct desk_box b=d->sh.icons[k].box;
                int col=(b.x-8+cw/2)/cw,row=(b.y-DESK_BAR-10+ch/2)/ch;
                if (col<0) col=0;
                if (row<0) row=0;
                if (col>=columns) col=columns-1;
                if (row>=rows) row=rows-1;
                int slot=(col*rows+row)%cells;
                while (occupied[slot/8] & (1u<<(slot%8))) slot=(slot+1)%cells;
                occupied[slot/8]|=1u<<(slot%8);
                cfg_icon_move(&d->sh.cfg,icon_position_name(d,k),8+(slot/rows)*cw,DESK_BAR+10+(slot%rows)*ch);
            }
        }
        d->sh.want_save=1;
        d->sh.icon_selected = -1;
        desk_shell_apply(d);
        break;
    }
    case SH_ACT_REFRESH:
        d->sh.want_reload=1; d->sh.want_wallpaper=1;
        if (d->sh.folder) d->sh.folder->command=DF_REFRESH;
        break;
    case SH_ACT_RENAME: desk_folder_rename(d,it->arg); break;
    case SH_ACT_FILE:
        if (d->sh.folder) { d->sh.folder->command=it->arg; d->sh.folder->index=d->sh.menu_owner-3; }
        break;
    case SH_ACT_OPEN_WITH:
        if (icon_is_entry(d, d->sh.menu_owner))
            launch_named(d,it->arg<0 ? "ciuknote" : desk_catalog[it->arg].name,d->sh.icons[d->sh.menu_owner].argument);
        break;
    case SH_ACT_ICON_OPEN: icon_open(d, it->arg); break;
    case SH_ACT_ICON_PROPERTIES:
        if (icon_is_entry(d, it->arg)) { d->sh.folder->command=DF_PROPERTIES; d->sh.folder->index=it->arg-3; break; }
        if (it->arg >= 0 && it->arg < d->sh.icon_count) {
            const struct sh_icon *ic = &d->sh.icons[it->arg];
            char text[200];
            if (ic->action == SH_ACT_LAUNCH)
                snprintf(text, sizeof(text), "%.60s\n\nType: Shortcut\nTarget: /Programs/%.24s%s%.60s", ic->label, ic->program,
                         ic->argument[0] ? " " : "", ic->argument);
            else snprintf(text, sizeof(text), "%.60s\n\nType: System folder\nDeleted desktop items are kept in C:\\Documents\\Trash.", ic->label);
            desk_shell_message(d, "Properties", text, 0);
        }
        break;
    case SH_ACT_TASK_RESTORE:
        if (it->arg>=0 && it->arg<DESK_CLIENTS) {
            if (d->clients[it->arg].minimized) desk_minimize(d,it->arg);
            else if (d->clients[it->arg].zoomed) desk_zoom(d,it->arg);
            desk_focus(d,it->arg);
        }
        break;
    case SH_ACT_TASK_MOVE: desk_geometry_begin(d,it->arg,0); break;
    case SH_ACT_TASK_SIZE: desk_geometry_begin(d,it->arg,1); break;
    case SH_ACT_TASK_MAXIMIZE:
        if (it->arg>=0 && it->arg<DESK_CLIENTS) {
            if (d->clients[it->arg].minimized) desk_minimize(d,it->arg);
            desk_focus(d,it->arg); desk_zoom(d,it->arg);
        }
        break;
    case SH_ACT_TASK_MINIMIZE: if (it->arg >= 0 && it->arg < DESK_CLIENTS && !d->clients[it->arg].minimized) desk_minimize(d, it->arg); break;
    case SH_ACT_TASK_CLOSE: desk_request_close(d, it->arg); break;
    case SH_ACT_DOS_FULLSCREEN:
        if (it->arg>=0 && it->arg<DESK_CLIENTS && d->clients[it->arg].dosbox) {
            if (d->clients[it->arg].minimized) desk_minimize(d,it->arg);
            desk_focus(d,it->arg);
            struct ciuki_message message;
            desk_message(&message,DESK_DOS_FULLSCREEN,4); message.data[0]=DESK_SHELL_VERSION;
            desk_queue(d,it->arg,&message);
        }
        break;
    case SH_ACT_CLEAR_RECENT: d->sh.cfg.recent_count = 0; d->sh.want_save = 1; break;
    default: break;
    }
}
static void activate(struct desktop *d, int level, int index)
{
    struct sh_item it = d->sh.menus[level].items[index];
    if (it.flags & SH_ITEM_DISABLED) return;
    desk_sound(d,SOUND_MENU_SELECT);
    if (it.action == SH_ACT_SUBMENU) { submenu(d, level, index); return; }
    desk_menu_close(d);
    act(d, &it);
}
static void draw_banner(struct du_canvas *c, struct desk_box b)
{
    /* Ciuki banner down the Start menu: the theme's selection colour
     * darkening downwards, the approved portrait at the foot and "CiukiOS"
     * reading upwards in the title face ("Ciuki" in the desktop label
     * colour, "OS" in white), rotated from an alpha mask. */
    const struct desk_theme *t = desk_theme;
    struct desk_paint p = painter(c);
    desk_paint_vgradient(&p, b.x, b.y, b.w, b.h, t->select, desk_mix(t->select, 0x000000u, 150));
    int32_t px = b.x + (b.w - 20) / 2, py = b.y + b.h - 24;
    du_icon_blit(c, px, py, desk_shell_icon("ciuki", 20), 20, DU_ICON_NORMAL);
    enum { MW = 120, MH = 24 };
    static uint32_t mask[MW * MH];
    memset(mask, 0, sizeof(mask));
    int font = t->font_title, line = desk_font(font)->ascent + desk_font(font)->descent;
    if (line > MH) line = MH;
    struct desk_paint m = {mask, MW, MH, 0, 0, MW, MH};
    int w = desk_paint_text(&m, font, 0, desk_font(font)->ascent, "Ciuki", 0x0000ffu);
    w += desk_paint_text(&m, font, w, desk_font(font)->ascent, "OS", 0x00ff00u);
    if (w > MW) w = MW;
    /* Mask column u runs up the banner from just above the portrait; mask
     * row v (glyph top first) runs left to right, centred on the banner. */
    int32_t x0 = b.x + (b.w - line) / 2 + 1, y0 = py - 6;
    struct du_rect vbox = du_intersection(c->clip, (struct du_rect){x0, y0-w+1, line, w});
    vbox = du_intersection(vbox, (struct du_rect){0, b.y, c->width, c->height-b.y});
    for (int y = vbox.y; y < vbox.y + vbox.h; y++) {
        int u = y0 - y;
        uint32_t *dst = p.pixels + (size_t)y * (size_t)p.width + (size_t)vbox.x;
        for (int x = 0; x < vbox.w; x++) {
            int v = vbox.x + x - x0;
            uint32_t k = mask[v * MW + u];
            if (!k) continue;
            if (k & 255u) dst[x] = desk_mix(t->desktop_label, dst[x], k & 255u);
            if ((k >> 8) & 255u) dst[x] = desk_mix(0xffffffu, dst[x], (k >> 8) & 255u);
        }
    }
}
static void draw_menu(struct desktop *d, const struct sh_menu *m, struct desk_box clip)
{
    struct desk_box b;
    if (!desk_intersect(outer(m->box), clip, &b)) return;
    struct du_canvas c = canvas(d, b);
    popup_face(d, &c, m->box);
    int32_t banner = m->kind == SH_MENU_START ? SH_BANNER : 0;
    if (banner) draw_banner(&c, (struct desk_box){m->box.x + 2, m->box.y + 2, banner + 1, m->box.h - 4});
    int platinum = desk_theme->control_style == DESK_CONTROLS_PLATINUM;
    int32_t x0 = m->box.x + 3 + banner, w = m->box.w - 6 - banner;
    for (int k = 0; k < m->count; k++) {
        const struct sh_item *it = &m->items[k];
        int32_t y = item_y(m, k), h = item_h(m, k);
        if (it->flags & SH_ITEM_SEPARATOR) {
            du_hline(&c, x0 + 2, y + 3, w - 4, DU_SHADOW);
            du_hline(&c, x0 + 2, y + 4, w - 4, DU_HILITE);
            continue;
        }
        int hot = k == m->hot && !(it->flags & SH_ITEM_DISABLED);
        if (hot) hot_row(&c, (struct du_rect){platinum ? x0 - 1 : x0, y, platinum ? w + 2 : w, h});
        const uint32_t *ic = desk_shell_icon(it->icon, m->icon);
        if (ic) du_icon_blit(&c, x0 + 6, y + (h - m->icon) / 2, ic, m->icon, it->flags & SH_ITEM_DISABLED ? DU_ICON_DISABLED : DU_ICON_NORMAL);
        int32_t tx = x0 + 6 + m->icon + 10, ty = y + (h - text_h()) / 2;
        if (it->flags & SH_ITEM_DISABLED) {
            if (platinum) du_text(&c, tx, ty, it->label, DU_GRAY); else du_text_disabled(&c, tx, ty, it->label);
        } else if (it->flags & SH_ITEM_DEFAULT) bold(&c, tx, ty, it->label, hot ? SH_HOT_INK : SH_INK);
        else du_text(&c, tx, ty, it->label, hot ? SH_HOT_INK : SH_INK);
        if (it->action == SH_ACT_SUBMENU) du_triangle(&c, x0 + w - 10, y + h / 2, 3, 4, hot ? SH_HOT_INK : SH_INK);
    }
}

/* ---- clock and calendar ------------------------------------------------ */
static void civil(uint64_t seconds, int *year, int *month, int *day, int *weekday)
{
    /* Days since 1970-01-01 to the proleptic Gregorian date (H. Hinnant's algorithm). */
    int64_t z = (int64_t)(seconds / 86400u) + 719468;
    *weekday = (int)((seconds / 86400u + 4) % 7);   /* 1970-01-01 was a Thursday */
    int64_t era = z / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)(mp < 10 ? mp + 3 : mp - 9);
    *year = (int)(yoe + era * 400 + (*month <= 2));
}
static const char *const weekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const months[] = {"January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December"};
static uint64_t wall_seconds(const struct desktop *d) { return d->app_test ? 12 * 3600 : d->realtime_second; }
static void clock_text(const struct desktop *d, char *out, size_t size)
{ desk_shell_clock_text_at(d, wall_seconds(d), out, size); }
static void date_text(const struct desktop *d, char *out, size_t size, int long_form)
{
    int y, mo, day, wd;
    civil(wall_seconds(d), &y, &mo, &day, &wd);
    if (long_form) snprintf(out, size, "%s, %d %s %d", weekdays[wd], day, months[mo - 1], y);
    else snprintf(out, size, "%.3s %d %.3s %d", weekdays[wd], day, months[mo - 1], y);
}

/* ---- taskbar ----------------------------------------------------------- */
static struct desk_box tray_box(const struct desktop *d)
{ return desk_shell_tray_box(d); }
static struct desk_box clock_box(const struct desktop *d)
{ return desk_shell_clock_box(d); }
static struct desk_box volume_icon_box(const struct desktop *d)
{ return desk_shell_tray_icon_box(d, SH_TRAY_VOLUME); }
static struct desk_box network_icon_box(const struct desktop *d)
{ return desk_shell_tray_icon_box(d, SH_TRAY_NETWORK); }
static struct desk_box media_icon_box(const struct desktop *d)
{ return desk_shell_tray_icon_box(d, SH_TRAY_MEDIA); }
static struct desk_box notification_icon_box(const struct desktop *d)
{ return desk_shell_tray_icon_box(d, SH_TRAY_NOTIFICATION); }
static struct desk_box tooltip_box(const struct desktop *d);
static struct desk_box notification_outer(const struct desktop *d);
static struct desk_box tray_popup(const struct desktop *d, struct desk_box anchor, int w, int h)
{
    if (w > (int)d->width) w = (int)d->width;
    int x = anchor.x + anchor.w - w;
    if (x < 0) x = 0;
    if (x + w > (int)d->width) x = (int)d->width - w;
    int y = anchor.y < DESK_BAR ? DESK_BAR + SH_TRAY_GAP : anchor.y - SH_TRAY_GAP - h;
    return (struct desk_box){x, y, w, h};
}
static unsigned media_count(const struct desktop *d)
{ unsigned n=0; for (unsigned i=3;i<26;i++) if (d->sh.media.removable_mask & (1u<<i)) n++; return n; }
static unsigned media_rows(const struct desktop *d)
{ unsigned n=media_count(d), max=((unsigned)d->height-2*DESK_BAR-32)/22; return n<max ? n : max; }
static struct desk_box media_popup(const struct desktop *d)
{ return tray_popup(d, media_icon_box(d), 248, 28 + (int)media_rows(d) * 22); }
void desk_shell_media_update(struct desktop *d,const struct ciuki_media_state *state)
{
    if (!memcmp(&d->sh.media,state,sizeof(*state))) return;
    tooltip_hide(d);
    int resized = !!d->sh.media.removable_mask != !!state->removable_mask;
    if (d->sh.media_open) damage(d,outer(media_popup(d)));
    if (d->sh.media.version == CIUKI_MEDIA_VERSION) {
        unsigned added = state->removable_mask & ~d->sh.media.removable_mask;
        unsigned removed = d->sh.media.removable_mask & ~state->removable_mask;
        for (unsigned i = 3; i < 26; i++) if ((state->removable_mask & d->sh.media.removable_mask & (1u<<i)) &&
            state->generation[i] != d->sh.media.generation[i]) { added |= 1u<<i; removed |= 1u<<i; }
        if (removed) desk_sound(d,SOUND_DEVICE_DISCONNECT);
        if (added) desk_sound(d,SOUND_DEVICE_CONNECT);
        if (added || removed) {
            char text[320]=""; size_t n=0; int unsafe=0;
            for (unsigned i=3;i<26;i++) if ((added|removed)&(1u<<i)) {
                int optical=(added&(1u<<i) ? state->type[i] : d->sh.media.type[i])==CIUKI_MEDIA_OPTICAL;
                int safe=state->type[i]==CIUKI_MEDIA_SAFE_REMOVED && state->generation[i]==d->sh.media.generation[i];
                if (!(added&(1u<<i)) && !optical && !safe) unsafe=1;
                const char *event=added&(1u<<i) ? (optical ? "Disc inserted" : "Media connected") :
                    optical ? "Disc ejected" : safe ? "Safe to unplug the USB device" : "Removed without safe removal";
                int wrote=snprintf(text+n,sizeof(text)-n,"%s%c: %s",n ? "\n" : "",'A'+i,event);
                if (wrote<0 || (size_t)wrote>=sizeof(text)-n) break;
                n+=(size_t)wrote;
            }
            desk_shell_notify(d,"Removable media",text,unsafe ? DU_DLG_ICON_WARNING : DU_DLG_ICON_INFO,SH_NOTIFY_MEDIA);
        }
    }
    for (unsigned i=3;i<26;i++) if (!(state->removable_mask&(1u<<i)) ||
        state->generation[i]!=d->sh.media.generation[i]) d->sh.media_labels[i][0]=0;
    d->sh.media=*state; d->sh.media_scroll=0;
    if (!state->removable_mask) d->sh.media_open=0;
    damage(d, resized ? (struct desk_box){0,0,(int)d->width,DESK_BAR} : tray_box(d));
    if (d->sh.media_open) damage(d,outer(media_popup(d)));
}
void desk_shell_media_labels(struct desktop *d,const struct ciuki_media_volume volumes[26])
{
    struct desk_box previous=d->sh.tooltip==4 ? tooltip_box(d) : (struct desk_box){0};
    int changed=0;
    for (unsigned i=3;i<26;i++) {
        char label[33]; snprintf(label,sizeof(label),"%.32s",volumes[i].label);
        if (strcmp(label,d->sh.media_labels[i])) { strcpy(d->sh.media_labels[i],label); changed=1; }
    }
    if (changed && d->sh.tooltip==4) { damage(d,previous); damage(d,tooltip_box(d)); }
}
static unsigned media_at(const struct desktop *d,unsigned row)
{
    unsigned at=0;
    for (unsigned i=3;i<26;i++) if (d->sh.media.removable_mask & (1u<<i)) {
        if (at++==row+d->sh.media_scroll) return i;
    }
    return 26;
}
static void draw_media(struct desktop *d,struct desk_box clip)
{
    struct desk_box p=media_popup(d), b;
    if (!desk_intersect(outer(p), clip, &b)) return;
    /* Same opaque face as the volume and network popups: an edge alone left
     * the interior transparent (f2-89). */
    struct du_canvas c=canvas(d,b); popup_face(d,&c,p);
    bold(&c,p.x+8,p.y+6,"Removable media",SH_INK);
    for (unsigned row=0;row<media_rows(d);row++) {
        unsigned drive=media_at(d,row); if (drive>=26) break;
        char label[96]; snprintf(label,sizeof(label),"%s %s%s%c:",d->sh.media.type[drive]==CIUKI_MEDIA_OPTICAL ? "Eject disc" : "Safely remove",
            d->sh.media_labels[drive],d->sh.media_labels[drive][0] ? " - " : "",'A'+drive);
        du_text_fit(&c,p.x+10,p.y+28+(int)row*22,p.w-20,label,SH_INK);
    }
}
static struct desk_box network_popup(const struct desktop *d)
{ return tray_popup(d, network_icon_box(d), 272, 112); }
static void network_mac(const struct ciuki_net_state *n, char text[18])
{ snprintf(text, 18, "%02x:%02x:%02x:%02x:%02x:%02x", n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]); }
static void tooltip_text(const struct desktop *d, char *text, size_t size)
{
    if (d->sh.tooltip>=16 && d->sh.tooltip<16+DESK_CLIENTS) {
        snprintf(text,size,"%s",d->clients[d->sh.tooltip-16].title); return;
    }
    if (d->sh.tooltip==3) {
        if (d->sh.mixer_state<0) snprintf(text,size,"No audio device");
        else if (!d->sh.mixer_state) snprintf(text,size,"Checking audio device...");
        else snprintf(text,size,"Volume: %d%%%s",d->sh.volume,d->sh.muted ? " (muted)" : "");
        return;
    }
    if (d->sh.tooltip==4) {
        size_t n=0; text[0]=0;
        for (unsigned i=3;i<26;i++) if (d->sh.media.removable_mask&(1u<<i)) {
            int wrote=snprintf(text+n,size-n,"%s%c: %s",n ? "\n" : "",'A'+i,
                d->sh.media_labels[i][0] ? d->sh.media_labels[i] : "Removable media");
            if (wrote<0 || (size_t)wrote>=size-n) break;
            n+=(size_t)wrote;
        }
        return;
    }
    if (d->sh.tooltip==5) { snprintf(text,size,"Notifications"); return; }
    if (d->sh.tooltip != 2) { date_text(d, text, size, 1); return; }
    const struct ciuki_net_state *n = &d->sh.network;
    if (!n->present) snprintf(text, size, "No network adapter");
    else if (n->state==CIUKI_NET_STOPPED && !strncmp(n->reason,"disabled",9)) snprintf(text,size,"Network: disabled");
    else if (n->link == CIUKI_NET_LINK_UP) {
        if (n->address_configured) {
            snprintf(text,size,"Network: connected (%u.%u.%u.%u)",n->ipv4[0],n->ipv4[1],n->ipv4[2],n->ipv4[3]); return;
        }
        char mac[18]; network_mac(n, mac);
        snprintf(text, size, "Network: cable connected (%s %s)", n->driver, mac);
    } else snprintf(text, size, n->link == CIUKI_NET_LINK_DOWN ? "Network: no cable" : "Network: link unknown");
}
static struct desk_box volume_popup(const struct desktop *d)
{ return tray_popup(d, volume_icon_box(d), 92, 168); }
void desk_shell_volume_damage(struct desktop *d)
{
    damage(d, tray_box(d));
    if (d->sh.volume_open) damage(d, outer(volume_popup(d)));
    if (d->sh.tooltip==3) {
        if (d->sh.tooltip_painted) damage(d,d->sh.tooltip_last);
        damage(d,tooltip_box(d));
    }
}
static struct desk_box volume_track(const struct desktop *d)
{ struct desk_box p = volume_popup(d); return (struct desk_box){p.x + 40, p.y + 30, 12, 100}; }
static struct desk_box volume_mute(const struct desktop *d)
{ struct desk_box p = volume_popup(d); return (struct desk_box){p.x + 10, p.y + p.h - 24, p.w - 20, 14}; }
static struct desk_box tooltip_box(const struct desktop *d)
{
    char text[1024]; tooltip_text(d, text, sizeof(text));
    int32_t w = du_text_width(text) + 16;
    if (w>360) w=360;
    if (w>(int)d->width-8) w=(int)d->width-8;
    int h=du_text_wrap(NULL,(struct du_rect){0,0,w-16,0},text,0)*line_h()+10;
    if (h>(int)d->height-2*DESK_BAR-12) h=(int)d->height-2*DESK_BAR-12;
    struct desk_box anchor=d->sh.tooltip>=16 ? desk_task_button(d,d->sh.tooltip-16) :
        d->sh.tooltip==2 ? network_icon_box(d) : d->sh.tooltip==3 ? volume_icon_box(d) :
        d->sh.tooltip==4 ? media_icon_box(d) : d->sh.tooltip==5 ? notification_icon_box(d) : clock_box(d);
    return tray_popup(d,anchor,w,h);
}
static void damage_moved_popup(struct desktop *d, struct desk_box anchor, int dx, int w, int h)
{
    damage(d, outer(tray_popup(d, anchor, w, h)));
    anchor.x += dx;
    damage(d, outer(tray_popup(d, anchor, w, h)));
}
void desk_shell_tray_damage(struct desktop *d, int previous_width)
{
    struct desk_box t = tray_box(d);
    int dx = t.w - previous_width;
    if (!dx) { damage(d, t); return; }
    int left=t.x+(dx<0 ? dx : 0);
    damage(d, (struct desk_box){left,0,(int)d->width-left,DESK_BAR});
    if (d->sh.volume_open) damage_moved_popup(d, volume_icon_box(d), dx, 92, 168);
    if (d->sh.network_open) damage_moved_popup(d, network_icon_box(d), dx, 272, 112);
    if (d->sh.media_open) damage_moved_popup(d, media_icon_box(d), dx, 248, 28 + (int)media_rows(d) * 22);
    if (d->sh.tooltip) {
        struct desk_box b=tooltip_box(d);
        damage(d,b); b.x+=dx; damage(d,b);
    }
    if (d->sh.notification.active) {
        struct desk_box b=notification_outer(d);
        damage(d,b); b.x+=dx; damage(d,b);
    }
}
void desk_shell_network_update(struct desktop *d, const struct ciuki_net_state *state)
{
    struct ciuki_net_state *old = &d->sh.network;
    int changed = old->present != state->present || old->link != state->link || old->state != state->state ||
                  old->address_configured != state->address_configured || memcmp(old->ipv4,state->ipv4,4) ||
                  memcmp(old->driver, state->driver, sizeof(old->driver)) ||
                  memcmp(old->mac, state->mac, sizeof(old->mac)) ||
                  memcmp(old->reason, state->reason, sizeof(old->reason));
    if (changed && d->sh.tooltip == 2) damage(d, tooltip_box(d));
    int notify=old->version && (old->present!=state->present || old->link!=state->link);
    *old = *state;
    if (!changed) return;
    if (notify) desk_shell_notify(d,"Network",!state->present ? "Network adapter removed." :
        state->link==CIUKI_NET_LINK_UP ? "The network cable is connected." : "The network cable is disconnected.",
        DU_DLG_ICON_INFO,SH_NOTIFY_NETWORK);
    damage(d, tray_box(d));
    if (d->sh.network_open) damage(d, outer(network_popup(d)));
    if (d->sh.tooltip == 2) damage(d, tooltip_box(d));
}
int desk_task_count(const struct desktop *d)
{
    int n = 0;
    for (int i = 0; i < DESK_CLIENTS; i++) n += desk_task_visible(&d->clients[i]);
    return n;
}
static void draw_taskbar(struct desktop *d, struct desk_box clip)
{
    const struct desk_theme *t = desk_theme;
    struct desk_box bar = desk_taskbar(d), b;
    if (!desk_intersect(bar, clip, &b)) return;
    /* Both themes align tasks from the left; the whole bar belongs to windows. */
    for (int i = 0; i < DESK_CLIENTS; i++) {
        struct desk_box button = desk_task_button(d, i), part;
        if (!desk_intersect(button, b, &part)) continue;
        struct du_canvas bc = canvas(d, part);
        struct du_rect r = {button.x + 1, button.y, button.w - 2, button.h};
        int active = d->focus == i && !d->clients[i].minimized;
        int hot=d->sh.hover_target==16+i;
        struct desk_paint p=painter(&bc);
        desk_paint_task(&p,r.x,r.y,r.w,r.h,
            (active ? DESK_TASK_ACTIVE : 0) | (hot ? DESK_TASK_HOT : 0) |
            (d->clients[i].minimized ? DESK_TASK_MINIMIZED : 0),d->lfb_bytes_per_pixel==2);
        if (button.w > 24) {
            const uint32_t *ic = desk_shell_icon(client_icon(d, i), 16);
            int dy = active ? 1 : 0;
            if (ic) du_icon_blit(&bc, button.x + 5 + dy, button.y + 5 + dy, ic, 16, d->clients[i].minimized ? DU_ICON_GHOST : DU_ICON_NORMAL);
            const char *title = d->clients[i].closing ? "Closing..." : d->clients[i].unresponsive ? "Not responding" : d->clients[i].title;
            struct du_canvas tc = bc;
            tc.clip = du_intersection(bc.clip, (struct du_rect){button.x + 24, button.y, button.w - 28, button.h});
            int font = active ? t->font_bold : t->font_ui;
            p=painter(&tc);
            desk_paint_text_ellipsis(&p,font,button.x+25+dy,button.y+17+dy,title,button.w-30-dy,
                                    active ? t->task_active_text : t->taskbar_text);
        }
    }
}
static void bar_text(struct du_canvas *c, int x, int y, const char *text)
{
    const struct desk_theme *t=desk_theme;
    if (t->topbar_text>0x808080u)
        du_text(c,x,y+1,text,desk_mix(0,t->topbar,100));
    du_text(c,x,y,text,t->topbar_text);
}
static void draw_tray(struct desktop *d, struct du_canvas *canvas)
{
    struct du_canvas c = *canvas;
    int net_disabled=d->sh.network.state==CIUKI_NET_STOPPED && !strncmp(d->sh.network.reason,"disabled",9);
    const char *net_icon = !d->sh.network.present ? "network-error" :
                          !net_disabled && d->sh.network.link == CIUKI_NET_LINK_UP ? "network-wired" : "network-offline";
    struct desk_box ni = network_icon_box(d);
    du_icon_blit(&c, ni.x, ni.y, desk_shell_icon(net_icon, 16), 16,
                 d->sh.network.present && !net_disabled ? DU_ICON_NORMAL : DU_ICON_DISABLED);
    const uint32_t *speaker = desk_shell_icon(d->sh.muted ? "audio-volume-muted" : "audio-volume-high", 16);
    struct desk_box vi = volume_icon_box(d);
    du_icon_blit(&c, vi.x, vi.y, speaker, 16, d->sh.mixer_state < 0 ? DU_ICON_DISABLED : DU_ICON_NORMAL);   /* muted: Tango's red cross */
    if (d->sh.media.removable_mask) {
        struct desk_box mi=media_icon_box(d);
        du_icon_blit(&c,mi.x,mi.y,desk_shell_icon("drive-removable-media",16),16,DU_ICON_NORMAL);
    }
    struct desk_box notification = notification_icon_box(d);
    du_icon_blit(&c,notification.x,notification.y,desk_shell_icon("dialog-information",16),16,
                 d->sh.notification.active ? DU_ICON_NORMAL : DU_ICON_GHOST);
    char clock[24];
    clock_text(d, clock, sizeof(clock));
    struct desk_box cb = clock_box(d);
    int width=du_text_width(clock);
    bar_text(&c, cb.x + (cb.w-width)/2, cb.y, clock);
    int year, month, day, weekday; char date[24];
    civil(wall_seconds(d),&year,&month,&day,&weekday);
    snprintf(date,sizeof(date),"%04d-%02d-%02d",year,month,day);
    width=du_text_width(date);
    bar_text(&c,cb.x+(cb.w-width)/2,cb.y+cb.h-text_h(),date);
}
static void check_mark(struct du_canvas *c, int x, int y, uint32_t ink)
{
    static const uint8_t rows[7] = {0x01, 0x03, 0x47, 0x6e, 0x7c, 0x38, 0x10};
    for (int r = 0; r < 7; r++) for (int col = 0; col < 7; col++)
        if (rows[r] & (1u << (6 - col))) du_fill(c, (struct du_rect){x + col, y + r, 1, 1}, ink);
}
static void draw_volume(struct desktop *d, struct desk_box clip)
{
    struct desk_box p = volume_popup(d), b;
    if (!desk_intersect(outer(p), clip, &b)) return;
    struct du_canvas c = canvas(d, b);
    popup_face(d, &c, p);
    bold(&c, p.x + (p.w - bold_width("Volume")) / 2, p.y + 10, "Volume", SH_INK);
    struct desk_box t = volume_track(d);
    if (d->sh.mixer_state < 0) {
        du_text(&c, p.x + 10, p.y + 60, "No audio", SH_INK);
        du_text(&c, p.x + 10, p.y + 60 + line_h(), "device", SH_INK);
        return;
    }
    du_well(&c, (struct du_rect){t.x + 3, t.y - 1, 6, t.h + 2}, SH_WINDOW);
    int32_t fill_h = d->sh.volume * t.h / 100;
    if (fill_h > 0) du_fill(&c, (struct du_rect){t.x + 5, t.y + t.h - fill_h, 2, fill_h}, desk_theme->accent);
    for (int k = 0; k <= 10; k++) du_hline(&c, t.x - 10, t.y + k * t.h / 10, k % 5 ? 4 : 6, DU_SHADOW);
    int32_t ty = t.y + t.h - d->sh.volume * t.h / 100;
    struct du_rect thumb = {t.x - 6, ty - 5, 24, 10};
    if (d->sh.mixer_state == 2) du_raised(&c, thumb, d->sh.volume_drag, 0);
    else { du_fill(&c, thumb, SH_FACE); du_edge(&c, thumb, DU_RAISED_THIN); }
    char value[16]; snprintf(value, sizeof(value), "%d%%", d->sh.volume);
    du_text(&c, t.x + 22, t.y + t.h / 2 - text_h() / 2, value, SH_INK);
    struct desk_box m = volume_mute(d);
    struct du_rect box = {m.x, m.y, 13, 13};
    du_well(&c, box, d->sh.mixer_state == 2 ? SH_WINDOW : SH_FACE);
    if (d->sh.muted) check_mark(&c, box.x + 3, box.y + 3, SH_INK);
    if (d->sh.mixer_state == 2) du_text(&c, m.x + 19, m.y + (13 - text_h()) / 2, "Mute", SH_INK);
    else du_text_disabled(&c, m.x + 19, m.y + (13 - text_h()) / 2, "Mute");
}
static void draw_network(struct desktop *d, struct desk_box clip)
{
    struct desk_box p = network_popup(d), b;
    if (!desk_intersect(outer(p), clip, &b)) return;
    struct du_canvas c = canvas(d, b);
    popup_face(d, &c, p);
    bold(&c, p.x + 10, p.y + 8, "Network", SH_INK);
    const struct ciuki_net_state *n = &d->sh.network;
    if (!n->present) { du_text(&c, p.x + 10, p.y + 32, "No network adapter", SH_INK); return; }
    char text[64], mac[18]; network_mac(n, mac);
    snprintf(text, sizeof(text), "Driver: %s", n->driver);
    du_text(&c, p.x + 10, p.y + 28, text, SH_INK);
    snprintf(text, sizeof(text), "MAC: %s", mac);
    du_text(&c, p.x + 10, p.y + 44, text, SH_INK);
    du_text(&c, p.x + 10, p.y + 60, n->link == CIUKI_NET_LINK_UP ? "Link: cable connected" :
            n->link == CIUKI_NET_LINK_DOWN ? "Link: no cable" : "Link: unknown", SH_INK);
    snprintf(text, sizeof(text), "State: %s", n->state == CIUKI_NET_QUARANTINED ? "quarantined" :
             n->state == CIUKI_NET_RUNNING ? "running" : "stopped");
    du_text(&c, p.x + 10, p.y + 76, text, SH_INK);
    snprintf(text, sizeof(text), "Reason: %s", n->reason);
    du_text(&c, p.x + 10, p.y + 92, text, SH_INK);
}
static void draw_tooltip(struct desktop *d, struct desk_box clip)
{
    struct desk_box t = tooltip_box(d), b;
    if (!desk_intersect(t, clip, &b)) return;
    struct du_canvas c = canvas(d, b);
    char text[1024]; tooltip_text(d, text, sizeof(text));
    du_fill(&c, R(t), SH_TIP);
    du_outline(&c, R(t), SH_INK);
    du_text_wrap(&c,(struct du_rect){t.x+8,t.y+5,t.w-16,t.h-10},text,SH_INK);
}

/* One bounded slot, no allocation or focus changes. Updating a volume event
 * coalesces key repeats without restarting the entry animation. */
static struct desk_box notification_box(const struct desktop *d)
{
    int w=(int)d->width-16; if (w>336) w=336;
    int lines=du_text_wrap(NULL,(struct du_rect){0,0,w-68,0},d->sh.notification.text,0);
    int h=lines*line_h()+64; if (h<102) h=102;
    if (h>(int)d->height-2*DESK_BAR-20) h=(int)d->height-2*DESK_BAR-20;
    struct desk_box b=tray_popup(d,notification_icon_box(d),w,h);
    b.h=h; b.y+=d->sh.notification.slide;
    return b;
}
static struct desk_box notification_outer(const struct desktop *d)
{
    struct desk_box b=notification_box(d); b.y-=10; b.h+=10; return outer(b);
}
static struct du_rect notification_area(const struct desktop *d)
{ return (struct du_rect){0,DESK_BAR,(int)d->width,(int)d->height-2*DESK_BAR}; }
/* Include the whole entry sweep, balloon tail and theme shadow. The painter
 * clips this overlay between the bars; the exclusion must use the same clip. */
struct desk_box desk_shell_notification_bounds(const struct desktop *d)
{
    if (!d->shell || !d->sh.notification.active) return (struct desk_box){0};
    struct desk_box b=notification_outer(d), clipped;
    b.y-=d->sh.notification.slide; b.h+=20;
    struct du_rect area=notification_area(d);
    if (!desk_intersect(b,(struct desk_box){area.x,area.y,area.w,area.h},&clipped)) return (struct desk_box){0};
    return clipped;
}

void desk_shell_notify(struct desktop *d,const char *title,const char *text,int icon,int action)
{
    if (!d->shell) return;
    struct sh_notification *n=&d->sh.notification;
    int attention=icon==DU_DLG_ICON_WARNING || icon==DU_DLG_ICON_ERROR;
    if (n->active && n->balloon && !attention && action!=SH_NOTIFY_MEDIA && d->now_ns<n->expires) return;
    int repeat=n->active && n->action==SH_NOTIFY_VOLUME && action==SH_NOTIFY_VOLUME;
    if (n->active) damage(d,notification_outer(d));
    n->slide=repeat ? n->slide : 20;
    if (!repeat) n->started=d->now_ns;
    n->expires=d->now_ns+8000000000ull;
    n->active=1; n->icon=icon; n->action=action;
    n->balloon=attention;
    snprintf(n->title,sizeof(n->title),"%s",title);
    snprintf(n->text,sizeof(n->text),"%s",text);
    tooltip_hide(d);
    damage(d,notification_outer(d));
    damage(d,notification_icon_box(d));
    if (n->balloon) desk_sound(d,du_message_sound(icon));
}
void desk_shell_notify_error(struct desktop *d,const char *title,int error)
{
    if (error<0) error=-error;
    char text[320]; snprintf(text,sizeof(text),"%s (%d)",strerror(error),error);
    desk_shell_notify(d,title,text,DU_DLG_ICON_ERROR,SH_NOTIFY_DETAILS);
}
static void draw_notification(struct desktop *d,struct desk_box clip)
{
    struct sh_notification *n=&d->sh.notification;
    struct desk_box b=notification_box(d);
    struct du_canvas c=canvas(d,clip);
    c.clip=du_intersection(c.clip,notification_area(d));
    popup_face(d,&c,b);
    struct desk_paint p=painter(&c);
    desk_paint_hgradient(&p,b.x+2,b.y+2,b.w-4,3,desk_theme->title[0],desk_theme->title[1]);
    du_icon_blit(&c,b.x+12,b.y+16,desk_shell_icon(du_message_icon_name(n->icon),32),32,DU_ICON_NORMAL);
    struct du_rect old=c.clip;
    c.clip=du_intersection(old,(struct du_rect){b.x+54,b.y+14,b.w-90,20});
    bold(&c,b.x+54,b.y+16,n->title,DU_INK); c.clip=old;
    for (int i=0;i<7;i++) {
        du_fill(&c,(struct du_rect){b.x+b.w-21+i,b.y+18+i,1,1},DU_INK);
        du_fill(&c,(struct du_rect){b.x+b.w-15-i,b.y+18+i,1,1},DU_INK);
    }
    du_text_wrap(&c,(struct du_rect){b.x+54,b.y+36,b.w-68,b.h-60},n->text,DU_INK);
    du_text(&c,b.x+54,b.y+b.h-20,"Click to open",DU_GRAY);
    if (n->balloon) {
        int x=b.x+b.w-28;
        for (int row=0;row<10;row++) {
            du_hline(&c,x+row,b.y-row,11-row,DU_FACE);
            du_fill(&c,(struct du_rect){x+row,b.y-row,1,1},desk_theme->outline);
            du_fill(&c,(struct du_rect){x+10,b.y-row,1,1},desk_theme->outline);
        }
    }
}

/* ---- desktop icons ----------------------------------------------------- */
/* Large icons: the Tango store's 48 px slot, centred at the top of a 96 x 94
 * cell, the label below in Inter 13 (up to two centred lines, broken by
 * width). Labels are white with a soft dark halo, in the manner of the
 * Windows Vista/7 desktop, so they read on light sky, dark rock and busy
 * foliage alike: the glyph coverage blurred twice by the 5-tap binomial
 * filter 1 4 6 4 1 (radius 4), raised 3x, capped at HALO_PEAK and moved
 * 1 px down, blended in black under the white text. It is computed once per
 * label (and theme text mode) and cached; a repaint only blends it. */
#define ICON_CELL_W 96
#define ICON_CELL_H 94
#define SH_DESKTOP_ICON 48
#define LABEL_TOP 54                                   /* label block (halo included) in the cell */
#define LABEL_H (ICON_CELL_H - LABEL_TOP)
#define LABEL_ROOM (ICON_CELL_W - 12)                  /* widest line; the halo stays in the cell */
#define LABEL_BASE 14                                  /* first baseline below LABEL_TOP */
#define LABEL_PITCH 16
#define HALO_GAIN 48                                   /* sixteenths: 3x the blurred coverage */
#define HALO_PEAK 210                                  /* at most 82 % black */
static void icon_add(struct desktop *d, const char *label, const char *program, const char *argument,
                     const char *icon, int action, int ciuki)
{
    if (d->sh.icon_count >= SH_ICONS) return;
    if (d->sh.icon_count >= d->sh.icon_capacity) {
        int capacity=d->sh.icon_capacity ? d->sh.icon_capacity*2 : 8;
        if (capacity>SH_ICONS) capacity=SH_ICONS;
        struct sh_icon *icons=realloc(d->sh.icons,(size_t)capacity*sizeof(*icons));
        if (!icons) { snprintf(d->notice,sizeof(d->notice),"Not enough memory for desktop icons"); return; }
        d->sh.icons=icons; d->sh.icon_capacity=capacity;
    }
    struct sh_icon *ic = &d->sh.icons[d->sh.icon_count++];
    memset(ic, 0, sizeof(*ic));
    snprintf(ic->label, sizeof(ic->label), "%s", label);
    snprintf(ic->program, sizeof(ic->program), "%s", program);
    snprintf(ic->argument, sizeof(ic->argument), "%s", argument);
    ic->icon = icon; ic->action = action; ic->ciuki = ciuki;
}
static void icons_layout(struct desktop *d)
{
    d->sh.background_valid=0;
    int32_t top = DESK_BAR + 10, bottom = (int32_t)d->height - DESK_BAR - 4;
    int32_t x = 8, y = top;
    for (int k = 0; k < d->sh.icon_count; k++) {
        if (y + ICON_CELL_H > bottom && y > top) { y = top; x += ICON_CELL_W; }
        int px=x,py=y;
        const char *name=icon_position_name(d, k);
        if (cfg_icon_position(&d->sh.cfg,name,&px,&py)) {
            if (px+ICON_CELL_W>(int)d->width) px=(int)d->width-ICON_CELL_W;
            if (px<0) px=0;
            if (py+ICON_CELL_H>bottom) py=bottom-ICON_CELL_H;
            if (py<top) py=top;
        }
        d->sh.icons[k].box = (struct desk_box){px, py, ICON_CELL_W, ICON_CELL_H};
        y += ICON_CELL_H;
    }
}
static void icons_build(struct desktop *d)
{
    d->sh.icon_count = 0; d->sh.icon_selected=-1;
    icon_add(d, "Computer", "files", "Computer", "ciuki-computer", SH_ACT_LAUNCH, 1);
    if (d->sh.icon_count!=1) return;
    icon_add(d, "Documents", "files", "/Documents", "user-home", SH_ACT_LAUNCH, 0);
    if (d->sh.icon_count!=2) return;
    icon_add(d, "Trash", "files", DESK_TRASH, "user-trash", SH_ACT_LAUNCH, 0);
    if (d->sh.icon_count!=3) return;
    if (d->sh.folder) for (unsigned i=0;i<d->sh.folder->model.count;i++) {
        const struct fm_entry *e=&d->sh.folder->model.entries[i];
        char path[FM_PATH]; if (fm_join(path,DESK_FOLDER,e->name)) return;
        const struct desk_catalog_entry *cat=desk_catalog_file(path);
        icon_add(d,e->name,e->meta.type==FM_DIR ? "files" : cat ? cat->name : "ciuknote",path,
            e->meta.type==FM_DIR ? "folder" : cat ? cat->tango : "text-x-generic", SH_ACT_LAUNCH,0);
        if (d->sh.icon_count!=(int)i+4) return;
    }
    /* Keep folder entries at index 3 + model index; Pictures is a shortcut. */
    if (d->pictures_exists) icon_add(d, "Pictures", "files", "/Documents/Pictures", "folder", SH_ACT_LAUNCH, 0);
    d->sh.icon_selected = -1;
}
static void icon_open(struct desktop *d, int index)
{
    if (index < 0 || index >= d->sh.icon_count) return;
    const struct sh_icon *ic = &d->sh.icons[index];
    if (ic->action==SH_ACT_WIN32_LINK) {
        unsigned k=(unsigned)strtoul(ic->argument,NULL,10);
        if (desk_link_launch(d,k)<0) desk_shell_message(d,"Desktop","This shortcut is unavailable or its target could not be started.",1);
    }
    else if (ic->action == SH_ACT_LAUNCH) launch_named(d, ic->program, ic->argument[0] ? ic->argument : NULL);
    else desk_shell_message(d, "Trash", "The Trash is empty.\n\nCiukiOS does not keep deleted files yet:\nFiles deletes them permanently.", 0);
}
static int icon_hit(const struct desktop *d, int32_t x, int32_t y)
{
    if (!d->sh.cfg.desktop_icons) return -1;
    for (int k = 0; k < d->sh.icon_count; k++) {
        struct desk_box b = d->sh.icons[k].box;
        struct desk_box hit = {b.x + 10, b.y + 2, b.w - 20, b.h - 4};
        if (desk_contains(hit, x, y)) return k;
    }
    return -1;
}
static int label_lines(const char *label, char lines[2][32])
{
    /* Up to two lines of at most LABEL_ROOM pixels, broken at the last space
     * that fits (a longer word is cut); a second line that still does not
     * fit ends with an ellipsis. Returns the number of lines. */
    const int f = DESK_FONT_LABEL;
    size_t n = strlen(label), cut = 0, skip = 1;
    lines[0][0] = lines[1][0] = 0;
    if (desk_text_measure(f, label) <= LABEL_ROOM) { snprintf(lines[0], 32, "%.31s", label); return 1; }
    for (size_t i = 1; i < n; i++) if (label[i] == ' ' && desk_text_measure_n(f, label, i) <= LABEL_ROOM) cut = i;
    if (!cut) { cut = desk_text_fit_chars(f, label, LABEL_ROOM); skip = 0; }
    if (!cut) cut = 1;
    snprintf(lines[0], 32, "%.*s", (int)cut, label);
    const char *rest = label + cut + skip;
    if (desk_text_measure(f, rest) <= LABEL_ROOM) snprintf(lines[1], 32, "%s", rest);
    else snprintf(lines[1], 32, "%.*s\xe2\x80\xa6", (int)desk_text_fit_chars(f, rest, LABEL_ROOM - desk_text_measure(f, "\xe2\x80\xa6")), rest);
    return 2;
}
static void binomial(const uint16_t *in, uint16_t *out, int dx, int dy)
{
    /* One pass of the 5-tap binomial filter along (dx, dy); zero outside. */
    static const unsigned k[5] = {1, 4, 6, 4, 1};
    for (int y = 0; y < LABEL_H; y++) for (int x = 0; x < ICON_CELL_W; x++) {
        unsigned sum = 0;
        for (int t = -2; t <= 2; t++) {
            int xx = x + t * dx, yy = y + t * dy;
            if (xx >= 0 && yy >= 0 && xx < ICON_CELL_W && yy < LABEL_H) sum += k[t + 2] * in[yy * ICON_CELL_W + xx];
        }
        out[y * ICON_CELL_W + x] = (uint16_t)((sum + 8) / 16);
    }
}
struct icon_halo { char text[sizeof(((struct sh_icon *)0)->label)]; int aa, valid; uint8_t alpha[ICON_CELL_W * LABEL_H]; };
static struct icon_halo **label_halo;
static unsigned halo_capacity;
static const uint8_t empty_halo[ICON_CELL_W * LABEL_H];
static const uint8_t *label_halo_of(int k, const char *label, char lines[2][32], int count)
{
    /* Halo alpha of desktop icon k's label block (ICON_CELL_W x LABEL_H). */
    int aa = desk_theme->text_aa;
    if ((unsigned)k>=halo_capacity) {
        unsigned next=(unsigned)k+16;
        struct icon_halo **p=realloc(label_halo,next*sizeof(*p));
        if (!p) return empty_halo;
        memset(p+halo_capacity,0,(next-halo_capacity)*sizeof(*p)); label_halo=p; halo_capacity=next;
    }
    if (!label_halo[k]) label_halo[k]=calloc(1,sizeof(**label_halo));
    if (!label_halo[k]) return empty_halo;
    if (label_halo[k]->valid && label_halo[k]->aa == aa && !strcmp(label_halo[k]->text, label)) return label_halo[k]->alpha;
    static uint32_t mask[ICON_CELL_W * LABEL_H];
    static uint16_t a[ICON_CELL_W * LABEL_H], b[ICON_CELL_W * LABEL_H];
    memset(mask, 0, sizeof(mask));
    struct desk_paint m = {mask, ICON_CELL_W, LABEL_H, 0, 0, ICON_CELL_W, LABEL_H};
    for (int l = 0; l < count; l++)
        desk_paint_text(&m, DESK_FONT_LABEL, (ICON_CELL_W - desk_text_measure(DESK_FONT_LABEL, lines[l])) / 2,
                        LABEL_BASE + l * LABEL_PITCH, lines[l], 0x0000ffu);
    for (int i = 0; i < ICON_CELL_W * LABEL_H; i++) a[i] = (uint16_t)(mask[i] & 255u);
    for (int pass = 0; pass < 2; pass++) { binomial(a, b, 1, 0); binomial(b, a, 0, 1); }
    for (int y = 0; y < LABEL_H; y++) for (int x = 0; x < ICON_CELL_W; x++) {
        unsigned v = y ? (unsigned)a[(y - 1) * ICON_CELL_W + x] * HALO_GAIN / 16 : 0;   /* 1 px down */
        label_halo[k]->alpha[y * ICON_CELL_W + x] = (uint8_t)(v > HALO_PEAK ? HALO_PEAK : v);
    }
    snprintf(label_halo[k]->text, sizeof(label_halo[k]->text), "%s", label);
    label_halo[k]->aa = aa; label_halo[k]->valid = 1;
    return label_halo[k]->alpha;
}
static void draw_icons(struct desktop *d, struct desk_box clip)
{
    if (!d->sh.cfg.desktop_icons) return;
    for (int k = 0; k < d->sh.icon_count; k++) {
        const struct sh_icon *ic = &d->sh.icons[k];
        struct desk_box b;
        if (!desk_intersect(ic->box, clip, &b)) continue;
        struct du_canvas c = canvas(d, b);
        struct desk_paint p = painter(&c);
        int selected = k == d->sh.icon_selected;
        int32_t ix = ic->box.x + (ic->box.w - SH_DESKTOP_ICON) / 2, iy = ic->box.y + 4;
        du_icon_blit(&c, ix, iy, desk_shell_icon(ic->icon, SH_DESKTOP_ICON), SH_DESKTOP_ICON, selected ? DU_ICON_SELECTED : DU_ICON_NORMAL);
        char lines[2][32];
        int count = label_lines(ic->label, lines), widest = 0;
        for (int l = 0; l < count; l++) { int w = desk_text_measure(DESK_FONT_LABEL, lines[l]); if (w > widest) widest = w; }
        int32_t top = ic->box.y + LABEL_TOP;
        struct desk_box sel = {ic->box.x + (ICON_CELL_W - widest) / 2 - 5, top + 1, widest + 10, count * LABEL_PITCH + 4};
        if (selected)   /* rounded, translucent: a light tint of the selection colour, the wallpaper shows through */
            desk_paint_round_blend(&p, sel.x, sel.y, sel.w, sel.h, 4, desk_mix(0xffffffu, SH_HOT, 80), 130,
                                   desk_mix(0xffffffu, SH_HOT, 170), 235);
        /* The cached halo, then white text over it. */
        const uint8_t *halo = label_halo_of(k, ic->label, lines, count);
        struct desk_box area;
        if (desk_intersect((struct desk_box){ic->box.x, top, ICON_CELL_W, LABEL_H}, b, &area))
            for (int32_t y = area.y; y < area.y + area.h; y++) {
                const uint8_t *row = halo + (size_t)(y - top) * ICON_CELL_W + (size_t)(area.x - ic->box.x);
                uint32_t *dst = d->pixels + (size_t)y * d->width + (size_t)area.x;
                for (int32_t x = 0; x < area.w; x++) if (row[x]) dst[x] = desk_mix(0x000000u, dst[x], row[x]);
            }
        for (int l = 0; l < count; l++)
            desk_paint_text(&p, DESK_FONT_LABEL, ic->box.x + (ICON_CELL_W - desk_text_measure(DESK_FONT_LABEL, lines[l])) / 2,
                            top + LABEL_BASE + l * LABEL_PITCH, lines[l], 0xffffffu);
        if (d->sh.folder && d->sh.folder->editing==k-3 && k>=3) {
            struct desk_folder *f=d->sh.folder;
            struct du_rect box={ic->box.x+2,top,ICON_CELL_W-4,24};
            du_well(&c,box,f->select_all ? DU_SELECT : DU_WINDOW);
            /* Show the end of long names while typing. */
            const char *shown=f->edit;
            while (*shown && du_text_width(shown)>box.w-8) shown++;
            du_text(&c,box.x+4,box.y+5,shown,f->select_all ? DU_SELECT_INK : DU_INK);
            if (!f->select_all) {
                char prefix[FM_NAME]; unsigned start=(unsigned)(shown-f->edit), count=f->cursor>start ? f->cursor-start : 0;
                memcpy(prefix,shown,count); prefix[count]=0;
                du_vline(&c,box.x+4+du_text_width(prefix),box.y+4,16,DU_INK);
            }
        }
        if (selected && d->focus < 0 && !d->sh.levels) du_dotted(&c, (struct du_rect){sel.x + 2, sel.y + 2, sel.w - 4, sel.h - 4});
    }
}
unsigned desk_shell_cursor(const struct desktop *d)
{
    int i=desk_hit(d,d->mouse_x,d->mouse_y);
    if (i>=0 && d->clients[i].internal==DESK_INTERNAL_RUN) {
        struct desk_box b=desk_content(&d->clients[i]);
        if (desk_contains((struct desk_box){b.x+56,b.y+52,b.w-68,22},d->mouse_x,d->mouse_y)) return DESK_CURSOR_TEXT;
    }
    if (i<0 && d->focus<0 && d->sh.cfg.desktop_icons && d->sh.folder && d->sh.folder->editing>=0) {
        int k=d->sh.folder->editing+3;
        if (k<d->sh.icon_count) {
            struct desk_box b=d->sh.icons[k].box;
            b.x+=2; b.y+=LABEL_TOP; b.w=ICON_CELL_W-4; b.h=24;
            if (desk_contains(b,d->mouse_x,d->mouse_y)) return DESK_CURSOR_TEXT;
        }
    }
    return DESK_CURSOR_ARROW;
}

/* ---- Alt+Tab switcher -------------------------------------------------- */
static struct desk_box switcher_box(const struct desktop *d)
{
    int32_t w = d->sh.switch_count * 44 + 24;
    if (w < 260) w = 260;
    return (struct desk_box){((int32_t)d->width - w) / 2, ((int32_t)d->height - 92) / 2, w, 92};
}
static void switcher_start(struct desktop *d, int reverse)
{
    d->sh.switch_count = 0;
    /* Most recently focused first, minimized windows included. */
    for (int n = 0; n < DESK_CLIENTS; n++) {
        int best = -1;
        for (int i = 0; i < DESK_CLIENTS; i++) {
            if (!desk_task_visible(&d->clients[i])) continue;
            int taken = 0;
            for (int k = 0; k < d->sh.switch_count; k++) taken |= d->sh.switch_list[k] == i;
            if (!taken && (best < 0 || d->clients[i].order > d->clients[best].order)) best = i;
        }
        if (best < 0) break;
        d->sh.switch_list[d->sh.switch_count++] = best;
    }
    if (d->sh.switch_count < 1) return;
    d->sh.switcher = 1;
    int focused_first = d->focus >= 0 && d->sh.switch_list[0] == d->focus;
    d->sh.switch_pick = reverse ? d->sh.switch_count - 1 : (focused_first && d->sh.switch_count > 1 ? 1 : 0);
    damage(d, outer(switcher_box(d)));
}
static void switcher_step(struct desktop *d, int reverse)
{
    if (!d->sh.switch_count) return;
    d->sh.switch_pick = (d->sh.switch_pick + (reverse ? d->sh.switch_count - 1 : 1)) % d->sh.switch_count;
    damage(d, outer(switcher_box(d)));
}
static void switcher_finish(struct desktop *d, int commit)
{
    if (!d->sh.switcher) return;
    damage(d, outer(switcher_box(d)));
    d->sh.switcher = 0;
    if (!commit || d->sh.switch_pick < 0 || d->sh.switch_pick >= d->sh.switch_count) return;
    int i = d->sh.switch_list[d->sh.switch_pick];
    if (i < 0 || i >= DESK_CLIENTS || !d->clients[i].window) return;
    if (d->clients[i].minimized) desk_minimize(d, i); else desk_focus(d, i);
}
static void draw_switcher(struct desktop *d, struct desk_box clip)
{
    struct desk_box s = switcher_box(d), b;
    if (!desk_intersect(outer(s), clip, &b)) return;
    struct du_canvas c = canvas(d, b);
    popup_face(d, &c, s);
    int32_t x = s.x + (s.w - d->sh.switch_count * 44) / 2;
    for (int k = 0; k < d->sh.switch_count; k++) {
        int i = d->sh.switch_list[k];
        struct du_rect cell = {x + k * 44 + 2, s.y + 12, 40, 40};
        if (k == d->sh.switch_pick) {
            du_fill(&c, cell, desk_mix(SH_HOT, SH_FACE, 60));
            du_outline(&c, cell, SH_HOT); du_outline(&c, (struct du_rect){cell.x + 1, cell.y + 1, 38, 38}, SH_HOT);
        }
        du_icon_blit(&c, cell.x + 4, cell.y + 4, desk_shell_icon(client_icon(d, i), 32), 32,
                d->clients[i].minimized ? DU_ICON_GHOST : DU_ICON_NORMAL);
    }
    if (d->sh.switch_pick >= 0 && d->sh.switch_pick < d->sh.switch_count) {
        const char *title = d->clients[d->sh.switch_list[d->sh.switch_pick]].title;
        struct du_rect field = {s.x + 10, s.y + 60, s.w - 20, 20};
        du_well(&c, field, SH_WINDOW);
        int32_t tw = bold_width(title);
        struct du_canvas fc = c; fc.clip = du_intersection(c.clip, (struct du_rect){field.x + 3, field.y, field.w - 6, field.h});
        bold(&fc, tw < field.w - 12 ? field.x + (field.w - tw) / 2 : field.x + 6, field.y + (field.h - text_h()) / 2, title, SH_INK);
    }
}

/* ---- background and bars ----------------------------------------------- */
/* The desktop colour: an unchanged default follows the theme's desktop. */
static uint32_t desktop_colour(const struct desktop *d)
{
    return d->sh.cfg.desktop_color == CFG_DESKTOP_COLOR ? desk_theme->desktop : d->sh.cfg.desktop_color;
}
void desk_shell_background(struct desktop *d, struct desk_box clip)
{
    bind(d);
    if (d->sh.wall && d->sh.wall_valid && d->sh.wall_w == d->width && d->sh.wall_h == d->height) {
        struct desk_shell *s=&d->sh;
        int outline=d->focus<0 && !s->levels;
        if (!s->background) s->background=malloc((size_t)d->width*d->height*4);
        /* Inline rename changes text/caret on every key. Keep it out of the
         * stable wallpaper/icon cache and rebuild once editing finishes. */
        if (s->folder && s->folder->editing>=0) s->background_valid=0;
        if (s->background && (!s->folder || s->folder->editing<0)) {
            uint32_t *out=d->pixels; d->pixels=s->background;
            if (!s->background_valid) {
                memcpy(s->background,s->wall,(size_t)d->width*d->height*4);
                draw_icons(d,(struct desk_box){0,DESK_BAR,(int)d->width,(int)d->height-2*DESK_BAR});
                s->background_rebuilds++; s->background_valid=1; s->background_serial++;
            } else if (s->background_selected!=s->icon_selected || s->background_outline!=outline) {
                int changed[2]={s->background_selected,s->icon_selected};
                for (int k=0;k<2;k++) if (changed[k]>=0 && changed[k]<s->icon_count) {
                    struct desk_box b;
                    if (!desk_intersect(s->icons[changed[k]].box,
                            (struct desk_box){0,0,(int)d->width,(int)d->height},&b)) continue;
                    for (int y=b.y;y<b.y+b.h;y++) memcpy(s->background+(size_t)y*d->width+b.x,
                        s->wall+(size_t)y*d->width+b.x,(size_t)b.w*4);
                    draw_icons(d,b); s->background_serial++;
                }
            }
            d->pixels=out; s->background_selected=s->icon_selected; s->background_outline=outline;
            for (int y=clip.y;y<clip.y+clip.h;y++) memcpy(out+(size_t)y*d->width+clip.x,
                    s->background+(size_t)y*d->width+clip.x,(size_t)clip.w*4);
            return;
        }
        for (int32_t y = clip.y; y < clip.y + clip.h; y++)
            memcpy(d->pixels + (size_t)y * d->width + (uint32_t)clip.x, d->sh.wall + (size_t)y * d->width + (uint32_t)clip.x,
                   (size_t)clip.w * 4u);
    } else {
        /* No cache (allocation failed or not built yet): the desktop colour and the portrait. */
        struct du_canvas c = canvas(d, clip);
        uint32_t colour = desktop_colour(d);
        du_fill(&c, R(clip), colour);
        if (d->portrait && strcmp(d->sh.cfg.wallpaper, "none")) {
            int32_t x0 = ((int32_t)d->width - DESK_PORTRAIT_SIZE) / 2;
            int32_t y0 = DESK_BAR + ((int32_t)d->height - DESK_BAR - DESK_PORTRAIT_SIZE - 36) / 2;
            if (y0 < DESK_BAR) y0 = DESK_BAR;
            struct desk_box b;
            if (desk_intersect((struct desk_box){x0, y0, DESK_PORTRAIT_SIZE, DESK_PORTRAIT_SIZE}, clip, &b)) {
                uint32_t *first_dst = d->pixels + (size_t)b.y * d->width + b.x;
                const uint32_t *first_src = d->portrait + (b.y-y0)*DESK_PORTRAIT_SIZE + b.x-x0;
                int stride = (int)d->width;
                for (int32_t y = 0; y < b.h; y++) {
                    uint32_t *dst = first_dst + (size_t)y * stride;
                    const uint32_t *src = first_src + (size_t)y * DESK_PORTRAIT_SIZE;
                    for (int32_t x = 0; x < b.w; x++) {
                        uint32_t v = src[x];
                        dst[x] = (v & 0xffffffu) == DESK_BACKGROUND ? colour : v;
                    }
                }
            }
        }
    }
    draw_icons(d, clip);
}
/* Sixteen coverage samples, in eighth-pixel integer coordinates. */
static unsigned circle_coverage(int x, int y, int diameter)
{
    if (diameter<=0 || x<0 || y<0 || x>=diameter || y>=diameter) return 0;
    int radius=diameter*4, rr=radius*radius;
    unsigned covered=0;
    for (int sy=1; sy<8; sy+=2) for (int sx=1; sx<8; sx+=2) {
        int dx=x*8+sx-radius, dy=y*8+sy-radius;
        covered+=dx*dx+dy*dy<=rr;
    }
    return covered;
}
/* A concave well in bar colours, clipped by the caller to the bar. */
static void draw_start_well(struct desktop *d, struct du_canvas *c)
{
    struct desk_box b=desk_start_button(d);
    int size=DESK_START_WELL, pad=(size-b.w)/2;
    struct desk_box ignored;
    if (!desk_intersect((struct desk_box){b.x-pad,b.y-pad,size,size},
                        (struct desk_box){c->clip.x,c->clip.y,c->clip.w,c->clip.h},&ignored)) return;
    struct desk_paint p=painter(c);
    const struct desk_theme *t=desk_theme;
    uint32_t dark=desk_mix(t->outline,t->topbar,150);
    uint32_t light=desk_mix(t->hilite,t->topbar,90);
    for (int y=0; y<size; y++) for (int x=0; x<size; x++) {
        unsigned covered=circle_coverage(x,y,size);
        if (!covered) continue;
        unsigned inner=circle_coverage(x-1,y-1,size-2);
        uint32_t face=desk_mix(light,dark,(unsigned)y*255/(size-1));
        face=desk_mix(y<size/2 ? dark : light,face,(covered-inner)*255/covered);
        desk_paint_blend(&p,b.x-pad+x,b.y-pad+y,1,1,face,covered*255/16);
    }
}
/* Shell chrome above windows and overlays. Portrait pixels are blitted last,
 * unchanged by the gloss, using the pinned deterministic area scaler. */
static void draw_start(struct desktop *d, struct du_canvas *c)
{
    const struct desk_theme *t = desk_theme;
    struct desk_box b = desk_start_button(d);
    struct desk_paint p = painter(c);
    int open = d->menu_open, hot = d->sh.start_hot && !open;
    uint32_t base = t->topbar_accent;
    uint32_t light = desk_mix(t->hilite, base, 120);
    uint32_t dark = desk_mix(t->outline, base, 150);
    int radius = b.w * 2, rr = radius * radius;
    /* One pixel of soft shadow only outside the lower, protruding circle. */
    for (int y=DESK_BAR; y<b.y+b.h+DESK_START_SHADOW; y++) for (int x=0; x<b.w; x++) {
        unsigned shadow=circle_coverage(x,y-b.y-DESK_START_SHADOW,b.w);
        unsigned face=circle_coverage(x,y-b.y,b.w);
        if (shadow>face) desk_paint_blend(&p,b.x+x,y,1,1,t->drop_shadow,(shadow-face)*9);
    }
    for (int y = 0; y < b.h; y++) for (int x = 0; x < b.w; x++) {
        unsigned covered = circle_coverage(x,y,b.w);
        if (!covered) continue;
        int dx = x * 4 + 2 - radius, dy = y * 4 + 2 - radius * 2 / 3;
        unsigned distance = (unsigned)(dx * dx + dy * dy) * 255 / (unsigned)rr;
        if (distance > 255) distance = 255;
        uint32_t face = open ? desk_mix(light,dark,distance) : desk_mix(dark,light,distance);
        /* Soft white ellipse over the upper hemisphere, brighter on hover. */
        int hx=x*2+1-b.w, hy=y*2+1-b.h/2;
        int ellipse=hx*hx*64+hy*hy*225, limit=b.w*b.w*36;
        if (y<b.h/2 && ellipse<limit)
            face=desk_mix(0xffffffu,face,(unsigned)(limit-ellipse)*(hot ? 66 : open ? 18 : 40)/limit);
        unsigned inner=circle_coverage(x-1,y-1,b.w-2);
        face=desk_mix(t->outline,face,(covered-inner)*210/covered);
        desk_paint_blend(&p,b.x+x,b.y+y,1,1,face,covered*255/16);
    }
    int shift = open ? DESK_START_PRESSED_SHIFT : 0;
    int x = b.x + DESK_START_MARK_X + shift;
    int y = b.y + DESK_START_MARK_Y + shift;
    const uint32_t *mark=desk_shell_icon("ciuki",SH_START_MARK);
    if (mark) for (int yy=0; yy<SH_START_MARK; yy++) for (int xx=0; xx<SH_START_MARK; xx++) {
        uint32_t src=mark[yy*SH_START_MARK+xx];
        /* Mask in the enclosing orb's coordinates: retain the complete
         * approved dog's silhouette, including ears near the mark corners. */
        unsigned coverage=circle_coverage(x+xx-b.x,y+yy-b.y,b.w);
        desk_paint_blend(&p,x+xx,y+yy,1,1,src,(src>>24)*coverage/16);
    }
}
void desk_shell_topbar(struct desktop *d, struct desk_box clip)
{
    /* Free space separates the orb and system status; captions stay in frames. */
    struct desk_box bar = {0,0,(int32_t)d->width,DESK_BAR}, b;
    if (!desk_intersect(bar,clip,&b)) return;
    bind(d);
    struct du_canvas c = canvas(d,b);
    draw_start_well(d,&c);
    draw_tray(d,&c);
}
void desk_shell_start(struct desktop *d, struct desk_box clip)
{
    struct desk_box b;
    if (!desk_intersect(desk_start_bounds(d),clip,&b)) return;
    bind(d);
    struct du_canvas c=canvas(d,b);
    draw_start(d,&c);
}
void desk_shell_overlay(struct desktop *d, struct desk_box clip)
{
    bind(d);
    draw_taskbar(d, clip);
    if (d->sh.notification.active) draw_notification(d,clip);
    for (int l = 0; l < d->sh.levels; l++) draw_menu(d, &d->sh.menus[l], clip);
    if (d->sh.volume_open) draw_volume(d, clip);
    if (d->sh.network_open) draw_network(d, clip);
    if (d->sh.media_open) draw_media(d,clip);
    if (d->sh.tooltip && !d->sh.notification.active && !d->sh.volume_open && !d->sh.network_open && !d->sh.media_open && !d->sh.levels) draw_tooltip(d, clip);
    if (d->sh.switcher) draw_switcher(d, clip);
}
/* Opaque overlay bodies, shared with foreign-surface clipping. Shadows and
 * the notification's tail remain dependent on the pixels underneath. */
unsigned desk_shell_occluders(const struct desktop *d,struct desk_box *boxes)
{
    unsigned n=0;if(!d->shell)return 0;
    if(d->sh.notification.active)boxes[n++]=notification_box(d);
    for(int l=0;l<d->sh.levels;l++)boxes[n++]=d->sh.menus[l].box;
    if(d->sh.volume_open)boxes[n++]=volume_popup(d);
    if(d->sh.network_open)boxes[n++]=network_popup(d);
    if(d->sh.media_open)boxes[n++]=media_popup(d);
    if(d->sh.tooltip && !d->sh.notification.active && !d->sh.volume_open &&
       !d->sh.network_open && !d->sh.media_open && !d->sh.levels)boxes[n++]=tooltip_box(d);
    if(d->sh.switcher)boxes[n++]=switcher_box(d);
    return n;
}
/* The window icon in a gradient title bar (compositor.c, weak hook). */
const uint32_t *desk_shell_window_icon(struct desktop *d, int index, int size)
{
    if (index < 0 || index >= DESK_CLIENTS) return NULL;
    bind(d);
    return desk_shell_icon(client_icon(d, index), size);
}

/* ---- built-in windows -------------------------------------------------- */
static const char *const about_tabs[] = {"General", "System", "Credits"};
static void internal_size(const struct desktop *d, int kind, uint32_t *w, uint32_t *h)
{
    switch (kind) {
    case DESK_INTERNAL_GAME_DATA: *w=480; *h=230; break;
    case DESK_INTERNAL_TRANSFER: *w = 454; *h = 230; break;
    case DESK_INTERNAL_PROGRAMS: *w=460; *h=420; break;
    case DESK_INTERNAL_ABOUT: *w = 540; *h = 420; break;
    case DESK_INTERNAL_AUTOPLAY: *w = 430; *h = 252; break;
    case DESK_INTERNAL_RUN: *w = 372; *h = 128; break;
    case DESK_INTERNAL_SHUTDOWN: *w = 352; *h = 132; break;
    case DESK_INTERNAL_POWER: {
        unsigned lines=1;
        for (const char *s=d->power.text; *s; s++) lines+=*s=='\n';
        *w=420; *h=lines*(unsigned)line_h()+70;
        if (*h<132) *h=132;
        break;
    }
    default: {
        int width,height;
        du_message_size(d->v3_dialog_kind==1&&d->v3_details_open?d->v3_details:d->sh.message_text,d->sh.message_kind,1,(int)d->width-20,&width,&height);
        if(d->v3_dialog_kind==1){if(width<420)width=420;if(d->v3_details_open){width=610;height=320;}}
        *w=(uint32_t)width; *h=(uint32_t)height;
    }
    }
    if (*w + 20 > d->width) *w = d->width > 40 ? d->width - 20 : 1;
    if (*h + 2 * DESK_BAR + 30 > d->height) *h = d->height > 2 * DESK_BAR + 60 ? d->height - 2 * DESK_BAR - 30 : 1;
}
static int internal_find(const struct desktop *d, int kind)
{
    for (int i = 0; i < DESK_CLIENTS; i++) if (d->clients[i].window && d->clients[i].internal == kind) return i;
    return -1;
}
int desk_internal_open(struct desktop *d, int kind)
{
    desk_v3_revoke(d); desk_v3_pointer_release(d);
    uint32_t owner=kind==DESK_INTERNAL_MESSAGE && d->focus>=0 ?
        (d->clients[d->focus].internal==DESK_INTERNAL_MESSAGE ? d->clients[d->focus].owner_id : d->clients[d->focus].window_id) : 0;
    int i = internal_find(d, kind);
    if (i >= 0 && kind != DESK_INTERNAL_MESSAGE) {
        if (d->clients[i].minimized) desk_minimize(d, i); else desk_focus(d, i);
        return i;
    }
    if (i >= 0) desk_drop_client(d, i);   /* one message at a time: the newest wins */
    for (i = 0; i < DESK_CLIENTS; i++) if (d->clients[i].channel < 0 && !d->clients[i].internal) break;
    if (i == DESK_CLIENTS) return -1;
    struct desk_client *c = &d->clients[i];
    uint32_t w, h;
    internal_size(d, kind, &w, &h);
    c->internal = kind; c->window = 1;
    c->owner_id=owner;
    if (kind != DESK_INTERNAL_MESSAGE && kind != DESK_INTERNAL_POWER)
        desk_sound(d,kind == DESK_INTERNAL_SHUTDOWN ? SOUND_QUESTION : SOUND_WINDOW_OPEN);
    if (!++d->next_window_id) ++d->next_window_id;
    c->window_id = d->next_window_id;
    c->info.width = w; c->info.height = h;
    snprintf(c->title, sizeof(c->title), "%s", kind == DESK_INTERNAL_ABOUT ? "About CiukiOS" : kind == DESK_INTERNAL_RUN ? "Run" :
             kind == DESK_INTERNAL_SHUTDOWN ? "Shut Down CiukiOS" : kind == DESK_INTERNAL_POWER ? "Shutting down..." : kind == DESK_INTERNAL_AUTOPLAY ? "AutoPlay" : kind==DESK_INTERNAL_PROGRAMS ? "Programs" : d->sh.message_title);
    c->frame = desk_frame(w, h, 0, DESK_BAR);
    if (kind == DESK_INTERNAL_ABOUT) { d->about = i; d->sh.about_tab = 0; d->sh.about_scroll = 0; d->sh.want_reload |= 2; }
    if (kind == DESK_INTERNAL_RUN) { d->sh.run_length = 0; d->sh.run_text[0] = 0; }
    if (kind == DESK_INTERNAL_SHUTDOWN) d->sh.shutdown_choice = 0;
    int32_t x = ((int32_t)d->width - c->frame.w) / 2, y = DESK_BAR + 20;
    if (kind == DESK_INTERNAL_RUN) { x = 8; y = DESK_BAR + 8; }   /* under the Ciuki button and its menu */
    else if (kind != DESK_INTERNAL_ABOUT) y = ((int32_t)d->height - c->frame.h) / 2 - 20;
    if (owner) for (int k=0;k<DESK_CLIENTS;k++) if (d->clients[k].window_id==owner) {
        struct desk_box f=d->clients[k].frame;
        x=f.x+(f.w-c->frame.w)/2; y=f.y+(f.h-c->frame.h)/2;
    }
    desk_move(d, i, x, y);
    if (d->sh.levels) desk_menu_close(d);
    desk_focus(d, i);
    damage(d, desk_outer(c));
    damage(d, desk_taskbar(d));
    return i;
}
void desk_internal_closed(struct desktop *d, int index)
{
    if (d->about == index) d->about = -1;
    if (d->clients[index].internal==DESK_INTERNAL_MESSAGE && d->v3_dialog_kind) {
        if(d->v3_dialog_kind==1)(void)desk_v3_dialog_accept(d);
        d->v3_dialog_kind=0; d->v3_dialog_pid=0;
    }
    if (d->clients[index].internal == DESK_INTERNAL_AUTOPLAY) d->sh.autoplay.open=0;
}
void desk_shell_message(struct desktop *d, const char *title, const char *text, int error)
{
    desk_shell_message_class(d,title,text,error ? DU_DLG_ICON_ERROR : DU_DLG_ICON_INFO);
}
void desk_shell_message_class(struct desktop *d, const char *title, const char *text, int error)
{
    snprintf(d->sh.message_title, sizeof(d->sh.message_title), "%s", title);
    snprintf(d->sh.message_text, sizeof(d->sh.message_text), "%s", text);
    d->sh.message_kind = error;
    desk_sound(d,du_message_sound(error));
    if (!d->shell) return;
    desk_internal_open(d, DESK_INTERNAL_MESSAGE);
}
static struct desk_box button_box(struct desk_box content, int index, int count)
{
    /* Right-aligned 75x23 buttons along the bottom edge. */
    int32_t x = content.x + content.w - 10 - (count - index) * 81 + 6;
    return (struct desk_box){x, content.y + content.h - 33, 75, 23};
}
static struct desk_box message_button_box(const struct desktop *d, struct desk_box content, int index)
{
    int count=d->v3_dialog_kind==1||d->v3_dialog_kind==2 ? 2 : 1;
    if (!d->v3_dialog_kind) return button_box(content,index,count);
    return (struct desk_box){content.x+content.w-12-(count-index)*140-(count-index-1)*8,
                            content.y+content.h-33,140,23};
}
static void push_button(struct du_canvas *c, struct desk_box b, const char *label, int def)
{
    /* libdeskui's push button (widgets.c du_draw_button) in the current theme. */
    const struct desk_theme *t = desk_theme;
    struct du_rect face = R(b);
    du_fill(c, face, SH_FACE);
    if (t->control_style == DESK_CONTROLS_PLATINUM) {
        if (def) {
            du_fill(c, (struct du_rect){b.x + 1, b.y, b.w - 2, b.h}, t->outline);
            du_fill(c, (struct du_rect){b.x, b.y + 1, b.w, b.h - 2}, t->outline);
            face = (struct du_rect){b.x + 2, b.y + 2, b.w - 4, b.h - 4};
        }
        du_raised(c, face, 0, 0);
    } else {
        if (def) { du_outline(c, face, DU_DARK); face = (struct du_rect){b.x + 1, b.y + 1, b.w - 2, b.h - 2}; }
        du_edge(c, face, DU_BUTTON_EDGE);
    }
    du_label(c, b.x + (b.w - du_label_width(label)) / 2, b.y + (b.h - DU_GLYPH_H) / 2, label, SH_INK, 1);
}
static void multiline(struct du_canvas *c, int32_t x, int32_t y, const char *text, uint32_t colour)
{
    char line[128]; size_t n = 0;
    for (const char *s = text;; s++) {
        if (!*s || *s == '\n' || n + 1 >= sizeof(line)) {
            line[n] = 0; du_text(c, x, y, line, colour); y += line_h(); n = 0;
            if (!*s) break;
            if (*s != '\n') line[n++] = *s;
        } else line[n++] = *s;
    }
}
static struct desk_box about_tab(struct desk_box content, int k)
{ return (struct desk_box){content.x + 10 + k * 76, content.y + 8, 76, DU_TAB_H}; }
static void tab_strip(struct du_canvas *c, struct desk_box content, int active)
{
    /* The toolkit's tab control (widgets.c draw_tabs) on the fixed About tabs. */
    const struct desk_theme *t = desk_theme;
    int platinum = t->control_style == DESK_CONTROLS_PLATINUM, base = content.y + 8 + DU_TAB_H - 1;
    for (int k = 0; k < 3; k++) {
        struct desk_box tb = about_tab(content, k);
        int sel = k == active;
        struct du_rect r = {tb.x - (sel ? 2 : 0), tb.y + (sel ? 0 : 2), tb.w + (sel ? 4 : 0), DU_TAB_H - (sel ? 0 : 2)};
        if (platinum) {
            struct desk_paint p = painter(c);
            desk_paint_vgradient(&p, r.x + 1, r.y + 1, r.w - 2, r.h - 1, sel ? t->face_top : desk_mix(DU_SHADOW, t->face_top, 50),
                                 sel ? DU_FACE : desk_mix(DU_SHADOW, t->face_bottom, 60));
            uint32_t edge = desk_mix(t->outline, DU_FACE, 150);
            du_hline(c, r.x + 2, r.y, r.w - 4, edge); du_vline(c, r.x, r.y + 2, r.h - 2, edge); du_vline(c, r.x + r.w - 1, r.y + 2, r.h - 2, edge);
            du_fill(c, (struct du_rect){r.x + 1, r.y + 1, 1, 1}, edge); du_fill(c, (struct du_rect){r.x + r.w - 2, r.y + 1, 1, 1}, edge);
            du_hline(c, r.x + 2, r.y + 1, r.w - 4, DU_HILITE); du_vline(c, r.x + 1, r.y + 2, r.h - 2, DU_HILITE);
            if (sel) { du_hline(c, r.x + 1, base, r.w - 2, DU_FACE); du_hline(c, r.x + 3, r.y + 1, r.w - 6, t->accent); }
        } else {
            du_fill(c, r, DU_FACE);
            du_hline(c, r.x + 2, r.y, r.w - 4, DU_HILITE); du_vline(c, r.x, r.y + 2, r.h - 2, DU_HILITE);
            du_fill(c, (struct du_rect){r.x + 1, r.y + 1, 1, 1}, DU_HILITE);
            du_vline(c, r.x + r.w - 1, r.y + 2, r.h - 2, DU_DARK); du_vline(c, r.x + r.w - 2, r.y + 1, r.h - 1, DU_SHADOW);
            if (sel) du_hline(c, r.x + 1, base, r.w - 3, DU_FACE);
        }
        int font = sel ? t->font_bold : t->font_ui, tw = du_text_width_font(font, about_tabs[k]);
        du_text_font(c, font, r.x + (r.w - tw) / 2, r.y + (DU_TAB_H - 2 - DU_GLYPH_H) / 2 + (sel ? 1 : 0), about_tabs[k], SH_INK);
    }
}
/* ---- About: scrollable credits (f2-97) --------------------------------- */
struct credit { unsigned char group; const char *name, *version, *licence; };
#define CREDIT(group, name, version, licence) {group, name, version, licence},
static const struct credit credits_list[] = {
#include "credits.inc"
};
#undef CREDIT
static const char *const credit_groups[] = {CREDIT_GROUPS};
#define CREDIT_COUNT (sizeof(credits_list) / sizeof(credits_list[0]))
#define CREDIT_GROUP_COUNT (sizeof(credit_groups) / sizeof(credit_groups[0]))
#define CREDIT_ROWS (CREDIT_COUNT + CREDIT_GROUP_COUNT)
#define ABOUT_HEADER_H 74
/* Geometry of the Credits page: the identity header on top, the scrolling list, the Licences row below. */
static void credits_geometry(struct desk_box page, struct desk_box *view, struct desk_box *licences, int *rows)
{
    int step = line_h();
    int top = page.y + 8 + ABOUT_HEADER_H, bottom = page.y + page.h - 38;
    *view = (struct desk_box){page.x + 8, top, page.w - 16, bottom - top};
    *licences = (struct desk_box){page.x + 12, page.y + page.h - 30, 96, 22};
    *rows = view->h / step;
}
static struct desk_box about_page_box(struct desk_box content)
{ return (struct desk_box){content.x + 8, content.y + 8 + DU_TAB_H - 1, content.w - 16, content.h - 27 - 42}; }
static int about_scroll_max(struct desk_box content)
{
    struct desk_box view, licences; int rows;
    credits_geometry(about_page_box(content), &view, &licences, &rows);
    return (int)CREDIT_ROWS > rows ? (int)CREDIT_ROWS - rows : 0;
}
static void about_scroll_to(struct desktop *d, int index, int row)
{
    struct desk_box content = desk_content(&d->clients[index]);
    int max = about_scroll_max(content);
    row = row < 0 ? 0 : row > max ? max : row;
    if (row == d->sh.about_scroll) return;
    d->sh.about_scroll = row;
    damage(d, desk_outer(&d->clients[index]));
    d->sh.want_reload |= 2;
}
static void about_open_licences(struct desktop *d)
{
    if (desk_task_launch(d, "files", "/system/licenses") < 0)
        desk_shell_message(d, "Licences", "Files could not be started.\n\nThe notices are in /system/licenses.", 1);
}
static void credits_page(struct desktop *d, struct du_canvas *c, struct desk_box content, struct desk_box page)
{
    const struct desk_theme *t = desk_theme;
    int step = line_h(), x = page.x + 12, y = page.y + 8;
    char line[160];
    if (d->portrait) {
        /* The approved portrait, unchanged, in a sunken frame (56 px area-sampled). */
        du_edge(c, (struct du_rect){x - 2, y - 2, 60, 60}, DU_SUNKEN);
        for (int32_t yy = 0; yy < 56; yy++) for (int32_t xx = 0; xx < 56; xx++)
            du_fill(c, (struct du_rect){x + xx, y + yy, 1, 1},
                    d->portrait[(yy * DESK_PORTRAIT_SIZE / 56) * DESK_PORTRAIT_SIZE + xx * DESK_PORTRAIT_SIZE / 56]);
    }
    int32_t tx = x + 70, right = page.x + page.w - tx - 8;
    du_text_font(c, t->font_large, tx, y - 1, "CiukiOS", t->select);
    du_text_fit(c, tx, y + 26, right, "Classic computing, one woof at a time.", SH_INK);
    snprintf(line, sizeof(line), "Dedicated to Ciuki.  Version %s", d->version[0] ? d->version : "unknown");
    du_text_fit(c, tx, y + 26 + step, right, line, t->dark);
    struct desk_box view, licences; int rows;
    credits_geometry(page, &view, &licences, &rows);
    du_hline(c, page.x + 8, view.y - 4, page.w - 16, DU_SHADOW);
    du_hline(c, page.x + 8, view.y - 3, page.w - 16, DU_HILITE);
    int32_t name_x = view.x + 6, version_x = view.x + view.w - 270, licence_x = view.x + view.w - 162;
    int skip = d->sh.about_scroll, row = 0, shown = 0;
    for (unsigned g = 0; g < CREDIT_GROUP_COUNT && shown < rows; g++) {
        if (row++ >= skip) { bold(c, name_x, view.y + shown * step + 1, credit_groups[g], t->select); shown++; }
        for (unsigned k = 0; k < CREDIT_COUNT && shown < rows; k++) {
            if (credits_list[k].group != g) continue;
            if (row++ < skip) continue;
            int32_t ly = view.y + shown * step + 1;
            du_text_fit(c, name_x + 8, ly, version_x - name_x - 12, credits_list[k].name, SH_INK);
            du_text_fit(c, version_x, ly, licence_x - version_x - 6, credits_list[k].version, t->dark);
            du_text_fit(c, licence_x, ly, view.x + view.w - licence_x - 14, credits_list[k].licence, SH_INK);
            shown++;
        }
    }
    int max = about_scroll_max(content);
    if (max > 0) {   /* a thin scroll bar: track and thumb */
        int32_t bx = view.x + view.w - 8, bh = rows * step;
        du_fill(c, (struct du_rect){bx, view.y, 7, bh}, DU_SHADOW);
        int th = bh * rows / (int)CREDIT_ROWS; if (th < 12) th = 12;
        int ty = (bh - th) * d->sh.about_scroll / max;
        du_fill(c, (struct du_rect){bx, view.y + ty, 7, th}, DU_FACE);
        du_edge(c, (struct du_rect){bx, view.y + ty, 7, th}, DU_BUTTON_EDGE);
    }
    push_button(c, licences, "&Licences", 0);
    du_text_fit(c, licences.x + licences.w + 10, licences.y + (licences.h - DU_GLYPH_H) / 2, page.x + page.w - licences.x - licences.w - 20,
                "Opens /system/licenses in Files. Scroll with keys or wheel.", DU_GRAY);
}
static void about_page(struct desktop *d, struct du_canvas *c, struct desk_box content)
{
    const struct desk_theme *t = desk_theme;
    int platinum = t->control_style == DESK_CONTROLS_PLATINUM;
    struct desk_box page = {content.x + 8, content.y + 8 + DU_TAB_H - 1, content.w - 16, content.h - 27 - 42};
    du_fill(c, R(page), SH_FACE);
    if (platinum) {
        du_outline(c, R(page), desk_mix(t->outline, DU_FACE, 150));
        du_hline(c, page.x + 1, page.y + 1, page.w - 2, DU_HILITE); du_vline(c, page.x + 1, page.y + 1, page.h - 2, DU_HILITE);
    } else {
        du_hline(c, page.x, page.y, page.w, DU_HILITE); du_vline(c, page.x, page.y, page.h, DU_HILITE);
        du_vline(c, page.x + page.w - 1, page.y, page.h, DU_DARK); du_vline(c, page.x + page.w - 2, page.y + 1, page.h - 2, DU_SHADOW);
        du_hline(c, page.x, page.y + page.h - 1, page.w, DU_DARK); du_hline(c, page.x + 1, page.y + page.h - 2, page.w - 2, DU_SHADOW);
    }
    tab_strip(c, content, d->sh.about_tab);
    int32_t x = page.x + 14, y = page.y + 14, step = line_h();
    char line[160];
    if (d->sh.about_tab == 0) {
        if (d->portrait) {
            /* The approved portrait, unchanged, in a sunken frame. */
            struct du_rect frame = {x - 2, y - 2, 116, 116};
            du_edge(c, frame, DU_SUNKEN);
            for (int32_t yy = 0; yy < 112; yy++) for (int32_t xx = 0; xx < 112; xx++) {
                uint32_t p = d->portrait[(yy * DESK_PORTRAIT_SIZE / 112) * DESK_PORTRAIT_SIZE + xx * DESK_PORTRAIT_SIZE / 112];
                du_fill(c, (struct du_rect){x + xx, y + yy, 1, 1}, p);
            }
        }
        int32_t tx = x + 132, right = page.x + page.w - tx - 8;
        du_text_font(c, t->font_large, tx, y + 2, "CiukiOS", t->select);
        du_text(c, tx, y + 30, "Classic computing, one woof at a time.", SH_INK);
        du_text_font(c, t->font_bold, tx, y + 30 + step, "Dedicated to Ciuki", t->dark);
        snprintf(line, sizeof(line), "Version: %s", d->version[0] ? d->version : "unknown");
        du_text_fit(c, tx, y + 36 + 2 * step, right, line, SH_INK);
        snprintf(line, sizeof(line), "Build: %s", d->build_identity);
        du_text_fit(c, tx, y + 36 + 3 * step, right, line, SH_INK);
        du_text(c, tx, y + 36 + 4 * step, d->realtime_source ? "Clock: RTC (UTC)" : "Clock: build seed (UTC)", SH_INK);
        snprintf(line, sizeof(line), "Display: %u x %u, 32-bit colour", (unsigned)d->width, (unsigned)d->height);
        du_text(c, tx, y + 36 + 5 * step, line, SH_INK);
        du_hline(c, page.x + 10, page.y + page.h - 30, page.w - 20, DU_SHADOW);
        du_hline(c, page.x + 10, page.y + page.h - 29, page.w - 20, DU_HILITE);
        du_text(c, x, page.y + page.h - 21, "The Ciuki portrait is a separate project asset.", DU_GRAY);
    } else if (d->sh.about_tab == 1) {
        du_icon_blit(c, x, y, desk_shell_icon("ciuki-computer", 32), 32, DU_ICON_NORMAL);
        int32_t tx = x + 46;
        bold(c, tx, y + 3, "System", SH_INK);
        snprintf(line, sizeof(line), "CiukiOS %s on %s", d->version[0] ? d->version : "", d->sh.cpu_name[0] ? d->sh.cpu_name : "i686");
        du_text_fit(c, tx, y + 3 + step, page.x + page.w - tx - 8, line, SH_INK);
        y += 46;
        static const char *const names[] = {"Physical memory:", "Available:", "Processes:", "Up time:", "Processor:", "Desktop windows:"};
        char values[6][48];
        int rows = 0;
        if (d->sh.system_valid) {
            snprintf(values[0], sizeof(values[0]), "%llu MB", (unsigned long long)(d->sh.mem_total_k / 1024));
            snprintf(values[1], sizeof(values[1]), "%llu MB", (unsigned long long)(d->sh.mem_free_k / 1024));
            snprintf(values[2], sizeof(values[2]), "%u", d->sh.processes);
            snprintf(values[3], sizeof(values[3]), "%u:%02u:%02u", d->sh.uptime_s / 3600, d->sh.uptime_s / 60 % 60, d->sh.uptime_s % 60);
            rows = 4;
        } else { du_text(c, x, y, "The kernel's process list (call 70) is unavailable.", SH_INK); y += step; }
        for (int k = 0; k < rows; k++) {
            du_text(c, x, y, names[k], t->dark);
            du_text(c, x + 130, y, values[k], SH_INK);
            y += step;
        }
        du_text(c, x, y, names[4], t->dark);
        du_text(c, x + 130, y, "not reported (no CPUID interface yet)", SH_INK); y += step;
        snprintf(line, sizeof(line), "%d", desk_task_count(d));
        du_text(c, x, y, names[5], t->dark);
        du_text(c, x + 130, y, line, SH_INK);
    } else {
        credits_page(d, c, content, page);
    }
}
static int programs_entry(const struct desktop *d, int row)
{
    for (unsigned k=0;k<d->v3_link_count;k++) if (!d->v3_links[k].desktop && row--==0) return (int)k;
    return -1;
}
static void programs_open_selected(struct desktop *d)
{
    int k=programs_entry(d,d->sh.programs_selected);
    if (k>=0 && desk_link_launch(d,(unsigned)k)<0) desk_shell_message(d,"Programs","This shortcut is unavailable or its target could not be started.",1);
}
void desk_internal_paint(struct desktop *d, int index, struct desk_box content, struct desk_box clip)
{
    const struct desk_client *cl = &d->clients[index];
    struct desk_box b;
    if (!desk_intersect(content, clip, &b)) return;
    bind(d);
    struct du_canvas c = canvas(d, b);
    du_fill(&c, R(content), SH_FACE);
    switch (cl->internal) {
    case DESK_INTERNAL_GAME_DATA: {
        char size[96];
        du_icon_blit(&c,content.x+14,content.y+16,desk_shell_icon("input-gaming",32),32,DU_ICON_NORMAL);
        bold(&c,content.x+60,content.y+16,desk_games[d->sh.save_game-1].title,SH_INK);
        snprintf(size,sizeof(size),"Game data: %u bytes of %u MiB",d->sh.save_bytes,d->sh.save_quota/(1024u*1024u));
        du_text(&c,content.x+14,content.y+62,size,SH_INK);
        du_text_wrap(&c,(struct du_rect){content.x+14,content.y+90,content.w-28,85},
            d->sh.save_confirm ? "Reset this game's saved games, high scores and setup choices? This cannot be undone." : d->sh.save_status,SH_INK);
        for (unsigned b=0;b<3;b++) {
            if (d->sh.save_confirm && b==2) continue;
            struct desk_box box={content.x+12+(int)b*154,content.y+content.h-36,146,26};
            push_button(&c,box,d->sh.save_confirm ? (b==0 ? "Reset game data" : "Cancel") :
                b==0 ? "Reset game data" : b==1 ? "Export saves" : "Close",d->sh.save_focus==b);
        }
        break;
    }
    case DESK_INTERNAL_TRANSFER:
        desk_folder_dialog_paint(d,content,clip); return;
    case DESK_INTERNAL_AUTOPLAY: {
        struct desk_autoplay *a=&d->sh.autoplay;
        char title[96];
        snprintf(title,sizeof(title),"%.32s (%c:)",a->content.inf.label[0] ? a->content.inf.label :
                 a->active.label[0] ? a->active.label : "Removable media",'A'+a->active.drive);
        if (a->content.cd_audio) snprintf(title,sizeof(title),"Audio CD");
        du_icon_blit(&c,content.x+12,content.y+12,desk_shell_icon(a->active.flags & CIUKI_MEDIA_OPTICAL ? "media-optical" : "drive-removable-media",32),32,DU_ICON_NORMAL);
        bold(&c,content.x+56,content.y+12,title,SH_INK);
        du_text(&c,content.x+56,content.y+32,"What do you want CiukiOS to do?",SH_INK);
        for (int k=0;k<AP_CHOICES;k++) {
            if (a->content.cd_audio ? (k!=AP_MUSIC && k!=AP_NOTHING) :
                ((k==AP_MUSIC && !a->content.music[0]) || (k==AP_PICTURES && !a->content.picture[0]) || (k==AP_RUN && !a->content.inf.open[0]))) continue;
            struct du_rect row={content.x+12,content.y+58+k*24,content.w-24,23};
            if (a->selected==k) du_fill(&c,row,SH_HOT);
            char label[100];
            du_text(&c,row.x+8,row.y+4,desk_autoplay_label(d,k,label,sizeof(label)),desk_autoplay_enabled(d,k) ? (a->selected==k ? SH_HOT_INK : SH_INK) : DU_GRAY);
        }
        du_well(&c,(struct du_rect){content.x+12,content.y+184,13,13},SH_WINDOW);
        if (a->always) check_mark(&c,content.x+15,content.y+187,SH_INK);
        du_text(&c,content.x+34,content.y+184,"Always do this for this device",a->active.id[0] ? SH_INK : DU_GRAY);
        push_button(&c,button_box(content,0,2),"OK",1);
        push_button(&c,button_box(content,1,2),"Cancel",0);
        break;
    }
    case DESK_INTERNAL_PROGRAMS: {
        du_text(&c,content.x+12,content.y+12,"Double-click a program, or select it and press Enter.",SH_INK);
        if (programs_entry(d,0)<0) du_text(&c,content.x+12,content.y+48,"No Windows program shortcuts are installed.",SH_INK);
        for (int row=d->sh.programs_scroll, y=content.y+40;y+22<content.y+content.h-8;row++,y+=22) {
            int k=programs_entry(d,row); if (k<0) break;
            int selected=row==d->sh.programs_selected;
            if (selected) du_fill(&c,(struct du_rect){content.x+8,y,content.w-16,22},SH_HOT);
            du_icon_blit(&c,content.x+12,y+3,desk_shell_icon("application-x-executable",16),16,DU_ICON_NORMAL);
            du_text_fit(&c,content.x+36,y+4,content.w-48,d->v3_links[k].launch.label,selected ? SH_HOT_INK : SH_INK);
        }
        break;
    }
    case DESK_INTERNAL_ABOUT:
        about_page(d, &c, content);
        push_button(&c, button_box(content, 0, 1), "OK", 1);
        break;
    case DESK_INTERNAL_RUN: {
        du_icon_blit(&c, content.x + 12, content.y + 12, desk_shell_icon("go-jump", 32), 32, DU_ICON_NORMAL);
        multiline(&c, content.x + 56, content.y + 14, "Type the name of a program in /Programs, followed by\nits arguments, and CiukiOS will start it.", SH_INK);
        du_label(&c, content.x + 12, content.y + 52 + (22 - DU_GLYPH_H) / 2, "&Open:", SH_INK, 1);
        struct du_rect field = {content.x + 56, content.y + 52, content.w - 68, 22};
        du_well(&c, field, SH_WINDOW);
        if (d->focus == index && desk_theme->focus_style == DESK_FOCUS_RING) du_focus_ring(&c, field);
        struct du_canvas fc = c; fc.clip = du_intersection(c.clip, (struct du_rect){field.x + 2, field.y + 2, field.w - 4, field.h - 4});
        /* Show the end of a long command line: drop leading characters until it fits. */
        int32_t room = field.w - 12;
        const char *shown = d->sh.run_text;
        while (*shown && du_text_width(shown) > room) shown++;
        int32_t tx = field.x + 5, ty = field.y + (field.h - DU_GLYPH_H) / 2;
        du_text(&fc, tx, ty, shown, SH_INK);
        if (d->focus == index) du_vline(&fc, tx + du_text_width(shown), ty - 2, DU_GLYPH_H + 5, SH_INK);
        push_button(&c, button_box(content, 0, 2), "OK", 1);
        push_button(&c, button_box(content, 1, 2), "Cancel", 0);
        break;
    }
    case DESK_INTERNAL_SHUTDOWN: {
        du_icon_blit(&c, content.x + 14, content.y + 16, desk_shell_icon("system-shutdown", 32), 32, DU_ICON_NORMAL);
        du_icon_blit(&c, content.x + 30, content.y + 32, desk_shell_icon("ciuki", 20), 20, DU_ICON_NORMAL);
        bold(&c, content.x + 64, content.y + 18, "What do you want CiukiOS to do?", SH_INK);
        multiline(&c, content.x + 64, content.y + 38, "Your settings are saved first. Programs\nand virtual machines will be closed.", SH_INK);
        static const char *const labels[] = {"&Restart", "&Shut Down", "Cancel"};
        for (int k = 0; k < 3; k++) push_button(&c, button_box(content, k, 3), labels[k], k == 1 + 0 * d->sh.shutdown_choice);
        break;
    }
    case DESK_INTERNAL_POWER:
        multiline(&c, content.x + 14, content.y + 18, d->power.text, SH_INK);
        push_button(&c, button_box(content, 0, d->power.state==DESK_POWER_BUSY ? 2 : 1), "Cancel", 1);
        if (d->power.state==DESK_POWER_BUSY) push_button(&c, button_box(content, 1, 2), "&Force", 0);
        break;
    default:
        du_icon_blit(&c, content.x + 14, content.y + 14, desk_shell_icon(du_message_icon_name(d->sh.message_kind), 32), 32, DU_ICON_NORMAL);
        du_text_wrap(&c,(struct du_rect){content.x+60,content.y+16,content.w-76,content.h-67},d->v3_dialog_kind==1&&d->v3_details_open?d->v3_details:d->sh.message_text,SH_INK);
        push_button(&c, message_button_box(d,content,0),
                    d->v3_dialog_kind==1 ? "Close" : d->v3_dialog_kind==2 ? "Restart Windows" : "OK",1);
        if (d->v3_dialog_kind==1) push_button(&c,message_button_box(d,content,1),"Details",0);
        if (d->v3_dialog_kind==2) push_button(&c,message_button_box(d,content,1),"Cancel",0);
        break;
    }
}
static void run_commit(struct desktop *d, int index)
{
    if (!d->sh.run_length) return;
    snprintf(d->sh.want_run, sizeof(d->sh.want_run), "%s", d->sh.run_text);
    desk_request_close(d, index);
}
/* Modal shutdown input never reaches a client, the menu, or title dragging. */
int desk_power_input(struct desktop *d, const struct ciuki_input_event *e)
{
    if (!d->power.state) return 0;
    if (d->power.state==DESK_POWER_DONE) return 1;
    int i=d->power.window;
    if (e->type==CIUKI_INPUT_KEY && e->value==1) {
        if (e->code==DU_KEY_ESC || e->code==DU_KEY_ENTER) desk_power_cancel(d);
        else if (e->code==0x21 && d->power.state==DESK_POWER_BUSY) desk_power_force(d);
    } else if (e->type==CIUKI_INPUT_BUTTON && e->code==CIUKI_BUTTON_LEFT && e->value==1 && i>=0) {
        struct desk_box content=desk_content(&d->clients[i]);
        int count=d->power.state==DESK_POWER_BUSY ? 2 : 1;
        if (desk_contains(button_box(content,0,count),d->mouse_x,d->mouse_y) ||
            desk_contains(desk_close_box(&d->clients[i]),d->mouse_x,d->mouse_y)) desk_power_cancel(d);
        else if (count==2 && desk_contains(button_box(content,1,2),d->mouse_x,d->mouse_y)) desk_power_force(d);
    }
    return 1;
}
int desk_internal_click(struct desktop *d, int index, int32_t x, int32_t y)
{
    struct desk_client *cl = &d->clients[index];
    struct desk_box content = desk_content(cl);
    switch (cl->internal) {
    case DESK_INTERNAL_GAME_DATA:
        for (unsigned b=0;b<(d->sh.save_confirm ? 2u : 3u);b++)
            if (desk_contains((struct desk_box){content.x+12+(int)b*154,content.y+content.h-36,146,26},x,y))
                game_data_action(d,index,b);
        return 1;
    case DESK_INTERNAL_PROGRAMS: {
        int row=(y-content.y-40)/22+d->sh.programs_scroll;
        if (y<content.y+40 || programs_entry(d,row)<0) return 1;
        if (d->sh.programs_click==row && d->now_ns-d->sh.programs_click_ns<(uint64_t)d->sh.cfg.double_click_ms*1000000ull) {
            d->sh.programs_selected=row; programs_open_selected(d); d->sh.programs_click_ns=0;
        } else {
            d->sh.programs_selected=row; d->sh.programs_click=row; d->sh.programs_click_ns=d->now_ns;
        }
        damage(d,desk_outer(cl)); return 1;
    }
    case DESK_INTERNAL_TRANSFER: desk_folder_dialog_click(d,x,y); return 1;
    case DESK_INTERNAL_AUTOPLAY: {
        struct desk_autoplay *a=&d->sh.autoplay;
        int row=(y-content.y-58)/24;
        if (y>=content.y+58 && y<content.y+178 && row<AP_CHOICES && desk_autoplay_enabled(d,row)) a->selected=row;
        if (y>=content.y+182 && y<content.y+206 && a->active.id[0]) a->always=!a->always;
        if (desk_contains(button_box(content,0,2),x,y)) a->want_commit=1;
        else if (desk_contains(button_box(content,1,2),x,y)) desk_request_close(d,index);
        damage(d,desk_outer(cl)); return 1;
    }
    case DESK_INTERNAL_ABOUT:
        for (int k = 0; k < 3; k++) if (desk_contains(about_tab(content, k), x, y)) { d->sh.about_tab = k; d->sh.about_scroll = 0; damage(d, desk_outer(cl)); d->sh.want_reload |= 2; return 1; }
        if (d->sh.about_tab == 2) {
            struct desk_box view, licences; int rows;
            credits_geometry(about_page_box(content), &view, &licences, &rows);
            if (desk_contains(licences, x, y)) { about_open_licences(d); return 1; }
            if (desk_contains(view, x, y) && x >= view.x + view.w - 10) {   /* scroll bar: page up or down, thumb row proportional */
                int max = about_scroll_max(content);
                about_scroll_to(d, index, max ? (y - view.y) * max / (rows * line_h()) : 0);
                return 1;
            }
        }
        if (desk_contains(button_box(content, 0, 1), x, y)) desk_request_close(d, index);
        return 1;
    case DESK_INTERNAL_RUN:
        if (desk_contains(button_box(content, 0, 2), x, y)) run_commit(d, index);
        else if (desk_contains(button_box(content, 1, 2), x, y)) desk_request_close(d, index);
        return 1;
    case DESK_INTERNAL_SHUTDOWN:
        for (int k = 0; k < 3; k++) if (desk_contains(button_box(content, k, 3), x, y)) {
            if (k < 2) d->sh.want_power = k == 0 ? 1 : 2;
            desk_request_close(d, index);
        }
        return 1;
    default:
        if (desk_contains(message_button_box(d,content,0),x,y)) {
            if (d->v3_dialog_kind) (void)desk_v3_dialog_accept(d);
            desk_request_close(d,index);
        } else if (d->v3_dialog_kind==1 && desk_contains(message_button_box(d,content,1),x,y)) {
            d->v3_details_open=!d->v3_details_open;
            uint32_t w,h;internal_size(d,DESK_INTERNAL_MESSAGE,&w,&h);
            cl->frame=desk_frame(w,h,cl->frame.x,cl->frame.y);damage_all(d);
        } else if (d->v3_dialog_kind==2 && desk_contains(message_button_box(d,content,1),x,y)) {
            d->v3_dialog_kind=0; desk_request_close(d,index);
        }
        return 1;
    }
}

/* ---- input ------------------------------------------------------------- */
static void volume_set_from(struct desktop *d, int32_t y)
{
    struct desk_box t = volume_track(d);
    int v = (t.y + t.h - y) * 100 / t.h;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v != d->sh.volume) {
        d->sh.volume = v; d->sh.want_mixer = 1;
        damage(d, outer(volume_popup(d))); damage(d,tray_box(d));
    }
}
static void tooltip_hide(struct desktop *d)
{
    if (d->sh.tooltip) {
        damage(d,tooltip_box(d));
        if (d->sh.tooltip_painted) damage(d,d->sh.tooltip_last);
        d->sh.tooltip=0; d->sh.tooltip_painted=0;
    }
}
int desk_shell_button(struct desktop *d, int button, int down)
{
    if (!d->shell) return 0;
    int32_t x = d->mouse_x, y = d->mouse_y;
    if (down && d->focus<0 && d->sh.folder && d->sh.folder->editing>=0) {
        struct desk_folder *f=d->sh.folder;
        struct desk_box b=d->sh.icons[f->editing+3].box;
        b.y+=LABEL_TOP; b.h=24;
        if (desk_contains(b,x,y)) return 1;
        f->command=DF_RENAME; f->index=f->editing; return 1;
    }
    if (!down) {
        if (button == CIUKI_BUTTON_LEFT && d->sh.icon_drag>=0) {
            int index=d->sh.icon_drag; d->sh.icon_drag=-1;
            if (d->sh.icon_dragged) {
                d->sh.click_icon=-1;
                int to=icon_hit(d,x,y);
                int folder_target=icon_is_entry(d, to) && to!=index &&
                    d->sh.folder->model.entries[to-3].meta.type==FM_DIR;
                if (icon_is_entry(d, index) && (desk_hit(d,x,y)>=0 || folder_target)) desk_folder_drop(d,-1,(d->sh.modifiers&DU_CTRL)!=0);
                else if (desk_hit(d,x,y)<0) {
                    struct desk_box *b=&d->sh.icons[index].box; damage(d,*b);
                    d->sh.background_valid=0;
                    b->x=x-b->w/2; b->y=y-b->h/2;
                    if (b->x<0) b->x=0;
                    if (b->x+b->w>(int)d->width) b->x=(int)d->width-b->w;
                    if (b->y<DESK_BAR+10) b->y=DESK_BAR+10;
                    if (b->y+b->h>(int)d->height-DESK_BAR) b->y=(int)d->height-DESK_BAR-b->h;
                    const char *name=icon_position_name(d, index);
                    if (cfg_icon_move(&d->sh.cfg,name,b->x,b->y)) d->sh.want_save=1;
                    else desk_shell_message(d,"Desktop","No space remains for icon positions in desktop.cfg.",1);
                    damage(d,*b);
                }
                return 1;
            }
        }
        if (button == CIUKI_BUTTON_LEFT && d->sh.volume_drag) { d->sh.volume_drag = 0; return 1; }
        return 0;
    }
    tooltip_hide(d);
    d->sh.hover_since = d->now_ns;
    if (d->sh.switcher) switcher_finish(d, 0);
    /* The protrusion is above the menu too: clicking it toggles Start. */
    if (d->sh.levels && desk_start_contains(d,x,y)) {
        int start=d->sh.menus[0].kind==SH_MENU_START;
        desk_menu_close(d);
        if (!start && button==CIUKI_BUTTON_LEFT) desk_menu_open(d,SH_MENU_START,0,0);
        return 1;
    }
    if (d->sh.levels) {
        for (int l = d->sh.levels - 1; l >= 0; l--) {
            int k = menu_hit(&d->sh.menus[l], x, y);
            if (k == -1) continue;
            if (k>=0 && button==CIUKI_BUTTON_RIGHT && d->sh.menus[l].items[k].action==SH_ACT_GAME)
                desk_game_properties(d,(unsigned)d->sh.menus[l].items[k].arg+1);
            else if (k >= 0 && button != CIUKI_BUTTON_MIDDLE) activate(d, l, k);
            return 1;
        }
        desk_menu_close(d);
    }
    if (button==CIUKI_BUTTON_RIGHT) {
        int page=desk_contains(volume_icon_box(d),x,y) ? CP_SOUNDS :
            desk_contains(network_icon_box(d),x,y) ? CP_NETWORK :
            desk_contains(media_icon_box(d),x,y) ? CP_AUTOPLAY :
            desk_contains(desk_shell_clock_box(d),x,y) ? CP_DATETIME : CP_HOME;
        if (page!=CP_HOME) {
            if (d->sh.volume_open) damage(d,outer(volume_popup(d)));
            if (d->sh.network_open) damage(d,outer(network_popup(d)));
            if (d->sh.media_open) damage(d,outer(media_popup(d)));
            d->sh.volume_open=d->sh.network_open=d->sh.media_open=0;
            d->sh.menu_owner=page; desk_menu_open(d,SH_MENU_TRAY_SETTINGS,x,y); return 1;
        }
    }
    if (d->sh.media_open) {
        struct desk_box p=media_popup(d);
        if (desk_contains(p,x,y)) {
            if (button==CIUKI_BUTTON_LEFT && y>=p.y+28) {
                unsigned drive=media_at(d,(unsigned)(y-p.y-28)/22);
                if (drive<26) { d->sh.want_remove=(int)drive; d->sh.remove_generation=d->sh.media.generation[drive]; }
            }
            d->sh.media_open=0; damage(d,outer(p)); return 1;
        }
        d->sh.media_open=0; damage(d,outer(p));
        if (desk_contains(media_icon_box(d),x,y)) return 1;
    }
    if (d->sh.network_open) {
        struct desk_box p = network_popup(d);
        if (desk_contains(p, x, y)) return 1;
        d->sh.network_open = 0;
        damage(d, outer(p));
        if (desk_contains(network_icon_box(d), x, y)) return 1;
    }
    if (d->sh.volume_open) {
        struct desk_box p = volume_popup(d);
        if (desk_contains(p, x, y)) {
            if (d->sh.mixer_state == 2 && button == CIUKI_BUTTON_LEFT) {
                struct desk_box t = volume_track(d), hit = {t.x - 8, t.y - 6, t.w + 16, t.h + 12};
                if (desk_contains(hit, x, y)) { d->sh.volume_drag = 1; volume_set_from(d, y); }
                else if (desk_contains(volume_mute(d), x, y)) { d->sh.muted = !d->sh.muted; d->sh.want_mixer = 1; damage(d, outer(p)); damage(d, tray_box(d)); }
            }
            return 1;
        }
        d->sh.volume_open = 0;
        damage(d, outer(p));
        if (desk_contains(volume_icon_box(d), x, y)) return 1;
    }
    /* The visible orb wins input over notifications underneath its circle. */
    if (desk_start_contains(d, x, y)) {
        if (d->sh.notification.active) {
            damage(d,notification_outer(d)); d->sh.notification.active=0; damage(d,notification_icon_box(d));
        }
        if (button == CIUKI_BUTTON_LEFT) desk_menu_open(d, SH_MENU_START, 0, 0);
        return 1;
    }
    if (d->sh.notification.active && du_contains(notification_area(d),x,y) && desk_contains(notification_box(d),x,y)) {
        if (button==CIUKI_BUTTON_LEFT) {
            struct sh_notification *n=&d->sh.notification;
            struct desk_box b=notification_box(d);
            int close=desk_contains((struct desk_box){b.x+b.w-26,b.y+12,18,18},x,y);
            damage(d,notification_outer(d)); n->active=0; damage(d,notification_icon_box(d));
            if (close) return 1;
            if (n->action==SH_NOTIFY_NETWORK) {
                if (program_index(d,"control")>=0) launch_named(d,"control","--panel=network");
                else { d->sh.network_open=1; damage(d,outer(network_popup(d))); }
            }
            else if (n->action==SH_NOTIFY_VOLUME) { d->sh.volume_open=1; damage(d,outer(volume_popup(d))); }
            else if (n->action==SH_NOTIFY_MEDIA && d->sh.media.removable_mask) { d->sh.media_open=1; damage(d,outer(media_popup(d))); }
            else desk_shell_message_class(d,n->title,n->text,n->icon);
        }
        return 1;
    }
    if (d->sh.notification.active) {
        damage(d,notification_outer(d)); d->sh.notification.active=0; damage(d,notification_icon_box(d));
    }
    struct desk_box bar = desk_taskbar(d);
    if (y >= 0 && y < DESK_BAR) {
        if (d->sh.media.removable_mask && desk_contains(media_icon_box(d),x,y) && (button==CIUKI_BUTTON_LEFT || button==CIUKI_BUTTON_RIGHT)) {
            d->sh.media_open=1; d->sh.want_reload|=32;
            damage(d,outer(media_popup(d))); return 1;
        }
        if (desk_contains(network_icon_box(d), x, y) && button == CIUKI_BUTTON_LEFT) {
            if (program_index(d,"control")>=0) launch_named(d,"control","--panel=network");
            else { d->sh.network_open=1; d->sh.want_reload|=16; damage(d,outer(network_popup(d))); }
            return 1;
        }
        if (desk_contains(volume_icon_box(d), x, y) && button == CIUKI_BUTTON_LEFT) {
            /* f1-100: opening the popup is the explicit action that asks
             * the kernel to re-read the hardware volume keys' state (8). */
            d->sh.volume_open = 1; d->sh.want_reload |= 4|8;
            damage(d, outer(volume_popup(d)));
            return 1;
        }
        if (desk_contains(notification_icon_box(d),x,y) && button==CIUKI_BUTTON_LEFT) {
            /* The current slot remains available through its top-bar anchor. */
            if (d->sh.notification.title[0])
                desk_shell_message_class(d,d->sh.notification.title,d->sh.notification.text,d->sh.notification.icon);
        }
        return 1;
    }
    if (desk_contains(bar,x,y)) {
        for (int i = 0; i < DESK_CLIENTS; i++) if (desk_contains(desk_task_button(d, i), x, y)) {
            if (button == CIUKI_BUTTON_RIGHT) {
                d->sh.menu_owner = i;
                desk_menu_open(d, SH_MENU_TASK, x, bar.y);
            } else if (button == CIUKI_BUTTON_LEFT) {
                if (d->clients[i].minimized || d->focus == i) desk_minimize(d, i);
                else desk_focus(d, i);
            }
            break;
        }
        return 1;
    }
    if (desk_hit(d, x, y) >= 0) return 0;
    int icon = icon_hit(d, x, y);
    if (button == CIUKI_BUTTON_LEFT) {
        if (icon >= 0 && icon == d->sh.click_icon && d->now_ns - d->sh.click_ns <= (uint64_t)d->sh.cfg.double_click_ms * 1000000ull) {
            d->sh.click_icon = -1;
            icon_open(d, icon);
            return 1;
        }
        if (icon != d->sh.icon_selected) {
            if (d->sh.icon_selected >= 0 && d->sh.icon_selected < d->sh.icon_count) damage(d, d->sh.icons[d->sh.icon_selected].box);
            d->sh.icon_selected = icon;
            if (icon >= 0) damage(d, d->sh.icons[icon].box);
        }
        d->sh.icon_drag=icon; d->sh.icon_dragged=0; d->sh.drag_start_x=x; d->sh.drag_start_y=y;
        d->sh.click_icon = icon; d->sh.click_ns = d->now_ns;
        return 0;   /* the window manager still clears the focus */
    }
    if (button == CIUKI_BUTTON_RIGHT) {
        if (icon >= 0) {
            if (d->sh.icon_selected >= 0 && d->sh.icon_selected < d->sh.icon_count) damage(d, d->sh.icons[d->sh.icon_selected].box);
            d->sh.icon_selected = icon; damage(d, d->sh.icons[icon].box);
            d->sh.menu_owner = icon;
            desk_menu_open(d, SH_MENU_ICON, x, y);
        } else desk_menu_open(d, SH_MENU_DESKTOP, x, y);
        return 1;
    }
    return 0;
}
static int hover_target(const struct desktop *d)
{
    int x=d->mouse_x,y=d->mouse_y;
    if (desk_contains(clock_box(d),x,y)) return 1;
    if (desk_contains(network_icon_box(d),x,y)) return 2;
    if (desk_contains(volume_icon_box(d),x,y)) return 3;
    if (d->sh.media.removable_mask && desk_contains(media_icon_box(d),x,y)) return 4;
    if (desk_contains(notification_icon_box(d),x,y)) return 5;
    for (int i=0;i<DESK_CLIENTS;i++) if (d->clients[i].window && desk_contains(desk_task_button(d,i),x,y)) return 16+i;
    return 0;
}
void desk_shell_motion(struct desktop *d)
{
    if (!d->shell) return;
    int32_t x = d->mouse_x, y = d->mouse_y;
    if (d->sh.icon_drag>=0 && (abs(x-d->sh.drag_start_x)>4 || abs(y-d->sh.drag_start_y)>4)) d->sh.icon_dragged=1;
    if (d->sh.volume_drag) volume_set_from(d, y);
    for (int l = d->sh.levels - 1; l >= 0; l--) {
        struct sh_menu *m = &d->sh.menus[l];
        int k = desk_start_contains(d,x,y) ? -1 : menu_hit(m, x, y);
        if (k == -1) continue;
        int hot = k >= 0 ? k : -1;
        menu_highlight(d,m,hot);
        if (k >= 0 && m->items[k].action == SH_ACT_SUBMENU) submenu(d, l, k);
        else if (k >= 0 && l + 1 < d->sh.levels) {
            for (int j = l + 1; j < d->sh.levels; j++) damage(d, outer(d->sh.menus[j].box));
            d->sh.levels = l + 1;
        }
        break;
    }
    int over_start = desk_start_contains(d, x, y);
    if (over_start != d->sh.start_hot) { d->sh.start_hot = over_start; damage(d, desk_start_bounds(d)); }
    int target=hover_target(d);
    if (target!=d->sh.hover_target) {
        if (d->sh.hover_target>=16) damage(d,desk_task_button(d,d->sh.hover_target-16));
        if (target>=16) damage(d,desk_task_button(d,target-16));
        tooltip_hide(d); d->sh.hover_target=target;
        d->sh.hover_since=target ? (d->now_ns ? d->now_ns : 1) : 0;
    } else if (target && !d->sh.hover_since) d->sh.hover_since=d->now_ns ? d->now_ns : 1;
}
void desk_shell_tick(struct desktop *d)
{
    if (!d->shell) return;
    if (!d->sh.network_poll_ns || (d->now_ns >= d->sh.network_poll_ns &&
        d->now_ns - d->sh.network_poll_ns >= 2000000000ull)) {
        d->sh.network_poll_ns = d->now_ns ? d->now_ns : 1;
        d->sh.want_reload |= 16|32;
    }
    struct sh_notification *n=&d->sh.notification;
    if (n->active) {
        uint64_t age=d->now_ns>=n->started ? d->now_ns-n->started : 0;
        int slide=age>=160000000ull ? 0 : 20-(int)(age/8000000ull);
        if (d->now_ns>=n->expires) { damage(d,notification_outer(d)); n->active=0; damage(d,notification_icon_box(d)); }
        else if (slide!=n->slide) {
            damage(d,notification_outer(d)); n->slide=slide; damage(d,notification_outer(d));
        }
    }
    int target=hover_target(d);
    if (target!=d->sh.hover_target) {
        if (d->sh.hover_target>=16) damage(d,desk_task_button(d,d->sh.hover_target-16));
        if (target>=16) damage(d,desk_task_button(d,target-16));
        tooltip_hide(d); d->sh.hover_target=target;
        d->sh.hover_since=target ? (d->now_ns ? d->now_ns : 1) : 0;
    }
    if (d->sh.tooltip && d->sh.tooltip!=target) tooltip_hide(d);
    if (!d->sh.tooltip && d->sh.hover_since && target && !n->active &&
        !d->sh.levels && !d->sh.volume_open && !d->sh.network_open && !d->sh.media_open &&
        d->now_ns>=d->sh.hover_since && d->now_ns-d->sh.hover_since>=600000000ull) {
        d->sh.tooltip=target; damage(d,tooltip_box(d));
    }
    if (d->sh.tooltip) {
        char text[1024]; tooltip_text(d,text,sizeof(text));
        uint32_t hash=2166136261u;
        for (const unsigned char *s=(const unsigned char *)text;*s;s++) hash=(hash^*s)*16777619u;
        struct desk_box b=tooltip_box(d);
        if (d->sh.tooltip_painted && (hash!=d->sh.tooltip_hash || memcmp(&b,&d->sh.tooltip_last,sizeof(b)))) {
            damage(d,d->sh.tooltip_last); damage(d,b);
        }
        d->sh.tooltip_hash=hash; d->sh.tooltip_last=b; d->sh.tooltip_painted=1;
    }
}
void desk_shell_scale_motion(struct desktop *d, int32_t *dx, int32_t *dy)
{
    int num = d->shell ? cfg_mouse_numerator(d->sh.cfg.mouse_speed) : 8;
    if (num == 8) return;
    int32_t ax = *dx * num + d->sh.motion_rx, ay = *dy * num + d->sh.motion_ry;
    *dx = ax / 8; *dy = ay / 8;
    d->sh.motion_rx = ax - *dx * 8; d->sh.motion_ry = ay - *dy * 8;
}
void desk_shell_clock_damage(struct desktop *d, uint64_t previous)
{
    if (d->realtime_second/60!=previous/60) {
        /* Date text changes only at midnight; its right-aligned previous
         * width can differ, so clear the bounded date region at that edge. */
        if (d->realtime_second/86400!=previous/86400)
            damage(d,(struct desk_box){(int)d->width-300,0,300,DESK_BAR});
        if (d->sh.tooltip==1 && d->realtime_second/86400!=previous/86400) {
            if(d->sh.tooltip_painted)damage(d,d->sh.tooltip_last);
            damage(d,tooltip_box(d));
        }
    }
    if (d->sh.cfg.clock_seconds || d->realtime_second/60!=previous/60) {
        /* Proportional digits and 12-hour rollover can move every tray icon
         * in the top bar. Erase their previous positions as well. */
        uint64_t old = d->app_test ? 12 * 3600 : previous;
        int dx = desk_shell_clock_width_at(d, wall_seconds(d)) - desk_shell_clock_width_at(d, old);
        if (dx) desk_shell_tray_damage(d, tray_box(d).w - dx);
        else damage(d, clock_box(d));
    }
}
static int menu_step(const struct sh_menu *m, int from, int dir)
{
    for (int n = 0, k = from; n < m->count; n++) {
        k = (k + dir + m->count) % m->count;
        if (!(m->items[k].flags & (SH_ITEM_SEPARATOR | SH_ITEM_DISABLED))) return k;
    }
    return from;
}
static int menu_key(struct desktop *d, int code)
{
    int l = d->sh.levels - 1;
    struct sh_menu *m = &d->sh.menus[l];
    if (code==DU_KEY_F10 && (d->sh.modifiers&DU_SHIFT) && m->hot>=0 && m->items[m->hot].action==SH_ACT_GAME) {
        desk_game_properties(d,(unsigned)m->items[m->hot].arg+1); return 1;
    }
    switch (code) {
    case DU_KEY_UP: case DU_KEY_DOWN:
        menu_highlight(d,m,menu_step(m, m->hot < 0 ? (code == DU_KEY_UP ? 0 : m->count - 1) : m->hot, code == DU_KEY_UP ? -1 : 1));
        return 1;
    case DU_KEY_RIGHT:
        if (m->hot >= 0 && m->items[m->hot].action == SH_ACT_SUBMENU) {
            submenu(d, l, m->hot);
            struct sh_menu *s = &d->sh.menus[d->sh.levels - 1];
            if (s != m) menu_highlight(d,s,menu_step(s,s->count-1,1));
        }
        return 1;
    case DU_KEY_LEFT:
        if (l > 0) { damage(d, outer(m->box)); d->sh.levels--; }
        return 1;
    case DU_KEY_ENTER: case DU_KEY_SPACE:
        if (m->hot >= 0) {
            if (m->items[m->hot].action == SH_ACT_SUBMENU) return menu_key(d, DU_KEY_RIGHT);
            activate(d, l, m->hot);
        }
        return 1;
    case DU_KEY_ESC:
        if (l > 0) { damage(d, outer(m->box)); d->sh.levels--; } else desk_menu_close(d);
        return 1;
    default: return 1;
    }
}
int desk_shell_wheel(struct desktop *d, int detents)
{
    if (!d->shell || d->focus < 0 || d->clients[d->focus].internal != DESK_INTERNAL_ABOUT || d->sh.about_tab != 2) return 0;
    about_scroll_to(d, d->focus, d->sh.about_scroll - detents * 3);
    return 1;
}
static unsigned keymap_mods(const struct desktop *d)
{
    return (d->sh.modifiers & DU_SHIFT ? CIUKI_KEYMAP_SHIFT : 0) | (d->sh.alt_code == 0x138 ? CIUKI_KEYMAP_ALTGR : 0);
}
static int internal_key(struct desktop *d, int index, int code, int value)
{
    struct desk_client *c = &d->clients[index];
    if (c->internal==DESK_INTERNAL_TRANSFER) { desk_folder_dialog_key(d,code,value); return 1; }
    if (value == 0) return 1;
    switch (c->internal) {
    case DESK_INTERNAL_GAME_DATA:
        if (code==DU_KEY_ESC) {
            if (d->sh.save_confirm) { d->sh.save_confirm=0; d->sh.save_focus=2; damage_all(d); }
            else desk_request_close(d,index);
        } else if (code==DU_KEY_TAB) {
            d->sh.save_focus=(d->sh.save_focus+1)%(d->sh.save_confirm ? 2u : 3u); damage_all(d);
        } else if (code==DU_KEY_ENTER || code==DU_KEY_SPACE) game_data_action(d,index,d->sh.save_focus);
        return 1;
    case DESK_INTERNAL_PROGRAMS:
        if (code==DU_KEY_ESC) desk_request_close(d,index);
        else if (code==DU_KEY_ENTER) programs_open_selected(d);
        else if (code==DU_KEY_UP && d->sh.programs_selected>0) d->sh.programs_selected--;
        else if (code==DU_KEY_DOWN && programs_entry(d,d->sh.programs_selected+1)>=0) d->sh.programs_selected++;
        {
            int rows=(desk_content(c).h-48)/22; if (rows<1) rows=1;
            if (d->sh.programs_selected<d->sh.programs_scroll) d->sh.programs_scroll=d->sh.programs_selected;
            if (d->sh.programs_selected>=d->sh.programs_scroll+rows) d->sh.programs_scroll=d->sh.programs_selected-rows+1;
        }
        damage(d,desk_outer(c)); return 1;
    case DESK_INTERNAL_AUTOPLAY: {
        struct desk_autoplay *a=&d->sh.autoplay;
        if (code==DU_KEY_ESC) desk_request_close(d,index);
        else if (code==DU_KEY_ENTER) a->want_commit=1;
        else if (code==DU_KEY_SPACE && a->active.id[0]) a->always=!a->always;
        else if (code==DU_KEY_UP || code==DU_KEY_DOWN || code==DU_KEY_TAB) {
            int step=code==DU_KEY_UP ? -1 : 1;
            do { a->selected=(a->selected+step+AP_CHOICES)%AP_CHOICES; } while (!desk_autoplay_enabled(d,a->selected));
        }
        damage(d,desk_outer(c)); return 1;
    }
    case DESK_INTERNAL_RUN:
        if (code == DU_KEY_ENTER) run_commit(d, index);
        else if (code == DU_KEY_ESC) desk_request_close(d, index);
        else if (code == DU_KEY_BACKSPACE) { if (d->sh.run_length) d->sh.run_text[--d->sh.run_length] = 0; }
        else if (!(d->sh.modifiers & DU_CTRL)) {
            unsigned ch = ciuki_keymap_char(d->sh.cfg.keyboard, (unsigned)code, keymap_mods(d));
            if (ch >= 32 && ch < 127 && d->sh.run_length + 1 < sizeof(d->sh.run_text)) {
                d->sh.run_text[d->sh.run_length++] = (char)ch; d->sh.run_text[d->sh.run_length] = 0;
            }
        }
        damage(d, desk_outer(c));
        return 1;
    case DESK_INTERNAL_ABOUT:
        if (code == DU_KEY_ESC || code == DU_KEY_ENTER) desk_request_close(d, index);
        else if (d->sh.about_tab == 2 && (code == DU_KEY_UP || code == DU_KEY_DOWN || code == DU_KEY_PGUP || code == DU_KEY_PGDN ||
                                           code == DU_KEY_HOME || code == DU_KEY_END)) {
            struct desk_box view, licences; int rows, max = about_scroll_max(desk_content(c)), at = d->sh.about_scroll;
            credits_geometry(about_page_box(desk_content(c)), &view, &licences, &rows);
            about_scroll_to(d, index, code == DU_KEY_UP ? at - 1 : code == DU_KEY_DOWN ? at + 1 : code == DU_KEY_PGUP ? at - rows + 1 :
                                       code == DU_KEY_PGDN ? at + rows - 1 : code == DU_KEY_HOME ? 0 : max);
        }
        else if (d->sh.about_tab == 2 && code == 0x26) about_open_licences(d);   /* L */
        else if (code == DU_KEY_TAB || code == DU_KEY_RIGHT || code == DU_KEY_LEFT) {
            d->sh.about_tab = (d->sh.about_tab + (code == DU_KEY_LEFT || (d->sh.modifiers & DU_SHIFT) ? 2 : 1)) % 3;
            d->sh.about_scroll = 0;
            d->sh.want_reload |= 2;
            damage(d, desk_outer(c));
        }
        return 1;
    case DESK_INTERNAL_SHUTDOWN:
        if (code == DU_KEY_ESC) desk_request_close(d, index);
        else if (code == DU_KEY_ENTER || code == 0x1f) { d->sh.want_power = 2; desk_request_close(d, index); }
        else if (code == 0x13) { d->sh.want_power = 1; desk_request_close(d, index); }
        return 1;
    default:
        if(d->v3_dialog_kind==1&&(code==DU_KEY_TAB||code==0x20)){
            d->v3_details_open=!d->v3_details_open;
            uint32_t w,h;internal_size(d,DESK_INTERNAL_MESSAGE,&w,&h);
            c->frame=desk_frame(w,h,c->frame.x,c->frame.y);damage_all(d);return 1;
        }
        if (code==DU_KEY_ENTER || code==DU_KEY_SPACE) {
            if (d->v3_dialog_kind) (void)desk_v3_dialog_accept(d);
            desk_request_close(d,index);
        } else if (code==DU_KEY_ESC) { if(d->v3_dialog_kind==1)(void)desk_v3_dialog_accept(d);d->v3_dialog_kind=0; desk_request_close(d,index); }
        return 1;
    }
}
static void toggle_start(struct desktop *d)
{
    if (d->sh.levels && d->sh.menus[0].kind == SH_MENU_START) desk_menu_close(d);
    else { desk_menu_close(d); desk_menu_open(d, SH_MENU_START, 0, 0); d->sh.menus[0].hot = 0; }
}
static void icon_select(struct desktop *d, int icon)
{
    if (d->sh.icon_selected >= 0 && d->sh.icon_selected < d->sh.icon_count) damage(d, d->sh.icons[d->sh.icon_selected].box);
    d->sh.icon_selected = icon;
    if (icon >= 0) damage(d, d->sh.icons[icon].box);
}
static int volume_action(struct desktop *d, unsigned action, int value, int notify)
{
    if (action<CIUKI_CONSUMER_VOLUME_UP || action>CIUKI_CONSUMER_MUTE) return 0;
    if (d->sh.mixer_state!=2) return 0;
    if (value) {
        if (action==CIUKI_CONSUMER_MUTE) {
            if (value!=1) return 1;
            d->sh.muted=!d->sh.muted;
        } else {
            int v=d->sh.volume+(action==CIUKI_CONSUMER_VOLUME_UP ? 1 : -1);
            if (v<0) v=0;
            if (v>100) v=100;
            if (v==d->sh.volume) return 1;
            d->sh.volume=v;
        }
        d->sh.want_mixer=1;
        if (notify) {
            char text[64]; snprintf(text,sizeof(text),"Volume: %d%%%s",d->sh.volume,d->sh.muted ? " (muted)" : "");
            desk_shell_notify(d,"Volume",text,DU_DLG_ICON_INFO,SH_NOTIFY_VOLUME);
        }
        desk_shell_volume_damage(d);
    }
    return 1;
}
int desk_shell_consumer(struct desktop *d, unsigned action, int value)
{
    if (action<CIUKI_CONSUMER_VOLUME_UP || action>CIUKI_CONSUMER_MUTE) return 0;
    (void)volume_action(d,action,value,1); return 1;
}
int desk_shell_key(struct desktop *d, int code, int value)
{
    if (!d->shell) return 0;
    if (value && d->sh.tooltip) { tooltip_hide(d); d->sh.hover_since=0; }
    int alt = code == 0x38 || code == 0x138, ctrl = code == 0x1d || code == 0x11d, shift = code == 0x2a || code == 0x36;
    int logo = code == 0x15b || code == 0x15c;
    if (ctrl || shift) {
        unsigned bit = ctrl ? DU_CTRL : DU_SHIFT;
        if (value) d->sh.modifiers |= bit; else d->sh.modifiers &= ~bit;
        return 0;
    }
    if (alt) {
        if (value == 1 && !(d->sh.modifiers & DU_ALT)) {
            d->sh.modifiers |= DU_ALT; d->sh.alt_routed = 0; d->sh.alt_used = 0; d->sh.alt_code = code;
            return 1;                       /* held back until the next key decides */
        }
        if (value == 2) return d->sh.alt_routed ? 0 : 1;
        if (value == 0) {
            d->sh.modifiers &= ~DU_ALT; d->sh.alt_code = 0;
            if (d->sh.switcher) switcher_finish(d, 1);
            if (d->sh.alt_used && !d->sh.alt_routed) return 1;
            if (!d->sh.alt_routed) desk_route_key(d, code, 1);   /* a lone Alt tap reaches the program */
            return 0;
        }
        return 1;
    }
    if (logo) {
        if (value == 1) d->sh.logo_down = 1;
        else if (value == 0 && d->sh.logo_down) { d->sh.logo_down = 0; toggle_start(d); }
        return 1;
    }
    if (value) d->sh.logo_down = 0;
    if (value==1 && code==0x153 && (d->sh.modifiers&(DU_CTRL|DU_ALT))==(DU_CTRL|DU_ALT)) {
        desk_v3_revoke(d); desk_v3_pointer_release(d); launch_named(d,"tasks",NULL); d->sh.alt_used=1; return 1;
    }
    if (d->sh.modifiers & DU_ALT) {
        if (code==DU_KEY_ENTER && d->focus>=0 && d->clients[d->focus].version==DESK_V3_VERSION) {
            if (value==1) {
                struct desk_client *c=&d->clients[d->focus]; struct ciuki_message m;
                desk_v3_message(&m,DESK_V3_LEASE_EVENT,20,0,c->v3_id,c->v3_generation);
                desk_put32(m.data+16,0); desk_put32(m.data+20,d->v3_lease_generation ? d->v3_lease_generation : 1);
                desk_put32(m.data+24,c->info.width); desk_put32(m.data+28,c->info.height); desk_put32(m.data+32,32);
                desk_v3_revoke(d); desk_queue(d,d->focus,&m);
            }
            d->sh.alt_used=1; return 1;
        }
        if (code==DU_KEY_SPACE) {
            if (value==1 && d->focus>=0) {
                int i=desk_modal_target(d,d->focus);
                d->sh.menu_owner=i;
                desk_menu_open(d,SH_MENU_TASK,d->clients[i].frame.x,d->clients[i].frame.y+DESK_TITLE);
                d->sh.menus[0].hot=0;
            }
            d->sh.alt_used=1; return 1;
        }
        if (code == DU_KEY_TAB) {
            if (value) { desk_v3_revoke(d); desk_v3_pointer_release(d); if (!d->sh.switcher) switcher_start(d, (d->sh.modifiers & DU_SHIFT) != 0); else switcher_step(d, (d->sh.modifiers & DU_SHIFT) != 0); }
            d->sh.alt_used = 1;
            return 1;
        }
        if (code == DU_KEY_F4) {
            if (value == 1) {
                if (d->sh.levels) desk_menu_close(d);
                else if (d->focus >= 0) desk_request_close(d, d->focus);
                else desk_internal_open(d, DESK_INTERNAL_SHUTDOWN);
            }
            d->sh.alt_used = 1;
            return 1;
        }
        if (code == DU_KEY_ESC) {
            if (value==1) {
                if (d->sh.switcher) switcher_finish(d,0);
                else {
                    int next=-1;
                    uint64_t below=d->focus>=0 ? d->clients[d->focus].order : UINT64_MAX;
                    for (int i=0;i<DESK_CLIENTS;i++) if (d->clients[i].window && !d->clients[i].minimized &&
                        desk_modal_target(d,i)==i && d->clients[i].order<below &&
                        (next<0 || d->clients[i].order>d->clients[next].order)) next=i;
                    if (next<0) next=desk_next_window(d,d->focus,0,NULL);
                    if (next>=0) {
                        /* Keep every order positive and unique: the scene
                         * painter traverses strictly increasing orders. */
                        int old=d->focus, owner=-1;
                        if (old>=0 && d->clients[old].owner_id) for (int i=0;i<DESK_CLIENTS;i++)
                            if (d->clients[i].window && d->clients[i].window_id==d->clients[old].owner_id) owner=i;
                        unsigned shift=owner>=0 ? 2u : 1u;
                        for (int i=0;i<DESK_CLIENTS;i++) if (d->clients[i].window) d->clients[i].order+=shift;
                        d->order+=shift;
                        if (owner>=0) { d->clients[owner].order=1; damage(d,desk_outer(&d->clients[owner])); }
                        if (old>=0) {
                            d->clients[old].order=shift;
                            damage(d,desk_outer(&d->clients[old]));
                        }
                        desk_focus(d,next);
                    }
                }
            }
            d->sh.alt_used = 1; return 1;
        }
        if (value && !d->sh.alt_routed) { desk_route_key(d, d->sh.alt_code, 1); d->sh.alt_routed = 1; }
        if (!value && d->sh.alt_used && (code == DU_KEY_TAB || code == DU_KEY_F4 || code==DU_KEY_SPACE || code==DU_KEY_ESC)) return 1;
    }
    if (!value && (code == DU_KEY_TAB || code == DU_KEY_F4 || code==DU_KEY_SPACE || code==DU_KEY_ESC) && d->sh.alt_used) return 1;
    if ((d->sh.modifiers & DU_CTRL) && code == DU_KEY_ESC) { if (value == 1) toggle_start(d); return 1; }
    if (d->focus<0 && d->sh.folder && d->sh.folder->editing>=0) {
        struct desk_folder *f=d->sh.folder;
        if (!value) return 1;
        size_t n=strlen(f->edit);
        if (code==DU_KEY_ESC) f->editing=-1;
        else if (code==DU_KEY_ENTER) { f->command=DF_RENAME; f->index=f->editing; }
        else if (code==0x1e && (d->sh.modifiers&DU_CTRL)) f->select_all=1;
        else if (code==DU_KEY_LEFT || code==DU_KEY_RIGHT || code==DU_KEY_HOME || code==DU_KEY_END) {
            if (code==DU_KEY_HOME) f->cursor=0;
            else if (code==DU_KEY_END) f->cursor=(unsigned)n;
            else if (code==DU_KEY_LEFT && f->cursor) f->cursor--;
            else if (code==DU_KEY_RIGHT && f->cursor<n) f->cursor++;
            f->select_all=0;
        } else if (code==DU_KEY_BACKSPACE || code==DU_KEY_DELETE) {
            if (f->select_all) { f->edit[0]=0; f->cursor=0; }
            else if (code==DU_KEY_BACKSPACE && f->cursor) { memmove(f->edit+f->cursor-1,f->edit+f->cursor,n-f->cursor+1); f->cursor--; }
            else if (code==DU_KEY_DELETE && f->cursor<n) memmove(f->edit+f->cursor,f->edit+f->cursor+1,n-f->cursor);
            f->select_all=0;
        } else if (!(d->sh.modifiers&DU_CTRL)) {
            unsigned ch=ciuki_keymap_char(d->sh.cfg.keyboard,(unsigned)code,keymap_mods(d));
            if (ch>=32 && ch<127) {
                if (f->select_all) { f->edit[0]=0; f->cursor=0; n=0; }
                if (n+1<sizeof(f->edit)) { memmove(f->edit+f->cursor+1,f->edit+f->cursor,n-f->cursor+1); f->edit[f->cursor++]=(char)ch; }
                f->select_all=0;
            }
        }
        damage_all(d); return 1;
    }
    if (d->sh.levels) return value ? menu_key(d, code) : 1;
    if (d->sh.media_open) {
        if (value && code==DU_KEY_ESC) { d->sh.media_open=0; damage(d,outer(media_popup(d))); return 1; }
        if (value && code==DU_KEY_DOWN && d->sh.media_scroll+media_rows(d)<media_count(d)) { d->sh.media_scroll++; damage(d,outer(media_popup(d))); return 1; }
        if (value && code==DU_KEY_UP && d->sh.media_scroll) { d->sh.media_scroll--; damage(d,outer(media_popup(d))); return 1; }
    }
    if (d->sh.network_open && code == DU_KEY_ESC) { if (value) { d->sh.network_open = 0; damage(d, outer(network_popup(d))); } return 1; }
    if (code==0x130 || code==0x12e || code==0x120 ||
        (d->sh.volume_open && (code==DU_KEY_UP || code==DU_KEY_DOWN))) {
        unsigned action=code==0x120 ? CIUKI_CONSUMER_MUTE :
            code==0x130 || code==DU_KEY_UP ? CIUKI_CONSUMER_VOLUME_UP : CIUKI_CONSUMER_VOLUME_DOWN;
        if (volume_action(d,action,value,code!=DU_KEY_UP && code!=DU_KEY_DOWN)) return 1;
    }
    if (d->sh.volume_open && code == DU_KEY_ESC) { if (value) { d->sh.volume_open = 0; damage(d, outer(volume_popup(d))); } return 1; }
    if (d->sh.notification.active && code==DU_KEY_ESC) {
        if (value) { damage(d,notification_outer(d)); d->sh.notification.active=0; damage(d,notification_icon_box(d)); }
        return 1;
    }
    if (d->focus >= 0 && d->clients[d->focus].internal) {
        /* AltGr characters reach the Run field; other Alt chords are ignored here. */
        if ((d->sh.modifiers & DU_ALT) && d->sh.alt_code != 0x138) return 1;
        return internal_key(d, d->focus, code, value);
    }
    if (d->focus < 0 && d->sh.cfg.desktop_icons && d->sh.icon_count && !d->shell_test && !d->tasks_test && !d->app_test) {
        if (!value) return 0;
        int s = d->sh.icon_selected; if (s>=d->sh.icon_count) s=-1;
        if (code==DU_KEY_UP || code==DU_KEY_DOWN || code==DU_KEY_LEFT || code==DU_KEY_RIGHT) {
            int next=s<0 ? 0 : s; int64_t best=INT64_MAX;
            if (s>=0) for (int i=0;i<d->sh.icon_count;i++) if (i!=s) {
                int dx=d->sh.icons[i].box.x-d->sh.icons[s].box.x, dy=d->sh.icons[i].box.y-d->sh.icons[s].box.y;
                if ((code==DU_KEY_UP && dy>=0) || (code==DU_KEY_DOWN && dy<=0) ||
                    (code==DU_KEY_LEFT && dx>=0) || (code==DU_KEY_RIGHT && dx<=0)) continue;
                int64_t distance=(int64_t)dx*dx+(int64_t)dy*dy;
                if (distance<best) { best=distance; next=i; }
            }
            icon_select(d,next); return 1;
        }
        if (d->sh.folder) {
            if (code==DU_KEY_F2 && icon_is_entry(d, s)) { desk_folder_rename(d,s); return 1; }
            if (code==DU_KEY_DELETE && icon_is_entry(d, s)) { d->sh.folder->command=DF_DELETE; d->sh.folder->index=s-3; return 1; }
            if (code==DU_KEY_F5) { d->sh.folder->command=DF_REFRESH; return 1; }
            if (d->sh.modifiers&DU_CTRL) {
                int command=code==0x2f ? DF_PASTE : code==0x2e ? DF_COPY : code==0x2d ? DF_CUT : 0;
                if (command && (command==DF_PASTE || icon_is_entry(d, s))) { d->sh.folder->command=command; d->sh.folder->index=s-3; return 1; }
            }
        }
        if (code == DU_KEY_ENTER && s >= 0) { icon_open(d, s); return 1; }
    }
    return 0;
}

/* ---- settings ---------------------------------------------------------- */
static void icons_prepare(void)
{
    if (!store_icon) return;
    /* The real Tango reader caches and decodes on first use. Reserve all
     * bounded shell/menu/window variants now, including the generic icon
     * used by crash victims whose title differs from the survivor's. */
    static const unsigned sizes[]={16,22,32,48};
    static const char *const extra[]={"go-jump","drive-removable-media","media-optical"};
    for (unsigned s=0;s<sizeof(sizes)/sizeof(*sizes);s++) {
        for (unsigned i=0;i<sizeof(icon_table)/sizeof(*icon_table);i++)
            (void)store_icon(icon_table[i].name,sizes[s]);
        for (unsigned i=0;i<sizeof(programs)/sizeof(*programs);i++)
            (void)store_icon(programs[i].icon,sizes[s]);
        for (unsigned i=0;i<DESK_CATALOG_COUNT;i++) (void)store_icon(desk_catalog[i].tango,sizes[s]);
        for (unsigned i=0;i<DESK_GAME_COUNT;i++) (void)store_icon(desk_games[i].tango,sizes[s]);
        for (unsigned i=0;i<sizeof(extra)/sizeof(*extra);i++) (void)store_icon(extra[i],sizes[s]);
    }
}
void desk_shell_reset(struct desktop *d)
{
    bind(d);
    icons_prepare();
    cfg_defaults(&d->sh.cfg);
    d->sh.icon_selected = d->sh.click_icon = d->sh.icon_drag = -1;
    d->sh.volume = 20;
}
void desk_shell_apply(struct desktop *d)
{
    bind(d);
    if (!d->sh.icon_count) icons_build(d);
    icons_layout(d);
    d->sh.wall_valid = 0;
    /* The theme named by desktop.cfg (default Ciuki Blue), at once. */
    if (desk_theme_use(d->sh.cfg.theme)) desk_theme_use(DESK_THEME_DEFAULT);
    if (d->sh.levels) desk_menu_close(d);
    damage_all(d);
}

/* Rebuilds the full-screen wallpaper cache when it is stale (settings,
 * geometry or a newly decoded image). Returns -1 when the request could not
 * be honoured: the desktop colour (and the portrait for "ciuki") is shown. */
int desk_shell_wallpaper_update(struct desktop *d)
{
    if (!d->shell) return 0;
    if (d->sh.wall_valid && d->sh.wall && d->sh.wall_w == d->width && d->sh.wall_h == d->height) return 0;
    if (!d->sh.wall || d->sh.wall_w != d->width || d->sh.wall_h != d->height) {
        free(d->sh.background); d->sh.background=NULL;
        free(d->sh.wall);
        d->sh.wall = malloc((size_t)d->width * d->height * 4u);
        d->sh.wall_w = d->sh.wall ? d->width : 0; d->sh.wall_h = d->sh.wall ? d->height : 0;
        if (!d->sh.wall) { d->sh.wall_valid = 0; return -1; }
    }
    /* Initial/resize placeholder only. The worker paints every selected
     * wallpaper, including procedural ones, before one complete publication. */
    int r = desk_wallpaper_render(d->sh.wall, d->width, d->height, DESK_BAR, DESK_BAR,
                                  d->sh.wallpaper_pending ? "none" : d->sh.cfg.wallpaper,
                                  d->sh.cfg.wallpaper_mode, desktop_colour(d), &d->sh.image, d->portrait);
    d->sh.wall_valid = 1;
    d->sh.background_valid=0;
    d->sh.image_failed = r < 0;
    damage_all(d);
    return r;
}

void desk_links_refresh(struct desktop *d)
{
    if (!d->v3_links_list || d->now_ns<d->v3_links_next) return;
    d->v3_links_next=d->now_ns+2000000000ull;
    if (!d->v3_links) d->v3_links=calloc(DESK_LINKS,sizeof(*d->v3_links));
    if (!d->v3_links) return;
    int n=d->v3_links_list(d,d->v3_links,DESK_LINKS);
    if (n==-EAGAIN) return;
    d->v3_link_count=n>0 && n<=(int)DESK_LINKS ? (unsigned)n : 0;
    /* Rebuild the native folder icons first, then append validated link icons. */
    icons_build(d);
    for (unsigned k=0;k<d->v3_link_count;k++) if (d->v3_links[k].desktop && desk_launch_valid(&d->v3_links[k].launch)) {
        char index[12]; snprintf(index,sizeof(index),"%u",k);
        icon_add(d,d->v3_links[k].launch.label,"",index,"application-x-executable",SH_ACT_WIN32_LINK,0);
    }
    icons_layout(d);
}
int desk_link_launch(struct desktop *d, unsigned index)
{
    if (index>=d->v3_link_count || !d->v3_link_read || !d->v3_launch) return -ENOENT;
    struct desk_launch_record *record=malloc(sizeof(*record)); if (!record) return -ENOMEM;
    int result=d->v3_link_read(d,d->v3_links[index].source,record);
    if (!result && !desk_launch_valid(record)) result=-EINVAL;
    if (!result) result=d->v3_launch(d,record);
    free(record); return result;
}
