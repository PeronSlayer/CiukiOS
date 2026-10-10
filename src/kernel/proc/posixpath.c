/* POSIX names over F1's drive namespace. All FAT access and node changes
 * share vfs->lock with legacy callers. See FILES-REPORT.md for sources.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/clock.h>
#include <ciuki/fat_native.h>

static struct px_namespace *spaces;

int px_error(int e)
{
    if (e == -FS_EQUARANTINED || e == -FS_EUCLEAN || e == -FS_ELOOP) return -EIO;
    return e;
}

static bool directory(const struct px_node *n)
{
    return n->kind == PX_DIRECTORY || n->kind == PX_MNT || n->kind == PX_DEV;
}
/* A cwd retains its identity after detach, but cannot resolve through an
 * expired mount, including . and ... Synthetic nodes belong to the lifetime
 * of the system root without being volume-backed open descriptions. */
static bool node_available(const struct px_node *n)
{
    if (!n || !n->linked) return false;
    if (!n->volume) n = n->space->root;
    return n && n->linked && n->volume && n->volume->mounted &&
        n->space->vfs->volumes[n->drive] == n->volume &&
        n->space->vfs->generation[n->drive] == n->generation;
}
static uint32_t volume_root(const struct fat_volume *v) { return v->type == 32 ? v->root : 0; }
static struct px_namespace *space_find(struct vfs *v)
{
    for (struct px_namespace *s = spaces; s; s = s->next) if (s->vfs == v) return s;
    return 0;
}
static struct px_node *node_new(struct px_namespace *s, struct px_node *parent, enum px_kind kind)
{
    if (s->next_ino == UINT64_MAX) return 0;
    struct px_node *n = kzalloc(sizeof(*n));
    if (!n) return 0;
    n->space = s; n->parent = parent; n->kind = kind; n->linked = true;
    n->ino = ++s->next_ino; n->next = s->nodes; s->nodes = n;
    return n;
}
/* Named identities survive close until mount teardown. Removed identities
 * need only their monotonically allocated number, not a permanent heap node.
 * Keep a removed parent while an open-unlinked child still points to it. */
static void node_forget(struct px_node *n)
{
    while (n && !n->linked && !n->refs && !n->legacy && !n->mountpoint) {
        struct px_namespace *s = n->space;
        for (struct px_node *child = s->nodes; child; child = child->next)
            if (child != n && child->parent == n) return;
        struct px_node **at = &s->nodes;
        while (*at && *at != n) at = &(*at)->next;
        if (*at) *at = n->next;
        struct px_node *parent = n->parent;
        kfree(n); n = parent;
    }
}
static bool entry_same(const struct fat_entry *a, const struct fat_entry *b)
{
    return a->parent == b->parent && a->index == b->index;
}
static struct px_node *node_find(struct px_namespace *s, struct fat_volume *v, const struct fat_entry *entry)
{
    for (struct px_node *n = s->nodes; n; n = n->next)
        if (n->linked && n->volume == v && entry_same(&n->entry, entry)) return n;
    return 0;
}
static struct px_node *node_entry(struct px_namespace *s, struct px_node *parent, const struct fat_entry *entry)
{
    struct px_node *n = node_find(s, parent->volume, entry);
    if (n) { n->parent = parent; return n; }
    n = node_new(s, parent, entry->attr & FAT_ATTR_DIR ? PX_DIRECTORY : PX_FILE);
    if (!n) return 0;
    n->volume = parent->volume; n->drive = parent->drive; n->generation = parent->generation;
    n->entry = *entry;
    for (unsigned i = 0; i < VFS_NODES; i++) {
        struct vfs_node *old = &s->vfs->nodes[i];
        if (old->refs && old->volume == n->volume && entry_same(&old->entry, entry)) {
            n->legacy = old; n->entry = old->entry; break;
        }
    }
    clock_fat_utc(n->entry.times.write_date, n->entry.times.write_time, &n->ctime.tv_sec);
    return n;
}
static struct px_node *mount_root(struct px_namespace *s, unsigned drive)
{
    struct fat_volume *v = s->vfs->volumes[drive];
    if (!v) return 0;
    for (struct px_node *n = s->nodes; n; n = n->next)
        if (n->linked && n->mountpoint && n->drive == drive && n->volume == v &&
            n->generation == s->vfs->generation[drive]) return n;
    struct px_node *n = node_new(s, drive == 2 ? 0 : s->mnt, PX_DIRECTORY);
    if (!n) return 0;
    n->volume = v; n->drive = drive; n->generation = s->vfs->generation[drive];
    n->mountpoint = true; n->entry.attr = FAT_ATTR_DIR; n->entry.index = UINT32_MAX;
    n->entry.first = volume_root(v); n->entry.name[0] = drive == 2 ? '/' : (char)('a' + drive);
    if (drive == 2) { n->parent = n; s->root = n; }
    return n;
}

