/* SPDX-License-Identifier: MIT
 * desktop.cfg parser and writer (settings.h). Integer-only, no I/O. */
#include "settings.h"
#include "theme.h"
#include "wallpaper.h"
#include <stdio.h>
#include <string.h>

const char *const desk_wallpaper_pattern_names[DESK_WALL_PATTERNS]={"none","bricks","buttons","checkerboard","circles","tiles"};
const char *const desk_wallpaper_pattern_labels[DESK_WALL_PATTERNS]={"None","Bricks","Buttons","Checkerboard","Circles","Tiles"};

const char *const cfg_theme_names[CFG_THEMES] = {"Platinum", "Ciuki Blue"};
const char *const cfg_mode_names[CFG_WALL_MODES] = {"center", "stretch", "tile", "fit", "fill"};
const char *const cfg_keyboard_names[CFG_KEYS_COUNT] = {"us", "it"};
const char *const cfg_keyboard_labels[CFG_KEYS_COUNT] = {"English (US)", "Italian"};

/* Preserve the selected pre-existing skin when the first edit is an Effect
 * or DPI change. Background colour's historical sentinel follows the skin. */
static void appearance_from_skin(struct ciuki_settings *s)
{
    const struct desk_theme *t=desk_theme_named(s->theme);
    if(!t) t=desk_theme_named(DESK_THEME_DEFAULT);
    ap_default(&s->appearance,0);
    struct ap_item *v=s->appearance.item;
    v[AP_DESKTOP].colour=s->desktop_color==CFG_DESKTOP_COLOR ? t->desktop : s->desktop_color;
    v[AP_ACTIVE_TITLE].colour=t->title[0]; v[AP_ACTIVE_TITLE].gradient=t->title[1]; v[AP_ACTIVE_TITLE].text=t->title_text;
    v[AP_ACTIVE_TITLE].font=t->font_title;
    v[AP_INACTIVE_TITLE].colour=t->title_inactive[0]; v[AP_INACTIVE_TITLE].gradient=t->title_inactive[1]; v[AP_INACTIVE_TITLE].text=t->title_text_inactive;
    v[AP_3D].colour=v[AP_MENU].colour=v[AP_MESSAGE].colour=t->face;
    v[AP_3D].text=v[AP_MENU].text=v[AP_MESSAGE].text=v[AP_WINDOW].text=t->ink;
    v[AP_WINDOW].colour=t->window; v[AP_DISABLED].text=t->gray;
    v[AP_ACTIVE_BORDER].colour=t->frame; v[AP_INACTIVE_BORDER].colour=t->frame_inactive;
    v[AP_SELECTED].colour=t->select; v[AP_SELECTED].text=t->select_ink;
    v[AP_ICON].text=0xffffff;
}
void cfg_defaults(struct ciuki_settings *s)
{
    memset(s, 0, sizeof(*s));
    reg_defaults(&s->regional, 0);
    ap_default(&s->appearance,5);
    strcpy(s->scheme,"Ciuki Blue"); strcpy(s->desktop_theme_name,"Ciuki Blue");
    strcpy(s->theme, DESK_THEME_DEFAULT);
    strcpy(s->wallpaper, "/system/wallpapers/Ciuk2.png");   /* owner's Ciuk2 scene (assets/wallpapers/ciuki) */
    s->wallpaper_mode = CFG_WALL_FILL;     /* the adaptive scenes are made for a centred, aspect-preserving fill */
    s->desktop_color = CFG_DESKTOP_COLOR;          /* DESK_BACKGROUND */
    s->clock_24h = 1;
    s->taskbar_clock = 1;
    s->quick_launch = 1;
    s->taskbar_top = 1;
    s->taskbar_size = 32;
    s->desktop_icons = 1;
    s->mouse_spi_speed=10;
    s->icon_auto_arrange = 0;
    s->icon_sort = 0;
    s->icon_spacing_x = 96; s->icon_spacing_y = 94; s->icon_font = DESK_FONT_LABEL;
    s->mouse_speed = CFG_MOUSE_NORMAL;
    s->double_click_ms = 500;
    s->keyboard = CFG_KEYS_US;
    s->repeat_delay = 1; s->repeat_speed = 31;
    s->cursor_blink_ms = 530; s->wheel_lines = 3; s->layout_switch = 1;
    s->saver_seconds=600; s->saver_kind=1;
    access_defaults(&s->access);
    appearance_from_skin(s);
}

const char *cfg_effective_theme(const struct ciuki_settings *s)
{ return (s->access.contrast & ACC_ON) && access_scheme_valid(s->access.scheme) ? s->access.scheme : s->theme; }

int cfg_theme_index(const char *name)
{
    for (int i = 0; i < CFG_THEMES; i++) if (!strcmp(name, cfg_theme_names[i])) return i;
    return -1;
}

int cfg_mouse_numerator(int speed)
{
    /* 1..10 -> 2/8 .. 20/8; 5 is exactly 1:1. */
    static const int table[11] = {8, 2, 3, 4, 6, 8, 10, 12, 14, 17, 20};
    return speed >= CFG_MOUSE_MIN && speed <= CFG_MOUSE_MAX ? table[speed] : 8;
}

static int number(const char *v, int lo, int hi, int *out)
{
    long n = 0;
    if (!*v) return 0;
    for (; *v; v++) {
        if (*v < '0' || *v > '9' || (*v-'0')>hi || n>(hi-(*v-'0'))/10) return 0;
        n = n * 10 + (*v - '0');
    }
    if (n < lo || n > hi) return 0;
    *out = (int)n;
    return 1;
}

static int flag(const char *v, int *out)
{
    if (!strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "on")) { *out = 1; return 1; }
    if (!strcmp(v, "0") || !strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "off")) { *out = 0; return 1; }
    return 0;
}

static int printable(const char *v)
{
    for (; *v; v++) if ((unsigned char)*v < 32 || *v == 127) return 0;
    return 1;
}

static int wallpaper_value(const char *v)
{
    if (!strcmp(v, "ciuki") || !strcmp(v, "none")) return 1;
    if (!strncmp(v, "pattern:", 8) && v[8] >= '0' && v[8] <= '7' && !v[9]) return 1;
    if (!strncmp(v, "builtin:", 8)) return v[8] != 0 && strlen(v) < 48;
    return v[0] == '/' && !strstr(v, "/../") && printable(v);
}

