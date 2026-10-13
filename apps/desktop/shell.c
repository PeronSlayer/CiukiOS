/* SPDX-License-Identifier: MIT
 * CiukiOS desktop shell: Start menu, taskbar buttons and tray, desktop
 * icons, built-in windows (About, Run, Shut Down, messages), Alt+Tab and the
 * other window-manager shortcuts. Drawing goes through the libdeskui
 * primitives and the theme painter (theme.h) on the compositor's frame:
 * colours, bevels and the anti-aliased Inter faces follow the current
 * theme (Platinum, Ciuki Blue, Classic 98). No file or kernel access here
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
static void popup_face(struct du_canvas *c, struct desk_box b)
{
    /* Platinum: light face, thin outline and a soft shadow (libdeskui menus);
     * Classic: the raised 3-D panel. */
    if (desk_theme->control_style == DESK_CONTROLS_PLATINUM) {
        if (desk_theme->shadow_size) du_shadow(c, R(b), SH_SHADOW);
        du_fill(c, R(b), desk_mix(DU_HILITE, DU_FACE, 110));
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
struct program_info { const char *name, *label, *icon, *category; };
/* Menu order of the known programs; anything else in /Programs is "developer".
 * Icons: the Tango names of icon_map.c program:* (Files and Task Manager
 * show the same icon for the same program; test_icons.py compares them). */
static const struct program_info programs[] = {
    {"ciuknote", "CiukNote", "accessories-text-editor", "programs"},
    {"files", "Files", "system-file-manager", "programs"},
    {"tasks", "Task Manager", "utilities-system-monitor", "programs"},
    {"dosbox", "DOS Box", "utilities-terminal", "programs"},
    {"demo", "Ciuki Demo", "preferences-desktop-peripherals", "programs"},   /* the keyboard/mouse input demo */
    {"lua", "Lua", "text-x-script", "programs"},
    {"control", "Control Panel", "preferences-desktop", "programs"},
    {"music", "Music Player", "audio-x-generic", "programs"},
    {"viewer", "Image Viewer", "image-x-generic", "programs"},
    {"paint", "CiukPaint", "applications-graphics", "programs"},
    {"terminal", "Terminal", "utilities-terminal", "programs"},
    {"calc", "Calculator", "accessories-calculator", "accessories"},
    {"charmap", "Character Map", "accessories-character-map", "accessories"},
    {"clock", "Clock", "appointment-new", "accessories"},
    {"help", "CiukiOS Help", "help-browser", "accessories"},
    {"mixer", "Volume Control", "audio-volume-high", "accessories"},
    {"regedit", "Registry Editor", "preferences-system", "accessories"},
    {"viewer", "Image Viewer", "image-x-generic", "accessories"},
    {"hello", "Hello", "face-smile", "developer"},
    {"ciukide", "CiukIDE", "applications-development", "developer"},
    {"tcc", "Tiny C Compiler", "text-x-script", "developer"},
    {"libc_smoke", "libc smoke test", "applications-system", "developer"},
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
    if (c->internal == DESK_INTERNAL_SHUTDOWN || c->internal == DESK_INTERNAL_POWER) return "system-shutdown";
    if (c->internal) return d->sh.message_kind ? "dialog-error" : "dialog-information";
    if (c->program == DESK_APP_TERMINAL) return "utilities-terminal";
    if (c->program == DESK_APP_FILES) return "system-file-manager";
    if (c->program == DESK_APP_NOTE) return "accessories-text-editor";
    if (c->program == DESK_APP_CONTROL) return "preferences-desktop";
    if (c->taskman) return "utilities-system-monitor";
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
static const char *const panel_names[] = {"", "display", "mouse", "keyboard", "sounds", "datetime", "system"};
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
        int developer = 0, accessories = 0;
        for (size_t k = 0; k < sizeof(programs) / sizeof(programs[0]); k++) {
            int at = program_index(d, programs[k].name);
            if (at < 0) continue;
            if (!strcmp(programs[k].category, "accessories")) { accessories = 1; continue; }
            if (strcmp(programs[k].category, "programs")) { developer = 1; continue; }
            item(m, programs[k].label, programs[k].icon, SH_ACT_LAUNCH, at, 0);
        }
        for (unsigned k = 0; k < d->program_count; k++) if (!program_find(d->programs[k].name)) developer = 1;
        if (!m->count) item(m, "(No programs)", NULL, SH_ACT_NONE, 0, SH_ITEM_DISABLED);
        if (accessories) item(m, "Accessories", "applications-accessories", SH_ACT_SUBMENU, SH_MENU_ACCESSORIES, 0);
        /* The bundled shareware games run in a DOS Box window (catalog.h desk_games). */
        if (program_index(d, "dosbox") >= 0) item(m, "Games", "applications-games", SH_ACT_SUBMENU, SH_MENU_GAMES, 0);
        if (developer) { separator(m); item(m, "Developer", "applications-development", SH_ACT_SUBMENU, SH_MENU_DEVELOPER, 0); }
        break;
    }
    case SH_MENU_ACCESSORIES:
        for (size_t k = 0; k < sizeof(programs) / sizeof(programs[0]); k++) {
            int at = program_index(d, programs[k].name);
            if (at >= 0 && !strcmp(programs[k].category, "accessories")) item(m, programs[k].label, programs[k].icon, SH_ACT_LAUNCH, at, 0);
        }
        break;
    case SH_MENU_GAMES:
        for (unsigned k = 0; k < DESK_GAME_COUNT; k++) item(m, desk_games[k].label, desk_games[k].tango, SH_ACT_GAME, (int)k, 0);
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
        item(m, "Display", "preferences-desktop-wallpaper", SH_ACT_CONTROL, 1, off);
        item(m, "Mouse", "input-mouse", SH_ACT_CONTROL, 2, off);
        item(m, "Keyboard", "input-keyboard", SH_ACT_CONTROL, 3, off);
        item(m, "Sounds", "audio-card", SH_ACT_CONTROL, 4, off);
        item(m, "Date & Time", "office-calendar", SH_ACT_CONTROL, 5, off);
        item(m, "System", "ciuki-computer", SH_ACT_CONTROL, 6, off);
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
        item(m,"Text Document","text-x-generic",SH_ACT_FILE,DF_TEXT,0);
        item(m,"Bitmap Image","applications-graphics",SH_ACT_FILE,DF_BITMAP,0);
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
        item(m,"New", "document-new", SH_ACT_SUBMENU, SH_MENU_NEW, d->sh.folder ? 0 : SH_ITEM_DISABLED);
        item(m,"Paste", "edit-paste", SH_ACT_FILE, DF_PASTE, d->sh.folder ? 0 : SH_ITEM_DISABLED);
        separator(m);
        item(m, "Arrange Icons", NULL, SH_ACT_ARRANGE, 0, 0);
        item(m, "Refresh", "view-refresh", SH_ACT_REFRESH, 0, 0);
        separator(m);
        item(m, "Display Settings...", "preferences-desktop-wallpaper", SH_ACT_CONTROL, 1, program_index(d, "control") < 0 ? SH_ITEM_DISABLED : 0);
        item(m, "Properties", "document-properties", SH_ACT_CONTROL, 1, program_index(d, "control") < 0 ? SH_ITEM_DISABLED : 0);
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
        item(m, "Restore", NULL, SH_ACT_TASK_RESTORE, owner, minimized ? SH_ITEM_DEFAULT : SH_ITEM_DISABLED);
        item(m, "Minimize", NULL, SH_ACT_TASK_MINIMIZE, owner, minimized ? SH_ITEM_DISABLED : 0);
        separator(m);
        item(m, "Close    Alt+F4", "process-stop", SH_ACT_TASK_CLOSE, owner, minimized ? 0 : SH_ITEM_DEFAULT);
        break;
    }
    default: break;
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
        damage(d, desk_start_button(d));
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
        damage(d, desk_start_button(d));
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
/* Programs > Games: a DOS Box window titled after the game, typing its command. */
static void launch_game(struct desktop *d, int index)
{
    if (index < 0 || (unsigned)index >= DESK_GAME_COUNT) return;
    char name[48];
    snprintf(name, sizeof(name), "--name=%s", desk_games[index].title);
    const char *args[] = {name, desk_games[index].command};
    int r = desk_task_launch_args(d, "dosbox", args, 2);
    if (r < 0) {
        char text[160];
        snprintf(text, sizeof(text), r == -ENOENT ? "CiukiOS cannot find 'dosbox' in /Programs." :
                 "'%s' could not be started (error %d).", desk_games[index].title, -r);
        desk_shell_message(d, "Games", text, 1);
    }
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
        else { snprintf(argument, sizeof(argument), "--panel=%s", panel_names[it->arg]); launch_named(d, "control", argument); }
        break;
    case SH_ACT_TASKMAN: launch_named(d, "tasks", NULL); break;
    case SH_ACT_GAME: launch_game(d, it->arg); break;
    case SH_ACT_KEYS:
        desk_shell_message(d, "Keyboard Shortcuts",
            "Win or Ctrl+Esc   Open the Start menu\nAlt+Tab           Switch between windows\n"
            "Alt+F4            Close the active window\nEnter             Open the selected icon\n"
            "Arrows            Move in menus and icons\nEsc               Close a menu", 0);
        break;
    case SH_ACT_ARRANGE: {
        cfg_icons_arrange(&d->sh.cfg); d->sh.want_save=1;
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
    case SH_ACT_TASK_RESTORE: if (it->arg >= 0 && it->arg < DESK_CLIENTS && d->clients[it->arg].minimized) desk_minimize(d, it->arg); break;
    case SH_ACT_TASK_MINIMIZE: if (it->arg >= 0 && it->arg < DESK_CLIENTS && !d->clients[it->arg].minimized) desk_minimize(d, it->arg); break;
    case SH_ACT_TASK_CLOSE: desk_request_close(d, it->arg); break;
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
    popup_face(&c, m->box);
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
{
    uint64_t s = wall_seconds(d);
    unsigned h = (unsigned)(s / 3600 % 24), m = (unsigned)(s / 60 % 60), sec = (unsigned)(s % 60);
    if (d->sh.cfg.clock_24h) {
        if (d->sh.cfg.clock_seconds) snprintf(out, size, "%02u:%02u:%02u", h, m, sec);
        else snprintf(out, size, "%02u:%02u", h, m);
    } else {
        unsigned h12 = h % 12 ? h % 12 : 12;
        if (d->sh.cfg.clock_seconds) snprintf(out, size, "%u:%02u:%02u %s", h12, m, sec, h < 12 ? "AM" : "PM");
        else snprintf(out, size, "%u:%02u %s", h12, m, h < 12 ? "AM" : "PM");
    }
}
static void date_text(const struct desktop *d, char *out, size_t size, int long_form)
{
    int y, mo, day, wd;
    civil(wall_seconds(d), &y, &mo, &day, &wd);
    if (long_form) snprintf(out, size, "%s, %d %s %d", weekdays[wd], day, months[mo - 1], y);
    else snprintf(out, size, "%.3s %d %.3s %d", weekdays[wd], day, months[mo - 1], y);
}

/* ---- taskbar ----------------------------------------------------------- */
static struct desk_box tray_box(const struct desktop *d)
{ return (struct desk_box){(int32_t)d->width - SH_TRAY_W - 4, (int32_t)d->height - DESK_BAR + 4, SH_TRAY_W, DESK_BAR - 7}; }
static struct desk_box volume_icon_box(const struct desktop *d)
{ struct desk_box t = tray_box(d); return (struct desk_box){t.x + 24, t.y + (t.h - 16) / 2, 16, 16}; }
static struct desk_box network_icon_box(const struct desktop *d)
{ struct desk_box t = tray_box(d); return (struct desk_box){t.x + 4, t.y + (t.h - 16) / 2, 16, 16}; }
static struct desk_box clock_box(const struct desktop *d)
{ struct desk_box t = tray_box(d); return (struct desk_box){t.x + 44, t.y, t.w - 46, t.h}; }
static struct desk_box network_popup(const struct desktop *d)
{ struct desk_box t = tray_box(d); return (struct desk_box){t.x + t.w - 272, t.y - 4 - 112, 272, 112}; }
static void network_mac(const struct ciuki_net_state *n, char text[18])
{ snprintf(text, 18, "%02x:%02x:%02x:%02x:%02x:%02x", n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]); }
static void tooltip_text(const struct desktop *d, char *text, size_t size)
{
    if (d->sh.tooltip != 2) { date_text(d, text, size, 1); return; }
    const struct ciuki_net_state *n = &d->sh.network;
    if (!n->present) snprintf(text, size, "No network adapter");
    else if (n->link == CIUKI_NET_LINK_UP) {
        char mac[18]; network_mac(n, mac);
        snprintf(text, size, "Network: cable connected (%s %s)", n->driver, mac);
    } else snprintf(text, size, n->link == CIUKI_NET_LINK_DOWN ? "Network: no cable" : "Network: link unknown");
}
static struct desk_box volume_popup(const struct desktop *d)
{ struct desk_box t = tray_box(d); return (struct desk_box){t.x + t.w - 92, t.y - 4 - 168, 92, 168}; }
static struct desk_box volume_track(const struct desktop *d)
{ struct desk_box p = volume_popup(d); return (struct desk_box){p.x + 40, p.y + 30, 12, 100}; }
static struct desk_box volume_mute(const struct desktop *d)
{ struct desk_box p = volume_popup(d); return (struct desk_box){p.x + 10, p.y + p.h - 24, p.w - 20, 14}; }
static struct desk_box tooltip_box(const struct desktop *d)
{
    char text[96]; tooltip_text(d, text, sizeof(text));
    int32_t w = du_text_width(text) + 10;
    struct desk_box t = tray_box(d);
    int32_t x = t.x + t.w - w;
    if (x < 0) x = 0;
    if (w > (int32_t)d->width) w = (int32_t)d->width;
    return (struct desk_box){x, t.y - 4 - 18, w, 18};
}
void desk_shell_network_update(struct desktop *d, const struct ciuki_net_state *state)
{
    struct ciuki_net_state *old = &d->sh.network;
    int changed = old->present != state->present || old->link != state->link || old->state != state->state ||
                  memcmp(old->driver, state->driver, sizeof(old->driver)) ||
                  memcmp(old->mac, state->mac, sizeof(old->mac)) ||
                  memcmp(old->reason, state->reason, sizeof(old->reason));
    if (changed && d->sh.tooltip == 2) damage(d, tooltip_box(d));
    *old = *state;
    if (!changed) return;
    damage(d, tray_box(d));
    if (d->sh.network_open) damage(d, outer(network_popup(d)));
    if (d->sh.tooltip == 2) damage(d, tooltip_box(d));
}
int desk_task_count(const struct desktop *d)
{
    int n = 0;
    for (int i = 0; i < DESK_CLIENTS; i++) n += d->clients[i].window != 0;
    return n;
}
static void draw_taskbar(struct desktop *d, struct desk_box clip)
{
    const struct desk_theme *t = desk_theme;
    struct desk_box bar = desk_taskbar(d), b;
    if (!desk_intersect(bar, clip, &b)) return;
    struct du_canvas c = canvas(d, b);
    /* Task buttons from the left margin (the Ciuki button is in the top bar), tray at the right. */
    for (int i = 0; i < DESK_CLIENTS; i++) {
        struct desk_box button = desk_task_button(d, i), part;
        if (!desk_intersect(button, b, &part)) continue;
        struct du_canvas bc = canvas(d, part);
        struct du_rect r = {button.x + 1, button.y, button.w - 2, button.h};
        int active = d->focus == i;
        if (active) {
            /* The focused window's button is pressed in. */
            du_fill(&bc, r, t->task_active);
            du_hline(&bc, r.x, r.y, r.w - 1, t->shadow); du_vline(&bc, r.x, r.y, r.h - 1, t->shadow);
            du_hline(&bc, r.x, r.y + r.h - 1, r.w, t->hilite); du_vline(&bc, r.x + r.w - 1, r.y, r.h, t->hilite);
            du_hline(&bc, r.x + 1, r.y + 1, r.w - 2, t->dark == 0 ? t->dark : desk_mix(t->dark, t->shadow, 90));
        } else du_raised(&bc, r, 0, 0);
        if (button.w > 24) {
            const uint32_t *ic = desk_shell_icon(client_icon(d, i), 16);
            int dy = active ? 1 : 0;
            if (ic) du_icon_blit(&bc, button.x + 5 + dy, button.y + 5 + dy, ic, 16, d->clients[i].minimized ? DU_ICON_GHOST : DU_ICON_NORMAL);
            const char *title = d->clients[i].closing ? "Closing..." : d->clients[i].unresponsive ? "Not responding" : d->clients[i].title;
            struct du_canvas tc = bc;
            tc.clip = du_intersection(bc.clip, (struct du_rect){button.x + 24, button.y, button.w - 28, button.h});
            int font = active ? t->font_bold : t->font_ui;
            du_text_font(&tc, font, button.x + 25 + dy, button.y + 17 + dy - desk_font_cap(font), title,
                         active ? t->task_active_text : t->taskbar_text);
        }
    }
    struct desk_box tray = tray_box(d);
    du_edge(&c, R(tray), DU_SUNKEN_THIN);
    const char *net_icon = !d->sh.network.present ? "network-error" :
                          d->sh.network.link == CIUKI_NET_LINK_UP ? "network-wired" : "network-offline";
    struct desk_box ni = network_icon_box(d);
    du_icon_blit(&c, ni.x, ni.y, desk_shell_icon(net_icon, 16), 16,
                 d->sh.network.present ? DU_ICON_NORMAL : DU_ICON_DISABLED);
    const uint32_t *speaker = desk_shell_icon(d->sh.muted ? "audio-volume-muted" : "audio-volume-high", 16);
    struct desk_box vi = volume_icon_box(d);
    du_icon_blit(&c, vi.x, vi.y, speaker, 16, d->sh.mixer_state < 0 ? DU_ICON_DISABLED : DU_ICON_NORMAL);   /* muted: Tango's red cross */
    char clock[24];
    clock_text(d, clock, sizeof(clock));
    struct desk_box cb = clock_box(d);
    du_text(&c, cb.x + cb.w - du_text_width(clock) - 6, cb.y + (cb.h - text_h()) / 2, clock, t->taskbar_text);
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
    popup_face(&c, p);
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
    popup_face(&c, p);
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
    char text[96]; tooltip_text(d, text, sizeof(text));
    du_fill(&c, R(t), SH_TIP);
    du_outline(&c, R(t), SH_INK);
    du_text(&c, t.x + 5, t.y + (t.h - text_h()) / 2, text, SH_INK);
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
    if (ic->action == SH_ACT_LAUNCH) launch_named(d, ic->program, ic->argument[0] ? ic->argument : NULL);
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
            if (!d->clients[i].window) continue;
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
    popup_face(&c, s);
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
                s->background_rebuilds++; s->background_valid=1;
            } else if (s->background_selected!=s->icon_selected || s->background_outline!=outline) {
                int changed[2]={s->background_selected,s->icon_selected};
                for (int k=0;k<2;k++) if (changed[k]>=0 && changed[k]<s->icon_count) {
                    struct desk_box b;
                    if (!desk_intersect(s->icons[changed[k]].box,
                            (struct desk_box){0,0,(int)d->width,(int)d->height},&b)) continue;
                    for (int y=b.y;y<b.y+b.h;y++) memcpy(s->background+(size_t)y*d->width+b.x,
                        s->wall+(size_t)y*d->width+b.x,(size_t)b.w*4);
                    draw_icons(d,b);
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
/* The Ciuki (Start) button: the approved portrait (deterministic area
 * scaling of the exact asset) and "CiukiOS" on a rounded, softly bevelled
 * gradient face; pressed in while its menu is open, lit under the pointer.
 * Classic 98 draws the raised 3-D button of that theme. */
static void draw_start(struct desktop *d, struct du_canvas *c)
{
    const struct desk_theme *t = desk_theme;
    struct desk_box b = desk_start_button(d);
    struct desk_paint p = painter(c);
    int open = d->menu_open, hot = d->sh.start_hot && !open;
    uint32_t base = open ? t->start_open : t->start;
    if (t->control_style == DESK_CONTROLS_CLASSIC) desk_paint_raised(&p, b.x, b.y, b.w, b.h, open, 0);
    else {
        /* A soft gradient around the theme's start colour (the normal-boot
         * heuristic of scripts/test/run.py reads that colour), lighter on hover. */
        uint32_t top = open ? desk_mix(t->dark, base, 24) : desk_mix(t->hilite, base, hot ? 190 : 100);
        uint32_t bottom = open ? desk_mix(t->hilite, base, 24) : desk_mix(t->shadow, base, hot ? 30 : 22);
        desk_paint_round_rect(&p, b.x, b.y, b.w, b.h, SH_START_RADIUS, top, bottom,
                              open ? t->outline : desk_mix(t->outline, t->topbar, 170), 255);
        /* Soft bevel: a light line under the top edge, or a shade while pressed. */
        desk_paint_blend(&p, b.x + SH_START_RADIUS, b.y + 1, b.w - 2 * SH_START_RADIUS, 1, open ? t->dark : t->hilite, open ? 70 : 200);
        if (!open) desk_paint_blend(&p, b.x + SH_START_RADIUS, b.y + b.h - 2, b.w - 2 * SH_START_RADIUS, 1, t->shadow, 60);
    }
    int font = t->font_title, shift = open ? 1 : 0, tw = desk_text_measure(font, "CiukiOS");
    int x = b.x + (b.w - SH_START_MARK - 6 - tw) / 2 + shift, y = b.y + (b.h - SH_START_MARK) / 2 + shift;
    du_icon_blit(c, x, y, desk_shell_icon("ciuki", SH_START_MARK), SH_START_MARK, DU_ICON_NORMAL);
    if (t->text_aa && !open) desk_paint_text(&p, font, x + SH_START_MARK + 6, b.y + 17 + shift, "CiukiOS", desk_mix(t->hilite, base, 150));
    desk_paint_text(&p, font, x + SH_START_MARK + 6, b.y + 16 + shift, "CiukiOS", t->start_text);
}
void desk_shell_topbar(struct desktop *d, struct desk_box clip)
{
    /* The compositor fills the bar and its edges in the theme; the shell
     * adds the Ciuki button, the active window title or notice (no tagline:
     * owner request 2026-10-12) and the date. */
    const struct desk_theme *t = desk_theme;
    struct desk_box bar = {0, 0, (int32_t)d->width, DESK_BAR}, b;
    if (!desk_intersect(bar, clip, &b)) return;
    bind(d);
    struct du_canvas c = canvas(d, b);
    struct desk_paint p = painter(&c);
    draw_start(d, &c);
    struct desk_box start = desk_start_button(d);
    int x = start.x + start.w;
    const char *what = d->notice[0] ? d->notice : NULL;
    char line[96];
    if (!what && d->focus >= 0 && d->clients[d->focus].window) { snprintf(line, sizeof(line), "%s", d->clients[d->focus].title); what = line; }
    char date[48];
    date_text(d, date, sizeof(date), 1);
    int dw = desk_text_measure(t->font_ui, date), room = (int)d->width - dw - 24 - (x + 14);
    if (room > 0) {
        struct desk_paint tp = p;
        struct desk_box tb;
        if (what && desk_intersect((struct desk_box){x + 14, 0, room, DESK_BAR}, b, &tb)) {
            tp.cx = tb.x; tp.cy = tb.y; tp.cw = tb.w; tp.ch = tb.h;
            desk_paint_text(&tp, t->font_ui, x + 14, 20, what,
                            d->notice[0] ? t->topbar_accent : desk_mix(t->topbar_text, t->topbar, 170));
        }
    }
    desk_paint_text(&p, t->font_ui, (int)d->width - dw - 12, 20, date, t->topbar_text);
}
void desk_shell_overlay(struct desktop *d, struct desk_box clip)
{
    bind(d);
    draw_taskbar(d, clip);
    for (int l = 0; l < d->sh.levels; l++) draw_menu(d, &d->sh.menus[l], clip);
    if (d->sh.volume_open) draw_volume(d, clip);
    if (d->sh.network_open) draw_network(d, clip);
    if (d->sh.tooltip && !d->sh.volume_open && !d->sh.network_open && !d->sh.levels) draw_tooltip(d, clip);
    if (d->sh.switcher) draw_switcher(d, clip);
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
    case DESK_INTERNAL_ABOUT: *w = 440; *h = 300; break;
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
        unsigned lines = 1;
        for (const char *s = d->sh.message_text; *s; s++) lines += *s == '\n';
        int32_t widest = 0;
        char line[128]; size_t n = 0;
        for (const char *s = d->sh.message_text;; s++) {
            if (!*s || *s == '\n' || n + 1 >= sizeof(line)) {
                line[n] = 0; n = 0;
                if (du_text_width(line) > widest) widest = du_text_width(line);
                if (!*s) break;
                if (*s != '\n') line[n++] = *s;
            } else line[n++] = *s;
        }
        *w = (uint32_t)(widest + 88 > 300 ? widest + 88 : 300);
        *h = lines * (unsigned)line_h() + 70 > 120 ? lines * (unsigned)line_h() + 70 : 120;
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
    if (kind != DESK_INTERNAL_MESSAGE && kind != DESK_INTERNAL_POWER)
        desk_sound(d,kind == DESK_INTERNAL_SHUTDOWN ? SOUND_QUESTION : SOUND_WINDOW_OPEN);
    if (!++d->next_window_id) ++d->next_window_id;
    c->window_id = d->next_window_id;
    c->info.width = w; c->info.height = h;
    snprintf(c->title, sizeof(c->title), "%s", kind == DESK_INTERNAL_ABOUT ? "About CiukiOS" : kind == DESK_INTERNAL_RUN ? "Run" :
             kind == DESK_INTERNAL_SHUTDOWN ? "Shut Down CiukiOS" : kind == DESK_INTERNAL_POWER ? "Shutting down..." : d->sh.message_title);
    c->frame = desk_frame(w, h, 0, DESK_BAR);
    if (kind == DESK_INTERNAL_ABOUT) { d->about = i; d->sh.about_tab = 0; d->sh.want_reload |= 2; }
    if (kind == DESK_INTERNAL_RUN) { d->sh.run_length = 0; d->sh.run_text[0] = 0; }
    if (kind == DESK_INTERNAL_SHUTDOWN) d->sh.shutdown_choice = 0;
    int32_t x = ((int32_t)d->width - c->frame.w) / 2, y = DESK_BAR + 20;
    if (kind == DESK_INTERNAL_RUN) { x = 8; y = DESK_BAR + 8; }   /* under the Ciuki button and its menu */
    else if (kind != DESK_INTERNAL_ABOUT) y = ((int32_t)d->height - c->frame.h) / 2 - 20;
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
}
void desk_shell_message(struct desktop *d, const char *title, const char *text, int error)
{
    snprintf(d->sh.message_title, sizeof(d->sh.message_title), "%s", title);
    snprintf(d->sh.message_text, sizeof(d->sh.message_text), "%s", text);
    d->sh.message_kind = error;
    desk_sound(d,error ? SOUND_ERROR : SOUND_INFORMATION);
    if (!d->shell) return;
    desk_internal_open(d, DESK_INTERNAL_MESSAGE);
}
static struct desk_box button_box(struct desk_box content, int index, int count)
{
    /* Right-aligned 75x23 buttons along the bottom edge. */
    int32_t x = content.x + content.w - 10 - (count - index) * 81 + 6;
    return (struct desk_box){x, content.y + content.h - 33, 75, 23};
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
        static const char *const credits[] = {
            "CiukiOS: kernel, desktop and tools, MIT licence",
            "Ciuki portrait: approved project asset (not PD)",
            "Icons: Tango Icon Theme 0.8.90, Public Domain",
            "Fonts: Inter and Inconsolata, SIL OFL 1.1",
            "Images: stb_image 2.30, Sean Barrett (MIT/PD)",
            "C library: newlib (BSD-style, COPYING.NEWLIB)",
            "Lua 5.4.8: Lua.org, PUC-Rio (MIT licence)",
            "OPL3: DBOPL, DOSBox Team (GPL-2.0-or-later)",
            "Notices: /system/licenses"};
        for (size_t k = 0; k < sizeof(credits) / sizeof(credits[0]); k++) du_text(c, x, y + (int32_t)k * (step + 1), credits[k], SH_INK);
    }
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
        du_icon_blit(&c, content.x + 14, content.y + 14, desk_shell_icon(d->sh.message_kind ? "dialog-error" : "dialog-information", 32), 32, DU_ICON_NORMAL);
        multiline(&c, content.x + 60, content.y + 18, d->sh.message_text, SH_INK);
        push_button(&c, button_box(content, 0, 1), "OK", 1);
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
    case DESK_INTERNAL_ABOUT:
        for (int k = 0; k < 3; k++) if (desk_contains(about_tab(content, k), x, y)) { d->sh.about_tab = k; damage(d, desk_outer(cl)); d->sh.want_reload |= 2; return 1; }
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
        if (desk_contains(button_box(content, 0, 1), x, y)) desk_request_close(d, index);
        return 1;
    }
}

/* ---- input ------------------------------------------------------------- */
static struct desk_box start_button(const struct desktop *d) { return desk_start_button(d); }
static void volume_set_from(struct desktop *d, int32_t y)
{
    struct desk_box t = volume_track(d);
    int v = (t.y + t.h - y) * 100 / t.h;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v != d->sh.volume) { d->sh.volume = v; damage(d, outer(volume_popup(d))); }
}
static void tooltip_hide(struct desktop *d)
{
    if (d->sh.tooltip) { damage(d, tooltip_box(d)); d->sh.tooltip = 0; }
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
        if (button == CIUKI_BUTTON_LEFT && d->sh.volume_drag) { d->sh.volume_drag = 0; d->sh.want_mixer = 1; return 1; }
        return 0;
    }
    tooltip_hide(d);
    d->sh.hover_since = d->now_ns;
    if (d->sh.switcher) switcher_finish(d, 0);
    if (d->sh.levels) {
        for (int l = d->sh.levels - 1; l >= 0; l--) {
            int k = menu_hit(&d->sh.menus[l], x, y);
            if (k == -1) continue;
            if (k >= 0 && button != CIUKI_BUTTON_MIDDLE) activate(d, l, k);
            return 1;
        }
        int start = d->sh.menus[0].kind == SH_MENU_START;
        desk_menu_close(d);
        if (start && desk_contains(start_button(d), x, y)) return 1;
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
    struct desk_box bar = desk_taskbar(d);
    if (desk_contains(start_button(d), x, y)) {
        if (button == CIUKI_BUTTON_LEFT) desk_menu_open(d, SH_MENU_START, 0, 0);
        return 1;
    }
    if (desk_contains(bar, x, y)) {
        if (desk_contains(network_icon_box(d), x, y) && button == CIUKI_BUTTON_LEFT) {
            d->sh.network_open = 1; d->sh.want_reload |= 16;
            damage(d, outer(network_popup(d)));
            return 1;
        }
        if (desk_contains(volume_icon_box(d), x, y) && button == CIUKI_BUTTON_LEFT) {
            d->sh.volume_open = 1; d->sh.want_reload |= 4;
            damage(d, outer(volume_popup(d)));
            return 1;
        }
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
void desk_shell_motion(struct desktop *d)
{
    if (!d->shell) return;
    int32_t x = d->mouse_x, y = d->mouse_y;
    if (d->sh.icon_drag>=0 && (abs(x-d->sh.drag_start_x)>4 || abs(y-d->sh.drag_start_y)>4)) d->sh.icon_dragged=1;
    if (d->sh.volume_drag) volume_set_from(d, y);
    for (int l = d->sh.levels - 1; l >= 0; l--) {
        struct sh_menu *m = &d->sh.menus[l];
        int k = menu_hit(m, x, y);
        if (k == -1) continue;
        int hot = k >= 0 ? k : -1;
        if (hot != m->hot) { m->hot = hot; damage(d, m->box); }
        if (k >= 0 && m->items[k].action == SH_ACT_SUBMENU) submenu(d, l, k);
        else if (k >= 0 && l + 1 < d->sh.levels) {
            for (int j = l + 1; j < d->sh.levels; j++) damage(d, outer(d->sh.menus[j].box));
            d->sh.levels = l + 1;
        }
        break;
    }
    int over_start = desk_contains(start_button(d), x, y);
    if (over_start != d->sh.start_hot) { d->sh.start_hot = over_start; damage(d, start_button(d)); }
    int over_clock = desk_contains(clock_box(d), x, y);
    int over_network = desk_contains(network_icon_box(d), x, y);
    if (d->sh.tooltip && d->sh.tooltip != (over_network ? 2 : over_clock ? 1 : 0)) {
        tooltip_hide(d); d->sh.hover_since = 0;
    }
    if (!over_clock && !over_network) { tooltip_hide(d); d->sh.hover_since = 0; }
    else if (!d->sh.hover_since) d->sh.hover_since = d->now_ns ? d->now_ns : 1;
}
void desk_shell_tick(struct desktop *d)
{
    if (!d->shell) return;
    if (!d->sh.network_poll_ns || (d->now_ns >= d->sh.network_poll_ns &&
        d->now_ns - d->sh.network_poll_ns >= 2000000000ull)) {
        d->sh.network_poll_ns = d->now_ns ? d->now_ns : 1;
        d->sh.want_reload |= 16;
    }
    int over_network = desk_contains(network_icon_box(d), d->mouse_x, d->mouse_y);
    if (!d->sh.tooltip && d->sh.hover_since && !d->sh.levels && !d->sh.volume_open && !d->sh.network_open &&
        (over_network || desk_contains(clock_box(d), d->mouse_x, d->mouse_y)) && d->now_ns - d->sh.hover_since >= 600000000ull) {
        d->sh.tooltip = over_network ? 2 : 1;
        damage(d, tooltip_box(d));
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
        if (d->sh.tooltip) damage(d,tooltip_box(d));
    }
    if (d->sh.cfg.clock_seconds || d->realtime_second/60!=previous/60) damage(d,clock_box(d));
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
    switch (code) {
    case DU_KEY_UP: case DU_KEY_DOWN:
        m->hot = menu_step(m, m->hot < 0 ? (code == DU_KEY_UP ? 0 : m->count - 1) : m->hot, code == DU_KEY_UP ? -1 : 1);
        damage(d, m->box); return 1;
    case DU_KEY_RIGHT:
        if (m->hot >= 0 && m->items[m->hot].action == SH_ACT_SUBMENU) {
            submenu(d, l, m->hot);
            struct sh_menu *s = &d->sh.menus[d->sh.levels - 1];
            if (s != m) { s->hot = menu_step(s, s->count - 1, 1); damage(d, s->box); }
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
static unsigned keymap_mods(const struct desktop *d)
{
    return (d->sh.modifiers & DU_SHIFT ? CIUKI_KEYMAP_SHIFT : 0) | (d->sh.alt_code == 0x138 ? CIUKI_KEYMAP_ALTGR : 0);
}
static int internal_key(struct desktop *d, int index, int code, int value)
{
    struct desk_client *c = &d->clients[index];
    if (value == 0) return 1;
    switch (c->internal) {
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
        else if (code == DU_KEY_TAB || code == DU_KEY_RIGHT || code == DU_KEY_LEFT) {
            d->sh.about_tab = (d->sh.about_tab + (code == DU_KEY_LEFT || (d->sh.modifiers & DU_SHIFT) ? 2 : 1)) % 3;
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
        if (code == DU_KEY_ESC || code == DU_KEY_ENTER || code == DU_KEY_SPACE) desk_request_close(d, index);
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
int desk_shell_key(struct desktop *d, int code, int value)
{
    if (!d->shell) return 0;
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
    if (d->sh.modifiers & DU_ALT) {
        if (code == DU_KEY_TAB) {
            if (value) { if (!d->sh.switcher) switcher_start(d, (d->sh.modifiers & DU_SHIFT) != 0); else switcher_step(d, (d->sh.modifiers & DU_SHIFT) != 0); }
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
        if (code == DU_KEY_ESC && d->sh.switcher) { if (value) switcher_finish(d, 0); d->sh.alt_used = 1; return 1; }
        if (value && !d->sh.alt_routed) { desk_route_key(d, d->sh.alt_code, 1); d->sh.alt_routed = 1; }
        if (!value && d->sh.alt_used && (code == DU_KEY_TAB || code == DU_KEY_F4)) return 1;
    }
    if (!value && (code == DU_KEY_TAB || code == DU_KEY_F4) && d->sh.alt_used) return 1;
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
    if (d->sh.network_open && code == DU_KEY_ESC) { if (value) { d->sh.network_open = 0; damage(d, outer(network_popup(d))); } return 1; }
    if (d->sh.volume_open && code == DU_KEY_ESC) { if (value) { d->sh.volume_open = 0; damage(d, outer(volume_popup(d))); } return 1; }
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
void desk_shell_reset(struct desktop *d)
{
    bind(d);
    cfg_defaults(&d->sh.cfg);
    d->sh.icon_selected = d->sh.click_icon = d->sh.icon_drag = -1;
    d->sh.volume = 50;
}
void desk_shell_apply(struct desktop *d)
{
    bind(d);
    if (!d->sh.icon_count) icons_build(d);
    icons_layout(d);
    d->sh.wall_valid = 0;
    /* The theme named by desktop.cfg (default Platinum), at once. */
    if (desk_theme_use(d->sh.cfg.theme)) desk_theme_use("Platinum");
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
    int r = desk_wallpaper_render(d->sh.wall, d->width, d->height, DESK_BAR, DESK_BAR, d->sh.cfg.wallpaper,
                                  d->sh.cfg.wallpaper_mode, desktop_colour(d), &d->sh.image, d->portrait);
    d->sh.wall_valid = 1;
    d->sh.background_valid=0;
    d->sh.image_failed = r < 0;
    damage_all(d);
    return r;
}
