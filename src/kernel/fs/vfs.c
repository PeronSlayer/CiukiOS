/* SPDX-License-Identifier: GPL-2.0-only */
#include "vfs.h"
#include "../include/ciuki/vfs_hooks.h"
#ifndef FS_HOST
#include <ciuki/kernel.h>
#endif
/* The host can inspect waits without requiring kernel logging. */
void vfs_wait_observer(char,uint32_t,char,const char *) __attribute__((weak));
static char first_drive(uint32_t mask) {
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) return (char)('A'+d);
    return '-';
}
static void wait_report(uint32_t start,char drive,char holder,const char *reason) {
    uint32_t ms=fs_now_ms()-start;
    if (ms<100) return;
#ifndef FS_HOST
    klog_diag("[storage] vfs_wait drive=%c ms=%u holder=%c reason=%s",drive,ms,holder,reason);
#endif
    if (vfs_wait_observer) vfs_wait_observer(drive,ms,holder,reason);
}
static void namespace_take(struct vfs *v,uint32_t mask) {
    uint32_t start=fs_now_ms();
    char holder=__atomic_load_n(&v->holder,__ATOMIC_RELAXED);
    fs_lock_take(&v->lock);
    wait_report(start,first_drive(mask),holder,"namespace");
}
/* f2-03 integration notes: weak observers keep the F1 harness independent.
 * 1. Detach respects native opens and invalidates mount-lifetime identities.
 * 2. Native/legacy share claims use the same lock and bidirectional matrix.
 * 3. Legacy mutations update native cached metadata/identities under that lock.
 * 4. The final legacy close releases retained open-unlinked chains exactly once.
 * 5. Legacy namespace mutations respect native cwd/open-directory pins.
 * 6. Native rmdir/replacement also honors legacy per-drive cwd pins. */
