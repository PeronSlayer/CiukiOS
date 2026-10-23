/* SPDX-License-Identifier: MIT
 * G2: the Windows 98 Recycle Bin. Compiled into model.c (this file is
 * #included at its end and uses its helpers), so every program that links the
 * Files model has the bin and no build list changes. Design, sources and the
 * INFO2 layout: docs/design/recycle-bin.md. */

int64_t (*rb_clock_hook)(void);
uint64_t (*rb_capacity_hook)(char drive);

static int64_t rb_now(void) { return rb_clock_hook ? rb_clock_hook() : (int64_t)time(NULL); }
uint64_t rb_capacity(char drive)
{
    uint64_t bytes = rb_capacity_hook ? rb_capacity_hook(drive) : 0;
    return bytes ? bytes : RB_ASSUMED_CAPACITY;
}
int fm_is_recycle(const char *path) { return path && !compare(path, FM_RECYCLE); }

/* ---- small helpers ---- */
static int rb_letter(char drive) { unsigned c = fold((unsigned char)drive); return c >= 'a' && c <= 'z' ? (int)(c - 'a') : -1; }
static char rb_upper(char drive) { int i = rb_letter(drive); return i < 0 ? 0 : (char)('A' + i); }

int rb_dir(char drive, char out[RB_DIRLEN])
{
    int i = rb_letter(drive);
    if (i < 2) return -EINVAL;                                  /* A: and B: are floppies */
    if (i == 2) snprintf(out, RB_DIRLEN, "/" RB_DIR);
    else snprintf(out, RB_DIRLEN, "/mnt/%c/" RB_DIR, 'a' + i);
    return 0;
}
static int rb_root(char drive, char out[FM_PATH])
{
    int i = rb_letter(drive);
    if (i < 2) return -EINVAL;
    if (i == 2) strcpy(out, "/"); else snprintf(out, FM_PATH, "/mnt/%c", 'a' + i);
    return 0;
}
static int rb_join(char *out, const char *dir, const char *name)
{
    size_t n = strlen(dir), m = strlen(name);
    if (n + m + 2 > FM_PATH) return -ENAMETOOLONG;
    memcpy(out, dir, n);
    if (n && out[n - 1] != '/') out[n++] = '/';
    memcpy(out + n, name, m + 1);
    return 0;
}
static const char *rb_leaf(const char *dos)
{
    const char *leaf = dos;
    for (const char *p = dos; *p; p++) if (*p == '\\' || *p == '/') leaf = p + 1;
    return leaf;
}
static void rb_stored_name(char drive, uint32_t index, const char *original, char out[RB_NAME])
{
    const char *leaf = rb_leaf(original), *dot = strrchr(leaf, '.');
    const char *ext = dot && dot != leaf && dot[1] && strlen(dot) <= 17 ? dot : "";
    snprintf(out, RB_NAME, "D%c%u%s", (char)fold((unsigned char)drive), (unsigned)index, ext);
}
void rb_item_name(const struct rb_item *item, char out[FM_NAME])
{
    snprintf(out, FM_NAME, "%s", item->original[0] ? rb_leaf(item->original) : item->stored);
}
void rb_item_location(const struct rb_item *item, char out[RB_PATH])
{
    out[0] = 0;
    if (!item->original[0]) return;
    const char *leaf = rb_leaf(item->original);
    size_t n = (size_t)(leaf - item->original);
    if (n > 3) n--;                                             /* C:\folder\name -> C:\folder, but C:\name -> C:\ */
    snprintf(out, RB_PATH, "%.*s", (int)n, item->original);
}
int rb_item_compare_location(const struct rb_item *a, const struct rb_item *b)
{
    /* Compare the folder parts of the original paths in place. */
    size_t an = (size_t)(rb_leaf(a->original) - a->original), bn = (size_t)(rb_leaf(b->original) - b->original);
    size_t n = an < bn ? an : bn;
    for (size_t i = 0; i < n; i++) {
        int d = (int)fold((unsigned char)a->original[i]) - (int)fold((unsigned char)b->original[i]);
        if (d) return d;
    }
    return an < bn ? -1 : an > bn;
}
const char *rb_item_leaf(const struct rb_item *item) { return item->original[0] ? rb_leaf(item->original) : item->stored; }
const struct rb_item *rb_entry_item(const struct fm_model *m, const struct fm_entry *e)
{ return e->bin && e->bin <= m->bin_count ? &m->bin[e->bin - 1] : NULL; }

/* ---- files ---- */
static int rb_read_file(const struct fm_vfs *fs, const char *path, unsigned char **out, size_t *size, size_t max, unsigned chunk)
{
    *out = NULL; *size = 0;
    int fd = fs->open(fs->ctx, path, 0);
    if (fd < 0) return fd;
    size_t cap = 4096, used = 0; unsigned char *buffer = malloc(cap + 1); int r = 0;
    if (!buffer) { fs->close(fs->ctx, fd); return -ENOMEM; }
    for (;;) {
        if (used == cap) {
            if (cap >= max) { r = -EFBIG; break; }
            cap *= 2; unsigned char *p = realloc(buffer, cap + 1);
            if (!p) { r = -ENOMEM; break; }
            buffer = p;
        }
        unsigned want = (unsigned)(cap - used);
        if (want > chunk) want = chunk;
        int got = fs->read(fs->ctx, fd, buffer + used, want);
        if (got == -EINTR) continue;
        if (got < 0) { r = got; break; }
        if (!got) break;
        used += (size_t)got;
    }
    int closed = fs->close(fs->ctx, fd);
    if (!r) r = closed;
    if (r) { free(buffer); return r; }
    buffer[used] = 0; *out = buffer; *size = used;
    return 0;
}
static int rb_write_all(const struct fm_vfs *fs, int fd, const unsigned char *data, size_t n)
{
    while (n) {
        int put = fs->write(fs->ctx, fd, data, n > FM_WRITE_BATCH ? FM_WRITE_BATCH : (unsigned)n);
        if (put == -EINTR) continue;
        if (put <= 0) return put ? put : -EIO;
        data += put; n -= (size_t)put;
    }
    return 0;
}
/* The previous content survives a failed write: exclusive temporary, sync,
 * replacing rename (the same publication as the file clipboard). */
