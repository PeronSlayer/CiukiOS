/* Files: the CiukiOS file manager, in the manner of Windows Explorer and
 * KDE Dolphin. Places, address bar, Back/Forward/Up, Details/Icons/List views
 * with sortable columns, multiple selection, context menus, New Folder, New
 * Text Document, Cut/Copy/Paste (background copy with progress), Rename in
 * place, Delete, Properties. Removable media (floppy, USB, CD-ROM) go
 * through MEDIA.DRV: read-only, their files are pasted (imported) onto the
 * system disk. Local files use the resident VFAT long-name service. */
#include "app.h"

#define MAX_ITEMS 172
#define PATH_LEN  SYS_PATH
#define NAME_LEN  13
#define HISTORY   6
#define CLIP_MAX  28

struct item {
    char name[NAME_LEN];
    u16 slot;              /* full name in name_seg (survives sorting) */
    u8 attr;
    u8 sel;
    u32 size;
    u16 date, time;
};
static struct item items[MAX_ITEMS];
static u16 name_seg;
static char name_cache[2][LFN_NAME];
static u8 name_cache_next;
static const char *item_name(const struct item *it)
{
    char *out;
    const char __far *src;
    int n = LFN_NAME - 1;
    if (!name_seg) return it->name;
    out = name_cache[(name_cache_next++) & 1];
    src = (const char __far *)(((u32)name_seg << 16) | (u16)(it->slot * LFN_NAME));
    while (n-- && *src) *out++ = *src++;
    *out = 0;
    return name_cache[(name_cache_next - 1) & 1];
}
static void item_set_name(struct item *it, const char *name)
{
    char __far *dst;
    int n = LFN_NAME - 1;
    str_ncopy(it->name, name, NAME_LEN);
    if (!name_seg) return;
    dst = (char __far *)(((u32)name_seg << 16) | (u16)(it->slot * LFN_NAME));
    while (n-- && *name) *dst++ = *name++;
    *dst = 0;
}
static int nitems, cur = -1, anchor_i = -1, top_row;
static char cwd[PATH_LEN];                 /* "C:\\APPS" or a media path "/DIR" */
static int media;                          /* 0 system disk, else MD_* device */
static char status_text[96];
static int view = 0;                       /* 0 details, 1 large icons, 2 list */
static int sort_col = 0, sort_desc = 0, show_hidden = 0;
static u16 history_seg;
static u8 back_media[HISTORY], fwd_media[HISTORY];
static int nback, nfwd;
static void history_put(int forward, int i, const char *path)
{
    char __far *dst = (char __far *)(((u32)history_seg << 16) |
                                   (u16)((forward * HISTORY + i) * PATH_LEN));
    int n = PATH_LEN;
    while (--n && *path) *dst++ = *path++;
    *dst = 0;
}
static void history_get(int forward, int i, char *path)
{
    const char __far *src = (const char __far *)(((u32)history_seg << 16) |
                                         (u16)((forward * HISTORY + i) * PATH_LEN));
    int n = PATH_LEN;
    while (--n && *src) *path++ = *src++;
    *path = 0;
}
static unsigned last_click_tick;
static int last_click_i = -1;
static int focus_area;                     /* 0 list, 1 address, 2 places */
static int drag_i = -1, dragging, drag_x, drag_y, drop_i = -1, drop_place = -1, drop_tree = -1;
static void tree_build(void);
static int place_focus;

/* Rename in place. */
static int renaming = -1;
static struct field rename_field;
static char rename_buf[LFN_NAME];
static struct field address;
static char address_buf[PATH_LEN];

/* ------------------------------------------------------------------ */
/* Removable media: MEDIA.DRV (src/com/media_driver_abi.h).            */
#define MD_MOUNT 1
#define MD_LIST 2
#define MD_PREVIEW_FILE 3
#define MD_IMPORT_FILE 4
#define MD_UP 7
#define MD_REFRESH 8
#pragma pack(push, 1)
struct media_row { char name[40]; u32 size; u8 directory, reserved; };
struct media_request {
    u16 version, operation, status, device, first, count, total, preview_bytes;
    u16 preview_more, bios_drive;
    char path[192];
    char destination[64];
    char message[128];
    struct media_row rows[6];
    char preview[512];
};
#pragma pack(pop)
static struct media_request mreq;
static u16 media_seg;

static int media_load(void)
{
    u8 head[32];
    int h, n;
    u16 paras;
    if (media_seg) return 1;
    h = dos_open("\\SYSTEM\\MEDIA.DRV", 0);
    if (h < 0) return 0;
    n = dos_read(h, head, 32);
    if (n != 32 || str_nicmp((char *)head + 4, "CMEDIA01", 8) || head[12] != 1) { dos_close(h); return 0; }
    paras = head[16] | (head[17] << 8);
    media_seg = dos_alloc(paras);
    if (!media_seg) { dos_close(h); return 0; }
    dos_seek(h, 0, 0);
    n = dos_read_far(h, media_seg, 0, 0xEF00);
    dos_close(h);
    if (n < 32) { dos_free(media_seg); media_seg = 0; return 0; }
    return 1;
}
static int media_call_dev(int op, int device)
{
    mreq.version = 1;
    mreq.operation = op;
    mreq.device = device;
    far_call_req(media_seg, 0, &mreq);
    return mreq.status == 0;
}
static int media_call(int op) { return media_call_dev(op, media); }

/* ------------------------------------------------------------------ */
/* Paths                                                               */
static int is_root(const char *p) { return p[0] && p[1] == ':' && p[2] == '\\' && !p[3]; }
static void path_join(char *out, const char *dir, const char *name)
{
    if (str_len(dir) + str_len(name) + 2 > PATH_LEN) { out[0] = 0; return; }
    str_copy(out, dir);
    if (media) {
        if (out[str_len(out) - 1] != '/') str_cat(out, "/");
    } else if (out[str_len(out) - 1] != '\\') str_cat(out, "\\");
    str_cat(out, name);
}
static void local_join(char *out, const char *dir, const char *name)
{
    if (str_len(dir) + str_len(name) + 2 > PATH_LEN) { out[0] = 0; return; }
    str_copy(out, dir);
    if (out[str_len(out) - 1] != '\\') str_cat(out, "\\");
    str_cat(out, name);
}
static void path_parent(char *p)
{
    int n = str_len(p);
    char sep = media ? '/' : '\\';
    if (media) {
        while (n > 1 && p[n - 1] != '/') n--;
        if (n > 1) n--;
        p[n] = 0;
        if (!p[0]) str_copy(p, "/");
        return;
    }
    if (is_root(p)) return;
    while (n > 3 && p[n - 1] != sep) n--;
    if (n > 3) n--;
    p[n] = 0;
    if (n == 2) { p[2] = '\\'; p[3] = 0; }
}
static const char *base_name(const char *p)
{
    const char *n = p;
    while (*p) { if (*p == '\\' || *p == '/' || *p == ':') n = p + 1; p++; }
    return n;
}
static const char *ext_of(const char *name)
{
    const char *e = 0;
    while (*name) { if (*name == '.') e = name + 1; name++; }
    return e ? e : "";
}
static const char *media_name(int d)
{
    return d == 1 ? "Floppy (A:)" : d == 2 ? "USB drive" : d == 3 ? "CD-ROM" : "Removable";
}

static void upper(char *s) { while (*s) { *s = to_upper(*s); s++; } }

/* ------------------------------------------------------------------ */
/* Types and sorting                                                   */
static int ext_is(const char *name, const char *list)   /* "TXT|INI" */
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
#define TEXT_TYPES "TXT|INI|CFG|LOG|MD|ASM|C|H|BAT|DOC|ME|1ST|DIZ|NFO|INF|CSV|XML|HTM|JSON|PY|SH"
static const char *type_name(const struct item *it, char *buf)
{
    const char *e;
    if (it->attr & A_DIR) return "File Folder";
    if (ext_is(item_name(it), "TXT|ME|1ST|DIZ|NFO")) return "Text Document";
    if (ext_is(item_name(it), "COM|EXE")) return "Application";
    if (ext_is(item_name(it), "BAT")) return "MS-DOS Batch File";
    if (ext_is(item_name(it), "INI|CFG|INF")) return "Configuration Settings";
    if (ext_is(item_name(it), "SYS|DRV|DLL|386|VXD")) return "System file";
    if (ext_is(item_name(it), "LOG")) return "Log File";
    if (ext_is(item_name(it), "CFN")) return "CiukiOS Font";
    if (ext_is(item_name(it), "BMP|PCX|PNG|GIF|JPG")) return "Image";
    if (ext_is(item_name(it), "WAV|MID|VOC|PCM|MP3")) return "Sound";
    if (ext_is(item_name(it), "ZIP|ARJ|LZH|RAR")) return "Compressed Archive";
    e = ext_of(item_name(it));
    if (!*e) return "File";
    str_ncopy(buf, e, 15);
    upper(buf);
    str_cat(buf, " File");
    return buf;
}
static int cmp_items(const struct item *a, const struct item *b)
{
    int r = 0;
    char ta[20], tb[20];
    if ((a->attr & A_DIR) != (b->attr & A_DIR)) return (a->attr & A_DIR) ? -1 : 1;
    switch (sort_col) {
    case 1: r = a->size < b->size ? -1 : a->size > b->size ? 1 : 0; break;
    case 2: r = str_icmp(type_name(a, ta), type_name(b, tb)); break;
    case 3: r = a->date != b->date ? (a->date < b->date ? -1 : 1) :
                a->time != b->time ? (a->time < b->time ? -1 : 1) : 0; break;
    }
    if (!r) r = str_icmp(item_name(a), item_name(b));
    return sort_desc ? -r : r;
}
static void sort_items(void)
{
    int i, j;
    struct item t;
    char keep[LFN_NAME];
    keep[0] = 0;
    if (cur >= 0 && cur < nitems) str_copy(keep, item_name(&items[cur]));
    for (i = 1; i < nitems; i++) {
        mem_copy(&t, &items[i], sizeof t);
        for (j = i; j > 0 && cmp_items(&t, &items[j - 1]) < 0; j--)
            mem_copy(&items[j], &items[j - 1], sizeof t);
        mem_copy(&items[j], &t, sizeof t);
    }
    if (keep[0]) for (i = 0; i < nitems; i++) if (!str_cmp(item_name(&items[i]), keep)) { cur = i; break; }
}

