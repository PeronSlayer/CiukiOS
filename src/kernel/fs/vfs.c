/* SPDX-License-Identifier: GPL-2.0-only */
#include "vfs.h"
#include "../include/ciuki/vfs_hooks.h"
#include <ciuki/kernel.h>
#ifdef FS_HOST
#pragma weak klog_periodic
#pragma weak klog_diag
#define DIAG_LOG(fn,...) do { if (fn) fn(__VA_ARGS__); } while (0)
#else
#define DIAG_LOG(fn,...) fn(__VA_ARGS__)
#endif
/* The host can inspect waits without requiring kernel logging. */
static void tag_copy(char out[12],const char *tag) {
    unsigned i=0; if (!tag || !*tag) tag="metadata";
    for (;i<11 && tag[i];i++) out[i]=tag[i];
    for (;i<12;i++) out[i]=0;
}
/* Namespace snapshots may be sampled before acquiring the existing lock.
 * Atomic scalar/byte access avoids a host data race; volume snapshots are
 * always taken while namespace is held and survive every VFS_IO phase. */
static struct vfs_hold_info holder_snapshot(const struct vfs_hold_info *h) {
    struct vfs_hold_info out;
    for (unsigned i=0;i<16;i++) out.task.name[i]=__atomic_load_n(&h->task.name[i],__ATOMIC_RELAXED);
    out.task.id=__atomic_load_n(&h->task.id,__ATOMIC_RELAXED);
    for (unsigned i=0;i<12;i++) out.op[i]=__atomic_load_n(&h->op[i],__ATOMIC_RELAXED);
    for (unsigned i=0;i<12;i++) out.site[i]=__atomic_load_n(&h->site[i],__ATOMIC_RELAXED);
    out.started=__atomic_load_n(&h->started,__ATOMIC_RELAXED);
    return out;
}
static void namespace_publish(struct vfs *v,const struct fs_task_identity *task,const char *tag) {
    for (unsigned i=0;i<16;i++) __atomic_store_n(&v->namespace_owner.task.name[i],task->name[i],__ATOMIC_RELAXED);
    __atomic_store_n(&v->namespace_owner.task.id,task->id,__ATOMIC_RELAXED);
    char op[12]; tag_copy(op,tag);
    for (unsigned i=0;i<12;i++) __atomic_store_n(&v->namespace_owner.op[i],op[i],__ATOMIC_RELAXED);
}
static void namespace_owner(struct vfs *v,const struct vfs_request *r) {
    struct fs_task_identity task=fs_request_identity(r);
    namespace_publish(v,&task,r ? r->operation : "metadata");
    vfs_site_locked(v,r && r->site ? r->site : "vfs");
}
void vfs_tag_locked(struct vfs *v,const char *tag) {
    char op[12]; tag_copy(op,tag);
    for (unsigned d=0;d<26;d++) if (v->active&(1u<<d)) memcpy(v->holds[d].op,op,sizeof(op));
    for (unsigned i=0;i<12;i++) __atomic_store_n(&v->namespace_owner.op[i],op[i],__ATOMIC_RELAXED);
}
void vfs_site_locked(struct vfs *v,const char *site) {
    char name[12]; tag_copy(name,site);
    for (unsigned d=0;d<26;d++) if (v->active&(1u<<d)) memcpy(v->holds[d].site,name,sizeof(name));
    for (unsigned i=0;i<12;i++) __atomic_store_n(&v->namespace_owner.site[i],name[i],__ATOMIC_RELAXED);
}
static char first_drive(uint32_t mask) {
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) return (char)('A'+d);
    return '-';
}
static struct vfs_pending_waits waits_detach(struct vfs *v) {
    struct vfs_pending_waits pending=v->diagnostic_pending;
    v->diagnostic_pending.count=0; return pending;
}
static void wait_capture(struct vfs_pending_waits *pending,uint32_t start,char drive,char holder,const char *reason,
                        const struct fs_task_identity *task,const struct vfs_hold_info *owner) {
    uint32_t ms=fs_now_ms()-start;
    if (ms<100) return;
    /* Admission has at most two waits in a turn; repeated file waits end
     * their turn with vfs_unlock before retrying. */
    if (pending->count<2) pending->records[pending->count++]=(struct vfs_wait_record){
        .task=*task,.owner=*owner,.ms=ms,.drive=drive,.holder=holder,.reason=reason};
}
struct hold_record { struct vfs_hold_info owner; uint32_t ms,waiters; char drive; };
static unsigned budget_drive(char drive) { return drive>='A' && drive<='Z' ? (unsigned)(drive-'A') : 26; }
static bool budget_allow(struct vfs_contention_budget *b,uint32_t ms,bool hold) {
    unsigned *count=hold ? &b->holds : &b->waits;
    uint32_t *max=hold ? &b->hold_max_ms : &b->wait_max_ms;
    uint32_t *suppressed=hold ? &b->hold_suppressed : &b->wait_suppressed;
    if (ms>*max) *max=ms;
    if (*count<8) { (*count)++; return true; }
    if (*suppressed<UINT32_MAX) (*suppressed)++;
    return false;
}
/* Called only at an unlocked boundary, after volume waiters are woken.
 * The existing identity lock serializes counters; no sink runs under it.
 * A sink may yield/reenter VFS without borrowing this turn's snapshots. */
static void diagnostics_publish(struct vfs *v,const struct hold_record *holds,unsigned count,
                                const struct vfs_pending_waits *pending) {
    struct vfs_contention_budget summary[27];
    bool hold_allowed[26],wait_allowed[2]; uint32_t now=fs_now_ms();
    fs_lock_take(&v->identity_lock);
    for (unsigned d=0;d<27;d++) {
        struct vfs_contention_budget *b=&v->diagnostic_budget[d];
        summary[d].valid=b->valid && now-b->start>=2000 && (b->hold_suppressed || b->wait_suppressed);
        if (summary[d].valid) summary[d]=*b;
        if (!b->valid || now-b->start>=2000) *b=(struct vfs_contention_budget){.start=now,.valid=true};
    }
    for (unsigned i=0;i<count;i++) hold_allowed[i]=budget_allow(&v->diagnostic_budget[budget_drive(holds[i].drive)],holds[i].ms,true);
    for (unsigned i=0;i<pending->count;i++) wait_allowed[i]=budget_allow(&v->diagnostic_budget[budget_drive(pending->records[i].drive)],pending->records[i].ms,false);
    fs_lock_drop(&v->identity_lock);
    for (unsigned d=0;d<27;d++) if (summary[d].valid) {
        DIAG_LOG(klog_periodic,"[storage] vfs_contention drive=%c hold_suppressed=%u wait_suppressed=%u hold_max_ms=%u wait_max_ms=%u",
            d<26 ? 'A'+d : '-',summary[d].hold_suppressed,summary[d].wait_suppressed,summary[d].hold_max_ms,summary[d].wait_max_ms);
    }
    for (unsigned i=0;i<count;i++) if (hold_allowed[i]) {
        DIAG_LOG(klog_periodic,"[storage] vfs_hold drive=%c ms=%u task=%s#%u op=%s site=%s waiters=%u",
            holds[i].drive,holds[i].ms,holds[i].owner.task.name,holds[i].owner.task.id,holds[i].owner.op,holds[i].owner.site,holds[i].waiters);
        if (vfs_hold_observer) vfs_hold_observer(holds[i].drive,holds[i].ms,&holds[i].owner,holds[i].waiters);
    }
    for (unsigned i=0;i<pending->count;i++) if (wait_allowed[i]) {
        const struct vfs_wait_record *r=&pending->records[i];
        DIAG_LOG(klog_diag,"[storage] vfs_wait drive=%c ms=%u holder=%c reason=%s task=%s#%u holder_task=%s#%u op=%s site=%s",
            r->drive,r->ms,r->holder,r->reason,r->task.name,r->task.id,
            r->owner.task.name[0] ? r->owner.task.name : "none",r->owner.task.id,r->owner.op[0] ? r->owner.op : "metadata",r->owner.site[0] ? r->owner.site : "vfs");
        if (vfs_wait_observer) vfs_wait_observer(r->drive,r->ms,r->holder,r->reason,&r->task,&r->owner);
    }
}
static void namespace_take(struct vfs *v,uint32_t mask) {
    uint32_t start=fs_now_ms();
    char holder=__atomic_load_n(&v->holder,__ATOMIC_RELAXED);
    struct vfs_hold_info owner=holder_snapshot(&v->namespace_owner);
    struct fs_task_identity task=fs_request_identity(fs_request_current());
    fs_lock_take(&v->lock);
    wait_capture(&v->diagnostic_pending,start,first_drive(mask),holder,"namespace",&task,&owner);
    namespace_owner(v,fs_request_current());
}
/* f2-03 integration notes: weak observers keep the F1 harness independent.
 * 1. Detach respects native opens and invalidates mount-lifetime identities.
 * 2. Native/legacy share claims use the same lock and bidirectional matrix.
 * 3. Legacy mutations update native cached metadata/identities under that lock.
 * 4. The final legacy close releases retained open-unlinked chains exactly once.
 * 5. Legacy namespace mutations respect native cwd/open-directory pins.
 * 6. Native rmdir/replacement also honors legacy per-drive cwd pins. */
/* dep_*: the entry naming `parent` (f1-89 walk dependency); dep_index
 * UINT32_MAX for the root, which nothing names. */
