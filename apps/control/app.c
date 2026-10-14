/* SPDX-License-Identifier: MIT
 * Control Panel pages (control.h). Settings I/O goes through cp_host; the
 * optional Fonts page reads its trusted store through libfont. */
#include "control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    C_BACK = 100, C_APPLETS, C_TREE, C_REFRESH, C_PROPERTIES, C_DRIVERS, C_LEASES,
    C_SYSTEM_TABS, C_CPU, C_UPTIME, C_SYSTEM_HARDWARE, C_FLAGS, C_BOOT_OPTIONS, C_SERIAL, C_BOOT_APPLY,
    C_DETAILS = 130, D_PROPERTIES = 200, D_INFO, C_SCHEME, C_UI_VOL, C_UI_VOL_V
};
static const char *const page_names[CP_PAGES] = {"display", "mouse", "keyboard", "sounds", "datetime", "system", "devices", "drivers", "network", "power", "adapter", "fonts", "autoplay"};
static const char *const page_labels[CP_PAGES] = {"Display", "Mouse", "Keyboard", "Sounds", "Date & Time", "System", "Device Manager", "Drivers", "Network", "Power", "Adapter", "Fonts", "AutoPlay"};
static const char *const page_icons[CP_PAGES] = {"device:display", "device:mouse", "device:keyboard", "device:sound-card",
    "settings:date-time", "device:computer", "settings:peripherals", "system-software-update", "device:network", "system-shutdown", "device:display", "preferences-desktop-font", "drive-removable-media"};
