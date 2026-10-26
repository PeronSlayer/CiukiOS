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
    out.attribution=__atomic_load_n(&h->attribution,__ATOMIC_RELAXED);
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
    __atomic_store_n(&v->namespace_owner.attribution,
        (r && r->operation && *r->operation ? VFS_ATTR_OPERATION : 0) |
        (r && r->site && *r->site ? VFS_ATTR_SITE : 0),__ATOMIC_RELAXED);
}
void vfs_tag_locked(struct vfs *v,const char *tag) {
    char op[12]; tag_copy(op,tag);
    uint8_t bit=tag && *tag ? VFS_ATTR_OPERATION : 0;
    for (unsigned d=0;d<26;d++) if (v->active&(1u<<d)) {
        memcpy(v->holds[d].op,op,sizeof(op));
        v->holds[d].attribution=(v->holds[d].attribution & ~VFS_ATTR_OPERATION) | bit;
    }
    for (unsigned i=0;i<12;i++) __atomic_store_n(&v->namespace_owner.op[i],op[i],__ATOMIC_RELAXED);
    uint8_t attribution=(__atomic_load_n(&v->namespace_owner.attribution,__ATOMIC_RELAXED) & ~VFS_ATTR_OPERATION) | bit;
    __atomic_store_n(&v->namespace_owner.attribution,attribution,__ATOMIC_RELAXED);
}
void vfs_site_locked(struct vfs *v,const char *site) {
    char name[12]; tag_copy(name,site);
    uint8_t bit=site && *site ? VFS_ATTR_SITE : 0;
    for (unsigned d=0;d<26;d++) if (v->active&(1u<<d)) {
        memcpy(v->holds[d].site,name,sizeof(name));
        v->holds[d].attribution=(v->holds[d].attribution & ~VFS_ATTR_SITE) | bit;
    }
    for (unsigned i=0;i<12;i++) __atomic_store_n(&v->namespace_owner.site[i],name[i],__ATOMIC_RELAXED);
    __atomic_store_n(&v->namespace_owner.attribution,
        (__atomic_load_n(&v->namespace_owner.attribution,__ATOMIC_RELAXED) & ~VFS_ATTR_SITE) | bit,__ATOMIC_RELAXED);
}
/* Public legacy VFS operations are an explicit caller family. The raw tag
 * API never turns a missing site into explicit attribution. */
static void operation_tag(struct vfs *v,const char *tag) {
    vfs_tag_locked(v,tag);
    if (!(__atomic_load_n(&v->namespace_owner.attribution,__ATOMIC_RELAXED)&VFS_ATTR_SITE))
        vfs_site_locked(v,"vfs");
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
        DIAG_LOG(klog_periodic,"[storage] vfs_hold drive=%c ms=%u task=%s#%u op=%s site=%s waiters=%u tagged=%u",
            holds[i].drive,holds[i].ms,holds[i].owner.task.name,holds[i].owner.task.id,holds[i].owner.op,holds[i].owner.site,holds[i].waiters,holds[i].owner.attribution);
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
    fs_event_init(&v->volume_changed); fs_event_init(&v->startup_event);
    for (unsigned d=0;d<26;d++) { fs_lock_init(&v->volume_lock[d]); fs_lock_classify(&v->volume_lock[d],2,'A'+d); }
}
void vfs_destroy(struct vfs *v) {
    if (files_detach) files_detach(v);
    for (unsigned d=0;d<26;d++) fs_lock_destroy(&v->volume_lock[d]);
    fs_event_destroy(&v->startup_event);
    fs_event_destroy(&v->volume_changed); fs_lock_destroy(&v->identity_lock); fs_lock_destroy(&v->lock);
}
static void volume_mutexes(struct vfs *v,uint32_t mask,bool take) {
    for (unsigned i=0;i<26;i++) {
        unsigned d=take ? i : 25-i;
        if (mask & (1u<<d)) { if (take) fs_lock_take(&v->volume_lock[d]); else fs_lock_drop(&v->volume_lock[d]); }
    }
}
struct vfs_waiter {
    struct vfs_waiter *next;
    uintptr_t token; /* f1-98: the waiting task, named by later tickets */
    uint32_t mask, ticket[26], started;
    struct vfs_file_identity *file; /* retained by the calling description */
    struct vfs_hold_info owner;
};
struct vfs_reader {
    struct vfs_reader *next;
    const bool *busy;
    unsigned drive;
    struct vfs_hold_info owner;
};
static bool read_compatible(const char *op) {
    static const char *const immutable[]={"commit","mkdir","read","open","lookup","readdir","find","stat","chdir"};
    for (unsigned i=0;i<sizeof(immutable)/sizeof(*immutable);i++)
        if (!strncmp(op,immutable[i],12)) return true;
    return false;
}
static void request_wait(const struct vfs_request *r,uint64_t begin,uint32_t mask,enum vfs_wait_reason reason,int outcome) {
    if (!r || !r->wait) return;
    uint64_t elapsed=r->now(r->arg)-begin;
    if ((elapsed || outcome) && (!r->wait->valid || !r->wait->outcome)) *r->wait=(struct vfs_wait_info){r->owner,elapsed,mask,reason,outcome,true};
}
/* f1-98 (R2): whom a waiter for drive d depends on: the holder of the
 * drive's reservation or guard, else the queued waiter whose ticket is served
 * next (it must run to be admitted). In-flight open-file readers are several
 * short device phases, not one owner: nobody is named for them. A token
 * stays valid for its hold: a task is never stopped inside a VFS call
 * (native stops wait for the syscall to unwind, proc_stop). */
static uintptr_t drive_holder(struct vfs *v,unsigned d) {
    if ((v->reserved|v->mutating)&(1u<<d)) return v->hold_token[d];
    for (struct vfs_waiter *w=v->waiters;w;w=w->next)
        if ((w->mask&(1u<<d)) && w->ticket[d]==v->serving[d]) return w->token;
    return 0;
}
/* The calling task gives drive d's turn up (unlock, device-window pause or a
 * cancelled ticket): every waiter's edge to it on that drive ends here. */
static void drives_released(struct vfs *v,uint32_t mask) {
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) fs_depend_release(&v->hold_token[d]);
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
        if (next==v->next[d]) v->urgent_gate_valid[d]=false;
    }
}
/* fx-23: a refused publication step closes its drive to tickets issued
 * after the refusal (T23 trial f441c5bd: the step needs an instant with no
 * reservation and no queued ticket, and a stream of short desktop C:
 * operations kept one from ever occurring for ~50 s). Tickets issued before
 * it are served first; then the step runs (express reservation, below);
 * then the gated tickets follow in their order. */
static bool ticket_gated(const struct vfs *v,unsigned d,uint32_t ticket) {
    /* The writer turn's owner is never gated: a suspended step resumes only
     * through that turn (test_t23_holds: copy holds the turn, DOS waits). */
    if (v->turn_depth[d] && v->turn_owner[d]==fs_task_token()) return false;
    return v->publish_waiting[d] && (int32_t)(ticket-v->gate_ticket[d])>=0;
}
static int reserve_file_request(struct vfs *v,uint32_t mask,const struct vfs_request *request,
                                struct vfs_file_identity *file) {
    if (!request) request=fs_request_current();
    /* A caller already holding drives never waits behind a gate (fx-23). */
    bool gate_exempt=v->active!=0;
    mask &= VFS_ALL_DRIVES; mask &= ~v->active;
    if (mask && request && request->wait && !request->wait->outcome) request->wait->drives=mask;
    if (!mask) return vfs_request_check(request);
    struct vfs_waiter waiter={.next=v->waiters,.token=fs_task_token(),.mask=mask,.started=fs_now_ms()};
    /* f1-93: an operation that changes no file's chain (stat, lookup, find,
     * readdir, open, ...) is admitted while open-file readers are in flight,
     * the converse of reader_admitted; otherwise the desktop's stat waits
     * for another task's 64 KiB device read. Publication steps still wait
     * for those readers (guard_busy). */
    char tag[12]; tag_copy(tag,request && request->operation ? request->operation : v->namespace_owner.op);
    bool compatible=read_compatible(tag);
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) {
        if (v->active && (v->next[d]!=v->serving[d] || (v->readers[d] && !compatible) || (v->mutating&(1u<<d)))) {
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
    uint8_t attribution=__atomic_load_n(&v->namespace_owner.attribution,__ATOMIC_RELAXED);
    if (request && request->operation) attribution=(attribution & ~VFS_ATTR_OPERATION) | (*request->operation ? VFS_ATTR_OPERATION : 0);
    if (request && request->site) attribution=(attribution & ~VFS_ATTR_SITE) | (*request->site ? VFS_ATTR_SITE : 0);
    waiter.file=file;
    waiter.owner=(struct vfs_hold_info){.task=task,.started=start,.attribution=attribution};
    memcpy(waiter.owner.op,operation,sizeof(operation));
    memcpy(waiter.owner.site,site,sizeof(site));
    struct vfs_hold_info owner=holder>='A' && holder<='Z' ? v->holds[holder-'A'] : (struct vfs_hold_info){0};
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) v->diagnostic_waiters[d]++;
    uint64_t request_start=request ? request->now(request->arg) : 0;
    struct vfs_pending_waits pending=waits_detach(v);
    int error=0;
    for (;;) {
        error=vfs_request_check(request);
        bool ready=true;
        for (unsigned d=0;d<26;d++) if ((mask&(1u<<d)) &&
            (waiter.ticket[d]!=v->serving[d] || (v->readers[d] && !compatible) || (v->mutating&(1u<<d)) ||
             (v->reserved&(1u<<d)) || (!gate_exempt && ticket_gated(v,d,waiter.ticket[d])))) ready=false;
        if (error || ready) break;
        /* f1-98: lend this waiter's priority to the task it waits for on its
         * first unready drive while it sleeps. */
        uintptr_t needed=0; const void *object=0;
        for (unsigned d=0;d<26 && !needed;d++) if ((mask&(1u<<d)) &&
            (waiter.ticket[d]!=v->serving[d] || (v->mutating&(1u<<d)))) {
            needed=drive_holder(v,d); object=&v->hold_token[d];
        }
        fs_depend(needed,object,FS_DEPEND_DRIVE);
        if (request) {
            fs_lock_drop(&v->lock); request->pause(request->arg); (void)fs_lock_retake(&v->lock,VFS_WAIT_NAMESPACE);
        } else fs_event_wait(&v->volume_changed,&v->lock);
    }
    fs_depend(0,0,0);
    struct vfs_waiter **link=&v->waiters;
    while (*link!=&waiter) link=&(*link)->next;
    *link=waiter.next;
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) v->diagnostic_waiters[d]--;
    v->diagnostic_pending=pending;
    request_wait(request,request_start,mask,VFS_WAIT_VOLUME,error);
    wait_capture(&v->diagnostic_pending,start,first_drive(mask),holder,error==-VFS_ETIMEOUT ? "volume:timeout" : error ? "volume:cancelled" : "volume",&task,&owner);
    if (error) { advance_cancelled(v); drives_released(v,mask); fs_event_wake(&v->volume_changed); return error; }
    v->reserved |= mask;
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) {
        v->hold_token[d]=waiter.token;
        v->holds[d]=(struct vfs_hold_info){.task=task,.started=fs_now_ms(),.attribution=attribution};
        memcpy(v->holds[d].op,operation,sizeof(operation));
        memcpy(v->holds[d].site,site,sizeof(site));
    }
    v->active_tickets |= mask;
    /* A free additional letter can precede an existing one. Reacquire the
     * complete set in order, retaining all reservations under namespace. */
    volume_mutexes(v,v->active,false); v->active |= mask;
    volume_mutexes(v,v->active,true);
    namespace_publish(v,&task,operation);
    for (unsigned i=0;i<12;i++) __atomic_store_n(&v->namespace_owner.site[i],site[i],__ATOMIC_RELAXED);
    __atomic_store_n(&v->namespace_owner.attribution,attribution,__ATOMIC_RELAXED);
    __atomic_store_n(&v->holder,first_drive(v->active),__ATOMIC_RELAXED);
    return 0;
}
static int reserve_request(struct vfs *v,uint32_t mask,const struct vfs_request *request) {
    return reserve_file_request(v,mask,request,0);
}
void vfs_reserve_file_locked(struct vfs *v,uint32_t mask,struct vfs_file_identity *file) {
    struct fs_request_scope scope; fs_request_enter(&scope,0);
    (void)reserve_file_request(v,mask,0,file); fs_request_leave(&scope);
}
/* T23 c42f556e: an admission-style reservation that ends at the request's
 * deadline (cleanly withdrawn, nothing reserved) for a caller that already
 * holds the namespace; the storage shutdown preflight uses it. */
