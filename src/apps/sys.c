/* System services shared by the desktop modules (Files, the desktop, the
 * Recycle Bin): the file clipboard, the Recycle Bin, moving and deleting
 * folder trees, and the desktop settings file. See app.h. */
#include "app.h"

/* ---------------- folder trees ---------------- */
/* Long names: one shared entry and two path buffers that grow and shrink
 * as the walk goes down and up, so a deep tree costs a few bytes of stack
 * per level (the modules have 4 KB). */
static struct dir_ent te;
static char ts[SYS_PATH], td[SYS_PATH];

static int is_dot(const char *n) { return n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])); }
/* Append \name to p (SYS_PATH): 0, or -3 when it does not fit. */
static int push(char *p, const char *name)
{
    int n = str_len(p);
    if (n + str_len(name) + 2 > SYS_PATH) return -3;
    if (n && p[n - 1] != '\\') p[n++] = '\\';
    str_copy(p + n, name);
    return 0;
}
static void join(char *out, const char *dir, const char *name)
{
    str_ncopy(out, dir, SYS_PATH);
    push(out, name);
}
/* The first entry of dir (not . or ..) into te: 1 found, 0 none. */
static int first_entry(const char *dir)
{
    char pat[SYS_PATH + 4];
    int r;
    join(pat, dir, "*.*");
    for (r = dir_first(pat, A_DIR | A_HIDDEN | A_SYSTEM | A_RDONLY | A_ARCH, &te); r >= 0; r = dir_next(&te)) {
        if (is_dot(te.name) || (te.attr & A_VOLUME)) continue;
        dir_close(&te);
        return 1;
    }
    return 0;
}

u32 sys_bytes;                          /* the bytes the last tree held */
/* Count a folder through nested LFN find handles. The pathname lives in ts
 * so recursion uses only a small stack frame, even for long components. */
static void measure_in(int depth, int *files, int *dirs, u32 *bytes)
{
    int base = str_len(ts), r;
    if (depth >= 8 || push(ts, "*.*") < 0) return;
    r = dir_first(ts, A_DIR | A_HIDDEN | A_SYSTEM | A_RDONLY | A_ARCH, &te);
    ts[base] = 0;
    while (r >= 0) {
        if (!is_dot(te.name) && !(te.attr & A_VOLUME)) {
            if (te.attr & A_DIR) {
                int h = te.h;
                ++*dirs;
                if (push(ts, te.name) == 0) {
                    measure_in(depth + 1, files, dirs, bytes);
                    ts[base] = 0;
                }
                te.h = h;
            } else { ++*files; *bytes += te.size; }
        }
        r = dir_next(&te);
    }
    dir_close(&te);
}
void tree_measure(const char *path, int *files, int *dirs, u32 *bytes)
{
    *files = *dirs = 0;
    *bytes = 0;
    str_ncopy(ts, path, SYS_PATH);
    measure_in(0, files, dirs, bytes);
}
/* ts: a folder whose contents go (each pass takes the first entry again). */
static int delete_in(int depth)
{
    int n = str_len(ts), r, guard = 0;
    while (first_entry(ts) && ++guard < 4000) {
        int attr = te.attr;
        if ((r = push(ts, te.name)) != 0) return r;
        if (attr & A_DIR) {
            r = depth < 32 ? delete_in(depth + 1) : -3;
            if (r >= 0) { dos_set_attr(ts, 0); r = dos_rmdir(ts); }
        } else {
            if (attr & (A_RDONLY | A_HIDDEN | A_SYSTEM)) dos_set_attr(ts, 0);
            r = dos_delete(ts);
        }
        ts[n] = 0;
        if (r < 0) return r;
    }
    return 0;
}
int tree_delete(const char *path)
{
    int attr = dos_get_attr(path), r;
    if (attr < 0) return attr;
    if (!(attr & A_DIR)) { dos_set_attr(path, 0); return dos_delete(path); }
    str_ncopy(ts, path, SYS_PATH);
    r = delete_in(0);
    if (r < 0) return r;
    dos_set_attr(path, 0);
    return dos_rmdir(path);
}

