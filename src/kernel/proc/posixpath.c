/* POSIX names over F1's drive namespace. Node changes share the short
 * namespace lock; filesystem work uses VFS_IO and drive reservations.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/clock.h>
#include <ciuki/fat_native.h>
#include <ciuki/audio.h>
#include <ciuki/com.h>
#pragma weak com_device_open
#pragma weak com_snapshot
#pragma weak audio_device_open
#pragma weak vfs_fat_transaction_locked /* older standalone personality models */
#pragma weak vfs_dir_step_locked
#pragma weak vfs_dir_walk_locked
#pragma weak vfs_chain_clusters_locked

static struct px_namespace *spaces;

/* No symlinks: the synthetic /mnt/<letter> transitions and cached cwd
 * ancestry determine every drive a path can visit, without filesystem I/O.
 * Preserve intermediate components (including those before '..') so invalid
 * paths still have their normal lookup/error semantics. */
static uint32_t path_drives(struct px_namespace *s,struct px_node *cwd,const char *path)
{
    uint8_t kinds[CIUKI_PATH_MAX/2+2]; unsigned depth=0;
    kinds[0]=2; /* 0=/mnt, 1=/dev, 2=C root, 3..25=mount letters */
    if (path[0]!='/' && cwd) {
        struct px_node *n=cwd; unsigned count=0;
        while (n!=s->root && count<CIUKI_PATH_MAX/2) {
            kinds[++count]=n==s->mnt ? 0 : n==s->dev || (!n->volume && !n->ro) ? 1 : n->drive;
            n=n->parent;
        }
        depth=count;
        for (unsigned i=1;i<=count/2;i++) { unsigned k=kinds[i]; kinds[i]=kinds[count+1-i]; kinds[count+1-i]=k; }
    }
    uint32_t mask=path[0]!='/' && cwd && (cwd->ro || (cwd->volume && !cwd->mountpoint)) ? vfs_drive_mask(cwd->drive) : 0;
    const char *p=path;
    while (*p) {
        while (*p=='/') p++;
        if (!*p) break;
        const char *start=p; while (*p && *p!='/') p++;
        unsigned len=(unsigned)(p-start), kind=kinds[depth];
        if (len==1 && start[0]=='.') continue;
        if (len==2 && start[0]=='.' && start[1]=='.') { if (depth) depth--; continue; }
        if (!depth && len==3 && path_upper(start[0])=='M' && path_upper(start[1])=='N' && path_upper(start[2])=='T') kind=0;
        else if (!depth && len==3 && path_upper(start[0])=='D' && path_upper(start[1])=='E' && path_upper(start[2])=='V') kind=1;
        else if (kind==0) { kind=len==1 ? (unsigned)(path_upper(start[0])-'A') : 26; if (kind<3 || kind>=26) kind=1; else mask|=1u<<kind; }
        else if (kind>=2) mask|=vfs_drive_mask(kind);
        if (depth+1<sizeof(kinds)/sizeof(*kinds)) kinds[++depth]=kind;
    }
    return mask | (kinds[depth]>=2 ? vfs_drive_mask(kinds[depth]) : 0);
}
static void reserve_paths(struct px_namespace *s,struct px_node *cwd,const char *a,const char *b)
{
    uint32_t mask=path_drives(s,cwd,a) | (b ? path_drives(s,cwd,b) : 0);
    bool nested=s->vfs->active!=0;
    for (;;) {
        vfs_reserve_locked(s->vfs,mask);
        uint32_t now=path_drives(s,cwd,a) | (b ? path_drives(s,cwd,b) : 0);
        if (!(now & ~s->vfs->active)) return;
        /* Cwd ancestry may have moved while initial admission slept. No
         * resolved identities have been borrowed yet; retry the growing set. */
        if (nested) fs_panic("incomplete path reservation");
        mask|=now; vfs_unlock(s->vfs); vfs_lock(s->vfs,0);
    }
}

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
    if (n->ro) return !px_ro_check_locked((struct px_node *)n);
    if (!n->volume) n = n->space->root;
    return n && n->linked && n->volume && n->volume->mounted &&
        n->space->vfs->volumes[n->drive] == n->volume &&
        n->space->vfs->generation[n->drive] == n->generation;
}
int px_ro_check_locked(struct px_node *n)
{
    struct vfs *v=n->space->vfs;
    if (n->volume || n->ro) vfs_reserve_locked(v,vfs_drive_mask(n->drive));
    if (!n->ro) {
        if (!n->volume) return 0;
        return n->drive<26 && n->volume->mounted && !n->volume->dev->quarantined &&
            v->volumes[n->drive]==n->volume && v->generation[n->drive]==n->generation ? 0 : -EIO;
    }
    if (!n->linked || n->drive>=26 || v->generation[n->drive]!=n->generation || v->ro_volumes[n->drive]!=n->ro) return -EIO;
    return px_error(vfs_ro_check_locked(v,n->drive));
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
static struct px_node *ro_node_entry(struct px_namespace *s, struct px_node *parent, const struct fs_ro_node *entry)
{
    for (struct px_node *n=s->nodes; n; n=n->next)
        if (n->linked && n->ro==parent->ro && n->generation==parent->generation && n->ro_id==entry->id) return n;
    struct px_node *n=node_new(s,parent,entry->attr&FS_RO_DIRECTORY ? PX_DIRECTORY : PX_FILE);
    if (!n) return 0;
    n->ro=parent->ro; n->ro_id=entry->id; n->ro_size=entry->size;
    n->drive=parent->drive; n->generation=parent->generation;
    memcpy(n->entry.name,entry->name,sizeof(n->entry.name)); memcpy(n->entry.alias,entry->alias,sizeof(n->entry.alias));
    n->entry.attr=entry->attr; return n;
}
static struct px_node *mount_root(struct px_namespace *s, unsigned drive)
{
    struct fs_ro_volume *ro=s->vfs->ro_volumes[drive];
    if (ro) {
        if (vfs_ro_check_locked(s->vfs,drive)) return 0;
        for (struct px_node *n=s->nodes; n; n=n->next)
            if (n->linked && n->mountpoint && n->ro==ro && n->drive==drive && n->generation==s->vfs->generation[drive]) return n;
        struct fs_ro_node entry; if (VFS_IO(s->vfs,ro->ops->root(ro,&entry))) return 0;
        struct px_node *n=node_new(s,s->mnt,PX_DIRECTORY); if (!n) return 0;
        n->ro=ro; n->ro_id=entry.id; n->ro_size=entry.size; n->drive=drive; n->generation=s->vfs->generation[drive];
        n->mountpoint=true; n->entry.attr=FS_RO_DIRECTORY|FS_RO_READONLY; n->entry.name[0]=(char)('a'+drive);
        return n;
    }
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
    vfs_lock(v,0);
    struct px_namespace *s = space_find(v);
    int err = 0;
    if (!s) {
        s = kzalloc(sizeof(*s));
        if (!s) { vfs_unlock(v); return -ENOMEM; }
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
            s->audio = node_new(s, s->dev, PX_AUDIO);
            s->mixer = node_new(s, s->dev, PX_MIXER);
            s->net = node_new(s, s->dev, PX_NET);
            s->media = node_new(s, s->dev, PX_MEDIA);
            if (!s->null || !s->console || !s->audio || !s->mixer || !s->net || !s->media) err = -ENOMEM;
        }
        if (!err) {
            memcpy(s->null->entry.name, "null", 5); memcpy(s->console->entry.name, "console", 8);
            memcpy(s->audio->entry.name, "audio", 6); memcpy(s->mixer->entry.name, "mixer", 6);
            memcpy(s->net->entry.name, "net", 4);
            memcpy(s->media->entry.name, "media", 6);
            if (com_snapshot) for (unsigned i=0;i<CIUKI_COM_MAX;i++) {
                struct ciuki_com_status info;
                if (!com_snapshot(i,&info,0)) continue;
                struct px_node *com=node_new(s,s->dev,PX_COM);
                if (!com) { err=-ENOMEM; break; }
                s->com[i]=com;
                com->drive=i;
                ksnprintf(com->entry.name,sizeof(com->entry.name),"com%u",i+1);
            }
        }
        if (err) files_detach(v);
    }
    if (!err && !v->volumes[2]) err = -ENOENT;
    if (!err && (!s->root->linked || s->root->generation != v->generation[2])) {
        if (!mount_root(s, 2)) err = -ENOMEM;
        else { s->dev->parent = s->root; s->mnt->parent = s->root; }
    }
    if (!err) *out = s;
    vfs_unlock(v);
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
    vfs_lock(s->vfs,0);
    for (struct px_node *n = s->nodes; n; n = n->next) { out->nodes++; out->pins += n->refs; }
    for (struct file_description *d = s->descriptions; d; d = d->next) out->descriptions++;
    vfs_unlock(s->vfs);
}
void px_retain(void *node) { if (node) __atomic_add_fetch(&((struct px_node *)node)->refs,1,__ATOMIC_RELAXED); }
void px_release(void *node) { if (node) __atomic_sub_fetch(&((struct px_node *)node)->refs,1,__ATOMIC_RELAXED); }