static int rb_publish(const struct fm_vfs *fs, const char *path, const char *temp, const unsigned char *data, size_t n)
{
    fs->unlink(fs->ctx, temp);                                  /* a leftover of a crash */
    int fd = fs->open(fs->ctx, temp, 1);
    if (fd < 0) return fd;
    int r = rb_write_all(fs, fd, data, n);
    if (!r) r = fs->sync(fs->ctx, fd);
    int closed = fs->close(fs->ctx, fd);
    if (!r) r = closed;
    if (!r) r = fs->rename(fs->ctx, temp, path);
    if (r) fs->unlink(fs->ctx, temp);
    return r;
}

/* ---- policy (Recycle Bin Properties) ---- */
void rb_policy_default(struct rb_policy *p)
{
    memset(p, 0, sizeof(*p));
    p->confirm = 1; p->percent = RB_DEFAULT_PERCENT;
    for (unsigned i = 0; i < 26; i++) p->drive_percent[i] = RB_DEFAULT_PERCENT;
}
unsigned rb_policy_percent(const struct rb_policy *p, char drive)
{
    int i = rb_letter(drive);
    return p->independent && i >= 0 ? p->drive_percent[i] : p->percent;
}
int rb_policy_nuke(const struct rb_policy *p, char drive)
{
    int i = rb_letter(drive);
    return p->independent && i >= 0 ? p->drive_nuke[i] : p->nuke;
}
uint64_t rb_limit(const struct rb_policy *p, char drive)
{
    unsigned percent = rb_policy_percent(p, drive);
    if (percent > 100) percent = 100;
    uint64_t capacity = rb_capacity(drive);
    return capacity / 100u * percent + capacity % 100u * percent / 100u;
}
static unsigned rb_percent_value(const char *text)
{
    char *end; unsigned long v = strtoul(text, &end, 10);
    return end == text ? RB_DEFAULT_PERCENT : v < 1 ? 1 : v > 100 ? 100 : (unsigned)v;
}
int rb_policy_load(const struct fm_vfs *fs, struct rb_policy *p)
{
    rb_policy_default(p);
    unsigned char *data; size_t n;
    int r = rb_read_file(fs, RB_POLICY_FILE, &data, &n, 16384, 128);   /* tiny: read in small pieces, also on a UI thread */
    if (r == -ENOENT) return 0;
    if (r) return r;
    for (char *line = (char *)data; line && *line;) {
        char *next = strpbrk(line, "\r\n");
        if (next) { *next++ = 0; while (*next == '\r' || *next == '\n') next++; }
        char *eq = strchr(line, '=');
        if (eq && line[0] != '#') {
            *eq++ = 0;
            if (!strcmp(line, "confirm")) p->confirm = atoi(eq) != 0;
            else if (!strcmp(line, "independent")) p->independent = atoi(eq) != 0;
            else if (!strcmp(line, "nuke")) p->nuke = atoi(eq) != 0;
            else if (!strcmp(line, "percent")) p->percent = rb_percent_value(eq);
            else if (!strncmp(line, "drive.", 6) && line[6] && line[7] == '.' ) {
                int i = rb_letter(line[6]);
                if (i >= 0 && !strcmp(line + 8, "nuke")) p->drive_nuke[i] = atoi(eq) != 0;
                else if (i >= 0 && !strcmp(line + 8, "percent")) p->drive_percent[i] = rb_percent_value(eq);
            }
        }
        line = next;
    }
    free(data);
    return 0;
}
int rb_policy_save(const struct fm_vfs *fs, const struct rb_policy *p)
{
    char text[2048]; size_t at = 0;
    at += (size_t)snprintf(text + at, sizeof(text) - at, "# CiukiOS Recycle Bin\nconfirm=%d\nindependent=%d\nnuke=%d\npercent=%u\n",
                           p->confirm != 0, p->independent != 0, p->nuke != 0, p->percent < 1 ? 1 : p->percent > 100 ? 100 : p->percent);
    for (unsigned i = 2; i < 26 && at + 80 < sizeof(text); i++)
        at += (size_t)snprintf(text + at, sizeof(text) - at, "drive.%c.nuke=%d\ndrive.%c.percent=%u\n", 'a' + i, p->drive_nuke[i] != 0,
                               'a' + i, p->drive_percent[i] < 1 ? 1 : p->drive_percent[i] > 100 ? 100 : p->drive_percent[i]);
    int r = fs->mkdir(fs->ctx, "/system/settings");
    if (r && r != -EEXIST) return r;
    return rb_publish(fs, RB_POLICY_FILE, "/system/settings/recycle.cfg.new", (const unsigned char *)text, at);
}