/* The size of a file (sys_bytes). */
static void count_file(const char *path)
{
    if (dir_first(path, A_HIDDEN | A_SYSTEM | A_RDONLY | A_ARCH, &te) >= 0) { sys_bytes += te.size; dir_close(&te); }
}
/* ts -> td for the contents of a folder that could not be renamed there. */
static int move_in(int depth)
{
    int ns = str_len(ts), nd = str_len(td), r, guard = 0;
    while (first_entry(ts) && ++guard < 4000) {
        int attr = te.attr;
        if ((r = push(ts, te.name)) != 0 || (r = push(td, te.name)) != 0) return r;
        if (!(attr & A_DIR)) { count_file(ts); r = dos_rename(ts, td); }
        else if (dos_rename(ts, td) >= 0) r = 0;
        else {
            r = dos_mkdir(td);
            if (r < 0 && dos_get_attr(td) < 0) return r;
            r = depth < 32 ? move_in(depth + 1) : -3;
            if (r >= 0) {
                dos_set_attr(td, attr & (A_HIDDEN | A_RDONLY | A_ARCH));
                dos_set_attr(ts, 0);
                r = dos_rmdir(ts);
            }
        }
        ts[ns] = td[nd] = 0;
        if (r < 0) return r;
    }
    return 0;
}
/* Files and folders move with a rename (with long names a folder moves to
 * another folder too); else the folder is made again at dst, its contents
 * moved one by one, and removed. */
int tree_move(const char *src, const char *dst)
{
    int attr, r;
    attr = dos_get_attr(src);
    if (attr < 0) return attr;
    if (!(attr & A_DIR)) { count_file(src); return dos_rename(src, dst); }
    if (dos_rename(src, dst) >= 0) return 0;
    r = dos_mkdir(dst);
    if (r < 0 && dos_get_attr(dst) < 0) return r;
    str_ncopy(ts, src, SYS_PATH);
    str_ncopy(td, dst, SYS_PATH);
    r = move_in(0);
    if (r < 0) return r;
    dos_set_attr(dst, attr & (A_HIDDEN | A_RDONLY | A_ARCH));
    dos_set_attr(src, 0);
    return dos_rmdir(src);
}

/* One file ts -> td (date and attributes kept). */
static int copy_file(char *buf, int n, u16 time, u16 date, int attr)
{
    int r, hs, hd;
    hs = dos_open(ts, 0);
    if (hs < 0) return hs;
    hd = dos_create(td);
    if (hd < 0) { dos_close(hs); return hd; }
    for (;;) {
        r = dos_read(hs, buf, n);
        if (r <= 0) break;
        if (dos_write(hd, buf, r) != r) { r = -29; break; }
        sys_bytes += r;
    }
    if (r == 0) dos_set_file_time(hd, time, date);
    dos_close(hs);
    dos_close(hd);
    if (r < 0) { dos_delete(td); return r; }
    dos_set_attr(td, attr & (A_RDONLY | A_HIDDEN | A_SYSTEM | A_ARCH));
    return 0;
}
/* The contents of folder ts into td: a search per level (they nest with
 * long names; the 8.3 kernel keeps one search, the old limit). */