/* ------------------------------------------------------------------ */
/* Listing                                                             */
static int count_selected(void)
{
    int i, n = 0;
    for (i = 0; i < nitems; i++) n += items[i].sel;
    return n;
}
static void select_only(int i)
{
    int k;
    for (k = 0; k < nitems; k++) items[k].sel = 0;
    if (i >= 0 && i < nitems) items[i].sel = 1;
    cur = i;
    anchor_i = i;
}
static int load_local(void)
{
    char pat[PATH_LEN + 6];
    struct dir_ent f;
    int r;
    path_join(pat, cwd, "*.*");
    r = dir_first(pat, A_DIR | A_HIDDEN | A_SYSTEM | A_RDONLY | A_ARCH, &f);
    if (r < 0 && r != -18 && r != -2) return r;
    for (; r >= 0 && nitems < MAX_ITEMS; r = dir_next(&f)) {
        struct item *it;
        if (f.attr & A_VOLUME) continue;
        if (f.name[0] == '.' && (!f.name[1] || (f.name[1] == '.' && !f.name[2]))) continue;
        if (!show_hidden && (f.attr & (A_HIDDEN | A_SYSTEM))) continue;
        it = &items[nitems++];
        it->slot = (u16)(nitems - 1);
        item_set_name(it, name_seg ? f.name : (f.alias[0] ? f.alias : f.name));
        it->attr = f.attr;
        it->size = f.size;
        it->date = f.date;
        it->time = f.time;
        it->sel = 0;
    }
    dir_close(&f);
    return 0;
}
static int load_media(void)
{
    u16 first = 0;
    str_ncopy(mreq.path, cwd, sizeof mreq.path);
    for (;;) {
        int i;
        mreq.first = first;
        if (!media_call(MD_LIST)) { str_ncopy(status_text, mreq.message, sizeof status_text); return -1; }
        for (i = 0; i < mreq.count && nitems < MAX_ITEMS; i++) {
            struct item *it = &items[nitems++];
            it->slot = (u16)(nitems - 1);
            item_set_name(it, mreq.rows[i].name);
            it->attr = mreq.rows[i].directory ? A_DIR | A_RDONLY : A_RDONLY;
            it->size = mreq.rows[i].size;
            it->date = it->time = 0;
            it->sel = 0;
        }
        first += mreq.count;
        if (!mreq.count || first >= mreq.total || nitems >= MAX_ITEMS) break;
    }
    return 0;
}
/* A signature of the folder's entries: Files refreshes when it changes. */
static u32 dir_sig;
static unsigned sig_tick;
static u16 seen_changes;                   /* fs_changes() at the last look */
static u32 list_sig(void)
{
    char pat[PATH_LEN + 6];
    struct dir_ent f;
    u32 h = 0;
    int r, n = 0;
    path_join(pat, cwd, "*.*");
    for (r = dir_first(pat, A_DIR | A_HIDDEN | A_SYSTEM | A_RDONLY | A_ARCH, &f); r >= 0 && n < 400; r = dir_next(&f), n++) {
        const char *c = f.name;
        h = h * 33 + f.size + f.date + ((u32)f.time << 3) + f.attr;
        while (*c) h = h * 31 + (u8)*c++;
    }
    dir_close(&f);
    return h + n;
}
static void set_title(void)
{
    const char *n = media ? (cwd[1] ? base_name(cwd) : media_name(media)) : (is_root(cwd) ? "Local Disk" : base_name(cwd));
    str_copy(app_title, n);
    if (!media && is_root(cwd)) {
        char d[6];
        d[0] = ' '; d[1] = '('; d[2] = cwd[0]; d[3] = ':'; d[4] = ')'; d[5] = 0;
        str_cat(app_title, d);
    }
    str_cat(app_title, " - Files");
}
static void refresh(void)
{
    char keep[LFN_NAME];
    int r;
    keep[0] = 0;
    if (cur >= 0 && cur < nitems) str_copy(keep, item_name(&items[cur]));
    nitems = 0;
    cur = -1;
    anchor_i = -1;
    renaming = -1;
    status_text[0] = 0;
    r = media ? load_media() : load_local();
    if (!media) dir_sig = list_sig();
    sig_tick = HOST.ticks;
    if (r < 0 && !media) str_copy(status_text, dos_error_text(r));
    sort_items();
    if (keep[0]) {
        int i;
        for (i = 0; i < nitems; i++) if (!str_icmp(item_name(&items[i]), keep)) { cur = i; break; }
    }
    if (cur < 0 && nitems) cur = 0;
    anchor_i = cur;
    tree_build();
    if (top_row < 0) top_row = 0;
    {
        char t[PATH_LEN + 16], n[8];
        str_copy(t, cwd);
        if (media) { str_copy(t, "media:"); t[6] = (char)('0' + media); t[7] = ':'; t[8] = 0; str_cat(t, cwd); }
        str_cat(t, " ");
        fmt_u32(n, (u32)nitems);
        str_cat(t, n);
        app_log("[FILES] list", t);
    }
    field_set(&address, address_buf, PATH_LEN, media ? media_name(media) : cwd);
    if (media) { str_copy(address_buf, media_name(media)); str_cat(address_buf, cwd); address.len = str_len(address_buf); address.cursor = address.len; }
    set_title();
}
static void push_history(void)
{
    int i;
    if (!history_seg) { nback = nfwd = 0; return; }
    if (nback == HISTORY) {
        for (i = 1; i < HISTORY; i++) {
            char path[PATH_LEN];
            history_get(0, i, path);
            history_put(0, i - 1, path);
            back_media[i - 1] = back_media[i];
        }
        nback--;
    }
    history_put(0, nback, cwd);
    back_media[nback++] = (u8)media;
}
/* Go to a folder (local path, or media device d with path). */
static int navigate(const char *path, int d, int record)
{
    char p[PATH_LEN];
    char canonical[SYS_PATH];
    int attr;
    str_ncopy(p, path, PATH_LEN);
    if (d) {
        if (!media_load()) { str_copy(status_text, "Removable media need SYSTEM\\MEDIA.DRV."); return 0; }
        if (d != media) {
            media = d;
            if (!media_call(MD_MOUNT)) {
                str_ncopy(status_text, mreq.message, sizeof status_text);
                app_log("[FILES] media error", status_text);
                media = 0;
                app_sound(5);
                return 0;
            }
        }
    } else {
        if (dos_lfn()) p[0] = to_upper(p[0]);
        else upper(p);
        if (p[1] != ':' || p[2] != '\\') {
            str_copy(status_text, "Type a full path, for example C:\\APPS.");
            return 0;
        }
        if (!is_root(p)) {
            if (p[str_len(p) - 1] == '\\') p[str_len(p) - 1] = 0;
            attr = dos_get_attr(p);
            if (attr < 0 || !(attr & A_DIR)) {
                str_copy(status_text, "Cannot find '");
                str_cat(status_text, p);
                str_cat(status_text, "'.");
                app_sound(5);
                return 0;
            }
            if (dos_lfn() && dos_long_path(p, canonical) >= 0 && str_len(canonical) < PATH_LEN)
                str_copy(p, canonical);
        }
    }
    if (record) { push_history(); nfwd = 0; }
    media = d;
    str_copy(cwd, p);
    top_row = 0;
    cur = -1;
    refresh();
    cur = anchor_i = nitems ? 0 : -1;
    return 1;
}
static void go_back(void)
{
    char path[PATH_LEN];
    if (!nback || !history_seg) return;
    if (nfwd < HISTORY) { history_put(1, nfwd, cwd); fwd_media[nfwd++] = (u8)media; }
    nback--;
    history_get(0, nback, path);
    navigate(path, back_media[nback], 0);
}
static void go_forward(void)
{
    char path[PATH_LEN];
    if (!nfwd || !history_seg) return;
    push_history();
    nfwd--;
    history_get(1, nfwd, path);
    navigate(path, fwd_media[nfwd], 0);
}
static void go_up(void)
{
    char p[PATH_LEN];
    char child[LFN_NAME];
    int i;
    if (media ? (cwd[0] == '/' && !cwd[1]) : is_root(cwd)) return;
    str_copy(child, base_name(cwd));
    str_copy(p, cwd);
    path_parent(p);
    if (navigate(p, media, 1)) {
        for (i = 0; i < nitems; i++) if (!str_icmp(item_name(&items[i]), child)) { cur = anchor_i = i; break; }
    }
}

/* ------------------------------------------------------------------ */
/* Clipboard and background jobs (copy, move, delete)                  */
static int nclip, clip_cut;                /* CLIPBRD.DAT, shared */
static void clip_refresh(void) { nclip = clip_count(&clip_cut); if (nclip > CLIP_MAX) nclip = CLIP_MAX; }

#define JOB_COPY 1
#define JOB_MOVE 2
#define JOB_DELETE 3
struct level {
    char src[PATH_LEN], dst[PATH_LEN];
    struct dos_find dta;
    int started;
};
static struct {
    int kind, active, index, depth, cancel;
    int src_h, dst_h;
    char file_src[PATH_LEN], file_dst[PATH_LEN];
    u16 fdate, ftime;
    u8 fattr;
    int files, folders, errors, skipped;
    u32 bytes;
    int ask;                     /* waiting for the replace question */
    int from_clip;
    int replace_all;
    char dest_dir[PATH_LEN];
    char message[96];
    char current[LFN_NAME];
} job;
static struct level lv[8];
static u16 jbuf;                            /* the job's 8 KB copy block */
#define JBUF_BYTES 8192
/* Source paths live in a DOS block while a job runs. Keeping the queue out
 * of DGROUP leaves room for the LFN runtime and its search buffers. */
static u16 jsrc_seg;
static u8 jmedia[CLIP_MAX];
static int njob;
static void job_source_put(int i, const char *path)
{
    char __far *dst = (char __far *)(((u32)jsrc_seg << 16) | (u16)(i * PATH_LEN));
    int n = PATH_LEN;
    while (--n && *path) *dst++ = *path++;
    *dst = 0;
}
static void job_source_get(int i, char *path)
{
    const char __far *src = (const char __far *)(((u32)jsrc_seg << 16) | (u16)(i * PATH_LEN));
    int n = PATH_LEN;
    while (--n && *src) *path++ = *src++;
    *path = 0;
}

static void job_error(const char *what, int e)
{
    job.errors++;
    str_ncopy(job.message, what, 40);
    str_cat(job.message, ": ");
    str_cat(job.message, dos_error_text(e));
}
/* A same-folder copy keeps a readable long name and its extension. */
static void unique_name(char *dst_path, const char *dir, const char *name)
{
    char t[LFN_NAME], n[6], suffix[20];
    int i, k, e = -1, limit, extlen;
    for (i = 1; name[i]; i++) if (name[i] == '.') e = i;
    extlen = e < 0 ? 0 : str_len(name + e);
    for (i = 1; i < 100; i++) {
        str_copy(suffix, " - Copy");
        if (i > 1) {
            fmt_u32(n, (u32)i);
            str_cat(suffix, " ("); str_cat(suffix, n); str_cat(suffix, ")");
        }
        limit = LFN_NAME - 1 - str_len(suffix) - extlen;
        if (limit > PATH_LEN - str_len(dir) - 2 - str_len(suffix) - extlen)
            limit = PATH_LEN - str_len(dir) - 2 - str_len(suffix) - extlen;
        if (limit < 1) break;
        for (k = 0; k < limit && name[k] && (e < 0 || k < e); k++) t[k] = name[k];
        t[k] = 0;
        str_cat(t, suffix);
        if (e >= 0) str_cat(t, name + e);
        path_join(dst_path, dir, t);
        if (dos_get_attr(dst_path) < 0) return;
    }
    dst_path[0] = 0;
}
static void job_close_files(void)
{
    if (job.src_h > 0) dos_close(job.src_h);
    if (job.dst_h > 0) dos_close(job.dst_h);
    job.src_h = job.dst_h = 0;
}
static void job_finish(void)
{
    char t[16];
    int i;
    if (dos_lfn()) for (i = 0; i < job.depth; i++) if (lv[i].started > 0) {
        struct dir_ent e;
        e.h = lv[i].started;
        dir_close(&e);
        lv[i].started = 0;
    }
    job_close_files();
    if (jbuf) { dos_free(jbuf); jbuf = 0; }
    if (jsrc_seg) { dos_free(jsrc_seg); jsrc_seg = 0; }
    job.active = 0;
    if (job.kind == JOB_MOVE && job.from_clip && !job.errors) { clip_clear(); nclip = 0; }  /* a cut is used once */
    if (job.errors) app_sound(5);
    else if (!job.cancel) {
        fmt_u32(t, (u32)(job.files + job.folders));
        str_copy(status_text, t);
        str_cat(status_text, job.kind == JOB_DELETE ? " item(s) deleted." :
                job.kind == JOB_MOVE ? " item(s) moved." : " item(s) copied.");
    }
    if (job.errors) str_copy(status_text, job.message);
    if (job.cancel) str_copy(status_text, "Cancelled.");
    app_log("[FILES] job", status_text);
    refresh();
}
/* Start copying one file (src -> dst), keeping its date and attributes. */
static int file_begin(const char *src, const char *dst)
{
    struct dir_ent f;
    int r;
    str_copy(job.file_src, src);
    str_copy(job.file_dst, dst);
    str_copy(job.current, base_name(src));
    r = dir_first(src, A_HIDDEN | A_SYSTEM | A_RDONLY | A_ARCH, &f);
    if (r < 0) { job_error(base_name(src), r); return 0; }
    dir_close(&f);
    job.fdate = f.date; job.ftime = f.time; job.fattr = f.attr;
    job.src_h = dos_open(src, 0);
    if (job.src_h < 0) { r = job.src_h; job.src_h = 0; job_error(base_name(src), r); return 0; }
    job.dst_h = dos_create(dst);
    if (job.dst_h < 0) { r = job.dst_h; job.dst_h = 0; job_error(base_name(dst), r); job_close_files(); return 0; }
    return 1;
}
/* One chunk; 1 when the file is complete (or failed). */
static int file_step(void)
{
    int n = dos_read_far(job.src_h, jbuf, 0, JBUF_BYTES), w;
    if (n < 0) { job_error(job.current, n); goto failed; }
    if (n > 0) {
        w = dos_write_far(job.dst_h, jbuf, 0, n);
        if (w < n) { job_error(job.current, w < 0 ? w : -112); goto failed; }
        job.bytes += n;
        return 0;
    }
    dos_set_file_time(job.dst_h, job.ftime, job.fdate);   /* the source's date */
    job_close_files();
    dos_set_attr(job.file_dst, job.fattr & (A_RDONLY | A_HIDDEN | A_SYSTEM | A_ARCH));
    job.files++;
    if (job.kind == JOB_MOVE) {
        dos_set_attr(job.file_src, 0);
        dos_delete(job.file_src);
    }
    return 1;
failed:
    job_close_files();
    dos_delete(job.file_dst);
    return 1;
}
static int is_dot(const char *n) { return n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])); }

/* Import one file from removable media device dev (MEDIA.DRV copies it). */
static void media_import(int dev, const char *src, const char *dst_dir)
{
    char dst[PATH_LEN];
    str_ncopy(mreq.path, src, sizeof mreq.path);
    local_join(dst, dst_dir, base_name(src));
    upper(dst);
    str_ncopy(mreq.destination, dst, sizeof mreq.destination);
    if (!media_call_dev(MD_IMPORT_FILE, dev)) { job.errors++; str_ncopy(job.message, mreq.message, sizeof job.message); }
    else job.files++;
}

