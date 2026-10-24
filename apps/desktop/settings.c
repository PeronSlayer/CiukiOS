/* SPDX-License-Identifier: MIT
 * desktop.cfg parser and writer (settings.h). Integer-only, no I/O. */
#include "settings.h"
#include "theme.h"
#include <stdio.h>
#include <string.h>

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
    ap_default(&s->appearance,5);
    strcpy(s->scheme,"Ciuki Blue"); strcpy(s->desktop_theme_name,"Ciuki Blue");
    strcpy(s->theme, DESK_THEME_DEFAULT);
    strcpy(s->wallpaper, "/system/wallpapers/Ciuk2.png");   /* owner's Ciuk2 scene (assets/wallpapers/ciuki) */
    s->wallpaper_mode = CFG_WALL_FILL;     /* the adaptive scenes are made for a centred, aspect-preserving fill */
    s->desktop_color = CFG_DESKTOP_COLOR;          /* DESK_BACKGROUND */
    s->clock_24h = 1;
    s->desktop_icons = 1;
    s->mouse_speed = CFG_MOUSE_NORMAL;
    s->double_click_ms = 500;
    s->keyboard = CFG_KEYS_US;
    s->saver_seconds=600; s->saver_kind=1;
    appearance_from_skin(s);
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

static int assign(struct ciuki_settings *s, const char *key, const char *v, int *recent_reset)
{
    int n;
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
    if(!s->appearance_set) appearance_from_skin(s);
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
    n=snprintf(out+used,size-used,"saver_enabled=%d\nsaver_seconds=%d\nsaver_kind=%d\nmonitor_seconds=%d\n",
               !!s->saver_enabled,s->saver_seconds,s->saver_kind,s->monitor_seconds);
    if (n<0 || (size_t)n>=size-used) return 0;
    used+=(size_t)n;
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