struct px_transaction { struct px_node *node,*parent,*replacement; bool naming;
    struct fat_entry *entry,*replacement_entry; };
static void px_transaction_visible(void *arg,const struct fat_entry *entry) {
    struct px_transaction *t=arg;
    if (t->naming) {
        t->node->entry=*entry; t->node->parent=t->parent; t->node->linked=true;
    }
}
static void px_transaction_unlinked(void *arg,bool replacement) {
    struct px_transaction *t=arg;
    struct px_node *n=replacement ? t->replacement : t->node;
    struct fat_entry *entry=replacement ? t->replacement_entry : t->entry;
    if (n) { n->linked=false; n->entry.index=UINT32_MAX; if (entry) n->entry.identity=entry->identity; }
}
int px_fat_transaction_locked(struct px_node *n,struct px_node *parent,struct px_node *replacement,
                               struct fat_transaction_args a) {
    struct vfs *v=n->space->vfs;
    bool namespace_op=a.op==FAT_TX_CREATE || a.op==FAT_TX_REMOVE || a.op==FAT_TX_RENAME || a.op==FAT_TX_RENAME_REPLACE;
    if (namespace_op && (n->namespace_busy || (replacement && replacement->namespace_busy))) return -FS_EAGAIN;
    struct px_node *old_parent=n->parent;
    px_retain(n); px_retain(parent); px_retain(replacement); px_retain(old_parent);
    if (namespace_op) { n->namespace_busy=true; if (replacement) replacement->namespace_busy=true; }
    struct px_transaction ctx={n,parent,replacement,namespace_op && a.op!=FAT_TX_REMOVE,a.entry,a.replacement};
    a.visible=px_transaction_visible; a.unlinked=px_transaction_unlinked; a.visible_ctx=&ctx;
    int e;
    if (vfs_fat_transaction_locked) e=vfs_fat_transaction_locked(v,n->volume,a);
    else {
        /* Link-only compatibility for the existing fake-FAT personality
         * models. The kernel and production FS harness link the real API. */
        switch (a.op) {
        case FAT_TX_LOOKUP: e=VFS_IO(v,fat_lookup(n->volume,a.directory,a.name,a.entry)); break;
        case FAT_TX_CREATE: e=VFS_IO(v,fat_create(n->volume,a.directory,a.name,a.attr,a.entry)); break;
        case FAT_TX_WRITE: e=VFS_IO(v,fat_write_cursor(n->volume,a.entry,a.cursor,a.position,a.data,a.size,a.done)); break;
        case FAT_TX_EXTEND: e=VFS_IO(v,fat_truncate(n->volume,a.entry,a.capacity)); break;
        case FAT_TX_TRUNCATE: e=VFS_IO(v,fat_truncate(n->volume,a.entry,a.position)); break;
        case FAT_TX_NEXT: e=VFS_IO(v,fat_next(n->volume,a.directory,a.directory_cursor,a.entry)); break;
        case FAT_TX_REMOVE: e=VFS_IO(v,fat_remove(n->volume,a.entry,a.flag)); break;
        case FAT_TX_RENAME: e=VFS_IO(v,fat_rename(n->volume,a.entry,a.directory,a.name)); break;
        case FAT_TX_RENAME_REPLACE: e=VFS_IO(v,fat_rename_replace(n->volume,a.entry,a.replacement,a.directory,a.name,a.flag)); break;
        case FAT_TX_COMMIT: e=VFS_IO(v,fat_commit(n->volume)); break;
        case FAT_TX_SET_METADATA: e=VFS_IO(v,fat_set_metadata(n->volume,a.entry,a.attr,a.times)); break;
        default: e=-FS_EOPNOTSUPP; break;
        }
    }
    if (namespace_op) {
        n->namespace_busy=false; if (replacement) replacement->namespace_busy=false;
        fs_event_wake(&v->volume_changed); /* resolvers waiting on this node */
    }
    px_release(old_parent); px_release(replacement); px_release(parent); px_release(n);
    return e;
}

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
    if (!parent->volume && !parent->ro) return 0;
    unsigned units = 3; /* drive letter, colon, root slash */
    const char *s = leaf;
    for (;;) {
        while (*s) { uint16_t c; int e = path_decode(&s, &c); if (e) return px_error(e); units++; }
        if (units > FS_PATH_CHARS) return -ENAMETOOLONG;
        if (parent->mountpoint) return 0;
        units++; s = parent->entry.name; parent = parent->parent;
    }
}

