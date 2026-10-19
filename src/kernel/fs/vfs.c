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
    fs_lock_init(&v->identity_lock);
    v->holder='-';
    fs_event_init(&v->volume_changed);
    for (unsigned d=0;d<26;d++) { fs_lock_init(&v->volume_lock[d]); fs_lock_classify(&v->volume_lock[d],2,'A'+d); }
}
void vfs_destroy(struct vfs *v) {
    if (files_detach) files_detach(v);
    for (unsigned d=0;d<26;d++) fs_lock_destroy(&v->volume_lock[d]);
    fs_event_destroy(&v->volume_changed); fs_lock_destroy(&v->identity_lock); fs_lock_destroy(&v->lock);
}
static void volume_mutexes(struct vfs *v,uint32_t mask,bool take) {
    for (unsigned i=0;i<26;i++) {
        unsigned d=take ? i : 25-i;
        if (mask & (1u<<d)) { if (take) fs_lock_take(&v->volume_lock[d]); else fs_lock_drop(&v->volume_lock[d]); }
    }
}
struct vfs_waiter { struct vfs_waiter *next; uint32_t mask, ticket[26]; };
static void request_wait(const struct vfs_request *r,uint64_t begin,uint32_t mask,enum vfs_wait_reason reason,int outcome) {
    if (!r || !r->wait) return;
    uint64_t elapsed=r->now(r->arg)-begin;
    if ((elapsed || outcome) && (!r->wait->valid || !r->wait->outcome)) *r->wait=(struct vfs_wait_info){r->owner,elapsed,mask,reason,outcome,true};
}
/* Every queued ticket, including native untimed callers, is represented while
 * sleeping. Cancellation advances over holes without stealing a live ticket. */
static void advance_cancelled(struct vfs *v) {
    for (unsigned d=0;d<26;d++) {
        if (v->reserved & (1u<<d)) continue;
        uint32_t next=v->next[d];
        for (struct vfs_waiter *w=v->waiters;w;w=w->next)
            if ((w->mask&(1u<<d)) && (uint32_t)(w->ticket[d]-v->serving[d]) < (uint32_t)(next-v->serving[d])) next=w->ticket[d];
        v->serving[d]=next;
    }
}
static int reserve_request(struct vfs *v,uint32_t mask,const struct vfs_request *request) {
    if (!request) request=fs_request_current();
    mask &= VFS_ALL_DRIVES; mask &= ~v->active;
    if (mask && request && request->wait && !request->wait->outcome) request->wait->drives=mask;
    if (!mask) return vfs_request_check(request);
    struct vfs_waiter waiter={.next=v->waiters,.mask=mask};
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) {
        if (v->active && (v->next[d]!=v->serving[d] || v->readers[d] || (v->mutating&(1u<<d)))) {
            if (request) return -FS_EBUSY;
            fs_panic("late volume reservation");
        }
    }
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) waiter.ticket[d]=v->next[d]++;
    v->waiters=&waiter;
    uint32_t start=fs_now_ms(); char holder=first_drive((v->reserved|v->mutating) & mask);
    uint64_t request_start=request ? request->now(request->arg) : 0;
    int error=0;
    for (;;) {
        error=vfs_request_check(request);
        bool ready=true;
        for (unsigned d=0;d<26;d++) if ((mask&(1u<<d)) &&
            (waiter.ticket[d]!=v->serving[d] || v->readers[d] || (v->mutating&(1u<<d)))) ready=false;
        if (error || ready) break;
        if (request) {
            fs_lock_drop(&v->lock); request->pause(request->arg); (void)fs_lock_retake(&v->lock,VFS_WAIT_NAMESPACE);
        } else fs_event_wait(&v->volume_changed,&v->lock);
    }
    struct vfs_waiter **link=&v->waiters;
    while (*link!=&waiter) link=&(*link)->next;
    *link=waiter.next;
    request_wait(request,request_start,mask,VFS_WAIT_VOLUME,error);
    wait_report(start,first_drive(mask),holder,error==-VFS_ETIMEOUT ? "volume:timeout" : error ? "volume:cancelled" : "volume");
    if (error) { advance_cancelled(v); fs_event_wake(&v->volume_changed); return error; }
    v->reserved |= mask;
    v->active_tickets |= mask;
    /* A free additional letter can precede an existing one. Reacquire the
     * complete set in order, retaining all reservations under namespace. */
    volume_mutexes(v,v->active,false); v->active |= mask;
    volume_mutexes(v,v->active,true);
    __atomic_store_n(&v->holder,first_drive(v->active),__ATOMIC_RELAXED);
    return 0;
}
void vfs_reserve_locked(struct vfs *v,uint32_t mask) {
    /* Legacy void helpers cannot return partial admission. They are also used
     * to retire published references; only int-returning admission may cancel. */
    struct fs_request_scope scope; fs_request_enter(&scope,0);
    (void)reserve_request(v,mask,0); fs_request_leave(&scope);
}
static int admit_request(struct vfs *v,uint32_t mask,const struct vfs_request *request) {
    if (!request) request=fs_request_current();
    if (!request) { vfs_lock(v,mask); return 0; }
    if (request->wait && !request->wait->valid) request->wait->drives=mask;
    int error; uint32_t start=fs_now_ms(); uint64_t begin=request->now(request->arg);
    while (!fs_lock_try_take(&v->lock)) {
        if ((error=vfs_request_check(request))) {
            request_wait(request,begin,mask,VFS_WAIT_NAMESPACE,error);
            wait_report(start,first_drive(mask),v->holder,error==-VFS_ETIMEOUT ? "namespace:timeout" : "namespace:cancelled");
            return error;
        }
        request->pause(request->arg);
    }
    request_wait(request,begin,mask,VFS_WAIT_NAMESPACE,0);
    error=reserve_request(v,mask,request);
    if (error) { fs_lock_drop(&v->lock); return error; }
    v->default_drive=26; return 0;
}

int vfs_admit(struct vfs *v,uint32_t mask,const struct vfs_request *request) {
    struct fs_request_scope scope;
    fs_request_enter(&scope,request ? request : fs_request_current());
    int error=admit_request(v,mask,request);
    fs_request_leave(&scope); return error;
}