struct resolved { unsigned drive; uint32_t parent, dep_parent, dep_index; char name[FS_NAME_BYTES]; };
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
    struct fs_task_identity task=request ? fs_request_identity(request) : holder_snapshot(&v->namespace_owner).task;
    char site[12]; tag_copy(site,request && request->site ? request->site : v->namespace_owner.site);
    char operation[12]; tag_copy(operation,request && request->operation ? request->operation : v->namespace_owner.op);
    struct vfs_hold_info owner=holder>='A' && holder<='Z' ? v->holds[holder-'A'] : (struct vfs_hold_info){0};
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) v->diagnostic_waiters[d]++;
    uint64_t request_start=request ? request->now(request->arg) : 0;
    struct vfs_pending_waits pending=waits_detach(v);
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
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) v->diagnostic_waiters[d]--;
    v->diagnostic_pending=pending;
    request_wait(request,request_start,mask,VFS_WAIT_VOLUME,error);
    wait_capture(&v->diagnostic_pending,start,first_drive(mask),holder,error==-VFS_ETIMEOUT ? "volume:timeout" : error ? "volume:cancelled" : "volume",&task,&owner);
    if (error) { advance_cancelled(v); fs_event_wake(&v->volume_changed); return error; }
    v->reserved |= mask;
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) {
        v->holds[d]=(struct vfs_hold_info){.task=task,.started=fs_now_ms()};
        memcpy(v->holds[d].op,operation,sizeof(operation));
        memcpy(v->holds[d].site,site,sizeof(site));
    }
    v->active_tickets |= mask;
    /* A free additional letter can precede an existing one. Reacquire the
     * complete set in order, retaining all reservations under namespace. */
    volume_mutexes(v,v->active,false); v->active |= mask;
    volume_mutexes(v,v->active,true);
    namespace_publish(v,&task,operation); vfs_site_locked(v,site);
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
    struct vfs_hold_info owner=holder_snapshot(&v->namespace_owner);
    struct fs_task_identity task=fs_request_identity(request);
    char holder=__atomic_load_n(&v->holder,__ATOMIC_RELAXED);
    while (!fs_lock_try_take(&v->lock)) {
        if ((error=vfs_request_check(request))) {
            request_wait(request,begin,mask,VFS_WAIT_NAMESPACE,error);
            struct vfs_pending_waits pending={0};
            wait_capture(&pending,start,first_drive(mask),holder,error==-VFS_ETIMEOUT ? "namespace:timeout" : "namespace:cancelled",&task,&owner);
            diagnostics_publish(v,0,0,&pending);
            return error;
        }
        request->pause(request->arg);
    }
    request_wait(request,begin,mask,VFS_WAIT_NAMESPACE,0);
    wait_capture(&v->diagnostic_pending,start,first_drive(mask),holder,"namespace",&task,&owner);
    namespace_owner(v,request);
    error=reserve_request(v,mask,request);
    if (error) {
        struct vfs_pending_waits pending=waits_detach(v);
        fs_lock_drop(&v->lock); diagnostics_publish(v,0,0,&pending); return error;
    }
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
    struct hold_record holds[26];
    unsigned count=0;
    struct vfs_pending_waits pending=waits_detach(v);
    uint32_t released=fs_now_ms();
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) {
        struct vfs_hold_info h=v->holds[d]; uint32_t held=released-h.started;
        if (held>=50) {
            holds[count].owner=h; holds[count].ms=held;
            holds[count].waiters=v->diagnostic_waiters[d]; holds[count++].drive='A'+d;
        }
        memset(&v->holds[d],0,sizeof(v->holds[d]));
    }
    v->active=0; v->active_tickets=0; v->reserved &= ~mask; v->mutating &= ~mask;
    for (unsigned d=0;d<26;d++) if (tickets&(1u<<d)) v->serving[d]++;
    advance_cancelled(v);
    __atomic_store_n(&v->holder,'-',__ATOMIC_RELAXED);
    volume_mutexes(v,mask,false); fs_lock_drop(&v->lock);
    if (mask) fs_event_wake(&v->volume_changed);
    diagnostics_publish(v,holds,count,&pending);
}
struct vfs_io_state vfs_io_begin_locked(struct vfs *v) {
    struct vfs_io_state state={.drives=v->active,.default_drive=v->default_drive,.logical=true,
        .diagnostics=waits_detach(v)};
    for (unsigned d=0;d<26;d++) if ((v->active&(1u<<d)) &&
        (!v->volumes[d] || !v->volumes[d]->dev || !v->volumes[d]->dev->logical)) state.logical=false;
    /* A logical provider can enter the outer physical VFS. Retain its
     * mutation guard, but no inner mutex across the outer namespace entry. */
    /* The mutation guard serializes FAT workspace/publication, and readers
     * use private workspaces. No physical FS mutex (and its preemption hold)
     * may cover filesystem/device work. This also applies to physical disks. */
    volume_mutexes(v,v->active,false);
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
    (void)fs_lock_retake(&v->lock,VFS_WAIT_NAMESPACE);
    volume_mutexes(v,mask,true); v->active=mask;
    v->diagnostic_pending=state.diagnostics;
    char drive=first_drive(mask);
    if (drive>='A' && drive<='Z') {
        namespace_publish(v,&v->holds[drive-'A'].task,v->holds[drive-'A'].op);
        vfs_site_locked(v,v->holds[drive-'A'].site);
    }
    else namespace_owner(v,fs_request_current());
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
/* f3-51c write generations, identity_lock held. */
static void change_record(struct vfs *v, struct fat_volume *vol, uint32_t parent, uint32_t index) {
    uint64_t mount=identity_generation(v,vol);
    struct vfs_change *slot=0;
    for (unsigned i=0;i<VFS_CHANGES && !slot;i++) {
        struct vfs_change *c=&v->changes[i];
        if (c->sequence && c->volume==vol && c->mount==mount && c->parent==parent && c->index==index) slot=c;
    }
    if (!slot) {
        slot=&v->changes[0];
        for (unsigned i=0;i<VFS_CHANGES;i++) if (v->changes[i].sequence<slot->sequence) slot=&v->changes[i];
        if (slot->sequence>v->change_floor) v->change_floor=slot->sequence; /* evicted: bound */
        *slot=(struct vfs_change){.volume=vol,.mount=mount,.parent=parent,.index=index};
    }
    slot->sequence=++v->change_sequence;
}
static uint64_t change_generation(struct vfs *v, struct fat_volume *vol, uint32_t parent, uint32_t index) {
    uint64_t mount=identity_generation(v,vol);
    for (unsigned i=0;i<VFS_CHANGES;i++) {
        struct vfs_change *c=&v->changes[i];
        if (c->sequence && c->volume==vol && c->mount==mount && c->parent==parent && c->index==index) return c->sequence;
    }
    return v->change_floor;
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
    /* f3-51c: any file of this volume may have changed (raw write, removal). */
    for (unsigned i=0;i<VFS_CHANGES;i++) if (v->changes[i].volume==vol) memset(&v->changes[i],0,sizeof(v->changes[i]));
    v->change_floor=++v->change_sequence;
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
    change_record(v,vol,old->parent,old->index);
    if (why==FAT_ID_RENAME && entry) change_record(v,vol,entry->parent,entry->index);
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
    if (v->transaction_users[d]) return -FS_EBUSY;
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
    /* A suspended mutation still borrows this table/description until its
     * final namespace turn returns to the public operation. Teardown waits
     * without reserving the volume needed by that retirement. */
    for (;;) {
        bool pending=false;
        for (unsigned d=0;d<26;d++) if ((mask&(1u<<d)) && v->transaction_users[d]) pending=true;
        if (!pending) break;
        fs_event_wait(&v->volume_changed,&v->lock);
    }
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
/* f1-89 writer turn. One FAT-mutating operation per letter owns it for its
 * whole life, so its unguarded transaction phases never race another
 * publisher (no spurious EAGAIN, no lost FSInfo/free-count updates). It is
 * acquired before volume admission and waited for with namespace only, so
 * the order is turn -> namespace -> volumes -> cache. Readers never touch
 * it; they are excluded from publication steps only by the short guard. */
static uint32_t turn_drives(struct vfs *v,uint32_t mask) {
    uint32_t out=0;
    for (unsigned d=0;d<26;d++) if ((mask&(1u<<d)) && v->volumes[d] &&
        !(v->volumes[d]->dev && v->volumes[d]->dev->logical)) out|=1u<<d;
    return out;
}
bool vfs_turn_held_locked(struct vfs *v,uint32_t mask) {
    uintptr_t self=fs_task_token();
    for (unsigned d=0;d<26;d++) if ((mask&(1u<<d)) && (!v->turn_depth[d] || v->turn_owner[d]!=self)) return false;
    return true;
}
/* A newcomer never passes a queued writer; queued writers compete only
 * among themselves once the owner leaves. The owner may nest. */
static bool turn_available(struct vfs *v,uint32_t mask,bool queued) {
    uintptr_t self=fs_task_token();
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) {
        if (v->turn_depth[d] && v->turn_owner[d]!=self) return false;
        if (!v->turn_depth[d] && v->turn_waiters[d] && !queued) return false;
    }
    return true;
}
static void turn_hold(struct vfs *v,uint32_t mask) {
    uintptr_t self=fs_task_token();
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) { v->turn_owner[d]=self; v->turn_depth[d]++; }
}
void vfs_turn_drop_locked(struct vfs *v,uint32_t mask) {
    uintptr_t self=fs_task_token(); bool wake=false;
    for (unsigned d=0;d<26;d++)
        if ((mask&(1u<<d)) && v->turn_depth[d] && v->turn_owner[d]==self && !--v->turn_depth[d]) {
            v->turn_owner[d]=0; wake=true;
        }
    if (wake) fs_event_wake(&v->volume_changed);
}
/* Namespace held; the caller holds no reservation that the owner could
 * need. Cancellable through the current request; on error nothing is held. */
