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
#include "input_text.h"
#include "catalog.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static int shortcut_hotkey(struct desktop *,int,int);
static void shortcut_programs_menu(struct desktop *,struct sh_menu *,int);

/* G25 accessibility glue (access_desktop.c); weak so hosts that omit it keep working. */
extern void desk_access_confirmed(struct desktop *, int feature, int yes) __attribute__((weak));
extern void desk_access_indicator_draw(struct desktop *, struct du_canvas *, struct desk_box) __attribute__((weak));
extern void desk_access_flash_draw(struct desktop *, struct desk_box) __attribute__((weak));
extern void desk_access_magnifier_set(struct desktop *, int on) __attribute__((weak));
extern int desk_access_magnifier_on(const struct desktop *) __attribute__((weak));
#include <strings.h>

/* ---- theme hooks ------------------------------------------------------- */
/* Every shell colour is a theme colour (desk_theme; DU_* are the same
 * fields under the toolkit's names). The tooltip keeps the classic paper. */
#define SH_FACE      DU_FACE
#define SH_INK       (desk_appearance_active ? desk_appearance.item[AP_MENU].text : DU_INK)
#define SH_HOT       DU_SELECT
#define SH_HOT_INK   DU_SELECT_INK
#define SH_WINDOW    DU_WINDOW
#define SH_TIP       0xffffe1u      /* tooltip paper */
static void tooltip_hide(struct desktop *d);
static void logon_paint(struct desktop *,struct desk_box);
static struct desk_box program_tray_box(const struct desktop *,unsigned);
/* G4: the taskbar's keyboard focus (KB-13) and the access keys of menus (SM-9). */
static void taskbar_set(struct desktop *d, int pick);
static void taskbar_blur(struct desktop *d, int refocus);
static unsigned keymap_mods(const struct desktop *d);
static int menu_letter(struct desktop *d, int level, unsigned ch);
static int internal_find(const struct desktop *d, int kind);

static void desktop_icon_rename_begin(struct desktop *, int);
/* fx-19 selection and drag session (defined after the icon painter). */
static int drop_lit(const struct desktop *d, int k);
static void dnd_commit(struct desktop *d, unsigned effect);
static void draw_band(struct desktop *d, struct desk_box clip);
static void draw_drag_image(struct desktop *d, struct desk_box clip);
static int selection_sync(struct desktop *d);
static int marked_count(const struct desktop *d, int files_only);

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
/* Nonstandard DPI sizes are deterministic conversions of the existing
 * Tango/approved portrait sources. Bound the optional cache to one MiB. */