int files_attach(struct vfs *v, struct px_namespace **out)
{
    fs_lock_take(&v->lock);
    struct px_namespace *s = space_find(v);
    int err = 0;
    if (!s) {
        s = kzalloc(sizeof(*s));
        if (!s) { fs_lock_drop(&v->lock); return -ENOMEM; }
        s->vfs = v; s->next = spaces; spaces = s;
        if (!v->volumes[2]) err = -ENOENT;
        if (!err && !mount_root(s, 2)) err = -ENOMEM;
        if (!err) {
            s->mnt = node_new(s, s->root, PX_MNT);
            s->dev = node_new(s, s->root, PX_DEV);
            if (!s->mnt || !s->dev) err = -ENOMEM;
        }
        if (!err) {
            memcpy(s->mnt->entry.name, "mnt", 4); memcpy(s->dev->entry.name, "dev", 4);
            s->null = node_new(s, s->dev, PX_NULL);
            s->console = node_new(s, s->dev, PX_CONSOLE);
            if (!s->null || !s->console) err = -ENOMEM;
        }
        if (!err) {
            memcpy(s->null->entry.name, "null", 5); memcpy(s->console->entry.name, "console", 8);
        }
        if (err) files_detach(v);
    }
    if (!err && !v->volumes[2]) err = -ENOENT;
    if (!err && (!s->root->linked || s->root->generation != v->generation[2])) {
        if (!mount_root(s, 2)) err = -ENOMEM;
        else { s->dev->parent = s->root; s->mnt->parent = s->root; }
    }
    if (!err) *out = s;
    fs_lock_drop(&v->lock);
    return err;
}

void files_detach(struct vfs *v)
{
    struct px_namespace **at = &spaces;
    while (*at && (*at)->vfs != v) at = &(*at)->next;
    if (!*at) return;
    struct px_namespace *s = *at;
    if (s->descriptions) panic("files: live descriptions at detach");
    *at = s->next;
    while (s->nodes) {
        struct px_node *n = s->nodes; s->nodes = n->next;
        if (n->refs) panic("files: pinned namespace at detach");
        kfree(n);
    }
    kfree(s);
}
void files_snapshot(struct px_namespace *s, struct file_ledger *out)
{
    memset(out, 0, sizeof(*out));
    fs_lock_take(&s->vfs->lock);
    for (struct px_node *n = s->nodes; n; n = n->next) { out->nodes++; out->pins += n->refs; }
    for (struct file_description *d = s->descriptions; d; d = d->next) out->descriptions++;
    fs_lock_drop(&s->vfs->lock);
}
void px_retain(void *node) { if (node) ((struct px_node *)node)->refs++; }
void px_release(void *node) { if (node) ((struct px_node *)node)->refs--; }

int px_validate(const char *path)
{
    unsigned bytes = 0;
    while (bytes < CIUKI_PATH_MAX && path[bytes]) bytes++;
    if (bytes == CIUKI_PATH_MAX) return -ENAMETOOLONG;
    if (!bytes) return -ENOENT;
    const char *s = path;
    while (*s) {
        if (*s == '/') { s++; continue; }
        const char *start = s; unsigned units = 0; uint16_t last = 0;
        while (*s && *s != '/') {
            uint16_t c; int err = path_decode(&s, &c);
            if (err) return px_error(err);
            if (++units > FS_NAME_CHARS) return -ENAMETOOLONG;
            if (c < 32 || c == 127 || c == '"' || c == '*' || c == ':' || c == '<' ||
                c == '>' || c == '?' || c == '\\' || c == '|') return -EINVAL;
            last = c;
        }
        unsigned n = (unsigned)(s - start);
        bool dot = (n == 1 && start[0] == '.') || (n == 2 && start[0] == '.' && start[1] == '.');
        if (!dot && (last == '.' || last == ' ')) return -EINVAL;
    }
    return 0;
}

