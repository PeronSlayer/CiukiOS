/* SPDX-License-Identifier: MIT
 * Control Panel pages (control.h). Settings I/O goes through cp_host; the
 * optional Fonts page reads its trusted store through libfont. */
#include "control.h"
#include "../desktop/saver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

#define CP_T_SAVER 701
#define CP_T_PREVIEW 700   /* timer id of the deferred wallpaper preview (outside the widget ids) */
enum {
    C_TABS = 1, C_CLOSE, C_STATUS,
    C_THEME_L, C_THEME, C_WALL_L, C_WALL, C_POS_L, C_POS0, C_POS1, C_POS2, C_POS3, C_POS4, C_COL_L,
    C_COL0, C_COL1, C_COL2, C_COL3, C_COL4, C_COL5, C_ICONS,
    C_SPEED_L, C_SPEED, C_SPEED_V, C_SLOW, C_FAST, C_DBL_L, C_DBL, C_DBL_V, C_MOUSE_NOTE,
    C_KEY_L, C_KEY, C_KEY_NOTE1, C_KEY_NOTE2,
    C_VOL_L, C_VOL, C_VOL_V, C_MUTE, C_SOUND_LIST, C_SOUND_TEST, C_SOUND_ENABLED, C_HARDWARE, C_AUDIO,
    C_DATE, C_H24, C_SECONDS, C_TIME_NOTE1, C_TIME_NOTE2,
    C_SYS0, C_SYS1, C_SYS2, C_SYS3, C_SYS4,
    C_TASKBAR_CLOCK=1960, C_QUICK_LAUNCH, C_TASKBAR_TOP, C_TASKBAR_AUTOHIDE, C_START_SMALL,
    C_BACK = 100, C_APPLETS, C_TREE, C_REFRESH, C_PROPERTIES, C_DRIVERS, C_LEASES,
    C_SYSTEM_TABS, C_CPU, C_UPTIME, C_SYSTEM_HARDWARE, C_FLAGS, C_BOOT_OPTIONS, C_SERIAL, C_BOOT_APPLY,
    C_DETAILS = 130, C_FIRMWARE = 131, D_FIRMWARE = 199, D_PROPERTIES = 200, D_INFO, C_SCHEME, C_UI_VOL, C_UI_VOL_V, C_SOUND_BROWSE, C_SOUND_NONE, C_SCHEME_SAVE, C_SCHEME_DELETE, D_SOUND_BROWSE, D_SCHEME_SAVE, C_MM_VOLUME, C_MM_RECORDER, C_MM_MUSIC, C_MM_CD, C_RGB_R=4000, C_RGB_G, C_RGB_B
};
enum { C_REPEAT_DELAY=1500, C_REPEAT_SPEED, C_BLINK, C_SWAP, C_TRAILS,
       C_WHEEL, C_POINTER, C_LAYOUT_SWITCH, C_DOUBLE_TEST };
static const char *const page_names[CP_PAGES] = {"display", "mouse", "keyboard", "sounds", "datetime", "system", "devices", "drivers", "network", "power", "adapter", "fonts", "autoplay", "programs", "joystick", "multimedia", "users", "passwords", "regional", "internet", "addhw", "access", "taskbar"};
static const char *const page_labels[CP_PAGES] = {"Display Properties", "Mouse", "Keyboard", "Sounds and Audio", "Date and Time", "System", "System", "System", "Network", "Power", "Display Properties", "Display Properties", "Storage and removable devices", "Add/Remove Programs", "Game controllers", "Multimedia", "User Accounts", "User Accounts", "Regional Settings", "Internet Properties", "Add New Hardware", "Accessibility Options", "Taskbar & Start Menu"};
static int wallpaper_path(const char *path)
{
    if (!path || path[0]!='/' || strstr(path,"/../") || strstr(path,"/./")) return 0;
    size_t n=strlen(path); if (n<2 || (n>=3 && !strcmp(path+n-3,"/..")) || (n>=2 && !strcmp(path+n-2,"/."))) return 0;
    const char *dot=strrchr(path,'.'); if (!dot) return 0;
    return !strcasecmp(dot,".png") || !strcasecmp(dot,".jpg") || !strcasecmp(dot,".jpeg") || !strcasecmp(dot,".bmp");
}
static void build(struct cp_app *a);
static void list_style(struct du_widget *,const char *);
static void refresh(struct cp_app *a);
static unsigned power_read(struct cp_app *a);
static void refresh(struct cp_app *a);
static void properties(struct cp_app *a, int resources);
static void details(struct cp_app *a);
static void firmware_show(struct cp_app *a, uint32_t index);
static const uint32_t swatches[6] = {0x375564u, 0x2f6f6bu, 0x3a3f6bu, 0x6b4f3au, 0x808080u, 0x101820u};
/* The Position buttons, in two rows of three: Center Tile Stretch / Fit Fill. */
#define CP_POSITIONS 5
static const int mode_order[CP_POSITIONS] = {CFG_WALL_CENTER, CFG_WALL_TILE, CFG_WALL_STRETCH, CFG_WALL_FIT, CFG_WALL_FILL};
static int power_meter_requested;   /* G11: "--panel=power-meter" opens the Power Meter tab */
int cp_page_index(const char *name)
{
    if (name && !strcmp(name, "storage")) return CP_AUTOPLAY;
    if (name && !strcmp(name, "power-meter")) { power_meter_requested = 1; return CP_POWER; }
    for (int i = 0; i < CP_PAGES; i++) if (name && !strcmp(name, page_names[i])) return i;
    return CP_HOME;
}
static struct du_widget *add(struct cp_app *a, enum du_kind k, int id, int x, int y, int w, int h, const char *text)
{ return du_add(a->ui, k, id, (struct du_rect){x, y, w, h}, text); }
/* Win98 trackbar for a value slider: 0..hi with a notched thumb; tick > 0 draws tick marks every tick units.
 * A scrollbar of total/page 1 covered 0..total-1, so callers pass hi = total - 1. PgUp/PgDn move big units. */
static struct du_widget *slider(struct cp_app *a, int id, int x, int y, int w, int hi, int tick)
{
    struct du_widget *s = add(a, DU_TRACKBAR, id, x, y, w, tick > 0 ? 30 : 26, "");
    int big = hi / 8; if (big < 1) big = 1;
    du_trackbar_set(s, 0, hi, 0, 1, big, tick);
    return s;
}
static void text(struct cp_app *a, int id, const char *s)
{ struct du_widget *w = du_get(a->ui, id); if (w && strcmp(w->text,s)) { du_set_text(w,s); du_invalidate(a->ui,w->box); } }
#include "datetime.inc"   /* G1: Date/Time Properties */
static void changed(struct cp_app *a);
static int load(struct cp_app *a, struct ciuki_settings *out);
static void list_style(struct du_widget *w, const char *title);
#include "access.inc"     /* G25: Accessibility Options */

#include "network.inc"
#include "internet.inc"
#include "game_properties.inc"
#include "joystick.inc"
#include "users.inc"
#include "accounts.inc"   /* sec-user-04: User Accounts over the account service */
static const char *class_icon(unsigned k);
static const char *product(struct cp_app *a, const struct ciuki_device_record *d, char *resolved, size_t size);
static void list_style(struct du_widget *w, const char *title);
#include "hardware.inc"   /* G30: device actions, Add New Hardware, Performance, Hardware Profiles */

/* ---- settings: merge the panel's keys into the file as it is now ---- */
static int load(struct cp_app *a, struct ciuki_settings *out)
{
    static char buffer[CFG_TEXT_MAX]; size_t n = 0;
    errno=0;
    int result=a->host.load ? a->host.load(a->host.ctx,buffer,sizeof(buffer),&n) : -1;
    int error=errno ? errno : EIO;
    if ((result && (error!=ENOENT || a->cfg_loaded)) || (!result && n>=sizeof(buffer))) {
        text(a,C_STATUS,"Settings could not be read. Your saved preferences have been kept.");
        errno=result ? error : EFBIG; return -1;
    }
    cfg_defaults(out);
    if (!result) cfg_parse(out,buffer,n);
    if (!result) a->cfg_loaded=1;
    return 0;
}
/* sec-user-04: a Standard user or Guest removes only programs installed in his
 * own profile; all-users entries stay visible, their Remove disabled with the
 * reason (contract 7.1; the kernel refuses the machine trees anyway). */
static int uninstall_locked(const struct cp_app *a, const struct desk_uninstall_entry *r)
{
    if (!a->machine_locked) return 0;
    const char *profile=getenv("USERPROFILE");
    size_t n=profile && !strncmp(profile,"/Windows/Profiles/",18) ? strlen(profile) : 0;
    return r->kind==DESK_PROGRAM_WIN32 || r->kind==DESK_PROGRAM_GAME || !n || strncmp(r->location,profile,n) ||
           (r->location[n] && r->location[n]!='/');
}
/* sec-user-04: one per-user lock preference (session_prefs.h) saved into the
 * caller's own desktop.cfg like the other pages; the desktop reloads it. */
static int secure_pref_save(struct cp_app *a, const char *key, int value)
{
    static struct ciuki_settings now; static char out[CFG_TEXT_MAX];
    if (load(a,&now) || desk_secure_store(now.extra,sizeof(now.extra),key,value)) return -1;
    size_t n=cfg_format(&now,out,sizeof(out));
    if (!n || !a->host.save || a->host.save(a->host.ctx,out,n)) return -1;
    a->cfg_loaded=1;
    if (!strcmp(key,DESK_SECURE_SAVER)) (void)desk_secure_store(a->cfg.extra,sizeof(a->cfg.extra),key,value);
    if (a->host.apply) (void)a->host.apply(a->host.ctx,DESK_SETTINGS_RELOAD,0,0);
    return 0;
}
static int sound_commit(struct cp_app *a, const struct sound_scheme *next)
{
    size_t n=sound_format(next,a->sound_text,sizeof(a->sound_text));
    if (!n || !a->host.save_sounds || a->host.save_sounds(a->host.ctx,a->sound_text,n)) {
        text(a,C_STATUS,"Sound settings could not be saved."); return -1;
    }
    a->sounds=*next;
    int r=a->host.apply ? a->host.apply(a->host.ctx,DESK_SETTINGS_SOUND_RELOAD,0,0) : -1;
    text(a,C_STATUS,r ? "Saved. Restart to apply sound settings." : "Sound settings saved and applied.");
    return 0;
}
static void changed(struct cp_app *a)
{
    static struct ciuki_settings now;
    if (load(a,&now)) return; /* never merge against a failed/partial read */
    if (a->page==CP_MOUSE) { now.mouse_speed=a->cfg.mouse_speed; now.mouse_spi_speed=a->cfg.mouse_spi_speed; now.double_click_ms=a->cfg.double_click_ms;
        now.mouse_swap=a->cfg.mouse_swap; now.mouse_trails=a->cfg.mouse_trails;
        now.wheel_lines=a->cfg.wheel_lines; now.pointer_scheme=a->cfg.pointer_scheme; }
    else if (a->page==CP_KEYBOARD) { now.keyboard=a->cfg.keyboard;
        now.repeat_delay=a->cfg.repeat_delay; now.repeat_speed=a->cfg.repeat_speed;
        now.cursor_blink_ms=a->cfg.cursor_blink_ms; now.layout_switch=a->cfg.layout_switch; }
    else if (a->page==CP_DISPLAY) {
        snprintf(now.wallpaper,sizeof(now.wallpaper),"%s",a->cfg.wallpaper);
        now.wallpaper_mode=a->cfg.wallpaper_mode; now.wallpaper_pattern=a->cfg.wallpaper_pattern; now.desktop_color=a->cfg.desktop_color;
    }
    else if (a->page==CP_POWER) now.monitor_seconds=a->cfg.monitor_seconds;
    else if (a->page==CP_DATETIME) { now.clock_24h=a->cfg.clock_24h; now.clock_seconds=a->cfg.clock_seconds; }
    else if (a->page==CP_ACCESS) ac_merge(a,&now);   /* G25: only the fields this panel changed */
    else if (a->page==CP_TASKBAR) {
        now.taskbar_clock=a->cfg.taskbar_clock;
        now.quick_launch=a->cfg.quick_launch;
        now.taskbar_top=a->cfg.taskbar_top;now.taskbar_autohide=a->cfg.taskbar_autohide;now.start_small=a->cfg.start_small;
        if (a->menu_cleared) now.recent_count=0;
    }
    static char out[CFG_TEXT_MAX];
    size_t n = cfg_format(&now, out, sizeof(out));
    if (!n || !a->host.save || a->host.save(a->host.ctx, out, n)) { text(a, C_STATUS, "The settings could not be saved."); return; }
    a->cfg_loaded=1;
    a->menu_cleared=0;
    int r = a->host.apply ? a->host.apply(a->host.ctx, DESK_SETTINGS_RELOAD, 0, 0) : -1;
    text(a, C_STATUS, a->host.save_async ? "Saving settings..." : r ? "Saved. The desktop applies it at its next start." : "Saved and applied.");
}
#include "regional.inc"
#include "power.inc"   /* G11: Power Management Properties */


/* Display previews use the existing durable settings/reload path. Keep the
 * entry snapshot until OK; rollback merges only this applet's owned keys. */
static void display_copy(struct ciuki_settings *to, const struct ciuki_settings *from)
{
    strcpy(to->desktop_theme_name,from->desktop_theme_name);
    to->pointer_scheme=from->pointer_scheme; to->document_icon=from->document_icon; to->recycle_icon=from->recycle_icon;
    memcpy(to->desktop_themes,from->desktop_themes,sizeof(to->desktop_themes));
    to->appearance=from->appearance; to->appearance_set=from->appearance_set;
    memcpy(to->scheme,from->scheme,sizeof(to->scheme));
    memcpy(to->schemes,from->schemes,sizeof(to->schemes));
    memcpy(to->theme, from->theme, sizeof(to->theme));
    memcpy(to->wallpaper, from->wallpaper, sizeof(to->wallpaper));
    to->wallpaper_mode = from->wallpaper_mode; to->wallpaper_pattern=from->wallpaper_pattern; to->desktop_color = from->desktop_color;
    to->saver_enabled=from->saver_enabled; to->saver_seconds=from->saver_seconds;
    to->saver_kind=from->saver_kind;
    to->desktop_icons = from->desktop_icons; to->display_mode = from->display_mode;
    to->icon_spacing_x=from->icon_spacing_x; to->icon_spacing_y=from->icon_spacing_y;
    to->icon_font=from->icon_font;
}
int cp_display_store(struct cp_app *a)
{
    static struct ciuki_settings current; static char out[CFG_TEXT_MAX];
    if (load(a,&current)) return -1;
    display_copy(&current, &a->cfg);
    if(a->display_sounds_dirty && a->host.save_sounds) {
        size_t sounds=sound_format(&a->sounds,a->sound_text,sizeof(a->sound_text));
        if(!sounds || a->host.save_sounds(a->host.ctx,a->sound_text,sounds)) {
            text(a,C_STATUS,"The theme sound scheme could not be saved."); return -1;
        }
    }
    size_t n = cfg_format(&current, out, sizeof(out));
    if (!n || !a->host.save || a->host.save(a->host.ctx, out, n)) {
        if(a->display_sounds_dirty && a->host.save_sounds) {
            size_t restore=sound_format(&a->display_original_sounds,a->sound_text,sizeof(a->sound_text));
            if(restore) (void)a->host.save_sounds(a->host.ctx,a->sound_text,restore);
            if(a->host.apply) a->host.apply(a->host.ctx,DESK_SETTINGS_SOUND_RELOAD,0,0);
        }
        text(a, C_STATUS, "Display settings could not be saved."); return -1;
    }
    if(a->display_sounds_dirty && a->host.apply) a->host.apply(a->host.ctx,DESK_SETTINGS_SOUND_RELOAD,0,0);
    du_theme_use(a->ui,a->cfg.theme);
    a->cfg_loaded=1;
    if(a->cfg.appearance_set) desk_appearance_apply(&a->cfg.appearance);
    a->display_preview = 1; a->display_dirty = 0;
    a->display_write = a->host.save_async;
    if (a->host.apply) a->host.apply(a->host.ctx, DESK_SETTINGS_RELOAD, 0, 0);
    text(a, C_STATUS, a->display_write ? "Saving display preview..." : "Preview applied. Cancel restores your previous settings.");
    return 0;
}
void cp_display_accept(struct cp_app *a)
{
    if (a->display_write || a->mode_state) return;
    a->display_session = a->display_preview = a->display_accept = 0;
    a->quit = 1;
}
int cp_display_leave(struct cp_app *a)
{
    if (!a->display_session) return 1;
    if (a->display_write || a->mode_state == CP_MODE_SAVE) {
        text(a, C_STATUS, "Waiting for display settings to finish saving..."); return 0;
    }
    if (a->mode_state) { cp_display_cancel(a); return 0; }
    if (a->display_preview) {
        a->sounds=a->display_original_sounds;
        display_copy(&a->cfg, &a->display_original);
        a->display_dirty=1;
        if (cp_display_store(a)) return 0;
        a->display_preview = 0;
        a->display_leaving = 1;
        if (a->display_write) return 0;
    }
    if (a->display_leaving && a->display_original_mode && a->display_ok) {
        uint16_t current = 0;
        for (unsigned i=0;i<a->display.mode_count;i++) if (a->display.modes[i].current) current=a->display.modes[i].id;
        if (current != a->display_original_mode) {
            a->previous_mode = a->display_original_mode;
            a->mode_state = CP_MODE_WAIT; cp_display_cancel(a);
            if (!a->display_leaving) return 0;
            du_timer_start(a->ui, CP_MODE_TIMER, 1000000000ull, 1);
        }
    }
    if (a->mode_state) return 0;
    a->sounds=a->display_original_sounds; a->display_sounds_dirty=0;
    display_copy(&a->cfg, &a->display_original);
    du_theme_use(a->ui, a->cfg.theme);
    if(a->cfg.appearance_set) desk_appearance_apply(&a->cfg.appearance);
    a->display_session = a->display_leaving = a->display_dirty = 0;
    return 1;
}
int cp_close(struct cp_app *a)
{
    if (a->regional_saving) { text(a,C_STATUS,"Waiting for regional settings to finish saving..."); return 0; }
    if (a->game_save) {
        game_note(a,"Waiting for game properties to finish saving...");return 0;
    }
    a->close_pending = 1; a->display_accept = a->navigation_pending = 0;
    if (!cp_display_leave(a)) return 0;
    a->close_pending = 0; a->quit = 1; return 1;
}
void cp_settings_saved(struct cp_app *a, int error)
{
    if (a->regional_saving) {
        a->regional_saving=0;
        if (!error && a->regional_accept) a->quit=1;
        if (!error) a->cfg.regional=a->regional_edit;
        a->regional_accept=0;
    }
    if (a->display_write) {
        a->display_write = 0;
        if (error) {
            a->display_accept = 0; a->display_dirty = 1; a->close_pending = a->navigation_pending = 0;
            if (a->display_leaving) { a->display_preview=1; a->display_leaving=0; }
        }
    }
    if (a->mode_state==CP_MODE_SAVE) { cp_display_saved(a,error); return; }
    if (!error && a->display_accept) cp_display_accept(a);
    if (error) {
        if (error<0) error=-error;
        char body[256]; snprintf(body,sizeof(body),"%s (%d)",strerror(error),error);
        text(a,C_STATUS,body);
        du_message(a->ui,D_INFO,"Settings not saved",body,DU_DLG_ICON_ERROR,NULL,0,0,0);
    } else text(a,C_STATUS,"Settings saved and applied.");
    cp_display_sync(a);
}

/* ---- display preview ---- */
/* The desktop colour as the desktop paints it: the unchanged default
 * follows the theme's desktop (shell.c desktop_colour). */
