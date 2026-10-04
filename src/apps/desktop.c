/* The desktop: its icons (the system's, and the files and folders of
 * C:\DESKTOP), and the context menus of the desktop, its icons, the
 * taskbar, the top bar and the windows' title bars.
 *
 * The shell paints the wallpaper, the bars and the windows. This module
 * paints on the desktop surface (window WIN_DESKTOP, under every window,
 * in screen coordinates) and shows its menus on the overlay (WIN_OVERLAY,
 * above everything). Icon positions are kept in \SYSTEM\UI\ICONPOS.DAT;
 * which system icons show is set in the Control Panel (DESKTOP.CFG). */
#include "app.h"

#define MAX_ICONS 48
#define CELL_W 84
#define CELL_H 82
#define TOP 38
#define DESK_DIR "C:\\DESKTOP"
#define TEXT_TYPES "TXT|INI|CFG|LOG|MD|ASM|C|H|BAT|DOC|ME|1ST|DIZ|NFO|INF|CSV|XML|HTM|JSON|PY|SH"

struct dicon {
    char name[LFN_NAME];    /* the full file name, or a system icon key */
    char label[16];
    int x, y;
    u8 sys;                 /* DI_* for a system icon, 0 for a file */
    u8 attr, sel;
    u32 size;
    u16 date, time;
};
static struct dicon icons[MAX_ICONS];
static int nicons, cur = -1;
static struct deskcfg cfg;
static int bin_full;
static u32 dir_sig;
static unsigned poll_tick;
static int area_w, area_h;
/* Compact top-bar readings. Polling uses only resident services: the disk
 * lamp follows filesystem changes, never a disk read in EV_POLL. */
static unsigned top_tick, top_disk_until, top_net_until;
static u16 top_seen_fs;
static u16 top_net_rx, top_net_tx;
static u16 top_mixer;
static u32 top_ram_total_kb;
static int top_cpu, top_net, top_volume, top_mute;
static u16 top_inw(u16 port);
#pragma aux top_inw = "in ax,dx" parm [dx] value [ax];
static void top_outw(u16 port, u16 v);
#pragma aux top_outw = "out dx,ax" parm [dx] [ax];
static u32 top_le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
/* MEMMAP.COM captures this complete E820 map before Jemm changes BIOS calls.
 * It is a capacity reading, loaded once; the poll path never reads a file. */
static void top_load_ram_total(void)
{
    static u8 map[16 + 64 * 24];
    int h = dos_open("\\SYSTEM\\MEMMAP.BIN", 0), n, count, i;
    u8 extra;
    u32 total = 0;
    top_ram_total_kb = 0;
    if (h < 0) return;
    n = dos_read(h, map, sizeof map);
    if (dos_read(h, &extra, 1) != 0) n = -1;
    dos_close(h);
    if (n < 16 || map[0] != 'C' || map[1] != 'M' ||
        map[2] != 'A' || map[3] != 'P' || map[4] != 1 || map[5] != 0 ||
        map[12] != 24 || map[13] || map[14] || map[15]) return;
    count = map[6] | ((int)map[7] << 8);
    if (count < 1 || count > 64 || n != 16 + count * 24) return;
    for (i = 0; i < count; ++i) {
        const u8 *p = map + 16 + i * 24;
        u32 base, length, cap, usable;
        if (top_le32(p + 16) != 1 || !(top_le32(p + 20) & 1) ||
            top_le32(p + 4)) continue;
        base = top_le32(p);
        length = top_le32(p + 8);
        cap = ((0xFFFFFFFFUL - base) >> 10) + 1;
        usable = top_le32(p + 12) || length > 0xFFFFFFFFUL - base
               ? cap : length >> 10;
        if (usable > 0xFFFFFFFFUL - total) return;
        total += usable;
    }
    top_ram_total_kb = total;
}
static void top_find_mixer(void)
{
    static const u16 ids[] = { 0x2415, 0x2425, 0x2445, 0x2485, 0x24C5, 0x24D5, 0x266E, 0x27DE, 0x7195, 0 };
    struct regs r;
    int i;
    top_mixer = 0;
    for (i = 0; ids[i] && !top_mixer; i++) {
        mem_set(&r, 0, sizeof r);
        r.ax = 0xB102; r.cx = ids[i]; r.dx = 0x8086;
        r.ds = r.es = app_seg();
        if (intr(0x1A, &r) || (r.ax & 0xFF00)) continue;
        {
            u16 bus = r.bx;
            mem_set(&r, 0, sizeof r);
            r.ax = 0xB109; r.bx = bus; r.di = 0x10;
            r.ds = r.es = app_seg();
            if (!intr(0x1A, &r) && !(r.ax & 0xFF00) && (r.cx & 1)) top_mixer = r.cx & 0xFFFC;
        }
    }
}
static void top_sample(void)
{
    u16 v = fs_changes();
    u16 seg = peek16(0, 0x61 * 4 + 2), off = peek16(0, 0x61 * 4);
    struct regs r;
    if (v != top_seen_fs) { top_seen_fs = v; top_disk_until = HOST.ticks + 18; }
    top_cpu = 100 - app_idle();
    if (top_cpu < 0) top_cpu = 0;
    if (top_cpu > 100) top_cpu = 100;
    top_volume = -1;
    top_mute = 0;
    if (top_mixer) {
        u16 level = top_inw(top_mixer + 0x02);
        int attenuation = (level >> 8) & 63;
        top_volume = 31 - (attenuation > 31 ? 31 : attenuation);
        top_mute = (level & 0x8000) != 0;
    }
    top_net = seg && peek8(seg, off + 3) == 'P' &&
              peek8(seg, off + 4) == 'K' && peek8(seg, off + 5) == 'T';
    if (top_net && peek8(seg, off + 11) == 'C' && peek8(seg, off + 12) == 'I' &&
        peek8(seg, off + 13) == 'U' && peek8(seg, off + 14) == 'K') {
        mem_set(&r, 0, sizeof r);
        r.ax = 0xFE02; r.ds = r.es = app_seg();
        if (!intr(0x61, &r)) {
            if (r.bx != top_net_rx || r.cx != top_net_tx) top_net_until = HOST.ticks + 18;
            top_net_rx = r.bx; top_net_tx = r.cx;
        }
    }
}
static void top_paint(void)
{
    int cell = area_w >= 800 ? 100 : 76;
    int meter = cell - 33;
    int x0 = area_w - 76 - cell * 5, x, i, color, fill, net_busy, disk_busy;
    char t[24], n[12];
    net_busy = top_net_until && (short)(top_net_until - HOST.ticks) > 0;
    disk_busy = top_disk_until && (short)(top_disk_until - HOST.ticks) > 0;
    ui_rect(x0, 3, cell * 5, 23, C_LIGHT);
    ui_rect(x0, 3, cell * 5, 1, C_PAPER);
    ui_rect(x0, 25, cell * 5, 1, C_SHADOW);
    for (i = 0; i < 5; i++) {
        x = x0 + i * cell;
        if (i) {
            ui_rect(x, 7, 1, 14, C_SHADOW);
            ui_rect(x + 1, 7, 1, 14, C_PAPER);
        }
        color = i == 0 ? (top_volume < 0 ? C_SHADOW : top_mute ? C_RED : C_PURPLE) :
                i == 1 ? (!top_net ? C_SHADOW : net_busy ? C_TEAL : C_GREEN) :
                i == 2 ? C_TEAL : i == 3 ? C_BLUE :
                (disk_busy ? C_YELLOW : C_SHADOW);
        if (i == 0) {                 /* speaker */
            ui_rect(x + 5, 11, 3, 7, color);
            ui_rect(x + 8, 9, 4, 11, color);
            ui_rect(x + 13, 11, 1, 7, color);
        } else if (i == 1) {          /* opposing network arrows */
            ui_rect(x + 5, 9, 9, 2, color);
            ui_rect(x + 12, 7, 2, 6, color);
            ui_rect(x + 6, 17, 9, 2, color);
            ui_rect(x + 6, 15, 2, 6, color);
        } else if (i == 2) {          /* processor die */
            ui_rect(x + 6, 8, 10, 12, color);
            ui_rect(x + 8, 10, 6, 8, C_LIGHT);
            ui_rect(x + 10, 12, 2, 4, color);
        } else if (i == 3) {          /* memory module */
            ui_rect(x + 5, 9, 11, 10, color);
            ui_rect(x + 7, 11, 7, 5, C_LIGHT);
            ui_rect(x + 7, 19, 2, 2, color);
            ui_rect(x + 12, 19, 2, 2, color);
        } else {                      /* disk activity lamp */
            ui_rect(x + 5, 9, 11, 11, color);
            ui_rect(x + 7, 11, 7, 3, C_LIGHT);
            ui_rect(x + 8, 17, 5, 1, C_LIGHT);
        }
        if (i == 0) {
            str_copy(t, cell == 76 ? "V " : "VOL ");
            if (top_volume < 0) str_cat(t, "--");
            else if (top_mute) str_cat(t, "M");
            else { fmt_u32(n, top_volume); str_cat(t, n); }
        }
        else if (i == 1) str_copy(t, top_net ? (cell == 76 ? "N ON" : "NET ON") :
                                          (cell == 76 ? "N OFF" : "NET OFF"));
        else if (i == 2) {
            str_copy(t, cell == 76 ? "C " : "CPU ");
            fmt_u32(n, top_cpu); str_cat(t, n); str_cat(t, "%");
        } else if (i == 3) {
            str_copy(t, cell == 76 ? "R " : "RAM ");
            if (top_ram_total_kb) {
                fmt_u32(n, (top_ram_total_kb + 512) / 1024);
                str_cat(t, n); str_cat(t, "M");
            } else str_cat(t, "--");
        } else str_copy(t, cell == 76 ? (disk_busy ? "D BUSY" : "D IDLE") :
                                           (disk_busy ? "DISK BUSY" : "DISK IDLE"));
        ui_text(x + 23, 4, t, C_INK);
        /* E820 gives capacity, not live free pages. Do not draw a usage
         * gauge from DOS or XMS counters, which cover different pools. */
        if (i != 3) ui_rect(x + 23, 22, meter, 2, C_FACE);
        fill = i == 0 ? (top_volume < 0 || top_mute ? 0 : top_volume * meter / 31) :
               i == 1 ? (!top_net ? 0 : net_busy ? meter : 5) :
               i == 2 ? top_cpu * meter / 100 :
               i == 3 ? 0 : (disk_busy ? meter : 0);
        if (fill > meter) fill = meter;
        if (fill > 0) ui_rect(x + 23, 22, fill, 2, color);
    }
}

