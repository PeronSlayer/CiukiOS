/* SPDX-License-Identifier: MIT
 * desktop.cfg parser and writer (settings.h). Integer-only, no I/O. */
#include "settings.h"
#include <stdio.h>
#include <string.h>

const char *const cfg_theme_names[CFG_THEMES] = {"Platinum", "Ciuki Blue", "Classic 98"};
const char *const cfg_mode_names[CFG_WALL_MODES] = {"center", "stretch", "tile", "fit", "fill"};
const char *const cfg_keyboard_names[CFG_KEYS_COUNT] = {"us", "it"};
const char *const cfg_keyboard_labels[CFG_KEYS_COUNT] = {"English (US)", "Italian"};

void cfg_defaults(struct ciuki_settings *s)
{
    memset(s, 0, sizeof(*s));
    strcpy(s->theme, "Platinum");
    strcpy(s->wallpaper, "/system/wallpapers/Ciuk2.png");   /* owner's Ciuk2 scene (assets/wallpapers/ciuki) */
    s->wallpaper_mode = CFG_WALL_FILL;     /* the adaptive scenes are made for a centred, aspect-preserving fill */
    s->desktop_color = CFG_DESKTOP_COLOR;          /* DESK_BACKGROUND */
    s->clock_24h = 1;
    s->desktop_icons = 1;
    s->mouse_speed = CFG_MOUSE_NORMAL;
    s->double_click_ms = 500;
    s->keyboard = CFG_KEYS_US;
}

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
        if (*v < '0' || *v > '9' || n > 100000) return 0;
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

static int assign(struct ciuki_settings *s, const char *key, const char *v, int *recent_reset)
{
    int n;
    if (!strcmp(key, "version")) return number(v, 1, 1000, &n);
    if (!strcmp(key, "theme")) {
        if (cfg_theme_index(v) < 0) return 0;
        snprintf(s->theme, sizeof(s->theme), "%s", v); return 1;
    }
    if (!strcmp(key, "wallpaper")) {
        if (strlen(v) >= sizeof(s->wallpaper) || !wallpaper_value(v)) return 0;
        snprintf(s->wallpaper, sizeof(s->wallpaper), "%s", v); return 1;
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
    if (!strcmp(key, "mouse_speed")) return number(v, CFG_MOUSE_MIN, CFG_MOUSE_MAX, &s->mouse_speed);
    if (!strcmp(key, "double_click_ms")) return number(v, 200, 900, &s->double_click_ms);

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
    return rejected;
}

size_t cfg_format(const struct ciuki_settings *s, char *out, size_t size)
{
    int n = snprintf(out, size,
        "# CiukiOS desktop settings (key=value). Written by the desktop and Control Panel.\n"
        "version=%d\ntheme=%s\nwallpaper=%s\nwallpaper_mode=%s\ndesktop_color=%06x\n"
        "clock_24h=%d\nclock_seconds=%d\ndesktop_icons=%d\nmouse_speed=%d\ndouble_click_ms=%d\nkeyboard=%s\ndisplay_mode=%u\n",
        CFG_VERSION, s->theme, s->wallpaper,
        cfg_mode_names[s->wallpaper_mode >= 0 && s->wallpaper_mode < CFG_WALL_MODES ? s->wallpaper_mode : 0],
        (unsigned)(s->desktop_color & 0xffffffu), !!s->clock_24h, !!s->clock_seconds, !!s->desktop_icons,
        s->mouse_speed, s->double_click_ms,
        cfg_keyboard_names[s->keyboard >= 0 && s->keyboard < CFG_KEYS_COUNT ? s->keyboard : 0], (unsigned)s->display_mode);
    if (n <= 0 || (size_t)n >= size) return 0;
    size_t used = (size_t)n;
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
int cfg_icon_position(const struct ciuki_settings *s, const char *name, int *x, int *y)
{
    char key[1538]; if (!icon_key(key, name)) return 0;
    size_t n = strlen(key);
    for (const char *p = s->extra; *p;) {
        if (!strncmp(p,key,n)) {
            const char *v = p+n, *comma = strchr(v, ','); char first[16];
            if (!comma || comma-v >= (int)sizeof(first)) return 0;
            memcpy(first,v,(size_t)(comma-v)); first[comma-v] = 0;
            const char *end = strchr(comma+1,'\n'); char second[16];
            if (!end || end-comma-1 >= (int)sizeof(second)) return 0;
            memcpy(second,comma+1,(size_t)(end-comma-1)); second[end-comma-1] = 0;
            return number(first,0,65535,x) && number(second,0,65535,y);
        }
        const char *end = strchr(p,'\n'); if (!end) break; p = end+1;
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
int cfg_icon_move(struct ciuki_settings *s, const char *name, int x, int y)
{
    char key[1538], line[1580]; if (x < 0 || y < 0 || x > 65535 || y > 65535 || !icon_key(key,name)) return 0;
    snprintf(line,sizeof(line),"%s%d,%d\n",key,x,y);
    size_t old = strlen(s->extra), len = strlen(line);
    /* Capacity is checked before removing the old entry. */
    if (old + len + 1 > sizeof(s->extra)) return 0;
    icon_remove(s,key); strcat(s->extra,line); return 1;
}
void cfg_icons_arrange(struct ciuki_settings *s) { icon_remove(s,NULL); }

void cfg_icon_forget(struct ciuki_settings *s, const char *name)
{ char key[1538]; if (icon_key(key,name)) icon_remove(s,key); }