static uint32_t desktop_colour(const struct cp_app *a, const struct desk_theme *t)
{
    return a->cfg.desktop_color == CFG_DESKTOP_COLOR ? t->desktop : a->cfg.desktop_color;
}
static const struct desk_theme *chosen(const struct cp_app *a)
{ const struct desk_theme *t = desk_theme_named(a->cfg.theme); return t ? t : desk_theme; }
static void preview_build(struct cp_app *a)
{
    uint32_t sw = a->host.screen_w ? a->host.screen_w : 1024, sh = a->host.screen_h ? a->host.screen_h : 768;
    if (a->cfg.wallpaper[0] == '/') {
        /* Image wallpapers: decode once per path (kept across theme, mode and
         * colour changes) and render straight at preview size. A full-screen
         * fill of the 1448x1086 default scene followed by a 1024x768 -> 192x144
         * downscale took seconds under TCG and the window opened black
         * (lead fix, 2026-10-12). */
        if (a->host.decode_begin) {
            /* Worker job: decode (when needed) and resample off the UI thread. Show the desktop colour now,
             * wait for a completion descriptor, never decode or resample here. */
            int have = a->image.pixels && !strncmp(a->image_path, a->cfg.wallpaper, sizeof(a->image_path));
            int failed = !have && !strncmp(a->decode_failed, a->cfg.wallpaper, sizeof(a->decode_failed));
            if (!failed && !a->decode_wait) {
                struct cp_preview_req req; cp_preview_request(a, &req, have);
                if (!a->host.decode_begin(a->host.ctx, &req)) {
                    if (have) memset(&a->image, 0, sizeof(a->image));   /* moved to the worker */
                    a->wait_req = req; a->wait_req.image = (struct desk_image){0};
                    a->decode_wait = 1; snprintf(a->decode_path, sizeof(a->decode_path), "%s", a->cfg.wallpaper);
                } else if (!have) snprintf(a->decode_failed, sizeof(a->decode_failed), "%s", a->cfg.wallpaper);
            }
            /* The previous preview stays while a render-only job runs; otherwise the desktop colour. */
            int keep = have || (a->decode_wait && !strncmp(a->image_path, a->cfg.wallpaper, sizeof(a->image_path)));
            if (!keep) for (size_t i = 0; i < 192 * 144; i++) a->preview[i] = desktop_colour(a, chosen(a));
            a->preview_valid = 1;
            return;
        }
        if (!a->image.pixels || strncmp(a->image_path, a->cfg.wallpaper, sizeof(a->image_path))) {
            desk_image_free(&a->image);
            if (a->host.decode && strncmp(a->decode_failed, a->cfg.wallpaper, sizeof(a->decode_failed)))
                a->host.decode(a->host.ctx, a->cfg.wallpaper, &a->image);
            snprintf(a->image_path, sizeof(a->image_path), "%s", a->cfg.wallpaper);
        }
        uint32_t bar = sh ? (32u * 144u) / sh : 6;
        desk_wallpaper_render_pattern(a->preview, 192, 144, bar, bar, a->cfg.wallpaper, a->cfg.wallpaper_mode,
                              desktop_colour(a, chosen(a)), &a->image, a->host.portrait,a->cfg.wallpaper_pattern);
        a->preview_valid = 1;   /* no alpha: the scaled path never carried one either */
        return;
    }
    uint32_t *full = malloc((size_t)sw * sh * 4u);
    if (full) {
        desk_wallpaper_render_pattern(full, sw, sh, 32, 32, a->cfg.wallpaper, a->cfg.wallpaper_mode, desktop_colour(a, chosen(a)), &a->image, a->host.portrait,a->cfg.wallpaper_pattern);
        struct desk_image src = {full, sw, sh};
        for (size_t i = 0; i < (size_t)sw * sh; i++) full[i] |= 0xff000000u;
        desk_image_scale(&src, a->preview, 192, 144, CFG_WALL_STRETCH, desktop_colour(a, chosen(a)));
        free(full);
    } else for (size_t i = 0; i < 192 * 144; i++) a->preview[i] = desktop_colour(a, chosen(a));
    a->preview_valid = 1;
}
/* f2-88b: the 192x144 preview as one clipped row copy per scanline (was 27,648 one-pixel fills). */
static void preview_blit(struct du_canvas *c, int x, int y, const uint32_t *src)
{
    int x0 = x < c->clip.x ? c->clip.x : x, x1 = x + 192 > c->clip.x + c->clip.w ? c->clip.x + c->clip.w : x + 192;
    int y0 = y < c->clip.y ? c->clip.y : y, y1 = y + 144 > c->clip.y + c->clip.h ? c->clip.y + c->clip.h : y + 144;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->width) x1 = c->width;
    if (y1 > c->height) y1 = c->height;
    if (x1 <= x0 || y1 <= y0) return;
    for (int row = y0; row < y1; row++)
        memcpy(c->pixels + (size_t)row * (size_t)c->width + (size_t)x0, src + (row - y) * 192 + (x0 - x), (size_t)(x1 - x0) * sizeof(uint32_t));
}
static void put(struct du_canvas *c, int x, int y, int w, int h, uint32_t colour) { du_fill(c, (struct du_rect){x, y, w, h}, colour); }
/* A live miniature of the selected built-in saver, clipped to the CRT. */
static void saver_monitor(struct cp_app *a, struct du_canvas *c)
{
    du_fill(c,(struct du_rect){198,44,164,126},DU_FACE);
    du_edge(c,(struct du_rect){198,44,164,126},DU_RAISED);
    du_edge(c,(struct du_rect){206,52,148,112},DU_SUNKEN);
    struct du_rect screen={208,54,144,108}, clip=c->clip;
    c->clip=du_intersection(clip,screen); du_fill(c,screen,0xff000000u);
    uint64_t frame=a->ui->now/50000000ull;
    unsigned sw=a->host.screen_w ? a->host.screen_w : 1024,sh=a->host.screen_h ? a->host.screen_h : 768;
    if (!a->cfg.saver_enabled) {
        du_fill(c,screen,desk_theme->desktop);
        du_text_fit(c,224,104,112,"CiukiOS",DU_HILITE);
    } else if (a->cfg.saver_kind==1 && a->host.portrait) {
        struct desk_saver_sprite logo=desk_saver_logo(sw,sh,frame);
        /* Render the same bouncing portrait in miniature, nearest neighbour. */
        int x=screen.x+logo.x*screen.w/(int)sw,y=screen.y+logo.y*screen.h/(int)sh;
        int w=(int)logo.size*screen.w/(int)sw,h=(int)logo.size*screen.h/(int)sh;
        for (int j=0;j<h;j++) for (int i=0;i<w;i++)
            du_fill(c,(struct du_rect){x+i,y+j,1,1},a->host.portrait[(j*256/h)*256+i*256/w]);
    } else if (a->cfg.saver_kind==2) {
        for (unsigned k=0;k<96;k++) {
            struct desk_saver_sprite star=desk_saver_star(sw,sh,frame,k);
            if (star.x<0 || star.y<0 || star.x>=(int)sw || star.y>=(int)sh) continue;
            du_fill(c,(struct du_rect){screen.x+star.x*screen.w/(int)sw,screen.y+star.y*screen.h/(int)sh,1,1},star.colour);
        }
    }
    c->clip=clip;
    struct du_widget *combo=du_get(a->ui,CP_SAVER_KIND);
    if (combo) {
        struct du_rect b=combo->box;
        du_well(c,b,DU_WINDOW);
        du_text(c,b.x+5,b.y+8,combo->text,DU_INK);
        du_raised(c,(struct du_rect){b.x+b.w-22,b.y+2,20,b.h-4},combo->pressed,combo->hot);
        du_triangle(c,b.x+b.w-12,b.y+b.h/2,1,3,DU_INK);
        if (a->ui->focus==(int)(combo-a->ui->widgets)) du_dotted(c,(struct du_rect){b.x+3,b.y+3,b.w-28,b.h-6});
    }
    for (int id=CP_SAVER_UP;id<=CP_SAVER_DOWN;id++) {
        struct du_widget *arrow=du_get(a->ui,id);
        if (arrow) {
            du_raised(c,arrow->box,arrow->pressed,arrow->hot);
            du_triangle(c,arrow->box.x+10,arrow->box.y+6,id==CP_SAVER_UP ? 0 : 1,3,arrow->disabled ? DU_GRAY : DU_INK);
        }
    }
    du_fill(c,(struct du_rect){260,170,40,5},DU_SHADOW);
    du_edge(c,(struct du_rect){244,175,72,5},DU_RAISED_THIN);
}

static void paint(struct du_ui *u, void *user)
{
    struct cp_app *a = user;
    struct du_canvas *c = &u->canvas;
    user_paint(a);
    if (a->page==CP_DISPLAY && a->display_tab==CP_SCREEN_SAVER) saver_monitor(a,c);
    else if (a->page == CP_DISPLAY && a->display_tab == CP_BACKGROUND) {
        if (!a->preview_valid) {
            /* An image wallpaper not decoded yet: paint the desktop colour now and
             * decode after this frame (timer), so the window never opens black
             * (reading and decoding the 2.5 MB default scene takes seconds under TCG). */
            if (a->cfg.wallpaper[0] == '/' && a->preview_pending == 0 && !a->decode_wait &&   /* 0 never scheduled, 1 scheduled, 2 fired */
                (!a->image.pixels || strncmp(a->image_path, a->cfg.wallpaper, sizeof(a->image_path)))) {
                for (size_t i = 0; i < 192 * 144; i++) a->preview[i] = desktop_colour(a, chosen(a));
                a->preview_pending = 1; a->preview_valid = 1;
                du_timer_start(u, CP_T_PREVIEW, 20000000ull, 0);
            } else { preview_build(a); a->preview_pending = 0; }
        }
        /* A small monitor: bezel, screen, stand. */
        put(c, 24, 44, 208, 160, DU_FACE);
        du_edge(c, (struct du_rect){24, 44, 208, 160}, DU_RAISED);
        du_edge(c, (struct du_rect){30, 50, 196, 148}, DU_SUNKEN);
        preview_blit(c, 32, 52, a->preview);
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
        } else if (t->title_style == DESK_TITLE_HGRADIENT) desk_paint_hgradient(&pp, 41, 133, 68, 8, t->title[0], t->title[1]);
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
        dt_paint(a, c);
    } else if (a->page == CP_DISPLAY && a->display_tab == CP_APPEARANCE) {
        const struct ap_item *v=&a->cfg.appearance.item[a->appearance_item];
        struct desk_paint target={c->pixels,c->width,c->height,c->clip.x,c->clip.y,c->clip.w,c->clip.h};
        desk_paint_hgradient(&target,244,72,278,40,v->colour,v->gradient);
        du_text_fit(c,252,86,260,"Appearance sample",v->text);
    }
    else if(a->page==CP_FONTS) cp_fonts_paint(a,c);
    else if (a->page == CP_SYSTEM && a->system_tab == 0 && a->host.portrait) {
        du_edge(c, (struct du_rect){22, 46, 100, 100}, DU_SUNKEN);
        struct du_rect r=du_intersection(c->clip,(struct du_rect){24,48,96,96});
        for (int y=r.y;y<r.y+r.h;y++) {
            const uint32_t *src=a->host.portrait+((y-48)*256/96)*256;
            uint32_t *dst=c->pixels+(size_t)y*c->width;
            for (int x=r.x;x<r.x+r.w;x++) dst[x]=src[(x-24)*256/96];
        }
        du_text_font(c, desk_theme->font_large, 140, 50, "CiukiOS", DU_SELECT);
    }
}

/* ---- state to widgets ---- */
static void toggle_label(struct cp_app *a, int id, const char *name, int on)
{ char s[64]; snprintf(s, sizeof(s), "%s: %s", name, on ? "On" : "Off"); text(a, id, s); }
static void sync(struct cp_app *a)
{
    struct du_ui *u=a->ui;
    struct visual_state { int value,disabled,def; unsigned active; } before[DU_WIDGETS];
    for (unsigned i=0;i<u->count;i++) before[i]=(struct visual_state){u->widgets[i].value,u->widgets[i].disabled,u->widgets[i].def,u->widgets[i].active};
    struct du_widget *w;
    int t = cfg_theme_index(a->cfg.theme);
    for (int k = 0; k < CFG_THEMES; k++) a->theme_items[k].selected = k == t;
    if ((w = du_get(u, C_THEME))) w->active = t < 0 ? 0 : (unsigned)t;
    unsigned wall = 0;
    for (unsigned k = 0; k < 2 + DESK_BUILTIN_WALLPAPERS + a->wall_count + 8; k++) {
        const char *spec = k == 0 ? "none" : k == 1 ? "ciuki" : NULL;
        char built[48];
        if (!spec && k < 2 + DESK_BUILTIN_WALLPAPERS) { snprintf(built, sizeof(built), "builtin:%s", desk_builtin_wallpapers[k - 2].id); spec = built; }
        unsigned file_end = 2 + DESK_BUILTIN_WALLPAPERS + a->wall_count;
        if (!spec && k < file_end) spec = a->wall_names[k - 2 - DESK_BUILTIN_WALLPAPERS];
        char pattern[16];
        if (!spec) { snprintf(pattern, sizeof(pattern), "pattern:%u", k - file_end); spec = pattern; }
        a->wall_items[k].selected = !strcmp(spec, a->cfg.wallpaper);
        if (a->wall_items[k].selected) wall = k;
    }
    if ((w = du_get(u, C_WALL))) { w->active = wall; du_list_ensure(w, wall); }
    for (int k = 0; k < CP_POSITIONS; k++) if ((w = du_get(u, C_POS0 + k))) w->def = mode_order[k] == a->cfg.wallpaper_mode;
    static const int rgb_ids[3]={C_RGB_R,C_RGB_G,C_RGB_B};
    for (unsigned k=0;k<3;k++) if ((w=du_get(u,rgb_ids[k]))) w->value=(int)((a->cfg.desktop_color>>(16-k*8))&255u);
    toggle_label(a, C_ICONS, "Desktop icons", a->cfg.desktop_icons);
    if ((w=du_get(u,CP_WALL_PATTERN))) du_combo_set(w,a->pattern_items,DESK_WALL_PATTERNS,a->cfg.wallpaper_pattern);
    char s[64];
    if ((w = du_get(u, C_SPEED))) w->value = a->cfg.mouse_speed - CFG_MOUSE_MIN;
    snprintf(s, sizeof(s), "%d of %d%s", a->cfg.mouse_speed, CFG_MOUSE_MAX, a->cfg.mouse_speed == CFG_MOUSE_NORMAL ? " (normal)" : ""); text(a, C_SPEED_V, s);
    if ((w = du_get(u, C_DBL))) w->value = (a->cfg.double_click_ms - 200) / 100;
    snprintf(s, sizeof(s), "%d ms", a->cfg.double_click_ms); text(a, C_DBL_V, s);
    if (a->page==CP_MOUSE || a->page==CP_KEYBOARD) {
        if ((w=du_get(u,C_REPEAT_DELAY))) w->value=a->cfg.repeat_delay;
        if ((w=du_get(u,C_REPEAT_SPEED))) w->value=a->cfg.repeat_speed;
        if ((w=du_get(u,C_BLINK))) w->value=a->cfg.cursor_blink_ms/100;
        if ((w=du_get(u,C_SWAP))) w->value=a->cfg.mouse_swap;
        if ((w=du_get(u,C_TRAILS))) w->value=a->cfg.mouse_trails;
        if ((w=du_get(u,C_WHEEL))) w->value=a->cfg.wheel_lines;
        if ((w=du_get(u,C_LAYOUT_SWITCH))) w->value=a->cfg.layout_switch;
        text(a,C_POINTER,a->cfg.pointer_scheme==1 ? "Pointers: Ciuki Large" : a->cfg.pointer_scheme==2 ? "Pointers: Ciuki Inverted" : "Pointers: Ciuki");
    }
    for (int k = 0; k < CFG_KEYS_COUNT; k++) a->key_items[k].selected = k == a->cfg.keyboard;
    if ((w = du_get(u, C_KEY))) w->active = (unsigned)a->cfg.keyboard;
    if ((w = du_get(u, C_VOL))) { w->value = a->volume; w->disabled = !a->mixer_ok; }
    snprintf(s, sizeof(s), "%d%%", a->volume); text(a, C_VOL_V, s);
    toggle_label(a, C_MUTE, "Mute", a->muted);
    if ((w = du_get(u, C_MUTE))) w->disabled = !a->mixer_ok;
    text(a, C_AUDIO, a->mixer_ok ? "Master output of the sound card (/dev/mixer)." : "No audio device: /dev/mixer is unavailable.");
    for (unsigned k = 0; k < SOUND_EVENTS; k++) a->sound_items[k].selected = k == a->sound_selected;
    if ((w = du_get(u,C_SOUND_LIST))) w->active = a->sound_selected;
    if ((w = du_get(u,C_SOUND_ENABLED))) w->value = a->sounds.enabled;
    struct sound_scheme defaults; sound_defaults(&defaults);
    unsigned scheme = !a->sounds.enabled ? 1 : memcmp(defaults.file,a->sounds.file,sizeof(defaults.file)) ? 2 : 0;
    for (unsigned k=0;k<a->sound_name_count;k++)
        if (!strcmp(a->sound_named,a->sound_names[k])) scheme=k+3;
    for (unsigned k = 0; k < 3+a->sound_name_count; k++) a->scheme_items[k].selected = k == scheme;
    if ((w=du_get(u,C_SCHEME_DELETE))) w->disabled=scheme<3;
    if ((w = du_get(u,C_SCHEME))) { w->active = scheme; du_list_ensure(w,scheme); }
    if ((w = du_get(u,C_UI_VOL))) w->value = a->sounds.volume;
    snprintf(a->sound_volume_label,sizeof(a->sound_volume_label),"%d%%",a->sounds.volume);
    text(a,C_UI_VOL_V,a->sound_volume_label);
    for (unsigned k = 0; k < SOUND_EVENTS; k++) a->sound_items[k].text[1] = a->sounds.file[k][0] ? a->sounds.file[k] : "(None)";
    if ((w = du_get(u,C_HARDWARE))) w->disabled = a->audio_testing;
    toggle_label(a, C_H24, "24-hour clock", a->cfg.clock_24h);
    toggle_label(a, C_SECONDS, "Show seconds", a->cfg.clock_seconds);
    if ((w = du_get(u,C_TASKBAR_CLOCK))) w->value = a->cfg.taskbar_clock;
    if ((w = du_get(u,C_QUICK_LAUNCH))) w->value = a->cfg.quick_launch;
    if ((w=du_get(u,C_TASKBAR_TOP))) w->value=a->cfg.taskbar_top;
    if ((w=du_get(u,C_TASKBAR_AUTOHIDE))) w->value=a->cfg.taskbar_autohide;
    if ((w=du_get(u,C_START_SMALL))) w->value=a->cfg.start_small;
    cp_display_sync(a);
    for (unsigned i=0;i<u->count;i++) {
        struct du_widget *v=&u->widgets[i];
        if (v->value!=before[i].value || v->disabled!=before[i].disabled || v->def!=before[i].def || v->active!=before[i].active) du_invalidate_widget(u,v->id);
    }
    if (a->page==CP_DISPLAY && !a->preview_valid) {
        du_invalidate(u,(struct du_rect){24,44,208,188});
        for (int i=0;i<6;i++) { struct du_widget *v=du_get(u,C_COL0+i); if (v && !v->hidden) du_invalidate(u,(struct du_rect){v->box.x-1,v->box.y-1,v->box.w+2,v->box.h+2}); }
    }
}
void cp_audio_result(struct cp_app *a, int result)
{
    a->audio_testing = 0;
    struct ciuki_audio_diagnostics d = {0};
    int read = a->host.audio_diagnostics ? a->host.audio_diagnostics(a->host.ctx, &d) : -1;
    char message[512];
    if (read || d.version != 1 || d.size != sizeof(d)) {
        snprintf(message, sizeof(message), "1 kHz tone result: %d\nKernel audio diagnostics unavailable (error %d).", result, read);
    } else {
        int compared = a->audio_before_ok && d.codec_id == a->audio_before.codec_id &&
                       d.fetched_frames >= a->audio_before.fetched_frames && d.irqs >= a->audio_before.irqs;
        uint64_t fetched = compared ? d.fetched_frames - a->audio_before.fetched_frames : 0;
        uint64_t irqs = compared ? d.irqs - a->audio_before.irqs : 0;
        snprintf(message, sizeof(message),
                 "1 kHz tone result: %d\nController: %04x:%04x  Codec: %08x\n"
                 "DMA advanced: %s (%llu frames)\nIRQs received: %s (%llu, line %u)\n"
                 "CIV: %u  PICB: %u  Kernel error: %d\nPower: %04x  Master: %04x  PCM: %04x\nRate: %u\n"
                 "Backend: %s%s\nKernel observations; check whether the tone was heard.",
                 result, d.controller >> 16, d.controller & 0xffff, d.codec_id,
                 compared ? fetched ? "yes" : "no" : "unknown", (unsigned long long)fetched,
                 compared ? irqs ? "yes" : "no" : "unknown", (unsigned long long)irqs, d.irq_line,
                 d.civ, d.picb, d.last_error, d.power, d.master, d.pcm, d.rate,
                 d.flags & CIUKI_AUDIO_DIAG_READY ? "ready" : "unavailable",
                 d.flags & CIUKI_AUDIO_DIAG_QUARANTINE ? ", quarantined" : "");
    }
    static const char *const buttons[] = {"OK"};
    du_message(a->ui, 899, "Test audio hardware", message, DU_DLG_ICON_INFO, buttons, 1, 0, 0);
    text(a,C_STATUS,result ? "Audio test failed; see hardware diagnostics." : "Audio test finished; see hardware diagnostics.");
    sync(a);
}
/* fx-uaccounts: adopt every finished worker result (User Accounts / Users,
 * Network, Game properties). The native loop calls this on every turn after
 * a completion wake-up, on every page: these pages have no one-second
 * heartbeat, so adoption must never depend on cp_tick. */