/* ---- INFO2 and the bin of one drive ---- */
struct rb_bin { char drive; struct rb_item *items; unsigned count, capacity; uint32_t next; int dirty, locked; char dir[RB_DIRLEN]; };
static void rb_bin_free(struct rb_bin *b) { free(b->items); memset(b, 0, sizeof(*b)); }
static uint32_t rb_get32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void rb_put32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
#define RB_EPOCH 11644473600ll          /* seconds between 1601 and 1970 (FILETIME) */
static int rb_add(struct rb_bin *b, const struct rb_item *item)
{
    if (b->count == b->capacity) {
        unsigned cap = b->capacity ? b->capacity * 2 : 32;
        struct rb_item *p = realloc(b->items, cap * sizeof(*p));
        if (!p) return -ENOMEM;
        b->items = p; b->capacity = cap;
    }
    b->items[b->count++] = *item;
    return 0;
}
struct rb_dirent { char name[RB_NAME]; struct fm_meta meta; uint8_t used; };
struct rb_dirs { struct rb_dirent *v; unsigned count, capacity; };
static int rb_reserved(const char *name)
{
    return !compare(name, RB_INFO) || !compare(name, RB_INFO ".tmp") || !compare(name, RB_INFO ".lck") || !compare(name, "desktop.ini");
}
static int rb_dirs_emit(const char *name, const struct fm_meta *meta, void *ctx)
{
    struct rb_dirs *d = ctx;
    if (!strcmp(name, ".") || !strcmp(name, "..") || strlen(name) >= RB_NAME || rb_reserved(name)) return 0;
    if (d->count >= FM_ENTRIES + 64) return 0;
    if (d->count == d->capacity) {
        unsigned cap = d->capacity ? d->capacity * 2 : 64;
        struct rb_dirent *p = realloc(d->v, cap * sizeof(*p));
        if (!p) return -ENOMEM;
        d->v = p; d->capacity = cap;
    }
    struct rb_dirent *e = &d->v[d->count++];
    memset(e, 0, sizeof(*e)); snprintf(e->name, sizeof(e->name), "%s", name); e->meta = *meta;
    return 0;
}
static int rb_dirent_order(const void *x, const void *y)
{ return compare(((const struct rb_dirent *)x)->name, ((const struct rb_dirent *)y)->name); }
static struct rb_dirent *rb_dirent_find(struct rb_dirs *d, const char *name)
{
    struct rb_dirent key; snprintf(key.name, sizeof(key.name), "%s", name);
    if (!d->count) return NULL;
    return bsearch(&key, d->v, d->count, sizeof(*d->v), rb_dirent_order);
}
/* Parses D<l><digits>[.ext]; returns the record number or UINT32_MAX. */
static uint32_t rb_parse_stored(const char *name, char drive)
{
    if (fold((unsigned char)name[0]) != 'd' || fold((unsigned char)name[1]) != fold((unsigned char)drive) || name[2] < '0' || name[2] > '9') return UINT32_MAX;
    unsigned long long v = 0; const char *p = name + 2;
    while (*p >= '0' && *p <= '9' && v < 0xffffffffull) v = v * 10 + (unsigned)(*p++ - '0');
    return (!*p || *p == '.') && v < 0xffffffffull ? (uint32_t)v : UINT32_MAX;
}

/* Reads INFO2, keeps the records whose stored item exists, adopts stored
 * items that have no record (a crash between the rename and the INFO2 write)
 * as unrestorable "orphans". The caller holds the lock when it will save. */
static int rb_load(const struct fm_vfs *fs, char drive, struct rb_bin *b)
{
    memset(b, 0, sizeof(*b)); b->drive = rb_upper(drive);
    int r = rb_dir(b->drive, b->dir);
    if (r) return r;
    struct rb_dirs dirs = {0};
    r = fs->list(fs->ctx, b->dir, FM_LIST_METADATA, rb_dirs_emit, &dirs);
    if (r == -ENOENT) return 0;                                 /* no bin yet: empty */
    if (r) { free(dirs.v); return r; }
    if (dirs.count) qsort(dirs.v, dirs.count, sizeof(*dirs.v), rb_dirent_order);
    char path[FM_PATH]; unsigned char *data = NULL; size_t size = 0;
    r = rb_join(path, b->dir, RB_INFO);
    if (!r) r = rb_read_file(fs, path, &data, &size, (size_t)RB_HEADER + (size_t)RB_RECORD * (FM_ENTRIES + 64), 65536);
    if (r == -ENOENT) r = 0;
    if (r) { free(dirs.v); return r; }
    if (size >= RB_HEADER && rb_get32(data + 12) == RB_RECORD) {
        b->next = rb_get32(data + 4);
        for (size_t at = RB_HEADER; at + RB_RECORD <= size && b->count < FM_ENTRIES; at += RB_RECORD) {
            const unsigned char *rec = data + at;
            if (!rec[0]) continue;                              /* a freed record */
            struct rb_item item; memset(&item, 0, sizeof(item));
            memcpy(item.original, rec, RB_PATH - 1);
            item.index = rb_get32(rec + 260);
            item.drive = b->drive;
            uint64_t ft = (uint64_t)rb_get32(rec + 268) | (uint64_t)rb_get32(rec + 272) << 32;
            item.deleted = (int64_t)(ft / 10000000ull) - RB_EPOCH;
            item.size = rb_get32(rec + 276);
            rb_stored_name(b->drive, item.index, item.original, item.stored);
            struct rb_dirent *e = rb_dirent_find(&dirs, item.stored);
            if (!e || e->used) { b->dirty = 1; continue; }      /* emptied behind our back, or a duplicate number */
            e->used = 1; item.directory = e->meta.type == FM_DIR; item.modified = e->meta.modified;
            if (item.index >= b->next) b->next = item.index + 1;
            if ((r = rb_add(b, &item))) { free(data); free(dirs.v); return r; }
        }
    } else if (size) b->dirty = 1;                              /* unreadable header: rebuilt on the next save */
    free(data);
    for (unsigned i = 0; i < dirs.count; i++) if (!dirs.v[i].used && b->count < FM_ENTRIES) {
        struct rb_item item; memset(&item, 0, sizeof(item));
        uint32_t index = rb_parse_stored(dirs.v[i].name, b->drive);
        item.drive = b->drive; item.directory = dirs.v[i].meta.type == FM_DIR; item.size = dirs.v[i].meta.size;
        item.deleted = item.modified = dirs.v[i].meta.modified;
        snprintf(item.stored, sizeof(item.stored), "%s", dirs.v[i].name);
        item.index = index != UINT32_MAX ? index : 0xffff0000u + i;
        if (index != UINT32_MAX && index >= b->next) b->next = index + 1;
        if ((r = rb_add(b, &item))) { free(dirs.v); return r; }
    }
    free(dirs.v);
    if (!b->next) b->next = 1;
    return 0;
}
static int rb_save(const struct fm_vfs *fs, struct rb_bin *b)
{
    unsigned live = 0;
    for (unsigned i = 0; i < b->count; i++) live += b->items[i].original[0] != 0;
    size_t size = RB_HEADER + (size_t)live * RB_RECORD;
    unsigned char *buffer = calloc(1, size);
    if (!buffer) return -ENOMEM;
    rb_put32(buffer, 4);                                        /* Windows 95/98/ME INFO2 */
    rb_put32(buffer + 4, b->next);                              /* CiukiOS: the next record number */
    rb_put32(buffer + 8, live);
    rb_put32(buffer + 12, RB_RECORD);
    size_t at = RB_HEADER;
    for (unsigned i = 0; i < b->count; i++) if (b->items[i].original[0]) {
        const struct rb_item *item = &b->items[i]; unsigned char *rec = buffer + at;
        memcpy(rec, item->original, strnlen(item->original, RB_PATH - 1));
        rb_put32(rec + 260, item->index);
        rb_put32(rec + 264, (uint32_t)(rb_letter(b->drive)));
        uint64_t ft = (uint64_t)((item->deleted < 0 ? 0 : item->deleted) + RB_EPOCH) * 10000000ull;
        rb_put32(rec + 268, (uint32_t)ft); rb_put32(rec + 272, (uint32_t)(ft >> 32));
        rb_put32(rec + 276, item->size > 0xffffffffull ? 0xffffffffu : (uint32_t)item->size);
        at += RB_RECORD;
    }
    char path[FM_PATH], temp[FM_PATH];
    int r = rb_join(path, b->dir, RB_INFO);
    if (!r) r = rb_join(temp, b->dir, RB_INFO ".tmp");
    if (!r) r = rb_publish(fs, path, temp, buffer, size);
    free(buffer);
    if (!r) b->dirty = 0;
    return r;
}
static int rb_lock(const struct fm_vfs *fs, struct rb_bin *b)
{
    char path[FM_PATH]; int r = rb_join(path, b->dir, RB_INFO ".lck");
    if (r) return r;
    for (unsigned attempt = 0; attempt < 60; attempt++) {
        int fd = fs->open(fs->ctx, path, 1);
        if (fd >= 0) { fs->close(fs->ctx, fd); b->locked = 1; return 0; }
        if (fd != -EEXIST) return fd;
        struct fm_meta meta;
        if (!fs->stat_path(fs->ctx, path, &meta) && meta.modified && rb_now() - meta.modified > 30) { fs->unlink(fs->ctx, path); continue; }   /* abandoned */
        struct timespec delay = {.tv_sec = 0, .tv_nsec = 25000000};
        while (nanosleep(&delay, &delay) && errno == EINTR) {}
    }
    return -EBUSY;
}
static void rb_unlock(const struct fm_vfs *fs, struct rb_bin *b)
{
    if (!b->locked) return;
    char path[FM_PATH];
    if (!rb_join(path, b->dir, RB_INFO ".lck")) fs->unlink(fs->ctx, path);
    b->locked = 0;
}