static int path_units(struct px_node *parent, const char *leaf)
{
    if (!parent->volume) return 0;
    unsigned units = 3; /* drive letter, colon, root slash */
    const char *s = leaf;
    for (;;) {
        while (*s) { uint16_t c; int e = path_decode(&s, &c); if (e) return px_error(e); units++; }
        if (units > FS_PATH_CHARS) return -ENAMETOOLONG;
        if (parent->mountpoint) return 0;
        units++; s = parent->entry.name; parent = parent->parent;
    }
}

static int child_lookup(struct px_node *p, const char *name, struct px_node **out)
{
    struct px_namespace *s = p->space;
    if (p == s->root && path_equal(name, "mnt")) { *out = s->mnt; return 0; }
    if (p == s->root && path_equal(name, "dev")) { *out = s->dev; return 0; }
    if (p == s->dev) {
        if (path_equal(name, "null")) *out = s->null;
        else if (path_equal(name, "console")) *out = s->console;
        else return -ENOENT;
        return 0;
    }
    if (p == s->mnt) {
        unsigned d = (unsigned)(path_upper((uint8_t)name[0]) - 'A');
        if (name[1] || d < 3 || d >= 26 || !s->vfs->volumes[d]) return -ENOENT;
        *out = mount_root(s, d); return *out ? 0 : -ENOMEM;
    }
    struct fat_entry entry;
    int err = fat_lookup(p->volume, p->entry.first, name, &entry);
    if (err) return px_error(err);
    *out = node_entry(s, p, &entry);
    return *out ? 0 : -ENOMEM;
}

int px_resolve_locked(struct px_namespace *s, struct px_node *cwd, const char *path,
                      bool missing_leaf, struct px_path *out)
{
    int err = px_validate(path);
    if (err) return err;
    memset(out, 0, sizeof(*out));
    struct px_node *n = *path == '/' ? s->root : cwd ? cwd : s->root;
    if (!node_available(n)) return -ENOENT;
    const char *p = path;
    out->trailing = path[strlen(path) - 1] == '/';
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        if (!directory(n)) return -ENOTDIR;
        const char *start = p;
        while (*p && *p != '/') p++;
        unsigned count = (unsigned)(p - start);
        memcpy(out->name, start, count); out->name[count] = 0;
        const char *next = p; while (*next == '/') next++;
        out->dot = path_equal(out->name, ".") || path_equal(out->name, "..");
        out->parent = n;
        if (out->dot) {
            if (out->name[1]) n = n->parent;
        } else {
            if ((err = path_units(n, out->name))) return err;
            err = child_lookup(n, out->name, &n);
            if (err) {
                if (err == -ENOENT && !*next && missing_leaf && out->parent->volume) return 0;
                return err;
            }
        }
        if (!node_available(n)) return -ENOENT;
    }
    if (out->trailing && !directory(n)) return -ENOTDIR;
    out->node = n;
    if (!out->parent) out->parent = n->parent;
    return 0;
}

int px_getcwd_locked(struct px_node *n, char out[CIUKI_PATH_MAX])
{
    if (!node_available(n)) return -ENOENT;
    char backwards[CIUKI_PATH_MAX]; unsigned used = 0, count = 0;
    while (n != n->space->root) {
        if (++count > FS_PATH_CHARS || !n->linked) return -EIO;
        unsigned length = (unsigned)strlen(n->entry.name);
        if (used + length + 1 >= sizeof(backwards)) return -ENAMETOOLONG;
        for (unsigned i = length; i; i--) backwards[used++] = n->entry.name[i - 1];
        backwards[used++] = '/'; n = n->parent;
        if (!node_available(n)) return -ENOENT;
    }
    if (!used) backwards[used++] = '/';
    for (unsigned i = 0; i < used; i++) out[i] = backwards[used - i - 1];
    out[used] = 0;
    return 0;
}
int px_chdir(struct process *p, const char *path)
{
    struct px_node *cwd = p->cwd;
    if (!cwd) return -ENOENT;
    px_retain(cwd);
    struct px_namespace *s = cwd->space;
    fs_lock_take(&s->vfs->lock);
    struct px_path result;
    int err = px_resolve_locked(s, cwd, path, false, &result);
    if (!err && !directory(result.node)) err = -ENOTDIR;
    if (!err) {
        px_retain(result.node); px_release(p->cwd); p->cwd = result.node;
        p->cwd_retain = px_retain; p->cwd_release = px_release;
    }
    fs_lock_drop(&s->vfs->lock);
    px_release(cwd);
    return err;
}

