/* Grant-scoped fences and faultable staging outside display locks.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/desktop.h>
#include <ciuki/display_batch.h>
#include <ciuki/signal.h>
struct batch_fence {
    struct batch_fence *next, *recovery_next;
    struct display_batch batch;
    struct desktop_description *refs[128];
    struct surface *reserved[128];
    unsigned ref_count, reserved_count;
    uint32_t blob_bytes;
    unsigned users;
    bool released, queued;
    bool busy;
};
static struct batch_fence *recovery_queue;
static struct task *reaper;
static bool reaper_starting;
static struct kwait reaper_wait;
static void free_staging(struct batch_fence *f)
{
    if (f->busy) { grant_batch_busy(false); f->busy=false; }
    for (unsigned i=0;i<f->reserved_count;i++) surface_batch_unreserve(f->reserved[i]);
    f->reserved_count=0;
    for (unsigned i=0;i<f->ref_count;i++) f->refs[i]->object.release(&f->refs[i]->object);
    f->ref_count=0;
    desktop_buffer_free((void *)f->batch.blob,f->blob_bytes,MM_IO_REQUEST); f->batch.blob=0;
}
static void drop_fence(struct batch_fence *f)
{
    if (f && !--f->users && f->released) {
        free_staging(f);
        desktop_buffer_free(f,sizeof(*f),MM_IO_REQUEST);
    }
}
static void enqueue_recovery(struct batch_fence *f)
{
    if (f->queued) return;
    f->queued=true; f->users++;
    f->recovery_next=recovery_queue; recovery_queue=f;
    kwait_wake_all(&reaper_wait);
}
static bool reap_pending(void *arg) { (void)arg; return recovery_queue!=NULL; }
static void reap_once(void)
{
    for (struct batch_fence *f=recovery_queue;f;) {
        struct batch_fence *next=f->recovery_next;
        if (fbdev_batch_recover(&f->batch)) {
            /* Another caller may have prepended a job during a wait slice. */
            struct batch_fence **link=&recovery_queue;
            while (*link && *link!=f) link=&(*link)->recovery_next;
            if (*link) *link=f->recovery_next;
            f->queued=false;
            free_staging(f); drop_fence(f);
        }
        f=next;
    }
}
static void reap_main(void *arg)
{
    (void)arg;
    for (;;) {
        (void)kwait_wait_until(&reaper_wait,reap_pending,0,deadline_after_ms(1000));
        reap_once();
        if (recovery_queue) task_sleep_ms(1);
    }
}
void grant_batch_close(struct desktop_description *d)
{
    struct batch_fence *f=d->display_fences; d->display_fences=NULL;
    while (f) {
        struct batch_fence *next=f->next;
        f->batch.cancel=true; f->released=true;
        if (f->batch.reply.state==CIUKI_FENCE_RECOVERING) enqueue_recovery(f);
        if (!f->users) { free_staging(f); desktop_buffer_free(f,sizeof(*f),MM_IO_REQUEST); }
        f=next;
    }
}
bool grant_batch_ram(const struct process *p, uint32_t start, uint32_t bytes)
{
    uint64_t end=(uint64_t)start+bytes;
    if (!bytes || end>UINT32_MAX) return false;
    for (uint64_t at=start;at<end;) {
        const struct ua_extent *e=ua_find(p->memory,(uint32_t)at);
        if (!e || e->preparing || e->kind==UA_SHARED || e->kind==UA_GUARD || e->end<=at) return false;
        at=e->end;
    }
    return true;
}
static bool intersects(uint32_t a, uint32_t n, uint32_t b, uint32_t m)
{ return (uint64_t)a+n>b && (uint64_t)b+m>a; }
/* Pinning prepares residency/COW before execution. Capture direct-map reply
 * fragments while pinned: publication below performs only memcpy, no fault,
 * page walk, allocation or post-drawing copy_to_user. */