static int turn_queue_locked(struct vfs *v,uint32_t mask) {
    if (turn_available(v,mask,false)) { turn_hold(v,mask); return 0; }
    uint32_t start=fs_now_ms(); int e=0;
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) v->turn_waiters[d]++;
    while (!e && !turn_available(v,mask,true))
        e=fs_event_wait_request(&v->volume_changed,&v->lock,VFS_WAIT_VOLUME);
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) v->turn_waiters[d]--;
    if (!e) turn_hold(v,mask);
    uint32_t waited=fs_now_ms()-start;
    v->turn_waits++; if (waited>v->turn_wait_max_ms) v->turn_wait_max_ms=waited;
    fs_event_wake(&v->volume_changed); /* other queued writers re-evaluate */
    return e;
}
int vfs_turn_take_locked(struct vfs *v,uint32_t drives,uint32_t *taken) {
    *taken=0; uint32_t mask=turn_drives(v,drives);
    if (!mask) return 0;
    if (turn_available(v,mask,false)) { turn_hold(v,mask); *taken=mask; return 0; }
    /* Same contract as vfs_file_wait_locked: no reservation is retained
     * while waiting, the caller's set is readmitted afterwards. */
    uint32_t active=v->active; unsigned default_drive=v->default_drive;
    struct vfs_hold_info owner=holder_snapshot(&v->namespace_owner);
    vfs_unlock(v); vfs_lock(v,0);
    int e=turn_queue_locked(v,mask);
    vfs_unlock(v); vfs_lock(v,active);
    v->default_drive=default_drive; vfs_tag_locked(v,owner.op); vfs_site_locked(v,owner.site);
    if (!e) *taken=mask;
    return e;
}
/* Public operations: queue for the turn with namespace only, then admit. */
static int turn_acquire(struct vfs *v,uint32_t drives,const struct vfs_request *request,uint32_t *taken) {
    *taken=0;
    int e=vfs_admit(v,0,request); if (e) return e;
    uint32_t mask=turn_drives(v,drives);
    struct fs_request_scope scope; fs_request_enter(&scope,request ? request : fs_request_current());
    if (mask) e=turn_queue_locked(v,mask);
    fs_request_leave(&scope);
    if (!e) *taken=mask;
    vfs_unlock(v); return e;
}
static void turn_release(struct vfs *v,uint32_t taken) {
    if (!taken) return;
    vfs_lock(v,0); vfs_turn_drop_locked(v,taken); vfs_unlock(v);
}
static void writer_unlock(struct vfs *v,uint32_t taken) {
    vfs_turn_drop_locked(v,taken); vfs_unlock(v);
}
/* Writer admission: the turn first (namespace only), then the ordinary
 * FIFO volume admission. A drive attached meanwhile restarts the pair. */
static int path_lock_writer(struct vfs_table *t,const char *a,const char *b,const char *c,uint32_t *turn) {
    *turn=0;
    for (;;) {
        uint32_t want=vfs_path_mask(t,a) | (b ? vfs_path_mask(t,b) : 0) | (c ? vfs_path_mask(t,c) : 0), taken;
        int e=turn_acquire(t->vfs,want,t->request,&taken); if (e) return e;
        e=path_lock(t,a,b,c);
        if (e) { turn_release(t->vfs,taken); return e; }
        if (!(turn_drives(t->vfs,t->vfs->active)&~taken)) { *turn=taken; return 0; }
        writer_unlock(t->vfs,taken);
    }
}
struct transaction_guard {
    struct vfs *v;
    uint32_t mask;
    unsigned drive, default_drive;
    bool retired;
    char op[12],site[12];
};
/* A publication step excludes metadata holders and queued tickets, not the
 * private open-file readers: those read other identities (the target's own
 * readers are refused by its writer flag and node pin, and drained at the
 * operation's admission), whose chains no step of this mutation changes. */
static bool guard_busy(struct transaction_guard *g) {
    struct vfs *v=g->v;
    bool busy=((v->reserved|v->mutating)&g->mask)!=0;
    for (unsigned d=0;d<26;d++) if (g->mask&(1u<<d)) busy|=v->next[d]!=v->serving[d];
    return busy;
}
static int transaction_guard_enter(void *arg) {
    struct transaction_guard *g=arg; struct vfs *v=g->v;
    vfs_lock(v,0);
    /* Never wait for a legacy writer while retaining cache reservations:
     * that writer may itself need one of our pinned sectors. */
    if (guard_busy(g)) { vfs_unlock(v); return -FS_EBUSY; }
    vfs_tag_locked(v,g->op); vfs_site_locked(v,g->site);
    vfs_reserve_locked(v,g->mask); v->default_drive=g->default_drive;
    return 0;
}
/* In-flight cluster set: namespace only, excluding a legacy VFS_IO mutator
 * (which walks the set without namespace). No reservation, no tickets. */
static int transaction_guard_claim(void *arg) {
    struct transaction_guard *g=arg; struct vfs *v=g->v;
    vfs_lock(v,0);
    if (v->mutating&g->mask) { vfs_unlock(v); return -FS_EBUSY; }
    return 0;
}
static void transaction_guard_unclaim(void *arg) { vfs_unlock(((struct transaction_guard *)arg)->v); }
static void transaction_guard_leave(void *arg) {
    struct transaction_guard *g=arg;
    /* Transfer the final validated namespace turn directly to the caller.
     * Dropping/reacquiring it here would make a just-resolved path stale. */
    if (!g->retired) vfs_unlock(g->v);
}
static void transaction_guard_retire(void *arg) {
    struct transaction_guard *g=arg;
    vfs_lock(g->v,g->mask); g->v->default_drive=g->default_drive;
    vfs_tag_locked(g->v,g->op); vfs_site_locked(g->v,g->site);
    g->retired=true;
}
static int transaction_guard_wait(void *arg) {
    struct transaction_guard *g=arg; struct vfs *v=g->v;
    vfs_lock(v,0);
    int e=guard_busy(g) ? fs_event_wait_request(&v->volume_changed,&v->lock,VFS_WAIT_VOLUME) : 0;
    vfs_unlock(v); return e;
}
int vfs_file_wait_locked(struct vfs *v,const bool *busy) {
    uint32_t mask=v->active; unsigned default_drive=v->default_drive;
    struct vfs_hold_info owner=holder_snapshot(&v->namespace_owner);
    int e=0;
    while (*busy && !e) {
        vfs_unlock(v); vfs_lock(v,0);
        while (*busy && !e) e=fs_event_wait_request(&v->volume_changed,&v->lock,VFS_WAIT_VOLUME);
        vfs_unlock(v); vfs_lock(v,mask);
    }
    v->default_drive=default_drive; vfs_tag_locked(v,owner.op); vfs_site_locked(v,owner.site);
    return e;
}
/* Pins protect the source volume, matching descriptions and their node for
 * the entire unlocked operation. The mutation sequence validates cold
 * planning against every other FAT publisher, including legacy callers. */