static const uint32_t *large_icon(const char *name,int size)
{
    static struct { char name[96]; int size; uint32_t *pixels; } cache[16];
    static unsigned count,bytes;
    if(!name || strlen(name)>=sizeof(cache[0].name) || size<49 || size>320) return NULL;
    for(unsigned i=0;i<count;i++) if(cache[i].size==size && !strcmp(cache[i].name,name)) return cache[i].pixels;
    const char *base=!strcmp(name,"ciuki-computer") ? "computer" : name;
    const uint32_t *source=strcmp(name,"ciuki") ? desk_shell_icon(base,48) : NULL;
    unsigned needed=(unsigned)size*(unsigned)size*4;
    if(count==16 || bytes+needed>1024*1024) {
        for(unsigned i=0;i<count;i++) free(cache[i].pixels);
        count=bytes=0;
    }
    uint32_t *pixels=calloc((size_t)size*size,4);
    if(!pixels) return NULL;
    if(source) area_scale(source,48,pixels,size,0,0,size);
    if(!strcmp(name,"ciuki")) portrait_badge(pixels,size,size,0,0);
    else if(!strcmp(name,"ciuki-computer")) portrait_badge(pixels,size,size*5/8,size-size*5/8,size-size*5/8);
    strcpy(cache[count].name,name); cache[count].size=size; cache[count].pixels=pixels;
    count++; bytes+=needed; return pixels;
}
const uint32_t *desk_shell_icon(const char *name, int size)
{
    if(size>48) return large_icon(name,size);
    if (!name || size <= 0) return NULL;
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
    {"printers", "Printers", "printer", "programs", NULL},
    {"control", "Control Panel", "preferences-desktop", "programs", NULL},
    {"music", "Music Player", "audio-x-generic", "programs", NULL},
    {"viewer", "Image Viewer", "image-x-generic", "programs", NULL},
    {"paint", "CiukPaint", "applications-graphics", "programs", NULL},
    {"terminal", "Terminal", "utilities-terminal", "programs", NULL},
    {"cdplayer", "CD Player", "media-optical", "accessories", "C:\\WINDOWS\\CDPLAYER.EXE"},
    {"calc", "Calculator", "accessories-calculator", "accessories", NULL},
    {"gfxdemo", "Graphics Demo", "applications-graphics", "accessories", NULL},
    {"charmap", "Character Map", "accessories-character-map", "accessories", NULL},
    {"wordpad", "WordPad", "x-office-document", "accessories", NULL},
    {"clock", "Clock", "appointment-new", "accessories", NULL},
    {"help", "CiukiOS Help", "help-browser", "accessories", NULL},
    {"mixer", "Volume Control", "audio-volume-high", "accessories", NULL},
    {"serial", "Serial Terminal", "network-wired", "accessories", NULL},
    {"mines", "Minesweeper", "face-smile", "games", NULL},
    {"solitaire", "Solitaire", "applications-games", "games", NULL},
    {"freecell", "FreeCell", "applications-games", "games", NULL},
    {"hearts", "Hearts", "emblem-favorite", "games", NULL},
    {"hwreport", "Hardware Report", "preferences-system", "system-tools", NULL},
    {"scandisk", "ScanDisk", "drive-harddisk", "system-tools", NULL},
    {"msconfig", "System Configuration", "preferences-system", "system-tools", NULL},
    {"logview", "Log Viewer", "text-x-generic", "system-tools", NULL},
    {"clipbrd", "Clipboard Viewer", "edit-paste", "system-tools", NULL},
    {"resmeter", "Resource Meter", "applications-system", "system-tools", NULL},
    {"schedtask", "Scheduled Tasks", "x-office-calendar", "system-tools", NULL},
    {"sysinfo", "System Information", "document-properties", "system-tools", NULL},
    {"sysmon", "System Monitor", "utilities-system-monitor", "system-tools", NULL},
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
const char *desk_program_menu_label(const char *name)
{ const struct program_info *p=program_find(name);return p ? p->label : name; }
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
    if (c->internal == DESK_INTERNAL_SHORTCUT || c->internal == DESK_INTERNAL_SHORTCUT_WIZARD) return "emblem-symbolic-link";
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
static int network_icon(const struct desktop *d, int index)
{
    return index>=0 && index<d->sh.icon_count && !strcmp(d->sh.icons[index].program,"control") &&
           !strcmp(d->sh.icons[index].argument,"--panel=network");
}
static int icon_is_entry(const struct desktop *d, int index)
{
    return d->sh.folder && index >= 3 && index < d->sh.icon_count &&
        (unsigned)(index - 3) < d->sh.folder->model.count;
}
static const char *icon_position_name(const struct desktop *d, int index)
{
    if (index < 3) return index == 0 ? "/Computer" : index == 1 ? (d->sh.profile.documents[0] ? d->sh.profile.documents : "/Documents") : "/Trash";
    if (network_icon(d,index)) return "/Network";
    if (index>=0 && index<d->sh.icon_count && d->sh.icons[index].action==SH_ACT_WIN32_LINK) {
        unsigned link=(unsigned)strtoul(d->sh.icons[index].argument,NULL,10);
        if (link<d->v3_link_count) return d->v3_links[link].source;
    }
    return icon_is_entry(d, index) ? d->sh.icons[index].label : "/Pictures";
}
/* SM-9: the underlined access keys of the shell's fixed menus, the Windows 98
 * assignments (Start: P D S F H R and Sh&ut Down; the window menu: R M S Mi&nimize
 * Ma&ximize C). A label may end with a shortcut after two spaces. Menus built
 * from the installed programs have none: a letter jumps to the next entry that
 * starts with it, as in Windows. */
static const struct { int kind; const char *label; char key; } access_keys[] = {
    {SH_MENU_START,"Programs",'p'},{SH_MENU_START,"Documents",'d'},{SH_MENU_START,"Settings",'s'},
    {SH_MENU_START,"Find",'f'},{SH_MENU_FIND,"Files or Folders...",'f'},
    {SH_MENU_START,"Help",'h'},{SH_MENU_START,"Run...",'r'},{SH_MENU_START,"Shut Down...",'u'},
    {SH_MENU_START,"Favorites",'a'},
    {SH_MENU_SETTINGS,"Control Panel",'c'},{SH_MENU_SETTINGS,"Task Manager",'t'},
    {SH_MENU_SETTINGS,"Taskbar & Start Menu",'k'},
    {SH_MENU_HELP,"Keyboard Shortcuts",'k'},{SH_MENU_HELP,"About CiukiOS",'a'},
    {SH_MENU_DOCUMENTS,"My Documents",'m'},{SH_MENU_DOCUMENTS,"Clear Recent Documents",'c'},
    {SH_MENU_DESKTOP,"Arrange icons",'i'},{SH_MENU_DESKTOP,"Line up",'u'},{SH_MENU_DESKTOP,"Refresh",'f'},
    {SH_MENU_DESKTOP,"New",'n'},{SH_MENU_DESKTOP,"Paste",'p'},{SH_MENU_DESKTOP,"Properties",'r'},
    {SH_MENU_NEW,"Folder",'f'},{SH_MENU_NEW,"Text file",'t'},{SH_MENU_NEW,"Bitmap Image",'b'},{SH_MENU_NEW,"Shortcut...",'s'},
    {SH_MENU_ARRANGE,"By Name",'n'},{SH_MENU_ARRANGE,"By Type",'t'},{SH_MENU_ARRANGE,"By Size",'s'},{SH_MENU_ARRANGE,"By Date",'d'},
    {SH_MENU_ICON,"Open",'o'},{SH_MENU_ICON,"Open With",'w'},{SH_MENU_ICON,"Cut",'t'},{SH_MENU_ICON,"Copy",'c'},
    {SH_MENU_ICON,"Rename",'m'},{SH_MENU_ICON,"Delete",'d'},{SH_MENU_ICON,"Properties",'r'},
    {SH_MENU_TASK,"Restore",'r'},{SH_MENU_TASK,"Move",'m'},{SH_MENU_TASK,"Size",'s'},{SH_MENU_TASK,"Minimize",'n'},
    {SH_MENU_TASK,"Maximize",'x'},{SH_MENU_TASK,"Full screen",'f'},{SH_MENU_TASK,"Close",'c'},
    {SH_MENU_TASKBAR,"Cascade Windows",'c'},{SH_MENU_TASKBAR,"Tile Windows Horizontally",'h'},
    {SH_MENU_TASKBAR,"Tile Windows Vertically",'v'},{SH_MENU_TASKBAR,"Minimize All Windows",'m'},
    {SH_MENU_TASKBAR,"Undo Minimize All",'u'},{SH_MENU_TASKBAR,"Undo Cascade",'u'},{SH_MENU_TASKBAR,"Undo Tile",'u'},
    {SH_MENU_TASKBAR,"Properties",'r'},
    {SH_MENU_DRAG,"Move Here",'m'},{SH_MENU_DRAG,"Copy Here",'c'},{SH_MENU_DRAG,"Create Shortcut(s) Here",'s'},
};
static void assign_marks(struct sh_menu *m)
{
    for (int k=0;k<m->count;k++) {
        struct sh_item *it=&m->items[k];
        it->mark=0;
        if (it->flags & SH_ITEM_SEPARATOR) continue;
        for (size_t a=0;a<sizeof(access_keys)/sizeof(*access_keys);a++) {
            if (access_keys[a].kind!=m->kind) continue;
            size_t len=strlen(access_keys[a].label);
            if (strncmp(it->label,access_keys[a].label,len) || (it->label[len] && strncmp(it->label+len,"  ",2))) continue;
            for (size_t b=0;b<len;b++) if (tolower((unsigned char)it->label[b])==access_keys[a].key) { it->mark=(uint8_t)(b+1); break; }
            break;
        }
    }
}
static int toolbar_hit(const struct desktop *,int,int);
static void toolbar_activate(struct desktop *,int);
static void draw_toolbars(struct desktop *,struct desk_box);
static void bar_motion(struct desktop *);
static void bar_reveal(struct desktop *);
static void bar_autohide(struct desktop *);
static int address_key(struct desktop *,int,int);
static const char *custom_basename(const char *path)
{ const char *p=strrchr(path,'/'); return p ? p+1 : path; }
static void custom_folder_items(struct desktop *d,struct sh_menu *m,int folder)
{
    const struct sh_folder_snapshot *s=d->sh.custom;
    if (!s) return;
    unsigned shown=0;int extra=0;unsigned limit=m->kind==SH_MENU_START ? SH_ITEMS-11 : SH_FOLDER_ITEMS;
    for (unsigned k=0;k<s->count;k++) if (s->entries[k].parent==folder) {
        if (shown++>=limit) { extra=1;continue; }
        const struct sh_folder_item *e=&s->entries[k];
        item(m,e->label,e->folder>=0 ? "folder" : "application-x-executable",
             e->folder>=0 ? SH_ACT_SUBMENU : SH_ACT_SHELL_ITEM,e->folder>=0 ? SH_MENU_FOLDER_BASE+e->folder : (int)k,0);
    }
    if (extra || s->truncated) item(m,"More... Open Folder","folder",SH_ACT_FOLDER_EXPLORE,folder,0);
}
static int custom_index(const struct desktop *d,const struct sh_item *it)
{
    if (!d->sh.custom) return -1;
    if (it->action==SH_ACT_SHELL_ITEM && it->arg>=0 && (unsigned)it->arg<d->sh.custom->count) return it->arg;
    if (it->action==SH_ACT_SUBMENU && it->arg>=SH_MENU_FOLDER_BASE) {
        for (unsigned k=0;k<d->sh.custom->count;k++) if (d->sh.custom->entries[k].folder==it->arg-SH_MENU_FOLDER_BASE) return (int)k;
    }
    return -1;
}
static void custom_request(struct desktop *d,int op,const char *path,const char *dest,const char *name)
{
    if ((path && strlen(path)>=CFG_PATH_MAX) || (dest && strlen(dest)>=CFG_PATH_MAX) || (name && strlen(name)>=sizeof(d->sh.edit.name))) {
        desk_shell_message(d,"Start Menu","The name or path is too long.",1);return;
    }
    if (d->sh.edit.operation) { desk_shell_message(d,"Start Menu","Another shell operation is in progress. Try again shortly.",1); return; }
    memset(&d->sh.edit,0,sizeof(d->sh.edit)); d->sh.edit.operation=op;
    if (path) snprintf(d->sh.edit.path,sizeof(d->sh.edit.path),"%s",path);
    if (dest) snprintf(d->sh.edit.destination,sizeof(d->sh.edit.destination),"%s",dest);
    if (name) snprintf(d->sh.edit.name,sizeof(d->sh.edit.name),"%s",name);
}
static void custom_prompt(struct desktop *d,int mode,const char *initial)
{
    int i=desk_internal_open(d,DESK_INTERNAL_RUN);
    if (i<0) return;
    d->sh.prompt_mode=mode;
    snprintf(d->clients[i].title,sizeof(d->clients[i].title),"%s",mode==1 ? "Address" : mode==2 ? "New Toolbar" : "Rename");
    snprintf(d->sh.run_text,sizeof(d->sh.run_text),"%s",initial ? initial : "");
    d->sh.run_length=(unsigned)strlen(d->sh.run_text); damage_all(d);
}
static void custom_reorder(struct desktop *d,int from,int to)
{
    struct sh_folder_snapshot *s=d->sh.custom;
    if (!s || from<0 || to<0 || (unsigned)from>=s->count || (unsigned)to>=s->count || from==to || s->entries[from].parent!=s->entries[to].parent || d->sh.edit.operation) return;
    int parent=s->entries[from].parent;
    struct sh_folder_item e=s->entries[from];
    if (from<to) memmove(s->entries+from,s->entries+from+1,(size_t)(to-from)*sizeof(e));
    else memmove(s->entries+to+1,s->entries+to,(size_t)(from-to)*sizeof(e));
    s->entries[to]=e;
    custom_request(d,SH_EDIT_ORDER,s->folders[parent],NULL,NULL);
    for (unsigned k=0;k<s->count && d->sh.edit.count<SH_FOLDER_ITEMS;k++) if (s->entries[k].parent==parent)
        snprintf(d->sh.edit.order[d->sh.edit.count++],CFG_PATH_MAX,"%s",custom_basename(s->entries[k].path));
}
static void build(struct desktop *d, struct sh_menu *m, int kind)
{
    memset(m, 0, sizeof(*m));
    m->kind = kind; m->hot = -1;
    m->row_h = desk_appearance_active ? desk_metric(AP_MENU,SH_SUB_ROW)+desk_scale(10) : kind == SH_MENU_START ? desk_menu_row(d) : SH_SUB_ROW;
    m->icon = 16;   /* every menu: the Tango 16 px artwork */
    if (kind>=128) { shortcut_programs_menu(d,m,kind); return; }
    m->icon = kind==SH_MENU_START && !d->sh.cfg.start_small ? 32 : 16;   /* G20: the Start menu's large icons */
    switch (kind) {
    case SH_MENU_START:
        custom_folder_items(d,m,SH_ROOT_START);
        if (m->count) separator(m);
        item(m, "Programs", "applications-other", SH_ACT_SUBMENU, SH_MENU_PROGRAMS, 0);
        item(m,"Favorites","folder",SH_ACT_SUBMENU,SH_MENU_FOLDER_BASE+SH_ROOT_FAVORITES,0);
        item(m, "Documents", "document-open", SH_ACT_SUBMENU, SH_MENU_DOCUMENTS, 0);
        item(m, "Settings", "preferences-system", SH_ACT_SUBMENU, SH_MENU_SETTINGS, 0);
        item(m, "Find", "system-search", SH_ACT_SUBMENU, SH_MENU_FIND, 0);
        item(m, "Help", "help-browser", SH_ACT_HELP, 0, 0); /* G15: launch Help directly */
        item(m, "Run...", "go-jump", SH_ACT_RUN, 0, 0);
        item(m, "About CiukiOS", "ciuki", SH_ACT_ABOUT, 0, 0);   /* G15: it left the Help submenu */
        separator(m);
        char logoff_label[44];
        snprintf(logoff_label,sizeof(logoff_label),"Log Off%s%.31s...",d->sh.profile.name[0] ? " " : "",d->sh.profile.name);
        item(m,logoff_label,"system-log-out",SH_ACT_LOGOFF,0,0);
        if (d->sh.profile.name[0] && (d->sh.users.flags&USERS_LOCK))
            item(m, "Lock", "system-lock-screen", SH_ACT_LOCK, 0, 0);
        item(m, "Shut Down...", "system-shutdown", SH_ACT_SHUTDOWN, 0, 0);
        break;
    case SH_MENU_FIND:
        item(m, "Files or Folders...", "system-search", SH_ACT_FIND, 0, 0);
        break;
    case SH_MENU_PROGRAMS: {
        /* f2-95: opening the menu asks for a fresh scan; the menu shows the
         * current snapshot and the next refresh publishes any change. */
        d->sh.custom_refresh=1;
        if (d->sh.custom && d->sh.custom->native_seeded) {
            custom_folder_items(d,m,SH_ROOT_PROGRAMS);
            if (!m->count) item(m,"(Empty)",NULL,SH_ACT_NONE,0,SH_ITEM_DISABLED);
            break;
        }
        d->v3_links_force|=DESK_LINKS_SCAN;
        desk_links_refresh(d);
        if (d->v3_link_count) item(m,"Programs folder","folder",SH_ACT_PROGRAMS_FOLDER,0,0);
        int developer = 0, accessories = 0, system_tools = 0, games = 0;
        for (size_t k = 0; k < sizeof(programs) / sizeof(programs[0]); k++) {
            int at = program_index(d, programs[k].name);
            if (at < 0 && !(programs[k].target && d->v3_launch)) continue;
            if (!strcmp(programs[k].category, "system-tools")) { system_tools = 1; continue; }
            if (!strcmp(programs[k].category, "accessories")) { accessories = 1; continue; }
            if (!strcmp(programs[k].category, "games")) { games = 1; continue; }
            if (strcmp(programs[k].category, "programs")) { developer = 1; continue; }
            item(m, programs[k].label, programs[k].icon, SH_ACT_LAUNCH, at, 0);
        }
        for (unsigned k = 0; k < d->program_count; k++) if (!program_find(d->programs[k].name)) developer = 1;
        if (!m->count) item(m, "(No programs)", NULL, SH_ACT_NONE, 0, SH_ITEM_DISABLED);
        if (system_tools) item(m, "System Tools", "applications-system", SH_ACT_SUBMENU, SH_MENU_SYSTEM_TOOLS, 0);
        if (accessories) item(m, "Accessories", "applications-accessories", SH_ACT_SUBMENU, SH_MENU_ACCESSORIES, 0);
        /* The bundled shareware games run in a DOS Box window (catalog.h desk_games). */
        if (games || program_index(d, "dosbox") >= 0) item(m, "Games", "applications-games", SH_ACT_SUBMENU, SH_MENU_GAMES, 0);
        if (developer) { separator(m); item(m, "Developer", "applications-development", SH_ACT_SUBMENU, SH_MENU_DEVELOPER, 0); }
        shortcut_programs_menu(d,m,kind);
        break;
    }
    case SH_MENU_SYSTEM_TOOLS:
        for (size_t k = 0; k < sizeof(programs) / sizeof(programs[0]); k++) {
            int at = program_index(d, programs[k].name);
            if (at >= 0 && !strcmp(programs[k].category, "system-tools"))
                item(m, programs[k].label, programs[k].icon, SH_ACT_LAUNCH, at, 0);
        }
        shortcut_programs_menu(d,m,kind);
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
        shortcut_programs_menu(d,m,kind);
        if (desk_access_magnifier_set) {   /* G25: Accessories > Accessibility > Magnifier, one entry here */
            if (m->count) separator(m);
            item(m, desk_access_magnifier_on(d) ? "Magnifier (on)" : "Magnifier", "preferences-desktop-accessibility", SH_ACT_MAGNIFIER, 0, 0);
        }
        break;
    case SH_MENU_GAMES: {
        unsigned native = 0;   /* G27: the native games first (Minesweeper, Solitaire, FreeCell, Hearts), then the bundled DOS games */
        for (size_t k = 0; k < sizeof(programs) / sizeof(programs[0]); k++) {
            int at = program_index(d, programs[k].name);
            if (at >= 0 && !strcmp(programs[k].category, "games")) { item(m, programs[k].label, programs[k].icon, SH_ACT_LAUNCH, at, 0); native++; }
        }
        if (native) separator(m);
        for (unsigned k = 0; k < DESK_GAME_COUNT; k++) if(!(d->games_removed&(1u<<k))) item(m, desk_games[k].label, desk_games[k].tango, SH_ACT_GAME, (int)k, 0);
        shortcut_programs_menu(d,m,kind);
        break;
    }
    case SH_MENU_DEVELOPER:
        for (unsigned k = 0; k < d->program_count && m->count < SH_ITEMS; k++) {
            const struct program_info *p = program_find(d->programs[k].name);
            if (p && strcmp(p->category, "developer")) continue;   /* Programs and Accessories list theirs */
            item(m, p ? p->label : d->programs[k].name, p ? p->icon : "application-x-executable", SH_ACT_LAUNCH, (int)k, 0);
        }
        break;
    case SH_MENU_DOCUMENTS:
        d->sh.custom_refresh=1;
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
        item(m, "Printers", "printer", SH_ACT_LAUNCH, program_index(d,"printers"), program_index(d,"printers")<0?SH_ITEM_DISABLED:0);
        separator(m);
        for (int k=0;k<CP_CATEGORIES;k++)
            item(m,cp_categories[k].label,cp_categories[k].icon,SH_ACT_SUBMENU,SH_MENU_SETTINGS_CATEGORY+k,off);
        separator(m);
        item(m,"Taskbar & Start Menu","preferences-system-windows",SH_ACT_CONTROL,CP_TASKBAR+1,off);
        item(m,"Folder Options","folder",SH_ACT_FOLDER_OPTIONS,0,program_index(d,"files")<0 ? SH_ITEM_DISABLED : 0);
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
        item(m,"Bitmap Image","image-x-generic",SH_ACT_FILE,DF_BITMAP,0);
        item(m,"Shortcut...","emblem-symbolic-link",SH_ACT_FILE,DF_SHORTCUT,0);
        item(m,"Document...","document-new",SH_ACT_FILES_ACTION,FILES_NEW_REGISTERED,0);
        break;
    case SH_MENU_ARRANGE:
        item(m,"By Name",NULL,SH_ACT_ICON_SORT,0,d->sh.cfg.icon_sort==0 ? SH_ITEM_DEFAULT : 0);
        item(m,"By Type",NULL,SH_ACT_ICON_SORT,1,d->sh.cfg.icon_sort==1 ? SH_ITEM_DEFAULT : 0);
        item(m,"By Size",NULL,SH_ACT_ICON_SORT,2,d->sh.cfg.icon_sort==2 ? SH_ITEM_DEFAULT : 0);
        item(m,"By Date",NULL,SH_ACT_ICON_SORT,3,d->sh.cfg.icon_sort==3 ? SH_ITEM_DEFAULT : 0);
        separator(m);
        item(m,"Auto Arrange",NULL,SH_ACT_ICON_AUTO,0,d->sh.cfg.icon_auto_arrange ? SH_ITEM_CHECKED : 0);
        break;
    case SH_MENU_OPEN_WITH:
        if (icon_is_entry(d, d->sh.menu_owner)) {
            const char *path=d->sh.icons[d->sh.menu_owner].argument;
            for (unsigned i=0;i<DESK_CATALOG_COUNT;i++) if (desk_catalog_supports(&desk_catalog[i],path))
                item(m,desk_catalog[i].label,desk_catalog[i].tango,SH_ACT_OPEN_WITH,(int)i,0);
            item(m,"CiukNote","accessories-text-editor",SH_ACT_OPEN_WITH,-1,0);
            item(m,"Choose Program...",NULL,SH_ACT_FILES_ACTION,FILES_OPEN_WITH,0);
        }
        break;
    case SH_MENU_DESKTOP:
        item(m,"Arrange icons",NULL,SH_ACT_SUBMENU,SH_MENU_ARRANGE,0);
        item(m,"Line up",NULL,SH_ACT_ARRANGE,1,0);
        item(m,"Refresh","view-refresh",SH_ACT_REFRESH,0,0);
        item(m,"New","document-new",SH_ACT_SUBMENU,SH_MENU_NEW,d->sh.folder ? 0 : SH_ITEM_DISABLED);
        item(m,"Paste","edit-paste",SH_ACT_FILE,DF_PASTE,d->sh.folder ? 0 : SH_ITEM_DISABLED);
        item(m,"Paste Shortcut",NULL,SH_ACT_FILE,DF_PASTE_SHORTCUT,d->sh.folder ? 0 : SH_ITEM_DISABLED);
        item(m,"Undo    Ctrl+Z","edit-undo",SH_ACT_FILE,DF_UNDO,d->sh.folder ? 0 : SH_ITEM_DISABLED);
        item(m,"Show Desktop Icons",NULL,SH_ACT_ICONS_SHOW,0,d->sh.cfg.desktop_icons ? SH_ITEM_CHECKED : 0);
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
        if (d->sh.menu_owner==0 || d->sh.menu_owner==1)
            item(m,"Explore","folder",SH_ACT_ICON_OPEN,d->sh.menu_owner,0);
        if (network_icon(d,d->sh.menu_owner))
            item(m,"Explore","network-workgroup",SH_ACT_ICON_OPEN,d->sh.menu_owner,0);
        if (d->sh.menu_owner == 2) {   /* G2: the Recycle Bin */
            item(m, "Empty Recycle Bin", "user-trash", SH_ACT_EMPTY_RECYCLE, 0, d->sh.folder && d->sh.folder->bin_full ? 0 : SH_ITEM_DISABLED);
            item(m,"Rename",NULL,SH_ACT_RENAME,2,0);
            separator(m);
            item(m, "Properties", "document-properties", SH_ACT_ICON_PROPERTIES, 2, 0);
            break;
        }
        if (icon_is_entry(d, d->sh.menu_owner)) {
            if (d->sh.folder->model.entries[d->sh.menu_owner-3].meta.type != FM_DIR)
                item(m,"Open With",NULL,SH_ACT_SUBMENU,SH_MENU_OPEN_WITH,0);
            item(m,"Cut", "edit-cut", SH_ACT_FILE, DF_CUT,0);
            item(m,"Copy", "edit-copy", SH_ACT_FILE, DF_COPY,0);
            if (d->sh.folder->model.entries[d->sh.menu_owner-3].meta.type==FM_DIR)
                item(m,"Explore",NULL,SH_ACT_FILES_ACTION,0,0);
            item(m,"Send To...",NULL,SH_ACT_FILES_ACTION,FILES_SEND_TO,0);
            item(m,"Copy To Folder...",NULL,SH_ACT_FILES_ACTION,FILES_COPY_TO,0);
            item(m,"Move To Folder...",NULL,SH_ACT_FILES_ACTION,FILES_MOVE_TO,0);
            item(m,"Create Shortcut",NULL,SH_ACT_FILE,DF_SHORTCUT,0);
            if (d->sh.folder->model.entries[d->sh.menu_owner-3].meta.type==FM_DIR)
                item(m,"Paste",NULL,SH_ACT_FILE,DF_PASTE_INTO,0);
            item(m,"Rename    F2", NULL, SH_ACT_RENAME,d->sh.menu_owner,marked_count(d,0)>1 ? SH_ITEM_DISABLED : 0);
            item(m,"Delete", "edit-delete", SH_ACT_FILE, DF_DELETE,0);
        } else if ((d->sh.menu_owner>=0 && d->sh.menu_owner<3) || network_icon(d,d->sh.menu_owner))
            item(m,"Rename",NULL,SH_ACT_RENAME,d->sh.menu_owner,0);
        separator(m);
        item(m, "Properties", "document-properties", SH_ACT_ICON_PROPERTIES, d->sh.menu_owner, 0);
        break;
    case SH_MENU_DRAG: {
        /* Windows 98 right-drag menu; the default is the plain-drag effect. */
        unsigned allowed=d->sh.dnd.allowed, def=d->sh.dnd.effect;
        item(m,"Move Here",NULL,SH_ACT_DRAG,FM_EFFECT_MOVE,!(allowed&FM_EFFECT_MOVE) ? SH_ITEM_DISABLED : def==FM_EFFECT_MOVE ? SH_ITEM_DEFAULT : 0);
        item(m,"Copy Here",NULL,SH_ACT_DRAG,FM_EFFECT_COPY,!(allowed&FM_EFFECT_COPY) ? SH_ITEM_DISABLED : def==FM_EFFECT_COPY ? SH_ITEM_DEFAULT : 0);
        item(m,"Create Shortcut(s) Here",NULL,SH_ACT_DRAG,FM_EFFECT_LINK,!(allowed&FM_EFFECT_LINK) ? SH_ITEM_DISABLED : def==FM_EFFECT_LINK ? SH_ITEM_DEFAULT : 0);
        separator(m);
        item(m,"Cancel",NULL,SH_ACT_DRAG,0,0);
        break;
    }
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
        if (c && c->dosbox) {
            item(m,"Mark",NULL,SH_ACT_DOS_MARK,owner,blocked ? SH_ITEM_DISABLED : 0);
            item(m,"Copy",NULL,SH_ACT_DOS_COPY,owner,blocked ? SH_ITEM_DISABLED : 0);
            item(m,"Paste",NULL,SH_ACT_DOS_PASTE,owner,blocked ? SH_ITEM_DISABLED : 0);
            item(m,"QuickEdit on/off",NULL,SH_ACT_DOS_QUICKEDIT,owner,blocked ? SH_ITEM_DISABLED : 0);
            item(m,"Exclusive mode on/off",NULL,SH_ACT_DOS_EXCLUSIVE,owner,blocked ? SH_ITEM_DISABLED : 0);
        }
        separator(m);
        item(m, "Close    Alt+F4", "process-stop", SH_ACT_TASK_CLOSE, owner, minimized ? 0 : SH_ITEM_DEFAULT);
        break;
    }
    case SH_MENU_TASKBAR: {
        /* The Windows 98 taskbar menu (Toolbars belongs to the taskbar
         * customisation, G20): the arrangements act on the windows that have
         * a task button; after one, the menu offers its Undo. */
        int arrangeable=0, minimizable=0, undo=desk_arrange_undo_kind(d);
        for (int i=0;i<DESK_CLIENTS;i++) { arrangeable+=desk_arrangeable(d,i); minimizable+=desk_minimizable(d,i); }
        unsigned none=arrangeable ? 0 : SH_ITEM_DISABLED;
        item(m,"Cascade Windows",NULL,SH_ACT_CASCADE,0,none);
        item(m,"Tile Windows Horizontally",NULL,SH_ACT_TILE_H,0,none);
        item(m,"Tile Windows Vertically",NULL,SH_ACT_TILE_V,0,none);
        if (undo==DESK_ARRANGE_MINIMIZE_ALL) item(m,"Undo Minimize All",NULL,SH_ACT_ARRANGE_UNDO,0,0);
        else {
            item(m,"Minimize All Windows",NULL,SH_ACT_MINIMIZE_ALL,0,minimizable ? 0 : SH_ITEM_DISABLED);
            if (undo==DESK_ARRANGE_CASCADE) item(m,"Undo Cascade",NULL,SH_ACT_ARRANGE_UNDO,0,0);
            else if (undo==DESK_ARRANGE_TILE_H || undo==DESK_ARRANGE_TILE_V) item(m,"Undo Tile",NULL,SH_ACT_ARRANGE_UNDO,0,0);
        }
        separator(m);
        item(m,"Properties","document-properties",SH_ACT_TASKBAR_PROPERTIES,0,program_index(d,"control")<0 ? SH_ITEM_DISABLED : 0);
        item(m,"Toolbars",NULL,SH_ACT_SUBMENU,SH_MENU_TOOLBARS,0);
        break;
    }
    case SH_MENU_POWER: {   /* G11: "click the icon to change Power Schemes on-the-fly" */
        int active=pp_active(&d->sh.power_cfg,&d->sh.power_summary);
        for (unsigned k=0;k<d->sh.power_cfg.count;k++)
            item(m,d->sh.power_cfg.schemes[k].name,(int)k==active ? "battery" : NULL,SH_ACT_POWER_SCHEME,(int)k,(int)k==active ? SH_ITEM_DEFAULT : 0);
        break;
    }
    case SH_MENU_POWER_METER: {
        unsigned off=program_index(d,"control")<0 ? SH_ITEM_DISABLED : 0;
        item(m,"Open Power Meter","battery",SH_ACT_POWER_METER,0,off|SH_ITEM_DEFAULT);
        item(m,"Adjust Power Properties","preferences-system",SH_ACT_CONTROL,CP_POWER+1,off);
        break;
    }
    case SH_MENU_TOOLBARS:
        item(m,d->sh.cfg.quick_launch ? "Quick Launch (on)" : "Quick Launch",NULL,SH_ACT_TOOLBAR,0,0);
        item(m,d->sh.cfg.toolbar_address ? "Address (on)" : "Address",NULL,SH_ACT_TOOLBAR,1,0);
        item(m,d->sh.cfg.toolbar_links ? "Links (on)" : "Links",NULL,SH_ACT_TOOLBAR,2,0);
        item(m,d->sh.cfg.toolbar_desktop ? "Desktop (on)" : "Desktop",NULL,SH_ACT_TOOLBAR,3,0);
        for (unsigned k=0;k<CFG_TOOLBARS;k++) if (d->sh.cfg.toolbar_folder[k][0])
            item(m,custom_basename(d->sh.cfg.toolbar_folder[k]),"folder",SH_ACT_TOOLBAR,4+(int)k,0);
        separator(m); item(m,"New Toolbar...","folder-new",SH_ACT_NEW_TOOLBAR,0,0);
        break;
    case SH_MENU_TOOLBAR_CONTEXT:
        item(m,"Close",NULL,SH_ACT_TOOLBAR,d->sh.custom_menu_root,0);
        if (d->sh.custom_menu_root!=1) item(m,"Open Folder","folder",SH_ACT_TOOLBAR_EXPLORE,d->sh.custom_menu_root,0);
        break;
    case SH_MENU_TOOLBAR_ALL:
        if (d->sh.cfg.toolbar_address) item(m,"Address",NULL,SH_ACT_ADDRESS,0,0);
        if (d->sh.cfg.quick_launch) item(m,"Quick Launch",NULL,SH_ACT_SUBMENU,SH_MENU_FOLDER_BASE+SH_ROOT_QUICK,0);
        if (d->sh.cfg.toolbar_links) item(m,"Links",NULL,SH_ACT_SUBMENU,SH_MENU_FOLDER_BASE+SH_ROOT_LINKS,0);
        if (d->sh.cfg.toolbar_desktop) item(m,"Desktop",NULL,SH_ACT_SUBMENU,SH_MENU_FOLDER_BASE+SH_ROOT_DESKTOP,0);
        for (unsigned k=0;k<CFG_TOOLBARS;k++) if (d->sh.cfg.toolbar_folder[k][0])
            item(m,custom_basename(d->sh.cfg.toolbar_folder[k]),"folder",SH_ACT_SUBMENU,SH_MENU_FOLDER_BASE+SH_ROOT_CUSTOM+(int)k,0);
        break;
    case SH_MENU_START_ITEM:
        item(m,"Open",NULL,SH_ACT_START_OPEN,0,SH_ITEM_DEFAULT);
        item(m,"Delete",NULL,SH_ACT_START_DELETE,0,0);
        item(m,"Rename",NULL,SH_ACT_START_RENAME,0,0);
        item(m,"Explore",NULL,SH_ACT_START_EXPLORE,0,0);
        item(m,"Properties","document-properties",SH_ACT_START_PROPERTIES,0,0);
        break;
    default:
        if (kind>=SH_MENU_FOLDER_BASE && kind<SH_MENU_FOLDER_BASE+SH_FOLDERS) {
            custom_folder_items(d,m,kind-SH_MENU_FOLDER_BASE);
            if (!m->count) item(m,"(Empty)",NULL,SH_ACT_NONE,0,SH_ITEM_DISABLED);
        }
        if (kind>=SH_MENU_SETTINGS_CATEGORY && kind<=SH_MENU_SETTINGS_LAST)
            for (unsigned k=0;k<CP_APPLETS;k++) if (cp_applets[k].category==kind-SH_MENU_SETTINGS_CATEGORY)
                item(m,cp_applets[k].label,cp_applets[k].icon,SH_ACT_CONTROL,cp_applets[k].page+1,program_index(d,"control")<0 ? SH_ITEM_DISABLED : 0);
        break;
    }
    assign_marks(m);
}
static int32_t item_y(const struct sh_menu *m, int index)
{
    int32_t y = m->box.y + 3 + (m->scroll_max ? 12 : 0) - m->scroll;
    for (int k = 0; k < index && k < m->count; k++) y += m->items[k].flags & SH_ITEM_SEPARATOR ? SH_SEPARATOR : m->row_h;
    return y;
}
static int32_t item_h(const struct sh_menu *m, int index)
{ return m->items[index].flags & SH_ITEM_SEPARATOR ? SH_SEPARATOR : m->row_h; }
static void layout(struct desktop *d, struct sh_menu *m, int32_t x, int32_t y, int above)
{
    struct desk_theme role; const struct desk_theme *saved=desk_theme_role(&role,AP_MENU);
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
    if (h>limit) { m->scroll_max=h-limit+24;h=limit; }
    if (above) y -= h;
    if (x + w > (int32_t)d->width) x = (int32_t)d->width - w;
    if (y + h > limit) y = limit - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    m->box = (struct desk_box){x, y, w, h};
    desk_theme=saved;
}
void desk_shell_custom_updated(struct desktop *d)
{
    int refresh=d->sh.custom_refresh;
    for (int l=0;l<d->sh.levels;l++) {
        struct sh_menu *m=&d->sh.menus[l];struct desk_box b=m->box;int kind=m->kind,hot=m->hot;
        damage(d,outer(b));build(d,m,kind);layout(d,m,b.x,b.y,0);
        if (hot<m->count) m->hot=hot;
        damage(d,outer(m->box));
    }
    d->sh.custom_refresh=refresh;
    damage(d,desk_taskbar(d));
}
static void menus_damage(struct desktop *d)
{
    for (int l = 0; l < d->sh.levels; l++) damage(d, outer(d->sh.menus[l].box));
}
void desk_menu_open(struct desktop *d, int kind, int32_t x, int32_t y)
{
    d->sh.start_keyboard = 0;
    if (kind==SH_MENU_START || kind>=SH_MENU_FOLDER_BASE) d->sh.custom_refresh=1;
    desk_v3_revoke(d); desk_v3_pointer_release(d);
    tooltip_hide(d);
    if (d->sh.icon_selected>=0) damage(d,d->sh.icons[d->sh.icon_selected].box);
    desk_sound(d,SOUND_MENU_OPEN);
    menus_damage(d);
    d->sh.levels = 1;
    build(d, &d->sh.menus[0], kind);
    d->sh.menus[0].opened=desk_appearance_active && (desk_appearance.effects&AP_MENU_ANIMATION) ? d->now_ns : 0;
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
    /* Dismissing the right-drag menu is its Cancel. */
    if (d->sh.dnd.active && d->sh.dnd.menu == 1 && d->sh.menus[0].kind == SH_MENU_DRAG) {
        menus_damage(d); d->sh.levels = 0;
        desk_shell_drag_cancel(d);
        return;
    }
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
    if (m->scroll_max && y<m->box.y+15) return -3;
    if (m->scroll_max && y>=m->box.y+m->box.h-15) return -4;
    for (int k = 0; k < m->count; k++) {
        int32_t top = item_y(m, k);
        if (y >= top && y < top + item_h(m, k))
            return m->items[k].flags & (SH_ITEM_SEPARATOR | SH_ITEM_DISABLED) ? -2 : k;
    }
    return -2;
}
static void menu_scroll(struct desktop *d,struct sh_menu *m,int delta)
{
    int scroll=m->scroll+delta;
    if (scroll<0) scroll=0;
    if (scroll>m->scroll_max) scroll=m->scroll_max;
    if (scroll!=m->scroll) { m->scroll=scroll;damage(d,m->box); }
}
static void menu_highlight(struct desktop *d,struct sh_menu *m,int hot)
{
    if (hot>=0 && hot<m->count && m->scroll_max) {
        int y=item_y(m,hot),top=m->box.y+15,bottom=m->box.y+m->box.h-15;
        if (y<top) menu_scroll(d,m,y-top);
        else if (y+item_h(m,hot)>bottom) menu_scroll(d,m,y+item_h(m,hot)-bottom);
    }
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
    desk_launch_origin(d, DESK_LAUNCH_GAME, desk_games[index].title);
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
static void desktop_refresh(struct desktop *d)
{
    d->v3_links_force|=DESK_LINKS_SCAN; d->v3_links_next=0; desk_links_refresh(d);
    if (d->sh.folder) d->sh.folder->command=DF_REFRESH;
    d->sh.background_valid=0; damage_all(d);
}
static void act(struct desktop *d, const struct sh_item *it)
{
    char argument[24];
    switch (it->action) {
    case SH_ACT_FIND: desk_shell_find(d, 0); break;
    case SH_ACT_FOLDER_EXPLORE:
        if (d->sh.custom && it->arg>=0 && (unsigned)it->arg<d->sh.custom->folders_count) launch_named(d,"files",d->sh.custom->folders[it->arg]);
        break;
    case SH_ACT_FOLDER_OPTIONS: launch_named(d,"files","--folder-options"); break;
    case SH_ACT_SHELL_ITEM:
        if (d->sh.custom && it->arg>=0 && (unsigned)it->arg<d->sh.custom->count)
            snprintf(d->sh.custom_open,sizeof(d->sh.custom_open),"%s",d->sh.custom->entries[it->arg].path);
        break;
    case SH_ACT_TOOLBAR: {
        int *flags[]={&d->sh.cfg.quick_launch,&d->sh.cfg.toolbar_address,&d->sh.cfg.toolbar_links,&d->sh.cfg.toolbar_desktop};
        if (it->arg>=0 && it->arg<4) *flags[it->arg]=!*flags[it->arg];
        else if (it->arg>=4 && it->arg<4+CFG_TOOLBARS) d->sh.cfg.toolbar_folder[it->arg-4][0]=0;
        d->sh.want_save=1; d->sh.custom_refresh=1; damage_all(d); break;
    }
    case SH_ACT_TOOLBAR_EXPLORE: {
        const char *path=it->arg==0 ? SH_QUICK_FOLDER : it->arg==2 ? SH_LINKS_FOLDER : it->arg==3 ? SH_DESKTOP_FOLDER : it->arg>=4 && it->arg<4+CFG_TOOLBARS ? d->sh.cfg.toolbar_folder[it->arg-4] : NULL;
        if (path) launch_named(d,"files",path);
        break;
    }
    case SH_ACT_ADDRESS: d->sh.address_focus=1;d->sh.bar_hidden=0;desk_focus(d,-1);damage(d,desk_taskbar(d));break;
    case SH_ACT_NEW_TOOLBAR: custom_prompt(d,2,"/"); break;
    case SH_ACT_START_OPEN: case SH_ACT_START_DELETE: case SH_ACT_START_RENAME: case SH_ACT_START_EXPLORE: case SH_ACT_START_PROPERTIES:
        if (d->sh.custom && d->sh.custom_item>=0 && (unsigned)d->sh.custom_item<d->sh.custom->count) {
            const struct sh_folder_item *e=&d->sh.custom->entries[d->sh.custom_item];
            if (it->action==SH_ACT_START_OPEN) snprintf(d->sh.custom_open,sizeof(d->sh.custom_open),"%s",e->path);
            else if (it->action==SH_ACT_START_DELETE) custom_request(d,SH_EDIT_DELETE,e->path,NULL,NULL);
            else if (it->action==SH_ACT_START_RENAME) { strcpy(d->sh.prompt_path,e->path);custom_prompt(d,3,e->label); }
            else if (it->action==SH_ACT_START_PROPERTIES) {
                int game=-1;for (unsigned k=0;k<DESK_GAME_COUNT;k++) if (!strcmp(e->label,desk_games[k].label)) game=(int)k;
                if (game>=0) desk_game_properties(d,(unsigned)game+1);
                else { char arg[CFG_PATH_MAX+16];snprintf(arg,sizeof(arg),"--properties=%s",e->path);launch_named(d,"files",arg); }
            } else launch_named(d,"files",d->sh.custom->folders[e->parent]);
        }
        break;
    case SH_ACT_LAUNCH: d->action = it->arg; break;
    case SH_ACT_ABOUT: desk_about(d); break;
    case SH_ACT_HELP: launch_named(d, "help", NULL); break;
    case SH_ACT_EMPTY_RECYCLE: desk_folder_empty_request(d); break;
    case SH_ACT_RUN: desk_internal_open(d, DESK_INTERNAL_RUN); break;
    case SH_ACT_LOGOFF: d->sh.want_power=3; break;
    case SH_ACT_LOCK: desk_session_lock(d); break;
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
        if (desk_link_launch(d,(unsigned)it->arg)<0) desk_shortcut_properties(d,(unsigned)it->arg,1);
        break;
    case SH_ACT_PROGRAMS_FOLDER: desk_internal_open(d,DESK_INTERNAL_PROGRAMS); break;
    case SH_ACT_TASKMAN: launch_named(d, "tasks", NULL); break;
    case SH_ACT_POWER_SCHEME: desk_shell_power_scheme(d, it->arg); break;
    case SH_ACT_POWER_METER: launch_named(d, "control", "--panel=power-meter"); break;
    case SH_ACT_GAME: launch_game(d, it->arg); break;
    case SH_ACT_KEYS:
        desk_shell_message(d, "Keyboard Shortcuts",
            "Win or Ctrl+Esc      Open the Start menu\n"
            "Win+D                Show the desktop; again, bring the windows back\n"
            "Win+M, Shift+Win+M   Minimize all windows, undo\n"
            "Win+E, Win+R         Open Computer, Run\n"
            "Win+F, Win+Ctrl+F    Find files or folders, find a computer\n"
            "Win+Break            System properties\n"
            "Win+Tab              Move through the taskbar buttons\n"
            "Alt+Tab              Switch between windows (add Shift to go back)\n"
            "Alt+Esc, Alt+F6      Next window, next window of the same program\n"
            "Alt+Space            Open the window menu\n"
            "Alt+F4               Close the window; on the desktop, Shut Down\n"
            "Alt+M                On the desktop or taskbar, minimize all windows\n"
            "Shift+F10, Menu key  Open the context menu of the selection\n"
            "Enter                Open the selected icon\n"
            "Arrows               Move in menus and icons\n"
            "Esc                  Close a menu", 0);
        break;
    case SH_ACT_CASCADE: (void)desk_arrange(d, DESK_ARRANGE_CASCADE); break;
    case SH_ACT_TILE_H: (void)desk_arrange(d, DESK_ARRANGE_TILE_H); break;
    case SH_ACT_TILE_V: (void)desk_arrange(d, DESK_ARRANGE_TILE_V); break;
    case SH_ACT_MINIMIZE_ALL: (void)desk_minimize_all(d); break;
    case SH_ACT_ARRANGE_UNDO: (void)desk_arrange_undo(d); break;
    /* G20 routes the taskbar context-menu Properties command to its dedicated
     * Taskbar & Start Menu Control Panel page. */
    case SH_ACT_TASKBAR_PROPERTIES: launch_named(d, "control", "--panel=taskbar"); break;
    case SH_ACT_ICON_SORT:
        if (it->arg>=0 && it->arg<=3) {
            d->sh.cfg.icon_sort=it->arg; d->sh.want_save=1; d->sh.icon_selected=-1; d->sh.icon_count=0;
            desk_shell_apply(d);
        }
        break;
    case SH_ACT_ICON_AUTO:
        d->sh.cfg.icon_auto_arrange=!d->sh.cfg.icon_auto_arrange;
        if (d->sh.cfg.icon_auto_arrange) cfg_icons_arrange_at(&d->sh.cfg,d->width,d->height);
        d->sh.want_save=1; d->sh.icon_selected=-1; d->sh.icon_count=0; desk_shell_apply(d);
        break;
    case SH_ACT_ARRANGE: {
        if (!it->arg) cfg_icons_arrange_at(&d->sh.cfg,d->width,d->height);
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
                cfg_icon_move_at(&d->sh.cfg,icon_position_name(d,k),d->width,d->height,8+(slot/rows)*cw,DESK_BAR+10+(slot%rows)*ch);
            }
        }
        d->sh.want_save=1;
        d->sh.icon_selected = -1;
        desk_shell_apply(d);
        break;
    }
    case SH_ACT_ICONS_SHOW:
        if (d->sh.dnd.active) desk_shell_drag_cancel(d);
        d->sh.band=0; d->sh.icon_pending=0; d->sh.icon_editing=-1;
        if (d->sh.folder) d->sh.folder->editing=-1;
        d->sh.cfg.desktop_icons=!d->sh.cfg.desktop_icons; d->sh.want_save=1;
        d->sh.background_valid=0; damage_all(d); break;
    case SH_ACT_REFRESH:
        desktop_refresh(d);
        break;
    case SH_ACT_RENAME:
        if (icon_is_entry(d,it->arg)) desk_folder_rename(d,it->arg);
        else if ((it->arg>=0 && it->arg<3) || network_icon(d,it->arg)) desktop_icon_rename_begin(d,it->arg);
        break;
    case SH_ACT_FILE:
        if (d->sh.folder) {
            d->sh.folder->command=it->arg; d->sh.folder->index=d->sh.menu_owner-3;
            if (it->arg==DF_CUT || it->arg==DF_COPY || it->arg==DF_DELETE || it->arg==DF_SHORTCUT) d->sh.folder->multi=selection_sync(d)>1;
            d->sh.folder->command_flags=0;
        }
        break;
    case SH_ACT_DRAG:
        if (d->sh.dnd.active && d->sh.dnd.menu) dnd_commit(d,(unsigned)it->arg);
        break;
    case SH_ACT_FILES_ACTION: {
        const char *path=icon_is_entry(d,d->sh.menu_owner)?d->sh.icons[d->sh.menu_owner].argument:"";
        char argument[DESK_ARG_MAX+1];
        if (!it->arg) { launch_named(d,"files",path); break; }
        if (it->arg==FILES_NEW_SHORTCUT || it->arg==FILES_NEW_REGISTERED) path="";
        char manifest[FM_PATH]=""; int selected=selection_sync(d);
        if (selected>1 && (it->arg==FILES_COPY_TO || it->arg==FILES_MOVE_TO || it->arg==FILES_SEND_TO)) {
            struct fm_model *model=&d->sh.folder->model;
            char **paths=calloc((unsigned)selected,sizeof(*paths)); unsigned used=0; int e=paths?0:-ENOMEM;
            for (unsigned i=0;!e && i<model->count;i++) if (model->entries[i].selected) {
                paths[used]=malloc(FM_PATH); if (!paths[used]) e=-ENOMEM;
                else e=fm_entry_path(model,i,paths[used]);
                used++;
            }
            if (!e) e=fm_shell_selection_write(&model->fs,paths,used,manifest);
            fm_drag_free(paths,NULL,used);
            if (e) { desk_shell_message(d,"Files","The selected items could not be passed to Files.",1); break; }
            path=manifest;
        }
        int n=snprintf(argument,sizeof(argument),"--file-action=%d:%s%s",it->arg,manifest[0]?"@":"",path);
        if (n<0 || (unsigned)n>=sizeof(argument)) desk_shell_message(d,"Files","This path is too long for the desktop launch channel.",1);
        else launch_named(d,"files",argument);
        break;
    }
    case SH_ACT_OPEN_WITH:
        if (icon_is_entry(d, d->sh.menu_owner))
            launch_named(d,it->arg<0 ? "ciuknote" : desk_catalog[it->arg].name,d->sh.icons[d->sh.menu_owner].argument);
        break;
    case SH_ACT_ICON_OPEN: icon_open(d, it->arg); break;
    case SH_ACT_ICON_PROPERTIES:
        if (it->arg==0 && d->sh.icon_count>0 && !strcmp(d->sh.icons[0].argument,"Computer")) { launch_named(d,"control","--panel=system"); break; }   /* My Computer */
        if (network_icon(d,it->arg)) { launch_named(d,"control","--panel=network"); break; }
        if (it->arg==2 && d->sh.icon_count>2) { launch_named(d,"files","recycle:properties"); break; }   /* G2 */
        if (it->arg>=0 && it->arg<d->sh.icon_count) {
            const struct sh_icon *ic=&d->sh.icons[it->arg];
            if (ic->action==SH_ACT_WIN32_LINK) { desk_shortcut_properties(d,(unsigned)strtoul(ic->argument,NULL,10),0); break; }
            if (!strcmp(ic->program,"dosbox")) for (unsigned k=0;k<DESK_GAME_COUNT;k++)
                if (strstr(ic->argument,desk_games[k].command)) {
                    char arg[24];snprintf(arg,sizeof(arg),"--game=%u",k+1);
                    launch_named(d,"control",arg);return;
                }
        }
        if (icon_is_entry(d, it->arg)) {
            char argument[FM_PATH+16];
            snprintf(argument,sizeof(argument),"--properties=%s",d->sh.icons[it->arg].argument);
            launch_named(d,"files",argument); break;
        }
        if (it->arg>=0 && it->arg<d->sh.icon_count && !strcmp(d->sh.icons[it->arg].program,"files")) {
            char argument[FM_PATH+16];
            const char *path=d->sh.icons[it->arg].argument;
            if (path[0]) { snprintf(argument,sizeof(argument),"--properties=%s",path); launch_named(d,"files",argument); break; }
        }
        if (it->arg >= 0 && it->arg < d->sh.icon_count) {
            const struct sh_icon *ic = &d->sh.icons[it->arg];
            char text[200];
            if (ic->action == SH_ACT_LAUNCH)
                snprintf(text, sizeof(text), "%.60s\n\nType: Shortcut\nTarget: /Programs/%.24s%s%.60s", ic->label, ic->program,
                         ic->argument[0] ? " " : "", ic->argument);
            else snprintf(text, sizeof(text), "%.60s\n\nType: System folder", ic->label);
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
    case SH_ACT_DOS_MARK: case SH_ACT_DOS_COPY: case SH_ACT_DOS_PASTE: case SH_ACT_DOS_QUICKEDIT: case SH_ACT_DOS_EXCLUSIVE:
        if (it->arg>=0 && it->arg<DESK_CLIENTS && d->clients[it->arg].dosbox) {
            desk_focus(d,it->arg); struct ciuki_message message;
            desk_message(&message,DESK_DOS_CLIPBOARD,8); message.data[0]=2;
            desk_put32(message.data+4,it->action==SH_ACT_DOS_MARK ? 1 : it->action==SH_ACT_DOS_COPY ? 2 : it->action==SH_ACT_DOS_PASTE ? 3 : it->action==SH_ACT_DOS_QUICKEDIT ? 4 : 5);
            desk_queue(d,it->arg,&message);
        }
        break;
    case SH_ACT_DOS_FULLSCREEN:
        if (it->arg>=0 && it->arg<DESK_CLIENTS && d->clients[it->arg].dosbox) {
            if (d->clients[it->arg].minimized) desk_minimize(d,it->arg);
            desk_focus(d,it->arg);
            struct ciuki_message message;
            desk_message(&message,DESK_DOS_FULLSCREEN,4); message.data[0]=DESK_SHELL_VERSION;
            desk_queue(d,it->arg,&message);
        }
        break;
    case SH_ACT_MAGNIFIER:
        if (desk_access_magnifier_set) desk_access_magnifier_set(d, !desk_access_magnifier_on(d));
        break;
    case SH_ACT_CLEAR_RECENT: custom_request(d,SH_EDIT_CLEAR,NULL,NULL,NULL); break;
    default: break;
    }
}
static void activate(struct desktop *d, int level, int index)
{
    struct sh_item it = d->sh.menus[level].items[index];
    if (it.flags & SH_ITEM_DISABLED) return;
    desk_sound(d,SOUND_MENU_SELECT);
    if (it.action == SH_ACT_SUBMENU) { submenu(d, level, index); return; }
    if (it.action == SH_ACT_DRAG && d->sh.dnd.menu) d->sh.dnd.menu = 2;   /* a choice, not a dismissal */
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
/* SM-9: the underlined access key of an item. */
static void mark_line(struct du_canvas *c, int32_t tx, int32_t ty, const struct sh_item *it, int bold_face, uint32_t colour)
{
    if (!it->mark) return;
    int font = bold_face ? desk_theme->font_bold : desk_theme->font_ui;
    int at = desk_text_measure_n(font, it->label, (size_t)(it->mark - 1));
    size_t used; unsigned ch = desk_utf8_decode(it->label + it->mark - 1, &used);
    int w = desk_text_advance(font, ch) - 1;
    du_hline(c, tx + at, ty + DU_GLYPH_H + 2, w > 1 ? w : 1, colour);
}
static void check_mark(struct du_canvas *, int, int, uint32_t);
static void draw_menu(struct desktop *d, const struct sh_menu *m, struct desk_box clip)
{
    struct desk_box b;
    if (!desk_intersect(outer(m->box), clip, &b)) return;
    if(m->opened && d->now_ns>=m->opened && d->now_ns-m->opened<120000000ull) {
        int visible=(int)((d->now_ns-m->opened)/20000000ull+1)*m->box.h/6;
        struct desk_box reveal={m->box.x,m->box.y,m->box.w,visible};
        if(!desk_intersect(b,reveal,&b)) return;
    }
    struct desk_theme role; const struct desk_theme *saved=desk_theme_role(&role,AP_MENU);
    struct du_canvas c = canvas(d, b);
    popup_face(d, &c, m->box);
    if(desk_appearance_active) du_fill(&c,(struct du_rect){m->box.x+1,m->box.y+1,m->box.w-2,m->box.h-2},desk_appearance.item[AP_MENU].colour);
    int32_t banner = m->kind == SH_MENU_START ? SH_BANNER : 0;
    if (banner) draw_banner(&c, (struct desk_box){m->box.x + 2, m->box.y + 2, banner + 1, m->box.h - 4});
    int platinum = desk_theme->control_style == DESK_CONTROLS_PLATINUM;
    int32_t x0 = m->box.x + 3 + banner, w = m->box.w - 6 - banner;
    if (m->scroll_max) {
        du_triangle(&c,x0+w/2,m->box.y+8,0,3,m->scroll ? SH_INK : DU_GRAY);
        du_triangle(&c,x0+w/2,m->box.y+m->box.h-8,1,3,m->scroll<m->scroll_max ? SH_INK : DU_GRAY);
    }
    c.clip=du_intersection(c.clip,(struct du_rect){x0,m->box.y+3+(m->scroll_max ? 12 : 0),w,m->box.h-6-(m->scroll_max ? 24 : 0)});
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
        const uint32_t *ic = it->action==SH_ACT_WIN32_LINK && d->v3_link_icon && it->arg>=0 && (unsigned)it->arg<d->v3_link_count ?
            d->v3_link_icon(d,d->v3_links[it->arg].source,(unsigned)m->icon) : NULL;
        if (!ic) ic=desk_shell_icon(it->icon,m->icon);
        if (ic) du_icon_blit(&c, x0 + 6, y + (h - m->icon) / 2, ic, m->icon, it->flags & SH_ITEM_DISABLED ? DU_ICON_DISABLED : DU_ICON_NORMAL);
        if (it->flags & SH_ITEM_CHECKED)
            check_mark(&c,x0+6+(m->icon-7)/2,y+(h-7)/2,it->flags & SH_ITEM_DISABLED ? DU_GRAY : hot ? SH_HOT_INK : SH_INK);
        int32_t tx = x0 + 6 + m->icon + 10, ty = y + (h - text_h()) / 2;
        if (it->flags & SH_ITEM_DISABLED) {
            if (platinum) du_text(&c, tx, ty, it->label, DU_GRAY); else du_text_disabled(&c, tx, ty, it->label);
            mark_line(&c, tx, ty, it, 0, DU_GRAY);
        } else if (it->flags & SH_ITEM_DEFAULT) {
            bold(&c, tx, ty, it->label, hot ? SH_HOT_INK : SH_INK);
            mark_line(&c, tx, ty, it, 1, hot ? SH_HOT_INK : SH_INK);
        } else {
            du_text(&c, tx, ty, it->label, hot ? SH_HOT_INK : SH_INK);
            mark_line(&c, tx, ty, it, 0, hot ? SH_HOT_INK : SH_INK);
        }
        if (it->action == SH_ACT_SUBMENU) du_triangle(&c, x0 + w - 10, y + h / 2, 3, 4, hot ? SH_HOT_INK : SH_INK);
    }
    desk_theme=saved;
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
/* G1: the tray shows local time (the system zone's current offset). */
static uint64_t wall_seconds(const struct desktop *d) { return d->app_test ? 12 * 3600 : d->realtime_second + (uint64_t)(int64_t)d->local_offset; }
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
    /* fx-17b: floppy diskettes (A:/B:, bits 0-1) have no tray entry, as in Windows 98. */
    int resized = !!(d->sh.media.removable_mask & ~7u) != !!(state->removable_mask & ~7u);
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
    /* fx-21b: a USB device the kernel quarantined at enumeration. Windows
     * shows this notification-area balloon ("USB Device not recognized: One
     * of the devices attached to this computer has malfunctioned", Microsoft
     * KB 2654149) and advises unplugging and reconnecting the device; a
     * re-plug re-enumerates the port. One balloon per counted device,
     * including a device that failed before the desktop started. */
    unsigned seen=d->sh.media.version==CIUKI_MEDIA_VERSION ? d->sh.media.usb_unrecognized : 0;
    if (state->version==CIUKI_MEDIA_VERSION && state->usb_unrecognized>seen)
        desk_shell_notify(d,DESK_USB_UNRECOGNIZED_TITLE,DESK_USB_UNRECOGNIZED_TEXT,DU_DLG_ICON_WARNING,SH_NOTIFY_DETAILS);
    for (unsigned i=3;i<26;i++) if (!(state->removable_mask&(1u<<i)) ||
        state->generation[i]!=d->sh.media.generation[i]) d->sh.media_labels[i][0]=0;
    d->sh.media=*state; d->sh.media_scroll=0;
    if (!(state->removable_mask & ~7u)) d->sh.media_open=0;
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
    if (d->sh.tooltip>=256 && d->sh.tooltip<256+SH_PROGRAM_TRAY) {
        snprintf(text,size,"%s",d->sh.program_tray[d->sh.tooltip-256].tip); return;
    }
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
    if (d->sh.tooltip==6) {   /* G11: percent and time left, the scheme */
        const struct pp_config *c=&d->sh.power_cfg;
        pp_tooltip(&d->sh.power_summary,c->schemes[pp_active(c,&d->sh.power_summary)].name,text,size);
        return;
    }
    if (d->sh.tooltip==7) {      /* G30: the Resource Meter tooltip, as rm_tooltip */
        snprintf(text,size,"System Resources: %u%%\nUser Resources: %u%%\nGDI Resources: %u%%",d->sh.resources.system,d->sh.resources.user,d->sh.resources.gdi);
        return;
    }
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
    struct desk_theme role; const struct desk_theme *saved=desk_theme_role(&role,AP_TOOLTIP);
    char text[1024]; tooltip_text(d, text, sizeof(text));
    int32_t w = du_text_width(text) + 16;
    if (w>360) w=360;
    if (w>(int)d->width-8) w=(int)d->width-8;
    int h=du_text_wrap(NULL,(struct du_rect){0,0,w-16,0},text,0)*line_h()+10;
    if (h>(int)d->height-2*DESK_BAR-12) h=(int)d->height-2*DESK_BAR-12;
    struct desk_box anchor=d->sh.tooltip>=256 ? program_tray_box(d,(unsigned)d->sh.tooltip-256) : d->sh.tooltip>=16 ? desk_task_button(d,d->sh.tooltip-16) :
        d->sh.tooltip==2 ? network_icon_box(d) : d->sh.tooltip==3 ? volume_icon_box(d) :
        d->sh.tooltip==4 ? media_icon_box(d) : d->sh.tooltip==5 ? notification_icon_box(d) :
        d->sh.tooltip==6 ? desk_shell_tray_icon_box(d, SH_TRAY_POWER) :
        d->sh.tooltip==7 ? desk_shell_tray_icon_box(d, SH_TRAY_RESOURCES) : clock_box(d);
    struct desk_box result=tray_popup(d,anchor,w,h);
    desk_theme=saved; return result;
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
void desk_shell_resources_update(struct desktop *d, const struct sh_resources *now)
{
    struct sh_resources *old = &d->sh.resources;
    int shown = !!now->visible;
    if (!shown) {
        if (!old->visible) return;
        int previous = tray_box(d).w;
        old->visible = 0;
        if (d->sh.tooltip==7) tooltip_hide(d);
        desk_shell_tray_damage(d, previous);
        return;
    }
    if (old->visible && old->system==now->system && old->user==now->user && old->gdi==now->gdi && old->level==now->level) return;
    int previous = tray_box(d).w, was = old->visible;
    *old = *now; old->visible = 1;
    if (!was) { desk_shell_tray_damage(d, previous); return; }
    damage(d, desk_shell_tray_icon_box(d, SH_TRAY_RESOURCES));
    if (d->sh.tooltip==7) damage(d, tooltip_box(d));
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
    if (d->sh.cfg.taskbar_autohide && d->sh.bar_hidden) return;
    if (!desk_intersect(bar, clip, &b)) return;
    struct desk_box quick=desk_quick_launch_button(d), part;
    if (desk_intersect(quick,b,&part)) {
        struct du_canvas qc=canvas(d,part); struct desk_paint qp=painter(&qc);
        desk_paint_task(&qp,quick.x+1,quick.y,quick.w-2,quick.h,0,d->lfb_bytes_per_pixel==2);
        const uint32_t *icon=desk_shell_icon("user-desktop",16);
        if (icon) du_icon_blit(&qc,quick.x+(quick.w-16)/2,quick.y+(quick.h-16)/2,icon,16,DU_ICON_NORMAL);
    }
    draw_toolbars(d,b);
    /* Both themes align tasks from the left; the whole bar belongs to windows. */
    for (int i = 0; i < DESK_CLIENTS; i++) {
        struct desk_box button = desk_task_button(d, i), part;
        if (!desk_intersect(button, b, &part)) continue;
        struct du_canvas bc = canvas(d, part);
        struct du_rect r = {button.x + 1, button.y, button.w - 2, button.h};
        int active = (d->focus == i && !d->clients[i].minimized) != !!d->clients[i].flash;
        int hot=d->sh.hover_target==16+i;
        struct desk_paint p=painter(&bc);
        desk_paint_task(&p,r.x,r.y,r.w,r.h,
            (active ? DESK_TASK_ACTIVE : 0) | (hot ? DESK_TASK_HOT : 0) |
            (d->clients[i].minimized ? DESK_TASK_MINIMIZED : 0),d->lfb_bytes_per_pixel==2);
        if (button.w > 24) {
            int icon_size=desk_scale(16);
            const uint32_t *ic = desk_shell_icon(client_icon(d, i), icon_size);
            int dy = active ? 1 : 0;
            if (ic) du_icon_blit(&bc, button.x + desk_scale(5) + dy, button.y + (button.h-icon_size)/2 + dy, ic, icon_size, d->clients[i].minimized ? DU_ICON_GHOST : DU_ICON_NORMAL);
            const char *title = d->clients[i].closing ? "Closing..." : d->clients[i].unresponsive ? "Not responding" : d->clients[i].title;
            struct du_canvas tc = bc;
            tc.clip = du_intersection(bc.clip, (struct du_rect){button.x + desk_scale(24), button.y, button.w - desk_scale(28), button.h});
            int font = active ? t->font_bold : t->font_ui;
            p=painter(&tc);
            desk_paint_text_ellipsis(&p,font,button.x+desk_scale(25)+dy,button.y+(button.h+desk_font_cap(font))/2+dy,title,button.w-desk_scale(30)-dy,
                                    active ? t->task_active_text : t->taskbar_text);
        }
        /* KB-13: the button in keyboard focus carries the theme's focus cue. */
        if (d->sh.taskbar_key && d->sh.task_pick == i)
            du_focus_ring(&bc, (struct du_rect){button.x + 3, button.y + 3, button.w - 6, button.h - 6});
    }
}
static void bar_text(struct du_canvas *c, int x, int y, const char *text)
{
    const struct desk_theme *t=desk_theme;
    if (t->topbar_text>0x808080u)
        du_text(c,x,y+1,text,desk_mix(0,t->topbar,100));
    du_text(c,x,y,text,t->topbar_text);
}
/* ---- G11 power policy (docs/design/power-policy.md) ---------------------- */
int desk_shell_power_shown(const struct desktop *d)
{ return d->sh.power_status.version == CIUKI_POWER_VERSION && !d->sh.power_meter_hidden; }
static void power_tooltip_damage(struct desktop *d, struct desk_box previous)
{
    if (d->sh.tooltip != 6) return;
    if (d->sh.tooltip_painted) damage(d,d->sh.tooltip_last);
    damage(d,previous); damage(d,tooltip_box(d));
}
void desk_shell_power_config(struct desktop *d)
{
    int width = desk_shell_tray_box(d).w;
    struct desk_box tip = d->sh.tooltip == 6 ? tooltip_box(d) : (struct desk_box){0};
    (void)pp_load(&d->sh.power_cfg, d->sh.cfg.extra);
    d->sh.power_meter_hidden = !d->sh.power_cfg.show_meter;
    if (d->sh.tooltip == 6 && !desk_shell_power_shown(d)) tooltip_hide(d);
    desk_shell_tray_damage(d,width);
    power_tooltip_damage(d,tip);
    d->sh.power_monitor_seconds = pp_timer(&d->sh.power_cfg,&d->sh.power_summary,PP_MONITOR);
}
void desk_shell_power_scheme(struct desktop *d, int scheme)
{
    if (scheme < 0 || (unsigned)scheme >= d->sh.power_cfg.count) return;
    struct desk_box tip = d->sh.tooltip == 6 ? tooltip_box(d) : (struct desk_box){0};
    struct pp_config next = d->sh.power_cfg;
    next.active = scheme;
    if (pp_store(&next, d->sh.cfg.extra, sizeof(d->sh.cfg.extra))) {
        desk_shell_notify_error(d,"Power scheme not saved",ENOSPC); return;
    }
    d->sh.power_cfg = next; d->sh.want_save = 1;
    d->sh.power_monitor_seconds = pp_timer(&d->sh.power_cfg,&d->sh.power_summary,PP_MONITOR);
    power_tooltip_damage(d,tip);
}
/* Shut down (a G3 session) or Stand by where it is qualified. */
static void power_choice(struct desktop *d, int action, int force)
{
    action = pp_action_effective(action, d->sh.power_can_standby);
    if (action == PP_ACTION_NONE || d->sh.gate || desk_session_busy(d)) return;
    if (action == PP_ACTION_STANDBY) { if (!d->sh.want_standby) d->sh.want_standby = 2; return; }
    d->sh.want_power = 2; d->sh.want_power_force = force;
}
int desk_shell_power_update(struct desktop *d, const struct ciuki_power_status *p)
{
    if (!p || p->version != CIUKI_POWER_VERSION || p->size != sizeof(*p) ||
        p->battery_count > CIUKI_POWER_BATTERIES || p->zone_count > CIUKI_POWER_ZONES || p->reserved) return -1;
    int width = desk_shell_tray_box(d).w;
    struct desk_box tip = d->sh.tooltip == 6 ? tooltip_box(d) : (struct desk_box){0};
    uint32_t lid = d->sh.power_status.version == CIUKI_POWER_VERSION ? d->sh.power_status.lid_open : CIUKI_POWER_UNKNOWN;
    d->sh.power_status = *p; desk_shell_tray_damage(d,width);
    pp_summarize(p,&d->sh.power_summary);
    power_tooltip_damage(d,tip);
    /* G11: the power button (ACPI's coalesced request) and the lid run the
     * Advanced choices; Stand by falls back to Shut down where unqualified. */
    if (p->shutdown_requests != d->sh.power_seen) {
        d->sh.power_seen = p->shutdown_requests;
        if (!d->sh.gate && !d->power.state)
            power_choice(d,d->sh.power_cfg.power_button == PP_BUTTON_STANDBY ? PP_ACTION_STANDBY : PP_ACTION_SHUTDOWN,0);
    }
    if (lid == 1 && p->lid_open == 0 && d->sh.power_cfg.lid != PP_LID_NONE)
        power_choice(d,d->sh.power_cfg.lid == PP_LID_STANDBY ? PP_ACTION_STANDBY : PP_ACTION_SHUTDOWN,0);
    return 0;
}
#include "shell_tray.h"

static void power_alarm(struct desktop *d, int kind)
{
    const struct pp_alarm *a = kind == PP_ALARM_CRITICAL ? &d->sh.power_cfg.critical : &d->sh.power_cfg.low;
    const struct pp_summary *s = &d->sh.power_summary;
    int action = pp_action_effective(a->action, d->sh.power_can_standby);
    fprintf(stderr,"[desktop] power alarm=%s percent=%d seconds=%d action=%d\n",
            kind == PP_ALARM_CRITICAL ? "critical" : "low", s->percent, s->seconds, action);
    /* One sound per alarm: the message window plays its warning sound. */
    if (a->sound && !a->message) desk_sound(d, kind == PP_ALARM_CRITICAL ? SOUND_ERROR : SOUND_INFORMATION);
    if (a->message) {
        char level[40] = "low", text[256];
        if (s->percent >= 0 && s->seconds >= 0)
            snprintf(level,sizeof(level),"at %d%% (%d hr %02d min)",s->percent,s->seconds/3600,s->seconds/60%60);
        else if (s->percent >= 0) snprintf(level,sizeof(level),"at %d%%",s->percent);
        snprintf(text,sizeof(text),"The battery is %s.\n%s",level,
                 action == PP_ACTION_SHUTDOWN ? "CiukiOS will now shut down. Programs are asked to save your work first." :
                 action == PP_ACTION_STANDBY ? "CiukiOS will now stand by." :
                 "Connect the computer to AC power, or save your work and shut down.");
        desk_shell_message_class(d, kind == PP_ALARM_CRITICAL ? "Critical Battery Alarm" : "Low Battery Alarm", text, DU_DLG_ICON_WARNING);
    }
    if (kind == PP_ALARM_LOW) desk_power_broadcast(d, PP_PBT_BATTERYLOW, 0);
    power_choice(d, action, a->force);
}
void desk_shell_power_events(struct desktop *d, const struct ciuki_power_events *e, int query)
{
    struct desk_shell *sh = &d->sh;
    int could = sh->power_can_standby;
    sh->power_can_standby = query >= 0 && (query & CIUKI_POWER_CAN_STANDBY) &&
                            !(query & (CIUKI_POWER_STOPPED | CIUKI_POWER_UNCERTAIN));
    if (could != sh->power_can_standby) {
        int i = internal_find(d, DESK_INTERNAL_SHUTDOWN);
        if (i >= 0) damage(d, desk_outer(&d->clients[i]));   /* the Stand by button */
    }
    if (e && e->version == CIUKI_POWER_EVENTS_VERSION && e->size == sizeof(*e)) {
        if (!sh->power_events_valid) sh->power_alarm.battery_low_seen = e->battery_low;   /* no alarm for old events */
        else if (e->sleep_requests != sh->power_events.sleep_requests && !sh->gate) {
            /* APM standby/suspend requests (sleep key, lid, BIOS timers):
             * honoured where Stand by is qualified, otherwise rejected by the
             * kernel and reported once per burst. */
            if (sh->power_can_standby) power_choice(d, PP_ACTION_STANDBY, 0);
            else if (!sh->power_sleep_notice_ns || d->now_ns >= sh->power_sleep_notice_ns + 10000000000ull) {
                sh->power_sleep_notice_ns = d->now_ns ? d->now_ns : 1;
                desk_shell_notify(d,"Power","Stand by is not available on this computer.",DU_DLG_ICON_INFO,SH_NOTIFY_DETAILS);
            }
        }
        sh->power_events = *e; sh->power_events_valid = 1;
    }
    sh->power_monitor_seconds = pp_timer(&sh->power_cfg, &sh->power_summary, PP_MONITOR);
    if (sh->gate || d->power.state) return;
    int alarm = pp_alarm_step(&sh->power_alarm, &sh->power_cfg, &sh->power_summary,
                              sh->power_events_valid ? sh->power_events.battery_low : 0);
    if (alarm) power_alarm(d, alarm);
    if (pp_status_changed(&sh->power_broadcast, &sh->power_summary)) desk_power_broadcast(d, PP_PBT_POWERSTATUSCHANGE, 0);
}
/* WM_POWERBROADCAST queue: one broadcast at a time through the broker; a
 * status change already waiting is not queued twice. */
void desk_power_broadcast(struct desktop *d, uint32_t event, uint32_t lparam)
{
    struct desk_power_broadcast *b = &d->pbt;
    if (event == PP_PBT_POWERSTATUSCHANGE)
        for (unsigned i = 0; i < b->count; i++) if (b->queue[i].event == event) return;
    if (b->count == DESK_PBT_QUEUE) {   /* drop the oldest status change, never a sequence step */
        unsigned i = 0;
        while (i < b->count && b->queue[i].event != PP_PBT_POWERSTATUSCHANGE) i++;
        if (i == b->count) return;
        memmove(&b->queue[i], &b->queue[i + 1], (b->count - i - 1) * sizeof(b->queue[0])); b->count--;
    }
    b->queue[b->count++] = (struct desk_pbt){event, lparam};
}
void desk_power_reply(struct desktop *d, int i, uint32_t serial, uint32_t answer)
{
    struct desk_client *c=&d->clients[i];
    if (!d->pbt.running || serial!=d->pbt.serial || c->power_serial!=serial || !c->power_pending) return;
    if (answer==2) { c->power_pending=2; return; }
    c->power_pending=0;
    if (d->pbt.current.event==PP_PBT_QUERYSUSPEND) {
        if (!answer) d->pbt.denied=1;
        else c->power_granted=1;
    }
}
static int native_power_pending(struct desktop *d)
{
    int pending=0;
    for (int i=0;i<DESK_CLIENTS;i++) {
        struct desk_client *c=&d->clients[i];
        if (c->channel<0 || c->internal || !c->power_pending || c->power_serial!=d->pbt.serial) continue;
        uint64_t limit=d->pbt.current.event==PP_PBT_QUERYSUSPEND ? DESK_POWER_QUERY_NS : 2000000000ull;
        if ((c->power_pending==1 || d->pbt.current.event!=PP_PBT_QUERYSUSPEND) && d->now_ns>=d->pbt.started_ns+limit) {
            c->power_pending=0; c->power_granted=1;
        } else pending=1;
    }
    return pending;
}
static void broadcast_service(struct desktop *d)
{
    struct desk_power_broadcast *b=&d->pbt;
    struct desk_w32_status st={0};
    if (b->running) {
        if (b->w32_running) {
            int e=d->w32_power ? d->w32_power(d,DESK_W32_POWER_STATUS,0,0,&st) : -ENODEV;
            if (e || st.state!=DESK_W32_PENDING) {
                if (!e && st.state==DESK_W32_VETOED) b->denied=1;
                b->w32_running=0;
            }
        }
        int pending=native_power_pending(d);
        if (!b->denied && (b->w32_running || pending)) return;
        if (d->w32_power) (void)d->w32_power(d,DESK_W32_POWER_RESET,0,0,&st);
        b->w32_running=0; b->running=0;
        b->done=(struct desk_pbt){b->current.event+1,b->current.lparam};
        if (b->current.event==PP_PBT_QUERYSUSPEND || b->current.event==PP_PBT_SUSPEND) {
            b->sequence_done=b->current.event+1;
            if (b->current.event==PP_PBT_QUERYSUSPEND) b->sequence_denied=b->denied;
        }
    }
    if (!b->count) return;
    struct desk_pbt next=b->queue[0];
    memmove(&b->queue[0],&b->queue[1],(--b->count)*sizeof(b->queue[0]));
    b->current=next; b->started_ns=d->now_ns; b->broadcasts++; b->denied=0;
    if (!++b->serial) ++b->serial;
    for (int i=0;i<DESK_CLIENTS;i++) {
        struct desk_client *c=&d->clients[i];
        if (c->channel<0 || c->internal || !c->power_aware) continue;
        c->power_pending=0;
        if (next.event==PP_PBT_QUERYSUSPENDFAILED && !c->power_granted) continue;
        if (next.event==PP_PBT_QUERYSUSPEND) c->power_granted=0;
        struct ciuki_message m; desk_message(&m,DESK_POWER_EVENT,16);
        desk_put32(m.data+4,b->serial); desk_put32(m.data+8,next.event); desk_put32(m.data+12,next.lparam);
        c->power_serial=b->serial; c->power_pending=1; desk_queue(d,i,&m);
    }
    b->w32_running=d->w32_power && !d->w32_power(d,DESK_W32_POWER_BEGIN,next.event,next.lparam,&st);
    b->running=1;
}
static int broadcast_finished(const struct desktop *d, uint32_t event)
{ return d->pbt.sequence_done == event + 1; }
/* The Windows 98 Stand by sequence (power-policy.md 22): query, suspend,
 * call 73 STANDBY, resume. Native queries share the same veto policy. */
static void standby_service(struct desktop *d)
{
    struct desk_power_broadcast *b = &d->pbt;
    switch (b->standby) {
    case DESK_STANDBY_IDLE:
        if (!d->sh.want_standby) return;
        b->standby_lparam = d->sh.want_standby == 1;
        d->sh.want_standby = 0;
        if (!d->sh.power_can_standby || desk_session_busy(d) || d->sh.gate) return;
        fprintf(stderr,"[desktop] standby begin user=%d\n",b->standby_lparam);
        for (int i=0;i<DESK_CLIENTS;i++) if (d->clients[i].dosbox && d->clients[i].channel>=0) {
            desk_shell_notify(d,"Stand by","DOS sessions will pause while the computer is in Stand by.",DU_DLG_ICON_INFO,SH_NOTIFY_DETAILS);
            break;
        }
        b->sequence_done=0; b->sequence_denied=0;
        b->done = (struct desk_pbt){0}; b->standby = DESK_STANDBY_QUERY;
        desk_power_broadcast(d, PP_PBT_QUERYSUSPEND, (uint32_t)b->standby_lparam);
        return;
    case DESK_STANDBY_QUERY:
        if (!broadcast_finished(d, PP_PBT_QUERYSUSPEND)) return;
        if (b->sequence_denied || desk_session_busy(d) || d->sh.gate) {
            fprintf(stderr,"[desktop] standby denied by a program\n");
            if (d->power_cancel_standby) (void)d->power_cancel_standby();
            desk_power_broadcast(d, PP_PBT_QUERYSUSPENDFAILED, 0);
            b->standby = DESK_STANDBY_IDLE; return;
        }
        b->sequence_done=0;
        b->done = (struct desk_pbt){0}; b->standby = DESK_STANDBY_SUSPEND;
        desk_power_broadcast(d, PP_PBT_SUSPEND, 0);
        return;
    case DESK_STANDBY_SUSPEND: {
        if (!broadcast_finished(d, PP_PBT_SUSPEND)) return;
        errno = 0;
        int r = desk_session_busy(d) || d->sh.gate ? (errno=EBUSY,-1) :
            d->power_standby ? d->power_standby() : (errno = ENOSYS, -1);
        b->standby_error = r ? (errno ? errno : EIO) : 0; b->standbys++;
        fprintf(stderr,"[desktop] standby result=%d\n",r ? -b->standby_error : 0);
        uint32_t failure = r ? PP_PBTF_RESUMEFROMFAILURE : 0;
        desk_power_broadcast(d, PP_PBT_RESUMEAUTOMATIC, 0);
        if (r) { if (d->power_cancel_standby) (void)d->power_cancel_standby(); b->resume_pending=0; desk_power_broadcast(d, PP_PBT_RESUMESUSPEND, failure); }
        else { b->resume_pending=1; b->resume_input_ns=d->last_input_ns; }
        damage(d,(struct desk_box){0,0,d->width,d->height});
        d->last_input_ns = d->now_ns;   /* the idle timer restarts at resume */
        b->resume_input_ns=d->last_input_ns;
        b->standby = DESK_STANDBY_IDLE;
        if (r) desk_shell_notify_error(d, "Stand by failed", b->standby_error);
        return;
    }
    default: b->standby = DESK_STANDBY_IDLE; return;
    }
}
void desk_power_policy_tick(struct desktop *d)
{
    if (d->pbt.resume_pending && d->last_input_ns>d->pbt.resume_input_ns) {
        d->pbt.resume_pending=0;
        desk_power_broadcast(d,PP_PBT_RESUMESUSPEND,0);
    }
    if (!d->shell) return;
    /* System standby after: no input for the current source's timer; display
     * leases, sessions and the power sequence keep the computer awake. */
    uint32_t idle = pp_timer(&d->sh.power_cfg, &d->sh.power_summary, PP_STANDBY);
    if (idle && d->sh.power_can_standby && !d->sh.want_standby && d->pbt.standby == DESK_STANDBY_IDLE &&
        !d->sh.gate && !desk_session_busy(d) && !d->v3_lease_state && d->last_input_ns &&
        d->now_ns >= d->last_input_ns + (uint64_t)idle * 1000000000ull) d->sh.want_standby = 2;
    standby_service(d);
    broadcast_service(d);
    int step = d->pbt.standby;
    standby_service(d);
    if (step != d->pbt.standby && !d->pbt.running) broadcast_service(d);   /* the next step starts at once */
}

static void draw_tray(struct desktop *d, struct du_canvas *canvas)
{
    struct du_canvas c = *canvas;
    if(d->sh.cfg.layout_switch) {
        struct desk_box li=desk_shell_tray_icon_box(d,SH_TRAY_LANGUAGE);
        du_text(&c,li.x,li.y+12,d->sh.cfg.keyboard==CFG_KEYS_IT ? "IT" : "EN",SH_INK);
    }
    for (unsigned k=0;k<SH_PROGRAM_TRAY;k++) if (d->sh.program_tray[k].window) {
        struct desk_box b=program_tray_box(d,k);
        du_icon_blit(&c,b.x,b.y,d->sh.program_tray[k].pixels,16,DU_ICON_NORMAL);
    }
    int net_disabled=d->sh.network.state==CIUKI_NET_STOPPED && !strncmp(d->sh.network.reason,"disabled",9);
    const char *net_icon = !d->sh.network.present ? "network-error" :
                          !net_disabled && d->sh.network.link == CIUKI_NET_LINK_UP ? "network-wired" : "network-offline";
    struct desk_box ni = network_icon_box(d);
    du_icon_blit(&c, ni.x, ni.y, desk_shell_icon(net_icon, SH_TRAY_ICON_SIZE), SH_TRAY_ICON_SIZE,
                 d->sh.network.present && !net_disabled ? DU_ICON_NORMAL : DU_ICON_DISABLED);
    const uint32_t *speaker = desk_shell_icon(d->sh.muted ? "audio-volume-muted" : "audio-volume-high", SH_TRAY_ICON_SIZE);
    struct desk_box vi = volume_icon_box(d);
    du_icon_blit(&c, vi.x, vi.y, speaker, SH_TRAY_ICON_SIZE, d->sh.mixer_state < 0 ? DU_ICON_DISABLED : DU_ICON_NORMAL);   /* muted: Tango's red cross */
    if (d->sh.media.removable_mask & ~7u) {
        struct desk_box mi=media_icon_box(d);
        du_icon_blit(&c,mi.x,mi.y,desk_shell_icon("drive-removable-media",SH_TRAY_ICON_SIZE),SH_TRAY_ICON_SIZE,DU_ICON_NORMAL);
    }
    if (desk_access_indicator_draw && desk_shell_tray_icon_box(d, SH_TRAY_ACCESS).w) desk_access_indicator_draw(d, &c, desk_shell_tray_icon_box(d, SH_TRAY_ACCESS));
    struct desk_box notification = notification_icon_box(d);
    du_icon_blit(&c,notification.x,notification.y,desk_shell_icon("dialog-information",SH_TRAY_ICON_SIZE),SH_TRAY_ICON_SIZE,
                 d->sh.notification.active ? DU_ICON_NORMAL : DU_ICON_GHOST);
    if (desk_shell_power_shown(d)) {
        const struct ciuki_power_status *p = &d->sh.power_status;
        struct desk_box bi = desk_shell_tray_icon_box(d, SH_TRAY_POWER);
        uint64_t full = 0, remaining = 0; unsigned unit = CIUKI_POWER_UNKNOWN, present = 0; int known = p->battery_count > 0;
        for (unsigned i = 0; i < p->battery_count && i < CIUKI_POWER_BATTERIES; i++) {
            const struct ciuki_power_battery *b = &p->batteries[i];
            if (b->present == 0) continue; /* fx-18: an empty bay (APM CH bit 4) is not unknown charge */
            present++;
            if (b->present != 1 || b->unit > CIUKI_POWER_UNIT_PERCENT || !b->full || b->full == CIUKI_POWER_UNKNOWN || b->remaining == CIUKI_POWER_UNKNOWN ||
                (unit != CIUKI_POWER_UNKNOWN && unit != b->unit)) { known = 0; break; }
            unit = b->unit; full += b->full; remaining += b->remaining;
        }
        if (!present) known = 0;
        unsigned percent = known && full ? (unsigned)(remaining * 100 / full) : 0;
        if (percent > 100) percent = 100;
        du_icon_blit(&c,bi.x,bi.y,desk_shell_icon(known && percent < 10 ? "battery-caution" : "battery",SH_TRAY_ICON_SIZE),SH_TRAY_ICON_SIZE,
                     known ? DU_ICON_NORMAL : DU_ICON_DISABLED);
        /* AC is separate from charge: '+' connected, '-' on battery (or APM
         * backup power), '?' unknown. ACPI and APM records share this path. */
        du_text(&c,bi.x+9,bi.y+4,p->ac_online == 1 ? "+" : p->ac_online == 0 || p->ac_online == CIUKI_POWER_AC_BACKUP ? "-" : "?",
                desk_theme->topbar_text);
    }
    if (d->sh.resources.visible) {
        /* G30: Tango "applications-system" with a level bar: green, amber (low) or red (critical). */
        struct desk_box ri = desk_shell_tray_icon_box(d, SH_TRAY_RESOURCES);
        du_icon_blit(&c, ri.x, ri.y, desk_shell_icon("applications-system", 16), 16, DU_ICON_NORMAL);
        static const uint32_t level_colors[3] = {0x2e9a2eu, 0xe0a000u, 0xcc2222u};
        int level = d->sh.resources.level > 2 ? 2 : (int)d->sh.resources.level;
        int filled = (int)(d->sh.resources.system > 100 ? 100 : d->sh.resources.system) * SH_TRAY_ICON_SIZE / 100;
        du_fill(&c, (struct du_rect){ri.x, ri.y + SH_TRAY_ICON_SIZE - 3, SH_TRAY_ICON_SIZE, 3}, 0x202020u);
        if (filled) du_fill(&c, (struct du_rect){ri.x, ri.y + SH_TRAY_ICON_SIZE - 3, filled, 3}, level_colors[level]);
    }
    if (!d->sh.cfg.taskbar_clock) return;
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
    struct desk_theme role; const struct desk_theme *saved=desk_theme_role(&role,AP_TOOLTIP);
    struct du_canvas c = canvas(d, b);
    char text[1024]; tooltip_text(d, text, sizeof(text));
    du_fill(&c, R(t), desk_appearance_active ? desk_appearance.item[AP_TOOLTIP].colour : SH_TIP);
    du_outline(&c, R(t), SH_INK);
    du_text_wrap(&c,(struct du_rect){t.x+8,t.y+5,t.w-16,t.h-10},text,desk_appearance_active ? desk_appearance.item[AP_TOOLTIP].text : SH_INK);
    desk_theme=saved;
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
    program_balloon_dismiss(d,0x403);
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
#define SH_LABEL_FONT (desk_appearance_active ? desk_item_font(AP_ICON) : d->sh.cfg.icon_font)
#define ICON_CELL_W (d->sh.cfg.icon_spacing_x)
#define ICON_CELL_H (d->sh.cfg.icon_spacing_y)
#define SH_DESKTOP_ICON 48
#define LABEL_TOP 54                                   /* label block (halo included) in the cell */
#define LABEL_H (ICON_CELL_H - LABEL_TOP)
#define LABEL_ROOM (ICON_CELL_W - 12)                  /* widest line; the halo stays in the cell */
#define LABEL_BASE (desk_font(d->sh.cfg.icon_font)->ascent+1)                                  /* first baseline below LABEL_TOP */
#define LABEL_PITCH (desk_font_line(d->sh.cfg.icon_font)-1)
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
        if (y + desk_metric(AP_ICON_Y,ICON_CELL_H) > bottom && y > top) { y = top; x += desk_metric(AP_ICON_X,ICON_CELL_W); }
        int px=x,py=y;
        const char *name=icon_position_name(d, k);
        if (!d->sh.cfg.icon_auto_arrange && cfg_icon_position_at(&d->sh.cfg,name,d->width,d->height,&px,&py)) {
            if (px+desk_metric(AP_ICON_X,ICON_CELL_W)>(int)d->width) px=(int)d->width-desk_metric(AP_ICON_X,ICON_CELL_W);
            if (px<0) px=0;
            if (py+desk_metric(AP_ICON_Y,ICON_CELL_H)>bottom) py=bottom-desk_metric(AP_ICON_Y,ICON_CELL_H);
            if (py<top) py=top;
        }
        d->sh.icons[k].box = (struct desk_box){px, py, desk_metric(AP_ICON_X,ICON_CELL_W), desk_metric(AP_ICON_Y,ICON_CELL_H)};
        if (d->sh.cfg.icon_auto_arrange && cfg_icon_move_at(&d->sh.cfg,name,d->width,d->height,px,py)) d->sh.want_save=1;
        y += desk_metric(AP_ICON_Y,ICON_CELL_H);
    }
}
static int desktop_icon_compare(const struct fm_entry *a, const struct fm_entry *b, int mode)
{
    int c=0;
    if (mode==2) c=a->meta.size<b->meta.size ? -1 : a->meta.size>b->meta.size ? 1 : 0;
    else if (mode==3) c=a->meta.modified<b->meta.modified ? -1 : a->meta.modified>b->meta.modified ? 1 : 0;
    else if (mode==1) {
        enum fm_kind ka=fm_kind(a),kb=fm_kind(b); c=ka<kb ? -1 : ka>kb ? 1 : 0;
        if (!c) c=strcasecmp(strrchr(a->name,'.') ? strrchr(a->name,'.') : "",strrchr(b->name,'.') ? strrchr(b->name,'.') : "");
    }
    if (!c) c=strcasecmp(a->name,b->name);
    if (!c) c=strcmp(a->name,b->name);
    return c;
}
static void desktop_icon_sort(struct desktop *d)
{
    struct fm_model *m=d->sh.folder ? &d->sh.folder->model : NULL;
    if (!m || d->sh.cfg.icon_sort<0 || d->sh.cfg.icon_sort>3) return;
    for (unsigned i=1;i<m->count;i++) {
        struct fm_entry entry=m->entries[i]; unsigned at=i;
        while (at && desktop_icon_compare(&entry,&m->entries[at-1],d->sh.cfg.icon_sort)<0) {
            m->entries[at]=m->entries[at-1]; at--;
        }
        m->entries[at]=entry;
    }
}
static void icons_build(struct desktop *d)
{
    /* Indices change: a desktop-source drag and a band end here. */
    if (d->sh.dnd.active && d->sh.dnd.source < 0) desk_shell_drag_cancel(d);
    d->sh.dnd.resolved_icon=-1;
    d->sh.band = 0; d->sh.icon_pending = 0; d->sh.icon_editing=-1;
    d->sh.icon_count = 0; d->sh.icon_selected=-1; d->sh.focus_unselected=0; d->sh.anchor=-1;
    desktop_icon_sort(d);
    icon_add(d, "My Computer", "files", "Computer", "ciuki-computer", SH_ACT_LAUNCH, 1);
    if (d->sh.icon_count!=1) return;
    icon_add(d, "My Documents", "files", d->sh.profile.documents[0] ? d->sh.profile.documents : "/Documents", d->sh.cfg.document_icon ? "folder" : "user-home", SH_ACT_LAUNCH, 0);
    if (d->sh.icon_count!=2) return;
    icon_add(d, "Recycle Bin", "files", "Recycle Bin", d->sh.cfg.recycle_icon ? "edit-delete" : "user-trash", SH_ACT_LAUNCH, 0);
    if (d->sh.icon_count!=3) return;
    if (d->sh.folder && d->sh.folder->bin_full) d->sh.icons[2].icon = d->sh.cfg.recycle_icon ? "edit-delete" : "user-trash-full";   /* G2: the bin holds something */
    if (d->sh.folder) for (unsigned i=0;i<d->sh.folder->model.count;i++) {
        const struct fm_entry *e=&d->sh.folder->model.entries[i];
        char path[FM_PATH]; if (fm_join(path,d->sh.profile.desktop[0] ? d->sh.profile.desktop : DESK_FOLDER,e->name)) return;
        const struct desk_catalog_entry *cat=desk_catalog_file(path);
        icon_add(d,e->name,e->meta.type==FM_DIR ? "files" : cat ? cat->name : "ciuknote",path,
            e->meta.type==FM_DIR ? "folder" : strlen(e->name)>4 && fm_same_path(e->name+strlen(e->name)-4,".lnk") ? "emblem-symbolic-link" : cat ? cat->tango : "text-x-generic", SH_ACT_LAUNCH,0);
        if (d->sh.icon_count!=(int)i+4) return;
        d->sh.icons[i+3].selected=(uint8_t)!!e->selected;
    }
    /* Keep folder entries at index 3 + model index; other system icons follow. */
    if (d->pictures_exists) {
        char path[FM_PATH];
        if (!fm_join(path,d->sh.profile.documents[0] ? d->sh.profile.documents : "/Documents","Pictures"))
            icon_add(d,"Pictures","files",path,"folder",SH_ACT_LAUNCH,0);
    }
    icon_add(d,"Network Neighborhood","control","--panel=network","network-workgroup",SH_ACT_LAUNCH,0);
    for (int k=0;k<d->sh.icon_count;k++) {
        char override[FM_NAME];
        if (cfg_icon_label(&d->sh.cfg,icon_position_name(d,k),override,sizeof(override)))
            snprintf(d->sh.icons[k].label,sizeof(d->sh.icons[k].label),"%s",override);
    }
    d->sh.icon_selected = -1;
}
static int desktop_program_target(struct desktop *d, const char *path, const struct fm_meta *meta)
{
    if (meta->type!=FM_FILE) return 0;
    if (meta->exec) return 1;
    for (unsigned i=0;i<d->program_count;i++) if (!strcmp(path,d->programs[i].path)) return 1;
    const char *ext=strrchr(path,'.');
    if (ext && (!strcasecmp(ext,".exe") || !strcasecmp(ext,".com") || !strcasecmp(ext,".bat") || !strcasecmp(ext,".pif"))) return 1;
    if (!d->sh.folder) return 0;
    struct fm_vfs *fs=&d->sh.folder->model.fs;
    int fd=fs->open(fs->ctx,path,0); if (fd<0) return 0;
    uint8_t signature[4]; unsigned at=0;
    while (at<sizeof(signature)) { int n=fs->read(fs->ctx,fd,signature+at,sizeof(signature)-at); if (n<=0) break; at+=(unsigned)n; }
    fs->close(fs->ctx,fd);
    return (at==4 && !memcmp(signature,"\177ELF",4)) || (at>=2 && signature[0]=='M' && signature[1]=='Z');
}
/* Resolve every desktop link through one bounded dispatcher. Never execute a
 * shell command: paths and argument vectors remain separate. */