void cp_jobs_poll(struct cp_app *a)
{
    user_poll(a);
    network_poll(a);
    game_poll(a);
}
void cp_tick(struct cp_app *a)
{
    cp_jobs_poll(a);
    if (a->page==CP_JOYSTICK) {
        if (a->joystick_save_pending && !a->joystick_busy && !a->joystick_calibrating) {
            struct ciuki_joystick_control c={.version=CIUKI_JOYSTICK_VERSION,.size=sizeof(c),.operation=CIUKI_JOY_SAVE_STATUS};
            (void)joystick_send(a,&c);
        } else if (!a->game_open) joystick_query(a,CIUKI_JOY_QUERY);
    }
    if (a->close_pending && !a->display_write && !a->mode_state) cp_close(a);
    if (a->navigation_pending && !a->display_write && !a->mode_state) {
        int page=a->next_page; a->navigation_pending=0; cp_open(a,page);
    }
    if (a->page == CP_SOUNDS && a->host.mixer && a->ui->capture < 0) {
        int volume, mute;
        if (!a->host.mixer(a->host.ctx, &volume, &mute) &&
            (a->volume != volume || a->muted != mute)) {
            a->volume = volume; a->muted = mute; sync(a);
        }
    }
    if (a->page == CP_POWER) { pw_tick(a); return; }
    if (a->page==CP_DATETIME) { dt_tick(a); return; }
    if (a->page==CP_ACCESS) { ac_tick(a); return; }
    if (a->page!=CP_SYSTEM) return;
    char previous[sizeof(a->clock_line)]; memcpy(previous,a->clock_line,sizeof(previous));
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
    if (a->page==CP_DATETIME && strcmp(previous,a->clock_line)) du_invalidate(a->ui,(struct du_rect){24,44,452,54});
    if (a->devices_ok && a->page == CP_SYSTEM) {
        uint64_t seconds = a->devices.uptime_ms / 1000;
        uint64_t current = a->host.monotonic ? a->host.monotonic(a->host.ctx) : a->ui->now;
        if (current >= a->snapshot_time) seconds += (current - a->snapshot_time) / 1000000000ull;
        char uptime[96]; snprintf(uptime, sizeof(uptime), "Uptime: %llu days, %02u:%02u:%02u", (unsigned long long)(seconds / 86400),
            (unsigned)(seconds / 3600 % 24), (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60));
        text(a, C_UPTIME, uptime);
    }
}
static void log_refresh(struct cp_app *a)
{
    static char bytes[CP_LOG_BYTES]; size_t length=0; uint32_t total=0;
    if (!a->log.text && du_ta_init(&a->log,CP_LOG_BYTES)) {
        snprintf(a->log_status,sizeof(a->log_status),"The log view could not be allocated."); return;
    }
    a->log.readonly=1;
    int error=a->host.log_read ? a->host.log_read(a->host.ctx,a->log_offset,bytes,sizeof(bytes),&length,&total) : -ENOSYS;
    if (!error && a->log_offset>=total && a->log_offset) {
        a->log_offset=total ? (total-1)/CP_LOG_BYTES*CP_LOG_BYTES : 0;
        error=a->host.log_read(a->host.ctx,a->log_offset,bytes,sizeof(bytes),&length,&total);
    }
    if (error || length>sizeof(bytes) || a->log_offset>total || length>total-a->log_offset) {
        if (!error) error=-EIO;
        snprintf(a->log_status,sizeof(a->log_status),"Boot log unavailable: %s (%d)",strerror(error<0 ? -error : error),error<0 ? -error : error);
        a->log_total=0; du_ta_set_text(&a->log,"",0);
    } else {
        a->log_total=total;
        for (size_t k=0;k<length;k++) if (!bytes[k]) bytes[k]=' ';
        du_ta_set_text(&a->log,bytes,length);
        snprintf(a->log_status,sizeof(a->log_status),"Boot log (read-only): bytes %u-%u of %u",length ? a->log_offset+1 : 0,a->log_offset+(unsigned)length,total);
    }
    text(a,CP_LOG_STATUS,a->log_status);
    struct du_widget *w=du_get(a->ui,CP_LOG_PREV); if (w) w->disabled=!a->log_total || !a->log_offset;
    w=du_get(a->ui,CP_LOG_NEXT); if (w) w->disabled=!a->log_total || a->log_offset+a->log.len>=a->log_total;
    du_invalidate_widget(a->ui,CP_LOG_VIEW); du_invalidate_widget(a->ui,CP_LOG_PREV); du_invalidate_widget(a->ui,CP_LOG_NEXT);
}
static void menu_begin(struct cp_app *a)
{
    int r=a->host.menu_begin ? a->host.menu_begin(a->host.ctx,&a->menu_edit,a->menu_args) : -ENOSYS;
    if (r) text(a,C_STATUS,"The Start menu operation could not be started. Try again.");
    else { a->menu_busy=1;text(a,C_STATUS,"Updating Start menu..."); }
}
void cp_menu_poll(struct cp_app *a)
{
    if (!a->menu_busy || !a->host.menu_poll || !a->host.menu_poll(a->host.ctx,&a->menu)) return;
    a->menu_busy=0;
    if (!a->menu.error && a->menu.operation==SH_EDIT_CLEAR) { a->menu_cleared=1;a->cfg.recent_count=0;changed(a); }
    if (!a->menu.error && a->host.programs_changed && a->menu.operation!=SH_EDIT_REFRESH) a->host.programs_changed(a->host.ctx);
    if (a->page==CP_TASKBAR && a->taskbar_tab==1) build(a);
    text(a,C_STATUS,a->menu.error ? "The Start menu operation failed. Check the path and try again." : "Start menu updated.");
}
static void menu_refresh(struct cp_app *a)
{
    if (a->menu_busy) return;
    memset(&a->menu_edit,0,sizeof(a->menu_edit));a->menu_edit.operation=SH_EDIT_REFRESH;menu_begin(a);
}
static int taskbar_event(struct cp_app *a,const struct du_event *e)
{
    if (a->page!=CP_TASKBAR) return 0;
    struct du_ui *u=a->ui;
    if (e->id==CP_TASKBAR_TABS && e->type==DU_EV_SELECT && e->index>=0 && e->index<2) {
        a->taskbar_tab=e->index;build(a);if (a->taskbar_tab) menu_refresh(a);return 1;
    }
    if (a->menu_busy) {
        if (e->id>=CP_START_ADD && e->id<=CP_START_LIST) return 1;
        return 0;
    }
    switch (e->id) {
    case CP_START_LIST:
        if (e->type==DU_EV_SELECT && e->index>=0 && (unsigned)e->index<a->menu.count) {
            a->menu_selected=(unsigned)e->index;
            for (unsigned k=0;k<a->menu.count;k++) a->menu_items[k].selected=k==a->menu_selected;
        }return 1;
    case CP_START_ADD:
        if (e->type==DU_EV_ACTIVATE) du_file_dialog(u,CP_ADD_TARGET,0,"Create Shortcut - select a program or document","/","",NULL,NULL);
        return 1;
    case CP_ADD_TARGET:
        if (e->type==DU_EV_DIALOG && !e->index && e->text) {
            if (strlen(e->text)>=sizeof(a->menu_target)) { text(a,C_STATUS,"The target path is too long.");return 1; }
            strcpy(a->menu_target,e->text);
            du_input_box(u,CP_ADD_ARGS,"Create Shortcut","Arguments (leave empty for none):","");
        }return 1;
    case CP_ADD_ARGS:
        if (e->type==DU_EV_DIALOG && !e->index && e->text) {
            snprintf(a->menu_args,sizeof(a->menu_args),"%s",e->text);
            du_input_box(u,CP_ADD_FOLDER,"Select Program Folder","Choose a folder in Start Menu/Programs (subfolders are allowed):",SH_PROGRAMS_FOLDER);
        }return 1;
    case CP_ADD_FOLDER:
        if (e->type==DU_EV_DIALOG && !e->index && e->text) {
            size_t prefix=strlen(SH_PROGRAMS_FOLDER);
            if (strlen(e->text)>=sizeof(a->menu_folder) || strncmp(e->text,SH_PROGRAMS_FOLDER,prefix) || (e->text[prefix] && e->text[prefix]!='/') || strstr(e->text,"/../")) {
                text(a,C_STATUS,"Choose a folder inside Start Menu/Programs.");return 1;
            }
            strcpy(a->menu_folder,e->text);
            const char *name=strrchr(a->menu_target,'/');
            du_input_box(u,CP_ADD_NAME,"Select a Title","Type a name for the shortcut:",name ? name+1 : a->menu_target);
        }return 1;
    case CP_ADD_NAME:
        if (e->type==DU_EV_DIALOG && !e->index && e->text) {
            if (strlen(e->text)>58 || !*e->text || strchr(e->text,'/') || strchr(e->text,'\\')) { text(a,C_STATUS,"Choose a shorter name without slashes.");return 1; }
            memset(&a->menu_edit,0,sizeof(a->menu_edit));a->menu_edit.operation=SH_EDIT_ADD;
            strcpy(a->menu_edit.path,a->menu_target);strcpy(a->menu_edit.destination,a->menu_folder);strcpy(a->menu_edit.name,e->text);menu_begin(a);
        }return 1;
    case CP_START_REMOVE:
        if (e->type==DU_EV_ACTIVATE && a->menu_selected<a->menu.count) {
            static const char *const buttons[]={"Remove","Cancel"};
            du_message(u,CP_REMOVE_CONFIRM,"Remove Shortcuts/Folders","Remove the selected shortcut from the Start menu? The program is kept installed.",DU_DLG_ICON_QUESTION,buttons,2,1,1);
        }return 1;
    case CP_REMOVE_CONFIRM:
        if (e->type==DU_EV_DIALOG && !e->index && a->menu_selected<a->menu.count) {
            memset(&a->menu_edit,0,sizeof(a->menu_edit));a->menu_edit.operation=SH_EDIT_DELETE;
            strcpy(a->menu_edit.path,a->menu.entries[a->menu_selected].path);menu_begin(a);
        }return 1;
    case CP_START_ADVANCED:
        if (e->type==DU_EV_ACTIVATE && (!a->host.menu_explore || a->host.menu_explore(a->host.ctx,SH_START_FOLDER))) text(a,C_STATUS,"The Start Menu folder could not be opened.");
        return 1;
    case CP_START_CLEAR:
        if (e->type==DU_EV_ACTIVATE) { memset(&a->menu_edit,0,sizeof(a->menu_edit));a->menu_edit.operation=SH_EDIT_CLEAR;menu_begin(a); }
        return 1;
    default:return 0;
    }
}
enum { A_SCHEME=4100,A_SAVE,A_DELETE,A_NAME,A_ITEMS,A_COLOUR,A_GRADIENT,A_TEXT,A_SIZE,A_FONT,A_POINTS,A_BOLD,A_ITALIC,
       A_EFFECT0,A_EFFECT1,A_EFFECT2,A_EFFECT3,A_SMALL,A_LARGE,A_DPI,A_SAVE_NAME,A_DOC_ICON,A_BIN_ICON };