static int child_lookup(struct px_node *p, const char *name, struct px_node **out, bool guarded)
{
    struct px_namespace *s = p->space;
    vfs_tag_locked(s->vfs,"lookup"); vfs_site_locked(s->vfs,"native");
    if (p == s->root && path_equal(name, "mnt")) { *out = s->mnt; return 0; }
    if (p == s->root && path_equal(name, "dev")) { *out = s->dev; return 0; }
    if (p == s->dev) {
        if (path_equal(name, "null")) *out = s->null;
        else if (path_equal(name, "console")) *out = s->console;
        else if (path_equal(name, "audio")) *out = s->audio;
        else if (path_equal(name, "mixer")) *out = s->mixer;
        else if (path_equal(name, "net")) *out = s->net;
        else if (path_equal(name, "media")) *out = s->media;
        else {
            for (unsigned i=0;i<CIUKI_COM_MAX;i++) {
                struct px_node *n=s->com[i];
                if (n && n->linked && path_equal(name,n->entry.name)) { *out=n; return 0; }
            }
            return -ENOENT;
        }
        return 0;
    }
    if (p == s->mnt) {
        unsigned d = (unsigned)(path_upper((uint8_t)name[0]) - 'A');
        if (name[1] || d < 3 || d >= 26 || !vfs_drive_present(s->vfs,d)) return -ENOENT;
        *out = mount_root(s, d); return *out ? 0 : -ENOMEM;
    }
    if (p->ro) {
        int err=px_ro_check_locked(p); if (err) return err;
        struct fs_ro_node entry; err=VFS_IO(s->vfs,fs_ro_lookup(p->ro,p->ro_id,name,&entry));
        if (err) { px_ro_check_locked(p); return px_error(err); }
        *out=ro_node_entry(s,p,&entry); return *out ? 0 : -ENOMEM;
    }
    struct fat_entry entry;
    int err = px_fat_transaction_locked(p,p->parent,0,(struct fat_transaction_args){.op=FAT_TX_LOOKUP,.directory=p->entry.first,.name=name,.entry=&entry,.guarded=guarded,
        .depend=!p->mountpoint,.depend_parent=p->entry.parent,.depend_index=p->entry.index});
    if (err) return px_error(err);
    *out = node_entry(s, p, &entry);
    /* The caller waits for the namespace operation, then restarts. */
    if (*out && (*out)->namespace_busy) return -EINPROGRESS;
    return *out ? 0 : -ENOMEM;
}

/* f1-89: unguarded lookups report EAGAIN when a publication overlapped
 * them; the walk restarts from its start because any earlier component may
 * be stale. The last attempt looks up under the short guard (cache warm).
 * A component whose namespace operation is still in flight is waited for
 * (its node stays retained), which does not consume an attempt. */
#define PX_RESOLVE_UNGUARDED 2u
#define PX_RESOLVE_GUARDED 64u /* cache-warming restarts (f1-89 A1 item 5) */
static int resolve_once(struct px_namespace *s, struct px_node *cwd, const char *path,
                        bool missing_leaf, struct px_path *out, bool guarded, struct px_node **busy);