static bool transaction_read_only(enum fat_transaction_op op) {
    return op==FAT_TX_LOOKUP || op==FAT_TX_NEXT || op==FAT_TX_COMMIT;
}
static int transaction_once(struct vfs *,struct fat_volume *,struct fat_transaction_args,unsigned);
/* noinline: callers such as vfs_log_rotate keep the i686 4 KiB frame. */
__attribute__((noinline)) int vfs_fat_transaction_locked(struct vfs *v, struct fat_volume *vol, struct fat_transaction_args a) {
    unsigned drive=26;
    for (unsigned d=0;d<26;d++) if (v->volumes[d]==vol) { drive=d; break; }
    if (drive==26 || !(v->active&(1u<<drive))) return -FS_EIO;
    if (a.guarded && (a.op==FAT_TX_LOOKUP || a.op==FAT_TX_NEXT)) {
        /* Reader restart fallback: the short guard excludes every
         * publication step; the preceding attempts warmed the cache. */
        return a.op==FAT_TX_LOOKUP ? VFS_IO(v,fat_lookup(vol,a.directory,a.name,a.entry)) :
            VFS_IO(v,fat_next(vol,a.directory,a.directory_cursor,a.entry));
    }
    /* LOG_WRITE publishes only DATA into owned reserved clusters: it never
     * stales another transaction, so the storage writer takes no turn. */
    if (vol->dev->logical || transaction_read_only(a.op) || a.op==FAT_TX_LOG_WRITE) return transaction_once(v,vol,a,drive);
    /* A mutation outside a writer-admitted operation (node-based callers
     * such as final close of an unlinked file or a native write) takes the
     * turn here. Its entry is the live node entry, copied by the
     * transaction after any wait, so no borrowed name can be stale. */
    uint32_t lazy=0;
    if (!vfs_turn_held_locked(v,1u<<drive)) {
        int e=vfs_turn_take_locked(v,1u<<drive,&lazy); if (e) return e;
        if (!(v->active&(1u<<drive)) || v->volumes[drive]!=vol) { vfs_turn_drop_locked(v,lazy); return -FS_EIO; }
    }
    int e=transaction_once(v,vol,a,drive);
    vfs_turn_drop_locked(v,lazy);
    return e;
}
static int transaction_once(struct vfs *v,struct fat_volume *vol,struct fat_transaction_args a,unsigned drive) {
    if (vol->dev->logical) {
        switch (a.op) {
        case FAT_TX_LOOKUP: return VFS_IO(v,fat_lookup(vol,a.directory,a.name,a.entry));
        case FAT_TX_CREATE: return VFS_IO(v,fat_create(vol,a.directory,a.name,a.attr,a.entry));
        case FAT_TX_WRITE: return VFS_IO(v,fat_write_cursor(vol,a.entry,a.cursor,a.position,a.data,a.size,a.done));
        case FAT_TX_LOG_WRITE: return VFS_IO(v,fat_write_reserved_cursor(vol,a.entry,a.cursor,a.capacity,a.position,a.data,a.size,a.done));
        case FAT_TX_COMMIT: return VFS_IO(v,fat_commit(vol));
        case FAT_TX_REMOVE: return VFS_IO(v,fat_remove(vol,a.entry,a.flag));
        case FAT_TX_RENAME: return VFS_IO(v,fat_rename(vol,a.entry,a.directory,a.name));
        case FAT_TX_PUBLISH: return VFS_IO(v,fat_publish_reserved(vol,a.entry,a.flag));
        case FAT_TX_EXTEND: return VFS_IO(v,fat_extend(vol,a.entry,a.capacity));
        case FAT_TX_RENAME_REPLACE: return VFS_IO(v,fat_rename_replace(vol,a.entry,a.replacement,a.directory,a.name,a.flag));
        case FAT_TX_TRUNCATE: return VFS_IO(v,fat_truncate(vol,a.entry,a.position));
        case FAT_TX_RESERVE: return VFS_IO(v,fat_reserve(vol,a.entry,a.capacity));
        case FAT_TX_NEXT: return VFS_IO(v,fat_next(vol,a.directory,a.directory_cursor,a.entry));
        case FAT_TX_SET_METADATA: return VFS_IO(v,fat_set_metadata(vol,a.entry,a.attr,a.times));
        }
        return -FS_EINVAL;
    }
    struct transaction_guard g={.v=v,.mask=v->active,.drive=drive,.default_drive=v->default_drive};
    memcpy(g.op,v->holds[drive].op,sizeof(g.op)); memcpy(g.site,v->holds[drive].site,sizeof(g.site));
    struct vfs_node *node=0;
    struct vfs_file_identity *identity=0,*replacement_id=0;
    uint64_t planned=vol->mutation_sequence;
    bool writer=a.entry && a.op!=FAT_TX_CREATE && a.op!=FAT_TX_LOOKUP && a.op!=FAT_TX_NEXT && a.op!=FAT_TX_COMMIT;
    if (writer) {
        int e=vfs_identity_get(v,vol,a.entry,&identity); if (e) return e;
        if (a.replacement) e=vfs_identity_get(v,vol,a.replacement,&replacement_id);
        /* Claim both only when both are available. No AB/BA ownership is
         * retained across a wait on the other rename identity. */
        while (!e && (identity->writer || (replacement_id && replacement_id->writer)))
            e=vfs_file_wait_locked(v,identity->writer ? &identity->writer : &replacement_id->writer);
        if (!e && (!vfs_identity_live(v,identity) || (replacement_id && !vfs_identity_live(v,replacement_id)))) e=-FS_EIO;
        if (!e && vol->mutation_sequence!=planned && a.op!=FAT_TX_LOG_WRITE) e=-FS_EAGAIN;
        if (e) { vfs_identity_put(v,replacement_id); vfs_identity_put(v,identity); return e; }
        identity->writer=true;
        if (replacement_id) replacement_id->writer=true;
    }
    for (unsigned i=0;i<VFS_NODES;i++) if (v->nodes[i].refs &&
        (a.entry==&v->nodes[i].entry || (identity && v->nodes[i].identity==identity))) { node=&v->nodes[i]; break; }
    if (node && node->pins) {
        if (replacement_id) { replacement_id->writer=false; vfs_identity_put(v,replacement_id); }
        if (identity) { identity->writer=false; vfs_identity_put(v,identity); }
        return -FS_EBUSY;
    }
    bool pinned[VFS_DESCRIPTIONS]={0};
    if (node) {
        node->pins++;
        for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) if (v->descriptions[i].refs && v->descriptions[i].node==node) {
            if (v->descriptions[i].io_active) {
                if (replacement_id) { replacement_id->writer=false; vfs_identity_put(v,replacement_id); }
                if (identity) { identity->writer=false; vfs_identity_put(v,identity); }
                node->pins--; return -FS_EBUSY;
            }
        }
        for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) if (v->descriptions[i].refs && v->descriptions[i].node==node) {
            pinned[i]=true; v->descriptions[i].refs++; v->descriptions[i].io_active=true;
        }
    }
    v->transaction_users[drive]++;
    a.sequence=vol->mutation_sequence;
    vfs_unlock(v);
    const struct fat_transaction_guard callbacks={&g,transaction_guard_enter,transaction_guard_leave,transaction_guard_wait,
        transaction_guard_retire,transaction_guard_claim,transaction_guard_unclaim};
    int e=fat_transaction_run(vol,&a,&callbacks);
    if (!g.retired) vfs_lock(v,g.mask);
    v->default_drive=g.default_drive;
    vfs_tag_locked(v,g.op); vfs_site_locked(v,g.site);
    v->transaction_users[drive]--;
    if (replacement_id) { replacement_id->writer=false; vfs_identity_put(v,replacement_id); }
    if (identity) { identity->writer=false; vfs_identity_put(v,identity); }
    if (node) node->pins--;
    for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) if (pinned[i]) {
        v->descriptions[i].io_active=false;
        int close_error=release_description(v,&v->descriptions[i]);
        if (!e) e=close_error;
    }
    fs_event_wake(&v->volume_changed);
    return e;
}
static uint32_t root(struct fat_volume *v) { return v->type==32 ? v->root : 0; }
/* f1-89: lookups run unguarded and report EAGAIN when any publication
 * overlapped them. Every earlier component may then be stale (a directory
 * can be removed and its cluster reused), so the whole path restarts from
 * the root. The final attempt runs each lookup under the short guard: the
 * earlier attempts warmed the cache and no publication can intervene. */
#define RESOLVE_ATTEMPTS 3u
static int resolve_once(struct vfs_table *t, const char *path, struct resolved *r, bool guarded) {
    char full[FS_PATH_BYTES]; int e=normalize(t,path,&r->drive,full); if (e) return e;
    struct fat_volume *v=t->vfs->volumes[r->drive]; r->parent=root(v); const char *s=full+1;
    r->dep_parent=0; r->dep_index=UINT32_MAX;
    if (!*s) { r->name[0]=0; return 0; }
    while (*s) {
        unsigned n=0; while (*s && *s!='/') { if (n>=sizeof(r->name)-1) return -FS_ENAMETOOLONG; r->name[n++]=*s++; } r->name[n]=0;
        if (!*s) return 0;
        struct fat_entry ent; if ((e=vfs_fat_transaction_locked(t->vfs,v,(struct fat_transaction_args){.op=FAT_TX_LOOKUP,.directory=r->parent,.name=r->name,.entry=&ent,.guarded=guarded,
            .depend=true,.depend_parent=r->dep_parent,.depend_index=r->dep_index}))) return e;
        if (!(ent.attr&FAT_ATTR_DIR)) return -FS_ENOTDIR;
        r->parent=ent.first; r->dep_parent=ent.parent; r->dep_index=ent.index; s++;
    }
    return -FS_EINVAL;
}
static int lookup_once(struct vfs_table *t, const char *path, struct resolved *r, struct fat_entry *ent, bool guarded) {
    int e=resolve_once(t,path,r,guarded); if (e) return e;
    struct fat_volume *v=t->vfs->volumes[r->drive];
    if (!r->name[0]) { memset(ent,0,sizeof(*ent)); ent->first=root(v); ent->attr=FAT_ATTR_DIR; ent->index=UINT32_MAX; ent->name[0]='/'; return 0; }
    return vfs_fat_transaction_locked(t->vfs,v,(struct fat_transaction_args){.op=FAT_TX_LOOKUP,.directory=r->parent,.name=r->name,.entry=ent,.guarded=guarded,
        .depend=true,.depend_parent=r->dep_parent,.depend_index=r->dep_index});
}
static void resolve_restart(struct vfs *v,unsigned attempt) {
    v->lookup_restarts++; if (attempt+2==RESOLVE_ATTEMPTS) v->lookup_guarded++;
}
static __attribute__((noinline)) int resolve(struct vfs_table *t, const char *path, struct resolved *r) {
    int e=-FS_EAGAIN;
    for (unsigned i=0;i<RESOLVE_ATTEMPTS && e==-FS_EAGAIN;i++) {
        if (i) resolve_restart(t->vfs,i-1);
        e=resolve_once(t,path,r,i+1==RESOLVE_ATTEMPTS);
    }
    return e;
}
static __attribute__((noinline)) int lookup(struct vfs_table *t, const char *path, struct resolved *r, struct fat_entry *ent) {
    int e=-FS_EAGAIN;
    for (unsigned i=0;i<RESOLVE_ATTEMPTS && e==-FS_EAGAIN;i++) {
        if (i) resolve_restart(t->vfs,i-1);
        e=lookup_once(t,path,r,ent,i+1==RESOLVE_ATTEMPTS);
    }
    return e;
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
    struct vfs *v=t->vfs; uint32_t turn=0;
    int admission=flags&(VFS_CREATE|VFS_TRUNCATE) ? path_lock_writer(t,path,0,0,&turn) : path_lock(t,path,0,0);
    if (admission) return admission; vfs_tag_locked(t->vfs,flags&VFS_TRUNCATE ? "truncate" : flags&VFS_CREATE ? "create" : "open");
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
        /* Unguarded create phases expose this slot: match no real entry. */
        node->entry.parent=node->entry.index=UINT32_MAX;
    }
    if (create && (attr&FAT_ATTR_DIR)) { e=-FS_EISDIR; goto out; }
    node->refs++; d->refs=1; d->node=node; t->handles[h]=d;
    d->access=flags&3; d->deny=(unsigned)deny; d->dos_mode=mode; d->owner=t->owner; d->owner_type=t->owner_type;
    if (node->refs==1) { node->volume=vol; node->drive=r.drive; node->generation=v->generation[r.drive]; }
    if (create) {
        if ((e=vfs_fat_transaction_locked(v,vol,(struct fat_transaction_args){.op=FAT_TX_CREATE,.directory=r.parent,.name=r.name,.attr=attr,.entry=&node->entry}))) goto undo;
        ent=node->entry;
    }
    if (node->refs==1) { node->volume=vol; node->drive=r.drive; node->generation=v->generation[r.drive]; node->entry=ent; node->log_capacity=0; }
    if (!node->identity && (e=vfs_identity_get(v,vol,&node->entry,&node->identity))) goto undo;
    if (flags&VFS_TRUNCATE) {
        struct fat_entry before=node->entry;
        e=table_range_check(t,node,d,0,before.size,true); if (e) goto undo;
        e=vfs_fat_transaction_locked(v,vol,(struct fat_transaction_args){.op=FAT_TX_TRUNCATE,.entry=&node->entry});
        if (px_legacy_changed) px_legacy_changed(v,vol,&before,&node->entry,false);
        if (e) goto undo;
    }
    d->node=node; d->position=0; d->cursor=(struct fat_cursor){0}; d->access=flags&3; d->deny=(unsigned)deny; t->handles[h]=d; d->dos_mode=mode; d->owner=t->owner; d->owner_type=t->owner_type;
    if (status) *status=create ? 2 : flags&VFS_TRUNCATE ? 3 : 1; e=h;
    goto out;