/* One unit of work; the job ends when every source is done. */
static void job_step(void)
{
    struct level *l;
    struct dir_ent e;
    const char *entry_name;
    u8 entry_attr;
    int r;
    if (!job.active || job.ask) return;
    if (job.cancel) {
        if (job.src_h) { job_close_files(); dos_delete(job.file_dst); }
        job_finish();
        return;
    }
    if (job.src_h) { file_step(); return; }
    if (job.depth > 0) {
        l = &lv[job.depth - 1];
        if (dos_lfn()) {
            if (l->started) {
                e.h = l->started;
                r = dir_next(&e);
            } else {
                char pat[PATH_LEN + 6];
                path_join(pat, l->src, "*.*");
                r = dir_first(pat, A_DIR | A_HIDDEN | A_SYSTEM | A_RDONLY | A_ARCH, &e);
            }
            l->started = e.h;
            entry_name = e.name;
            entry_attr = e.attr;
        } else {
            dos_set_dta(&l->dta);
            r = l->started ? dos_find_next() : 0;
            if (!l->started) {
                char pat[PATH_LEN + 6];
                path_join(pat, l->src, "*.*");
                r = dos_find_first(pat, A_DIR | A_HIDDEN | A_SYSTEM | A_RDONLY | A_ARCH);
                l->started = 1;
            }
            entry_name = l->dta.name;
            entry_attr = l->dta.attr;
        }
        if (r >= 0 && (is_dot(entry_name) || (entry_attr & A_VOLUME))) return;
        if (r < 0) {                          /* this folder is done */
            job.depth--;
            if (job.kind != JOB_COPY) {
                r = dos_rmdir(l->src);
                if (r < 0) job_error(base_name(l->src), r);
            }
            job.folders++;
            return;
        }
        {
            char s[PATH_LEN], d[PATH_LEN];
            local_join(s, l->src, entry_name);
            if (job.kind != JOB_DELETE) local_join(d, l->dst, entry_name);
            str_copy(job.current, entry_name);
            if (entry_attr & A_DIR) {
                if (job.depth >= 8) { job_error(entry_name, -3); return; }
                if (job.kind != JOB_DELETE) {
                    r = dos_mkdir(d);
                    if (r < 0 && dos_get_attr(d) < 0) { job_error(entry_name, r); return; }
                }
                l = &lv[job.depth++];
                str_copy(l->src, s);
                if (job.kind != JOB_DELETE) str_copy(l->dst, d);
                l->started = 0;
                return;
            }
            if (job.kind == JOB_DELETE) {
                dos_set_attr(s, 0);
                r = dos_delete(s);
                if (r < 0) job_error(entry_name, r); else job.files++;
                return;
            }
            file_begin(s, d);
        }
        return;
    }
    /* The next top-level source. */
    if (job.index >= njob) { job_finish(); return; }
    {
        char source[PATH_LEN];
        const char *src = source;
        char dst[PATH_LEN];
        int attr;
        job_source_get(job.index, source);
        if (job.kind == JOB_DELETE) {
            attr = dos_get_attr(src);
            str_copy(job.current, base_name(src));
            if (attr >= 0 && (attr & A_DIR)) {
                l = &lv[job.depth++];
                str_copy(l->src, src);
                l->started = 0;
            } else {
                dos_set_attr(src, 0);
                r = dos_delete(src);
                if (r < 0) job_error(base_name(src), r); else job.files++;
            }
            job.index++;
            return;
        }
        if (jmedia[job.index]) {
            str_copy(job.current, base_name(src));
            media_import(jmedia[job.index], src, job.dest_dir);
            job.index++;
            return;
        }
        local_join(dst, job.dest_dir, base_name(src));
        attr = dos_get_attr(src);
        if (attr < 0) { job_error(base_name(src), attr); job.index++; return; }
        if (!str_icmp(src, dst)) {
            if (job.kind == JOB_MOVE) { job.index++; return; }    /* already here */
            unique_name(dst, job.dest_dir, base_name(src));
            if (!dst[0]) {
                str_copy(job.message, "There is no room for a copy name in this path.");
                job.errors++; job.index++; return;
            }
        } else if (dos_get_attr(dst) >= 0 && !job.replace_all) {
            if (job.ask == 0 && job.skipped != -1) {
                job.ask = 1;                                      /* the UI asks */
                str_copy(job.file_dst, dst);
                str_copy(job.current, base_name(src));
                return;
            }
        }
        job.skipped = 0;
        if (attr & A_DIR) {
            /* A folder into itself would never end. */
            int n = str_len(src);
            if (!str_nicmp(job.dest_dir, src, n) && (job.dest_dir[n] == '\\' || !job.dest_dir[n])) {
                str_copy(job.message, "A folder cannot be copied into itself.");
                job.errors++;
                job.index++;
                return;
            }
            if (job.kind == JOB_MOVE && dos_rename(src, dst) >= 0) { job.folders++; job.index++; return; }
            r = dos_mkdir(dst);
            if (r < 0 && dos_get_attr(dst) < 0) { job_error(base_name(src), r); job.index++; return; }
            l = &lv[job.depth++];
            str_copy(l->src, src);
            str_copy(l->dst, dst);
            l->started = 0;
            job.index++;
            return;
        }
        if (job.kind == JOB_MOVE) {
            if (dos_get_attr(dst) >= 0) { dos_set_attr(dst, 0); dos_delete(dst); }
            if (dos_rename(src, dst) >= 0) { job.files++; job.index++; return; }
        }
        job.index++;
        dos_set_attr(dst, 0);                       /* a replaced read-only file */
        file_begin(src, dst);
    }
}
static void job_start(int kind, const char *dest)
{
    mem_set(&job, 0, sizeof job);
    job.kind = kind;
    job.active = 1;
    if (!jbuf) jbuf = dos_alloc(JBUF_BYTES / 16);
    if (!jsrc_seg) jsrc_seg = dos_alloc((CLIP_MAX * PATH_LEN + 15) / 16);
    if (!jbuf || !jsrc_seg) {
        if (jbuf) { dos_free(jbuf); jbuf = 0; }
        if (jsrc_seg) { dos_free(jsrc_seg); jsrc_seg = 0; }
        job.active = 0;
        str_copy(status_text, dos_error_text(-8));
        return;
    }
    if (dest) str_copy(job.dest_dir, dest);
}

/* Selected items as the clipboard (or the delete list). */
static void selection_to_clip(int cut)
{
    char p[PATH_LEN];
    int i;
    nclip = 0;
    clip_begin(cut);
    for (i = 0; i < nitems && nclip < CLIP_MAX; i++) {
        if (!items[i].sel && !(i == cur && !count_selected())) continue;
        path_join(p, cwd, item_name(&items[i]));
        clip_add(p, media);
        nclip++;
    }
    clip_end();
    clip_cut = cut;
}

/* ------------------------------------------------------------------ */
/* Menus                                                               */
enum {
    C_NEWFOLDER = 1, C_NEWTEXT, C_OPEN, C_OPEN_NOTEPAD, C_OPEN_EDIT, C_DELETE, C_RENAME,
    C_PROPERTIES, C_CLOSE, C_CUT, C_COPY, C_PASTE, C_SELALL, C_INVERT, C_ICONS, C_LIST,
    C_DETAILS, C_SORT_NAME, C_SORT_SIZE, C_SORT_TYPE, C_SORT_DATE, C_HIDDEN, C_REFRESH,
    C_BACK, C_FORWARD, C_UP, C_GO_C, C_GO_APPS, C_GO_SYSTEM, C_GO_FLOPPY, C_GO_USB, C_GO_CD,
    C_HELP, C_ABOUT, C_GO_DESKTOP, C_GO_BIN, C_TO_DESKTOP, C_DELETE_NOW
};
static struct menu_item m_file[] = {
    { "New &Folder", "Ctrl+Shift+N", C_NEWFOLDER, 0 }, { "New &Text Document", 0, C_NEWTEXT, 0 },
    { "", 0, 0, MI_SEP }, { "&Open", "Enter", C_OPEN, 0 }, { "Open with Ciuk&Note", 0, C_OPEN_NOTEPAD, 0 },
    { "", 0, 0, MI_SEP }, { "&Delete", "Del", C_DELETE, 0 }, { "Rena&me", "F2", C_RENAME, 0 },
    { "P&roperties", "Alt+Enter", C_PROPERTIES, 0 }, { "", 0, 0, MI_SEP }, { "&Close", "Alt+F4", C_CLOSE, 0 } };
static struct menu_item m_edit[] = {
    { "Cu&t", "Ctrl+X", C_CUT, 0 }, { "&Copy", "Ctrl+C", C_COPY, 0 }, { "&Paste", "Ctrl+V", C_PASTE, 0 },
    { "", 0, 0, MI_SEP }, { "Select &All", "Ctrl+A", C_SELALL, 0 }, { "&Invert Selection", 0, C_INVERT, 0 } };
static struct menu_item m_view[] = {
    { "Lar&ge Icons", 0, C_ICONS, 0 }, { "&List", 0, C_LIST, 0 }, { "&Details", 0, C_DETAILS, 0 },
    { "", 0, 0, MI_SEP }, { "Sort by &Name", 0, C_SORT_NAME, 0 }, { "Sort by &Size", 0, C_SORT_SIZE, 0 },
    { "Sort by T&ype", 0, C_SORT_TYPE, 0 }, { "Sort by Dat&e", 0, C_SORT_DATE, 0 },
    { "", 0, 0, MI_SEP }, { "&Hidden Files", 0, C_HIDDEN, 0 }, { "", 0, 0, MI_SEP },
    { "&Refresh", "F5", C_REFRESH, 0 } };
static struct menu_item m_go[] = {
    { "&Back", "Alt+Left", C_BACK, 0 }, { "&Forward", "Alt+Right", C_FORWARD, 0 },
    { "&Up One Level", "Backspace", C_UP, 0 }, { "", 0, 0, MI_SEP },
    { "&Local Disk", 0, C_GO_C, 0 }, { "&Applications", 0, C_GO_APPS, 0 }, { "&System", 0, C_GO_SYSTEM, 0 },
    { "", 0, 0, MI_SEP }, { "Flo&ppy", 0, C_GO_FLOPPY, 0 }, { "USB &Drive", 0, C_GO_USB, 0 },
    { "&CD-ROM", 0, C_GO_CD, 0 }, { "", 0, 0, MI_SEP }, { "D&esktop", 0, C_GO_DESKTOP, 0 },
    { "&Recycle Bin", 0, C_GO_BIN, 0 } };
static struct menu_item m_help[] = {
    { "&Keyboard Shortcuts", "F1", C_HELP, 0 }, { "", 0, 0, MI_SEP }, { "&About Files", 0, C_ABOUT, 0 } };
static struct menu menus[] = {
    { "&File", m_file, 11 }, { "&Edit", m_edit, 6 }, { "&View", m_view, 12 },
    { "&Go", m_go, 14 }, { "&Help", m_help, 3 } };
static struct menubar bar = { menus, 5, 0, 0, 0, -1, 0 };
static struct menu_item m_item[] = {
    { "&Open", 0, C_OPEN, 0 }, { "Open with Ciuk&Note", 0, C_OPEN_NOTEPAD, 0 },
    { "&Edit (DOS Editor)", 0, C_OPEN_EDIT, 0 }, { "", 0, 0, MI_SEP },
    { "Cu&t", 0, C_CUT, 0 }, { "&Copy", 0, C_COPY, 0 }, { "&Paste", 0, C_PASTE, 0 },
    { "Copy to Des&ktop", 0, C_TO_DESKTOP, 0 }, { "", 0, 0, MI_SEP },
    { "&Delete", 0, C_DELETE, 0 }, { "Rena&me", 0, C_RENAME, 0 },
    { "", 0, 0, MI_SEP }, { "P&roperties", 0, C_PROPERTIES, 0 } };
#define ITEM_MENU_COUNT 13
static struct menu_item m_back[] = {
    { "Lar&ge Icons", 0, C_ICONS, 0 }, { "&List", 0, C_LIST, 0 }, { "&Details", 0, C_DETAILS, 0 },
    { "", 0, 0, MI_SEP }, { "Sort by &Name", 0, C_SORT_NAME, 0 }, { "Sort by &Size", 0, C_SORT_SIZE, 0 },
    { "Sort by T&ype", 0, C_SORT_TYPE, 0 }, { "Sort by Dat&e", 0, C_SORT_DATE, 0 },
    { "", 0, 0, MI_SEP }, { "&Refresh", 0, C_REFRESH, 0 }, { "&Paste", 0, C_PASTE, 0 },
    { "", 0, 0, MI_SEP }, { "New &Folder", 0, C_NEWFOLDER, 0 }, { "New &Text Document", 0, C_NEWTEXT, 0 },
    { "", 0, 0, MI_SEP }, { "Pr&operties", 0, C_PROPERTIES, 0 } };
static struct popup ctx;

