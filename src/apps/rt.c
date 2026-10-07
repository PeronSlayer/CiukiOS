/* Runtime of the desktop application modules: shell services, strings,
 * numbers and DOS calls (no C library). See app.h. */
#include "app.h"
#include "player_audio.h"

struct sargs { int x, y, w, h; const char *s; int v, v2, v3; };
int svc(int n, struct sargs *a);


static u16 my_ds(void);
#pragma aux my_ds = "mov ax,ds" value [ax];

/* ---------------- shell services ---------------- */
static void call5(int n, int x, int y, int w, int h, const char *s, int v)
{
    struct sargs a;
    a.x = x; a.y = y; a.w = w; a.h = h; a.s = s; a.v = v; a.v2 = 0; a.v3 = 0;
    svc(n, &a);
}
void ui_rect(int x, int y, int w, int h, int c) { if (w > 0 && h > 0) call5(0, x, y, w, h, "", c); }
void ui_text(int x, int y, const char *s, int c) { call5(1, x, y, 0, 0, s, c); }
void ui_bevel(int x, int y, int w, int h, int f) { call5(2, x, y, w, h, "", f); }
void ui_inset(int x, int y, int w, int h) { call5(3, x, y, w, h, "", 0); }
void ui_button(int x, int y, int w, int h, const char *l, int id) { call5(4, x, y, w, h, l, id); }
void ui_hit(int x, int y, int w, int h, int id) { call5(5, x, y, w, h, "", id); }
void ui_icon(int x, int y, int icon) { call5(6, x, y, 0, 0, "", icon); }
int ui_measure(const char *s)
{
    return ui_measure_style(s, 0);
}
int ui_measure_style(const char *s, int style)
{
    struct sargs a;
    a.x = a.y = a.w = a.h = a.v2 = a.v3 = 0;
    a.s = s; a.v = style;
    return svc(7, &a);
}
void ui_repaint(void) { call5(8, 0, 0, 0, 0, "", 0); }
void ui_repaint_win(int w) { call5(8, 0, 0, 0, 0, "", w); }
void app_command(const char *c) { call5(9, 0, 0, 0, 0, c, 0); }
void app_open(int window, const char *arg) { call5(10, 0, 0, 0, 0, arg, window); }
void app_close(void) { call5(11, 0, 0, 0, 0, "", 0); }
void app_sound(int e) { call5(12, 0, 0, 0, 0, "", e); }
int app_windows(char *buffer)
{
    struct sargs a;
    a.s = buffer;
    return svc(13, &a);
}
void app_window_cmd(int window, int command)
{
    struct sargs a;
    a.s = ""; a.v = window; a.v2 = command;
    svc(14, &a);
}
int app_idle(void) { struct sargs a; a.s = ""; return svc(15, &a); }
void ui_mono(int x, int y, const char *s, int c, int cell) { call5(16, x, y, cell, 0, s, c); }
int win_open(int x, int y, int w, int h, const char *title, int style)
{
    struct sargs a;
    a.x = x; a.y = y; a.w = w; a.h = h; a.s = title; a.v = style; a.v2 = 0; a.v3 = 0;
    return svc(17, &a);
}
void win_close(int window) { call5(18, 0, 0, 0, 0, "", window); }
void ui_damage(int x, int y, int w, int h) { call5(19, x, y, w, h, "", 0); }
void shell_action(int code) { call5(20, 0, 0, 0, 0, "", code); }
void ui_overlay(int on) { call5(21, 0, 0, 0, 0, "", on); }
int app_font(const char *path, int slot)
{
    struct sargs a;
    a.x = a.y = a.w = a.h = 0; a.s = path; a.v = slot; a.v2 = 0; a.v3 = 0;
    return svc(22, &a);
}
void app_settings(void) { call5(23, 0, 0, 0, 0, "", 0); }
void ui_palette(const u8 *c) { call5(24, 0, 0, 0, 0, (const char *)c, 0); }
void ui_bitmap(int x, int y, int w, int h, unsigned seg, unsigned off, int stride, int zoom)
{
    struct sargs a;
    a.x = x; a.y = y; a.w = w; a.h = h; a.s = ""; a.v = (int)seg; a.v2 = (int)off;
    a.v3 = (stride & 0x0FFF) | ((zoom & 15) << 12);
    svc(26, &a);
}
unsigned fs_changes(void) { struct sargs a; a.s = ""; a.v = 0; return (unsigned)svc(25, &a); }
int app_band_info(void *out19)
{
    struct sargs a;
    a.x = a.y = a.w = a.h = 0; a.s = (const char *)out19;
    a.v = a.v2 = a.v3 = 0;
    return svc(27, &a);
}
int app_desktop_focus(void)
{
    struct sargs a;
    a.s = "";
    return svc(28, &a);
}
int app_display_mode(unsigned mode)
{
    struct sargs a;
    a.x = a.y = a.w = a.h = 0; a.s = "";
    a.v = mode; a.v2 = a.v3 = 0;
    return svc(29, &a);
}
int app_wallpaper(struct app_wallpaper_info *info)
{
    struct sargs a;
    info->bytes=sizeof *info;
    a.x=a.y=a.w=a.h=a.v=a.v2=a.v3=0;
    a.s=(const char *)info;
    return svc(32,&a);
}
int app_wallpaper_apply(unsigned index,unsigned style)
{
    struct sargs a;
    struct app_wallpaper_info info;
    mem_set(&info,0,sizeof info);info.bytes=sizeof info;
    info.index=(unsigned char)index;info.style=(unsigned char)style;
    a.x=a.y=a.w=a.h=a.v2=a.v3=0;a.v=1;a.s=(const char *)&info;
    return svc(32,&a);
}
int app_wallpaper_count(void)
{
    struct sargs a;
    struct app_wallpaper_info info;
    mem_set(&info,0,sizeof info);info.bytes=sizeof info;
    a.x=a.y=a.w=a.h=a.v2=a.v3=0;a.v=2;a.s=(const char *)&info;
    return svc(32,&a);
}
void ui_cursor(int type)
{
    struct sargs a;
    a.x = a.y = a.w = a.h = a.v2 = a.v3 = 0; a.s = ""; a.v = type;
    svc(30, &a);
}
int app_audio(int op, struct app_audio_packet *packet)
{
    struct sargs a;
    a.x = a.y = a.w = a.h = a.v2 = a.v3 = 0;
    a.s = (const char *)packet; a.v = op;
    return svc(31, &a);
}
static void fs_changed(void) { struct sargs a; a.s = ""; a.v = 1; svc(25, &a); }

