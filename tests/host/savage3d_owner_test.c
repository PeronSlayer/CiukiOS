/* Production 3D owner against the real keyed-marker implementation and the
 * unchanged committed/cache VFS fixture. Each variant is a separate boot.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main old_marker_main
#define klog old_marker_log
#include "display_attempt_test.c"
#undef klog
#undef main
#include <stdlib.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#include "../../src/kernel/drivers/video/s3savage/savage3d.h"
#define THREE_KEY "C:/system/settings/accel-3d-s3savage-5333-8c2e.cfg"
struct ciuki_boot_info g_boot;
static struct pci_func pci3={.vendor=0x5333,.device=0x8c2e,.revision=1,.command=3,
    .bar={0xd0000000,0xe0000000,0xf0000000}};
static struct fb_device fb3={.width=1024,.height=768,.pitch=4096,.size=3145728,
    .phys=0xe0000000,.bpp=32,.present=true,.generation=42};
static struct savage2d engine3;
static struct resource claims3[2];
static unsigned variant3, claimed3, released3, quarantined3, locked3, stores3, packets3, logs3;
static bool prepared3;
static void *allocations3[512];
static unsigned allocations_count3, reserves3;
static void (*worker3)(void *);
static bool frame3, worker_running3;
static uint64_t now3;
static unsigned prepares3;
static uint32_t heap_first3=0x800000;
static struct task worker_task3;
struct task *g_current;
static unsigned reaped3, marker_checks3, optional_calls3[2], fullsize_calls3, d24_logs3;
void task_reap(struct task *t)
{ assert(t==&worker_task3 && t!=g_current && t->state==T_ZOMBIE && !reaped3); reaped3++; t->state=T_DEAD; }
void kwait_init(struct kwait *q) { memset(q,0,sizeof(*q)); }
bool kwait_wait_until(struct kwait *q,kwait_cond_fn cond,void *arg,uint64_t deadline)
{ (void)q; assert(worker_running3 && frame3 && deadline==UINT64_MAX && cond(arg)); return true; }
void kwait_wake_all(struct kwait *q) { (void)q; assert(frame3); }
struct task *task_create_kernel(const char *name,void (*fn)(void *),void *arg,enum task_prio prio)
{
    assert(!worker3 && !locked3 && !arg && prio==P_IDLE && !strcmp(name,"savage-3d-probe"));
    worker3=fn; worker_task3=(struct task){.state=T_READY}; return &worker_task3;
}
void task_start(struct task *t) { assert(t==&worker_task3 && (!frame3 || variant3==18)); }
void task_yield(void)
{
    assert(worker_running3 && frame3 && !locked3);
    if ((variant3==15 || variant3==16) && size_of(THREE_KEY)) now3+=variant3==15 ? 600000 : 5000000;
}
void task_sleep_ms(uint32_t ms) { assert(ms==1 && !locked3); now3+=ms*1000; }
/* The real blkreq.c request scope (built with FS_HOST) converts the owner's
 * deadline exactly as the block layer does for every marker VFS operation. */