static int shortcut_resolve(struct desktop *d, struct desk_launch_record *record, unsigned depth)
{
    while (depth++<8) {
        if (!desk_launch_valid(record)) return -EINVAL;
        if (desk_target_url(record->target)) return 0;
        char path[FM_PATH]; int r=fm_posix_path(path,record->target); if (r) return r;
        const char *ext=strrchr(path,'.');
        if (!ext || strcasecmp(ext,".lnk")) return 0;
        r=desk_folder_link_read(d,path,record); if (r) return r;
    }
    return -EINVAL;
}
int desk_shell_open_record(struct desktop *d, const struct desk_launch_record *record, unsigned depth)
{
    if (!desk_launch_valid(record) || depth>=8) return -EINVAL;
    /* The native shell also works without an asynchronous path resolver.
     * An installed resolver owns the request, including any refusal. */
    if (desk_target_url(record->target)) return d->open_path ? d->open_path(d,record->target) : desk_task_launch(d,"netsurf",record->target);
    char path[FM_PATH];
    int r=fm_posix_path(path,record->target); if (r) return r;
    const char *ext=strrchr(path,'.');
    if (ext && !strcasecmp(ext,".lnk")) {
        struct desk_launch_record *next=malloc(sizeof(*next)); if (!next) return -ENOMEM;
        r=desk_folder_link_read(d,path,next);
        if (!r) r=desk_shell_open_record(d,next,depth+1);
        free(next); return r;
    }
    for (unsigned i=0;i<d->program_count;i++) if (!strcmp(path,d->programs[i].path)) {
        if (!record->arguments[0]) return desk_task_launch(d,d->programs[i].name,NULL);
        char text[sizeof(record->arguments)]; strcpy(text,record->arguments);
        const char *args[6]; unsigned count=0; char *read=text,*write=text;
        while (*read) {
            while (*read==' ' || *read=='\t') read++;
            if (!*read) break;
            if (count==6) return -E2BIG;
            args[count++]=write; int quote=0;
            while (*read && (quote || (*read!=' ' && *read!='\t'))) {
                if (*read=='"') { quote=!quote; read++; } else *write++=*read++;
            }
            if (quote) return -EINVAL;
            while (*read==' ' || *read=='\t') read++;
            *write++=0;
        }
        return count ? desk_task_launch_args(d,d->programs[i].name,args,count) : desk_task_launch(d,d->programs[i].name,NULL);
    }
    struct fm_meta meta;
    if (!d->sh.folder) return d->v3_launch ? d->v3_launch(d,record) : -ENODEV;
    struct fm_vfs *fs=&d->sh.folder->model.fs;
    r=fs->stat_path(fs->ctx,path,&meta); if (r) return r;
    if (meta.type==FM_DIR) return desk_task_launch(d,"files",path);
    if (desktop_program_target(d,path,&meta)) {
        if (!d->v3_launch) return -ENOSYS;
        struct desk_launch_record *launch=malloc(sizeof(*launch)); if (!launch) return -ENOMEM;
        *launch=*record;
        if (strlen(path)>=sizeof(launch->target)) r=-ENAMETOOLONG;
        else { strcpy(launch->target,path);
            if (record->directory[0]) {
                r=fm_posix_path(path,record->directory);
                if (!r && strlen(path)>=sizeof(launch->directory)) r=-ENAMETOOLONG;
                if (!r) strcpy(launch->directory,path);
            }
            if (!r) r=d->v3_launch(d,launch);
        }
        free(launch); return r;
    }
    const struct desk_catalog_entry *cat=desk_catalog_file(path);
    return desk_task_launch(d,cat ? cat->name : "ciuknote",path);
}
static void icon_open(struct desktop *d, int index)
{
    if (index < 0 || index >= d->sh.icon_count) return;
    const struct sh_icon *ic = &d->sh.icons[index];
    if (icon_is_entry(d,index) && d->open_path) {
        int e=d->open_path(d,ic->argument);
        if (e<0) desk_shell_message(d,"Desktop","The item could not be opened.",1);
        return;
    }
    if (ic->action==SH_ACT_WIN32_LINK) {
        unsigned k=(unsigned)strtoul(ic->argument,NULL,10);
        if (desk_link_launch(d,k)<0) desk_shortcut_properties(d,k,1);
    }
    else if (icon_is_entry(d,index)) {
        struct desk_launch_record *record=calloc(1,sizeof(*record));
        int r=-ENOMEM;
        if (record) {
            snprintf(record->label,sizeof(record->label),"%.63s",ic->label);
            if (strlen(ic->argument)>=sizeof(record->target)) r=-ENAMETOOLONG;
            else { strcpy(record->target,ic->argument); record->show=1; r=desk_shell_open_record(d,record,0); }
            free(record);
        }
        if (r<0) desk_shell_notify_error(d,"Cannot open shortcut or item",r);
    }
    else if (ic->action == SH_ACT_LAUNCH) launch_named(d, ic->program, ic->argument[0] ? ic->argument : NULL);
}
/* Double-click or Enter on a selected icon opens every selected icon. */
static void open_selection(struct desktop *d, int index)
{
    if (!desk_shell_icon_marked(d, index)) { icon_open(d, index); return; }
    int opened = 0;
    for (int k = 0; k < d->sh.icon_count && opened < 16; k++) if (desk_shell_icon_marked(d, k)) { icon_open(d, k); opened++; }
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
static void desktop_icon_rename_begin(struct desktop *d, int index)
{
    if (index<0 || index>=d->sh.icon_count || (d->sh.folder && d->sh.folder->busy)) return;
    d->sh.icon_editing=index;
    d->sh.icon_edit_cursor=(unsigned)snprintf(d->sh.icon_edit,sizeof(d->sh.icon_edit),"%s",d->sh.icons[index].label);
    desk_focus(d,-1); damage(d,d->sh.icons[index].box);
}
static int label_lines(const struct desktop *d, const char *label, char lines[2][32],int room)
{
    /* Up to two lines of at most room pixels, broken at the last space
     * that fits (a longer word is cut); a second line that still does not
     * fit ends with an ellipsis. Returns the number of lines. */
    const int f = SH_LABEL_FONT;
    size_t n = strlen(label), cut = 0, skip = 1;
    lines[0][0] = lines[1][0] = 0;
    if (desk_text_measure(f, label) <= room) { snprintf(lines[0], 32, "%.31s", label); return 1; }
    for (size_t i = 1; i < n; i++) if (label[i] == ' ' && desk_text_measure_n(f, label, i) <= room) cut = i;
    if (!cut) { cut = desk_text_fit_chars(f, label, room); skip = 0; }
    if (!cut) cut = 1;
    snprintf(lines[0], 32, "%.*s", (int)cut, label);
    const char *rest = label + cut + skip;
    if (desk_text_measure(f, rest) <= room) snprintf(lines[1], 32, "%s", rest);
    else snprintf(lines[1], 32, "%.*s\xe2\x80\xa6", (int)desk_text_fit_chars(f, rest, room - desk_text_measure(f, "\xe2\x80\xa6")), rest);
    return 2;
}
static void binomial(const struct desktop *d, const uint16_t *in, uint16_t *out, int dx, int dy)
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
struct icon_halo { char text[sizeof(((struct sh_icon *)0)->label)]; int aa, valid, width, height, font; uint8_t alpha[]; };
static struct icon_halo **label_halo;
static unsigned halo_capacity;
static const uint8_t empty_halo[256 * 256];
static const uint8_t *label_halo_of(const struct desktop *d, int k, const char *label, char lines[2][32], int count)
{
    /* Halo alpha of desktop icon k's label block (ICON_CELL_W x LABEL_H). */
    int aa = desk_theme->text_aa;
    if ((unsigned)k>=halo_capacity) {
        unsigned next=(unsigned)k+16;
        struct icon_halo **p=realloc(label_halo,next*sizeof(*p));
        if (!p) return empty_halo;
        memset(p+halo_capacity,0,(next-halo_capacity)*sizeof(*p)); label_halo=p; halo_capacity=next;
    }
    if (label_halo[k] && (label_halo[k]->width!=ICON_CELL_W || label_halo[k]->height!=LABEL_H || label_halo[k]->font!=d->sh.cfg.icon_font)) { free(label_halo[k]); label_halo[k]=NULL; }
    if (!label_halo[k]) {
        label_halo[k]=calloc(1,sizeof(**label_halo)+(size_t)ICON_CELL_W*LABEL_H);
        if (label_halo[k]) { label_halo[k]->width=ICON_CELL_W; label_halo[k]->height=LABEL_H; label_halo[k]->font=d->sh.cfg.icon_font; }
    }
    if (!label_halo[k]) return empty_halo;
    if (label_halo[k]->valid && label_halo[k]->aa == aa && !strcmp(label_halo[k]->text, label)) return label_halo[k]->alpha;
    static uint32_t mask[256 * 256];
    static uint16_t a[256 * 256], b[256 * 256];
    memset(mask, 0, (size_t)ICON_CELL_W * LABEL_H * sizeof(*mask));
    struct desk_paint m = {mask, ICON_CELL_W, LABEL_H, 0, 0, ICON_CELL_W, LABEL_H};
    for (int l = 0; l < count; l++)
        desk_paint_text(&m, SH_LABEL_FONT, (ICON_CELL_W - desk_text_measure(SH_LABEL_FONT, lines[l])) / 2,
                        LABEL_BASE + l * LABEL_PITCH, lines[l], 0x0000ffu);
    for (int i = 0; i < ICON_CELL_W * LABEL_H; i++) a[i] = (uint16_t)(mask[i] & 255u);
    for (int pass = 0; pass < 2; pass++) { binomial(d,a, b, 1, 0); binomial(d,b, a, 0, 1); }
    for (int y = 0; y < LABEL_H; y++) for (int x = 0; x < ICON_CELL_W; x++) {
        unsigned v = y ? (unsigned)a[(y - 1) * ICON_CELL_W + x] * HALO_GAIN / 16 : 0;   /* 1 px down */
        label_halo[k]->alpha[y * ICON_CELL_W + x] = (uint8_t)(v > HALO_PEAK ? HALO_PEAK : v);
    }
    snprintf(label_halo[k]->text, sizeof(label_halo[k]->text), "%s", label);
    label_halo[k]->aa = aa; label_halo[k]->valid = 1;
    return label_halo[k]->alpha;
}
extern void desk_recovery_paint(struct desktop *, struct desk_box) __attribute__((weak));
extern void desk_recovery_tick(struct desktop *) __attribute__((weak));
static void draw_icons(struct desktop *d, struct desk_box clip)
{
    /* G14: Safe mode's corner labels under the icons; no icons at a command prompt. */
    if (desk_recovery_paint) desk_recovery_paint(d, clip);
    if (!d->sh.cfg.desktop_icons || d->terminal_shell) return;
    for (int k = 0; k < d->sh.icon_count; k++) {
        const struct sh_icon *ic = &d->sh.icons[k];
        struct desk_box b;
        if (!desk_intersect(ic->box, clip, &b)) continue;
        struct du_canvas c = canvas(d, b);
        struct desk_paint p = painter(&c);
        int selected = desk_shell_icon_marked(d, k) || drop_lit(d, k);
        int icon_size=desk_appearance_active ? desk_scale((desk_appearance.effects&AP_LARGE_ICONS) ? 48 : desk_appearance.item[AP_ICON].size) : SH_DESKTOP_ICON;
        int32_t ix = ic->box.x + (ic->box.w - icon_size) / 2, iy = ic->box.y + desk_scale(4);
        const uint32_t *custom=NULL;
        if (ic->action==SH_ACT_WIN32_LINK && d->v3_link_icon) {
            unsigned link=(unsigned)strtoul(ic->argument,NULL,10);
            if (link<d->v3_link_count) custom=d->v3_link_icon(d,d->v3_links[link].source,32);
        }
        if (custom) du_icon_blit(&c,ix+(icon_size-32)/2,iy+(icon_size-32)/2,custom,32,selected ? DU_ICON_SELECTED : DU_ICON_NORMAL);
        else du_icon_blit(&c, ix, iy, desk_shell_icon(ic->icon, icon_size), icon_size, selected ? DU_ICON_SELECTED : DU_ICON_NORMAL);
        const char *extension=strrchr(ic->argument,'.');
        if (strcmp(ic->icon,"emblem-symbolic-link") && (ic->action==SH_ACT_WIN32_LINK || (extension && !strcasecmp(extension,".lnk"))))
            du_icon_blit(&c,ix+icon_size-16,iy+icon_size-16,desk_shell_icon("emblem-symbolic-link",16),16,DU_ICON_NORMAL);   /* G19: the link arrow */
        char lines[2][32];
        int count = label_lines(d,ic->label, lines,ic->box.w-desk_scale(12)), widest = 0;
        for (int l = 0; l < count; l++) { int w = desk_text_measure(SH_LABEL_FONT, lines[l]); if (w > widest) widest = w; }
        int cell_width=ic->box.w;
        int pitch=desk_appearance_active ? desk_font_line(SH_LABEL_FONT)+desk_scale(2) : LABEL_PITCH;
        int baseline=desk_appearance_active ? desk_font_cap(SH_LABEL_FONT)+desk_scale(4) : LABEL_BASE;
        int32_t top = ic->box.y + (desk_appearance_active ? icon_size+desk_scale(6) : LABEL_TOP);
        struct desk_box sel = {ic->box.x + (cell_width - widest) / 2 - 5, top + 1, widest + 10, count * pitch + 4};
        if (selected)   /* rounded, translucent: a light tint of the selection colour, the wallpaper shows through */
            desk_paint_round_blend(&p, sel.x, sel.y, sel.w, sel.h, 4, desk_mix(0xffffffu, SH_HOT, 80), 130,
                                   desk_mix(0xffffffu, SH_HOT, 170), 235);
        /* The cached halo, then white text over it. */
        const uint8_t *halo = desk_appearance_active ? empty_halo : label_halo_of(d,k, ic->label, lines, count);
        struct desk_box area;
        if (!desk_appearance_active && desk_intersect((struct desk_box){ic->box.x, top, ICON_CELL_W, LABEL_H}, b, &area))
            for (int32_t y = area.y; y < area.y + area.h; y++) {
                const uint8_t *row = halo + (size_t)(y - top) * ICON_CELL_W + (size_t)(area.x - ic->box.x);
                uint32_t *dst = d->pixels + (size_t)y * d->width + (size_t)area.x;
                for (int32_t x = 0; x < area.w; x++) if (row[x]) dst[x] = desk_mix(0x000000u, dst[x], row[x]);
            }
        for (int l = 0; l < count; l++)
            desk_paint_text(&p, SH_LABEL_FONT, ic->box.x + (cell_width - desk_text_measure(SH_LABEL_FONT, lines[l])) / 2,
                            top + baseline + l * pitch, lines[l], desk_appearance_active ? selected ? desk_appearance.item[AP_SELECTED].text : desk_appearance.item[AP_ICON].text : 0xffffffu);
        if (d->sh.icon_editing==k) {
            struct du_rect box={ic->box.x+2,top,cell_width-4,desk_scale(24)};
            du_well(&c,box,0xffffffu);
            const char *shown=d->sh.icon_edit;
            while (*shown && du_text_width(shown)>box.w-8) shown++;
            du_text(&c,box.x+4,box.y+5,shown,DU_INK);
            char prefix[FM_NAME]; unsigned start=(unsigned)(shown-d->sh.icon_edit);
            unsigned n=d->sh.icon_edit_cursor>start ? d->sh.icon_edit_cursor-start : 0;
            if (n<sizeof(prefix)) { memcpy(prefix,shown,n); prefix[n]=0; du_vline(&c,box.x+4+du_text_width(prefix),box.y+4,16,DU_INK); }
        }
        if (d->sh.folder && d->sh.folder->editing==k-3 && k>=3) {
            struct desk_folder *f=d->sh.folder;
            struct du_rect box={ic->box.x+2,top,cell_width-4,desk_scale(24)};
            du_well(&c,box,f->select_all ? DU_SELECT : DU_WINDOW);
            /* Show the end of long names while typing. */
            const char *shown=f->edit;
            while (*shown && du_text_width(shown)>box.w-8) shown++;
            du_text(&c,box.x+4,box.y+5,shown,f->select_all ? DU_SELECT_INK : DU_INK);
            if (!f->select_all && !d->shell_caret_hidden) {
                char prefix[FM_NAME]; unsigned start=(unsigned)(shown-f->edit), count=f->cursor>start ? f->cursor-start : 0;
                memcpy(prefix,shown,count); prefix[count]=0;
                du_vline(&c,box.x+4+du_text_width(prefix),box.y+4,16,DU_INK);
            }
        }
        if (k == d->sh.icon_selected && d->focus < 0 && !d->sh.levels) du_dotted(&c, (struct du_rect){sel.x + 2, sel.y + 2, sel.w - 4, sel.h - 4});
    }
}
unsigned desk_shell_cursor(const struct desktop *d)
{
    struct desk_box bar=desk_taskbar(d);
    if (d->sh.bar_resize) return d->sh.cfg.taskbar_edge>=CFG_BAR_LEFT ? DESK_CURSOR_RESIZE_EW : DESK_CURSOR_RESIZE_NS;
    if (desk_contains(bar,d->mouse_x,d->mouse_y) && (desk_taskbar_front(d) || desk_hit(d,d->mouse_x,d->mouse_y)<0)) {
        int edge=d->sh.cfg.taskbar_edge;
        if (edge==CFG_BAR_BOTTOM ? d->mouse_y<bar.y+4 : edge==CFG_BAR_TOP ? d->mouse_y>=bar.y+bar.h-4 : edge==CFG_BAR_LEFT ? d->mouse_x>=bar.x+bar.w-4 : d->mouse_x<bar.x+4)
            return edge>=CFG_BAR_LEFT ? DESK_CURSOR_RESIZE_EW : DESK_CURSOR_RESIZE_NS;
        int slot=toolbar_hit(d,d->mouse_x,d->mouse_y);struct desk_toolbar_slot slots[DESK_TOOLBAR_SLOTS];int n=desk_toolbar_slots(d,slots);
        if (slot>=0 && slot<n && slots[slot].kind==DESK_TOOLBAR_ADDRESS) return DESK_CURSOR_TEXT;
    }
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

/* ---- fx-19 desktop selection, rubber band and drag and drop ------------
 * Windows 98 desktop list view: Ctrl toggles, Shift extends from the anchor
 * (icon view: the rectangle between anchor and item), a press on empty
 * space starts a rubber band (Ctrl toggles what it covers, Shift adds).
 * Dragging follows OLE DoDragDrop: the shell owns the pointer from the
 * SM_CXDRAG threshold to the drop; targets report DROPEFFECT values; Esc
 * cancels; the right button ends in a Move/Copy/Shortcut/Cancel menu.
 * Paths are handed to the folder layer (folder.c); bounded shortcut resolution
 * is cached for the hovered icon during a drag and revalidated at activation. */
int desk_shell_icon_marked(const struct desktop *d, int k)
{
    if (k < 0 || k >= d->sh.icon_count) return 0;
    return d->sh.icons[k].selected || (k == d->sh.icon_selected && !d->sh.focus_unselected);
}
static int drop_lit(const struct desktop *d, int k)
{ return d->sh.dnd.active && !d->sh.dnd.menu && d->sh.dnd.drop_index == k && k >= 0; }
/* One icon's look changed: repair the cached wallpaper/icon layer there. */
static void icon_repaint(struct desktop *d, int k)
{
    struct desk_shell *s = &d->sh;
    if (k < 0 || k >= s->icon_count) return;
    struct desk_box b;
    if (s->background && s->background_valid && s->wall &&
        desk_intersect(s->icons[k].box, (struct desk_box){0, 0, (int)d->width, (int)d->height}, &b)) {
        uint32_t *out = d->pixels; d->pixels = s->background;
        for (int y = b.y; y < b.y + b.h; y++)
            memcpy(s->background + (size_t)y * d->width + b.x, s->wall + (size_t)y * d->width + b.x, (size_t)b.w * 4);
        draw_icons(d, b); s->background_serial++;
        d->pixels = out;
    }
    damage(d, s->icons[k].box);
}
static void mark(struct desktop *d, int k, int on)
{
    if (k < 0 || k >= d->sh.icon_count || desk_shell_icon_marked(d, k) == (on != 0)) return;
    d->sh.icons[k].selected = on != 0;
    if (k == d->sh.icon_selected) d->sh.focus_unselected = !on;
    icon_repaint(d, k);
}
static void select_none(struct desktop *d) { for (int k = 0; k < d->sh.icon_count; k++) mark(d, k, 0); }
/* Moves the focus rectangle, keeping every icon's selection state. */
static void icon_focus(struct desktop *d, int k)
{
    int old = d->sh.icon_selected;
    if (old == k) return;
    if (old >= 0 && old < d->sh.icon_count) { d->sh.icons[old].selected = desk_shell_icon_marked(d, old); damage(d, d->sh.icons[old].box); }
    d->sh.icon_selected = k;
    d->sh.focus_unselected = k >= 0 && k < d->sh.icon_count && !d->sh.icons[k].selected;
    if (k >= 0 && k < d->sh.icon_count) damage(d, d->sh.icons[k].box);
}
static void select_only(struct desktop *d, int k)
{
    select_none(d); icon_focus(d, k);
    if (k >= 0) { mark(d, k, 1); d->sh.anchor = k; }
}
static int marked_count(const struct desktop *d, int files_only)
{
    int n = 0;
    for (int k = 0; k < d->sh.icon_count; k++) n += desk_shell_icon_marked(d, k) && (!files_only || icon_is_entry(d, k));
    return n;
}
/* Shift: every icon inside the rectangle spanned by the anchor and k. */
static void select_range(struct desktop *d, int k, int add)
{
    int a = d->sh.anchor >= 0 && d->sh.anchor < d->sh.icon_count ? d->sh.anchor : k;
    struct desk_box A = d->sh.icons[a].box, B = d->sh.icons[k].box;
    int x0 = A.x < B.x ? A.x : B.x, y0 = A.y < B.y ? A.y : B.y;
    int x1 = A.x + A.w > B.x + B.w ? A.x + A.w : B.x + B.w, y1 = A.y + A.h > B.y + B.h ? A.y + A.h : B.y + B.h;
    for (int i = 0; i < d->sh.icon_count; i++) {
        struct desk_box b = d->sh.icons[i].box;
        int cx = b.x + b.w / 2, cy = b.y + b.h / 2, inside = cx >= x0 && cx < x1 && cy >= y0 && cy < y1;
        if (inside) mark(d, i, 1); else if (!add) mark(d, i, 0);
    }
    icon_focus(d, k); mark(d, k, 1);
}
/* Synchronise the Files model's selection with the desktop's (commands). */
static int selection_sync(struct desktop *d)
{
    if (!d->sh.folder) return 0;
    struct fm_model *m = &d->sh.folder->model; int n = 0;
    for (unsigned i = 0; i < m->count; i++) {
        int k = (int)i + 3;
        m->entries[i].selected = k < d->sh.icon_count && desk_shell_icon_marked(d, k);
        n += m->entries[i].selected != 0;
    }
    return n;
}

static struct desk_box band_box(const struct desktop *d)
{
    const struct desk_shell *s = &d->sh;
    int x0 = s->band_x0 < s->band_x1 ? s->band_x0 : s->band_x1, x1 = s->band_x0 < s->band_x1 ? s->band_x1 : s->band_x0;
    int y0 = s->band_y0 < s->band_y1 ? s->band_y0 : s->band_y1, y1 = s->band_y0 < s->band_y1 ? s->band_y1 : s->band_y0;
    return (struct desk_box){x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}
static void band_damage(struct desktop *d, struct desk_box b)
{
    damage(d, (struct desk_box){b.x, b.y, b.w, 1}); damage(d, (struct desk_box){b.x, b.y + b.h - 1, b.w, 1});
    damage(d, (struct desk_box){b.x, b.y, 1, b.h}); damage(d, (struct desk_box){b.x + b.w - 1, b.y, 1, b.h});
}
/* The desktop marquee: every other pixel of its outline inverted, as the
 * Windows 9x focus-rectangle XOR, visible on any wallpaper. */
static void draw_band(struct desktop *d, struct desk_box clip)
{
    if (!d->sh.band) return;
    struct desk_box b = band_box(d);
    if (b.w < 2 && b.h < 2) return;
    struct desk_box edges[4] = {{b.x, b.y, b.w, 1}, {b.x, b.y + b.h - 1, b.w, 1}, {b.x, b.y, 1, b.h}, {b.x + b.w - 1, b.y, 1, b.h}};
    for (unsigned e = 0; e < 4; e++) {
        struct desk_box r;
        if (!desk_intersect(edges[e], clip, &r)) continue;
        for (int y = r.y; y < r.y + r.h; y++) for (int x = r.x; x < r.x + r.w; x++)
            if (!((x + y) & 1)) { uint32_t *p = d->pixels + (size_t)y * d->width + x; *p ^= 0xffffffu; }
    }
}
static void band_begin(struct desktop *d, int x, int y)
{
    struct desk_shell *s = &d->sh;
    if (s->icon_count > s->band_capacity) {
        uint8_t *o = realloc(s->band_original, (size_t)s->icon_count);
        if (!o) return;
        s->band_original = o; s->band_capacity = s->icon_count;
    }
    s->band_modifiers = s->modifiers & (DU_CTRL | DU_SHIFT);
    if (!s->band_modifiers) select_none(d);
    for (int k = 0; k < s->icon_count; k++) s->band_original[k] = (uint8_t)desk_shell_icon_marked(d, k);
    s->band = 1; s->band_x0 = s->band_x1 = x; s->band_y0 = s->band_y1 = y;
}
static void band_update(struct desktop *d, int x, int y)
{
    struct desk_shell *s = &d->sh;
    if (y < DESK_BAR) y = DESK_BAR;
    if (y >= (int)d->height - DESK_BAR) y = (int)d->height - DESK_BAR - 1;
    if (x == s->band_x1 && y == s->band_y1) return;
    band_damage(d, band_box(d));
    s->band_x1 = x; s->band_y1 = y;
    struct desk_box b = band_box(d);
    band_damage(d, b);
    for (int k = 0; k < s->icon_count && k < s->band_capacity; k++) {
        struct desk_box ib = s->icons[k].box, hit = {ib.x + 10, ib.y + 2, ib.w - 20, ib.h - 4}, ignored;
        unsigned in = desk_intersect(hit, b, &ignored) != 0;
        mark(d, k, s->band_modifiers & DU_CTRL ? s->band_original[k] ^ in : s->band_original[k] | in);
    }
}
static void band_end(struct desktop *d)
{
    struct desk_shell *s = &d->sh;
    if (!s->band) return;
    band_damage(d, band_box(d)); s->band = 0;
    /* The focus goes to the first icon the band selected. */
    for (int k = 0; k < s->icon_count; k++) if (desk_shell_icon_marked(d, k) && (k >= s->band_capacity || !s->band_original[k])) {
        int keep = desk_shell_icon_marked(d, k); icon_focus(d, k); mark(d, k, keep);
        if (!(s->band_modifiers & DU_SHIFT)) s->anchor = k;
        break;
    }
}

/* ---- drag session ---- */
static uint32_t dnd_counter;
static unsigned dnd_keys(const struct desktop *d)
{
    unsigned m = d->sh.modifiers;
    return (m & DU_SHIFT ? FM_KEY_SHIFT : 0) | (m & DU_CTRL ? FM_KEY_CTRL : 0) | (m & DU_ALT ? FM_KEY_ALT : 0);
}
static int volume_optical(const struct desktop *d, int volume)
{
    return volume >= 'D' && volume <= 'Z' && (d->sh.media.removable_mask & (1u << (volume - 'A'))) &&
           d->sh.media.type[volume - 'A'] == CIUKI_MEDIA_OPTICAL;
}
static int files_target(const struct desktop *d, int i)
{
    const struct desk_client *c = &d->clients[i];
    return c->window && !c->closing && c->program == DESK_APP_FILES && c->version == DESK_SHELL_VERSION;
}
static int app_target(const struct desktop *d, int i)
{
    const struct desk_client *c = &d->clients[i];
    return c->window && !c->closing && c->accept_files && (c->version == DESK_SHELL_VERSION || c->version == DESK_V3_VERSION);
}
static int dnd_send(struct desktop *d, int i, unsigned op, unsigned length, const uint32_t *words, unsigned n)
{
    if (i < 0 || i >= DESK_CLIENTS || !d->clients[i].window) return 0;
    struct ciuki_message m; desk_message(&m, op, length);
    unsigned offset=4;
    if (d->clients[i].version==DESK_V3_VERSION) {
        desk_v3_message(&m,op,length-4,0,d->clients[i].v3_id,d->clients[i].v3_generation); offset=16;
        if (op==DESK_DROP_FILES) {
            int fd=d->drop_snapshot ? d->drop_snapshot(d) : -1;
            if (fd<0) return 0;
            m.fd_count=1; m.fds[0]=fd;
        }
    } else { m.data[0]=(uint8_t)d->clients[i].version; m.data[2]=(uint8_t)d->clients[i].minor; }
    for (unsigned k = 0; k < n; k++) desk_put32(m.data + offset + 4 * k, words[k]);
    desk_queue(d, i, &m);
    return 1;
}
static void dnd_over(struct desktop *d, int phase)
{
    struct sh_dnd *g = &d->sh.dnd;
    if (g->kind != SH_DROP_FILES || g->target < 0 || !files_target(d, g->target)) return;
    struct desk_box b = desk_content(&d->clients[g->target]);
    uint32_t w[5] = {g->serial, (uint32_t)phase, (uint32_t)(d->mouse_x - b.x), (uint32_t)(d->mouse_y - b.y), g->keys};
    dnd_send(d, g->target, DESK_DRAG_OVER, 24, w, 5);
    g->repeat_ns = d->now_ns;
}
static struct desk_box dnd_image_box(const struct desktop *d)
{
    const struct sh_dnd *g = &d->sh.dnd;
    if (!g->active || g->menu) return (struct desk_box){0, 0, 0, 0};
    int dx = d->mouse_x - g->start_x, dy = d->mouse_y - g->start_y;
    if (g->source < 0) {
        struct desk_box u = {0, 0, 0, 0};
        for (unsigned n = 0; n < g->icon_count; n++) {
            struct desk_box b = d->sh.icons[g->icons[n]].box; b.x += dx; b.y += dy;
            if (!u.w) u = b;
            else {
                int x0 = u.x < b.x ? u.x : b.x, y0 = u.y < b.y ? u.y : b.y;
                int x1 = u.x + u.w > b.x + b.w ? u.x + u.w : b.x + b.w, y1 = u.y + u.h > b.y + b.h ? u.y + u.h : b.y + b.h;
                u = (struct desk_box){x0, y0, x1 - x0, y1 - y0};
            }
        }
        return u;
    }
    unsigned rows = g->count < 4 ? g->count : 4;
    return (struct desk_box){d->mouse_x - 16, d->mouse_y - 16, 220, (int)rows * 36 + (g->count > 4 ? 18 : 0)};
}
static void dnd_image_damage(struct desktop *d)
{
    struct sh_dnd *g = &d->sh.dnd;
    if (g->image.w) damage(d, g->image);
    g->image = dnd_image_box(d);
    if (g->image.w) damage(d, g->image);
}
static void dnd_free(struct desktop *d)
{
    struct sh_dnd *g = &d->sh.dnd;
    for (unsigned i = 0; g->paths && i < g->count; i++) free(g->paths[i]);
    free(g->paths); free(g->types);
    g->paths = NULL; g->types = NULL; g->count = 0;
}
/* Which object is under the pointer and what a drop there would do. */
static void dnd_evaluate(struct desktop *d)
{
    struct sh_dnd *g = &d->sh.dnd;
    if (!g->active || g->menu) return;
    int x = d->mouse_x, y = d->mouse_y, kind = SH_DROP_NONE, target = -1, drop_index = -1, task = -1;
    unsigned allowed = 0, effect = FM_EFFECT_NONE;
    char destination[FM_PATH] = "";
    g->keys = dnd_keys(d);
    int window = desk_hit(d, x, y);
    int menu_folder=-1;
    if (g->count && desk_start_contains(d,x,y)) { if (!g->start_hover_ns) g->start_hover_ns=d->now_ns ? d->now_ns : 1; }
    else g->start_hover_ns=0;
    for (int l=d->sh.levels-1;l>=0;l--) if (desk_contains(d->sh.menus[l].box,x,y)) {
        int mk=d->sh.menus[l].kind;
        menu_folder=mk==SH_MENU_START ? SH_ROOT_START : mk==SH_MENU_PROGRAMS ? SH_ROOT_PROGRAMS : mk>=SH_MENU_FOLDER_BASE ? mk-SH_MENU_FOLDER_BASE : -1;
        int hit=menu_hit(&d->sh.menus[l],x,y);
        if (hit>=0) {
            menu_highlight(d,&d->sh.menus[l],hit);
            const struct sh_item *it=&d->sh.menus[l].items[hit];
            if (it->action==SH_ACT_SUBMENU && (it->arg==SH_MENU_PROGRAMS || it->arg>=SH_MENU_FOLDER_BASE)) {
                menu_folder=it->arg==SH_MENU_PROGRAMS ? SH_ROOT_PROGRAMS : it->arg-SH_MENU_FOLDER_BASE;
                submenu(d,l,hit);
            }
        }
        break;
    }
    if (g->count && (desk_start_contains(d,x,y) || menu_folder>=0)) {
        kind=SH_DROP_START;allowed=effect=FM_EFFECT_LINK;
        const char *path=menu_folder>=0 && d->sh.custom ? d->sh.custom->folders[menu_folder] :
            menu_folder==SH_ROOT_PROGRAMS ? SH_PROGRAMS_FOLDER : menu_folder==SH_ROOT_FAVORITES ? SH_FAVORITES_FOLDER : SH_START_FOLDER;
        snprintf(destination,sizeof(destination),"%s",path);
    } else if (y < DESK_BAR) kind = SH_DROP_NONE;
    else if (d->shell && desk_contains(desk_taskbar(d), x, y)) {
        for (int i = 0; i < DESK_CLIENTS; i++) if (desk_task_visible(&d->clients[i]) && desk_contains(desk_task_button(d, i), x, y)) task = i;
        kind = SH_DROP_TASK;
        int slot=toolbar_hit(d,x,y),root=-1;
        if (desk_contains(desk_quick_launch_button(d),x,y)) root=SH_ROOT_QUICK;
        if (slot>=0) {
            struct desk_toolbar_slot slots[DESK_TOOLBAR_SLOTS];int n=desk_toolbar_slots(d,slots);
            if (slot<n && slots[slot].kind==DESK_TOOLBAR_FOLDER) root=slots[slot].arg;
            else if (slot<n && slots[slot].kind==DESK_TOOLBAR_ITEM && d->sh.custom) root=d->sh.custom->entries[slots[slot].arg].parent;
        }
        if (g->count && root>=0) {
            kind=SH_DROP_QUICK;allowed=effect=FM_EFFECT_LINK;
            snprintf(destination,sizeof(destination),"%s",root==SH_ROOT_QUICK ? SH_QUICK_FOLDER : d->sh.custom ? d->sh.custom->folders[root] : "");
        } else if (task<0 && g->count==1 && g->types[0]=='D') {
            kind=SH_DROP_TOOLBAR;allowed=effect=FM_EFFECT_LINK;
        }
    } else if (window >= 0) {
        const struct desk_client *c = &d->clients[window];
        if (desk_modal_target(d, window) == window && desk_contains(desk_content(c), x, y) && g->count) {
            if (files_target(d, window)) {
                kind = SH_DROP_FILES; target = window;
                if (g->status_target == window) { effect = g->effect; allowed = g->allowed; }
            } else if (app_target(d, window)) {
                /* WM_DROPFILES targets open what they receive: the copy cursor. */
                kind = SH_DROP_APP; target = window; allowed = FM_EFFECT_COPY;
                effect = g->keys & (FM_KEY_SHIFT | FM_KEY_ALT) ? FM_EFFECT_NONE : FM_EFFECT_COPY;
            }
        }
    } else {
        int k = icon_hit(d, x, y);
        if (k >= 0 && !(g->source < 0 && desk_shell_icon_marked(d, k))) {
            const struct sh_icon *ic = &d->sh.icons[k];
            int entry_dir = icon_is_entry(d, k) && d->sh.folder->model.entries[k - 3].meta.type == FM_DIR;
            if (k == 2 && g->count) {
                kind = SH_DROP_TRASH; target = k; allowed = volume_optical(d, g->source_volume) ? 0 : FM_EFFECT_MOVE;   /* a CD cannot be deleted from */
                snprintf(destination, sizeof(destination), "%s", DESK_TRASH);
            } else if (g->count && (k == 1 || entry_dir || (ic->action == SH_ACT_LAUNCH && !strcmp(ic->program, "files") && ic->argument[0] == '/'))) {
                kind = SH_DROP_ICON; target = k; allowed = g->source_allowed;
                snprintf(destination, sizeof(destination), "%s", ic->argument);
            } else if (g->count && (icon_is_entry(d,k) || ic->action==SH_ACT_WIN32_LINK)) {
                if (g->resolved_icon==k) {
                    kind=g->resolved_kind; target=k;
                    allowed=kind==SH_DROP_PROGRAM ? FM_EFFECT_COPY : kind==SH_DROP_ICON ? g->source_allowed : 0;
                    snprintf(destination,sizeof(destination),"%s",g->resolved_destination);
                } else {
                const char *linkpath=ic->action==SH_ACT_WIN32_LINK ? icon_position_name(d,k) : ic->argument;
                const char *ext=strrchr(linkpath,'.');
                if (ic->action==SH_ACT_WIN32_LINK || (ext && !strcasecmp(ext,".lnk"))) {
                    struct desk_launch_record *record=malloc(sizeof(*record));
                    if (record && !desk_folder_link_read(d,linkpath,record) && !shortcut_resolve(d,record,0) && !desk_target_url(record->target)) {
                        char path[FM_PATH]; struct fm_meta meta;
                        if (!fm_posix_path(path,record->target) && !d->sh.folder->model.fs.stat_path(d->sh.folder->model.fs.ctx,path,&meta)) {
                            if (meta.type==FM_DIR) { kind=SH_DROP_ICON; target=k; allowed=g->source_allowed; snprintf(destination,sizeof(destination),"%s",path); }
                            else if (desktop_program_target(d,path,&meta)) { kind=SH_DROP_PROGRAM; target=k; allowed=FM_EFFECT_COPY; }
                        }
                    }
                    free(record);
                } else if (icon_is_entry(d,k) && desktop_program_target(d,ic->argument,&d->sh.folder->model.entries[k-3].meta)) {
                    kind=SH_DROP_PROGRAM; target=k; allowed=FM_EFFECT_COPY;
                }
                g->resolved_icon=k; g->resolved_kind=kind;
                snprintf(g->resolved_destination,sizeof(g->resolved_destination),"%s",destination);
                }
            }
        } else {
            kind = SH_DROP_DESKTOP;
            snprintf(destination, sizeof(destination), "%s", d->sh.profile.desktop[0] ? d->sh.profile.desktop : DESK_FOLDER);
            if (g->source < 0) allowed = FM_EFFECT_MOVE | (g->count ? FM_EFFECT_COPY | FM_EFFECT_LINK : 0);   /* reposition, or Ctrl: copy beside */
            else allowed = g->source_allowed;
        }
        if (kind == SH_DROP_TRASH) {
            effect = g->keys & (FM_KEY_CTRL | FM_KEY_ALT) ? FM_EFFECT_NONE : FM_EFFECT_MOVE;
            if (!allowed) effect = FM_EFFECT_NONE;
            /* Items already in a Recycle Bin (or the legacy Trash) are not deleted again. */
            for (unsigned i = 0; i < g->count && effect; i++) {
                char dir[FM_PATH];
                if (fm_beneath(g->paths[i], DESK_TRASH) || (!rb_dir(fm_drive_letter(g->paths[i]), dir) && fm_beneath(g->paths[i], dir))) effect = FM_EFFECT_NONE;
            }
        } else if (kind == SH_DROP_DESKTOP && g->source < 0) {
            effect = fm_drop_effect(g->source_volume,fm_volume(destination),g->keys,allowed);
        } else if (kind == SH_DROP_ICON || kind == SH_DROP_DESKTOP) {
            if (volume_optical(d, fm_volume(destination))) allowed = 0;
            for (unsigned i = 0; i < g->count && allowed; i++) if (g->types[i] == 'D' && fm_beneath(destination, g->paths[i])) allowed = 0;
            effect = fm_drop_effect(g->source_volume, fm_volume(destination), g->keys, allowed);
            if (effect && fm_drop_check(g->paths, g->count, destination, effect)) effect = FM_EFFECT_NONE;
        }
        if (kind==SH_DROP_PROGRAM) effect=FM_EFFECT_COPY;
        if ((kind == SH_DROP_ICON || kind == SH_DROP_TRASH || kind==SH_DROP_PROGRAM) && effect) drop_index = target;
    }
    if (kind != g->kind || target != g->target) {
        if (g->kind == SH_DROP_FILES) dnd_over(d, 1);
        g->status_target = -1;
        if (kind == SH_DROP_FILES) effect = FM_EFFECT_NONE, allowed = 0;   /* until the target answers */
    }
    g->kind = kind; g->target = target;
    snprintf(g->destination, sizeof(g->destination), "%s", destination);
    g->effect = effect; g->allowed = allowed;
    if (kind == SH_DROP_FILES) dnd_over(d, 0);
    if (task != g->hover_task) { g->hover_task = task; g->hover_ns = d->now_ns ? d->now_ns : 1; }
    if (drop_index != g->drop_index) { int old = g->drop_index; g->drop_index = drop_index; icon_repaint(d, old); icon_repaint(d, drop_index); }
}
static void dnd_start(struct desktop *d)
{
    struct sh_dnd *g = &d->sh.dnd;
    g->start_hover_ns=0;g->opened_start=0;
    g->active = 1; g->menu = 0; g->resolved_icon=-1; g->serial = ++dnd_counter ? dnd_counter : ++dnd_counter;
    g->kind = SH_DROP_NONE; g->target = -1; g->status_target = -1; g->hover_task = -1; g->drop_index = -1;
    g->start_x = d->mouse_x; g->start_y = d->mouse_y; g->image = (struct desk_box){0, 0, 0, 0};
    g->source_volume = g->count ? fm_volume(g->paths[0]) : 0;
    g->source_allowed = volume_optical(d, g->source_volume) ? FM_EFFECT_COPY | FM_EFFECT_LINK : FM_EFFECTS;
    tooltip_hide(d);
    dnd_evaluate(d); dnd_image_damage(d); desk_cursor_update(d);
}
/* A Files window started a drag (client.c: DRAG_BEGIN from its own press). */
int desk_shell_drag_begin(struct desktop *d, int client, unsigned button, unsigned count)
{
    struct sh_dnd *g = &d->sh.dnd;
    if (!d->shell || g->active || d->sh.levels || d->sh.band || d->drag >= 0 || d->power.state) return -EBUSY;
    dnd_free(d);
    int r = desk_folder_drag_load(d, &g->paths, &g->types, &g->count);
    if (!r && g->count != count) r = -EINVAL;
    if (r) { dnd_free(d); return r; }
    g->source = client; g->source_pid = d->clients[client].pid; g->button = button == 2 ? CIUKI_BUTTON_RIGHT : CIUKI_BUTTON_LEFT;
    g->icon_count = 0;
    /* The source still holds the button: it stops receiving motion. */
    dnd_start(d);
    return 0;
}
/* The desktop's own selection leaves the threshold square. */
static void dnd_desktop_begin(struct desktop *d, unsigned button)
{
    struct sh_dnd *g = &d->sh.dnd;
    dnd_free(d); g->icon_count = 0;
    int files = marked_count(d, 1), n = 0;
    if (files) {
        g->paths = calloc((size_t)files, sizeof(*g->paths)); g->types = calloc((size_t)files, 1);
        if (!g->paths || !g->types) { dnd_free(d); return; }
    }
    for (int k = 0; k < d->sh.icon_count; k++) if (desk_shell_icon_marked(d, k)) {
        if (g->icon_count < SH_DRAG_IMAGE_ITEMS) g->icons[g->icon_count++] = k;
        if (!icon_is_entry(d, k) || n >= files) continue;
        g->paths[n] = malloc(FM_PATH);
        if (!g->paths[n]) { dnd_free(d); return; }
        snprintf(g->paths[n], FM_PATH, "%s", d->sh.icons[k].argument);
        g->types[n] = d->sh.folder->model.entries[k - 3].meta.type == FM_DIR ? 'D' : 'F';
        g->count = (unsigned)++n;
    }
    if (!g->icon_count) { dnd_free(d); return; }
    if (g->count && desk_folder_drag_save(d, g->paths, g->types, g->count)) { dnd_free(d); g->count = 0; }
    g->source = -1; g->source_pid = 0; g->button = button;
    d->sh.icon_drag = -1; d->sh.icon_pending = 0;
    int x = d->mouse_x, y = d->mouse_y;
    d->mouse_x = d->sh.drag_start_x; d->mouse_y = d->sh.drag_start_y;
    dnd_start(d);                       /* the image origin is the press point */
    d->mouse_x = x; d->mouse_y = y;
    dnd_evaluate(d); dnd_image_damage(d);
}
static void dnd_finish(struct desktop *d, unsigned performed)
{
    struct sh_dnd *g = &d->sh.dnd;
    if (g->source >= 0 && g->source < DESK_CLIENTS && d->clients[g->source].window && d->clients[g->source].pid == g->source_pid) {
        uint32_t w[1] = {performed};
        dnd_send(d, g->source, DESK_DRAG_END, 8, w, 1);
    }
    int old = g->drop_index,opened_start=g->opened_start;
    if (g->image.w) damage(d, g->image);
    dnd_free(d);
    memset(g, 0, sizeof(*g)); g->drop_index = -1; g->hover_task = -1; g->target = -1; g->status_target = -1; g->source = -1;
    if (opened_start) {
        desk_menu_close(d);
        if (d->menu_open) { d->menu_open=0;desk_focus(d,d->menu_focus);damage(d,desk_start_bounds(d)); }
    }
    icon_repaint(d, old);
    desk_cursor_update(d);
}
void desk_shell_drag_cancel(struct desktop *d)
{
    struct sh_dnd *g = &d->sh.dnd;
    if (!g->active) return;
    if (g->kind == SH_DROP_FILES) dnd_over(d, 1);
    dnd_finish(d, FM_EFFECT_NONE);
}
/* Desktop source onto free space: the icons keep their offsets (Windows). */
static void dnd_reposition(struct desktop *d)
{
    struct sh_dnd *g = &d->sh.dnd;
    if (d->sh.cfg.icon_auto_arrange) { d->sh.cfg.icon_auto_arrange=0; d->sh.want_save=1; }
    int dx = g->menu ? g->menu_x - g->start_x : d->mouse_x - g->start_x, dy = g->menu ? g->menu_y - g->start_y : d->mouse_y - g->start_y;
    int saved = 1;
    for (int k = 0; k < d->sh.icon_count; k++) if (desk_shell_icon_marked(d, k)) {
        struct desk_box *b = &d->sh.icons[k].box; damage(d, *b);
        b->x += dx; b->y += dy;
        if (b->x < 0) b->x = 0;
        if (b->x + b->w > (int)d->width) b->x = (int)d->width - b->w;
        if (b->y < DESK_BAR + 10) b->y = DESK_BAR + 10;
        if (b->y + b->h > (int)d->height - DESK_BAR) b->y = (int)d->height - DESK_BAR - b->h;
        if (cfg_icon_move_at(&d->sh.cfg, icon_position_name(d, k),d->width,d->height, b->x, b->y)) d->sh.want_save = 1; else saved = 0;
        damage(d, *b);
    }
    d->sh.background_valid = 0;
    if (!saved) desk_shell_message(d, "Desktop", "No space remains for icon positions in desktop.cfg.", 1);
}
static void dnd_commit(struct desktop *d, unsigned effect)
{
    struct sh_dnd *g = &d->sh.dnd;
    if (!g->active) return;
    int x = g->menu ? g->menu_x : d->mouse_x, y = g->menu ? g->menu_y : d->mouse_y;
    unsigned performed = FM_EFFECT_NONE;
    if (effect && (effect & g->allowed) == effect) switch (g->kind) {
    case SH_DROP_START: case SH_DROP_QUICK: {
        int paths_fit=1;for (unsigned k=0;k<g->count;k++) if (strlen(g->paths[k])>=CFG_PATH_MAX) paths_fit=0;
        if (!paths_fit) { desk_shell_message(d,"Shortcuts","One of the paths is too long to create a shortcut.",1);break; }
        if (!d->sh.edit.operation && g->count && g->count<=SH_FOLDER_ITEMS) {
            custom_request(d,SH_EDIT_ADD,NULL,g->destination,NULL);
            d->sh.edit.sources_count=g->count;
            for (unsigned k=0;k<g->count;k++) snprintf(d->sh.edit.sources[k],CFG_PATH_MAX,"%s",g->paths[k]);
            performed=FM_EFFECT_LINK;
        } else desk_shell_message(d,"Shortcuts","Another shell operation is in progress, or too many items were selected. Try a smaller selection.",1);
        break;
    }
    case SH_DROP_TOOLBAR:
        if (g->count==1 && !d->sh.edit.operation) { custom_request(d,SH_EDIT_TOOLBAR,g->paths[0],NULL,NULL);performed=FM_EFFECT_LINK; }
        break;
    case SH_DROP_FILES:
        if (g->target >= 0 && files_target(d, g->target)) {
            struct desk_box b = desk_content(&d->clients[g->target]);
            uint32_t w[4] = {g->serial, (uint32_t)(x - b.x), (uint32_t)(y - b.y), effect};
            dnd_send(d, g->target, DESK_DRAG_DROP, 20, w, 4); performed = effect;
        }
        break;
    case SH_DROP_PROGRAM: {
        const struct sh_icon *ic=&d->sh.icons[g->target];
        struct desk_launch_record *record=calloc(1,sizeof(*record)); int r=-ENOMEM;
        if (record) {
            const char *linkpath=ic->action==SH_ACT_WIN32_LINK ? icon_position_name(d,g->target) : ic->argument;
            const char *ext=strrchr(linkpath,'.');
            if (ic->action==SH_ACT_WIN32_LINK || (ext && !strcasecmp(ext,".lnk"))) { r=desk_folder_link_read(d,linkpath,record); if (!r) r=shortcut_resolve(d,record,0); }
            else { r=strlen(ic->argument)>=sizeof(record->target) ? -ENAMETOOLONG : 0;
                if (!r) { strcpy(record->target,ic->argument); snprintf(record->label,sizeof(record->label),"%.63s",ic->label); record->show=1; } }
            unsigned at=(unsigned)strlen(record->arguments);
            for (unsigned i=0;!r && i<g->count;i++) {
                if (strchr(g->paths[i],'"')) { r=-EINVAL; break; }
                size_t n=strlen(g->paths[i]); if (at+n+4>=sizeof(record->arguments)) { r=-ENAMETOOLONG; break; }
                if (at) record->arguments[at++]=' ';
                record->arguments[at++]='"'; memcpy(record->arguments+at,g->paths[i],n); at+=(unsigned)n;
                record->arguments[at++]='"'; record->arguments[at]=0;
            }
            if (!r) r=desk_shell_open_record(d,record,0);
            free(record);
        }
        if (r<0) desk_shell_notify_error(d,"Cannot open dropped items",r); else performed=FM_EFFECT_COPY;
        break;
    }
    case SH_DROP_APP:
        if (g->target >= 0 && app_target(d, g->target)) {
            struct desk_box b = desk_content(&d->clients[g->target]);
            uint32_t w[3] = {g->serial, (uint32_t)(x - b.x), (uint32_t)(y - b.y)};
            if (dnd_send(d, g->target, DESK_DROP_FILES, 16, w, 3)) {
                performed = FM_EFFECT_COPY; desk_focus(d, g->target);
            }
        }
        break;
    case SH_DROP_DESKTOP: case SH_DROP_ICON: case SH_DROP_TRASH: {
        if (g->kind == SH_DROP_DESKTOP && g->source < 0 && effect == FM_EFFECT_MOVE) { dnd_reposition(d); performed = effect; break; }
        if (!g->count) break;
        int r = g->kind == SH_DROP_TRASH ? desk_folder_delete_request(d, g->paths, g->count, 0) :
                desk_folder_transfer(d, g->paths, g->count, g->destination, effect, 0, g->kind == SH_DROP_DESKTOP, x, y);
        if (r == -EBUSY) desk_shell_message(d, "Desktop", "Another file operation is in progress.\nTry again when it has finished.", 1);
        else if (r) desk_shell_notify_error(d, "Desktop", r);
        else performed = effect;
        break;
    }
    default: break;
    }
    else if (g->kind == SH_DROP_TASK && !g->menu)
        desk_shell_message(d, "Taskbar", "You cannot drop an item onto a button on the taskbar.\n\nHowever, if you drag the item over a button "
                           "without releasing the mouse button, the window will come forward after a moment, allowing you to drop the item inside the window.", 0);
    if (g->kind == SH_DROP_FILES && !performed) dnd_over(d, 1);
    dnd_finish(d, performed);
}
int desk_shell_drag_motion(struct desktop *d)
{
    struct sh_dnd *g = &d->sh.dnd;
    if (!g->active) return 0;
    if (g->menu) return 1;
    dnd_evaluate(d); dnd_image_damage(d);
    return 1;
}
void desk_shell_drag_status(struct desktop *d, int client, uint32_t serial, unsigned effect, unsigned allowed)
{
    struct sh_dnd *g = &d->sh.dnd;
    if (!g->active || g->menu || serial != g->serial || g->kind != SH_DROP_FILES || client != g->target) return;
    g->allowed = allowed & g->source_allowed; g->effect = effect & g->allowed; g->status_target = client;
    desk_cursor_update(d);
}
/* Release of the drag button drops; a right drag asks first. Returns 0 so
 * the release still reaches a source window that holds the button. */
int desk_shell_drag_button(struct desktop *d, int button, int down)
{
    struct sh_dnd *g = &d->sh.dnd;
    if (!g->active || g->menu) return 0;
    if (down) return 1;                  /* no chords during a drag */
    if ((unsigned)button != g->button) return 1;
    if (g->button == CIUKI_BUTTON_RIGHT && g->kind != SH_DROP_NONE && g->kind != SH_DROP_TASK && g->allowed) {
        /* The menu's default is the effect without modifier keys. */
        unsigned keys = d->sh.modifiers; d->sh.modifiers &= ~(unsigned)(DU_CTRL | DU_SHIFT | DU_ALT);
        if (g->kind != SH_DROP_FILES) dnd_evaluate(d);
        else g->effect = fm_drop_effect(g->source_volume, fm_volume(g->destination[0] ? g->destination : "/"), 0, g->allowed);
        d->sh.modifiers = keys;
        if (g->image.w) damage(d, g->image);
        g->image = (struct desk_box){0, 0, 0, 0};
        g->menu = 1; g->menu_x = d->mouse_x; g->menu_y = d->mouse_y;
        desk_menu_open(d, SH_MENU_DRAG, d->mouse_x, d->mouse_y);
        desk_cursor_update(d);
        return 0;
    }
    dnd_commit(d, g->effect);
    return 0;
}
static void dnd_key(struct desktop *d)
{
    struct sh_dnd *g = &d->sh.dnd;
    if (!g->active || g->menu) return;
    unsigned keys = dnd_keys(d);
    if (keys != g->keys) { dnd_evaluate(d); desk_cursor_update(d); }
}
static void dnd_tick(struct desktop *d)
{
    struct sh_dnd *g = &d->sh.dnd;
    if (!g->active || g->menu) return;
    if (g->start_hover_ns && d->now_ns-g->start_hover_ns>=500000000ull && !d->menu_open) {
        g->opened_start=1;desk_menu_open(d,SH_MENU_START,0,0);dnd_evaluate(d);
    }
    /* OLE repeats DragOver while the pointer rests (auto-scroll, hover). */
    if (g->kind == SH_DROP_FILES && d->now_ns - g->repeat_ns >= 100000000ull) dnd_over(d, 0);
    /* Windows 98: resting on a taskbar button brings its window forward. */
    if (g->hover_task >= 0 && g->hover_ns && d->now_ns - g->hover_ns >= 1000000000ull) {
        int i = g->hover_task; g->hover_ns = 0;
        if (d->clients[i].window) {
            if (d->clients[i].minimized) desk_minimize(d, i);
            desk_focus(d, i);
        }
    }
}
/* The drag image: the dragged icons, translucent, following the pointer. */
static void draw_drag_image(struct desktop *d, struct desk_box clip)
{
    struct sh_dnd *g = &d->sh.dnd;
    struct desk_box image = dnd_image_box(d), b;
    if (!image.w || !desk_intersect(image, clip, &b)) return;
    struct du_canvas c = canvas(d, b);
    struct desk_paint p = painter(&c);
    int dx = d->mouse_x - g->start_x, dy = d->mouse_y - g->start_y;
    if (g->source < 0) {
        for (unsigned n = 0; n < g->icon_count; n++) {
            const struct sh_icon *ic = &d->sh.icons[g->icons[n]];
            int x = ic->box.x + dx, y = ic->box.y + dy;
            du_icon_blit(&c, x + (ICON_CELL_W - SH_DESKTOP_ICON) / 2, y + 4, desk_shell_icon(ic->icon, SH_DESKTOP_ICON), SH_DESKTOP_ICON, DU_ICON_GHOST);
            char lines[2][32]; int count = label_lines(d,ic->label, lines,ic->box.w-desk_scale(12)), widest = 0;
            for (int l = 0; l < count; l++) { int w = desk_text_measure(SH_LABEL_FONT, lines[l]); if (w > widest) widest = w; }
            desk_paint_round_blend(&p, x + (ICON_CELL_W - widest) / 2 - 5, y + LABEL_TOP + 1, widest + 10, count * LABEL_PITCH + 4, 4,
                                   SH_HOT, 110, SH_HOT, 140);
            for (int l = 0; l < count; l++)
                desk_paint_text(&p, SH_LABEL_FONT, x + (ICON_CELL_W - desk_text_measure(SH_LABEL_FONT, lines[l])) / 2,
                                y + LABEL_TOP + LABEL_BASE + l * LABEL_PITCH, lines[l], 0xffffffu);
        }
        return;
    }
    unsigned rows = g->count < 4 ? g->count : 4;
    for (unsigned r = 0; r < rows; r++) {
        const struct desk_catalog_entry *cat = g->types[r] == 'D' ? NULL : desk_catalog_file(g->paths[r]);
        const char *icon = g->types[r] == 'D' ? "folder" : cat ? cat->tango : "text-x-generic";
        int x = image.x, y = image.y + (int)r * 36;
        du_icon_blit(&c, x, y, desk_shell_icon(icon, 32), 32, DU_ICON_GHOST);
        char label[40]; snprintf(label, sizeof(label), "%.39s", fm_basename(g->paths[r]));
        int w = du_text_width(label); if (w > 170) w = 170;
        desk_paint_round_blend(&p, x + 36, y + 7, w + 8, 18, 3, SH_HOT, 110, SH_HOT, 140);
        du_text_fit(&c, x + 40, y + 9, 170, label, SH_HOT_INK);
    }
    if (g->count > 4) {
        char more[48]; snprintf(more, sizeof(more), "and %u more", g->count - 4);
        int w = du_text_width(more);
        desk_paint_round_blend(&p, image.x + 36, image.y + (int)rows * 36, w + 8, 16, 3, SH_HOT, 110, SH_HOT, 140);
        du_text(&c, image.x + 40, image.y + (int)rows * 36 + 1, more, SH_HOT_INK);
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
            draw_band(d,clip);
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
    draw_band(d, clip);
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
    int size=desk_scale(DESK_START_WELL), pad=(size-b.w)/2;
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
    int open = d->menu_open, hot = (d->sh.start_hot || (d->sh.taskbar_key && d->sh.task_pick == DESK_CLIENTS)) && !open;
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
    int shift = open ? desk_scale(DESK_START_PRESSED_SHIFT) : 0;
    int mark_size=desk_scale(SH_START_MARK);
    int x = b.x + desk_scale(DESK_START_MARK_X) + shift;
    int y = b.y + desk_scale(DESK_START_MARK_Y) + shift;
    const uint32_t *mark=desk_shell_icon("ciuki",mark_size);
    if (mark) for (int yy=0; yy<mark_size; yy++) for (int xx=0; xx<mark_size; xx++) {
        uint32_t src=mark[yy*mark_size+xx];
        /* Mask in the enclosing orb's coordinates: retain the complete
         * approved dog's silhouette, including ears near the mark corners. */
        unsigned coverage=circle_coverage(x+xx-b.x,y+yy-b.y,b.w);
        desk_paint_blend(&p,x+xx,y+yy,1,1,src,(src>>24)*coverage/16);
    }
}
void desk_shell_bar(struct desktop *d,struct desk_box clip)
{
    struct desk_box b=desk_taskbar(d);struct du_canvas c=canvas(d,clip);struct desk_paint p=painter(&c);
    desk_paint_bar(&p,b.x,b.y,b.w,b.h,desk_theme->taskbar,d->lfb_bytes_per_pixel==2);
    draw_taskbar(d,clip);
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
    if (d->sh.notification.active) draw_notification(d,clip);
    for (int l = 0; l < d->sh.levels; l++) draw_menu(d, &d->sh.menus[l], clip);
    if (d->sh.volume_open) draw_volume(d, clip);
    if (d->sh.network_open) draw_network(d, clip);
    if (d->sh.media_open) draw_media(d,clip);
    if (d->sh.tooltip && !d->sh.notification.active && !d->sh.volume_open && !d->sh.network_open && !d->sh.media_open && !d->sh.levels) draw_tooltip(d, clip);
    if (d->sh.switcher) draw_switcher(d, clip);
    if (d->sh.dnd.active) draw_drag_image(d, clip);
    if (d->sh.user_gate>0) logon_paint(d,clip);
    if (desk_access_flash_draw) desk_access_flash_draw(d, clip);   /* G25 SoundSentry: inverts what was painted */
}
/* Opaque overlay bodies, shared with foreign-surface clipping. Shadows and
 * the notification's tail remain dependent on the pixels underneath. */
unsigned desk_shell_occluders(const struct desktop *d,struct desk_box *boxes)
{
    unsigned n=0;if(!d->shell)return 0;
    if (d->sh.user_gate>0) { boxes[0]=(struct desk_box){0,0,(int)d->width,(int)d->height}; return 1; }
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
/* fx-15: the shell's part of desk_scene_reconcile (compositor.c). The
 * taskbar repaints a task button whose window appeared, vanished, was
 * renamed, activated, minimised, hovered or changed its icon (the
 * Windows 9x shell hook: HSHELL_WINDOWCREATED/DESTROYED/ACTIVATED/REDRAW);
 * a popup (menu, tray popup, notification, switcher, tooltip) that opens,
 * closes or moves exposes its old box and paints its new one; the focus
 * outline of the selected desktop icon follows the desktop's activation
 * (the list view's WM_SETFOCUS/WM_KILLFOCUS repaint). */
static int tooltip_visible(const struct desktop *d)
{ return d->sh.tooltip && !d->sh.notification.active && !d->sh.volume_open && !d->sh.network_open && !d->sh.media_open && !d->sh.levels; }
void desk_shell_reconcile(struct desktop *d)
{
    bind(d);
    for (int i=0;i<DESK_CLIENTS;i++) {
        struct desk_task_shown now={.box=desk_task_button(d,i)}, *was=&d->task_shown[i];
        const struct desk_client *c=&d->clients[i];
        if (now.box.w>0) {
            now.icon=client_icon(d,i);
            now.flags=((d->focus==i && !c->minimized) != !!c->flash ? 1u : 0u)|(d->sh.hover_target==16+i ? 2u : 0u)|(c->minimized ? 4u : 0u)|
                (d->sh.taskbar_key && d->sh.task_pick==i ? 8u : 0u);
            snprintf(now.title,sizeof(now.title),"%s",c->closing ? "Closing..." : c->unresponsive ? "Not responding" : c->title);
        }
        if (memcmp(&was->box,&now.box,sizeof(now.box)) || was->icon!=now.icon || was->flags!=now.flags || strcmp(was->title,now.title)) {
            damage(d,was->box); damage(d,now.box);
        }
        *was=now;
    }
    int start_focus=d->sh.taskbar_key && d->sh.task_pick==DESK_CLIENTS;
    if (start_focus!=d->sh.start_focus_shown) { damage(d,desk_start_bounds(d)); d->sh.start_focus_shown=start_focus; }
    struct desk_box popups[SH_LEVELS+6]={{0}};
    for (int l=0;l<d->sh.levels && l<SH_LEVELS;l++) popups[l]=outer(d->sh.menus[l].box);
    if (d->sh.volume_open) popups[SH_LEVELS]=outer(volume_popup(d));
    if (d->sh.network_open) popups[SH_LEVELS+1]=outer(network_popup(d));
    if (d->sh.media_open) popups[SH_LEVELS+2]=outer(media_popup(d));
    if (d->sh.notification.active) popups[SH_LEVELS+3]=notification_outer(d);
    if (d->sh.switcher) popups[SH_LEVELS+4]=outer(switcher_box(d));
    for (unsigned k=0;k<SH_LEVELS+6;k++) {
        if (memcmp(&popups[k],&d->popup_shown[k],sizeof(popups[k]))) { damage(d,d->popup_shown[k]); damage(d,popups[k]); }
        d->popup_shown[k]=popups[k];
    }
    struct desk_box tip={0,0,0,0}; uint32_t hash=0;
    if (tooltip_visible(d)) {
        char text[1024]; tooltip_text(d,text,sizeof(text));
        hash=2166136261u;
        for (const unsigned char *t=(const unsigned char *)text;*t;t++) hash=(hash^*t)*16777619u;
        tip=tooltip_box(d);
    }
    if (memcmp(&tip,&d->tip_shown,sizeof(tip)) || hash!=d->tip_shown_hash) { damage(d,d->tip_shown); damage(d,tip); }
    d->tip_shown=tip; d->tip_shown_hash=hash;
    int icon=d->sh.cfg.desktop_icons && d->sh.icon_selected>=0 && d->sh.icon_selected<d->sh.icon_count ? d->sh.icon_selected : -1;
    struct desk_box outline=icon>=0 && d->focus<0 && !d->sh.levels ? d->sh.icons[icon].box : (struct desk_box){0,0,0,0};
    if (icon!=d->outline_icon || memcmp(&outline,&d->outline_shown,sizeof(outline))) { damage(d,d->outline_shown); damage(d,outline); }
    d->outline_icon=icon; d->outline_shown=outline;
    /* fx-15b (fx-19 state): the desktop marquee and the drag image are
     * screen objects of their own. A band that ends or changes by any path
     * (a rebuilt icon list ends it, icons_build) repaints its old and new
     * outline; the drag image follows the pointer wherever the pointer
     * moved (input, a v3 warp or clip), its "as painted" record included. */
    struct desk_box band={0,0,0,0};
    if (d->sh.band) { band=band_box(d); if (band.w<2 && band.h<2) band=(struct desk_box){0,0,0,0}; }
    if (memcmp(&band,&d->band_shown,sizeof(band))) {
        if (d->band_shown.w) band_damage(d,d->band_shown);
        if (band.w) band_damage(d,band);
    }
    d->band_shown=band;
    struct desk_box image=dnd_image_box(d);
    if (memcmp(&image,&d->sh.dnd.image,sizeof(image))) dnd_image_damage(d);
    /* About > System lists the number of desktop windows. */
    int windows=d->about>=0 && d->clients[d->about].window && d->sh.about_tab==1 ? desk_task_count(d) : -1;
    if (windows!=d->about_windows && windows>=0) damage(d,desk_content(&d->clients[d->about]));
    d->about_windows=windows;
}
/* WM-11: a click on the caption icon opens the window menu under it; a second
 * click within the double-click time closes the window (Windows 98). The
 * click that merely dismissed the open menu does not reopen it. */
int desk_shell_caption_icon(struct desktop *d, int i, uint64_t stamp, int menu_was_open)
{
    if (!d->shell || i < 0 || i >= DESK_CLIENTS || !d->clients[i].window) return 0;
    uint64_t interval = (uint64_t)d->sh.cfg.double_click_ms * 1000000ull;
    int again = d->icon_click_window == i && d->icon_click_ns && stamp >= d->icon_click_ns && stamp - d->icon_click_ns <= interval;
    if (again) {
        d->icon_click_window = -1;
        if (d->sh.levels) desk_menu_close(d);
        desk_request_close(d, i);
        return 1;
    }
    if (menu_was_open && d->icon_click_window == i) { d->icon_click_window = -1; return 1; }
    d->icon_click_window = i; d->icon_click_ns = stamp;
    d->sh.menu_owner = i;
    desk_menu_open(d, SH_MENU_TASK, d->clients[i].frame.x, d->clients[i].frame.y + DESK_TITLE);
    return 1;
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
/* G3: the Windows 98 texts, with CiukiOS for Windows. */
static const char *end_task_text(const struct desktop *d)
{
    return d->end_task.kind==DESK_HANG_DOS ?
        "Windows cannot shut down this program automatically. It is recommended that you quit this program with its quit or exit command.\n\n"
        "If you close it now, you will lose any unsaved information in this program. Do you want to close it now?" :
        "This program is not responding. It may be busy, waiting for a response from you, or it may have stopped running.\n\n"
        "Click Cancel to return to CiukiOS and check the status of the program.\n\n"
        "Click End Task to close the program immediately. You will lose any unsaved information in this program.";
}
static void internal_size(const struct desktop *d, int kind, uint32_t *w, uint32_t *h)
{
    switch (kind) {
    case DESK_INTERNAL_END_TASK: {
        int width,height;
        du_message_size(end_task_text(d),DU_DLG_ICON_WARNING,2,420,&width,&height);
        if (width<400) width=400;
        *w=(uint32_t)width; *h=(uint32_t)height;
        break;
    }
    case DESK_INTERNAL_GAME_DATA: *w=480; *h=230; break;
    case DESK_INTERNAL_SHORTCUT_WIZARD: *w=520; *h=380; break;
    case DESK_INTERNAL_TRANSFER: *w = 454; *h = 230; break;
    case DESK_INTERNAL_SHORTCUT: *w=548; *h=430; break;
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
        du_message_size(d->v3_dialog_kind==1&&d->v3_details_open?d->v3_details:d->sh.message_text,d->sh.message_kind,d->sh.confirm ? 2 : 1,(int)d->width-20,&width,&height);
        if(d->v3_dialog_kind==1){if(width<420)width=420;if(d->v3_details_open){width=610;height=320;}}
        *w=(uint32_t)width; *h=(uint32_t)height;
    }
    }
    if (*w + 20 > d->width) *w = d->width > 40 ? d->width - 20 : 1;
    if (*h + 2 * DESK_BAR + 30 > d->height) *h = d->height > (uint32_t)(2 * DESK_BAR + 60) ? d->height - 2 * DESK_BAR - 30 : 1;
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
        desk_sound(d,kind == DESK_INTERNAL_SHUTDOWN ? SOUND_QUESTION : kind==DESK_INTERNAL_END_TASK ? du_message_sound(DU_DLG_ICON_WARNING) : SOUND_WINDOW_OPEN);
    if (!++d->next_window_id) ++d->next_window_id;
    c->window_id = d->next_window_id;
    c->info.width = w; c->info.height = h;
    snprintf(c->title, sizeof(c->title), "%s", kind==DESK_INTERNAL_SHORTCUT_WIZARD ? "Create Shortcut" : kind == DESK_INTERNAL_ABOUT ? "About CiukiOS" : kind == DESK_INTERNAL_RUN ? "Run" :
             kind == DESK_INTERNAL_SHUTDOWN ? "Shut Down CiukiOS" : kind == DESK_INTERNAL_POWER ? "Shutting down..." : kind == DESK_INTERNAL_AUTOPLAY ? "AutoPlay" : kind==DESK_INTERNAL_PROGRAMS ? "Programs" :
             kind==DESK_INTERNAL_END_TASK ? d->end_task.name : d->sh.message_title);
    c->frame = desk_frame(w, h, 0, DESK_BAR);
    if (kind == DESK_INTERNAL_ABOUT) { d->about = i; d->sh.about_tab = 0; d->sh.about_scroll = 0; d->sh.want_reload |= 2; }
    if (kind == DESK_INTERNAL_RUN) {
        d->sh.run_length=0; d->sh.run_text[0]=0;
        d->sh.run=calloc(1,sizeof(*d->sh.run));
        if (!d->sh.run) { c->internal=0; c->window=0; return -1; }
        d->sh.run->selected=-1; d->sh.run_history_request=1;
    }
    if (kind == DESK_INTERNAL_RUN) { d->sh.prompt_mode=0; d->sh.run_length = 0; d->sh.run_text[0] = 0; }
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
    if (d->clients[index].internal==DESK_INTERNAL_SHORTCUT_WIZARD && d->sh.folder) { d->sh.folder->wizard_window=-1; desk_folder_shortcut_close(d); }
    if (d->about == index) d->about = -1;
    if (d->clients[index].internal==DESK_INTERNAL_RUN && d->sh.run) {
        struct desk_run_ui *r=d->sh.run;
        if (r->browser) du_dialog_close(r->browser);
        free(r->browser); free(r->pixels); free(r->entries); free(r); d->sh.run=NULL;
        d->sh.run_history_request=0;
    }
    if (d->clients[index].internal==DESK_INTERNAL_MESSAGE && d->sh.confirm) {   /* closing the question answers No */
        int action=d->sh.confirm; d->sh.confirm=0;
        if (action>=SH_CONFIRM_ACCESS) { if (desk_access_confirmed) desk_access_confirmed(d,action-SH_CONFIRM_ACCESS,0); }
        else desk_folder_cancelled(d);
    }
    if (d->clients[index].internal==DESK_INTERNAL_MESSAGE && d->v3_dialog_kind) {
        if(d->v3_dialog_kind==1)(void)desk_v3_dialog_accept(d);
        d->v3_dialog_kind=0; d->v3_dialog_pid=0;
    }
    if (d->clients[index].internal == DESK_INTERNAL_AUTOPLAY) d->sh.autoplay.open=0;
    /* G3: closing the dialog itself (title button, Esc) is Cancel / No. */
    if (d->clients[index].internal == DESK_INTERNAL_END_TASK && d->end_task.kind && d->end_task.window==index) {
        d->end_task.window=-1; desk_end_task_choice(d,0);
    }
}
void desk_shell_message(struct desktop *d, const char *title, const char *text, int error)
{
    desk_shell_message_class(d,title,text,error ? DU_DLG_ICON_ERROR : DU_DLG_ICON_INFO);
}
void desk_shell_confirm(struct desktop *d, const char *title, const char *text, int icon, int action)
{
    for (int i=0;i<DESK_CLIENTS;i++) if (d->clients[i].window && d->clients[i].internal==DESK_INTERNAL_MESSAGE) desk_drop_client(d,i);
    snprintf(d->sh.message_title, sizeof(d->sh.message_title), "%s", title);
    snprintf(d->sh.message_text, sizeof(d->sh.message_text), "%s", text);
    d->sh.message_kind = icon; d->sh.confirm = action;
    desk_sound(d,du_message_sound(icon));
    if (!d->shell) { d->sh.confirm = 0; desk_folder_cancelled(d); return; }
    desk_internal_open(d, DESK_INTERNAL_MESSAGE);
}
void desk_shell_bin_state(struct desktop *d, int full)
{
    if (d->sh.icon_count > 2 && !strcmp(d->sh.icons[2].label, "Recycle Bin")) {
        d->sh.icons[2].icon = d->sh.cfg.recycle_icon ? "edit-delete" : full ? "user-trash-full" : "user-trash";
        icon_repaint(d, 2);
    }
}
static void confirm_answer(struct desktop *d, int index, int yes)
{
    int action = d->sh.confirm; d->sh.confirm = 0;
    if (action >= SH_CONFIRM_ACCESS) { if (desk_access_confirmed) desk_access_confirmed(d, action - SH_CONFIRM_ACCESS, yes); }
    else if (yes) desk_folder_confirmed(d, action); else desk_folder_cancelled(d);
    desk_request_close(d, index);
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
#include "run_ui.inc"
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
        du_text(c, tx, y + 36 + 4 * step, !d->realtime_source ? "Clock: build seed (UTC)" : (d->time_flags & CIUKI_TIME_RTC_LOCAL) ? "Clock: RTC (local time)" : "Clock: RTC (UTC)", SH_INK);
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
#include "shell_g18.h"

static int programs_entry(const struct desktop *d, int row)
{
    for (unsigned k=0;k<d->v3_link_count;k++) if (!d->v3_links[k].desktop && row--==0) return (int)k;
    return -1;
}
static void programs_open_selected(struct desktop *d)
{
    int k=programs_entry(d,d->sh.programs_selected);
    if (k>=0 && desk_link_launch(d,(unsigned)k)<0) desk_shortcut_properties(d,(unsigned)k,1);
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
    case DESK_INTERNAL_SHORTCUT: shortcut_paint(d,index,&c,content); break;
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
    case DESK_INTERNAL_SHORTCUT_WIZARD:
        desk_folder_shortcut_paint(d,content,clip); return;
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
        struct desk_run_ui *r=d->sh.run;
        if (r && r->browser) { run_browser_paint(d,content,clip); break; }
        du_icon_blit(&c, content.x + 12, content.y + 12, desk_shell_icon("go-jump", 32), 32, DU_ICON_NORMAL);
        multiline(&c, content.x + 56, content.y + 14, "Type the name of a program, folder, document,\nor Internet address, and CiukiOS will open it.", SH_INK);
        multiline(&c, content.x + 56, content.y + 14, d->sh.prompt_mode==1 ? "Type an address, folder or file to open." : d->sh.prompt_mode==2 ? "Type the existing folder to use as a new toolbar." : d->sh.prompt_mode==3 ? "Type a new name for this Start menu item." : "Type the name of a program in /Programs, followed by\nits arguments, and CiukiOS will start it.", SH_INK);
        du_label(&c, content.x + 12, content.y + 52 + (22 - DU_GLYPH_H) / 2, "&Open:", SH_INK, 1);
        struct du_rect field = {content.x + 56, content.y + 52, content.w - 88, 22};
        du_well(&c, field, SH_WINDOW);
        if (d->focus == index && desk_theme->focus_style == DESK_FOCUS_RING) du_focus_ring(&c, field);
        struct du_canvas fc = c; fc.clip = du_intersection(c.clip, (struct du_rect){field.x + 2, field.y + 2, field.w - 4, field.h - 4});
        /* Show the end of a long command line: drop leading characters until it fits. */
        int32_t room = field.w - 12;
        const char *shown = d->sh.run_text;
        unsigned cursor=r ? r->cursor : d->sh.run_length;
        if (cursor>d->sh.run_length) cursor=d->sh.run_length;
        while (*shown && shown<d->sh.run_text+cursor && du_text_width(shown)>room) shown+=du_utf8_next(shown,0);
        char prefix[DESK_RUN_MAX]; unsigned prefix_bytes=cursor-(unsigned)(shown-d->sh.run_text);
        memcpy(prefix,shown,prefix_bytes); prefix[prefix_bytes]=0;
        int32_t tx = field.x + 5, ty = field.y + (field.h - DU_GLYPH_H) / 2;
        if (r && r->select_all && d->focus==index) du_fill(&fc,(struct du_rect){tx,ty-1,du_text_width(shown),DU_GLYPH_H+2},SH_HOT);
        du_text(&fc, tx, ty, shown, r && r->select_all && d->focus==index ? SH_HOT_INK : SH_INK);
        if (d->focus == index && (!r || (!r->focus && !r->select_all)) && !d->shell_caret_hidden) du_vline(&fc, tx + du_text_width(prefix), ty - 2, DU_GLYPH_H + 5, SH_INK);
        push_button(&c,(struct desk_box){content.x+content.w-32,content.y+52,20,22},"v",0);
        if (r && r->dropdown) {
            struct du_rect list={content.x+56,content.y+76,content.w-68,run_rows(r)*18+4}; du_well(&c,list,SH_WINDOW);
            for (int row=0;row<run_rows(r);row++) {
                int k=r->top+row; int selected=k==r->selected;
                if (selected) du_fill(&c,(struct du_rect){list.x+2,list.y+2+row*18,list.w-4,18},SH_HOT);
                du_text_fit(&c,list.x+5,list.y+4+row*18,list.w-10,r->history.commands[(unsigned)(r->history.order[k]-'a')],selected?SH_HOT_INK:SH_INK);
            }
        }
        static const char *const labels[]={"OK","Cancel","&Browse..."};
        for (int k=0;k<3;k++) {
            struct desk_box b=button_box(content,k,3); push_button(&c,b,labels[k],k==0);
            if (r && r->focus==k+1) du_focus_ring(&c,(struct du_rect){b.x+4,b.y+4,b.w-8,b.h-8});
        }
        break;
    }
    case DESK_INTERNAL_SHUTDOWN: {
        du_icon_blit(&c, content.x + 14, content.y + 16, desk_shell_icon("system-shutdown", 32), 32, DU_ICON_NORMAL);
        du_icon_blit(&c, content.x + 30, content.y + 32, desk_shell_icon("ciuki", 20), 20, DU_ICON_NORMAL);
        bold(&c, content.x + 64, content.y + 18, "What do you want CiukiOS to do?", SH_INK);
        multiline(&c, content.x + 64, content.y + 38, "Your settings are saved first. Programs\nand virtual machines will be closed.", SH_INK);
        static const char *const labels[] = {"Stand &by", "&Restart", "&Shut Down", "Cancel"};
        /* shutdown_choice 1: Restart is the default (Ctrl+Alt+Del pressed again in Task Manager).
         * G11: Stand by comes first, as in Windows 98, only where it is qualified. */
        int first = d->sh.power_can_standby ? 0 : 1, count = 4 - first;
        for (int k = 0; k < count; k++)
            push_button(&c, button_box(content, k, count), labels[k + first], k + first == (d->sh.shutdown_choice == 1 ? 1 : 2));
        break;
    }
    case DESK_INTERNAL_END_TASK:
        du_icon_blit(&c, content.x + 14, content.y + 14, desk_shell_icon(du_message_icon_name(DU_DLG_ICON_WARNING), 32), 32, DU_ICON_NORMAL);
        du_text_wrap(&c,(struct du_rect){content.x+60,content.y+16,content.w-76,content.h-67},end_task_text(d),SH_INK);
        push_button(&c, button_box(content, 0, 2), d->end_task.kind==DESK_HANG_DOS ? "&Yes" : "&End Task", 0);
        push_button(&c, button_box(content, 1, 2), d->end_task.kind==DESK_HANG_DOS ? "&No" : "Cancel", 1);
        break;
    case DESK_INTERNAL_POWER:
        multiline(&c, content.x + 14, content.y + 18, d->power.text, SH_INK);
        if (d->power.state!=DESK_POWER_DONE)
            push_button(&c, button_box(content, 0, d->power.state==DESK_POWER_BUSY ? 2 : 1), "Cancel", 1);
        if (d->power.state==DESK_POWER_BUSY) push_button(&c, button_box(content, 1, 2), "&Force", 0);
        break;
    default:
        du_icon_blit(&c, content.x + 14, content.y + 14, desk_shell_icon(du_message_icon_name(d->sh.message_kind), 32), 32, DU_ICON_NORMAL);
        du_text_wrap(&c,(struct du_rect){content.x+60,content.y+16,content.w-76,content.h-67},d->v3_dialog_kind==1&&d->v3_details_open?d->v3_details:d->sh.message_text,SH_INK);
        if (d->sh.confirm) {   /* G2: Yes is the default button */
            push_button(&c, button_box(content,0,2), "&Yes", 1); push_button(&c, button_box(content,1,2), "&No", 0);
            break;
        }
        push_button(&c, message_button_box(d,content,0),
                    d->v3_dialog_kind==1 ? "Close" : d->v3_dialog_kind==2 ? "Restart Windows" : "OK",1);
        if (d->v3_dialog_kind==1) push_button(&c,message_button_box(d,content,1),"Details",0);
        if (d->v3_dialog_kind==2) push_button(&c,message_button_box(d,content,1),"Cancel",0);
        break;
    }
}
static void run_commit(struct desktop *d, int index)
{
    struct desk_run_ui *r=d->sh.run;
    if (!d->sh.run_length || !r || r->busy) return;
    desk_run_trim(d->sh.run_text); d->sh.run_length=(unsigned)strlen(d->sh.run_text);
    const char *p=d->sh.run_text; while (*p==' ' || *p=='\t') p++;
    if (!*p) return;
    /* G20: the same prompt opens an address, names a toolbar or renames a Start menu entry; it closes at once. */
    if (d->sh.prompt_mode) {
        if (d->sh.prompt_mode==1) snprintf(d->sh.custom_open,sizeof(d->sh.custom_open),"%.*s",(int)sizeof(d->sh.custom_open)-1,d->sh.run_text);
        else if (d->sh.prompt_mode==2) custom_request(d,SH_EDIT_TOOLBAR,d->sh.run_text,NULL,NULL);
        else if (d->sh.prompt_mode==3) custom_request(d,SH_EDIT_RENAME,d->sh.prompt_path,NULL,d->sh.run_text);
        desk_request_close(d, index);
        return;
    }
    /* G9: Run queues the command and stays open, busy, until desk_run_result reports it. */
    snprintf(d->sh.want_run, sizeof(d->sh.want_run), "%s", d->sh.run_text);
    r->busy=1;
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
    case DESK_INTERNAL_SHORTCUT: return shortcut_click(d,index,x,y);
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
    case DESK_INTERNAL_SHORTCUT_WIZARD: desk_folder_shortcut_click(d,x,y); return 1;
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
    case DESK_INTERNAL_RUN: {
        struct desk_run_ui *r=d->sh.run; if (!r) return 1;
        if (r->browser) {
            struct du_ui *u=r->browser;
            struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=x-content.x+u->dialog.box.x+DESK_BORDER,.value2=y-content.y+u->dialog.box.y+DESK_TITLE};
            run_browser_input(d,&e);
            e=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=CIUKI_BUTTON_LEFT,.value=1}; run_browser_input(d,&e);
            if (r->browser) { e.value=0; run_browser_input(d,&e); }
        } else if (desk_contains(button_box(content,0,3),x,y)) run_commit(d,index);
        else if (desk_contains(button_box(content,1,3),x,y)) desk_request_close(d,index);
        else if (desk_contains(button_box(content,2,3),x,y)) run_browse(d);
        else if (y>=content.y+52 && y<content.y+74 && x>=content.x+content.w-32) { r->dropdown=!r->dropdown; run_resize(d); }
        else if (r->dropdown && x>=content.x+56 && x<content.x+content.w-12 && y>=content.y+78 && y<content.y+78+run_rows(r)*18) {
            run_select(d,r->top+(y-content.y-78)/18); r->dropdown=0; run_resize(d);
        } else { r->focus=0; r->select_all=1; r->cursor=d->sh.run_length; }
        damage(d,desk_outer(cl)); return 1;
    }
    case DESK_INTERNAL_END_TASK:
        if (desk_contains(button_box(content,0,2),x,y)) desk_end_task_choice(d,1);
        else if (desk_contains(button_box(content,1,2),x,y)) desk_end_task_choice(d,0);
        return 1;
    case DESK_INTERNAL_SHUTDOWN: {
        int first = d->sh.power_can_standby ? 0 : 1, count = 4 - first;
        for (int k = 0; k < count; k++) if (desk_contains(button_box(content, k, count), x, y)) {
            int choice = k + first;   /* 0 Stand by, 1 Restart, 2 Shut Down, 3 Cancel */
            if (!choice) d->sh.want_standby = 1;
            else if (choice < 3) d->sh.want_power = choice;
            desk_request_close(d, index);
        }
        return 1;
    }
    default:
        if (d->sh.confirm) {
            if (desk_contains(button_box(content,0,2),x,y)) confirm_answer(d,index,1);
            else if (desk_contains(button_box(content,1,2),x,y)) confirm_answer(d,index,0);
            return 1;
        }
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
    if (!down && button==CIUKI_BUTTON_LEFT && d->sh.bar_drag) {
        d->sh.bar_drag=d->sh.bar_resize=0;d->sh.want_save=1;desk_shell_geometry(d);return 1;
    }
    if (!down && button==CIUKI_BUTTON_LEFT && d->sh.custom_drag>=0) {
        int from=d->sh.custom_drag, dragging=d->sh.custom_dragging;
        d->sh.custom_drag=-1;d->sh.custom_dragging=0;
        for (int l=d->sh.levels-1;l>=0;l--) {
            int hit=menu_hit(&d->sh.menus[l],d->mouse_x,d->mouse_y);
            if (hit>=0) {
                int to=custom_index(d,&d->sh.menus[l].items[hit]);
                if (dragging) { custom_reorder(d,from,to);desk_menu_close(d); }
                else activate(d,l,hit);
                break;
            }
        }
        return 1;
    }
    /* fx-19: a drag session sees the buttons first (drop, right-drag menu);
     * a release still reaches the source window that holds the button. */
    if (d->sh.dnd.active && !d->sh.dnd.menu) return desk_shell_drag_button(d, button, down) && down;
    int32_t x = d->mouse_x, y = d->mouse_y;
    for (unsigned k=0;k<SH_PROGRAM_TRAY;k++) if (desk_contains(program_tray_box(d,k),x,y)) {
        uint32_t message=button==CIUKI_BUTTON_LEFT ? (down ? 0x201u : 0x202u) :
            button==CIUKI_BUTTON_RIGHT ? (down ? 0x204u : 0x205u) : (down ? 0x207u : 0x208u);
        struct sh_program_tray *t=&d->sh.program_tray[k];
        if (down && button==CIUKI_BUTTON_LEFT) {
            if (t->click_ns && d->now_ns-t->click_ns<=(uint64_t)d->sh.cfg.double_click_ms*1000000ull) {
                message=0x203; t->click_ns=0;
            } else t->click_ns=d->now_ns ? d->now_ns : 1;
        }
        program_tray_event(d,k,message); return 1;
    }
    if (down && d->sh.address_focus) {
        int slot=toolbar_hit(d,x,y);struct desk_toolbar_slot slots[DESK_TOOLBAR_SLOTS];int n=desk_toolbar_slots(d,slots);
        if (slot<0 || slot>=n || slots[slot].kind!=DESK_TOOLBAR_ADDRESS) { d->sh.address_focus=0;damage(d,desk_taskbar(d)); }
    }
    if (down && d->focus<0 && d->sh.folder && d->sh.folder->editing>=0) {
        struct desk_folder *f=d->sh.folder;
        struct desk_box b=d->sh.icons[f->editing+3].box;
        b.y+=LABEL_TOP; b.h=24;
        if (desk_contains(b,x,y)) return 1;
        f->command=DF_RENAME; f->index=f->editing; return 1;
    }
    if (!down) {
        if (button == CIUKI_BUTTON_LEFT && d->sh.band) { band_end(d); return 1; }
        if (d->sh.icon_drag>=0 && button==d->sh.icon_button) {
            /* A press without a drag: the deferred click applies now. */
            int index=d->sh.icon_drag, pending=d->sh.icon_pending; d->sh.icon_drag=-1; d->sh.icon_pending=0;
            if (index<d->sh.icon_count && pending==2) mark(d,index,0);
            else if (index<d->sh.icon_count && pending==1) select_only(d,index);
            if (button==CIUKI_BUTTON_RIGHT && index<d->sh.icon_count) {
                d->sh.menu_owner=index; desk_menu_open(d,SH_MENU_ICON,x,y); return 1;
            }
        }
        if (button == CIUKI_BUTTON_LEFT && d->sh.volume_drag) { d->sh.volume_drag = 0; return 1; }
        return 0;
    }
    tooltip_hide(d);
    d->sh.hover_since = d->now_ns;
    if (d->sh.switcher) switcher_finish(d, 0);
    if (d->sh.taskbar_key) taskbar_blur(d, 0);   /* a click moves the focus to what it hits */
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
            if (k==-3 || k==-4) { menu_scroll(d,&d->sh.menus[l],k==-3 ? -SH_SUB_ROW : SH_SUB_ROW);return 1; }
            int ci=k>=0 ? custom_index(d,&d->sh.menus[l].items[k]) : -1;
            if (ci>=0 && button==CIUKI_BUTTON_RIGHT) {
                d->sh.custom_item=ci; desk_menu_close(d); desk_menu_open(d,SH_MENU_START_ITEM,x,y); return 1;
            }
            if (ci>=0 && button==CIUKI_BUTTON_LEFT) {
                d->sh.custom_drag=ci; d->sh.custom_dragging=0; d->sh.custom_press_x=x;d->sh.custom_press_y=y;return 1;
            }
            if (k>=0 && button==CIUKI_BUTTON_RIGHT && d->sh.menus[l].items[k].action==SH_ACT_GAME)
                desk_game_properties(d,(unsigned)d->sh.menus[l].items[k].arg+1);
            else if (k>=0 && button==CIUKI_BUTTON_RIGHT && d->sh.menus[l].items[k].action==SH_ACT_WIN32_LINK)
                desk_shortcut_properties(d,(unsigned)d->sh.menus[l].items[k].arg,0);
            else if (k >= 0 && button != CIUKI_BUTTON_MIDDLE) activate(d, l, k);
            return 1;
        }
        desk_menu_close(d);
    }
    if(button==CIUKI_BUTTON_LEFT && d->sh.cfg.layout_switch && desk_contains(desk_shell_tray_icon_box(d,SH_TRAY_LANGUAGE),x,y)) {
        d->sh.cfg.keyboard^=1;d->text_dead=0;desk_preferences_changed(d);return 1;
    }
    /* G22: handle the second click before the popup's outside-click path. */
    if (button==CIUKI_BUTTON_LEFT && desk_contains(volume_icon_box(d),x,y)) {
        if (d->sh.volume_click_ns && d->now_ns-d->sh.volume_click_ns <=
            (uint64_t)d->sh.cfg.double_click_ms*1000000ull) {
            d->sh.volume_click_ns=0;
            if (d->sh.volume_open) damage(d,outer(volume_popup(d)));
            d->sh.volume_open=0;
            launch_named(d,"mixer",NULL);
            return 1;
        }
        d->sh.volume_click_ns=d->now_ns ? d->now_ns : 1;
    } else d->sh.volume_click_ns=0;
    /* G1: double-clicking the tray clock opens Date/Time Properties. */
    if (button==CIUKI_BUTTON_LEFT && desk_contains(desk_shell_clock_box(d),x,y)) {
        if (d->sh.clock_click_ns && d->now_ns-d->sh.clock_click_ns<=(uint64_t)d->sh.cfg.double_click_ms*1000000ull) {
            d->sh.clock_click_ns=0;
            if (program_index(d,"control")>=0) launch_named(d,"control","--panel=datetime");
        } else d->sh.clock_click_ns=d->now_ns ? d->now_ns : 1;
        return 1;
    }
    if (button==CIUKI_BUTTON_RIGHT && desk_contains(desk_shell_tray_icon_box(d, SH_TRAY_POWER),x,y)) {
        /* G11: right click on the power meter (Windows 98: the battery status
         * and the properties). */
        if (d->sh.volume_open) damage(d,outer(volume_popup(d)));
        if (d->sh.network_open) damage(d,outer(network_popup(d)));
        if (d->sh.media_open) damage(d,outer(media_popup(d)));
        d->sh.volume_open=d->sh.network_open=d->sh.media_open=0;
        desk_menu_open(d,SH_MENU_POWER_METER,x,y); return 1;
    }
    if (button==CIUKI_BUTTON_RIGHT) {
        int page=desk_contains(volume_icon_box(d),x,y) ? CP_SOUNDS :
            desk_contains(desk_shell_tray_icon_box(d, SH_TRAY_ACCESS),x,y) ? CP_ACCESS :
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
            program_balloon_dismiss(d,0x403); damage(d,notification_outer(d)); d->sh.notification.active=0; damage(d,notification_icon_box(d));
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
            if (d->sh.balloon_window) { program_balloon_dismiss(d,close ? 0x403u : 0x405u); return 1; }
            if (close) return 1;
            if (n->action==SH_NOTIFY_NETWORK) {
                if (program_index(d,"control")>=0) launch_named(d,"control","--panel=network");
                else { d->sh.network_open=1; damage(d,outer(network_popup(d))); }
            }
            else if (n->action==SH_NOTIFY_VOLUME) { d->sh.volume_open=1; damage(d,outer(volume_popup(d))); }
            else if (n->action==SH_NOTIFY_MEDIA && (d->sh.media.removable_mask & ~7u)) { d->sh.media_open=1; damage(d,outer(media_popup(d))); }
            else desk_shell_message_class(d,n->title,n->text,n->icon);
        }
        return 1;
    }
    if (d->sh.notification.active) {
        program_balloon_dismiss(d,0x403); damage(d,notification_outer(d)); d->sh.notification.active=0; damage(d,notification_icon_box(d));
    }
    struct desk_box bar = desk_taskbar(d);
    if (y >= 0 && y < DESK_BAR) {
        if (d->sh.resources.visible && desk_contains(desk_shell_tray_icon_box(d, SH_TRAY_RESOURCES),x,y) && button == CIUKI_BUTTON_LEFT) {
            /* G30: the icon opens the running Resource Meter window, as double-clicking it does in Windows 98. */
            for (int i=0;i<DESK_CLIENTS;i++) if (d->clients[i].window && !strcmp(d->clients[i].application,"resmeter")) { desk_task_switch(d,d->clients[i].window_id); break; }
            return 1;
        }
        if (desk_contains(desk_shell_tray_icon_box(d, SH_TRAY_POWER),x,y) && button == CIUKI_BUTTON_LEFT) {
            /* G11: Windows 98's meter: a click changes the power scheme. */
            struct desk_box bi=desk_shell_tray_icon_box(d, SH_TRAY_POWER);
            desk_menu_open(d,SH_MENU_POWER,bi.x,bi.y+bi.h);
            return 1;
        }
        if (desk_contains(desk_shell_tray_icon_box(d, SH_TRAY_ACCESS),x,y) && button == CIUKI_BUTTON_LEFT) {   /* G25: the status indicator opens Accessibility Options */
            if (program_index(d,"control")>=0) launch_named(d,"control","--panel=access");
            return 1;
        }
        if ((d->sh.media.removable_mask & ~7u) && desk_contains(media_icon_box(d),x,y) && (button==CIUKI_BUTTON_LEFT || button==CIUKI_BUTTON_RIGHT)) {
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
        /* WM-12: the empty part of the bar opens the taskbar menu. */
        if (button==CIUKI_BUTTON_RIGHT) { d->sh.menu_owner=-1; desk_menu_open(d, SH_MENU_TASKBAR, x, DESK_BAR); }
        return 1;
    }
    if (desk_contains(bar,x,y) && (desk_taskbar_front(d) || desk_hit(d,x,y)<0)) {
        if (button==CIUKI_BUTTON_LEFT) d->sh.bar_raised=1;
        int tool=toolbar_hit(d,x,y);
        if (button==CIUKI_BUTTON_RIGHT && (tool>=0 || desk_contains(desk_quick_launch_button(d),x,y))) {
            int code=0;
            if (tool>=0) {
                struct desk_toolbar_slot slots[DESK_TOOLBAR_SLOTS];int n=desk_toolbar_slots(d,slots);
                if (tool<n) {
                    int root=slots[tool].kind==DESK_TOOLBAR_ITEM && d->sh.custom ? d->sh.custom->entries[slots[tool].arg].parent : slots[tool].arg;
                    code=slots[tool].kind==DESK_TOOLBAR_ADDRESS ? 1 : root==SH_ROOT_QUICK ? 0 : root==SH_ROOT_LINKS ? 2 : root==SH_ROOT_DESKTOP ? 3 : root-SH_ROOT_CUSTOM+4;
                }
            }
            d->sh.custom_menu_root=code;desk_menu_open(d,SH_MENU_TOOLBAR_CONTEXT,x,y);return 1;
        }
        if (tool>=0 && button==CIUKI_BUTTON_LEFT) { toolbar_activate(d,tool);return 1; }
        int on_button = 0;
        if (desk_contains(desk_quick_launch_button(d),x,y)) {
            if (button==CIUKI_BUTTON_LEFT) (void)desk_show_desktop(d);
            else if (button==CIUKI_BUTTON_RIGHT) { d->sh.menu_owner=-1; desk_menu_open(d,SH_MENU_TASKBAR,x,bar.y); }
            return 1;
        }
        for (int i = 0; i < DESK_CLIENTS; i++) if (desk_contains(desk_task_button(d, i), x, y)) {
            on_button = 1;
            if (button == CIUKI_BUTTON_RIGHT) {
                d->sh.menu_owner = i;
                desk_menu_open(d, SH_MENU_TASK, x, bar.y);
            } else if (button == CIUKI_BUTTON_LEFT) {
                if (d->clients[i].minimized || d->focus == i) desk_minimize(d, i);
                else desk_focus(d, i);
            }
            break;
        }
        /* WM-12: Cascade, Tile, Minimize All and Undo, on the empty part of the taskbar. */
        if (!on_button && button == CIUKI_BUTTON_RIGHT) { d->sh.menu_owner = -1; desk_menu_open(d, SH_MENU_TASKBAR, x, bar.y); }
        if (!on_button && button==CIUKI_BUTTON_LEFT) {
            d->sh.bar_drag=1;d->sh.bar_origin_edge=d->sh.cfg.taskbar_edge;d->sh.bar_origin_size=d->sh.cfg.taskbar_size;
            d->sh.bar_press_x=x;d->sh.bar_press_y=y;
            int edge=d->sh.cfg.taskbar_edge;
            d->sh.bar_resize=edge==CFG_BAR_BOTTOM ? y<bar.y+4 : edge==CFG_BAR_TOP ? y>=bar.y+bar.h-4 : edge==CFG_BAR_LEFT ? x>=bar.x+bar.w-4 : x<bar.x+4;
        }
        return 1;
    }
    if (desk_hit(d, x, y) >= 0) return 0;
    int icon = icon_hit(d, x, y);
    unsigned mods = d->sh.modifiers;
    if (button == CIUKI_BUTTON_LEFT) {
        if (icon >= 0 && icon == d->sh.click_icon && d->now_ns - d->sh.click_ns <= (uint64_t)d->sh.cfg.double_click_ms * 1000000ull) {
            d->sh.click_icon = -1; d->sh.icon_drag = -1; d->sh.icon_pending = 0;
            open_selection(d, icon);
            return 1;
        }
        /* Windows 98 desktop list view: Shift extends from the anchor, Ctrl
         * toggles; a press on a selected icon keeps the selection for a
         * drag and applies the click at release. Empty space: rubber band. */
        d->sh.icon_pending = 0;
        if (icon >= 0) {
            if (mods & DU_SHIFT) select_range(d, icon, (mods & DU_CTRL) != 0);
            else if (mods & DU_CTRL) {
                int was = desk_shell_icon_marked(d, icon);
                icon_focus(d, icon); mark(d, icon, 1); d->sh.anchor = icon;
                if (was) d->sh.icon_pending = 2;
            } else if (desk_shell_icon_marked(d, icon)) { icon_focus(d, icon); mark(d, icon, 1); d->sh.anchor = icon; d->sh.icon_pending = 1; }
            else select_only(d, icon);
        } else {
            if (!(mods & (DU_CTRL | DU_SHIFT))) { select_none(d); icon_focus(d, -1); }
            if (d->sh.cfg.desktop_icons) band_begin(d, x, y);
        }
        d->sh.icon_drag=icon; d->sh.icon_button=CIUKI_BUTTON_LEFT; d->sh.icon_dragged=0; d->sh.drag_start_x=x; d->sh.drag_start_y=y;
        d->sh.click_icon = icon; d->sh.click_ns = d->now_ns;
        return 0;   /* the window manager still clears the focus */
    }
    if (button == CIUKI_BUTTON_RIGHT) {
        if (icon >= 0) {
            /* Right press selects; the menu waits for the release so that a
             * right-drag can start (WM_CONTEXTMENU follows the button up). */
            if (!desk_shell_icon_marked(d, icon)) { if (mods & DU_CTRL) { icon_focus(d, icon); mark(d, icon, 1); } else select_only(d, icon); }
            else { icon_focus(d, icon); mark(d, icon, 1); }
            d->sh.icon_drag=icon; d->sh.icon_button=CIUKI_BUTTON_RIGHT; d->sh.icon_dragged=0; d->sh.icon_pending=0;
            d->sh.drag_start_x=x; d->sh.drag_start_y=y;
        } else desk_menu_open(d, SH_MENU_DESKTOP, x, y);
        return 1;
    }
    return 0;
}
static int hover_target(const struct desktop *d)
{
    int x=d->mouse_x,y=d->mouse_y;
    for (unsigned k=0;k<SH_PROGRAM_TRAY;k++) if (desk_contains(program_tray_box(d,k),x,y)) return 256+(int)k;
    if (desk_contains(clock_box(d),x,y)) return 1;
    if (desk_contains(network_icon_box(d),x,y)) return 2;
    if (desk_contains(volume_icon_box(d),x,y)) return 3;
    if ((d->sh.media.removable_mask & ~7u) && desk_contains(media_icon_box(d),x,y)) return 4;
    if (desk_contains(notification_icon_box(d),x,y)) return 5;
    if (desk_shell_power_shown(d) && desk_contains(desk_shell_tray_icon_box(d,SH_TRAY_POWER),x,y)) return 6;
    if (d->sh.resources.visible && desk_contains(desk_shell_tray_icon_box(d,SH_TRAY_RESOURCES),x,y)) return 7;
    for (int i=0;i<DESK_CLIENTS;i++) if (d->clients[i].window && desk_contains(desk_task_button(d,i),x,y)) return 16+i;
    return 0;
}
void desk_shell_motion(struct desktop *d)
{
    if (!d->shell) return;
    int32_t x = d->mouse_x, y = d->mouse_y;
    if (d->sh.bar_drag) { bar_motion(d);return; }
    if (d->sh.custom_drag>=0 && (abs(x-d->sh.custom_press_x)>4 || abs(y-d->sh.custom_press_y)>4)) d->sh.custom_dragging=1;
    bar_reveal(d);
    if (d->sh.dnd.active && !d->sh.dnd.menu) { desk_shell_drag_motion(d); return; }
    /* SM_CXDRAG/SM_CYDRAG: 4 pixels on either side of the press point. */
    if (d->sh.icon_drag>=0 && !d->sh.icon_dragged && (abs(x-d->sh.drag_start_x)>4 || abs(y-d->sh.drag_start_y)>4)) {
        d->sh.icon_dragged=1; d->sh.click_icon=-1; dnd_desktop_begin(d,(unsigned)d->sh.icon_button);
    }
    if (d->sh.band) band_update(d,x,y);
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
    if (target>=256) program_tray_event(d,(unsigned)target-256,0x200);
    if (target!=d->sh.hover_target) {
        if (d->sh.hover_target>=16 && d->sh.hover_target<16+DESK_CLIENTS) damage(d,desk_task_button(d,d->sh.hover_target-16));
        if (target>=16 && target<16+DESK_CLIENTS) damage(d,desk_task_button(d,target-16));
        tooltip_hide(d); d->sh.hover_target=target;
        d->sh.hover_since=target ? (d->now_ns ? d->now_ns : 1) : 0;
    } else if (target && !d->sh.hover_since) d->sh.hover_since=d->now_ns ? d->now_ns : 1;
}
void desk_shell_tick(struct desktop *d)
{
    if (!d->shell) return;
    int run=-1,editing=d->sh.folder && d->sh.folder->editing>=0;
    for(int i=0;i<DESK_CLIENTS;i++)if(d->clients[i].window && !d->clients[i].minimized &&
        d->clients[i].internal==DESK_INTERNAL_RUN && d->focus==i)run=i;
    unsigned old_caret=d->shell_caret_hidden;
    uint64_t interval=(uint64_t)d->sh.cfg.cursor_blink_ms*1000000;
    if(!editing && run<0) {d->shell_caret_hidden=0;d->shell_caret_next=0;}
    else if(!interval) {d->shell_caret_hidden=0;d->shell_caret_next=UINT64_MAX;}
    else if(!d->shell_caret_next || d->shell_caret_next==UINT64_MAX)d->shell_caret_next=d->now_ns+interval;
    else if(d->now_ns>=d->shell_caret_next) {
        d->shell_caret_hidden^=1;d->shell_caret_next=d->now_ns+interval;
    }
    if(old_caret!=d->shell_caret_hidden) {
        if(run>=0)damage(d,desk_content(&d->clients[run]));
        if(editing)icon_repaint(d,d->sh.folder->editing+3);
    }
    while(d->pointer_trail_count && d->pointer_trail[d->pointer_trail_count-1].until<=d->now_ns) {
        unsigned k=--d->pointer_trail_count;
        damage(d,(struct desk_box){d->pointer_trail[k].x-64,d->pointer_trail[k].y-64,128,128});
    }
    bar_autohide(d);
    dnd_tick(d);
    desk_power_policy_tick(d);
    if (desk_recovery_tick && !d->sh.gate) desk_recovery_tick(d);
    if (!d->sh.network_poll_ns || (d->now_ns >= d->sh.network_poll_ns &&
        d->now_ns - d->sh.network_poll_ns >= 2000000000ull)) {
        d->sh.network_poll_ns = d->now_ns ? d->now_ns : 1;
        d->sh.want_reload |= 16|32;
    }
    program_balloon_tick(d);
    struct sh_notification *n=&d->sh.notification;
    if (n->active) {
        uint64_t age=d->now_ns>=n->started ? d->now_ns-n->started : 0;
        int slide=age>=160000000ull ? 0 : 20-(int)(age/8000000ull);
        if (d->now_ns>=n->expires) { program_balloon_dismiss(d,0x404); damage(d,notification_outer(d)); n->active=0; damage(d,notification_icon_box(d)); }
        else if (slide!=n->slide) {
            damage(d,notification_outer(d)); n->slide=slide; damage(d,notification_outer(d));
        }
    }
    int target=hover_target(d);
    if (target!=d->sh.hover_target) {
        if (d->sh.hover_target>=16 && d->sh.hover_target<16+DESK_CLIENTS) damage(d,desk_task_button(d,d->sh.hover_target-16));
        if (target>=16 && target<16+DESK_CLIENTS) damage(d,desk_task_button(d,target-16));
        tooltip_hide(d); d->sh.hover_target=target;
        d->sh.hover_since=target ? (d->now_ns ? d->now_ns : 1) : 0;
    }
    if (d->sh.tooltip && d->sh.tooltip!=target) tooltip_hide(d);
    for(int level=0;level<d->sh.levels;level++) {
        struct sh_menu *m=&d->sh.menus[level];
        if(m->opened && d->now_ns>=m->opened) {
            unsigned step=(unsigned)((d->now_ns-m->opened)/20000000ull+1);
            if(step>6) step=6;
            if(step!=m->animation_step) { damage(d,outer(m->box)); m->animation_step=step; }
            if(step==6) m->opened=0;
        }
    }
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
    int num = d->shell ? cfg_mouse_numerator((d->sh.cfg.mouse_spi_speed+1)/2) : 8;
    if (num == 8) return;
    int32_t ax = *dx * num + d->sh.motion_rx, ay = *dy * num + d->sh.motion_ry;
    *dx = ax / 8; *dy = ay / 8;
    d->sh.motion_rx = ax - *dx * 8; d->sh.motion_ry = ay - *dy * 8;
}
void desk_shell_clock_damage(struct desktop *d, uint64_t previous)
{
    uint64_t now = d->realtime_second + (uint64_t)(int64_t)d->local_offset;   /* G1: local edges */
    if (now/60!=previous/60) {
        /* Date text changes only at midnight; its right-aligned previous
         * width can differ, so clear the bounded date region at that edge. */
        if (now/86400!=previous/86400)
            damage(d,(struct desk_box){(int)d->width-300,0,300,DESK_BAR});
        if (d->sh.tooltip==1 && now/86400!=previous/86400) {
            if(d->sh.tooltip_painted)damage(d,d->sh.tooltip_last);
            damage(d,tooltip_box(d));
        }
    }
    if (d->sh.cfg.clock_seconds || now/60!=previous/60) {
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
    case DU_KEY_HOME: case DU_KEY_END:   /* SM-9: the first and the last enabled item */
        menu_highlight(d, m, menu_step(m, code == DU_KEY_HOME ? m->count - 1 : 0, code == DU_KEY_HOME ? 1 : -1));
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
        if (l > 0) { damage(d, outer(m->box)); d->sh.levels--; }
        else {
            /* Ctrl+Esc, Esc (Windows 98 General Keyboard-only Commands): the
             * Start menu closes and the focus stays on the taskbar. */
            int keyboard = d->sh.start_keyboard && m->kind == SH_MENU_START;
            desk_menu_close(d);
            if (keyboard) taskbar_set(d, DESK_CLIENTS);
        }
        return 1;
    default:
        if (!(d->sh.modifiers & (DU_CTRL | DU_ALT))) {
            unsigned ch = ciuki_keymap_char(d->sh.cfg.keyboard, (unsigned)code, keymap_mods(d));
            if (ch >= 'A' && ch <= 'Z') ch += 32;
            if (ch > 32 && ch < 127) return menu_letter(d, l, ch);
        }
        return 1;
    }
}
/* SM-9: a letter in an open menu. The item whose access key it is runs (a
 * submenu opens); when several items share it, the highlight moves to the next
 * one; a menu without access keys jumps to the next entry that starts with it. */