static void build(struct cp_app *a);
static void refresh(struct cp_app *a);
static void properties(struct cp_app *a, int resources);
static void details(struct cp_app *a);
static const uint32_t swatches[6] = {0x375564u, 0x2f6f6bu, 0x3a3f6bu, 0x6b4f3au, 0x808080u, 0x101820u};
/* The Position buttons, in two rows of three: Center Tile Stretch / Fit Fill. */
#define CP_POSITIONS 5
static const int mode_order[CP_POSITIONS] = {CFG_WALL_CENTER, CFG_WALL_TILE, CFG_WALL_STRETCH, CFG_WALL_FIT, CFG_WALL_FILL};
int cp_page_index(const char *name)
{
    for (int i = 0; i < CP_PAGES; i++) if (name && !strcmp(name, page_names[i])) return i;
    return CP_HOME;
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
    text(a, C_STATUS, a->host.save_async ? "Saving settings..." : r ? "Saved. The desktop applies it at its next start." : "Saved and applied.");
}
void cp_settings_saved(struct cp_app *a, int error)
{
    if (a->mode_state==CP_MODE_SAVE) { cp_display_saved(a,error); return; }
    text(a,C_STATUS,error ? "The settings could not be saved." : "Settings saved and applied.");
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
        if (!a->image.pixels || strncmp(a->image_path, a->cfg.wallpaper, sizeof(a->image_path))) {
            desk_image_free(&a->image);
            if (a->host.decode) a->host.decode(a->host.ctx, a->cfg.wallpaper, &a->image);
            snprintf(a->image_path, sizeof(a->image_path), "%s", a->cfg.wallpaper);
        }
        uint32_t bar = sh ? (32u * 144u) / sh : 6;
        desk_wallpaper_render(a->preview, 192, 144, bar, bar, a->cfg.wallpaper, a->cfg.wallpaper_mode,
                              desktop_colour(a, chosen(a)), &a->image, a->host.portrait);
        a->preview_valid = 1;   /* no alpha: the scaled path never carried one either */
        return;
    }
    uint32_t *full = malloc((size_t)sw * sh * 4u);
    if (full) {
        desk_wallpaper_render(full, sw, sh, 32, 32, a->cfg.wallpaper, a->cfg.wallpaper_mode, desktop_colour(a, chosen(a)), &a->image, a->host.portrait);
        struct desk_image src = {full, sw, sh};
        for (size_t i = 0; i < (size_t)sw * sh; i++) full[i] |= 0xff000000u;
        desk_image_scale(&src, a->preview, 192, 144, CFG_WALL_STRETCH, desktop_colour(a, chosen(a)));
        free(full);
    } else for (size_t i = 0; i < 192 * 144; i++) a->preview[i] = desktop_colour(a, chosen(a));
    a->preview_valid = 1;
}
static void put(struct du_canvas *c, int x, int y, int w, int h, uint32_t colour) { du_fill(c, (struct du_rect){x, y, w, h}, colour); }
static void paint(struct du_ui *u, void *user)
{
    struct cp_app *a = user;
    struct du_canvas *c = &u->canvas;
    if (a->page == CP_DISPLAY) {
        if (!a->preview_valid) {
            /* An image wallpaper not decoded yet: paint the desktop colour now and
             * decode after this frame (timer), so the window never opens black
             * (reading and decoding the 2.5 MB default scene takes seconds under TCG). */
            if (a->cfg.wallpaper[0] == '/' && a->preview_pending == 0 &&   /* 0 never scheduled, 1 scheduled, 2 fired */
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
    } else if (a->page == CP_ADAPTER) cp_adapter_paint(a, c);
    else if (a->page == CP_FONTS) cp_fonts_paint(a, c);
    else if (a->page == CP_SYSTEM && a->system_tab == 0 && a->host.portrait) {
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
    struct du_widget *w;
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
    if ((w = du_get(u, C_WALL))) { w->active = wall; du_list_ensure(w, wall); }
    for (int k = 0; k < CP_POSITIONS; k++) if ((w = du_get(u, C_POS0 + k))) w->def = mode_order[k] == a->cfg.wallpaper_mode;
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
    for (unsigned k = 0; k < SOUND_EVENTS; k++) a->sound_items[k].selected = k == a->sound_selected;
    if ((w = du_get(u,C_SOUND_LIST))) w->active = a->sound_selected;
    if ((w = du_get(u,C_SOUND_ENABLED))) w->value = a->sounds.enabled;
    struct sound_scheme defaults; sound_defaults(&defaults);
    unsigned scheme = !a->sounds.enabled ? 1 : memcmp(defaults.file,a->sounds.file,sizeof(defaults.file)) ? 2 : 0;
    for (unsigned k = 0; k < 3; k++) a->scheme_items[k].selected = k == scheme;
    if ((w = du_get(u,C_SCHEME))) w->active = scheme;
    if ((w = du_get(u,C_UI_VOL))) w->value = a->sounds.volume;
    snprintf(a->sound_volume_label,sizeof(a->sound_volume_label),"%d%%",a->sounds.volume);
    text(a,C_UI_VOL_V,a->sound_volume_label);
    for (unsigned k = 0; k < SOUND_EVENTS; k++) a->sound_items[k].text[1] = a->sounds.file[k][0] ? a->sounds.file[k] : "(None)";
    if ((w = du_get(u,C_HARDWARE))) w->disabled = a->audio_testing;
    toggle_label(a, C_H24, "24-hour clock", a->cfg.clock_24h);
    toggle_label(a, C_SECONDS, "Show seconds", a->cfg.clock_seconds);
    cp_display_sync(a);
    u->dirty = 1;
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
void cp_tick(struct cp_app *a)
{
    if (a->page == CP_SOUNDS && a->host.mixer && a->ui->capture < 0) {
        int volume, mute;
        if (!a->host.mixer(a->host.ctx, &volume, &mute) &&
            (a->volume != volume || a->muted != mute)) {
            a->volume = volume; a->muted = mute; sync(a); a->ui->dirty = 1;
        }
    }
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
    if (a->devices_ok && a->page == CP_SYSTEM) {
        uint64_t seconds = a->devices.uptime_ms / 1000;
        uint64_t current = a->host.monotonic ? a->host.monotonic(a->host.ctx) : a->ui->now;
        if (current >= a->snapshot_time) seconds += (current - a->snapshot_time) / 1000000000ull;
        char uptime[96]; snprintf(uptime, sizeof(uptime), "Uptime: %llu days, %02u:%02u:%02u", (unsigned long long)(seconds / 86400),
            (unsigned)(seconds / 3600 % 24), (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60));
        text(a, C_UPTIME, uptime);
    }
}
static void event(struct du_ui *u, const struct du_event *e, void *user)
{
    struct cp_app *a = user;
    int save = 0;
    if (cp_display_event(a, e)) return;
    if (cp_fonts_event(a, e)) return;
    if (e->type == DU_EV_TICK && e->id == CP_T_PREVIEW) {   /* deferred wallpaper decode */
        a->preview_pending = 2; a->preview_valid = 0; u->dirty = 1;
        return;
    }
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
    case C_BACK: if (e->type == DU_EV_ACTIVATE) cp_open(a, CP_HOME); break;
    case C_APPLETS: if (e->type == DU_EV_OPEN && e->index >= 0 && e->index < CP_PAGES) cp_open(a, e->index); break;
    case C_DRIVERS: case C_LEASES:
        if (e->type == DU_EV_SELECT || e->type == DU_EV_OPEN) {
            a->detail_list = e->id; a->detail_index = e->index < 0 ? 0 : (unsigned)e->index;
            if (e->type == DU_EV_OPEN) details(a);
        }
        break;
    case C_DETAILS: if (e->type == DU_EV_ACTIVATE) details(a); break;
    case C_SYSTEM_TABS:
        if (e->type == DU_EV_SELECT && e->index >= 0 && e->index < 3) { a->system_tab = e->index; build(a); }
        break;
    case C_REFRESH: if (e->type == DU_EV_ACTIVATE) { refresh(a); build(a); } break;
    case C_TREE:
        if (e->type == DU_EV_SELECT && e->index >= 0 && (unsigned)e->index < a->tree_count) {
            int index = a->tree_device[e->index];
            a->selected_device_id = index >= 0 ? a->devices.devices[index].id : 0;
            du_get(u, C_PROPERTIES)->disabled = !a->devices_ok || index < 0;
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
    case C_CLOSE: if (e->type == DU_EV_ACTIVATE) { if (a->mode_state!=CP_MODE_SAVE) cp_display_cancel(a); a->quit = 1; } break;
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
            } else break;
            save = 1; a->preview_valid = 0;
        }
        break;
    case C_POS0: case C_POS1: case C_POS2: case C_POS3: case C_POS4:
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
    case C_SOUND_ENABLED: case C_SCHEME: case C_UI_VOL:
        if ((e->id == C_SCHEME && e->type == DU_EV_SELECT && e->index >= 0 && e->index < 2) ||
            (e->id == C_UI_VOL && e->type == DU_EV_CHANGE && e->index >= 0 && e->index <= 100) ||
            (e->id == C_SOUND_ENABLED && (e->type == DU_EV_CHANGE || e->type == DU_EV_ACTIVATE))) {
            struct sound_scheme next = a->sounds;
            if (e->id == C_SCHEME) {
                if (!e->index) { int volume = next.volume; sound_defaults(&next); next.volume = volume; }
                else next.enabled = 0;
            } else if (e->id == C_UI_VOL) next.volume = e->index;
            else next.enabled = !next.enabled;
            size_t n = sound_format(&next,a->sound_text,sizeof(a->sound_text));
            if (!n || !a->host.save_sounds || a->host.save_sounds(a->host.ctx,a->sound_text,n)) text(a,C_STATUS,"Sound settings could not be saved.");
            else {
                a->sounds = next;
                int r = a->host.apply ? a->host.apply(a->host.ctx,DESK_SETTINGS_SOUND_RELOAD,0,0) : -1;
                text(a,C_STATUS,r ? "Saved. Restart to apply sound settings." : "Sound settings saved and applied.");
            }
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
            !memchr(d->driver, 0, sizeof(d->driver)) || !memchr(d->reason, 0, sizeof(d->reason))) return 0;
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
    /* Display names only; never used to select a driver. PCI IDs checked
     * against Linux pci_ids.h and pciutils pci.ids (device-manager.md). */
    static const struct { unsigned vendor, device; const char *name; } table[] = {
        {0x8086, 0x1237, "Intel 440FX host bridge"}, {0x8086, 0x7190, "Intel 440BX host bridge"},
        {0x8086, 0x3575, "Intel 830MP host bridge"}, {0x8086, 0x7000, "Intel PIIX ISA bridge"},
        {0x8086, 0x7010, "Intel PIIX IDE controller"}, {0x8086, 0x7110, "Intel PIIX4 ISA bridge"},
        {0x8086, 0x7111, "Intel PIIX4 IDE controller"}, {0x8086, 0x2415, "Intel ICH AC97 audio"},
        {0x8086, 0x2485, "Intel ICH3 AC97 audio"}, {0x8086, 0x1229, "Intel PRO/100 Ethernet"},
        {0x5333, 0x8c2e, "S3 SuperSavage/IXC"}, {0x1002, 0x4c4d, "ATI Rage Mobility"},
        {0x1013, 0x00b8, "Cirrus Logic GD5446"}, {0x1234, 0x1111, "Bochs VBE display"},
        {0x10ec, 0x8139, "Realtek RTL8139 Ethernet"}, {0x10b7, 0x9055, "3Com 3C905B Ethernet"},
        {0x1106, 0x3099, "VIA VT8366 host bridge"}, {0x1106, 0x3074, "VIA VT8233 ISA bridge"},
    };
    if (d->bus_type == CIUKI_BUS_PCI) {
        if (a->host.hwid_name && !a->host.hwid_name(a->host.ctx, d->vendor, d->device, resolved, size) && resolved[0])
            return resolved;
        for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++)
            if (table[i].vendor == d->vendor && table[i].device == d->device) return table[i].name;
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
            snprintf(a->device_names[k], sizeof(a->device_names[k]), "%s%s", product(a, d, resolved, sizeof(resolved)),
                     d->state == CIUKI_DEVICE_QUARANTINED ? " (quarantined)" : d->state == CIUKI_DEVICE_ABSENT ? " (absent)" : "");
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
    snprintf(title, sizeof(title), "%s Properties", product(a, d, resolved_title, sizeof(resolved_title)));
    if (!resources) {
        char resolved[65]; const char *name = product(a, d, resolved, sizeof(resolved));
        at = (size_t)snprintf(body, sizeof(body), "General\n\n%s\nClass: %s\nDriver: %s\nStatus: %s\n",
            name, cp_device_class(d->class_id), d->driver[0] ? d->driver : "None bound", cp_device_state(d->state));
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
        snprintf(body + at, sizeof(body) - at, "Activation sequence: %u   Generation: %u", d->activation_seq, d->generation);
    } else {
        char resolved[65]; const char *name = product(a, d, resolved, sizeof(resolved));
        at = (size_t)snprintf(body, sizeof(body), "Resources\n\n%s\n", name);
        for (unsigned k = 0; k < d->resource_count && at + 80 < sizeof(body); k++) {
            const struct ciuki_device_resource *r = &d->resources[k]; char value[64];
            range(value, sizeof(value), r->type, r->start, r->end, r->flags);
            at += (size_t)snprintf(body + at, sizeof(body) - at, "%s: %s\n", resource_name(r->type), value);
        }
        snprintf(body + at, sizeof(body) - at, "%s", d->flags & CIUKI_DEVICES_TRUNCATED ? "More resources in the lease list." :
                 d->resource_count ? "Assigned resources are read-only." : "No resource assignment recorded.");
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
    if (a->page == CP_DEVICES) {
        w = add(a, DU_TREE, C_TREE, 24, 40, 452, 270, ""); w->inactive_selection = 1;
        w->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_GROW_Y;
        add(a, DU_BUTTON, C_PROPERTIES, 24, 320, 110, 24, "Properties...")->anchors = DU_ANCHOR_BOTTOM;
        cp_tree_build(a);
        add(a, DU_BUTTON, C_REFRESH, 366, 320, 110, 24, "Refresh")->anchors = DU_ANCHOR_RIGHT | DU_ANCHOR_BOTTOM;
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
    } else if (a->page == CP_SYSTEM) {
        static const char *const tabs[] = {"General", "Hardware", "Advanced"};
        for (int k = 0; k < 3; k++) a->system_tabs[k].text[0] = tabs[k];
        /* System uses its own tabs instead of the applet's single title tab. */
        w = du_get(u, C_TABS); w->id = C_SYSTEM_TABS; w->items = a->system_tabs; w->count = 3; w->active = (unsigned)a->system_tab;
        if (a->system_tab == 0) {
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
        } else if (a->system_tab == 1) {
            add(a, DU_LABEL, C_SYS0, 24, 44, 452, 14, "Hardware summary");
            for (unsigned cls = 0; cls < CIUKI_DEVICE_CLASSES; cls++) {
                unsigned count = 0, ready = 0;
                if (a->devices_ok) for (unsigned k = 0; k < a->devices.count; k++) if (a->devices.devices[k].class_id == cls) {
                    count++; if (a->devices.devices[k].state == CIUKI_DEVICE_READY) ready++;
                }
                snprintf(line, sizeof(line), "%s: %u listed, %u ready", cp_device_class(cls), count, ready);
                add(a, DU_LABEL, 300 + (int)cls, 24, 76 + (int)cls * 24, 452, 14, a->devices_ok ? line : "Inventory unavailable.");
            }
        } else {
            snprintf(line, sizeof(line), "Current boot flags: 0x%08X", a->devices.boot_flags); add(a, DU_LABEL, C_FLAGS, 24, 44, 452, 14, a->devices_ok ? line : "Boot flags unavailable.");
            snprintf(line, sizeof(line), "Input policy: %s", a->devices.input_policy ? "firmware-first" : "native"); add(a, DU_LABEL, C_SYS0, 24, 66, 452, 14, a->devices_ok ? line : "Input policy unavailable.");
            add(a, DU_LABEL, C_SYS1, 24, 94, 452, 14, "Boot options (read-only):");
            add(a, DU_LABEL, C_BOOT_OPTIONS, 24, 116, 452, 14, a->devices_ok ? a->devices.boot_options : "Unavailable");
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
            w = add(a, DU_CHECKBOX, C_SERIAL, 24, 174, 452, 24, "Serial log at next boot"); w->value = a->serial_next; w->disabled = !a->boot_ok;
            w = add(a, DU_BUTTON, C_BOOT_APPLY, 24, 212, 140, 24, "Apply to BOOT.CFG"); w->disabled = !a->boot_ok || !a->host.boot_save;
            add(a, DU_LABEL, C_SYS2, 24, 254, 452, 14, "This choice takes effect after restarting CiukiOS.");
            if (!a->boot_ok) add(a, DU_LABEL, C_SYS3, 24, 280, 452, 14, "BOOT.CFG is unavailable or exceeds the loader limit.");
        }
    } else if (a->page == CP_ADAPTER) {
        add(a, DU_BUTTON, CP_REFRESH, 24, 324, 84, 24, "Refresh");
    } else if (a->page == CP_NETWORK) {
        add(a, DU_LABEL, C_SYS0, 24, 48, 452, 14, "Network settings will be available in F6.");
        add(a, DU_LABEL, C_SYS1, 24, 76, 452, 14, "Detected adapters are listed in Device Manager.");
    } else if (a->page == CP_POWER) {
        add(a, DU_LABEL, C_SYS0, 24, 48, 452, 14, "Power and restart");
        add(a, DU_LABEL, C_SYS1, 24, 82, 452, 14, "Use Start > Shut Down to restart or turn off this computer.");
        add(a, DU_LABEL, C_SYS2, 24, 110, 452, 14, "CiukiOS closes programs and flushes disks before stopping.");
        add(a, DU_LABEL, C_SYS3, 24, 148, 452, 14, "Standby and automatic power saving are not available yet.");
    }
}

static void build(struct cp_app *a)
{
    struct du_ui *u = a->ui;
    du_release(u); du_dialog_close(u);
    u->count = 0; u->focus = -1;
    struct du_widget *w;
    if (a->page == CP_HOME) {
        add(a, DU_LABEL, C_TABS, 12, 12, 476, 16, "Control Panel");
        for (int k = 0; k < CP_PAGES; k++) {
            struct du_item *it = &a->applet_items[k]; memset(it, 0, sizeof(*it));
            it->text[0] = page_labels[k]; it->icon16 = du_icon_argb(du_icon_name(page_icons[k]), 16);
            it->icon32 = du_icon_argb(du_icon_name(page_icons[k]), 32);
        }
        w = add(a, DU_LISTVIEW, C_APPLETS, 8, 36, CP_WIDTH - 16, CP_HEIGHT - 84, "");
        w->items = a->applet_items; w->count = CP_PAGES; w->view = DU_ICONS;
        w->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_GROW_Y;
    } else {
        a->tab_items[0].text[0] = page_labels[a->page];
        w = add(a, DU_TABS, C_TABS, 8, 8, CP_WIDTH - 16, CP_HEIGHT - 64, "");
        w->items = a->tab_items; w->count = 1;
        w->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_GROW_Y;
        add(a, DU_BUTTON, C_BACK, 12, CP_HEIGHT - 34, 110, 24, "Control Panel")->anchors = DU_ANCHOR_BOTTOM;
    }
    add(a, DU_BUTTON, C_CLOSE, CP_WIDTH - 88, CP_HEIGHT - 34, 80, 24, "Close")->anchors = DU_ANCHOR_RIGHT | DU_ANCHOR_BOTTOM;
    add(a, DU_LABEL, C_STATUS, 12, CP_HEIGHT - 52, 476, 16, a->status)->anchors = DU_ANCHOR_GROW_X | DU_ANCHOR_BOTTOM;
    if (a->page == CP_HOME) text(a, C_STATUS, "Open an applet to change its settings.");
    if (a->page == CP_DISPLAY) {
    /* Display */
    add(a, DU_LABEL, C_THEME_L, 248, 40, 200, 14, "&Theme:");
    for (int k = 0; k < CFG_THEMES; k++) { memset(&a->theme_items[k], 0, sizeof(a->theme_items[k])); a->theme_items[k].text[0] = cfg_theme_names[k]; }
    w = add(a, DU_LISTVIEW, C_THEME, 248, 56, 228, 72, ""); w->items = a->theme_items; w->count = CFG_THEMES; list_style(w, "Theme");
    add(a, DU_LABEL, C_WALL_L, 248, 136, 200, 14, "&Wallpaper:");
    unsigned walls = 2 + DESK_BUILTIN_WALLPAPERS + a->wall_count;
    items(a->wall_items, a->wall_labels, walls, wall_label, a);
    for (unsigned k = 0; k < walls; k++) a->wall_items[k].icon16 = NULL;
    w = add(a, DU_LISTVIEW, C_WALL, 248, 152, CP_WIDTH - 272, 22 + 2 * DU_ROW, ""); w->items = a->wall_items; w->count = walls; list_style(w, "Wallpaper");
    add(a, DU_LABEL, C_POS_L, 24, 224, 100, 14, "Position:");
    /* Under the 208 px monitor: three 68 px buttons per row, 2 px apart. */
    static const char *const pos[CP_POSITIONS] = {"Center", "Tile", "Stretch", "Fit", "Fill"};
    for (int k = 0; k < CP_POSITIONS; k++) add(a, DU_BUTTON, C_POS0 + k, 24 + k % 3 * 70, 240 + k / 3 * 24, 68, 22, pos[k]);
    add(a, DU_LABEL, C_COL_L, 24, 296, 100, 14, "Desktop colour:");
    for (int k = 0; k < 6; k++) add(a, DU_BUTTON, C_COL0 + k, 24 + k * 34, 312, 30, 24, "");
    add(a, DU_BUTTON, C_ICONS, 248, 212, CP_WIDTH - 272, 24, "");
    add(a, DU_LABEL, CP_AREA_LABEL, 248, 240, CP_WIDTH - 272, 14, "Screen area:");
    w = add(a, DU_LISTVIEW, CP_AREA, 248, 256, CP_WIDTH - 272, 22 + DU_ROW, ""); w->items = a->area_items; list_style(w, "Resolution");
    add(a, DU_LABEL, CP_DEPTH_LABEL, 248, 306, CP_WIDTH - 272, 14, "Colours:");
    w = add(a, DU_LISTVIEW, CP_DEPTH, 248, 322, CP_WIDTH - 272, 22 + DU_ROW, ""); w->items = a->depth_items; list_style(w, "Colour depth");
    add(a, DU_BUTTON, CP_APPLY, 24, 338, 84, 24, "Apply");
    add(a, DU_LABEL, CP_MODE_NOTE, 116, 342, 116, 14, "");
    } else if (a->page == CP_MOUSE) {
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
    } else if (a->page == CP_KEYBOARD) {
    /* Keyboard */
    add(a, DU_LABEL, C_KEY_L, 24, 44, 200, 14, "Keyboard &layout:");
    for (int k = 0; k < CFG_KEYS_COUNT; k++) { memset(&a->key_items[k], 0, sizeof(a->key_items[k])); a->key_items[k].text[0] = cfg_keyboard_labels[k]; }
    w = add(a, DU_LISTVIEW, C_KEY, 24, 60, 220, 56, ""); w->items = a->key_items; w->count = CFG_KEYS_COUNT; list_style(w, "Layout");
    add(a, DU_LABEL, C_KEY_NOTE1, 24, 126, 452, 14, "The Run dialog uses the new layout at once; programs started");
    add(a, DU_LABEL, C_KEY_NOTE2, 24, 140, 452, 14, "afterwards receive it in CIUKI_KEYMAP.");
    } else if (a->page == CP_SOUNDS) {
    /* Sounds */
    add(a, DU_LABEL, C_VOL_L, 24, 44, 180, 14, "Master &volume:");
    w = add(a, DU_SCROLLBAR, C_VOL, 24, 62, 160, 17, ""); w->vertical = 0; w->total = 101; w->page = 1;
    add(a, DU_LABEL, C_VOL_V, 194, 62, 48, 17, "");
    add(a, DU_LABEL, 898, 270, 44, 180, 14, "UI sounds volume:");
    w = add(a, DU_SCROLLBAR, C_UI_VOL, 270, 62, 150, 17, ""); w->vertical = 0; w->total = 101; w->page = 1;
    add(a, DU_LABEL, C_UI_VOL_V, 430, 62, 46, 17, "");
    add(a, DU_BUTTON, C_MUTE, 24, 88, 140, 24, "");
    add(a, DU_CHECKBOX, C_SOUND_ENABLED, 184, 88, 290, 24, "Play system sounds");
    static const char *const schemes[] = {"Default", "No sounds", "Custom"};
    for (unsigned k = 0; k < 3; k++) a->scheme_items[k].text[0] = schemes[k];
    w = add(a,DU_TABS,C_SCHEME,24,124,452,28,""); w->items = a->scheme_items; w->count = 3;
    for (unsigned k = 0; k < SOUND_EVENTS; k++) {
        a->sound_items[k].text[0] = sound_labels[k]; a->sound_items[k].text[1] = a->sounds.file[k];
    }
    w = add(a,DU_LISTVIEW,C_SOUND_LIST,24,176,452,110,""); w->items = a->sound_items; w->count = SOUND_EVENTS;
    list_style(w,"Event"); w->column_count = 2;
    w->columns[0] = (struct du_column){"Event",170,0}; w->columns[1] = (struct du_column){"Assigned sound",258,0};
    add(a,DU_BUTTON,C_SOUND_TEST,24,294,90,24,"Preview");
    add(a,DU_BUTTON,C_HARDWARE,124,294,220,24,"Test audio hardware");
    add(a,DU_LABEL,C_AUDIO,24,326,452,14,"");
    } else if (a->page == CP_DATETIME) {
    /* Date & Time */
    add(a, DU_LABEL, C_DATE, 24, 106, 452, 14, "");
    add(a, DU_BUTTON, C_H24, 24, 134, 200, 24, "");
    add(a, DU_BUTTON, C_SECONDS, 24, 164, 200, 24, "");
    add(a, DU_LABEL, C_TIME_NOTE1, 24, 202, 452, 14, "The clock reads the computer's real-time clock (UTC).");
    add(a, DU_LABEL, C_TIME_NOTE2, 24, 216, 452, 14, "Setting it is not available yet: the kernel has no call for it.");
    } else if (a->page==CP_AUTOPLAY) {
        static const char *const actions[]={"Open folder to view files","Play music","View pictures","Run program","Do nothing"};
        int count=a->host.autoplay_list ? a->host.autoplay_list(a->host.ctx,a->autoplay,AP_PREFS) : -1;
        a->autoplay_count=count>=0 && count<=AP_PREFS ? (unsigned)count : 0;
        a->autoplay_selected=0;
        add(a,DU_LABEL,949,24,48,508,16,"Remembered choices for removable media and CDs");
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
    } else {
        system_pages(a);
    }
    if (a->page == CP_FONTS) cp_fonts_build(a);
    if (a->page == CP_DISPLAY || a->page == CP_ADAPTER || a->page == CP_SYSTEM) cp_display_refresh(a);
    cp_tick(a); sync(a);
    if (a->page == CP_DISPLAY) {
        du_get(u, C_THEME)->anchors = du_get(u, C_WALL)->anchors = DU_ANCHOR_GROW_X;
        du_get(u, C_ICONS)->anchors = DU_ANCHOR_GROW_X;
        du_get(u, CP_AREA)->anchors = du_get(u, CP_DEPTH)->anchors = DU_ANCHOR_GROW_X;
    }
    /* Honor the current surface when returning from an applet after a resize. */
    int dx = u->canvas.width - CP_WIDTH, dy = u->canvas.height - CP_HEIGHT;
    for (unsigned k = 0; k < u->count; k++) {
        struct du_widget *v = &u->widgets[k];
        if (v->anchors & DU_ANCHOR_RIGHT) v->box.x += dx;
        if (v->anchors & DU_ANCHOR_BOTTOM) v->box.y += dy;
        if (v->anchors & DU_ANCHOR_GROW_X) v->box.w += dx;
        if (v->anchors & DU_ANCHOR_GROW_Y) v->box.h += dy;
    }
    du_focus(u, a->page == CP_HOME ? C_APPLETS : a->page == CP_DEVICES ? C_TREE : C_BACK);
}
void cp_open(struct cp_app *a, int page)
{
    if (a->mode_state==CP_MODE_SAVE) { text(a,C_STATUS,"Waiting for display settings to finish saving..."); return; }
    if (page != a->page && a->mode_state) cp_display_cancel(a);
    if (a->page == CP_FONTS && page != CP_FONTS) cp_fonts_close(a);
    a->page = page >= 0 && page < CP_PAGES ? page : CP_HOME;
    a->status[0] = 0;
    if (a->page == CP_DRIVERS) { a->detail_list = C_DRIVERS; a->detail_index = 0; }
    if (a->page == CP_DEVICES || a->page == CP_DRIVERS || a->page == CP_SYSTEM) refresh(a);
    build(a);
}
int cp_attach(struct cp_app *a, struct du_ui *u, int page)
{
    a->ui = u;
    load(a, &a->cfg); du_theme_use(u, a->cfg.theme);
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
void cp_detach(struct cp_app *a) { cp_fonts_close(a); cp_display_cancel(a); du_timer_stop(a->ui, CP_MODE_TIMER); desk_image_free(&a->image); }