static int copy_in(char *buf, int n, int depth)
{
    char pat[SYS_PATH + 4];
    int ns = str_len(ts), nd = str_len(td), r, h;
    join(pat, ts, "*.*");
    for (r = dir_first(pat, A_DIR | A_HIDDEN | A_SYSTEM | A_RDONLY | A_ARCH, &te); r >= 0; r = dir_next(&te)) {
        int attr = te.attr;
        u16 time = te.time, date = te.date;
        if (is_dot(te.name) || (attr & A_VOLUME)) continue;
        if ((r = push(ts, te.name)) != 0 || (r = push(td, te.name)) != 0) break;
        h = te.h;
        if (attr & A_DIR) {
            r = dos_mkdir(td);
            if (r < 0 && dos_get_attr(td) >= 0) r = 0;
            if (r >= 0) r = depth < 32 ? copy_in(buf, n, depth + 1) : -3;
        } else r = copy_file(buf, n, time, date, attr);
        te.h = h;
        ts[ns] = td[nd] = 0;
        if (r < 0) break;
    }
    dir_close(&te);
    return r == -18 || r == -2 ? 0 : r;
}
/* A copy of a file or folder (dates and attributes kept); buf is a work
 * buffer of n bytes. */
int tree_copy(const char *src, const char *dst, char *buf, int n)
{
    int attr, r;
    attr = dos_get_attr(src);
    if (attr < 0) return attr;
    str_ncopy(ts, src, SYS_PATH);
    str_ncopy(td, dst, SYS_PATH);
    if (attr & A_DIR) {
        r = dos_mkdir(dst);
        if (r < 0 && dos_get_attr(dst) < 0) return r;
        return copy_in(buf, n, 0);
    }
    if (dir_first(src, A_HIDDEN | A_SYSTEM | A_RDONLY | A_ARCH, &te) < 0) return -2;
    dir_close(&te);
    return copy_file(buf, n, te.time, te.date, te.attr);
}

/* Make every missing folder of path (C:\A\B\C). */
int make_dirs(const char *path)
{
    char p[SYS_PATH];
    int i, n = str_len(path);
    if (n >= SYS_PATH) return -3;
    for (i = 3; i <= n; i++) {
        if (path[i] != '\\' && path[i]) continue;
        str_ncopy(p, path, i + 1);
        if (dos_get_attr(p) < 0) {
            int r = dos_mkdir(p);
            if (r < 0) return r;
        }
    }
    return 0;
}

/* ---------------- file clipboard ---------------- */
static const char clip_path[] = "\\SYSTEM\\UI\\CLIPBRD.DAT";
#pragma pack(push, 1)
struct clip_head { char magic[4]; u8 cut, count, pad[2]; };   /* CCB2: SYS_PATH paths */
struct clip_rec { u8 media; char path[SYS_PATH]; };
#pragma pack(pop)

int clip_count(int *cut)
{
    struct clip_head h;
    int f = dos_open(clip_path, 0), n = 0;
    if (cut) *cut = 0;
    if (f < 0) return 0;
    if (dos_read(f, &h, sizeof h) == sizeof h && !str_nicmp(h.magic, "CCB2", 4)) {
        n = h.count;
        if (cut) *cut = h.cut;
    }
    dos_close(f);
    return n;
}
int clip_get(int i, char *path)
{
    struct clip_rec r;
    int f = dos_open(clip_path, 0), m = -1;
    path[0] = 0;
    if (f < 0) return -1;
    dos_seek(f, (long)sizeof(struct clip_head) + (long)i * sizeof r, 0);
    if (dos_read(f, &r, sizeof r) == sizeof r) {
        str_ncopy(path, r.path, SYS_PATH);
        m = r.media;
    }
    dos_close(f);
    return m;
}
static int clip_h = -1, clip_n;
static u8 clip_cut_flag;
void clip_begin(int cut)
{
    struct clip_head h;
    clip_h = dos_create(clip_path);
    clip_n = 0;
    clip_cut_flag = (u8)cut;
    if (clip_h < 0) return;
    mem_set(&h, 0, sizeof h);
    dos_write(clip_h, &h, sizeof h);
}
void clip_add(const char *path, int media)
{
    struct clip_rec r;
    if (clip_h < 0 || clip_n >= 250) return;
    mem_set(&r, 0, sizeof r);
    r.media = (u8)media;
    str_ncopy(r.path, path, SYS_PATH);
    dos_write(clip_h, &r, sizeof r);
    clip_n++;
}
void clip_end(void)
{
    struct clip_head h;
    if (clip_h < 0) return;
    mem_copy(h.magic, "CCB2", 4);
    h.cut = clip_cut_flag;
    h.count = (u8)clip_n;
    h.pad[0] = h.pad[1] = 0;
    dos_seek(clip_h, 0, 0);
    dos_write(clip_h, &h, sizeof h);
    dos_close(clip_h);
    clip_h = -1;
}
void clip_clear(void) { clip_begin(0); clip_end(); }