struct sysdef { u8 bit; const char *label; const char *key; int icon; };
static const struct sysdef sysdefs[] = {
    { DI_COMPUTER, "Computer", "*COMPUTR", ICON_COMPUTER },
    { DI_PROGRAMS, "Programs", "*PROGRAM", ICON_WINDOWS },
    { DI_BIN, "Recycle Bin", "*BIN", ICON_BIN },
    { DI_CONTROL, "Control Panel", "*CONTROL", ICON_CONTROL },
    { DI_DOS, "DOS Prompt", "*DOS", ICON_DOS },
    { DI_FLOPPY, "Floppy", "*FLOPPY", ICON_FLOPPY },
    { DI_USB, "USB drive", "*USB", ICON_USB },
    { DI_CD, "CD-ROM", "*CDROM", ICON_CD } };
#define NSYS 8

static char msg[320];
static char work[2048];                     /* copies */

/* ------------------------------------------------------------------ */
/* Names and types                                                     */
static const char *ext_of(const char *name)
{
    const char *e = 0;
    while (*name) { if (*name == '.') e = name + 1; name++; }
    return e ? e : "";
}
static int ext_is(const char *name, const char *list)
{
    const char *e = ext_of(name);
    int n = str_len(e);
    while (*list) {
        int k = 0;
        while (list[k] && list[k] != '|') k++;
        if (k == n && n && !str_nicmp(e, list, n)) return 1;
        list += k;
        if (*list == '|') list++;
    }
    return 0;
}
static void desk_path(char *out, const char *name)
{
    if (str_len(name) + sizeof DESK_DIR + 1 > SYS_PATH) { out[0] = 0; return; }
    str_copy(out, DESK_DIR "\\");
    str_cat(out, name);
}
static const struct sysdef *sysdef_of(const struct dicon *d)
{
    int i;
    for (i = 0; i < NSYS; i++) if (sysdefs[i].bit == d->sys) return &sysdefs[i];
    return 0;
}
static int icon_id(const struct dicon *d)
{
    if (d->sys == DI_BIN) return bin_full ? ICON_BIN_FULL : ICON_BIN;
    if (d->sys) return sysdef_of(d)->icon;
    if (d->attr & A_DIR) return ICON_FOLDER;
    if (ext_is(d->name, "COM|EXE")) return ICON_PROGRAM;
    if (ext_is(d->name, "BAT")) return ICON_DOS;
    if (ext_is(d->name, "CFN")) return ICON_FONTFILE;
    return ICON_TEXT;
}
static const char *type_of(const struct dicon *d)
{
    if (d->sys) return "System folder";
    if (d->attr & A_DIR) return "File Folder";
    if (ext_is(d->name, "TXT|ME|1ST|DIZ|NFO")) return "Text Document";
    if (ext_is(d->name, "COM|EXE")) return "Application";
    if (ext_is(d->name, "BAT")) return "MS-DOS Batch File";
    if (ext_is(d->name, "CFN")) return "CiukiOS Font";
    return "File";
}

/* ------------------------------------------------------------------ */
/* Positions                                                           */
#pragma pack(push, 1)
struct posrec { char name[13]; int x, y; };
#pragma pack(pop)
static const char pos_path[] = "\\SYSTEM\\UI\\ICONPOS.DAT";
static struct posrec pos[MAX_ICONS];
static int npos;

/* ICONPOS.DAT keeps its original 8.3 record format. A long named file is
 * identified by its stable FAT alias, so old position files still load. */
static void pos_name(const struct dicon *d, char *out)
{
    char path[SYS_PATH], short_path[SYS_PATH];
    const char *base, *scan;
    if (d->sys) { str_ncopy(out, d->name, 13); return; }
    desk_path(path, d->name);
    if (dos_short_path(path, short_path) < 0) { str_ncopy(out, d->name, 13); return; }
    base = short_path;
    for (scan = short_path; *scan; scan++) if (*scan == '\\' || *scan == '/') base = scan + 1;
    str_ncopy(out, base, 13);
}

static void pos_load(void)
{
    int h = dos_open(pos_path, 0), n;
    npos = 0;
    if (h < 0) return;
    n = dos_read(h, pos, sizeof pos);
    dos_close(h);
    if (n > 0) npos = n / (int)sizeof(struct posrec);
}
static void pos_save(void)
{
    int h, i;
    for (i = 0; i < nicons && i < MAX_ICONS; i++) {
        pos_name(&icons[i], pos[i].name);
        pos[i].x = icons[i].x;
        pos[i].y = icons[i].y;
    }
    npos = i;
    h = dos_create(pos_path);
    if (h < 0) return;
    dos_write(h, pos, npos * (int)sizeof(struct posrec));
    dos_close(h);
}
static int rows(void) { int r = (area_h - 32 - 30 - TOP) / CELL_H; return r < 1 ? 1 : r; }
static int cols(void) { int c = (area_w - 8) / CELL_W; return c < 1 ? 1 : c; }
static int cell_x(int c) { return 8 + c * CELL_W; }
static int cell_y(int r) { return TOP + r * CELL_H; }
static int occupied(int x, int y, int skip)
{
    int i;
    for (i = 0; i < nicons; i++) {
        if (i == skip || icons[i].x < 0) continue;
        if (icons[i].x < x + CELL_W - 8 && icons[i].x + CELL_W - 8 > x &&
            icons[i].y < y + CELL_H - 8 && icons[i].y + CELL_H - 8 > y) return 1;
    }
    return 0;
}
/* The first free cell, down the columns from the top left. */
static void auto_place(int i)
{
    int c, r;
    for (c = 0; c < cols(); c++)
        for (r = 0; r < rows(); r++)
            if (!occupied(cell_x(c), cell_y(r), i)) { icons[i].x = cell_x(c); icons[i].y = cell_y(r); return; }
    icons[i].x = cell_x(0);
    icons[i].y = cell_y(0);
}
static void clamp_icon(int i)
{
    if (icons[i].x > area_w - CELL_W) icons[i].x = area_w - CELL_W;
    if (icons[i].y > area_h - 32 - CELL_H) icons[i].y = area_h - 32 - CELL_H;
    if (icons[i].x < 0) icons[i].x = 0;
    if (icons[i].y < 32) icons[i].y = 32;
}

