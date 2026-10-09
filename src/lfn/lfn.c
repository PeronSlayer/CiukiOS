/* LFN.COM, resident part: the Windows 95 long file name API (INT 21h
 * AX=71xxh) on the CiukiDOS FAT16 volume, in the VFAT on-disk format.
 * See lfn_start.asm and docs/design-long-file-names-2026-09-30.md.
 *
 * Design in short:
 *  - Paths are resolved here (long or short names in every component) to a
 *    short path the kernel understands. Everything that allocates or frees
 *    clusters or opens files is done by the kernel's own 8.3 calls on that
 *    short path; this code only reads directories and writes directory
 *    entries (long name entries, moved short entries), plus the FAT when a
 *    directory needs another cluster.
 *  - Nothing is cached across calls. The kernel's one-sector FAT cache is
 *    written back before the FAT is read here, and dropped after the FAT is
 *    written here. The kernel's InDOS byte is raised for the whole call, so
 *    the VM manager never switches VMs in the middle of one.
 */
typedef unsigned char u8;
typedef unsigned int u16;
typedef unsigned long u32;
#define FP(s, o) ((u8 __far *)(((u32)(s) << 16) | (u16)(o)))
#define NONE 0xFFFFu
#define MAXP 261

struct regs { u16 ax, bx, cx, dx, si, di, ds, es, flags, bp; };
struct kio { u32 lba; u16 off, seg, write; };
extern struct regs R;
int kdos(struct regs *r);
int kio(struct kio *q);
int kflush(void);
int kinvalidate(void);
int kenter(void);
int kleave(void);
u32 mul16(u16 a, u16 b);
#pragma aux mul16 parm [ax] [dx] value [dx ax] modify exact [ax dx];
void com1(const char *s);
static u16 my_ds(void);
#pragma aux my_ds = "mov ax,ds" value [ax];

