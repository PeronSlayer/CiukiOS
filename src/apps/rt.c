/* Runtime of the desktop application modules: shell services, strings,
 * numbers and DOS calls (no C library). See app.h. */
#include "app.h"

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
    struct sargs a;
    a.s = s;
    return svc(7, &a);
}
void ui_repaint(void) { call5(8, 0, 0, 0, 0, "", 0); }
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

void dos_set_dta(void *dta) { r_clear(); R.ax = 0x1A00; R.dx = (u16)dta; dos(); }
int dos_find_first(const char *p, int attr) { r_clear(); R.ax = 0x4E00; R.cx = attr; R.dx = (u16)p; return dos(); }
int dos_find_next(void) { r_clear(); R.ax = 0x4F00; return dos(); }
int dos_open(const char *p, int mode) { r_clear(); R.ax = 0x3D00 | mode; R.dx = (u16)p; return dos(); }
int dos_create(const char *p) { r_clear(); R.ax = 0x3C00; R.dx = (u16)p; return dos(); }
int dos_create_new(const char *p)
{
    int h = dos_open(p, 0);
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
int dos_mkdir(const char *p) { r_clear(); R.ax = 0x3900; R.dx = (u16)p; return dos(); }
int dos_rmdir(const char *p) { r_clear(); R.ax = 0x3A00; R.dx = (u16)p; return dos(); }
int dos_delete(const char *p) { r_clear(); R.ax = 0x4100; R.dx = (u16)p; return dos(); }
int dos_rename(const char *f, const char *t) { r_clear(); R.ax = 0x5600; R.dx = (u16)f; R.di = (u16)t; return dos(); }
int dos_get_attr(const char *p)
{
    r_clear(); R.ax = 0x4300; R.dx = (u16)p;
    if (intr(0x21, &R)) return -(int)R.ax;
    return R.cx;
}
int dos_set_attr(const char *p, int a) { r_clear(); R.ax = 0x4301; R.cx = a; R.dx = (u16)p; return dos(); }
int dos_get_drive(void) { r_clear(); R.ax = 0x1900; dos(); return R.ax & 0xFF; }
void dos_set_drive(int d) { r_clear(); R.ax = 0x0E00; R.dx = d; dos(); }
int dos_chdir(const char *p) { r_clear(); R.ax = 0x3B00; R.dx = (u16)p; return dos(); }
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