static int menu_letter(struct desktop *d, int level, unsigned ch)
{
    struct sh_menu *m = &d->sh.menus[level];
    int found[SH_ITEMS], n = 0;
    for (int k = 0; k < m->count; k++) {
        const struct sh_item *it = &m->items[k];
        if (it->flags & (SH_ITEM_SEPARATOR | SH_ITEM_DISABLED)) continue;
        if (it->mark && (unsigned)tolower((unsigned char)it->label[it->mark - 1]) == ch) found[n++] = k;
    }
    if (n == 1) {
        int k = found[0];
        if (m->items[k].action == SH_ACT_SUBMENU) { menu_highlight(d, m, k); return menu_key(d, DU_KEY_RIGHT); }
        activate(d, level, k);
        return 1;
    }
    if (!n) for (int k = 0; k < m->count; k++) {
        const struct sh_item *it = &m->items[k];
        if (!(it->flags & (SH_ITEM_SEPARATOR | SH_ITEM_DISABLED)) && (unsigned)tolower((unsigned char)it->label[0]) == ch) found[n++] = k;
    }
    if (!n) return 1;
    int next = found[0];
    for (int j = 0; j < n; j++) if (found[j] > m->hot) { next = found[j]; break; }
    menu_highlight(d, m, next);
    return 1;
}
int desk_shell_wheel(struct desktop *d, int detents)
{
    if (d->focus>=0 && d->clients[d->focus].internal==DESK_INTERNAL_RUN && d->sh.run) {
        if (d->sh.run->browser) { struct ciuki_input_event e={.type=CIUKI_INPUT_WHEEL,.value=detents}; run_browser_input(d,&e); }
        else { run_select(d,d->sh.run->selected-detents); damage(d,desk_outer(&d->clients[d->focus])); }
        return 1;
    }
    if (d->sh.levels) {
        for (int l=d->sh.levels-1;l>=0;l--) if (desk_contains(d->sh.menus[l].box,d->mouse_x,d->mouse_y)) {
            menu_scroll(d,&d->sh.menus[l],-detents*SH_SUB_ROW*3);return 1;
        }
    }
    if (!d->shell || d->focus < 0 || d->clients[d->focus].internal != DESK_INTERNAL_ABOUT || d->sh.about_tab != 2) return 0;
    unsigned lines=(unsigned)d->sh.cfg.wheel_lines;
    if(lines==101) {
        struct desk_box view,licences;int rows;
        credits_geometry(about_page_box(desk_content(&d->clients[d->focus])),&view,&licences,&rows);lines=(unsigned)rows;
    }
    int64_t next=(int64_t)d->sh.about_scroll-(int64_t)detents*lines;
    about_scroll_to(d,d->focus,next<0 ? 0 : next>2147483647 ? 2147483647 : (int)next);
    return 1;
}
static unsigned keymap_mods(const struct desktop *d)
{
    return (d->sh.modifiers & DU_SHIFT ? CIUKI_KEYMAP_SHIFT : 0) | (d->sh.alt_code == 0x138 ? CIUKI_KEYMAP_ALTGR : 0);
}
static unsigned shell_text(struct desktop *d, unsigned code, char text[9])
{
    unsigned mods=keymap_mods(d)|(d->text_caps ? CIUKI_KEYMAP_CAPS : 0), chars[2];
    unsigned ch=ciuki_input_char(d->sh.cfg.keyboard,code,mods),n=0;
    unsigned count=ciuki_input_compose(&d->text_dead,ch,d->sh.cfg.keyboard==CFG_KEYS_IT && code==0x0d && (mods&CIUKI_KEYMAP_SHIFT),chars);
    for(unsigned k=0;k<count;k++)if(chars[k]>=32)n+=ciuki_input_utf8(chars[k],text+n);
    text[n]=0;return n;
}
static int internal_key(struct desktop *d, int index, int code, int value)
{
    struct desk_client *c = &d->clients[index];
    if (c->internal==DESK_INTERNAL_SHORTCUT) return shortcut_key(d,index,code,value);
    if (c->internal==DESK_INTERNAL_TRANSFER) { desk_folder_dialog_key(d,code,value); return 1; }
    if (c->internal==DESK_INTERNAL_RUN && d->sh.run && d->sh.run->browser && value==0) {
        struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=code,.value=value}; run_browser_input(d,&e); return 1;
    }
    if (value == 0) return 1;
    if (c->internal==DESK_INTERNAL_SHORTCUT_WIZARD) { desk_folder_shortcut_key(d,code,ciuki_keymap_char(d->sh.cfg.keyboard,(unsigned)code,keymap_mods(d))); return 1; }
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
        else if (code==DU_KEY_ENTER && (d->sh.modifiers&DU_ALT)) { int link=programs_entry(d,d->sh.programs_selected); if(link>=0)desk_shortcut_properties(d,(unsigned)link,0); }
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
    case DESK_INTERNAL_RUN: {
        struct desk_run_ui *r=d->sh.run; if (!r) return 1;
        if (r->browser) {
            struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=code,.value=value}; run_browser_input(d,&e); return 1;
        }
        d->shell_caret_hidden=0; d->shell_caret_next=0;
        if (code==DU_KEY_ESC) { if (r->dropdown) { r->dropdown=0; run_resize(d); } else desk_request_close(d,index); }
        else if (code==DU_KEY_TAB) r->focus=(r->focus+((d->sh.modifiers&DU_SHIFT)?3:1))%4;
        else if ((d->sh.modifiers&DU_ALT) && code==0x30) run_browse(d); /* Alt+B */
        else if (code==DU_KEY_F4) { r->dropdown=!r->dropdown; run_resize(d); }
        else if (code==DU_KEY_ENTER || (code==DU_KEY_SPACE && r->focus)) {
            if (r->dropdown) { r->dropdown=0; run_resize(d); }
            else if (r->focus==2) desk_request_close(d,index);
            else if (r->focus==3) run_browse(d);
            else run_commit(d,index);
        } else if (code==DU_KEY_UP || code==DU_KEY_DOWN) {
            run_select(d,r->selected<0 ? 0 : r->selected+(code==DU_KEY_UP?-1:1));
            if (d->sh.modifiers&DU_ALT) { r->dropdown=1; run_resize(d); }
        } else if (!r->focus && !r->busy) {
            if ((d->sh.modifiers&DU_CTRL) && code==0x1e) r->select_all=1;
            else if (code==DU_KEY_HOME || code==DU_KEY_END || code==DU_KEY_LEFT || code==DU_KEY_RIGHT) {
                if (code==DU_KEY_HOME || (r->select_all && code==DU_KEY_LEFT)) r->cursor=0;
                else if (code==DU_KEY_END || r->select_all) r->cursor=d->sh.run_length;
                else if (code==DU_KEY_LEFT && r->cursor) r->cursor=(unsigned)du_utf8_prev(d->sh.run_text,r->cursor);
                else if (code==DU_KEY_RIGHT && r->cursor<d->sh.run_length) r->cursor=(unsigned)du_utf8_next(d->sh.run_text,r->cursor);
                r->select_all=0;
            } else if (code==DU_KEY_BACKSPACE || code==DU_KEY_DELETE) {
                if (r->select_all) { d->sh.run_text[0]=0; r->cursor=0; }
                else if (code==DU_KEY_BACKSPACE && r->cursor) {
                    unsigned prev=(unsigned)du_utf8_prev(d->sh.run_text,r->cursor);
                    memmove(d->sh.run_text+prev,d->sh.run_text+r->cursor,d->sh.run_length-r->cursor+1); r->cursor=prev;
                } else if (code==DU_KEY_DELETE && r->cursor<d->sh.run_length) {
                    size_t next=du_utf8_next(d->sh.run_text,r->cursor);
                    memmove(d->sh.run_text+r->cursor,d->sh.run_text+next,d->sh.run_length-next+1);
                }
                d->sh.run_length=(unsigned)strlen(d->sh.run_text); r->select_all=0;
            } else if (!(d->sh.modifiers&DU_CTRL)) {
                char text[9]; unsigned n=shell_text(d,(unsigned)code,text);
                if (n && (r->select_all ? 0 : d->sh.run_length)+n<sizeof(d->sh.run_text)) {
                    if (r->select_all) { d->sh.run_length=0; r->cursor=0; d->sh.run_text[0]=0; }
                    memmove(d->sh.run_text+r->cursor+n,d->sh.run_text+r->cursor,d->sh.run_length-r->cursor+1);
                    memcpy(d->sh.run_text+r->cursor,text,n); d->sh.run_length+=n; r->cursor+=n; r->select_all=0;
                }
            }
        }
        damage(d,desk_outer(c)); return 1;
    }
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
    case DESK_INTERNAL_END_TASK:
        /* Cancel / No is the default button: Enter keeps the program. */
        if (code == DU_KEY_ESC || code == DU_KEY_ENTER || code == 0x2e || code == 0x31) desk_end_task_choice(d, 0);
        else if ((d->end_task.kind == DESK_HANG_DOS && code == 0x15) || (d->end_task.kind == DESK_HANG_NOT_RESPONDING && code == 0x12))
            desk_end_task_choice(d, 1);   /* Y (Yes) / E (End Task) */
        return 1;
    case DESK_INTERNAL_SHUTDOWN:
        if (code == DU_KEY_ESC) desk_request_close(d, index);
        else if (code == DU_KEY_ENTER && d->sh.shutdown_choice == 1) { d->sh.want_power = 1; desk_request_close(d, index); }
        else if (code == DU_KEY_ENTER || code == 0x1f) { d->sh.want_power = 2; desk_request_close(d, index); }
        else if (code == 0x13) { d->sh.want_power = 1; desk_request_close(d, index); }
        else if (code == 0x30 && d->sh.power_can_standby) { d->sh.want_standby = 1; desk_request_close(d, index); }   /* B */
        return 1;
    default:
        if (d->sh.confirm) {
            if (code==DU_KEY_ENTER || code==DU_KEY_SPACE || code==0x15) confirm_answer(d,index,1);   /* Enter, Space, Y */
            else if (code==DU_KEY_ESC || code==0x31) confirm_answer(d,index,0);                      /* Esc, N */
            return 1;
        }
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
    taskbar_blur(d, 0);
    if (d->sh.levels && d->sh.menus[0].kind == SH_MENU_START) desk_menu_close(d);
    else { desk_menu_close(d); desk_menu_open(d, SH_MENU_START, 0, 0); d->sh.menus[0].hot = 0; d->sh.start_keyboard = 1; }
}
/* ---- KB-13: the keyboard focus of the taskbar ---------------------------
 * Windows 98 (General Keyboard-only Commands): Ctrl+Esc then Esc puts the
 * focus on the Start button, Tab and the arrows move over the task buttons,
 * Enter or Space presses the one in focus, Shift+F10 or the Menu key opens
 * its menu, Esc gives the focus back; Win+Tab moves over the task buttons
 * from the active window. The taskbar being active, no window is. */