undo: t->handles[h]=0; if (!--node->refs) { vfs_identity_put(v,node->identity); memset(node,0,sizeof(*node)); } memset(d,0,sizeof(*d));
out: writer_unlock(v,turn); return e;
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
    char holder=first_drive((v->reserved|v->mutating)&mask);
    struct vfs_hold_info owner=holder>='A' && holder<='Z' ? v->holds[holder-'A'] : (struct vfs_hold_info){0};
    struct fs_task_identity task=fs_request_identity(request);
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) v->diagnostic_waiters[d]++;
    struct vfs_pending_waits pending=waits_detach(v);
    if (!request) fs_event_wait(&v->volume_changed,&v->lock);
    else { fs_lock_drop(&v->lock); request->pause(request->arg); (void)fs_lock_retake(&v->lock,VFS_WAIT_NAMESPACE); }
    v->diagnostic_pending=pending;
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) v->diagnostic_waiters[d]--;
    int error=vfs_request_check(request);
    request_wait(request,begin,mask,VFS_WAIT_VOLUME,error);
    if (error || !request) wait_capture(&v->diagnostic_pending,start,first_drive(mask),holder,error==-VFS_ETIMEOUT ? "volume:timeout" : error ? "volume:cancelled" : "volume",&task,&owner);
    return error;
}
static int handle_lock(struct vfs_table *t,int h) {
    uint32_t start=fs_now_ms(); uint64_t begin=t->request ? t->request->now(t->request->arg) : 0;
    for (;;) {
        int error=vfs_admit(t->vfs,0,t->request); if (error) return error;
        struct vfs_description *desc=h>=0 && h<(int)VFS_HANDLES ? t->handles[h] : 0;
        if (desc && (desc->io_active || desc->node->pins || (desc->node->identity && desc->node->identity->writer))) {
            error=file_wait_locked(t->vfs,handle_mask(t,h),t->request,start,begin);
            vfs_unlock(t->vfs); if (error) return error; continue;
        }
        uint32_t mask=handle_mask(t,h); error=reserve_request(t->vfs,mask,t->request);
        if (error) { vfs_unlock(t->vfs); return error; }
        desc=h>=0 && h<(int)VFS_HANDLES ? t->handles[h] : 0;
        if (handle_mask(t,h)==mask && (!desc || (!desc->io_active && !desc->node->pins &&
            (!desc->node->identity || !desc->node->identity->writer)))) return 0;
        vfs_unlock(t->vfs); /* slot was replaced while admission slept */
    }
}
static int handle_lock_writer(struct vfs_table *t,int h,uint32_t *turn) {
    *turn=0;
    for (;;) {
        int e=vfs_admit(t->vfs,0,t->request); if (e) return e;
        uint32_t want=turn_drives(t->vfs,handle_mask(t,h)), taken=0;
        struct fs_request_scope scope; fs_request_enter(&scope,t->request ? t->request : fs_request_current());
        if (want) e=turn_queue_locked(t->vfs,want);
        fs_request_leave(&scope);
        if (!e) taken=want;
        vfs_unlock(t->vfs); if (e) return e;
        e=handle_lock(t,h);
        if (e) { turn_release(t->vfs,taken); return e; }
        if (!(turn_drives(t->vfs,t->vfs->active)&~taken)) { *turn=taken; return 0; }
        writer_unlock(t->vfs,taken);
    }
}
static struct vfs_description *get(struct vfs_table *t, int h) { return h>=0 && h<(int)VFS_HANDLES ? t->handles[h] : 0; }
/* Only the operations below leave every other open regular file's chain
 * unchanged. Unknown mutations remain excluded. Compare common identities,
 * rather than personality-specific node addresses. */