static void extra_add(struct ciuki_settings *s, const char *line)
{
    size_t used = strlen(s->extra), n = strlen(line);
    if (used + n + 2 > sizeof(s->extra)) return;
    memcpy(s->extra + used, line, n);
    s->extra[used + n] = '\n';
    s->extra[used + n + 1] = 0;
}


/* ---- G25 access_* keys ---------------------------------------------------
 * access_sticky=<flags>, access_filter=<flags>,<wait>,<delay>,<repeat>,<bounce>,
 * access_toggle=<flags>, access_mouse=<flags>,<max>,<time>,<ctrl>,
 * access_sentry=<flags>,<effect>, access_contrast=<flags>,<scheme>,
 * access_timeout=<flags>,<ms>, access_general=<showsounds>,<status>,<pointer>,
 * access_magnifier=<on>,<level>,<follow>,<invert>,<dock>,<percent>.
 * Flags are hexadecimal Win32 values (access.h); the others decimal. */
static int hex_field(const char **p, uint32_t mask, uint32_t *out)
{
    const char *v = *p; uint32_t n = 0; int digits = 0;
    for (; (*v >= '0' && *v <= '9') || (*v >= 'a' && *v <= 'f') || (*v >= 'A' && *v <= 'F'); v++, digits++) {
        n = n << 4 | (uint32_t)(*v <= '9' ? *v - '0' : (*v | 32) - 'a' + 10);
        if (digits >= 8) return 0;
    }
    if (!digits) return 0;
    *out = n & mask; *p = v; return 1;
}
static int dec_field(const char **p, uint32_t lo, uint32_t hi, uint32_t *out)
{
    const char *v = *p; uint32_t n = 0; int digits = 0;
    for (; *v >= '0' && *v <= '9'; v++, digits++) { n = n * 10 + (uint32_t)(*v - '0'); if (digits >= 9) return 0; }
    if (!digits || n < lo || n > hi) return 0;
    *out = n; *p = v; return 1;
}
static int comma(const char **p) { if (**p != ',') return 0; (*p)++; return 1; }
static int access_assign(struct access_settings *a, const char *key, const char *v)
{
    uint32_t n[6];
    if (!strcmp(key, "access_sticky")) { if (!hex_field(&v, 0x1ffu, &n[0]) || *v) return 0; a->sticky = n[0]; return 1; }
    if (!strcmp(key, "access_toggle")) { if (!hex_field(&v, 0x3fu, &n[0]) || *v) return 0; a->toggle = n[0]; return 1; }
    if (!strcmp(key, "access_filter")) {
        if (!hex_field(&v, 0x107fu, &n[0]) || !comma(&v) || !dec_field(&v, 0, ACC_FILTER_MAX_MS, &n[1]) || !comma(&v) ||
            !dec_field(&v, 0, ACC_FILTER_MAX_MS, &n[2]) || !comma(&v) || !dec_field(&v, 0, ACC_FILTER_MAX_MS, &n[3]) || !comma(&v) ||
            !dec_field(&v, 0, ACC_FILTER_MAX_MS, &n[4]) || *v) return 0;
        a->filter = n[0]; a->filter_wait = n[1]; a->filter_delay = n[2]; a->filter_repeat = n[3]; a->filter_bounce = n[4];
        return 1;
    }
    if (!strcmp(key, "access_mouse")) {
        if (!hex_field(&v, 0xffu, &n[0]) || !comma(&v) || !dec_field(&v, ACC_MOUSE_MAX_SPEED_MIN, ACC_MOUSE_MAX_SPEED_MAX, &n[1]) || !comma(&v) ||
            !dec_field(&v, ACC_MOUSE_TIME_MIN, ACC_MOUSE_TIME_MAX, &n[2]) || !comma(&v) || !dec_field(&v, 1, 32, &n[3]) || *v) return 0;
        a->mouse = n[0]; a->mouse_max = n[1]; a->mouse_time = n[2]; a->mouse_ctrl = n[3];
        return 1;
    }
    if (!strcmp(key, "access_sentry")) {
        if (!hex_field(&v, 0x7u, &n[0]) || !comma(&v) || !dec_field(&v, 0, ACC_SS_MAX, &n[1]) || *v) return 0;
        a->sentry = n[0]; a->sentry_effect = n[1]; return 1;
    }
    if (!strcmp(key, "access_contrast")) {
        if (!hex_field(&v, 0x7fu, &n[0]) || !comma(&v) || !access_scheme_valid(v)) return 0;
        a->contrast = n[0]; snprintf(a->scheme, sizeof(a->scheme), "%s", v); return 1;
    }
    if (!strcmp(key, "access_timeout")) {
        if (!hex_field(&v, 0x3u, &n[0]) || !comma(&v) || !dec_field(&v, ACC_TIMEOUT_MIN_MS / 1000, ACC_TIMEOUT_MAX_MS / 1000, &n[1]) || *v) return 0;
        a->timeout = n[0]; a->timeout_ms = n[1] * 1000u; return 1;
    }
    if (!strcmp(key, "access_general")) {
        if (!dec_field(&v, 0, 1, &n[0]) || !comma(&v) || !dec_field(&v, 0, 1, &n[1]) || !comma(&v) || !dec_field(&v, 0, ACC_POINTERS - 1, &n[2]) || *v) return 0;
        a->show_sounds = n[0]; a->status = n[1]; a->pointer = n[2]; return 1;
    }
    if (!strcmp(key, "access_magnifier")) {
        if (!dec_field(&v, 0, 1, &n[0]) || !comma(&v) || !dec_field(&v, ACC_MAG_LEVEL_MIN, ACC_MAG_LEVEL_MAX, &n[1]) || !comma(&v) ||
            !dec_field(&v, 0, 7, &n[2]) || !comma(&v) || !dec_field(&v, 0, 1, &n[3]) || !comma(&v) || !dec_field(&v, 0, 1, &n[4]) || !comma(&v) ||
            !dec_field(&v, ACC_MAG_PERCENT_MIN, ACC_MAG_PERCENT_MAX, &n[5]) || *v) return 0;
        a->mag_on = n[0]; a->mag_level = n[1]; a->mag_follow = n[2]; a->mag_invert = n[3]; a->mag_dock = n[4]; a->mag_percent = n[5];
        return 1;
    }
    return -1;
}
static int access_key_known(const char *key) { return !strncmp(key, "access_", 7); }

