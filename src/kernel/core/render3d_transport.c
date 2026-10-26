/* F7 Win32 staging to the existing display watchdog/worker. GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/desktop.h>
#include <ciuki/display.h>
#include <ciuki/w32_core.h>
#include "../drivers/video/render3d_transport.h"
struct wire_job {
    struct ciuki_w32_render3d_args args;
    uint32_t process, refs;
    void *data;
    bool queued, done, cancelled;
    int error;
};
static struct wire_job *pending, *staging;
static struct render3d_service *service;
static bool polling;
static struct ciuki_r3d_caps caps_snapshot;
static uint32_t resident_bytes;
/* One production ledger, including the service itself, wire jobs, staging,
 * allocation prefixes and retained quarantine. Reserve physical pages before
 * allocation; neither a failed allocation nor a free may lose its charge. */
static void *ralloc(void *ctx,size_t bytes)
{
    (void)ctx;
    if (!bytes || bytes>R3D_RAM_LIMIT-8) return 0;
    uint32_t extent=(uint32_t)bytes+8;
    uint32_t charge=(extent+PAGE_SIZE-1)&~(PAGE_SIZE-1);
    uint32_t flags=irq_save();
    bool fits=charge<=R3D_RAM_LIMIT-resident_bytes;
    if (fits) resident_bytes+=charge;
    irq_restore(flags);
    if (!fits) return 0;
    uint32_t *p=desktop_buffer_alloc(extent,MM_IO_REQUEST);
    if (!p) {
        flags=irq_save(); resident_bytes-=charge; irq_restore(flags);
        return 0;
    }
    p[0]=extent; p[1]=charge; return p+2;
}
static void rfree(void *ctx,void *data)
{
    (void)ctx;
    if (data) {
        uint32_t *p=(uint32_t *)data-2, charge=p[1];
        desktop_buffer_free(p,p[0],MM_IO_REQUEST);
        uint32_t flags=irq_save(); resident_bytes-=charge; irq_restore(flags);
    }
}
static uint64_t now(void *ctx) { (void)ctx; return g_ticks*1000; }
static void drop(struct wire_job *j)
{
    uint32_t flags=irq_save();
    bool last=!--j->refs; irq_restore(flags);
    if (!last) return;
    rfree(0,j->data);
    rfree(0,j);
}
static void cleanup(void *arg)
{
    struct wire_job *j=arg;
    uint32_t flags=irq_save();
    j->cancelled=true;
    if (staging==j) staging=0;
    bool dequeued=j->queued && pending==j;
    if (dequeued) { pending=0; j->queued=false; }
    irq_restore(flags);
    if (dequeued) drop(j);
    drop(j);
}
extern int display_present_render3d(void *,uint32_t,const struct ciuki_w32_render3d_args *,const void *,uint32_t);
void w32_render3d_poll(void)
{
    if (polling) return;
    polling=true;
    if (service) {
        for (unsigned i=0;i<R3D_CONTEXTS;i++) {
            struct render3d_context *c=&service->contexts[i];
            struct process *p=c->live ? proc_find(c->process) : 0;
            if (c->live && (!p || p->state!=PROC_LIVE)) render3d_process_exit(service,c->process);
        }
        (void)render3d_step(service);
    }
    uint32_t flags=irq_save();
    struct wire_job *j=pending; pending=0;
    if (j) j->queued=false;
    irq_restore(flags);
    if (j) {
        struct process *p=proc_find(j->process);
        int e=j->cancelled || !p || p->state!=PROC_LIVE ? -ECANCELED : 0;
        if (!e && service && service->generation!=fbdev_get()->generation) {
            /* Owner's mode hook already cancels/losses its device. Retain
             * quarantined service memory; an unproven writer cannot be freed. */
            e=render3d_close(service);
            if (!e) { rfree(0,service); service=0; }
        }
        if (!e && !service) {
            service=ralloc(0,sizeof(*service));
            struct render3d_host h={.alloc=ralloc,.free=rfree,.now_us=now};
            if (!service) e=-ENOMEM;
            else {
                e=render3d_native_init(service,&h);
                if (e) { rfree(0,service); service=0; }
            }
        }
        if (!e) {
            struct render3d_transport t={service,0,display_present_render3d};
            e=render3d_transport_dispatch(&t,j->process,&j->args,j->data,j->args.bytes);
        }
        p=proc_find(j->process);
        if (service && (j->cancelled || !p || p->state!=PROC_LIVE)) render3d_process_exit(service,j->process);
        flags=irq_save(); j->error=e; j->done=true; irq_restore(flags); drop(j);
    }
    uint32_t saved=irq_save();
    if (service) render3d_wire_caps(service,&caps_snapshot);
    else caps_snapshot=(struct ciuki_r3d_caps){0};
    polling=false;
    irq_restore(saved);
}
uint32_t w32_render3d_call(struct proc_thread *t,struct ciuki_w32_render3d_args *a)
{
    if (!render3d_wire_valid(a) || (a->action!=CIUKI_R3D_PRESENT && a->bytes &&
        (!a->address || (uint64_t)a->address+a->bytes>UINT32_MAX))) { a->error=EINVAL; return 0; }
    if (a->action==CIUKI_R3D_QUERY) {
        struct ciuki_r3d_caps out;
        uint32_t flags=irq_save();
        bool cached=caps_snapshot.version && caps_snapshot.generation==fbdev_get()->generation;
        if (cached) {
            /* Read only, bounded qualification predicates. A worker in flight
             * publishes no hardware authority until its mutation completes. */
            if (service && !polling) render3d_wire_caps(service,&caps_snapshot);
            render3d_filter_caps(&caps_snapshot,a,&out);
            if (polling) out.features=0;
        }
        irq_restore(flags);
        if (cached) {
            int e=ua_write(t->process->memory,a->address,&out,sizeof(out));
            a->error=e<0 ? (uint32_t)-e : 0; a->result=!e;
            return 0;
        }
    }
    if (display_worker_start()) { a->error=ENOMEM; return 0; }
    struct wire_job *j=ralloc(0,sizeof(*j));
    if (!j) { a->error=ENOMEM; return 0; }
    memset(j,0,sizeof(*j)); j->args=*a; j->process=t->process->pid; j->refs=1;
    t->cleanup=cleanup; t->operation=j;
    bool output=a->action==CIUKI_R3D_QUERY || a->action==CIUKI_R3D_READ;
    uint32_t flags=irq_save();
    int e=staging ? -EAGAIN : 0;
    if (!e) staging=j;
    irq_restore(flags);
    if (!e && a->bytes) {
        j->data=ralloc(0,a->bytes);
        if (!j->data) e=-ENOMEM;
        else if (output || a->action==CIUKI_R3D_PRESENT) memset(j->data,0,a->bytes);
        else e=ua_read(t->process->memory,j->data,a->address,a->bytes);
    }
    flags=irq_save();
    if (!e && pending) e=-EAGAIN;
    if (!e) { j->refs++; j->queued=true; pending=j; }
    irq_restore(flags);
    if (!e) display_worker_wake();
    uint64_t start=g_ticks;
    for (unsigned i=0;!e && !j->done && i<4096;i++) {
        if (g_ticks-start>=2000) { e=-ETIMEDOUT; break; }
        task_sleep_ms(1);
    }
    if (!e && !j->done) e=-ETIMEDOUT;
    if (!e) e=j->error;
    if (!e && output) e=ua_write(t->process->memory,a->address,j->data,a->bytes);
    if (!e) { *a=j->args; a->result=1; }
    a->error=e<0 ? (uint32_t)-e : 0;
    t->cleanup=0; t->operation=0; cleanup(j);
    return 0;
}