struct resolved { unsigned drive; uint32_t parent; char name[FS_NAME_BYTES]; };
void vfs_init(struct vfs *v) {
    memset(v,0,sizeof(*v)); fs_lock_init(&v->lock); fs_lock_classify(&v->lock,1,'-');
    v->holder='-';
    fs_event_init(&v->volume_changed);
    for (unsigned d=0;d<26;d++) { fs_lock_init(&v->volume_lock[d]); fs_lock_classify(&v->volume_lock[d],2,'A'+d); }
}
void vfs_destroy(struct vfs *v) {
    if (files_detach) files_detach(v);
    for (unsigned d=0;d<26;d++) fs_lock_destroy(&v->volume_lock[d]);
    fs_event_destroy(&v->volume_changed); fs_lock_destroy(&v->lock);
}
static void volume_mutexes(struct vfs *v,uint32_t mask,bool take) {
    for (unsigned i=0;i<26;i++) {
        unsigned d=take ? i : 25-i;
        if (mask & (1u<<d)) { if (take) fs_lock_take(&v->volume_lock[d]); else fs_lock_drop(&v->volume_lock[d]); }
    }
}
void vfs_reserve_locked(struct vfs *v,uint32_t mask) {
    mask &= VFS_ALL_DRIVES; mask &= ~v->active;
    if (!mask) return;
    uint32_t ticket[26]={0};
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) {
        if (v->active && v->next[d]!=v->serving[d]) fs_panic("late volume reservation");
        ticket[d]=v->next[d]++;
    }
    uint32_t start=fs_now_ms(); char holder=first_drive((v->reserved|v->mutating) & mask);
    for (;;) {
        bool ready=true;
        for (unsigned d=0;d<26;d++) if ((mask&(1u<<d)) &&
            (ticket[d]!=v->serving[d] || v->readers[d] || (v->mutating&(1u<<d)))) ready=false;
        if (ready) break;
        fs_event_wait(&v->volume_changed,&v->lock);
    }
    wait_report(start,first_drive(mask),holder,"volume");
    v->reserved |= mask;
    v->active_tickets |= mask;
    /* A free additional letter can precede an existing one. Reacquire the
     * complete set in order, retaining all reservations under namespace. */
    volume_mutexes(v,v->active,false); v->active |= mask;
    volume_mutexes(v,v->active,true);
    __atomic_store_n(&v->holder,first_drive(v->active),__ATOMIC_RELAXED);
}
void vfs_lock(struct vfs *v,uint32_t mask) {
    namespace_take(v,mask); vfs_reserve_locked(v,mask); v->default_drive=26;
}
void vfs_unlock(struct vfs *v) {
    uint32_t mask=v->active,tickets=v->active_tickets;
    v->active=0; v->active_tickets=0; v->reserved &= ~mask; v->mutating &= ~mask;
    for (unsigned d=0;d<26;d++) if (tickets&(1u<<d)) v->serving[d]++;
    __atomic_store_n(&v->holder,'-',__ATOMIC_RELAXED);
    volume_mutexes(v,mask,false); fs_lock_drop(&v->lock);
    if (mask) fs_event_wake(&v->volume_changed);
}
struct vfs_io_state vfs_io_begin_locked(struct vfs *v) {
    struct vfs_io_state state={v->active,v->default_drive};
    /* Transfer lifetime/ordering ownership to the mutation guard. It is
     * distinct from the FIFO metadata ticket and never borrowed by a data
     * reader. Repeated FAT phases retain this guard, preserving every C2
     * boundary while no ticket spans a transport wait. */
    uint32_t tickets=v->active_tickets;
    v->mutating|=v->active; v->reserved&=~v->active;
    v->active=0; v->active_tickets=0;
    for (unsigned d=0;d<26;d++) if (tickets&(1u<<d)) v->serving[d]++;
    if (tickets) fs_event_wake(&v->volume_changed);
    __atomic_store_n(&v->holder,'-',__ATOMIC_RELAXED);
    fs_lock_drop(&v->lock); return state;
}
void vfs_io_end_locked(struct vfs *v,struct vfs_io_state state) {
    uint32_t mask=state.drives;
    volume_mutexes(v,mask,false); namespace_take(v,mask);
    volume_mutexes(v,mask,true); v->active=mask;
    v->default_drive=state.default_drive;
    __atomic_store_n(&v->holder,first_drive(mask),__ATOMIC_RELAXED);
}
static uint32_t vfs_path_mask(struct vfs_table *t,const char *path) {
    unsigned d=t->drive;
    if (path && path[0] && path[1]==':') d=(unsigned)(path_upper((uint8_t)path[0])-'A');
    return vfs_drive_mask(d);
}
static void path_lock(struct vfs_table *t,const char *a,const char *b,const char *c) {
    for (;;) {
        vfs_lock(t->vfs,0);
        uint32_t mask=vfs_path_mask(t,a) | (b ? vfs_path_mask(t,b) : 0) | (c ? vfs_path_mask(t,c) : 0);
        vfs_reserve_locked(t->vfs,mask);
        uint32_t now=vfs_path_mask(t,a) | (b ? vfs_path_mask(t,b) : 0) | (c ? vfs_path_mask(t,c) : 0);
        if (!(now & ~t->vfs->active)) { t->vfs->default_drive=t->drive; return; }
        vfs_unlock(t->vfs); /* default drive changed during admission */
    }
}
int vfs_attach_locked(struct vfs *v, unsigned d, struct fat_volume *vol) {
    if (d>=26 || !vol || !vol->mounted) return -FS_EINVAL;
    /* Publication is namespace-only. A preparer/teardown that owns this
     * letter must finish first; queued, unadmitted operations borrow no
     * identity yet and will inspect the new publication when admitted. */
    if (((v->reserved|v->mutating) & ~v->active) & vfs_drive_mask(d)) return -FS_EBUSY;
    int e=0;
    if (vfs_drive_present(v,d)) e=-FS_EBUSY;
    for (unsigned i=0;i<26;i++) if (v->volumes[i]) {
        struct fat_volume *other=v->volumes[i];
        if (other->cache!=vol->cache) e=-FS_EINVAL;
        if (other->dev==vol->dev && other->start<vol->start+vol->sectors && vol->start<other->start+other->sectors) e=-FS_EBUSY;
    }
    if (!e) { v->volumes[d]=vol; v->generation[d]++; }
    return e;
}
int vfs_attach(struct vfs *v, unsigned d, struct fat_volume *vol) {
    vfs_lock(v,0); int e=vfs_attach_locked(v,d,vol);
    vfs_unlock(v); return e;
}
bool vfs_drive_present(struct vfs *v, unsigned d) { return d < 26 && (v->volumes[d] || v->ro_volumes[d]); }
int vfs_attach_ro_locked(struct vfs *v, unsigned d, struct fs_ro_volume *vol)
{
    if (d < 3 || d >= 26 || !vol || !vol->mounted) return -FS_EINVAL;
    if (((v->reserved|v->mutating) & ~v->active) & vfs_drive_mask(d)) return -FS_EBUSY;
    int e = vfs_drive_present(v, d) ? -FS_EBUSY : 0;
    for (unsigned i = 0; !e && i < 26; i++) {
        if (v->volumes[i] && (v->volumes[i]->cache != vol->cache || v->volumes[i]->dev == vol->dev)) e = -FS_EBUSY;
        if (v->ro_volumes[i] && (v->ro_volumes[i]->cache != vol->cache || v->ro_volumes[i]->dev == vol->dev)) e = -FS_EBUSY;
    }
    if (!e) { v->ro_volumes[d] = vol; v->generation[d]++; }
    return e;
}
int vfs_attach_ro(struct vfs *v, unsigned d, struct fs_ro_volume *vol)
{
    vfs_lock(v,0);
    int e = vfs_attach_ro_locked(v, d, vol);
    vfs_unlock(v); return e;
}
int vfs_ro_check_locked(struct vfs *v, unsigned d)
{
    vfs_reserve_locked(v,vfs_drive_mask(d));
    if (d >= 26 || !v->ro_volumes[d]) return -FS_EIO;
    struct fs_ro_volume *ro = v->ro_volumes[d];
    if (!VFS_IO(v,fs_ro_check(ro))) return 0;
    vfs_ro_expire_locked(v, d);
    return -FS_EIO;
}
void vfs_ro_expire_locked(struct vfs *v, unsigned d)
{
    vfs_reserve_locked(v,vfs_drive_mask(d));
    if (d >= 26 || !v->ro_volumes[d]) return;
    struct fs_ro_volume *ro = v->ro_volumes[d];
    if (px_volume_detached) px_volume_detached(v, d);
    VFS_IO_VOID(v,fs_ro_unmount(ro)); v->ro_volumes[d] = 0; v->generation[d]++;
    for (unsigned i = 0; i < VFS_TABLES; i++) if (v->tables[i]) {
        memcpy(v->tables[i]->cwd[d], "/", 2); v->tables[i]->cwd_cluster[d] = 0;
    }
}
static bool ro_target(struct vfs_table *t, const char *path)
{
    unsigned d = t->drive;
    if (path && path[0] && path[1] == ':') d = (unsigned)(path_upper((uint8_t)path[0]) - 'A');
    vfs_lock(t->vfs,0);
    bool result=d<26 && t->vfs->ro_volumes[d];
    vfs_unlock(t->vfs); return result;
}
static int ro_path(struct vfs_table *t, const char *path, unsigned *drive, struct fs_ro_node *node,
                   char canonical[FS_PATH_BYTES])
{
    unsigned d = t->vfs->default_drive;
    if (!path || !path[0]) return -FS_EINVAL;
    if (path[1] == ':') d = (unsigned)(path_upper((uint8_t)path[0]) - 'A');
    if (d >= 26) return -FS_EINVAL;
    char full[FS_PATH_BYTES];
    int e = path_normalize(path, d, t->cwd[d], drive, full); if (e) return e;
    e = vfs_ro_check_locked(t->vfs, d); if (e) return e;
    struct fs_ro_volume *v = t->vfs->ro_volumes[d];
    e = VFS_IO(t->vfs,v->ops->root(v, node)); if (e) return e;
    if (canonical) memcpy(canonical, "/", 2);
    const char *p = full + 1; unsigned used = 1;
    while (*p) {
        char name[FS_NAME_BYTES]; unsigned n = 0;
        while (*p && *p != '/') name[n++] = *p++;
        name[n] = 0;
        if (!(node->attr & FS_RO_DIRECTORY)) return -FS_ENOTDIR;
        e = VFS_IO(t->vfs,fs_ro_lookup(v, node->id, name, node)); if (e) break;
        if (canonical) {
            n = (unsigned)strlen(node->name);
            if (used > 1) canonical[used++] = '/';
            if (used + n >= FS_PATH_BYTES) return -FS_ENAMETOOLONG;
            memcpy(canonical + used, node->name, n + 1); used += n;
        }
        if (*p) p++;
    }
    if (e == -FS_EIO) vfs_ro_check_locked(t->vfs, d);
    return e;
}
static void ro_entry(const struct fs_ro_node *n, struct fat_entry *out)
{
    memset(out, 0, sizeof(*out)); memcpy(out->name, n->name, sizeof(out->name));
    memcpy(out->alias, n->alias, sizeof(out->alias)); out->size = (uint32_t)n->size; out->attr = n->attr;
}
static int ro_open(struct vfs_table *t, const char *path, unsigned flags, unsigned deny)
{
    struct vfs *v = t->vfs; path_lock(t,path,0,0);
    struct fs_ro_node ent; unsigned drive; int e;
    if (flags & (VFS_WRITE | VFS_CREATE | VFS_TRUNCATE)) { e = -FS_EROFS; goto out; }
    e = ro_path(t, path, &drive, &ent, 0); if (e) goto out;
    if (ent.attr & FS_RO_DIRECTORY) { e = -FS_EISDIR; goto out; }
    if (px_ro_share && (e=px_ro_share(v,drive,ent.id,VFS_READ,deny))) goto out;
    if (ent.size > UINT32_MAX) { e = -FS_EFBIG; goto out; }
    struct vfs_node *node = 0, *free_node = 0;
    for (unsigned i = 0; i < VFS_NODES; i++) {
        struct vfs_node *n = &v->nodes[i];
        if (!n->refs) free_node = n;
        else if (n->ro == v->ro_volumes[drive] && n->ro_id == ent.id && n->generation == v->generation[drive]) node = n;
    }
    if (node) for (unsigned i = 0; i < VFS_DESCRIPTIONS; i++) {
        struct vfs_description *d = &v->descriptions[i];
        if (d->refs && d->node == node && ((d->deny & VFS_READ) || (deny & d->access))) { e = -FS_EACCES; goto out; }
    }
    if (!node) node = free_node;
    struct vfs_description *desc = 0; int h = -1;
    for (unsigned i = 0; i < VFS_DESCRIPTIONS; i++) if (!v->descriptions[i].refs) { desc = &v->descriptions[i]; break; }
    for (unsigned i = 0; i < VFS_HANDLES; i++) if (!t->handles[i]) { h = (int)i; break; }
    if (!node || !desc || h < 0) { e = -FS_EMFILE; goto out; }
    if (!node->refs) {
        node->ro = v->ro_volumes[drive]; node->ro_id = ent.id; node->ro_size = ent.size;
        node->drive = drive; node->generation = v->generation[drive]; ro_entry(&ent, &node->entry);
    }
    node->refs++; *desc = (struct vfs_description){ .node = node, .refs = 1, .access = VFS_READ, .deny = deny };
    t->handles[h] = desc; e = h;
out: vfs_unlock(v); return e;
}
static int ro_handle(struct vfs *v, struct vfs_node *n)
{
    if (n->drive >= 26 || n->generation != v->generation[n->drive] || v->ro_volumes[n->drive] != n->ro) return -FS_EIO;
    return vfs_ro_check_locked(v, n->drive);
}
int vfs_detach_locked(struct vfs *v, unsigned d) {
    vfs_reserve_locked(v,vfs_drive_mask(d));
    if (d>=26) return -FS_EINVAL;
    if (v->ro_volumes[d]) {
        struct fs_ro_volume *ro = v->ro_volumes[d];
        for (unsigned i=0; i<VFS_NODES; i++) if (v->nodes[i].refs && v->nodes[i].ro==ro && v->nodes[i].generation==v->generation[d]) return -FS_EBUSY;
        if (px_ro_busy && px_ro_busy(v,ro)) return -FS_EBUSY;
        if (px_volume_detached) px_volume_detached(v,d);
        VFS_IO_VOID(v,fs_ro_unmount(ro)); v->ro_volumes[d]=0; v->generation[d]++;
        for (unsigned i=0; i<VFS_TABLES; i++) if (v->tables[i]) { memcpy(v->tables[i]->cwd[d],"/",2); v->tables[i]->cwd_cluster[d]=0; }
        return 0;
    }
    int e=0; struct fat_volume *vol=v->volumes[d];
    if (!vol) e=-FS_ENOENT;
    for (unsigned i=0;i<VFS_NODES;i++) if (v->nodes[i].refs && v->nodes[i].volume==vol) e=-FS_EBUSY;
    if (!e && px_volume_busy && px_volume_busy(v,vol)) e=-FS_EBUSY;
    if (!e) e=VFS_IO(v,fat_unmount(vol));
    if (!e) {
        if (px_volume_detached) px_volume_detached(v,d);
        v->volumes[d]=0; v->generation[d]++;
        for (unsigned i=0;i<VFS_TABLES;i++) if (v->tables[i]) {
            v->tables[i]->cwd[d][0]='/'; v->tables[i]->cwd[d][1]=0; v->tables[i]->cwd_cluster[d]=0;
        }
    }
    return e;
}
int vfs_detach(struct vfs *v, unsigned d) {
    vfs_lock(v,0);
    int e=vfs_detach_locked(v,d);
    vfs_unlock(v); return e;
}
int vfs_table_init(struct vfs *v, struct vfs_table *t, unsigned d) {
    if (d>=26) return -FS_EINVAL;
    vfs_lock(v,0); int slot=-1;
    for (unsigned i=0;i<VFS_TABLES;i++) { if (v->tables[i]==t) { vfs_unlock(v); return -FS_EBUSY; } if (!v->tables[i]) slot=(int)i; }
    if (slot<0) { vfs_unlock(v); return -FS_EMFILE; }
    memset(t,0,sizeof(*t)); t->vfs=v; t->drive=d;
    for (unsigned i=0;i<26;i++) { t->cwd[i][0]='/'; t->cwd[i][1]=0; }
    v->tables[slot]=t; vfs_unlock(v); return 0;
}
static int release_description(struct vfs *v,struct vfs_description *d) {
    if (!d) return 0;
    int e=0;
    if (d->refs==1) {
        /* Retain the slots until final-close callbacks finish their I/O. */
        if (d->node->refs==1) {
            if (px_legacy_closed) e=px_legacy_closed(v,d->node);
            memset(d->node,0,sizeof(*d->node));
        } else d->node->refs--;
        memset(d,0,sizeof(*d));
    } else d->refs--;
    return e;
}
static int close_handle(struct vfs_table *t, unsigned h) {
    struct vfs_description *d=t->handles[h]; t->handles[h]=0;
    return release_description(t->vfs,d);
}
void vfs_table_destroy(struct vfs_table *t) {
    struct vfs *v=t->vfs; vfs_lock(v,0);
    uint32_t mask=0;
    for (unsigned i=0;i<VFS_HANDLES;i++) if (t->handles[i]) mask|=vfs_drive_mask(t->handles[i]->node->drive);
    vfs_reserve_locked(v,mask);
    for (unsigned i=0;i<VFS_HANDLES;i++) close_handle(t,i);
    for (unsigned i=0;i<VFS_TABLES;i++) if (v->tables[i]==t) v->tables[i]=0;
    t->vfs=0; vfs_unlock(v);
}
static int normalize(struct vfs_table *t, const char *s, unsigned *drive, char out[FS_PATH_BYTES]) {
    if (!s || !*s) return -FS_EINVAL;
    unsigned d=t->vfs->default_drive;
    if (s[1]==':') { d=(unsigned)(path_upper((uint8_t)s[0])-'A'); if (d>=26) return -FS_EINVAL; }
    int e=path_normalize(s,d,t->cwd[d],drive,out); if (e) return e;
    return t->vfs->volumes[*drive] ? 0 : -FS_ENOENT;
}
static uint32_t root(struct fat_volume *v) { return v->type==32 ? v->root : 0; }
static int resolve(struct vfs_table *t, const char *path, struct resolved *r) {
    char full[FS_PATH_BYTES]; int e=normalize(t,path,&r->drive,full); if (e) return e;
    struct fat_volume *v=t->vfs->volumes[r->drive]; r->parent=root(v); const char *s=full+1;
    if (!*s) { r->name[0]=0; return 0; }
    while (*s) {
        unsigned n=0; while (*s && *s!='/') { if (n>=sizeof(r->name)-1) return -FS_ENAMETOOLONG; r->name[n++]=*s++; } r->name[n]=0;
        if (!*s) return 0;
        struct fat_entry ent; if ((e=VFS_IO(t->vfs,fat_lookup(v,r->parent,r->name,&ent)))) return e;
        if (!(ent.attr&FAT_ATTR_DIR)) return -FS_ENOTDIR;
        r->parent=ent.first; s++;
    }
    return -FS_EINVAL;
}
static int lookup(struct vfs_table *t, const char *path, struct resolved *r, struct fat_entry *ent) {
    int e=resolve(t,path,r); if (e) return e;
    struct fat_volume *v=t->vfs->volumes[r->drive];
    if (!r->name[0]) { memset(ent,0,sizeof(*ent)); ent->first=root(v); ent->attr=FAT_ATTR_DIR; ent->index=UINT32_MAX; ent->name[0]='/'; return 0; }
    return VFS_IO(t->vfs,fat_lookup(v,r->parent,r->name,ent));
}
static struct vfs_node *node_find(struct vfs *v, struct fat_volume *vol, const struct fat_entry *ent) {
    for (unsigned i=0;i<VFS_NODES;i++) if (v->nodes[i].refs && v->nodes[i].volume==vol && v->nodes[i].entry.parent==ent->parent && v->nodes[i].entry.index==ent->index) return &v->nodes[i];
    return 0;
}
static int handle_free(struct vfs_table *t) { for (unsigned i=0;i<VFS_HANDLES;i++) if (!t->handles[i]) return (int)i; return -FS_EMFILE; }
int vfs_open(struct vfs_table *t, const char *path, unsigned flags, enum vfs_share deny, uint8_t attr) {
    if (!(flags&3) || flags&~31u || deny>VFS_DENY_ALL || (flags&VFS_TRUNCATE && !(flags&VFS_WRITE)) || (flags&VFS_EXCLUSIVE && !(flags&VFS_CREATE))) return -FS_EINVAL;
    if (ro_target(t,path)) return ro_open(t,path,flags,(unsigned)deny);
    struct vfs *v=t->vfs; path_lock(t,path,0,0);
    struct resolved r; struct fat_entry ent; int e=lookup(t,path,&r,&ent);
    bool create=e==-FS_ENOENT && (flags&VFS_CREATE);
    /* A missing ancestor is distinguished by resolving again before create. */
    if (create) e=resolve(t,path,&r);
    if (e) goto out;
    if (!create && (flags&(VFS_CREATE|VFS_EXCLUSIVE))==(VFS_CREATE|VFS_EXCLUSIVE)) { e=-FS_EEXIST; goto out; }
    struct fat_volume *vol=v->volumes[r.drive];
    if (!create && (ent.attr&FAT_ATTR_DIR)) { e=-FS_EISDIR; goto out; }
    if ((flags&VFS_WRITE) && (vol->readonly || (!create && (ent.attr&FAT_ATTR_RO)))) { e=vol->readonly ? -FS_EROFS : -FS_EACCES; goto out; }
    if (!create && px_legacy_share && (e=px_legacy_share(v,vol,&ent,flags&3,(unsigned)deny))) goto out;
    int h=handle_free(t); if (h<0) { e=h; goto out; }
    struct vfs_description *d=0; for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) if (!v->descriptions[i].refs) { d=&v->descriptions[i]; break; }
    if (!d) { e=-FS_EMFILE; goto out; }
    struct vfs_node *node=create ? 0 : node_find(v,vol,&ent);
    if (node) {
        for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) {
            struct vfs_description *other=&v->descriptions[i];
            if (other->refs && other->node==node && ((other->deny&(flags&3)) || ((unsigned)deny&other->access))) { e=-FS_EACCES; goto out; }
        }
    } else {
        for (unsigned i=0;i<VFS_NODES;i++) if (!v->nodes[i].refs) { node=&v->nodes[i]; break; }
        if (!node) { e=-FS_EMFILE; goto out; }
    }
    if (create && (attr&FAT_ATTR_DIR)) { e=-FS_EISDIR; goto out; }
    node->refs++; d->refs=1; d->node=node; t->handles[h]=d;
    if (node->refs==1) { node->volume=vol; node->drive=r.drive; node->generation=v->generation[r.drive]; }
    if (create) { if ((e=VFS_IO(v,fat_create(vol,r.parent,r.name,attr,&ent)))) goto undo; }
    if (node->refs==1) { node->volume=vol; node->drive=r.drive; node->generation=v->generation[r.drive]; node->entry=ent; node->log_capacity=0; }
    if (flags&VFS_TRUNCATE) {
        struct fat_entry before=node->entry;
        e=VFS_IO(v,fat_truncate(vol,&node->entry,0));
        if (px_legacy_changed) px_legacy_changed(v,vol,&before,&node->entry,false);
        if (e) goto undo;
    }
    d->node=node; d->position=0; d->access=flags&3; d->deny=(unsigned)deny; t->handles[h]=d; e=h;
    goto out;