/* ---------------- Recycle Bin ---------------- */
/* X:\RECYCLED keeps the deleted items as DXn.ext (X the drive, n a number)
 * and INFO2.DAT, a record for each: where it was (a long path), when it was
 * deleted. An INFO.DAT of 128-byte records (80-character paths, before long
 * names) is read as it is and moved to INFO2.DAT at the first change. */
static char bin_dir[] = "C:\\RECYCLED";
static char bin_info[] = "C:\\RECYCLED\\INFO2.DAT";
static char bin_old[] = "C:\\RECYCLED\\INFO.DAT";
#pragma pack(push, 1)
struct binrec1 {
    char orig[80];
    char stored[13];
    u8 attr;
    u16 date, time;
    u32 size;
    u16 ddate, dtime;
    u8 used;
    u8 pad[21];
};
#pragma pack(pop)

static void bin_drive(const char *path)
{
    char d = path[0] && path[1] == ':' ? to_upper(path[0]) : (char)('A' + dos_get_drive());
    bin_dir[0] = bin_info[0] = bin_old[0] = d;
}
static int rec_read(const char *file, int i, void *r, int size)
{
    int f = dos_open(file, 0), n;
    if (f < 0) return -1;
    dos_seek(f, (long)i * size, 0);
    n = dos_read(f, r, size);
    dos_close(f);
    return n == size;
}
static int rec_count(const char *file, int size)
{
    int f = dos_open(file, 0);
    long n;
    if (f < 0) return -1;
    n = dos_seek(f, 0, 2);
    dos_close(f);
    return n > 0 ? (int)(n / size) : 0;
}
static void rec_upgrade(const struct binrec1 *o, struct binrec *r)
{
    mem_set(r, 0, sizeof *r);
    str_ncopy(r->orig, o->orig, sizeof o->orig);
    mem_copy(r->stored, o->stored, 13);
    r->attr = o->attr; r->date = o->date; r->time = o->time; r->size = o->size;
    r->ddate = o->ddate; r->dtime = o->dtime; r->used = o->used;
}
int bin_get(int i, struct binrec *r)
{
    struct binrec1 o;
    int k = rec_read(bin_info, i, r, sizeof *r);
    if (k >= 0) return k;
    if (rec_read(bin_old, i, &o, sizeof o) <= 0) return 0;
    rec_upgrade(&o, r);
    return 1;
}
int bin_count(void)
{
    int n = rec_count(bin_info, sizeof(struct binrec));
    if (n >= 0) return n;
    n = rec_count(bin_old, sizeof(struct binrec1));
    return n > 0 ? n : 0;
}
/* INFO.DAT's records to INFO2.DAT (once, before a change). */
static void bin_upgrade(void)
{
    struct binrec r;
    int i, n, f;
    if (dos_get_attr(bin_info) >= 0 || dos_get_attr(bin_old) < 0) return;
    n = bin_count();
    f = dos_create(bin_info);
    if (f < 0) return;
    for (i = 0; i < n; i++) { if (!bin_get(i, &r)) mem_set(&r, 0, sizeof r); dos_write(f, &r, sizeof r); }
    dos_close(f);
    dos_set_attr(bin_old, 0);
    dos_delete(bin_old);
}
static int bin_put(int i, struct binrec *r)
{
    int f, n;
    bin_upgrade();
    f = dos_open(bin_info, 2);
    if (f < 0) f = dos_create(bin_info);
    if (f < 0) return f;
    dos_seek(f, (long)i * sizeof *r, 0);
    n = dos_write(f, r, sizeof *r);
    dos_close(f);
    return n == sizeof *r ? 0 : -29;
}
/* The stored item's full path. */
void bin_stored(const struct binrec *r, char *out)
{
    join(out, bin_dir, r->stored);
}
/* 1 when record i is a deleted item still in the bin. */
int bin_item(int i, struct binrec *r)
{
    char p[SYS_PATH];
    if (!bin_get(i, r) || !r->used) return 0;
    bin_stored(r, p);
    return dos_get_attr(p) >= 0;
}
int bin_items(u32 *bytes)
{
    struct binrec r;
    int i, n = 0, total = bin_count();
    if (bytes) *bytes = 0;
    for (i = 0; i < total; i++) if (bin_item(i, &r)) { n++; if (bytes) *bytes += r.size; }
    return n;
}