int px_resolve_locked(struct px_namespace *s, struct px_node *cwd, const char *path,
                      bool missing_leaf, struct px_path *out)
{
    int err = px_validate(path);
    if (err) return err;
    for (unsigned attempt=0;;) {
        struct px_node *busy=0;
        err=resolve_once(s,cwd,path,missing_leaf,out,attempt>=PX_RESOLVE_UNGUARDED,&busy);
        if (err==-EINPROGRESS && busy) {
            px_retain(busy);
            int waited=px_error(vfs_file_wait_locked(s->vfs,&busy->namespace_busy));
            px_release(busy); node_forget(busy); /* a failed create retired meanwhile */
            if (waited) return waited;
            continue;
        }
        if (err!=-EAGAIN || ++attempt>=PX_RESOLVE_UNGUARDED+PX_RESOLVE_GUARDED) return err==-EINPROGRESS ? -EAGAIN : err;
        s->vfs->lookup_restarts++; if (attempt>=PX_RESOLVE_UNGUARDED) s->vfs->lookup_guarded++;
    }
}
static int resolve_once(struct px_namespace *s, struct px_node *cwd, const char *path,
                        bool missing_leaf, struct px_path *out, bool guarded, struct px_node **busy)
{
    int err;
    reserve_paths(s,cwd,path,0);
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
            err = child_lookup(n, out->name, &n, guarded);
            if (err == -EINPROGRESS) { *busy = n; return err; }
            if (err) {
                if (err == -ENOENT && !*next && missing_leaf && (out->parent->volume || out->parent->ro)) return 0;
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
    if (n->volume || n->ro) {
        vfs_reserve_locked(n->space->vfs,vfs_drive_mask(n->drive));
        if (!node_available(n)) return -ENOENT;
    }
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
    vfs_lock(s->vfs,0);
    struct px_path result;
    int err = px_resolve_locked(s, cwd, path, false, &result);
    if (!err && !directory(result.node)) err = -ENOTDIR;
    if (!err) {
        px_retain(result.node); px_release(p->cwd); p->cwd = result.node;
        p->cwd_retain = px_retain; p->cwd_release = px_release;
    }
    vfs_unlock(s->vfs);
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
        if (d->win32) a=d->share_access;
        if (d->references && d->node == n && ((d->deny & access) || (deny & a))) return -EACCES;
    }
    for (unsigned i = 0; i < VFS_DESCRIPTIONS; i++) {
        struct vfs_description *d = &n->space->vfs->descriptions[i];
        if (d->refs && (n->ro ? d->node->ro==n->ro && d->node->ro_id==n->ro_id && d->node->generation==n->generation :
            d->node->volume == n->volume && (n->linked ? entry_same(&d->node->entry, &n->entry) : d->node == n->legacy)) &&
            ((d->deny & access) || (deny & d->access))) return -EACCES;
    }
    return 0;
}
/* Both APIs use F1-84's common lock identities. A native open has no lock
 * exemption; Win32 exemption requires this description AND the current PID. */
int px_description_range_locked(struct file_description *d,uint64_t start,uint64_t length,bool write)
{
    struct px_node *n=d->node;
    struct vfs_node key={.volume=n->volume,.entry=n->entry,.ro=n->ro,.ro_id=n->ro_id,
        .generation=n->generation,.drive=n->drive,.identity=d->identity};
    struct vfs_lock_owner owner=d->lock_owner;
    struct proc_thread *t=proc_thread_for(g_current);
    if (d->win32 && t) owner.id=t->process->pid;
    int e=vfs_description_io_locked(n->space->vfs,&key,&d->lock_description,owner,start,length,write);
    return e ? (d->win32 ? e : -EACCES) : 0;
}
int px_range_locked(struct px_node *n,uint64_t start,uint64_t length)
{
    struct vfs_node key={.volume=n->volume,.entry=n->entry,.ro=n->ro,.ro_id=n->ro_id,
        .generation=n->generation,.drive=n->drive};
    return vfs_description_io_locked(n->space->vfs,&key,0,(struct vfs_lock_owner){0},start,length,true) ? -EACCES : 0;
}

/* Clusters of a FAT chain for st_blocks. f1-93: one FAT sector per sector
 * visited and never a cold device read under the volume reservation (the
 * walk can release and readmit it); before, one guarded VFS_IO per cluster
 * held the drive for a 128 MiB image's whole 32768-cluster walk. */
static int px_stat_fill(struct px_node *, struct ciuki_stat *, uint32_t chain);
static int chain_clusters(struct px_node *n, uint32_t first, uint32_t *count)
{
    if (vfs_chain_clusters_locked) return px_error(vfs_chain_clusters_locked(n->space->vfs,n->volume,first,count));
    uint32_t c = first; *count = 0; /* standalone personality models */
    while (c) {
        if (c < 2 || c >= n->volume->clusters + 2 || ++*count > n->volume->clusters) return -EIO;
        uint32_t next; int err = VFS_IO(n->space->vfs,fat_get_cluster(n->volume, c, &next));
        if (err) return px_error(err);
        uint32_t end = n->volume->type == 12 ? 0xff8 : n->volume->type == 16 ? 0xfff8 : 0x0ffffff8;
        if (next >= end) break;
        c = next;
        if (!c) return -EIO;
    }
    return 0;
}
int px_stat_locked(struct px_node *n, struct ciuki_stat *out)
{
    vfs_tag_locked(n->space->vfs,"stat"); vfs_site_locked(n->space->vfs,"native");
    int live=px_ro_check_locked(n); if (live) return live;
    uint32_t chain = 0;
    if (n->volume && n->entry.first) {
        /* The walk may release the reservation: keep the node (and an
         * unlinked file's chain) alive; a chain replaced meanwhile is
         * walked again so st_blocks matches the entry reported. */
        px_retain(n);
        int err = 0;
        for (unsigned tries = 0;; tries++) {
            uint32_t first = n->entry.first;
            err = first ? chain_clusters(n, first, &chain) : 0;
            if (!first) chain = 0;
            if (err || n->entry.first == first || tries == 2) break;
        }
        if (!err) err = px_stat_fill(n, out, chain);
        int drop = px_drop_locked(n);
        return err ? err : drop;
    }
    return px_stat_fill(n, out, 0);
}
static int px_stat_fill(struct px_node *n, struct ciuki_stat *out, uint32_t chain)
{
    int live=px_ro_check_locked(n); if (live) return live;
    memset(out, 0, sizeof(*out));
    out->st_ino = n->ino; out->st_nlink = n->linked ? 1 : 0;
    out->st_mode = directory(n) ? S_IFDIR | 0777 : n->kind == PX_FILE ?
        S_IFREG | (n->entry.attr & FAT_ATTR_RO ? 0444 : 0666) : S_IFCHR | ((n->kind == PX_NET || n->kind == PX_MEDIA) ? 0444 : 0666);
    out->st_blksize = 4096;
    if (n->ro) {
        out->st_dev=n->drive+1; out->st_size=n->kind==PX_FILE ? (int64_t)n->ro_size : 0;
        out->st_blksize=n->ro->dev->sector_size; out->st_blocks=(n->ro_size+511)/512;
        out->st_mode=directory(n) ? S_IFDIR|0555 : S_IFREG|0444; return 0;
    }
    if (!n->volume) { out->st_rdev = n->kind == PX_COM ? 16+n->drive : n->kind == PX_NULL ? 1 : n->kind == PX_CONSOLE ? 2 : n->kind == PX_AUDIO ? 3 : n->kind == PX_MIXER ? 4 : n->kind == PX_NET ? 5 : n->kind == PX_MEDIA ? 6 : 0; return 0; }
    out->st_dev = n->drive + 1;
    out->st_size = n->kind == PX_FILE ? n->entry.size : 0;
    out->st_blksize = n->volume->spc * 512;
    if (!n->entry.first && n->mountpoint && n->volume->type != 32)
        out->st_blocks = ((uint32_t)n->volume->root_entries * 32 + 511) / 512;
    else out->st_blocks = (int64_t)chain * n->volume->spc;
    clock_fat_utc(n->entry.times.access_date, 0, &out->st_atim.tv_sec);
    clock_fat_utc(n->entry.times.write_date, n->entry.times.write_time, &out->st_mtim.tv_sec);
    out->st_ctim = n->ctime;
    return 0;
}