/* ---- the legacy Trash folder of the pre-G2 desktop ---- */
static int rb_count_emit(const char *name, const struct fm_meta *meta, void *ctx)
{ (void)meta; if (strcmp(name, ".") && strcmp(name, "..")) (*(unsigned *)ctx)++; return 0; }
static unsigned rb_legacy_count(const struct fm_vfs *fs)
{
    unsigned n = 0;
    if (fs->list(fs->ctx, RB_LEGACY_TRASH, FM_LIST_NAMES_ONLY, rb_count_emit, &n)) return 0;
    return n;
}
struct rb_walk { const struct fm_vfs *fs; uint64_t bytes; unsigned visited; };
static void rb_tree_bytes(struct rb_walk *w, const char *path, unsigned depth)
{
    if (depth > FM_DEPTH) return;
    struct listing l = {0};
    if (w->fs->list(w->fs->ctx, path, FM_LIST_METADATA, collect, &l)) { free(l.entries); return; }
    for (unsigned i = 0; i < l.count && w->visited < 200000; i++) {
        w->visited++;
        if (l.entries[i].meta.type == FM_FILE) w->bytes += l.entries[i].meta.size;
        else if (l.entries[i].meta.type == FM_DIR) {
            char child[FM_PATH];
            if (!rb_join(child, path, l.entries[i].name)) rb_tree_bytes(w, child, depth + 1);
        }
    }
    free(l.entries);
}
/* The old Trash only ever received Desktop items: their original folder is
 * the Desktop. Items keep their content and become ordinary bin records. */
static int rb_migrate_into(const struct fm_vfs *fs, struct rb_bin *b)
{
    struct listing l = {0};
    int r = fs->list(fs->ctx, RB_LEGACY_TRASH, FM_LIST_METADATA, collect, &l);
    if (r) { free(l.entries); return r == -ENOENT ? 0 : r; }
    int64_t now = rb_now();
    char *scratch = malloc(2u * FM_PATH);
    if (!scratch) { free(l.entries); return -ENOMEM; }
    for (unsigned i = 0; i < l.count; i++) {
        const struct fm_entry *e = &l.entries[i];
        if (e->meta.type == FM_OTHER) continue;
        struct rb_item item; memset(&item, 0, sizeof(item));
        char original[RB_PATH + 8], *source = scratch, *target = scratch + FM_PATH;
        if (snprintf(original, sizeof(original), "C:\\Documents\\Desktop\\%s", e->name) >= RB_PATH) continue;
        strcpy(item.original, original);
        item.drive = b->drive; item.index = b->next; item.directory = e->meta.type == FM_DIR;
        item.deleted = e->meta.modified > 0 ? e->meta.modified : now; item.modified = e->meta.modified;
        if (e->meta.type == FM_FILE) item.size = e->meta.size;
        rb_stored_name(b->drive, item.index, original, item.stored);
        if (rb_join(source, RB_LEGACY_TRASH, e->name) || rb_join(target, b->dir, item.stored)) continue;
        if (e->meta.type == FM_DIR) { struct rb_walk w = {fs, 0, 0}; rb_tree_bytes(&w, source, 0); item.size = w.bytes; }
        if (fs->rename(fs->ctx, source, target)) continue;       /* stays in the Trash folder; tried again later */
        b->next++;
        if ((r = rb_add(b, &item))) { free(l.entries); free(scratch); return r; }
        b->dirty = 1;
    }
    free(l.entries); free(scratch);
    if (!rb_legacy_count(fs)) fs->rmdir(fs->ctx, RB_LEGACY_TRASH);
    return 0;
}