static bool reader_admitted(struct vfs *v,unsigned drive,struct vfs_file_identity *id) {
    uint32_t mask=vfs_drive_mask(drive);
    if (id && id->writer) return false;
    for (unsigned i=0;i<VFS_NODES;i++) if (v->nodes[i].pins && v->nodes[i].identity==id) return false;
    if (!((v->reserved|v->mutating)&mask)) {
        const struct vfs_request *r=fs_request_current();
        /* A decoder deadline may pass a queued normal metadata ticket while
         * another reader drains. It never passes an active mutation of this
         * file. Ordinary readers still close the gate for FIFO writers. */
        return v->next[drive]==v->serving[drive] || (r && r->urgent_reader);
    }
    if (v->io_file[drive]) return v->io_file[drive]->identity!=id;
    const char *op=v->holds[drive].op;
    static const char *const immutable[]={"commit","mkdir","read","open","lookup","readdir","find","stat"};
    for (unsigned i=0;i<sizeof(immutable)/sizeof(*immutable);i++)
        if (!strncmp(op,immutable[i],12)) return true;
    return false;
}
static struct fat_volume read_view(const struct fat_volume *v) {
    return (struct fat_volume){.cache=v->cache,.dev=v->dev,.start=v->start,.sectors=v->sectors,
        .reserved=v->reserved,.fat_sectors=v->fat_sectors,.root_sector=v->root_sector,
        .data_sector=v->data_sector,.clusters=v->clusters,.root=v->root,
        .root_entries=v->root_entries,.type=v->type,.spc=v->spc,.fats=v->fats,
        .media=v->media,.active_fat=v->active_fat,.mounted=true};
}
int vfs_read_identity_locked(struct vfs *v,struct vfs_file_identity *id,struct fat_volume *vol,
    const struct fat_entry *entry,struct fat_cursor *cursor,bool *busy,uint64_t *pos,const uint64_t *current,
    bool positioned,int (*validate)(void *,uint64_t,size_t),void *arg,void *buf,size_t n,size_t *done) {
    uint32_t saved=v->active,mask=0; unsigned drive=26;
    for (unsigned d=0;d<26;d++) if (v->volumes[d]==vol) { drive=d; mask=1u<<d; break; }
    if (!mask || !vfs_identity_live(v,id)) return -FS_EIO;
    uint32_t start=fs_now_ms(); const struct vfs_request *r=fs_request_current();
    uint64_t begin=r ? r->now(r->arg) : 0;
    /* Existing multi-path kernel consumers may have borrowed other names
     * under their complete reservation set. Preserve that contract. Native
     * read syscalls enter with namespace only and use the per-file phase. */
    if (saved) {
        if (!(saved&mask)) return -FS_EBUSY; /* caller did not reserve its complete borrowed set */
        *busy=true;
        int e=VFS_IO(v,fat_read_cursor(vol,entry,cursor,*pos,buf,n,done));
        *busy=false; fs_event_wake(&v->volume_changed); return e;
    }
    while (*busy || !reader_admitted(v,drive,id)) {
        int e=file_wait_locked(v,mask,r,start,begin);
        if (e) { if (saved) vfs_reserve_locked(v,saved); return e; }
    }
    if (!vfs_identity_live(v,id) || v->volumes[drive]!=vol) {
        if (saved) vfs_reserve_locked(v,saved); return -FS_EIO;
    }
    struct fat_volume view=read_view(vol); struct fat_entry snapshot=*entry;
    if (!positioned) *pos=*current; /* shared-description offset after admission */
    int permission=validate ? validate(arg,*pos,n) : 0;
    if (permission) { if (saved) vfs_reserve_locked(v,saved); return permission; }
    *busy=true; v->readers[drive]++; vfs_unlock(v);
    int e=fat_read_cursor(&view,&snapshot,cursor,*pos,buf,n,done);
    vfs_lock(v,0); *busy=false; v->readers[drive]--; fs_event_wake(&v->volume_changed);
    /* Do not erase corruption observed in the private reader workspace. */
    if (view.readonly_reason) {
        vfs_reserve_locked(v,mask);
        if (v->volumes[drive]==vol && vfs_identity_live(v,id)) {
            vol->ro_reasons|=view.ro_reasons;
            fat_revoke_write(vol,view.readonly_reason,view.readonly_cluster);
        }
    }
    if (saved) vfs_reserve_locked(v,saved);
    return e;
}
int vfs_handle_stat(struct vfs_table *t,int h,struct fat_entry *entry,uint64_t *position) {
    int e=handle_lock(t,h); if (e) return e; vfs_tag_locked(t->vfs,"stat");
    struct vfs_description *d=get(t,h);
    e=!d ? -FS_EBADF : (d->node->generation!=t->vfs->generation[d->node->drive] || !vfs_identity_live(t->vfs,d->node->identity)) ? -FS_EIO : 0;
    if (!e) { if (entry) *entry=d->node->entry; if (position) *position=d->position; }
    vfs_unlock(t->vfs); return e;
}
int vfs_handle_version(struct vfs_table *t,int h,struct vfs_file_version *out) {
    if (!out) return -FS_EINVAL;
    int e=handle_lock(t,h); if (e) return e; vfs_tag_locked(t->vfs,"stat");
    struct vfs *v=t->vfs; struct vfs_description *d=get(t,h);
    e=!d ? -FS_EBADF : (d->node->generation!=v->generation[d->node->drive] || !vfs_identity_live(v,d->node->identity)) ? -FS_EIO :
        d->node->ro || !d->node->volume ? -FS_EOPNOTSUPP : 0;
    if (!e) {
        const struct vfs_node *n=d->node;
        memset(out,0,sizeof(*out));
        fs_lock_take(&v->identity_lock);
        out->mount=identity_generation(v,n->volume);
        out->write_generation=change_generation(v,n->volume,n->entry.parent,n->entry.index);
        fs_lock_drop(&v->identity_lock);
        out->drive=n->drive; out->drive_generation=v->generation[n->drive];
        out->parent=n->entry.parent; out->index=n->entry.index; out->first=n->entry.first; out->size=n->entry.size;
        out->write_date=n->entry.times.write_date; out->write_time=n->entry.times.write_time;
    }
    vfs_unlock(v); return e;
}
int vfs_handle_times(struct vfs_table *t,int h,uint16_t date,uint16_t time) {
    uint32_t turn;
    int e=handle_lock_writer(t,h,&turn); if (e) return e;
    struct vfs_description *d=get(t,h);
    e=!d ? -FS_EBADF : (d->node->generation!=t->vfs->generation[d->node->drive] ||
        !vfs_identity_live(t->vfs,d->node->identity)) ? -FS_EIO : d->node->ro ? -FS_EROFS : 0;
    if (!e) {
        struct fat_entry before=d->node->entry;
        struct fat_times times=before.times; times.write_date=date; times.write_time=time;
        e=vfs_fat_transaction_locked(t->vfs,d->node->volume,(struct fat_transaction_args){.op=FAT_TX_SET_METADATA,
            .entry=&d->node->entry,.attr=before.attr,.times=&times});
        if (!e && px_legacy_changed) px_legacy_changed(t->vfs,d->node->volume,&before,&d->node->entry,false);
    }
    writer_unlock(t->vfs,turn); return e;
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
        struct vfs_description *from=get(src,h), *to=get(dst,target);
        if ((from && from->io_active) || (to && to->io_active)) {
            int e=vfs_file_wait_locked(src->vfs,from && from->io_active ? &from->io_active : &to->io_active);
            vfs_unlock(src->vfs); if (e) return e; continue;
        }
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
        int admission=vfs_admit(v,0,t->request); if (admission) return admission; vfs_tag_locked(t->vfs,"read");
        struct vfs_description *d=get(t,h);
        if (!d || d->node->ro || !(d->access&VFS_READ)) { vfs_unlock(v); break; }
        struct vfs_node *node=d->node; unsigned drive=node->drive;
        if (drive>=26 || node->generation!=v->generation[drive] || v->volumes[drive]!=node->volume || !vfs_identity_live(v,node->identity)) {
            vfs_unlock(v); return -FS_EIO;
        }
        uint32_t mask=vfs_drive_mask(drive);
        bool admitted=reader_admitted(v,drive,node->identity);
        if (d->io_active || !admitted) {
            int error=file_wait_locked(v,mask,t->request,start,begin);
            vfs_unlock(v); if (error) return error; continue;
        }
        int locked=table_range_check(t,node,d,d->position,n,false);
        if (locked) { vfs_unlock(v); return locked; }
        struct fat_volume *vol=node->volume;
        uint32_t media_generation=vol->dev->media_generation;
        struct fat_volume view=read_view(vol);
        d->io_active=true; d->refs++; v->readers[drive]++; uint64_t position=d->position;
        struct fat_entry entry=node->entry;
        vfs_unlock(v);
        int e=fat_read_stream(&view,&entry,position,buf,n,done,&d->stream,&d->cursor);
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
    int admission=handle_lock(t,h); if (admission) return admission; vfs_tag_locked(t->vfs,"read"); struct vfs_description *d=get(t,h); int e;
    if (!d) e=-FS_EBADF;
    else if (d->node->generation!=t->vfs->generation[d->node->drive] || !vfs_identity_live(t->vfs,d->node->identity)) e=-FS_EIO;
    else if (!(d->access&VFS_READ)) e=-FS_EACCES;
    else if ((e=table_range_check(t,d->node,d,d->position,n,false))) { }
    else if (d->node->ro) { e=ro_handle(t->vfs,d->node); if (!e) e=VFS_IO(t->vfs,fs_ro_read(d->node->ro,d->node->ro_id,d->position,buf,n,done)); if (e) ro_handle(t->vfs,d->node); d->position+=*done; }
    else { e=VFS_IO(t->vfs,fat_read_cursor(d->node->volume,&d->node->entry,&d->cursor,d->position,buf,n,done)); d->position+=*done; }
    vfs_unlock(t->vfs); return e;
}
int vfs_write(struct vfs_table *t, int h, const void *buf, size_t n, size_t *done) {
    uint32_t turn;
    if (!done) return -FS_EINVAL; *done=0;
    int admission=handle_lock_writer(t,h,&turn); if (admission) return admission; vfs_tag_locked(t->vfs,"write"); struct vfs_description *d=get(t,h); int e;
    if (!d) e=-FS_EBADF;
    else if (d->node->generation!=t->vfs->generation[d->node->drive] || !vfs_identity_live(t->vfs,d->node->identity)) e=-FS_EIO;
    else if (d->node->ro) { e=ro_handle(t->vfs,d->node); if (!e) e=-FS_EROFS; }
    else if (!(d->access&VFS_WRITE)) e=-FS_EACCES;
    else if (d->node->volume->readonly) e=-FS_EROFS;
    else if (!buf && n) e=-FS_EINVAL;
    else if (d->position>UINT32_MAX || n>UINT32_MAX-d->position) e=-FS_EFBIG;
    else if ((e=table_range_check(t,d->node,d,d->position,n,true))) { }
    else if (d->node->log_capacity) e=-FS_EBUSY;
    else {
        cache_stream_cancel(&d->stream);
        struct fat_entry before=d->node->entry;
        e=0;
        size_t limit=(size_t)d->node->volume->spc*512*FAT_TRANSACTION_CLUSTERS;
        if (limit>131072) limit=131072;
        while (n && d->position>=(uint64_t)d->node->entry.size+limit && !e) {
            uint64_t end=(uint64_t)d->node->entry.size+32768;
            if (end>d->position) end=d->position;
            e=vfs_fat_transaction_locked(t->vfs,d->node->volume,(struct fat_transaction_args){.op=FAT_TX_EXTEND,
                .entry=&d->node->entry,.capacity=(uint32_t)end});
        }
        while (*done<n && !e) {
            size_t step=limit, written=0;
            if (d->position>d->node->entry.size) step-=d->position-d->node->entry.size;
            if (step>n-*done) step=n-*done;
            e=vfs_fat_transaction_locked(t->vfs,d->node->volume,(struct fat_transaction_args){.op=FAT_TX_WRITE,
                .entry=&d->node->entry,.cursor=&d->cursor,.position=d->position,.data=(const uint8_t *)buf+*done,.size=step,.done=&written});
            d->position+=written; *done+=written;
        }
        if (px_legacy_changed) px_legacy_changed(t->vfs,d->node->volume,&before,&d->node->entry,*done != 0);
    }
    writer_unlock(t->vfs,turn); return e;
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
    uint32_t turn;
    int admission=handle_lock_writer(t,h,&turn); if (admission) return admission; vfs_tag_locked(t->vfs,"truncate"); struct vfs_description *d=get(t,h);
    int e=!d ? -FS_EBADF : (d->node->generation!=t->vfs->generation[d->node->drive] || !vfs_identity_live(t->vfs,d->node->identity)) ? -FS_EIO : d->node->ro ? ro_handle(t->vfs,d->node) : !(d->access&VFS_WRITE) ? -FS_EACCES : 0;
    if (d && d->node->ro && !e) e=-FS_EROFS;
    if (d && d->node->log_capacity && !e) e=-FS_EBUSY;
    if (!e) {
        uint64_t old=d->node->entry.size,start=size<old ? size : old;
        e=table_range_check(t,d->node,d,start,size<old ? old-size : size-old,true);
    }
    if (!e) {
        struct fat_entry before=d->node->entry;
        e=vfs_fat_transaction_locked(t->vfs,d->node->volume,(struct fat_transaction_args){.op=FAT_TX_TRUNCATE,.entry=&d->node->entry,.position=size});
        if (px_legacy_changed) px_legacy_changed(t->vfs,d->node->volume,&before,&d->node->entry,false);
    }
    writer_unlock(t->vfs,turn); return e;
}
int vfs_commit(struct vfs_table *t, int h) {
    int admission=handle_lock(t,h); if (admission) return admission; vfs_tag_locked(t->vfs,"commit"); struct vfs_description *d=get(t,h);
    int e=!d ? -FS_EBADF : (d->node->generation!=t->vfs->generation[d->node->drive] || !vfs_identity_live(t->vfs,d->node->identity)) ? -FS_EIO : d->node->ro ? ro_handle(t->vfs,d->node) : vfs_fat_transaction_locked(t->vfs,d->node->volume,(struct fat_transaction_args){.op=FAT_TX_COMMIT}); vfs_unlock(t->vfs); return e;
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
    int e=vfs_admit(t->vfs,vfs_drive_mask(drive),t->request); if (e) return e; vfs_tag_locked(t->vfs,"commit");
    struct fat_volume *v=t->vfs->volumes[drive];
    if (v && !v->readonly) e=vfs_fat_transaction_locked(t->vfs,v,(struct fat_transaction_args){.op=FAT_TX_COMMIT});
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
    uint32_t turn;
    int admission=handle_lock_writer(t,h,&turn); if (admission) return admission; vfs_tag_locked(t->vfs,"log_reserve"); vfs_site_locked(t->vfs,"writer");
    struct vfs_description *d; int e=log_handle(t,h,&d);
    if (!e && (!capacity || (capacity&511) || d->node->log_capacity)) e=-FS_EINVAL;
    if (!e) {
        struct fat_entry before=d->node->entry;
        e=vfs_fat_transaction_locked(t->vfs,d->node->volume,(struct fat_transaction_args){.op=FAT_TX_TRUNCATE,.entry=&d->node->entry});
        if (!e) e=vfs_fat_transaction_locked(t->vfs,d->node->volume,(struct fat_transaction_args){.op=FAT_TX_TRUNCATE,.entry=&d->node->entry,.position=capacity});
        if (!e) {
            d->node->log_capacity=capacity; d->node->entry.size=0; d->position=0;
            e=vfs_fat_transaction_locked(t->vfs,d->node->volume,(struct fat_transaction_args){.op=FAT_TX_PUBLISH,.entry=&d->node->entry});
        }
        if (px_legacy_changed) px_legacy_changed(t->vfs,d->node->volume,&before,&d->node->entry,false);
    }
    writer_unlock(t->vfs,turn); return e;
}
int vfs_log_write(struct vfs_table *t, int h, const void *buf, size_t n, uint32_t size, size_t *done) {
    if (!done) return -FS_EINVAL; *done=0;
    int admission=handle_lock(t,h); if (admission) return admission; vfs_tag_locked(t->vfs,"log_write"); vfs_site_locked(t->vfs,"writer");
    struct vfs_description *d; int e=log_handle(t,h,&d);
    if (!e && (!d->node->log_capacity || size>d->node->log_capacity)) e=-FS_EINVAL;
    if (!e) {
        struct fat_entry before=d->node->entry;
        e=vfs_fat_transaction_locked(t->vfs,d->node->volume,(struct fat_transaction_args){.op=FAT_TX_LOG_WRITE,
            .entry=&d->node->entry,.cursor=&d->cursor,.capacity=d->node->log_capacity,.position=d->position,.data=buf,.size=n,.done=done});
        d->position+=*done;
        if (!e) d->node->entry.size=size;
        if (px_legacy_changed) px_legacy_changed(t->vfs,d->node->volume,&before,&d->node->entry,*done != 0);
    }
    vfs_unlock(t->vfs); return e;
}
int vfs_log_publish(struct vfs_table *t, int h, bool finish) {
    uint32_t turn;
    int admission=handle_lock_writer(t,h,&turn); if (admission) return admission; vfs_tag_locked(t->vfs,"commit");
    vfs_site_locked(t->vfs,"writer");
    struct vfs_description *d; int e=log_handle(t,h,&d);
    if (!e && !d->node->log_capacity) e=-FS_EINVAL;
    if (!e) {
        struct fat_entry before=d->node->entry;
        e=vfs_fat_transaction_locked(t->vfs,d->node->volume,(struct fat_transaction_args){.op=FAT_TX_PUBLISH,.entry=&d->node->entry,.flag=finish});
        if (!e && finish) d->node->log_capacity=0;
        if (px_legacy_changed) px_legacy_changed(t->vfs,d->node->volume,&before,&d->node->entry,false);
    }
    writer_unlock(t->vfs,turn); return e;
}
/* f0-49 pagefile. Only the pager's exclusive VFS_DENY_ALL read/write handle
 * qualifies; each call holds one volume reservation for one bounded step.
 * Data is never read through the cache and live clusters never move. */