/* f1-89: native FAT mutations take the drive's writer turn before they
 * resolve or borrow anything (see vfs_turn_take_locked). Lookups-only
 * opens never take it. */
int px_turn_take_path_locked(struct px_namespace *s, struct px_node *cwd, const char *path, uint32_t *turn)
{
    *turn = 0;
    int err = px_validate(path); if (err) return err;
    return px_error(vfs_turn_take_locked(s->vfs, path_drives(s, cwd, path), turn));
}
static int open_inner(struct px_namespace *, struct px_node *, const char *, uint32_t, struct file_description *);
int px_open_locked(struct px_namespace *s, struct px_node *cwd, const char *path,
                   uint32_t flags, struct file_description *d)
{
    if (!(flags & (O_CREAT | O_TRUNC)) || px_validate(path)) return open_inner(s, cwd, path, flags, d);
    uint32_t turn; int err = px_error(vfs_turn_take_locked(s->vfs, path_drives(s, cwd, path), &turn));
    if (!err) err = open_inner(s, cwd, path, flags, d);
    vfs_turn_drop_locked(s->vfs, turn);
    return err;
}
static int open_inner(struct px_namespace *s, struct px_node *cwd, const char *path,
                   uint32_t flags, struct file_description *d)
{
    vfs_tag_locked(s->vfs,flags&O_TRUNC ? "truncate" : flags&O_CREAT ? "create" : "open");
    vfs_site_locked(s->vfs,"native");
    struct px_path r;
    int err = px_resolve_locked(s, cwd, path, !!(flags & O_CREAT), &r);
    if (err) return err;
    if (((r.node && r.node->ro) || (r.parent && r.parent->ro)) &&
        ((flags&O_ACCMODE)!=O_RDONLY || (flags&(O_CREAT|O_TRUNC)))) return -EROFS;
    if (r.node && r.node->delete_pending) return -EACCES;
    if (r.node && r.node->ro && (err=px_share_locked(r.node,d->win32 ? d->share_access : VFS_READ,d->deny))) return err;
    if (r.node && (r.node->kind == PX_NULL || r.node->kind == PX_CONSOLE || r.node->kind == PX_AUDIO || r.node->kind == PX_MIXER || r.node->kind == PX_NET || r.node->kind == PX_MEDIA || r.node->kind==PX_COM)) {
        if ((err = device_open(r.node->kind, flags))) return err;
    } else if (r.node && directory(r.node)) {
        if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC))) return -EISDIR;
    } else if (flags & O_DIRECTORY) return -ENOTDIR;
    if (r.node && (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) return -EEXIST;
    if (!r.node && r.trailing) return -ENOENT;
    if (r.node && r.node->volume) {
        unsigned access = (flags & O_ACCMODE) == O_RDONLY ? VFS_READ :
                          (flags & O_ACCMODE) == O_WRONLY ? VFS_WRITE : VFS_READ | VFS_WRITE;
        if ((err = px_share_locked(r.node, d->win32 ? d->share_access : access, d->deny))) return err;
        if (access & VFS_WRITE) {
            if (r.node->volume->readonly) return -EROFS;
            if (r.node->entry.attr & FAT_ATTR_RO) return -EACCES;
        }
    }
    if (!r.node) {
        /* Reserve metadata BEFORE FAT create; allocation failure is pure. */
        struct px_node *n = node_new(s, r.parent, PX_FILE);
        if (!n) return -ENOMEM;
        n->linked = false; n->volume=r.parent->volume; n->drive=r.parent->drive;
        vfs_tag_locked(s->vfs,"create");
        vfs_site_locked(s->vfs,"native");
        n->generation=r.parent->generation;
        err = px_fat_transaction_locked(n,r.parent,0,(struct fat_transaction_args){.op=FAT_TX_CREATE,.directory=r.parent->entry.first,.name=r.name,.entry=&n->entry});
        if (err) { node_forget(n); return px_error(err); }
        n->linked = true; n->volume = r.parent->volume; n->drive = r.parent->drive;
        n->generation = r.parent->generation; r.node = n;
        px_changed_locked(n); px_changed_locked(r.parent);
    }
    if (flags & O_TRUNC) {
        if ((err=px_range_locked(r.node,0,r.node->entry.size))) return err;
        vfs_tag_locked(s->vfs,"truncate");
        vfs_site_locked(s->vfs,"native");
        err = px_fat_transaction_locked(r.node,r.node->parent,0,(struct fat_transaction_args){.op=FAT_TX_TRUNCATE,.entry=&r.node->entry});
        px_changed_locked(r.node);
        if (err) return px_error(err);
    }
    d->node = r.node;
    if (r.node->kind==PX_COM) {
        err=com_device_open ? com_device_open(d,flags) : -ENODEV;
        if (err) { d->node=0; return err; }
    }
    if (r.node->kind == PX_AUDIO || r.node->kind == PX_MIXER) {
        err = audio_device_open ? audio_device_open(d, flags) : r.node->kind == PX_AUDIO ? -ENODEV : 0;
        if (err) { d->node = 0; return err; }
    }
    px_retain(r.node);
    d->next = s->descriptions; s->descriptions = d;
    return 0;
}