static int assign(struct ciuki_settings *s, const char *key, const char *v, int *recent_reset)
{
    int n;
    int regional = reg_assign(&s->regional, key, v);
    if (regional >= 0) return regional;
    if (access_key_known(key)) { int r = access_assign(&s->access, key, v); if (r >= 0) return r; }
    if(!strcmp(key,"desktop_theme_name")) {
        if(strlen(v)>=sizeof(s->desktop_theme_name) || !printable(v)) return 0;
        strcpy(s->desktop_theme_name,v); return 1;
    }
    if(!strcmp(key,"document_icon")) return number(v,0,1,&s->document_icon);
    if(!strcmp(key,"recycle_icon")) return number(v,0,1,&s->recycle_icon);
    if(!strcmp(key,"pointer_scheme")) return number(v,0,2,&s->pointer_scheme);
    if(!strncmp(key,"desktop_theme",13) && key[13]>='0' && key[13]<'0'+CFG_DESKTOP_THEMES && key[14]=='.') {
        struct cfg_desktop_theme *t=&s->desktop_themes[key[13]-'0']; const char *part=key+15;
        if(!strcmp(part,"name")) {
            if(!*v || strlen(v)>=sizeof(t->name) || !printable(v)) return 0;
            strcpy(t->name,v); return 1;
        }
        if(!strcmp(part,"appearance")) return ap_decode(&t->appearance,v);
        if(!strcmp(part,"wallpaper")) {
            if(strlen(v)>=sizeof(t->wallpaper) || !wallpaper_value(v)) return 0;
            strcpy(t->wallpaper,v); return 1;
        }
        if(!strcmp(part,"skin")) {
            const char *name=desk_theme_preference(v); if(!name) return 0;
            strcpy(t->skin,name); return 1;
        }
        if(!strcmp(part,"mode")) return number(v,0,CFG_WALL_MODES-1,&t->wallpaper_mode);
        if(!strcmp(part,"colour")) { if(strlen(v)!=6 || !ap_unhex(v,6,&t->desktop_color)) return 0; return 1; }
        if(!strcmp(part,"saver_enabled")) return flag(v,&t->saver_enabled);
        if(!strcmp(part,"saver_seconds")) return number(v,0,2147483647,&t->saver_seconds);
        if(!strcmp(part,"saver_kind")) return number(v,0,2,&t->saver_kind);
        if(!strcmp(part,"document_icon")) return number(v,0,1,&t->document_icon);
        if(!strcmp(part,"recycle_icon")) return number(v,0,1,&t->recycle_icon);
        if(!strcmp(part,"pointers")) return number(v,0,2,&t->pointer_scheme);
        if(!strcmp(part,"sound_enabled")) return flag(v,&t->sounds.enabled);
        if(!strcmp(part,"sound_volume")) return number(v,0,100,&t->sounds.volume);
        if(!strncmp(part,"sound",5) && number(part+5,0,SOUND_EVENTS-1,&n)) {
            if(strlen(v)>=SOUND_NAME_MAX || !printable(v) || strstr(v,"..") || strchr(v,'/')) return 0;
            strcpy(t->sounds.file[n],v); return 1;
        }
        return 0;
    }
    if (!strcmp(key,"appearance")) {
        if (!ap_decode(&s->appearance,v)) return 0;
        s->appearance_set=1; return 1;
    }
    if (!strcmp(key,"appearance_scheme")) {
        if (!*v || strlen(v)>=sizeof(s->scheme) || !printable(v)) return 0;
        strcpy(s->scheme,v); return 1;
    }
    if (!strncmp(key,"appearance_saved",16) && key[16]>='0' && key[16]<'0'+AP_SCHEMES && !key[17]) {
        const char *sep=strchr(v,'|');
        if (!sep || sep==v || sep-v>=32) return 0;
        struct ap_scheme next={0};
        memcpy(next.name,v,(size_t)(sep-v));
        if (!printable(next.name) || !ap_decode(&next.value,sep+1)) return 0;
        s->schemes[key[16]-'0']=next; return 1;
    }
    if (!strcmp(key, "version")) return number(v, 1, 1000, &n);
    if (!strcmp(key, "theme")) {
        const char *name = desk_theme_preference(v);
        if (!name) return 0;
        snprintf(s->theme, sizeof(s->theme), "%s", name); return 1;
    }
    if (!strcmp(key, "wallpaper")) {
        if (strlen(v) >= sizeof(s->wallpaper) || !wallpaper_value(v)) return 0;
        snprintf(s->wallpaper, sizeof(s->wallpaper), "%s", v); return 1;
    }
    if (!strcmp(key,"wallpaper_pattern")) {
        for (int i=0;i<DESK_WALL_PATTERNS;i++) if (!strcmp(v,desk_wallpaper_pattern_names[i])) { s->wallpaper_pattern=i; return 1; }
        return 0;
    }
    if (!strcmp(key, "wallpaper_mode")) {
        for (int i = 0; i < CFG_WALL_MODES; i++) if (!strcmp(v, cfg_mode_names[i])) { s->wallpaper_mode = i; return 1; }
        return 0;
    }
    if (!strcmp(key, "desktop_color")) {
        uint32_t c = 0; size_t len = strlen(v);
        if (len != 6) return 0;
        for (size_t i = 0; i < 6; i++) {
            char ch = v[i];
            unsigned d = ch >= '0' && ch <= '9' ? (unsigned)(ch - '0') : ch >= 'a' && ch <= 'f' ? (unsigned)(ch - 'a' + 10) :
                         ch >= 'A' && ch <= 'F' ? (unsigned)(ch - 'A' + 10) : 16u;
            if (d > 15) return 0;
            c = c << 4 | d;
        }
        s->desktop_color = c; return 1;
    }
    if (!strcmp(key, "clock_24h")) return flag(v, &s->clock_24h);
    if (!strcmp(key, "clock_seconds")) return flag(v, &s->clock_seconds);
    if (!strcmp(key, "desktop_icons")) return flag(v, &s->desktop_icons);
    if (!strcmp(key, "repeat_delay")) return number(v, 0, 3, &s->repeat_delay);
    if (!strcmp(key, "repeat_speed")) return number(v, 0, 31, &s->repeat_speed);
    if (!strcmp(key, "cursor_blink_ms")) return number(v, 0, 1200, &s->cursor_blink_ms);
    if (!strcmp(key, "mouse_swap")) return flag(v, &s->mouse_swap);
    if (!strcmp(key, "mouse_trails")) return number(v, 0, 7, &s->mouse_trails);
    if (!strcmp(key, "wheel_lines")) return number(v, 0, 101, &s->wheel_lines);
    if (!strcmp(key, "pointer_scheme")) return number(v, 0, 2, &s->pointer_scheme);
    if (!strcmp(key, "layout_switch")) return flag(v, &s->layout_switch);
    if (!strcmp(key, "mouse_speed")) {int ok=number(v, CFG_MOUSE_MIN, CFG_MOUSE_MAX, &s->mouse_speed);if(ok)s->mouse_spi_speed=s->mouse_speed*2;return ok;}
    if (!strcmp(key, "mouse_spi_speed")) return number(v, 1, 20, &s->mouse_spi_speed);
    if (!strcmp(key, "double_click_ms")) return number(v, 0, 5000, &s->double_click_ms);
    if (!strcmp(key, "taskbar_clock")) return flag(v, &s->taskbar_clock);
    if (!strcmp(key, "quick_launch")) return flag(v, &s->quick_launch);
    if (!strcmp(key,"taskbar_top")) return flag(v,&s->taskbar_top);
    if (!strcmp(key,"taskbar_autohide")) return flag(v,&s->taskbar_autohide);
    if (!strcmp(key,"start_small")) return flag(v,&s->start_small);
    if (!strcmp(key,"taskbar_edge")) return number(v,0,3,&s->taskbar_edge);
    if (!strcmp(key,"taskbar_size")) return number(v,32,512,&s->taskbar_size);
    if (!strcmp(key,"toolbar_address")) return flag(v,&s->toolbar_address);
    if (!strcmp(key,"toolbar_links")) return flag(v,&s->toolbar_links);
    if (!strcmp(key,"toolbar_desktop")) return flag(v,&s->toolbar_desktop);
    for (unsigned k=0;k<CFG_TOOLBARS;k++) {
        char name[24]; snprintf(name,sizeof(name),"toolbar_folder%u",k);
        if (!strcmp(key,name)) {
            if (*v && (v[0]!='/' || strlen(v)>=CFG_PATH_MAX || !printable(v) || strstr(v,"/../"))) return 0;
            snprintf(s->toolbar_folder[k],CFG_PATH_MAX,"%s",v); return 1;
        }
    }
    if (!strcmp(key, "icon_auto_arrange")) return flag(v, &s->icon_auto_arrange);
    if (!strcmp(key, "icon_sort")) return number(v, 0, 3, &s->icon_sort);
    if (!strcmp(key, "icon_spacing_x")) return number(v, CFG_ICON_SPACING_MIN, CFG_ICON_SPACING_MAX, &s->icon_spacing_x);
    if (!strcmp(key, "icon_spacing_y")) return number(v, CFG_ICON_SPACING_MIN, CFG_ICON_SPACING_MAX, &s->icon_spacing_y);
    if (!strcmp(key, "icon_font")) return number(v, 0, DESK_FONTS - 1, &s->icon_font);
    if (!strcmp(key, "mouse_speed")) return number(v, CFG_MOUSE_MIN, CFG_MOUSE_MAX, &s->mouse_speed);
    if (!strcmp(key, "double_click_ms")) return number(v, 200, 900, &s->double_click_ms);

    if (!strcmp(key,"saver_enabled")) return flag(v,&s->saver_enabled);
    if (!strcmp(key,"saver_seconds")) return number(v,0,2147483647,&s->saver_seconds);
    if (!strcmp(key,"saver_kind")) return number(v,0,2,&s->saver_kind);
    if (!strcmp(key,"monitor_seconds")) return number(v,0,2147483647,&s->monitor_seconds);
    if (!strcmp(key, "keyboard")) {
        for (int i = 0; i < CFG_KEYS_COUNT; i++) if (!strcmp(v, cfg_keyboard_names[i])) { s->keyboard = i; return 1; }
        return 0;
    }
    if (!strcmp(key, "display_mode")) {
        if (!number(v, 0, 65535, &n)) return 0;
        s->display_mode = (uint16_t)n; return 1;
    }
    if (!strcmp(key, "recent")) {
        if (!*recent_reset) { s->recent_count = 0; *recent_reset = 1; }
        if (v[0] != '/' || strlen(v) >= CFG_PATH_MAX || !printable(v) || s->recent_count >= CFG_RECENT_MAX) return 0;
        snprintf(s->recent[s->recent_count++], CFG_PATH_MAX, "%s", v); return 1;
    }
    return -1;
}