static int pagefile_handle(struct vfs_table *t, int h, struct vfs_description **out) {
    struct vfs_description *d=get(t,h); *out=d;
    if (!d) return -FS_EBADF;
    if (d->node->ro || !d->node->volume) return -FS_EROFS;
    if ((d->access&(VFS_READ|VFS_WRITE))!=(VFS_READ|VFS_WRITE) || d->deny!=VFS_DENY_ALL ||
        d->node->log_capacity) return -FS_EACCES;
    if (d->node->generation!=t->vfs->generation[d->node->drive] || !vfs_identity_live(t->vfs,d->node->identity)) return -FS_EIO;
    return 0;
}
int vfs_pagefile_extend(struct vfs_table *t, int h, uint32_t size) {
    uint32_t turn;
    int admission=handle_lock_writer(t,h,&turn); if (admission) return admission; vfs_tag_locked(t->vfs,"truncate");
    struct vfs_description *d; int e=pagefile_handle(t,h,&d);
    if (!e) {
        struct fat_entry before=d->node->entry;
        t->vfs->io_file[d->node->drive]=d->node;
        if (size<d->node->entry.size) e=-FS_EINVAL;
        while (!e && size>d->node->entry.size) {
            uint64_t end=(uint64_t)d->node->entry.size+(uint32_t)d->node->volume->spc*512*FAT_TRANSACTION_CLUSTERS;
            if (end>size) end=size;
            e=vfs_fat_transaction_locked(t->vfs,d->node->volume,(struct fat_transaction_args){.op=FAT_TX_RESERVE,
                .entry=&d->node->entry,.capacity=(uint32_t)end});
        }
        t->vfs->io_file[d->node->drive]=0;
        if (px_legacy_changed) px_legacy_changed(t->vfs,d->node->volume,&before,&d->node->entry,!e);
    }
    writer_unlock(t->vfs,turn); return e;
}
int vfs_pagefile_trim(struct vfs_table *t, int h, uint32_t size) {
    uint32_t turn;
    int admission=handle_lock_writer(t,h,&turn); if (admission) return admission; vfs_tag_locked(t->vfs,"truncate");
    struct vfs_description *d; int e=pagefile_handle(t,h,&d);
    if (!e && size>d->node->entry.size) e=-FS_EINVAL;
    if (!e && size<d->node->entry.size) {
        /* Shorter length first, then the trailing link and chain release:
         * a cut leaves owned excess (reused by the next extension). */
        struct fat_entry before=d->node->entry;
        d->node->entry.size=size;
        e=vfs_fat_transaction_locked(t->vfs,d->node->volume,(struct fat_transaction_args){.op=FAT_TX_PUBLISH,.entry=&d->node->entry,.flag=true});
        if (px_legacy_changed) px_legacy_changed(t->vfs,d->node->volume,&before,&d->node->entry,false);
    }
    writer_unlock(t->vfs,turn); return e;
}
static bool pagefile_end(const struct fat_volume *v, uint32_t n) {
    uint32_t mask=v->type==12 ? 0xfffu : v->type==16 ? 0xffffu : 0x0fffffffu;
    return (n&mask)>=mask-7;
}
static int pagefile_walk(struct fat_volume *v, const struct fat_entry *entry, struct vfs_pagefile_cursor *c,
                         uint32_t index, uint32_t count, struct vfs_pagefile_run *runs, unsigned max,
                         unsigned *used, uint32_t *mapped) {
    /* f0-49b: same rules as the f3-51c file cursors. A pair taken under an
     * older chain generation may name a cluster a trim released (and another
     * file then took): restart from the first cluster instead. */
    if (c->first!=entry->first || c->generation!=entry->chain_generation || c->index>index || !c->cluster) {
        c->generation=entry->chain_generation; c->first=entry->first; c->cluster=entry->first; c->index=0;
    }
    uint32_t work=0;
    while (c->index<index) {
        if (work++>=PAGEFILE_MAP_WORK) return 0; /* bounded step, resumable */
        uint32_t next; int e=fat_get_cluster(v,c->cluster,&next); if (e) return e;
        if (pagefile_end(v,next) || next<2 || next>=v->clusters+2) return -FS_EIO;
        c->cluster=next; c->index++;
    }
    while (*mapped<count && work++<PAGEFILE_MAP_WORK) {
        if (c->cluster<2 || c->cluster>=v->clusters+2) return -FS_EIO;
        uint64_t lba=v->start+v->data_sector+(uint64_t)(c->cluster-2)*v->spc;
        struct vfs_pagefile_run *r=*used ? &runs[*used-1] : 0;
        if (r && r->lba+r->sectors==lba && r->sectors<=UINT32_MAX-v->spc) r->sectors+=v->spc;
        else {
            if (*used==max) return 0; /* caller supplies another run array */
            runs[(*used)++]=(struct vfs_pagefile_run){.lba=lba,.sectors=v->spc};
        }
        (*mapped)++;
        uint32_t next; int e=fat_get_cluster(v,c->cluster,&next); if (e) return e;
        if (pagefile_end(v,next)) { c->index++; c->cluster=0; return *mapped<count ? -FS_EIO : 0; }
        c->cluster=next; c->index++;
    }
    return 0;
}
int vfs_pagefile_map(struct vfs_table *t, int h, struct vfs_pagefile_cursor *c, uint32_t index,
                     uint32_t count, struct vfs_pagefile_run *runs, unsigned max, unsigned *used,
                     uint32_t *mapped, uint32_t *cluster_bytes) {
    if (!c || !runs || !max || !used || !mapped || *used>max) return -FS_EINVAL;
    *mapped=0;
    int admission=handle_lock(t,h); if (admission) return admission;
    struct vfs_description *d; int e=pagefile_handle(t,h,&d);
    if (!e) {
        struct fat_volume *v=d->node->volume;
        if (cluster_bytes) *cluster_bytes=(uint32_t)v->spc*512;
        uint32_t clusters=d->node->entry.size/((uint32_t)v->spc*512);
        if (!d->node->entry.first || index>clusters || count>clusters-index) e=-FS_EINVAL;
        else e=VFS_IO(t->vfs,pagefile_walk(v,&d->node->entry,c,index,count,runs,max,used,mapped));
    }
    if (e) *c=(struct vfs_pagefile_cursor){0}; /* I/O, corruption or a lost handle */
    vfs_unlock(t->vfs); return e;
}
int vfs_stat(struct vfs_table *t, const char *path, struct fat_entry *out) {
    int ro=ro_target(t,path); if (ro<0) return ro;
    if (ro) {
        int admission=path_lock(t,path,0,0); if (admission) return admission; vfs_tag_locked(t->vfs,"stat"); struct fs_ro_node n; unsigned d;
        int e=ro_path(t,path,&d,&n,0); if (!e && n.size>UINT32_MAX) e=-FS_EFBIG;
        if (!e) ro_entry(&n,out); vfs_unlock(t->vfs); return e;
    }
    int admission=path_lock(t,path,0,0); if (admission) return admission; vfs_tag_locked(t->vfs,"stat"); struct resolved r; int e=lookup(t,path,&r,out);
    if (!e) { struct vfs_node *n=node_find(t->vfs,t->vfs->volumes[r.drive],out); if (n) *out=n->entry; }
    vfs_unlock(t->vfs); return e;
}
static int metadata(struct vfs_table *t, const char *path, int attr, const struct fat_times *times) {
    int ro=ro_target(t,path); if (ro<0) return ro;
    if (ro) return -FS_EROFS;
    uint32_t turn; int admission=path_lock_writer(t,path,0,0,&turn); if (admission) return admission; vfs_tag_locked(t->vfs,"attrib"); struct resolved r; struct fat_entry ent; int e=lookup(t,path,&r,&ent);
    if (!e && ent.index==UINT32_MAX) e=-FS_EACCES;
    if (!e) {
        struct fat_volume *vol=t->vfs->volumes[r.drive]; struct vfs_node *n=node_find(t->vfs,vol,&ent);
        if (n) ent=n->entry;
        struct fat_entry before=ent;
        e=vfs_fat_transaction_locked(t->vfs,vol,(struct fat_transaction_args){.op=FAT_TX_SET_METADATA,
            .entry=&ent,.attr=attr<0 ? ent.attr : (uint8_t)attr,.times=times});
        if (!e && px_legacy_changed) px_legacy_changed(t->vfs,vol,&before,&ent,false);
        if (!e && n) n->entry=ent;
    }
    writer_unlock(t->vfs,turn); return e;
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
        int admission=path_lock(t,p,0,0); if (admission) return admission; vfs_tag_locked(t->vfs,"find"); char parent[FS_PATH_BYTES];
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

    int admission=path_lock(t,p,0,0); if (admission) return admission; vfs_tag_locked(t->vfs,"find"); struct resolved r; int e=resolve(t,p,&r);
    if (!e && !r.name[0]) e=-FS_EINVAL;
    if (!e) { memset(f,0,sizeof(*f)); f->drive=r.drive; f->directory=r.parent; f->generation=t->vfs->generation[r.drive]; f->attr_mask=mask; memcpy(f->pattern,r.name,strlen(r.name)+1); e=find_next(t,f,ent); }
    vfs_unlock(t->vfs); return e;
}
int vfs_find_next(struct vfs_table *t, struct vfs_find *f, struct fat_entry *ent) {
    int admission=vfs_admit(t->vfs,vfs_drive_mask(f->drive),t->request); if (admission) return admission; vfs_tag_locked(t->vfs,"find");
    int e=find_next(t,f,ent); vfs_unlock(t->vfs); return e;
}
/* Does ancestor contain directory? Resolve on-disk parents, so aliases cannot
 * evade cycle/cwd checks. Root . . references are normalized to root(). */