static int mkdir_inner(struct px_namespace *, struct px_node *, const char *, uint32_t);
int px_mkdir_locked(struct px_namespace *s, struct px_node *cwd, const char *path, uint32_t mode)
{
    if (px_validate(path)) return mkdir_inner(s, cwd, path, mode);
    uint32_t turn; int err = px_error(vfs_turn_take_locked(s->vfs, path_drives(s, cwd, path), &turn));
    if (!err) err = mkdir_inner(s, cwd, path, mode);
    vfs_turn_drop_locked(s->vfs, turn);
    return err;
}
static int mkdir_inner(struct px_namespace *s, struct px_node *cwd, const char *path, uint32_t mode)
{
    vfs_tag_locked(s->vfs,"mkdir");
    vfs_site_locked(s->vfs,"native");
    if (mode & ~0777u) return -EINVAL;
    struct px_path r; int err = px_resolve_locked(s, cwd, path, true, &r);
    if (err) return err;
    if ((r.node && r.node->ro) || (r.parent && r.parent->ro)) return -EROFS;
    if (r.node) return -EEXIST;
    struct px_node *n = node_new(s, r.parent, PX_DIRECTORY);
    if (!n) return -ENOMEM;
    n->linked = false; n->volume=r.parent->volume; n->drive=r.parent->drive;
    vfs_tag_locked(s->vfs,"mkdir");
    vfs_site_locked(s->vfs,"native");
    n->generation=r.parent->generation;
    err = px_fat_transaction_locked(n,r.parent,0,(struct fat_transaction_args){.op=FAT_TX_CREATE,.directory=r.parent->entry.first,.name=r.name,.attr=FAT_ATTR_DIR,.entry=&n->entry});
    if (err) { node_forget(n); return px_error(err); }
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
    /* Detach first. An open admitted before the tombstone owns its chain
     * even when there were no opens at initial planning. */
    bool retain = n->kind == PX_FILE;
    if (retain && (!fat_native_ready || !fat_native_ready())) return -EIO;
    struct fat_entry entry = n->entry;
    if (retain) entry.first = 0; /* durable name removal, no chain release */
    vfs_tag_locked(n->space->vfs,"unlink");
    vfs_site_locked(n->space->vfs,"native");
    int err = px_fat_transaction_locked(n,n->parent,0,(struct fat_transaction_args){.op=FAT_TX_REMOVE,.entry=&entry,.flag=directory(n)});
    if (err) return px_error(err);
    n->linked = false;
    if (retain) n->entry.index = UINT32_MAX;
    else { n->entry.first = 0; n->entry.size = 0; }
    if (retain && !n->refs && !legacy && n->entry.first) {
        err=px_fat_transaction_locked(n,n->parent,0,(struct fat_transaction_args){.op=FAT_TX_REMOVE,.entry=&n->entry});
        if (err) return px_error(err);
        n->entry.first=0; n->entry.size=0;
    }
    px_changed_locked(n); px_changed_locked(n->parent);
    node_forget(n);
    return 0;
}
int px_delete_finish_locked(struct px_node *n)
{
    if (!n->delete_pending || !n->linked) return 0;
    for (struct file_description *d=n->space->descriptions;d;d=d->next)
        if (d->node==n && d->references) return 0;
    if (legacy_open(n)) return 0;
    return remove_node(n);
}
int px_drop_locked(struct px_node *n)
{
    if (n->volume || n->ro) vfs_reserve_locked(n->space->vfs,vfs_drive_mask(n->drive));
    px_release(n);
    if (!n->ro && !n->refs && !n->linked && n->kind == PX_FILE && !legacy_open(n) && n->entry.first) {
        /* Detached media names are not open-unlinked files on a live FAT.
         * Closing an expired description must never release its old chain. */
        if (n->drive>=26 || n->space->vfs->generation[n->drive]!=n->generation ||
            n->space->vfs->volumes[n->drive]!=n->volume) { node_forget(n); return 0; }
        vfs_tag_locked(n->space->vfs,"unlink");
        vfs_site_locked(n->space->vfs,"native");
        int err = px_fat_transaction_locked(n,n->parent,0,(struct fat_transaction_args){.op=FAT_TX_REMOVE,.entry=&n->entry});
        n->entry.first = 0; n->entry.size = 0;
        node_forget(n);
        return px_error(err);
    }
    node_forget(n);
    return 0;
}
static int remove_inner(struct px_namespace *, struct px_node *, const char *, bool);
int px_remove_locked(struct px_namespace *s, struct px_node *cwd, const char *path, bool dir)
{
    if (px_validate(path)) return remove_inner(s, cwd, path, dir);
    uint32_t turn; int err = px_error(vfs_turn_take_locked(s->vfs, path_drives(s, cwd, path), &turn));
    if (!err) err = remove_inner(s, cwd, path, dir);
    vfs_turn_drop_locked(s->vfs, turn);
    return err;
}
static int remove_inner(struct px_namespace *s, struct px_node *cwd, const char *path, bool dir)
{
    vfs_tag_locked(s->vfs,"unlink");
    vfs_site_locked(s->vfs,"native");
    struct px_path r; int err = px_resolve_locked(s, cwd, path, false, &r);
    if (err) return err;
    if (r.node->ro) return -EROFS;
    if (dir && r.dot) return -EINVAL;
    if (!dir && directory(r.node)) return -EISDIR;
    if (dir && !directory(r.node)) return -ENOTDIR;
    if (!r.node->volume || r.node->mountpoint) return -EBUSY;
    if (px_share_locked(r.node,4,0)) return -EACCES;
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
    while (!(err = px_fat_transaction_locked(n,n->parent,0,(struct fat_transaction_args){.op=FAT_TX_NEXT,.directory=n->entry.first,.directory_cursor=&cursor,.entry=&child})))
        if (!path_equal(child.name, ".") && !path_equal(child.name, "..")) return -ENOTEMPTY;
    return err == -FS_ENOENT ? 0 : px_error(err);
}
static int rename_inner(struct px_namespace *, struct px_node *, const char *, const char *);
int px_rename_locked(struct px_namespace *s, struct px_node *cwd, const char *from, const char *to)
{
    if (px_validate(from) || px_validate(to)) return rename_inner(s, cwd, from, to);
    uint32_t turn; int err = px_error(vfs_turn_take_locked(s->vfs, path_drives(s, cwd, from) | path_drives(s, cwd, to), &turn));
    if (!err) err = rename_inner(s, cwd, from, to);
    vfs_turn_drop_locked(s->vfs, turn);
    return err;
}
static int rename_inner(struct px_namespace *s, struct px_node *cwd, const char *from, const char *to)
{
    vfs_tag_locked(s->vfs,"rename");
    vfs_site_locked(s->vfs,"native");
    int valid=px_validate(from); if (!valid) valid=px_validate(to); if (valid) return valid;
    reserve_paths(s,cwd,from,to);
    struct px_path a, b;
    int err = px_resolve_locked(s, cwd, from, false, &a);
    if (!err) {
        /* Destination lookup drops namespace while fetching cold metadata.
         * Keep the already-resolved source alive and reject a changed view. */
        px_retain(a.node); px_retain(a.parent);
        uint64_t sequence=a.node->volume ? a.node->volume->mutation_sequence : 0;
        err=px_resolve_locked(s,cwd,to,true,&b);
        if (!err && a.node->volume && a.node->volume->mutation_sequence!=sequence) err=-EAGAIN;
        px_release(a.parent); px_release(a.node);
    }
    if (err) return err;
    if (a.node->ro || (b.parent && b.parent->ro)) return -EROFS;
    if (a.dot || b.dot) return -EINVAL;
    if (a.node->delete_pending || (b.node && b.node->delete_pending)) return -EACCES;
    if (px_share_locked(a.node,4,0) || (b.node && px_share_locked(b.node,4,0))) return -EACCES;
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
        bool retain = b.node->kind == PX_FILE;
        vfs_tag_locked(s->vfs,"rename");
        vfs_site_locked(s->vfs,"native");
        err = px_fat_transaction_locked(a.node,b.parent,b.node,(struct fat_transaction_args){.op=FAT_TX_RENAME_REPLACE,
            .entry=&a.node->entry,.replacement=&b.node->entry,.directory=b.parent->entry.first,.name=b.name,.flag=retain});
        if (b.node->entry.index == UINT32_MAX) {
            b.node->linked = false;
            if (!err && retain && !b.node->refs && !legacy && b.node->entry.first) {
                err=px_fat_transaction_locked(b.node,b.parent,0,(struct fat_transaction_args){.op=FAT_TX_REMOVE,.entry=&b.node->entry});
                if (!err) { b.node->entry.first=0; b.node->entry.size=0; }
            }
            px_changed_locked(b.node); node_forget(b.node);
        }
    } else if (fat_rename_replace) {
        vfs_tag_locked(s->vfs,"rename");
        vfs_site_locked(s->vfs,"native");
        err = px_fat_transaction_locked(a.node,b.parent,0,(struct fat_transaction_args){.op=FAT_TX_RENAME_REPLACE,
            .entry=&a.node->entry,.directory=b.parent->entry.first,.name=b.name});
    } else {
        vfs_tag_locked(s->vfs,"rename");
        vfs_site_locked(s->vfs,"native");
        err = px_fat_transaction_locked(a.node,b.parent,0,(struct fat_transaction_args){.op=FAT_TX_RENAME,
            .entry=&a.node->entry,.directory=b.parent->entry.first,.name=b.name});
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
    vfs_tag_locked(dir->space->vfs,"readdir"); vfs_site_locked(dir->space->vfs,"native");
    uint64_t listed_size = 0; int64_t listed_mtime = 0; uint8_t listed_type = 0;
    if (dir==dir->space->mnt) {
        uint32_t mask=0; for (unsigned i=3;i<26;i++) if (dir->space->vfs->ro_volumes[i]) mask|=1u<<i;
        vfs_reserve_locked(dir->space->vfs,mask);
    }
    int live=px_ro_check_locked(dir); if (live) return live;
    if (!directory(dir)) return -ENOTDIR;
    const char *name;
    uint32_t cursor = d->cursor;
    if (d->position < 2) { name = d->position ? ".." : "."; n = d->position ? dir->parent : dir; }
    else if (dir == dir->space->dev) {
        struct px_node *devices[] = { dir->space->null, dir->space->console, dir->space->audio, dir->space->mixer, dir->space->net, dir->space->media };
        if (d->position<8) n=devices[d->position-2];
        else {
            unsigned index=0;
            for (unsigned port=0;port<CIUKI_COM_MAX;port++) {
                struct px_node *com=dir->space->com[port];
                if (com && com->linked && index++==d->position-8) n=com;
            }
            if (!n) return 0;
        }
        name=n->entry.name;
    } else if (dir == dir->space->mnt) {
        if (!cursor) cursor = 3;
        while (cursor < 26 && !vfs_drive_present(dir->space->vfs,cursor)) cursor++;
        if (cursor == 26) return 0;
        n = mount_root(dir->space, cursor++); if (!n) return -ENOMEM;
        name = n->entry.name;
    } else if (dir == dir->space->root && d->position < 4) {
        n = d->position == 2 ? dir->space->mnt : dir->space->dev; name = n->entry.name;
    } else if (dir->ro) {
        struct fs_ro_node entry; int err=VFS_IO(dir->space->vfs,fs_ro_next(dir->ro,dir->ro_id,&cursor,&entry));
        if (err) { px_ro_check_locked(dir); return err==-FS_ENOENT ? 0 : px_error(err); }
        n=ro_node_entry(dir->space,dir,&entry); if (!n) return -ENOMEM; name=n->entry.name;
    } else {
        struct fat_entry entry; int err;
        for (;;) {
            /* f1-93: never a cold directory sector under the reservation.
             * f1-93b: nor a preemption point; the description keeps the
             * walk position so a step does not walk the chain from its start. */
            err = vfs_dir_walk_locked ? vfs_dir_walk_locked(dir->space->vfs, dir->volume, dir->entry.first, 0, &cursor, &entry, false, &d->dir_hint) :
                vfs_dir_step_locked ? vfs_dir_step_locked(dir->space->vfs, dir->volume, dir->entry.first, 0, &cursor, &entry, false) :
                VFS_IO(dir->space->vfs,fat_next(dir->volume, dir->entry.first, &cursor, &entry));
            if (err) return err == -FS_ENOENT ? 0 : px_error(err);
            if (path_equal(entry.name, ".") || path_equal(entry.name, "..")) continue;
            if (dir == dir->space->root && (path_equal(entry.name, "mnt") || path_equal(entry.name, "dev"))) continue;
            break;
        }
        n = node_entry(dir->space, dir, &entry); if (!n) return -ENOMEM;
        name = n->entry.name;
        listed_type = entry.attr & FAT_ATTR_DIR ? DT_DIR : DT_REG;
        listed_size = entry.attr & FAT_ATTR_DIR ? 0 : entry.size;
        clock_fat_utc(entry.times.write_date, entry.times.write_time, &listed_mtime);
    }
    if (d->position == INT64_MAX) return -EIO;
    memset(out, 0, sizeof(*out));
    out->d_ino = n->ino; out->d_off = (int64_t)++d->position;
    out->d_reclen = sizeof(*out); out->d_namlen = (uint16_t)strlen(name);
    out->d_type = directory(n) ? DT_DIR : n->kind == PX_FILE ? DT_REG : DT_CHR;
    if (listed_type) {
        out->d_type = listed_type; out->d_size = listed_size; out->d_mtime = listed_mtime;
    } else if (n->ro) out->d_size = n->kind == PX_FILE ? n->ro_size : 0;
    else if (n->volume) {
        out->d_size = n->kind == PX_FILE ? n->entry.size : 0;
        clock_fat_utc(n->entry.times.write_date, n->entry.times.write_time, &out->d_mtime);
    }
    memcpy(out->d_name, name, out->d_namlen); d->cursor = cursor;
    return sizeof(*out);
}