int cfg_parse(struct ciuki_settings *s, const char *text, size_t length)
{
    int rejected = 0, recent_reset = 0;
    size_t at = 0;
    while (at < length) {
        char line[1600];
        size_t n = 0, start = at;
        while (at < length && text[at] != '\n') at++;
        size_t end = at;
        if (at < length) at++;
        if (end > start && text[end - 1] == '\r') end--;
        if (end - start >= sizeof(line)) { rejected++; continue; }
        memcpy(line, text + start, end - start); n = end - start; line[n] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq || eq == p) { rejected++; continue; }
        char *k_end = eq;
        while (k_end > p && (k_end[-1] == ' ' || k_end[-1] == '\t')) k_end--;
        *k_end = 0;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        size_t vl = strlen(v);
        while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) v[--vl] = 0;
        int r = assign(s, p, v, &recent_reset);
        if (r < 0) {
            char kept[sizeof(line) + 2];
            snprintf(kept, sizeof(kept), "%s=%s", p, v);
            if (printable(kept)) extra_add(s, kept); else rejected++;
        } else if (!r) rejected++;
    }
    reg_sanitize(&s->regional);
    if(!s->appearance_set) appearance_from_skin(s);
    return rejected;
}

size_t cfg_format(const struct ciuki_settings *s, char *out, size_t size)
{
    int n = snprintf(out, size,
        "# CiukiOS desktop settings (key=value). Written by the desktop and Control Panel.\n"
        "version=%d\ntheme=%s\naccess_contrast=%x,%s\nwallpaper=%s\nwallpaper_mode=%s\ndesktop_color=%06x\n"
        "clock_24h=%d\nclock_seconds=%d\ndesktop_icons=%d\nicon_auto_arrange=%d\nicon_sort=%d\n"
        "taskbar_clock=%d\nquick_launch=%d\ntaskbar_top=%d\ntaskbar_autohide=%d\nstart_small=%d\n"
        "taskbar_edge=%d\ntaskbar_size=%d\ntoolbar_address=%d\ntoolbar_links=%d\ntoolbar_desktop=%d\n"
        "mouse_speed=%d\ndouble_click_ms=%d\nkeyboard=%s\ndisplay_mode=%u\n",
        CFG_VERSION, s->theme, (unsigned)(s->access.contrast & 0x7fu),
        access_scheme_valid(s->access.scheme) ? s->access.scheme : acc_high_contrast_names[0], s->wallpaper,
        cfg_mode_names[s->wallpaper_mode >= 0 && s->wallpaper_mode < CFG_WALL_MODES ? s->wallpaper_mode : 0],
        (unsigned)(s->desktop_color & 0xffffffu), !!s->clock_24h, !!s->clock_seconds, !!s->desktop_icons,
        !!s->icon_auto_arrange, s->icon_sort < 0 || s->icon_sort > 3 ? 0 : s->icon_sort,
        !!s->taskbar_clock, !!s->quick_launch, !!s->taskbar_top, !!s->taskbar_autohide, !!s->start_small,
        s->taskbar_edge, s->taskbar_size, !!s->toolbar_address, !!s->toolbar_links, !!s->toolbar_desktop,
        s->mouse_speed, s->double_click_ms,
        cfg_keyboard_names[s->keyboard >= 0 && s->keyboard < CFG_KEYS_COUNT ? s->keyboard : 0], (unsigned)s->display_mode);
    if (n <= 0 || (size_t)n >= size) return 0;
    size_t used = (size_t)n;
    n = snprintf(out + used, size - used,
        "mouse_spi_speed=%d\nrepeat_delay=%d\nrepeat_speed=%d\ncursor_blink_ms=%d\nmouse_swap=%d\nmouse_trails=%d\nwheel_lines=%d\npointer_scheme=%d\nlayout_switch=%d\n",
        s->mouse_spi_speed, s->repeat_delay, s->repeat_speed, s->cursor_blink_ms, s->mouse_swap,
        s->mouse_trails, s->wheel_lines, s->pointer_scheme, s->layout_switch);
    if (n <= 0 || (size_t)n >= size - used) return 0;
    used += (size_t)n;
    n = snprintf(out + used, size - used, "regional_locale=%s\n", reg_locale(&s->regional));
    if (n <= 0 || (size_t)n >= size - used) return 0;
    used += (size_t)n;
    for (unsigned i = 0; i < REG_FIELDS; i++) {
        const char *v=s->regional.value[i];
        n = snprintf(out + used, size - used, "regional_%s=%s\n", reg_keys[i], reg_eq(v," ") ? "\\s" : v);
        if (n <= 0 || (size_t)n >= size - used) return 0;
        used += (size_t)n;
    }
    n=snprintf(out+used,size-used,"wallpaper_pattern=%s\nicon_spacing_x=%d\nicon_spacing_y=%d\nicon_font=%d\n",
               desk_wallpaper_pattern_names[s->wallpaper_pattern>=0 && s->wallpaper_pattern<DESK_WALL_PATTERNS ? s->wallpaper_pattern : 0],
               s->icon_spacing_x,s->icon_spacing_y,s->icon_font);
    if (n<0 || (size_t)n>=size-used) return 0;
    used+=(size_t)n;
    n=snprintf(out+used,size-used,"saver_enabled=%d\nsaver_seconds=%d\nsaver_kind=%d\nmonitor_seconds=%d\n",
               !!s->saver_enabled,s->saver_seconds,s->saver_kind,s->monitor_seconds);
    if (n<0 || (size_t)n>=size-used) return 0;
    used+=(size_t)n;
    const struct access_settings *a = &s->access;
    n = snprintf(out + used, size - used,
        "access_sticky=%x\naccess_filter=%x,%u,%u,%u,%u\naccess_toggle=%x\naccess_mouse=%x,%u,%u,%u\naccess_sentry=%x,%u\n"
        "access_timeout=%x,%u\naccess_general=%u,%u,%u\naccess_magnifier=%u,%u,%u,%u,%u,%u\n",
        (unsigned)(a->sticky & 0x1ffu), (unsigned)(a->filter & 0x107fu), (unsigned)a->filter_wait, (unsigned)a->filter_delay,
        (unsigned)a->filter_repeat, (unsigned)a->filter_bounce, (unsigned)(a->toggle & 0x3fu), (unsigned)(a->mouse & 0xffu),
        (unsigned)a->mouse_max, (unsigned)a->mouse_time, (unsigned)a->mouse_ctrl, (unsigned)(a->sentry & 7u),
        (unsigned)a->sentry_effect, (unsigned)(a->timeout & 3u), (unsigned)(a->timeout_ms / 1000u), (unsigned)!!a->show_sounds,
        (unsigned)!!a->status, (unsigned)a->pointer, (unsigned)!!a->mag_on, (unsigned)a->mag_level, (unsigned)a->mag_follow,
        (unsigned)!!a->mag_invert, (unsigned)a->mag_dock, (unsigned)a->mag_percent);
    if (n <= 0 || (size_t)n >= size - used) return 0;
    used += (size_t)n;
    n=snprintf(out+used,size-used,"taskbar_top=%d\ntaskbar_autohide=%d\nstart_small=%d\ntaskbar_edge=%d\ntaskbar_size=%d\n"
               "toolbar_address=%d\ntoolbar_links=%d\ntoolbar_desktop=%d\n",
               !!s->taskbar_top,!!s->taskbar_autohide,!!s->start_small,s->taskbar_edge,s->taskbar_size,
               !!s->toolbar_address,!!s->toolbar_links,!!s->toolbar_desktop);
    if (n<0 || (size_t)n>=size-used) return 0;
    used+=(size_t)n;
    for (unsigned k=0;k<CFG_TOOLBARS;k++) {
        n=snprintf(out+used,size-used,"toolbar_folder%u=%s\n",k,s->toolbar_folder[k]);
        if (n<0 || (size_t)n>=size-used) return 0;
        used+=(size_t)n;
    }
    char encoded[AP_HEX_BYTES+1];
    if (s->appearance_set) {
        ap_encode(&s->appearance,encoded);
        n=snprintf(out+used,size-used,"appearance=%s\nappearance_scheme=%s\n",encoded,s->scheme);
        if (n<0 || (size_t)n>=size-used) return 0;
        used+=(size_t)n;
    }
    for (unsigned i=0;i<AP_SCHEMES;i++) if(s->schemes[i].name[0]) {
        ap_encode(&s->schemes[i].value,encoded);
        n=snprintf(out+used,size-used,"appearance_saved%u=%s|%s\n",i,s->schemes[i].name,encoded);
        if(n<0 || (size_t)n>=size-used) return 0;
        used+=(size_t)n;
    }
    n=snprintf(out+used,size-used,"desktop_theme_name=%s\npointer_scheme=%d\ndocument_icon=%d\nrecycle_icon=%d\n",s->desktop_theme_name,s->pointer_scheme,s->document_icon,s->recycle_icon);
    if(n<0 || (size_t)n>=size-used) return 0;
    used+=(size_t)n;
    for(unsigned i=0;i<CFG_DESKTOP_THEMES;i++) if(s->desktop_themes[i].name[0]) {
        const struct cfg_desktop_theme *t=&s->desktop_themes[i];
        ap_encode(&t->appearance,encoded);
        n=snprintf(out+used,size-used,
          "desktop_theme%u.name=%s\ndesktop_theme%u.appearance=%s\ndesktop_theme%u.wallpaper=%s\n"
          "desktop_theme%u.skin=%s\ndesktop_theme%u.mode=%d\ndesktop_theme%u.colour=%06x\n"
          "desktop_theme%u.saver_enabled=%d\ndesktop_theme%u.saver_seconds=%d\ndesktop_theme%u.saver_kind=%d\n"
          "desktop_theme%u.pointers=%d\ndesktop_theme%u.sound_enabled=%d\ndesktop_theme%u.sound_volume=%d\n",
          i,t->name,i,encoded,i,t->wallpaper,i,t->skin,i,t->wallpaper_mode,i,(unsigned)t->desktop_color,
          i,t->saver_enabled,i,t->saver_seconds,i,t->saver_kind,i,t->pointer_scheme,i,t->sounds.enabled,i,t->sounds.volume);
        if(n<0 || (size_t)n>=size-used) return 0;
        used+=(size_t)n;
        n=snprintf(out+used,size-used,"desktop_theme%u.document_icon=%d\ndesktop_theme%u.recycle_icon=%d\n",i,t->document_icon,i,t->recycle_icon);
        if(n<0 || (size_t)n>=size-used) return 0;
        used+=(size_t)n;
        for(unsigned j=0;j<SOUND_EVENTS;j++) {
            n=snprintf(out+used,size-used,"desktop_theme%u.sound%u=%s\n",i,j,t->sounds.file[j]);
            if(n<0 || (size_t)n>=size-used) return 0;
            used+=(size_t)n;
        }
    }
    for (unsigned i = 0; i < s->recent_count && i < CFG_RECENT_MAX; i++) {
        n = snprintf(out + used, size - used, "recent=%s\n", s->recent[i]);
        if (n <= 0 || (size_t)n >= size - used) return 0;
        used += (size_t)n;
    }
    n = snprintf(out + used, size - used, "%s", s->extra);
    if (n < 0 || (size_t)n >= size - used) return 0;
    return used + (size_t)n;
}