/* ---- opening a bin ---- */
static int rb_open(const struct fm_vfs *fs, char drive, int write, struct rb_bin *b)
{
    char dir[FM_PATH]; int r = rb_dir(drive, dir);
    if (r) return r;
    if (write) {
        r = fs->mkdir(fs->ctx, dir);
        if (r && r != -EEXIST) return r;
    }
    struct rb_bin lock = {0};
    memcpy(lock.dir, dir, strnlen(dir, RB_DIRLEN - 1)); lock.dir[RB_DIRLEN - 1] = 0;   /* "/mnt/x/RECYCLED" */
    if (write) { r = rb_lock(fs, &lock); if (r) return r; }
    r = rb_load(fs, drive, b);
    if (r) { rb_unlock(fs, &lock); return r; }
    b->locked = lock.locked;
    if (write && rb_upper(drive) == 'C' && rb_legacy_count(fs)) {
        r = rb_migrate_into(fs, b);
        if (r) { rb_unlock(fs, b); rb_bin_free(b); return r; }
    }
    return 0;
}
static int rb_close(const struct fm_vfs *fs, struct rb_bin *b, int save)
{
    int r = 0;
    if (b->locked && (save || b->dirty)) r = rb_save(fs, b);
    rb_unlock(fs, b); rb_bin_free(b);
    return r;
}
/* Listing: take the legacy items in first, under the lock. */
static int rb_peek(const struct fm_vfs *fs, char drive, struct rb_bin *b)
{
    if (rb_upper(drive) == 'C' && rb_legacy_count(fs)) {
        struct rb_bin w;
        int r = rb_open(fs, drive, 1, &w);
        if (r) return r;
        r = rb_close(fs, &w, 1);
        if (r) return r;
    }
    return rb_load(fs, drive, b);
}
int rb_migrate(const struct fm_vfs *fs)
{
    if (!rb_legacy_count(fs)) return 0;
    struct rb_bin b; int r = rb_open(fs, 'C', 1, &b);
    return r ? r : rb_close(fs, &b, 1);
}

unsigned rb_drives(const struct fm_vfs *fs, uint32_t fixed, char out[26])
{
    unsigned n = 0; out[n++] = 'C';
    for (unsigned i = 3; i < 26; i++) if (fixed & (1u << i)) {
        char root[FM_PATH]; struct fm_meta meta;
        if (!rb_root((char)('A' + i), root) && !fs->stat_path(fs->ctx, root, &meta) && meta.type == FM_DIR) out[n++] = (char)('A' + i);
    }
    return n;
}
int rb_usage(const struct fm_vfs *fs, char drive, struct rb_usage *u)
{
    memset(u, 0, sizeof(*u));
    struct rb_bin b; int r = rb_peek(fs, drive, &b);
    if (r) return r;
    u->items = b.count;
    for (unsigned i = 0; i < b.count; i++) u->bytes += b.items[i].size;
    rb_bin_free(&b);
    return 0;
}
static int rb_name_emit(const char *name, const struct fm_meta *meta, void *ctx)
{
    (void)meta;
    if (!strcmp(name, ".") || !strcmp(name, "..") || rb_reserved(name)) return 0;
    (*(unsigned *)ctx)++;
    return 0;
}
int rb_describe(const struct fm_vfs *fs, uint32_t fixed, unsigned *count, char name[FM_NAME], int *folder)
{
    char drives[26]; unsigned nd = rb_drives(fs, fixed, drives); int r = 0;
    *count = 0; name[0] = 0; *folder = 0;
    for (unsigned d = 0; d < nd; d++) {
        struct rb_bin b;
        r = rb_peek(fs, drives[d], &b);
        if (r == -ENOENT) { r = 0; continue; }
        if (r) return r;
        if (b.count == 1 && !*count) { rb_item_name(&b.items[0], name); *folder = b.items[0].directory; }
        *count += b.count;
        rb_bin_free(&b);
    }
    if (*count != 1) { name[0] = 0; *folder = 0; }
    return 0;
}
int rb_full(const struct fm_vfs *fs, uint32_t fixed)
{
    if (rb_legacy_count(fs)) return 1;
    char drives[26]; unsigned n = rb_drives(fs, fixed, drives);
    for (unsigned i = 0; i < n; i++) {
        char dir[FM_PATH], info[FM_PATH]; struct fm_meta meta;
        if (rb_dir(drives[i], dir) || rb_join(info, dir, RB_INFO)) continue;
        if (!fs->stat_path(fs->ctx, info, &meta) && meta.size > RB_HEADER) return 1;
        unsigned count = 0;
        if (!fs->list(fs->ctx, dir, FM_LIST_NAMES_ONLY, rb_name_emit, &count) && count) return 1;
    }
    return 0;
}

/* ---- the bin as a folder view ---- */
static int rb_list_into(const struct fm_vfs *fs, uint32_t fixed, struct listing *l)
{
    char drives[26]; unsigned nd = rb_drives(fs, fixed, drives);
    unsigned capacity = 0; int r = 0;
    for (unsigned d = 0; d < nd && !r; d++) {
        struct rb_bin b;
        r = rb_peek(fs, drives[d], &b);
        if (r == -ENOENT) { r = 0; continue; }
        if (r) break;
        for (unsigned i = 0; i < b.count && !r; i++) {
            if (l->count >= FM_ENTRIES) break;
            if (l->count == capacity) {
                capacity = capacity ? capacity * 2 : 32; if (capacity > FM_ENTRIES) capacity = FM_ENTRIES;
                struct fm_entry *pe = realloc(l->entries, capacity * sizeof(*pe));
                struct rb_item *pb = pe ? realloc(l->bin, capacity * sizeof(*pb)) : NULL;
                if (pe) l->entries = pe;
                if (!pe || !pb) { r = -ENOMEM; break; }
                l->bin = pb; l->capacity = capacity;
            }
            const struct rb_item *item = &b.items[i];
            struct fm_entry *e = &l->entries[l->count];
            memset(e, 0, sizeof(*e));
            snprintf(e->name, sizeof(e->name), "%s", item->stored);
            e->meta = (struct fm_meta){.type=item->directory ? FM_DIR : FM_FILE, .size=item->size,
                .ino=item->index, .dev=(uint32_t)item->drive, .mode=item->directory ? 0040777u : 0100666u,
                .modified=item->modified ? item->modified : item->deleted};
            l->bin[l->count] = *item;
            e->bin = ++l->count;
            l->bin_count = l->count;
        }
        rb_bin_free(&b);
    }
    if (r) { free(l->entries); free(l->bin); memset(l, 0, sizeof(*l)); }
    return r;
}