void px_changed_locked(struct px_node *n)
{
    file_clock_now(CLOCK_REALTIME, 0, &n->ctime);
    for (unsigned i = 0; i < VFS_NODES; i++) {
        struct vfs_node *old = &n->space->vfs->nodes[i];
        if (old->refs && old->volume == n->volume &&
            (old == n->legacy || (n->linked && entry_same(&old->entry, &n->entry)))) {
            old->entry = n->entry; n->legacy = old;
        }
    }
}

static bool legacy_open(struct px_node *n)
{
    if (n->legacy && n->legacy->refs && n->legacy->volume == n->volume) return true;
    if (!n->linked) return false;
    for (unsigned i = 0; i < VFS_NODES; i++) {
        struct vfs_node *old = &n->space->vfs->nodes[i];
        if (old->refs && old->volume == n->volume && entry_same(&old->entry, &n->entry)) {
            n->legacy = old; return true;
        }
    }
    return false;
}

int px_share_locked(struct px_node *n, uint32_t access, uint32_t deny)
{
    for (struct file_description *d = n->space->descriptions; d; d = d->next) {
        unsigned a = (d->flags & O_ACCMODE) == O_RDONLY ? VFS_READ :
                     (d->flags & O_ACCMODE) == O_WRONLY ? VFS_WRITE : VFS_READ | VFS_WRITE;
        if (d->references && d->node == n && ((d->deny & access) || (deny & a))) return -EACCES;
    }
    for (unsigned i = 0; i < VFS_DESCRIPTIONS; i++) {
        struct vfs_description *d = &n->space->vfs->descriptions[i];
        if (d->refs && d->node->volume == n->volume &&
            (n->linked ? entry_same(&d->node->entry, &n->entry) : d->node == n->legacy) &&
            ((d->deny & access) || (deny & d->access))) return -EACCES;
    }
    return 0;
}

int px_stat_locked(struct px_node *n, struct ciuki_stat *out)
{
    memset(out, 0, sizeof(*out));
    out->st_ino = n->ino; out->st_nlink = n->linked ? 1 : 0;
    out->st_mode = directory(n) ? S_IFDIR | 0777 : n->kind == PX_FILE ?
        S_IFREG | (n->entry.attr & FAT_ATTR_RO ? 0444 : 0666) : S_IFCHR | 0666;
    out->st_blksize = 4096;
    if (!n->volume) { out->st_rdev = n->kind == PX_NULL ? 1 : n->kind == PX_CONSOLE ? 2 : 0; return 0; }
    out->st_dev = n->drive + 1;
    out->st_size = n->kind == PX_FILE ? n->entry.size : 0;
    out->st_blksize = n->volume->spc * 512;
    uint32_t c = n->entry.first, count = 0;
    if (!c && n->mountpoint && n->volume->type != 32)
        out->st_blocks = ((uint32_t)n->volume->root_entries * 32 + 511) / 512;
    else while (c) {
        if (c < 2 || c >= n->volume->clusters + 2 || ++count > n->volume->clusters) return -EIO;
        uint32_t next; int err = fat_get_cluster(n->volume, c, &next);
        if (err) return px_error(err);
        uint32_t end = n->volume->type == 12 ? 0xff8 : n->volume->type == 16 ? 0xfff8 : 0x0ffffff8;
        out->st_blocks += n->volume->spc;
        if (next >= end) break;
        c = next;
        if (!c) return -EIO;
    }
    clock_fat_utc(n->entry.times.access_date, 0, &out->st_atim.tv_sec);
    clock_fat_utc(n->entry.times.write_date, n->entry.times.write_time, &out->st_mtim.tv_sec);
    out->st_ctim = n->ctime;
    return 0;
}