void cfg_recent_add(struct ciuki_settings *s, const char *path)
{
    if (!path || path[0] != '/' || strlen(path) >= CFG_PATH_MAX || !printable(path)) return;
    unsigned found = s->recent_count;
    for (unsigned i = 0; i < s->recent_count; i++) if (!strcmp(s->recent[i], path)) found = i;
    unsigned last = found < s->recent_count ? found : (s->recent_count < CFG_RECENT_MAX ? s->recent_count : CFG_RECENT_MAX - 1);
    for (unsigned i = last; i > 0; i--) memcpy(s->recent[i], s->recent[i - 1], CFG_PATH_MAX);
    snprintf(s->recent[0], CFG_PATH_MAX, "%s", path);
    if (found >= s->recent_count && s->recent_count < CFG_RECENT_MAX) s->recent_count++;
}

/* Hex names preserve every byte without conflicting with key=value syntax. */
static int icon_key(char *out, const char *name)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = strlen(name); if (n > 765) return 0;
    memcpy(out, "icon.", 5);
    for (size_t i = 0; i < n; i++) { unsigned c = (unsigned char)name[i]; out[5+i*2] = hex[c>>4]; out[6+i*2] = hex[c&15]; }
    out[5+n*2] = '='; out[6+n*2] = 0; return 1;
}
static int position_read(const struct ciuki_settings *s, const char *key, int *x, int *y)
{
    size_t n = strlen(key);
    for (const char *p = s->extra; *p;) {
        const char *end = strchr(p,'\n'); if (!end) break;
        if (!strncmp(p,key,n)) {
            const char *v=p+n, *comma=memchr(v,',',(size_t)(end-v));
            char first[16], second[16];
            if (!comma || comma-v >= (int)sizeof(first) || end-comma-1 >= (int)sizeof(second)) return 0;
            memcpy(first,v,(size_t)(comma-v)); first[comma-v]=0;
            memcpy(second,comma+1,(size_t)(end-comma-1)); second[end-comma-1]=0;
            return number(first,0,65535,x) && number(second,0,65535,y);
        }
        p=end+1;
    }
    return 0;
}
static void icon_remove(struct ciuki_settings *s, const char *key)
{
    char *p = s->extra; size_t n = key ? strlen(key) : 5;
    while (*p) {
        char *end = strchr(p,'\n'); if (!end) break;
        if (!strncmp(p,key ? key : "icon.",n)) memmove(p,end+1,strlen(end+1)+1);
        else p = end+1;
    }
}
static size_t key_bytes(const struct ciuki_settings *s, const char *key)
{
    size_t bytes=0,n=strlen(key);
    for (const char *p=s->extra; *p;) {
        const char *end=strchr(p,'\n'); if (!end) break;
        if (!strncmp(p,key,n)) bytes+=(size_t)(end-p)+1;
        p=end+1;
    }
    return bytes;
}
static int position_write(struct ciuki_settings *s, const char *key, int x, int y)
{
    char line[1640];
    if (x<0 || y<0 || x>65535 || y>65535) return 0;
    int count=snprintf(line,sizeof(line),"%s%d,%d\n",key,x,y);
    if (count<0 || (size_t)count>=sizeof(line) || strlen(s->extra)-key_bytes(s,key)+(size_t)count+1>sizeof(s->extra)) return 0;
    icon_remove(s,key); strcat(s->extra,line); return 1;
}
int cfg_icon_position(const struct ciuki_settings *s, const char *name, int *x, int *y)
{ char key[1538]; return icon_key(key,name) && position_read(s,key,x,y); }
int cfg_icon_move(struct ciuki_settings *s, const char *name, int x, int y)
{ char key[1538]; return icon_key(key,name) && position_write(s,key,x,y); }
void cfg_icons_arrange(struct ciuki_settings *s) { icon_remove(s,NULL); }
static int profile_keys(char prefix[40], char marker[48], unsigned w, unsigned h)
{
    if (!w || !h || w>65535 || h>65535) return 0;
    snprintf(prefix,40,"icon.%ux%u.",w,h);
    snprintf(marker,48,"iconlayout.%ux%u=",w,h); return 1;
}
static int resolution_key(char out[1600], const char *name, const char *prefix)
{
    if (!icon_key(out,name)) return 0;
    size_t n=strlen(prefix),tail=strlen(out+5)+1;
    memmove(out+n,out+5,tail); memcpy(out,prefix,n); return 1;
}
int cfg_icon_position_at(const struct ciuki_settings *s, const char *name, unsigned w, unsigned h, int *x, int *y)
{
    char key[1600],prefix[40],marker[48];
    if (!profile_keys(prefix,marker,w,h) || !resolution_key(key,name,prefix)) return 0;
    if (position_read(s,key,x,y)) return 1;
    return !key_bytes(s,marker) && cfg_icon_position(s,name,x,y);
}
int cfg_icon_move_at(struct ciuki_settings *s, const char *name, unsigned w, unsigned h, int x, int y)
{
    char key[1600],prefix[40],marker[48];
    if (!profile_keys(prefix,marker,w,h) || !resolution_key(key,name,prefix) || x<0 || y<0 || x>65535 || y>65535) return 0;
    /* Legacy keys remain a read-only migration fallback for icons not moved
     * in this resolution. Arrange explicitly disables that fallback. */
    return position_write(s,key,x,y);
}
int cfg_icons_arrange_at(struct ciuki_settings *s, unsigned w, unsigned h)
{
    char prefix[40],marker[48];
    if (!profile_keys(prefix,marker,w,h)) return 0;
    if (!key_bytes(s,marker) && strlen(s->extra)-key_bytes(s,prefix)+strlen(marker)+3>sizeof(s->extra)) return 0;
    icon_remove(s,prefix);
    if (!key_bytes(s,marker)) { strcat(s->extra,marker); strcat(s->extra,"1\n"); }
    return 1;
}
static int position_matches(const char *p, const char *end, const char *tail, size_t n)
{
    const char *eq=memchr(p,'=',(size_t)(end-p));
    return !strncmp(p,"icon.",5) && eq && (size_t)(eq-p)+1>=n+5 &&
           (eq+1-n==p+5 || eq[-(int)n]=='.') && !memcmp(eq+1-n,tail,n);
}
void cfg_icon_forget(struct ciuki_settings *s, const char *name)
{
    char key[1538]; if (!icon_key(key,name)) return;
    char *p=s->extra; size_t n=strlen(key+5);
    while (*p) {
        char *end=strchr(p,'\n'); if (!end) break;
        if (position_matches(p,end,key+5,n)) memmove(p,end+1,strlen(end+1)+1);
        else p=end+1;
    }
}
int cfg_icon_rename(struct ciuki_settings *s, const char *old_name, const char *new_name)
{
    char old_key[1538],new_key[1538];
    if (!icon_key(old_key,old_name) || !icon_key(new_key,new_name)) return 0;
    if (!strcmp(old_key,new_key)) return 1;
    size_t old_n=strlen(old_key+5),new_n=strlen(new_key+5),removed=0,matches=0;
    for (const char *p=s->extra; *p;) {
        const char *end=strchr(p,'\n'); if (!end) break;
        if (position_matches(p,end,old_key+5,old_n)) matches++;
        if (position_matches(p,end,new_key+5,new_n)) removed+=(size_t)(end-p)+1;
        p=end+1;
    }
    ptrdiff_t final=(ptrdiff_t)(strlen(s->extra)-removed)+(ptrdiff_t)matches*((ptrdiff_t)new_n-(ptrdiff_t)old_n);
    if (final<0 || (size_t)final>=sizeof(s->extra)) return 0;
    cfg_icon_forget(s,new_name);
    for (char *p=s->extra; *p;) {
        char *end=strchr(p,'\n'); if (!end) break;
        if (position_matches(p,end,old_key+5,old_n)) {
            char *eq=memchr(p,'=',(size_t)(end-p)),*start=eq+1-old_n;
            memmove(start+new_n,start+old_n,strlen(start+old_n)+1);
            memcpy(start,new_key+5,new_n); end=strchr(p,'\n');
        }
        p=end+1;
    }
    return 1;
}