int vfs_reserve_request_locked(struct vfs *v,uint32_t mask,const struct vfs_request *request) {
    struct fs_request_scope scope; fs_request_enter(&scope,request);
    int error=reserve_request(v,mask,request); fs_request_leave(&scope);
    return error;
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
    bool blocked=false;
    while (!fs_lock_try_take(&v->lock)) {
        if ((error=vfs_request_check(request))) {
            if (blocked) fs_depend(0,0,0);
            request_wait(request,begin,mask,VFS_WAIT_NAMESPACE,error);
            struct vfs_pending_waits pending={0};
            wait_capture(&pending,start,first_drive(mask),holder,error==-VFS_ETIMEOUT ? "namespace:timeout" : "namespace:cancelled",&task,&owner);
            diagnostics_publish(v,0,0,&pending);
            return error;
        }
        /* f1-98: the namespace owner holds the drives being reserved. */
        blocked=true; fs_depend(fs_lock_holder(&v->lock),&v->lock,FS_DEPEND_DRIVE);
        request->pause(request->arg);
    }
    if (blocked) fs_depend(0,0,0);
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
        v->hold_token[d]=0;
    }
    drives_released(v,mask);
    v->active=0; v->active_tickets=0; v->reserved &= ~mask; v->mutating &= ~mask;
    for (unsigned d=0;d<26;d++) if (tickets&(1u<<d)) v->serving[d]++;
    advance_cancelled(v);
    __atomic_store_n(&v->holder,'-',__ATOMIC_RELAXED);
    volume_mutexes(v,mask,false); fs_lock_drop(&v->lock);
    if (mask) fs_event_wake(&v->volume_changed);
    diagnostics_publish(v,holds,count,&pending);
}
struct vfs_io_state vfs_io_begin_locked(struct vfs *v) { return vfs_io_begin_mode_locked(v,VFS_WINDOW_NONE); }
struct vfs_io_state vfs_io_begin_mode_locked(struct vfs *v,enum vfs_window mode) {
    struct vfs_io_state state={.drives=v->active,.default_drive=v->default_drive,.logical=true,
        .diagnostics=waits_detach(v)};
    uintptr_t self=fs_task_token();
    for (unsigned d=0;d<26;d++) if (v->active&(1u<<d)) {
        __atomic_store_n(&v->io_owner[d],self,__ATOMIC_RELAXED); v->io_mode[d]=(uint8_t)mode;
        v->io_sequence[d]=v->volumes[d] ? v->volumes[d]->mutation_sequence : 0;
    }
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
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) { __atomic_store_n(&v->io_owner[d],0,__ATOMIC_RELAXED); v->io_mode[d]=0; }
    volume_mutexes(v,mask,true); v->active=mask;
    v->diagnostic_pending=state.diagnostics;
    char drive=first_drive(mask);
    if (drive>='A' && drive<='Z') {
        namespace_publish(v,&v->holds[drive-'A'].task,v->holds[drive-'A'].op);
        const struct vfs_hold_info *h=&v->holds[drive-'A'];
        for (unsigned i=0;i<12;i++) __atomic_store_n(&v->namespace_owner.site[i],h->site[i],__ATOMIC_RELAXED);
        __atomic_store_n(&v->namespace_owner.attribution,h->attribution,__ATOMIC_RELAXED);
    }
    else namespace_owner(v,fs_request_current());
    v->default_drive=state.default_drive;
    __atomic_store_n(&v->holder,first_drive(mask),__ATOMIC_RELAXED);
}
/* f1-93 device windows. A VFS_IO_MODE owner gives the drive's guard up
 * around one device call of a logical provider: the hold ends (and is
 * reported as a segment), waiters run, and the guard is readmitted through
 * the ordinary FIFO reservation, uncancellably, with the same attribution.
 * transaction_users keeps the letter attached meanwhile. */
static void window_pause(struct vfs *v,unsigned d,struct vfs_hold_info *saved) {
    struct hold_record record; unsigned count=0;
    fs_lock_take(&v->lock);
    *saved=v->holds[d];
    uint32_t held=fs_now_ms()-saved->started;
    if (held>=50) { record=(struct hold_record){*saved,held,v->diagnostic_waiters[d],(char)('A'+d)}; count=1; }
    memset(&v->holds[d],0,sizeof(v->holds[d]));
    v->hold_token[d]=0; drives_released(v,1u<<d);
    v->mutating&=~(1u<<d);
    __atomic_store_n(&v->io_owner[d],0,__ATOMIC_RELAXED); v->io_mode[d]=0;
    v->transaction_users[d]++; v->window_pauses++;
    fs_event_wake(&v->volume_changed);
    fs_lock_drop(&v->lock);
    struct vfs_pending_waits none={0};
    if (count) diagnostics_publish(v,&record,count,&none);
}
static void window_resume(struct vfs *v,unsigned d,const struct vfs_hold_info *saved,enum vfs_window mode) {
    fs_lock_take(&v->lock);
    namespace_publish(v,&saved->task,saved->op);
    for (unsigned i=0;i<12;i++) __atomic_store_n(&v->namespace_owner.site[i],saved->site[i],__ATOMIC_RELAXED);
    __atomic_store_n(&v->namespace_owner.attribution,saved->attribution,__ATOMIC_RELAXED);
    vfs_reserve_locked(v,1u<<d);
    v->holds[d].task=saved->task; v->holds[d].attribution=saved->attribution;
    memcpy(v->holds[d].op,saved->op,sizeof(saved->op)); memcpy(v->holds[d].site,saved->site,sizeof(saved->site));
    /* The guard again, exactly as vfs_io_begin_locked converts a ticket. */
    volume_mutexes(v,v->active,false);
    uint32_t tickets=v->active_tickets;
    v->mutating|=v->active; v->reserved&=~v->active;
    v->active=0; v->active_tickets=0;
    for (unsigned i=0;i<26;i++) if (tickets&(1u<<i)) v->serving[i]++;
    advance_cancelled(v);
    if (tickets) fs_event_wake(&v->volume_changed);
    __atomic_store_n(&v->holder,'-',__ATOMIC_RELAXED);
    __atomic_store_n(&v->io_owner[d],fs_task_token(),__ATOMIC_RELAXED); v->io_mode[d]=(uint8_t)mode;
    v->transaction_users[d]--;
    struct vfs_pending_waits pending=waits_detach(v);
    fs_lock_drop(&v->lock);
    diagnostics_publish(v,0,0,&pending);
}
static int io_window(void *ctx,struct fat_volume *vol,bool write,int (*call)(void *),void *arg) {
    struct vfs *v=ctx; unsigned d=26;
    for (unsigned i=0;i<26;i++) if (__atomic_load_n(&v->volumes[i],__ATOMIC_RELAXED)==vol) { d=i; break; }
    if (d==26 || __atomic_load_n(&v->io_owner[d],__ATOMIC_RELAXED)!=fs_task_token()) return call(arg);
    enum vfs_window mode=(enum vfs_window)v->io_mode[d];
    /* A reader never writes; a write outside the turn keeps its guard. */
    if (mode==VFS_WINDOW_NONE || (write && mode!=VFS_WINDOW_FREE)) return call(arg);
    uint64_t snapshot=v->io_sequence[d];
    /* Announce a write before it can become visible: a reader readmitted
     * while it is in flight sees a changed sequence and restarts. */
    if (write) vol->mutation_sequence++;
    struct vfs_hold_info saved; window_pause(v,d,&saved);
    int e=call(arg);
    window_resume(v,d,&saved,mode);
    v->io_sequence[d]=snapshot;
    if (!e && mode==VFS_WINDOW_VALIDATE && vol->mutation_sequence!=snapshot) {
        __atomic_add_fetch(&v->window_restarts,1,__ATOMIC_RELAXED); e=-FS_EAGAIN;
    }
    return e;
}
int vfs_io_window_call(struct vfs *v,struct fat_volume *vol,bool write,int (*call)(void *),void *arg) {
    return vol->io_window==io_window && vol->io_window_ctx==v ? io_window(v,vol,write,call,arg) : call(arg);
}
static uint32_t vfs_path_mask(struct vfs_table *t,const char *path) {
    unsigned d=t->drive;
    if (path && path[0] && path[1]==':') d=(unsigned)(path_upper((uint8_t)path[0])-'A');
    return vfs_drive_mask(d);
}
/* op: the read-only operation's tag, set before the volume reservation so
 * that admission can see it (f1-93); 0 keeps the caller's own tagging. */
