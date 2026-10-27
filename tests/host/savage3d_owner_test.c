/* Production 3D owner against the real keyed-marker implementation and the
 * unchanged committed/cache VFS fixture. Each variant is a separate boot.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kheap_limits.h>
#define main old_marker_main
#define klog old_marker_log
#include "display_attempt_test.c"
#undef klog
#undef main
#include <stdlib.h>
#include <ciuki/mm.h>
#include <ciuki/desktop.h>
#include <ciuki/task.h>
#include "../../src/kernel/drivers/video/s3savage/savage3d.h"
#define THREE_KEY "C:/system/settings/accel-3d-s3savage-5333-8c2e.cfg"
struct ciuki_boot_info g_boot;
static struct pci_func pci3={.vendor=0x5333,.device=0x8c2e,.revision=1,.command=3,
    .bar={0xd0000000,0xe0000000,0xf0000000}};
static struct fb_device fb3={.width=1024,.height=768,.pitch=4096,.size=3145728,
    .phys=0xe0000000,.bpp=32,.present=true,.generation=42};
static struct savage2d engine3;
static struct resource claims3[16];
static unsigned variant3, claimed3, released3, quarantined3, locked3, stores3, packets3, logs3;
static bool prepared3, return_busy3;
static unsigned take_busy3;
static void *allocations3[512];
static unsigned allocations_count3, reserves3, page_live3, prepare_ok3, minimum_started3;
static uint32_t page_sizes3[512], page_bytes3;
static unsigned page_attempts3, page_failures3, fail_page3=UINT32_MAX;
static void (*worker3)(void *);
static bool frame3, worker_running3;
static uint64_t now3;
static unsigned prepares3;
static uint32_t heap_first3=0x300000;
static struct savage3d_device *observed3;
static unsigned minimum_calls3;
static struct task worker_task3;
struct task *g_current;
static unsigned reaped3, marker_checks3, optional_calls3[2], fullsize_calls3, d24_logs3, optional_refusals3;
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
        strstr(line,"[3d] runtime") || strstr(line,"[3d] attachment") || strstr(line,"[accel] retry "));
    if (strstr(line,"phase=prepare index=0 state=end") && strstr(line,"error=0")) prepare_ok3++;
    if (strstr(line,"phase=minimum index=0 state=begin")) minimum_started3++;
    if ((variant3==5 || variant3==11) && strstr(line,"[3d] probe result=")) assert(strstr(line,"marker=unknown"));
    if (strstr(line,"d24s8") || strstr(line,"D24S8")) d24_logs3++;
    if (variant3==22 && strstr(line,"[3d] probe result=")) assert(strstr(line,"result=qualified") && strstr(line,"marker=clear"));
    if ((variant3==22 || variant3==26) && strstr(line,"phase=optional_refused")) {
        assert(strstr(line,variant3==22 ? "gate=rgb565 reason=budget_phase" : "gate=d24s8 reason=budget_total"));
        optional_refusals3++;
    }
    if (variant3==26 && strstr(line,"[3d] probe result=")) assert(strstr(line,"result=qualified") && strstr(line,"marker=clear"));
    if (variant3==27 && strstr(line,"[3d] probe result=")) assert(strstr(line,"reason=budget_total") && strstr(line,"marker=retained"));
    if (variant3==25 && strstr(line,"[3d] probe result=") && strstr(line,"budget_revalidate")) assert(strstr(line,"marker=clear"));
}
static void *backing_alloc3(size_t bytes)
{
    assert(!locked3);
    if (variant3==9 && allocations_count3==2) return 0;
    void *p=calloc(1,bytes); assert(p && allocations_count3<512);
    allocations3[allocations_count3++]=p; return p;
}
void *(kmalloc)(size_t bytes)
{ if (bytes > KHEAP_MAX_REQUEST) return 0; return backing_alloc3(bytes); }
void *desktop_buffer_alloc(uint32_t bytes,enum mm_owner owner)
{
    assert(bytes && owner==MM_IO_REQUEST);
    if (page_attempts3++==fail_page3) { page_failures3++; return 0; }
    void *p=backing_alloc3(bytes);
    if (p) {
        page_sizes3[allocations_count3-1]=bytes; page_live3++;
        page_bytes3+=(bytes+PAGE_SIZE-1)&~(PAGE_SIZE-1);
        assert(page_bytes3+savage3d_backend()->resident_bytes<=R3D_RAM_LIMIT);
    }
    return p;
}
void desktop_buffer_free(void *p,uint32_t bytes,enum mm_owner owner)
{
    assert(owner==MM_IO_REQUEST && p);
    for (unsigned i=0;i<allocations_count3;i++) if (allocations3[i]==p) {
        assert(page_sizes3[i]==bytes && page_live3);
        page_sizes3[i]=0; page_live3--;
        page_bytes3-=(bytes+PAGE_SIZE-1)&~(PAGE_SIZE-1);
        kfree(p); return;
    }
    assert(0);
}
void kfree(void *p)
{
    assert(!locked3);
    for (unsigned i=0;i<allocations_count3;i++) if (allocations3[i]==p) { allocations3[i]=0; free(p); return; }
    assert(!p);
}
const struct fb_device *fbdev_get(void) { return &fb3; }
const struct pci_func *display_driver_owner(void) { return &pci3; }
uint32_t pmm_free_count(void) { return 10000; }
uint32_t pmm_total_usable(void) { return 20000; }
int registry_claim(enum res_type type,uint32_t first,uint32_t end,const char *owner,bool share)
{
    assert(!locked3 && prepared3 && claimed3<16 && type==RES_MMIO && !share && !strcmp(owner,"savage-3d"));
    if (!(claimed3%2)) assert(first==0xe0000000+heap_first3 && end==0xe0000000+engine3.cursor_offset);
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
void *vmm_reserve_mmio(uint32_t bytes) { reserves3++; return backing_alloc3(bytes); }
void *vmm_replace_mmio(void *p,uint32_t capacity,uint32_t phys,uint32_t bytes)
{
    assert(!locked3 && capacity>=bytes && (phys==0xe0000000+heap_first3 || phys==0xf0000000));
    return variant3==4 && phys==0xf0000000 ? 0 : p;
}
void vmm_revoke_mmio(void *p,uint32_t bytes) { assert(!locked3 && p && bytes); }
int savage2d_cache_transfer(void *ctx,bool take)
{
    assert(ctx==&engine3 && prepared3 && !locked3);
    if (take) engine3.rows=engine3.height;
    else engine3.rows=2048;
    return 0;
}
int fbdev_render3d_cache(uint32_t generation,int (*fn)(void *,bool),void *ctx,bool take)
{
    assert(generation==fb3.generation && prepared3 && !locked3);
    if (!take && return_busy3) { return_busy3=false; return -EBUSY; }
    if (take && take_busy3 && worker_running3==false) { take_busy3--; return -EBUSY; }
    return fn(ctx,take);
}
int fbdev_render3d_prepare(uint32_t generation)
{
    assert(!locked3 && !prepared3 && generation==fb3.generation);
    if (variant3==19 && prepares3++<3) return -EBUSY;
    prepared3=true; return 0;
}
void fbdev_render3d_prepared(void) { assert(!locked3 && prepared3); prepared3=false; }
int fbdev_render3d_enter(uint32_t generation)
{
    assert(!locked3 && prepared3 && generation==fb3.generation && size_of(THREE_KEY)==14 && !opened && !table_open);
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
    observed3=d;
    if (variant3==25 && minimum_calls3++) {
        now3+=SAVAGE3D_REVALIDATE_US;
        return d->probe_checkpoint(d->probe_ctx);
    }
    d->qualified=true; d->engine->render3d_qualified=true; return 0;
}
int __wrap_savage3d_fullsize_probe(struct savage3d_device *d,struct render3d_service *r)
{
    if (variant3<21) return __real_savage3d_fullsize_probe(d,r);
    assert(d->consumer && r && d->fullsize_testing==false);
    fullsize_calls3++; d->fullsize_qualified=d->perspective_qualified=true; return 0;
}
static int optional3(struct savage3d_device *d,bool depth24)
{
    assert(d->consumer && (depth24 ? d->depth24_testing : d->color565_testing) && !locked3);
    optional_calls3[depth24]++;
    if (variant3==22 && !depth24) {
        /* A phase overrun inside the optional gate with a restored, idle
         * engine refuses only that format (amendment 1, f7-1213 P2-1). */
        d->probe_phase(d->probe_ctx,"rgb565",1,true,0);
        now3+=SAVAGE3D_PROBE_PHASE_US;
        int e=d->probe_checkpoint(d->probe_ctx); assert(e==-ETIMEDOUT);
        d->probe_phase(d->probe_ctx,"rgb565",1,false,e);
        return e;
    }
    if ((variant3==26 || variant3==27) && depth24) {
        /* The optional gates must stop before the marker CLEAR reserve. */
        d->probe_phase(d->probe_ctx,"d24s8",1,true,0);
        assert(!d->probe_checkpoint(d->probe_ctx));
        now3+=SAVAGE3D_PROBE_TOTAL_US-SAVAGE3D_PROBE_MARKER_US;
        int e=d->probe_checkpoint(d->probe_ctx); assert(e==-ETIMEDOUT);
        d->probe_phase(d->probe_ctx,"d24s8",1,false,e);
        /* Variant 27: the overrun left an engine that is not proven idle. */
        if (variant3==27) savage3d_backend()->quarantine(d);
        return e;
    }
    if (variant3==23 && !depth24) return -EILSEQ; /* clean numeric refusal */
    if (depth24) d->depth24_qualified=true; else d->color565_qualified=true;
    return 0;
}
int __wrap_savage3d_color565_probe(struct savage3d_device *d,struct render3d_service *r)
{ if (variant3<21) return __real_savage3d_color565_probe(d,r); assert(r); return optional3(d,false); }
int __wrap_savage3d_depth24_probe(struct savage3d_device *d,struct render3d_service *r)
{ if (variant3<21) return __real_savage3d_depth24_probe(d,r); assert(r); return optional3(d,true); }
static void *owner_alloc3(void *ctx,size_t n) { (void)ctx; return backing_alloc3(n); }
static void owner_free3(void *ctx,void *p) { (void)ctx; kfree(p); }
int main(int argc,char **argv)
{
    assert(argc==2 || argc==3); variant3=(unsigned)atoi(argv[1]); reset();
    if (argc==3) { assert(variant3==23); fail_page3=(unsigned)atoi(argv[2]); }
    g_boot.display.vram_bytes=16*1024*1024;
    strcpy(g_boot.options,variant3==0 ? "x3d=probe 3d=probeX" : "3d=probe");
    engine3=(struct savage2d){.chip=SAVAGE_SUPER,.active=true,.cpu_safe=true,
        .width=1024,.height=768,.rows=2048,.cursor_offset=0xfff000,
        .io={.read32=read_engine3,.write32=write_engine3,.now_us=clock_engine3,
            .drain=drain_engine3,.fault=fault_engine3,.read16=read16_engine3}};
    if (variant3==1) engine3.cursor_offset=0x801000;
    /* Variants 22-23, 26-27: fewer 2D rows leave room for full-size and D24 scratch. */
    if (variant3==22 || variant3==23 || variant3==26 || variant3==27) { engine3.rows=768; heap_first3=0x300000; }
    storage_observer=marker_deadline3;
    if (variant3==17 || variant3==24 || variant3==25) { g_boot.display.vram_bytes=15532032; engine3.cursor_offset=15532032-4096; }
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
    if (argc==3) {
        assert(page_failures3==1 && !page_live3 && !page_bytes3 &&
            !locked3 && !prepared3 && !quarantined3 && claimed3==released3 && !size_of(THREE_KEY));
        if (fail_page3<6) assert(!prepare_ok3 && !minimum_started3 && !engine3.render3d_qualified);
        else assert(prepare_ok3 && minimum_started3 && engine3.render3d_qualified);
        printf("probe page failure index=%u scratch/fullsize/reference cleanup PASS\n",fail_page3);
        goto teardown;
    }
    bool admitted=variant3==21 || variant3==22 || variant3==23 || variant3==24 || variant3==25 || variant3==26;
    assert(!locked3 && !prepared3 && engine3.render3d_qualified==admitted);
    if (variant3==21) {
        assert(prepare_ok3 && minimum_started3 && !page_live3);
        puts("bounded heap: background prepare reached minimum; page scratch fully released PASS");
    }
    if (variant3==9) assert(!prepare_ok3 && !minimum_started3 && !page_live3);
    if (!engine3.quarantined) assert(!page_live3);
    /* Every marker operation reached the real block-request conversion. */
    assert(!commits || marker_checks3);
    if (claimed3) assert(reserves3==2);
    assert(!packets3); /* every variant stops before the first BCI command */
    if (variant3==0) assert(!logs3 && !claimed3 && !commits);
    else if (variant3==1 || variant3==3 || variant3==8 || variant3==12 || variant3==13 || variant3==20) assert(!claimed3 && !commits && !stores3);
    else if (admitted) {
        /* Variant 21: the default census refuses the full-size scratch in
         * preflight (-ENOMEM); the restored minimum stays admitted. Variant
         * 23: a clean optional refusal does not stop the other gate. */
        assert(claimed3==2 && released3==2 && !quarantined3 && !stores3 && !size_of(THREE_KEY) &&
            commits==2 && truncates==1 && engine3.active && !engine3.quarantined);
        if (variant3==26) {
            /* Total overrun in the first D24/S8 gate: only that gate is
             * refused, no further gate starts, the 640x480 full-size target
             * is kept and the marker CLEAR still had its 1 s reserve. */
            assert(fullsize_calls3==1 && !optional_calls3[0] && optional_calls3[1]==1 && optional_refusals3==1);
        } else {
            assert(fullsize_calls3==3);
            assert(optional_calls3[0]==3 && optional_calls3[1]==3);
            assert(d24_logs3==9 || variant3==22); /* prepare begin/end, result */
            if (variant3==22) assert(optional_refusals3==3);
        }
    }
    else if (variant3==27) {
        /* An overrun without a restored, idle engine is not contained: no
         * further gate, claims quarantined and the marker retained. */
        assert(engine3.quarantined && !engine3.active && quarantined3==2 && !released3 &&
            size_of(THREE_KEY)==14 && commits==1 && !truncates);
        assert(fullsize_calls3==1 && !optional_calls3[0] && optional_calls3[1]==1);
    }
    else if (variant3==7 || variant3==10 || variant3==18 || variant3==19 || variant3==17) {
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
    if (variant3==24 || variant3==25) {
        struct render3d_service *client=calloc(1,sizeof(*client)); assert(client);
        struct render3d_host h={.alloc=owner_alloc3,.free=owner_free3,.now_us=clock_engine3};
        for (unsigned mode=0;mode<4;mode++) {
            unsigned full_before=fullsize_calls3,format_before=optional_calls3[0]+optional_calls3[1];
            unsigned claims_before=claimed3,releases_before=released3;
            if (mode) {
                assert(!savage3d_owner_stop(&engine3));
                fb3.width=mode==1 ? 640 : 1024; fb3.height=mode==1 ? 480 : 768;
                fb3.bpp=mode==3 ? 16 : 32; fb3.pitch=fb3.width*(fb3.bpp/8); fb3.size=fb3.pitch*fb3.height;
                fb3.generation++; engine3.width=fb3.width; engine3.height=fb3.height; engine3.rows=2048;
                heap_first3=(fb3.size+4095)&~4095u;
                if (mode!=1) savage3d_boot_diagnostic(&engine3,&fb3);
                assert(!engine3.render3d_qualified); /* old generation revoked */
                worker_running3=true; /* short gate on display worker, no desktop-frame dependency */
            }
            int attach=savage3d_owner_attach(client,&h,fb3.generation);
            if (variant3==25 && mode) {
                worker_running3=false;
                /* Amendment 1: a missed 50 ms deadline with a restored, idle
                 * engine refuses this mode only; the marker is cleared. */
                assert(attach==-ENODEV && !engine3.render3d_qualified && !size_of(THREE_KEY) &&
                    claimed3==released3 && !quarantined3 && !observed3->lost && engine3.rows==2048);
                puts("T23 cached minimum recheck: 50 ms expiry, no raster rerun, no admission, marker cleared, claims/cache returned PASS");
                break;
            }
            assert(!attach);
            worker_running3=false;
            assert(fullsize_calls3==full_before && optional_calls3[0]+optional_calls3[1]==format_before);
            struct ciuki_r3d_caps caps; render3d_backend_caps(client,&caps);
            assert(caps.max_width==fb3.width && caps.max_height==fb3.height && (caps.depth_formats&CIUKI_R3D_D24S8));
            assert(engine3.rows==2048 && claimed3-released3==0); /* query borrows nothing */
            /* A passing drawing-domain conflict at the first OPEN is retried
             * (1 ms sleeps), never a permanent software context. */
            take_busy3=variant3==24 && mode==0 ? 3 : 0;
            uint64_t before_open=now3;
            unsigned c[2]; assert(!render3d_create_context(client,7,R3D_HARDWARE_REQUIRED,&c[0]));
            assert(engine3.rows==engine3.height && claimed3-released3==2 && !take_busy3);
            if (variant3==24 && mode==0) {
                assert(now3-before_open==3000);
                puts("T23 first hardware OPEN: transient drawing-domain EBUSY retried 3 x 1 ms, reservation taken PASS");
            }
            assert(!render3d_create_context(client,7,R3D_HARDWARE_REQUIRED,&c[1]));
            assert(!render3d_destroy_context(client,7,c[0]) && claimed3-released3==2);
            return_busy3=variant3==24 && mode==0;
            bool deferred=return_busy3;
            assert(!render3d_destroy_context(client,7,c[1]) && claimed3==released3);
            if (deferred) {
                assert(engine3.rows==engine3.height);
                savage3d_boot_diagnostic(&engine3,&fb3);
                puts("T23 last-context cache return: passing presenter contention deferred and retried PASS");
            }
            assert(engine3.rows==2048);
            assert(observed3 && observed3->qualified && !observed3->lost && !size_of(THREE_KEY));
            printf("T23 owner mode=%ux%ux%u generation=%u heap_first=%u heap_bytes=%u texture=%u short_recheck=%u claims=%u releases=%u PASS\n",
                fb3.width,fb3.height,fb3.bpp,fb3.generation,observed3->heap.first,observed3->heap.bytes,
                observed3->texture_bytes,mode!=0,claimed3-claims_before,released3-releases_before);
            assert(!render3d_close(client));
        }
        free(client);
    }
teardown:
    /* Destroy the emulated device/boot, then free all retained allocations.
     * This is fixture teardown, not a production quarantine-release path. */
    for (unsigned i=0;i<allocations_count3;i++) free(allocations3[i]);
    printf("owner marker/claim pre-write variant %u PASS\n",variant3); return 0;
}