/* ---- deciding what to recycle ---- */
int rb_would_recycle(const struct rb_policy *policy, uint32_t fixed, unsigned flags, const char *path)
{
    if (flags & RB_PERMANENT) return 0;
    char drive = fm_drive_letter(path);
    if (!drive || rb_letter(drive) < 2) return 0;
    if (drive != 'C' && !(fixed & (1u << rb_letter(drive)))) return 0;          /* removable media: Windows 98 deletes at once */
    if (rb_policy_nuke(policy, drive)) return 0;
    char dir[FM_PATH];
    if (rb_dir(drive, dir) || fm_beneath(path, dir)) return 0;                  /* inside a bin: a permanent delete */
    if (fm_beneath(path, RB_LEGACY_TRASH)) return 0;
    return 1;
}
unsigned rb_classify(const struct rb_policy *policy, uint32_t fixed, unsigned flags, char *const *paths, unsigned count)
{
    unsigned n = 0;
    for (unsigned i = 0; i < count; i++) n += rb_would_recycle(policy, fixed, flags, paths[i]) != 0;
    return n;
}
void rb_delete_prompt(unsigned count, int folder, const char *name, int recycle, char *title, size_t title_size, char *text, size_t text_size)
{
    if (!count) {   /* Empty Recycle Bin when the number of items is not at hand */
        snprintf(title, title_size, "Confirm Multiple File Delete");
        snprintf(text, text_size, "Are you sure you want to delete all of the items in the Recycle Bin?");
    } else if (count > 1) {
        snprintf(title, title_size, "Confirm Multiple File Delete");
        snprintf(text, text_size, recycle ? "Are you sure you want to send these %u items to the Recycle Bin?" : "Are you sure you want to delete these %u items?", count);
    } else if (folder) {
        snprintf(title, title_size, "Confirm Folder Delete");
        snprintf(text, text_size, recycle ? "Are you sure you want to remove the folder '%.120s' and move all its contents to the Recycle Bin?" :
                 "Are you sure you want to remove the folder '%.120s' and all its contents?", name);
    } else {
        snprintf(title, title_size, "Confirm File Delete");
        snprintf(text, text_size, recycle ? "Are you sure you want to send '%.120s' to the Recycle Bin?" : "Are you sure you want to delete '%.120s'?", name);
    }
}
const char *rb_refusal_text(int reason)
{
    switch (reason) {
    case RB_REFUSE_LARGE: return "is too large for the Recycle Bin and was not deleted.\nIncrease the Recycle Bin's maximum size, or hold Shift while you delete to remove it permanently.";
    case RB_REFUSE_LONG: return "has a path that is too long for the Recycle Bin and was not deleted.\nHold Shift while you delete to remove it permanently.";
    case RB_REFUSE_UNKNOWN: return "cannot be restored because its original location is not known.\nDrag it out of the Recycle Bin instead.";
    default: return "was not deleted.";
    }
}

/* ---- jobs ---- */
static void rb_job_free(struct fm_job *j)
{
    struct rb_job *b = j->bin;
    if (!b) return;
    for (unsigned i = 0; b->target && i < j->count; i++) free(b->target[i]);
    free(b->target); free(b->item); free(b->recycle); free(b->done); free(b->refused);
    free(b); j->bin = NULL;
}
static int rb_job_attach(struct fm_job *j, enum rb_mode mode, const struct rb_policy *policy, uint32_t fixed)
{
    struct rb_job *b = calloc(1, sizeof(*b));
    if (!b) return -ENOMEM;
    b->mode = mode; if (policy) b->policy = *policy; else rb_policy_default(&b->policy);
    b->fixed = fixed | (1u << 2);
    b->target = calloc(j->count, sizeof(*b->target)); b->item = calloc(j->count, sizeof(*b->item));
    b->recycle = calloc(j->count, 1); b->done = calloc(j->count, 1); b->refused = calloc(j->count, 1);
    if (!b->target || !b->item || !b->recycle || !b->done || !b->refused) {
        free(b->target); free(b->item); free(b->recycle); free(b->done); free(b->refused); free(b);
        return -ENOMEM;
    }
    j->bin = b;
    return 0;
}
static void rb_refuse(struct fm_job *j, unsigned i, int reason)
{
    struct rb_job *b = j->bin;
    b->refused[i] = 1; b->recycle[i] = 0; b->refused_count++;
    if (!b->refused_reason) { b->refused_reason = reason; snprintf(b->refused_name, sizeof(b->refused_name), "%s", fm_basename(j->sources[i])); }
}
int rb_delete_job(struct fm_job *j, const struct fm_vfs *fs, char *const *paths, unsigned count,
                  const struct rb_policy *policy, uint32_t fixed, unsigned flags)
{
    for (unsigned i = 0; i < count; i++) if (fm_drive_root(paths[i])) return -EBUSY;
    int r = fm_job_start(j, fs, FM_DELETE, paths, count, NULL);
    if (r) return r;
    r = rb_job_attach(j, RB_RECYCLE, policy, fixed);
    if (r) { fm_job_destroy(j); return r; }
    for (unsigned i = 0; i < count; i++) j->bin->recycle[i] = (uint8_t)(rb_would_recycle(&j->bin->policy, j->bin->fixed, flags, paths[i]) != 0);
    return 0;
}
/* Items already inside a bin: stored paths. */
static int rb_inside_bin(char *const *paths, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        char drive = fm_drive_letter(paths[i]), dir[FM_PATH];
        if (!drive || rb_dir(drive, dir) || !fm_beneath(paths[i], dir) || fm_same_path(paths[i], dir)) return 0;
    }
    return 1;
}
int rb_purge_job(struct fm_job *j, const struct fm_vfs *fs, char *const *paths, unsigned count)
{
    if (!count || !rb_inside_bin(paths, count)) return -EINVAL;
    int r = fm_job_start(j, fs, FM_DELETE, paths, count, NULL);
    if (r) return r;
    r = rb_job_attach(j, RB_PURGE, NULL, 0);
    if (r) fm_job_destroy(j);
    return r;
}
int rb_restore_job(struct fm_job *j, const struct fm_vfs *fs, char *const *paths, unsigned count)
{
    if (!count || !rb_inside_bin(paths, count)) return -EINVAL;
    int r = fm_job_start(j, fs, FM_MOVE, paths, count, "/");
    if (r) return r;
    r = rb_job_attach(j, RB_RESTORE, NULL, 0);
    if (r) fm_job_destroy(j);
    return r;
}
struct rb_names { char **v; unsigned count, capacity; const char *dir; };
static int rb_names_emit(const char *name, const struct fm_meta *meta, void *ctx)
{
    struct rb_names *n = ctx; (void)meta;
    if (!strcmp(name, ".") || !strcmp(name, "..") || rb_reserved(name)) return 0;
    if (n->count >= FM_ENTRIES) return -E2BIG;
    if (n->count == n->capacity) {
        unsigned cap = n->capacity ? n->capacity * 2 : 32;
        char **p = realloc(n->v, cap * sizeof(*p));
        if (!p) return -ENOMEM;
        n->v = p; n->capacity = cap;
    }
    char *path = malloc(FM_PATH);
    if (!path) return -ENOMEM;
    if (rb_join(path, n->dir, name)) { free(path); return 0; }
    n->v[n->count++] = path;
    return 0;
}
int rb_empty_job(struct fm_job *j, const struct fm_vfs *fs, uint32_t fixed)
{
    char drives[26]; unsigned nd = rb_drives(fs, fixed, drives);
    struct rb_names names = {0}; int r = 0;
    for (unsigned d = 0; d < nd && !r; d++) {
        char dir[FM_PATH]; if (rb_dir(drives[d], dir)) continue;
        names.dir = dir;
        r = fs->list(fs->ctx, dir, FM_LIST_NAMES_ONLY, rb_names_emit, &names);
        if (r == -ENOENT) r = 0;
    }
    if (!r && !names.count) r = -ENOENT;                         /* nothing to empty */
    if (!r) r = rb_purge_job(j, fs, names.v, names.count);
    for (unsigned i = 0; i < names.count; i++) free(names.v[i]);
    free(names.v);
    return r;
}