static void appearance_build(struct cp_app *a)
{
    struct du_widget *w; struct du_ui *u=a->ui;
    a->appearance_count=0;
    int catalogue=a->display_tab==CP_THEMES ? CFG_DESKTOP_THEMES : AP_SCHEMES;
    for(int i=0;i<AP_BUILTINS+catalogue;i++) {
        const char *name=i<AP_BUILTINS ? ap_builtin_name(i) : a->display_tab==CP_THEMES ? a->cfg.desktop_themes[i-AP_BUILTINS].name : a->cfg.schemes[i-AP_BUILTINS].name;
        if(!*name) continue;
        unsigned n=a->appearance_count++;
        snprintf(a->appearance_names[n],32,"%s",name);
        a->appearance_schemes[n]=(struct du_item){.text={a->appearance_names[n]},.selected=!strcmp(name,a->display_tab==CP_THEMES ? a->cfg.desktop_theme_name : a->cfg.scheme)};
    }
    add(a,DU_LABEL,4190,24,44,190,16,"Scheme:");
    w=add(a,DU_LISTVIEW,A_SCHEME,24,64,200,80,"");
    w->items=a->appearance_schemes; w->count=a->appearance_count; list_style(w,"Scheme");
    for(unsigned i=0;i<a->appearance_count;i++) if(a->appearance_schemes[i].selected) w->active=i;
    du_list_ensure(w,w->active);
    add(a,DU_BUTTON,A_SAVE,24,150,96,24,"Save As...");
    if(a->display_tab==CP_APPEARANCE) add(a,DU_BUTTON,4132,244,114,278,20,"Installed Fonts...");
    add(a,DU_BUTTON,A_DELETE,128,150,96,24,"Delete");
    if(a->display_tab==CP_THEMES) {
        add(a,DU_LABEL,4191,24,194,490,18,"A package includes appearance, wallpaper, sounds, pointers,");
        add(a,DU_LABEL,4192,24,222,490,18,"screen saver and icon sizes. Save As captures current choices.");
        return;
    }
    for(int i=0;i<AP_ITEMS;i++) a->appearance_items[i]=(struct du_item){.text={ap_item_name(i)},.selected=i==a->appearance_item};
    w=add(a,DU_LISTVIEW,A_ITEMS,24,198,200,144,""); w->items=a->appearance_items; w->count=AP_ITEMS;
    w->active=(unsigned)a->appearance_item; list_style(w,"Item"); du_list_ensure(w,w->active);
    struct ap_item *v=&a->cfg.appearance.item[a->appearance_item];
    const char *const labels[]={"Colour 1","Colour 2","Text colour"};
    const uint32_t colours[]={v->colour,v->gradient,v->text}; char value[32];
    for(int i=0;i<3;i++) {
        add(a,DU_LABEL,4193+i,244,142+i*26,100,18,labels[i]);
        snprintf(value,sizeof(value),"%06x",(unsigned)colours[i]);
        add(a,DU_EDIT,A_COLOUR+i,352,138+i*26,170,24,value);
    }
    snprintf(value,sizeof(value),"Size: %u pixels",v->size); add(a,DU_LABEL,4196,244,222,278,18,value);
    w=slider(a,A_SIZE,244,242,278,ap_size_max(a->appearance_item)-ap_size_min(a->appearance_item),0); w->value=v->size-ap_size_min(a->appearance_item);
    snprintf(value,sizeof(value),"Font: %s",desk_font(v->font)->name); add(a,DU_BUTTON,A_FONT,244,270,278,24,value);
    snprintf(value,sizeof(value),"Point size: %u",v->points); add(a,DU_LABEL,4197,244,300,278,16,value);
    w=slider(a,A_POINTS,244,316,140,30,0); w->value=v->points-6;
    add(a,DU_BUTTON,A_BOLD,390,316,64,24,(v->style&1) ? "Bold: On" : "Bold");
    add(a,DU_BUTTON,A_ITALIC,458,316,64,24,(v->style&2) ? "Italic: On" : "Italic");
    (void)u;
}
static int appearance_event(struct cp_app *a,const struct du_event *e)
{
    if(e->id<A_SCHEME || e->id>A_BIN_ICON) return 0;
    struct appearance *p=&a->cfg.appearance;
    struct ap_item *v=&p->item[a->appearance_item]; int changed=0, rebuild=0;
    if((e->id==A_DOC_ICON || e->id==A_BIN_ICON) && e->type==DU_EV_ACTIVATE) {
        if(e->id==A_DOC_ICON) a->cfg.document_icon^=1; else a->cfg.recycle_icon^=1;
        changed=rebuild=1;
    } else if(e->id==A_SCHEME && e->type==DU_EV_SELECT && e->index>=0 && (unsigned)e->index<a->appearance_count) {
        const char *name=a->appearance_names[e->index];
        if(a->display_tab==CP_THEMES) {
            if(e->index<AP_BUILTINS) {
                changed=!cfg_scheme_use(&a->cfg,name); sound_defaults(&a->sounds);
                strcpy(a->cfg.wallpaper,e->index==5 ? "/system/wallpapers/Ciuk2.png" : "ciuki");
                a->cfg.wallpaper_mode=CFG_WALL_CENTER;
                a->cfg.saver_kind=1; a->cfg.saver_enabled=1; a->cfg.saver_seconds=600;
                a->cfg.pointer_scheme=e->index==1 || e->index==2 ? 1 : 0;
                if(a->cfg.pointer_scheme) a->cfg.appearance.effects|=AP_LARGE_ICONS;
            } else changed=!cfg_desktop_theme_use(&a->cfg,name,&a->sounds);
            if(changed) { a->display_sounds_dirty=1; snprintf(a->cfg.desktop_theme_name,sizeof(a->cfg.desktop_theme_name),"%s",name); }
        } else changed=!cfg_scheme_use(&a->cfg,name);
        rebuild=1;
    } else if(e->id==A_ITEMS && e->type==DU_EV_SELECT && e->index>=0 && e->index<AP_ITEMS) {
        a->appearance_item=e->index; rebuild=1;
    } else if(e->id==A_SAVE && e->type==DU_EV_ACTIVATE) {
        du_input_box(a->ui,A_SAVE_NAME,"Save Scheme","Type a name for this scheme:","My Scheme");
    } else if(e->id==A_SAVE_NAME && e->type==DU_EV_DIALOG && !e->index && e->text) {
        changed=!(a->display_tab==CP_THEMES ? cfg_desktop_theme_save(&a->cfg,e->text,&a->sounds) : cfg_scheme_save(&a->cfg,e->text));
        if(changed && a->display_tab==CP_THEMES) snprintf(a->cfg.desktop_theme_name,sizeof(a->cfg.desktop_theme_name),"%s",e->text);
        rebuild=1;
        if(!changed) text(a,C_STATUS,"Choose a unique name; the user scheme catalogue may be full.");
    } else if(e->id==A_DELETE && e->type==DU_EV_ACTIVATE) {
        changed=!(a->display_tab==CP_THEMES ? cfg_desktop_theme_delete(&a->cfg,a->cfg.desktop_theme_name) : cfg_scheme_delete(&a->cfg,a->cfg.scheme)); rebuild=1;
        if(!changed) text(a,C_STATUS,"Built-in schemes cannot be deleted.");
    } else if(e->id>=A_COLOUR && e->id<=A_TEXT && (e->type==DU_EV_CHANGE || e->type==DU_EV_COMMIT)) {
        struct du_widget *w=du_get(a->ui,e->id); uint32_t value;
        if(w && strlen(w->text)==6 && ap_unhex(w->text,6,&value)) {
            if(e->id==A_COLOUR) v->colour=value;
            else if(e->id==A_GRADIENT) v->gradient=value;
            else v->text=value;
            changed=1;
        }
    } else if((e->id==A_SIZE || e->id==A_POINTS) && e->type==DU_EV_CHANGE) {
        if(e->index>=0 && e->index<(e->id==A_SIZE ? ap_size_max(a->appearance_item)-ap_size_min(a->appearance_item)+1 : 31)) {
            if(e->id==A_SIZE) v->size=(uint8_t)(e->index+ap_size_min(a->appearance_item)); else v->points=(uint8_t)(e->index+6);
            char label[64];
            snprintf(label,sizeof(label),e->id==A_SIZE ? "Size: %u pixels" : "Point size: %u",e->id==A_SIZE ? v->size : v->points);
            text(a,e->id==A_SIZE ? 4196 : 4197,label); changed=1;
        }
    } else if(e->id==A_FONT && e->type==DU_EV_ACTIVATE) { v->font=(v->font+1)%DESK_FONTS; changed=rebuild=1; }
    else if((e->id==A_BOLD || e->id==A_ITALIC) && e->type==DU_EV_ACTIVATE) { v->style^=e->id==A_BOLD ? 1 : 2; changed=rebuild=1; }
    else if(e->id>=A_EFFECT0 && e->id<=A_EFFECT3 && e->type==DU_EV_ACTIVATE) { p->effects^=1u<<(e->id-A_EFFECT0); changed=rebuild=1; }
    else if((e->id==A_SMALL || e->id==A_LARGE) && e->type==DU_EV_ACTIVATE) { p->dpi=e->id==A_SMALL ? 96 : 120; changed=rebuild=1; }
    else if(e->id==A_DPI && e->type==DU_EV_CHANGE && e->index>=0 && e->index<=192) {
        char label[64]; p->dpi=(uint16_t)(48+e->index);
        snprintf(label,sizeof(label),"Custom: %u dpi",p->dpi); text(a,4198,label); changed=1;
    }
    if(changed) {
        a->cfg.appearance_set=1; a->cfg.desktop_color=p->item[AP_DESKTOP].colour;
        a->display_dirty=1; a->preview_valid=0;
    }
    if(rebuild) build(a);
    if(changed) du_invalidate(a->ui,(struct du_rect){0,0,a->ui->canvas.width,a->ui->canvas.height});
    return 1;
}
static void advanced_snapshot(struct cp_app *a)
{
    a->advanced_original=a->cfg.appearance; a->advanced_original_set=a->cfg.appearance_set;
    a->advanced_dirty=a->display_dirty; a->advanced_colour=a->cfg.desktop_color;
}
static void advanced_change(struct du_ui *u,const struct du_event *e,void *ctx)
{
    struct cp_app *a=ctx; struct du_event changed=*e;
    if((e->id==A_SMALL || e->id==A_LARGE) && e->type==DU_EV_ACTIVATE) {
        changed=(struct du_event){.type=DU_EV_CHANGE,.id=A_DPI,.index=(e->id==A_SMALL ? 96 : 120)-48};
    }
    if(changed.id!=A_DPI || changed.type!=DU_EV_CHANGE) return;
    (void)appearance_event(a,&changed);
    u->dialog.items[7].value=a->cfg.appearance.dpi-48;
    char label[64]; snprintf(label,sizeof(label),"Custom font size: %u dpi",a->cfg.appearance.dpi);
    du_set_text(&u->dialog.items[8],label);
}
static int advanced_accept(struct du_ui *u,int button,void *ctx)
{
    struct cp_app *a=ctx;
    if(a->display_write) { du_set_text(&u->dialog.items[9],"Waiting for settings to finish saving..."); return 1; }
    if(button==1) {
        a->cfg.appearance=a->advanced_original; a->cfg.appearance_set=a->advanced_original_set;
        a->display_dirty=a->advanced_dirty; a->cfg.desktop_color=a->advanced_colour;
        build(a);
    } else if(button==2) {
        if(cp_display_store(a)) du_set_text(&u->dialog.items[9],"The font settings could not be saved.");
        else advanced_snapshot(a);
        return 1;
    } else build(a);
    return 1;
}
static void advanced_open(struct cp_app *a)
{
    static const struct du_item pages[]={{.text={"General"}}};
    advanced_snapshot(a);
    du_property_sheet(a->ui,4134,"Advanced Display Properties",500,280,pages,1);
    a->ui->dialog.changed=advanced_change; a->ui->dialog.accept=advanced_accept;
    a->ui->dialog.accept_ctx=a; a->ui->dialog.sheet_ctx=a;
    du_property_control(a->ui,4,DU_LABEL,4150,(struct du_rect){24,42,440,20},"Font size:");
    du_property_control(a->ui,5,DU_BUTTON,A_SMALL,(struct du_rect){24,70,204,26},"Small Fonts (96 dpi)");
    du_property_control(a->ui,6,DU_BUTTON,A_LARGE,(struct du_rect){244,70,224,26},"Large Fonts (120 dpi)");
    struct du_widget *w=du_property_control(a->ui,7,DU_TRACKBAR,A_DPI,(struct du_rect){24,106,444,30},"");
    du_trackbar_set(w,0,192,a->cfg.appearance.dpi-48,1,24,24);
    char label[64]; snprintf(label,sizeof(label),"Custom font size: %u dpi",a->cfg.appearance.dpi);
    du_property_control(a->ui,8,DU_LABEL,4151,(struct du_rect){24,142,444,20},label);
    du_property_control(a->ui,9,DU_LABEL,4152,(struct du_rect){24,170,444,20},"Reopen windows after applying the new font size.");
}
static void event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct cp_app *a = user;
    if (user_event(a,e)) return;
    if (regional_event(a,e)) return;
    if (cp_pack_event(a, e)) return;
    if (cp_pack_event(a, e) || taskbar_event(a,e)) return;
    int save = 0;
    if (e->type == DU_EV_KEY && e->key == DU_KEY_ESC) { cp_close(a); return; }
    if (e->type==DU_EV_KEY && a->page==CP_DISPLAY && a->display_tab==CP_SCREEN_SAVER &&
        du_focused(u)==CP_SAVER_WAIT && (e->key==DU_KEY_UP || e->key==DU_KEY_DOWN)) {
        struct du_event arrow={.type=DU_EV_ACTIVATE,.id=e->key==DU_KEY_UP ? CP_SAVER_UP : CP_SAVER_DOWN};
        event(u,&arrow,user); return;
    }
    if (e->type == DU_EV_KEY && e->key == DU_KEY_ENTER && a->page == CP_DISPLAY) {
        struct du_event ok = {.type=DU_EV_ACTIVATE,.id=CP_OK}; event(u,&ok,user); return;
    }
    if ((e->id==CP_CANCEL || e->id==C_CLOSE) && e->type==DU_EV_ACTIVATE) { cp_close(a); return; }
    if (a->display_write && e->id==C_BACK && e->type==DU_EV_ACTIVATE) { a->navigation_pending=1; a->next_page=CP_HOME; return; }
    if (a->display_write && e->type != DU_EV_TICK) return;
    if (internet_event(a,e)) return;
    if (network_event(a, e)) return;
    if (game_event(a,e)) return;
    if (cp_display_event(a, e)) return;
    if (cp_fonts_event(a, e)) return;
    if (appearance_event(a,e)) return;
    if (e->type == DU_EV_TICK && e->id == CP_T_PREVIEW) {   /* deferred wallpaper decode */
        a->preview_pending=2; a->preview_valid=0; du_invalidate(u,(struct du_rect){24,44,208,188});
        return;
    }
    if (e->type==DU_EV_TICK && e->id==CP_T_SAVER) {
        if (a->page==CP_DISPLAY && a->display_tab==CP_SCREEN_SAVER)
            du_invalidate(u,(struct du_rect){208,54,144,108});
        return;
    }
    if (dt_event(a, e)) return;
    if (pw_event(a, e)) return;
    if (hw_event(a, e)) return;
    if (ac_event(a, e)) return;
    switch (e->id) {
    case 950:
        if (e->type==DU_EV_SELECT && e->index>=0 && (unsigned)e->index<a->autoplay_count) a->autoplay_selected=(unsigned)e->index;
        break;
    case 951:
        if (e->type==DU_EV_ACTIVATE && a->autoplay_selected<a->autoplay_count && a->host.autoplay_forget) {
            int result=a->host.autoplay_forget(a->host.ctx,a->autoplay[a->autoplay_selected].id);
            build(a); text(a,C_STATUS,result ? "The choice could not be removed." : "This device will ask every time.");
        }
        break;
    case 952: if (e->type==DU_EV_ACTIVATE) build(a); break;
    case 982:
        if (e->type==DU_EV_SELECT && e->index>=0 && (unsigned)e->index<a->uninstall_count) {
            a->uninstall_selected=(unsigned)e->index;
            struct desk_uninstall_entry *r=&a->uninstall[a->uninstall_selected];
            du_get(u,983)->disabled=r->no_remove || r->system_component || uninstall_locked(a,r);   /* sec-user-04 */
            text(a,985,uninstall_locked(a,r) ? "You must be an administrator to change this setting." : "Protected components cannot be removed.");
        }
        break;
    case 983:
        if (e->type==DU_EV_ACTIVATE && a->uninstall_selected<a->uninstall_count) {
            struct desk_uninstall_entry *r=&a->uninstall[a->uninstall_selected];
            if(r->no_remove || r->system_component){text(a,C_STATUS,"This system component is protected.");break;}
            if(uninstall_locked(a,r)){text(a,C_STATUS,"You must be an administrator to change this setting.");break;}   /* sec-user-04 */
            char prompt[512];
            snprintf(prompt,sizeof(prompt),"Remove %s? Its shortcuts will also be removed.%s",r->name,
                r->kind==DESK_PROGRAM_GAME ? " This bundled game cannot be restored without reinstalling the system." : "");
            static const char *const buttons[]={"Remove","Cancel"};
            a->remove_pending=1;
            du_message(u,987,"Remove program",prompt,DU_DLG_ICON_WARNING,buttons,2,1,1);
        }
        break;
    case 987:
        if(e->type==DU_EV_DIALOG && a->remove_pending){
            a->remove_pending=0;
            if(e->index || a->uninstall_selected>=a->uninstall_count)break;
            struct desk_uninstall_entry *r=&a->uninstall[a->uninstall_selected];
            int result=a->host.program_remove ? a->host.program_remove(a->host.ctx,r) :
                r->no_remove || r->system_component ? -EPERM : a->host.uninstall_run ? a->host.uninstall_run(a->host.ctx,r->key) : -ENOSYS;
            if(a->host.programs_changed)a->host.programs_changed(a->host.ctx);
            build(a);
            char status[96];snprintf(status,sizeof(status),result ? "Removal incomplete: %s. Refresh and try again." : "Program removed.",result ? strerror(-result) : "");
            text(a,C_STATUS,status);
        }
        break;
    case 984: if (e->type==DU_EV_ACTIVATE) build(a); break;
    case 986:
        if(e->type==DU_EV_ACTIVATE)du_file_dialog(u,988,0,"Install a program from removable media or CD-ROM","/", "",NULL,NULL);
        break;
    case 988:
        if(e->type==DU_EV_DIALOG && !e->index && e->text){
            int result=a->host.program_install ? a->host.program_install(a->host.ctx,e->text) : -ENOSYS;
            text(a,C_STATUS,result ? "Select SETUP.EXE, INSTALL.EXE or a .BAT installer on removable media." : "Installer requested.");
        }
        break;
    case C_BACK: if (e->type == DU_EV_ACTIVATE) cp_open(a, CP_HOME); break;
    case J_PROPERTIES: case J_NEXT: case J_CALIBRATE: case J_CAPTURE: case J_ROUTE: case J_OFF: case J_MODE: case J_GAME: case J_SAVE_GAME: case J_DEFAULT_GAME: case J_SECOND: case J_GAME_OFF:
        if (e->type==DU_EV_ACTIVATE) joystick_event(a,e->id);
        break;
    case CP_CATEGORY:
        if (e->type == DU_EV_SELECT && e->index >= 0 && e->index < CP_CATEGORIES) {
            a->category=e->index; build(a); du_focus(u,CP_CATEGORY);
        }
        break;
    case C_APPLETS:
        if (e->type == DU_EV_OPEN && e->index >= 0 && (unsigned)e->index < du_get(u,C_APPLETS)->count) {
            int page=a->applet_pages[e->index];
            if (page==CP_DISPLAY && a->host.open_applet) {
                if (a->host.open_applet(a->host.ctx,page)) text(a,C_STATUS,"Display Properties could not be opened.");
            } else cp_open(a,page);
        }
        break;
    case 976:
        if(e->type==DU_EV_ACTIVATE && a->host.accel_retry){
            int r=a->host.accel_retry(a->host.ctx);
            text(a,975,r==-EPERM?"Only administrators can retry acceleration.": /* sec-sysvc2 */
                       r?"Could not re-arm acceleration; the marker was retained.":"Acceleration will be retried after you restart CiukiOS.");
        }
        break;
    case CP_DISPLAY_TABS_ID:
        if (e->type == DU_EV_SELECT && e->index >= 0 && e->index < CP_DISPLAY_TABS && !a->mode_state) {
            if (a->display_tab != e->index) cp_preview_cancel(a);
            a->display_tab=e->index; build(a); du_focus(u,CP_DISPLAY_TABS_ID);
        }
        break;
    case CP_OK:
        if (e->type == DU_EV_ACTIVATE) {
            a->display_accept=1;
            struct du_event apply={.type=DU_EV_ACTIVATE,.id=CP_APPLY};
            cp_display_event(a,&apply);
            if (a->display_accept && !a->display_dirty && !a->display_write && !a->mode_state) cp_display_accept(a);
        }
        break;
    case CP_CANCEL: if (e->type == DU_EV_ACTIVATE) cp_close(a); break;
    case CP_LOG_PREV: case CP_LOG_NEXT: case CP_OPEN_LOG:
        if (e->type==DU_EV_ACTIVATE) {
            if (e->id==CP_LOG_PREV && a->log_offset>=CP_LOG_BYTES) a->log_offset-=CP_LOG_BYTES;
            if (e->id==CP_LOG_NEXT && a->log_offset+a->log.len<a->log_total) a->log_offset+=CP_LOG_BYTES;
            log_refresh(a);
        }
        break;
    case CP_SAVER_KIND:
        if (e->type==DU_EV_ACTIVATE) {
            struct du_widget *w=du_get(u,CP_SAVER_KIND);
            du_popup(u,a->saver_items,4,w->box.x,w->box.y+w->box.h,CP_SAVER_KIND);
        }
        break;
    case CP_SAVER_NONE: case CP_SAVER_BLANK: case CP_SAVER_LOGO: case CP_SAVER_STARS:
        if (e->type==DU_EV_ACTIVATE) {
            int choice=e->id-CP_SAVER_NONE;
            a->cfg.saver_enabled=choice!=0;
            if (choice) {
                a->cfg.saver_kind=choice-1;
                if (a->cfg.saver_seconds<60) a->cfg.saver_seconds=60;
                if (a->cfg.saver_seconds>86400) a->cfg.saver_seconds=86400;
            }
            save=1; build(a);
        }
        break;
    case CP_SAVER_UP: case CP_SAVER_DOWN:
        if (e->type==DU_EV_ACTIVATE) {
            int value=a->cfg.saver_seconds/60+(e->id==CP_SAVER_UP ? 1 : -1);
            if (value>=1 && value<=1440) { a->cfg.saver_seconds=value*60; save=1; build(a); du_focus(u,CP_SAVER_WAIT); }
        }
        break;
    case CP_SAVER_WAIT:
        if (e->type==DU_EV_COMMIT) {
            char value[16]; snprintf(value,sizeof(value),"%d",a->cfg.saver_seconds/60);
            du_set_text(du_get(u,CP_SAVER_WAIT),value); du_invalidate_widget(u,CP_SAVER_WAIT);
        }
        if (e->type==DU_EV_CHANGE) {
            const char *s=du_get(u,CP_SAVER_WAIT)->text; char *end;
            long value=strtol(s,&end,10);
            if (*s && !*end && value>=1 && value<=1440) { a->cfg.saver_seconds=(int)value*60; save=1; }
        }
        break;
    case CP_MONITOR_WAIT:
        if (e->type==DU_EV_CHANGE && e->index>=0 && e->index<=1440) {
            a->cfg.monitor_seconds=e->index*60; save=1; build(a);
        }
        break;
    case CP_SAVER_PASSWORD:
        if (e->type==DU_EV_CHANGE && accounts_mode(a)) {   /* sec-user-04 */
            if (secure_pref_save(a,DESK_SECURE_SAVER,e->index!=0)) text(a,C_STATUS,"The screen saver setting could not be saved.");
            build(a);
        } else if (e->type==DU_EV_CHANGE && a->user_loaded && !a->user_busy) {
            user_submit(a,USERS_POLICY); build(a);
        }
        break;
    case CP_SAVER_CHANGE:
        if (e->type==DU_EV_ACTIVATE) cp_open(a,CP_PASSWORDS);
        break;
    case CP_SAVER_POWER:
        if (e->type==DU_EV_ACTIVATE && a->host.open_applet &&
            a->host.open_applet(a->host.ctx,CP_POWER))
            text(a,C_STATUS,"Power Management could not be opened.");
        break;
    case CP_SAVER_SETTINGS:
        if (e->type==DU_EV_ACTIVATE)
            du_message(u,CP_SAVER_SETTINGS,"Screen Saver Settings","This screen saver has no additional settings.",DU_DLG_ICON_INFO,NULL,0,0,0);
        break;
    case CP_SAVER_PREVIEW:
        if (e->type==DU_EV_ACTIVATE && a->host.apply &&
            a->host.apply(a->host.ctx,DESK_SETTINGS_SAVER_PREVIEW,a->cfg.saver_kind,0))
            text(a,C_STATUS,"Screen saver preview could not be started.");
        break;
    case CP_POWER_ACTION:
        if (e->type == DU_EV_ACTIVATE && a->host.power && a->host.power(a->host.ctx))
            text(a,C_STATUS,"The shutdown dialog could not be opened.");
        break;
    case C_DRIVERS: case C_LEASES:
        if (e->type == DU_EV_SELECT || e->type == DU_EV_OPEN) {
            a->detail_list = e->id; a->detail_index = e->index < 0 ? 0 : (unsigned)e->index;
            if (e->type == DU_EV_OPEN) details(a);
        }
        break;
    case C_DETAILS: if (e->type == DU_EV_ACTIVATE) details(a); break;
    case C_SYSTEM_TABS:
        if (e->type == DU_EV_SELECT && e->index >= 0 && e->index < 6) {
            a->system_tab = e->index;
            cp_open(a,e->index==1 ? CP_DEVICES : e->index==3 ? CP_DRIVERS : CP_SYSTEM);
        }
        break;
    case CP_STARTUP_LIST:
        if (e->type==DU_EV_SELECT && e->index>=0 && e->index<a->startup_count) {
            a->startup_selected=e->index;
            du_get(u,CP_STARTUP_ENABLED)->value=a->startup[e->index].enabled;
            du_invalidate_widget(u,CP_STARTUP_ENABLED);
        }
        break;
    case CP_STARTUP_ENABLED:
        if (e->type==DU_EV_CHANGE && a->startup_selected>=0 && a->startup_selected<a->startup_count) {
            struct desk_startup_entry *item=&a->startup[a->startup_selected];
            int enabled=!!du_get(u,CP_STARTUP_ENABLED)->value;
            int error=a->host.startup_enable ? a->host.startup_enable(a->host.ctx,item,enabled) : -ENOSYS;
            if (!error) item->enabled=enabled;
            du_get(u,CP_STARTUP_ENABLED)->value=item->enabled;
            du_invalidate_widget(u,CP_STARTUP_ENABLED);
            a->startup_items[a->startup_selected].text[2]=item->enabled ? (item->error ? "Invalid" : "Enabled") : "Disabled";
            du_invalidate(u,du_get(u,CP_STARTUP_LIST)->box);
            text(a,CP_STARTUP_NOTE,error ? "Startup setting could not be saved." : "Saved. Changes take effect at the next desktop start.");
        }
        break;
    case CP_STARTUP_REFRESH:
        if (e->type==DU_EV_ACTIVATE) build(a);
        break;
    case C_REFRESH: if (e->type == DU_EV_ACTIVATE) { refresh(a); build(a); } break;
    case C_TREE:
        if (e->type == DU_EV_SELECT && e->index >= 0 && (unsigned)e->index < a->tree_count) {
            int index = a->tree_device[e->index];
            a->selected_device_id = index >= 0 ? a->devices.devices[index].id : 0;
            du_get(u, C_PROPERTIES)->disabled = !a->devices_ok || index < 0;
            dm_buttons(a);
            cp_pack_buttons(a);
        }
        if (e->type == DU_EV_OPEN && e->index >= 0 && (unsigned)e->index < a->tree_count) {
            int device = a->tree_device[e->index];
            if (device < 0) { a->collapsed ^= 1u << (unsigned)(-device - 1); cp_tree_build(a); }
            else { a->properties_device = device; properties(a, 0); }
        }
        break;
    case C_PROPERTIES:
        if (e->type == DU_EV_ACTIVATE) {
            struct du_widget *tree = du_get(u, C_TREE);
            if (tree && tree->active < a->tree_count && a->tree_device[tree->active] >= 0) {
                a->properties_device = a->tree_device[tree->active]; properties(a, 0);
            }
        }
        break;
    case D_PROPERTIES: if (e->type == DU_EV_DIALOG && e->index < 2) properties(a, e->index == 1); break;
    case C_FIRMWARE:
        if (e->type == DU_EV_ACTIVATE) { memset(&a->firmware, 0, sizeof(a->firmware)); firmware_show(a, 0); }
        break;
    case D_FIRMWARE:
        if (e->type == DU_EV_DIALOG && e->index < 2)
            firmware_show(a, e->index == 0 ? (a->firmware_index ? a->firmware_index - 1 : 0) :
                          a->firmware_index + 1 < a->firmware.total ? a->firmware_index + 1 : a->firmware_index);
        break;
    case C_SERIAL: if (e->type == DU_EV_CHANGE) a->serial_next = du_get(u, C_SERIAL)->value != 0; break;
    case C_BOOT_APPLY:
        if (e->type == DU_EV_ACTIVATE) {
            char current[CP_BOOT_MAX], out[CP_BOOT_MAX]; size_t n = 0;
            if (!a->host.boot_load || a->host.boot_load(a->host.ctx, current, sizeof(current), &n) ||
                cp_boot_serial(current, n, a->serial_next, out, sizeof(out)) || !a->host.boot_save ||
                a->host.boot_save(a->host.ctx, out, strlen(out))) text(a, C_STATUS, "BOOT.CFG could not be saved.");
            else text(a, C_STATUS, "Saved. Serial logging changes at the next boot.");
        }
        break;
    case C_CLOSE: if (e->type == DU_EV_ACTIVATE) cp_close(a); break;
    case 4133:
        if(e->type==DU_EV_ACTIVATE) advanced_open(a);
        break;
    case 4132:
        if(e->type==DU_EV_ACTIVATE) cp_open(a,CP_FONTS);
        break;
    case 4130:
        if(e->type==DU_EV_ACTIVATE) du_file_dialog(u,4131,0,"Choose a desktop background","/system/wallpapers","",NULL,NULL);
        break;
    case 4131:
        if(e->type==DU_EV_DIALOG && !e->index) {
            if(e->text && strlen(e->text)<sizeof(a->cfg.wallpaper) && wallpaper_path(e->text)) {
                snprintf(a->cfg.wallpaper,sizeof(a->cfg.wallpaper),"%s",e->text); a->preview_valid=0; save=1;
                text(a,C_STATUS,"Wallpaper selected. Apply to publish it on the desktop.");
            } else text(a,C_STATUS,"Choose a PNG, JPEG or BMP image.");
        }
        break;
    case C_THEME:
        if (e->type == DU_EV_SELECT && e->index >= 0 && e->index < CFG_THEMES) {
            snprintf(a->cfg.theme, sizeof(a->cfg.theme), "%s", cfg_theme_names[e->index]); save = 1; a->preview_valid = 0;
        }
        break;
    case C_WALL:
        if (e->type == DU_EV_SELECT && e->index >= 0) {
            unsigned k = (unsigned)e->index;
            if (k == 0) strcpy(a->cfg.wallpaper, "none");
            else if (k == 1) strcpy(a->cfg.wallpaper, "ciuki");
            else if (k < 2 + DESK_BUILTIN_WALLPAPERS) snprintf(a->cfg.wallpaper, sizeof(a->cfg.wallpaper), "builtin:%s", desk_builtin_wallpapers[k - 2].id);
            else if (k < 2 + DESK_BUILTIN_WALLPAPERS + a->wall_count) {
                snprintf(a->cfg.wallpaper, sizeof(a->cfg.wallpaper), "%s", a->wall_names[k - 2 - DESK_BUILTIN_WALLPAPERS]);
            } else if (k < 2 + DESK_BUILTIN_WALLPAPERS + a->wall_count + 8) {
                snprintf(a->cfg.wallpaper,sizeof(a->cfg.wallpaper),"pattern:%u",k-2-DESK_BUILTIN_WALLPAPERS-a->wall_count);
            } else break;
            save = 1; a->preview_valid = 0;
        }
        break;
    case CP_WALL_PATTERN:
        if (e->type==DU_EV_SELECT && e->index>=0 && e->index<DESK_WALL_PATTERNS) {
            a->cfg.wallpaper_pattern=e->index; save=1; a->preview_valid=0;
        }
        break;
    case C_POS0: case C_POS1: case C_POS2: case C_POS3: case C_POS4:
        if (e->type == DU_EV_ACTIVATE) { a->cfg.wallpaper_mode = mode_order[e->id - C_POS0]; save = 1; a->preview_valid = 0; }
        break;
    case C_COL0: case C_COL1: case C_COL2: case C_COL3: case C_COL4: case C_COL5:
        if (e->type == DU_EV_ACTIVATE) { a->cfg.desktop_color = swatches[e->id - C_COL0]; save = 1; a->preview_valid = 0; }
        break;
    case C_RGB_R: case C_RGB_G: case C_RGB_B:
        if (e->type == DU_EV_CHANGE && e->index >= 0 && e->index <= 255) {
            unsigned shift=(unsigned)(C_RGB_B-e->id)*8u;
            a->cfg.desktop_color=(a->cfg.desktop_color&~(0xffu<<shift))|((uint32_t)e->index<<shift);
            save=1; a->preview_valid=0;
        }
        break;
    case C_ICONS: if (e->type == DU_EV_ACTIVATE) { a->cfg.desktop_icons = !a->cfg.desktop_icons; save = 1; } break;
    case C_REPEAT_DELAY: if(e->type==DU_EV_CHANGE) {a->cfg.repeat_delay=e->index;save=1;} break;
    case C_REPEAT_SPEED: if(e->type==DU_EV_CHANGE) {a->cfg.repeat_speed=e->index;save=1;} break;
    case C_BLINK: if(e->type==DU_EV_CHANGE) {a->cfg.cursor_blink_ms=e->index*100;save=1;} break;
    case C_SWAP: if(e->type==DU_EV_CHANGE) {a->cfg.mouse_swap=e->index!=0;save=1;} break;
    case C_TRAILS: if(e->type==DU_EV_CHANGE) {a->cfg.mouse_trails=e->index;save=1;} break;
    case C_WHEEL: if(e->type==DU_EV_CHANGE) {a->cfg.wheel_lines=e->index;save=1;} break;
    case C_LAYOUT_SWITCH: if(e->type==DU_EV_CHANGE) {a->cfg.layout_switch=e->index!=0;save=1;} break;
    case C_POINTER: if(e->type==DU_EV_ACTIVATE) {a->cfg.pointer_scheme=(a->cfg.pointer_scheme+1)%3;save=1;} break;
    case C_DOUBLE_TEST: if(e->type==DU_EV_ACTIVATE) {
        static uint64_t last; uint64_t now=a->ui->now;
        text(a,C_DOUBLE_TEST,last && now>=last && now-last<=(uint64_t)a->cfg.double_click_ms*1000000 ? "Double click detected" : "Click again to test"); last=now;
    } break;
    case C_SPEED: if (e->type == DU_EV_CHANGE) { a->cfg.mouse_speed = CFG_MOUSE_MIN + e->index; a->cfg.mouse_spi_speed=a->cfg.mouse_speed*2; save = 1; } break;
    case C_DBL: if (e->type == DU_EV_CHANGE) { a->cfg.double_click_ms = 200 + e->index * 100; save = 1; } break;
    case C_KEY: if (e->type == DU_EV_SELECT && e->index >= 0 && e->index < CFG_KEYS_COUNT) { a->cfg.keyboard = e->index; save = 1; } break;
    case C_VOL: case C_MUTE:
        if ((e->id == C_VOL && e->type == DU_EV_CHANGE) || (e->id == C_MUTE && e->type == DU_EV_ACTIVATE)) {
            if (e->id == C_VOL) a->volume = e->index; else a->muted = !a->muted;
            int r = a->host.apply ? a->host.apply(a->host.ctx, DESK_SETTINGS_MASTER, a->volume, a->muted) : -1;
            text(a, C_STATUS, r ? "The volume could not be changed." : "Volume applied.");
        }
        break;
    case C_SOUND_LIST:
        if (e->type == DU_EV_SELECT && e->index >= 0 && e->index < SOUND_EVENTS) a->sound_selected = (unsigned)e->index;
        break;
    case C_SOUND_TEST: case C_HARDWARE:
        if (e->type == DU_EV_ACTIVATE) {
            if (e->id == C_HARDWARE) a->audio_before_ok = a->host.audio_diagnostics &&
                !a->host.audio_diagnostics(a->host.ctx, &a->audio_before) &&
                a->audio_before.version == 1 && a->audio_before.size == sizeof(a->audio_before);
            int r = a->host.apply ? a->host.apply(a->host.ctx,e->id == C_HARDWARE ? DESK_SETTINGS_AUDIO_TEST : DESK_SETTINGS_SOUND_TEST,
                                               e->id == C_HARDWARE ? 0 : (int)a->sound_selected,0) : -1;
            if (!r && e->id == C_HARDWARE) a->audio_testing = 1;
            if (r && e->id == C_HARDWARE) { cp_audio_result(a, r); break; }
            text(a,C_STATUS,r ? "Audio test could not start." : e->id == C_HARDWARE ? "Testing audio hardware..." : "Playing selected sound.");
        }
        break;
    case C_MM_CD:
        if (e->type==DU_EV_CHANGE && a->page==CP_MULTIMEDIA && a->host.audio_line) {
            unsigned volume=(unsigned)du_get(u,C_MM_CD)->value;
            struct ciuki_mixer_line line={.version=1,.size=sizeof(line),.line=CIUKI_MIXER_CD,.left=volume,.right=volume};
            int error=a->host.audio_line(a->host.ctx,&line,1);
            text(a,C_STATUS,error ? "CD volume could not be changed." : "CD volume requested.");
        }
        break;
    case C_MM_VOLUME: case C_MM_RECORDER: case C_MM_MUSIC:
        if (e->type==DU_EV_ACTIVATE) {
            int error=a->host.multimedia_open ? a->host.multimedia_open(a->host.ctx,(unsigned)(e->id-C_MM_VOLUME)) : -ENOSYS;
            text(a,C_STATUS,error ? "The multimedia program could not be opened." : "Program requested.");
        }
        break;
    case C_SOUND_BROWSE:
        if (e->type==DU_EV_ACTIVATE)
            du_file_dialog(u,D_SOUND_BROWSE,0,"Choose a PCM WAV sound (22050 Hz, mono, 16-bit, up to 2 seconds)",SOUND_DIR,"",NULL,NULL);
        break;
    case C_SOUND_NONE: case D_SOUND_BROWSE:
        if ((e->id==C_SOUND_NONE && e->type==DU_EV_ACTIVATE) ||
            (e->id==D_SOUND_BROWSE && e->type==DU_EV_DIALOG && !e->index && e->text)) {
            const char *path=e->id==C_SOUND_NONE ? "" : e->text;
            struct sound_scheme next=a->sounds;
            if (sound_assign(&next,a->sound_selected,path) || (path[0] &&
                (!a->host.sound_validate || a->host.sound_validate(a->host.ctx,path)))) {
                text(a,C_STATUS,"Cannot assign this sound: choose a supported PCM WAV file."); break;
            }
            if (!sound_commit(a,&next)) a->sound_named[0]=0;
        }
        break;
    case C_SCHEME_SAVE:
        if (e->type==DU_EV_ACTIVATE) du_input_box(u,D_SCHEME_SAVE,"Save sound scheme","Name (letters, digits, hyphen or underscore):",a->sound_named);
        break;
    case D_SCHEME_SAVE:
        if (e->type==DU_EV_DIALOG && !e->index && e->text) {
            if (!a->host.sound_scheme || a->host.sound_scheme(a->host.ctx,SOUND_SCHEME_SAVE,e->text,&a->sounds))
                text(a,C_STATUS,"Scheme could not be saved. Choose a valid custom name.");
            else { snprintf(a->sound_named,sizeof(a->sound_named),"%s",e->text); build(a); text(a,C_STATUS,"Named scheme saved."); }
        }
        break;
    case C_SCHEME_DELETE:
        if (e->type==DU_EV_ACTIVATE && a->sound_named[0]) {
            if (!a->host.sound_scheme || a->host.sound_scheme(a->host.ctx,SOUND_SCHEME_DELETE,a->sound_named,&a->sounds))
                text(a,C_STATUS,"Scheme could not be deleted.");
            else { a->sound_named[0]=0; build(a); text(a,C_STATUS,"Named scheme deleted; active sounds retained."); }
        }
        break;
    case C_SOUND_ENABLED: case C_SCHEME: case C_UI_VOL:
        if ((e->id == C_SCHEME && e->type == DU_EV_SELECT && e->index >= 0 && (unsigned)e->index < 3+a->sound_name_count) ||
            (e->id == C_UI_VOL && e->type == DU_EV_CHANGE && e->index >= 0 && e->index <= 100) ||
            (e->id == C_SOUND_ENABLED && (e->type == DU_EV_CHANGE || e->type == DU_EV_ACTIVATE))) {
            struct sound_scheme next = a->sounds;
            if (e->id == C_SCHEME) {
                if (!e->index) { int volume = next.volume; sound_defaults(&next); next.volume = volume; }
                else if (e->index==1) next.enabled = 0;
                else if (e->index==2) break;
                else if (!a->host.sound_scheme || a->host.sound_scheme(a->host.ctx,SOUND_SCHEME_LOAD,a->sound_names[e->index-3],&next)) {
                    text(a,C_STATUS,"Scheme could not be loaded."); break;
                }
            } else if (e->id == C_UI_VOL) next.volume = e->index;
            else next.enabled = !next.enabled;
            if (!sound_commit(a,&next)) {
                if (e->id==C_SCHEME && e->index>=3)
                    snprintf(a->sound_named,sizeof(a->sound_named),"%s",a->sound_names[e->index-3]);
                else a->sound_named[0]=0;
            }
        }
        break;
    case C_H24: if (e->type == DU_EV_ACTIVATE) { a->cfg.clock_24h = !a->cfg.clock_24h; save = 1; cp_tick(a); } break;
    case C_SECONDS: if (e->type == DU_EV_ACTIVATE) { a->cfg.clock_seconds = !a->cfg.clock_seconds; save = 1; cp_tick(a); } break;
    case C_TASKBAR_TOP: if (e->type==DU_EV_CHANGE || e->type==DU_EV_ACTIVATE) { a->cfg.taskbar_top=!a->cfg.taskbar_top;save=1; }break;
    case C_TASKBAR_AUTOHIDE: if (e->type==DU_EV_CHANGE || e->type==DU_EV_ACTIVATE) { a->cfg.taskbar_autohide=!a->cfg.taskbar_autohide;save=1; }break;
    case C_START_SMALL: if (e->type==DU_EV_CHANGE || e->type==DU_EV_ACTIVATE) { a->cfg.start_small=!a->cfg.start_small;save=1; }break;
    case C_TASKBAR_CLOCK: if (e->type == DU_EV_CHANGE || e->type == DU_EV_ACTIVATE) { a->cfg.taskbar_clock = !a->cfg.taskbar_clock; save = 1; } break;
    case C_QUICK_LAUNCH: if (e->type == DU_EV_CHANGE || e->type == DU_EV_ACTIVATE) { a->cfg.quick_launch = !a->cfg.quick_launch; save = 1; } break;
    default: break;
    }
    if (save && a->page == CP_DISPLAY) {
        a->cfg.appearance.item[AP_DESKTOP].colour=a->cfg.desktop_color;
        a->display_dirty = 1;
        text(a,C_STATUS,"Choose Apply to preview, OK to keep, or Cancel to restore.");
    } else if (save) changed(a);
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
    unsigned file_start = 2 + DESK_BUILTIN_WALLPAPERS;
    if (k >= file_start + a->wall_count) {
        static const char *const names[] = {"Pattern: dots", "Pattern: sparse", "Pattern: checker",
            "Pattern: diagonal /", "Pattern: diagonal \\", "Pattern: horizontal", "Pattern: vertical", "Pattern: lattice"};
        return names[k - file_start - a->wall_count];
    }
    const char *path = a->wall_names[k - file_start], *base = strrchr(path, '/');
    return base ? base + 1 : path;
}
/* Device inventory validation precedes every use of counted arrays/strings. */
int cp_snapshot_valid(const struct ciuki_devices_info *s)
{
    if (s->version != CIUKI_DEVICES_VERSION || s->size != sizeof(*s) || s->count > CIUKI_DEVICES_MAX ||
        s->driver_count > CIUKI_DRIVERS_MAX || s->lease_count > CIUKI_LEASES_MAX ||
        !memchr(s->cpu_vendor, 0, sizeof(s->cpu_vendor)) || !memchr(s->cpu_brand, 0, sizeof(s->cpu_brand)) ||
        !memchr(s->boot_options, 0, sizeof(s->boot_options))) return 0;
    for (unsigned i = 0; i < s->count; i++) {
        const struct ciuki_device_record *d = &s->devices[i];
        if (d->class_id >= CIUKI_DEVICE_CLASSES || d->state > CIUKI_DEVICE_FIRMWARE_FIRST || d->bus_type > CIUKI_BUS_USB ||
            d->resource_count > CIUKI_DEVICE_RESOURCES || !memchr(d->name, 0, sizeof(d->name)) ||
            !memchr(d->driver, 0, sizeof(d->driver)) || !memchr(d->reason, 0, sizeof(d->reason)) ||
            !memchr(d->driver_display_name,0,sizeof(d->driver_display_name)) ||
            d->accel_ready>1 || d->native_used>1 || d->cpu_ready>1) return 0;
        for (unsigned j = 0; j < d->resource_count; j++) {
            const struct ciuki_device_resource *r = &d->resources[j];
            if (r->type > CIUKI_RESOURCE_MEMORY || r->end < r->start ||
                (!(r->flags & CIUKI_RESOURCE_UNKNOWN_SIZE) && r->end == r->start)) return 0;
        }
    }
    for (unsigned i = 0; i < s->driver_count; i++) {
        const struct ciuki_driver_record *r = &s->drivers[i];
        if (!memchr(r->name, 0, sizeof(r->name)) || !memchr(r->build, 0, sizeof(r->build)) ||
            !memchr(r->policy, 0, sizeof(r->policy)) || r->state > CIUKI_DEVICE_FIRMWARE_FIRST) return 0;
    }
    for (unsigned i = 0; i < s->lease_count; i++) {
        const struct ciuki_device_lease *r = &s->leases[i];
        if (!memchr(r->owner, 0, sizeof(r->owner)) || r->type > CIUKI_RESOURCE_MEMORY || r->state > 6 || r->end <= r->start) return 0;
    }
    return 1;
}
const char *cp_device_class(unsigned k)
{
    static const char *const names[] = {"System devices", "Display adapters", "Keyboards", "Mice", "Disk drives",
        "CD-ROM drives", "Sound", "Network adapters", "Ports"};
    return k < CIUKI_DEVICE_CLASSES ? names[k] : "Unknown devices";
}
const char *cp_device_state(unsigned k)
{
    static const char *const names[] = {"Ready", "Absent", "Quarantined", "Firmware-first"};
    return k < 4 ? names[k] : "Unknown";
}
static const char *class_icon(unsigned k)
{
    static const char *const names[] = {"settings:system", "device:display", "device:keyboard", "device:mouse",
        "device:harddisk", "device:cdrom", "device:sound-card", "device:network", "utilities-terminal"};
    return k < CIUKI_DEVICE_CLASSES ? names[k] : "settings:system";
}
int cp_hwid_lookup_json(const char *json, unsigned vendor, unsigned device, char *out, size_t size)
{
    if (!json || !out || size < 2) return -1;
    char key[16]; snprintf(key, sizeof(key), "\"%04x:%04x\"", vendor & 0xffffu, device & 0xffffu);
    const char *entry = strstr(json, key);
    if (!entry || !(entry = strchr(entry + strlen(key), ':'))) return -1;
    while (*++entry == ' ' || *entry == '\t' || *entry == '\r' || *entry == '\n') {}
    if (*entry++ != '"') return -1;
    size_t n = 0;
    while (*entry && *entry != '"' && n + 1 < size && n < 64) out[n++] = *entry++;
    if (*entry != '"' || !n) return -1;
    out[n] = 0;
    return 0;
}
static const char *product(struct cp_app *a, const struct ciuki_device_record *d, char *resolved, size_t size)
{
    if (d->bus_type == CIUKI_BUS_PCI) {
        if (a->host.hwid_name && !a->host.hwid_name(a->host.ctx, d->vendor, d->device, resolved, size) && resolved[0])
            return resolved;
        if (d->pci_class == 1 && d->subclass == 1) return "PCI IDE controller";
        if (d->pci_class == 6) return "PCI bridge";
        if (d->pci_class == 0x0c && d->subclass == 3) return "USB controller";
        return cp_device_class(d->class_id);
    }
    if (d->bus_type == CIUKI_BUS_USB && a->host.usb_hwid_name &&
        !a->host.usb_hwid_name(a->host.ctx, d->vendor, d->device, resolved, size) && resolved[0]) return resolved;
    return d->name;
}
void cp_tree_build(struct cp_app *a)
{
    struct du_widget *w = du_get(a->ui, C_TREE);
    int old = -1;
    for (unsigned k = 0; a->devices_ok && k < a->devices.count; k++)
        if (a->devices.devices[k].id == a->selected_device_id) { old = (int)k; break; }
    a->tree_count = 1;
    memset(a->tree_items, 0, sizeof(a->tree_items));
    a->tree_items[0].text[0] = "Computer"; a->tree_items[0].children = 1;
    a->tree_items[0].expanded = !(a->collapsed & 1); a->tree_device[0] = -1;
    a->tree_items[0].icon16 = du_icon_argb(DU_ICON_CIUKI, 16);
    if (!(a->collapsed & 1)) for (unsigned cls = 0; cls < CIUKI_DEVICE_CLASSES; cls++) {
        unsigned row = a->tree_count++;
        struct du_item *it = &a->tree_items[row];
        it->text[0] = cp_device_class(cls); it->depth = 1;
        it->expanded = !(a->collapsed & (2u << cls));
        it->icon16 = du_icon_argb(du_icon_name(class_icon(cls)), 16);
        a->tree_device[row] = -(int)cls - 2;
        if (!a->devices_ok) continue;
        for (unsigned k = 0; k < a->devices.count; k++) {
            const struct ciuki_device_record *d = &a->devices.devices[k];
            if (d->class_id != cls) continue;
            it->children = 1;
            if (!it->expanded) continue;
            unsigned n = a->tree_count++;
            char resolved[65];
            enum dc_status ds = dc_status_of(hw_devices(a), d->id);
            snprintf(a->device_names[k], sizeof(a->device_names[k]), "%s%s%s", product(a, d, resolved, sizeof(resolved)),
                     d->state == CIUKI_DEVICE_QUARANTINED ? " (quarantined)" : d->state == CIUKI_DEVICE_ABSENT ? " (absent)" : "",
                     ds == DC_DISABLED ? " (disabled)" : ds == DC_REMOVED ? " (removed)" : "");
            a->tree_items[n].text[0] = a->device_names[k]; a->tree_items[n].depth = 2;
            a->tree_items[n].icon16 = du_icon_argb(du_icon_name(class_icon(cls)), 16); a->tree_device[n] = (int)k;
        }
    }
    if (w) {
        w->items = a->tree_items; w->count = a->tree_count; w->active = 0;
        for (unsigned k = 0; k < a->tree_count; k++) if (a->tree_device[k] == old) { w->active = k; break; }
        du_list_ensure(w, w->active);
    }
    struct du_widget *properties_button = du_get(a->ui, C_PROPERTIES);
    if (properties_button) properties_button->disabled = !a->devices_ok || !w || w->active >= a->tree_count || a->tree_device[w->active] < 0;
    cp_pack_buttons(a);
    dm_buttons(a);
    a->ui->dirty = 1;
}
int cp_boot_serial(const char *src, size_t n, int serial, char *out, size_t cap)
{
    if (!src || !out || n >= CP_BOOT_MAX || cap < 1) return -1;
    for (size_t k = 0; k < n; k++) if (!src[k] || (unsigned char)src[k] > 126 ||
        ((unsigned char)src[k] < 32 && src[k] != '\n' && src[k] != '\r' && src[k] != '\t')) return -1;
    size_t at = 0; int found = 0;
    for (size_t k = 0; k < n;) {
        size_t end = k;
        if (src[k] == ' ' || src[k] == '\r' || src[k] == '\n' || src[k] == '\t') end++;
        else while (end < n && src[end] != ' ' && src[end] != '\r' && src[end] != '\n' && src[end] != '\t') end++;
        if (end - k >= 7 && !memcmp(src + k, "serial=", 7)) {
            if (at + 8 >= cap) return -1;
            memcpy(out + at, "serial=", 7); out[at + 7] = serial ? '1' : '0'; at += 8; found = 1;
        } else {
            if (at + end - k >= cap) return -1;
            memcpy(out + at, src + k, end - k); at += end - k;
        }
        k = end;
    }
    if (!found) {
        if (at && out[at - 1] != ' ' && out[at - 1] != '\n' && out[at - 1] != '\r' && out[at - 1] != '\t') {
            if (at + 1 >= cap) return -1;
            out[at++] = ' ';
        }
        if (at + 8 >= cap) return -1;
        memcpy(out + at, "serial=", 7); out[at + 7] = serial ? '1' : '0'; at += 8;
    }
    if (at >= CP_BOOT_MAX) return -1;
    out[at] = 0; return 0;
}
static void refresh(struct cp_app *a)
{
    a->devices.version = CIUKI_DEVICES_VERSION; a->devices.size = sizeof(a->devices);
    a->devices_ok = a->host.devices && !a->host.devices(a->host.ctx, &a->devices) && cp_snapshot_valid(&a->devices);
    a->snapshot_time = a->host.monotonic ? a->host.monotonic(a->host.ctx) : a->ui->now;
    snprintf(a->status, sizeof(a->status), "%s", !a->devices_ok ? "Device information is unavailable." :
             a->devices.flags & CIUKI_DEVICES_TRUNCATED ? "Inventory limit reached; some devices are omitted." : "Read-only device inventory.");
}
static const char *resource_name(unsigned type)
{
    static const char *const names[] = {"I/O", "IRQ", "DMA", "Memory"};
    return type < 4 ? names[type] : "Unknown";
}
static void firmware_show(struct cp_app *a, uint32_t index)
{
    struct ciuki_firmware_page *p = &a->firmware;
    uint32_t gen = p->generation;
    memset(p, 0, sizeof(*p)); p->version = CIUKI_FIRMWARE_VERSION; p->size = sizeof(*p);
    p->cursor = index / CIUKI_FIRMWARE_PAGE_RECORDS * CIUKI_FIRMWARE_PAGE_RECORDS; p->generation = gen;
    char body[DU_DIALOG_TEXT];
    int valid = a->host.firmware && !a->host.firmware(a->host.ctx, p) &&
                p->version == CIUKI_FIRMWARE_VERSION && p->size == sizeof(*p) && p->count <= 4 &&
                p->total <= CIUKI_DEVICES_MAX && p->cursor <= index && index - p->cursor < p->count;
    const struct ciuki_firmware_record *r = valid ? &p->records[index - p->cursor] : NULL;
    if (r && (r->cid_count > 4 || r->resource_count > 8 || !memchr(r->path, 0, sizeof(r->path)) ||
              !memchr(r->hid, 0, sizeof(r->hid)) || !memchr(r->reason, 0, sizeof(r->reason)))) valid = 0;
    if (r) for (unsigned i = 0; valid && i < r->cid_count; i++)
        if (!memchr(r->cid[i], 0, sizeof(r->cid[i]))) valid = 0;
    if (!valid) {
        snprintf(body, sizeof(body), "%s", !a->host.firmware ? "Firmware inventory is unavailable." :
                 p->flags & CIUKI_FIRMWARE_PENDING ? "Firmware inventory is still being collected. Reopen to refresh." :
                 !p->total ? "No firmware devices recorded. Firmware data may be absent or invalid." :
                 "Firmware inventory changed or is invalid. Close and reopen to refresh.");
    } else {
        a->firmware_index = index;
        size_t at = (size_t)snprintf(body, sizeof(body), "%u of %u  %s\n%s\nID %u  Parent %u  HID %s\n%s\n%s\n",
            index + 1, p->total, r->origin == CIUKI_FIRMWARE_ACPI ? "ACPI" : "PnP BIOS", r->path, r->id, r->parent,
            r->hid[0] ? r->hid : "unknown", r->state == CIUKI_FIRMWARE_ROUTING_UNQUALIFIED ? "Interrupt routing unqualified" :
            r->state == CIUKI_FIRMWARE_UNSUPPORTED ? "Unsupported or incomplete" : "Discovered", r->reason);
        for (unsigned i = 0; i < r->cid_count && at + 64 < sizeof(body); i++)
            at += (size_t)snprintf(body + at, sizeof(body) - at, "CID %s\n", r->cid[i]);
        static const char *const caps[] = {"button", "lid", "AC", "battery", "thermal", "power", "wake", "EC"};
        at += (size_t)snprintf(body + at, sizeof(body) - at, "Described capabilities:");
        for (unsigned i = 0; i < 8 && at + 16 < sizeof(body); i++) if (r->capabilities & (1u << i))
            at += (size_t)snprintf(body + at, sizeof(body) - at, " %s", caps[i]);
        if (at + 96 < sizeof(body)) at += (size_t)snprintf(body + at, sizeof(body) - at, "\nCurrent power values unknown; no takeover.\n");
        for (unsigned i = 0; i < r->resource_count && at + 80 < sizeof(body); i++) {
            const struct ciuki_firmware_resource *res = &r->resources[i];
            at += (size_t)snprintf(body + at, sizeof(body) - at, "%s %llX..%llX (end exclusive)\n", resource_name(res->type),
                (unsigned long long)res->start, (unsigned long long)res->end);
        }
        if (at + 64 < sizeof(body) && (p->flags || r->flags))
            snprintf(body + at, sizeof(body) - at, "Inventory incomplete or truncated; unlisted facts are unknown.");
    }
    static const char *const buttons[] = {"Previous", "Next", "Close"};
    du_message(a->ui, D_FIRMWARE, "Firmware devices", body, DU_DLG_ICON_INFO, buttons, 3, 2, 2);
}
static const char *lease_state(unsigned state)
{
    static const char *const names[] = {"Firmware", "Claimed", "Active", "Quiescing", "Released", "Quarantined", "Discovered"};
    return state < 7 ? names[state] : "Unknown";
}
static void range(char *text, size_t cap, unsigned type, uint64_t start, uint64_t end, unsigned flags)
{
    if (type == CIUKI_RESOURCE_IRQ || type == CIUKI_RESOURCE_DMA) snprintf(text, cap, "%llu", (unsigned long long)start);
    else if (flags & CIUKI_RESOURCE_UNKNOWN_SIZE) snprintf(text, cap, "0x%llX (size unknown)", (unsigned long long)start);
    else snprintf(text, cap, "0x%llX - 0x%llX", (unsigned long long)start, (unsigned long long)(end - 1));
}
static void properties(struct cp_app *a, int resources)
{
    if (!a->devices_ok || a->properties_device < 0 || (unsigned)a->properties_device >= a->devices.count) return;
    const struct ciuki_device_record *d = &a->devices.devices[a->properties_device];
    char body[DU_DIALOG_TEXT], title[128]; size_t at = 0;
    char resolved_title[65];
    hw_ready(a);
    enum dc_status ds = dc_status_of(hw_devices(a), d->id);
    snprintf(title, sizeof(title), "%s Properties", product(a, d, resolved_title, sizeof(resolved_title)));
    if (!resources) {
        char resolved[65]; const char *name = product(a, d, resolved, sizeof(resolved));
        at = (size_t)snprintf(body, sizeof(body), "General\n\n%s\nClass: %s\nDriver: %s\nStatus: %s\n",
            name, cp_device_class(d->class_id), d->driver_display_name[0] ? d->driver_display_name :
                d->driver[0] ? d->driver : "None bound", cp_device_state(d->state));
        const char *reason = d->reason;
        while (*reason) {
            size_t n = strlen(reason), cut = n > 52 ? 52 : n;
            if (cut < n) { while (cut > 1 && reason[cut] != ' ') cut--; if (cut == 1) cut = 52; }
            at += (size_t)snprintf(body + at, sizeof(body) - at, "%.*s\n", (int)cut, reason);
            reason += cut; while (*reason == ' ') reason++;
        }
        if (d->bus_type == CIUKI_BUS_PCI) at += (size_t)snprintf(body + at, sizeof(body) - at,
            "PCI %02X:%02X.%u   ID %04X:%04X\nSubsystem %04X:%04X   Revision %02X\nClass %02X:%02X:%02X\n",
            d->bus, d->slot, d->function, d->vendor, d->device, d->sub_vendor, d->sub_device, d->revision, d->pci_class, d->subclass, d->prog_if);
        else at += (size_t)snprintf(body + at, sizeof(body) - at, "Bus: %s   Channel/unit: %u/%u\n",
            d->bus_type == CIUKI_BUS_LOADER ? "Loader" : d->bus_type == CIUKI_BUS_USB ? "USB" : "Legacy", d->slot, d->function);
        at += (size_t)snprintf(body + at, sizeof(body) - at, "Activation sequence: %u   Generation: %u\n", d->activation_seq, d->generation);
        if (at >= sizeof(body)) at = sizeof(body) - 1;
        if (ds == DC_DISABLED) at += (size_t)snprintf(body + at, sizeof(body) - at, "This device is disabled.\n");
        else if (ds == DC_REMOVED) at += (size_t)snprintf(body + at, sizeof(body) - at, "(removed, detected again after a restart)\n");
        if (at >= sizeof(body)) at = sizeof(body) - 1;
        if (dc_driver_override(hw_devices(a), d->id)) at += (size_t)snprintf(body + at, sizeof(body) - at, "Driver update pending: %s (loaded after a restart)\n", dc_driver_override(hw_devices(a), d->id));
        if (at >= sizeof(body)) at = sizeof(body) - 1;
        snprintf(body + at, sizeof(body) - at, "Hardware profile: %s", a->hp.p[a->hp.active].name);
    } else {
        char resolved[65]; const char *name = product(a, d, resolved, sizeof(resolved));
        at = (size_t)snprintf(body, sizeof(body), "Resources\n\n%s\n", name);
        for (unsigned k = 0; k < dc_resource_count(hw_devices(a), d) && at + 80 < sizeof(body); k++) {
            const struct ciuki_device_resource *r = &d->resources[k]; char value[64];
            range(value, sizeof(value), r->type, r->start, r->end, r->flags);
            at += (size_t)snprintf(body + at, sizeof(body) - at, "%s: %s\n", resource_name(r->type), value);
        }
        snprintf(body + at, sizeof(body) - at, "%s", ds != DC_ACTIVE ? "No resources are held while the device is disabled or removed." :
                 d->flags & CIUKI_DEVICES_TRUNCATED ? "More resources in the lease list." :
                 d->resource_count ? "Assigned resources are read-only." : "No resource assignment recorded.");
    }
    if (!strcmp(d->driver,"gameport") || (d->bus_type==CIUKI_BUS_USB && !strcmp(d->name,"USB HID game controller"))) {
        cp_open(a,CP_JOYSTICK); return;
    }
    static const char *const buttons[] = {"General", "Resources", "Close"};
    du_message(a->ui, D_PROPERTIES, title, body, DU_DLG_ICON_INFO, buttons, 3, resources ? 1 : 0, 2);
    du_dialog_icon(a->ui, du_icon_argb(du_icon_name(class_icon(d->class_id)), 32));
}
static void details(struct cp_app *a)
{
    if (!a->devices_ok) return;
    char body[512], assignment[64]; const char *title;
    if (a->detail_list == C_LEASES) {
        if (a->detail_index >= a->devices.lease_count) return;
        const struct ciuki_device_lease *l = &a->devices.leases[a->detail_index];
        range(assignment, sizeof(assignment), l->type, l->start, l->end, 0);
        title = "Resource Lease";
        snprintf(body, sizeof(body), "Owner: %s\nType: %s\nAssignment: %s\nState: %s\nGeneration: %u",
                 l->owner, resource_name(l->type), assignment, lease_state(l->state), l->generation);
    } else {
        if (a->detail_index >= a->devices.driver_count) return;
        const struct ciuki_driver_record *d = &a->devices.drivers[a->detail_index];
        title = "Driver Details";
        snprintf(body, sizeof(body), "Built-in driver: %s\nKernel build: %s\nDevices bound: %u\nState: %s\n\nBoot policy:\n%s",
                 d->name, d->build, d->bound_devices, cp_device_state(d->state), d->policy);
    }
    du_message(a->ui, D_INFO, title, body, DU_DLG_ICON_INFO, NULL, 0, 0, 0);
}
static void system_pages(struct cp_app *a)
{
    struct du_widget *w; struct du_ui *u = a->ui; char line[512];
    if (a->page == CP_SYSTEM || a->page == CP_DEVICES || a->page == CP_DRIVERS) {
        static const char *const tabs[] = {"General", "Hardware check", "Logs", "Drivers", "Performance", "Hardware Profiles", "Startup"};
        for (int k=0;k<7;k++) a->system_tabs[k].text[0]=tabs[k];
        w=du_get(u,C_TABS); w->id=C_SYSTEM_TABS; w->items=a->system_tabs; w->count=7;
        w->active=(unsigned)a->system_tab;
    }
    if (a->page == CP_DEVICES && a->dm_updating) {
        dm_update_build(a);
    } else if (a->page == CP_DEVICES) {
        w = add(a, DU_TREE, C_TREE, 24, 40, 452, 208, ""); w->inactive_selection = 1;
        w->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_GROW_Y;
        add(a, DU_BUTTON, C_PROPERTIES, 24, 256, 104, 24, "Properties...")->anchors = DU_ANCHOR_BOTTOM;
        add(a, DU_BUTTON, H_DM_DISABLE, 136, 256, 104, 24, "Disable")->anchors = DU_ANCHOR_BOTTOM;
        add(a, DU_BUTTON, H_DM_ENABLE, 248, 256, 104, 24, "Enable")->anchors = DU_ANCHOR_BOTTOM;
        add(a, DU_BUTTON, H_DM_REMOVE, 360, 256, 116, 24, "Remove")->anchors = DU_ANCHOR_RIGHT | DU_ANCHOR_BOTTOM;
        add(a, DU_BUTTON, C_FIRMWARE, 24, 288, 104, 24, "Firmware...")->anchors = DU_ANCHOR_BOTTOM;
        add(a, DU_BUTTON, H_DM_UPDATE, 136, 288, 140, 24, "Update Driver...")->anchors = DU_ANCHOR_BOTTOM;
        cp_tree_build(a);
        add(a, DU_BUTTON, C_REFRESH, 366, 288, 110, 24, "Refresh")->anchors = DU_ANCHOR_RIGHT | DU_ANCHOR_BOTTOM;
        add(a, DU_BUTTON, CP_PACK_INSTALL, 24, 320, 250, 24, "Install driver from pack...")->anchors = DU_ANCHOR_BOTTOM;
        add(a, DU_BUTTON, CP_PACK_REMOVE, 282, 320, 194, 24, "Remove driver package...")->anchors = DU_ANCHOR_RIGHT | DU_ANCHOR_BOTTOM;
        cp_pack_buttons(a);
        dm_buttons(a);
    } else if (a->page == CP_DRIVERS) {
        unsigned n = a->devices_ok ? a->devices.driver_count : 0;
        for (unsigned k = 0; k < n; k++) {
            const struct ciuki_driver_record *d = &a->devices.drivers[k];
            snprintf(a->driver_bound[k], sizeof(a->driver_bound[k]), "%u", d->bound_devices);
            a->driver_items[k] = (struct du_item){.text = {d->name, d->build, a->driver_bound[k], cp_device_state(d->state), d->policy}};
        }
        add(a, DU_LABEL, C_SYS0, 24, 40, 452, 14, "Built-in drivers (kernel build; bindings and boot policy)");
        w = add(a, DU_LISTVIEW, C_DRIVERS, 24, 58, 452, 128, ""); w->items = a->driver_items; w->count = n; w->view = DU_DETAILS;
        w->inactive_selection = 1; w->column_count = 4; w->anchors = DU_ANCHOR_GROW_X;
        w->columns[0] = (struct du_column){"Driver", 115, 0}; w->columns[1] = (struct du_column){"Build", 110, 0};
        w->columns[2] = (struct du_column){"Bound", 48, 1}; w->columns[3] = (struct du_column){"State", 110, 0};

        add(a, DU_LABEL, C_SYS1, 24, 196, 452, 14, "Loader, firmware and native resource leases");
        n = a->devices_ok ? a->devices.lease_count : 0;
        for (unsigned k = 0; k < n; k++) {
            const struct ciuki_device_lease *l = &a->devices.leases[k];
            range(a->lease_range[k], sizeof(a->lease_range[k]), l->type, l->start, l->end, 0);
            snprintf(a->lease_gen[k], sizeof(a->lease_gen[k]), "%u", l->generation);
            a->lease_items[k] = (struct du_item){.text = {l->owner, resource_name(l->type), a->lease_range[k], lease_state(l->state), a->lease_gen[k]}};
        }
        w = add(a, DU_LISTVIEW, C_LEASES, 24, 214, 452, 100, ""); w->items = a->lease_items; w->count = n; w->view = DU_DETAILS;
        w->inactive_selection = 1; w->column_count = 4; w->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_GROW_Y;
        w->columns[0] = (struct du_column){"Owner", 106, 0}; w->columns[1] = (struct du_column){"Type", 48, 0};
        w->columns[2] = (struct du_column){"Assignment", 182, 0}; w->columns[3] = (struct du_column){"State", 95, 0};
        add(a, DU_BUTTON, C_DETAILS, 24, 320, 110, 24, "Details...")->anchors = DU_ANCHOR_BOTTOM;
        add(a, DU_BUTTON, C_REFRESH, 366, 320, 110, 24, "Refresh")->anchors = DU_ANCHOR_RIGHT | DU_ANCHOR_BOTTOM;
    } else if (a->page == CP_SYSTEM && a->system_tab == 4) {
        perf_build(a);
    } else if (a->page == CP_SYSTEM && a->system_tab == 5) {
        hp_build(a);
    } else if (a->page == CP_SYSTEM) {
        if (a->system_tab == 6) {
            text(a,C_STATUS,"");
            if (!a->startup) a->startup=calloc(DESK_STARTUP_MAX,sizeof(*a->startup));
            a->startup_count=a->startup && a->host.startup_list ? a->host.startup_list(a->host.ctx,a->startup,DESK_STARTUP_MAX) : -ENOSYS;
            if (a->startup_count>(int)DESK_STARTUP_MAX) a->startup_count=-EOVERFLOW;
            a->startup_selected=0;
            for (int k=0;k<a->startup_count;k++) {
                struct desk_startup_entry *item=&a->startup[k];
                /* Source IDs come from the shared snapshot model. */
                static const char *const sources[]={"HKLM RunServicesOnce","HKLM RunServices","HKLM RunOnce","HKCU RunOnce","HKLM Run","HKCU Run","StartUp folder"};
                a->startup_items[k]=(struct du_item){.text={item->name,item->source<7 ? sources[item->source] : "Unknown",item->enabled ? (item->error ? "Invalid" : "Enabled") : "Disabled"},.selected=k==0};
            }
            add(a,DU_LABEL,C_SYS0,24,44,500,28,"Programs run when the desktop starts. Hold Shift to skip once.");
            w=add(a,DU_LISTVIEW,CP_STARTUP_LIST,24,78,CP_WIDTH-48,220,"");
            w->items=a->startup_items; w->count=a->startup_count>0 ? (unsigned)a->startup_count : 0;
            w->view=DU_DETAILS; w->inactive_selection=1; w->column_count=3;
            w->columns[0]=(struct du_column){"Item",210,0}; w->columns[1]=(struct du_column){"Location",180,0}; w->columns[2]=(struct du_column){"State",90,0};
            w->anchors=DU_ANCHOR_GROW_X|DU_ANCHOR_GROW_Y;
            w=add(a,DU_CHECKBOX,CP_STARTUP_ENABLED,24,306,300,24,"Enable selected startup item");
            w->value=a->startup_count>0 ? a->startup[0].enabled : 0;
            w->disabled=a->startup_count<=0 || !a->host.startup_enable; w->anchors=DU_ANCHOR_BOTTOM;
            add(a,DU_BUTTON,CP_STARTUP_REFRESH,390,306,140,24,"Refresh")->anchors=DU_ANCHOR_RIGHT|DU_ANCHOR_BOTTOM;
            add(a,DU_LABEL,CP_STARTUP_NOTE,24,338,CP_WIDTH-48,18,a->startup_count<0 ? "Startup list unavailable. Check registry and shortcuts." : a->startup_count ? "Changes take effect at the next desktop start." : "No startup programs.")->anchors=DU_ANCHOR_BOTTOM;
        } else if (a->system_tab == 0) {
            snprintf(line, sizeof(line), "Version %s", a->host.version[0] ? a->host.version : "unknown"); add(a, DU_LABEL, C_SYS0, 140, 74, 336, 14, line);
            snprintf(line, sizeof(line), "Build %s", a->host.build[0] ? a->host.build : "unknown"); add(a, DU_LABEL, C_SYS1, 140, 92, 336, 14, line);
            add(a, DU_LABEL, C_SYS4, 24, 160, 452, 14, "Classic computing, one woof at a time.");
            snprintf(line, sizeof(line), "CPU: %s", a->devices_ok ? a->devices.cpu_brand[0] ? a->devices.cpu_brand : a->devices.cpu_vendor : a->host.machine);
            add(a, DU_LABEL, C_CPU, 24, 190, 452, 14, line);
            snprintf(line, sizeof(line), "Family %u, model %u, stepping %u", a->devices.cpu_family, a->devices.cpu_model, a->devices.cpu_stepping);
            add(a, DU_LABEL, C_SYS2, 24, 210, 452, 14, a->devices_ok ? line : "CPU details unavailable.");
            snprintf(line, sizeof(line), "RAM %llu MB, %llu MB available, %u processes", (unsigned long long)(a->host.mem_total_k / 1024),
                     (unsigned long long)(a->host.mem_free_k / 1024), a->host.processes);
            add(a, DU_LABEL, C_SYS3, 24, 240, 452, 14, line);
            add(a, DU_LABEL, C_UPTIME, 24, 266, 452, 14, "Uptime unavailable.");
            add(a, DU_LABEL, CP_SUMMARY, 24, 296, CP_WIDTH - 48, 14, "");
        } else {
            snprintf(line, sizeof(line), "Current boot flags: 0x%08X", a->devices.boot_flags); add(a, DU_LABEL, C_FLAGS, 24, 80, 280, 14, a->devices_ok ? line : "Boot flags unavailable.");
            snprintf(line, sizeof(line), "Input policy: %s", a->devices.input_policy ? "firmware-first" : "native"); add(a, DU_LABEL, C_SYS0, 320, 80, 212, 14, a->devices_ok ? line : "Input policy unavailable.");
            add(a, DU_LABEL, C_SYS1, 24, 104, 128, 14, "Boot options:");
            add(a, DU_LABEL, C_BOOT_OPTIONS, 152, 104, 380, 14, a->devices_ok ? a->devices.boot_options : "Unavailable");
            a->boot_length = 0;
            a->boot_ok = a->host.boot_load && !a->host.boot_load(a->host.ctx, a->boot_text, sizeof(a->boot_text), &a->boot_length);
            char check[CP_BOOT_MAX];
            if (a->boot_ok && cp_boot_serial(a->boot_text, a->boot_length, 1, check, sizeof(check))) a->boot_ok = 0;
            a->serial_next = a->devices_ok && !!(a->devices.boot_flags & 2u);
            if (a->boot_ok) {
                /* The persisted next-boot choice may differ from the current boot. */
                a->boot_text[a->boot_length] = 0;
                /* Loader default is on; any exact serial=0 token latches it
                 * off. serial=1 cannot clear an earlier off token. */
                a->serial_next = 1;
                for (size_t k = 0; k + 8 <= a->boot_length; k++) if ((!k || a->boot_text[k - 1] == ' ' || a->boot_text[k - 1] == '\n' || a->boot_text[k - 1] == '\r' || a->boot_text[k - 1] == '\t') &&
                    !memcmp(a->boot_text + k, "serial=", 7) && (k + 8 == a->boot_length || a->boot_text[k + 8] == ' ' || a->boot_text[k + 8] == '\n' || a->boot_text[k + 8] == '\r' || a->boot_text[k + 8] == '\t')) if (a->boot_text[k + 7] == '0') a->serial_next = 0;
            }
            w = add(a, DU_CHECKBOX, C_SERIAL, 24, 44, 300, 24, "Serial log at next boot"); w->value = a->serial_next; w->disabled = !a->boot_ok;
            w = add(a, DU_BUTTON, C_BOOT_APPLY, 352, 44, 180, 24, "Apply to BOOT.CFG"); w->disabled = !a->boot_ok || !a->host.boot_save;
            add(a,DU_LABEL,C_SYS2,24,126,508,14,a->boot_ok ? "Serial logging changes take effect after restarting CiukiOS." : "BOOT.CFG is unavailable; the log view remains read-only.");
            log_refresh(a);
            add(a,DU_LABEL,CP_LOG_STATUS,24,154,508,16,a->log_status)->anchors=DU_ANCHOR_GROW_X;
            w=add(a,a->log.text ? DU_TEXTAREA : DU_LABEL,CP_LOG_VIEW,24,176,CP_WIDTH-48,140,"");
            w->data=a->log.text ? &a->log : NULL; w->anchors=DU_ANCHOR_GROW_X|DU_ANCHOR_GROW_Y;
            add(a,DU_BUTTON,CP_LOG_PREV,24,328,88,24,"Previous")->anchors=DU_ANCHOR_BOTTOM;
            add(a,DU_BUTTON,CP_OPEN_LOG,120,328,88,24,"Refresh log")->anchors=DU_ANCHOR_BOTTOM;
            add(a,DU_BUTTON,CP_LOG_NEXT,216,328,88,24,"Next")->anchors=DU_ANCHOR_BOTTOM;
            du_get(u,CP_LOG_PREV)->disabled=!a->log_total || !a->log_offset;
            du_get(u,CP_LOG_NEXT)->disabled=!a->log_total || a->log_offset+a->log.len>=a->log_total;

        }
    } else if (a->page == CP_ADAPTER) {
        add(a, DU_BUTTON, CP_REFRESH, 24, 324, 84, 24, "Refresh");
    } else if (a->page == CP_NETWORK) {
        network_build(a);
    }
}