u16 app_seg(void) { return my_ds(); }

/* A line on COM1 for the test gates ("[FILES] ..."). */
static u8 inb(u16 port);
#pragma aux inb = "in al,dx" parm [dx] value [al];
static void outb(u16 port, u8 v);
#pragma aux outb = "out dx,al" parm [dx] [al];
static void com_char(char c)
{
    int guard = 0;
    while (!(inb(0x3FD) & 0x20) && ++guard < 30000) ;
    outb(0x3F8, (u8)c);
}
void app_log(const char *a, const char *b)
{
    while (*a) com_char(*a++);
    if (b) { com_char(' '); while (*b) com_char(*b++); }
    com_char('\r');
    com_char('\n');
}

/* ---------------- strings ---------------- */
int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
void str_copy(char *d, const char *s) { while ((*d++ = *s++) != 0) ; }
void str_ncopy(char *d, const char *s, int n)
{
    if (n <= 0) return;
    while (--n > 0 && *s) *d++ = *s++;
    *d = 0;
}
void str_cat(char *d, const char *s) { str_copy(d + str_len(d), s); }
int str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}
char to_upper(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int str_icmp(const char *a, const char *b)
{
    while (*a && to_upper(*a) == to_upper(*b)) { a++; b++; }
    return (u8)to_upper(*a) - (u8)to_upper(*b);
}
int str_nicmp(const char *a, const char *b, int n)
{
    while (n > 0 && *a && to_upper(*a) == to_upper(*b)) { a++; b++; n--; }
    if (!n) return 0;
    return (u8)to_upper(*a) - (u8)to_upper(*b);
}
void mem_copy(void *d, const void *s, int n)
{
    char *dd = d; const char *ss = s;
    while (n-- > 0) *dd++ = *ss++;
}
void mem_move(void *d, const void *s, int n)
{
    char *dd = d; const char *ss = s;
    if (dd < ss) { while (n-- > 0) *dd++ = *ss++; }
    else { dd += n; ss += n; while (n-- > 0) *--dd = *--ss; }
}
int mem_cmp(const void *a, const void *b, int n)
{
    const u8 *p = a, *q = b;
    while (n-- > 0) { if (*p != *q) return *p - *q; p++; q++; }
    return 0;
}
void mem_set(void *d, int v, int n) { char *dd = d; while (n-- > 0) *dd++ = (char)v; }

/* ---------------- numbers ---------------- */
void fmt_u32(char *d, u32 v)
{
    char t[12];
    int n = 0;
    do { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    while (n) *d++ = t[--n];
    *d = 0;
}
void fmt_u32_group(char *d, u32 v)
{
    char t[16];
    int n = 0, g = 0;
    do {
        if (g == 3) { t[n++] = ','; g = 0; }
        t[n++] = (char)('0' + (int)(v % 10)); v /= 10; g++;
    } while (v);
    while (n) *d++ = t[--n];
    *d = 0;
}
void fmt_hex4(char *d, u16 v)
{
    int i;
    for (i = 0; i < 4; i++) {
        int n = (v >> (12 - 4 * i)) & 15;
        d[i] = (char)(n < 10 ? '0' + n : 'A' + n - 10);
    }
    d[4] = 0;
}
void fmt_2(char *d, int v) { d[0] = (char)('0' + v / 10 % 10); d[1] = (char)('0' + v % 10); d[2] = 0; }
u16 parse_u16(const char *s)
{
    u16 v = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}
void fmt_size(char *d, u32 bytes)
{
    u32 kb = (bytes + 1023) / 1024;
    if (bytes == 0) { str_copy(d, "0 KB"); return; }
    if (kb < 10000) { fmt_u32_group(d, kb); str_cat(d, " KB"); return; }
    fmt_u32_group(d, (kb + 512) / 1024);
    str_cat(d, " MB");
}
void fmt_date(char *d, u16 fdate)
{
    int day = fdate & 31, month = (fdate >> 5) & 15, year = 1980 + (fdate >> 9);
    fmt_2(d, day); d[2] = '/';
    fmt_2(d + 3, month); d[5] = '/';
    fmt_u32(d + 6, (u32)year);
}
void fmt_time(char *d, u16 ftime)
{
    fmt_2(d, ftime >> 11); d[2] = ':';
    fmt_2(d + 3, (ftime >> 5) & 63);
}

/* ---------------- DOS ---------------- */
static struct regs R;
static void r_clear(void) { mem_set(&R, 0, sizeof R); R.ds = R.es = my_ds(); }
static int dos(void) { return intr(0x21, &R) ? -(int)R.ax : (int)R.ax; }
/* A call that changes the file system: views are told (fs_changes). */
static int dosm(void) { int r = dos(); if (r >= 0) fs_changed(); return r; }

void dos_set_dta(void *dta) { r_clear(); R.ax = 0x1A00; R.dx = (u16)dta; dos(); }
int dos_find_first(const char *p, int attr) { r_clear(); R.ax = 0x4E00; R.cx = attr; R.dx = (u16)p; return dos(); }
int dos_find_next(void) { r_clear(); R.ax = 0x4F00; return dos(); }

/* Long file names: the Windows 95 API (INT 21h AX=71xxh) of LFN.COM, the
 * kernel's resident extension. Each call below uses it when it is there
 * and falls back to the 8.3 call when it reports itself absent (7100h). */
#define LFN_ABSENT (-0x7100)
static int lfn_state = -1;
int dos_lfn(void)
{
    char fs[8];
    if (lfn_state < 0) {
        r_clear(); R.ax = 0x71A0; R.dx = (u16)"C:\\"; R.di = (u16)fs; R.cx = sizeof fs;
        lfn_state = !intr(0x21, &R) && (R.bx & 0x4000) != 0;
    }
    return lfn_state;
}
/* The LFN call in R: its result, or LFN_ABSENT (then R is set again). */
static int lfn(u16 ax, int modifies)
{
    int r;
    /* The first capability query uses R itself. Preserve the registers that
     * the caller prepared for its actual 71xxh operation. */
    struct regs requested = R;
    if (!dos_lfn()) { R = requested; return LFN_ABSENT; }
    R = requested;
    R.ax = ax;
    r = modifies ? dosm() : dos();
    return r;
}
static int lfn_open(const char *p, u16 mode, u16 attr, u16 action)
{
    r_clear(); R.bx = mode; R.cx = attr; R.dx = action; R.si = (u16)p; R.di = 1;
    return lfn(0x716C, action & 0x12);
}
int dos_open(const char *p, int mode)
{
    int r = lfn_open(p, mode, 0, 1);
    if (r != LFN_ABSENT) return r;
    r_clear(); R.ax = 0x3D00 | mode; R.dx = (u16)p; return dos();
}
int dos_create(const char *p)
{
    int r = lfn_open(p, 2, 0, 0x12), h;
    if (r != LFN_ABSENT) return r;
    r_clear(); R.ax = 0x3C00; R.dx = (u16)p;
    h = dosm();
    if (h >= 0) dos_write(h, "", 0);            /* an older kernel did not truncate */
    return h;
}
int dos_create_new(const char *p)
{
    int h = lfn_open(p, 2, 0, 0x10);
    if (h != LFN_ABSENT) return h;
    h = dos_open(p, 0);
    if (h >= 0) { dos_close(h); return -80; }
    return dos_create(p);
}
int dos_close(int h) { r_clear(); R.ax = 0x3E00; R.bx = h; return dos(); }
int dos_read(int h, void *b, int n) { r_clear(); R.ax = 0x3F00; R.bx = h; R.cx = n; R.dx = (u16)b; return dos(); }
int dos_write(int h, const void *b, int n) { r_clear(); R.ax = 0x4000; R.bx = h; R.cx = n; R.dx = (u16)b; return dos(); }
int dos_read_far(int h, u16 seg, u16 off, int n)
{
    r_clear(); R.ax = 0x3F00; R.bx = h; R.cx = n; R.dx = off; R.ds = seg; return dos();
}
int dos_write_far(int h, u16 seg, u16 off, int n)
{
    r_clear(); R.ax = 0x4000; R.bx = h; R.cx = n; R.dx = off; R.ds = seg; return dos();
}
long dos_seek(int h, long pos, int whence)
{
    r_clear(); R.ax = 0x4200 | whence; R.bx = h; R.cx = (u16)(pos >> 16); R.dx = (u16)pos;
    if (intr(0x21, &R)) return -(long)R.ax;
    return ((long)R.dx << 16) | R.ax;
}
/* A call on DS:DX (and ES:DI): the LFN function, else the 8.3 one. */
static int path_call(u16 lfn_ax, u16 ax, const char *p, const char *p2, int modifies)
{
    int r;
    r_clear(); R.dx = (u16)p; R.di = (u16)p2;
    r = lfn(lfn_ax, modifies);
    if (r != LFN_ABSENT) return r;
    r_clear(); R.ax = ax; R.dx = (u16)p; R.di = (u16)p2;
    return modifies ? dosm() : dos();
}
int dos_mkdir(const char *p) { return path_call(0x7139, 0x3900, p, 0, 1); }
int dos_rmdir(const char *p) { return path_call(0x713A, 0x3A00, p, 0, 1); }
int dos_delete(const char *p) { return path_call(0x7141, 0x4100, p, 0, 1); }
int dos_rename(const char *f, const char *t) { return path_call(0x7156, 0x5600, f, t, 1); }
int dos_get_attr(const char *p)
{
    int r;
    r_clear(); R.dx = (u16)p; R.bx = 0;
    r = lfn(0x7143, 0);
    if (r == LFN_ABSENT) { r_clear(); R.ax = 0x4300; R.dx = (u16)p; r = dos(); }
    return r < 0 ? r : (int)R.cx;
}
int dos_set_attr(const char *p, int a)
{
    int r;
    r_clear(); R.dx = (u16)p; R.bx = 1; R.cx = a;
    r = lfn(0x7143, 1);
    if (r != LFN_ABSENT) return r;
    r_clear(); R.ax = 0x4301; R.cx = a; R.dx = (u16)p; return dosm();
}
/* The 8.3 form of a path (for programs run by name, and the kernel's own
 * calls), and its long form. out holds SYS_PATH bytes. */
static int truename(const char *p, char *out, int form)
{
    int r;
    r_clear(); R.si = (u16)p; R.di = (u16)out; R.cx = form;
    r = lfn(0x7160, 0);
    if (r == LFN_ABSENT) { str_ncopy(out, p, SYS_PATH); return 0; }
    return r < 0 ? r : 0;
}
int dos_short_path(const char *p, char *out) { return truename(p, out, 1); }
int dos_long_path(const char *p, char *out) { return truename(p, out, 2); }

/* Folder listings with long names: a search handle each (they nest). */
static u8 fdata[318];                   /* Win32 find data */
static struct dos_find fdta;
static void ent_from_find(struct dir_ent *e)
{
    e->attr = fdata[0];
    e->time = *(u16 *)(fdata + 20);
    e->date = *(u16 *)(fdata + 22);
    e->size = *(u32 *)(fdata + 32);
    str_ncopy(e->name, (char *)fdata + 44, LFN_NAME);
    str_ncopy(e->alias, (char *)fdata + 304, 13);
}
static void ent_from_dta(struct dir_ent *e)
{
    e->attr = fdta.attr; e->time = fdta.time; e->date = fdta.date; e->size = fdta.size;
    str_copy(e->name, fdta.name);
    e->alias[0] = 0;
}
int dir_first(const char *pattern, int attr, struct dir_ent *e)
{
    int r;
    r_clear(); R.dx = (u16)pattern; R.di = (u16)fdata; R.cx = attr; R.si = 1;
    e->h = 0;
    r = lfn(0x714E, 0);
    if (r != LFN_ABSENT) {
        if (r < 0) return r;
        e->h = r;
        ent_from_find(e);
        return 0;
    }
    dos_set_dta(&fdta);
    r_clear(); R.ax = 0x4E00; R.cx = attr; R.dx = (u16)pattern;
    r = dos();
    if (r < 0) return r;
    e->h = -1;
    ent_from_dta(e);
    return 0;
}
int dir_next(struct dir_ent *e)
{
    int r;
    if (!e->h) return -18;
    if (e->h < 0) {
        dos_set_dta(&fdta);
        r_clear(); R.ax = 0x4F00;
        if ((r = dos()) < 0) { e->h = 0; return r; }
        ent_from_dta(e);
        return 0;
    }
    r_clear(); R.ax = 0x714F; R.bx = e->h; R.di = (u16)fdata; R.si = 1;
    if ((r = dos()) < 0) { dir_close(e); return r; }
    ent_from_find(e);
    return 0;
}
void dir_close(struct dir_ent *e)
{
    if (e->h > 0) { r_clear(); R.ax = 0x71A1; R.bx = e->h; dos(); }
    e->h = 0;
}
/* A name Windows accepts for a file or folder. */
int valid_file_name(const char *s)
{
    int n = str_len(s);
    int base = 0, ext = -1;
    if (!n || n > 255 || s[n - 1] == '.' || s[n - 1] == ' ' || s[0] == ' ') return 0;
    if (!dos_lfn()) {                   /* 8.3 only */
        const char *t;
        for (t = s; *t; t++) {
            if (*t == ' ' || *t == '+' || *t == ',' || *t == ';' || *t == '=' || *t == '[' || *t == ']') return 0;
            if (*t == '.') { if (ext >= 0) return 0; ext = 0; }
            else if (ext >= 0) { if (++ext > 3) return 0; }
            else if (++base > 8) return 0;
        }
        if (!base) return 0;
    }
    for (; *s; s++) {
        if ((u8)*s < 32) return 0;
        switch (*s) { case '\\': case '/': case ':': case '*': case '?': case '"': case '<': case '>': case '|': return 0; }
    }
    return 1;
}
int dos_get_drive(void) { r_clear(); R.ax = 0x1900; dos(); return R.ax & 0xFF; }
void dos_set_drive(int d) { r_clear(); R.ax = 0x0E00; R.dx = d; dos(); }
int dos_chdir(const char *p) { return path_call(0x713B, 0x3B00, p, 0, 0); }
int dos_getcwd(int drive, char *buf) { r_clear(); R.ax = 0x4700; R.dx = drive; R.si = (u16)buf; return dos(); }
int dos_set_file_time(int h, u16 t, u16 d)
{
    r_clear(); R.ax = 0x5701; R.bx = h; R.cx = t; R.dx = d;
    return dos();
}
int dos_file_time(int h, u16 *t, u16 *d)
{
    r_clear(); R.ax = 0x5700; R.bx = h;
    if (intr(0x21, &R)) return -(int)R.ax;
    *t = R.cx; *d = R.dx;
    return 0;
}
long dos_disk_free(int drive, long *total)
{
    long per;
    r_clear(); R.ax = 0x3600; R.dx = drive;
    intr(0x21, &R);
    if (R.ax == 0xFFFF) { if (total) *total = 0; return -1; }
    per = (long)R.ax * (long)R.cx;
    if (total) *total = per * (long)R.dx;
    return per * (long)R.bx;
}
void dos_get_date(int *y, int *m, int *d, int *wd)
{
    r_clear(); R.ax = 0x2A00; intr(0x21, &R);
    *y = R.cx; *m = R.dx >> 8; *d = R.dx & 0xFF; if (wd) *wd = R.ax & 0xFF;
}
void dos_get_time(int *h, int *m, int *s)
{
    r_clear(); R.ax = 0x2C00; intr(0x21, &R);
    *h = R.cx >> 8; *m = R.cx & 0xFF; if (s) *s = R.dx >> 8;
}
u16 dos_alloc(u16 paras)
{
    r_clear(); R.ax = 0x4800; R.bx = paras;
    if (intr(0x21, &R)) return 0;
    return R.ax;
}
void dos_free(u16 seg) { r_clear(); R.ax = 0x4900; R.es = seg; intr(0x21, &R); }
u16 dos_largest(void) { r_clear(); R.ax = 0x4800; R.bx = 0xFFFF; intr(0x21, &R); return R.bx; }
u16 dos_psp(void) { r_clear(); R.ax = 0x6200; intr(0x21, &R); return R.bx; }
int bios_printer(int port, int c)
{
    r_clear(); R.ax = (c < 0) ? 0x0200 : (u16)(c & 0xFF); R.dx = port;
    intr(0x17, &R);
    return R.ax >> 8;
}
u8 peek8(u16 seg, u16 off) { return *(u8 __far *)(((u32)seg << 16) | off); }
u16 peek16(u16 seg, u16 off) { return *(u16 __far *)(((u32)seg << 16) | off); }
void poke8(u16 seg, u16 off, u8 v) { *(u8 __far *)(((u32)seg << 16) | off) = v; }
void far_copy(u16 dseg, u16 doff, u16 sseg, u16 soff, u16 n)
{
    u8 __far *d = (u8 __far *)(((u32)dseg << 16) | doff);
    u8 __far *s = (u8 __far *)(((u32)sseg << 16) | soff);
    if (d < s || dseg != sseg) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
}

const char *dos_error_text(int e)
{
    if (e < 0) e = -e;
    switch (e) {
    case 2: return "The file was not found.";
    case 3: return "The path was not found.";
    case 4: return "Too many files are open.";
    case 5: return "Access is denied.";
    case 8: return "Not enough memory.";
    case 15: return "The drive is not valid.";
    case 16: return "The current folder cannot be removed.";
    case 17: return "The destination is on another drive.";
    case 18: return "There are no more files.";
    case 19: return "The disk is write-protected.";
    case 21: return "The drive is not ready.";
    case 80: return "A file with this name already exists.";
    case 82: return "The folder cannot be created.";
    case 112: return "There is not enough space on the disk.";
    }
    return "The disk operation failed.";
}

int key_ctrl_letter(int key)
{
    int c = KEY_CHAR(key);
    if (c >= 1 && c <= 26 && KEY_SCAN(key) != 0x1C && KEY_SCAN(key) != 0x0E &&
        KEY_SCAN(key) != 0x0F) return 'A' + c - 1;
    return 0;
}

int key_alt_letter(int key)
{
    static const char row1[] = "QWERTYUIOP", row2[] = "ASDFGHJKL", row3[] = "ZXCVBNM";
    int s = KEY_SCAN(key);
    if (KEY_CHAR(key) != 0) return 0;
    if (s >= 0x10 && s <= 0x19) return row1[s - 0x10];
    if (s >= 0x1E && s <= 0x26) return row2[s - 0x1E];
    if (s >= 0x2C && s <= 0x32) return row3[s - 0x2C];
    return 0;
}