static void taskbar_damage(struct desktop *d, int pick)
{
    if (pick == DESK_CLIENTS) damage(d, desk_start_bounds(d));
    else if (pick >= 0 && pick < DESK_CLIENTS) damage(d, desk_task_button(d, pick));
}
static void taskbar_set(struct desktop *d, int pick)
{
    if (d->sh.bar_hidden) { d->sh.bar_hidden=0;damage_all(d); }
    d->sh.bar_raised=1;
    if (d->sh.taskbar_key) taskbar_damage(d, d->sh.task_pick);
    else {
        d->sh.task_return = d->focus >= 0 ? d->clients[d->focus].window_id : 0;
        d->sh.taskbar_key = 1;
        if (d->focus >= 0) desk_focus(d, -1);
    }
    d->sh.task_pick = pick;
    taskbar_damage(d, pick);
}
static void taskbar_blur(struct desktop *d, int refocus)
{
    if (!d->sh.taskbar_key) return;
    taskbar_damage(d, d->sh.task_pick);
    d->sh.taskbar_key = 0;
    uint32_t id = d->sh.task_return;
    d->sh.task_return = 0;
    if (refocus && id) for (int i = 0; i < DESK_CLIENTS; i++)
        if (d->clients[i].window && d->clients[i].window_id == id && !d->clients[i].minimized) { desk_focus(d, i); break; }
}
void desk_shell_taskbar_leave(struct desktop *d) { taskbar_blur(d, 0); }
/* The next task button in slot order (the Start button first when asked). */
static int task_step(const struct desktop *d, int from, int dir, int with_start)
{
    int list[DESK_CLIENTS + 1], n = 0, at = -1;
    if (with_start) list[n++] = DESK_CLIENTS;
    for (int i = 0; i < DESK_CLIENTS; i++) if (desk_task_visible(&d->clients[i])) list[n++] = i;
    for (int k = 0; k < n; k++) if (list[k] == from) at = k;
    if (!n) return -1;
    if (at < 0) return dir > 0 ? list[0] : list[n - 1];
    return list[(at + dir + n) % n];
}
static int taskbar_keys(struct desktop *d, int code)
{
    int shift = (d->sh.modifiers & DU_SHIFT) != 0, pick = d->sh.task_pick;
    if (pick < DESK_CLIENTS && !desk_task_visible(&d->clients[pick])) pick = task_step(d, pick, 1, 1);
    switch (code) {
    case DU_KEY_TAB: case DU_KEY_RIGHT: case DU_KEY_LEFT: {
        int dir = code == DU_KEY_LEFT || (code == DU_KEY_TAB && shift) ? -1 : 1;
        /* "CTRL+ESC, ESC ... use TAB to go to desktop": Tab past either end leaves the taskbar. */
        if (code == DU_KEY_TAB && ((dir > 0 && pick == task_step(d, DESK_CLIENTS, -1, 1)) || (dir < 0 && pick == DESK_CLIENTS))) {
            taskbar_blur(d, 0); return 1;
        }
        int next = task_step(d, pick, dir, 1);
        if (next >= 0) taskbar_set(d, next);
        return 1;
    }
    case DU_KEY_HOME: taskbar_set(d, DESK_CLIENTS); return 1;
    case DU_KEY_END: { int last = task_step(d, DESK_CLIENTS, -1, 1); if (last >= 0) taskbar_set(d, last); return 1; }
    case DU_KEY_ENTER: case DU_KEY_SPACE:
        taskbar_blur(d, 0);
        if (pick == DESK_CLIENTS) { desk_menu_close(d); desk_menu_open(d, SH_MENU_START, 0, 0); d->sh.menus[0].hot = 0; d->sh.start_keyboard = 1; }
        else if (pick >= 0) {
            /* The click on a task button: restore a minimised window, minimise the active one. */
            if (d->clients[pick].minimized || d->focus == pick) desk_minimize(d, pick);
            else desk_focus(d, pick);
        }
        return 1;
    case DU_KEY_ESC: taskbar_blur(d, 1); return 1;
    case DU_KEY_MENU: case DU_KEY_F10:
        if (code == DU_KEY_F10 && !shift) return 0;
        if (pick >= 0 && pick < DESK_CLIENTS) {
            struct desk_box b = desk_task_button(d, pick);
            d->sh.menu_owner = pick; desk_menu_open(d, SH_MENU_TASK, b.x, desk_taskbar(d).y);
        } else { d->sh.menu_owner = -1; desk_menu_open(d, SH_MENU_TASKBAR, 0, desk_taskbar(d).y); }
        d->sh.menus[0].hot = menu_step(&d->sh.menus[0], d->sh.menus[0].count - 1, 1);
        return 1;
    default: return 0;
    }
}
/* Keys a shortcut consumed: their repeats and releases never reach a program. */
static void swallow_press(struct desktop *d, int code)
{
    for (unsigned k = 0; k < sizeof(d->sh.chord) / sizeof(*d->sh.chord); k++)
        if (!d->sh.chord[k]) { d->sh.chord[k] = (uint16_t)(code + 1); return; }
}
static int swallowed(struct desktop *d, int code, int release)
{
    for (unsigned k = 0; k < sizeof(d->sh.chord) / sizeof(*d->sh.chord); k++)
        if (d->sh.chord[k] == code + 1) { if (release) d->sh.chord[k] = 0; return 1; }
    return 0;
}
void desk_shell_find(struct desktop *d, int computer)
{
    desk_menu_close(d);
    if (computer) {
        desk_shell_message(d, "Find: Computer",
            "Finding a computer on the network is not available in this version of CiukiOS yet.", 0);
    } else launch_named(d, "files", "find:");
}
/* Win+key shortcuts of the Microsoft Natural Keyboard (Windows 98 Resource Kit,
 * Appendix G): D, M, Shift+M, E, F, Ctrl+F, R, Tab, Break and F1. */
