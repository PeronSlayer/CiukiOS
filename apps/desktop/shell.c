/* SPDX-License-Identifier: MIT
 * CiukiOS desktop shell: Start menu, taskbar buttons and tray, desktop
 * icons, built-in windows (About, Run, Shut Down, messages), Alt+Tab and the
 * other window-manager shortcuts. Drawing goes through the libdeskui
 * primitives on the compositor's frame, so the toolkit's theme applies.
 * No file or kernel access here (shell.h). Integer-only. */
#include "desktop.h"
#include "../libdeskui/deskui.h"
#include "shell_icons.h"
#include "keymap.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- theme hooks ------------------------------------------------------- */
/* Every shell colour is a toolkit colour; the few identity colours are the
 * compositor's own (cream caption, top bar). Lead note: when libdeskui gains
 * du_theme_* (wt/ui-look), desk_theme_apply() is the one place to call it. */
#define SH_FACE      DU_FACE
#define SH_INK       DU_INK
#define SH_HOT       DU_SELECT
#define SH_HOT_INK   DU_SELECT_INK
#define SH_WINDOW    DU_WINDOW
#define SH_CREAM     0xf6dfb6u      /* the compositor's "CiukiOS" caption */
#define SH_TIP       0xffffe1u      /* tooltip paper */
__attribute__((weak)) void desk_theme_apply(const char *name) { (void)name; }

static struct du_canvas canvas(struct desktop *d, struct desk_box clip)
{
    struct du_canvas c = {d->pixels, (int)d->width, (int)d->height, {clip.x, clip.y, clip.w, clip.h}};
    return c;
}
static struct du_rect R(struct desk_box b) { return (struct du_rect){b.x, b.y, b.w, b.h}; }
static void damage(struct desktop *d, struct desk_box b) { desk_damage_add(&d->damage, b, d->width, d->height); }
static void damage_all(struct desktop *d)
{ damage(d, (struct desk_box){0, 0, (int32_t)d->width, (int32_t)d->height}); }
static void bold(struct du_canvas *c, int x, int y, const char *s, uint32_t colour)
{ du_text(c, x, y, s, colour); du_text(c, x + 1, y, s, colour); }
static int text_h(void) { return DU_GLYPH_H; }