/* mkdir -p for a restore into a folder that has been removed since. */
static int rb_mkdir_p(const struct fm_vfs *fs, const char *path)
{
    char partial[FM_PATH]; size_t n = strlen(path);
    if (n >= FM_PATH) return -ENAMETOOLONG;
    for (size_t i = 1; i <= n; i++) if (path[i] == '/' || !path[i]) {
        memcpy(partial, path, i); partial[i] = 0;
        struct fm_meta meta;
        int r = fs->stat_path(fs->ctx, partial, &meta);
        if (!r) { if (meta.type != FM_DIR) return -ENOTDIR; continue; }
        if (r != -ENOENT) return r;
        r = fs->mkdir(fs->ctx, partial);
        if (r && r != -EEXIST) return r;
    }
    return 0;
}
__attribute__((noinline)) static int rb_begin_restore(struct fm_job *j)
{
    struct rb_job *b = j->bin;
    struct rb_bin *bins = calloc(26, sizeof(*bins)); uint32_t loaded = 0; int r = 0, first = 1;
    if (!bins) return -ENOMEM;
    for (unsigned i = 0; i < j->count && !r; i++) {
        char drive = fm_drive_letter(j->sources[i]); int di = rb_letter(drive);
        if (di < 0) continue;
        if (!(loaded & (1u << di))) { r = rb_peek(&j->fs, drive, &bins[di]); if (r) break; loaded |= 1u << di; }
        const struct rb_item *item = NULL;
        for (unsigned k = 0; k < bins[di].count; k++) if (!compare(bins[di].items[k].stored, fm_basename(j->sources[i]))) item = &bins[di].items[k];
        char posix[FM_PATH], parent[FM_PATH];
        if (!item || !item->original[0] || fm_posix_path(posix, item->original) || !posix[0] || fm_parent(parent, posix)) { rb_refuse(j, i, RB_REFUSE_UNKNOWN); continue; }
        r = rb_mkdir_p(&j->fs, parent);
        if (r) break;
        b->target[i] = strdup(posix);
        if (!b->target[i]) { r = -ENOMEM; break; }
        if (first) { snprintf(j->destination, sizeof(j->destination), "%s", parent); first = 0; }
    }
    for (unsigned d = 0; d < 26; d++) rb_bin_free(&bins[d]);
    free(bins);
    return r;
}
__attribute__((noinline)) static int rb_job_begin(struct fm_job *j)
{
    struct rb_job *b = j->bin;
    if (b->begun) return 0;
    b->begun = 1; b->now = rb_now();
    if (b->mode == RB_RESTORE) return rb_begin_restore(j);
    if (b->mode != RB_RECYCLE) return 0;
    uint32_t seen = 0;
    for (unsigned i = 0; i < j->count; i++) {
        if (!b->recycle[i]) continue;
        char drive = fm_drive_letter(j->sources[i]); int di = rb_letter(drive);
        if (di < 0 || (seen & (1u << di))) continue;
        seen |= 1u << di;
        struct rb_bin bin; int r = rb_open(&j->fs, drive, 1, &bin);
        if (r) return r;
        uint64_t budget = rb_limit(&b->policy, drive);
        b->limit_drive = rb_upper(drive); b->limit_percent = rb_policy_percent(&b->policy, drive); b->limit_bytes = budget;
        for (unsigned k = i; k < j->count; k++) {
            if (!b->recycle[k] || fm_drive_letter(j->sources[k]) != drive) continue;
            char display[FM_PATH];
            if (fm_display_path(display, j->sources[k]) || strlen(display) >= RB_PATH) { rb_refuse(j, k, RB_REFUSE_LONG); continue; }
            uint64_t size = j->totals[k].bytes;
            if (size > budget) { rb_refuse(j, k, RB_REFUSE_LARGE); continue; }
            budget -= size;
            struct rb_item *item = &b->item[k];
            item->drive = rb_upper(drive); item->index = bin.next++; item->size = size; item->deleted = b->now;
            item->directory = 0; memcpy(item->original, display, strlen(display) + 1);   /* shorter than RB_PATH: checked above */
            rb_stored_name(drive, item->index, display, item->stored);
            char target[FM_PATH];
            if (rb_join(target, bin.dir, item->stored)) { rb_close(&j->fs, &bin, 0); return -ENAMETOOLONG; }
            b->target[k] = strdup(target);
            if (!b->target[k]) { rb_close(&j->fs, &bin, 0); return -ENOMEM; }
        }
        bin.dirty = 1;                                           /* the reserved record numbers */
        r = rb_close(&j->fs, &bin, 1);
        if (r) return r;
    }
    return 0;
}
static int rb_oldest(const struct rb_bin *b, const uint8_t *keep)
{
    int best = -1;
    for (unsigned i = 0; i < b->count; i++) {
        if (keep[i]) continue;
        if (best < 0 || b->items[i].deleted < b->items[best].deleted ||
            (b->items[i].deleted == b->items[best].deleted && b->items[i].index < b->items[best].index)) best = (int)i;
    }
    return best;
}
/* Removes stored items from disk for good: an ordinary job, run here. */
static void rb_destroy_stored(const struct fm_vfs *fs, char **paths, unsigned count)
{
    if (!count) return;
    struct fm_job *sub = calloc(1, sizeof(*sub));
    if (!sub) return;
    if (!fm_job_start(sub, fs, FM_DELETE, paths, count, NULL)) {
        for (unsigned i = 0; i < 50000000u && sub->state == FM_RUNNING; i++) fm_job_step(sub);
        fm_job_destroy(sub);
    }
    free(sub);
}
__attribute__((noinline)) static void rb_job_finish(struct fm_job *j)
{
    struct rb_job *b = j->bin;
    if (b->finished) return;
    b->finished = 1;
    if (!b->begun) return;
    uint32_t seen = 0;
    for (unsigned i = 0; i < j->count; i++) {
        char drive = fm_drive_letter(j->sources[i]); int di = rb_letter(drive);
        if (di < 0 || (seen & (1u << di)) || !b->done[i] || (b->mode == RB_RECYCLE && !b->recycle[i])) continue;
        seen |= 1u << di;
        struct rb_bin bin;
        if (rb_open(&j->fs, drive, 1, &bin)) continue;
        if (b->mode == RB_RECYCLE) {
            uint8_t *keep = NULL; char **gone = NULL; unsigned ngone = 0; int ok = 1;
            for (unsigned k = i; k < j->count; k++) {
                if (fm_drive_letter(j->sources[k]) != drive || !b->done[k] || !b->recycle[k]) continue;
                b->item[k].modified = b->now;
                /* The load adopted this job's own finished item as an orphan (its record was not written yet). */
                int replaced = 0;
                for (unsigned x = 0; x < bin.count && !replaced; x++)
                    if (!bin.items[x].original[0] && !compare(bin.items[x].stored, b->item[k].stored)) { bin.items[x] = b->item[k]; replaced = 1; }
                if (!replaced && rb_add(&bin, &b->item[k])) { ok = 0; break; }
            }
            if (ok) {
                keep = calloc(bin.count ? bin.count : 1, 1);
                gone = calloc(bin.count ? bin.count : 1, sizeof(*gone));
            }
            if (keep && gone) {
                uint64_t total = 0, limit = rb_limit(&b->policy, drive);
                for (unsigned k = 0; k < bin.count; k++) total += bin.items[k].size;
                /* This job's own items are never purged (they fit the limit when vetted). */
                for (unsigned k = 0; k < bin.count; k++) for (unsigned s = 0; s < j->count; s++)
                    if (b->done[s] && b->recycle[s] && bin.items[k].index == b->item[s].index && bin.items[k].drive == b->item[s].drive &&
                        !strcmp(bin.items[k].stored, b->item[s].stored)) keep[k] = 1;
                while (total > limit) {
                    int victim = rb_oldest(&bin, keep);
                    if (victim < 0) break;
                    char path[FM_PATH];
                    if (!rb_join(path, bin.dir, bin.items[victim].stored)) {
                        gone[ngone] = strdup(path);
                        if (gone[ngone]) ngone++;
                    }
                    total -= bin.items[victim].size < total ? bin.items[victim].size : total;
                    keep[victim] = 1;                              /* handled; remove from the list below */
                    bin.items[victim].original[0] = 0; bin.items[victim].stored[0] = 0;
                }
                /* Drop the purged records (original cleared) from the in-memory list. */
                unsigned out = 0;
                for (unsigned k = 0; k < bin.count; k++) if (bin.items[k].stored[0] || bin.items[k].original[0]) bin.items[out++] = bin.items[k];
                bin.count = out;
            }
            bin.dirty = 1;
            rb_close(&j->fs, &bin, 1);
            rb_destroy_stored(&j->fs, gone, ngone);
            for (unsigned k = 0; gone && k < ngone; k++) free(gone[k]);
            free(gone); free(keep);
        } else {
            /* PURGE and RESTORE: the records of the items that left the bin. */
            for (unsigned k = i; k < j->count; k++) {
                if (fm_drive_letter(j->sources[k]) != drive || !b->done[k]) continue;
                for (unsigned x = 0; x < bin.count; x++) if (!compare(bin.items[x].stored, fm_basename(j->sources[k]))) {
                    memmove(&bin.items[x], &bin.items[x + 1], (bin.count - x - 1) * sizeof(bin.items[0])); bin.count--; break;
                }
            }
            bin.dirty = 1;
            rb_close(&j->fs, &bin, 1);
        }
    }
}