static int icon_label_key(char *out, const char *name)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = strlen(name); if (n > 765) return 0;
    memcpy(out, "iconlabel.", 10);
    for (size_t i=0;i<n;i++) { unsigned c=(unsigned char)name[i]; out[10+i*2]=hex[c>>4]; out[11+i*2]=hex[c&15]; }
    out[10+n*2]='='; out[11+n*2]=0; return 1;
}
int cfg_icon_label(const struct ciuki_settings *s, const char *name, char *out, size_t size)
{
    char key[1540]; if (!s || !name || !out || !size || !icon_label_key(key,name)) return 0;
    size_t n=strlen(key);
    for (const char *p=s->extra; *p;) {
        const char *end=strchr(p,'\n'); if (!end) break;
        if (!strncmp(p,key,n)) {
            size_t len=(size_t)(end-p)-n; if (!len || len>=size) return 0;
            memcpy(out,p+n,len); out[len]=0; return 1;
        }
        p=end+1;
    }
    return 0;
}
int cfg_icon_label_set(struct ciuki_settings *s, const char *name, const char *label)
{
    char key[1540], line[1640];
    if (!s || !name || !label || !icon_label_key(key,name)) return 0;
    size_t n=strlen(label); if (!n || n>=64) return 0;
    for (size_t i=0;i<n;i++) if ((unsigned char)label[i]<32 || label[i]=='\r' || label[i]=='\n') return 0;
    int count=snprintf(line,sizeof(line),"%s%s\n",key,label);
    if (count<=0 || (size_t)count>=sizeof(line)) return 0;
    size_t prefix=strlen(key), old=strlen(s->extra), removed=0; char *p=s->extra;
    for (const char *q=s->extra; *q;) {
        const char *end=strchr(q,'\n'); if (!end) break;
        if (!strncmp(q,key,prefix)) removed+=(size_t)(end-q)+1;
        q=end+1;
    }
    if (old-removed+(size_t)count+1>sizeof(s->extra)) return 0;
    while (*p) { char *end=strchr(p,'\n'); if (!end) break;
        if (!strncmp(p,key,prefix)) memmove(p,end+1,strlen(end+1)+1); else p=end+1; }
    old=strlen(s->extra);
    memcpy(s->extra+old,line,(size_t)count+1); return 1;
}
void cfg_icon_label_forget(struct ciuki_settings *s, const char *name)
{
    char key[1540]; if (!s || !name || !icon_label_key(key,name)) return;
    size_t n=strlen(key); char *p=s->extra;
    while (*p) { char *end=strchr(p,'\n'); if (!end) break;
        if (!strncmp(p,key,n)) memmove(p,end+1,strlen(end+1)+1); else p=end+1; }
}