void vfs_lock(struct vfs *v,uint32_t mask) {
    namespace_take(v,mask); vfs_reserve_locked(v,mask); v->default_drive=26;
}
void vfs_unlock(struct vfs *v) {
    uint32_t mask=v->active,tickets=v->active_tickets;
    v->active=0; v->active_tickets=0; v->reserved &= ~mask; v->mutating &= ~mask;
    for (unsigned d=0;d<26;d++) if (tickets&(1u<<d)) v->serving[d]++;
    advance_cancelled(v);
    __atomic_store_n(&v->holder,'-',__ATOMIC_RELAXED);
    volume_mutexes(v,mask,false); fs_lock_drop(&v->lock);
    if (mask) fs_event_wake(&v->volume_changed);
}
struct vfs_io_state vfs_io_begin_locked(struct vfs *v) {
    struct vfs_io_state state={v->active,v->default_drive,true};
    for (unsigned d=0;d<26;d++) if ((v->active&(1u<<d)) &&
        (!v->volumes[d] || !v->volumes[d]->dev || !v->volumes[d]->dev->logical)) state.logical=false;
    /* A logical provider can enter the outer physical VFS. Retain its
     * mutation guard, but no inner mutex across the outer namespace entry. */
    if (state.logical) volume_mutexes(v,v->active,false);
    /* Transfer lifetime/ordering ownership to the mutation guard. It is
     * distinct from the FIFO metadata ticket and never borrowed by a data
     * reader. Repeated FAT phases retain this guard, preserving every C2
     * boundary while no ticket spans a transport wait. */
    uint32_t tickets=v->active_tickets;
    v->mutating|=v->active; v->reserved&=~v->active;
    v->active=0; v->active_tickets=0;
    for (unsigned d=0;d<26;d++) if (tickets&(1u<<d)) v->serving[d]++;
    advance_cancelled(v);
    if (tickets) fs_event_wake(&v->volume_changed);
    __atomic_store_n(&v->holder,'-',__ATOMIC_RELAXED);
    fs_lock_drop(&v->lock); return state;
}
void vfs_io_end_locked(struct vfs *v,struct vfs_io_state state) {
    uint32_t mask=state.drives;
    if (!state.logical) volume_mutexes(v,mask,false);
    (void)fs_lock_retake(&v->lock,VFS_WAIT_NAMESPACE);
    volume_mutexes(v,mask,true); v->active=mask;
    v->default_drive=state.default_drive;
    __atomic_store_n(&v->holder,first_drive(mask),__ATOMIC_RELAXED);
}
static uint32_t vfs_path_mask(struct vfs_table *t,const char *path) {
    unsigned d=t->drive;
    if (path && path[0] && path[1]==':') d=(unsigned)(path_upper((uint8_t)path[0])-'A');
    return vfs_drive_mask(d);
}
static int path_lock(struct vfs_table *t,const char *a,const char *b,const char *c) {
    for (;;) {
        int error=vfs_admit(t->vfs,0,t->request); if (error) return error;
        uint32_t mask=vfs_path_mask(t,a) | (b ? vfs_path_mask(t,b) : 0) | (c ? vfs_path_mask(t,c) : 0);
        error=reserve_request(t->vfs,mask,t->request);
        if (error) { vfs_unlock(t->vfs); return error; }
        uint32_t now=vfs_path_mask(t,a) | (b ? vfs_path_mask(t,b) : 0) | (c ? vfs_path_mask(t,c) : 0);
        if (!(now & ~t->vfs->active)) { t->vfs->default_drive=t->drive; return 0; }
        vfs_unlock(t->vfs); /* default drive changed during admission */
    }
}
static uint64_t identity_generation(struct vfs *v, struct fat_volume *vol) {
    (void)v;
    return (uint64_t)vol->identity_mount_generation<<32 | (vol->dev ? vol->dev->media_generation : 0);
}
static void invalidate_pages(struct vfs *v, struct vfs_file_identity *id) {
    for (unsigned i=0;i<VFS_PAGE_IDENTITIES;i++)
        if (v->page_identities[i].file==id) v->page_identities[i].valid=false;
}
/* Find by a retained serial first (open-unlinked and rename), otherwise by
 * the validated owning short-entry location. LFN text is never an identity. */