int bin_send(const char *path)
{
    struct binrec r;
    char dst[SYS_PATH], n[8];
    const char *base = path, *e = 0, *p;
    int i, attr, total, slot = -1, num = 1, y, mo, d, wd, h, mi, s, err;
    bin_drive(path);
    attr = dos_get_attr(path);
    if (attr < 0) return attr;
    for (p = path; *p; p++) if (*p == '\\') base = p + 1;
    for (p = base; *p; p++) if (*p == '.') e = p;
    /* Deleting the bin itself (or a folder holding it) is for good. */
    if (!str_nicmp(path, bin_dir, str_len(path)) && (bin_dir[str_len(path)] == '\\' || !bin_dir[str_len(path)]))
        return tree_delete(path);
    if (dos_get_attr(bin_dir) < 0) {
        err = dos_mkdir(bin_dir);
        if (err < 0) return err;
        dos_set_attr(bin_dir, A_HIDDEN | A_SYSTEM);
    }
    total = bin_count();
    for (i = 0; i < total; i++) {
        if (!bin_get(i, &r)) break;
        if (!r.used) { if (slot < 0) slot = i; continue; }
    }
    if (slot < 0) slot = total;
    /* A free name: DC1.TXT, DC2.TXT... */
    for (;;) {
        str_copy(r.stored, "D");
        r.stored[1] = bin_dir[0]; r.stored[2] = 0;
        fmt_u32(n, (u32)num);
        str_cat(r.stored, n);
        if (e && !(attr & A_DIR)) {
            char *x = r.stored + str_len(r.stored);
            str_ncopy(x, e, 5);
            while (*x) { *x = to_upper(*x); x++; }
        }
        join(dst, bin_dir, r.stored);
        if (dos_get_attr(dst) < 0) break;
        if (++num > 9999) return -5;
    }
    mem_set(&r.orig, 0, sizeof r.orig);
    str_ncopy(r.orig, path, sizeof r.orig);
    r.attr = (u8)attr;
    r.date = r.time = 0;
    if (!(attr & A_DIR) && dir_first(path, A_HIDDEN | A_SYSTEM | A_RDONLY | A_ARCH, &te) >= 0) {
        dir_close(&te);
        r.date = te.date; r.time = te.time;
    }
    dos_get_date(&y, &mo, &d, &wd);
    dos_get_time(&h, &mi, &s);
    r.ddate = (u16)(((y - 1980) << 9) | (mo << 5) | d);
    r.dtime = (u16)((h << 11) | (mi << 5) | (s / 2));
    r.used = 1;
    mem_set(r.pad, 0, sizeof r.pad);
    sys_bytes = 0;
    err = tree_move(path, dst);
    r.size = sys_bytes;
    if (err < 0) return err;
    bin_put(slot, &r);
    return 0;
}

/* Back to where it was (its folders made again): 0, -80 when an item of
 * that name is there. */