uint32_t blk_request_remaining_ms(void);
bool blk_request_interrupted(void);
static void marker_deadline3(void)
{
    const struct vfs_request *r=fs_request_current();
    if (!r) { assert(!worker_running3); return; } /* fixture-side retry setup */
    assert(!locked3 && worker_running3 && r->background && !strcmp(r->operation,"3d-probe-marker"));
    uint32_t left=blk_request_remaining_ms();
    assert(left>=SAVAGE3D_PROBE_MARKER_US/2000 && left<=SAVAGE3D_PROBE_MARKER_US/1000 && !blk_request_interrupted());
    marker_checks3++;
}
void klog(const char *fmt,...)
{
    assert(!locked3); logs3++;
    char line[512]; va_list ap; va_start(ap,fmt); vsnprintf(line,sizeof(line),fmt,ap); va_end(ap);
    assert(strstr(line,"[3d] probe") || strstr(line,"[3d] fullsize") || strstr(line,"[3d] precision") ||
        strstr(line,"[3d] attachment") || strstr(line,"[accel] retry "));
    if ((variant3==5 || variant3==11) && strstr(line,"[3d] probe result=")) assert(strstr(line,"marker=unknown"));
    if (strstr(line,"d24s8") || strstr(line,"D24S8")) d24_logs3++;
    if (variant3==22 && strstr(line,"[3d] probe result=")) assert(strstr(line,"reason=budget_phase") && strstr(line,"marker=retained"));
}
void *kmalloc(size_t bytes)
{
    assert(!locked3);
    if (variant3==9 && allocations_count3==2) return 0;
    void *p=calloc(1,bytes); assert(p && allocations_count3<512);
    allocations3[allocations_count3++]=p; return p;
}
void kfree(void *p)
{
    assert(!locked3);
    for (unsigned i=0;i<allocations_count3;i++) if (allocations3[i]==p) { allocations3[i]=0; free(p); return; }
    assert(!p);
}
const struct pci_func *display_driver_owner(void) { return &pci3; }
uint32_t pmm_free_count(void) { return 10000; }
uint32_t pmm_total_usable(void) { return 20000; }
int registry_claim(enum res_type type,uint32_t first,uint32_t end,const char *owner,bool share)
{
    assert(!locked3 && prepared3 && claimed3<2 && type==RES_MMIO && !share && !strcmp(owner,"savage-3d"));
    if (!claimed3) assert(first==0xe0000000+heap_first3 && end==0xe0fff000);
    else assert(first==0xf0000000 && end==0xf0800000);
    unsigned i=claimed3++;
    claims3[i]=(struct resource){.type=type,.start=first,.end=end,.owner=owner,.generation=7+i,.state=RS_CLAIMED};
    return (int)i;
}
const struct resource *registry_get(unsigned i) { assert(i<claimed3); return &claims3[i]; }
void registry_inventory(struct registry_inventory_copy *out)
{
    memset(out,0,sizeof(*out));
    if (variant3==12) {
        out->resource_count=1;
        out->resources[0]=(struct resource){.type=RES_MMIO,.start=0xe0900000,.end=0xe0901000,.state=RS_QUARANTINED};
    }
    if (variant3==13) out->truncated=true;
}
int registry_activate(int h,gen_t generation)
{
    assert(!locked3 && h>=0 && (unsigned)h<claimed3 && generation==claims3[h].generation && claims3[h].state==RS_CLAIMED);
    claims3[h].state=RS_ACTIVE; return 0;
}
int registry_quiesce(int h,gen_t generation,registry_idle_proof_fn proof)
{
    assert(!locked3 && h>=0 && (unsigned)h<claimed3 && generation==claims3[h].generation && claims3[h].state==RS_ACTIVE);
    claims3[h].state=RS_QUIESCING; return proof(h,generation) ? 0 : -EIO;
}
int registry_release(int h,uint32_t generation)
{
    assert(!locked3 && h>=0 && (unsigned)h<claimed3 && generation==claims3[h].generation && claims3[h].state==RS_QUIESCING);
    claims3[h].state=RS_RELEASED; released3++; return 0;
}
int registry_quarantine(int h,gen_t generation)
{
    assert(h>=0 && (unsigned)h<claimed3 && generation==claims3[h].generation);
    claims3[h].state=RS_QUARANTINED; quarantined3++; return 0;
}
void *vmm_reserve_mmio(uint32_t bytes) { reserves3++; return kmalloc(bytes); }
void *vmm_replace_mmio(void *p,uint32_t capacity,uint32_t phys,uint32_t bytes)
{
    assert(!locked3 && capacity>=bytes && (phys==0xe0000000+heap_first3 || phys==0xf0000000));
    return variant3==4 && phys==0xf0000000 ? 0 : p;
}
void vmm_revoke_mmio(void *p,uint32_t bytes) { assert(!locked3 && p && bytes); }
int fbdev_render3d_prepare(uint32_t generation)
{
    assert(!locked3 && !prepared3 && generation==42);
    if (variant3==19 && prepares3++<3) return -EBUSY;
    prepared3=true; return 0;
}
void fbdev_render3d_prepared(void) { assert(!locked3 && prepared3); prepared3=false; }
int fbdev_render3d_enter(uint32_t generation)
{
    assert(!locked3 && prepared3 && generation==42 && size_of(THREE_KEY)==14 && !opened && !table_open);
    if (variant3==6) return -EBUSY;
    locked3=1; return 0;
}
void fbdev_render3d_leave(void) { assert(locked3); locked3=0; }
void fbdev_render3d_yield(void) { assert(locked3); }
static uint32_t read_engine3(void *ctx,uint32_t reg)
{ (void)ctx; (void)reg; assert(locked3 && size_of(THREE_KEY)==14 && !opened && !table_open); return variant3==7 ? UINT32_MAX : 0; }
static uint16_t read16_engine3(void *ctx,uint32_t reg) { return (uint16_t)read_engine3(ctx,reg); }
static void write_engine3(void *ctx,uint32_t reg,uint32_t value)
{ (void)ctx; (void)reg; (void)value; assert(locked3 && size_of(THREE_KEY)==14 && !opened && !table_open); stores3++; }
static void drain_engine3(void *ctx) { (void)ctx; }
static uint64_t clock_engine3(void *ctx) { (void)ctx; return now3; }
static void fault_engine3(void *ctx) { (void)ctx; engine3.cpu_safe=false; }
int savage2d_render3d_batch(struct savage2d *s,const uint32_t *words,unsigned n)
{ (void)s; (void)words; (void)n; assert(0); return -EIO; }
int savage2d_render3d_packet(struct savage2d *s,const uint32_t *words,unsigned n)
{ assert(s==&engine3 && words && n && locked3 && size_of(THREE_KEY)==14 && !opened && !table_open); packets3++; return -EIO; }
/* Variants 21-23 test the owner's decisions after a restored, qualified
 * minimum (the engine model is the backend suite's); earlier variants call
 * the production gates unchanged (link-time --wrap). */
