/* SPDX-License-Identifier: MIT
 * Included after recycle.inc to share G2's bin lock and INFO2 transaction. */
#include "cleanup.h"
#define CLEAN_AGE (7ll * 24 * 60 * 60)
#define CLEAN_PARENT_NONE UINT32_MAX
struct clean_entry {
    char *path;
    struct fm_meta meta;
    unsigned parent, category;
    int anchor, bin_top;
};
struct fm_cleanup {
    struct fm_vfs fs;
    struct clean_entry *entries;
    unsigned count, capacity, next, selected;
    char drive;
    uint32_t dev;
    int64_t now;
    fm_cleanup_cancelled cancelled;
    void *cancel_ctx;
    struct fm_cleanup_category categories[FM_CLEAN_CATEGORIES];
    struct fm_cleanup_progress progress;
};
static int clean_cancelled(struct fm_cleanup *p)
{ return p->cancelled && p->cancelled(p->cancel_ctx); }
static int clean_safe(const struct fm_cleanup *p, const struct fm_meta *m)
{
    return m->dev == p->dev && (m->type == FM_DIR || m->type == FM_FILE) &&
           !(m->attributes & (1u | 4u)) && (m->mode & 0200u);
}
static int clean_anchor_safe(const struct fm_cleanup *p, const struct fm_meta *m)
{ return m->dev == p->dev && m->type == FM_DIR && !(m->attributes & 1u) && (m->mode & 0200u); }
static void clean_error(struct fm_cleanup *p, int r)
{
    p->progress.errors++;
    if (!p->progress.first_error) p->progress.first_error = r;
}
static int clean_add(struct fm_cleanup *p, const char *path, const struct fm_meta *m,
                     unsigned parent, unsigned category, int anchor, int top)
{
    if (p->count >= FM_ENTRIES) return -E2BIG;
    if (p->count == p->capacity) {
        unsigned cap = p->capacity ? p->capacity * 2 : 32;
        struct clean_entry *v = realloc(p->entries, cap * sizeof(*v));
        if (!v) return -ENOMEM;
        p->entries = v; p->capacity = cap;
    }
    char *copy = strdup(path);
    if (!copy) return -ENOMEM;
    p->entries[p->count++] = (struct clean_entry){copy, *m, parent, category, anchor, top};
    if (m->type == FM_FILE && !anchor) {
        struct fm_cleanup_category *c = &p->categories[category];
        if (UINT64_MAX - c->bytes < m->size) return -EOVERFLOW;
        c->bytes += m->size; c->items++;
    }
    return 0;
}
struct clean_walk { struct fm_cleanup *p; unsigned parent, depth, category; int bin_root; };
static int clean_walk_dir(struct fm_cleanup *, unsigned, unsigned, int);
static int clean_emit(const char *name, const struct fm_meta *listed, void *ctx)
{
    struct clean_walk *w = ctx; struct fm_cleanup *p = w->p;
    (void)listed;
    if (clean_cancelled(p)) return -ECANCELED;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    if (!*name || strchr(name, '/') || strchr(name, '\\')) return -EINVAL;
    if (w->bin_root && rb_reserved(name)) return 0;
    char path[FM_PATH];
    int r = fm_join(path, p->entries[w->parent].path, name);
    if (r) return r;
    struct fm_meta m;
    if (clean_cancelled(p)) return -ECANCELED;
    r = p->fs.stat_path(p->fs.ctx, path, &m);
    if (r) { clean_error(p, r); return 0; }
    if (fm_volume(path) != p->drive || !clean_safe(p, &m)) return 0;
    if (m.type == FM_FILE && w->category == FM_CLEAN_TEMP &&
        (m.modified <= 0 || m.modified > p->now || p->now - m.modified < CLEAN_AGE)) return 0;
    if (m.type == FM_DIR && w->depth >= FM_DEPTH) return -E2BIG;
    unsigned index = p->count;
    r = clean_add(p, path, &m, w->parent, w->category, 0, w->bin_root);
    if (r) return r;
    return m.type == FM_DIR ? clean_walk_dir(p, index, w->depth + 1, 0) : 0;
}
static int clean_walk_dir(struct fm_cleanup *p, unsigned parent, unsigned depth, int bin_root)
{
    if (clean_cancelled(p)) return -ECANCELED;
    struct clean_walk w = {p, parent, depth, p->entries[parent].category, bin_root};
    return p->fs.list(p->fs.ctx, p->entries[parent].path, FM_LIST_METADATA, clean_emit, &w);
}
static int clean_root(struct fm_cleanup *p, const char *path, unsigned category)
{
    char prefix[FM_PATH];
    unsigned parent = CLEAN_PARENT_NONE;
    size_t start = p->drive == 'C' ? 1 : 6;
    size_t length = strlen(path);
    for (size_t at = start; at <= length; at++) {
        if (at != start && at != length && path[at] != '/') continue;
        if (clean_cancelled(p)) return -ECANCELED;
        memcpy(prefix, path, at); prefix[at] = 0;
        struct fm_meta m;
        int r = p->fs.stat_path(p->fs.ctx, prefix, &m);
        if (r == -ENOENT) return 0;
        if (r) { clean_error(p, r); return 0; }
        if (!clean_anchor_safe(p, &m)) return 0;
        unsigned index = p->count;
        r = clean_add(p, prefix, &m, parent, category, 1, 0);
        if (r) return r;
        parent = index;
    }
    return clean_walk_dir(p, parent, 0, category == FM_CLEAN_RECYCLE);
}
int fm_cleanup_scan_cancelable(const struct fm_vfs *fs, char drive, int64_t now,
                              fm_cleanup_cancelled cancelled, void *ctx, struct fm_cleanup **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    int di = rb_letter(drive);
    if (!fs || !fs->stat_path || !fs->list || !fs->unlink || !fs->rmdir || di < 2 || now <= 0) return -EINVAL;
    struct fm_cleanup *p = calloc(1, sizeof(*p));
    if (!p) return -ENOMEM;
    p->fs = *fs; p->drive = (char)('A' + di); p->now = now;
    p->cancelled = cancelled; p->cancel_ctx = ctx;
    char root[FM_PATH]; struct fm_meta m;
    int r = rb_root(p->drive, root);
    if (!r && clean_cancelled(p)) r = -ECANCELED;
    if (!r) r = fs->stat_path(fs->ctx, root, &m);
    if (!r && m.type != FM_DIR) r = -ENOTDIR;
    if (!r) p->dev = m.dev;
    p->categories[FM_CLEAN_TEMP].available = 1;
    p->categories[FM_CLEAN_RECYCLE].available = !!(fs->open && fs->read && fs->write && fs->close && fs->sync && fs->mkdir && fs->rename);
    char path[FM_PATH];
    if (!r && p->drive == 'C') r = clean_root(p, "/tmp", FM_CLEAN_TEMP);
    if (!r) { r = fm_join(path, root, "TEMP"); if (!r) r = clean_root(p, path, FM_CLEAN_TEMP); }
    if (!r) { r = rb_join(path, root, "system/TEMP"); if (!r) r = clean_root(p, path, FM_CLEAN_TEMP); }
    if (!r && p->categories[FM_CLEAN_RECYCLE].available) {
        char bin[RB_DIRLEN]; r = rb_dir(p->drive, bin);
        if (!r) r = clean_root(p, bin, FM_CLEAN_RECYCLE);
    }
    if (r) { fm_cleanup_destroy(p); return r; }
    p->cancelled = NULL; p->cancel_ctx = NULL;
    p->progress.state = FM_CLEAN_READY; *out = p; return 0;
}
int fm_cleanup_scan(const struct fm_vfs *fs, char drive, int64_t now, struct fm_cleanup **out)
{ return fm_cleanup_scan_cancelable(fs, drive, now, NULL, NULL, out); }
const struct fm_cleanup_category *fm_cleanup_category(const struct fm_cleanup *p, unsigned category)
{ return p && category < FM_CLEAN_CATEGORIES ? &p->categories[category] : NULL; }
int fm_cleanup_start(struct fm_cleanup *p, unsigned mask)
{
    if (!p || p->progress.state != FM_CLEAN_READY || mask >> FM_CLEAN_CATEGORIES) return -EINVAL;
    for (unsigned i = 0; i < FM_CLEAN_CATEGORIES; i++)
        if ((mask & (1u << i)) && !p->categories[i].available) return -EINVAL;
    p->selected = mask; p->next = p->count; p->progress.state = FM_CLEAN_RUNNING; return 0;
}
static int clean_identity(const struct fm_meta *a, const struct fm_meta *b)
{ return a->ino == b->ino && a->dev == b->dev && a->type == b->type && a->mode == b->mode && a->attributes == b->attributes; }
static int clean_check(struct fm_cleanup *p, unsigned index)
{
    unsigned at = index;
    while (at != CLEAN_PARENT_NONE) {
        struct clean_entry *e = &p->entries[at]; struct fm_meta m;
        int r = p->fs.stat_path(p->fs.ctx, e->path, &m);
        if (r) return r;
        if (!(e->anchor ? clean_anchor_safe(p, &m) : clean_safe(p, &m)) || !clean_identity(&e->meta, &m)) return -EAGAIN;
        if (at == index && m.type == FM_FILE && (m.size != e->meta.size || m.modified != e->meta.modified)) return -EAGAIN;
        at = e->parent;
    }
    return 0;
}
static int clean_remove(struct fm_cleanup *p, struct clean_entry *e)
{
    /* Check before opening the bin too: a replaced ancestor must not even
     * receive an INFO2 lock file. Check again after acquiring that lock. */
    struct rb_bin bin = {0}; int locked = 0;
    int r = clean_check(p, (unsigned)(e - p->entries));
    if (r) return r;
    if (e->category == FM_CLEAN_RECYCLE) {
        r = rb_open(&p->fs, p->drive, 1, &bin);
        if (r) { rb_bin_free(&bin); return r; }
        locked = 1;
    }
    r = clean_check(p, (unsigned)(e - p->entries));
    if (!r) r = e->meta.type == FM_DIR ? p->fs.rmdir(p->fs.ctx, e->path) : p->fs.unlink(p->fs.ctx, e->path);
    if (!r) {
        if (e->meta.type == FM_FILE) { p->progress.items++; p->progress.bytes += e->meta.size; }
        if (locked && e->bin_top) {
            for (unsigned k = 0; k < bin.count; k++) if (!compare(bin.items[k].stored, fm_basename(e->path))) {
                memmove(&bin.items[k], &bin.items[k + 1], (bin.count - k - 1) * sizeof(*bin.items));
                bin.count--; bin.dirty = 1; break;
            }
        }
    }
    if (locked) { int saved = rb_close(&p->fs, &bin, 0); if (!r) r = saved; }
    return r;
}
int fm_cleanup_step(struct fm_cleanup *p)
{
    if (!p || p->progress.state != FM_CLEAN_RUNNING) return 0;
    if (!p->next) { p->progress.state = FM_CLEAN_DONE; return 0; }
    struct clean_entry *e = &p->entries[--p->next];
    if ((p->selected & (1u << e->category)) && !e->anchor &&
        (e->meta.type == FM_FILE || e->category == FM_CLEAN_RECYCLE)) {
        int r = clean_remove(p, e);
        if (r == -EAGAIN || r == -ENOENT) p->progress.skipped++;
        else if (r) clean_error(p, r);
    }
    return 1;
}
void fm_cleanup_cancel(struct fm_cleanup *p)
{ if (p && p->progress.state == FM_CLEAN_RUNNING) p->progress.state = FM_CLEAN_CANCELLED; }
void fm_cleanup_progress(const struct fm_cleanup *p, struct fm_cleanup_progress *out)
{ if (out) { if (p) *out = p->progress; else memset(out, 0, sizeof(*out)); } }
void fm_cleanup_destroy(struct fm_cleanup *p)
{
    if (!p) return;
    for (unsigned i = 0; i < p->count; i++) free(p->entries[i].path);
    free(p->entries); free(p);
}
uint64_t fm_cleanup_low_threshold(uint64_t capacity)
{
    const uint64_t mib = 1024ull * 1024;
    if (capacity <= 512 * mib) return 25 * mib;
    if (capacity >= 2048 * mib) return 65 * mib;
    return 25 * mib + (capacity - 512 * mib) * 40 / 1536;
}
int fm_cleanup_space_warning(uint32_t *seen, char drive, uint64_t capacity, uint64_t free_bytes)
{
    int di = rb_letter(drive);
    if (!seen || di < 0 || !capacity || free_bytes > capacity) return FM_SPACE_OK;
    if (!free_bytes) return FM_SPACE_FULL;
    if (free_bytes >= fm_cleanup_low_threshold(capacity) || (*seen & (1u << di))) return FM_SPACE_OK;
    *seen |= 1u << di; return FM_SPACE_LOW;
}