/* Protocol keys are append-only, independent of the C layout. */
int *cfg_input_field(struct ciuki_settings *s, unsigned key)
{
    switch(key) {
    case 1:return &s->keyboard; case 2:return &s->repeat_delay; case 3:return &s->repeat_speed;
    case 4:return &s->cursor_blink_ms; case 5:return &s->mouse_swap; case 6:return &s->mouse_trails;
    case 7:return &s->wheel_lines; case 8:return &s->pointer_scheme; case 9:return &s->mouse_spi_speed;
    case 10:return &s->double_click_ms; case 11:return &s->layout_switch; default:return NULL;
    }
}
int cfg_input_set(struct ciuki_settings *s, unsigned key, unsigned value)
{
    static const unsigned maximum[]={0,1,3,31,1200,1,7,101,2,20,5000,1};
    int *p=cfg_input_field(s,key);
    if (!p || value>maximum[key] || (key==9 && !value)) return -1;
    *p=(int)value;return 0;
}

/* User names never shadow the supplied schemes. Save replaces an existing
 * user entry atomically and refuses a full catalogue without changing it. */
int cfg_scheme_save(struct ciuki_settings *s,const char *name)
{
    if(!name || !*name || strlen(name)>=32 || !printable(name) || strchr(name,'|')) return -1;
    for(int i=0;i<AP_BUILTINS;i++) if(ap_name_equal(name,ap_builtin_name(i))) return -1;
    int slot=-1;
    for(int i=0;i<AP_SCHEMES;i++) {
        if(ap_name_equal(s->schemes[i].name,name)) { slot=i; break; }
        if(slot<0 && !s->schemes[i].name[0]) slot=i;
    }
    if(slot<0) return -1;
    char saved_name[32]; strcpy(saved_name,name);
    strcpy(s->schemes[slot].name,saved_name); s->schemes[slot].value=s->appearance;
    strcpy(s->scheme,name); s->appearance_set=1; return 0;
}
int cfg_scheme_delete(struct ciuki_settings *s,const char *name)
{
    for(int i=0;i<AP_SCHEMES;i++) if(ap_name_equal(name,s->schemes[i].name) && *name) {
        memset(&s->schemes[i],0,sizeof(s->schemes[i])); return 0;
    }
    return -1;
}
int cfg_scheme_use(struct ciuki_settings *s,const char *name)
{
    for(int i=0;i<AP_BUILTINS;i++) if(ap_name_equal(name,ap_builtin_name(i))) {
        if(i==5) { strcpy(s->theme,DESK_THEME_DEFAULT); s->desktop_color=CFG_DESKTOP_COLOR; appearance_from_skin(s); }
        else ap_default(&s->appearance,i);
        strcpy(s->scheme,ap_builtin_name(i)); s->appearance_set=1;
        s->desktop_color=s->appearance.item[AP_DESKTOP].colour; return 0;
    }
    for(int i=0;i<AP_SCHEMES;i++) if(*name && ap_name_equal(name,s->schemes[i].name)) {
        s->appearance=s->schemes[i].value; strcpy(s->scheme,s->schemes[i].name); s->appearance_set=1;
        s->desktop_color=s->appearance.item[AP_DESKTOP].colour; return 0;
    }
    return -1;
}