int __real_savage3d_probe(struct savage3d_device *);
int __real_savage3d_fullsize_probe(struct savage3d_device *,struct render3d_service *);
int __real_savage3d_color565_probe(struct savage3d_device *,struct render3d_service *);
int __real_savage3d_depth24_probe(struct savage3d_device *,struct render3d_service *);
int __wrap_savage3d_probe(struct savage3d_device *d)
{
    if (variant3<21) return __real_savage3d_probe(d);
    assert(!locked3 && prepared3 && d->consumer && d->probe_checkpoint && d->job_yield && size_of(THREE_KEY)==14);
    d->qualified=true; d->engine->render3d_qualified=true; return 0;
}
int __wrap_savage3d_fullsize_probe(struct savage3d_device *d,struct render3d_service *r)
{
    if (variant3<21) return __real_savage3d_fullsize_probe(d,r);
    assert(variant3!=21 && d->consumer && r && d->fullsize_testing==false);
    fullsize_calls3++; d->fullsize_qualified=d->perspective_qualified=true; return 0;
}
static int optional3(struct savage3d_device *d,bool depth24)
{
    assert(d->consumer && (depth24 ? d->depth24_testing : d->color565_testing) && !locked3 && variant3!=21);
    optional_calls3[depth24]++;
    if (variant3==22 && !depth24) {
        /* A phase overrun inside the optional gate: the checkpoint reports the
         * budget timeout, which must propagate and stop further gates. */
        now3+=SAVAGE3D_PROBE_PHASE_US;
        int e=d->probe_checkpoint(d->probe_ctx); assert(e==-ETIMEDOUT); return e;
    }
    if (variant3==23 && !depth24) return -EILSEQ; /* clean numeric refusal */
    if (depth24) d->depth24_qualified=true; else d->color565_qualified=true;
    return 0;
}
int __wrap_savage3d_color565_probe(struct savage3d_device *d,struct render3d_service *r)
{ if (variant3<21) return __real_savage3d_color565_probe(d,r); assert(r); return optional3(d,false); }
int __wrap_savage3d_depth24_probe(struct savage3d_device *d,struct render3d_service *r)
{ if (variant3<21) return __real_savage3d_depth24_probe(d,r); assert(r); return optional3(d,true); }
int main(int argc,char **argv)
{
    assert(argc==2); variant3=(unsigned)atoi(argv[1]); reset();
    g_boot.display.vram_bytes=16*1024*1024;
    strcpy(g_boot.options,variant3==0 ? "x3d=probe 3d=probeX" : "3d=probe");
    engine3=(struct savage2d){.chip=SAVAGE_SUPER,.active=true,.cpu_safe=true,
        .width=1024,.height=768,.rows=2048,.cursor_offset=0xfff000,
        .io={.read32=read_engine3,.write32=write_engine3,.now_us=clock_engine3,
            .drain=drain_engine3,.fault=fault_engine3,.read16=read16_engine3}};
    if (variant3==1) engine3.cursor_offset=0x801000;
    /* Variants 22-23: fewer 2D rows leave room for full-size and D24 scratch. */
    if (variant3==22 || variant3==23) { engine3.rows=768; heap_first3=0x300000; }
    storage_observer=marker_deadline3;
    if (variant3==17) { g_boot.display.vram_bytes=15532032; engine3.cursor_offset=15532032-4096; }
    if (variant3==2) put(THREE_KEY,"accel=attempt\n");
    if (variant3==5) fail_step=5; /* commit cut */
    if (variant3==11) fail_step=6; /* committed BEGIN, failed close */
    if (variant3==14) volume.readonly=true;
    if (variant3==8) pci3.bar[2]|=4; /* unsupported 64-bit BAR, no sizing */
    if (variant3!=3) savage3d_bind_vfs(&storage);
    if (variant3==18) { frame3=true; savage3d_desktop_presented(42); }
    savage3d_boot_diagnostic(&engine3,&fb3);
    unsigned previous_logs=logs3; savage3d_boot_diagnostic(&engine3,&fb3); assert(logs3==previous_logs);
    assert(!claimed3 && !commits && !stores3 && !prepared3 && !engine3.render3d_qualified);
    /* A completed frame wakes the job; an obsolete mode cancels before claims. */
    frame3=true;
    savage3d_desktop_presented(41);
    if (variant3!=20) savage3d_desktop_presented(42);
    if (worker3) {
        assert(!reaped3);
        worker_running3=true; worker3(0); worker_running3=false;
        /* The finished worker is a zombie after task_exit; the next display
         * notification on another task reaps it once and clears the pointer. */
        savage3d_desktop_presented(42); assert(!reaped3); /* not yet exited */
        worker_task3.state=T_ZOMBIE;
        savage3d_desktop_presented(42); assert(reaped3==1);
        savage3d_boot_diagnostic(&engine3,&fb3); savage3d_desktop_presented(42); assert(reaped3==1);
        assert(worker_task3.state==T_DEAD);
    }
    bool admitted=variant3==21 || variant3==23;
    assert(!locked3 && !prepared3 && engine3.render3d_qualified==admitted);
    /* Every marker operation reached the real block-request conversion. */
    assert(!commits || marker_checks3);
    if (claimed3) assert(reserves3==2);
    assert(!packets3); /* every variant stops before the first BCI command */
    if (variant3==0) assert(!logs3 && !claimed3 && !commits);
    else if (variant3==1 || variant3==3 || variant3==8 || variant3==12 || variant3==13 || variant3==17 || variant3==20) assert(!claimed3 && !commits && !stores3);
    else if (admitted) {
        /* Variant 21: the default census refuses the full-size scratch in
         * preflight (-ENOMEM); the restored minimum stays admitted. Variant
         * 23: a clean optional refusal does not stop the other gate. */
        assert(claimed3==2 && !released3 && !quarantined3 && !stores3 && !size_of(THREE_KEY) &&
            commits==2 && truncates==1 && engine3.active && !engine3.quarantined);
        assert(fullsize_calls3==(variant3==23 ? 1u : 0u));
        assert(optional_calls3[0]==(variant3==23 ? 1u : 0u) && optional_calls3[1]==(variant3==23 ? 1u : 0u));
        assert(variant3==21 ? !d24_logs3 : d24_logs3==3); /* prepare begin/end, result */
    }
    else if (variant3==22) {
        /* Budget overrun in the RGB565 gate: no D24/S8 gate starts; the
         * attempt is refused, restored, released and its marker retained. */
        assert(claimed3==2 && released3==2 && !quarantined3 && !stores3 && size_of(THREE_KEY)==14 &&
            commits==1 && !truncates && engine3.active && !engine3.quarantined);
        assert(fullsize_calls3==1 && optional_calls3[0]==1 && !optional_calls3[1] && !d24_logs3);
    }
    else if (variant3==7 || variant3==10 || variant3==18 || variant3==19) {
        assert(engine3.quarantined && !engine3.active && quarantined3==2 && !released3 &&
            size_of(THREE_KEY)==14 && stores3==(variant3==7 ? 0u : 1u));
        if (variant3==19) assert(prepares3==4 && now3==3000);
    }
    else {
        assert(claimed3==2 && released3==2 && !quarantined3 && !stores3);
        if (variant3==2) assert(size_of(THREE_KEY)==14 && !commits && !truncates);
        if (variant3==6) assert(!size_of(THREE_KEY) && commits==2 && truncates==1);
        if (variant3==11) assert(size_of(THREE_KEY)==14 && commits==1 && !truncates);
        if (variant3==15 || variant3==16) {
            assert(size_of(THREE_KEY)==14 && commits==1 && !truncates && engine3.active && !engine3.quarantined);
            bool previous=false;
            assert(!display_attempt_marker(&storage,"3d-s3savage",0x5333,0x8c2e,0,false,&previous,0) && previous);
            assert(commits==1 && !stores3); /* next boot cannot retry */
        }
    }
    if (variant3==2) {
        /* Same production descriptor policy: an explicit retry clears only
         * this device's 2D and 3D attempt files, retaining another key. */
        static const struct display_driver declaration={.id="s3savage",
            .attempt_files={LEGACY,"C:/system/settings/accel-3d-s3savage-5333-%04x.cfg"}};
        static const char other[]="C:/system/settings/accel-3d-s3savage-5333-8a22.cfg";
        put(other,"accel=attempt\n"); put(DISPLAY_RETRY_PATH,savage_request);
        bool retained=true; retry_mode=true;
        assert(!display_attempt_prepare(&storage,&declaration,&pci3,&retained));
        assert(!retained && !size_of(THREE_KEY) && size_of(other)==14);
    }
    /* Destroy the emulated device/boot, then free all retained allocations.
     * This is fixture teardown, not a production quarantine-release path. */
    for (unsigned i=0;i<allocations_count3;i++) free(allocations3[i]);
    printf("owner marker/claim pre-write variant %u PASS\n",variant3); return 0;
}