static void flag(struct menu_item *it, int f, int on) { if (on) it->flags |= f; else it->flags &= ~f; }
static void update_menus(void)
{
    int any = cur >= 0 && nitems > 0, ro = media != 0;
    int is_dir = any && (items[cur].attr & A_DIR);
    flag(&m_file[0], MI_DISABLED, ro); flag(&m_file[1], MI_DISABLED, ro);
    flag(&m_file[3], MI_DISABLED, !any); flag(&m_file[4], MI_DISABLED, !any || is_dir);
    flag(&m_file[6], MI_DISABLED, !any || ro); flag(&m_file[7], MI_DISABLED, !any || ro);
    flag(&m_edit[0], MI_DISABLED, !any || ro); flag(&m_edit[1], MI_DISABLED, !any);
    flag(&m_edit[2], MI_DISABLED, !nclip || ro);
    flag(&m_view[0], MI_CHECKED, view == 1); flag(&m_view[1], MI_CHECKED, view == 2);
    flag(&m_view[2], MI_CHECKED, view == 0);
    flag(&m_view[4], MI_CHECKED, sort_col == 0); flag(&m_view[5], MI_CHECKED, sort_col == 1);
    flag(&m_view[6], MI_CHECKED, sort_col == 2); flag(&m_view[7], MI_CHECKED, sort_col == 3);
    flag(&m_view[9], MI_CHECKED, show_hidden);
    flag(&m_go[0], MI_DISABLED, !nback); flag(&m_go[1], MI_DISABLED, !nfwd);
    mem_copy(&m_back[0].flags, &m_view[0].flags, sizeof(int));
    flag(&m_back[0], MI_CHECKED, view == 1); flag(&m_back[1], MI_CHECKED, view == 2);
    flag(&m_back[2], MI_CHECKED, view == 0);
    flag(&m_back[4], MI_CHECKED, sort_col == 0); flag(&m_back[5], MI_CHECKED, sort_col == 1);
    flag(&m_back[6], MI_CHECKED, sort_col == 2); flag(&m_back[7], MI_CHECKED, sort_col == 3);
    flag(&m_back[10], MI_DISABLED, !nclip || ro);
    flag(&m_back[12], MI_DISABLED, ro); flag(&m_back[13], MI_DISABLED, ro);
    flag(&m_item[1], MI_DISABLED, is_dir || ro); flag(&m_item[2], MI_DISABLED, is_dir || ro);
    flag(&m_item[4], MI_DISABLED, ro); flag(&m_item[6], MI_DISABLED, !nclip || !is_dir || ro);
    flag(&m_item[9], MI_DISABLED, ro); flag(&m_item[10], MI_DISABLED, ro);
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
#define ROW_H 18
#define CELL_W 92
#define CELL_H 72
#define LIST_COL_W 170
static int X0, Y0, W, H, tb_y, addr_y, body_y, body_h, places_w, lx, ly, lw, lh;
static void layout(void)
{
    X0 = HOST.x + 3; Y0 = HOST.y + TITLE_H; W = HOST.w - 6; H = HOST.h - TITLE_H - 4;
    tb_y = Y0 + MENUBAR_H + 2;
    addr_y = tb_y + 32;
    body_y = addr_y + 28;
    body_h = Y0 + H - 22 - body_y;
    places_w = W >= 560 ? 160 : 0;
    lx = X0 + places_w + 2;
    ly = body_y;
    lw = W - places_w - 4 - 16;
    lh = body_h;
}
static int col_x(int percent) { return lx + (int)((long)lw * percent / 100); }
static int details_rows(void) { return (lh - 22) / ROW_H; }
static int icon_cols(void) { int c = lw / CELL_W; return c < 1 ? 1 : c; }
static int icon_rows(void) { return lh / CELL_H; }
static int list_rows(void) { int r = (lh - 4) / ROW_H; return r < 1 ? 1 : r; }
static int list_cols(void) { int c = lw / LIST_COL_W; return c < 1 ? 1 : c; }
/* Scroll unit: rows (details, icons) or columns (list). */
static int page_units(void) { return view == 0 ? details_rows() : view == 1 ? icon_rows() : list_cols(); }
static int unit_of(int i) { return view == 0 ? i : view == 1 ? i / icon_cols() : i / list_rows(); }
static int total_units(void) { return nitems ? unit_of(nitems - 1) + 1 : 0; }
static void ensure_visible(int i)
{
    int u, p;
    if (i < 0) return;
    u = unit_of(i);
    p = page_units();
    if (p < 1) p = 1;
    if (u < top_row) top_row = u;
    if (u >= top_row + p) top_row = u - p + 1;
    if (top_row < 0) top_row = 0;
}
static void item_rect(int i, int *x, int *y, int *w, int *h)
{
    if (view == 0) { *x = lx + 2; *y = ly + 22 + (i - top_row) * ROW_H; *w = lw - 4; *h = ROW_H; }
    else if (view == 1) {
        int c = icon_cols();
        *x = lx + 4 + (i % c) * CELL_W; *y = ly + 4 + (i / c - top_row) * CELL_H; *w = CELL_W - 4; *h = CELL_H - 4;
    } else {
        int r = list_rows();
        *x = lx + 4 + (i / r - top_row) * LIST_COL_W; *y = ly + 2 + (i % r) * ROW_H; *w = LIST_COL_W - 8; *h = ROW_H;
    }
}
static int item_at(int sx, int sy)
{
    int i, x, y, w, h;
    for (i = 0; i < nitems; i++) {
        int u = unit_of(i);
        if (u < top_row || u >= top_row + page_units()) continue;
        item_rect(i, &x, &y, &w, &h);
        if (sx >= x && sx < x + w && sy >= y && sy < y + h) {
            if (view == 0 && sx > x + 4 + 22 + ui_measure(item_name(&items[i])) + 8 && sx < x + (w * 45) / 100)
                return i;               /* the whole name column counts */
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Dialogs                                                             */
enum { D_NONE, D_MSG, D_DELETE, D_REPLACE, D_PROGRESS, D_PROPS, D_ABOUT, D_HELP, D_PREVIEW };
static struct dialog dlg;
static int dlg_kind;
static struct dctl dc[20];
static char dl[10][72];
static void ctl(int i, int type, int x, int y, int w, int h, const char *text, int id)
{
    mem_set(&dc[i], 0, sizeof dc[i]);
    dc[i].type = type; dc[i].x = x; dc[i].y = y; dc[i].w = w; dc[i].h = h;
    dc[i].text = text; dc[i].id = id;
}
static void message(const char *title, const char *text)
{
    msgbox(&dlg, title, text, "OK");
    dlg_kind = D_MSG;
    app_log("[FILES] message", text);
}
static char msg[320];

static void progress_dialog(void)
{
    const char *t = job.kind == JOB_DELETE ? "Deleting..." : job.kind == JOB_MOVE ? "Moving..." : "Copying...";
    ctl(0, DC_LABEL, 0, 0, 0, 16, dl[0], 0);
    ctl(1, DC_LABEL, 0, 20, 0, 16, dl[1], 0);
    ctl(2, DC_LABEL, 0, 70, 0, 16, dl[2], 0);
    ctl(3, DC_BUTTON, 290, 66, 80, 24, "Cancel", 1);
    dialog_show(&dlg, t, dc, 4, 390, 96, 1, 1);
    dlg_kind = D_PROGRESS;
}
static void progress_update(void)
{
    char n[16];
    str_copy(dl[0], job.kind == JOB_DELETE ? "Deleting " : "Copying ");
    str_ncopy(dl[0] + str_len(dl[0]), job.current, 40);
    str_copy(dl[1], job.kind == JOB_DELETE ? "" : "To ");
    if (job.kind != JOB_DELETE) str_ncopy(dl[1] + 3, job.dest_dir, 60);
    fmt_u32(n, (u32)(job.files)); str_copy(dl[2], n); str_cat(dl[2], " files, ");
    fmt_size(n, job.bytes); str_cat(dl[2], n);
}

static void replace_dialog(void)
{
    str_copy(msg, "This folder already contains a file named '");
    str_cat(msg, job.current);
    str_cat(msg, "'.\nWould you like to replace the existing file?");
    msgbox(&dlg, "Confirm File Replace", msg, "Yes|Yes to All|No|Cancel");
    dlg_kind = D_REPLACE;
}

/* Properties of the selection (one item: its details and attributes). */
static u32 prop_size;
static int prop_files, prop_dirs, prop_index = -1;
static int big_icon_id(const struct item *it);
/* Properties: a window of its own, modeless (as in Explorer). */
static struct dialog pdlg;
static struct dctl pdc[14];
static char pdl[7][72], ptitle[40], prop_path[PATH_LEN];
static int prop_icon = -1, prop_attr_ok;
static void pctl(int i, int type, int x, int y, int w, int h, const char *text, int id)
{
    mem_set(&pdc[i], 0, sizeof pdc[i]);
    pdc[i].type = type; pdc[i].x = x; pdc[i].y = y; pdc[i].w = w; pdc[i].h = h;
    pdc[i].text = text; pdc[i].id = id;
}
static void properties_dialog(void)
{
    int i, n = count_selected(), y = 0, k = 0, attr = 0;
    char t[24], tb[20];
    prop_size = 0; prop_files = prop_dirs = 0; prop_index = -1; prop_icon = ICON_FOLDER;
    if (!n && cur >= 0) { n = 1; prop_index = cur; }
    if (n == 1 && prop_index < 0) for (i = 0; i < nitems; i++) if (items[i].sel) prop_index = i;
    prop_path[0] = 0;
    if (n == 0) {                                   /* the folder itself */
        str_ncopy(pdl[0], media ? media_name(media) : cwd, sizeof pdl[0]);
        for (i = 0; i < nitems; i++) {
            if (items[i].attr & A_DIR) prop_dirs++; else { prop_files++; prop_size += items[i].size; }
        }
        str_copy(pdl[1], "Type: File Folder");
    } else if (n == 1) {
        struct item *it = &items[prop_index];
        str_ncopy(pdl[0], item_name(it), sizeof pdl[0]);
        str_copy(pdl[1], "Type: ");
        str_cat(pdl[1], type_name(it, tb));
        prop_icon = big_icon_id(it) >= 0 ? big_icon_id(it) : ICON_TEXT;
        attr = it->attr;
        if (!media) local_join(prop_path, cwd, item_name(it));
        if ((it->attr & A_DIR) && !media) tree_measure(prop_path, &prop_files, &prop_dirs, &prop_size);
        else prop_size = it->size;
    } else {
        fmt_u32(t, (u32)n); str_copy(pdl[0], t); str_cat(pdl[0], " items selected");
        for (i = 0; i < nitems; i++) if (items[i].sel) {
            if (items[i].attr & A_DIR) prop_dirs++; else { prop_files++; prop_size += items[i].size; }
        }
        str_copy(pdl[1], "Type: Multiple types");
    }
    str_copy(pdl[2], "Location: ");
    str_ncopy(pdl[2] + 10, media ? media_name(media) : cwd, 60);
    str_copy(pdl[3], "Size: ");
    fmt_size(t, prop_size); str_cat(pdl[3], t); str_cat(pdl[3], " (");
    fmt_u32_group(t, prop_size); str_cat(pdl[3], t); str_cat(pdl[3], " bytes)");
    str_copy(pdl[4], "Contains: ");
    fmt_u32(t, (u32)prop_files); str_cat(pdl[4], t); str_cat(pdl[4], " files, ");
    fmt_u32(t, (u32)prop_dirs); str_cat(pdl[4], t); str_cat(pdl[4], " folders");
    pdl[5][0] = 0;
    if (n == 1 && items[prop_index].date) {
        str_copy(pdl[5], "Modified: ");
        fmt_date(t, items[prop_index].date); str_cat(pdl[5], t); str_cat(pdl[5], "  ");
        fmt_time(t, items[prop_index].time); str_cat(pdl[5], t);
    }
    pctl(k++, DC_LABEL, 52, y + 8, 0, 16, pdl[0], 0); y += 44;
    pctl(k++, DC_LABEL, 0, y, 0, 16, pdl[1], 0); y += 22;
    pctl(k++, DC_LABEL, 0, y, 0, 16, pdl[2], 0); y += 22;
    pctl(k++, DC_LABEL, 0, y, 0, 16, pdl[3], 0); y += 22;
    pctl(k++, DC_LABEL, 0, y, 0, 16, pdl[4], 0); y += 22;
    if (pdl[5][0]) { pctl(k++, DC_LABEL, 0, y, 0, 16, pdl[5], 0); y += 22; }
    y += 8;
    prop_attr_ok = n == 1 && !media;
    pctl(k++, DC_GROUP, 0, y, 364, 50, "Attributes", 0);
    pctl(k, DC_CHECK, 12, y + 22, 0, 17, "&Read-only", 0);
    pdc[k].value = (attr & A_RDONLY) != 0; pdc[k].disabled = !prop_attr_ok; k++;
    pctl(k, DC_CHECK, 132, y + 22, 0, 17, "&Hidden", 0);
    pdc[k].value = (attr & A_HIDDEN) != 0; pdc[k].disabled = !prop_attr_ok; k++;
    pctl(k, DC_CHECK, 242, y + 22, 0, 17, "&Archive", 0);
    pdc[k].value = (attr & A_ARCH) != 0; pdc[k].disabled = !prop_attr_ok; k++;
    y += 64;
    pctl(k++, DC_BUTTON, 104, y, 84, 26, "OK", 1);
    pctl(k++, DC_BUTTON, 192, y, 84, 26, "Cancel", 2);
    pctl(k, DC_BUTTON, 280, y, 84, 26, "&Apply", 3); pdc[k].disabled = !prop_attr_ok; k++;
    str_ncopy(ptitle, n == 1 ? item_name(&items[prop_index]) : "Selection", 14);
    str_cat(ptitle, " Properties");
    app_log("[FILES] properties", pdl[3]);
    if (pdlg.open) { pdlg.open = 0; dialog_sync(&pdlg); }
    dialog_show(&pdlg, ptitle, pdc, k, 380, y + 34, 1, 2);
    pdlg.modeless = 1;
    dialog_sync(&pdlg);
}
static void properties_apply(void)
{
    int k, attr, i;
    if (!prop_attr_ok || !prop_path[0]) return;
    for (k = 0; k < pdlg.n && pdc[k].type != DC_CHECK; k++) ;
    if (k + 2 >= pdlg.n) return;
    attr = dos_get_attr(prop_path);
    if (attr < 0) { message("Properties", dos_error_text(attr)); return; }
    attr &= A_SYSTEM;
    if (pdc[k].value) attr |= A_RDONLY;
    if (pdc[k + 1].value) attr |= A_HIDDEN;
    if (pdc[k + 2].value) attr |= A_ARCH;
    i = dos_set_attr(prop_path, attr);
    if (i < 0) message("Properties", dos_error_text(i));
    app_log("[FILES] attributes", prop_path);
    refresh();
}
/* The Properties window's own events. */
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
    if (r == 3) pdlg.open = 1;                       /* Apply keeps it open */
    dialog_sync(&pdlg);
    return 1;
}

static const char *help_text[] = {
    "Enter  open        Backspace  up one level   F5  refresh",
    "F2  rename         Del  delete               Alt+Enter  properties",
    "Ctrl+C  copy       Ctrl+X  cut               Ctrl+V  paste",
    "Ctrl+A  select all          Ctrl+Shift+N  new folder",
    "Alt+Left/Right  back/forward     Alt+D or F4  address bar",
    "Shift+click, Shift+arrows: range; Ctrl+click: add or remove.",
    "Right-click (or the Menu key) opens the context menu.",
    "Removable media are read-only: copy files, paste on C:."
};
static void help_dialog(void)
{
    int i;
    for (i = 0; i < 8; i++) ctl(i, DC_LABEL, 0, i * 18, 0, 16, help_text[i], 0);
    ctl(8, DC_BUTTON, 200, 8 * 18 + 10, 80, 24, "OK", 1);
    dialog_show(&dlg, "Files - Keyboard Shortcuts", dc, 9, 480, 8 * 18 + 40, 1, 1);
    dlg_kind = D_HELP;
}
static void about_dialog(void)
{
    ctl(0, DC_LABEL, 50, 0, 0, 16, "CiukiOS Files", 0);
    ctl(1, DC_LABEL, 50, 20, 0, 16, "Version 0.8.0", 0);
    ctl(2, DC_LABEL, 50, 40, 0, 16, "A modern Retro OS", 0);
    ctl(3, DC_BUTTON, 130, 70, 80, 24, "OK", 1);
    dialog_show(&dlg, "About Files", dc, 4, 340, 100, 1, 1);
    dlg_kind = D_ABOUT;
}
static void preview_dialog(const char *name)
{
    int line = 0, k = 0, i;
    for (i = 0; line < 10 && i < mreq.preview_bytes; i++) {
        char c = mreq.preview[i];
        if (c == '\r') continue;
        if (c == '\n' || k >= 70) { dl[line][k] = 0; line++; k = 0; if (c == '\n') continue; }
        if (line >= 10) break;
        dl[line][k++] = c == '\t' ? ' ' : c;
    }
    if (line < 10) dl[line++][k] = 0;
    for (i = 0; i < line; i++) ctl(i, DC_LABEL, 0, i * 18, 0, 16, dl[i], 0);
    ctl(line, DC_BUTTON, 230, line * 18 + 10, 80, 24, "OK", 1);
    str_copy(msg, "Preview - ");
    str_cat(msg, name);
    app_log("[FILES] preview", name);
    dialog_show(&dlg, msg, dc, line + 1, 540, line * 18 + 40, 1, 1);
    dlg_kind = D_PREVIEW;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
static void start_rename(int i)
{
    if (i < 0 || i >= nitems || media) return;
    renaming = i;
    field_set(&rename_field, rename_buf, LFN_NAME, item_name(&items[i]));
    rename_field.sel = 1;
    focus_area = 0;
}
static void commit_rename(void)
{
    char from[PATH_LEN], to[PATH_LEN], name[LFN_NAME];
    int r, i = renaming;
    renaming = -1;
    if (i < 0) return;
    str_ncopy(name, rename_field.text, LFN_NAME);
    if (!str_cmp(name, item_name(&items[i]))) return;
    if (!valid_file_name(name) || str_len(cwd) + str_len(name) + 2 > PATH_LEN) {
        message("Rename", "Invalid file name or path too long.");
        return;
    }
    local_join(from, cwd, item_name(&items[i]));
    local_join(to, cwd, name);
    if (dos_get_attr(to) >= 0) { message("Rename", "A file or folder with that name already exists."); return; }
    r = dos_rename(from, to);
    if (r < 0) { message("Rename", dos_error_text(r)); return; }
    item_set_name(&items[i], name);
    app_log("[FILES] renamed", name);
    cur = i;
    refresh();
    for (i = 0; i < nitems; i++) if (!str_cmp(item_name(&items[i]), name)) { select_only(i); ensure_visible(i); }
}
static void create_new(int folder)
{
    char name[LFN_NAME], path[PATH_LEN], n[4];
    int i, r;
    if (media) return;
    for (i = 0; i < 100; i++) {
        str_copy(name, folder ? "New Folder" : "New Text Document");
        if (i) {
            fmt_u32(n, (u32)i);
            str_cat(name, " ("); str_cat(name, n); str_cat(name, ")");
        }
        if (!folder) str_cat(name, ".txt");
        if (str_len(cwd) + str_len(name) + 2 > PATH_LEN) {
            message(folder ? "New Folder" : "New Text Document", "The path is too long.");
            return;
        }
        local_join(path, cwd, name);
        if (dos_get_attr(path) < 0) break;
    }
    if (folder) r = dos_mkdir(path);
    else { r = dos_create_new(path); if (r >= 0) dos_close(r); }
    if (r < 0) { message(folder ? "New Folder" : "New Text Document", dos_error_text(r)); return; }
    app_log("[FILES] created", name);
    refresh();
    for (i = 0; i < nitems; i++) if (!str_cmp(item_name(&items[i]), name)) { select_only(i); ensure_visible(i); start_rename(i); }
}
static void open_item(int i, int how)       /* how: 0 default, 1 Notepad, 2 EDIT */
{
    char p[PATH_LEN], cmd[PATH_LEN + 12];
    struct item *it;
    if (i < 0 || i >= nitems) return;
    it = &items[i];
    if (it->attr & A_DIR) {
        path_join(p, cwd, item_name(it));
        navigate(p, media, 1);
        return;
    }
    if (media) {
        str_ncopy(mreq.path, cwd, sizeof mreq.path);
        path_join(mreq.path, cwd, item_name(it));
        if (media_call(MD_PREVIEW_FILE)) preview_dialog(item_name(it));
        else message("Files", mreq.message);
        str_ncopy(mreq.path, cwd, sizeof mreq.path);
        return;
    }
    local_join(p, cwd, item_name(it));
    /* Fonts open in the Control Panel's Fonts, driver packages in Devices. */
    if (how == 0 && ext_is(item_name(it), "CFN")) {
        str_copy(cmd, "font:"); str_cat(cmd, p); app_open(WIN_CONTROL, cmd); return;
    }
    if (how == 0 && ext_is(item_name(it), "BMP")) { app_log("[FILES] open", p); app_open(WIN_PAINT, p); return; }
    if (how == 0 && ext_is(item_name(it), "HTM|HTML")) { app_log("[FILES] open", p); app_open(WIN_BROWSER, p); return; }
    if (how == 0 && !str_icmp(item_name(it), "DRIVER.INF")) {
        str_copy(cmd, "install:"); str_cat(cmd, cwd); app_open(WIN_DEVICES, cmd); return;
    }
    if (how == 1 || (how == 0 && ext_is(item_name(it), TEXT_TYPES))) { app_log("[FILES] open", p); app_open(WIN_NOTEPAD, p); return; }
    if (how == 2) {
        str_copy(cmd, "EDIT ");
        str_cat(cmd, p);
        app_command(cmd);
        return;
    }
    if (ext_is(item_name(it), "COM|EXE")) {
        /* Programs often load data from their own folder. */
        dos_set_drive(to_upper(cwd[0]) - 'A');
        dos_chdir(cwd);
        app_log("[FILES] execute", p);
        app_open(WIN_DOS, p);
        return;
    }
    str_copy(msg, "No program is registered for this type of file.\nUse Open with CiukNote to view it as text.");
    message("Files", msg);
}
static int delete_for_good;               /* Shift+Del, or inside the bin */
static int in_bin(void) { return !media && !str_nicmp(cwd + 1, ":\\RECYCLED", 10) && (!cwd[11] || cwd[11] == '\\'); }
static void delete_selection(int permanent)
{
    int n = count_selected();
    char t[12];
    if (media || cur < 0) return;
    if (!n) { select_only(cur); n = 1; }
    delete_for_good = permanent || in_bin();
    if (n == 1) {
        int i;
        for (i = 0; i < nitems; i++) if (items[i].sel) break;
        str_copy(msg, delete_for_good ? "Are you sure you want to permanently delete '" :
                 "Are you sure you want to send '");
        str_cat(msg, item_name(&items[i]));
        if (delete_for_good) str_cat(msg, (items[i].attr & A_DIR) ? "'\nand everything in it?" : "'?");
        else str_cat(msg, "' to the Recycle Bin?");
    } else {
        fmt_u32(t, (u32)n);
        str_copy(msg, delete_for_good ? "Are you sure you want to permanently delete these " :
                 "Are you sure you want to send these ");
        str_cat(msg, t);
        str_cat(msg, delete_for_good ? " items?" : " items\nto the Recycle Bin?");
    }
    msgbox(&dlg, n == 1 ? (delete_for_good ? "Confirm File Delete" : "Confirm File Delete") :
           "Confirm Multiple File Delete", msg, "Yes|No");
    dlg_kind = D_DELETE;
    app_log("[FILES] confirm", msg);
}
/* The selection to the Recycle Bin (renames: no data is copied). */
static void recycle_selection(void)
{
    char p[PATH_LEN], n[8];
    int i, done = 0, r = 0;
    for (i = 0; i < nitems; i++) {
        if (!items[i].sel) continue;
        path_join(p, cwd, item_name(&items[i]));
        r = bin_send(p);
        if (r < 0) { str_copy(msg, item_name(&items[i])); str_cat(msg, ": "); str_cat(msg, dos_error_text(r)); break; }
        done++;
    }
    fmt_u32(n, (u32)done);
    app_log("[FILES] recycled", n);
    refresh();
    if (r < 0) { app_sound(5); message("Delete", msg); return; }
    str_copy(status_text, n);
    str_cat(status_text, " item(s) moved to the Recycle Bin.");
    ui_repaint_win(WIN_DESKTOP);
}
static void begin_job_from_selection(int kind, const char *dest)
{
    int i;
    job_start(kind, dest);
    if (!job.active) return;
    njob = 0;
    for (i = 0; i < nitems && njob < CLIP_MAX; i++) {
        char path[PATH_LEN];
        if (!items[i].sel) continue;
        path_join(path, cwd, item_name(&items[i]));
        job_source_put(njob, path);
        jmedia[njob++] = (u8)media;
    }
    progress_update();
    progress_dialog();
}
static void paste(const char *dest)
{
    int i;
    clip_refresh();
    if (!nclip || media) return;
    job_start(clip_cut ? JOB_MOVE : JOB_COPY, dest);
    if (!job.active) return;
    for (i = 0; i < nclip; i++) {
        char path[PATH_LEN];
        jmedia[i] = (u8)clip_get(i, path);
        job_source_put(i, path);
    }
    njob = nclip;
    job.from_clip = 1;
    if (clip_cut && jmedia[0]) job.kind = JOB_COPY;      /* media stay as they are */
    progress_update();
    progress_dialog();
}
static void command(int id)
{
    int i;
    switch (id) {
    case C_NEWFOLDER: create_new(1); break;
    case C_NEWTEXT: create_new(0); break;
    case C_OPEN: open_item(cur, 0); break;
    case C_OPEN_NOTEPAD: open_item(cur, 1); break;
    case C_OPEN_EDIT: open_item(cur, 2); break;
    case C_DELETE: delete_selection(HOST.shift & SH_SHIFT); break;
    case C_DELETE_NOW: delete_selection(1); break;
    case C_GO_DESKTOP: make_dirs("C:\\DESKTOP"); navigate("C:\\DESKTOP", 0, 1); break;
    case C_GO_BIN: app_open(WIN_RECYCLE, ""); break;
    case C_TO_DESKTOP:
        if (cur < 0 || make_dirs("C:\\DESKTOP") < 0) break;
        if (!count_selected()) select_only(cur);
        begin_job_from_selection(JOB_COPY, "C:\\DESKTOP");
        break;
    case C_RENAME: start_rename(cur); break;
    case C_PROPERTIES: properties_dialog(); break;
    case C_CLOSE: app_close(); break;
    case C_CUT: case C_COPY:
        if (cur < 0) break;
        if (!count_selected()) select_only(cur);
        selection_to_clip(id == C_CUT && !media);
        str_copy(status_text, clip_cut ? "Cut: paste to move." : "Copied: paste into a folder on C:.");
        break;
    case C_PASTE:
        if (cur >= 0 && (items[cur].attr & A_DIR) && items[cur].sel && count_selected() == 1 && ctx.open == 0) {
            /* Paste goes into the current folder (Explorer's Edit menu). */
        }
        paste(cwd);
        break;
    case C_SELALL: for (i = 0; i < nitems; i++) items[i].sel = 1; break;
    case C_INVERT: for (i = 0; i < nitems; i++) items[i].sel = !items[i].sel; break;
    case C_ICONS: view = 1; top_row = 0; ensure_visible(cur); break;
    case C_LIST: view = 2; top_row = 0; ensure_visible(cur); break;
    case C_DETAILS: view = 0; top_row = 0; ensure_visible(cur); break;
    case C_SORT_NAME: case C_SORT_SIZE: case C_SORT_TYPE: case C_SORT_DATE:
        sort_col = id - C_SORT_NAME; sort_desc = 0; sort_items(); break;
    case C_HIDDEN: show_hidden = !show_hidden; refresh(); break;
    case C_REFRESH: refresh(); break;
    case C_BACK: go_back(); break;
    case C_FORWARD: go_forward(); break;
    case C_UP: go_up(); break;
    case C_GO_C: { char p[4]; p[0] = cwd[0] && cwd[1] == ':' ? cwd[0] : 'C'; p[1] = ':'; p[2] = '\\'; p[3] = 0; navigate(p, 0, 1); } break;
    case C_GO_APPS: navigate("C:\\APPS", 0, 1); break;
    case C_GO_SYSTEM: navigate("C:\\SYSTEM", 0, 1); break;
    case C_GO_FLOPPY: navigate("/", 1, 1); break;
    case C_GO_USB: navigate("/", 2, 1); break;
    case C_GO_CD: navigate("/", 3, 1); break;
    case C_HELP: help_dialog(); break;
    case C_ABOUT: about_dialog(); break;
    }
    ensure_visible(cur);
}
static void context_paste_into(void)
{
    char p[PATH_LEN];
    local_join(p, cwd, item_name(&items[cur]));
    paste(p);
}

/* ------------------------------------------------------------------ */
/* Folders tree: the local disk, with the folders along the current path
 * opened (as Explorer shows it after navigating).                      */
#define TREE_MAX 40
#define TREE_ROW 18
struct tnode { char name[13]; u8 depth, parent, flags; };  /* 1 on the path, 2 current */
static struct tnode tree[TREE_MAX];
static int ntree, tree_top;
static int tree_insert_children(const char *dir, int pos, int depth, int parent, const char *want)
{
    char pat[PATH_LEN + 6], tmp[24][13];
    struct dos_find f;
    int r, n = 0, i, j;
    local_join(pat, dir, "*.*");
    dos_set_dta(&f);
    for (r = dos_find_first(pat, A_DIR | (show_hidden ? A_HIDDEN | A_SYSTEM : 0)); r >= 0; r = dos_find_next()) {
        if (!(f.attr & A_DIR) || is_dot(f.name)) continue;
        if (n == 24) {                          /* keep the one on the path */
            if (want && !str_icmp(f.name, want)) str_copy(tmp[23], f.name);
            continue;
        }
        str_copy(tmp[n++], f.name);
    }
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && str_icmp(tmp[j], tmp[j - 1]) < 0; j--) {
            char t[13];
            str_copy(t, tmp[j]); str_copy(tmp[j], tmp[j - 1]); str_copy(tmp[j - 1], t);
        }
    if (n > TREE_MAX - ntree) n = TREE_MAX - ntree;
    if (n <= 0) return 0;
    for (i = ntree - 1; i >= pos; i--) mem_copy(&tree[i + n], &tree[i], sizeof tree[0]);
    for (i = 0; i < n; i++) {
        str_copy(tree[pos + i].name, tmp[i]);
        tree[pos + i].depth = (u8)depth;
        tree[pos + i].parent = (u8)parent;
        tree[pos + i].flags = 0;
    }
    ntree += n;
    return n;
}
static void tree_path(int n, char *out)
{
    int chain[10], k = 0;
    while (n > 0 && k < 10) { chain[k++] = n; n = tree[n].parent; }
    out[0] = tree[0].name[0]; out[1] = ':'; out[2] = '\\'; out[3] = 0;
    while (k--) {
        char t[PATH_LEN];
        local_join(t, out, tree[chain[k]].name);
        str_copy(out, t);
    }
}
static void tree_build(void)
{
    char dir[PATH_LEN], shortcwd[PATH_LEN], part[13];
    const char *rest;
    int node = 0, depth = 1, k, n;
    ntree = 0;
    if (media || !cwd[0]) return;
    tree[0].name[0] = cwd[0]; tree[0].name[1] = ':'; tree[0].name[2] = 0;
    tree[0].depth = 0; tree[0].parent = 0; tree[0].flags = 1;
    ntree = 1;
    dir[0] = cwd[0]; dir[1] = ':'; dir[2] = '\\'; dir[3] = 0;
    /* The sidebar stores 8.3 component names. Resolve a long current path
     * to its stable aliases before matching those children. The main Files
     * view keeps and displays the complete long path. */
    if (dos_short_path(cwd, shortcwd) < 0) str_ncopy(shortcwd, cwd, PATH_LEN);
    rest = shortcwd + 3;
    for (;;) {
        k = 0;
        while (*rest && *rest != '\\' && k < 12) part[k++] = *rest++;
        part[k] = 0;
        if (*rest == '\\') rest++;
        n = tree_insert_children(dir, node + 1, depth, node, k ? part : 0);
        if (!k) break;
        for (k = node + 1; k < node + 1 + n; k++) if (!str_icmp(tree[k].name, part)) break;
        if (k >= node + 1 + n) break;
        tree[k].flags |= 1;
        {
            char t[PATH_LEN];
            local_join(t, dir, part);
            str_copy(dir, t);
        }
        node = k;
        depth++;
    }
    tree[node].flags |= 2;
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
static void small_icon(int x, int y, const struct item *it)
{
    if (it->attr & A_DIR) {
        ui_rect(x + 1, y + 3, 6, 2, C_BROWN);
        ui_rect(x + 1, y + 5, 14, 9, C_YELLOW);
        ui_rect(x + 1, y + 13, 14, 1, C_BROWN);
        return;
    }
    if (ext_is(item_name(it), "COM|EXE")) {
        ui_rect(x + 1, y + 2, 14, 12, C_SHADOW);
        ui_rect(x + 2, y + 3, 12, 3, C_TITLE);
        ui_rect(x + 2, y + 6, 12, 7, C_PAPER);
        return;
    }
    ui_rect(x + 3, y + 1, 10, 14, C_PAPER);
    ui_rect(x + 3, y + 1, 10, 1, C_SHADOW);
    ui_rect(x + 3, y + 14, 10, 1, C_SHADOW);
    ui_rect(x + 3, y + 1, 1, 14, C_SHADOW);
    ui_rect(x + 12, y + 1, 1, 14, C_SHADOW);
    if (ext_is(item_name(it), TEXT_TYPES)) {
        ui_rect(x + 5, y + 4, 6, 1, C_BLUE);
        ui_rect(x + 5, y + 7, 6, 1, C_BLUE);
        ui_rect(x + 5, y + 10, 5, 1, C_BLUE);
    } else if (ext_is(item_name(it), "SYS|DRV|DLL|386|VXD|INI|CFG")) {
        ui_rect(x + 6, y + 6, 4, 4, C_SHADOW);
    }
}
static int big_icon_id(const struct item *it)
{
    if (it->attr & A_DIR) return ICON_FOLDER;
    if (ext_is(item_name(it), "COM|EXE")) return ICON_PROGRAM;
    if (ext_is(item_name(it), TEXT_TYPES)) return ICON_EDITOR;
    if (ext_is(item_name(it), "BAT")) return ICON_DOS;
    return -1;
}
static int tb_hot = -1;                    /* the button under the pointer */
static void tb_button(int x, int w, const char *label, int enabled, int hot)
{
    /* Flat until the pointer is over it, as Explorer's toolbar. */
    if (hot && enabled) ui_bevel(x, tb_y, w, 28, C_LIGHT);
    else ui_bevel(x, tb_y, w, 28, C_FACE);
    ui_text(x + (w - ui_measure(label)) / 2, tb_y + 6, label, enabled ? C_INK : C_SHADOW);
}
struct tbdef { const char *label; int w, cmd; };
static struct tbdef tb[] = {
    { "< Back", 60, C_BACK }, { "Forward >", 76, C_FORWARD }, { "Up", 36, C_UP },
    { "Refresh", 62, C_REFRESH }, { 0, 8, 0 }, { "Cut", 40, C_CUT }, { "Copy", 46, C_COPY },
    { "Paste", 50, C_PASTE }, { 0, 8, 0 }, { "Delete", 56, C_DELETE },
    { "Properties", 80, C_PROPERTIES }, { 0, 8, 0 }, { "Views", 52, -1 } };
#define TB_COUNT 13
static int tb_enabled(int cmd)
{
    int any = cur >= 0 && nitems > 0;
    switch (cmd) {
    case C_BACK: return nback > 0;
    case C_FORWARD: return nfwd > 0;
    case C_UP: return media ? cwd[1] != 0 : !is_root(cwd);
    case C_CUT: case C_DELETE: return any && !media;
    case C_COPY: return any;
    case C_PASTE: return nclip && !media;
    }
    return 1;
}
struct place { const char *label; const char *path; int device; int icon; };
static struct place places[] = {
    { "Desktop", "C:\\DESKTOP", 0, 14 }, { "Local Disk (C:)", "C:\\", 0, 3 },
    { "Applications", "C:\\APPS", 0, 1 }, { "System", "C:\\SYSTEM", 0, 1 },
    { "Recycle Bin", "", -1, 18 }, { 0, 0, 0, 0 },
    { "Floppy", "/", 1, 10 }, { "USB drive", "/", 2, 15 }, { "CD-ROM", "/", 3, 16 } };
#define PLACE_COUNT 9
#define PLACE_SPLIT 5
static int place_y(int i) { return body_y + 24 + i * 24 - (i > PLACE_SPLIT ? 4 : 0); }
static int place_current(int i)
{
    if (places[i].device) return media == places[i].device;
    if (media) return 0;
    if (i == 1) return is_root(cwd);
    return !str_icmp(cwd, places[i].path) || (!str_nicmp(cwd, places[i].path, str_len(places[i].path)) && cwd[str_len(places[i].path)] == '\\');
}
static void go_place(int i)
{
    if (places[i].device < 0) { app_open(WIN_RECYCLE, ""); return; }
    if (i == 0) make_dirs(places[i].path);
    navigate(places[i].path, places[i].device, 1);
}
static int tree_y0(void) { return place_y(PLACE_COUNT - 1) + 30; }
static int tree_rows(void) { int r = (body_y + body_h - 4 - (tree_y0() + 20)) / TREE_ROW; return r < 0 ? 0 : r; }
static void draw_tree(void)
{
    int i, y, rows = tree_rows(), cur_node = -1;
    if (!ntree || rows <= 0) return;
    y = tree_y0();
    ui_rect(X0 + 8, y - 4, places_w - 16, 1, C_SHADOW);
    ui_text(X0 + 10, y, "Folders", C_TITLE | BOLD);
    for (i = 0; i < ntree; i++) if (tree[i].flags & 2) cur_node = i;
    if (cur_node >= 0 && cur_node < tree_top) tree_top = cur_node;
    if (cur_node >= tree_top + rows) tree_top = cur_node - rows + 1;
    if (tree_top > ntree - 1) tree_top = 0;
    for (i = tree_top; i < ntree && i < tree_top + rows; i++) {
        int ry = y + 20 + (i - tree_top) * TREE_ROW, x = X0 + 10 + tree[i].depth * 10, fg = C_INK;
        char t[20];
        if (tree[i].flags & 2) { ui_rect(x + 18, ry, places_w - (x - X0) - 26, TREE_ROW, C_TITLE); fg = C_PAPER; }
        if (drop_tree == i) draw_focus(x + 18, ry, places_w - (x - X0) - 26, TREE_ROW);
        if (i == 0) { ui_rect(x, ry + 5, 14, 7, C_SHADOW); ui_rect(x + 1, ry + 6, 12, 4, C_FACE); }
        else { ui_rect(x + 1, ry + 3, 5, 2, C_BROWN); ui_rect(x + 1, ry + 5, 13, 8, (tree[i].flags & 1) ? C_YELLOW : C_BROWN); }
        if (i == 0) { str_copy(t, "Disk "); str_cat(t, tree[0].name); }
        else text_fit(tree[i].name, places_w - (x - X0) - 30, t);
        ui_text(x + 20, ry + 1, t, fg);
    }
}
static int tree_at(int sy)
{
    int y = tree_y0() + 20, i;
    if (!ntree || sy < y) return -1;
    i = tree_top + (sy - y) / TREE_ROW;
    return i < ntree && i < tree_top + tree_rows() ? i : -1;
}
static void draw_places(void)
{
    int i;
    if (!places_w) return;
    ui_inset(X0 + 2, body_y, places_w - 4, body_h);
    ui_text(X0 + 10, body_y + 4, "Places", C_TITLE | BOLD);
    for (i = 0; i < PLACE_COUNT; i++) {
        int y = place_y(i), fg = C_INK;
        if (!places[i].label) {
            ui_rect(X0 + 8, y + 8, places_w - 16, 1, C_SHADOW);
            ui_text(X0 + 10, y + 10, "Removable", C_SHADOW);
            continue;
        }
        if (place_current(i)) { ui_rect(X0 + 5, y, places_w - 10, 22, C_TITLE); fg = C_PAPER; }
        if (focus_area == 2 && place_focus == i) draw_focus(X0 + 5, y, places_w - 10, 22);
        /* Small device glyphs. */
        if (places[i].icon == 14) { ui_rect(X0 + 10, y + 5, 16, 11, C_SHADOW); ui_rect(X0 + 11, y + 6, 14, 8, C_BLUE); ui_rect(X0 + 15, y + 16, 6, 2, C_SHADOW); }
        else if (places[i].icon == 18) { ui_rect(X0 + 12, y + 5, 12, 2, C_SHADOW); ui_rect(X0 + 13, y + 7, 10, 11, C_GREEN); ui_rect(X0 + 15, y + 9, 1, 7, C_PAPER); ui_rect(X0 + 19, y + 9, 1, 7, C_PAPER); }
        else if (places[i].icon == 1) { ui_rect(X0 + 11, y + 6, 6, 2, C_BROWN); ui_rect(X0 + 11, y + 8, 14, 9, C_YELLOW); }
        else if (places[i].icon == 3) { ui_rect(X0 + 10, y + 8, 16, 8, C_SHADOW); ui_rect(X0 + 11, y + 9, 14, 5, C_FACE); ui_rect(X0 + 21, y + 11, 2, 2, C_GREEN); }
        else if (places[i].icon == 10) { ui_rect(X0 + 11, y + 4, 14, 14, C_BLUE); ui_rect(X0 + 14, y + 4, 8, 5, C_PAPER); }
        else if (places[i].icon == 15) { ui_rect(X0 + 14, y + 4, 8, 14, C_SHADOW); ui_rect(X0 + 15, y + 5, 6, 4, C_PAPER); }
        else { ui_rect(X0 + 11, y + 4, 14, 14, C_FACE); ui_rect(X0 + 16, y + 9, 4, 4, C_SHADOW); }
        ui_text(X0 + 32, y + 3, places[i].label, fg);
        if (drop_place == i) draw_focus(X0 + 5, y, places_w - 10, 22);
    }
    draw_tree();
}
static void draw_details_header(void)
{
    static const char *names[] = { "Name", "Size", "Type", "Modified" };
    int cx[5], i;
    cx[0] = lx + 2; cx[1] = col_x(44); cx[2] = col_x(58);
    cx[3] = col_x(79); cx[4] = lx + lw - 2;
    for (i = 0; i < 4; i++) {
        ui_bevel(cx[i], ly + 2, cx[i + 1] - cx[i], 20, C_FACE);
        ui_text(cx[i] + 6, ly + 4, names[i], C_INK);
        if (sort_col == i) {           /* sort arrow */
            int ax = cx[i + 1] - 14, k;
            for (k = 0; k < 4; k++) ui_rect(ax + (sort_desc ? k : 3 - k), ly + 8 + k, 1 + 2 * (sort_desc ? 3 - k : k), 1, C_SHADOW);
        }
    }
}
static void draw_item(int i)
{
    int x, y, w, h, fg = C_INK, sel;
    char t[72], tb2[20];
    struct item *it = &items[i];
    item_rect(i, &x, &y, &w, &h);
    sel = it->sel;
    if (view == 1) {
        int id = big_icon_id(it);
        if (id >= 0) ui_icon(x + (w - 44) / 2, y + 4, id);
        else { ui_rect(x + w / 2 - 12, y + 6, 24, 30, C_PAPER); ui_rect(x + w / 2 - 12, y + 6, 24, 1, C_SHADOW);
               ui_rect(x + w / 2 - 12, y + 35, 24, 1, C_SHADOW); ui_rect(x + w / 2 - 12, y + 6, 1, 30, C_SHADOW);
               ui_rect(x + w / 2 + 11, y + 6, 1, 30, C_SHADOW); }
        text_fit(item_name(it), w - 4, t);
        if (sel) { ui_rect(x + (w - ui_measure(t)) / 2 - 2, y + 48, ui_measure(t) + 4, 17, C_TITLE); fg = C_PAPER; }
        if (renaming == i) field_draw(&rename_field, x, y + 46, w, 1);
        else ui_text(x + (w - ui_measure(t)) / 2, y + 48, t, fg);
        if (i == cur && HOST.active && focus_area == 0) draw_focus(x, y, w, h);
        return;
    }
    small_icon(x + 4, y + 1, it);
    if (view == 0) {
        int c1 = col_x(44), c2 = col_x(58), c3 = col_x(79);
        text_fit(item_name(it), c1 - x - 30, t);
        if (sel) { ui_rect(x + 24, y, ui_measure(t) + 6, h, C_TITLE); fg = C_PAPER; }
        if (renaming == i) field_draw(&rename_field, x + 22, y - 2, c1 - x - 26, 1);
        else ui_text(x + 27, y + 1, t, fg);
        if (!(it->attr & A_DIR)) {
            fmt_size(t, it->size);
            ui_text(c2 - 8 - ui_measure(t), y + 1, t, C_INK);
        }
        draw_frame_text(c2 + 6, y + 1, c3 - c2 - 10, type_name(it, tb2), C_INK);
        if (it->date) {
            fmt_date(t, it->date);
            str_cat(t, " ");
            fmt_time(t + str_len(t), it->time);
            draw_frame_text(c3 + 6, y + 1, lx + lw - c3 - 10, t, C_INK);
        }
        if (i == cur && HOST.active && focus_area == 0) draw_focus(x + 24, y, (sel ? ui_measure(t) : 0) + 6 > 6 ? c1 - x - 26 : c1 - x - 26, h);
    } else {
        text_fit(item_name(it), w - 30, t);
        if (sel) { ui_rect(x + 24, y, ui_measure(t) + 6, h, C_TITLE); fg = C_PAPER; }
        if (renaming == i) field_draw(&rename_field, x + 22, y - 2, w - 24, 1);
        else ui_text(x + 27, y + 1, t, fg);
        if (i == cur && HOST.active && focus_area == 0) draw_focus(x + 24, y, ui_measure(t) + 6, h);
    }
}
static void draw_status(void)
{
    char t[96], n[24];
    int sy = Y0 + H - 20, sel = count_selected(), i;
    u32 bytes = 0;
    ui_rect(X0, sy - 1, W, 21, C_FACE);
    if (status_text[0]) str_copy(t, status_text);
    else {
        fmt_u32(n, (u32)(sel ? sel : nitems));
        str_copy(t, n);
        str_cat(t, sel ? " object(s) selected" : " object(s)");
        for (i = 0; i < nitems; i++) if ((!sel || items[i].sel) && !(items[i].attr & A_DIR)) bytes += items[i].size;
        str_cat(t, "   ");
        fmt_size(n, bytes);
        str_cat(t, n);
    }
    ui_inset(X0 + 2, sy, W - 214, 19);
    draw_frame_text(X0 + 6, sy + 1, W - 224, t, C_INK);
    ui_inset(X0 + W - 210, sy, 208, 19);
    if (media) str_copy(t, "Read-only medium");
    else {
        long total, free = dos_disk_free(to_upper(cwd[0]) - 'A' + 1, &total);
        str_copy(t, "Free space: ");
        if (free >= 0) { fmt_size(n, (u32)free); str_cat(t, n); } else str_cat(t, "unknown");
    }
    ui_text(X0 + W - 204, sy + 1, t, C_INK);
}
static void paint(void)
{
    static int logged_x, logged_y;
    int i, x;
    layout();
    if (lx != logged_x || ly != logged_y) {       /* for the gates: where rows are */
        char t[32], n[8];
        logged_x = lx; logged_y = ly;
        fmt_u32(t, (u32)lx); str_cat(t, " ");
        fmt_u32(n, (u32)(ly + (view == 0 ? 22 : 2))); str_cat(t, n); str_cat(t, " ");
        fmt_u32(n, (u32)(X0 + 40)); str_cat(t, n); str_cat(t, " ");
        fmt_u32(n, (u32)(tree_y0() + 20)); str_cat(t, n);
        app_log("[FILES] geometry", t);
    }
    update_menus();
    bar.x = X0; bar.y = Y0; bar.w = W;
    ui_rect(X0, tb_y - 1, W, body_y - tb_y + 1, C_FACE);
    /* Toolbar. */
    x = X0 + 4;
    for (i = 0; i < TB_COUNT; i++) {
        if (!tb[i].label) { ui_rect(x + 3, tb_y + 3, 1, 22, C_SHADOW); ui_rect(x + 4, tb_y + 3, 1, 22, C_PAPER); x += tb[i].w; continue; }
        if (x + tb[i].w > X0 + W - 4) break;        /* a narrow window */
        tb_button(x, tb[i].w, tb[i].label, tb[i].cmd < 0 || tb_enabled(tb[i].cmd), i == tb_hot);
        x += tb[i].w + 2;
    }
    /* Address bar. */
    ui_text(X0 + 6, addr_y + 3, "Address", C_INK);
    field_draw(&address, X0 + 66, addr_y, W - 66 - 50, focus_area == 1 && HOST.active);
    ui_bevel(X0 + W - 46, addr_y, 42, 22, C_FACE);
    ui_text(X0 + W - 36, addr_y + 3, "Go", C_INK);
    draw_places();
    /* The list. */
    ui_inset(lx, ly, lw, lh);
    ui_rect(lx + 2, ly + 2, lw - 4, lh - 4, C_PAPER);
    if (view == 0) draw_details_header();
    for (i = 0; i < nitems; i++) {
        int u = unit_of(i);
        if (u < top_row || u >= top_row + page_units()) continue;
        draw_item(i);
    }
    if (!nitems) ui_text(lx + 20, ly + (view == 0 ? 30 : 10), media ? "This medium is empty or unreadable." : "This folder is empty.", C_SHADOW);
    if (dragging && drop_i >= 0) {
        int x, y, w, h;
        item_rect(drop_i, &x, &y, &w, &h);
        ui_rect(x, y, w, 1, C_TITLE); ui_rect(x, y + h - 1, w, 1, C_TITLE);
        ui_rect(x, y, 1, h, C_TITLE); ui_rect(x + w - 1, y, 1, h, C_TITLE);
    }
    draw_scroll(lx + lw, ly, lh, top_row, total_units(), page_units());
    draw_status();
    if (dragging) {                               /* what a drop would do */
        char t[32], n[8];
        int copy = (HOST.shift & SH_CTRL) || media, tx = HOST.mx + 18, ty = HOST.my + 18;
        fmt_u32(n, (u32)count_selected());
        str_copy(t, copy ? "Copy " : "Move ");
        str_cat(t, n);
        str_cat(t, " item(s)");
        ui_bevel(tx, ty, ui_measure(t) + 12, 20, C_LIGHT);
        ui_text(tx + 6, ty + 2, t, C_INK);
    }
    menubar_draw(&bar);
    if (ctx.open) popup_draw(&ctx);
}
/* The dialog's own window. */
static void paint_dialog(void)
{
    if (dlg_kind == D_PROGRESS) progress_update();
    dialog_draw(&dlg);
    if (dlg_kind == D_ABOUT) ui_icon(dlg.x + 8, dlg.y + DIALOG_TITLE_H + 2, ICON_FOLDER);
    if (dlg_kind == D_PROGRESS) {
        int bx = dlg.x + 8, by = dlg.y + DIALOG_TITLE_H + 6 + 44, bw = 270, fill;
        ui_inset(bx, by, bw, 14);
        fill = (int)((job.bytes / 4096 + job.files * 4) % (bw - 4));
        ui_rect(bx + 2, by + 2, fill, 10, C_TITLE);
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
static void dialog_result(int r)
{
    int kind = dlg_kind;
    if (r < 0) return;
    dlg_kind = D_NONE;
    switch (kind) {
    case D_DELETE:
        if (r == MB_YES) { if (delete_for_good) begin_job_from_selection(JOB_DELETE, 0); else recycle_selection(); }
        break;
    case D_REPLACE:
        if (r == MB_YES) { job.skipped = -1; job.ask = 0; }
        else if (r == MB_YESALL) { job.replace_all = 1; job.ask = 0; }
        else if (r == MB_NO) { job.index++; job.ask = 0; job.skipped = 0; }
        else { job.cancel = 1; job.ask = 0; }
        if (job.active) progress_dialog();
        break;
    case D_PROGRESS:
        job.cancel = 1;
        break;
    }
}
static void extend_to(int i)
{
    int a = anchor_i < 0 ? i : anchor_i, k;
    for (k = 0; k < nitems; k++) items[k].sel = (k >= (a < i ? a : i) && k <= (a < i ? i : a));
    cur = i;
}
static void click_item(int i, int shift)
{
    if (i < 0) {
        if (!(shift & (SH_CTRL | SH_SHIFT))) { int k; for (k = 0; k < nitems; k++) items[k].sel = 0; }
        return;
    }
    if (shift & SH_SHIFT) extend_to(i);
    else if (shift & SH_CTRL) { items[i].sel = !items[i].sel; cur = i; anchor_i = i; }
    else select_only(i);
}
static int toolbar_at(int sx, int sy)
{
    int x = X0 + 4, i;
    if (sy < tb_y || sy >= tb_y + 28) return -1;
    for (i = 0; i < TB_COUNT; i++) {
        if (!tb[i].label) { x += tb[i].w; continue; }
        if (x + tb[i].w > X0 + W - 4) break;
        if (sx >= x && sx < x + tb[i].w) return i;
        x += tb[i].w + 2;
    }
    return -1;
}
static int toolbar_hit(int sx, int sy)
{
    int x = X0 + 4, i;
    if (sy < tb_y || sy >= tb_y + 28) return 0;
    for (i = 0; i < TB_COUNT; i++) {
        if (!tb[i].label) { x += tb[i].w; continue; }
        if (x + tb[i].w > X0 + W - 4) break;
        if (sx >= x && sx < x + tb[i].w) {
            if (tb[i].cmd < 0) {                       /* Views: cycle */
                view = (view + 1) % 3;
                top_row = 0;
                ensure_visible(cur);
            } else if (tb_enabled(tb[i].cmd)) command(tb[i].cmd);
            return 1;
        }
        x += tb[i].w + 2;
    }
    return 0;
}
/* Drag and drop: selected items onto a folder, a place or a tree folder
 * move there (copy with Ctrl, or from removable media). */
static int drag_move(int sx, int sy)
{
    int i, p;
    if (!dragging) {
        int dx = sx - drag_x, dy = sy - drag_y;
        if ((long)dx * dx + (long)dy * dy < 36) return 0;
        dragging = 1;
        if (!items[drag_i].sel) select_only(drag_i);
    }
    drop_i = drop_place = drop_tree = -1;
    if (sx >= lx && sx < lx + lw && sy >= ly && sy < ly + lh) {
        i = item_at(sx, sy);
        if (i >= 0 && (items[i].attr & A_DIR) && !items[i].sel && !media) drop_i = i;
    } else if (places_w && sx >= X0 && sx < X0 + places_w && sy >= body_y && sy < body_y + body_h) {
        for (p = 0; p < PLACE_COUNT; p++)
            if (places[p].label && places[p].device <= 0 && sy >= place_y(p) && sy < place_y(p) + 22) drop_place = p;
        if (drop_place < 0) drop_tree = tree_at(sy);
    }
    return 1;
}
static int drag_drop(int sx, int sy)
{
    char dest[PATH_LEN];
    int copy = (HOST.shift & SH_CTRL) || media, was = dragging;
    if (dragging) drag_move(sx, sy);
    dest[0] = 0;
    if (was && drop_i >= 0) local_join(dest, cwd, item_name(&items[drop_i]));
    else if (was && drop_place >= 0 && places[drop_place].device < 0) {
        dragging = 0;
        drag_i = drop_i = drop_place = drop_tree = -1;
        if (!media) delete_selection(0);            /* onto the Recycle Bin */
        return 1;
    }
    else if (was && drop_place >= 0) str_copy(dest, places[drop_place].path);
    else if (was && drop_tree >= 0) tree_path(drop_tree, dest);
    dragging = 0;
    drag_i = drop_i = drop_place = drop_tree = -1;
    if (!was) return 0;
    if (!dest[0]) return 1;
    app_log("[FILES] drop", dest);
    begin_job_from_selection(copy ? JOB_COPY : JOB_MOVE, dest);
    return 1;
}

static int on_mouse(int kind, int x, int y)
{
    int sx = HOST.x + x, sy = HOST.y + TITLE_H + y, r, i;
    layout();
    if (dlg.open) {
        r = dialog_mouse(&dlg, kind, sx, sy);
        if (r >= 0) dialog_result(r);
        return 1;
    }
    if (ctx.open) {
        r = popup_mouse(&ctx, kind, sx, sy);
        if (r >= 0) {
            if (r == C_PASTE && cur >= 0 && (items[cur].attr & A_DIR) && items[cur].sel && ctx.items == m_item)
                context_paste_into();
            else command(r);
        }
        return 1;
    }
    if (drag_i >= 0 && kind == MOUSE_MOVE) return drag_move(sx, sy);
    if (drag_i >= 0 && kind == MOUSE_UP) return drag_drop(sx, sy);
    r = menubar_mouse(&bar, kind, sx, sy);
    if (r >= 0) { command(r); return 1; }
    if (r == -1) return 1;
    if (renaming >= 0 && kind == MOUSE_DOWN) {
        int rx, ry, rw, rh;
        item_rect(renaming, &rx, &ry, &rw, &rh);
        if (sy >= ry - 2 && sy < ry + rh + 22 && sx >= rx && sx < rx + rw) return 1;
        commit_rename();
    }
    if (kind == MOUSE_DOWN) {
        if (toolbar_hit(sx, sy)) return 1;
        if (sy >= addr_y && sy < addr_y + 22) {
            if (sx >= X0 + W - 46) { focus_area = 0; navigate(address.text, 0, 1); }
            else if (sx >= X0 + 66) { focus_area = 1; field_click(&address, X0 + 66, W - 116, sx); }
            return 1;
        }
        if (places_w && sx >= X0 && sx < X0 + places_w && sy >= body_y && sy < body_y + body_h) {
            for (i = 0; i < PLACE_COUNT; i++) {
                int py = place_y(i);
                if (places[i].label && sy >= py && sy < py + 22) {
                    focus_area = 2; place_focus = i;
                    go_place(i);
                    return 1;
                }
            }
            i = tree_at(sy);
            if (i >= 0) {
                char p[PATH_LEN];
                tree_path(i, p);
                navigate(p, 0, 1);
            }
            return 1;
        }
        if (sx >= lx + lw && sx < lx + lw + 16 && sy >= ly && sy < ly + lh) {
            long p = scroll_hit(lx + lw, ly, lh, sy, total_units(), page_units());
            if (p == -1 && top_row > 0) top_row--;
            else if (p == -2 && top_row + page_units() < total_units()) top_row++;
            else if (p >= 16) top_row = (int)(p - 16);
            return 1;
        }
        if (view == 0 && sy >= ly + 2 && sy < ly + 22 && sx >= lx && sx < lx + lw) {
            int c = sx < col_x(44) ? 0 : sx < col_x(58) ? 1 : sx < col_x(79) ? 2 : 3;
            if (c == sort_col) sort_desc = !sort_desc; else { sort_col = c; sort_desc = 0; }
            sort_items();
            return 1;
        }
    }
    if (sx >= lx && sx < lx + lw && sy >= ly && sy < ly + lh) {
        i = item_at(sx, sy);
        if (kind == MOUSE_DOWN) {
            focus_area = 0;
            if (i >= 0 && i == last_click_i && HOST.ticks - last_click_tick < 9 && !(HOST.shift & (SH_SHIFT | SH_CTRL))) {
                last_click_i = -1;
                select_only(i);
                open_item(i, 0);
                return 1;
            }
            last_click_i = i;
            last_click_tick = HOST.ticks;
            click_item(i, HOST.shift);
            if (i >= 0) { drag_i = i; drag_x = sx; drag_y = sy; dragging = 0; }
            return 1;
        }
        if (kind == MOUSE_RIGHT) {
            update_menus();
            if (i >= 0) {
                if (!items[i].sel) select_only(i); else cur = i;
                update_menus();
                popup_open(&ctx, m_item, ITEM_MENU_COUNT, sx, sy, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
                app_log("[FILES] menu", "item");
            } else {
                int k;
                for (k = 0; k < nitems; k++) items[k].sel = 0;
                update_menus();
                popup_open(&ctx, m_back, 16, sx, sy, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
                app_log("[FILES] menu", "background");
            }
            return 1;
        }
    }
    return kind == MOUSE_DOWN || kind == MOUSE_RIGHT;
}
static void move_cursor(int to, int shift)
{
    if (!nitems) return;
    if (to < 0) to = 0;
    if (to >= nitems) to = nitems - 1;
    if (shift & SH_SHIFT) extend_to(to);
    else if (shift & SH_CTRL) cur = to;
    else select_only(to);
    ensure_visible(cur);
}
static int on_key(int key)
{
    int s = KEY_SCAN(key), ch = KEY_CHAR(key), shift = HOST.shift, r, letter, i;
    layout();
    if (dlg.open) {
        r = dialog_key(&dlg, key, shift);
        if (r >= 0) dialog_result(r);
        return 1;
    }
    if (ctx.open) {
        r = popup_key(&ctx, key);
        if (r >= 0) command(r);
        return 1;
    }
    r = menubar_key(&bar, key, shift);
    if (r >= 0) { command(r); return 1; }
    if (r == -1) return 1;
    if (renaming >= 0) {
        if (ch == 13) commit_rename();
        else if (key == K_ESC) renaming = -1;
        else field_key(&rename_field, key, shift);
        return 1;
    }
    if (focus_area == 1) {                              /* address bar */
        if (ch == 13) { focus_area = 0; navigate(address.text, 0, 1); return 1; }
        if (key == K_ESC) { focus_area = 0; refresh(); return 1; }
        if (s == 0x0F) { focus_area = 0; return 1; }
        field_key(&address, key, shift);
        return 1;
    }
    if (key == 0x5D00) {                                /* Shift+F10, Menu key */
        update_menus();
        if (cur >= 0) {
            int x, y, w, h;
            item_rect(cur, &x, &y, &w, &h);
            popup_open(&ctx, m_item, ITEM_MENU_COUNT, x + 30, y + h, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
        } else popup_open(&ctx, m_back, 16, lx + 20, ly + 30, HOST.x + HOST.w - 4, HOST.y + HOST.h - 4);
        app_log("[FILES] menu", cur >= 0 ? "item" : "background");
        return 1;
    }
    letter = (shift & SH_CTRL) ? key_ctrl_letter(key) : 0;
    if (letter) {
        switch (letter) {
        case 'A': command(C_SELALL); return 1;
        case 'C': command(C_COPY); return 1;
        case 'X': command(C_CUT); return 1;
        case 'V': command(C_PASTE); return 1;
        case 'N': command(C_NEWFOLDER); return 1;   /* Ctrl+Shift+N (BIOS: as Ctrl+N) */
        }
        return 1;
    }
    if ((shift & SH_ALT) && !ch) {
        if (s == 0x9B || s == K_LEFT) { command(C_BACK); return 1; }
        if (s == 0x9D || s == K_RIGHT) { command(C_FORWARD); return 1; }
        if (s == 0x98 || s == K_UP) { command(C_UP); return 1; }
        if (s == 0x20) { focus_area = 1; address.sel = 1; return 1; }      /* Alt+D */
        if (s == 0x1C || s == 0xA6) { command(C_PROPERTIES); return 1; }  /* Alt+Enter */
    }
    if (s == 0xA6 || (s == 0x1C && (shift & SH_ALT))) { command(C_PROPERTIES); return 1; }
    if (ch == 13) { if (cur >= 0) open_item(cur, 0); return 1; }
    if (ch == 8) { command(C_UP); return 1; }
    if (key == K_ESC) { for (i = 0; i < nitems; i++) items[i].sel = 0; return 1; }
    if (s == 0x0F) { focus_area = (focus_area + 1) % 3; return 1; }
    if (focus_area == 2) {
        if (s == K_UP || s == K_DOWN) {
            do { place_focus = (place_focus + (s == K_UP ? PLACE_COUNT - 1 : 1)) % PLACE_COUNT; } while (!places[place_focus].label);
            return 1;
        }
        if (ch == ' ') { go_place(place_focus); return 1; }
    }
    if (!ch || ch == 0xE0) {
        int step_v = view == 1 ? icon_cols() : 1, step_h = view == 2 ? list_rows() : view == 1 ? 1 : 0;
        switch (s) {
        case K_F1: command(C_HELP); return 1;
        case K_F2: command(C_RENAME); return 1;
        case K_F5: command(C_REFRESH); return 1;
        case 0x3E: focus_area = 1; address.sel = 1; return 1;          /* F4 */
        case K_DEL: command(C_DELETE); return 1;
        case K_UP: move_cursor(cur - step_v, shift); return 1;
        case K_DOWN: move_cursor(cur < 0 ? 0 : cur + step_v, shift); return 1;
        case K_LEFT: if (step_h) move_cursor(cur - step_h, shift); return 1;
        case K_RIGHT: if (step_h) move_cursor(cur + step_h, shift); return 1;
        case K_HOME: move_cursor(0, shift); return 1;
        case K_END: move_cursor(nitems - 1, shift); return 1;
        case K_PGUP: move_cursor(cur - (view == 0 ? details_rows() : view == 1 ? icon_cols() * icon_rows() : list_rows() * list_cols()), shift); return 1;
        case K_PGDN: move_cursor(cur + (view == 0 ? details_rows() : view == 1 ? icon_cols() * icon_rows() : list_rows() * list_cols()), shift); return 1;
        }
        return 0;
    }
    if (ch == ' ' && cur >= 0) { if (shift & SH_CTRL) items[cur].sel = !items[cur].sel; else select_only(cur); return 1; }
    if (ch > ' ') {                                      /* type to find */
        char c = to_upper((char)ch);
        for (i = 1; i <= nitems; i++) {
            int k = (cur + i) % nitems;
            if (to_upper(item_name(&items[k])[0]) == c) { move_cursor(k, 0); return 1; }
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
static int files_hover(int sx, int sy)
{
    int i;
    layout();
    ui_dirty = 0;
    if (dialog_mine(&dlg)) { dialog_hover(&dlg, sx, sy); return ui_dirty; }
    if (dlg.open) return 0;
    if (ctx.open) { popup_mouse(&ctx, MOUSE_HOVER, sx, sy); return ui_dirty; }
    if (menubar_open(&bar)) { menubar_mouse(&bar, MOUSE_HOVER, sx, sy); return ui_dirty; }
    i = toolbar_at(sx, sy);
    if (i >= 0 && tb[i].cmd >= 0 && !tb_enabled(tb[i].cmd)) i = -1;
    if (i == tb_hot) return 0;
    tb_hot = i;
    return 1;
}
static int files_event(int ev, int a, int b, int c);
int app_event(int ev, int a, int b, int c)
{
    int r, orig = ev;
    if (ev == EV_OPEN && a == 2) {                 /* our other windows closed */
        dlg.win = 0; pdlg.win = 0; pdlg.open = 0;
        dialog_sync(&dlg);
        return 1;
    }
    if (pdlg.win && HOST.window == pdlg.win) return props_event(ev, a, b, c);
    r = dialog_pre(&dlg, WIN_FILES, &ev, &a);
    if (r >= 0) return r;
    if (dialog_mine(&dlg) && ev == EV_PAINT) { paint_dialog(); return 0; }
    if (ev == EV_MOUSE && a == MOUSE_HOVER) return files_hover(HOST.x + b, HOST.y + TITLE_H + c);
    if (ev == EV_MOUSE && (a == MOUSE_DOWN || a == MOUSE_RIGHT)) { clip_refresh(); tb_hot = -1; }
    if (ev == EV_KEY) clip_refresh();
    r = files_event(ev, a, b, c);
    if (ev == EV_POLL && r == 1 && dlg.win) ui_repaint_win(dlg.win);
    r = dialog_post(&dlg, WIN_FILES, ev, r);
    return orig == EV_CLOSE && ev != EV_CLOSE ? 0 : r;   /* a dialog's close box */
}
static int files_event(int ev, int a, int b, int c)
{
    int i;
    switch (ev) {
    case EV_OPEN: {
        char arg[APP_ARG_BYTES];
        if (!name_seg) name_seg = dos_alloc((u16)(((u32)MAX_ITEMS * LFN_NAME + 15) / 16));
        if (!history_seg) { history_seg = dos_alloc((2 * HISTORY * PATH_LEN + 15) / 16); nback = nfwd = 0; }
        str_ncopy(arg, APP_ARG, APP_ARG_BYTES);
        APP_ARG[0] = 0;
        clip_refresh();
        if (!cwd[0]) {
            make_dirs("C:\\DESKTOP");
            HDR_WIDTH = HOST.screen_w - 40 > 780 ? 780 : HOST.screen_w - 40;
            HDR_HEIGHT = HOST.screen_h - 90 > 520 ? 520 : HOST.screen_h - 90;
        }
        if (!str_nicmp(arg, "media:", 6)) { navigate(arg[8] ? arg + 8 : "/", arg[6] - '0', cwd[0] != 0); return 1; }
        if (arg[0] && arg[1] == ':') { if (navigate(arg, 0, cwd[0] != 0)) return 1; }
        if (!cwd[0]) {
            char p[4];
            p[0] = (char)('A' + dos_get_drive()); p[1] = ':'; p[2] = '\\'; p[3] = 0;
            navigate(p, 0, 0);
        } else refresh();
        return 1;
    }
    case EV_PAINT:
        paint();
        return 0;
    case EV_KEY:
        return on_key(a);
    case EV_MOUSE:
        return on_mouse(a, b, c);
    case EV_POLL:
        if (!job.active) {
            /* Changes made elsewhere (the desktop, a DOS program) appear. */
            if (!media && !dlg.open && renaming < 0 && drag_i < 0 && (unsigned)(HOST.ticks - sig_tick) >= 18 &&
                fs_changes() != seen_changes) {
                seen_changes = fs_changes();
                sig_tick = HOST.ticks;
                if (list_sig() != dir_sig) { refresh(); return 1; }
            }
            return 0;
        }
        if (job.ask) {
            if (!dlg.open || dlg_kind != D_REPLACE) { replace_dialog(); return 1; }
            return 0;
        }
        for (i = 0; i < 16 && job.active && !job.ask; i++) job_step();
        if (!job.active) { dlg.open = 0; dlg_kind = D_NONE; return 1; }
        return (HOST.ticks & 3) == 0 ? 1 : 2;
    case EV_CLOSE:
        if (job.active) { job.cancel = 1; job_step(); }
        if (media_seg) { dos_free(media_seg); media_seg = 0; }
        if (name_seg) { dos_free(name_seg); name_seg = 0; }
        if (history_seg) { dos_free(history_seg); history_seg = 0; nback = nfwd = 0; }
        return 0;
    case EV_SUSPEND:
        if (job.active) return 1;
        if (media) { str_copy(APP_ARG, "media:"); APP_ARG[6] = (char)('0' + media); APP_ARG[7] = ':'; str_ncopy(APP_ARG + 8, cwd, 70); }
        else str_copy(APP_ARG, cwd);
        if (media_seg) { dos_free(media_seg); media_seg = 0; }
        if (name_seg) { dos_free(name_seg); name_seg = 0; }
        if (history_seg) { dos_free(history_seg); history_seg = 0; nback = nfwd = 0; }
        return 0;
    }
    (void)c;
    return 0;
}