static const char *power_number(char out[16], uint32_t n)
{ if (n == CIUKI_POWER_UNKNOWN) return "Unknown"; snprintf(out,16,"%u",n); return out; }
static unsigned power_read(struct cp_app *a)
{
    struct ciuki_power_status p;
    memset(a->power_lines,0,sizeof(a->power_lines));
    if (!a->host.power_status || a->host.power_status(a->host.ctx,&p) || p.version != CIUKI_POWER_VERSION ||
        p.size != sizeof(p) || p.battery_count > CIUKI_POWER_BATTERIES || p.zone_count > CIUKI_POWER_ZONES) {
        snprintf(a->power_lines[0],128,"Power status: Unknown"); return 20;
    }
    unsigned n=0;
    snprintf(a->power_lines[n++],128,"AC: %s     Lid: %s",p.ac_online==1 ? "Connected" : p.ac_online==0 ? "Disconnected" :
             p.ac_online==CIUKI_POWER_AC_BACKUP ? "Backup power" : "Unknown",
             p.lid_open==1 ? "Open" : p.lid_open==0 ? "Closed" : "Unknown");
    /* One provider, ACPI or APM 1.2, owns these facts; the other is excluded. */
    const char *via=p.provider==CIUKI_POWER_PROVIDER_ACPI ? " (ACPI)" : p.provider==CIUKI_POWER_PROVIDER_APM ? " (APM 1.2)" : "";
    snprintf(a->power_lines[n++],128,"Control: %s%s; firmware status: 0x%08x",
             p.ownership==CIUKI_POWER_OWNED ? "CiukiOS" : p.ownership==CIUKI_POWER_FIRMWARE ? "Firmware retained" :
             p.ownership==CIUKI_POWER_QUARANTINED ? "Uncertain; claims retained" : "Pending",via,p.error);
    if (!p.battery_count) snprintf(a->power_lines[n++],128,"Batteries: none reported");
    for (unsigned i=0;i<p.battery_count;i++) {
        const struct ciuki_power_battery *b=&p.batteries[i]; char x[16],y[16],z[16];
        int percent=b->unit==CIUKI_POWER_UNIT_PERCENT;
        /* fx-18: an empty battery bay (APM 1.2 CH bit 4) has no other facts. */
        if (b->present==0) { snprintf(a->power_lines[n++],128,"Battery %u: Not installed",b->id); continue; }
        snprintf(a->power_lines[n++],128,"Battery %u: %s; capacity %s / %s %s",b->id,
            b->present==0 ? "Absent" : b->state==CIUKI_POWER_UNKNOWN ? "Unknown" : b->state&1 ? "Discharging" : b->state&2 ? "Charging" : "Idle",
            power_number(x,b->remaining),power_number(y,b->full),b->unit==0 ? "mWh" : b->unit==1 ? "mAh" : percent ? "%" : "(units unknown)");
        if (percent) {
            if (b->seconds==CIUKI_POWER_UNKNOWN) snprintf(a->power_lines[n++],128,"Remaining life: Unknown%s",b->state!=CIUKI_POWER_UNKNOWN && b->state&4 ? "; critical" : "");
            else snprintf(a->power_lines[n++],128,"Remaining life: %u h %02u min%s",b->seconds/3600,b->seconds/60%60,
                          b->state!=CIUKI_POWER_UNKNOWN && b->state&4 ? "; critical" : "");
        } else snprintf(a->power_lines[n++],128,"Design %s; rate %s %s; voltage %s mV",power_number(x,b->design),power_number(y,b->rate),
            b->unit==0 ? "mW" : b->unit==1 ? "mA" : "(units unknown)",power_number(z,b->voltage));
    }
    for (unsigned i=0;i<p.zone_count;i++) {
        const struct ciuki_power_zone *z=&p.zones[i]; char t[16],c[16],v[16];
        snprintf(a->power_lines[n++],128,"Zone %u: %s; critical %s; passive %s (0.1 K)",z->id,
            power_number(t,z->temperature),power_number(c,z->critical),power_number(v,z->passive));
    }
    /* Stable widget count lets a failed refresh clear previous values. */
    return 20;
}
/* Add/Remove Programs: the icon of the program's own entry (same Tango names as the Start menu): a bundled game,
 * a /Programs application by its name, a DOS program through the DOS box, any other installed program generic. */