static int logo_chord(struct desktop *d, int code)
{
    int ctrl = (d->sh.modifiers & DU_CTRL) != 0, shift = (d->sh.modifiers & DU_SHIFT) != 0;
    switch (code) {
    case 0x20: case 0x32: case 0x12: case 0x21: case 0x13: case DU_KEY_TAB: case 0x200: case DU_KEY_F1: break;
    default: return 0;
    }
    if (d->sh.switcher) switcher_finish(d, 0);
    if (d->sh.levels) desk_menu_close(d);
    switch (code) {
    case 0x20: (void)desk_show_desktop(d); break;                                  /* D */
    case 0x32: if (shift) (void)desk_arrange_undo(d); else (void)desk_minimize_all(d); break;   /* M */
    case 0x12: launch_named(d, "files", "Computer"); break;                       /* E: Computer, the Explorer of Windows 98 */
    case 0x21: desk_shell_find(d, ctrl); break;                                   /* F, Ctrl+F */
    case 0x13: desk_internal_open(d, DESK_INTERNAL_RUN); break;                   /* R */
    case DU_KEY_TAB: {                                                            /* Tab: cycle the taskbar buttons */
        int from = d->sh.taskbar_key ? d->sh.task_pick : d->focus;
        int next = task_step(d, from, shift ? -1 : 1, 0);
        if (next >= 0) taskbar_set(d, next);
        break;
    }
    case 0x200:                                                                   /* Break: System Properties */
        if (program_index(d, "control") >= 0) launch_named(d, "control", "--panel=system");
        else desk_shell_message(d, "System Properties", "The Control Panel is not installed.", 1);
        break;
    default:                                                                      /* F1 */
        if (program_index(d, "help") >= 0) launch_named(d, "help", NULL);
        else { struct sh_item keys = {.action = SH_ACT_KEYS}; act(d, &keys); }
        break;
    }
    return 1;
}
/* KB-8: Shift+F10 and the Menu key on the desktop open the context menu of
 * the selection, or the desktop's own menu when nothing is selected. */