static int path_lock_op(struct vfs_table *t,const char *a,const char *b,const char *c,const char *op) {
    for (;;) {
        int error=vfs_admit(t->vfs,0,t->request); if (error) return error;
        if (op) operation_tag(t->vfs,op);
        uint32_t mask=vfs_path_mask(t,a) | (b ? vfs_path_mask(t,b) : 0) | (c ? vfs_path_mask(t,c) : 0);
        error=reserve_request(t->vfs,mask,t->request);
        if (error) { vfs_unlock(t->vfs); return error; }
        uint32_t now=vfs_path_mask(t,a) | (b ? vfs_path_mask(t,b) : 0) | (c ? vfs_path_mask(t,c) : 0);
        if (!(now & ~t->vfs->active)) { t->vfs->default_drive=t->drive; return 0; }
        vfs_unlock(t->vfs); /* default drive changed during admission */
    }
}
static int path_lock(struct vfs_table *t,const char *a,const char *b,const char *c) { return path_lock_op(t,a,b,c,0); }
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
    for (unsigned i=0;i<VFS_VERSION_PINS;i++) {
        struct vfs_version_watch *w=&v->version_watches[i];
        if (w->refs && w->volume==vol && w->mount==mount && w->parent==parent && w->index==index)
            w->sequence=slot->sequence;
    }
}
static uint64_t change_generation(struct vfs *v, struct fat_volume *vol, uint32_t parent, uint32_t index) {
    uint64_t mount=identity_generation(v,vol);
    for (unsigned i=0;i<VFS_VERSION_PINS;i++) {
        struct vfs_version_watch *w=&v->version_watches[i];
        if (w->refs && w->volume==vol && w->mount==mount && w->parent==parent && w->index==index) return w->sequence;
    }
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
uint64_t vfs_identity_file_id(struct vfs *v, struct vfs_file_identity *id,
                              uint32_t *volume_serial) {
    *volume_serial=0;
    if (!id) return 0;
    fs_lock_take(&v->identity_lock);
    uint64_t file_id=0;
    if (id->valid && id->volume->mounted &&
        id->generation==identity_generation(v,id->volume) && !id->volume->dev->quarantined) {
        /* FAT clusters use at most 28 bits. Bit 63 is reserved for retained
         * unlinked generations, which cannot collide with owning locations.
         * Bias by one so fixed-root entry zero is a usable nonzero index. */
        file_id=id->linked ? (((uint64_t)id->parent<<32)|id->index)+1 : id->unlinked_id;
        if (file_id) *volume_serial=id->volume->volume_serial;
    }
    fs_lock_drop(&v->identity_lock); return file_id;
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
    for (unsigned i=0;i<VFS_VERSION_PINS;i++) if (v->version_watches[i].volume==vol && v->version_watches[i].refs)
        v->version_watches[i].sequence=v->change_floor;
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
        else if (why==FAT_ID_UNLINK) {
            if (id->linked) {
                /* Keep deleted-but-retained files distinct from slot reuse,
                 * without exporting the recyclable identity-cache serial. */
                id->unlinked_id=v->next_unlinked_id<INT64_MAX ?
                    (UINT64_C(1)<<63)|++v->next_unlinked_id : 0;
            }
            id->linked=false;
        }
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
        v->volumes[d]=vol; v->generation[d]++; namespace_changed();
        vol->identity_changed=identity_change; vol->identity_ctx=v;
        /* f1-93: a logical provider has no block cache; its device calls
         * are windows of the caller's VFS_IO_MODE (see io_window). */
        if (vol->dev && vol->dev->logical) { vol->io_window=io_window; vol->io_window_ctx=v; }
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
    if (!e) { v->ro_volumes[d] = vol; v->generation[d]++; namespace_changed(); }
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
    VFS_IO_VOID(v,fs_ro_unmount(ro)); v->ro_volumes[d] = 0; v->generation[d]++; namespace_changed();
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
        VFS_IO_VOID(v,fs_ro_unmount(ro)); v->ro_volumes[d]=0; v->generation[d]++; namespace_changed();
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
        vol->io_window=0; vol->io_window_ctx=0;
        v->volumes[d]=0; v->generation[d]++; namespace_changed();
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
            /* This phase only retires the description's speculative reads.
             * Its live slot and io_file pin the file; no FAT workspace or
             * metadata guard is needed during the wait/cache scan. */
            struct vfs_hold_info saved;
            if (announce) window_pause(v,drive,&saved);
            cache_stream_close(&d->stream);
            if (announce) window_resume(v,drive,&saved,VFS_WINDOW_NONE);
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
    operation_tag(v,"close"); vfs_site_locked(v,"teardown");
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
/* f1-89 writer turn. One FAT-mutating operation per letter plans and
 * publishes at a time (no spurious EAGAIN, no lost FSInfo/free-count
 * updates). Data transactions give it up around device waits and rebase or
 * merge on return (Amendment 1 item 4). It is acquired before volume
 * admission and waited for with namespace only, so the order is turn ->
 * namespace -> volumes -> cache; nobody waits for it while holding cache
 * pins. Readers never touch it; they are excluded from publication steps
 * only by the short guard. */
/* f1-93: logical volumes too. Their mutations give the volume guard up
 * around provider I/O (device windows), so the turn is what keeps a
 * second mutator out for the operation's life. */
static uint32_t turn_drives(struct vfs *v,uint32_t mask) {
    uint32_t out=0;
    for (unsigned d=0;d<26;d++) if ((mask&(1u<<d)) && v->volumes[d]) out|=1u<<d;
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
static void turn_freed(struct vfs *v,unsigned d) { v->turn_free_since[d]=fs_now_ms(); }
void vfs_turn_drop_locked(struct vfs *v,uint32_t mask) {
    uintptr_t self=fs_task_token(); bool wake=false;
    for (unsigned d=0;d<26;d++)
        if ((mask&(1u<<d)) && v->turn_depth[d] && v->turn_owner[d]==self && !--v->turn_depth[d]) {
            v->turn_owner[d]=0; turn_freed(v,d); wake=true;
            fs_depend_release(&v->turn_owner[d]);
        }
    if (wake) fs_event_wake(&v->volume_changed);
}
/* A wait ends when the turn last became free, not when this task ran again:
 * time the releaser spends on its own device work afterwards is not ours. */
static void turn_wait_record(struct vfs *v,uint32_t mask,uint32_t start) {
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) {
        uint32_t end=v->turn_free_since[d];
        if ((int32_t)(end-start)<0) end=start;
        v->turn_waits[d]++; if (end-start>v->turn_wait_max_ms[d]) v->turn_wait_max_ms[d]=end-start;
    }
}
/* f1-98: a queued writer lends its priority to the turn's owner. */
static void turn_depend(struct vfs *v,uint32_t mask) {
    uintptr_t self=fs_task_token();
    for (unsigned d=0;d<26;d++) if ((mask&(1u<<d)) && v->turn_depth[d] && v->turn_owner[d]!=self) {
        fs_depend(v->turn_owner[d],&v->turn_owner[d],FS_DEPEND_DRIVE); return;
    }
    fs_depend(0,0,0);
}
/* Namespace held; the caller holds no reservation that the owner could
 * need. Cancellable through the current request; on error nothing is held. */
static int turn_queue_locked(struct vfs *v,uint32_t mask) {
    if (turn_available(v,mask,false)) { turn_hold(v,mask); return 0; }
    uint32_t start=fs_now_ms(); int e=0;
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) v->turn_waiters[d]++;
    while (!e && !turn_available(v,mask,true)) {
        turn_depend(v,mask);
        e=fs_event_wait_request(&v->volume_changed,&v->lock,VFS_WAIT_VOLUME);
    }
    fs_depend(0,0,0);
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) v->turn_waiters[d]--;
    if (!e) turn_hold(v,mask);
    turn_wait_record(v,mask,start);
    fs_event_wake(&v->volume_changed); /* other queued writers re-evaluate */
    return e;
}
uint32_t vfs_turn_suspend_locked(struct vfs *v,unsigned drive) {
    if (drive>=26 || !v->turn_depth[drive] || v->turn_owner[drive]!=fs_task_token()) return 0;
    uint32_t depth=v->turn_depth[drive];
    v->turn_depth[drive]=0; v->turn_owner[drive]=0; turn_freed(v,drive);
    fs_depend_release(&v->turn_owner[drive]);
    fs_event_wake(&v->volume_changed); return depth;
}
bool vfs_turn_try_resume_locked(struct vfs *v,unsigned drive,uint32_t depth) {
    if (!depth) return true;
    if (!turn_available(v,1u<<drive,false)) return false;
    v->turn_owner[drive]=fs_task_token(); v->turn_depth[drive]=depth;
    return true;
}
/* Uncancellable: the transaction has durable steps behind it and owns its
 * identity pins; it queues like any writer and never passes one. */
void vfs_turn_resume_locked(struct vfs *v,unsigned drive,uint32_t depth) {
    if (!depth) return;
    uint32_t mask=1u<<drive;
    if (!turn_available(v,mask,false)) {
        uint32_t start=fs_now_ms();
        v->turn_waiters[drive]++;
        while (!turn_available(v,mask,true)) { turn_depend(v,mask); fs_event_wait(&v->volume_changed,&v->lock); }
        fs_depend(0,0,0);
        v->turn_waiters[drive]--;
        turn_wait_record(v,mask,start);
    }
    v->turn_owner[drive]=fs_task_token(); v->turn_depth[drive]=depth;
    fs_event_wake(&v->volume_changed);
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
    uint32_t mask, turn_saved;
    unsigned drive, default_drive;
    bool retired, gate, read_only;
    char op[12],site[12];
};
/* A publication step excludes metadata holders, queued tickets and, by lead
 * decision (f1-89 Amendment 1 item 4), in-flight open-file readers.
 * f1-93: a read-only transaction (lookup, next entry, commit) publishes
 * nothing: its short steps exclude metadata holders only, never wait for
 * open-file readers and never close reader admission, so a desktop stat is
 * not queued behind another task's 64 KiB device read. */
static bool guard_busy(struct transaction_guard *g) {
    struct vfs *v=g->v;
    bool busy=((v->reserved|v->mutating)&g->mask)!=0;
    if (g->read_only) return busy;
    /* Tickets issued after a refused step (fx-23) wait for it, so only the
     * ones issued before the gate keep the step out. */
    for (unsigned d=0;d<26;d++) if (g->mask&(1u<<d))
        busy|=(v->publish_waiting[d] ? (int32_t)(v->gate_ticket[d]-v->serving[d])>0 : v->next[d]!=v->serving[d]) || v->readers[d];
    return busy;
}
/* The guard's reservation, tagged before admission (see reserve_file_request). */
static void guard_gate(struct transaction_guard *g,bool on);
static void guard_lock(struct transaction_guard *g) {
    vfs_lock(g->v,0); vfs_tag_locked(g->v,g->op); vfs_site_locked(g->v,g->site);
    guard_gate(g,false); /* fx-23: never queue behind our own gate */
    vfs_reserve_locked(g->v,g->mask);
}
/* Namespace held. While a step waits, new readers of its letters wait too
 * (reader_admitted): in-flight readers drain and the step gets its turn. */
static void guard_gate(struct transaction_guard *g,bool on) {
    if (on && g->read_only) return;
    if (g->gate==on) return;
    g->gate=on;
    for (unsigned d=0;d<26;d++) if (g->mask&(1u<<d)) {
        if (on && !g->v->publish_waiting[d]++) g->v->gate_ticket[d]=g->v->next[d];
        if (!on) g->v->publish_waiting[d]--;
    }
    if (!on) fs_event_wake(&g->v->volume_changed);
}
/* Namespace held, the drives free (guard_busy false): a reservation that
 * consumes no ticket, so releasing it advances no queue (fx-23). */