int bin_restore(int i)
{
    struct binrec r;
    char src[SYS_PATH], dir[SYS_PATH];
    int n, err;
    if (!bin_item(i, &r)) return -2;
    if (dos_get_attr(r.orig) >= 0) return -80;
    str_copy(dir, r.orig);
    n = str_len(dir);
    while (n > 3 && dir[n - 1] != '\\') n--;
    dir[n > 3 ? n - 1 : n] = 0;
    err = make_dirs(dir);
    if (err < 0) return err;
    bin_stored(&r, src);
    sys_bytes = 0;
    err = tree_move(src, r.orig);
    if (err < 0) return err;
    dos_set_attr(r.orig, r.attr & (A_RDONLY | A_HIDDEN | A_SYSTEM | A_ARCH));
    r.used = 0;
    return bin_put(i, &r);
}
int bin_purge(int i)
{
    struct binrec r;
    char p[SYS_PATH];
    int err;
    if (!bin_get(i, &r) || !r.used) return 0;
    bin_stored(&r, p);
    err = dos_get_attr(p) >= 0 ? tree_delete(p) : 0;
    if (err < 0) return err;
    r.used = 0;
    return bin_put(i, &r);
}
int bin_empty(void)
{
    char p[SYS_PATH];
    int err = 0, guard = 0, f;
    bin_drive("C:");
    for (;;) {
        char pat[SYS_PATH];
        int r, found = 0;
        join(pat, bin_dir, "*.*");
        for (r = dir_first(pat, A_DIR | A_HIDDEN | A_SYSTEM | A_RDONLY | A_ARCH, &te); r >= 0; r = dir_next(&te)) {
            if (is_dot(te.name) || (te.attr & A_VOLUME) || !str_icmp(te.name, "INFO.DAT") ||
                !str_icmp(te.name, "INFO2.DAT")) continue;
            dir_close(&te);
            found = 1;
            break;
        }
        if (!found || ++guard > 4000) break;
        join(p, bin_dir, te.name);
        err = tree_delete(p);
        if (err < 0) break;
    }
    f = dos_create(bin_info);
    if (f >= 0) dos_close(f);
    if (dos_get_attr(bin_old) >= 0) { dos_set_attr(bin_old, 0); dos_delete(bin_old); }
    return err;
}

/* ---------------- desktop settings ---------------- */
static const char cfg_path[] = "\\SYSTEM\\UI\\DESKTOP.CFG";
/* The colours the desktop starts with (shell_gui.inc ui_palette). */
const u8 cfg_default_palette[48] = {
    9, 10, 12,  13, 18, 30,  18, 32, 23,  13, 23, 24,
    37, 15, 16,  31, 24, 37,  37, 29, 15,  51, 52, 53,
    28, 30, 34,  27, 36, 48,  24, 41, 38,  39, 49, 48,
    49, 25, 23,  43, 34, 46,  57, 47, 27,  61, 61, 60 };
void cfg_defaults(struct deskcfg *c)
{
    mem_set(c, 0, sizeof *c);
    mem_copy(c->magic, "CUI1", 4);
    mem_copy(c->palette, cfg_default_palette, 48);
    c->mouse_speed = 2;
    c->mouse_swap = 0;
    c->dblclick = 9;
    c->kbd_rate = 0;
    c->kbd_delay = 1;
    c->font[0] = 0;
    c->icons_hidden = 0;
    c->cursor_scheme = 0;
    c->reserved = 0;
}
int cfg_load(struct deskcfg *c)
{
    int f = dos_open(cfg_path, 0), n = 0;
    cfg_defaults(c);
    if (f < 0) return 0;
    n = dos_read(f, c, sizeof *c);
    dos_close(f);
    if (n < 72 || str_nicmp(c->magic, "CUI1", 4)) { cfg_defaults(c); return 0; }
    return 1;
}
int cfg_save(struct deskcfg *c)
{
    int f = dos_create(cfg_path), n;
    if (f < 0) return f;
    n = dos_write(f, c, sizeof *c);
    dos_close(f);
    app_settings();
    return n == sizeof *c ? 0 : -29;
}