static const char *uninstall_icon(const struct desk_uninstall_entry *r)
{
    static char object[48];
    if (r->kind==DESK_PROGRAM_GAME) return du_icon_name("menu:game");
    if (r->system_component) return "package-x-generic";
    if (!strncmp(r->location,"/Programs/",10)) {
        size_t n=strcspn(r->location+10,"/");
        if (n && n<sizeof(object)-9 && !memchr(r->location+10,'.',n)) {
            memcpy(object,"program:",8); memcpy(object+8,r->location+10,n); object[8+n]=0;
            return du_icon_name(object);
        }
    }
    if (r->kind==DESK_PROGRAM_DOS) return du_icon_name("program:dosbox");
    return du_icon_name("type:executable");
}
/* fx-exeicons: the program's own icon (DisplayIcon, or its Start menu
 * shortcut; programs.c), extracted by libdeskui's icon worker; the kind's
 * icon until then. */
static const uint32_t *uninstall_pixels(const struct desk_uninstall_entry *r, unsigned size)
{
    const uint32_t *own = r->icon[0] ? du_file_icon_flags(r->icon, 0, size, 0) : NULL;
    return own ? own : du_icon_argb(uninstall_icon(r), size);
}
void cp_uninstall_icons(struct cp_app *a)
{
    if (a->page != CP_UNINSTALL) return;
    for (unsigned k = 0; k < a->uninstall_count; k++) {
        a->uninstall_items[k].icon16 = uninstall_pixels(&a->uninstall[k], 16);
        a->uninstall_items[k].icon32 = uninstall_pixels(&a->uninstall[k], 32);
    }
    if (a->ui) a->ui->dirty = 1;
}
/* A single-page applet used to wear a one-tab strip repeating the window title. Win98 property sheets put the
 * tabs directly on the dialog and a page without siblings has none: hide the strip and lift the body into it. */