static void express_reserve_locked(struct vfs *v,uint32_t mask) {
    mask&=VFS_ALL_DRIVES; mask&=~v->active;
    if (!mask) return;
    if ((v->reserved|v->mutating)&mask) fs_panic("express reservation of a held drive");
    struct vfs_hold_info owner=holder_snapshot(&v->namespace_owner);
    v->reserved|=mask;
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) {
        v->hold_token[d]=fs_task_token();
        v->holds[d]=owner; v->holds[d].started=fs_now_ms();
    }
    volume_mutexes(v,v->active,false); v->active|=mask;
    volume_mutexes(v,v->active,true);
    __atomic_store_n(&v->holder,first_drive(v->active),__ATOMIC_RELAXED);
}
static int transaction_guard_enter(void *arg) {
    struct transaction_guard *g=arg; struct vfs *v=g->v;
    vfs_lock(v,0);
    /* Never wait for a legacy writer while retaining cache reservations:
     * that writer may itself need one of our pinned sectors. */
    if (guard_busy(g)) { guard_gate(g,true); vfs_unlock(v); return -FS_EBUSY; }
    vfs_tag_locked(v,g->op); vfs_site_locked(v,g->site);
    /* fx-23: tickets queued after the gate wait for this step: take the
     * drive ahead of them (no ticket), else as before. */
    bool behind=false;
    for (unsigned d=0;d<26;d++) if ((g->mask&(1u<<d)) && v->next[d]!=v->serving[d]) behind=true;
    if (behind) { express_reserve_locked(v,g->mask); guard_gate(g,false); }
    else { guard_gate(g,false); vfs_reserve_locked(v,g->mask); } /* never queue behind our own gate */
    v->default_drive=g->default_drive;
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
/* f1-89 Amendment 1 item 4: no reservation is held between steps, so the
 * turn can be given up around device waits; resume leaves namespace held. */
static void transaction_guard_suspend(void *arg) {
    struct transaction_guard *g=arg; struct vfs *v=g->v;
    vfs_lock(v,0); g->turn_saved=vfs_turn_suspend_locked(v,g->drive); vfs_unlock(v);
}
static void transaction_guard_resume(void *arg) {
    struct transaction_guard *g=arg; struct vfs *v=g->v;
    vfs_lock(v,0);
    /* fx-23: a step that must wait for the writer turn opens its gate first:
     * the turn's holder may be queued behind a gated ticket. */
    if (g->turn_saved && !turn_available(v,1u<<g->drive,false)) guard_gate(g,false);
    vfs_turn_resume_locked(v,g->drive,g->turn_saved); g->turn_saved=0;
}
static int transaction_guard_try_resume(void *arg) {
    struct transaction_guard *g=arg; struct vfs *v=g->v;
    vfs_lock(v,0);
    if (vfs_turn_try_resume_locked(v,g->drive,g->turn_saved)) { g->turn_saved=0; return 1; }
    vfs_unlock(v); return 0;
}
/* Wait, with no reservation and no writer turn, until a busy file condition
 * clears: its owner may be a suspended data transaction that needs the turn
 * back. The caller's admission set and turn are restored afterwards. */
static bool flag_busy(void *arg) { return *(const bool *)arg; }
struct node_busy { struct vfs *v; struct vfs_node *node; };
static bool node_busy(void *arg) {
    struct node_busy *b=arg;
    if (!b->node->refs) return false;
    if (b->node->pins) return true;
    for (unsigned i=0;i<VFS_DESCRIPTIONS;i++)
        if (b->v->descriptions[i].refs && b->v->descriptions[i].node==b->node && b->v->descriptions[i].io_active) return true;
    return false;
}
static __attribute__((noinline)) int wait_unturned(struct vfs *v,unsigned drive,bool (*busy)(void *),void *arg) {
    uint32_t depth=vfs_turn_suspend_locked(v,drive);
    uint32_t active=v->active; unsigned default_drive=v->default_drive;
    struct vfs_hold_info owner=holder_snapshot(&v->namespace_owner);
    vfs_unlock(v); vfs_lock(v,0);
    int e=0;
    while (!e && busy(arg)) e=fs_event_wait_request(&v->volume_changed,&v->lock,VFS_WAIT_VOLUME);
    vfs_turn_resume_locked(v,drive,depth);
    vfs_unlock(v); vfs_lock(v,active);
    v->default_drive=default_drive; vfs_tag_locked(v,owner.op); vfs_site_locked(v,owner.site);
    return e;
}
static void transaction_guard_leave(void *arg) {
    struct transaction_guard *g=arg;
    /* Transfer the final validated namespace turn directly to the caller.
     * Dropping/reacquiring it here would make a just-resolved path stale. */
    if (!g->retired) vfs_unlock(g->v);
}
static void transaction_guard_retire(void *arg) {
    struct transaction_guard *g=arg;
    guard_lock(g); g->v->default_drive=g->default_drive;
    guard_gate(g,false);
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
#define VFS_STEP_ATTEMPTS 64u
/* f1-93b: a guarded lookup that resumes in place returns EAGAIN to its walk
 * after this many warms of the same sector at the same position (evicted
 * again before it could be used). Every resumed walk is also bounded in
 * sections: a FAT directory has at most 65536 slots, and each section
 * settles at least one of them or warms a missing sector. */
#define VFS_STEP_ATTEMPTS_GUARDED 8u
#define VFS_STEP_SECTIONS 131072u
/* Pins protect the source volume, matching descriptions and their node for
 * the entire unlocked operation. The mutation sequence validates cold
 * planning against every other FAT publisher, including legacy callers. */
/* Read one volume sector into the cache with no reservation or turn held;
 * the caller's admission set is readmitted afterwards (as for the turn).
 * f1-93: sector UINT64_MAX reads nothing: a bounded guard section yields. */
static __attribute__((noinline)) int warm_sector(struct vfs *v,struct fat_volume *vol,unsigned drive,uint64_t sector) {
    uint32_t active=v->active; unsigned default_drive=v->default_drive;
    struct vfs_hold_info owner=holder_snapshot(&v->namespace_owner);
    v->transaction_users[drive]++; /* no detach while the read runs */
    vfs_unlock(v);
    /* f1-93: the whole read-ahead run of the sector (FAT copy, cluster or
     * root directory), so one warm serves the rest of the walk.
     * f1-93b: a section that ended at its work bound offers its preemption
     * point here, with nothing held (the T23 b453bc92 holds were the guard
     * kept by a starved holder preempted inside its section). */
    int e=0;
    if (sector==UINT64_MAX) fs_service();
    else e=fat_warm(vol,sector);
    /* Readmitted with its own tag (a read-only step is admitted beside
     * open-file readers, f1-93). */
    vfs_lock(v,0); vfs_tag_locked(v,owner.op); vfs_site_locked(v,owner.site);
    vfs_reserve_locked(v,active);
    v->transaction_users[drive]--; fs_event_wake(&v->volume_changed);
    v->default_drive=default_drive; vfs_tag_locked(v,owner.op); vfs_site_locked(v,owner.site);
    if (!e && (!(v->active&(1u<<drive)) || v->volumes[drive]!=vol)) e=-FS_EIO;
    return e;
}
static bool transaction_read_only(enum fat_transaction_op op) {
    return op==FAT_TX_LOOKUP || op==FAT_TX_NEXT || op==FAT_TX_COMMIT;
}
static int transaction_once(struct vfs *,struct fat_volume *,struct fat_transaction_args,unsigned);
/* noinline: callers such as vfs_log_rotate keep the i686 4 KiB frame. */
__attribute__((noinline)) int vfs_fat_transaction_locked(struct vfs *v, struct fat_volume *vol, struct fat_transaction_args a) {
    unsigned drive=26;
    for (unsigned d=0;d<26;d++) if (v->volumes[d]==vol) { drive=d; break; }
    if (drive==26 || !(v->active&(1u<<drive))) return -FS_EIO;
    if (a.guarded && !vol->dev->logical && (a.op==FAT_TX_LOOKUP || a.op==FAT_TX_NEXT)) {
        /* Reader restart fallback (f1-89 Amendment 1 item 5): the short
         * guard excludes every publication step, and the lookup reads only
         * resident sectors. A miss is read with no reservation held.
         * f1-93b: so is a section's work bound, and the lookup resumes in
         * place when nothing was published meanwhile (no earlier component
         * can be stale); a publication still restarts the caller's walk. */
        uint64_t sequence=vol->mutation_sequence, last=UINT64_MAX;
        struct fat_dir_hint hint={0};
        uint32_t at=a.op==FAT_TX_NEXT ? *a.directory_cursor : 0, last_at=UINT32_MAX;
        for (unsigned stalls=0,sections=0;;) {
            if (++sections>VFS_STEP_SECTIONS) return -FS_EAGAIN;
            uint64_t miss; uint32_t pos=at;
            int e=VFS_IO(v,fat_dir_cached(vol,a.directory,a.op==FAT_TX_LOOKUP ? a.name : 0,&pos,a.entry,false,&hint,&miss));
            if (e!=-FS_EAGAIN || miss==UINT64_MAX) {
                if (a.op==FAT_TX_NEXT) *a.directory_cursor=pos;
                return e;
            }
            at=pos;
            if (miss==FAT_CACHE_YIELD) v->step_yields++;
            else if (miss==last && at==last_at) { if (++stalls==VFS_STEP_ATTEMPTS_GUARDED) return -FS_EAGAIN; }
            else v->warm_restarts++;
            last=miss; last_at=at;
            if ((e=warm_sector(v,vol,drive,miss==FAT_CACHE_YIELD ? UINT64_MAX : miss))) return e;
            if (vol->mutation_sequence!=sequence) return -FS_EAGAIN;
        }
    }
    /* LOG_WRITE publishes only DATA into owned reserved clusters: it never
     * stales another transaction, so the storage writer takes no turn.
     * f1-93: a logical mutation takes the turn like a physical one (its
     * device windows give the volume guard up). */
    if (transaction_read_only(a.op) || (a.op==FAT_TX_LOG_WRITE && !vol->dev->logical)) return transaction_once(v,vol,a,drive);
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
/* f1-93: a logical provider (no block cache, no reserved publication) runs
 * the FAT operation directly on the source volume, but never holds its
 * volume guard across a provider call: the turn holder's calls are FREE
 * windows, a barrier is FREE, a reader's are VALIDATE windows (EAGAIN:
 * the caller's walk restarts, as for an unguarded physical lookup). */
static __attribute__((noinline)) int logical_once(struct vfs *v,struct fat_volume *vol,const struct fat_transaction_args *pa,unsigned drive) {
    struct fat_transaction_args a=*pa;
    enum vfs_window m=vfs_turn_held_locked(v,1u<<drive) || a.op==FAT_TX_COMMIT ? VFS_WINDOW_FREE :
        a.op==FAT_TX_LOOKUP || a.op==FAT_TX_NEXT ? VFS_WINDOW_VALIDATE : VFS_WINDOW_NONE;
    switch (a.op) {
    case FAT_TX_LOOKUP: return VFS_IO_MODE(v,m,fat_lookup(vol,a.directory,a.name,a.entry));
    case FAT_TX_CREATE: return VFS_IO_MODE(v,m,fat_create(vol,a.directory,a.name,a.attr,a.entry));
    case FAT_TX_WRITE: return VFS_IO_MODE(v,m,fat_write_cursor(vol,a.entry,a.cursor,a.position,a.data,a.size,a.done));
    case FAT_TX_LOG_WRITE: return VFS_IO_MODE(v,m,fat_write_reserved_cursor(vol,a.entry,a.cursor,a.capacity,a.position,a.data,a.size,a.done));
    case FAT_TX_COMMIT: return VFS_IO_MODE(v,m,fat_commit(vol));
    case FAT_TX_REMOVE: return VFS_IO_MODE(v,m,fat_remove(vol,a.entry,a.flag));
    case FAT_TX_RENAME: return VFS_IO_MODE(v,m,fat_rename(vol,a.entry,a.directory,a.name));
    case FAT_TX_PUBLISH: return VFS_IO_MODE(v,m,fat_publish_reserved(vol,a.entry,a.flag));
    case FAT_TX_EXTEND: return VFS_IO_MODE(v,m,fat_extend(vol,a.entry,a.capacity));
    case FAT_TX_RENAME_REPLACE: return VFS_IO_MODE(v,m,fat_rename_replace(vol,a.entry,a.replacement,a.directory,a.name,a.flag));
    case FAT_TX_TRUNCATE: return VFS_IO_MODE(v,m,fat_truncate(vol,a.entry,a.position));
    case FAT_TX_RESERVE: return VFS_IO_MODE(v,m,fat_reserve(vol,a.entry,a.capacity));
    case FAT_TX_NEXT: {
        uint32_t start=*a.directory_cursor;
        int e=VFS_IO_MODE(v,m,fat_next(vol,a.directory,a.directory_cursor,a.entry));
        if (e==-FS_EAGAIN) *a.directory_cursor=start;
        return e;
    }
    case FAT_TX_SET_METADATA: return VFS_IO_MODE(v,m,fat_set_metadata(vol,a.entry,a.attr,a.times));
    case FAT_TX_SET_LABEL: return VFS_IO_MODE(v,m,fat_set_label(vol,a.name));
    }
    return -FS_EINVAL;
}
static int transaction_once(struct vfs *v,struct fat_volume *vol,struct fat_transaction_args a,unsigned drive) {
    if (vol->dev->logical) return logical_once(v,vol,&a,drive);
    struct transaction_guard g={.v=v,.mask=v->active,.drive=drive,.default_drive=v->default_drive,
        .read_only=transaction_read_only(a.op)};
    memcpy(g.op,v->holds[drive].op,sizeof(g.op)); memcpy(g.site,v->holds[drive].site,sizeof(g.site));
    struct vfs_node *node=0;
    struct vfs_file_identity *identity=0,*replacement_id=0;
    uint64_t planned=vol->mutation_sequence;
    bool writer=a.entry && a.op!=FAT_TX_CREATE && a.op!=FAT_TX_LOOKUP && a.op!=FAT_TX_NEXT && a.op!=FAT_TX_COMMIT;
    bool waited=false;
retry:
    node=0; identity=0; replacement_id=0;
    if (writer) {
        int e=vfs_identity_get(v,vol,a.entry,&identity); if (e) return e;
        if (a.replacement) e=vfs_identity_get(v,vol,a.replacement,&replacement_id);
        /* Claim both only when both are available. No AB/BA ownership is
         * retained across a wait on the other rename identity. The owner
         * may be a suspended data transaction: wait without the turn. */
        while (!e && (identity->writer || (replacement_id && replacement_id->writer))) {
            e=wait_unturned(v,drive,flag_busy,identity->writer ? &identity->writer : &replacement_id->writer);
            waited=true;
        }
        if (!e && (!vfs_identity_live(v,identity) || (replacement_id && !vfs_identity_live(v,replacement_id)))) e=-FS_EIO;
        /* f1-89 Amendment 1 item 2: a borrowed entry that still names a slot
         * after its file was unlinked must never rewrite (resurrect) it. */
        if (!e && ((!identity->linked && a.entry->index!=UINT32_MAX) ||
                   (replacement_id && !replacement_id->linked && a.replacement->index!=UINT32_MAX))) e=-FS_ENOENT;
        /* Only a caller's copy from a path walk can go stale by waiting. */
        if (!e && waited && a.borrowed && vol->mutation_sequence!=planned) e=-FS_EAGAIN;
        if (e) { vfs_identity_put(v,replacement_id); vfs_identity_put(v,identity); return e; }
        identity->writer=true;
        if (replacement_id) replacement_id->writer=true;
    }
    for (unsigned i=0;i<VFS_NODES;i++) if (v->nodes[i].refs &&
        (a.entry==&v->nodes[i].entry || (identity && v->nodes[i].identity==identity))) { node=&v->nodes[i]; break; }
    if (node) {
        /* In use by another transaction (possibly suspended) or a reader:
         * wait without the turn, then claim everything again. */
        struct node_busy busy={v,node};
        if (node_busy(&busy)) {
            if (replacement_id) { replacement_id->writer=false; vfs_identity_put(v,replacement_id); }
            if (identity) { identity->writer=false; vfs_identity_put(v,identity); }
            fs_event_wake(&v->volume_changed);
            int e=wait_unturned(v,drive,node_busy,&busy); waited=true;
            if (e) return e;
            goto retry;
        }
    }
    bool pinned[VFS_DESCRIPTIONS]={0};
    if (node) {
        node->pins++;
        for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) if (v->descriptions[i].refs && v->descriptions[i].node==node) {
            pinned[i]=true; v->descriptions[i].refs++; v->descriptions[i].io_active=true;
        }
    }
    v->transaction_users[drive]++;
    a.sequence=vol->mutation_sequence;
    vfs_unlock(v);
    const struct fat_transaction_guard callbacks={&g,transaction_guard_enter,transaction_guard_leave,transaction_guard_wait,
        transaction_guard_retire,transaction_guard_claim,transaction_guard_unclaim,
        transaction_guard_suspend,transaction_guard_resume,transaction_guard_try_resume};
    int e=fat_transaction_run(vol,&a,&callbacks);
    if (!g.retired) guard_lock(&g);
    guard_gate(&g,false);
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
/* f1-93: directory steps and chain walks that never hold a volume across
 * a cold device read (vfs.h). A physical step reads resident sectors only
 * under the guard; the first miss is read with nothing held (warm_sector)
 * and the step restarts from the same cursor, so a completed step saw one
 * consistent state. A logical step uses device windows. Bounded: every
 * restart warms a whole read-ahead run, and an exhausted bound falls back
 * to the guarded cold read (counted; zero in every host gate). */
static unsigned volume_drive(struct vfs *v,struct fat_volume *vol) {
    for (unsigned d=0;d<26;d++) if (v->volumes[d]==vol) return d;
    return 26;
}
static enum vfs_window reader_window(struct vfs *v,unsigned drive) {
    return vfs_turn_held_locked(v,1u<<drive) ? VFS_WINDOW_FREE : VFS_WINDOW_VALIDATE;
}
static int dir_step_cold(struct fat_volume *vol,uint32_t dir,const char *name,uint32_t *cursor,struct fat_entry *e,bool labels) {
    return name ? fat_lookup(vol,dir,name,e) : labels ? fat_next_dos(vol,dir,cursor,e) : fat_next(vol,dir,cursor,e);
}
__attribute__((noinline)) int vfs_dir_walk_locked(struct vfs *v,struct fat_volume *vol,uint32_t dir,const char *name,
                                                  uint32_t *cursor,struct fat_entry *e,bool labels,struct fat_dir_hint *hint) {
    unsigned drive=volume_drive(v,vol);
    if (drive==26 || !(v->active&(1u<<drive))) return -FS_EIO;
    uint32_t scratch=0; if (!cursor) cursor=&scratch;
    struct fat_dir_hint local={0}; if (!hint) hint=&local;
    /* start: the step's first slot (a lookup's is 0); at: where the next
     * section resumes (the first entry not yet settled). */
    uint32_t start=name ? 0 : *cursor, at=start, last_at=UINT32_MAX;
    uint64_t sequence=vol->mutation_sequence, last=UINT64_MAX;
    unsigned sections=0;
    for (unsigned attempt=0;;attempt++) {
        int r;
        if (++sections>VFS_STEP_SECTIONS) attempt=VFS_STEP_ATTEMPTS;
        if (vol->dev->logical) {
            enum vfs_window m=attempt<VFS_STEP_ATTEMPTS ? reader_window(v,drive) : VFS_WINDOW_NONE;
            if (m==VFS_WINDOW_NONE) v->cold_fallbacks++;
            uint32_t pos=start;
            r=VFS_IO_MODE(v,m,dir_step_cold(vol,dir,name,&pos,e,labels));
            if (r!=-FS_EAGAIN || m!=VFS_WINDOW_VALIDATE) { if (!name) *cursor=pos; return r; }
            continue;
        }
        if (attempt>=VFS_STEP_ATTEMPTS) {
            /* Bounded: only restarts after a publication and warms that made
             * no progress count; then the old guarded cold step (counted). */
            v->cold_fallbacks++;
            uint32_t pos=start;
            r=VFS_IO(v,dir_step_cold(vol,dir,name,&pos,e,labels));
            if (!name) *cursor=pos;
            return r;
        }
        uint64_t miss; uint32_t pos=at;
        r=VFS_IO(v,fat_dir_cached(vol,dir,name,&pos,e,labels,hint,&miss));
        if (r!=-FS_EAGAIN || miss==UINT64_MAX) { if (!name) *cursor=pos; return r; }
        at=pos;
        if (miss==FAT_CACHE_YIELD) { v->step_yields++; attempt--; }
        else if (miss!=last || at!=last_at) { v->warm_restarts++; attempt--; } /* progress */
        last=miss; last_at=at;
        if ((r=warm_sector(v,vol,drive,miss==FAT_CACHE_YIELD ? UINT64_MAX : miss))) return r;
        if (vol->mutation_sequence!=sequence) {
            /* A publication while the guard was given up: the settled
             * entries may have changed, restart the step (counted). */
            sequence=vol->mutation_sequence; at=start; hint->valid=false; last=UINT64_MAX;
            v->step_restarts++; attempt++;
        }
    }
}
__attribute__((noinline)) int vfs_dir_step_locked(struct vfs *v,struct fat_volume *vol,uint32_t dir,const char *name,
                                                  uint32_t *cursor,struct fat_entry *e,bool labels) {
    return vfs_dir_walk_locked(v,vol,dir,name,cursor,e,labels,0);
}
__attribute__((noinline)) int vfs_chain_clusters_locked(struct vfs *v,struct fat_volume *vol,uint32_t first,uint32_t *clusters) {
    unsigned drive=volume_drive(v,vol);
    if (drive==26 || !(v->active&(1u<<drive))) return -FS_EIO;
    struct fat_chain_walk w={first,first,0};
    uint64_t sequence=vol->mutation_sequence;
    for (unsigned attempt=0;;attempt++) {
        bool logical=vol->dev->logical, fallback=attempt>=4*VFS_STEP_ATTEMPTS;
        enum vfs_window m=logical && !fallback ? reader_window(v,drive) : VFS_WINDOW_NONE;
        uint64_t miss; int r;
        if (fallback) v->cold_fallbacks++;
        r=VFS_IO_MODE(v,m,fat_chain_clusters(vol,&w,!logical && !fallback,&miss));
        if (r!=-FS_EAGAIN) { if (!r) *clusters=w.count; return r; }
        /* A miss is warmed, a full section yields; either releases the
         * guard. A walk that saw a publication restarts from first. */
        if (miss==FAT_CACHE_YIELD) miss=UINT64_MAX; /* never from a chain walk; a yield if it were */
        if (!logical && miss!=UINT64_MAX) v->warm_restarts++;
        if ((r=warm_sector(v,vol,drive,logical ? UINT64_MAX : miss))) return r;
        if (vol->mutation_sequence!=sequence) { w=(struct fat_chain_walk){first,first,0}; sequence=vol->mutation_sequence; }
    }
}
uint64_t vfs_change_generation(struct vfs *v,struct fat_volume *vol,uint32_t parent,uint32_t index) {
    fs_lock_take(&v->identity_lock);
    uint64_t g=change_generation(v,vol,parent,index);
    fs_lock_drop(&v->identity_lock); return g;
}
static uint32_t root(struct fat_volume *v) { return v->type==32 ? v->root : 0; }
/* f1-89: lookups run unguarded and report EAGAIN when any publication
 * overlapped them. Every earlier component may then be stale (a directory
 * can be removed and its cluster reused), so the whole path restarts from
 * the root. The final attempt runs each lookup under the short guard: the
 * earlier attempts warmed the cache and no publication can intervene. */
#define RESOLVE_UNGUARDED 2u
/* Guarded attempts restart after warming one missing sector each; a path
 * reaches only a few dozen sectors, so this bound is never the limit. */
#define RESOLVE_GUARDED 64u
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
    v->lookup_restarts++; if (attempt>=RESOLVE_UNGUARDED) v->lookup_guarded++;
}
static __attribute__((noinline)) int resolve(struct vfs_table *t, const char *path, struct resolved *r) {
    int e=-FS_EAGAIN;
    for (unsigned i=0;i<RESOLVE_UNGUARDED+RESOLVE_GUARDED && e==-FS_EAGAIN;i++) {
        if (i) resolve_restart(t->vfs,i);
        e=resolve_once(t,path,r,i>=RESOLVE_UNGUARDED);
    }
    return e;
}
static __attribute__((noinline)) int lookup(struct vfs_table *t, const char *path, struct resolved *r, struct fat_entry *ent) {
    int e=-FS_EAGAIN;
    for (unsigned i=0;i<RESOLVE_UNGUARDED+RESOLVE_GUARDED && e==-FS_EAGAIN;i++) {
        if (i) resolve_restart(t->vfs,i);
        e=lookup_once(t,path,r,ent,i>=RESOLVE_UNGUARDED);
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
    int admission=flags&(VFS_CREATE|VFS_TRUNCATE) ? path_lock_writer(t,path,0,0,&turn) : path_lock_op(t,path,0,0,"open");
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
struct file_wait_evidence { bool valid; char holder; struct vfs_hold_info owner; };
static bool meaningful_owner(const struct vfs_hold_info *owner) {
    return owner->task.name[0] && owner->op[0] && owner->site[0] &&
        owner->attribution==VFS_ATTR_COMPLETE;
}
static void file_wait_owner(struct vfs *v,uint32_t mask,const bool *busy,
                            struct file_wait_evidence *evidence) {
    if (!evidence || evidence->valid) return;
    /* The description, rather than an unrelated volume writer, is the
     * blocker for shared-offset I/O. Records live until I/O retirement. */
    for (struct vfs_reader *r=v->reader_owners;busy && r;r=r->next)
        if (r->busy==busy && meaningful_owner(&r->owner)) {
            *evidence=(struct file_wait_evidence){true,(char)('A'+r->drive),r->owner}; return;
        }
    for (unsigned d=0;d<26;d++) if ((mask&(1u<<d)) &&
        ((v->reserved|v->mutating)&(1u<<d)) && meaningful_owner(&v->holds[d])) {
        *evidence=(struct file_wait_evidence){true,(char)('A'+d),v->holds[d]}; return;
    }
}
static void file_wait_complete(struct vfs *v,uint32_t mask,const struct vfs_request *request,
    uint32_t start,struct file_wait_evidence *e,int error) {
    if (!e || !e->valid) return;
    if (!request) request=fs_request_current();
    struct fs_task_identity task=fs_request_identity(request);
    wait_capture(&v->diagnostic_pending,start,first_drive(mask),e->holder,
        error==-VFS_ETIMEOUT ? "volume:timeout" : error ? "volume:cancelled" : "volume",&task,&e->owner);
    e->valid=false;
}
static int file_wait_locked(struct vfs *v,uint32_t mask,const struct vfs_request *request,
                            uint32_t start,uint64_t begin,struct file_wait_evidence *evidence) {
    if (!request) request=fs_request_current();
    char holder=first_drive((v->reserved|v->mutating)&mask);
    struct vfs_hold_info owner=holder>='A' && holder<='Z' ? v->holds[holder-'A'] : (struct vfs_hold_info){0};
    struct fs_task_identity task=fs_request_identity(request);
    file_wait_owner(v,mask,0,evidence);
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) v->diagnostic_waiters[d]++;
    struct vfs_pending_waits pending=waits_detach(v);
    if (!request) fs_event_wait(&v->volume_changed,&v->lock);
    else { fs_lock_drop(&v->lock); request->pause(request->arg); (void)fs_lock_retake(&v->lock,VFS_WAIT_NAMESPACE); }
    v->diagnostic_pending=pending;
    for (unsigned d=0;d<26;d++) if (mask&(1u<<d)) v->diagnostic_waiters[d]--;
    int error=vfs_request_check(request);
    request_wait(request,begin,mask,VFS_WAIT_VOLUME,error);
    if (error && evidence) file_wait_complete(v,mask,request,start,evidence,error);
    else if (!evidence && (error || !request)) wait_capture(&v->diagnostic_pending,start,first_drive(mask),holder,error==-VFS_ETIMEOUT ? "volume:timeout" : error ? "volume:cancelled" : "volume",&task,&owner);
    return error;
}
/* wait=false (writer holding the turn): a busy description is reported as
 * EBUSY instead of waited for; its owner may need the turn (f1-89 A1 #4). */
static int handle_lock_mode(struct vfs_table *t,int h,bool wait) {
    uint32_t start=fs_now_ms(); uint64_t begin=t->request ? t->request->now(t->request->arg) : 0;
    for (;;) {
        int error=vfs_admit(t->vfs,0,t->request); if (error) return error;
        struct vfs_description *desc=h>=0 && h<(int)VFS_HANDLES ? t->handles[h] : 0;
        if (desc && (desc->io_active || desc->node->pins || (desc->node->identity && desc->node->identity->writer))) {
            if (!wait) { vfs_unlock(t->vfs); return -FS_EBUSY; }
            error=file_wait_locked(t->vfs,handle_mask(t,h),t->request,start,begin,0);
            vfs_unlock(t->vfs); if (error) return error; continue;
        }
        uint32_t mask=handle_mask(t,h); error=reserve_file_request(t->vfs,mask,t->request,desc ? desc->node->identity : 0);
        if (error) { vfs_unlock(t->vfs); return error; }
        desc=h>=0 && h<(int)VFS_HANDLES ? t->handles[h] : 0;
        if (handle_mask(t,h)==mask && (!desc || (!desc->io_active && !desc->node->pins &&
            (!desc->node->identity || !desc->node->identity->writer)))) return 0;
        vfs_unlock(t->vfs); /* slot was replaced or became busy while admission slept */
        if (!wait && handle_mask(t,h)==mask) return -FS_EBUSY;
    }
}
static int handle_lock(struct vfs_table *t,int h) { return handle_lock_mode(t,h,true); }
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
        e=handle_lock_mode(t,h,false);
        if (e==-FS_EBUSY) {
            /* Never wait on a busy file while holding the turn. */
            turn_release(t->vfs,taken);
            if ((e=handle_lock(t,h))) return e;
            vfs_unlock(t->vfs); continue;
        }
        if (e) { turn_release(t->vfs,taken); return e; }
        if (!(turn_drives(t->vfs,t->vfs->active)&~taken)) { *turn=taken; return 0; }
        writer_unlock(t->vfs,taken);
    }
}
static struct vfs_description *get(struct vfs_table *t, int h) { return h>=0 && h<(int)VFS_HANDLES ? t->handles[h] : 0; }
/* Only the operations below leave every other open regular file's chain
 * unchanged. Unknown mutations remain excluded. Compare common identities,
 * rather than personality-specific node addresses. */
static bool reader_admitted(struct vfs *v,unsigned drive,struct vfs_file_identity *id,
                            struct file_wait_evidence *evidence) {
    uint32_t mask=vfs_drive_mask(drive);
    /* f1-89: a transaction writing this file (its writer flag or a node
     * pin) excludes its readers through the unguarded transfer, and a
     * waiting publication step closes admission while in-flight readers
     * drain (Amendment 1 item 4). */
    if (id && id->writer) return false;
    for (unsigned i=0;i<VFS_NODES;i++) if (v->nodes[i].pins && v->nodes[i].identity==id) return false;
    if (v->publish_waiting[drive]) return false; /* one short publication step first */
    /* An unrelated in-flight mutation must not turn queued tickets into a
     * barrier for urgent readers. Actual same-file mutations remain unsafe
     * even during a reader turn. */
    if ((v->reserved|v->mutating)&mask) {
        bool compatible=v->io_file[drive] ? v->io_file[drive]->identity!=id : read_compatible(v->holds[drive].op);
        if (!compatible) file_wait_owner(v,mask,0,evidence);
        return compatible;
    }
    struct vfs_waiter *head=0;
    /* Only the serving operation can benefit from draining readers. A
     * younger ticket (including one waiting on another volume) cannot keep
     * closing admission. Unknown path mutations conservatively conflict. */
    for (struct vfs_waiter *w=v->waiters;w;w=w->next)
        if ((w->mask&mask) && w->ticket[drive]==v->serving[drive]) { head=w; break; }
    bool conflict=head && !read_compatible(head->owner.op) && (!head->file || head->file==id);
    bool runnable=head!=0;
    for (unsigned d=0;head && d<26;d++) if ((head->mask&(1u<<d)) &&
        (head->ticket[d]!=v->serving[d] || ((v->reserved|v->mutating)&(1u<<d)))) runnable=false;
    const struct vfs_request *r=fs_request_current();
    bool urgent=r && r->urgent_reader;
    uint32_t now=fs_now_ms();
    if (conflict && runnable && now-head->started>=VFS_URGENT_OVERTAKE_MS) {
        if (!v->urgent_gate_valid[drive]) {
            v->urgent_gate_valid[drive]=true; v->urgent_gate_start[drive]=now;
        }
        /* Share this clock across successive aged tickets. At most 10 ms
         * closed, followed by 10 ms in which urgent readers can enter. */
        if (!urgent || (now-v->urgent_gate_start[drive])%(2*VFS_URGENT_DRAIN_MS)<VFS_URGENT_DRAIN_MS) {
            if (evidence && !evidence->valid && meaningful_owner(&head->owner))
                *evidence=(struct file_wait_evidence){true,(char)('A'+drive),head->owner};
            return false;
        }
    } else if (!head) v->urgent_gate_valid[drive]=false;
    bool admitted=v->next[drive]==v->serving[drive] || urgent;
    if (!admitted && head && evidence && !evidence->valid && meaningful_owner(&head->owner))
        *evidence=(struct file_wait_evidence){true,(char)('A'+drive),head->owner};
    return admitted;
}
static void reader_begin(struct vfs *v,struct vfs_reader *reader,unsigned drive,const bool *busy) {
    /* A wait may have let another task replace namespace_owner, and the
     * namespace retake does not republish it. Build the record from this
     * reader's own request only: no task, operation or site is inherited.
     * A reader without an explicit site stays fallback-attributed. */
    const struct vfs_request *r=fs_request_current();
    *reader=(struct vfs_reader){.next=v->reader_owners,.busy=busy,.drive=drive,
        .owner={.task=fs_request_identity(r),.started=fs_now_ms(),.attribution=VFS_ATTR_OPERATION}};
    tag_copy(reader->owner.op,"read");
    tag_copy(reader->owner.site,r && r->site && *r->site ? r->site : "vfs");
    if (r && r->site && *r->site) reader->owner.attribution|=VFS_ATTR_SITE;
    v->reader_owners=reader;
}
static void reader_end(struct vfs *v,struct vfs_reader *reader) {
    struct vfs_reader **link=&v->reader_owners;
    while (*link!=reader) link=&(*link)->next;
    *link=reader->next;
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
    struct file_wait_evidence evidence={0};
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
    while (*busy || !reader_admitted(v,drive,id,&evidence)) {
        if (*busy) file_wait_owner(v,mask,busy,&evidence);
        int e=file_wait_locked(v,mask,r,start,begin,&evidence);
        if (e) { if (saved) vfs_reserve_locked(v,saved); return e; }
    }
    file_wait_complete(v,mask,r,start,&evidence,0);
    if (!vfs_identity_live(v,id) || v->volumes[drive]!=vol) {
        if (saved) vfs_reserve_locked(v,saved); return -FS_EIO;
    }
    struct fat_volume view=read_view(vol); struct fat_entry snapshot=*entry;
    if (!positioned) *pos=*current; /* shared-description offset after admission */
    int permission=validate ? validate(arg,*pos,n) : 0;
    if (permission) { if (saved) vfs_reserve_locked(v,saved); return permission; }
    struct vfs_reader reader; reader_begin(v,&reader,drive,busy);
    *busy=true; v->readers[drive]++; vfs_unlock(v);
    int e=fat_read_cursor(&view,&snapshot,cursor,*pos,buf,n,done);
    vfs_lock(v,0); reader_end(v,&reader); *busy=false; v->readers[drive]--; fs_event_wake(&v->volume_changed);
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
    int e=handle_lock(t,h); if (e) return e; operation_tag(t->vfs,"stat");
    struct vfs_description *d=get(t,h);
    e=!d ? -FS_EBADF : (d->node->generation!=t->vfs->generation[d->node->drive] || !vfs_identity_live(t->vfs,d->node->identity)) ? -FS_EIO : 0;
    if (!e) { if (entry) *entry=d->node->entry; if (position) *position=d->position; }
    vfs_unlock(t->vfs); return e;
}
int vfs_handle_version(struct vfs_table *t,int h,struct vfs_file_version *out) {
    if (!out) return -FS_EINVAL;
    int e=handle_lock(t,h); if (e) return e; operation_tag(t->vfs,"stat");
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
int vfs_version_pin(struct vfs_table *t,int h,const struct vfs_file_version *version,struct vfs_version_pin *pin) {
    if (!version || !pin) return -FS_EINVAL;
    *pin=(struct vfs_version_pin){0};
    struct vfs_file_version current;
    int e=vfs_handle_version(t,h,&current); if (e) return e;
    if (!vfs_file_version_equal(version,&current)) return -FS_EAGAIN;
    e=handle_lock(t,h); if (e) return e; operation_tag(t->vfs,"stat");
    struct vfs *v=t->vfs; struct vfs_description *d=get(t,h);
    if (!d || !d->node->volume) e=-FS_EBADF;
    else {
        fs_lock_take(&v->identity_lock);
        struct fat_volume *vol=d->node->volume;
        uint64_t mount=identity_generation(v,vol);
        if (mount!=version->mount || d->node->drive!=version->drive || v->generation[d->node->drive]!=version->drive_generation ||
            d->node->entry.parent!=version->parent || d->node->entry.index!=version->index ||
            d->node->entry.first!=version->first || d->node->entry.size!=version->size ||
            d->node->entry.times.write_date!=version->write_date || d->node->entry.times.write_time!=version->write_time ||
            change_generation(v,vol,version->parent,version->index)!=version->write_generation)
            e=-FS_EAGAIN;
        struct vfs_version_watch *slot=0,*free=0;
        for (unsigned i=0;i<VFS_VERSION_PINS;i++) {
            struct vfs_version_watch *w=&v->version_watches[i];
            if (!w->refs) free=w;
            else if (w->volume==vol && w->mount==mount && w->parent==version->parent && w->index==version->index) slot=w;
        }
        if (!e && !slot) {
            slot=free;
            if (!slot || v->next_version_watch==UINT64_MAX) e=-FS_ENOSPC;
            else *slot=(struct vfs_version_watch){.volume=vol,.mount=mount,.parent=version->parent,.index=version->index,
                .sequence=version->write_generation,.serial=++v->next_version_watch};
        }
        if (!e && slot->refs==UINT32_MAX) e=-FS_ENOSPC;
        if (!e) { slot->refs++; *pin=(struct vfs_version_pin){v,slot->serial}; }
        fs_lock_drop(&v->identity_lock);
    }
    vfs_unlock(v); return e;
}
void vfs_version_unpin(struct vfs_version_pin *pin) {
    if (!pin || !pin->vfs || !pin->serial) return;
    struct vfs *v=pin->vfs; fs_lock_take(&v->identity_lock);
    for (unsigned i=0;i<VFS_VERSION_PINS;i++) {
        struct vfs_version_watch *w=&v->version_watches[i];
        if (w->serial==pin->serial && w->refs) { if (!--w->refs) memset(w,0,sizeof(*w)); break; }
    }
    fs_lock_drop(&v->identity_lock); *pin=(struct vfs_version_pin){0};
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
    int admission=handle_lock(t,h); if (admission) return admission; operation_tag(t->vfs,"close"); int e=get(t,h) ? 0 : -FS_EBADF; if (!e) e=close_handle(t,(unsigned)h); vfs_unlock(t->vfs); return e;
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
    struct file_wait_evidence evidence={0};
    /* Open identities pin the chain against namespace removal. A published
     * writer names its node before dropping namespace. Never borrow a mutable
     * entry or volume diagnostic workspace across a different file's I/O. */
    for (;;) {
        int admission=vfs_admit(v,0,t->request); if (admission) return admission; operation_tag(t->vfs,"read");
        struct vfs_description *d=get(t,h);
        if (!d || d->node->ro || !(d->access&VFS_READ)) { vfs_unlock(v); break; }
        struct vfs_node *node=d->node; unsigned drive=node->drive;
        if (drive>=26 || node->generation!=v->generation[drive] || v->volumes[drive]!=node->volume || !vfs_identity_live(v,node->identity)) {
            vfs_unlock(v); return -FS_EIO;
        }
        uint32_t mask=vfs_drive_mask(drive);
        bool admitted=d->io_active ? false : reader_admitted(v,drive,node->identity,&evidence);
        if (d->io_active || !admitted) {
            if (d->io_active) file_wait_owner(v,mask,&d->io_active,&evidence);
            int error=file_wait_locked(v,mask,t->request,start,begin,&evidence);
            vfs_unlock(v); if (error) return error; continue;
        }
        file_wait_complete(v,mask,t->request,start,&evidence,0);
        int locked=table_range_check(t,node,d,d->position,n,false);
        if (locked) { vfs_unlock(v); return locked; }
        struct fat_volume *vol=node->volume;
        uint32_t media_generation=vol->dev->media_generation;
        struct fat_volume view=read_view(vol);
        struct vfs_reader reader; reader_begin(v,&reader,drive,&d->io_active);
        d->io_active=true; d->refs++; v->readers[drive]++; uint64_t position=d->position;
        struct fat_entry entry=node->entry;
        vfs_unlock(v);
        int e=fat_read_stream(&view,&entry,position,buf,n,done,&d->stream,&d->cursor);
        (void)fs_lock_retake(&v->lock,VFS_WAIT_NAMESPACE);
        reader_end(v,&reader); d->position+=*done; d->io_active=false; v->readers[drive]--;
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
    int admission=handle_lock(t,h); if (admission) return admission; operation_tag(t->vfs,"read"); struct vfs_description *d=get(t,h); int e;
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
    int admission=handle_lock_writer(t,h,&turn); if (admission) return admission; operation_tag(t->vfs,"write"); struct vfs_description *d=get(t,h); int e;
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
        /* A write slice is one in-flight claim set (256 clusters); a gap
         * before the position is extended in 128 KiB steps first. */
        size_t limit=(size_t)d->node->volume->spc*512*FAT_TRANSACTION_CLUSTERS, extend=limit<131072 ? limit : 131072;
        while (n && d->position>=(uint64_t)d->node->entry.size+limit && !e) {
            uint64_t end=(uint64_t)d->node->entry.size+extend;
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
            error=file_wait_locked(t->vfs,handle_mask(t,h),t->request,start,begin,0);
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
    int admission=handle_lock_writer(t,h,&turn); if (admission) return admission; operation_tag(t->vfs,"truncate"); struct vfs_description *d=get(t,h);
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
    int admission=handle_lock(t,h); if (admission) return admission; operation_tag(t->vfs,"commit"); struct vfs_description *d=get(t,h);
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
int vfs_flush_volume_until(struct vfs *vfs, unsigned drive, const struct vfs_request *request) {
    if (drive>=26) return -FS_EINVAL;
    struct fs_request_scope scope;
    fs_request_enter(&scope,request ? request : fs_request_current());
    int e=vfs_admit(vfs,vfs_drive_mask(drive),request);
    if (e) { fs_request_leave(&scope); return e; }
    operation_tag(vfs,"commit");
    struct fat_volume *v=vfs->volumes[drive];
    if (v && !v->readonly) e=vfs_fat_transaction_locked(vfs,v,(struct fat_transaction_args){.op=FAT_TX_COMMIT});
    else if (!v && !vfs->ro_volumes[drive]) e=-FS_ENOENT;
    vfs_unlock(vfs); fs_request_leave(&scope); return e;
}
int vfs_flush_drive(struct vfs_table *t,unsigned drive) {
    return vfs_flush_volume_until(t->vfs,drive,t->request);
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
    int admission=handle_lock_writer(t,h,&turn); if (admission) return admission; operation_tag(t->vfs,"log_reserve"); vfs_site_locked(t->vfs,"writer");
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
    int admission=handle_lock(t,h); if (admission) return admission; operation_tag(t->vfs,"log_write"); vfs_site_locked(t->vfs,"writer");
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
    int admission=handle_lock_writer(t,h,&turn); if (admission) return admission; operation_tag(t->vfs,"commit");
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
    int admission=handle_lock_writer(t,h,&turn); if (admission) return admission; operation_tag(t->vfs,"truncate");
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
    int admission=handle_lock_writer(t,h,&turn); if (admission) return admission; operation_tag(t->vfs,"truncate");
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
        int admission=path_lock(t,path,0,0); if (admission) return admission; operation_tag(t->vfs,"stat"); struct fs_ro_node n; unsigned d;
        int e=ro_path(t,path,&d,&n,0); if (!e && n.size>UINT32_MAX) e=-FS_EFBIG;
        if (!e) ro_entry(&n,out); vfs_unlock(t->vfs); return e;
    }
    int admission=path_lock_op(t,path,0,0,"stat"); if (admission) return admission; struct resolved r; int e=lookup(t,path,&r,out);
    if (!e) { struct vfs_node *n=node_find(t->vfs,t->vfs->volumes[r.drive],out); if (n) *out=n->entry; }
    vfs_unlock(t->vfs); return e;
}
static int metadata(struct vfs_table *t, const char *path, int attr, const struct fat_times *times) {
    int ro=ro_target(t,path); if (ro<0) return ro;
    if (ro) return -FS_EROFS;
    uint32_t turn; int admission=path_lock_writer(t,path,0,0,&turn); if (admission) return admission; operation_tag(t->vfs,"attrib"); struct resolved r; struct fat_entry ent; int e=lookup(t,path,&r,&ent);
    if (!e && ent.index==UINT32_MAX) e=-FS_EACCES;
    if (!e) {
        struct fat_volume *vol=t->vfs->volumes[r.drive]; struct vfs_node *n=node_find(t->vfs,vol,&ent);
        if (n) ent=n->entry;
        struct fat_entry before=ent;
        e=vfs_fat_transaction_locked(t->vfs,vol,(struct fat_transaction_args){.op=FAT_TX_SET_METADATA,
            .entry=&ent,.attr=attr<0 ? ent.attr : (uint8_t)attr,.times=times,.borrowed=true});
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
    while (!(e=vfs_dir_step_locked(t->vfs,t->vfs->volumes[f->drive],f->directory,0,&f->cursor,ent,(f->attr_mask&VFS_FIND_DOS)!=0))) {
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
        int admission=path_lock(t,p,0,0); if (admission) return admission; operation_tag(t->vfs,"find"); char parent[FS_PATH_BYTES];
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

    int admission=path_lock_op(t,p,0,0,"find"); if (admission) return admission; struct resolved r; int e=resolve(t,p,&r);
    if (!e && !r.name[0]) e=-FS_EINVAL;
    if (!e) { memset(f,0,sizeof(*f)); f->drive=r.drive; f->directory=r.parent; f->generation=t->vfs->generation[r.drive]; f->attr_mask=mask; memcpy(f->pattern,r.name,strlen(r.name)+1); e=find_next(t,f,ent); }
    vfs_unlock(t->vfs); return e;
}
int vfs_find_next(struct vfs_table *t, struct vfs_find *f, struct fat_entry *ent) {
    /* Tagged before the reservation: a find is admitted beside readers. */
    int admission=vfs_admit(t->vfs,0,t->request); if (admission) return admission; operation_tag(t->vfs,"find");
    admission=reserve_request(t->vfs,vfs_drive_mask(f->drive),t->request);
    if (admission) { vfs_unlock(t->vfs); return admission; }
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
    int e=vfs_fat_transaction_locked(v,vol,(struct fat_transaction_args){.op=FAT_TX_REMOVE,.entry=entry,.flag=dir,.borrowed=true});
    if (!e && px_legacy_changed) px_legacy_changed(v,vol,&before,0,false);
    return e;
}
static int remove_path(struct vfs_table *t, const char *path, bool dir) {
    int ro=ro_target(t,path); if (ro<0) return ro;
    if (ro) return -FS_EROFS;
    uint32_t turn; int admission=path_lock_writer(t,path,0,0,&turn); if (admission) return admission; operation_tag(t->vfs,"unlink"); struct resolved r; struct fat_entry ent; int e=lookup(t,path,&r,&ent);
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
    uint32_t turn; int admission=path_lock_writer(t,current,previous,oldest,&turn); if (admission) return admission; operation_tag(t->vfs,"rename");
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
        e=vfs_fat_transaction_locked(v,v->volumes[drive],(struct fat_transaction_args){.op=FAT_TX_PUBLISH,.entry=entry,.flag=true,.borrowed=true});
        if (px_legacy_changed) px_legacy_changed(v,v->volumes[drive],&entries[2],entry,false);
        entries[2]=*entry;
        if (!e) e=resolve(t,paths[i],&r);
        if (!e) e=vfs_fat_transaction_locked(v,v->volumes[drive],(struct fat_transaction_args){.op=FAT_TX_RENAME,.entry=entry,.directory=parent,.name=r.name,.borrowed=true});
        if (!e && px_legacy_changed) px_legacy_changed(v,v->volumes[drive],&entries[2],entry,false);
    }
    writer_unlock(v,turn); return e;
}
int vfs_delete(struct vfs_table *t, const char *p) { return remove_path(t,p,false); }
int vfs_rmdir(struct vfs_table *t, const char *p) { return remove_path(t,p,true); }
int vfs_mkdir(struct vfs_table *t, const char *p, uint8_t attr) {
    int ro=ro_target(t,p); if (ro<0) return ro;
    if (ro) return -FS_EROFS;
    uint32_t turn; int admission=path_lock_writer(t,p,0,0,&turn); if (admission) return admission; operation_tag(t->vfs,"mkdir"); struct resolved r; int e=resolve(t,p,&r); struct fat_entry ent;
    if (!e && !r.name[0]) e=-FS_EEXIST;
    if (!e) e=vfs_fat_transaction_locked(t->vfs,t->vfs->volumes[r.drive],(struct fat_transaction_args){.op=FAT_TX_CREATE,.directory=r.parent,.name=r.name,.attr=attr|FAT_ATTR_DIR,.entry=&ent});
    writer_unlock(t->vfs,turn); return e;
}
int vfs_rename(struct vfs_table *t, const char *from, const char *to) {
    int ro=ro_target(t,from); if (ro<0) return ro; if (ro) return -FS_EROFS;
    ro=ro_target(t,to); if (ro<0) return ro; if (ro) return -FS_EROFS;
    uint32_t turn; int admission=path_lock_writer(t,from,to,0,&turn); if (admission) return admission; operation_tag(t->vfs,"rename"); struct resolved a,b; struct fat_entry ent; int e=lookup(t,from,&a,&ent);
    if (!e) e=resolve(t,to,&b);
    if (!e && (!a.name[0] || !b.name[0])) e=-FS_EACCES;
    if (!e && a.drive!=b.drive) e=-FS_EXDEV;
    if (!e) {
        struct fat_volume *v=t->vfs->volumes[a.drive]; e=busy(t->vfs,v,&ent);
        if (!e && (ent.attr&FAT_ATTR_DIR)) { e=contains(t->vfs,v,ent.first,b.parent); if (e>0) e=-FS_EINVAL; }
        if (!e) {
            struct fat_entry before=ent;
            e=vfs_fat_transaction_locked(t->vfs,v,(struct fat_transaction_args){.op=FAT_TX_RENAME,.entry=&ent,.directory=b.parent,.name=b.name,.borrowed=true});
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

    int admission=path_lock_op(t,p,0,0,"chdir"); if (admission) return admission; struct resolved r; struct fat_entry ent; int e=lookup(t,p,&r,&ent);
    if (!e && !(ent.attr&FAT_ATTR_DIR)) e=-FS_ENOTDIR;
    if (!e) {
        char full[FS_PATH_BYTES], canonical[FS_PATH_BYTES]; unsigned d; e=normalize(t,p,&d,full);
        if (!e) {
            unsigned used=1; canonical[0]='/'; canonical[1]=0; uint32_t dir=root(t->vfs->volumes[d]); const char *s=full+1;
            while (*s && !e) {
                unsigned n=0; while (*s && *s!='/') r.name[n++]=*s++; r.name[n]=0;
                e=vfs_dir_step_locked(t->vfs,t->vfs->volumes[d],dir,r.name,0,&ent,false); if (e) break;
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
    if (v) { *cb=(uint32_t)v->spc*512; *bytes=(uint64_t)fat_free_estimate(v,0) * *cb; } /* fx-17b: bound while counting */
    vfs_unlock(t->vfs); return e;
}

/* G29d: program starts are queued here by exec (fdtable.c image_fat, the
 * namespace held) and written to CIUKIST.LOG later by the background flusher
 * (mount.c), as Windows 98's Task Monitor logs application starts
 * asynchronously. Before the queue, every launch wrote and committed the log
 * inside exec and so waited on C: publication (QEMU 56e58374). The record is
 * advisory: the queue is bounded, an overflow drops the oldest start, and a
 * stale identity is ignored at use (fat_startup.inc). */
void vfs_startup_record_locked(struct vfs *v,struct fat_volume *vol,const struct fat_entry *file,
                              const uint32_t *order,unsigned count)
{
    if (!count || count>VFS_STARTUP_READS || !vol || vol->readonly || !file->size || (file->attr&FAT_ATTR_DIR)) return;
    unsigned drive=26;
    for (unsigned d=0;d<26;d++) if (v->volumes[d]==vol) drive=d;
    if (drive==26) return;
    struct vfs_startup_hint hint={.volume=vol,.parent=file->parent,.index=file->index,.first=file->first,
        .size=file->size,.write_date=file->times.write_date,.write_time=file->times.write_time,
        .attr=file->attr,.count=(uint8_t)count};
    memcpy(hint.alias,file->alias,sizeof(hint.alias)-1);
    memcpy(hint.order,order,count*sizeof(*order));
    fs_lock_take(&v->identity_lock);
    hint.mount=identity_generation(v,vol);
    if (v->startup_count==VFS_STARTUP_QUEUE) { /* drop the oldest start */
        v->startup_head=(v->startup_head+1)%VFS_STARTUP_QUEUE; v->startup_count--; v->startup_dropped++;
    }
    v->startup_queue[(v->startup_head+v->startup_count)%VFS_STARTUP_QUEUE]=hint;
    v->startup_count++; v->startup_queued++; v->startup_last_ms=fs_now_ms();
    fs_lock_drop(&v->identity_lock);
    fs_event_wake(&v->startup_event);
}
bool vfs_startup_wait(struct vfs *v)
{
    fs_lock_take(&v->identity_lock);
    if (!v->startup_count) fs_event_wait(&v->startup_event,&v->identity_lock);
    bool queued=v->startup_count!=0;
    fs_lock_drop(&v->identity_lock);
    return queued;
}
/* fx-launch: the start log is written after the launch burst, once no
 * start has been queued for quiet_ms (a Win32 start queues its EXE and each
 * DLL over seconds; on T23 trial 2ca3b0b6 the flusher and CDPLAYER.EXE
 * waited on each other's C: holds in four consecutive windows), at the
 * latest max_ms after this call, and at once at 3/4 of the queue. */
void vfs_startup_settle(struct vfs *v,uint32_t quiet_ms,uint32_t max_ms)
{
    uint32_t start=fs_now_ms();
    fs_lock_take(&v->identity_lock);
    for (;;) {
        uint32_t now=fs_now_ms(), spent=now-start, idle=now-v->startup_last_ms;
        if (v->startup_count>=VFS_STARTUP_URGENT || spent>=max_ms || idle>=quiet_ms) break;
        uint32_t wait=quiet_ms-idle;
        if (wait>max_ms-spent) wait=max_ms-spent;
        fs_event_wait_ms(&v->startup_event,&v->identity_lock,wait);
    }
    fs_lock_drop(&v->identity_lock);
}
static void startup_entry(const struct vfs_startup_hint *h,struct fat_entry *e)
{
    memset(e,0,sizeof(*e));
    e->parent=h->parent; e->index=h->index; e->first=h->first; e->size=h->size; e->attr=h->attr;
    e->times.write_date=h->write_date; e->times.write_time=h->write_time;
    memcpy(e->alias,h->alias,sizeof(h->alias));
}
/* One volume's queued starts (batch[first..count) naming `vol` and `mount`):
 * ordinary exclusive handle, short FAT transactions, no volume reservation
 * across the log's device I/O. Returns the starts written or an error. */
static int startup_write(struct vfs *v,struct vfs_table *table,unsigned drive,struct fat_volume *vol,uint64_t mount,
                         const struct vfs_startup_hint *batch,unsigned first,unsigned count,void *data,void *spare)
{
    char path[]="A:/CIUKIST.LOG"; path[0]=(char)('A'+drive);
    int e=vfs_table_init(v,table,drive);
    if (e) return e;
    table->request=fs_request_current();
    int fd=vfs_open(table,path,VFS_READ|VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM);
    int applied=0;
    if (fd<0) e=fd;
    else {
        struct fat_entry log; size_t done=0, size=fat_startup_size(); uint64_t position;
        e=vfs_handle_stat(table,fd,&log,0);
        if (!e && (log.attr&(FAT_ATTR_DIR|FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM))!=(FAT_ATTR_HIDDEN|FAT_ATTR_SYSTEM)) e=-FS_EACCES;
        if (!e && log.size==size) e=vfs_read(table,fd,data,size,&done);
        for (unsigned i=first;!e && i<count;i++) {
            if (batch[i].volume!=vol || batch[i].mount!=mount) continue;
            struct fat_entry image; startup_entry(&batch[i],&image);
            memcpy(spare,data,size); /* a refused hint leaves the log as it was */
            if (fat_startup_prepare(vol,&image,batch[i].order,batch[i].count,data,done)) { memcpy(data,spare,size); continue; }
            done=size; applied++;
        }
        if (!e && applied) {
            e=vfs_seek(table,fd,0,VFS_SEEK_SET,&position);
            if (!e) e=vfs_write(table,fd,data,size,&done);
            if (!e && done!=size) e=-FS_EIO;
            if (!e && log.size>size) e=vfs_truncate(table,fd,size);
            if (!e) e=vfs_commit(table,fd);
        }
        int closed=vfs_close(table,fd);
        if (!e) e=closed;
    }
    vfs_table_destroy(table);
    return e ? e : applied;
}
int vfs_startup_flush(struct vfs *v,bool wait)
{
    fs_lock_take(&v->identity_lock);
    bool idle=!v->startup_count && !v->startup_flushing, busy=v->startup_flushing;
    fs_lock_drop(&v->identity_lock);
    if (idle) return 0;
    if (busy && !wait) return -FS_EBUSY;
    struct vfs_startup_hint *batch=fs_contig_alloc(sizeof(v->startup_queue));
    struct vfs_table *table=fs_contig_alloc(sizeof(*table));
    void *data=fs_page_alloc(), *spare=fs_page_alloc();
    int result=!batch || !table || !data || !spare ? -FS_ENOMEM : 0;
    unsigned count=0;
    fs_lock_take(&v->identity_lock);
    if (result) { /* advisory: drop the queue once instead of retrying it */
        v->startup_failed+=v->startup_count; v->startup_head=v->startup_count=0;
    } else {
        /* A flush in progress holds the log open; only the start-log task
         * waits for it, then takes what remains. */
        while (wait && v->startup_flushing) fs_event_wait(&v->startup_event,&v->identity_lock);
        if (v->startup_flushing) result=-FS_EBUSY;
        else {
            for (;count<v->startup_count;count++)
                batch[count]=v->startup_queue[(v->startup_head+count)%VFS_STARTUP_QUEUE];
            v->startup_head=v->startup_count=0;
            v->startup_flushing=count!=0;
        }
    }
    fs_lock_drop(&v->identity_lock);
    unsigned written=0, failed=0;
    for (unsigned i=0;i<count;i++) {
        struct fat_volume *vol=batch[i].volume;
        if (!vol) continue; /* already written with an earlier start of its volume */
        uint64_t mount=batch[i].mount; unsigned drive=26, starts=0;
        for (unsigned j=i;j<count;j++) if (batch[j].volume==vol && batch[j].mount==mount) starts++;
        vfs_lock(v,0);
        for (unsigned d=0;d<26;d++) if (v->volumes[d]==vol) drive=d;
        bool live=drive<26 && !vol->readonly && identity_generation(v,vol)==mount;
        vfs_unlock(v);
        int e=live ? startup_write(v,table,drive,vol,mount,batch,i,count,data,spare) : -FS_ENOENT;
        if (e>=0) { written+=(unsigned)e; failed+=starts-(unsigned)e; }
        else { failed+=starts; if (!result) result=e; }
        for (unsigned j=i;j<count;j++) if (batch[j].volume==vol && batch[j].mount==mount) batch[j].volume=0;
    }
    if (count) {
        fs_lock_take(&v->identity_lock);
        v->startup_flushing=false; v->startup_written+=written; v->startup_failed+=failed;
        fs_lock_drop(&v->identity_lock);
        fs_event_wake(&v->startup_event);
    }
    fs_contig_free(batch,sizeof(v->startup_queue)); fs_contig_free(table,sizeof(*table));
    fs_page_free(data); fs_page_free(spare);
    return result ? result : (int)written;
}