int px_open_locked(struct px_namespace *s, struct px_node *cwd, const char *path,
                   uint32_t flags, struct file_description *d)
{
    struct px_path r;
    int err = px_resolve_locked(s, cwd, path, !!(flags & O_CREAT), &r);
    if (err) return err;
    if (r.node && (r.node->kind == PX_NULL || r.node->kind == PX_CONSOLE)) {
        if ((err = device_open(r.node->kind, flags))) return err;
    } else if (r.node && directory(r.node)) {
        if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) return -EISDIR;
    } else if (flags & O_DIRECTORY) return -ENOTDIR;
    if (r.node && (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) return -EEXIST;
    if (!r.node && r.trailing) return -ENOENT;
    if (r.node && r.node->volume) {
        unsigned access = (flags & O_ACCMODE) == O_RDONLY ? VFS_READ :
                          (flags & O_ACCMODE) == O_WRONLY ? VFS_WRITE : VFS_READ | VFS_WRITE;
        if ((err = px_share_locked(r.node, access, d->deny))) return err;
        if (access & VFS_WRITE) {
            if (r.node->volume->readonly) return -EROFS;
            if (r.node->entry.attr & FAT_ATTR_RO) return -EACCES;
        }
    }
    if (!r.node) {
        /* Reserve metadata BEFORE FAT create; allocation failure is pure. */
        struct px_node *n = node_new(s, r.parent, PX_FILE);
        if (!n) return -ENOMEM;
        n->linked = false;
        err = fat_create(r.parent->volume, r.parent->entry.first, r.name, 0, &n->entry);
        if (err) { s->nodes = n->next; kfree(n); return px_error(err); }
        n->linked = true; n->volume = r.parent->volume; n->drive = r.parent->drive;
        n->generation = r.parent->generation; r.node = n;
        px_changed_locked(n); px_changed_locked(r.parent);
    }
    if (flags & O_TRUNC) {
        err = fat_truncate(r.node->volume, &r.node->entry, 0);
        px_changed_locked(r.node);
        if (err) return px_error(err);
    }
    d->node = r.node; px_retain(r.node);
    d->next = s->descriptions; s->descriptions = d;
    return 0;
}

int px_mkdir_locked(struct px_namespace *s, struct px_node *cwd, const char *path, uint32_t mode)
{
    if (mode & ~0777u) return -EINVAL;
    struct px_path r; int err = px_resolve_locked(s, cwd, path, true, &r);
    if (err) return err;
    if (r.node) return -EEXIST;
    struct px_node *n = node_new(s, r.parent, PX_DIRECTORY);
    if (!n) return -ENOMEM;
    n->linked = false;
    err = fat_create(r.parent->volume, r.parent->entry.first, r.name, FAT_ATTR_DIR, &n->entry);
    if (err) { s->nodes = n->next; kfree(n); return px_error(err); }
    n->linked = true; n->volume = r.parent->volume; n->drive = r.parent->drive;
    n->generation = r.parent->generation;
    px_changed_locked(n); px_changed_locked(r.parent);
    return 0;
}

/* FAT integration: a detached regular entry uses index UINT32_MAX. FAT must
 * skip owning-entry updates and release only its chain at the final close.
 * This is distinct from a root DIRECTORY entry with the same sentinel. */