static struct vfs_file_identity *identity_find(struct vfs *v, struct fat_volume *vol,
                                              const struct fat_entry *entry) {
    uint64_t generation=identity_generation(v,vol);
    if (entry->identity) for (unsigned i=0;i<VFS_IDENTITIES;i++) {
        struct vfs_file_identity *id=&v->identities[i];
        if (id->valid && id->volume==vol && id->generation==generation && id->serial==entry->identity) return id;
    }
    for (unsigned i=0;i<VFS_IDENTITIES;i++) {
        struct vfs_file_identity *id=&v->identities[i];
        if (id->valid && id->volume==vol && id->generation==generation && id->linked &&
            entry->index!=UINT32_MAX && id->parent==entry->parent && id->index==entry->index) return id;
    }
    return 0;
}
int vfs_identity_get(struct vfs *v, struct fat_volume *vol, struct fat_entry *entry,
                      struct vfs_file_identity **out) {
    if (!v || !vol || !vol->dev || !entry || !out) return -FS_EINVAL;
    fs_lock_take(&v->identity_lock);
    int e=0; uint64_t generation=identity_generation(v,vol);
    struct vfs_file_identity *id=identity_find(v,vol,entry);
    if (!vol->mounted || vol->dev->quarantined || !vol->identity_mount_generation) e=-FS_EIO;
    if (!e && !id) {
        /* A retained serial cannot silently acquire a replacement identity. */
        if (entry->index==UINT32_MAX) e=-FS_EIO;
        else for (unsigned i=0;i<VFS_IDENTITIES;i++) {
            struct vfs_file_identity *free=&v->identities[i];
            if (!free->refs && !free->pages) { id=free; break; }
        }
        if (!e && (!id || v->next_identity==UINT64_MAX)) e=-FS_ENOSPC;
        if (!e) *id=(struct vfs_file_identity){.volume=vol,.serial=++v->next_identity,
            .generation=generation,.parent=entry->parent,.index=entry->index,.valid=true,
            .linked=entry->index!=UINT32_MAX};
    }
    if (!e && id->refs==UINT32_MAX) e=-FS_ENOSPC;
    if (!e) { id->refs++; entry->identity=id->serial; *out=id; }
    fs_lock_drop(&v->identity_lock); return e;
}
bool vfs_identity_live(struct vfs *v, struct vfs_file_identity *id) {
    if (!id) return true; /* read-only providers have their existing generation checks */
    fs_lock_take(&v->identity_lock);
    bool live=id->valid && id->generation==identity_generation(v,id->volume) && !id->volume->dev->quarantined;
    fs_lock_drop(&v->identity_lock); return live;
}
void vfs_identity_put(struct vfs *v, struct vfs_file_identity *id) {
    if (!id) return;
    fs_lock_take(&v->identity_lock); if (id->refs) id->refs--; fs_lock_drop(&v->identity_lock);
}
int vfs_page_get(struct vfs *v, struct vfs_file_identity *id, uint64_t index,
                 struct vfs_page_identity **out) {
    if (!v || !id || !out) return -FS_EINVAL;
    fs_lock_take(&v->identity_lock); int e=0;
    struct vfs_page_identity *page=0,*free=0;
    if (!id->valid || (!id->refs && !id->pages) || id->generation!=identity_generation(v,id->volume) ||
        id->volume->dev->quarantined) e=-FS_EIO;
    for (unsigned i=0;!e && i<VFS_PAGE_IDENTITIES;i++) {
        struct vfs_page_identity *p=&v->page_identities[i];
        if (!p->refs) { if (!free) free=p; }
        else if (p->valid && p->file==id && p->key.index==index) { page=p; break; }
    }
    if (!e && !page) {
        if (!free || id->pages==UINT32_MAX) e=-FS_ENOSPC;
        else { page=free; *page=(struct vfs_page_identity){.file=id,
            .key={id->generation,id->serial,index},.valid=true}; id->pages++; }
    }
    if (!e && page->refs==UINT32_MAX) e=-FS_ENOSPC;
    if (!e) { page->refs++; *out=page; }
    fs_lock_drop(&v->identity_lock); return e;
}
void vfs_page_put(struct vfs *v, struct vfs_page_identity *page) {
    if (!page) return;
    fs_lock_take(&v->identity_lock);
    if (page->refs && !--page->refs) { page->file->pages--; memset(page,0,sizeof(*page)); }
    fs_lock_drop(&v->identity_lock);
}
void vfs_identity_expire(struct vfs *v, struct fat_volume *vol) {
    for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) {
        struct vfs_description *d=&v->descriptions[i];
        if (d->refs && d->node->volume==vol) cache_stream_cancel(&d->stream);
    }
    fs_lock_take(&v->identity_lock);
    for (unsigned i=0;i<VFS_IDENTITIES;i++) if (v->identities[i].volume==vol) {
        v->identities[i].valid=false; invalidate_pages(v,&v->identities[i]);
    }
    fs_lock_drop(&v->identity_lock);
}
void vfs_raw_changed(struct vfs *v, struct fat_volume *vol) { vfs_identity_expire(v,vol); }
bool vfs_identity_busy(struct vfs *v, struct fat_volume *vol) {
    fs_lock_take(&v->identity_lock); bool busy=false;
    uint64_t generation=identity_generation(v,vol);
    for (unsigned i=0;i<VFS_IDENTITIES;i++) {
        struct vfs_file_identity *id=&v->identities[i];
        if (id->volume==vol && id->generation==generation && (id->refs || id->pages)) busy=true;
    }
    fs_lock_drop(&v->identity_lock); return busy;
}
static void identity_change(void *ctx, struct fat_volume *vol, const struct fat_entry *old,
                             const struct fat_entry *entry, enum fat_identity_change why) {
    struct vfs *v=ctx;
    /* The caller reserves this volume through publication. Description
     * slots on it cannot close/reopen until this callback has returned. */
    for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) {
        struct vfs_description *d=&v->descriptions[i];
        if (d->refs && d->node->volume==vol && d->node->entry.parent==old->parent &&
            d->node->entry.index==old->index) cache_stream_cancel(&d->stream);
    }
    fs_lock_take(&v->identity_lock);
    struct vfs_file_identity *id=identity_find(v,vol,old);
    if (id) {
        if (why==FAT_ID_RENAME) { id->parent=entry->parent; id->index=entry->index; id->linked=true; }
        else if (why==FAT_ID_UNLINK) id->linked=false;
        else invalidate_pages(v,id); /* writes/truncate also invalidate partial EOF pages */
    }
    fs_lock_drop(&v->identity_lock);
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
    if (!e && v->next_identity_generation==UINT32_MAX) e=-FS_ENOSPC;
    if (!e) {
        fs_lock_take(&v->identity_lock);
        vol->identity_mount_generation=++v->next_identity_generation;
        v->volumes[d]=vol; v->generation[d]++;
        vol->identity_changed=identity_change; vol->identity_ctx=v;
        fs_lock_drop(&v->identity_lock);
    }
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
static int ro_target(struct vfs_table *t, const char *path)
{
    unsigned d = t->drive;
    if (path && path[0] && path[1] == ':') d = (unsigned)(path_upper((uint8_t)path[0]) - 'A');
    int admission=vfs_admit(t->vfs,0,t->request); if (admission) return admission;
    int result=d<26 && t->vfs->ro_volumes[d];
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
static int ro_open(struct vfs_table *t, const char *path, unsigned flags, unsigned deny, unsigned mode, unsigned *status)
{
    struct vfs *v = t->vfs; int admission=path_lock(t,path,0,0); if (admission) return admission;
    struct fs_ro_node ent; unsigned drive; int e;
    if (flags & (VFS_WRITE | VFS_CREATE | VFS_TRUNCATE)) { e = -FS_EROFS; goto out; }
    e = ro_path(t, path, &drive, &ent, 0); if (e) goto out;
    if (flags&VFS_FAIL_EXISTING) { e=-FS_EEXIST; goto out; }
    if (ent.attr & FS_RO_DIRECTORY) { e = -FS_EISDIR; goto out; }
    if (px_ro_share && (e=px_ro_share(v,drive,ent.id,VFS_READ,deny,mode,t->owner))) goto out;
    if (ent.size > UINT32_MAX) { e = -FS_EFBIG; goto out; }
    struct vfs_node *node = 0, *free_node = 0;
    for (unsigned i = 0; i < VFS_NODES; i++) {
        struct vfs_node *n = &v->nodes[i];
        if (!n->refs) free_node = n;
        else if (n->ro == v->ro_volumes[drive] && n->ro_id == ent.id && n->generation == v->generation[drive]) node = n;
    }
    if (node) for (unsigned i = 0; i < VFS_DESCRIPTIONS; i++) {
        struct vfs_description *d = &v->descriptions[i];
        if (d->refs && d->node == node) {
            unsigned old_mode=d->dos_mode==255 ? d->deny==3 ? 1 : d->deny==2 ? 2 : d->deny==1 ? 3 : 4 : d->dos_mode;
            unsigned new_mode=mode==255 ? deny==3 ? 1 : deny==2 ? 2 : deny==1 ? 3 : 4 : mode;
            e=vfs_dos_share(old_mode,d->access,d->owner,new_mode,VFS_READ,d->owner_type==t->owner_type ? t->owner : ~d->owner,true);
            if (e) goto out;
        }
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
    node->refs++; *desc = (struct vfs_description){ .node = node, .refs = 1, .access = VFS_READ, .deny = mode==0 ? VFS_DENY_WRITE : deny, .owner=t->owner, .owner_type=t->owner_type, .dos_mode=mode };
    t->handles[h] = desc; if (status) *status=1; e = h;
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
    if (!e && vfs_identity_busy(v,vol)) e=-FS_EBUSY;
    if (!e) e=VFS_IO(v,fat_unmount(vol));
    if (!e) {
        if (px_volume_detached) px_volume_detached(v,d);
        vfs_identity_expire(v,vol); vol->identity_changed=0; vol->identity_ctx=0;
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
    t->owner=(uintptr_t)t; /* distinct default native contexts, including inheritance */
    for (unsigned i=0;i<26;i++) { t->cwd[i][0]='/'; t->cwd[i][1]=0; }
    v->tables[slot]=t; vfs_unlock(v); return 0;
}
static int release_description(struct vfs *v,struct vfs_description *d) {
    if (!d) return 0;
    int e=0;
    if (d->refs==1) {
        if (d->stream.cache) {
            /* Retain the closing slots/volume guard through retirement, but
             * never hold namespace across an issued speculative command.
             * Identified unrelated readers may pass this close phase. */
            unsigned drive=d->node->drive;
            struct vfs_node *previous=v->io_file[drive];
            bool announce=(v->active&vfs_drive_mask(drive)) && !previous;
            if (announce) v->io_file[drive]=d->node;
            struct vfs_io_state io=vfs_io_begin_locked(v);
            cache_stream_close(&d->stream);
            vfs_io_end_locked(v,io);
            if (announce) v->io_file[drive]=previous;
        }
        for (unsigned i=0;i<VFS_RANGE_LOCKS;i++) if (v->range_locks[i].description==d)
            memset(&v->range_locks[i],0,sizeof(v->range_locks[i]));
        /* Retain the slots until final-close callbacks finish their I/O. */
        if (d->node->refs==1) {
            if (px_legacy_closed) e=px_legacy_closed(v,d->node);
            vfs_identity_put(v,d->node->identity);
            memset(d->node,0,sizeof(*d->node));
        } else d->node->refs--;
        memset(d,0,sizeof(*d));
    } else d->refs--;
    return e;
}
static int close_handle(struct vfs_table *t, unsigned h) {
    struct vfs_description *d=t->handles[h]; t->handles[h]=0;
    if (d) for (unsigned i=0;i<VFS_RANGE_LOCKS;i++)
        if (t->vfs->range_locks[i].description==d && (t->vfs->range_locks[i].owner==t->owner && t->vfs->range_locks[i].owner_type==t->owner_type))
            memset(&t->vfs->range_locks[i],0,sizeof(t->vfs->range_locks[i]));
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
    for (unsigned i=0;i<VFS_NODES;i++) if (v->nodes[i].refs && v->nodes[i].volume==vol && v->nodes[i].generation==v->generation[v->nodes[i].drive] && v->nodes[i].entry.parent==ent->parent && v->nodes[i].entry.index==ent->index) return &v->nodes[i];
    return 0;
}
static int table_range_check(struct vfs_table *,struct vfs_node *,struct vfs_description *,uint64_t,uint64_t,bool);
static int handle_free(struct vfs_table *t) { for (unsigned i=0;i<VFS_HANDLES;i++) if (!t->handles[i]) return (int)i; return -FS_EMFILE; }
int vfs_open_claim(struct vfs_table *t, const char *path, unsigned flags, enum vfs_share deny, uint8_t attr, unsigned mode, unsigned *status) {
    if (!(flags&3) || flags&~63u || deny>VFS_DENY_ALL || (flags&VFS_TRUNCATE && !(flags&VFS_WRITE)) || (flags&VFS_EXCLUSIVE && !(flags&VFS_CREATE))) return -FS_EINVAL;
    if (mode>4 && mode!=255) return -FS_EINVAL;
    int ro=ro_target(t,path); if (ro<0) return ro;
    if (ro) return ro_open(t,path,flags,(unsigned)deny,mode,status);
    struct vfs *v=t->vfs; int admission=path_lock(t,path,0,0); if (admission) return admission;
    struct resolved r; struct fat_entry ent; int e=lookup(t,path,&r,&ent);
    bool create=e==-FS_ENOENT && (flags&VFS_CREATE);
    /* A missing ancestor is distinguished by resolving again before create. */
    if (create) e=resolve(t,path,&r);
    if (e) goto out;
    if (flags&VFS_FAIL_EXISTING) { e=-FS_EEXIST; goto out; }
    if (!create && (flags&(VFS_CREATE|VFS_EXCLUSIVE))==(VFS_CREATE|VFS_EXCLUSIVE)) { e=-FS_EEXIST; goto out; }
    struct fat_volume *vol=v->volumes[r.drive];
    if (!create && (ent.attr&FAT_ATTR_DIR)) { e=-FS_EISDIR; goto out; }
    if ((flags&VFS_WRITE) && (vol->readonly || (!create && (ent.attr&FAT_ATTR_RO)))) { e=vol->readonly ? -FS_EROFS : -FS_EACCES; goto out; }
    if (mode==0) deny=!create && (ent.attr&FAT_ATTR_RO) && (flags&3)==VFS_READ ? VFS_DENY_WRITE : VFS_DENY_ALL;
    if (!create && px_legacy_share && (e=px_legacy_share(v,vol,&ent,flags&3,(unsigned)deny,mode,t->owner))) goto out;
    int h=handle_free(t); if (h<0) { e=h; goto out; }
    struct vfs_description *d=0; for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) if (!v->descriptions[i].refs) { d=&v->descriptions[i]; break; }
    if (!d) { e=-FS_EMFILE; goto out; }
    struct vfs_node *node=create ? 0 : node_find(v,vol,&ent);
    if (node) {
        if (!vfs_identity_live(v,node->identity)) { e=-FS_EIO; goto out; }
        for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) {
            struct vfs_description *other=&v->descriptions[i];
            if (other->refs && other->node==node) {
                unsigned old_mode=other->dos_mode==255 ? other->deny==3 ? 1 : other->deny==2 ? 2 : other->deny==1 ? 3 : 4 : other->dos_mode;
                unsigned new_mode=mode==255 ? deny==3 ? 1 : deny==2 ? 2 : deny==1 ? 3 : 4 : mode;
                e=vfs_dos_share(old_mode,other->access,other->owner,new_mode,flags&3,other->owner_type==t->owner_type ? t->owner : ~other->owner,(ent.attr&FAT_ATTR_RO)!=0);
                if (mode==255 && other->dos_mode==255)
                    e=((other->deny&(flags&3)) || ((unsigned)deny&other->access)) ? -FS_EACCES : 0;
                if (e) goto out;
            }
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
    if (!node->identity && (e=vfs_identity_get(v,vol,&node->entry,&node->identity))) goto undo;
    if (flags&VFS_TRUNCATE) {
        struct fat_entry before=node->entry;
        e=table_range_check(t,node,d,0,before.size,true); if (e) goto undo;
        e=VFS_IO(v,fat_truncate(vol,&node->entry,0));
        if (px_legacy_changed) px_legacy_changed(v,vol,&before,&node->entry,false);
        if (e) goto undo;
    }
    d->node=node; d->position=0; d->access=flags&3; d->deny=(unsigned)deny; t->handles[h]=d; d->dos_mode=mode; d->owner=t->owner; d->owner_type=t->owner_type;
    if (status) *status=create ? 2 : flags&VFS_TRUNCATE ? 3 : 1; e=h;
    goto out;
undo: t->handles[h]=0; if (!--node->refs) { vfs_identity_put(v,node->identity); memset(node,0,sizeof(*node)); } memset(d,0,sizeof(*d));
out: vfs_unlock(v); return e;
}
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share deny,uint8_t attr) {
    if (flags&VFS_FAIL_EXISTING) return -FS_EINVAL;
    int e=vfs_open_claim(t,path,flags,deny,attr,255,0);
    return e==-VFS_ESHARE || e==-VFS_ECRITICAL_SHARE ? -FS_EACCES : e;
}

static uint32_t handle_mask(struct vfs_table *t,int h) {
    struct vfs_description *d=h>=0 && h<(int)VFS_HANDLES ? t->handles[h] : 0;
    return d ? vfs_drive_mask(d->node->drive) : 0;
}
/* Per-file readers and seeks can wait without taking a metadata ticket.
 * Keep their admission cancellable, with the same evidence as ticket waits.
 * Cleanup after issued I/O still reacquires namespace unconditionally. */
static int file_wait_locked(struct vfs *v,uint32_t mask,const struct vfs_request *request,
                            uint32_t start,uint64_t begin) {
    if (!request) request=fs_request_current();
    if (!request) { fs_event_wait(&v->volume_changed,&v->lock); return 0; }
    char holder=first_drive((v->reserved|v->mutating)&mask);
    fs_lock_drop(&v->lock); request->pause(request->arg); (void)fs_lock_retake(&v->lock,VFS_WAIT_NAMESPACE);
    int error=vfs_request_check(request);
    request_wait(request,begin,mask,VFS_WAIT_VOLUME,error);
    if (error) wait_report(start,first_drive(mask),holder,error==-VFS_ETIMEOUT ? "volume:timeout" : "volume:cancelled");
    return error;
}
static int handle_lock(struct vfs_table *t,int h) {
    uint32_t start=fs_now_ms(); uint64_t begin=t->request ? t->request->now(t->request->arg) : 0;
    for (;;) {
        int error=vfs_admit(t->vfs,0,t->request); if (error) return error;
        struct vfs_description *desc=h>=0 && h<(int)VFS_HANDLES ? t->handles[h] : 0;
        if (desc && desc->io_active) {
            error=file_wait_locked(t->vfs,handle_mask(t,h),t->request,start,begin);
            vfs_unlock(t->vfs); if (error) return error; continue;
        }
        uint32_t mask=handle_mask(t,h); error=reserve_request(t->vfs,mask,t->request);
        if (error) { vfs_unlock(t->vfs); return error; }
        if (handle_mask(t,h)==mask) return 0;
        vfs_unlock(t->vfs); /* slot was replaced while admission slept */
    }
}
static struct vfs_description *get(struct vfs_table *t, int h) { return h>=0 && h<(int)VFS_HANDLES ? t->handles[h] : 0; }
int vfs_handle_stat(struct vfs_table *t,int h,struct fat_entry *entry,uint64_t *position) {
    int e=handle_lock(t,h); if (e) return e;
    struct vfs_description *d=get(t,h);
    e=!d ? -FS_EBADF : (d->node->generation!=t->vfs->generation[d->node->drive] || !vfs_identity_live(t->vfs,d->node->identity)) ? -FS_EIO : 0;
    if (!e) { if (entry) *entry=d->node->entry; if (position) *position=d->position; }
    vfs_unlock(t->vfs); return e;
}
int vfs_handle_times(struct vfs_table *t,int h,uint16_t date,uint16_t time) {
    int e=handle_lock(t,h); if (e) return e;
    struct vfs_description *d=get(t,h);
    e=!d ? -FS_EBADF : (d->node->generation!=t->vfs->generation[d->node->drive] ||
        !vfs_identity_live(t->vfs,d->node->identity)) ? -FS_EIO : d->node->ro ? -FS_EROFS : 0;
    if (!e) {
        struct fat_entry before=d->node->entry;
        struct fat_times times=before.times; times.write_date=date; times.write_time=time;
        e=VFS_IO(t->vfs,fat_set_metadata(d->node->volume,&d->node->entry,before.attr,&times));
        if (!e && px_legacy_changed) px_legacy_changed(t->vfs,d->node->volume,&before,&d->node->entry,false);
    }
    vfs_unlock(t->vfs); return e;
}
int vfs_request_check(const struct vfs_request *r) {
    if (!r) r=fs_request_current();
    if (!r) return 0;
    if (r->cancelled && r->cancelled(r->arg)) return -VFS_ECANCELLED;
    return r->now(r->arg)>=r->deadline ? -VFS_ETIMEOUT : 0;
}
static unsigned dos_deny(unsigned mode) {
    static const unsigned deny[]={0,VFS_DENY_ALL,VFS_DENY_WRITE,VFS_DENY_READ,VFS_DENY_NONE};
    return mode<=4 ? deny[mode] : 0;
}
int vfs_dos_share(unsigned old_mode,unsigned old_access,uint64_t old_owner,
                  unsigned mode,unsigned access,uint64_t owner,bool readonly) {
    if (old_mode && mode) return ((dos_deny(old_mode)&access) || (dos_deny(mode)&old_access)) ? -VFS_ESHARE : 0;
    if (!old_mode && !mode)
        return old_owner==owner || (readonly && old_access==VFS_READ && access==VFS_READ) ? 0 : -VFS_ECRITICAL_SHARE;
    if (!old_mode) return old_access==VFS_READ && access==VFS_READ && readonly && (mode==2 || mode==4) ? 0 : -VFS_ESHARE;
    return old_access==VFS_READ && access==VFS_READ && readonly && (old_mode==2 || old_mode==4) ? 0 : -VFS_ECRITICAL_SHARE;
}
/* Equal retained identity survives aliases and rename. Synthetic native nodes
 * are snapshots of that identity; never equate a reused directory slot. */
static bool lock_same_file(const struct vfs_node *a, const struct vfs_node *b) {
    if (a==b) return true;
    if (a->identity && b->identity) return a->identity==b->identity;
    if (a->entry.identity && b->entry.identity && a->volume==b->volume && a->generation==b->generation)
        return a->entry.identity==b->entry.identity;
    return a->generation==b->generation && (a->ro ? a->ro==b->ro && a->ro_id==b->ro_id :
        a->volume==b->volume && a->entry.parent==b->entry.parent && a->entry.index==b->entry.index);
}
int vfs_description_io_locked(struct vfs *v,struct vfs_node *node,struct vfs_description *description,
                              struct vfs_lock_owner owner,uint64_t start,uint64_t length,bool write) {
    if (!length) return 0;
    if (length>UINT64_MAX-start) return -FS_EINVAL;
    for (unsigned i=0;i<VFS_RANGE_LOCKS;i++) {
        struct vfs_range_lock *l=&v->range_locks[i];
        if (!l->description || !lock_same_file(l->node,node) || start>=l->end || l->start>=start+length) continue;
        bool mine=l->description==description && l->owner==owner.id && l->owner_type==owner.type;
        if (l->shared ? write : !mine) return -VFS_ELOCK;
    }
    return 0;
}
int vfs_range_check_owner(struct vfs *v,struct vfs_node *node,struct vfs_description *description,
                          struct vfs_lock_owner owner,uint64_t start,uint64_t length) {
    return vfs_description_io_locked(v,node,description,owner,start,length,true);
}
int vfs_range_check(struct vfs *v,struct vfs_node *node,struct vfs_description *d,
                    uint64_t owner,uint64_t start,uint64_t length) {
    return vfs_range_check_owner(v,node,d,(struct vfs_lock_owner){d ? d->owner_type : VFS_OWNER_NATIVE,owner},start,length);
}
static int table_range_check(struct vfs_table *t, struct vfs_node *node, struct vfs_description *d,
                              uint64_t start,uint64_t length,bool write) {
    return vfs_description_io_locked(t->vfs,node,d,(struct vfs_lock_owner){t->owner_type,t->owner},start,length,write);
}
void vfs_description_unlock_locked(struct vfs *v,struct vfs_description *d) {
    for (unsigned i=0;i<VFS_RANGE_LOCKS;i++) if (v->range_locks[i].description==d)
        memset(&v->range_locks[i],0,sizeof(v->range_locks[i]));
}
int vfs_description_lock_locked(struct vfs *v,struct vfs_description *d,struct vfs_lock_owner owner,
                                uint64_t start,uint64_t length,bool exclusive,bool unlock) {
    if (!d || !d->node || owner.type>=VFS_OWNER_TYPES || length>UINT64_MAX-start) return -FS_EINVAL;
    if (d->node->generation!=v->generation[d->node->drive] || !vfs_identity_live(v,d->node->identity)) return -FS_EIO;
    struct vfs_range_lock *slot=0,*match=0;
    for (unsigned i=0;i<VFS_RANGE_LOCKS;i++) {
        struct vfs_range_lock *l=&v->range_locks[i];
        if (!l->description) { if (!slot) slot=l; continue; }
        if (!lock_same_file(l->node,d->node)) continue;
        bool mine=l->description==d && l->owner==owner.id && l->owner_type==owner.type;
        if (mine && l->start==start && l->length==length && (!match || !l->shared)) match=l;
        if (!unlock && length && l->length && start<l->end && l->start<start+length &&
            (exclusive || (!l->shared && !mine))) return -VFS_ELOCK;
    }
    if (unlock) { if (!match) return -VFS_ELOCK; memset(match,0,sizeof(*match)); return 0; }
    if (!slot) return -VFS_ELOCKFULL;
    *slot=(struct vfs_range_lock){.description=d,.node=d->node,.owner=owner.id,.owner_type=owner.type,
        .start=start,.end=start+length,.length=length,.shared=!exclusive};
    return 0;
}
int vfs_lock_range(struct vfs_table *t,int h,uint32_t start,uint32_t length,bool unlock) {
    int e=handle_lock(t,h); if (e) return e;
    struct vfs_description *d=get(t,h);
    e=!d ? -FS_EBADF : (uint64_t)start+length>UINT32_MAX ? -FS_EINVAL :
        vfs_description_lock_locked(t->vfs,d,(struct vfs_lock_owner){t->owner_type,t->owner},start,length,true,unlock);
    vfs_unlock(t->vfs); return e;
}
int vfs_table_set_owner(struct vfs_table *t, enum vfs_owner_type type, uint64_t owner) {
    if (!t || !t->vfs || (unsigned)type>=VFS_OWNER_TYPES) return -FS_EINVAL;
    struct vfs *v=t->vfs; vfs_lock(v,0); int e=0;
    for (unsigned i=0;i<VFS_HANDLES;i++) if (t->handles[i]) e=-FS_EBUSY;
    if (!e) { t->owner_type=type; t->owner=owner; t->owner_registered=true; }
    vfs_unlock(v); return e;
}
void vfs_release_lock_owner(struct vfs *v, struct vfs_lock_owner owner) {
    vfs_lock(v,0);
    for (unsigned i=0;i<VFS_RANGE_LOCKS;i++) {
        struct vfs_range_lock *l=&v->range_locks[i];
        if (l->description && l->owner==owner.id && l->owner_type==owner.type) memset(l,0,sizeof(*l));
    }
    vfs_unlock(v);
}
void vfs_release_owner(struct vfs *v,uint64_t owner) {
    vfs_release_lock_owner(v,(struct vfs_lock_owner){VFS_OWNER_NATIVE,owner});
}
int vfs_close(struct vfs_table *t, int h) {
    int admission=handle_lock(t,h); if (admission) return admission; int e=get(t,h) ? 0 : -FS_EBADF; if (!e) e=close_handle(t,(unsigned)h); vfs_unlock(t->vfs); return e;
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
    if (d && src==dst && target==h) { vfs_unlock(src->vfs); return h; }
    if (d) {
        if (target==-1) target=handle_free(dst);
        if (target<0) e=target;
        else {
            struct vfs_description *old=dst->handles[target];
            d->refs++; dst->handles[target]=d;
            if (old) for (unsigned i=0;i<VFS_RANGE_LOCKS;i++) {
                struct vfs_range_lock *l=&src->vfs->range_locks[i];
                if (l->description==old && l->owner==dst->owner && l->owner_type==dst->owner_type) memset(l,0,sizeof(*l));
            }
            release_description(src->vfs,old); e=target;
        }
    }
    vfs_unlock(src->vfs); return e;
}
int vfs_read(struct vfs_table *t, int h, void *buf, size_t n, size_t *done) {
    if (!done) return -FS_EINVAL; *done=0;
    struct vfs *v=t->vfs;
    uint32_t start=fs_now_ms(); uint64_t begin=t->request ? t->request->now(t->request->arg) : 0;
    /* Open identities pin the chain against namespace removal. A published
     * writer names its node before dropping namespace. Never borrow a mutable
     * entry or volume diagnostic workspace across a different file's I/O. */
    for (;;) {
        int admission=vfs_admit(v,0,t->request); if (admission) return admission;
        struct vfs_description *d=get(t,h);
        if (!d || d->node->ro || !(d->access&VFS_READ)) { vfs_unlock(v); break; }
        struct vfs_node *node=d->node; unsigned drive=node->drive;
        if (drive>=26 || node->generation!=v->generation[drive] || v->volumes[drive]!=node->volume || !vfs_identity_live(v,node->identity)) {
            vfs_unlock(v); return -FS_EIO;
        }
        uint32_t mask=vfs_drive_mask(drive);
        bool admitted=((v->reserved|v->mutating)&mask) ? v->io_file[drive] && v->io_file[drive]!=node :
            v->next[drive]==v->serving[drive];
        if (d->io_active || !admitted) {
            int error=file_wait_locked(v,mask,t->request,start,begin);
            vfs_unlock(v); if (error) return error; continue;
        }
        int locked=table_range_check(t,node,d,d->position,n,false);
        if (locked) { vfs_unlock(v); return locked; }
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
        int e=fat_read_stream(&view,&entry,position,buf,n,done,&d->stream);
        (void)fs_lock_retake(&v->lock,VFS_WAIT_NAMESPACE);
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
    int admission=handle_lock(t,h); if (admission) return admission; struct vfs_description *d=get(t,h); int e;
    if (!d) e=-FS_EBADF;
    else if (d->node->generation!=t->vfs->generation[d->node->drive] || !vfs_identity_live(t->vfs,d->node->identity)) e=-FS_EIO;
    else if (!(d->access&VFS_READ)) e=-FS_EACCES;
    else if ((e=table_range_check(t,d->node,d,d->position,n,false))) { }
    else if (d->node->ro) { e=ro_handle(t->vfs,d->node); if (!e) e=VFS_IO(t->vfs,fs_ro_read(d->node->ro,d->node->ro_id,d->position,buf,n,done)); if (e) ro_handle(t->vfs,d->node); d->position+=*done; }
    else { e=VFS_IO(t->vfs,fat_read(d->node->volume,&d->node->entry,d->position,buf,n,done)); d->position+=*done; }
    vfs_unlock(t->vfs); return e;
}
int vfs_write(struct vfs_table *t, int h, const void *buf, size_t n, size_t *done) {
    if (!done) return -FS_EINVAL; *done=0;
    int admission=handle_lock(t,h); if (admission) return admission; struct vfs_description *d=get(t,h); int e;
    if (!d) e=-FS_EBADF;
    else if (d->node->generation!=t->vfs->generation[d->node->drive] || !vfs_identity_live(t->vfs,d->node->identity)) e=-FS_EIO;
    else if (d->node->ro) { e=ro_handle(t->vfs,d->node); if (!e) e=-FS_EROFS; }
    else if (!(d->access&VFS_WRITE)) e=-FS_EACCES;
    else if ((e=table_range_check(t,d->node,d,d->position,n,true))) { }
    else if (d->node->log_capacity) e=-FS_EBUSY;
    else {
        cache_stream_cancel(&d->stream);
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
    uint32_t start=fs_now_ms(); uint64_t begin=t->request ? t->request->now(t->request->arg) : 0;
    for (;;) {
        int error=vfs_admit(t->vfs,0,t->request); if (error) return error;
        struct vfs_description *desc=get(t,h);
        if (desc && (desc->io_active || t->vfs->io_file[desc->node->drive]==desc->node)) {
            error=file_wait_locked(t->vfs,handle_mask(t,h),t->request,start,begin);
            vfs_unlock(t->vfs); if (error) return error; continue;
        }
        if (desc && desc->node->ro) { vfs_unlock(t->vfs); error=handle_lock(t,h); if (error) return error; }
        break;
    }
    struct vfs_description *d=get(t,h); int e=0;
    if (!d) e=-FS_EBADF;
    else if (d->node->generation!=t->vfs->generation[d->node->drive] ||
             !vfs_identity_live(t->vfs,d->node->identity)) e=-FS_EIO;
    else if (d->node->ro && (e=ro_handle(t->vfs,d->node))) { }
    else {
        uint64_t base=whence==VFS_SEEK_SET ? 0 : whence==VFS_SEEK_CUR ? d->position : d->node->entry.size;
        uint64_t magnitude=off<0 ? (uint64_t)(-(off+1))+1 : (uint64_t)off;
        if (off<0 && magnitude>base) e=-FS_EINVAL;
        else if (off>=0 && magnitude>UINT64_MAX-base) e=-FS_EINVAL;
        else { cache_stream_cancel(&d->stream); d->position=off<0 ? base-magnitude : base+magnitude; *position=d->position; }
    }
    vfs_unlock(t->vfs); return e;
}
int vfs_truncate(struct vfs_table *t, int h, uint64_t size) {
    int admission=handle_lock(t,h); if (admission) return admission; struct vfs_description *d=get(t,h);
    int e=!d ? -FS_EBADF : (d->node->generation!=t->vfs->generation[d->node->drive] || !vfs_identity_live(t->vfs,d->node->identity)) ? -FS_EIO : d->node->ro ? ro_handle(t->vfs,d->node) : !(d->access&VFS_WRITE) ? -FS_EACCES : 0;
    if (d && d->node->ro && !e) e=-FS_EROFS;
    if (d && d->node->log_capacity && !e) e=-FS_EBUSY;
    if (!e) {
        uint64_t old=d->node->entry.size,start=size<old ? size : old;
        e=table_range_check(t,d->node,d,start,size<old ? old-size : size-old,true);
    }
    if (!e) {
        struct fat_entry before=d->node->entry;
        e=VFS_IO(t->vfs,fat_truncate(d->node->volume,&d->node->entry,size));
        if (px_legacy_changed) px_legacy_changed(t->vfs,d->node->volume,&before,&d->node->entry,false);
    }
    vfs_unlock(t->vfs); return e;
}
int vfs_commit(struct vfs_table *t, int h) {
    int admission=handle_lock(t,h); if (admission) return admission; struct vfs_description *d=get(t,h);
    int e=!d ? -FS_EBADF : (d->node->generation!=t->vfs->generation[d->node->drive] || !vfs_identity_live(t->vfs,d->node->identity)) ? -FS_EIO : d->node->ro ? ro_handle(t->vfs,d->node) : VFS_IO(t->vfs,fat_commit(d->node->volume)); vfs_unlock(t->vfs); return e;
}
int vfs_completion(struct vfs_table *t, int h, struct cache_completion *out) {
    if (!out) return -FS_EINVAL;
    int e=handle_lock(t,h); if (e) return e;
    struct vfs_description *d=get(t,h);
    if (!d) e=-FS_EBADF;
    else if (d->node->generation!=t->vfs->generation[d->node->drive] || !vfs_identity_live(t->vfs,d->node->identity)) e=-FS_EIO;
    else if (d->node->ro) e=-FS_EROFS;
    else e=cache_completion(d->node->volume->cache,d->node->volume->dev,out);
    vfs_unlock(t->vfs); return e;
}
int vfs_flush_drive(struct vfs_table *t,unsigned drive) {
    if (drive>=26) return -FS_EINVAL;
    int e=vfs_admit(t->vfs,vfs_drive_mask(drive),t->request); if (e) return e;
    struct fat_volume *v=t->vfs->volumes[drive];
    if (v && !v->readonly) e=VFS_IO(t->vfs,fat_commit(v));
    else if (!v && !t->vfs->ro_volumes[drive]) e=-FS_ENOENT;
    vfs_unlock(t->vfs); return e;
}
static int log_handle(struct vfs_table *t, int h, struct vfs_description **out) {
    struct vfs_description *d=get(t,h); *out=d;
    if (!d) return -FS_EBADF;
    if (d->node->ro) return -FS_EROFS;
    if (!(d->access&VFS_WRITE) || !(d->deny&VFS_DENY_WRITE)) return -FS_EACCES;
    return 0;
}
int vfs_log_reserve(struct vfs_table *t, int h, uint32_t capacity) {
    int admission=handle_lock(t,h); if (admission) return admission;
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
    int admission=handle_lock(t,h); if (admission) return admission;
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
    int admission=handle_lock(t,h); if (admission) return admission;
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
    int ro=ro_target(t,path); if (ro<0) return ro;
    if (ro) {
        int admission=path_lock(t,path,0,0); if (admission) return admission; struct fs_ro_node n; unsigned d;
        int e=ro_path(t,path,&d,&n,0); if (!e && n.size>UINT32_MAX) e=-FS_EFBIG;
        if (!e) ro_entry(&n,out); vfs_unlock(t->vfs); return e;
    }
    int admission=path_lock(t,path,0,0); if (admission) return admission; struct resolved r; int e=lookup(t,path,&r,out);
    if (!e) { struct vfs_node *n=node_find(t->vfs,t->vfs->volumes[r.drive],out); if (n) *out=n->entry; }
    vfs_unlock(t->vfs); return e;
}
static int metadata(struct vfs_table *t, const char *path, int attr, const struct fat_times *times) {
    int ro=ro_target(t,path); if (ro<0) return ro;
    if (ro) return -FS_EROFS;
    int admission=path_lock(t,path,0,0); if (admission) return admission; struct resolved r; struct fat_entry ent; int e=lookup(t,path,&r,&ent);
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
static bool dos_match(const char *pattern,const char *name) {
    uint16_t mask[11],text[11];
    for (unsigned i=0;i<11;i++) mask[i]=text[i]=' ';
    unsigned at=0; bool extension=false,star=false;
    while (*pattern) {
        uint16_t ch; if (path_decode(&pattern,&ch)) return false;
        if (ch=='.') { at=8; extension=true; continue; }
        unsigned end=extension ? 11 : 8;
        if (ch=='*') {
            while (at<end) mask[at++]='?'; star=true;
            while (*pattern && *pattern!='.') { uint16_t skip; if (path_decode(&pattern,&skip)) return false; }
        } else { if (at>=end) return false; mask[at++]=path_upper(ch); }
    }
    if (star && !extension) for (unsigned i=8;i<11;i++) mask[i]='?';
    at=0; extension=false;
    while (*name) {
        uint16_t ch; if (path_decode(&name,&ch)) return false;
        if (ch=='.') { at=8; extension=true; continue; }
        if (at>=(extension ? 11u : 8u)) return false;
        text[at++]=path_upper(ch);
    }
    for (unsigned i=0;i<11;i++) if (mask[i]!='?' && mask[i]!=text[i]) return false;
    return true;
}
static int find_next(struct vfs_table *t, struct vfs_find *f, struct fat_entry *ent) {
    if (f->ro) {
        if (f->drive>=26 || f->generation!=t->vfs->generation[f->drive]) return -FS_EIO;
        int e=vfs_ro_check_locked(t->vfs,f->drive); if (e) return e;
        struct fs_ro_node n;
        while (!(e=VFS_IO(t->vfs,fs_ro_next(t->vfs->ro_volumes[f->drive],f->ro_directory,&f->cursor,&n)))) {
            if (n.attr & (2|4|16) & ~f->attr_mask) continue;
            if ((f->attr_mask&0x3f)==FAT_ATTR_VOLUME) continue;
            if ((f->attr_mask&VFS_FIND_DOS) ? dos_match(f->pattern,n.alias) : (path_wildcard(f->pattern,n.name) || path_wildcard(f->pattern,n.alias))) {
                if (n.size>UINT32_MAX) return -FS_EFBIG; ro_entry(&n,ent); return 0;
            }
        }
        if (e==-FS_EIO) vfs_ro_check_locked(t->vfs,f->drive); return e;
    }

    if (f->drive>=26 || !t->vfs->volumes[f->drive] || f->generation!=t->vfs->generation[f->drive]) return -FS_EIO;
    int e;
    while (!(e=VFS_IO(t->vfs,(f->attr_mask&VFS_FIND_DOS) ? fat_next_dos(t->vfs->volumes[f->drive],f->directory,&f->cursor,ent) : fat_next(t->vfs->volumes[f->drive],f->directory,&f->cursor,ent)))) {
        if ((f->attr_mask&VFS_FIND_DOS) && ((f->attr_mask&0x3f)==FAT_ATTR_VOLUME ? !(ent->attr&FAT_ATTR_VOLUME) : (ent->attr&FAT_ATTR_VOLUME)!=0)) continue;
        if (ent->attr & (FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM|FAT_ATTR_DIR) & ~f->attr_mask) continue;
        if ((f->attr_mask&VFS_FIND_DOS) && !(ent->attr&FAT_ATTR_VOLUME)) {
            if (dos_match(f->pattern,ent->alias)) return 0;
        } else if (path_wildcard(f->pattern,ent->name) || path_wildcard(f->pattern,ent->alias)) return 0;
    }
    return e;
}
int vfs_find_first(struct vfs_table *t, const char *p, uint8_t mask, struct vfs_find *f, struct fat_entry *ent) {
    int ro=ro_target(t,p); if (ro<0) return ro;
    if (ro) {
        int admission=path_lock(t,p,0,0); if (admission) return admission; char parent[FS_PATH_BYTES];
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

    int admission=path_lock(t,p,0,0); if (admission) return admission; struct resolved r; int e=resolve(t,p,&r);
    if (!e && !r.name[0]) e=-FS_EINVAL;
    if (!e) { memset(f,0,sizeof(*f)); f->drive=r.drive; f->directory=r.parent; f->generation=t->vfs->generation[r.drive]; f->attr_mask=mask; memcpy(f->pattern,r.name,strlen(r.name)+1); e=find_next(t,f,ent); }
    vfs_unlock(t->vfs); return e;
}
int vfs_find_next(struct vfs_table *t, struct vfs_find *f, struct fat_entry *ent) {
    int admission=vfs_admit(t->vfs,vfs_drive_mask(f->drive),t->request); if (admission) return admission;
    int e=find_next(t,f,ent); vfs_unlock(t->vfs); return e;
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
    int ro=ro_target(t,path); if (ro<0) return ro;
    if (ro) return -FS_EROFS;
    int admission=path_lock(t,path,0,0); if (admission) return admission; struct resolved r; struct fat_entry ent; int e=lookup(t,path,&r,&ent);
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
    int admission=path_lock(t,current,previous,oldest); if (admission) return admission;
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
    int ro=ro_target(t,p); if (ro<0) return ro;
    if (ro) return -FS_EROFS;
    int admission=path_lock(t,p,0,0); if (admission) return admission; struct resolved r; int e=resolve(t,p,&r); struct fat_entry ent;
    if (!e && !r.name[0]) e=-FS_EEXIST;
    if (!e) e=VFS_IO(t->vfs,fat_create(t->vfs->volumes[r.drive],r.parent,r.name,attr|FAT_ATTR_DIR,&ent));
    vfs_unlock(t->vfs); return e;
}
int vfs_rename(struct vfs_table *t, const char *from, const char *to) {
    int ro=ro_target(t,from); if (ro<0) return ro; if (ro) return -FS_EROFS;
    ro=ro_target(t,to); if (ro<0) return ro; if (ro) return -FS_EROFS;
    int admission=path_lock(t,from,to,0); if (admission) return admission; struct resolved a,b; struct fat_entry ent; int e=lookup(t,from,&a,&ent);
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
    int ro=ro_target(t,p); if (ro<0) return ro;
    if (ro) {
        int admission=path_lock(t,p,0,0); if (admission) return admission; char canonical[FS_PATH_BYTES]; struct fs_ro_node n; unsigned d;
        int e=ro_path(t,p,&d,&n,canonical); if (!e && !(n.attr&FS_RO_DIRECTORY)) e=-FS_ENOTDIR;
        if (!e) memcpy(t->cwd[d],canonical,strlen(canonical)+1);
        vfs_unlock(t->vfs); return e;
    }

    int admission=path_lock(t,p,0,0); if (admission) return admission; struct resolved r; struct fat_entry ent; int e=lookup(t,p,&r,&ent);
    if (!e && !(ent.attr&FAT_ATTR_DIR)) e=-FS_ENOTDIR;
    if (!e) {
        char full[FS_PATH_BYTES], canonical[FS_PATH_BYTES]; unsigned d; e=normalize(t,p,&d,full);
        if (!e) {
            unsigned used=1; canonical[0]='/'; canonical[1]=0; uint32_t dir=root(t->vfs->volumes[d]); const char *s=full+1;
            while (*s && !e) {
                unsigned n=0; while (*s && *s!='/') r.name[n++]=*s++; r.name[n]=0;
                e=VFS_IO(t->vfs,fat_lookup(t->vfs->volumes[d],dir,r.name,&ent)); if (e) break;
                bool dos=t->owner_type==VFS_OWNER_DOS ||
                    (!t->owner_registered && t->owner && t->owner!=(uintptr_t)t);
                const char *component=dos && ent.alias[0] ? ent.alias : ent.name;
                n=(unsigned)strlen(component); if (used>1) canonical[used++]='/';
                if (used+n>=sizeof(canonical)) { e=-FS_ENAMETOOLONG; break; }
                memcpy(canonical+used,component,n+1); used+=n; dir=ent.first; if (*s) s++;
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
    if (d>=26 || !out) return -FS_EINVAL;
    int admission=vfs_admit(t->vfs,vfs_drive_mask(d),t->request); if (admission) return admission;
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