static void desktop_menu_key(struct desktop *d)
{
    int s = d->sh.icon_selected;
    if (d->sh.cfg.desktop_icons && s >= 0 && s < d->sh.icon_count) {
        if (!desk_shell_icon_marked(d, s)) { mark(d, s, 1); d->sh.anchor = s; }
        struct desk_box b = d->sh.icons[s].box;
        d->sh.menu_owner = s;
        desk_menu_open(d, SH_MENU_ICON, b.x + b.w / 2, b.y + b.h / 2);
    } else desk_menu_open(d, SH_MENU_DESKTOP, d->mouse_x, d->mouse_y);
    d->sh.menus[0].hot = menu_step(&d->sh.menus[0], d->sh.menus[0].count - 1, 1);
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
    if (code==CIUKI_KEY_PRINTSCREEN || code==0x54) {
        extern int desk_clip_key(struct desktop *, int, int, int) __attribute__((weak));
        if (desk_clip_key && desk_clip_key(d,code,value,!!(d->sh.modifiers&DU_ALT))<0)
            desk_shell_message(d,"Clipboard","The screen could not be copied to the clipboard.",1);
        return 1;
    }

    if (!d->shell) return 0;
    if (value && code==DU_KEY_ESC && d->sh.bar_drag) {
        d->sh.cfg.taskbar_edge=d->sh.bar_origin_edge;d->sh.cfg.taskbar_size=d->sh.bar_origin_size;
        d->sh.bar_drag=d->sh.bar_resize=0;desk_shell_geometry(d);return 1;
    }
    if (value && code==DU_KEY_ESC && d->sh.custom_drag>=0) {
        d->sh.custom_drag=-1;d->sh.custom_dragging=0;return 1;
    }
    if (d->sh.address_focus && address_key(d,code,value)) return 1;
    if (value && d->sh.tooltip) { tooltip_hide(d); d->sh.hover_since=0; }
    int alt = code == 0x38 || code == 0x138, ctrl = code == 0x1d || code == 0x11d, shift = code == 0x2a || code == 0x36;
    if (d->sh.dnd.active && !d->sh.dnd.menu) {
        /* QueryContinueDrag: Esc cancels; modifier changes update the effect. */
        if (code == DU_KEY_ESC) { if (value) desk_shell_drag_cancel(d); return 1; }
        if (ctrl || shift) {
            unsigned bit = ctrl ? DU_CTRL : DU_SHIFT;
            if (value) d->sh.modifiers |= bit; else d->sh.modifiers &= ~bit;
            dnd_key(d); return 0;
        }
        if (alt) {
            if (value) d->sh.modifiers |= DU_ALT; else d->sh.modifiers &= ~DU_ALT;
            d->sh.alt_used = 1; d->sh.alt_code = value ? code : 0; dnd_key(d); return 1;
        }
        return 1;
    }
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
    if (shortcut_hotkey(d,code,value)) return 1;
    if (logo) {
        /* Either Windows key; the Start menu opens when the last one is
         * released without another key between (a chord is not a tap). */
        unsigned bit = code == 0x15b ? 1u : 2u;
        if (value == 1) { if (!d->sh.logo_keys) d->sh.logo_down = 1; d->sh.logo_keys |= bit; }
        else if (value == 0) {
            d->sh.logo_keys &= ~bit;
            if (!d->sh.logo_keys && d->sh.logo_down) { d->sh.logo_down = 0; toggle_start(d); }
        }
        return 1;
    }
    if (!value) { if (swallowed(d, code, 1)) return 1; }
    else if (value == 2 && swallowed(d, code, 0)) return 1;
    if (value) d->sh.logo_down = 0;
    if (value == 1 && d->sh.logo_keys && logo_chord(d, code)) { swallow_press(d, code); return 1; }
    if (value==1 && code==0x153 && (d->sh.modifiers&(DU_CTRL|DU_ALT))==(DU_CTRL|DU_ALT)) {
        desk_v3_revoke(d); desk_v3_pointer_release(d);
        /* Windows 98 (RK-apps "Closing Failed Applications"): one Close Program
         * list; pressed again it restarts the computer. G3 offers the restart
         * through the Shut Down dialog (Restart the default button), so every
         * program is still asked and nothing unsaved is lost. */
        int tasks=-1;
        for (int i=0;i<DESK_CLIENTS;i++) if (d->clients[i].window && d->clients[i].taskman) tasks=i;
        if (tasks>=0 && d->focus==tasks && !d->session.state) {
            if (desk_internal_open(d,DESK_INTERNAL_SHUTDOWN)>=0) { d->sh.shutdown_choice=1; damage_all(d); }
        } else if (tasks>=0) { if (d->clients[tasks].minimized) desk_minimize(d,tasks); else desk_focus(d,tasks); }
        else launch_named(d,"tasks",NULL);
        d->sh.alt_used=1; return 1;
    }
    if (d->sh.modifiers & DU_ALT) {
        if (d->focus>=0 && d->clients[d->focus].internal==DESK_INTERNAL_RUN && d->sh.run && !d->sh.run->browser &&
            (code==0x30 || code==DU_KEY_UP || code==DU_KEY_DOWN)) {
            d->sh.alt_used=1; return internal_key(d,d->focus,code,value);
        }
        if (code==DU_KEY_ENTER && d->focus<0 && d->sh.icon_selected>=0 && d->sh.icon_selected<d->sh.icon_count) {
            if (value==1) act(d,&(struct sh_item){.action=SH_ACT_ICON_PROPERTIES,.arg=d->sh.icon_selected});
            d->sh.alt_used=1; return 1;
        }
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
        if (code == 0x1f && d->focus < 0 && d->sh.icon_selected < 0 && !desk_task_count(d)) {
            /* ALT+S, with no window open and nothing selected on the desktop: the Start menu. */
            if (value == 1) toggle_start(d);
            d->sh.alt_used = 1; return 1;
        }
        if (code == 0x32 && d->focus < 0) {
            /* KB-13: Alt+M with the focus on the desktop or the taskbar minimises all windows. */
            if (value == 1) { if (d->sh.levels) desk_menu_close(d); (void)desk_minimize_all(d); }
            d->sh.alt_used = 1; return 1;
        }
        if (code == 0x40) {
            /* KB-9: Alt+F6 brings the next window of the same program to the front. */
            if (value == 1 && d->focus >= 0) {
                const struct desk_client *f = &d->clients[d->focus];
                int back = -1;
                for (int i = 0; i < DESK_CLIENTS; i++) {
                    const struct desk_client *c = &d->clients[i];
                    if (i == d->focus || !c->window || c->minimized || c->owner_id || c->internal || desk_modal_target(d, i) != i) continue;
                    if (!((f->pid && c->pid == f->pid) || (f->application[0] && !strcmp(f->application, c->application)))) continue;
                    if (back < 0 || c->order < d->clients[back].order) back = i;
                }
                if (back >= 0) desk_focus(d, back);
            }
            d->sh.alt_used = 1; return 1;
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
        if (!value && d->sh.alt_used && (code == DU_KEY_TAB || code == DU_KEY_F4 || code==DU_KEY_SPACE || code==DU_KEY_ESC || code==0x40 || code==0x32 || code==0x1f)) return 1;
    }
    if (!value && (code == DU_KEY_TAB || code == DU_KEY_F4 || code==DU_KEY_SPACE || code==DU_KEY_ESC || code==0x40 || code==0x32 || code==0x1f) && d->sh.alt_used) return 1;
    if ((d->sh.modifiers & DU_CTRL) && code == DU_KEY_ESC) { if (value == 1) toggle_start(d); return 1; }
    /* G15: F1 with no window focused (the desktop) opens its Help topic; a focused program gets the key. */
    if (code == DU_KEY_F1 && !(d->sh.modifiers & (DU_CTRL | DU_ALT | DU_SHIFT)) && d->focus < 0 && !d->sh.levels && !d->session.state) {
        if (value == 1) launch_named(d, "help", "topic:desktop");
        return 1;
    }
    if (d->focus<0 && d->sh.icon_editing>=0 && d->sh.icon_editing<d->sh.icon_count) {
        if (!value) return 1;
        size_t n=strlen(d->sh.icon_edit); unsigned pos=d->sh.icon_edit_cursor; int edited=d->sh.icon_editing;
        if (code==DU_KEY_ESC) d->sh.icon_editing=-1;
        else if (code==DU_KEY_ENTER) {
            const char *name=icon_position_name(d,edited);
            if (cfg_icon_label_set(&d->sh.cfg,name,d->sh.icon_edit)) {
                snprintf(d->sh.icons[edited].label,sizeof(d->sh.icons[edited].label),"%s",d->sh.icon_edit); d->sh.want_save=1;
            } else desk_shell_message(d,"Desktop","Enter a label of 1 to 63 characters.",1);
            d->sh.icon_editing=-1;
        } else if (code==DU_KEY_HOME) d->sh.icon_edit_cursor=0;
        else if (code==DU_KEY_END) d->sh.icon_edit_cursor=(unsigned)n;
        else if (code==DU_KEY_LEFT && pos) d->sh.icon_edit_cursor--;
        else if (code==DU_KEY_RIGHT && pos<n) d->sh.icon_edit_cursor++;
        else if (code==DU_KEY_BACKSPACE && pos) { memmove(d->sh.icon_edit+pos-1,d->sh.icon_edit+pos,n-pos+1); d->sh.icon_edit_cursor--; }
        else if (code==DU_KEY_DELETE && pos<n) memmove(d->sh.icon_edit+pos,d->sh.icon_edit+pos+1,n-pos);
        else if (!(d->sh.modifiers&DU_CTRL)) {
            unsigned ch=ciuki_keymap_char(d->sh.cfg.keyboard,(unsigned)code,keymap_mods(d));
            if (ch>=32 && ch<127 && n+1<sizeof(d->sh.icon_edit)) { memmove(d->sh.icon_edit+pos+1,d->sh.icon_edit+pos,n-pos+1); d->sh.icon_edit[pos]=(char)ch; d->sh.icon_edit_cursor++; }
        }
        d->sh.background_valid=0; damage(d,d->sh.icons[edited].box); return 1;
    }
    if (d->focus<0 && d->sh.folder && d->sh.folder->editing>=0) {
        struct desk_folder *f=d->sh.folder;
        if (!value) return 1;
        d->shell_caret_hidden=0;d->shell_caret_next=0;
        size_t n=strlen(f->edit);
        if (code==DU_KEY_ESC) f->editing=-1;
        else if (code==DU_KEY_ENTER) { f->command=DF_RENAME; f->index=f->editing; }
        else if (code==0x1e && (d->sh.modifiers&DU_CTRL)) f->select_all=1;
        else if (code==DU_KEY_LEFT || code==DU_KEY_RIGHT || code==DU_KEY_HOME || code==DU_KEY_END) {
            if (code==DU_KEY_HOME) f->cursor=0;
            else if (code==DU_KEY_END) f->cursor=(unsigned)n;
            else if (code==DU_KEY_LEFT && f->cursor) f->cursor=(unsigned)du_utf8_prev(f->edit,f->cursor);
            else if (code==DU_KEY_RIGHT && f->cursor<n) f->cursor=(unsigned)du_utf8_next(f->edit,f->cursor);
            f->select_all=0;
        } else if (code==DU_KEY_BACKSPACE || code==DU_KEY_DELETE) {
            if (f->select_all) { f->edit[0]=0; f->cursor=0; }
            else if (code==DU_KEY_BACKSPACE && f->cursor) { unsigned prev=(unsigned)du_utf8_prev(f->edit,f->cursor);memmove(f->edit+prev,f->edit+f->cursor,n-f->cursor+1); f->cursor=prev; }
            else if (code==DU_KEY_DELETE && f->cursor<n) {size_t next=du_utf8_next(f->edit,f->cursor);memmove(f->edit+f->cursor,f->edit+next,n-next+1);}
            f->select_all=0;
        } else if (!(d->sh.modifiers&DU_CTRL)) {
            char text[9];unsigned bytes=shell_text(d,(unsigned)code,text);
            if(bytes) {
                if(f->select_all){f->edit[0]=0;f->cursor=0;n=0;}
                if(n+bytes<sizeof(f->edit)) {memmove(f->edit+f->cursor+bytes,f->edit+f->cursor,n-f->cursor+1);memcpy(f->edit+f->cursor,text,bytes);f->cursor+=bytes;}
                f->select_all=0;
            }
        }
        damage_all(d); return 1;
    }
    if (d->sh.levels) return value ? menu_key(d, code) : 1;
    if (d->sh.taskbar_key && d->focus < 0 && value && !d->sh.dnd.active) {
        if (taskbar_keys(d, code)) { swallow_press(d, code); return 1; }
    }
    if (d->focus < 0 && value == 1 && !d->shell_test && !d->tasks_test && !d->app_test &&
        (!d->sh.folder || d->sh.folder->editing < 0)) {
        /* KB-8 and G7's entry: the desktop's context menu key and F3 (Find). */
        if (code == DU_KEY_MENU || (code == DU_KEY_F10 && (d->sh.modifiers & DU_SHIFT))) {
            desktop_menu_key(d); swallow_press(d, code); return 1;
        }
        if (code == 0x3d) { desk_shell_find(d, 0); swallow_press(d, code); return 1; }
    }
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
        if (value) { program_balloon_dismiss(d,0x403); damage(d,notification_outer(d)); d->sh.notification.active=0; damage(d,notification_icon_box(d)); }
        return 1;
    }
    if (d->focus >= 0 && d->clients[d->focus].internal) {
        /* AltGr characters reach the Run field; other Alt chords are ignored here. */
        if ((d->sh.modifiers & DU_ALT) && d->sh.alt_code != 0x138 &&
            !(d->clients[d->focus].internal==DESK_INTERNAL_RUN && d->sh.run && d->sh.run->browser)) return 1;
        return internal_key(d, d->focus, code, value);
    }
    if (d->focus<0 && value && code==DU_KEY_F5) { desktop_refresh(d); return 1; }
    if (d->focus < 0 && d->sh.cfg.desktop_icons && d->sh.icon_count && !d->shell_test && !d->tasks_test && !d->app_test) {
        if (!value) return 0;
        int s = d->sh.icon_selected; if (s>=d->sh.icon_count) s=-1;
        if (code==DU_KEY_HOME || code==DU_KEY_END) {
            if (d->sh.icon_count) select_only(d,code==DU_KEY_HOME ? 0 : d->sh.icon_count-1);
            return 1;
        }
        if (code==DU_KEY_UP || code==DU_KEY_DOWN || code==DU_KEY_LEFT || code==DU_KEY_RIGHT) {
            int next=s<0 ? 0 : s; int64_t best=INT64_MAX;
            if (s>=0) for (int i=0;i<d->sh.icon_count;i++) if (i!=s) {
                int dx=d->sh.icons[i].box.x-d->sh.icons[s].box.x, dy=d->sh.icons[i].box.y-d->sh.icons[s].box.y;
                if ((code==DU_KEY_UP && dy>=0) || (code==DU_KEY_DOWN && dy<=0) ||
                    (code==DU_KEY_LEFT && dx>=0) || (code==DU_KEY_RIGHT && dx<=0)) continue;
                int64_t distance=(int64_t)dx*dx+(int64_t)dy*dy;
                if (distance<best) { best=distance; next=i; }
            }
            /* Shift extends from the anchor, Ctrl moves only the focus. */
            unsigned mods=d->sh.modifiers;
            if (mods&DU_SHIFT) select_range(d,next,(mods&DU_CTRL)!=0);
            else if (mods&DU_CTRL) icon_focus(d,next);
            else select_only(d,next);
            return 1;
        }
        if (code==DU_KEY_SPACE && (d->sh.modifiers&DU_CTRL)) {
            if (s>=0) { mark(d,s,!desk_shell_icon_marked(d,s)); d->sh.anchor=s; }
            return 1;
        }
        if (code==0x1e && (d->sh.modifiers&(DU_CTRL|DU_ALT))==DU_CTRL) {
            for (int k=0;k<d->sh.icon_count;k++) mark(d,k,1);
            return 1;
        }
        if (!(d->sh.modifiers&(DU_CTRL|DU_ALT|DU_SHIFT)) && d->now_ns-d->sh.icon_typeahead_ns>1000000000ull)
            d->sh.icon_typeahead_length=0;
        if (!(d->sh.modifiers&(DU_CTRL|DU_ALT)) && code!=DU_KEY_BACKSPACE) {
            unsigned ch=ciuki_keymap_char(d->sh.cfg.keyboard,(unsigned)code,keymap_mods(d));
            if (ch>=32 && ch<127 && d->sh.icon_typeahead_length+1<sizeof(d->sh.icon_typeahead)) {
                d->sh.icon_typeahead[d->sh.icon_typeahead_length++]=(char)tolower((unsigned char)ch);
                d->sh.icon_typeahead[d->sh.icon_typeahead_length]=0; d->sh.icon_typeahead_ns=d->now_ns;
                for (int off=1;off<=d->sh.icon_count;off++) {
                    int k=(s+off+d->sh.icon_count)%d->sh.icon_count;
                    if (!strncasecmp(d->sh.icons[k].label,d->sh.icon_typeahead,d->sh.icon_typeahead_length)) { select_only(d,k); break; }
                }
                return 1;
            }
        }
        /* File commands act on the selection; the focus, or the first
         * selected file when the focus is a system icon. */
        int primary=icon_is_entry(d,s) && desk_shell_icon_marked(d,s) ? s : -1;
        for (int k=3;primary<0 && k<d->sh.icon_count;k++) if (icon_is_entry(d,k) && desk_shell_icon_marked(d,k)) primary=k;
        if (primary<0 && icon_is_entry(d,s)) primary=s;
        if (d->sh.folder) {
            if (code==DU_KEY_F2 && icon_is_entry(d, s)) { desk_folder_rename(d,s); return 1; }
            if (code==DU_KEY_F2 && ((s>=0 && s<3) || network_icon(d,s))) { desktop_icon_rename_begin(d,s); return 1; }
            if (code==DU_KEY_DELETE && primary>=0) {
                /* G2: Shift+Delete bypasses the Recycle Bin. */
                d->sh.folder->command=DF_DELETE; d->sh.folder->index=primary-3; d->sh.folder->multi=selection_sync(d)>1;
                d->sh.folder->command_flags=(d->sh.modifiers&(DU_CTRL|DU_ALT|DU_SHIFT))==DU_SHIFT ? RB_PERMANENT : 0; return 1;
            }

            if (d->sh.modifiers&DU_CTRL) {
                int command=code==0x2c ? DF_UNDO : code==0x2f ? DF_PASTE : code==0x2e ? DF_COPY : code==0x2d ? DF_CUT : 0;
                if (command && (command==DF_PASTE || command==DF_UNDO || primary>=0)) {
                    d->sh.folder->command=command; d->sh.folder->index=primary-3;
                    if (command!=DF_PASTE) d->sh.folder->multi=selection_sync(d)>1;
                    return 1;
                }
            }
        }
        if (code == DU_KEY_ENTER && s >= 0) { open_selection(d, s); return 1; }
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
    static const char *const extra[]={"system-search","go-jump","drive-removable-media","media-optical","user-trash-full","system-log-out","system-lock-screen","system-users","dialog-password","emblem-symbolic-link","network-workgroup"};
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
    pp_defaults(&d->sh.power_cfg);
    d->sh.power_disk_sent = CIUKI_POWER_UNKNOWN;
    d->sh.custom_drag=d->sh.custom_item=-1;
    d->sh.custom_refresh=1;
    d->sh.icon_selected = d->sh.click_icon = d->sh.icon_drag = -1;
    d->sh.icon_editing=-1;
    d->sh.anchor = -1; d->sh.dnd.drop_index = d->sh.dnd.target = d->sh.dnd.status_target = d->sh.dnd.hover_task = -1;
    d->sh.volume = 20;
}
void desk_shell_apply(struct desktop *d)
{
    bind(d);
    if (desk_theme_use(cfg_effective_theme(&d->sh.cfg))) desk_theme_use(DESK_THEME_DEFAULT);   /* G25: high contrast names the scheme */
    if(d->sh.cfg.appearance_set) desk_appearance_apply(&d->sh.cfg.appearance);
    desk_theme_changed(d); d->cursor_loaded=DESK_CURSOR_COUNT; d->cursor_dirty=1;
    if (!d->sh.cfg.desktop_icons) {
        if (d->sh.dnd.active) desk_shell_drag_cancel(d);
        d->sh.band=0; d->sh.icon_pending=0; d->sh.icon_editing=-1;
        if (d->sh.folder) d->sh.folder->editing=-1;
    }
    if (!d->sh.icon_count) icons_build(d);
    if(d->sh.icon_count>1) d->sh.icons[1].icon=d->sh.cfg.document_icon ? "folder" : "user-home";
    if(d->sh.icon_count>2) d->sh.icons[2].icon=d->sh.cfg.recycle_icon ? "edit-delete" : d->sh.folder && d->sh.folder->bin_full ? "user-trash-full" : "user-trash";
    icons_layout(d);
    d->sh.wall_valid = 0;
    if (d->sh.levels) desk_menu_close(d);
    d->shell_caret_next=0;d->shell_caret_hidden=0;
    d->input_volatile_mask=0;
    d->pointer_trail_count=0;
    d->input_change_flags=2;d->input_change_key=0;
    desk_preferences_changed(d);d->input_change_flags=0;
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
    int r = desk_wallpaper_render_pattern(d->sh.wall, d->width, d->height, DESK_BAR, DESK_BAR,
                                  d->sh.wallpaper_pending ? "none" : d->sh.cfg.wallpaper,
                                  d->sh.cfg.wallpaper_mode, desktop_colour(d), &d->sh.image, d->portrait,d->sh.cfg.wallpaper_pattern);
    d->sh.wall_valid = 1;
    d->sh.background_valid=0;
    d->sh.image_failed = r < 0;
    damage_all(d);
    return r;
}

void desk_links_refresh(struct desktop *d)
{
    /* Menu item arguments and dynamic folder IDs index this snapshot.
     * Keep it stable while displayed so an installer cannot reorder a
     * visible item's target underneath the next click. */
    if (d->sh.levels) return;
    if (!d->v3_links_list || d->now_ns<d->v3_links_next) return;
    d->v3_links_next=d->now_ns+2000000000ull;
    if (!d->v3_links) d->v3_links=calloc(DESK_LINKS,sizeof(*d->v3_links));
    if (!d->v3_links) return;
    int n=d->v3_links_list(d,d->v3_links,DESK_LINKS);
    if (n==-EAGAIN) return;
    d->v3_link_count=n>0 && n<=(int)DESK_LINKS ? (unsigned)n : 0;
    /* fx-15: the icons that leave or move are exposed, the new ones painted
     * (the list view's LVN_DELETEITEM/LVN_INSERTITEM repaint); the selection
     * a rebuild drops is repainted with them. */
    for (int k=0;k<d->sh.icon_count;k++) damage(d,d->sh.icons[k].box);
    struct sh_icon selected={0}; int had=d->sh.icon_selected>=0 && d->sh.icon_selected<d->sh.icon_count;
    if (had) selected=d->sh.icons[d->sh.icon_selected];
    /* Rebuild the native folder icons first, then append validated link icons. */
    icons_build(d);
    for (unsigned k=0;k<d->v3_link_count;k++) if (d->v3_links[k].desktop && desk_launch_valid(&d->v3_links[k].launch)) {
        int native_entry=0;
        if (d->sh.folder) for (unsigned i=0;i<d->sh.folder->model.count;i++) {
            char path[FM_PATH];
            if (!fm_entry_path(&d->sh.folder->model,i,path) && fm_same_path(path,d->v3_links[k].source)) { native_entry=1; break; }
        }
        if (native_entry) continue;
        char index[12]; snprintf(index,sizeof(index),"%u",k);
        icon_add(d,d->v3_links[k].launch.label,"",index,"application-x-executable",SH_ACT_WIN32_LINK,0);
    }
    icons_layout(d);
    /* An unrelated shortcut change keeps the selection (Windows 98's
     * desktop refresh does), matched by label, program and argument. */
    for (int k=0;had && k<d->sh.icon_count;k++) if (!strcmp(d->sh.icons[k].label,selected.label) &&
        !strcmp(d->sh.icons[k].program,selected.program) && !strcmp(d->sh.icons[k].argument,selected.argument)) { d->sh.icon_selected=k; break; }
    for (int k=0;k<d->sh.icon_count;k++) damage(d,d->sh.icons[k].box);
}
int desk_link_launch(struct desktop *d, unsigned index)
{
    if (index>=d->v3_link_count || (!d->sh.folder && !d->v3_link_read)) return -ENOENT;
    struct desk_launch_record *record=malloc(sizeof(*record)); if (!record) return -ENOMEM;
    /* G18: the desktop's cached snapshot is read without I/O when it installed one; G19: otherwise through the folder model. */
    int result=d->v3_link_read ? d->v3_link_read(d,d->v3_links[index].source,record) : desk_folder_link_read(d,d->v3_links[index].source,record);
    if (!result && !desk_launch_valid(record)) result=-EINVAL;
    if (!result) {
        /* G19 opens a target inside the VFS itself (program, folder, document, chained link); G18 hands any other
         * target (a Win32 or DOS program) to the asynchronous launcher so the desktop never waits for it. */
        char path[FM_PATH]; struct fm_meta meta; struct fm_vfs *fs=d->sh.folder ? &d->sh.folder->model.fs : NULL;
        int inside=fs && !desk_target_url(record->target) && !fm_posix_path(path,record->target) && !fs->stat_path(fs->ctx,path,&meta);
        result=!inside && d->v3_launch ? d->v3_launch(d,record) : desk_shell_open_record(d,record,0);
    }
    free(record); return result;
}

#include "logon.inc"
#include "taskbar_custom.inc"