static int remove_node(struct px_node *n)
{
    bool legacy = n->kind == PX_FILE && legacy_open(n);
    bool retain = n->kind == PX_FILE && (n->refs || legacy);
    if (retain && (!fat_native_ready || !fat_native_ready())) return -EIO;
    struct fat_entry entry = n->entry;
    if (retain) entry.first = 0; /* durable name removal, no chain release */
    int err = fat_remove(n->volume, &entry, directory(n));
    if (err) return px_error(err);
    n->linked = false;
    if (retain) n->entry.index = UINT32_MAX;
    else { n->entry.first = 0; n->entry.size = 0; }
    px_changed_locked(n); px_changed_locked(n->parent);
    node_forget(n);
    return 0;
}
int px_drop_locked(struct px_node *n)
{
    px_release(n);
    if (!n->refs && !n->linked && n->kind == PX_FILE && !legacy_open(n) && n->entry.first) {
        int err = fat_remove(n->volume, &n->entry, false);
        n->entry.first = 0; n->entry.size = 0;
        node_forget(n);
        return px_error(err);
    }
    node_forget(n);
    return 0;
}
int px_remove_locked(struct px_namespace *s, struct px_node *cwd, const char *path, bool dir)
{
    struct px_path r; int err = px_resolve_locked(s, cwd, path, false, &r);
    if (err) return err;
    if (dir && r.dot) return -EINVAL;
    if (!dir && directory(r.node)) return -EISDIR;
    if (dir && !directory(r.node)) return -ENOTDIR;
    if (!r.node->volume || r.node->mountpoint) return -EBUSY;
    if (dir && (r.node->refs || legacy_open(r.node))) return -EBUSY;
    if (dir) {
        err = vfs_directory_pinned(s->vfs, r.node->volume, r.node->entry.first);
        if (err) return err < 0 ? px_error(err) : -EBUSY;
    }
    return remove_node(r.node);
}
static int empty_directory(struct px_node *n)
{
    struct fat_entry child; uint32_t cursor = 0; int err;
    while (!(err = fat_next(n->volume, n->entry.first, &cursor, &child)))
        if (!path_equal(child.name, ".") && !path_equal(child.name, "..")) return -ENOTEMPTY;
    return err == -FS_ENOENT ? 0 : px_error(err);
}
int px_rename_locked(struct px_namespace *s, struct px_node *cwd, const char *from, const char *to)
{
    struct px_path a, b;
    int err = px_resolve_locked(s, cwd, from, false, &a);
    if (!err) err = px_resolve_locked(s, cwd, to, true, &b);
    if (err) return err;
    if (a.dot || b.dot) return -EINVAL;
    if (a.node == b.node) {
        if (!a.node->volume || a.node->mountpoint || !path_equal(a.node->entry.name, b.name) ||
            !strncmp(a.node->entry.name, b.name, FS_NAME_BYTES)) return 0;
    } else {
        if (!a.node->volume || a.node->mountpoint || (b.node && (!b.node->volume || b.node->mountpoint))) return -EBUSY;
        if (a.node->volume != b.parent->volume) return -EXDEV;
        if (b.trailing && !b.node) return -ENOENT;
        if (b.node && directory(b.node) != directory(a.node)) return directory(a.node) ? -ENOTDIR : -EISDIR;
        if (directory(a.node)) {
            struct px_node *p = b.parent;
            for (;;) {
                if (p == a.node) return -EINVAL;
                if (p->mountpoint || !p->volume) break;
                p = p->parent;
            }
        }
        if (b.node && directory(b.node)) {
            if (b.node->refs || legacy_open(b.node)) return -EBUSY;
            err = vfs_directory_pinned(s->vfs, b.node->volume, b.node->entry.first);
            if (err) return err < 0 ? px_error(err) : -EBUSY;
            if ((err = empty_directory(b.node))) return err;
        }
    }
    if (a.node->volume->readonly) return -EROFS;
    if ((a.node->entry.attr & FAT_ATTR_RO) || (b.node && (b.node->entry.attr & FAT_ATTR_RO))) return -EACCES;
    legacy_open(a.node); /* preserve the legacy node across a new owning-entry index */
    struct px_node *parent = a.node->parent;
    if (b.node && b.node != a.node) {
        if (!fat_rename_replace || !fat_native_ready || !fat_native_ready()) return -EIO;
        bool legacy = b.node->kind == PX_FILE && legacy_open(b.node);
        bool retain = b.node->kind == PX_FILE && (b.node->refs || legacy);
        err = fat_rename_replace(a.node->volume, &a.node->entry, &b.node->entry,
                                 b.parent->entry.first, b.name, retain);
        if (b.node->entry.index == UINT32_MAX) {
            b.node->linked = false; px_changed_locked(b.node); node_forget(b.node);
        }
    } else if (fat_rename_replace) {
        err = fat_rename_replace(a.node->volume, &a.node->entry, 0, b.parent->entry.first, b.name, false);
    } else {
        err = fat_rename(a.node->volume, &a.node->entry, b.parent->entry.first, b.name);
    }
    if (!err) {
        a.node->parent = b.parent;
        /* fat_rename publishes UCS-2 bytes but its F1 result retains old name. */
        memcpy(a.node->entry.name, b.name, strlen(b.name) + 1);
        px_changed_locked(a.node); px_changed_locked(parent); px_changed_locked(b.parent);
    }
    return px_error(err);
}