static int native_share(struct px_node *n,unsigned access,unsigned deny,unsigned mode,uint64_t owner)
{
    /* The VFS caller checks its own descriptions, including DOS compatibility
     * ownership. This observer contributes native descriptions only. */
    for (struct file_description *d=n->space->descriptions;d;d=d->next) {
        unsigned a=(d->flags&O_ACCMODE)==O_RDONLY ? VFS_READ :
                   (d->flags&O_ACCMODE)==O_WRONLY ? VFS_WRITE : VFS_READ|VFS_WRITE;
        if (!d->references || d->node!=n) continue;
        if (d->win32) a=d->share_access;
        if (mode==255) { if ((d->deny&access) || (deny&a)) return -EACCES; }
        else {
            unsigned denied=d->deny&3;
            unsigned old=denied==3 ? 1 : denied==2 ? 2 : denied==1 ? 3 : 4;
            a&=3;
            int e=vfs_dos_share(old,a,0,mode,access,owner,n->ro || (n->entry.attr&FAT_ATTR_RO));
            if (e) return e;
        }
    }
    return 0;
}
int px_legacy_share(struct vfs *v, struct fat_volume *vol, const struct fat_entry *e, unsigned access, unsigned deny,unsigned mode,uint64_t owner)
{
    struct px_namespace *s=space_find(v); if (!s) return 0;
    struct px_node *n=node_find(s,vol,e);
    return n && n->delete_pending ? -EACCES : n ? native_share(n,access,deny,mode,owner) : 0;
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
void px_legacy_changed(struct vfs *v, struct fat_volume *vol, const struct fat_entry *old, const struct fat_entry *entry, bool committed_write)
{
    struct px_namespace *s = space_find(v); if (!s) return;
    struct px_node *n = node_find(s, vol, old);
    if (!n && entry)
        for (struct px_node *candidate = s->nodes; candidate; candidate = candidate->next)
            if (candidate->legacy && entry == &candidate->legacy->entry) { n = candidate; break; }
    if (!n) return;
    if (!committed_write && entry && !memcmp(old, entry, sizeof(*entry))) return;
    if (!entry) { n->linked = false; n->entry.index = UINT32_MAX; } /* open-unlinked, f1-89 item 2 */
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
        if (n->delete_pending && n->linked) {
            px_retain(n);int e=px_delete_finish_locked(n);int drop=px_drop_locked(n);return e ? e : drop;
        }
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
    for (struct px_node *n = s->nodes; n; n = n->next) if ((n->volume || n->ro) && n->drive == drive) n->linked = false;
}

bool px_ro_busy(struct vfs *v, struct fs_ro_volume *ro)
{
    struct px_namespace *s=space_find(v); if (!s) return false;
    for (struct file_description *d=s->descriptions; d; d=d->next)
        if (d->node->ro==ro && d->node->linked && d->node->generation==v->generation[d->node->drive]) return true;
    return false;
}
int px_ro_share(struct vfs *v, unsigned drive, uint64_t id, unsigned access, unsigned deny,unsigned mode,uint64_t owner)
{
    struct px_namespace *s=space_find(v); if (!s) return 0;
    for (struct px_node *n=s->nodes; n; n=n->next)
        if (n->linked && n->ro==v->ro_volumes[drive] && n->ro_id==id && n->generation==v->generation[drive]) return native_share(n,access,deny,mode,owner);
    return 0;
}