struct batch_transport {
    struct process *process;
    struct desktop_description *grant;
    struct batch_fence *fence;
    struct batch_fence *staging;
    struct batch_fence *control;
    struct ua_pin pin;
    bool pinned, busy, creating_reaper;
    uint8_t *reply[2];
    unsigned first;
};
static int reply_pin(struct batch_transport *t, uint32_t address, uint32_t bytes)
{
    if ((uint64_t)address+bytes>UINT32_MAX) return -EOVERFLOW;
    if (bytes!=64 || !grant_batch_ram(t->process,address,bytes)) return -EFAULT;
    int e=ua_pin(t->process->memory,&t->pin,address,bytes,true); if (e) return e;
    t->pinned=true;
    unsigned n=PAGE_SIZE-(address&(PAGE_SIZE-1)); t->first=n<64 ? n : 64;
    uint32_t phys=as_lookup(&t->process->memory->as,address,0);
    if (!phys) return -EFAULT;
    t->reply[0]=P2V(phys);
    if (t->first<64) {
        phys=as_lookup(&t->process->memory->as,address+t->first,0);
        if (!phys) return -EFAULT;
        t->reply[1]=P2V(phys);
    }
    return 0;
}
static void reply_publish(struct batch_transport *t, const struct ciuki_batch_reply *q)
{
    memcpy(t->reply[0],q,t->first);
    if (t->first<64) memcpy(t->reply[1],(const uint8_t *)q+t->first,64-t->first);
}
static bool interrupted(void *arg);
static int authority(void *arg)
{
    struct batch_transport *t=arg;
    int e=grant_batch_authorized(t->process,t->grant);
    return e ? e : interrupted(t) ? -EINTR : 0;
}
static bool interrupted(void *arg)
{
    struct batch_transport *t=arg;
    struct proc_thread *thread=proc_thread_for(g_current);
    return t->process->state!=PROC_LIVE || (thread && proc_signal_caught(thread));
}
static void transport_cleanup(void *arg)
{
    struct batch_transport *t=arg;
    if (t->creating_reaper) {
        uint32_t flags=irq_save(); reaper_starting=false; irq_restore(flags);
    }
    if (t->staging) {
        free_staging(t->staging);
        desktop_buffer_free(t->staging,sizeof(*t->staging),MM_IO_REQUEST);
    }
    if (t->fence) {
        struct display_batch *b=&t->fence->batch;
        b->interrupted=0; b->authority=0; b->interrupted_arg=0;
        if (b->reply.state==CIUKI_FENCE_RUNNING) fbdev_batch_abort(b);
        if (!b->reply.admitted) {
            /* Teardown while waiting to acquire presenter is still refusal:
             * this sequence never became an admitted fence or owned pixels. */
            struct batch_fence **link=(struct batch_fence **)&t->grant->display_fences;
            while (*link && *link!=t->fence) link=&(*link)->next;
            if (*link) *link=t->fence->next;
            t->fence->released=true;
        }
        if (b->reply.state==CIUKI_FENCE_RECOVERING) {
            if (t->busy) { t->fence->busy=true; t->busy=false; }
            enqueue_recovery(t->fence);
        } else free_staging(t->fence);
    }
    if (t->pinned) ua_unpin(t->process->memory,&t->pin);
    if (t->busy) grant_batch_busy(false);
    if (t->control) fbdev_batch_recovery_abandon(&t->control->batch);
    drop_fence(t->control);
    drop_fence(t->fence);
    t->grant->object.release(&t->grant->object);
}
static int ensure_reaper(struct batch_transport *t)
{
    uint32_t flags=irq_save();
    if (reaper) { irq_restore(flags); return 0; }
    if (reaper_starting) { irq_restore(flags); return -EBUSY; }
    reaper_starting=true; t->creating_reaper=true;
    irq_restore(flags);
    kwait_init(&reaper_wait);
    struct task *worker=task_create_kernel("display-recovery",reap_main,0,P_NORMAL);
    flags=irq_save();
    if (worker) { reaper=worker; task_start(worker); }
    reaper_starting=false; t->creating_reaper=false;
    irq_restore(flags);
    return worker ? 0 : -ENOMEM;
}
static int resolve_targets(struct batch_transport *t, struct batch_fence *f)
{
    struct display_batch *b=&f->batch;
    for (unsigned i=0;i<b->header.target_count;i++) {
        struct ciuki_batch_target desc;
        memcpy(&desc,b->blob+b->header.targets_offset+i*32,32);
        const struct fb_device *screen=fbdev_get();
        if (desc.flags || desc.kind<1 || desc.kind>3) return -EINVAL;
        for (unsigned n=0;n<4;n++) if (desc.reserved[n]) return -EINVAL;
        if (desc.kind==CIUKI_TARGET_SCANOUT) {
            if (desc.handle || desc.generation!=b->header.generation) return -EINVAL;
            if (!screen->present) return -ENODEV;
            if (desc.generation!=screen->generation) return -EAGAIN;
            b->targets[i]=(struct swdib_target){.device=*screen,.identity=screen,.rights=PROT_READ|PROT_WRITE,.kind=1};
        } else if (desc.kind==CIUKI_TARGET_SYSTEM) {
            struct desktop_description *d=desktop_fd(t->process,(int32_t)desc.handle);
            if (!desc.generation) return -EAGAIN;
            int e=surface_batch_target(d,desc.generation,&b->targets[i]); if (e) return e;
            d->object.retain(&d->object); f->refs[f->ref_count++]=d;
        } else {
            struct desktop_description *lease=desktop_fd(t->process,(int32_t)desc.handle);
            if (!lease || lease->kind!=DESKTOP_DISPLAY) return -EBADF;
            if (lease!=t->grant || lease->owner!=t->process->pid) return -EACCES;
            int e=fbdev_batch_video_target(desc.generation,&b->targets[i]); if (e) return e;
            /* The grant owns this complete driver lease. Admission transfers
             * its pin to fbdev's in-flight owner; mode/stop cannot reclaim it
             * before completion. Grant destruction transfers that same pin
             * to the internal recovery owner, without a self-reference cycle. */
        }
    }
    return 0;
}
static int reserve_targets(struct batch_fence *f)
{
    struct surface *objects[128]; unsigned count=0;
    for (unsigned i=0;i<f->ref_count;i++) {
        if (f->refs[i]->kind!=DESKTOP_SURFACE) continue;
        struct surface *s=f->refs[i]->u.surface;
        unsigned at=0; while (at<count && (uintptr_t)objects[at]<(uintptr_t)s) at++;
        if (at<count && objects[at]==s) continue;
        for (unsigned n=count;n>at;n--) objects[n]=objects[n-1];
        objects[at]=s; count++;
    }
    for (unsigned i=0;i<count;i++) {
        int e=surface_batch_reserve(objects[i]); if (e) return e;
        f->reserved[f->reserved_count++]=objects[i];
    }
    return 0;
}
static int submit(struct batch_transport *t, uint32_t address)
{
    struct ciuki_batch_submit q;
    if ((uint64_t)address+32>UINT32_MAX) return -EOVERFLOW;
    if (!grant_batch_ram(t->process,address,32) || copy_from_user(&q,address,32)) return -EFAULT;
    if (q.version!=1) return -EOPNOTSUPP;
    if (q.size!=32 || q.flags || q.reserved[0] || q.reserved[1]) return -EINVAL;
    if ((uint64_t)q.input_address+q.input_bytes>UINT32_MAX || (uint64_t)q.reply_address+q.reply_bytes>UINT32_MAX) return -EOVERFLOW;
    if (intersects(address,32,q.input_address,q.input_bytes) || intersects(address,32,q.reply_address,q.reply_bytes) ||
        intersects(q.input_address,q.input_bytes,q.reply_address,q.reply_bytes)) return -EINVAL;
    int e=reply_pin(t,q.reply_address,q.reply_bytes); if (e) return e;
    struct ciuki_batch_reply refused={.size=64,.version=1};
    if (q.input_bytes>CIUKI_BATCH_MAX_BYTES) { e=-E2BIG; goto refused; }
    if (q.input_bytes<64 || (q.input_bytes&7)) { e=-EINVAL; goto refused; }
    if (!grant_batch_ram(t->process,q.input_address,q.input_bytes)) { e=-EACCES; goto refused; }
    struct batch_fence *f=desktop_buffer_alloc(sizeof(*f),MM_IO_REQUEST);
    if (!f) { e=-ENOMEM; goto refused; }
    t->staging=f;
    f->batch.reply=refused;
    f->batch.preflight_pause=kpreempt_point;
    f->blob_bytes=q.input_bytes;
    f->batch.blob=desktop_buffer_alloc(q.input_bytes,MM_IO_REQUEST);
    if (!f->batch.blob) { e=-ENOMEM; goto refused; }
    if (copy_from_user((void *)f->batch.blob,q.input_address,q.input_bytes)) e=-EFAULT;
    else e=display_batch_header(f->batch.blob,q.input_bytes,&f->batch.header);
    if (!e) e=resolve_targets(t,f);
    if (!e) e=display_batch_validate(&f->batch);
    unsigned fences=0;
    for (struct batch_fence *p=t->grant->display_fences;p;p=p->next) fences++;
    if (!e && (fences==64 || t->grant->display_sequence==UINT64_MAX)) e=-ENOSPC;
    if (!e) e=ensure_reaper(t);
    if (!e) e=reserve_targets(f);
    /* Creation/prefaulting may yield; reserve the quota and sequence again
     * immediately before the non-yielding fence publication. */
    fences=0;
    for (struct batch_fence *p=t->grant->display_fences;p;p=p->next) fences++;
    if (!e && (fences>=64 || t->grant->display_sequence==UINT64_MAX)) e=-ENOSPC;
    if (e) goto refused;
    f->batch.interrupted=interrupted; f->batch.interrupted_arg=t; f->batch.authority=authority;
    t->fence=f;
    t->staging=0;
    f->users=1;
    /* Reserve the sequence/slot before pixels; publish the job before slices
     * permit teardown. An admission refusal returns no allocated fence. */
    f->batch.reply.fence=++t->grant->display_sequence;
    f->batch.reply.state=CIUKI_FENCE_RUNNING;
    f->next=t->grant->display_fences; t->grant->display_fences=f;
    grant_batch_busy(true); t->busy=true;
    e=fbdev_batch_execute(&f->batch);
    if (!f->batch.reply.admitted) {
        t->grant->display_fences=f->next;
        t->fence=0; free_staging(f); desktop_buffer_free(f,sizeof(*f),MM_IO_REQUEST);
        goto refused;
    }
    reply_publish(t,&f->batch.reply);
    return e;
refused:
    refused.error=e; reply_publish(t,&refused); return e;
}
static int control(struct batch_transport *t, uint32_t address)
{
    struct ciuki_fence_control q;
    if ((uint64_t)address+32>UINT32_MAX) return -EOVERFLOW;
    if (!grant_batch_ram(t->process,address,32) || copy_from_user(&q,address,32)) return -EFAULT;
    if (q.version!=1) return -EOPNOTSUPP;
    if (q.size!=32 || q.reserved || q.action<1 || q.action>4 || q.timeout_ms>1000 ||
        (q.action!=CIUKI_FENCE_WAIT && q.timeout_ms) || intersects(address,32,q.reply_address,q.reply_bytes)) return -EINVAL;
    int e=reply_pin(t,q.reply_address,q.reply_bytes); if (e) return e;
    struct batch_fence **at=(struct batch_fence **)&t->grant->display_fences;
    while (*at && (*at)->batch.reply.fence!=q.fence) at=&(*at)->next;
    if (!*at) return -ENOENT;
    struct batch_fence *f=*at;
    f->users++; t->control=f;
    if (q.action==CIUKI_FENCE_CANCEL) { f->batch.cancel=true; f->batch.reply.flags|=CIUKI_FENCE_CANCEL_REQUESTED; }
    if (q.action==CIUKI_FENCE_WAIT) {
        uint64_t deadline=deadline_after_ms(q.timeout_ms);
        while (f->batch.reply.state==CIUKI_FENCE_RECOVERING || f->batch.reply.state==CIUKI_FENCE_RUNNING) {
            if (!q.timeout_ms || g_ticks>=deadline) { e=-ETIMEDOUT; break; }
            if (interrupted(t)) { e=-EINTR; break; }
            if (f->batch.reply.state==CIUKI_FENCE_RECOVERING && fbdev_batch_recover(&f->batch)) { free_staging(f); break; }
            task_sleep_ms(1);
        }
    }
    if (q.action==CIUKI_FENCE_RELEASE) {
        if (f->batch.reply.state==CIUKI_FENCE_RUNNING || f->batch.reply.state==CIUKI_FENCE_RECOVERING) e=-EBUSY;
        else {
            /* WAIT may have yielded while a second thread released another
             * fence. Resolve the list link again; retained metadata stays
             * alive until every control call has published its snapshot. */
            struct batch_fence **link=(struct batch_fence **)&t->grant->display_fences;
            while (*link && *link!=f) link=&(*link)->next;
            if (*link) *link=f->next;
            f->released=true; reply_publish(t,&f->batch.reply); return 0;
        }
    }
    reply_publish(t,&f->batch.reply);
    return e;
}
int grant_batch_request(struct process *p, int32_t fd, uint32_t address, bool fence)
{
    struct desktop_description *d=desktop_fd(p,fd);
    if (!d || d->kind!=DESKTOP_DISPLAY) return -EBADF;
    if (d->owner!=p->pid || !(d->maximum&PROT_WRITE)) return -EACCES;
    int e=fence ? 0 : grant_batch_authorized(p,d); if (e) return e;
    struct batch_transport t={.process=p,.grant=d};
    d->object.retain(&d->object);
    struct proc_thread *thread=proc_thread_for(g_current);
    if (thread) { thread->cleanup=transport_cleanup; thread->operation=&t; }
    if (!fence && interrupted(&t)) e=-EINTR;
    else e=fence ? control(&t,address) : submit(&t,address);
    if (thread) { thread->cleanup=0; thread->operation=0; }
    transport_cleanup(&t);
    return e;
}