undo: t->handles[h]=0; if (!--node->refs) memset(node,0,sizeof(*node)); memset(d,0,sizeof(*d));
out: vfs_unlock(v); return e;
}
static uint32_t handle_mask(struct vfs_table *t,int h) {
    struct vfs_description *d=h>=0 && h<(int)VFS_HANDLES ? t->handles[h] : 0;
    return d ? vfs_drive_mask(d->node->drive) : 0;
}
static void handle_lock(struct vfs_table *t,int h) {
    for (;;) {
        vfs_lock(t->vfs,0);
        struct vfs_description *desc=h>=0 && h<(int)VFS_HANDLES ? t->handles[h] : 0;
        if (desc && desc->io_active) {
            fs_event_wait(&t->vfs->volume_changed,&t->vfs->lock);
            vfs_unlock(t->vfs); continue;
        }
        uint32_t mask=handle_mask(t,h); vfs_reserve_locked(t->vfs,mask);
        if (handle_mask(t,h)==mask) return;
        vfs_unlock(t->vfs); /* slot was replaced while admission slept */
    }
}
static struct vfs_description *get(struct vfs_table *t, int h) { return h>=0 && h<(int)VFS_HANDLES ? t->handles[h] : 0; }
int vfs_close(struct vfs_table *t, int h) {
    handle_lock(t,h); int e=get(t,h) ? 0 : -FS_EBADF; if (!e) e=close_handle(t,(unsigned)h); vfs_unlock(t->vfs); return e;
}
int vfs_dup(struct vfs_table *src, int h, struct vfs_table *dst, int target) {
    if (src->vfs!=dst->vfs || target< -1 || target>=(int)VFS_HANDLES) return -FS_EINVAL;
    for (;;) {
        vfs_lock(src->vfs,0);
        uint32_t mask=handle_mask(src,h)|handle_mask(dst,target);
        vfs_reserve_locked(src->vfs,mask);
        if ((handle_mask(src,h)|handle_mask(dst,target))==mask) break;
        vfs_unlock(src->vfs);
    }
    struct vfs_description *d=get(src,h); int e=-FS_EBADF;
    if (d) {
        if (target==-1) target=handle_free(dst);
        if (target<0) e=target;
        else {
            struct vfs_description *old=dst->handles[target];
            d->refs++; dst->handles[target]=d;
            release_description(src->vfs,old); e=target;
        }
    }
    vfs_unlock(src->vfs); return e;
}
int vfs_read(struct vfs_table *t, int h, void *buf, size_t n, size_t *done) {
    if (!done) return -FS_EINVAL; *done=0;
    struct vfs *v=t->vfs;
    /* Open identities pin the chain against namespace removal. A published
     * writer names its node before dropping namespace. Never borrow a mutable
     * entry or volume diagnostic workspace across a different file's I/O. */
    for (;;) {
        vfs_lock(v,0); struct vfs_description *d=get(t,h);
        if (!d || d->node->ro || !(d->access&VFS_READ)) { vfs_unlock(v); break; }
        struct vfs_node *node=d->node; unsigned drive=node->drive;
        if (drive>=26 || node->generation!=v->generation[drive] || v->volumes[drive]!=node->volume) {
            vfs_unlock(v); return -FS_EIO;
        }
        uint32_t mask=vfs_drive_mask(drive);
        bool admitted=((v->reserved|v->mutating)&mask) ? v->io_file[drive] && v->io_file[drive]!=node :
            v->next[drive]==v->serving[drive];
        if (d->io_active || !admitted) {
            fs_event_wait(&v->volume_changed,&v->lock); vfs_unlock(v); continue;
        }
        struct fat_volume *vol=node->volume;
        uint32_t media_generation=vol->dev->media_generation;
        struct fat_volume view={.cache=vol->cache,.dev=vol->dev,.start=vol->start,.sectors=vol->sectors,
            .reserved=vol->reserved,.fat_sectors=vol->fat_sectors,.root_sector=vol->root_sector,
            .data_sector=vol->data_sector,.clusters=vol->clusters,.root=vol->root,
            .root_entries=vol->root_entries,.type=vol->type,.spc=vol->spc,.fats=vol->fats,
            .media=vol->media,.active_fat=vol->active_fat,.mounted=true};
        d->io_active=true; d->refs++; v->readers[drive]++; uint64_t position=d->position;
        struct fat_entry entry=node->entry;
        vfs_unlock(v);
        int e=fat_read(&view,&entry,position,buf,n,done);
        vfs_lock(v,0);
        d->position+=*done; d->io_active=false; v->readers[drive]--;
        if (view.readonly_reason) {
            fs_event_wake(&v->volume_changed);
            vfs_reserve_locked(v,mask);
            if (v->generation[drive]==node->generation && v->volumes[drive]==vol &&
                vol->dev->media_generation==media_generation) {
                vol->ro_reasons|=view.ro_reasons;
                if (!vol->readonly_reason) memcpy(vol->operation_file,view.operation_file,sizeof(vol->operation_file));
                fat_revoke_write(vol,view.readonly_reason,view.readonly_cluster);
            }
        }
        (void)release_description(v,d);
        fs_event_wake(&v->volume_changed); vfs_unlock(v); return e;
    }
    handle_lock(t,h); struct vfs_description *d=get(t,h); int e;
    if (!d) e=-FS_EBADF; else if (d->node->ro) { e=ro_handle(t->vfs,d->node); if (!e) e=VFS_IO(t->vfs,fs_ro_read(d->node->ro,d->node->ro_id,d->position,buf,n,done)); if (e) ro_handle(t->vfs,d->node); d->position+=*done; }
    else if (!(d->access&VFS_READ)) e=-FS_EACCES;
    else { e=VFS_IO(t->vfs,fat_read(d->node->volume,&d->node->entry,d->position,buf,n,done)); d->position+=*done; }
    vfs_unlock(t->vfs); return e;
}
int vfs_write(struct vfs_table *t, int h, const void *buf, size_t n, size_t *done) {
    if (!done) return -FS_EINVAL; *done=0;
    handle_lock(t,h); struct vfs_description *d=get(t,h); int e;
    if (!d) e=-FS_EBADF; else if (d->node->ro) { e=ro_handle(t->vfs,d->node); if (!e) e=-FS_EROFS; }
    else if (!(d->access&VFS_WRITE)) e=-FS_EACCES;
    else if (d->node->log_capacity) e=-FS_EBUSY;
    else {
        struct fat_entry before=d->node->entry;
        t->vfs->io_file[d->node->drive]=d->node;
        e=VFS_IO(t->vfs,fat_write(d->node->volume,&d->node->entry,d->position,buf,n,done)); d->position+=*done;
        t->vfs->io_file[d->node->drive]=0;
        if (px_legacy_changed) px_legacy_changed(t->vfs,d->node->volume,&before,&d->node->entry,*done != 0);
    }
    vfs_unlock(t->vfs); return e;
}
int vfs_seek(struct vfs_table *t, int h, int64_t off, enum vfs_whence whence, uint64_t *position) {
    if (!position || whence>VFS_SEEK_END) return -FS_EINVAL;
    for (;;) {
        vfs_lock(t->vfs,0); struct vfs_description *desc=get(t,h);
        if (desc && (desc->io_active || t->vfs->io_file[desc->node->drive]==desc->node)) {
            fs_event_wait(&t->vfs->volume_changed,&t->vfs->lock); vfs_unlock(t->vfs); continue;
        }
        if (desc && desc->node->ro) { vfs_unlock(t->vfs); handle_lock(t,h); }
        break;
    }
    struct vfs_description *d=get(t,h); int e=0;
    if (!d) e=-FS_EBADF;
    else if (d->node->ro && (e=ro_handle(t->vfs,d->node))) { }
    else {
        uint64_t base=whence==VFS_SEEK_SET ? 0 : whence==VFS_SEEK_CUR ? d->position : d->node->entry.size;
        uint64_t magnitude=off<0 ? (uint64_t)(-(off+1))+1 : (uint64_t)off;
        if (off<0 && magnitude>base) e=-FS_EINVAL;
        else if (off>=0 && magnitude>UINT64_MAX-base) e=-FS_EINVAL;
        else { d->position=off<0 ? base-magnitude : base+magnitude; *position=d->position; }
    }
    vfs_unlock(t->vfs); return e;
}
int vfs_truncate(struct vfs_table *t, int h, uint64_t size) {
    handle_lock(t,h); struct vfs_description *d=get(t,h);
    int e=!d ? -FS_EBADF : d->node->ro ? ro_handle(t->vfs,d->node) : !(d->access&VFS_WRITE) ? -FS_EACCES : 0;
    if (d && d->node->ro && !e) e=-FS_EROFS;
    if (d && d->node->log_capacity && !e) e=-FS_EBUSY;
    if (!e) {
        struct fat_entry before=d->node->entry;
        e=VFS_IO(t->vfs,fat_truncate(d->node->volume,&d->node->entry,size));
        if (px_legacy_changed) px_legacy_changed(t->vfs,d->node->volume,&before,&d->node->entry,false);
    }
    vfs_unlock(t->vfs); return e;
}
int vfs_commit(struct vfs_table *t, int h) {
    handle_lock(t,h); struct vfs_description *d=get(t,h);
    int e=!d ? -FS_EBADF : d->node->ro ? ro_handle(t->vfs,d->node) : VFS_IO(t->vfs,fat_commit(d->node->volume)); vfs_unlock(t->vfs); return e;
}
static int log_handle(struct vfs_table *t, int h, struct vfs_description **out) {
    struct vfs_description *d=get(t,h); *out=d;
    if (!d) return -FS_EBADF;
    if (d->node->ro) return -FS_EROFS;
    if (!(d->access&VFS_WRITE) || !(d->deny&VFS_DENY_WRITE)) return -FS_EACCES;
    return 0;
}
int vfs_log_reserve(struct vfs_table *t, int h, uint32_t capacity) {
    handle_lock(t,h);
    struct vfs_description *d; int e=log_handle(t,h,&d);
    if (!e && (!capacity || (capacity&511) || d->node->log_capacity)) e=-FS_EINVAL;
    if (!e) {
        struct fat_entry before=d->node->entry;
        e=VFS_IO(t->vfs,fat_truncate(d->node->volume,&d->node->entry,0));
        if (!e) e=VFS_IO(t->vfs,fat_truncate(d->node->volume,&d->node->entry,capacity));
        if (!e) {
            d->node->log_capacity=capacity; d->node->entry.size=0; d->position=0;
            e=VFS_IO(t->vfs,fat_publish_reserved(d->node->volume,&d->node->entry,false));
        }
        if (px_legacy_changed) px_legacy_changed(t->vfs,d->node->volume,&before,&d->node->entry,false);
    }
    vfs_unlock(t->vfs); return e;
}
int vfs_log_write(struct vfs_table *t, int h, const void *buf, size_t n, uint32_t size, size_t *done) {
    if (!done) return -FS_EINVAL; *done=0;
    handle_lock(t,h);
    struct vfs_description *d; int e=log_handle(t,h,&d);
    if (!e && (!d->node->log_capacity || size>d->node->log_capacity)) e=-FS_EINVAL;
    if (!e) {
        struct fat_entry before=d->node->entry;
        e=VFS_IO(t->vfs,fat_write_reserved(d->node->volume,&d->node->entry,d->node->log_capacity,d->position,buf,n,done));
        d->position+=*done;
        if (!e) d->node->entry.size=size;
        if (px_legacy_changed) px_legacy_changed(t->vfs,d->node->volume,&before,&d->node->entry,*done != 0);
    }
    vfs_unlock(t->vfs); return e;
}
int vfs_log_publish(struct vfs_table *t, int h, bool finish) {
    handle_lock(t,h);
    struct vfs_description *d; int e=log_handle(t,h,&d);
    if (!e && !d->node->log_capacity) e=-FS_EINVAL;
    if (!e) {
        struct fat_entry before=d->node->entry;
        e=VFS_IO(t->vfs,fat_publish_reserved(d->node->volume,&d->node->entry,finish));
        if (!e && finish) d->node->log_capacity=0;
        if (px_legacy_changed) px_legacy_changed(t->vfs,d->node->volume,&before,&d->node->entry,false);
    }
    vfs_unlock(t->vfs); return e;
}
int vfs_stat(struct vfs_table *t, const char *path, struct fat_entry *out) {
    if (ro_target(t,path)) {
        path_lock(t,path,0,0); struct fs_ro_node n; unsigned d;
        int e=ro_path(t,path,&d,&n,0); if (!e && n.size>UINT32_MAX) e=-FS_EFBIG;
        if (!e) ro_entry(&n,out); vfs_unlock(t->vfs); return e;
    }
    path_lock(t,path,0,0); struct resolved r; int e=lookup(t,path,&r,out);
    if (!e) { struct vfs_node *n=node_find(t->vfs,t->vfs->volumes[r.drive],out); if (n) *out=n->entry; }
    vfs_unlock(t->vfs); return e;
}
static int metadata(struct vfs_table *t, const char *path, int attr, const struct fat_times *times) {
    if (ro_target(t,path)) return -FS_EROFS;
    path_lock(t,path,0,0); struct resolved r; struct fat_entry ent; int e=lookup(t,path,&r,&ent);
    if (!e && ent.index==UINT32_MAX) e=-FS_EACCES;
    if (!e) {
        struct fat_volume *vol=t->vfs->volumes[r.drive]; struct vfs_node *n=node_find(t->vfs,vol,&ent);
        if (n) ent=n->entry;
        struct fat_entry before=ent;
        e=VFS_IO(t->vfs,fat_set_metadata(vol,&ent,attr<0 ? ent.attr : (uint8_t)attr,times));
        if (!e && px_legacy_changed) px_legacy_changed(t->vfs,vol,&before,&ent,false);
        if (!e && n) n->entry=ent;
    }
    vfs_unlock(t->vfs); return e;
}
int vfs_attrib(struct vfs_table *t, const char *p, uint8_t attr) { return metadata(t,p,attr,0); }
int vfs_times(struct vfs_table *t, const char *p, const struct fat_times *times) { return times ? metadata(t,p,-1,times) : -FS_EINVAL; }
static int find_next(struct vfs_table *t, struct vfs_find *f, struct fat_entry *ent) {
    if (f->ro) {
        if (f->drive>=26 || f->generation!=t->vfs->generation[f->drive]) return -FS_EIO;
        int e=vfs_ro_check_locked(t->vfs,f->drive); if (e) return e;
        struct fs_ro_node n;
        while (!(e=VFS_IO(t->vfs,fs_ro_next(t->vfs->ro_volumes[f->drive],f->ro_directory,&f->cursor,&n)))) {
            if (n.attr & (2|4|16) & ~f->attr_mask) continue;
            if (path_wildcard(f->pattern,n.name) || path_wildcard(f->pattern,n.alias)) {
                if (n.size>UINT32_MAX) return -FS_EFBIG; ro_entry(&n,ent); return 0;
            }
        }
        if (e==-FS_EIO) vfs_ro_check_locked(t->vfs,f->drive); return e;
    }

    if (f->drive>=26 || !t->vfs->volumes[f->drive] || f->generation!=t->vfs->generation[f->drive]) return -FS_EIO;
    int e;
    while (!(e=VFS_IO(t->vfs,fat_next(t->vfs->volumes[f->drive],f->directory,&f->cursor,ent)))) {
        if (ent->attr & (FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM|FAT_ATTR_DIR) & ~f->attr_mask) continue;
        if (path_wildcard(f->pattern,ent->name) || path_wildcard(f->pattern,ent->alias)) return 0;
    }
    return e;
}
int vfs_find_first(struct vfs_table *t, const char *p, uint8_t mask, struct vfs_find *f, struct fat_entry *ent) {
    if (ro_target(t,p)) {
        path_lock(t,p,0,0); char parent[FS_PATH_BYTES];
        size_t n=strlen(p); int e=0; if (n>=sizeof(parent)) e=-FS_ENAMETOOLONG;
        if (!e) {
            memcpy(parent,p,n+1); char *leaf=parent;
            for (char *a=parent; *a; a++) if (*a=='/' || *a=='\\' || *a==':') leaf=a+1;
            if (strlen(leaf)>=sizeof(f->pattern)) { vfs_unlock(t->vfs); return -FS_ENAMETOOLONG; }
            memset(f,0,sizeof(*f)); memcpy(f->pattern,leaf,strlen(leaf)+1);
            if (leaf==parent) memcpy(parent,".",2); else if (leaf-parent==3 && parent[1]==':') *leaf=0; else leaf[-1]=0;
            struct fs_ro_node dir; e=ro_path(t,parent,&f->drive,&dir,0);
            if (!e) { f->ro=true; f->ro_directory=dir.id; f->generation=t->vfs->generation[f->drive]; f->attr_mask=mask; e=find_next(t,f,ent); }
        }
        vfs_unlock(t->vfs); return e;
    }

    path_lock(t,p,0,0); struct resolved r; int e=resolve(t,p,&r);
    if (!e && !r.name[0]) e=-FS_EINVAL;
    if (!e) { memset(f,0,sizeof(*f)); f->drive=r.drive; f->directory=r.parent; f->generation=t->vfs->generation[r.drive]; f->attr_mask=mask; memcpy(f->pattern,r.name,strlen(r.name)+1); e=find_next(t,f,ent); }
    vfs_unlock(t->vfs); return e;
}
int vfs_find_next(struct vfs_table *t, struct vfs_find *f, struct fat_entry *ent) {
    vfs_lock(t->vfs,vfs_drive_mask(f->drive)); int e=find_next(t,f,ent); vfs_unlock(t->vfs); return e;
}
/* Does ancestor contain directory? Resolve on-disk parents, so aliases cannot
 * evade cycle/cwd checks. Root . . references are normalized to root(). */