/* ---- icons ------------------------------------------------------------- */
/* Tango 0.8.90 names (the icon family of AGENTS.md). Until du_icon_blit(name,
 * size) lands (wt/icons), each name maps to the sprites converted today;
 * smaller sizes are area-averaged from the next larger sprite and the
 * identity icons add the approved Ciuki portrait (deterministic scaling). */
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
    const struct icon_map *m = icon_lookup(!strcmp(name, "ciuki-computer") ? "computer" : name);
    if (!m && !ciuki) m = icon_lookup("application-x-executable");
    if (m && !ciuki) {
        if (size == 16 && m->s16) return m->s16;
        if (size == 22 && m->s22) return m->s22;
        if (size == 32 && m->s32) return m->s32;
    }
    if (icon_cache_count == ICON_CACHE) icon_cache_count = 0;   /* bounded; callers redraw each frame */
    uint32_t *dst = icon_cache[icon_cache_count].pixels;
    memset(dst, 0, sizeof(icon_cache[0].pixels));
    if (m) {
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
/* Menu order of the known programs; anything else in /bin is "developer". */
static const struct program_info programs[] = {
    {"ciuknote", "CiukNote", "accessories-text-editor", "programs"},
    {"files", "Files", "system-file-manager", "programs"},
    {"tasks", "Task Manager", "utilities-system-monitor", "programs"},
    {"dosbox", "DOS Box", "utilities-terminal", "programs"},
    {"demo", "Ciuki Demo", "applications-games", "programs"},
    {"lua", "Lua", "text-x-script", "programs"},
    {"control", "Control Panel", "preferences-desktop", "programs"},
    {"hello", "Hello", "application-x-executable", "developer"},
    {"libc_smoke", "libc smoke test", "application-x-executable", "developer"},
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
    if (c->internal == DESK_INTERNAL_RUN) return "application-x-executable";
    if (c->internal == DESK_INTERNAL_SHUTDOWN) return "system-shutdown";
    if (c->internal) return "dialog-information";
    if (c->program == DESK_APP_FILES) return "system-file-manager";
    if (c->program == DESK_APP_NOTE) return "accessories-text-editor";
    if (c->program == DESK_APP_CONTROL) return "preferences-desktop";
    if (c->taskman) return "utilities-system-monitor";
    if (!strncmp(c->title, "DOS", 3)) return "utilities-terminal";
    if (strstr(c->title, "Demo") || strstr(c->title, "demo")) return "applications-games";
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
static void build(struct desktop *d, struct sh_menu *m, int kind)
{
    memset(m, 0, sizeof(*m));
    m->kind = kind; m->hot = -1;
    m->row_h = kind == SH_MENU_START ? SH_START_ROW : SH_SUB_ROW;
    m->icon = kind == SH_MENU_START ? 22 : 16;
    switch (kind) {
    case SH_MENU_START:
        item(m, "Programs", "applications-other", SH_ACT_SUBMENU, SH_MENU_PROGRAMS, 0);
        item(m, "Documents", "document-open", SH_ACT_SUBMENU, SH_MENU_DOCUMENTS, 0);
        item(m, "Settings", "preferences-desktop", SH_ACT_SUBMENU, SH_MENU_SETTINGS, 0);
        item(m, "Help", "help-browser", SH_ACT_SUBMENU, SH_MENU_HELP, 0);
        item(m, "Run...", "application-x-executable", SH_ACT_RUN, 0, 0);
        separator(m);
        item(m, "Shut Down...", "system-shutdown", SH_ACT_SHUTDOWN, 0, 0);
        break;
    case SH_MENU_PROGRAMS: {
        int developer = 0;
        for (size_t k = 0; k < sizeof(programs) / sizeof(programs[0]); k++) {
            int at = program_index(d, programs[k].name);
            if (at < 0) continue;
            if (strcmp(programs[k].category, "programs")) { developer = 1; continue; }
            item(m, programs[k].label, programs[k].icon, SH_ACT_LAUNCH, at, 0);
        }
        for (unsigned k = 0; k < d->program_count; k++) if (!program_find(d->programs[k].name)) developer = 1;
        if (!m->count) item(m, "(No programs)", NULL, SH_ACT_NONE, 0, SH_ITEM_DISABLED);
        if (developer) { separator(m); item(m, "Developer", "applications-development", SH_ACT_SUBMENU, SH_MENU_DEVELOPER, 0); }
        break;
    }
    case SH_MENU_DEVELOPER:
        for (unsigned k = 0; k < d->program_count && m->count < SH_ITEMS; k++) {
            const struct program_info *p = program_find(d->programs[k].name);
            if (p && !strcmp(p->category, "programs")) continue;
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
    case SH_MENU_DESKTOP:
        item(m, "Arrange Icons", NULL, SH_ACT_ARRANGE, 0, 0);
        item(m, "Refresh", "view-refresh", SH_ACT_REFRESH, 0, 0);
        separator(m);
        item(m, "Display Settings...", "preferences-desktop-wallpaper", SH_ACT_CONTROL, 1, program_index(d, "control") < 0 ? SH_ITEM_DISABLED : 0);
        item(m, "Properties", "document-properties", SH_ACT_CONTROL, 1, program_index(d, "control") < 0 ? SH_ITEM_DISABLED : 0);
        break;
    case SH_MENU_ICON:
        item(m, "Open", NULL, SH_ACT_ICON_OPEN, d->sh.menu_owner, SH_ITEM_DEFAULT);
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
    for (int l = 0; l < d->sh.levels; l++) damage(d, d->sh.menus[l].box);
}
void desk_menu_open(struct desktop *d, int kind, int32_t x, int32_t y)
{
    menus_damage(d);
    d->sh.levels = 1;
    build(d, &d->sh.menus[0], kind);
    if (kind == SH_MENU_START) {
        layout(d, &d->sh.menus[0], 2, (int32_t)d->height - DESK_BAR, 1);
        if (!d->menu_open) { d->menu_focus = d->focus; desk_focus(d, -1); }
        d->menu_open = 1;
        if (d->shell_test && d->shell_stage == 0) d->shell_stage = 1;
        damage(d, desk_taskbar(d));
    } else layout(d, &d->sh.menus[0], x, y, 0);
    damage(d, d->sh.menus[0].box);
}
void desk_menu_close(struct desktop *d)
{
    if (!d->sh.levels) return;
    menus_damage(d);
    int start = d->sh.menus[0].kind == SH_MENU_START;
    d->sh.levels = 0;
    if (start) {
        d->menu_open = 0;
        desk_focus(d, d->menu_focus);
        damage(d, desk_taskbar(d));
    }
}
static void submenu(struct desktop *d, int level, int index)
{
    /* Opens (or keeps) the submenu of menus[level].items[index]. */
    struct sh_menu *m = &d->sh.menus[level];
    const struct sh_item *it = &m->items[index];
    if (level + 1 < d->sh.levels && d->sh.menus[level + 1].kind == it->arg) return;
    for (int l = level + 1; l < d->sh.levels; l++) damage(d, d->sh.menus[l].box);
    d->sh.levels = level + 1;
    if (level + 1 >= SH_LEVELS) return;
    struct sh_menu *s = &d->sh.menus[level + 1];
    build(d, s, it->arg);
    int32_t y = item_y(m, index) - 3;
    layout(d, s, m->box.x + m->box.w - 3, y, 0);
    if (s->box.x < m->box.x + m->box.w - 3) layout(d, s, m->box.x - s->box.w + 3, y, 0);
    d->sh.levels = level + 2;
    damage(d, s->box);
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
        snprintf(text, sizeof(text), r == -ENOENT ? "CiukiOS cannot find '%s' in /bin." :
                 "'%s' could not be started (error %d).", name, -r);
        desk_shell_message(d, "Programs", text, 1);
    }
}
static void act(struct desktop *d, const struct sh_item *it)
{
    char argument[24];
    switch (it->action) {
    case SH_ACT_LAUNCH: d->action = it->arg; break;
    case SH_ACT_ABOUT: desk_about(d); break;
    case SH_ACT_RUN: desk_internal_open(d, DESK_INTERNAL_RUN); break;
    case SH_ACT_SHUTDOWN: desk_internal_open(d, DESK_INTERNAL_SHUTDOWN); break;
    case SH_ACT_DOCUMENT:
        if (it->arg >= 0 && (unsigned)it->arg < d->sh.cfg.recent_count) launch_named(d, "ciuknote", d->sh.cfg.recent[it->arg]);
        break;
    case SH_ACT_CONTROL:
        if (it->arg <= 0) launch_named(d, "control", NULL);
        else { snprintf(argument, sizeof(argument), "--panel=%s", panel_names[it->arg]); launch_named(d, "control", argument); }
        break;
    case SH_ACT_TASKMAN: launch_named(d, "tasks", NULL); break;
    case SH_ACT_KEYS:
        desk_shell_message(d, "Keyboard Shortcuts",
            "Win or Ctrl+Esc   Open the Start menu\nAlt+Tab           Switch between windows\n"
            "Alt+F4            Close the active window\nEnter             Open the selected icon\n"
            "Arrows            Move in menus and icons\nEsc               Close a menu", 0);
        break;
    case SH_ACT_ARRANGE: {
        /* Sort by label, then the grid places them in order. */
        for (int a = 1; a < d->sh.icon_count; a++) for (int b = a; b > 0 && strcmp(d->sh.icons[b - 1].label, d->sh.icons[b].label) > 0; b--) {
            struct sh_icon t = d->sh.icons[b]; d->sh.icons[b] = d->sh.icons[b - 1]; d->sh.icons[b - 1] = t;
        }
        d->sh.icon_selected = -1;
        desk_shell_apply(d);
        break;
    }
    case SH_ACT_REFRESH: d->sh.want_reload = 1; break;
    case SH_ACT_ICON_OPEN: icon_open(d, it->arg); break;
    case SH_ACT_ICON_PROPERTIES:
        if (it->arg >= 0 && it->arg < d->sh.icon_count) {
            const struct sh_icon *ic = &d->sh.icons[it->arg];
            char text[200];
            if (ic->action == SH_ACT_LAUNCH)
                snprintf(text, sizeof(text), "%s\n\nType: Shortcut\nTarget: /bin/%s%s%s", ic->label, ic->program,
                         ic->argument[0] ? " " : "", ic->argument);
            else snprintf(text, sizeof(text), "%s\n\nType: System folder\nThe Trash is not available yet:\nCiukiOS deletes files permanently.", ic->label);
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
    if (it.action == SH_ACT_SUBMENU) { submenu(d, level, index); return; }
    desk_menu_close(d);
    act(d, &it);
}
static void draw_banner(struct du_canvas *c, struct desk_box b)
{
    /* Vertical gradient from the toolkit's selection colour to its shadow,
     * the approved portrait at the foot and "CiukiOS" reading upwards. */
    for (int32_t y = 0; y < b.h; y++) {
        uint32_t t = (uint32_t)(y * 256 / (b.h > 1 ? b.h : 1)), out = 0;
        for (unsigned s = 0; s < 24; s += 8)
            out |= ((((DU_SHADOW >> s) & 255) * (256 - t) + ((SH_HOT >> s) & 255) * t) >> 8) << s;
        du_fill(c, (struct du_rect){b.x, b.y + y, b.w, 1}, out);
    }
    int32_t px = b.x + (b.w - 20) / 2, py = b.y + b.h - 24;
    const uint32_t *badge = desk_shell_icon("ciuki", 20);
    du_icon_blit(c, px, py, badge, 20, DU_ICON_NORMAL);
    /* Rotated glyphs: column col of the glyph becomes row (4-col) going up. */
    const char *word = "CiukiOS";
    int32_t x0 = b.x + (b.w - 14) / 2, y = py - 8;
    for (const char *s = word; *s; s++) {
        int bold_part = s < word + 5;
        for (unsigned row = 0; row < 7; row++) {
            uint8_t bits = desk_glyph_row((unsigned char)*s, row);
            for (unsigned col = 0; col < 5; col++) if (bits & (1u << (4 - col))) {
                int32_t gx = x0 + (int32_t)row * 2, gy = y - (int32_t)col * 2;
                du_fill(c, (struct du_rect){gx, gy - 1, 2, 2}, bold_part ? SH_CREAM : DU_HILITE);
            }
        }
        y -= 12;
    }
}
static void draw_menu(struct desktop *d, const struct sh_menu *m, struct desk_box clip)
{
    struct desk_box b;
    if (!desk_intersect(m->box, clip, &b)) return;
    struct du_canvas c = canvas(d, b);
    du_fill(&c, R(m->box), SH_FACE);
    du_edge(&c, R(m->box), DU_RAISED);
    int32_t banner = m->kind == SH_MENU_START ? SH_BANNER : 0;
    if (banner) draw_banner(&c, (struct desk_box){m->box.x + 3, m->box.y + 3, banner, m->box.h - 6});
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
        if (hot) du_fill(&c, (struct du_rect){x0, y, w, h}, SH_HOT);
        const uint32_t *ic = desk_shell_icon(it->icon, m->icon);
        if (ic) du_icon_blit(&c, x0 + 6, y + (h - m->icon) / 2, ic, m->icon, it->flags & SH_ITEM_DISABLED ? DU_ICON_DISABLED : DU_ICON_NORMAL);
        int32_t tx = x0 + 6 + m->icon + 10, ty = y + (h - text_h()) / 2;
        if (it->flags & SH_ITEM_DISABLED) du_text_disabled(&c, tx, ty, it->label);
        else if (it->flags & SH_ITEM_DEFAULT) bold(&c, tx, ty, it->label, hot ? SH_HOT_INK : SH_INK);
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
{ struct desk_box t = tray_box(d); return (struct desk_box){t.x + 4, t.y + (t.h - 16) / 2, 16, 16}; }
static struct desk_box clock_box(const struct desktop *d)
{ struct desk_box t = tray_box(d); return (struct desk_box){t.x + 24, t.y, t.w - 26, t.h}; }
static struct desk_box volume_popup(const struct desktop *d)
{ struct desk_box t = tray_box(d); return (struct desk_box){t.x + t.w - 92, t.y - 4 - 168, 92, 168}; }
static struct desk_box volume_track(const struct desktop *d)
{ struct desk_box p = volume_popup(d); return (struct desk_box){p.x + 40, p.y + 30, 12, 100}; }
static struct desk_box volume_mute(const struct desktop *d)
{ struct desk_box p = volume_popup(d); return (struct desk_box){p.x + 10, p.y + p.h - 24, p.w - 20, 14}; }
static struct desk_box tooltip_box(const struct desktop *d)
{
    char text[64]; date_text(d, text, sizeof(text), 1);
    int32_t w = du_text_width(text) + 10;
    struct desk_box t = tray_box(d);
    int32_t x = t.x + t.w - w;
    if (x < 0) x = 0;
    return (struct desk_box){x, t.y - 4 - 18, w, 18};
}
int desk_task_count(const struct desktop *d)
{
    int n = 0;
    for (int i = 0; i < DESK_CLIENTS; i++) n += d->clients[i].window != 0;
    return n;
}
static void draw_taskbar(struct desktop *d, struct desk_box clip)
{
    struct desk_box bar = desk_taskbar(d), b;
    if (!desk_intersect(bar, clip, &b)) return;
    struct du_canvas c = canvas(d, b);
    du_hline(&c, bar.x, bar.y, bar.w, DU_LIGHT);
    du_hline(&c, bar.x, bar.y + 1, bar.w, DU_HILITE);
    /* Start button: the cream face and the portrait are drawn by the
     * compositor (desk_shell_draw); the bevel and the bold label here. */
    struct du_rect start = {4, bar.y + 3, 76, 26};
    du_edge(&c, start, d->menu_open ? DU_PRESSED : DU_BUTTON_EDGE);
    bold(&c, 34, bar.y + 12, "Ciuki", SH_INK);
    for (int i = 0; i < DESK_CLIENTS; i++) {
        struct desk_box button = desk_task_button(d, i), part;
        if (!desk_intersect(button, b, &part)) continue;
        struct du_canvas bc = canvas(d, part);
        struct du_rect r = {button.x + 1, button.y, button.w - 2, button.h};
        int active = d->focus == i;
        if (active) { du_checker(&bc, r, DU_HILITE, DU_LIGHT); du_edge(&bc, r, DU_PRESSED); }
        else { du_fill(&bc, r, SH_FACE); du_edge(&bc, r, DU_BUTTON_EDGE); }
        if (button.w > 24) {
            const uint32_t *ic = desk_shell_icon(client_icon(d, i), 16);
            int dy = active ? 1 : 0;
            if (ic) du_icon_blit(&bc, button.x + 5, button.y + 5 + dy, ic, 16, d->clients[i].minimized ? DU_ICON_GHOST : DU_ICON_NORMAL);
            const char *title = d->clients[i].closing ? "Closing..." : d->clients[i].unresponsive ? "Not responding" : d->clients[i].title;
            struct du_canvas tc = bc;
            tc.clip = du_intersection(bc.clip, (struct du_rect){button.x + 24, button.y, button.w - 30, button.h});
            if (active) bold(&tc, button.x + 25, button.y + 10, title, SH_INK);
            else du_text(&tc, button.x + 25, button.y + 9, title, SH_INK);
        }
    }
    struct desk_box tray = tray_box(d);
    du_fill(&c, R(tray), SH_FACE);
    du_edge(&c, R(tray), DU_SUNKEN_THIN);
    const uint32_t *speaker = desk_shell_icon(d->sh.muted ? "audio-volume-muted" : "audio-volume-high", 16);
    struct desk_box vi = volume_icon_box(d);
    du_icon_blit(&c, vi.x, vi.y, speaker, 16, d->sh.mixer_state < 0 ? DU_ICON_DISABLED : DU_ICON_NORMAL);
    if (d->sh.muted) {   /* a red bar across a muted speaker */
        for (int k = 0; k < 14; k++) du_fill(&c, (struct du_rect){vi.x + 1 + k, vi.y + 14 - k, 2, 2}, 0xc0201eu);
    }
    char clock[24];
    clock_text(d, clock, sizeof(clock));
    struct desk_box cb = clock_box(d);
    du_text(&c, cb.x + cb.w - du_text_width(clock) - 4, cb.y + (cb.h - text_h()) / 2, clock, SH_INK);
}
static void draw_volume(struct desktop *d, struct desk_box clip)
{
    struct desk_box p = volume_popup(d), b;
    if (!desk_intersect(p, clip, &b)) return;
    struct du_canvas c = canvas(d, b);
    du_fill(&c, R(p), SH_FACE);
    du_edge(&c, R(p), DU_RAISED);
    du_text(&c, p.x + (p.w - du_text_width("Volume")) / 2, p.y + 10, "Volume", SH_INK);
    struct desk_box t = volume_track(d);
    if (d->sh.mixer_state < 0) {
        du_text(&c, p.x + 8, p.y + 60, "No audio", SH_INK);
        du_text(&c, p.x + 8, p.y + 72, "device", SH_INK);
        return;
    }
    du_fill(&c, (struct du_rect){t.x + 4, t.y, 4, t.h}, SH_WINDOW);
    du_edge(&c, (struct du_rect){t.x + 3, t.y - 1, 6, t.h + 2}, DU_SUNKEN);
    for (int k = 0; k <= 10; k++) du_hline(&c, t.x - 10, t.y + k * t.h / 10, 6, DU_SHADOW);
    int32_t ty = t.y + t.h - d->sh.volume * t.h / 100;
    struct du_rect thumb = {t.x - 6, ty - 5, 24, 10};
    du_fill(&c, thumb, SH_FACE);
    du_edge(&c, thumb, d->sh.mixer_state == 2 ? DU_BUTTON_EDGE : DU_RAISED_THIN);
    char value[16]; snprintf(value, sizeof(value), "%d%%", d->sh.volume);
    du_text(&c, t.x + 22, t.y + t.h / 2 - 3, value, SH_INK);
    struct desk_box m = volume_mute(d);
    struct du_rect box = {m.x, m.y + 1, 12, 12};
    du_fill(&c, box, d->sh.mixer_state == 2 ? SH_WINDOW : SH_FACE);
    du_edge(&c, box, DU_SUNKEN);
    if (d->sh.muted) for (int k = 0; k < 3; k++) {
        du_fill(&c, (struct du_rect){box.x + 3 + k, box.y + 5 + k, 1, 2}, SH_INK);
        du_fill(&c, (struct du_rect){box.x + 6 + k, box.y + 7 - k, 1, 2}, SH_INK);
    }
    if (d->sh.mixer_state == 2) du_text(&c, m.x + 18, m.y + 3, "Mute", SH_INK);
    else du_text_disabled(&c, m.x + 18, m.y + 3, "Mute");
}
static void draw_tooltip(struct desktop *d, struct desk_box clip)
{
    struct desk_box t = tooltip_box(d), b;
    if (!desk_intersect(t, clip, &b)) return;
    struct du_canvas c = canvas(d, b);
    char text[64]; date_text(d, text, sizeof(text), 1);
    du_fill(&c, R(t), SH_TIP);
    du_outline(&c, R(t), SH_INK);
    du_text(&c, t.x + 5, t.y + 5, text, SH_INK);
}

/* ---- desktop icons ----------------------------------------------------- */
#define ICON_CELL_W 84
#define ICON_CELL_H 76
static void icon_add(struct desktop *d, const char *label, const char *program, const char *argument,
                     const char *icon, int action, int ciuki)
{
    if (d->sh.icon_count >= SH_ICONS) return;
    if (action == SH_ACT_LAUNCH && program_index(d, program) < 0) return;
    struct sh_icon *ic = &d->sh.icons[d->sh.icon_count++];
    memset(ic, 0, sizeof(*ic));
    snprintf(ic->label, sizeof(ic->label), "%s", label);
    snprintf(ic->program, sizeof(ic->program), "%s", program);
    snprintf(ic->argument, sizeof(ic->argument), "%s", argument);
    ic->icon = icon; ic->action = action; ic->ciuki = ciuki;
}
static void icons_layout(struct desktop *d)
{
    int32_t top = DESK_BAR + 10, bottom = (int32_t)d->height - DESK_BAR - 4;
    int32_t x = 8, y = top;
    for (int k = 0; k < d->sh.icon_count; k++) {
        if (y + ICON_CELL_H > bottom && y > top) { y = top; x += ICON_CELL_W; }
        d->sh.icons[k].box = (struct desk_box){x, y, ICON_CELL_W, ICON_CELL_H};
        y += ICON_CELL_H;
    }
}
static void icons_build(struct desktop *d)
{
    d->sh.icon_count = 0;
    icon_add(d, "Computer", "files", "/", "ciuki-computer", SH_ACT_LAUNCH, 1);
    icon_add(d, "Documents", "files", "/home", "user-home", SH_ACT_LAUNCH, 0);
    icon_add(d, "DOS Box", "dosbox", "", "utilities-terminal", SH_ACT_LAUNCH, 0);
    icon_add(d, "CiukNote", "ciuknote", "", "accessories-text-editor", SH_ACT_LAUNCH, 0);
    icon_add(d, "Control Panel", "control", "", "preferences-desktop", SH_ACT_LAUNCH, 0);
    icon_add(d, "Trash", "", "", "user-trash", SH_ACT_ICON_PROPERTIES, 0);
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
static void label_lines(const char *label, char lines[2][16])
{
    /* Up to two centred lines of 13 cells, broken at a space when possible. */
    size_t n = strlen(label);
    lines[0][0] = lines[1][0] = 0;
    if (n <= 13) { snprintf(lines[0], 16, "%s", label); return; }
    size_t cut = 13;
    while (cut > 0 && label[cut] != ' ') cut--;
    if (!cut) cut = 13;
    snprintf(lines[0], 16, "%.*s", (int)cut, label);
    snprintf(lines[1], 16, "%.13s", label + cut + (label[cut] == ' '));
}
static void draw_icons(struct desktop *d, struct desk_box clip)
{
    if (!d->sh.cfg.desktop_icons) return;
    for (int k = 0; k < d->sh.icon_count; k++) {
        const struct sh_icon *ic = &d->sh.icons[k];
        struct desk_box b;
        if (!desk_intersect(ic->box, clip, &b)) continue;
        struct du_canvas c = canvas(d, b);
        int selected = k == d->sh.icon_selected;
        int32_t ix = ic->box.x + (ic->box.w - 32) / 2, iy = ic->box.y + 4;
        du_icon_blit(&c, ix, iy, desk_shell_icon(ic->icon, 32), 32, selected ? DU_ICON_SELECTED : DU_ICON_NORMAL);
        char lines[2][16];
        label_lines(ic->label, lines);
        for (int l = 0; l < 2 && lines[l][0]; l++) {
            int32_t tw = du_text_width(lines[l]), tx = ic->box.x + (ic->box.w - tw) / 2, ty = iy + 38 + l * 11;
            if (selected) {
                du_fill(&c, (struct du_rect){tx - 2, ty - 2, tw + 4, 11}, SH_HOT);
                du_text(&c, tx, ty, lines[l], SH_HOT_INK);
            } else {
                du_text(&c, tx + 1, ty + 1, lines[l], 0x000000u);
                du_text(&c, tx, ty, lines[l], 0xffffffu);
            }
        }
        if (selected && d->focus < 0 && !d->sh.levels) {
            int32_t tw = du_text_width(lines[0]);
            du_dotted(&c, (struct du_rect){ic->box.x + (ic->box.w - tw) / 2 - 3, iy + 35, tw + 6, lines[1][0] ? 24 : 13});
        }
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
    damage(d, switcher_box(d));
}
static void switcher_step(struct desktop *d, int reverse)
{
    if (!d->sh.switch_count) return;
    d->sh.switch_pick = (d->sh.switch_pick + (reverse ? d->sh.switch_count - 1 : 1)) % d->sh.switch_count;
    damage(d, switcher_box(d));
}
static void switcher_finish(struct desktop *d, int commit)
{
    if (!d->sh.switcher) return;
    damage(d, switcher_box(d));
    d->sh.switcher = 0;
    if (!commit || d->sh.switch_pick < 0 || d->sh.switch_pick >= d->sh.switch_count) return;
    int i = d->sh.switch_list[d->sh.switch_pick];
    if (i < 0 || i >= DESK_CLIENTS || !d->clients[i].window) return;
    if (d->clients[i].minimized) desk_minimize(d, i); else desk_focus(d, i);
}
static void draw_switcher(struct desktop *d, struct desk_box clip)
{
    struct desk_box s = switcher_box(d), b;
    if (!desk_intersect(s, clip, &b)) return;
    struct du_canvas c = canvas(d, b);
    du_fill(&c, R(s), SH_FACE);
    du_edge(&c, R(s), DU_RAISED);
    int32_t x = s.x + (s.w - d->sh.switch_count * 44) / 2;
    for (int k = 0; k < d->sh.switch_count; k++) {
        int i = d->sh.switch_list[k];
        struct du_rect cell = {x + k * 44 + 2, s.y + 12, 40, 40};
        if (k == d->sh.switch_pick) du_outline(&c, cell, SH_HOT), du_outline(&c, (struct du_rect){cell.x + 1, cell.y + 1, 38, 38}, SH_HOT);
        du_icon_blit(&c, cell.x + 4, cell.y + 4, desk_shell_icon(client_icon(d, i), 32), 32,
                d->clients[i].minimized ? DU_ICON_GHOST : DU_ICON_NORMAL);
    }
    if (d->sh.switch_pick >= 0 && d->sh.switch_pick < d->sh.switch_count) {
        const char *title = d->clients[d->sh.switch_list[d->sh.switch_pick]].title;
        struct du_rect field = {s.x + 10, s.y + 60, s.w - 20, 20};
        du_fill(&c, field, SH_FACE);
        du_edge(&c, field, DU_SUNKEN_THIN);
        du_text_fit(&c, field.x + 6, field.y + 7, field.w - 12, title, SH_INK);
    }
}

/* ---- background and bars ----------------------------------------------- */
void desk_shell_background(struct desktop *d, struct desk_box clip)
{
    if (d->sh.wall && d->sh.wall_valid && d->sh.wall_w == d->width && d->sh.wall_h == d->height) {
        for (int32_t y = clip.y; y < clip.y + clip.h; y++)
            memcpy(d->pixels + (size_t)y * d->width + (uint32_t)clip.x, d->sh.wall + (size_t)y * d->width + (uint32_t)clip.x,
                   (size_t)clip.w * 4u);
    } else {
        /* No cache (allocation failed or not built yet): the desktop colour and the portrait. */
        struct du_canvas c = canvas(d, clip);
        du_fill(&c, R(clip), d->sh.cfg.desktop_color);
        if (d->portrait && strcmp(d->sh.cfg.wallpaper, "none")) {
            int32_t x0 = ((int32_t)d->width - DESK_PORTRAIT_SIZE) / 2;
            int32_t y0 = DESK_BAR + ((int32_t)d->height - DESK_BAR - DESK_PORTRAIT_SIZE - 36) / 2;
            if (y0 < DESK_BAR) y0 = DESK_BAR;
            struct desk_box b;
            if (desk_intersect((struct desk_box){x0, y0, DESK_PORTRAIT_SIZE, DESK_PORTRAIT_SIZE}, clip, &b))
                for (int32_t y = b.y; y < b.y + b.h; y++) for (int32_t x = b.x; x < b.x + b.w; x++)
                    d->pixels[(uint32_t)y * d->width + (uint32_t)x] = d->portrait[(y - y0) * DESK_PORTRAIT_SIZE + x - x0];
        }
    }
    draw_icons(d, clip);
}
void desk_shell_topbar(struct desktop *d, struct desk_box clip)
{
    struct desk_box bar = {0, 0, (int32_t)d->width, DESK_BAR}, b;
    if (!desk_intersect(bar, clip, &b)) return;
    struct du_canvas c = canvas(d, b);
    du_icon_blit(&c, 8, 6, desk_shell_icon("ciuki", 20), 20, DU_ICON_NORMAL);
    bold(&c, 34, 12, "CiukiOS", SH_CREAM);
    const char *what = d->notice[0] ? d->notice : NULL;
    char line[96];
    if (!what && d->focus >= 0 && d->clients[d->focus].window) { snprintf(line, sizeof(line), "%s", d->clients[d->focus].title); what = line; }
    du_text(&c, 92, 12, what ? what : "A modern Retro OS", 0xffffffu);
    char date[48];
    date_text(d, date, sizeof(date), 1);
    du_text(&c, (int)d->width - du_text_width(date) - 12, 12, date, 0xffffffu);
    du_hline(&c, 0, DESK_BAR - 1, (int)d->width, 0x10212au);
}
void desk_shell_overlay(struct desktop *d, struct desk_box clip)
{
    draw_taskbar(d, clip);
    for (int l = 0; l < d->sh.levels; l++) draw_menu(d, &d->sh.menus[l], clip);
    if (d->sh.volume_open) draw_volume(d, clip);
    if (d->sh.tooltip && !d->sh.volume_open && !d->sh.levels) draw_tooltip(d, clip);
    if (d->sh.switcher) draw_switcher(d, clip);
}

/* ---- built-in windows -------------------------------------------------- */
static const char *const about_tabs[] = {"General", "System", "Credits"};
static void internal_size(const struct desktop *d, int kind, uint32_t *w, uint32_t *h)
{
    switch (kind) {
    case DESK_INTERNAL_ABOUT: *w = 440; *h = 300; break;
    case DESK_INTERNAL_RUN: *w = 372; *h = 128; break;
    case DESK_INTERNAL_SHUTDOWN: *w = 352; *h = 132; break;
    default: {
        unsigned lines = 1;
        for (const char *s = d->sh.message_text; *s; s++) lines += *s == '\n';
        int32_t widest = 0, run = 0;
        for (const char *s = d->sh.message_text;; s++) {
            if (!*s || *s == '\n') { if (run > widest) widest = run; run = 0; if (!*s) break; }
            else if (((unsigned char)*s & 0xc0) != 0x80) run += DU_CHAR_W;
        }
        *w = (uint32_t)(widest + 80 > 300 ? widest + 80 : 300);
        *h = lines * 12 + 70 > 120 ? lines * 12 + 70 : 120;
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
    if (!++d->next_window_id) ++d->next_window_id;
    c->window_id = d->next_window_id;
    c->info.width = w; c->info.height = h;
    snprintf(c->title, sizeof(c->title), "%s", kind == DESK_INTERNAL_ABOUT ? "About CiukiOS" : kind == DESK_INTERNAL_RUN ? "Run" :
             kind == DESK_INTERNAL_SHUTDOWN ? "Shut Down CiukiOS" : d->sh.message_title);
    c->frame = desk_frame(w, h, 0, DESK_BAR);
    if (kind == DESK_INTERNAL_ABOUT) { d->about = i; d->sh.about_tab = 0; d->sh.want_reload |= 2; }
    if (kind == DESK_INTERNAL_RUN) { d->sh.run_length = 0; d->sh.run_text[0] = 0; }
    if (kind == DESK_INTERNAL_SHUTDOWN) d->sh.shutdown_choice = 0;
    int32_t x = ((int32_t)d->width - c->frame.w) / 2, y = DESK_BAR + 20;
    if (kind == DESK_INTERNAL_RUN) { x = 8; y = (int32_t)d->height - DESK_BAR - c->frame.h - 8; }
    else if (kind != DESK_INTERNAL_ABOUT) y = ((int32_t)d->height - c->frame.h) / 2 - 20;
    desk_move(d, i, x, y);
    if (d->sh.levels) desk_menu_close(d);
    desk_focus(d, i);
    damage(d, c->frame);
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
    du_fill(c, R(b), SH_FACE);
    if (def) { du_outline(c, R(b), DU_DARK); du_edge(c, (struct du_rect){b.x + 1, b.y + 1, b.w - 2, b.h - 2}, DU_BUTTON_EDGE); }
    else du_edge(c, R(b), DU_BUTTON_EDGE);
    du_label(c, b.x + (b.w - du_text_width(label) + (strchr(label, '&') ? DU_CHAR_W : 0)) / 2, b.y + 8, label, SH_INK, 1);
}
static void multiline(struct du_canvas *c, int32_t x, int32_t y, const char *text, uint32_t colour)
{
    char line[128]; size_t n = 0;
    for (const char *s = text;; s++) {
        if (!*s || *s == '\n' || n + 1 >= sizeof(line)) {
            line[n] = 0; du_text(c, x, y, line, colour); y += 12; n = 0;
            if (!*s) break;
            if (*s != '\n') line[n++] = *s;
        } else line[n++] = *s;
    }
}
static struct desk_box about_tab(struct desk_box content, int k)
{ return (struct desk_box){content.x + 8 + k * 76, content.y + 8, 74, 20}; }
static void about_page(struct desktop *d, struct du_canvas *c, struct desk_box content)
{
    struct desk_box page = {content.x + 8, content.y + 27, content.w - 16, content.h - 27 - 42};
    du_fill(c, R(page), SH_FACE);
    du_edge(c, R(page), DU_RAISED);
    for (int k = 0; k < 3; k++) {
        struct desk_box t = about_tab(content, k);
        int on = k == d->sh.about_tab;
        struct du_rect r = {t.x, t.y + (on ? -2 : 0), t.w, t.h + (on ? 3 : 1)};
        du_fill(c, r, SH_FACE);
        du_hline(c, r.x + 1, r.y, r.w - 2, DU_HILITE); du_vline(c, r.x, r.y + 1, r.h - 1, DU_HILITE);
        du_vline(c, r.x + r.w - 1, r.y + 1, r.h - 2, DU_DARK); du_vline(c, r.x + r.w - 2, r.y + 1, r.h - 2, DU_SHADOW);
        du_text(c, t.x + (t.w - du_text_width(about_tabs[k])) / 2, t.y + 7 - (on ? 1 : 0), about_tabs[k], SH_INK);
    }
    int32_t x = page.x + 14, y = page.y + 14;
    char line[160];
    if (d->sh.about_tab == 0) {
        if (d->portrait) {
            struct du_rect frame = {x - 2, y - 2, 116, 116};
            du_edge(c, frame, DU_SUNKEN);
            for (int32_t yy = 0; yy < 112; yy++) for (int32_t xx = 0; xx < 112; xx++) {
                uint32_t p = d->portrait[(yy * DESK_PORTRAIT_SIZE / 112) * DESK_PORTRAIT_SIZE + xx * DESK_PORTRAIT_SIZE / 112];
                du_fill(c, (struct du_rect){x + xx, y + yy, 1, 1}, p);
            }
        }
        int32_t tx = x + 132;
        for (int s = 0; s < 2; s++) for (const char *p = "CiukiOS"; *p; p++) {
            int32_t gx = tx + (int32_t)(p - "CiukiOS") * 12 + s;
            for (unsigned row = 0; row < 7; row++) {
                uint8_t bits = desk_glyph_row((unsigned char)*p, row);
                for (unsigned col = 0; col < 5; col++) if (bits & (1u << (4 - col)))
                    du_fill(c, (struct du_rect){gx + (int32_t)col * 2, y + 4 + (int32_t)row * 2, 2, 2}, SH_HOT);
            }
        }
        du_text(c, tx, y + 26, "A modern Retro OS", SH_INK);
        du_text(c, tx, y + 40, "Dedicated to Ciuki", DU_SHADOW);
        snprintf(line, sizeof(line), "Version: %s", d->version[0] ? d->version : "unknown");
        du_text_fit(c, tx, y + 62, page.x + page.w - tx - 8, line, SH_INK);
        snprintf(line, sizeof(line), "Build: %s", d->build_identity);
        du_text_fit(c, tx, y + 76, page.x + page.w - tx - 8, line, SH_INK);
        du_text(c, tx, y + 90, d->realtime_source ? "Clock: RTC (UTC)" : "Clock: build seed (UTC)", SH_INK);
        snprintf(line, sizeof(line), "Display: %u x %u, 32-bit colour", (unsigned)d->width, (unsigned)d->height);
        du_text(c, tx, y + 104, line, SH_INK);
        du_hline(c, page.x + 10, page.y + page.h - 34, page.w - 20, DU_SHADOW);
        du_hline(c, page.x + 10, page.y + page.h - 33, page.w - 20, DU_HILITE);
        du_text(c, x, page.y + page.h - 24, "The Ciuki portrait is a separate project asset.", DU_SHADOW);
    } else if (d->sh.about_tab == 1) {
        du_icon_blit(c, x, y, desk_shell_icon("ciuki-computer", 32), 32, DU_ICON_NORMAL);
        int32_t tx = x + 46;
        bold(c, tx, y + 2, "System", SH_INK);
        snprintf(line, sizeof(line), "CiukiOS %s on %s", d->version[0] ? d->version : "", d->sh.cpu_name[0] ? d->sh.cpu_name : "i686");
        du_text_fit(c, tx, y + 18, page.x + page.w - tx - 8, line, SH_INK);
        y += 48;
        if (d->sh.system_valid) {
            snprintf(line, sizeof(line), "Physical memory:  %llu MB", (unsigned long long)(d->sh.mem_total_k / 1024));
            du_text(c, x, y, line, SH_INK); y += 14;
            snprintf(line, sizeof(line), "Available:        %llu MB", (unsigned long long)(d->sh.mem_free_k / 1024));
            du_text(c, x, y, line, SH_INK); y += 14;
            snprintf(line, sizeof(line), "Processes:        %u", d->sh.processes);
            du_text(c, x, y, line, SH_INK); y += 14;
            snprintf(line, sizeof(line), "Up time:          %u:%02u:%02u", d->sh.uptime_s / 3600, d->sh.uptime_s / 60 % 60, d->sh.uptime_s % 60);
            du_text(c, x, y, line, SH_INK); y += 14;
        } else { du_text(c, x, y, "The kernel's process list (call 70) is unavailable.", SH_INK); y += 14; }
        du_text(c, x, y, "Processor:        not reported (no CPUID interface yet)", SH_INK); y += 14;
        snprintf(line, sizeof(line), "Desktop windows:  %d", desk_task_count(d));
        du_text(c, x, y, line, SH_INK);
    } else {
        static const char *const credits[] = {
            "CiukiOS: kernel, desktop and tools, MIT licence",
            "Ciuki portrait: approved project asset (not PD)",
            "Icons: Tango Icon Theme 0.8.90, Public Domain",
            "Font: original CiukiOS 5x7 bitmap glyphs",
            "Images: stb_image 2.30, Sean Barrett (MIT/PD)",
            "C library: newlib (BSD-style, COPYING.NEWLIB)",
            "Lua 5.4.8: Lua.org, PUC-Rio (MIT licence)",
            "OPL3: DBOPL, DOSBox Team (GPL-2.0-or-later)",
            "Notices: /system/licenses"};
        for (size_t k = 0; k < sizeof(credits) / sizeof(credits[0]); k++) du_text(c, x, y + (int32_t)k * 15, credits[k], SH_INK);
    }
}
void desk_internal_paint(struct desktop *d, int index, struct desk_box content, struct desk_box clip)
{
    const struct desk_client *cl = &d->clients[index];
    struct desk_box b;
    if (!desk_intersect(content, clip, &b)) return;
    struct du_canvas c = canvas(d, b);
    du_fill(&c, R(content), SH_FACE);
    switch (cl->internal) {
    case DESK_INTERNAL_ABOUT:
        about_page(d, &c, content);
        push_button(&c, button_box(content, 0, 1), "OK", 1);
        break;
    case DESK_INTERNAL_RUN: {
        du_icon_blit(&c, content.x + 12, content.y + 12, desk_shell_icon("application-x-executable", 32), 32, DU_ICON_NORMAL);
        multiline(&c, content.x + 56, content.y + 14, "Type the name of a program in /bin, followed by\nits arguments, and CiukiOS will start it.", SH_INK);
        du_label(&c, content.x + 12, content.y + 58, "&Open:", SH_INK, 1);
        struct du_rect field = {content.x + 56, content.y + 52, content.w - 68, 20};
        du_fill(&c, field, SH_WINDOW);
        du_edge(&c, field, DU_SUNKEN);
        struct du_canvas fc = c; fc.clip = du_intersection(c.clip, (struct du_rect){field.x + 2, field.y + 2, field.w - 4, field.h - 4});
        int32_t room = (field.w - 10) / DU_CHAR_W, start = (int32_t)d->sh.run_length > room ? (int32_t)d->sh.run_length - room : 0;
        du_text(&fc, field.x + 4, field.y + 7, d->sh.run_text + start, SH_INK);
        if (d->focus == index) du_vline(&fc, field.x + 4 + ((int32_t)d->sh.run_length - start) * DU_CHAR_W, field.y + 4, 12, SH_INK);
        push_button(&c, button_box(content, 0, 2), "OK", 1);
        push_button(&c, button_box(content, 1, 2), "Cancel", 0);
        break;
    }
    case DESK_INTERNAL_SHUTDOWN: {
        du_icon_blit(&c, content.x + 14, content.y + 16, desk_shell_icon("system-shutdown", 32), 32, DU_ICON_NORMAL);
        du_icon_blit(&c, content.x + 30, content.y + 32, desk_shell_icon("ciuki", 20), 20, DU_ICON_NORMAL);
        bold(&c, content.x + 64, content.y + 18, "What do you want CiukiOS to do?", SH_INK);
        multiline(&c, content.x + 64, content.y + 38, "Your settings are saved first. Close your\ndocuments before you continue.", SH_INK);
        static const char *const labels[] = {"&Restart", "&Shut Down", "Cancel"};
        for (int k = 0; k < 3; k++) push_button(&c, button_box(content, k, 3), labels[k], k == 1 + 0 * d->sh.shutdown_choice);
        break;
    }
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
int desk_internal_click(struct desktop *d, int index, int32_t x, int32_t y)
{
    struct desk_client *cl = &d->clients[index];
    struct desk_box content = desk_content(cl);
    switch (cl->internal) {
    case DESK_INTERNAL_ABOUT:
        for (int k = 0; k < 3; k++) if (desk_contains(about_tab(content, k), x, y)) { d->sh.about_tab = k; damage(d, cl->frame); d->sh.want_reload |= 2; return 1; }
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
static struct desk_box start_button(const struct desktop *d) { return (struct desk_box){4, (int32_t)d->height - DESK_BAR + 3, 76, 26}; }
static void volume_set_from(struct desktop *d, int32_t y)
{
    struct desk_box t = volume_track(d);
    int v = (t.y + t.h - y) * 100 / t.h;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v != d->sh.volume) { d->sh.volume = v; damage(d, volume_popup(d)); }
}
static void tooltip_hide(struct desktop *d)
{
    if (d->sh.tooltip) { damage(d, tooltip_box(d)); d->sh.tooltip = 0; }
}
int desk_shell_button(struct desktop *d, int button, int down)
{
    if (!d->shell) return 0;
    int32_t x = d->mouse_x, y = d->mouse_y;
    if (!down) {
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
    if (d->sh.volume_open) {
        struct desk_box p = volume_popup(d);
        if (desk_contains(p, x, y)) {
            if (d->sh.mixer_state == 2 && button == CIUKI_BUTTON_LEFT) {
                struct desk_box t = volume_track(d), hit = {t.x - 8, t.y - 6, t.w + 16, t.h + 12};
                if (desk_contains(hit, x, y)) { d->sh.volume_drag = 1; volume_set_from(d, y); }
                else if (desk_contains(volume_mute(d), x, y)) { d->sh.muted = !d->sh.muted; d->sh.want_mixer = 1; damage(d, p); damage(d, tray_box(d)); }
            }
            return 1;
        }
        d->sh.volume_open = 0;
        damage(d, p);
        if (desk_contains(volume_icon_box(d), x, y)) return 1;
    }
    struct desk_box bar = desk_taskbar(d);
    if (desk_contains(start_button(d), x, y)) {
        if (button == CIUKI_BUTTON_LEFT) desk_menu_open(d, SH_MENU_START, 0, 0);
        return 1;
    }
    if (desk_contains(bar, x, y)) {
        if (desk_contains(volume_icon_box(d), x, y) && button == CIUKI_BUTTON_LEFT) {
            d->sh.volume_open = 1; d->sh.want_reload |= 4;
            damage(d, volume_popup(d));
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
    if (d->sh.volume_drag) volume_set_from(d, y);
    for (int l = d->sh.levels - 1; l >= 0; l--) {
        struct sh_menu *m = &d->sh.menus[l];
        int k = menu_hit(m, x, y);
        if (k == -1) continue;
        int hot = k >= 0 ? k : -1;
        if (hot != m->hot) { m->hot = hot; damage(d, m->box); }
        if (k >= 0 && m->items[k].action == SH_ACT_SUBMENU) submenu(d, l, k);
        else if (k >= 0 && l + 1 < d->sh.levels) {
            for (int j = l + 1; j < d->sh.levels; j++) damage(d, d->sh.menus[j].box);
            d->sh.levels = l + 1;
        }
        break;
    }
    int over_clock = desk_contains(clock_box(d), x, y);
    if (!over_clock) { tooltip_hide(d); d->sh.hover_since = 0; }
    else if (!d->sh.hover_since) d->sh.hover_since = d->now_ns ? d->now_ns : 1;
}
void desk_shell_tick(struct desktop *d)
{
    if (!d->shell) return;
    if (!d->sh.tooltip && d->sh.hover_since && !d->sh.levels && !d->sh.volume_open &&
        desk_contains(clock_box(d), d->mouse_x, d->mouse_y) && d->now_ns - d->sh.hover_since >= 600000000ull) {
        d->sh.tooltip = 1;
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
        if (l > 0) { damage(d, m->box); d->sh.levels--; }
        return 1;
    case DU_KEY_ENTER: case DU_KEY_SPACE:
        if (m->hot >= 0) {
            if (m->items[m->hot].action == SH_ACT_SUBMENU) return menu_key(d, DU_KEY_RIGHT);
            activate(d, l, m->hot);
        }
        return 1;
    case DU_KEY_ESC:
        if (l > 0) { damage(d, m->box); d->sh.levels--; } else desk_menu_close(d);
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
        damage(d, c->frame);
        return 1;
    case DESK_INTERNAL_ABOUT:
        if (code == DU_KEY_ESC || code == DU_KEY_ENTER) desk_request_close(d, index);
        else if (code == DU_KEY_TAB || code == DU_KEY_RIGHT || code == DU_KEY_LEFT) {
            d->sh.about_tab = (d->sh.about_tab + (code == DU_KEY_LEFT || (d->sh.modifiers & DU_SHIFT) ? 2 : 1)) % 3;
            d->sh.want_reload |= 2;
            damage(d, c->frame);
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
    if (d->sh.levels) return value ? menu_key(d, code) : 1;
    if (d->sh.volume_open && code == DU_KEY_ESC) { if (value) { d->sh.volume_open = 0; damage(d, volume_popup(d)); } return 1; }
    if (d->focus >= 0 && d->clients[d->focus].internal) {
        /* AltGr characters reach the Run field; other Alt chords are ignored here. */
        if ((d->sh.modifiers & DU_ALT) && d->sh.alt_code != 0x138) return 1;
        return internal_key(d, d->focus, code, value);
    }
    if (d->focus < 0 && d->sh.cfg.desktop_icons && d->sh.icon_count && !d->shell_test && !d->tasks_test && !d->app_test) {
        if (!value) return 0;
        int s = d->sh.icon_selected;
        if (code == DU_KEY_UP || code == DU_KEY_LEFT) { icon_select(d, s <= 0 ? 0 : s - 1); return 1; }
        if (code == DU_KEY_DOWN || code == DU_KEY_RIGHT) { icon_select(d, s < 0 ? 0 : s + 1 < d->sh.icon_count ? s + 1 : s); return 1; }
        if (code == DU_KEY_ENTER && s >= 0) { icon_open(d, s); return 1; }
    }
    return 0;
}

/* ---- settings ---------------------------------------------------------- */
void desk_shell_reset(struct desktop *d)
{
    portrait_src = d->portrait;
    cfg_defaults(&d->sh.cfg);
    d->sh.icon_selected = d->sh.click_icon = -1;
    d->sh.volume = 50;
}
void desk_shell_apply(struct desktop *d)
{
    portrait_src = d->portrait;
    if (!d->sh.icon_count) icons_build(d);
    icons_layout(d);
    d->sh.wall_valid = 0;
    desk_theme_apply(d->sh.cfg.theme);
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
        free(d->sh.wall);
        d->sh.wall = malloc((size_t)d->width * d->height * 4u);
        d->sh.wall_w = d->sh.wall ? d->width : 0; d->sh.wall_h = d->sh.wall ? d->height : 0;
        if (!d->sh.wall) { d->sh.wall_valid = 0; return -1; }
    }
    int r = desk_wallpaper_render(d->sh.wall, d->width, d->height, DESK_BAR, DESK_BAR, d->sh.cfg.wallpaper,
                                  d->sh.cfg.wallpaper_mode, d->sh.cfg.desktop_color, &d->sh.image, d->portrait);
    d->sh.wall_valid = 1;
    d->sh.image_failed = r < 0;
    damage_all(d);
    return r;
}