/* ------------------------------------------------------------------ */
/* Loading                                                             */
static u32 list_sig(void)
{
    struct dir_ent f;
    u32 h = 0;
    int r, n = 0;
    for (r = dir_first(DESK_DIR "\\*.*", A_DIR | A_HIDDEN | A_SYSTEM | A_RDONLY | A_ARCH, &f); r >= 0 && n < 200; r = dir_next(&f), n++) {
        const char *c = f.name;
        h = h * 33 + f.size + f.date + ((u32)f.time << 3) + f.attr;
        while (*c) h = h * 31 + (u8)*c++;
    }
    dir_close(&f);
    return h + n;
}
static void log_icons(void)
{
    char t[40], n[8];
    int i;
    fmt_u32(t, (u32)nicons);
    app_log("[DESK] icons", t);
    for (i = 0; i < nicons; i++) {
        str_ncopy(t, icons[i].name, 24);
        str_cat(t, " ");
        fmt_u32(n, (u32)(icons[i].x + CELL_W / 2)); str_cat(t, n); str_cat(t, " ");
        fmt_u32(n, (u32)(icons[i].y + 20)); str_cat(t, n);
        app_log("[DESK] icon", t);
    }
}
static void reload(void)
{
    struct dir_ent f;
    char keep[LFN_NAME];
    int i, r, k;
    keep[0] = 0;
    if (cur >= 0 && cur < nicons) str_copy(keep, icons[cur].name);
    area_w = HOST.screen_w;
    area_h = HOST.screen_h;
    cfg_load(&cfg);
    bin_full = bin_items(0) > 0;
    pos_load();
    nicons = 0;
    for (i = 0; i < NSYS; i++) {
        struct dicon *d;
        if (cfg.icons_hidden & sysdefs[i].bit) continue;
        d = &icons[nicons++];
        mem_set(d, 0, sizeof *d);
        str_copy(d->name, sysdefs[i].key);
        str_copy(d->label, sysdefs[i].label);
        d->sys = sysdefs[i].bit;
    }
    make_dirs(DESK_DIR);
    for (r = dir_first(DESK_DIR "\\*.*", A_DIR | A_RDONLY | A_ARCH, &f); r >= 0 && nicons < MAX_ICONS; r = dir_next(&f)) {
        struct dicon *d;
        if (f.name[0] == '.' || (f.attr & A_VOLUME)) continue;
        d = &icons[nicons++];
        mem_set(d, 0, sizeof *d);
        str_ncopy(d->name, f.name, LFN_NAME);
        str_ncopy(d->label, f.name, 13);
        d->attr = f.attr;
        d->size = f.size;
        d->date = f.date;
        d->time = f.time;
    }
    dir_close(&f);
    dir_sig = list_sig();
    /* Saved positions first, then the new icons in free cells. */
    for (i = 0; i < nicons; i++) {
        char key[13];
        icons[i].x = -1;
        pos_name(&icons[i], key);
        for (k = 0; k < npos; k++) {
            if (!str_icmp(pos[k].name, key)) { icons[i].x = pos[k].x; icons[i].y = pos[k].y; clamp_icon(i); break; }
        }
    }
    for (i = 0; i < nicons; i++) if (icons[i].x < 0) auto_place(i);
    cur = -1;
    if (keep[0]) for (i = 0; i < nicons; i++) if (!str_icmp(icons[i].name, keep)) { cur = i; icons[i].sel = 1; }
    log_icons();
}
static void arrange(int by_type)
{
    int i, j, n = 0;
    struct dicon t;
    /* System icons first, then folders, then files; by name or type. */
    for (i = 1; i < nicons; i++) {
        mem_copy(&t, &icons[i], sizeof t);
        for (j = i; j > 0; j--) {
            struct dicon *p = &icons[j - 1];
            int a = t.sys ? 0 : (t.attr & A_DIR) ? 1 : 2, b = p->sys ? 0 : (p->attr & A_DIR) ? 1 : 2, c;
            if (a == 0 && b == 0) break;         /* system icons keep their order */
            c = a != b ? a - b : by_type ? str_icmp(ext_of(t.name), ext_of(p->name)) : 0;
            if (!c) c = str_icmp(t.name, p->name);
            if (c >= 0) break;
            mem_copy(&icons[j], p, sizeof t);
        }
        mem_copy(&icons[j], &t, sizeof t);
    }
    for (i = 0; i < nicons; i++, n++) {
        icons[i].x = cell_x(n / rows());
        icons[i].y = cell_y(n % rows());
    }
    cur = -1;
    pos_save();
    app_log("[DESK] arranged", by_type ? "type" : "name");
}
static void line_up(void)
{
    int i;
    for (i = 0; i < nicons; i++) {
        int c = (icons[i].x - 8 + CELL_W / 2) / CELL_W, r = (icons[i].y - TOP + CELL_H / 2) / CELL_H;
        if (c < 0) c = 0;
        if (r < 0) r = 0;
        if (r >= rows()) r = rows() - 1;
        icons[i].x = cell_x(c);
        icons[i].y = cell_y(r);
    }
    for (i = 0; i < nicons; i++) if (occupied(icons[i].x, icons[i].y, i)) auto_place(i);
    pos_save();
}

/* ------------------------------------------------------------------ */
/* Selection, geometry                                                 */
static int renaming = -1;
static struct field rename_field;
static char rename_buf[LFN_NAME];
static int drag_i = -1, dragging, drag_x, drag_y, drag_dx, drag_dy, drop_i = -1;
static unsigned last_click_tick;
static int last_click_i = -1;

