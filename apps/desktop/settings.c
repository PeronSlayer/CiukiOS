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

void cfg_defaults(struct ciuki_settings *s)
{
    memset(s, 0, sizeof(*s));
    strcpy(s->theme, DESK_THEME_DEFAULT);
    strcpy(s->wallpaper, "/system/wallpapers/Ciuk2.png");   /* owner's Ciuk2 scene (assets/wallpapers/ciuki) */
    s->wallpaper_mode = CFG_WALL_FILL;     /* the adaptive scenes are made for a centred, aspect-preserving fill */
    s->desktop_color = CFG_DESKTOP_COLOR;          /* DESK_BACKGROUND */
    s->clock_24h = 1;
    s->desktop_icons = 1;
    s->icon_auto_arrange = 0;
    s->icon_sort = 0;
    s->icon_spacing_x = 96; s->icon_spacing_y = 94; s->icon_font = DESK_FONT_LABEL;
    s->mouse_speed = CFG_MOUSE_NORMAL;
    s->double_click_ms = 500;
    s->keyboard = CFG_KEYS_US;
    s->saver_seconds=600; s->saver_kind=1;
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
    return rejected;
}

size_t cfg_format(const struct ciuki_settings *s, char *out, size_t size)
{
    int n = snprintf(out, size,
        "# CiukiOS desktop settings (key=value). Written by the desktop and Control Panel.\n"
        "version=%d\ntheme=%s\nwallpaper=%s\nwallpaper_mode=%s\ndesktop_color=%06x\n"
        "clock_24h=%d\nclock_seconds=%d\ndesktop_icons=%d\nicon_auto_arrange=%d\nicon_sort=%d\nmouse_speed=%d\ndouble_click_ms=%d\nkeyboard=%s\ndisplay_mode=%u\n",
        CFG_VERSION, s->theme, s->wallpaper,
        cfg_mode_names[s->wallpaper_mode >= 0 && s->wallpaper_mode < CFG_WALL_MODES ? s->wallpaper_mode : 0],
        (unsigned)(s->desktop_color & 0xffffffu), !!s->clock_24h, !!s->clock_seconds, !!s->desktop_icons,
        !!s->icon_auto_arrange, s->icon_sort < 0 || s->icon_sort > 3 ? 0 : s->icon_sort,
        s->mouse_speed, s->double_click_ms,
        cfg_keyboard_names[s->keyboard >= 0 && s->keyboard < CFG_KEYS_COUNT ? s->keyboard : 0], (unsigned)s->display_mode);
    if (n <= 0 || (size_t)n >= size) return 0;
    size_t used = (size_t)n;
    n=snprintf(out+used,size-used,"wallpaper_pattern=%s\nicon_spacing_x=%d\nicon_spacing_y=%d\nicon_font=%d\n",
               desk_wallpaper_pattern_names[s->wallpaper_pattern>=0 && s->wallpaper_pattern<DESK_WALL_PATTERNS ? s->wallpaper_pattern : 0],
               s->icon_spacing_x,s->icon_spacing_y,s->icon_font);
    if (n<0 || (size_t)n>=size-used) return 0;
    used+=(size_t)n;
    n=snprintf(out+used,size-used,"saver_enabled=%d\nsaver_seconds=%d\nsaver_kind=%d\nmonitor_seconds=%d\n",
               !!s->saver_enabled,s->saver_seconds,s->saver_kind,s->monitor_seconds);
    if (n<0 || (size_t)n>=size-used) return 0;
    used+=(size_t)n;
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