int px_getdents_locked(struct file_description *d, struct ciuki_dirent *out)
{
    struct px_node *dir = d->node, *n = 0;
    if (!directory(dir)) return -ENOTDIR;
    const char *name;
    uint32_t cursor = d->cursor;
    if (d->position < 2) { name = d->position ? ".." : "."; n = d->position ? dir->parent : dir; }
    else if (dir == dir->space->dev) {
        if (d->position > 3) return 0;
        n = d->position == 2 ? dir->space->null : dir->space->console; name = n->entry.name;
    } else if (dir == dir->space->mnt) {
        if (!cursor) cursor = 3;
        while (cursor < 26 && !dir->space->vfs->volumes[cursor]) cursor++;
        if (cursor == 26) return 0;
        n = mount_root(dir->space, cursor++); if (!n) return -ENOMEM;
        name = n->entry.name;
    } else if (dir == dir->space->root && d->position < 4) {
        n = d->position == 2 ? dir->space->mnt : dir->space->dev; name = n->entry.name;
    } else {
        struct fat_entry entry; int err;
        for (;;) {
            err = fat_next(dir->volume, dir->entry.first, &cursor, &entry);
            if (err) return err == -FS_ENOENT ? 0 : px_error(err);
            if (path_equal(entry.name, ".") || path_equal(entry.name, "..")) continue;
            if (dir == dir->space->root && (path_equal(entry.name, "mnt") || path_equal(entry.name, "dev"))) continue;
            break;
        }
        n = node_entry(dir->space, dir, &entry); if (!n) return -ENOMEM;
        name = n->entry.name;
    }
    if (d->position == INT64_MAX) return -EIO;
    memset(out, 0, sizeof(*out));
    out->d_ino = n->ino; out->d_off = (int64_t)++d->position;
    out->d_reclen = sizeof(*out); out->d_namlen = (uint16_t)strlen(name);
    out->d_type = directory(n) ? DT_DIR : n->kind == PX_FILE ? DT_REG : DT_CHR;
    memcpy(out->d_name, name, out->d_namlen); d->cursor = cursor;
    return sizeof(*out);
}

int px_legacy_share(struct vfs *v, struct fat_volume *vol, const struct fat_entry *e, unsigned access, unsigned deny)
{
    struct px_namespace *s = space_find(v); if (!s) return 0;
    struct px_node *n = node_find(s, vol, e);
    return n ? px_share_locked(n, access, deny) : 0;
}
bool px_legacy_busy(struct vfs *v, struct fat_volume *vol, const struct fat_entry *e)
{
    struct px_namespace *s = space_find(v); if (!s) return false;
    struct px_node *n = node_find(s, vol, e);
    if (!n) return false;
    for (struct px_node *p = s->nodes; p; p = p->next) if (p->refs) {
        struct px_node *ancestor = p;
        for (;;) {
            if (ancestor == n) return true;
            if (!ancestor->parent || ancestor->parent == ancestor) break;
            ancestor = ancestor->parent;
        }
    }
    return false;
}
void px_legacy_changed(struct vfs *v, struct fat_volume *vol, const struct fat_entry *old, const struct fat_entry *entry)
{
    struct px_namespace *s = space_find(v); if (!s) return;
    struct px_node *n = node_find(s, vol, old);
    if (!n && entry)
        for (struct px_node *candidate = s->nodes; candidate; candidate = candidate->next)
            if (candidate->legacy && entry == &candidate->legacy->entry) { n = candidate; break; }
    if (!n) return;
    if (entry && !memcmp(old, entry, sizeof(*entry))) return;
    if (!entry) n->linked = false;
    else {
        n->entry = *entry;
        if (entry->parent != old->parent) {
            for (struct px_node *parent = s->nodes; parent; parent = parent->next)
                if (parent->linked && parent->volume == vol && directory(parent) && parent->entry.first == entry->parent) {
                    n->parent = parent; break;
                }
        }
    }
    file_clock_now(CLOCK_REALTIME, 0, &n->ctime);
    node_forget(n);
}
int px_legacy_closed(struct vfs *v, struct vfs_node *old)
{
    struct px_namespace *s = space_find(v); if (!s) return 0;
    for (struct px_node *n = s->nodes; n; n = n->next) if (n->legacy == old) {
        n->legacy = 0;
        if (!n->linked && !n->refs && n->entry.first) {
            px_retain(n); return px_drop_locked(n);
        }
        node_forget(n);
        return 0;
    }
    return 0;
}
bool px_volume_busy(struct vfs *v, struct fat_volume *vol)
{
    struct px_namespace *s = space_find(v); if (!s) return false;
    /* List membership lasts through final-close cleanup under this lock.
     * fdtable may already have decremented references to zero while waiting
     * for the lock; such a description must still block durable detach. */
    for (struct file_description *d = s->descriptions; d; d = d->next)
        if (d->node->volume == vol) return true;
    return false;
}
void px_volume_detached(struct vfs *v, unsigned drive)
{
    struct px_namespace *s = space_find(v); if (!s) return;
    for (struct px_node *n = s->nodes; n; n = n->next) if (n->volume && n->drive == drive) n->linked = false;
}