static int icon_at(int sx, int sy)
{
    int i;
    for (i = nicons - 1; i >= 0; i--) {
        int x = icons[i].x, y = icons[i].y;
        if (sx >= x + 16 && sx < x + CELL_W - 16 && sy >= y && sy < y + 42) return i;
        if (sx >= x + 2 && sx < x + CELL_W - 2 && sy >= y + 42 && sy < y + 78) return i;
    }
    return -1;
}
static void damage_icon(int i)
{
    if (i >= 0 && i < nicons) ui_damage(icons[i].x - 2, icons[i].y - 2, CELL_W + 4, CELL_H + 2);
}
static void select_only(int i)
{
    int k;
    for (k = 0; k < nicons; k++) { if (icons[k].sel) damage_icon(k); icons[k].sel = 0; }
    if (i >= 0) { icons[i].sel = 1; damage_icon(i); }
    if (cur >= 0) damage_icon(cur);
    cur = i;
}
static int count_selected(void) { int i, n = 0; for (i = 0; i < nicons; i++) n += icons[i].sel; return n; }
static int file_selected(void)
{
    int i;
    for (i = 0; i < nicons; i++) if (icons[i].sel && !icons[i].sys) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Dialogs                                                             */
enum { D_NONE, D_MSG, D_DELETE, D_EMPTY };
static struct dialog dlg;
static int dlg_kind, delete_for_good;
static void message(const char *title, const char *text)
{
    msgbox(&dlg, title, text, "OK");
    dlg_kind = D_MSG;
    app_log("[DESK] message", text);
}

/* Properties of a file, a folder or the Recycle Bin: its own window. */
static struct dialog pdlg;
static struct dctl pdc[12];
static char pdl[6][64], ptitle[40], prop_path[SYS_PATH];
static int prop_icon, prop_bin;
static void pctl(int i, int type, int x, int y, int w, int h, const char *text, int id)
{
    mem_set(&pdc[i], 0, sizeof pdc[i]);
    pdc[i].type = type; pdc[i].x = x; pdc[i].y = y; pdc[i].w = w; pdc[i].h = h;
    pdc[i].text = text; pdc[i].id = id;
}
static void properties(int i)
{
    struct dicon *d = &icons[i];
    char t[72];
    int k = 0, y = 0;
    u32 bytes;
    prop_icon = icon_id(d);
    prop_bin = d->sys == DI_BIN;
    str_copy(pdl[0], d->label);
    if (prop_bin) {
        int n = bin_items(&bytes);
        str_copy(pdl[1], "Type: System folder");
        str_copy(pdl[2], "Location: C:\\RECYCLED");
        str_copy(pdl[3], "Contains: "); fmt_u32(t, (u32)n); str_cat(pdl[3], t); str_cat(pdl[3], " item(s)");
        str_copy(pdl[4], "Size: "); fmt_size(t, bytes); str_cat(pdl[4], t);
        pdl[5][0] = 0;
    } else {
        str_copy(pdl[1], "Type: "); str_cat(pdl[1], type_of(d));
        str_copy(pdl[2], "Location: " DESK_DIR);
        str_copy(pdl[3], "Size: ");
        if (d->attr & A_DIR) str_cat(pdl[3], "(folder)");
        else { fmt_size(t, d->size); str_cat(pdl[3], t); str_cat(pdl[3], " ("); fmt_u32_group(t, d->size); str_cat(pdl[3], t); str_cat(pdl[3], " bytes)"); }
        str_copy(pdl[4], "Modified: ");
        fmt_date(t, d->date); str_cat(pdl[4], t); str_cat(pdl[4], "  ");
        fmt_time(t, d->time); str_cat(pdl[4], t);
        pdl[5][0] = 0;
        desk_path(prop_path, d->name);
    }
    pctl(k++, DC_LABEL, 52, 8, 0, 16, pdl[0], 0); y = 44;
    pctl(k++, DC_LABEL, 0, y, 0, 16, pdl[1], 0); y += 22;
    pctl(k++, DC_LABEL, 0, y, 0, 16, pdl[2], 0); y += 22;
    pctl(k++, DC_LABEL, 0, y, 0, 16, pdl[3], 0); y += 22;
    pctl(k++, DC_LABEL, 0, y, 0, 16, pdl[4], 0); y += 30;
    if (prop_bin) {
        pctl(k++, DC_BUTTON, 0, y, 150, 26, "&Empty Recycle Bin", 4);
        pdc[k - 1].disabled = !bin_full;
        pctl(k++, DC_BUTTON, 250, y, 84, 26, "OK", 1);
    } else {
        pctl(k++, DC_GROUP, 0, y, 334, 50, "Attributes", 0); y += 22;
        pctl(k, DC_CHECK, 12, y, 0, 17, "&Read-only", 0); pdc[k++].value = (d->attr & A_RDONLY) != 0;
        pctl(k, DC_CHECK, 124, y, 0, 17, "&Hidden", 0); pdc[k++].value = (d->attr & A_HIDDEN) != 0;
        pctl(k, DC_CHECK, 222, y, 0, 17, "&Archive", 0); pdc[k++].value = (d->attr & A_ARCH) != 0;
        y += 42;
        pctl(k++, DC_BUTTON, 74, y, 84, 26, "OK", 1);
        pctl(k++, DC_BUTTON, 162, y, 84, 26, "Cancel", 2);
        pctl(k++, DC_BUTTON, 250, y, 84, 26, "&Apply", 3);
    }
    str_copy(ptitle, d->label);
    str_cat(ptitle, " Properties");
    if (pdlg.open) { pdlg.open = 0; dialog_sync(&pdlg); }
    dialog_show(&pdlg, ptitle, pdc, k, 350, y + 34, 1, 2);
    pdlg.modeless = 1;
    dialog_sync(&pdlg);
    app_log("[DESK] properties", d->name);
}
static void properties_apply(void)
{
    int k, attr;
    if (prop_bin || !prop_path[0]) return;
    for (k = 0; k < pdlg.n && pdc[k].type != DC_CHECK; k++) ;
    if (k + 2 >= pdlg.n) return;
    attr = dos_get_attr(prop_path);
    if (attr < 0) return;
    attr &= A_SYSTEM;
    if (pdc[k].value) attr |= A_RDONLY;
    if (pdc[k + 1].value) attr |= A_HIDDEN;
    if (pdc[k + 2].value) attr |= A_ARCH;
    dos_set_attr(prop_path, attr);
    reload();
    ui_repaint();
}
static void empty_bin_ask(void)
{
    u32 bytes;
    char t[12];
    int n = bin_items(&bytes);
    if (!n) return;
    fmt_u32(t, (u32)n);
    str_copy(msg, "Are you sure you want to delete all of the ");
    str_cat(msg, t);
    str_cat(msg, " items\nin the Recycle Bin? They will be deleted permanently.");
    msgbox(&dlg, "Confirm Multiple File Delete", msg, "Yes|No");
    dlg_kind = D_EMPTY;
    app_log("[DESK] confirm", msg);
}
static int props_event(int ev, int a, int b, int c)
{
    int r = -1;
    if (ev == EV_PAINT) {
        dialog_draw(&pdlg);
        ui_icon(pdlg.x + 6, pdlg.y + DIALOG_TITLE_H + 2, prop_icon);
        return 0;
    }
    if (ev == EV_CLOSE) { pdlg.win = 0; pdlg.open = 0; return 0; }
    if (ev == EV_KEY) r = dialog_key(&pdlg, a, HOST.shift);
    if (ev == EV_MOUSE) {
        if (a == MOUSE_HOVER) { ui_dirty = 0; dialog_hover(&pdlg, HOST.x + b, HOST.y + TITLE_H + c); return ui_dirty; }
        r = dialog_mouse(&pdlg, a, HOST.x + b, HOST.y + TITLE_H + c);
    }
    if (r == 1 || r == 3) properties_apply();
    if (r == 3) pdlg.open = 1;
    if (r == 4) { pdlg.open = 0; empty_bin_ask(); }
    dialog_sync(&pdlg);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Operations                                                          */
static void open_icon(int i, int how)           /* how: 0 open, 1 Notepad, 2 explore */
{
    struct dicon *d;
    char p[SYS_PATH + 8];
    if (i < 0 || i >= nicons) return;
    d = &icons[i];
    app_log("[DESK] open", d->name);
    switch (d->sys) {
    case DI_COMPUTER: app_open(WIN_FILES, "C:\\"); return;
    case DI_PROGRAMS: shell_action(1); return;
    case DI_BIN: app_open(WIN_RECYCLE, ""); return;
    case DI_CONTROL: app_open(WIN_CONTROL, ""); return;
    case DI_DOS: shell_action(3); return;
    case DI_FLOPPY: app_open(WIN_FILES, "media:1:/"); return;
    case DI_USB: app_open(WIN_FILES, "media:2:/"); return;
    case DI_CD: app_open(WIN_FILES, "media:3:/"); return;
    }
    desk_path(p, d->name);
    if (d->attr & A_DIR) { app_open(WIN_FILES, p); return; }
    if (how == 0 && ext_is(d->name, "COM|EXE|BAT")) {
        dos_set_drive(2);
        dos_chdir(DESK_DIR);
        app_open(WIN_DOS, p);
        return;
    }
    if (how == 0 && ext_is(d->name, "CFN")) {
        char t[SYS_PATH + 8];
        str_copy(t, "font:"); str_cat(t, p);
        app_open(WIN_CONTROL, t);
        return;
    }
    app_open(WIN_NOTEPAD, p);
}
static void new_item(int folder, int x, int y)
{
    char name[LFN_NAME], path[SYS_PATH], n[4];
    int i, r;
    for (i = 0; i < 100; i++) {
        str_copy(name, folder ? "New Folder" : "New Text Document");
        if (i) {
            fmt_u32(n, (u32)i);
            str_cat(name, " ("); str_cat(name, n); str_cat(name, ")");
        }
        if (!folder) str_cat(name, ".txt");
        desk_path(path, name);
        if (dos_get_attr(path) < 0) break;
    }
    if (folder) r = dos_mkdir(path);
    else { r = dos_create_new(path); if (r >= 0) dos_close(r); }
    if (r < 0) { message(folder ? "New Folder" : "New Text Document", dos_error_text(r)); return; }
    app_log("[DESK] created", name);
    reload();
    for (i = 0; i < nicons; i++) if (!str_icmp(icons[i].name, name)) {
        if (x >= 0) {
            icons[i].x = x - CELL_W / 2; icons[i].y = y - 20;
            clamp_icon(i);
            if (occupied(icons[i].x, icons[i].y, i)) auto_place(i);
            pos_save();
        }
        select_only(i);
        renaming = i;
        field_set(&rename_field, rename_buf, LFN_NAME, name);
        rename_field.sel = 1;
    }
    ui_repaint();
}
static void commit_rename(void)
{
    char from[SYS_PATH], to[SYS_PATH], name[LFN_NAME];
    int r, i = renaming;
    renaming = -1;
    ui_repaint();
    if (i < 0 || icons[i].sys) return;
    str_ncopy(name, rename_field.text, LFN_NAME);
    if (!str_cmp(name, icons[i].name)) return;
    if (!valid_file_name(name) || str_len(name) + sizeof DESK_DIR + 1 > SYS_PATH) {
        message("Rename", "Invalid file name or path too long.");
        return;
    }
    desk_path(from, icons[i].name);
    desk_path(to, name);
    if (dos_get_attr(to) >= 0) { message("Rename", "A file or folder with that name already exists."); return; }
    r = dos_rename(from, to);
    if (r < 0) { message("Rename", dos_error_text(r)); return; }
    str_copy(icons[i].name, name);
    pos_save();                              /* the position follows the name */
    app_log("[DESK] renamed", name);
    reload();
}
static void delete_ask(int permanent)
{
    int i, n = 0, first = -1;
    char t[12];
    for (i = 0; i < nicons; i++) if (icons[i].sel && !icons[i].sys) { n++; if (first < 0) first = i; }
    if (!n) return;
    delete_for_good = permanent;
    if (n == 1) {
        str_copy(msg, permanent ? "Are you sure you want to permanently delete '" : "Are you sure you want to send '");
        str_cat(msg, icons[first].name);
        str_cat(msg, permanent ? "'?" : "' to the Recycle Bin?");
    } else {
        fmt_u32(t, (u32)n);
        str_copy(msg, permanent ? "Are you sure you want to permanently delete these " : "Are you sure you want to send these ");
        str_cat(msg, t);
        str_cat(msg, permanent ? " items?" : " items\nto the Recycle Bin?");
    }
    msgbox(&dlg, n == 1 ? "Confirm File Delete" : "Confirm Multiple File Delete", msg, "Yes|No");
    dlg_kind = D_DELETE;
    app_log("[DESK] confirm", msg);
}
static void delete_selected(void)
{
    char p[SYS_PATH], n[8];
    int i, r = 0, done = 0;
    for (i = 0; i < nicons; i++) {
        if (!icons[i].sel || icons[i].sys) continue;
        desk_path(p, icons[i].name);
        r = delete_for_good ? tree_delete(p) : bin_send(p);
        if (r < 0) { str_copy(msg, icons[i].name); str_cat(msg, ": "); str_cat(msg, dos_error_text(r)); break; }
        done++;
    }
    fmt_u32(n, (u32)done);
    app_log(delete_for_good ? "[DESK] deleted" : "[DESK] recycled", n);
    reload();
    ui_repaint();
    if (r < 0) message("Delete", msg);
}
static void to_clipboard(int cut)
{
    char p[SYS_PATH];
    int i, n = 0;
    clip_begin(cut);
    for (i = 0; i < nicons; i++) {
        if (!icons[i].sel || icons[i].sys) continue;
        desk_path(p, icons[i].name);
        clip_add(p, 0);
        n++;
    }
    clip_end();
    app_log(cut ? "[DESK] cut" : "[DESK] copied", 0);
}
/* dst for a copy of name in dir: NAME, then NAME~1... */
static void target_name(char *dst, const char *dir, const char *name, int unique)
{
    char base[9], ext[5], t[16], n[4];
    int i, k = 0;
    const char *e = ext_of(name);
    str_copy(dst, dir); str_cat(dst, "\\"); str_cat(dst, name);
    if (!unique || dos_get_attr(dst) < 0) return;
    for (i = 0; name[i] && name[i] != '.' && k < 8; i++) base[k++] = name[i];
    base[k] = 0;
    str_ncopy(ext, e, 4);
    for (i = 1; i < 100; i++) {
        fmt_u32(n, (u32)i);
        str_ncopy(t, base, 8 - str_len(n));
        str_cat(t, "~"); str_cat(t, n);
        if (ext[0]) { str_cat(t, "."); str_cat(t, ext); }
        str_copy(dst, dir); str_cat(dst, "\\"); str_cat(dst, t);
        if (dos_get_attr(dst) < 0) return;
    }
}
static const char *base_of(const char *p)
{
    const char *b = p;
    while (*p) { if (*p == '\\' || *p == '/' || *p == ':') b = p + 1; p++; }
    return b;
}
/* The clipboard into dir (the desktop or one of its folders). */
static void paste_into(const char *dir)
{
    char src[SYS_PATH], dst[SYS_PATH], n[8];
    int cut, count = clip_count(&cut), i, r = 0, done = 0, media;
    for (i = 0; i < count; i++) {
        media = clip_get(i, src);
        if (media) { r = -5; str_copy(msg, "Files from removable media are pasted in Files."); break; }
        if (!str_icmp(src, dir)) continue;
        target_name(dst, dir, base_of(src), !cut);
        if (!str_icmp(src, dst)) continue;
        if (cut && dos_get_attr(dst) >= 0) { r = -80; str_copy(msg, base_of(src)); str_cat(msg, ": a file or folder with that name exists."); break; }
        r = cut ? tree_move(src, dst) : tree_copy(src, dst, work, sizeof work);
        if (r < 0) { str_copy(msg, base_of(src)); str_cat(msg, ": "); str_cat(msg, dos_error_text(r)); break; }
        done++;
    }
    if (cut && r >= 0) clip_clear();
    fmt_u32(n, (u32)done);
    app_log(cut ? "[DESK] pasted (moved)" : "[DESK] pasted", n);
    reload();
    ui_repaint();
    if (r < 0) message("Paste", msg);
}
/* Selected icons dropped on folder or Recycle Bin icon t. */
static void drop_on(int t)
{
    char src[SYS_PATH], dst[SYS_PATH], dir[SYS_PATH];
    int i, r = 0;
    if (icons[t].sys == DI_BIN) { delete_for_good = 0; delete_selected(); return; }
    desk_path(dir, icons[t].name);
    for (i = 0; i < nicons; i++) {
        if (!icons[i].sel || icons[i].sys || i == t) continue;
        desk_path(src, icons[i].name);
        target_name(dst, dir, icons[i].name, 0);
        if (dos_get_attr(dst) >= 0) { r = -80; break; }
        r = tree_move(src, dst);
        if (r < 0) break;
    }
    app_log("[DESK] moved into", icons[t].name);
    reload();
    ui_repaint();
    if (r < 0) message("Move", r == -80 ? "A file or folder with that name is already there." : dos_error_text(r));
}
static void show_desktop(void)
{
    static char list[32 * 25];
    int n = app_windows(list), w;
    for (w = 1; w < n; w++) if ((list[w * 25] & 0x7F) == 1) app_window_cmd(w, 3);
    app_log("[DESK] show desktop", 0);
}

/* ------------------------------------------------------------------ */
/* Menus                                                               */
enum {
    M_OPEN = 1, M_NOTEPAD, M_EXPLORE, M_CUT, M_COPY, M_PASTE, M_DELETE, M_RENAME, M_PROPS,
    M_HIDE, M_EMPTY, M_ARR_NAME, M_ARR_TYPE, M_LINEUP, M_REFRESH, M_NEWFOLDER, M_NEWTEXT,
    M_DESK_PROPS, M_TASKS, M_SHOWDESK, M_CONTROL, M_RUN, M_PROGRAMS, M_DOS, M_FILES, M_ABOUT,
    M_SHUTDOWN, M_W_RESTORE, M_W_MIN, M_W_MAX, M_W_CLOSE, M_DISPLAY, M_SOUND, M_NETWORK
};
static struct menu_item m_back[] = {
    { "Arrange Icons by &Name", 0, M_ARR_NAME, 0 }, { "Arrange Icons by &Type", 0, M_ARR_TYPE, 0 },
    { "&Line Up Icons", 0, M_LINEUP, 0 }, { "", 0, 0, MI_SEP }, { "R&efresh", "F5", M_REFRESH, 0 },
    { "", 0, 0, MI_SEP }, { "&Paste", "Ctrl+V", M_PASTE, 0 }, { "", 0, 0, MI_SEP },
    { "New &Folder", 0, M_NEWFOLDER, 0 }, { "New Text &Document", 0, M_NEWTEXT, 0 },
    { "", 0, 0, MI_SEP }, { "P&roperties", 0, M_DESK_PROPS, 0 } };
static struct menu_item m_file[] = {
    { "&Open", "Enter", M_OPEN, 0 }, { "Open with Ciuk&Note", 0, M_NOTEPAD, 0 }, { "", 0, 0, MI_SEP },
    { "Cu&t", "Ctrl+X", M_CUT, 0 }, { "&Copy", "Ctrl+C", M_COPY, 0 }, { "&Paste", 0, M_PASTE, 0 },
    { "", 0, 0, MI_SEP }, { "&Delete", "Del", M_DELETE, 0 }, { "Rena&me", "F2", M_RENAME, 0 },
    { "", 0, 0, MI_SEP }, { "P&roperties", "Alt+Enter", M_PROPS, 0 } };
static struct menu_item m_computer[] = {
    { "&Open", 0, M_OPEN, 0 }, { "&Explore", 0, M_EXPLORE, 0 }, { "", 0, 0, MI_SEP },
    { "Re&move from Desktop", 0, M_HIDE, 0 }, { "", 0, 0, MI_SEP }, { "P&roperties", 0, M_PROPS, 0 } };
static struct menu_item m_bin[] = {
    { "&Open", 0, M_OPEN, 0 }, { "Empty Recycle &Bin", 0, M_EMPTY, 0 }, { "", 0, 0, MI_SEP },
    { "Re&move from Desktop", 0, M_HIDE, 0 }, { "", 0, 0, MI_SEP }, { "P&roperties", 0, M_PROPS, 0 } };
static struct menu_item m_sys[] = {
    { "&Open", 0, M_OPEN, 0 }, { "", 0, 0, MI_SEP }, { "Re&move from Desktop", 0, M_HIDE, 0 } };
static struct menu_item m_taskbar[] = {
    { "&Task Manager", "Ctrl+Shift+Esc", M_TASKS, 0 }, { "&Show the Desktop", "Win+D", M_SHOWDESK, 0 },
    { "", 0, 0, MI_SEP }, { "&Programs", "Win", M_PROGRAMS, 0 }, { "&Run...", "Win+R", M_RUN, 0 },
    { "", 0, 0, MI_SEP }, { "&Control Panel", 0, M_CONTROL, 0 } };
static struct menu_item m_topbar[] = {
    { "&Programs", "Win", M_PROGRAMS, 0 }, { "&Run...", "Win+R", M_RUN, 0 }, { "&DOS Prompt", 0, M_DOS, 0 },
    { "", 0, 0, MI_SEP }, { "&Files", "Win+E", M_FILES, 0 }, { "&Control Panel", 0, M_CONTROL, 0 },
    { "&Task Manager", 0, M_TASKS, 0 }, { "", 0, 0, MI_SEP }, { "&About CiukiOS", "Win+F1", M_ABOUT, 0 },
    { "Shut Do&wn...", 0, M_SHUTDOWN, 0 } };
/* The CiukiOS menu: drops from the brand in the top bar. */
static struct menu_item m_start[] = {
    { "&Programs", "Win", M_PROGRAMS, 0 }, { "&Files", "Win+E", M_FILES, 0 },
    { "&Run...", "Win+R", M_RUN, 0 }, { "&DOS Prompt", 0, M_DOS, 0 }, { "", 0, 0, MI_SEP },
    { "&Control Panel", 0, M_CONTROL, 0 }, { "D&isplay", 0, M_DISPLAY, 0 },
    { "&Sound", 0, M_SOUND, 0 }, { "&Network", 0, M_NETWORK, 0 },
    { "&Task Manager", "Ctrl+Shift+Esc", M_TASKS, 0 }, { "", 0, 0, MI_SEP },
    { "&About CiukiOS", "Win+F1", M_ABOUT, 0 }, { "Shut Do&wn...", 0, M_SHUTDOWN, 0 } };
static struct menu_item m_window[] = {
    { "&Restore", 0, M_W_RESTORE, 0 }, { "Mi&nimize", 0, M_W_MIN, 0 }, { "Ma&ximize", 0, M_W_MAX, 0 },
    { "", 0, 0, MI_SEP }, { "&Close", "Alt+F4", M_W_CLOSE, 0 } };
static struct popup pop;
static int menu_icon = -1, menu_window = -1, menu_x, menu_y;

static void flag(struct menu_item *it, int f, int on) { if (on) it->flags |= f; else it->flags &= ~f; }
static void damage_pop(void) { ui_damage(pop.x, pop.y, pop.w + 4, pop.h + 4); }
static void menu_close(void)
{
    if (pop.w) damage_pop();
    pop.open = 0;
    ui_overlay(0);
}
static void menu_open(struct menu_item *items, int count, int x, int y, const char *what)
{
    popup_open(&pop, items, count, x, y, area_w - 2, area_h - 2);
    ui_overlay(1);
    damage_pop();
    app_log("[DESK] menu", what);
}
/* The speaker indicator's popup: the master volume only, and a gear for the
 * Sound applet. It lives on the overlay like the menus. */
#define VOL_W 220
#define VOL_H 60
#define VOL_TRACK_X 14
#define VOL_TRACK_W 150
static struct { int open, x, y, drag; } vol;
static void vol_damage(void) { ui_damage(vol.x, vol.y, VOL_W + 4, VOL_H + 4); }
static void vol_close(void)
{
    if (!vol.open) return;
    vol.open = 0;
    vol_damage();
    ui_overlay(0);
}
static void vol_open(void)
{
    int cell = area_w >= 800 ? 100 : 76;
    menu_close();
    vol.x = area_w - 76 - cell * 5;
    if (vol.x + VOL_W > area_w - 4) vol.x = area_w - 4 - VOL_W;
    if (vol.x < 2) vol.x = 2;
    vol.y = 29;
    vol.drag = 0;
    vol.open = 1;
    top_sample();
    ui_overlay(1);
    vol_damage();
    app_log("[DESK] volume", "open");
}
static void vol_set(int level)
{
    int cell = area_w >= 800 ? 100 : 76;
    if (!top_mixer) return;
    if (level < 0) level = 0;
    if (level > 31) level = 31;
    top_outw(top_mixer + 0x02, (u16)(((31 - level) << 8) | (31 - level)));   /* unmuted */
    top_volume = level;
    top_mute = 0;
    vol_damage();
    ui_damage(area_w - 76 - cell * 5, 0, cell, 29);
}
static void vol_draw(void)
{
    int x = vol.x, y = vol.y, t, gx = x + VOL_W - 38, gy = y + 26;
    char n[16];
    ui_bevel(x, y, VOL_W, VOL_H, C_FACE);
    ui_text(x + VOL_TRACK_X, y + 7, "Volume", C_INK);
    if (top_volume < 0) str_copy(n, "No device");
    else if (top_mute) str_copy(n, "Muted");
    else fmt_u32(n, top_volume);
    ui_text(x + VOL_TRACK_X + VOL_TRACK_W - ui_measure(n), y + 7, n, C_INK);
    ui_inset(x + VOL_TRACK_X, y + 37, VOL_TRACK_W, 5);
    t = x + VOL_TRACK_X + (int)((long)(VOL_TRACK_W - 11) * (top_volume < 0 ? 0 : top_volume) / 31);
    ui_bevel(t, y + 29, 11, 21, C_FACE);
    /* The gear: advanced sound settings. */
    ui_bevel(gx, gy, 26, 26, C_FACE);
    ui_rect(gx + 11, gy + 4, 4, 18, C_INK);
    ui_rect(gx + 4, gy + 11, 18, 4, C_INK);
    ui_rect(gx + 7, gy + 7, 12, 12, C_INK);
    ui_rect(gx + 6, gy + 6, 3, 3, C_INK);
    ui_rect(gx + 17, gy + 6, 3, 3, C_INK);
    ui_rect(gx + 6, gy + 17, 3, 3, C_INK);
    ui_rect(gx + 17, gy + 17, 3, 3, C_INK);
    ui_rect(gx + 10, gy + 10, 6, 6, C_FACE);
}
static int vol_level_at(int sx)
{
    return (int)((long)(sx - (vol.x + VOL_TRACK_X + 5)) * 31 / (VOL_TRACK_W - 11));
}
static int vol_mouse(int kind, int sx, int sy)
{
    int inside = sx >= vol.x && sx < vol.x + VOL_W && sy >= vol.y && sy < vol.y + VOL_H;
    if (kind == MOUSE_HOVER) return 0;
    if (kind == MOUSE_UP) { vol.drag = 0; return 1; }
    if (kind == MOUSE_MOVE) { if (vol.drag) vol_set(vol_level_at(sx)); return 1; }
    if (kind != MOUSE_DOWN) { vol_close(); return 1; }
    if (!inside) { vol_close(); return 1; }
    if (sx >= vol.x + VOL_W - 38 && sy >= vol.y + 26) {
        vol_close();
        app_open(WIN_CONTROL, "sound");
        return 1;
    }
    if (sx < vol.x + VOL_TRACK_X + VOL_TRACK_W + 6 && sy >= vol.y + 26) {
        vol.drag = 1;
        vol_set(vol_level_at(sx));
    }
    return 1;
}
static int module_window(int w) { return w == WIN_FILES || w == WIN_TASKS || (w >= WIN_NOTEPAD && w <= WIN_BROWSER); }
static void window_menu(int w, int x, int y)
{
    static char list[32 * 25];
    char t[8];
    int st;
    app_windows(list);
    st = list[w * 25] & 0x7F;
    if (!st) return;
    menu_window = w;
    flag(&m_window[0], MI_DISABLED, st != 2 && (list[w * 25] & 0x80));
    flag(&m_window[1], MI_DISABLED, w == 0 || st == 2 || w > WIN_BROWSER);   /* dialogs */
    flag(&m_window[2], MI_DISABLED, !module_window(w));
    fmt_2(t, w);
    menu_open(m_window, 5, x, y, "window");
    app_log("[DESK] window menu", t);
}
static void context_menu(int x, int y, int context)
{
    int i, cut, clip = clip_count(&cut) > 0;
    menu_icon = -1;
    menu_window = -1;
    menu_x = x; menu_y = y;
    if (renaming >= 0) commit_rename();
    if (context >= 0x100 && context < 0x300) { window_menu(context & 0xFF, x, y); return; }
    if (context >= 200 && context < 232) { window_menu(context - 200, x, y); return; }
    if (y < 29) { menu_open(m_topbar, 10, x, y, "top bar"); return; }
    if (y >= area_h - 32) { menu_open(m_taskbar, 7, x, y, "taskbar"); return; }
    if (context) return;
    i = icon_at(x, y);
    if (i < 0) {
        select_only(-1);
        flag(&m_back[6], MI_DISABLED, !clip);
        menu_open(m_back, 12, x, y, "background");
        return;
    }
    if (!icons[i].sel) select_only(i);
    cur = i;
    menu_icon = i;
    switch (icons[i].sys) {
    case 0:
        flag(&m_file[1], MI_DISABLED, (icons[i].attr & A_DIR) != 0);
        flag(&m_file[5], MI_DISABLED, !clip || !(icons[i].attr & A_DIR) || count_selected() > 1);
        menu_open(m_file, 11, x, y, (icons[i].attr & A_DIR) ? "folder" : "file");
        break;
    case DI_COMPUTER: menu_open(m_computer, 6, x, y, "computer"); break;
    case DI_BIN:
        flag(&m_bin[1], MI_DISABLED, !bin_full);
        menu_open(m_bin, 6, x, y, "recycle bin");
        break;
    default: menu_open(m_sys, 3, x, y, "system"); break;
    }
}
static void start_rename(int i)
{
    if (i < 0 || icons[i].sys) return;
    renaming = i;
    field_set(&rename_field, rename_buf, LFN_NAME, icons[i].name);
    rename_field.sel = 1;
    damage_icon(i);
}
static void command(int id)
{
    int i = menu_icon >= 0 ? menu_icon : cur, w = menu_window;
    menu_close();
    menu_window = -1;
    menu_icon = -1;
    switch (id) {
    case M_OPEN: open_icon(i, 0); break;
    case M_NOTEPAD: open_icon(i, 1); break;
    case M_EXPLORE: app_open(WIN_FILES, "C:\\"); break;
    case M_CUT: to_clipboard(1); break;
    case M_COPY: to_clipboard(0); break;
    case M_PASTE:
        if (i >= 0 && !icons[i].sys && (icons[i].attr & A_DIR)) {
            char p[SYS_PATH];
            desk_path(p, icons[i].name);
            paste_into(p);
        } else paste_into(DESK_DIR);
        break;
    case M_DELETE: delete_ask(HOST.shift & SH_SHIFT); break;
    case M_RENAME: start_rename(i); break;
    case M_PROPS:
        if (i < 0) break;
        if (icons[i].sys == DI_COMPUTER) app_open(WIN_CONTROL, "system");
        else properties(i);
        break;
    case M_HIDE:
        if (i < 0 || !icons[i].sys) break;
        cfg.icons_hidden |= icons[i].sys;
        cfg_save(&cfg);
        reload();
        ui_repaint();
        break;
    case M_EMPTY: empty_bin_ask(); break;
    case M_ARR_NAME: arrange(0); ui_repaint(); break;
    case M_ARR_TYPE: arrange(1); ui_repaint(); break;
    case M_LINEUP: line_up(); ui_repaint(); break;
    case M_REFRESH: reload(); ui_repaint(); break;
    case M_NEWFOLDER: new_item(1, menu_x, menu_y); break;
    case M_NEWTEXT: new_item(0, menu_x, menu_y); break;
    case M_DESK_PROPS: app_open(WIN_CONTROL, "appearance"); break;
    case M_TASKS: shell_action(23); break;
    case M_SHOWDESK: show_desktop(); break;
    case M_CONTROL: app_open(WIN_CONTROL, ""); break;
    case M_RUN: shell_action(4); break;
    case M_PROGRAMS: shell_action(1); break;
    case M_DOS: shell_action(3); break;
    case M_FILES: app_open(WIN_FILES, ""); break;
    case M_ABOUT: shell_action(6); break;
    case M_SHUTDOWN: shell_action(7); break;
    case M_DISPLAY: app_open(WIN_CONTROL, "display"); break;
    case M_SOUND: app_open(WIN_CONTROL, "sound"); break;
    case M_NETWORK: app_open(WIN_CONTROL, "network"); break;
    case M_W_RESTORE: app_window_cmd(w, 1); break;
    case M_W_MIN: app_window_cmd(w, 3); break;
    case M_W_MAX: app_window_cmd(w, 4); break;
    case M_W_CLOSE: app_window_cmd(w, 2); break;
    }
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
static void draw_icon(int i, int dx, int dy, int ghost)
{
    struct dicon *d = &icons[i];
    int x = d->x + dx, y = d->y + dy, tx, tw;
    char t[24];
    if (ghost) {
        ui_rect(x + 20, y, CELL_W - 40, 1, C_PAPER); ui_rect(x + 20, y + 41, CELL_W - 40, 1, C_PAPER);
        ui_rect(x + 20, y, 1, 42, C_PAPER); ui_rect(x + CELL_W - 21, y, 1, 42, C_PAPER);
        return;
    }
    if (drop_i == i) ui_rect(x + 18, y - 1, CELL_W - 36, 44, C_TITLE);
    ui_icon(x + (CELL_W - 44) / 2, y, icon_id(d));
    if (renaming == i) { field_draw(&rename_field, x + 2, y + 43, CELL_W - 4, 1); return; }
    /* The label, on two lines when it is too wide (split at a space). */
    {
        char a[72], b[72];
        int k, lines = 1, wa, wb = 0, top = y + 44, h;
        str_ncopy(a, d->sys ? d->label : d->name, 64);
        b[0] = 0;
        if (ui_measure(a) > CELL_W - 6)
            for (k = str_len(a) - 1; k > 0; k--)
                if (a[k] == ' ') { a[k] = 0; if (ui_measure(a) <= CELL_W - 6) { str_copy(b, a + k + 1); lines = 2; break; } a[k] = ' '; }
        text_fit(a, CELL_W - 6, t);
        str_copy(a, t);
        if (lines == 2) { text_fit(b, CELL_W - 6, t); str_copy(b, t); wb = ui_measure(b); }
        wa = ui_measure(a);
        tw = wa > wb ? wa : wb;
        h = lines * 17 + 1;
        if (d->sel) ui_rect(x + (CELL_W - tw) / 2 - 3, top, tw + 6, h, C_TITLE);
        for (k = 0; k < lines; k++) {
            const char *line = k ? b : a;
            int lw = k ? wb : wa, ly = top + 1 + k * 17;
            tx = x + (CELL_W - lw) / 2;
            if (!d->sel) ui_text(tx + 1, ly + 1, line, C_INK);    /* legible on any wallpaper */
            ui_text(tx, ly, line, C_PAPER);
        }
        if (i == cur && HOST.active) draw_focus(x + (CELL_W - tw) / 2 - 3, top, tw + 6, h);
    }
}
static void paint(void)
{
    int i;
    if (HOST.window == WIN_OVERLAY) {
        if (vol.open) vol_draw();
        else popup_draw(&pop);
        return;
    }
    top_paint();
    for (i = 0; i < nicons; i++) draw_icon(i, 0, 0, 0);
    if (dragging)
        for (i = 0; i < nicons; i++) if (icons[i].sel) draw_icon(i, drag_dx, drag_dy, 1);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
static void dialog_result(int r)
{
    int kind = dlg_kind;
    dlg_kind = D_NONE;
    if (kind == D_DELETE && r == MB_YES) delete_selected();
    if (kind == D_EMPTY && r == MB_YES) {
        bin_empty();
        app_log("[DESK] emptied", "Recycle Bin");
        reload();
        ui_repaint();
    }
}
static void damage_drag(void)
{
    int i;
    for (i = 0; i < nicons; i++)
        if (icons[i].sel) ui_damage(icons[i].x + drag_dx - 2, icons[i].y + drag_dy - 2, CELL_W + 4, CELL_H + 4);
    if (drop_i >= 0) damage_icon(drop_i);
}
static int on_mouse(int kind, int sx, int sy)
{
    int i, r;
    if (dlg.open) return dialog_mouse(&dlg, kind, sx, sy) >= 0;   /* raises it */
    if (HOST.window == WIN_OVERLAY) {
        if (vol.open) return vol_mouse(kind, sx, sy);
        if (!pop.open) { ui_overlay(0); return 1; }
        ui_dirty = 0;
        r = popup_mouse(&pop, kind, sx, sy);
        if (r >= 0) { command(r); return 1; }
        if (r == -2) { menu_close(); return 1; }
        if (ui_dirty) damage_pop();
        return ui_dirty;
    }
    if (kind == MOUSE_HOVER) return 0;
    if (kind == MOUSE_RIGHT) { context_menu(sx, sy, HOST.context); return 1; }
    if (sy < 29 || sy >= area_h - 32) return 0;            /* the bars */
    if (kind == MOUSE_DOWN) {
        i = icon_at(sx, sy);
        if (renaming >= 0) {
            if (i == renaming) return 1;
            commit_rename();
        }
        if (i >= 0 && i == last_click_i && (unsigned)(HOST.ticks - last_click_tick) < (unsigned)HOST.dblclick) {
            last_click_i = -1;
            select_only(i);
            open_icon(i, 0);
            return 1;
        }
        last_click_i = i;
        last_click_tick = HOST.ticks;
        if (i >= 0 && (HOST.shift & SH_CTRL)) { icons[i].sel = !icons[i].sel; cur = i; damage_icon(i); }
        else if (i < 0 || !icons[i].sel) select_only(i);
        else { damage_icon(cur); cur = i; damage_icon(i); }
        if (i >= 0) { drag_i = i; drag_x = sx; drag_y = sy; dragging = 0; drag_dx = drag_dy = 0; }
        return 1;
    }
    if (kind == MOUSE_MOVE && drag_i >= 0) {
        int t;
        if (!dragging && (long)(sx - drag_x) * (sx - drag_x) + (long)(sy - drag_y) * (sy - drag_y) < 36) return 0;
        damage_drag();
        dragging = 1;
        drag_dx = sx - drag_x;
        drag_dy = sy - drag_y;
        t = icon_at(sx, sy);
        drop_i = t >= 0 && !icons[t].sel && ((icons[t].attr & A_DIR) || icons[t].sys == DI_BIN) && file_selected() ? t : -1;
        damage_drag();
        return 1;
    }
    if (kind == MOUSE_UP && drag_i >= 0) {
        int was = dragging, t = drop_i;
        damage_drag();
        drag_i = -1; dragging = 0; drop_i = -1;
        if (!was) return 1;
        if (t >= 0) { drop_on(t); return 1; }
        for (i = 0; i < nicons; i++) if (icons[i].sel) {
            icons[i].x += drag_dx; icons[i].y += drag_dy;
            clamp_icon(i);
            damage_icon(i);
        }
        pos_save();
        app_log("[DESK] moved", icons[cur >= 0 ? cur : 0].name);
        return 1;
    }
    return 0;
}
/* The nearest icon in a direction from the current one. */
static int neighbour(int dx, int dy)
{
    int i, best = -1;
    long bd = 0x7FFFFFFFL;
    if (cur < 0) return nicons ? 0 : -1;
    for (i = 0; i < nicons; i++) {
        long ex = icons[i].x - icons[cur].x, ey = icons[i].y - icons[cur].y, along = ex * dx + ey * dy, across, d;
        if (i == cur || along <= 0) continue;
        across = dx ? ey : ex;
        if (across < 0) across = -across;
        d = along + across * 3;
        if (d < bd) { bd = d; best = i; }
    }
    return best >= 0 ? best : cur;
}
static int on_key(int key)
{
    int s = KEY_SCAN(key), ch = KEY_CHAR(key), shift = HOST.shift, i, letter;
    if (dlg.open) {
        int r = dialog_key(&dlg, key, shift);
        if (r >= 0) dialog_result(r);
        return 1;
    }
    if (vol.open) {
        if ((key >> 8) == 0x4B) vol_set(top_volume - 1);
        else if ((key >> 8) == 0x4D) vol_set(top_volume + 1);
        else vol_close();
        return 1;
    }
    if (HOST.window == WIN_OVERLAY || pop.open) {
        int r;
        ui_dirty = 0;
        r = popup_key(&pop, key);
        if (r >= 0) command(r);
        else if (r == -2) menu_close();
        else damage_pop();
        return 1;
    }
    if (renaming >= 0) {
        if (ch == 13) commit_rename();
        else if (key == K_ESC) { renaming = -1; ui_repaint(); }
        else { field_key(&rename_field, key, shift); damage_icon(renaming); }
        return 1;
    }
    if (key == 0x5D00 || (s == K_SHIFT_F10 && !ch)) {                 /* Menu key */
        if (cur >= 0) context_menu(icons[cur].x + CELL_W / 2, icons[cur].y + 40, 0);
        else context_menu(area_w / 2, area_h / 2, 0);
        return 1;
    }
    letter = (shift & SH_CTRL) ? key_ctrl_letter(key) : 0;
    if (letter) {
        if (letter == 'C' && file_selected()) to_clipboard(0);
        if (letter == 'X' && file_selected()) to_clipboard(1);
        if (letter == 'V') paste_into(DESK_DIR);
        if (letter == 'A') { for (i = 0; i < nicons; i++) icons[i].sel = 1; ui_repaint(); }
        return 1;
    }
    if ((s == 0x1C && (shift & SH_ALT)) || s == 0xA6) { if (cur >= 0) command(M_PROPS); return 1; }
    if (ch == 13) { if (cur >= 0) open_icon(cur, 0); return 1; }
    if (key == K_ESC) { select_only(-1); return 1; }
    if (!ch || ch == 0xE0) {
        switch (s) {
        case K_F2: start_rename(cur); return 1;
        case K_F5: reload(); ui_repaint(); return 1;
        case K_DEL:
            if (!file_selected() && cur >= 0 && !icons[cur].sys) icons[cur].sel = 1;
            if (file_selected()) delete_ask(shift & SH_SHIFT);
            return 1;
        case K_UP: select_only(neighbour(0, -1)); return 1;
        case K_DOWN: select_only(neighbour(0, 1)); return 1;
        case K_LEFT: select_only(neighbour(-1, 0)); return 1;
        case K_RIGHT: select_only(neighbour(1, 0)); return 1;
        case K_HOME: select_only(nicons ? 0 : -1); return 1;
        }
        return 0;
    }
    if (ch > ' ') {                                            /* type to find */
        char c = to_upper((char)ch);
        for (i = 1; i <= nicons; i++) {
            int k = ((cur < 0 ? -1 : cur) + i + nicons) % nicons;
            if (to_upper(icons[k].label[0]) == c) { select_only(k); return 1; }
        }
        return 1;
    }
    return 0;
}

/* Refreshes when files changed (the shell counts changes: no disk access
 * while nothing happens, as DOS programs may be running in their VMs). */
static u16 seen_changes;
static int poll(void)
{
    u16 now;
    if ((unsigned)(HOST.ticks - top_tick) >= 18) {
        int cell = area_w >= 800 ? 100 : 76;
        top_tick = HOST.ticks;
        top_sample();
        ui_damage(area_w - 76 - cell * 5, 0, cell * 5, 29);
        /* Keep this damage precise. Return 1 asks the shell to repaint the
         * desktop owner's entire surface, interrupting every running game. */
        return 3;
    }
    if ((unsigned)(HOST.ticks - poll_tick) < 9 || renaming >= 0 || drag_i >= 0) return 0;
    poll_tick = HOST.ticks;
    now = fs_changes();
    if (now != seen_changes || HOST.screen_w != area_w || HOST.screen_h != area_h) {
        seen_changes = now;
        menu_close();
        reload();
        ui_repaint();
        return 1;
    }
    return 0;
}

int app_event(int ev, int a, int b, int c)
{
    int r, orig = ev;
    if (ev == EV_PAINT_TOPBAR) { top_paint(); return 0; }
    if (ev == EV_TOPBAR) {
        if (a == 1) {
            vol_close();
            if (pop.open) menu_close();
            else menu_open(m_start, 13, 4, 29, "start");
        } else if (a == 2) {
            if (vol.open) vol_close();
            else vol_open();
        }
        return 1;
    }
    if (ev == EV_OPEN) {
        if (a == 2) { dlg.win = 0; pdlg.win = 0; pdlg.open = 0; dialog_sync(&dlg); return 1; }
        str_copy(app_title, "Desktop");
        vol.open = 0; vol.drag = 0;
        seen_changes = fs_changes();
        reload();
        top_seen_fs = fs_changes();
        top_load_ram_total();
        top_find_mixer();
        top_sample();
        return 1;
    }
    if (pdlg.win && HOST.window == pdlg.win) return props_event(ev, a, b, c);
    r = dialog_pre(&dlg, WIN_DESKTOP, &ev, &a);
    if (r >= 0) return r;
    switch (ev) {
    case EV_PAINT:
        if (dialog_mine(&dlg)) dialog_draw(&dlg);
        else paint();
        return 0;
    case EV_MOUSE:
        if (dialog_mine(&dlg)) {
            int sx = HOST.x + b, sy = HOST.y + TITLE_H + c;
            if (a == MOUSE_HOVER) { ui_dirty = 0; dialog_hover(&dlg, sx, sy); r = ui_dirty; }
            else { r = dialog_mouse(&dlg, a, sx, sy); if (r >= 0) dialog_result(r); r = 1; }
        } else r = on_mouse(a, HOST.x + b, HOST.y + TITLE_H + c);
        break;
    case EV_KEY: r = on_key(a); break;
    case EV_POLL: r = poll(); break;
    case EV_SUSPEND: menu_close(); vol_close(); return 0;
    default: r = 0;
    }
    dialog_sync(&dlg);
    return orig == EV_CLOSE && ev != EV_CLOSE ? 0 : r;
}