/* The volume (lfn_init.c). */
u16 g_spc, g_shift, g_fat, g_fatsz, g_nfats, g_rootn, g_maxcl;
u32 g_root, g_data, g_serial;
u8 dbuf[512];                       /* one directory sector */

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scpy(char *d, const char *s) { while ((*d++ = *s++) != 0) ; }
static void mcpy(void *d, const void *s, int n) { u8 *a = d; const u8 *b = s; while (n-- > 0) *a++ = *b++; }
static void mset(void *d, int v, int n) { u8 *a = d; while (n-- > 0) *a++ = (u8)v; }
static int mcmp(const void *d, const void *s, int n)
{
    const u8 *a = d, *b = s;
    while (n-- > 0) if (*a++ != *b++) return 1;
    return 0;
}
static char up(char c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int ieq(const char *a, const char *b)
{
    while (*a && up(*a) == up(*b)) { a++; b++; }
    return !*a && !*b;
}
static u16 w16(const u8 *p) { return p[0] | ((u16)p[1] << 8); }
static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void fput16(u8 __far *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void fput32(u8 __far *p, u32 v) { fput16(p, (u16)v); fput16(p + 2, (u16)(v >> 16)); }
static int far_str(char *d, u16 seg, u16 off, int max)     /* 0, or too long */
{
    u8 __far *s = FP(seg, off);
    int i;
    for (i = 0; i < max; i++) if ((d[i] = s[i]) == 0) return 0;
    d[max - 1] = 0;
    return 1;
}
static void far_put(u16 seg, u16 off, const char *s)
{
    u8 __far *d = FP(seg, off);
    while ((*d++ = *s++) != 0) ;
}

/* ------------------------------------------------------------------ */
/* CP437 <-> UTF-16                                                    */
static const u16 cp437[128] = {
    0xC7,0xFC,0xE9,0xE2,0xE4,0xE0,0xE5,0xE7,0xEA,0xEB,0xE8,0xEF,0xEE,0xEC,0xC4,0xC5,
    0xC9,0xE6,0xC6,0xF4,0xF6,0xF2,0xFB,0xF9,0xFF,0xD6,0xDC,0xA2,0xA3,0xA5,0x20A7,0x192,
    0xE1,0xED,0xF3,0xFA,0xF1,0xD1,0xAA,0xBA,0xBF,0x2310,0xAC,0xBD,0xBC,0xA1,0xAB,0xBB,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    0x3B1,0xDF,0x393,0x3C0,0x3A3,0x3C3,0xB5,0x3C4,0x3A6,0x398,0x3A9,0x3B4,0x221E,0x3C6,0x3B5,0x2229,
    0x2261,0xB1,0x2265,0x2264,0x2320,0x2321,0xF7,0x2248,0xB0,0x2219,0xB7,0x221A,0x207F,0xB2,0x25A0,0xA0 };
static u16 to_uni(u8 c) { return c < 0x80 ? c : cp437[c - 0x80]; }
static u8 lossy;                    /* a character had no CP437 form */
static char from_uni(u16 u)
{
    int i;
    if (u < 0x80) return (char)u;
    for (i = 0; i < 128; i++) if (cp437[i] == u) return (char)(0x80 + i);
    lossy = 1;
    return '_';
}

/* ------------------------------------------------------------------ */
/* Sectors, the FAT and the kernel's FAT cache                         */
static struct kio q;
static int sec_io(u32 lba, void *buf, int write)
{
    q.lba = lba; q.off = (u16)buf; q.seg = my_ds(); q.write = write;
    return kio(&q) ? 5 : 0;
}
static u32 dlba = 0;                /* dbuf's sector, 0 none */
static u8 ddirty;
static int dflush(void)
{
    if (ddirty) { ddirty = 0; return sec_io(dlba, dbuf, 1); }
    return 0;
}
static int dload(u32 lba)
{
    int e;
    if (dlba == lba) return 0;
    if ((e = dflush()) != 0) return e;
    dlba = 0;
    if ((e = sec_io(lba, dbuf, 0)) != 0) return e;
    dlba = lba;
    return 0;
}
static u8 fbuf[512];
static u16 fsec = NONE;
static u8 fdirty;
/* The kernel's dirty FAT sector, written back before the FAT is read. */
static int ksync(void)
{
    return kflush() ? 5 : 0;
}
static int fflush(void)
{
    u16 i;
    if (!fdirty) return 0;
    fdirty = 0;
    for (i = 0; i < g_nfats; i++)
        if (sec_io((u32)g_fat + (u32)i * g_fatsz + fsec, fbuf, 1)) return 5;
    /* The kernel re-reads what it had cached. */
    return kinvalidate() ? 5 : 0;
}
static int ioerr;
static u16 fat_get(u16 c)
{
    u16 s = c >> 8;
    if (fsec != s) {
        if (fflush() || ksync() || sec_io((u32)g_fat + s, fbuf, 0)) { ioerr = 5; fsec = NONE; return 0xFFF7; }
        fsec = s;
    }
    return w16(fbuf + ((c & 255) << 1));
}
static void fat_set(u16 c, u16 v)
{
    fat_get(c);
    if (fsec != (c >> 8)) return;
    put16(fbuf + ((c & 255) << 1), v);
    fdirty = 1;
}
static u32 clus_lba(u16 c) { return g_data + ((u32)(c - 2) << g_shift); }

/* The kernel's INT 21h: our buffers written first and forgotten after. */
static u16 cc_dir = NONE, cc_n, cc_cl;
static void forget(void) { dlba = 0; fsec = NONE; cc_dir = NONE; }
static struct regs K;
static int kcall(void)
{
    dflush();
    fflush();
    forget();
    return kdos(&K);
}
static void kclear(void) { mset(&K, 0, sizeof K); K.ds = K.es = my_ds(); }

/* ------------------------------------------------------------------ */
/* Directory slots: dir is its first cluster, 0 the root.              */
static u32 slot_lba(u16 dir, u16 i)
{
    u16 n;
    if (!dir) return i < g_rootn ? g_root + (i >> 4) : 0;
    n = (i >> 4) >> g_shift;
    if (cc_dir != dir || n < cc_n) { cc_dir = dir; cc_n = 0; cc_cl = dir; }
    while (cc_n < n) {
        u16 x = fat_get(cc_cl);
        if (x < 2 || x > g_maxcl) return 0;
        cc_cl = x; cc_n++;
    }
    if (cc_cl < 2 || cc_cl > g_maxcl) return 0;
    return clus_lba(cc_cl) + ((i >> 4) & (g_spc - 1));
}
/* Slot i of dir in dbuf, 0 past the end of the directory. */
static u8 *slot(u16 dir, u16 i)
{
    u32 l = slot_lba(dir, i);
    if (!l) return 0;
    if (dload(l)) { ioerr = 5; return 0; }
    return dbuf + ((i & 15) << 5);
}

static u8 lfn_sum(const u8 *n)
{
    u8 s = 0;
    int i;
    for (i = 0; i < 11; i++) s = (u8)(((s & 1) << 7) + (s >> 1) + n[i]);
    return s;
}
static const u8 lfn_off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

/* "NAME.EXT" from an entry; lower: the NT lower-case flags apply. */
static void short_name(const u8 *e, char *o, int lower)
{
    int i, n = 0;
    for (i = 0; i < 8 && e[i] != ' '; i++) {
        char c = (char)(i == 0 && e[0] == 5 ? 0xE5 : e[i]);
        o[n++] = (lower && (e[12] & 8) && c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    if (e[8] != ' ') {
        o[n++] = '.';
        for (i = 8; i < 11 && e[i] != ' '; i++)
            o[n++] = (lower && (e[12] & 16) && e[i] >= 'A' && e[i] <= 'Z') ? e[i] + 32 : e[i];
    }
    o[n] = 0;
}

struct dent {
    u16 idx;                        /* the short entry's slot */
    u16 first;                      /* its first long name slot (idx: none) */
    u8 e[32];
    u8 lossy;
    char sname[13];                 /* "NAME.EXT" */
    char lname[MAXP];               /* long name, else the short name */
};
/* The next entry of dir from slot *pi (volume labels skipped): 1 found. */
static int dir_next(u16 dir, u16 *pi, struct dent *d)
{
    u16 i = *pi, first = 0;
    u8 want = 0, sum = 0, done = 0;
    for (;; i++) {
        u8 *p = slot(dir, i);
        if (!p || !p[0]) { *pi = i; return 0; }
        if (p[0] == 0xE5) { want = done = 0; continue; }
        if (p[11] == 0x0F) {
            u8 seq = p[0] & 0x1F;
            int k;
            if (p[0] & 0x40) {
                if (!seq || seq > 20) { want = done = 0; continue; }
                want = seq; sum = p[13]; first = i; done = 0; lossy = 0;
                d->lname[seq * 13] = 0;
            } else if (!want || seq != want || p[13] != sum) { want = done = 0; continue; }
            for (k = 0; k < 13; k++) {
                u16 u = w16(p + lfn_off[k]), pos = (seq - 1) * 13 + k;
                if (u == 0) { d->lname[pos] = 0; break; }
                if (u != 0xFFFF) d->lname[pos] = from_uni(u);
            }
            if (--want == 0) done = 1;
            continue;
        }
        if (p[11] & 0x08) { want = done = 0; continue; }
        mcpy(d->e, p, 32);
        d->idx = i;
        short_name(p, d->sname, 0);
        if (done && lfn_sum(p) == sum && d->lname[0]) { d->first = first; d->lossy = lossy; }
        else { d->first = i; d->lossy = 0; short_name(p, d->lname, 1); }
        *pi = i + 1;
        return 1;
    }
}
/* By name (long or short; short only when shortonly): 1 found. */
static int dir_find(u16 dir, const char *name, struct dent *d, int shortonly)
{
    u16 i = 0;
    while (dir_next(dir, &i, d))
        if (ieq(name, d->sname) || (!shortonly && ieq(name, d->lname))) return 1;
    return 0;
}
static int short_exists(u16 dir, const u8 *n11, u16 skip)
{
    u16 i;
    for (i = 0;; i++) {
        u8 *p = slot(dir, i);
        if (!p || !p[0]) return 0;
        if (p[0] == 0xE5 || p[11] == 0x0F || (p[11] & 8) || i == skip) continue;
        if (!mcmp(p, n11, 11)) return 1;
    }
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
static int has_wild(const char *s) { for (; *s; s++) if (*s == '*' || *s == '?') return 1; return 0; }
static int valid_name(const char *s)
{
    int n = 0;
    if (!s[0] || (s[0] == '.' && (!s[1] || (s[1] == '.' && !s[2])))) return 0;
    for (; *s; s++, n++) {
        u8 c = (u8)*s;
        if (c < 32 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') return 0;
    }
    return n <= 255;
}
/* The 8.3 basis of a long name (Windows 95): upper case, no spaces or
 * leading dots, the last dot starts the extension; +,;=[] become _.
 * Returns 1 when that was lossy (a numeric tail is needed). */
static int alias_basis(const char *s, u8 *o, int *blen)
{
    const char *dot = 0, *p;
    int n = 0, lossy_ = 0;
    mset(o, ' ', 11);
    while (*s == '.') { s++; lossy_ = 1; }
    for (p = s; *p; p++) if (*p == '.') dot = p;
    for (p = s; *p && p != dot; p++) {
        char c = up(*p);
        if (c == ' ' || c == '.') { lossy_ = 1; continue; }
        if (c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']') { c = '_'; lossy_ = 1; }
        if (n < 8) o[n++] = (u8)c; else lossy_ = 1;
    }
    *blen = n;
    if (dot) {
        int k = 0;
        for (p = dot + 1; *p; p++) {
            char c = up(*p);
            if (c == ' ') { lossy_ = 1; continue; }
            if (c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']') { c = '_'; lossy_ = 1; }
            if (k < 3) o[8 + k++] = (u8)c; else lossy_ = 1;
        }
    }
    if (!n) { o[0] = '_'; *blen = 1; lossy_ = 1; }
    if (o[0] == 0xE5) o[0] = 5;
    return lossy_;
}
/* The short name for long name s in dir (skip: the entry being renamed).
 * Returns the number of long name entries it needs (0: none). */
static int make_alias(u16 dir, const char *s, u8 *o, u16 skip)
{
    int blen, lossy_ = alias_basis(s, o, &blen);
    u32 n;
    char f[13];
    if (!lossy_ && !short_exists(dir, o, skip)) {
        short_name(o, f, 0);
        return mcmp(f, s, slen(s) + 1) ? (slen(s) + 12) / 13 : 0;
    }
    for (n = 1; n < 1000000L; n++) {
        char t[8];
        int tl = 0, k;
        u32 v = n;
        char r[7];
        int rl = 0;
        while (v) { r[rl++] = (char)('0' + (int)(v % 10)); v /= 10; }
        t[tl++] = '~';
        while (rl) t[tl++] = r[--rl];
        k = blen < 8 - tl ? blen : 8 - tl;
        mset(o + k, ' ', 8 - k);
        mcpy(o + k, t, tl);
        if (!short_exists(dir, o, skip)) break;
        if (ioerr) break;
    }
    return (slen(s) + 12) / 13;
}

/* ------------------------------------------------------------------ */
/* Writing entries                                                     */
static u16 end_at;                  /* first 00h slot seen by dir_room */
static int dir_grow(u16 dir)
{
    u16 last = dir, c, x, guard = 0, k;
    int e;
    for (;;) {
        x = fat_get(last);
        if (ioerr) return 5;
        if (x >= 0xFFF8) break;
        if (x < 2 || x > g_maxcl || ++guard > 4096) return 5;
        last = x;
    }
    for (c = 2; c <= g_maxcl; c++) {
        if (fat_get(c) == 0) break;
        if (ioerr) return 5;
    }
    if (c > g_maxcl) return 112;
    if ((e = dflush()) != 0) return e;
    dlba = 0;
    mset(dbuf, 0, 512);
    for (k = 0; k < g_spc; k++) if ((e = sec_io(clus_lba(c) + k, dbuf, 1)) != 0) return e;
    fat_set(c, 0xFFFF);
    fat_set(last, c);
    return fflush();
}
/* The first run of n free slots of dir (slots lo..hi count as free, the
 * entry being moved); a subdirectory grows when it has none. */
static int dir_room(u16 dir, int n, u16 lo, u16 hi, u16 *run)
{
    u16 i, start = 0, len = 0;
    int end = 0, e;
    end_at = NONE;
    for (i = 0;; i++) {
        u8 *p = slot(dir, i);
        if (ioerr) return 5;
        if (!p) {
            if (!dir) return 5;
            if ((e = dir_grow(dir)) != 0) return e;
            if (end_at == NONE) end_at = i;
            end = 1;
            p = slot(dir, i);
            if (!p) return 5;
        }
        if (!end && !p[0]) { end = 1; end_at = i; }
        if (end || p[0] == 0xE5 || (i >= lo && i <= hi)) {
            if (!len) start = i;
            if (++len == n) { *run = start; return 0; }
        } else len = 0;
        if (i == 0xFFFE) return 5;
    }
}
/* n long name entries for name, then short entry e, from slot run. */
static int dir_write(u16 dir, u16 run, const char *name, int n, const u8 *e)
{
    int k, j, len = slen(name);
    u8 sum = lfn_sum(e), *p;
    for (k = 0; k < n; k++) {
        int seq = n - k;
        if (!(p = slot(dir, run + k))) return 5;
        p[0] = (u8)(seq | (k ? 0 : 0x40));
        p[11] = 0x0F; p[12] = 0; p[13] = sum; p[26] = p[27] = 0;
        for (j = 0; j < 13; j++) {
            int pos = (seq - 1) * 13 + j;
            put16(p + lfn_off[j], pos < len ? to_uni((u8)name[pos]) : pos == len ? 0 : 0xFFFF);
        }
        ddirty = 1;
    }
    if (!(p = slot(dir, run + n))) return 5;
    mcpy(p, e, 32);
    ddirty = 1;
    /* Written past the old end: the slot after keeps the end mark. */
    if (end_at != NONE && run + n >= end_at && (p = slot(dir, run + n + 1)) != 0 && p[0]) {
        p[0] = 0;
        ddirty = 1;
    }
    return 0;
}
/* Delete slots a..b of dir (b included) that are not in keep_a..keep_b. */
static void dir_drop(u16 dir, u16 a, u16 b, u16 keep_a, u16 keep_b)
{
    u16 k;
    for (k = a; k <= b && k != NONE; k++) {
        u8 *p;
        if (k >= keep_a && k <= keep_b) continue;
        if ((p = slot(dir, k)) != 0) { p[0] = 0xE5; ddirty = 1; }
    }
}
/* Put entry e (with long name name, n entries) in dir; it replaces slots
 * lo..hi (NONE: a new entry). */
static int dir_place(u16 dir, const char *name, int n, const u8 *e, u16 lo, u16 hi)
{
    u16 run;
    int err = dir_room(dir, n + 1, lo, hi, &run);
    if (err) return err;
    if ((err = dir_write(dir, run, name, n, e)) != 0) return err;
    if (lo != NONE) dir_drop(dir, lo, hi, run, run + n);
    if ((err = dflush()) != 0) return err;
    return ioerr;
}
/* The long name entries of an entry, dropped (checked first: they are
 * still that entry's). */
static void drop_lfn(u16 dir, struct dent *d)
{
    u8 sum = lfn_sum(d->e);
    u16 k;
    for (k = d->first; k < d->idx; k++) {
        u8 *p = slot(dir, k);
        if (p && p[0] != 0xE5 && p[11] == 0x0F && p[13] == sum) { p[0] = 0xE5; ddirty = 1; }
    }
    dflush();
}
/* Long name entries whose short entry is gone: deleted (so a folder that
 * an 8.3 program emptied can be removed). */
static void dir_sweep(u16 dir)
{
    u16 i, first = 0;
    u8 want = 0, sum = 0, complete = 0, run = 0;
    for (i = 0;; i++) {
        u8 *p = slot(dir, i);
        if (!p || !p[0]) { if (run) dir_drop(dir, first, i - 1, NONE, NONE); break; }
        if (p[0] != 0xE5 && p[11] == 0x0F) {
            u8 seq = p[0] & 0x1F;
            if (run && !complete && !(p[0] & 0x40) && seq == want && p[13] == sum) {
                if (--want == 0) complete = 1;
                continue;
            }
            if (run) dir_drop(dir, first, i - 1, NONE, NONE);
            if ((p = slot(dir, i)) == 0) break;
            if ((p[0] & 0x40) && seq >= 1 && seq <= 20) {
                run = 1; first = i; want = seq - 1; sum = p[13]; complete = want == 0;
            } else { p[0] = 0xE5; ddirty = 1; run = 0; }
            continue;
        }
        if (run && !(complete && p[0] != 0xE5 && !(p[11] & 8) && lfn_sum(p) == sum))
            dir_drop(dir, first, i - 1, NONE, NONE);
        run = 0;
    }
    dflush();
}

/* ------------------------------------------------------------------ */
/* Paths                                                               */
struct path { char drive; char norm[MAXP]; char *leaf; };
static struct path P1, P2;
static char tmp[MAXP];
/* DS:DX-style caller path -> "\A\B\leaf" (absolute; . and .. applied;
 * trailing dots and spaces of each name dropped, as Windows does). */
static int path_parse(u16 seg, u16 off, struct path *pp)
{
    char *s = tmp, *o = pp->norm;
    int n = 0;
    if (far_str(tmp, seg, off, MAXP)) return 3;
    while (*s == ' ') s++;
    if (!*s) return 3;
    if (s[1] == ':') { pp->drive = up(s[0]); s += 2; }
    else { kclear(); K.ax = 0x1900; kcall(); pp->drive = (char)('A' + (K.ax & 0xFF)); }
    if (pp->drive != 'C' && pp->drive != 'D') return 15;
    o[0] = 0;
    if (*s != '\\' && *s != '/') {
        char cwd[68];
        kclear(); K.ax = 0x4700; K.dx = pp->drive - 'A' + 1; K.si = (u16)cwd;
        if (kcall()) return 15;
        for (n = 0; cwd[n]; n++) o[n + 1] = cwd[n];
        if (n) { o[0] = '\\'; n++; }
        o[n] = 0;
    }
    while (*s) {
        char *t;
        int k;
        while (*s == '\\' || *s == '/') s++;
        if (!*s) break;
        for (t = s; *t && *t != '\\' && *t != '/'; t++) ;
        k = (int)(t - s);
        if (k == 1 && s[0] == '.') { s = t; continue; }
        if (k == 2 && s[0] == '.' && s[1] == '.') {
            while (n > 0 && o[n - 1] != '\\') n--;
            if (n > 0) n--;
            o[n] = 0;
            s = t;
            continue;
        }
        while (k > 0 && (s[k - 1] == '.' || s[k - 1] == ' ')) k--;
        if (!k) return 3;
        if (n + k + 1 >= MAXP - 3) return 3;
        o[n++] = '\\';
        mcpy(o + n, s, k);
        n += k;
        o[n] = 0;
        s = t;
    }
    if (!n) { o[0] = '\\'; o[1] = 0; pp->leaf = o + 1; return 0; }
    while (n > 0 && o[n - 1] != '\\') n--;
    pp->leaf = o + n;
    return 0;
}

struct res {
    u16 dir;                        /* the parent folder */
    u8 found, root;
    struct dent d;                  /* the leaf, when found */
    char sp[MAXP];                  /* short path of the parent ("C:") */
    char lp[MAXP];                  /* long path of the parent */
};
static struct res A, B;
static struct dent T;
static void pcat(char *d, const char *s)
{
    int n = slen(d);
    if (n + slen(s) + 2 >= MAXP) return;
    d[n] = '\\';
    scpy(d + n + 1, s);
}
/* Parent folder and leaf (flags 1: no lookup of the leaf, a pattern; 2:
 * the leaf matches short names only). */
static int resolve(struct path *pp, struct res *r, int flags)
{
    char *c = pp->norm + 1, *e;
    r->dir = 0; r->found = 0; r->root = !*pp->leaf;
    r->sp[0] = r->lp[0] = pp->drive; r->sp[1] = r->lp[1] = ':'; r->sp[2] = r->lp[2] = 0;
    while (c < pp->leaf) {
        for (e = c; *e != '\\'; e++) ;
        *e = 0;
        if (has_wild(c) || !dir_find(r->dir, c, &T, 0) || !(T.e[11] & 0x10)) { *e = '\\'; return ioerr ? ioerr : 3; }
        *e = '\\';
        r->dir = w16(T.e + 26);
        pcat(r->sp, T.sname);
        pcat(r->lp, T.lname);
        c = e + 1;
    }
    if (r->root || (flags & 1)) return 0;
    if (has_wild(pp->leaf)) return (flags & 2) ? 2 : 0;
    r->found = (u8)dir_find(r->dir, pp->leaf, &r->d, flags & 2);
    return ioerr;
}
static char kp[MAXP];               /* a short path for the kernel */
static const char *kpath(struct res *r, const char *name)
{
    scpy(kp, r->sp);
    if (name) pcat(kp, name);
    else if (!kp[2]) { kp[2] = '\\'; kp[3] = 0; }
    return kp;
}
static int kpath_call(u16 ax, struct res *r, const char *name)
{
    kclear(); K.ax = ax; K.dx = (u16)kpath(r, name);
    if (slen(kp) > 90) return 3;
    return kcall() ? (int)K.ax : 0;
}

/* ------------------------------------------------------------------ */
/* Times                                                               */
static void now(u16 *time, u16 *date)
{
    kclear(); K.ax = 0x2C00; kcall();
    *time = ((K.cx >> 8) << 11) | ((K.cx & 0xFF) << 5) | ((K.dx >> 8) >> 1);
    kclear(); K.ax = 0x2A00; kcall();
    *date = ((K.cx - 1980) << 9) | ((K.dx >> 8) << 5) | (K.dx & 0xFF);
}
static void mul_add(u16 *w, u16 m, u16 a)
{
    u32 carry = a;
    int i;
    for (i = 0; i < 4; i++) { carry += mul16(w[i], m); w[i] = (u16)carry; carry >>= 16; }
}
/* DOS date/time -> FILETIME (100 ns since 1601), local time as UTC. */
static void filetime(u8 __far *o, u16 date, u16 time)
{
    static const u16 mdays[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    u16 w[4], y, m = (date >> 5) & 15, i;
    u32 days;
    fput32(o, 0); fput32(o + 4, 0);
    if (!date || m < 1 || m > 12) return;
    y = 1980 + (date >> 9);
    days = 138426L + (date & 31) - 1 + mdays[m - 1];
    for (i = 1980; i < y; i++) days += (i & 3) || i == 2100 ? 365 : 366;
    if (m > 2 && !(y & 3) && y != 2100) days++;
    w[0] = (u16)days; w[1] = (u16)(days >> 16); w[2] = w[3] = 0;
    mul_add(w, 24, time >> 11);
    mul_add(w, 60, (time >> 5) & 63);
    mul_add(w, 60, (time & 31) * 2);
    mul_add(w, 10000, 0);
    mul_add(w, 1000, 0);
    for (i = 0; i < 4; i++) fput16(o + 2 * i, w[i]);
}
static void stamp(u8 __far *o, u16 date, u16 time, int dos)
{
    if (dos) { fput16(o, time); fput16(o + 2, date); fput32(o + 4, 0); }
    else filetime(o, date, time);
}

/* ------------------------------------------------------------------ */
/* Find handles                                                        */
#define NFIND 10
#define PATLEN 64
struct find { u16 psp, dir, next; u8 used, allow, need, once; u16 age; char pat[PATLEN]; };
static struct find F[NFIND];
static u16 find_age;
static u16 cur_psp(void) { kclear(); K.ax = 0x6200; kcall(); return K.bx; }
static int glob(const char *p, const char *s)
{
    const char *sp = 0, *ss = 0;
    while (*s) {
        if (*p == '*') { sp = ++p; ss = s; continue; }
        if (*p && (*p == '?' || up(*p) == up(*s))) { p++; s++; continue; }
        if (sp) { p = sp; s = ++ss; continue; }
        return 0;
    }
    for (;;) {
        if (*p == '*' || *p == '?') p++;
        else if (p[0] == '.' && (p[1] == '*' || !p[1])) p++;
        else break;
    }
    return !*p;
}
static int name_match(const char *pat, struct dent *d)
{
    if (!mcmp(pat, "*.*", 4) || !mcmp(pat, "*", 2)) return 1;
    return glob(pat, d->lname) || glob(pat, d->sname);
}
static int attr_ok(u8 a, u8 allow, u8 need)
{
    if ((a & 0x08) && !(allow & 0x08)) return 0;
    if (a & ~allow & 0x16) return 0;
    return (a & need) == need;
}
static void find_fill(struct dent *d, int dos)
{
    u8 __far *f = FP(R.es, R.di);
    u8 *e = d->e;
    int i;
    for (i = 0; i < 318; i++) f[i] = 0;
    fput32(f, e[11]);
    stamp(f + 4, w16(e + 16), w16(e + 14), dos);
    stamp(f + 12, w16(e + 18), 0, dos);
    stamp(f + 20, w16(e + 24), w16(e + 22), dos);
    fput16(f + 32, w16(e + 28));
    fput16(f + 34, w16(e + 30));
    far_put(R.es, R.di + 44, d->lname);
    if (d->first != d->idx) far_put(R.es, R.di + 304, d->sname);
    R.cx = d->lossy ? 1 : 0;
}
static int find_step(struct find *h)
{
    while (dir_next(h->dir, &h->next, &T)) {
        if (!name_match(h->pat, &T) || !attr_ok(T.e[11], h->allow, h->need)) continue;
        find_fill(&T, R.si == 1);
        return 0;
    }
    return ioerr ? ioerr : 18;
}

/* ------------------------------------------------------------------ */
/* The functions                                                        */

static int f_mkdir(void)
{
    u8 al[11];
    int n, e;
    u16 t, d;
    char s[13];
    if ((e = path_parse(R.ds, R.dx, &P1)) != 0 || (e = resolve(&P1, &A, 0)) != 0) return e;
    if (A.root || A.found) return 5;
    if (!valid_name(P1.leaf)) return 3;
    n = make_alias(A.dir, P1.leaf, al, NONE);
    if (ioerr) return ioerr;
    if (n) { u16 run; if ((e = dir_room(A.dir, n + 1, NONE, NONE, &run)) != 0) return e; }
    short_name(al, s, 0);
    if ((e = kpath_call(0x3900, &A, s)) != 0) return e;
    if (!dir_find(A.dir, s, &T, 1)) return ioerr ? ioerr : 5;
    /* The kernel writes a fixed date on folders: made now. */
    now(&t, &d);
    put16(T.e + 14, t); put16(T.e + 22, t);
    put16(T.e + 16, d); put16(T.e + 18, d); put16(T.e + 24, d);
    return dir_place(A.dir, P1.leaf, n, T.e, T.first, T.idx);
}

static int f_rmdir(void)
{
    int e;
    if ((e = path_parse(R.ds, R.dx, &P1)) != 0 || (e = resolve(&P1, &A, 0)) != 0) return e;
    if (A.root) return 16;
    if (!A.found) return 3;
    if (!(A.d.e[11] & 0x10)) return 5;
    dir_sweep(w16(A.d.e + 26));
    if ((e = kpath_call(0x3A00, &A, A.d.sname)) != 0) return e;
    drop_lfn(A.dir, &A.d);
    return 0;
}

static int f_chdir(void)
{
    int e;
    if ((e = path_parse(R.ds, R.dx, &P1)) != 0 || (e = resolve(&P1, &A, 0)) != 0) return e;
    if (A.root) return kpath_call(0x3B00, &A, 0);
    if (!A.found || !(A.d.e[11] & 0x10)) return 3;
    return kpath_call(0x3B00, &A, A.d.sname);
}

static int f_delete(void)
{
    int e, wild, any = 0;
    u16 i = 0;
    if ((e = path_parse(R.ds, R.dx, &P1)) != 0) return e;
    wild = R.si == 1 && has_wild(P1.leaf);
    if ((e = resolve(&P1, &A, wild ? 1 : 0)) != 0) return e;
    if (A.root) return 5;
    if (!wild) {
        if (!A.found) return 2;
        if (A.d.e[11] & 0x18) return 5;
        if ((e = kpath_call(0x4100, &A, A.d.sname)) != 0) return e;
        drop_lfn(A.dir, &A.d);
        return 0;
    }
    while (dir_next(A.dir, &i, &T)) {
        if (T.e[11] & 0x18) continue;
        if (!name_match(P1.leaf, &T) || !attr_ok(T.e[11], (u8)R.cx, (u8)(R.cx >> 8))) continue;
        if ((e = kpath_call(0x4100, &A, T.sname)) != 0) return e;
        drop_lfn(A.dir, &T);
        any = 1;
    }
    return ioerr ? ioerr : any ? 0 : 2;
}

static int f_attr(void)
{
    int e;
    u8 bl = (u8)R.bx, *p;
    if ((e = path_parse(R.ds, R.dx, &P1)) != 0 || (e = resolve(&P1, &A, 0)) != 0) return e;
    if (A.root) { if (bl) return 5; R.cx = 0x10; return 0; }
    if (!A.found) return 2;
    p = A.d.e;
    switch (bl) {
    case 0: R.cx = p[11]; R.ax = p[11]; return 0;
    case 2: R.ax = w16(p + 28); R.dx = w16(p + 30); return 0;
    case 4: R.cx = w16(p + 22); R.di = w16(p + 24); return 0;
    case 6: R.di = w16(p + 18); return 0;
    case 8: R.cx = w16(p + 14); R.di = w16(p + 16); R.si = p[13]; return 0;
    case 1: case 3: case 5: case 7: break;
    default: return 1;
    }
    if (!(p = slot(A.dir, A.d.idx))) return 5;
    switch (bl) {
    case 1:
        if ((R.cx & 0x18) != (p[11] & 0x18) && (R.cx & 0x18)) return 5;
        p[11] = (u8)((p[11] & 0x18) | (R.cx & 0x27));
        break;
    case 3: put16(p + 22, R.cx); put16(p + 24, R.di); break;
    case 5: put16(p + 18, R.di); break;
    case 7: put16(p + 14, R.cx); put16(p + 16, R.di); p[13] = (u8)R.si; break;
    }
    ddirty = 1;
    return dflush();
}

/* The long form of a short path "A~1\B" (from AH=47h) into o. */
static int long_of(const char *s, char *o)
{
    u16 dir = 0;
    char c[16];
    o[0] = 0;
    while (*s) {
        int k = 0;
        while (*s && *s != '\\' && k < 15) c[k++] = *s++;
        c[k] = 0;
        while (*s == '\\') s++;
        if (!dir_find(dir, c, &T, 1)) return 3;
        if (o[0]) pcat(o, T.lname); else scpy(o, T.lname);
        dir = w16(T.e + 26);
    }
    return 0;
}
static int f_getcwd(void)
{
    char cwd[68];
    u8 dl = (u8)R.dx;
    kclear(); K.ax = 0x4700; K.dx = dl; K.si = (u16)cwd;
    if (kcall()) return (int)K.ax;
    if (long_of(cwd, B.lp)) scpy(B.lp, cwd);
    far_put(R.ds, R.si, B.lp);
    R.ax = 0x0100;
    return 0;
}

static int f_findfirst(void)
{
    int e, i, best = 0;
    struct find *h;
    if ((e = path_parse(R.ds, R.dx, &P1)) != 0) return e;
    if ((e = resolve(&P1, &A, 0)) != 0) return e == 2 ? 3 : e;
    if (A.root) return 3;
    if (!has_wild(P1.leaf)) {
        if (!A.found || !attr_ok(A.d.e[11], (u8)R.cx, (u8)(R.cx >> 8))) return 2;
    } else if (slen(P1.leaf) >= PATLEN) return 3;
    for (i = 0; i < NFIND; i++) {
        if (!F[i].used) { best = i; break; }
        if (F[i].age < F[best].age) best = i;      /* all used: the oldest goes */
    }
    h = &F[best];
    h->used = 1; h->psp = cur_psp(); h->dir = A.dir; h->next = 0;
    h->allow = (u8)R.cx; h->need = (u8)(R.cx >> 8); h->age = ++find_age;
    if (!has_wild(P1.leaf)) {
        h->once = 1;
        find_fill(&A.d, R.si == 1);
    } else {
        h->once = 0;
        scpy(h->pat, P1.leaf);
        if ((e = find_step(h)) != 0) { h->used = 0; return e == 18 ? 2 : e; }
    }
    R.ax = best + 1;
    return 0;
}
static struct find *handle(void)
{
    u16 b = R.bx - 1;
    return b < NFIND && F[b].used ? &F[b] : 0;
}
static int f_findnext(void)
{
    struct find *h = handle();
    if (!h) return 6;
    h->age = ++find_age;
    if (h->once) return 18;
    return find_step(h);
}
static int f_findclose(void)
{
    struct find *h = handle();
    if (!h) return 6;
    h->used = 0;
    return 0;
}

/* Is folder cluster c the folder dir or one of its parents? */
static int is_above(u16 c, u16 dir)
{
    int guard = 0;
    while (dir && ++guard < 64) {
        u8 *p;
        if (dir == c) return 1;
        p = slot(dir, 1);           /* ".." */
        if (!p || p[0] != '.' || p[1] != '.') return 0;
        dir = w16(p + 26);
    }
    return 0;
}
static int f_rename(void)
{
    int e, n, same;
    u8 al[11], ne[32];
    if ((e = path_parse(R.ds, R.dx, &P1)) != 0 || (e = resolve(&P1, &A, 0)) != 0) return e;
    if (A.root) return 5;
    if (!A.found) return 2;
    if ((e = path_parse(R.es, R.di, &P2)) != 0 || (e = resolve(&P2, &B, 0)) != 0) return e;
    if (P1.drive != P2.drive) return 17;
    if (B.root || !valid_name(P2.leaf)) return 5;
    same = A.dir == B.dir;
    if (B.found && !(same && B.d.idx == A.d.idx)) return 5;
    if ((A.d.e[11] & 0x10) && !same && is_above(w16(A.d.e + 26), B.dir)) return 5;
    n = make_alias(B.dir, P2.leaf, al, same ? A.d.idx : NONE);
    if (ioerr) return ioerr;
    mcpy(ne, A.d.e, 32);
    mcpy(ne, al, 11);
    ne[12] = 0;
    if (same) return dir_place(A.dir, P2.leaf, n, ne, A.d.first, A.d.idx);
    if ((e = dir_place(B.dir, P2.leaf, n, ne, NONE, NONE)) != 0) return e;
    dir_drop(A.dir, A.d.first, A.d.idx, NONE, NONE);
    if (ne[11] & 0x10) {            /* a moved folder: its ".." */
        u8 *p = slot(w16(ne + 26), 1);
        if (p && p[0] == '.' && p[1] == '.') { put16(p + 26, B.dir); ddirty = 1; }
    }
    return dflush() ? 5 : ioerr;
}

static int f_open(void)
{
    int e, n, h, act;
    u16 mode = R.bx & 7, action = R.dx;
    u8 al[11];
    char s[13];
    if (mode == 4) mode = 0;
    if (mode > 2) return 12;
    if ((e = path_parse(R.ds, R.si, &P1)) != 0 || (e = resolve(&P1, &A, 0)) != 0) return e;
    if (A.root) return 5;
    if (A.found) {
        if (A.d.e[11] & 0x18) return 5;
        if (!(action & 3)) return 80;
        act = 1;
        if (action & 2) {           /* replace: the kernel's create truncates */
            kclear(); K.ax = 0x3C00; K.cx = A.d.e[11] & 7; K.dx = (u16)kpath(&A, A.d.sname);
            if (kcall()) return (int)K.ax;
            if (mode == 2) { R.ax = K.ax; R.cx = 3; return 0; }
            h = K.ax;
            kclear(); K.ax = 0x3E00; K.bx = h; kcall();
            act = 3;
        }
        kclear(); K.ax = 0x3D00 | mode; K.dx = (u16)kpath(&A, A.d.sname);
        if (kcall()) return (int)K.ax;
        R.ax = K.ax; R.cx = act;
        return 0;
    }
    if (!(action & 0x10)) return 2;
    if (!valid_name(P1.leaf)) return 3;
    n = make_alias(A.dir, P1.leaf, al, NONE);
    if (ioerr) return ioerr;
    if (n) { u16 run; if ((e = dir_room(A.dir, n + 1, NONE, NONE, &run)) != 0) return e; }
    short_name(al, s, 0);
    kclear(); K.ax = 0x3C00; K.cx = R.cx & 7; K.dx = (u16)kpath(&A, s);
    if (slen(kp) > 90) return 3;
    if (kcall()) return (int)K.ax;
    h = K.ax;
    if (n) {
        /* The entry moves (long name entries before it): no handle may
         * point at it meanwhile. */
        kclear(); K.ax = 0x3E00; K.bx = h; kcall();
        if (!dir_find(A.dir, s, &T, 1)) return ioerr ? ioerr : 5;
        if ((e = dir_place(A.dir, P1.leaf, n, T.e, T.first, T.idx)) != 0) return e;
        kclear(); K.ax = 0x3D00 | mode; K.dx = (u16)kpath(&A, s);
        if (kcall()) return (int)K.ax;
        h = K.ax;
    } else if (mode != 2) {
        kclear(); K.ax = 0x3E00; K.bx = h; kcall();
        kclear(); K.ax = 0x3D00 | mode; K.dx = (u16)kpath(&A, s);
        if (kcall()) return (int)K.ax;
        h = K.ax;
    }
    R.ax = h; R.cx = 2;
    return 0;
}

static int f_truename(void)
{
    int e;
    u8 cl = (u8)R.cx;
    if ((e = path_parse(R.ds, R.si, &P1)) != 0) return e;
    if ((e = resolve(&P1, &A, cl ? 0 : 1)) != 0) return e == 2 ? 3 : e;
    if (!cl) {
        /* Canonical: the existing folders' long names, the leaf as given. */
        scpy(tmp, A.lp);
        if (!A.root) pcat(tmp, P1.leaf);
    } else if (A.root) scpy(tmp, A.sp);
    else {
        if (!A.found) return 2;
        scpy(tmp, cl == 1 ? A.sp : A.lp);
        pcat(tmp, cl == 1 ? A.d.sname : A.d.lname);
    }
    if (!tmp[2]) { tmp[2] = '\\'; tmp[3] = 0; }
    far_put(R.es, R.di, tmp);
    return 0;
}

static int f_volume(void)
{
    char r[8];
    u8 __far *o = FP(R.es, R.di);
    far_str(r, R.ds, R.dx, 8);
    if (up(r[0]) != 'C' && up(r[0]) != 'D') return 15;
    R.bx = 0x4006;                  /* case preserved, Unicode on disk, LFN API */
    R.dx = 260;
    if (R.cx >= 4) { o[0] = 'F'; o[1] = 'A'; o[2] = 'T'; o[3] = 0; }
    R.cx = 255;
    R.ax = 0;
    return 0;
}

static int f_handle_info(void)
{
    u8 __far *o = FP(R.ds, R.dx);
    u16 t, d, plo, phi, slo, shi, h = R.bx;
    int i;
    kclear(); K.ax = 0x5700; K.bx = h;
    if (kcall()) return (int)K.ax;
    t = K.cx; d = K.dx;
    kclear(); K.ax = 0x4201; K.bx = h; kcall(); plo = K.ax; phi = K.dx;
    kclear(); K.ax = 0x4202; K.bx = h; kcall(); slo = K.ax; shi = K.dx;
    kclear(); K.ax = 0x4200; K.bx = h; K.cx = phi; K.dx = plo; kcall();
    for (i = 0; i < 52; i++) o[i] = 0;
    fput32(o, 0x20);
    filetime(o + 4, d, t);
    filetime(o + 12, d, 0);
    filetime(o + 20, d, t);
    fput32(o + 28, g_serial);
    fput16(o + 36, shi);
    fput16(o + 38, slo);
    fput32(o + 40, 1);
    fput32(o + 48, h);
    return 0;
}

static int f_short_name(void)
{
    u8 o[11];
    char s[13];
    int blen;
    if (far_str(tmp, R.ds, R.si, MAXP)) return 3;
    if (alias_basis(tmp, o, &blen)) {        /* lossy: the first numeric tail */
        int k = blen < 6 ? blen : 6;
        mset(o + k, ' ', 8 - k);
        o[k] = '~'; o[k + 1] = '1';
    }
    if ((R.dx >> 8) == 0) { u8 __far *d = FP(R.es, R.di); int i; for (i = 0; i < 11; i++) d[i] = o[i]; }
    else { short_name(o, s, 0); far_put(R.es, R.di, s); }
    return 0;
}

static int f_time(void)
{
    if ((u8)R.bx != 1) return 1;
    filetime(FP(R.es, R.di), R.dx, R.cx);
    return 0;
}

/* The 8.3 calls: AH=41h/3Ah/56h drop the long name entries of what they
 * delete or rename. Only for a leaf given by its short name; everything
 * else goes to the kernel unchanged. */
static int hook83(void)
{
    u8 ah = (u8)(R.ax >> 8);
    u16 seg = R.ds, off = R.dx;
    if (path_parse(seg, off, &P1) || !*P1.leaf || resolve(&P1, &A, 2) || !A.found) return -1;
    if (A.d.first == A.d.idx) return -1;
    if (ah == 0x3A) dir_sweep(w16(A.d.e + 26));
    mcpy(&K, &R, sizeof K);
    kcall();
    R.ax = K.ax;
    R.flags = K.flags;
    if (!(K.flags & 1)) drop_lfn(A.dir, &A.d);
    return 0;
}

static void op_begin(void) { forget(); ioerr = 0; ddirty = fdirty = 0; }
static void op_end(void) { dflush(); fflush(); }

/* AX = 0: handled (R holds the results), else pass the call on. */
int lfn_dispatch(void)
{
    u8 ah = (u8)(R.ax >> 8), al = (u8)R.ax;
    int e = 0, i;
    if (ah == 0x4C || ah == 0) {
        u16 psp = cur_psp();
        for (i = 0; i < NFIND; i++) if (F[i].psp == psp) F[i].used = 0;
        return 1;
    }
    if (kenter()) { R.ax = 5; R.flags |= 1; return 0; }
    op_begin();
    if (ah != 0x71) {
        e = hook83();
        op_end();
        if (kleave() && e >= 0) { R.ax = 5; R.flags |= 1; return 0; }
        return e < 0 ? 1 : 0;
    }
    switch (al) {
    case 0x39: e = f_mkdir(); break;
    case 0x3A: e = f_rmdir(); break;
    case 0x3B: e = f_chdir(); break;
    case 0x41: e = f_delete(); break;
    case 0x43: e = f_attr(); break;
    case 0x47: e = f_getcwd(); break;
    case 0x4E: e = f_findfirst(); break;
    case 0x4F: e = f_findnext(); break;
    case 0xA1: e = f_findclose(); break;
    case 0x56: e = f_rename(); break;
    case 0x60: e = f_truename(); break;
    case 0x6C: e = f_open(); break;
    case 0xA0: e = f_volume(); break;
    case 0xA6: e = f_handle_info(); break;
    case 0xA7: e = f_time(); break;
    case 0xA8: e = f_short_name(); break;
    case 0xFF:
        if (R.bx != 0x4C46) { e = -1; break; }
        R.bx = 0x4F4B; R.ax = 0x0100;
        break;
    default: e = -1; break;
    }
    op_end();
    if (!e && ioerr) e = ioerr;
    if (kleave() && !e) e = 5;
    if (e < 0) { R.ax = 0x7100; R.flags |= 1; }
    else if (e) { R.ax = (u16)e; R.flags |= 1; }
    else R.flags &= ~1;
    return 0;
}