static __attribute__((noinline)) int contains(struct vfs *space, struct fat_volume *v, uint32_t ancestor, uint32_t dir) {
    for (uint32_t i=0;i<v->clusters;i++) {
        if (dir==ancestor) return 1; if (!dir || dir==root(v)) return 0;
        struct fat_entry parent; int e=VFS_IO(space,fat_lookup(v,dir,"..",&parent)); if (e) return e;
        dir=parent.first;
    }
    return -FS_ELOOP;
}
/* Caller already holds v->lock; shared with the native path view. */
int vfs_directory_pinned(struct vfs *v, struct fat_volume *vol, uint32_t directory) {
    for (unsigned i=0;i<VFS_TABLES;i++) if (v->tables[i])
        for (unsigned d=0;d<26;d++) if (v->volumes[d]==vol) {
            uint32_t cwd=v->tables[i]->cwd_cluster[d];
            int e=contains(v,vol,directory,cwd ? cwd : root(vol));
            if (e) return e;
        }
    return 0;
}
static int busy(struct vfs *v, struct fat_volume *vol, const struct fat_entry *ent) {
    if (node_find(v,vol,ent)) return -FS_EBUSY;
    if (px_legacy_busy && px_legacy_busy(v,vol,ent)) return -FS_EBUSY;
    if (!(ent->attr&FAT_ATTR_DIR)) return 0;
    for (unsigned i=0;i<VFS_NODES;i++) if (v->nodes[i].refs && v->nodes[i].volume==vol) {
        int e=contains(v,vol,ent->first,v->nodes[i].entry.parent); if (e) return e<0 ? e : -FS_EBUSY;
    }
    for (unsigned i=0;i<VFS_TABLES;i++) if (v->tables[i]) {
        struct vfs_table *t=v->tables[i];
        for (unsigned d=0;d<26;d++) if (v->volumes[d]==vol) {
            int e=contains(v,vol,ent->first,t->cwd_cluster[d] ? t->cwd_cluster[d] : root(vol));
            if (e) return e<0 ? e : -FS_EBUSY;
        }
    }
    return 0;
}
static int remove_path(struct vfs_table *t, const char *path, bool dir) {
    if (ro_target(t,path)) return -FS_EROFS;
    path_lock(t,path,0,0); struct resolved r; struct fat_entry ent; int e=lookup(t,path,&r,&ent);
    if (!e && !r.name[0]) e=-FS_EACCES;
    if (!e) {
        struct fat_volume *v=t->vfs->volumes[r.drive];
        if (!(e=busy(t->vfs,v,&ent))) e=VFS_IO(t->vfs,fat_remove(v,&ent,dir));
        if (!e && px_legacy_changed) px_legacy_changed(t->vfs,v,&ent,0,false);
    }
    vfs_unlock(t->vfs); return e;
}
int vfs_log_rotate(struct vfs_table *t, const char *current, const char *previous,
                   const char *oldest, uint32_t capacity) {
    const char *paths[3]={current,previous,oldest};
    struct fat_entry entries[3];
    struct resolved r;
    bool present[3]={false};
    unsigned drive=26; uint32_t parent=0;
    struct vfs *v=t->vfs;
    path_lock(t,current,previous,oldest);
    int e=capacity ? 0 : -FS_EINVAL;
    /* Preflight every entry before changing any name or reservation. */
    for (unsigned i=0;!e && i<3;i++) {
        e=lookup(t,paths[i],&r,&entries[i]);
        if (e==-FS_ENOENT) e=resolve(t,paths[i],&r);
        else if (!e) present[i]=true;
        if (e) break;
        if (!i) { drive=r.drive; parent=r.parent; }
        if (r.drive!=drive || r.parent!=parent || !r.name[0]) { e=-FS_EINVAL; break; }
        if (present[i]) {
            if (entries[i].attr&(FAT_ATTR_RO|FAT_ATTR_DIR)) { e=-FS_EACCES; break; }
            e=busy(v,v->volumes[drive],&entries[i]);
        }
    }
    if (!e && v->volumes[drive]->readonly) e=-FS_EROFS;
    if (!e && present[2]) {
        e=VFS_IO(v,fat_remove(v->volumes[drive],&entries[2],false));
        if (!e && px_legacy_changed) px_legacy_changed(v,v->volumes[drive],&entries[2],0,false);
    }
    for (unsigned i=2;!e && i;i--) if (present[i-1]) {
        struct fat_entry *entry=&entries[i-1];
        entries[2]=*entry; /* oldest is gone; reuse its scratch storage */
        /* Release the entire unpublished tail, even when size is zero.
         * Finishing in place does not allocate a second full-file chain. */
        if (entry->size>capacity) entry->size=capacity;
        e=VFS_IO(v,fat_publish_reserved(v->volumes[drive],entry,true));
        if (px_legacy_changed) px_legacy_changed(v,v->volumes[drive],&entries[2],entry,false);
        entries[2]=*entry;
        if (!e) e=resolve(t,paths[i],&r);
        if (!e) e=VFS_IO(v,fat_rename(v->volumes[drive],entry,parent,r.name));
        if (!e && px_legacy_changed) px_legacy_changed(v,v->volumes[drive],&entries[2],entry,false);
    }
    vfs_unlock(v); return e;
}
int vfs_delete(struct vfs_table *t, const char *p) { return remove_path(t,p,false); }
int vfs_rmdir(struct vfs_table *t, const char *p) { return remove_path(t,p,true); }
int vfs_mkdir(struct vfs_table *t, const char *p, uint8_t attr) {
    if (ro_target(t,p)) return -FS_EROFS;
    path_lock(t,p,0,0); struct resolved r; int e=resolve(t,p,&r); struct fat_entry ent;
    if (!e && !r.name[0]) e=-FS_EEXIST;
    if (!e) e=VFS_IO(t->vfs,fat_create(t->vfs->volumes[r.drive],r.parent,r.name,attr|FAT_ATTR_DIR,&ent));
    vfs_unlock(t->vfs); return e;
}
int vfs_rename(struct vfs_table *t, const char *from, const char *to) {
    if (ro_target(t,from) || ro_target(t,to)) return -FS_EROFS;
    path_lock(t,from,to,0); struct resolved a,b; struct fat_entry ent; int e=lookup(t,from,&a,&ent);
    if (!e) e=resolve(t,to,&b);
    if (!e && (!a.name[0] || !b.name[0])) e=-FS_EACCES;
    if (!e && a.drive!=b.drive) e=-FS_EXDEV;
    if (!e) {
        struct fat_volume *v=t->vfs->volumes[a.drive]; e=busy(t->vfs,v,&ent);
        if (!e && (ent.attr&FAT_ATTR_DIR)) { e=contains(t->vfs,v,ent.first,b.parent); if (e>0) e=-FS_EINVAL; }
        if (!e) {
            struct fat_entry before=ent;
            e=VFS_IO(t->vfs,fat_rename(v,&ent,b.parent,b.name));
            if (!e && px_legacy_changed) px_legacy_changed(t->vfs,v,&before,&ent,false);
        }
    }
    vfs_unlock(t->vfs); return e;
}
int vfs_chdir(struct vfs_table *t, const char *p) {
    if (ro_target(t,p)) {
        path_lock(t,p,0,0); char canonical[FS_PATH_BYTES]; struct fs_ro_node n; unsigned d;
        int e=ro_path(t,p,&d,&n,canonical); if (!e && !(n.attr&FS_RO_DIRECTORY)) e=-FS_ENOTDIR;
        if (!e) memcpy(t->cwd[d],canonical,strlen(canonical)+1);
        vfs_unlock(t->vfs); return e;
    }

    path_lock(t,p,0,0); struct resolved r; struct fat_entry ent; int e=lookup(t,p,&r,&ent);
    if (!e && !(ent.attr&FAT_ATTR_DIR)) e=-FS_ENOTDIR;
    if (!e) {
        char full[FS_PATH_BYTES], canonical[FS_PATH_BYTES]; unsigned d; e=normalize(t,p,&d,full);
        if (!e) {
            unsigned used=1; canonical[0]='/'; canonical[1]=0; uint32_t dir=root(t->vfs->volumes[d]); const char *s=full+1;
            while (*s && !e) {
                unsigned n=0; while (*s && *s!='/') r.name[n++]=*s++; r.name[n]=0;
                e=VFS_IO(t->vfs,fat_lookup(t->vfs->volumes[d],dir,r.name,&ent)); if (e) break;
                n=(unsigned)strlen(ent.name); if (used>1) canonical[used++]='/';
                if (used+n>=sizeof(canonical)) { e=-FS_ENAMETOOLONG; break; }
                memcpy(canonical+used,ent.name,n+1); used+=n; dir=ent.first; if (*s) s++;
            }
            if (!e) { memcpy(t->cwd[d],canonical,used+1); t->cwd_cluster[d]=dir; }
        }
    }
    vfs_unlock(t->vfs); return e;
}
int vfs_set_drive(struct vfs_table *t, unsigned d) {
    if (d>=26) return -FS_EINVAL; vfs_lock(t->vfs,vfs_drive_mask(d));
    int e=t->vfs->ro_volumes[d] ? vfs_ro_check_locked(t->vfs,d) : t->vfs->volumes[d] ? 0 : -FS_ENOENT; if (!e) t->drive=d; vfs_unlock(t->vfs); return e;
}
int vfs_getcwd(struct vfs_table *t, unsigned d, char out[FS_PATH_BYTES]) {
    if (d>=26 || !out) return -FS_EINVAL; vfs_lock(t->vfs,vfs_drive_mask(d));
    int e=t->vfs->ro_volumes[d] ? vfs_ro_check_locked(t->vfs,d) : t->vfs->volumes[d] ? 0 : -FS_ENOENT; if (!e) memcpy(out,t->cwd[d],strlen(t->cwd[d])+1); vfs_unlock(t->vfs); return e;
}
int vfs_free_space(struct vfs_table *t, unsigned d, uint64_t *bytes, uint32_t *cb) {
    if (d<26 && t->vfs->ro_volumes[d]) {
        if (!bytes || !cb) return -FS_EINVAL; vfs_lock(t->vfs,vfs_drive_mask(d));
        int e=vfs_ro_check_locked(t->vfs,d); if (!e) { *bytes=0; *cb=t->vfs->ro_volumes[d]->dev->sector_size; }
        vfs_unlock(t->vfs); return e;
    }

    if (d>=26 || !bytes || !cb) return -FS_EINVAL; vfs_lock(t->vfs,vfs_drive_mask(d));
    struct fat_volume *v=t->vfs->volumes[d]; int e=v ? 0 : -FS_ENOENT;
    if (v) { *cb=(uint32_t)v->spc*512; *bytes=(uint64_t)v->free_clusters * *cb; }
    vfs_unlock(t->vfs); return e;
}