static __attribute__((noinline)) int contains(struct vfs *space, struct fat_volume *v, uint32_t ancestor, uint32_t dir) {
    for (uint32_t i=0;i<v->clusters;i++) {
        if (dir==ancestor) return 1; if (!dir || dir==root(v)) return 0;
        struct fat_entry parent; int e=vfs_fat_transaction_locked(space,v,(struct fat_transaction_args){.op=FAT_TX_LOOKUP,.directory=dir,.name="..",.entry=&parent}); if (e) return e;
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
/* Separate frame: the unlink publication clears the caller's index, so the
 * announced identity is a copy (keeps vfs_log_rotate under 4 KiB on i686). */
static __attribute__((noinline)) int remove_announced(struct vfs *v,struct fat_volume *vol,struct fat_entry *entry,bool dir) {
    struct fat_entry before=*entry;
    int e=vfs_fat_transaction_locked(v,vol,(struct fat_transaction_args){.op=FAT_TX_REMOVE,.entry=entry,.flag=dir});
    if (!e && px_legacy_changed) px_legacy_changed(v,vol,&before,0,false);
    return e;
}
static int remove_path(struct vfs_table *t, const char *path, bool dir) {
    int ro=ro_target(t,path); if (ro<0) return ro;
    if (ro) return -FS_EROFS;
    uint32_t turn; int admission=path_lock_writer(t,path,0,0,&turn); if (admission) return admission; vfs_tag_locked(t->vfs,"unlink"); struct resolved r; struct fat_entry ent; int e=lookup(t,path,&r,&ent);
    if (!e && !r.name[0]) e=-FS_EACCES;
    if (!e) {
        struct fat_volume *v=t->vfs->volumes[r.drive];
        if (!(e=busy(t->vfs,v,&ent))) e=remove_announced(t->vfs,v,&ent,dir);
    }
    writer_unlock(t->vfs,turn); return e;
}
int vfs_log_rotate(struct vfs_table *t, const char *current, const char *previous,
                   const char *oldest, uint32_t capacity) {
    const char *paths[3]={current,previous,oldest};
    struct fat_entry entries[3];
    struct resolved r;
    bool present[3]={false};
    unsigned drive=26; uint32_t parent=0;
    struct vfs *v=t->vfs;
    uint32_t turn; int admission=path_lock_writer(t,current,previous,oldest,&turn); if (admission) return admission; vfs_tag_locked(t->vfs,"rename");
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
    if (!e && present[2]) e=remove_announced(v,v->volumes[drive],&entries[2],false);
    for (unsigned i=2;!e && i;i--) if (present[i-1]) {
        struct fat_entry *entry=&entries[i-1];
        entries[2]=*entry; /* oldest is gone; reuse its scratch storage */
        /* Release the entire unpublished tail, even when size is zero.
         * Finishing in place does not allocate a second full-file chain. */
        if (entry->size>capacity) entry->size=capacity;
        e=vfs_fat_transaction_locked(v,v->volumes[drive],(struct fat_transaction_args){.op=FAT_TX_PUBLISH,.entry=entry,.flag=true});
        if (px_legacy_changed) px_legacy_changed(v,v->volumes[drive],&entries[2],entry,false);
        entries[2]=*entry;
        if (!e) e=resolve(t,paths[i],&r);
        if (!e) e=vfs_fat_transaction_locked(v,v->volumes[drive],(struct fat_transaction_args){.op=FAT_TX_RENAME,.entry=entry,.directory=parent,.name=r.name});
        if (!e && px_legacy_changed) px_legacy_changed(v,v->volumes[drive],&entries[2],entry,false);
    }
    writer_unlock(v,turn); return e;
}
int vfs_delete(struct vfs_table *t, const char *p) { return remove_path(t,p,false); }
int vfs_rmdir(struct vfs_table *t, const char *p) { return remove_path(t,p,true); }
int vfs_mkdir(struct vfs_table *t, const char *p, uint8_t attr) {
    int ro=ro_target(t,p); if (ro<0) return ro;
    if (ro) return -FS_EROFS;
    uint32_t turn; int admission=path_lock_writer(t,p,0,0,&turn); if (admission) return admission; vfs_tag_locked(t->vfs,"mkdir"); struct resolved r; int e=resolve(t,p,&r); struct fat_entry ent;
    if (!e && !r.name[0]) e=-FS_EEXIST;
    if (!e) e=vfs_fat_transaction_locked(t->vfs,t->vfs->volumes[r.drive],(struct fat_transaction_args){.op=FAT_TX_CREATE,.directory=r.parent,.name=r.name,.attr=attr|FAT_ATTR_DIR,.entry=&ent});
    writer_unlock(t->vfs,turn); return e;
}
int vfs_rename(struct vfs_table *t, const char *from, const char *to) {
    int ro=ro_target(t,from); if (ro<0) return ro; if (ro) return -FS_EROFS;
    ro=ro_target(t,to); if (ro<0) return ro; if (ro) return -FS_EROFS;
    uint32_t turn; int admission=path_lock_writer(t,from,to,0,&turn); if (admission) return admission; vfs_tag_locked(t->vfs,"rename"); struct resolved a,b; struct fat_entry ent; int e=lookup(t,from,&a,&ent);
    if (!e) e=resolve(t,to,&b);
    if (!e && (!a.name[0] || !b.name[0])) e=-FS_EACCES;
    if (!e && a.drive!=b.drive) e=-FS_EXDEV;
    if (!e) {
        struct fat_volume *v=t->vfs->volumes[a.drive]; e=busy(t->vfs,v,&ent);
        if (!e && (ent.attr&FAT_ATTR_DIR)) { e=contains(t->vfs,v,ent.first,b.parent); if (e>0) e=-FS_EINVAL; }
        if (!e) {
            struct fat_entry before=ent;
            e=vfs_fat_transaction_locked(t->vfs,v,(struct fat_transaction_args){.op=FAT_TX_RENAME,.entry=&ent,.directory=b.parent,.name=b.name});
            if (!e && px_legacy_changed) px_legacy_changed(t->vfs,v,&before,&ent,false);
        }
    }
    writer_unlock(t->vfs,turn); return e;
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