static void flatten_page(struct cp_app *a)
{
    struct du_ui *u = a->ui;
    struct du_widget *t = du_get(u, C_TABS);
    if (!t || t->kind != DU_TABS || t->count != 1 || t->items != a->tab_items || t->items[0].text[0] != page_labels[a->page]) return;
    int shift = DU_TAB_H - 1, top = t->box.y + shift, bottom = t->box.y + t->box.h;
    for (unsigned k = 0; k < u->count; k++) {
        struct du_widget *v = &u->widgets[k];
        if (v == t || v->box.y < top || v->box.y + v->box.h > bottom) continue;
        if ((v->anchors & DU_ANCHOR_BOTTOM) && !(v->anchors & DU_ANCHOR_TOP)) continue;
        v->box.y -= shift;
        if ((v->anchors & DU_ANCHOR_GROW_Y) == DU_ANCHOR_GROW_Y) v->box.h += shift;
    }
    t->hidden = 1;
}
static void build(struct cp_app *a)
{
    a->building=1;
    du_timer_stop(a->ui,CP_T_SAVER);
    struct du_ui *u = a->ui;
    du_release(u); du_dialog_close(u);
    u->count = 0; u->focus = -1;
    struct du_widget *w;
    if (a->page == CP_HOME) {
        for (int k=0;k<CP_CATEGORIES;k++) {
            a->category_items[k]=(struct du_item){.text={cp_categories[k].label},
                .icon16=du_icon_argb(du_icon_name(cp_categories[k].icon),16),.selected=k==a->category};
        }
        w=add(a,DU_TREE,CP_CATEGORY,8,8,182,CP_HEIGHT-36,"");
        w->items=a->category_items; w->count=CP_CATEGORIES; w->active=(unsigned)a->category;
        w->inactive_selection=1; w->anchors=DU_ANCHOR_GROW_Y;
        unsigned count=0;
        for (unsigned k=0;k<CP_APPLETS;k++) if (cp_applets[k].category==a->category) {
            a->applet_pages[count]=cp_applets[k].page;
            a->applet_items[count++]=(struct du_item){.text={cp_applets[k].label},
                .icon16=du_icon_argb(du_icon_name(cp_applets[k].icon),16),
                .icon32=du_icon_argb(du_icon_name(cp_applets[k].icon),32)};
        }
        /* A Win98 Control Panel is a folder window: the applets are large icons, the status bar counts them. */
        w=add(a,DU_LISTVIEW,C_APPLETS,198,8,CP_WIDTH-206,CP_HEIGHT-36,"");
        w->items=a->applet_items; w->count=count; w->view=DU_ICONS;
        w->anchors=DU_ANCHOR_GROW_X|DU_ANCHOR_GROW_Y;
        snprintf(a->status,sizeof(a->status),"%u object(s)",count);
    } else {
        a->tab_items[0].text[0] = page_labels[a->page];
        w = add(a, DU_TABS, C_TABS, 8, 8, CP_WIDTH - 16, CP_HEIGHT - 80, "");
        w->items = a->tab_items; w->count = 1;
        w->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_GROW_Y;
        add(a, DU_BUTTON, C_BACK, 12, CP_HEIGHT - 34, 110, 24, "Control Panel")->anchors = DU_ANCHOR_BOTTOM;
    }
    if (a->page == CP_DISPLAY) {
        add(a,DU_BUTTON,CP_OK,CP_WIDTH-272,CP_HEIGHT-34,80,24,"OK")->anchors=DU_ANCHOR_RIGHT|DU_ANCHOR_BOTTOM;
        du_get(u,CP_OK)->def=1;
        add(a,DU_BUTTON,CP_CANCEL,CP_WIDTH-184,CP_HEIGHT-34,80,24,"Cancel")->anchors=DU_ANCHOR_RIGHT|DU_ANCHOR_BOTTOM;
        add(a,DU_BUTTON,CP_APPLY,CP_WIDTH-96,CP_HEIGHT-34,88,24,"Apply")->anchors=DU_ANCHOR_RIGHT|DU_ANCHOR_BOTTOM;
    } else if (a->page != CP_HOME) add(a, DU_BUTTON, C_CLOSE, CP_WIDTH - 88, CP_HEIGHT - 34, 80, 24, "Close")->anchors = DU_ANCHOR_RIGHT | DU_ANCHOR_BOTTOM;
    if (a->page == CP_HOME) add(a, DU_STATUS, C_STATUS, 0, CP_HEIGHT - 20, CP_WIDTH, 20, a->status)->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_BOTTOM;
    else add(a, DU_LABEL, C_STATUS, 12, CP_HEIGHT - 62, 476, 16, a->status)->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_BOTTOM;
    if (user_page(a)) {
        user_build(a);
    } else if (a->page == CP_DISPLAY) {
        static const char *const tabs[]={"Background","Appearance","Settings","Screen Saver","Effects","Themes"};
        for (int k=0;k<CP_DISPLAY_TABS;k++) a->display_tabs[k].text[0]=tabs[k];
        w=du_get(u,C_TABS); w->id=CP_DISPLAY_TABS_ID; w->items=a->display_tabs;
        w->count=CP_DISPLAY_TABS; w->active=(unsigned)a->display_tab; w->disabled=a->mode_state!=0;
        if (a->display_tab==CP_BACKGROUND) {
            add(a,DU_LABEL,C_WALL_L,248,44,240,16,"&Picture:");
            unsigned walls=2+DESK_BUILTIN_WALLPAPERS+a->wall_count+8;
            items(a->wall_items,a->wall_labels,walls,wall_label,a);
            for (unsigned k=0;k<walls;k++) a->wall_items[k].icon16=NULL;
            w=add(a,DU_LISTVIEW,C_WALL,248,64,CP_WIDTH-272,138,"");
            w->items=a->wall_items; w->count=walls; list_style(w,"Wallpaper"); w->anchors=DU_ANCHOR_GROW_X;
            add(a,DU_LABEL,C_POS_L,24,224,100,14,"Position:");
            static const char *const pos[CP_POSITIONS]={"Center","Tile","Stretch","Fit","Fill"};
            for (int k=0;k<CP_POSITIONS;k++) add(a,DU_BUTTON,C_POS0+k,24+k%3*70,240+k/3*24,68,22,pos[k]);
            add(a,DU_LABEL,C_COL_L,24,296,180,14,"Desktop colour:");
            for (int k=0;k<6;k++) add(a,DU_BUTTON,C_COL0+k,24+k*34,312,30,24,"");
            add(a,DU_BUTTON,4130,248,206,100,22,"Browse...");
            add(a,DU_LABEL,2002,356,209,48,16,"Pattern:");   /* G19: a mask drawn behind the picture */
            for (int k=0;k<DESK_WALL_PATTERNS;k++) a->pattern_items[k]=(struct du_item){.text={desk_wallpaper_pattern_labels[k]}};
            w=add(a,DU_COMBO,CP_WALL_PATTERN,404,206,CP_WIDTH-404-24,22,"");
            du_combo_set(w,a->pattern_items,DESK_WALL_PATTERNS,a->cfg.wallpaper_pattern); w->anchors=DU_ANCHOR_GROW_X;
            add(a,DU_BUTTON,C_ICONS,248,232,CP_WIDTH-272,24,"")->anchors=DU_ANCHOR_GROW_X;
            static const int rgb_ids[3]={C_RGB_R,C_RGB_G,C_RGB_B};
            static const char *const rgb_names[]={"Red", "Green", "Blue"};
            uint32_t colour=a->cfg.desktop_color;
            for (unsigned k=0;k<3;k++) {
                add(a,DU_LABEL,4010+(int)k,248,261+(int)k*26,44,16,rgb_names[k]);
                w=slider(a,rgb_ids[k],292,256+(int)k*26,CP_WIDTH-316,255,0);
                w->value=(int)((colour>>(16-k*8))&255u);
            }
        } else if (a->display_tab==CP_APPEARANCE) {
            appearance_build(a);
        } else if(a->display_tab==CP_THEMES) {
            appearance_build(a);
        } else if(a->display_tab==CP_EFFECTS) {
            static const char *const names[]={"Use large icons","Show window contents while dragging","Smooth edges of screen fonts","Animate menus"};
            for(int i=0;i<4;i++) {
                char label[128]; snprintf(label,sizeof(label),"[%c] %s",(a->cfg.appearance.effects&(1u<<i)) ? 'X' : ' ',names[i]);
                add(a,DU_BUTTON,A_EFFECT0+i,24,56+i*44,490,30,label);
            }
            add(a,DU_BUTTON,A_DOC_ICON,24,244,490,30,a->cfg.document_icon ? "Documents icon: Folder" : "Documents icon: Home");
            add(a,DU_BUTTON,A_BIN_ICON,24,286,490,30,a->cfg.recycle_icon ? "Recycle Bin icon: Delete" : "Recycle Bin icon: Trash");
            add(a,DU_LABEL,4189,24,338,490,18,"Computer and About use the approved Ciuki portrait.");
        } else if (a->display_tab==CP_SCREEN_SAVER) {
            static const char *const names[]={"(None)","Blank","Ciuki logo","Starfield"};
            int choice=a->cfg.saver_enabled ? a->cfg.saver_kind+1 : 0;
            add(a,DU_GROUP,1979,24,180,CP_WIDTH-48,126,"Screen saver");
            for (int k=0;k<4;k++) a->saver_items[k]=(struct du_menu_item){names[k],CP_SAVER_NONE+k,0,0,k==choice ? DU_ITEM_CHECKED : 0};
            w=add(a,DU_BUTTON,CP_SAVER_KIND,40,202,234,24,names[choice]);
            add(a,DU_BUTTON,CP_SAVER_SETTINGS,286,202,104,24,"Settings...")->disabled=!choice;
            add(a,DU_BUTTON,CP_SAVER_PREVIEW,400,202,104,24,"Preview")->disabled=!choice || !a->host.apply;
            if (accounts_mode(a)) {
                /* sec-user-04: per-user "On resume, password protect" (XP
                 * HKCU ScreenSaverIsSecure); LOGON locks only for an owner
                 * with a password. */
                w=add(a,DU_CHECKBOX,CP_SAVER_PASSWORD,40,232,250,22,"On resume, password protect");
                w->value=desk_secure_pref(a->cfg.extra,DESK_SECURE_SAVER);
                add(a,DU_BUTTON,CP_SAVER_CHANGE,400,232,104,22,"Password...");
            } else {
            w=add(a,DU_CHECKBOX,CP_SAVER_PASSWORD,40,232,210,22,"Password protected");
            w->value=!!(a->users.flags&USERS_LOCK); w->disabled=!a->user_loaded || a->user_busy || !a->host.user_begin;
            add(a,DU_BUTTON,CP_SAVER_CHANGE,400,232,104,22,"Change...")->disabled=!a->host.user_begin;
            if (!a->user_loaded && !a->user_busy && a->host.user_begin) user_submit(a,USERS_READ);
            }
            add(a,DU_LABEL,1977,40,266,42,18,"&Wait:");
            char wait[16]; snprintf(wait,sizeof(wait),"%d",a->cfg.saver_seconds/60);
            w=add(a,DU_EDIT,CP_SAVER_WAIT,94,262,50,24,wait); w->disabled=!choice; w->edit_up_down=1;
            add(a,DU_BUTTON,CP_SAVER_UP,144,262,20,12,"+")->disabled=!choice;
            add(a,DU_BUTTON,CP_SAVER_DOWN,144,274,20,12,"-")->disabled=!choice;
            add(a,DU_LABEL,1978,174,266,90,16,"minutes");
            add(a,DU_LABEL,1976,40,288,460,14,accounts_mode(a) ? "Uses your account password; without one the saver only hides the screen." :
                                                             "Uses the named profile password; change it in Passwords.");
            add(a,DU_GROUP,1975,24,316,CP_WIDTH-48,42,"Energy saving features of monitor");
            add(a,DU_LABEL,1974,40,336,338,16,"Configure monitor power saving.");
            add(a,DU_BUTTON,CP_SAVER_POWER,400,330,104,24,"Settings...")->disabled=!a->host.open_applet;
            if (choice>1) du_timer_start(u,CP_T_SAVER,100000000ull,1);
        } else {
            add(a,DU_LABEL,CP_AREA_LABEL,24,44,232,16,"Resolution:");
            w=add(a,DU_LISTVIEW,CP_AREA,24,64,232,100,""); w->items=a->area_items; list_style(w,"Screen area");
            add(a,DU_LABEL,CP_DEPTH_LABEL,272,44,248,16,"Colour depth:");
            w=add(a,DU_LISTVIEW,CP_DEPTH,272,64,CP_WIDTH-296,100,""); w->items=a->depth_items; list_style(w,"Colours"); w->anchors=DU_ANCHOR_GROW_X;
            add(a,DU_LABEL,CP_MODE_NOTE,24,174,400,16,"");
            add(a,DU_LABEL,972,24,202,CP_WIDTH-48,16,a->adapter_lines[4]);
            add(a,DU_LABEL,973,24,228,CP_WIDTH-48,16,a->adapter_lines[2]);
            add(a,DU_LABEL,974,24,254,CP_WIDTH-48,16,a->monitor_line);
            add(a,DU_LABEL,975,24,280,232,18,"Advanced font size:");
            add(a,DU_BUTTON,A_SMALL,24,306,112,24,"Small (96 dpi)");
            add(a,DU_BUTTON,A_LARGE,142,306,112,24,"Large (120 dpi)");
            char dpi[64]; snprintf(dpi,sizeof(dpi),"Custom: %u dpi",a->cfg.appearance.dpi);
            add(a,DU_LABEL,4198,272,286,240,18,dpi);
            w=slider(a,A_DPI,272,308,240,192,0); w->value=a->cfg.appearance.dpi-48;
            add(a,DU_BUTTON,CP_REFRESH,24,342,88,24,"Refresh");
            add(a,DU_BUTTON,4133,400,342,112,24,"Advanced...");
            if(a->host.accel_retry){
                text(a,975,"Retry re-arms the boot test; restart is required.");
                add(a,DU_BUTTON,976,128,342,252,24,"Retry acceleration after restart");
            }
        }
    } else if (a->page == CP_INTERNET) {
        internet_build(a);
    } else if(a->page==CP_FONTS) {
        add(a,DU_LABEL,4199,24,60,490,20,"Installed fonts and preview"); cp_fonts_build(a);
    } else if (a->page == CP_JOYSTICK) {
        joystick_build(a);
    } else if (a->page == CP_MOUSE) {
    /* Mouse */
    add(a, DU_LABEL, C_SPEED_L, 24, 44, 200, 14, "Pointer &speed:");
    slider(a, C_SPEED, 24, 56, 300, CFG_MOUSE_MAX - CFG_MOUSE_MIN, 1);
    add(a, DU_LABEL, C_SPEED_V, 336, 60, 140, 17, "");
    add(a, DU_LABEL, C_SLOW, 24, 86, 60, 14, "Slow");
    add(a, DU_LABEL, C_FAST, 294, 86, 40, 14, "Fast");
    add(a, DU_LABEL, C_DBL_L, 24, 116, 200, 14, "&Double-click speed:");
    slider(a, C_DBL, 24, 128, 300, 7, 1);
    add(a, DU_LABEL, C_DBL_V, 336, 132, 140, 17, "");
    add(a,DU_CHECKBOX,C_SWAP,24,168,240,22,"Swap left and right buttons");
    add(a,DU_BUTTON,C_DOUBLE_TEST,288,166,220,24,"Double click here to test");
    add(a,DU_LABEL,1510,24,202,160,14,"Pointer trails (0/1 = off):");
    slider(a,C_TRAILS,200,194,300,7,1);
    add(a,DU_LABEL,1511,24,234,170,14,"Wheel lines (101 = page):");
    slider(a,C_WHEEL,200,229,300,101,0);
    add(a,DU_BUTTON,C_POINTER,24,272,300,24,"");
    } else if (a->page == CP_KEYBOARD) {
    /* Keyboard */
    add(a, DU_LABEL, C_KEY_L, 24, 44, 200, 14, "Keyboard &layout:");
    for (int k = 0; k < CFG_KEYS_COUNT; k++) { memset(&a->key_items[k], 0, sizeof(a->key_items[k])); a->key_items[k].text[0] = cfg_keyboard_labels[k]; }
    w = add(a, DU_LISTVIEW, C_KEY, 24, 60, 220, 56, ""); w->items = a->key_items; w->count = CFG_KEYS_COUNT; list_style(w, "Layout");
    add(a,DU_CHECKBOX,C_LAYOUT_SWITCH,264,62,270,42,"Left Alt+Shift / language indicator");
    add(a,DU_LABEL,1512,24,134,190,14,"Repeat delay (short to long):");
    slider(a,C_REPEAT_DELAY,224,128,280,3,1);
    add(a,DU_LABEL,1513,24,174,190,14,"Repeat rate (slow to fast):");
    slider(a,C_REPEAT_SPEED,224,168,280,31,0);
    add(a,DU_LABEL,1514,24,214,190,14,"Cursor blink (0 = steady):");
    slider(a,C_BLINK,224,206,280,12,1);
    add(a,DU_LABEL,C_KEY_NOTE1,24,258,480,14,"Italian: Shift+^, then a vowel; Space inserts the accent.");
    add(a,DU_EDIT,1515,24,284,480,24,"Type here to test repeat and blink");
    add(a, DU_LABEL, C_KEY_NOTE2, 24, 318, 480, 14, "The Run dialog uses the new layout at once; programs started");
    add(a, DU_LABEL, 1516, 24, 332, 480, 14, "afterwards receive it in CIUKI_KEYMAP.");
    } else if (a->page == CP_MULTIMEDIA) {
        struct ciuki_audio_diagnostics diag={0};
        struct ciuki_audio_capture cap={.version=1,.size=sizeof(cap)};
        int playback=a->host.audio_diagnostics && !a->host.audio_diagnostics(a->host.ctx,&diag) &&
            (diag.flags&CIUKI_AUDIO_DIAG_READY);
        int capture=a->host.capture_caps && !a->host.capture_caps(a->host.ctx,&cap) && cap.available;
        add(a,DU_GROUP,1800,16,38,528,100,"Audio");
        add(a,DU_LABEL,1801,28,62,494,20,playback ? "Preferred playback: system audio device" : "Preferred playback: no available device");
        add(a,DU_LABEL,1802,28,88,494,20,capture ? "Preferred recording: system capture device (microphone / line in)" : "Preferred recording: no supported capture device");
        add(a,DU_LABEL,1803,28,112,494,20,"One system device; automatic routing (no alternate devices).");
        add(a,DU_GROUP,1804,16,146,528,70,"MIDI");
        add(a,DU_LABEL,1805,28,170,494,20,"Windows programs: built-in TinySoundFont synthesizer");
        add(a,DU_LABEL,1806,28,192,494,20,"DOS: OPL3 synthesis; native MIDI port is not yet available.");
        add(a,DU_GROUP,1807,16,224,528,66,"Devices");
        char device[128];
        if (playback) snprintf(device,sizeof(device),"Controller %08x; codec %08x; playback %u Hz",diag.controller,diag.codec_id,diag.rate);
        else snprintf(device,sizeof(device),"Audio hardware unavailable or quarantined.");
        add(a,DU_LABEL,1808,28,248,494,20,device);
        add(a,DU_LABEL,1809,28,268,110,20,"CD volume:");
        struct ciuki_mixer_line cd={.version=1,.size=sizeof(cd),.line=CIUKI_MIXER_CD};
        int cd_ok=a->host.audio_line && !a->host.audio_line(a->host.ctx,&cd,0) && cd.available;
        struct du_widget *cd_slider=slider(a,C_MM_CD,150,262,300,100,0);
        du_trackbar_set(cd_slider,0,100,0,1,10,0); cd_slider->value=(int)((cd.left+cd.right)/2); cd_slider->disabled=!cd_ok;
        add(a,DU_BUTTON,C_MM_VOLUME,24,306,152,26,"Volume Control");
        add(a,DU_BUTTON,C_MM_RECORDER,190,306,152,26,"Sound Recorder");
        add(a,DU_BUTTON,C_MM_MUSIC,356,306,152,26,"Music Player");
    } else if (a->page == CP_TASKBAR) {
    a->taskbar_tabs[0]=(struct du_item){.text={"Taskbar Options"},.selected=!a->taskbar_tab};
    a->taskbar_tabs[1]=(struct du_item){.text={"Start Menu Programs"},.selected=a->taskbar_tab==1};
    /* The two tabs sit directly on the sheet (the page already owns the outer tab control). */
    w=du_get(u,C_TABS);w->id=CP_TASKBAR_TABS;w->items=a->taskbar_tabs;w->count=2;w->active=(unsigned)a->taskbar_tab;
    if (!a->taskbar_tab) {
        add(a,DU_CHECKBOX,C_TASKBAR_TOP,24,48,452,24,"Always on top");
        add(a,DU_CHECKBOX,C_TASKBAR_AUTOHIDE,24,80,452,24,"Auto hide");
        add(a,DU_CHECKBOX,C_START_SMALL,24,112,452,24,"Show small icons in Start menu");
        add(a,DU_CHECKBOX,C_TASKBAR_CLOCK,24,144,452,24,"Show clock");
        add(a,DU_CHECKBOX,C_QUICK_LAUNCH,24,176,452,24,"Show Quick Launch");
    } else {
        add(a,DU_LABEL,1978,24,44,CP_WIDTH-48,18,"Customize the Programs menu:");
        add(a,DU_BUTTON,CP_START_ADD,24,70,88,24,"Add...");
        add(a,DU_BUTTON,CP_START_REMOVE,120,70,88,24,"Remove...");
        add(a,DU_BUTTON,CP_START_ADVANCED,216,70,100,24,"Advanced...");
        if (a->menu_selected>=a->menu.count) a->menu_selected=0;
        for (unsigned k=0;k<a->menu.count;k++) a->menu_items[k]=(struct du_item){.text={a->menu.entries[k].label,a->menu.entries[k].path},.selected=k==a->menu_selected};
        w=add(a,DU_LISTVIEW,CP_START_LIST,24,106,CP_WIDTH-48,124,"");w->items=a->menu_items;w->count=a->menu.count;w->active=a->menu_selected;list_style(w,"Shortcut");
        w->column_count=2;w->columns[0]=(struct du_column){"Shortcut",160,0};w->columns[1]=(struct du_column){"Location",CP_WIDTH-224,0};
        add(a,DU_LABEL,1979,24,246,CP_WIDTH-48,18,"Documents menu - clear the list of recently opened documents:");
        add(a,DU_BUTTON,CP_START_CLEAR,24,272,88,24,"Clear");
    }
    } else if (a->page == CP_SOUNDS) {
    /* Sounds */
    add(a, DU_LABEL, C_VOL_L, 24, 44, 180, 14, "Master &volume:");
    slider(a, C_VOL, 24, 56, 160, 100, 10);
    add(a, DU_LABEL, C_VOL_V, 194, 60, 48, 17, "");
    add(a, DU_LABEL, 898, 270, 44, 180, 14, "UI sounds volume:");
    slider(a, C_UI_VOL, 270, 56, 150, 100, 10);
    add(a, DU_LABEL, C_UI_VOL_V, 430, 60, 46, 17, "");
    add(a, DU_BUTTON, C_MUTE, 24, 88, 140, 24, "");
    add(a, DU_CHECKBOX, C_SOUND_ENABLED, 184, 88, 290, 24, "Play system sounds");
    static const char *const schemes[] = {"Default", "No sounds", "Custom"};
    for (unsigned k = 0; k < 3; k++) a->scheme_items[k].text[0] = schemes[k];
    int count=a->host.sound_schemes ? a->host.sound_schemes(a->host.ctx,a->sound_names,SOUND_SCHEMES_MAX) : 0;
    a->sound_name_count=count>=0 && count<=(int)SOUND_SCHEMES_MAX ? (unsigned)count : 0;
    for (unsigned k=0;k<a->sound_name_count;k++) a->scheme_items[k+3].text[0]=a->sound_names[k];
    w = add(a,DU_LISTVIEW,C_SCHEME,24,120,270,52,""); w->items = a->scheme_items; w->count = 3+a->sound_name_count;
    list_style(w,"Sound scheme");
    add(a,DU_BUTTON,C_SCHEME_SAVE,306,124,82,24,"Save As...");
    add(a,DU_BUTTON,C_SCHEME_DELETE,396,124,80,24,"Delete");
    for (unsigned k = 0; k < SOUND_EVENTS; k++) {
        a->sound_items[k].text[0] = sound_labels[k]; a->sound_items[k].text[1] = a->sounds.file[k];
    }
    w = add(a,DU_LISTVIEW,C_SOUND_LIST,24,180,452,94,""); w->items = a->sound_items; w->count = SOUND_EVENTS;
    list_style(w,"Event"); w->column_count = 2;
    w->columns[0] = (struct du_column){"Event",170,0}; w->columns[1] = (struct du_column){"Assigned sound",258,0};
    add(a,DU_BUTTON,C_SOUND_TEST,24,280,90,24,"Preview");
    add(a,DU_BUTTON,C_SOUND_BROWSE,124,280,90,24,"Browse...");
    add(a,DU_BUTTON,C_SOUND_NONE,224,280,90,24,"None");
    add(a,DU_BUTTON,C_HARDWARE,24,312,220,24,"Test audio hardware");
    add(a,DU_LABEL,C_AUDIO,24,342,500,14,"");
    } else if (a->page == CP_DATETIME) {
    dt_build(a);
    } else if (a->page == CP_REGIONAL) {
    regional_build(a);
    } else if (a->page == CP_POWER) {
    pw_build(a);

    } else if (a->page == CP_ACCESS) {
    ac_build(a);
    } else if (a->page==CP_UNINSTALL) {
        int count=a->host.uninstall_list ? a->host.uninstall_list(a->host.ctx,a->uninstall,CP_PROGRAM_MAX) : -ENOSYS;
        a->uninstall_count=count>=0 && count<=(int)CP_PROGRAM_MAX ? (unsigned)count : 0;
        a->uninstall_selected=0;
        for (unsigned k=0;k<a->uninstall_count;k++) {
            struct desk_uninstall_entry *r=&a->uninstall[k];
            r->name[sizeof(r->name)-1]=0; r->key[sizeof(r->key)-1]=0;
            memset(&a->uninstall_items[k],0,sizeof(a->uninstall_items[k]));
            a->uninstall_items[k].icon16=uninstall_pixels(r,16);
            a->uninstall_items[k].icon32=uninstall_pixels(r,32);
            a->uninstall_items[k].text[0]=r->name;
            a->uninstall_items[k].text[1]=r->version;
            a->uninstall_items[k].text[2]=r->publisher;
            snprintf(a->uninstall_sizes[k],sizeof(a->uninstall_sizes[k]),"%llu KB%s",
                (unsigned long long)((r->disk_bytes+1023)/1024),r->no_remove || r->system_component ? " (protected)" : "");
            a->uninstall_items[k].text[3]=a->uninstall_sizes[k];
        }
        add(a,DU_LABEL,981,24,48,508,16,"Install/Uninstall: select a program to remove.");
        w=add(a,DU_LISTVIEW,982,24,76,508,224,"");
        w->items=a->uninstall_items; w->count=a->uninstall_count; list_style(w,"Name");
        w->column_count=4;
        w->columns[0]=(struct du_column){"Name",170,0};
        w->columns[1]=(struct du_column){"Version",64,0};
        w->columns[2]=(struct du_column){"Publisher",130,0};
        w->columns[3]=(struct du_column){"Size on disk",140,0};
        w->anchors=DU_ANCHOR_GROW_X|DU_ANCHOR_GROW_Y;
        du_set_text(w,count<0 ? "Program inventory unavailable. Refresh to try again." : "No programs are registered.");
        add(a,DU_BUTTON,983,24,316,100,24,"Remove")->disabled=!a->uninstall_count ||
            (!a->host.program_remove && !a->host.uninstall_run) || a->uninstall[0].no_remove || a->uninstall[0].system_component ||
            uninstall_locked(a,&a->uninstall[0]);   /* sec-user-04 */
        add(a,DU_BUTTON,984,136,316,96,24,"Refresh");
        add(a,DU_BUTTON,986,244,316,100,24,"Install")->disabled=!a->host.program_install;
        add(a,DU_LABEL,985,24,350,508,16,a->uninstall_count && a->uninstall[0].inventory_more ? "More programs exist. Remove entries and refresh to see more." :
            a->uninstall_count && uninstall_locked(a,&a->uninstall[0]) ? "You must be an administrator to change this setting." :
            "Protected components cannot be removed.");   /* sec-user-04 */
    } else if (a->page==CP_AUTOPLAY) {
        static const char *const actions[]={"Open folder to view files","Play music","View pictures","Run program","Do nothing"};
        int count=a->host.autoplay_list ? a->host.autoplay_list(a->host.ctx,a->autoplay,AP_PREFS) : -1;
        a->autoplay_count=count>=0 && count<=AP_PREFS ? (unsigned)count : 0;
        a->autoplay_selected=0;
        unsigned disks=0,optical=0;
        if (a->devices_ok) for (unsigned k=0;k<a->devices.count;k++) {
            disks+=a->devices.devices[k].class_id==CIUKI_DEVICE_DISK;
            optical+=a->devices.devices[k].class_id==CIUKI_DEVICE_CDROM;
        }
        char storage[128]; snprintf(storage,sizeof(storage),"Storage: %u disks, %u optical drives. Remembered media choices:",disks,optical);
        add(a,DU_LABEL,949,24,48,508,16,a->devices_ok ? storage : "Storage inventory unavailable. Remembered media choices:");
        for (unsigned i=0;i<a->autoplay_count;i++) {
            struct ap_preference *p=&a->autoplay[i];
            memset(&a->autoplay_items[i],0,sizeof(a->autoplay_items[i]));
            a->autoplay_items[i].text[0]=p->label[0] ? p->label : p->id;
            a->autoplay_items[i].text[1]=p->action>=0 && p->action<AP_CHOICES ? actions[p->action] : "Ask every time";
        }
        w=add(a,DU_LISTVIEW,950,24,76,508,224,""); w->items=a->autoplay_items; w->count=a->autoplay_count;
        list_style(w,"Device"); w->column_count=2;
        w->columns[0]=(struct du_column){"Device",232,0}; w->columns[1]=(struct du_column){"Action",252,0};
        du_set_text(w,count<0 ? "The registry could not be read." : "No remembered choices. AutoPlay asks every time.");
        w->anchors=DU_ANCHOR_GROW_X|DU_ANCHOR_GROW_Y;
        add(a,DU_BUTTON,951,24,316,180,24,"Ask every time")->disabled=!a->autoplay_count;
        add(a,DU_BUTTON,952,216,316,96,24,"Refresh");
        add(a,DU_LABEL,953,24,350,508,16,"Select an action in AutoPlay to remember a new choice.");
    } else if (a->page==CP_ADDHW) {
        aw_build(a);
    } else {
        system_pages(a);
    }
    if (a->page == CP_SYSTEM && a->system_tab!=6) cp_display_refresh(a);
    cp_tick(a); sync(a);
    flatten_page(a);
    /* Honor the current surface when returning from an applet after a resize. */
    int dx = u->canvas.width - CP_WIDTH, dy = u->canvas.height - CP_HEIGHT;
    for (unsigned k = 0; k < u->count; k++) {
        struct du_widget *v = &u->widgets[k];
        if (v->anchors & DU_ANCHOR_RIGHT) v->box.x += dx;
        if (v->anchors & DU_ANCHOR_BOTTOM) v->box.y += dy;
        if (v->anchors & DU_ANCHOR_GROW_X) v->box.w += dx;
        if (v->anchors & DU_ANCHOR_GROW_Y) v->box.h += dy;
    }
    a->building=0;
    /* build() replaces the whole widget list: the previous page's pixels (tabs, controls, status line) must not
     * survive under the new one. A Win98 property sheet repaints its dialog when the page changes. */
    u->dirty=1;
    du_focus(u, a->page == CP_HOME ? C_APPLETS : a->page == CP_DEVICES && !a->dm_updating ? C_TREE : a->page == CP_ADDHW ? H_AW_NEXT : C_BACK);
}
void cp_open(struct cp_app *a, int page)
{
    if (a->regional_saving) { text(a,C_STATUS,"Waiting for regional settings to finish saving..."); return; }
    if (page==CP_REGIONAL && a->page!=CP_REGIONAL) {
        load(a,&a->cfg); a->regional_edit=a->cfg.regional;
        reg_sanitize(&a->regional_edit); a->regional_tab=0;
    }
    user_wipe(a);
    if (page==CP_USERS || page==CP_PASSWORDS) a->user_loaded=0;
    /* sec-user-04: secrets never survive a page change; Passwords opens the own-password task. */
    accounts_wipe(a);
    if (page==CP_USERS || page==CP_PASSWORDS) { a->account_loaded=0; a->account_task=0; a->account_note[0]=0; a->account_password_page=page==CP_PASSWORDS; }
    a->machine_locked=a->host.self_admin && !a->host.self_admin(a->host.ctx);
    if (a->game_save) { game_note(a,"Waiting for game properties to finish saving...");return; }
    if (a->mode_state==CP_MODE_SAVE) { text(a,C_STATUS,"Waiting for display settings to finish saving..."); return; }
    if (a->display_session && page != CP_DISPLAY && page != CP_ADAPTER && page != CP_FONTS && !cp_display_leave(a)) {
        a->next_page=page; a->navigation_pending=1; return;
    }
    if (page == CP_ADAPTER) { page=CP_DISPLAY; a->display_tab=CP_SCREEN_SETTINGS; }

    /* Preview state cancels immediately; installed fonts remain owned until detach. */
    if (a->page == CP_DISPLAY && page != CP_DISPLAY) cp_preview_cancel(a);
    if (page==CP_DEVICES) a->system_tab=1;
    if (page==CP_DRIVERS) a->system_tab=3;
    if (page==CP_SYSTEM && a->system_tab!=2 && a->system_tab!=4 && a->system_tab!=5 && a->system_tab!=6) a->system_tab=0;
    aw_stop(a);
    if (page==CP_DISPLAY && !a->display_session) {
        load(a,&a->cfg); a->display_original=a->cfg; a->display_original_sounds=a->sounds; a->display_sounds_dirty=0; a->display_session=1; a->display_original_mode=0;
        cp_display_refresh(a);
        for (unsigned i=0;i<a->display.mode_count;i++) if (a->display.modes[i].current) a->display_original_mode=a->display.modes[i].id;
    }
    if(page==CP_INTERNET) { a->internet_loaded=0;a->internet_tab=0; }
    a->page = page >= 0 && page < CP_PAGES ? page : CP_HOME;
    a->status[0] = 0;
    a->dm_updating = 0;
    if (a->page == CP_ACCESS) { load(a,&a->cfg); a->access_loaded=a->cfg.access; a->ac_view=AC_V_MAIN; }
    if (a->page == CP_DRIVERS) { a->detail_list = C_DRIVERS; a->detail_index = 0; }
    if (a->page == CP_DEVICES || a->page == CP_DRIVERS || a->page == CP_SYSTEM || a->page == CP_AUTOPLAY) refresh(a);
    if (a->page == CP_POWER) pw_open(a);
    if (a->page == CP_DEVICES || a->page == CP_DRIVERS || a->page == CP_SYSTEM || a->page == CP_AUTOPLAY || a->page == CP_ADDHW) refresh(a);
    if (a->page == CP_ADDHW) aw_start(a);
    build(a);
}
int cp_wallpaper_set(struct cp_app *a, const char *path)
{
    if (!a || !path || !wallpaper_path(path) || strlen(path)>=sizeof(a->cfg.wallpaper)) return -EINVAL;
    snprintf(a->cfg.wallpaper,sizeof(a->cfg.wallpaper),"%s",path);
    a->preview_valid=0; changed(a);
    text(a,C_STATUS,"Wallpaper saved and sent to the desktop.");
    return 0;
}
void cp_assets_ready(struct cp_app *a)
{
    a->preview_valid=0;
    a->wall_count=a->host.wallpapers ? (unsigned)a->host.wallpapers(a->host.ctx,a->wall_names,CP_WALLPAPERS) : 0;
    if (a->wall_count>CP_WALLPAPERS) a->wall_count=0;
    if (a->page==CP_DISPLAY || a->page==CP_DEVICES || a->page==CP_DRIVERS || a->page==CP_SYSTEM) cp_open(a,a->page);
}
int cp_attach(struct cp_app *a, struct du_ui *u, int page)
{
    a->ui = u;
    a->page=CP_HOME;
    if (!a->cfg_loaded) cfg_defaults(&a->cfg); /* UI fallback; saves still require load() */
    load(a, &a->cfg); du_theme_use(u, a->cfg.theme);
    refresh(a); /* Boot-selected profile is part of the inventory snapshot. */
    hw_load_state(a);
    sound_defaults(&a->sounds);
    if (a->host.load_sounds) { size_t n = 0;
        if (!a->host.load_sounds(a->host.ctx, a->sound_text, sizeof(a->sound_text), &n))
            (void)sound_parse(&a->sounds, a->sound_text, n);
    }
    a->wall_count = a->host.wallpapers ? (unsigned)a->host.wallpapers(a->host.ctx, a->wall_names, CP_WALLPAPERS) : 0;
    if (a->wall_count > CP_WALLPAPERS) a->wall_count = 0;
    a->mixer_ok = a->host.mixer && !a->host.mixer(a->host.ctx, &a->volume, &a->muted);
    u->event = event; u->paint = paint; u->user = a;
    cp_open(a, page);
    return 0;
}
void cp_detach(struct cp_app *a) { du_timer_stop(a->ui,CP_T_SAVER); free(a->startup); a->startup=NULL; cp_preview_cancel(a); free(a->pack); a->pack=NULL; du_ta_free(&a->log); cp_fonts_close(a); if (a->display_session) cp_display_leave(a); cp_display_cancel(a); du_timer_stop(a->ui, CP_MODE_TIMER); desk_image_free(&a->image); }