int cfg_desktop_theme_save(struct ciuki_settings *s,const char *name,const struct sound_scheme *sounds)
{
    if(!name || !*name || strlen(name)>=32 || !printable(name)) return -1;
    for(int i=0;i<AP_BUILTINS;i++) if(ap_name_equal(name,ap_builtin_name(i))) return -1;
    int slot=-1;
    for(int i=0;i<CFG_DESKTOP_THEMES;i++) {
        if(ap_name_equal(s->desktop_themes[i].name,name)) { slot=i; break; }
        if(slot<0 && !s->desktop_themes[i].name[0]) slot=i;
    }
    if(slot<0) return -1;
    char saved_name[32]; strcpy(saved_name,name);
    struct cfg_desktop_theme *t=&s->desktop_themes[slot];
    memset(t,0,sizeof(*t)); strcpy(t->name,saved_name); strcpy(t->wallpaper,s->wallpaper); strcpy(t->skin,s->theme);
    t->appearance=s->appearance; t->wallpaper_mode=s->wallpaper_mode; t->desktop_color=s->desktop_color;
    t->saver_enabled=s->saver_enabled; t->saver_seconds=s->saver_seconds; t->saver_kind=s->saver_kind;
    t->pointer_scheme=s->pointer_scheme; t->sounds=*sounds; t->document_icon=s->document_icon; t->recycle_icon=s->recycle_icon;
    strcpy(s->desktop_theme_name,saved_name);
    return 0;
}
int cfg_desktop_theme_use(struct ciuki_settings *s,const char *name,struct sound_scheme *sounds)
{
    for(int i=0;i<CFG_DESKTOP_THEMES;i++) if(*name && ap_name_equal(s->desktop_themes[i].name,name)) {
        const struct cfg_desktop_theme *t=&s->desktop_themes[i];
        if(!ap_valid(&t->appearance) || !wallpaper_value(t->wallpaper) || !desk_theme_preference(t->skin)) return -1;
        s->appearance=t->appearance; s->appearance_set=1; strcpy(s->theme,t->skin);
        strcpy(s->wallpaper,t->wallpaper); s->wallpaper_mode=t->wallpaper_mode; s->desktop_color=t->desktop_color;
        s->saver_enabled=t->saver_enabled; s->saver_seconds=t->saver_seconds; s->saver_kind=t->saver_kind;
        s->pointer_scheme=t->pointer_scheme; s->document_icon=t->document_icon; s->recycle_icon=t->recycle_icon; *sounds=t->sounds; strcpy(s->desktop_theme_name,t->name); return 0;
    }
    return -1;
}
int cfg_desktop_theme_delete(struct ciuki_settings *s,const char *name)
{
    for(int i=0;i<CFG_DESKTOP_THEMES;i++) if(*name && ap_name_equal(s->desktop_themes[i].name,name)) {
        memset(&s->desktop_themes[i],0,sizeof(s->desktop_themes[i])); return 0;
    }
    return -1;
}
