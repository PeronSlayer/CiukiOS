/* Production glue for the existing Savage owner; no independent PCI claim.
 * Algorithms/register formats are independently implemented from the pinned
 * sources in f7-3d-acceleration-contract.md. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/mm.h>
#include <ciuki/display_driver.h>
#include <ciuki/task.h>
#include "../../../fs/fs_port.h"
#include <ciuki/selftest.h>
#include "savage3d.h"
static struct savage3d_device device;
static struct vfs *marker_vfs;
static volatile uint8_t *linear, *aperture;
static void *linear_window, *aperture_window;
static uint32_t linear_capacity;
static int claims[2]={-1,-1};
static gen_t generations[2];
static bool reported, marker_fresh, marker_retained, marker_unknown;
static bool cache_borrowed, stopping, probe_active, revalidation_pending, cache_return_pending;
static uint32_t scheduled_generation;
static uint64_t runtime_triangles, runtime_copies, runtime_scanouts;
static uint64_t runtime_downloads, runtime_download_pixels, runtime_uploads, runtime_upload_pixels, runtime_upload_skips;
/* Raster evidence survives scanout generations, never a different device or
 * firmware VRAM layout. The layout key is deliberately stricter than PCI ID. */
static struct {
    bool valid;
    uint16_t vendor, id;
    uint8_t revision;
    uint32_t lfb, alias, vram, cursor, generation;
} layout_key;
static struct target_result {
    uint32_t width, height;
    bool full, perspective, depth24, color565;
    uint32_t sample_delta, filter_bits;
} targets[3];
static void save_target(unsigned i)
{
    targets[i]=(struct target_result){device.mode_width,device.mode_height,
        device.fullsize_qualified,device.perspective_qualified,device.depth24_qualified,
        device.color565_qualified,device.sample_delta_q16,device.filter_fraction_bits};
}
static void select_target(const struct fb_device *fb)
{
    device.fullsize_qualified=device.perspective_qualified=device.depth24_qualified=device.color565_qualified=false;
    device.mode_width=fb->width; device.mode_height=fb->height;
    for (unsigned i=0;i<3;i++) if (targets[i].full && targets[i].width==fb->width && targets[i].height==fb->height) {
        const struct target_result *t=&targets[i];
        device.fullsize_qualified=t->full; device.perspective_qualified=t->perspective;
        device.depth24_qualified=t->depth24; device.color565_qualified=t->color565;
        device.sample_delta_q16=t->sample_delta; device.filter_fraction_bits=t->filter_bits;
        uint32_t rows=(fb->height+15)&~15u;
        uint32_t color=((fb->width*4+127)&~127u)*rows;
        uint32_t d24=3*((color+4095)&~4095u)+4*8192u+24576u;
        if ((uint64_t)d24+device.texture_bytes>device.heap.bytes) device.depth24_qualified=false;
        /* The texture pool is what this mode's largest admitted targets
         * leave (f7-1213 review P3-1): a refused D24/S8 returns its share. */
        uint32_t d16=2*((color+4095)&~4095u)+((((fb->width*2+127)&~127u)*rows+4095)&~4095u)+4*8192u+24576u;
        uint32_t targets_bytes=device.depth24_qualified ? d24 : d16;
        if (device.heap.bytes>targets_bytes+SAVAGE3D_TEXTURE_MIN) {
            uint32_t pool=(device.heap.bytes-targets_bytes)&~4095u;
            if (pool>device.texture_bytes) device.texture_bytes=pool;
        }
        break;
    }
}
static struct savage2d *pending_engine;
static struct fb_device pending_fb;
static struct task *probe_task;
static bool probe_finished;
static struct kwait probe_wait;
static bool desktop_ready;
static uint32_t desktop_generation;
static uint64_t probe_started, phase_started, phase_deadline=UINT64_MAX, revalidate_deadline;
/* fx-selftest: the last qualification's outcome and phases, read-only. */
static struct render3d_qualification_report last_report;
static const char *budget_reason;
/* f7-1213 review P2-1 (lead decision, amendment 1): every gate after the
 * qualified minimum is optional. It must finish SAVAGE3D_PROBE_MARKER_US
 * before the total so that the marker CLEAR keeps its own budget, and an
 * overrun there (or of the cached-revalidation deadline) is contained in
 * optional_reason: with a restored, proven-idle engine it refuses only that
 * gate and the marker is cleared normally. budget_reason stays reserved for
 * mandatory phases and non-idle outcomes (retained marker). */
static bool optional_gate, optional_stop, optional_phase;
static const char *optional_reason;
static unsigned checkpoint_count;
static uint64_t probe_now(void)
{ return pending_engine->io.now_us(pending_engine->io.ctx); }
static int probe_check(void)
{
    if (!budget_reason && !optional_reason) {
        uint64_t now=probe_now();
        uint64_t total=optional_gate ? SAVAGE3D_PROBE_TOTAL_US-SAVAGE3D_PROBE_MARKER_US : SAVAGE3D_PROBE_TOTAL_US;
        const char *reason=0;
        bool phase=false;
        if (revalidate_deadline && now>=revalidate_deadline) reason="budget_revalidate";
        else if (now-probe_started>=total) reason="budget_total";
        else if (now>=phase_deadline) { reason="budget_phase"; phase=true; }
        if (reason && (optional_gate || revalidate_deadline)) { optional_reason=reason; optional_phase=phase; }
        else if (reason) budget_reason=reason;
    }
    return budget_reason || optional_reason ? -ETIMEDOUT : 0;
}
/* After an optional gate or the cached revalidation: is an overrun contained
 * (restored, idle engine)? A phase overrun refuses only that gate; a total
 * overrun also stops the remaining optional gates. */
static bool engine_restored(void)
{
    return !device.lost && device.engine && device.engine->active && device.engine->cpu_safe &&
        !device.engine->event_pending && !device.engine->quarantined;
}
static bool optional_contained(const char *gate)
{
    if (!optional_reason) return !budget_reason && !device.lost && !optional_stop;
    if (!engine_restored()) { budget_reason=optional_reason; optional_reason=0; return false; }
    bool phase=optional_phase;
    klog("[3d] probe phase=optional_refused gate=%s reason=%s width=%u height=%u restored=1 marker=clear_pending elapsed_us=%llu",
        gate,optional_reason,device.mode_width,device.mode_height,probe_now()-probe_started);
    optional_reason=0;
    if (!phase) optional_stop=true;
    return phase && !optional_stop;
}
static int probe_checkpoint(void *ctx)
{
    (void)ctx;
    if (++checkpoint_count>1000000) budget_reason="budget_work"; /* frozen-clock bound */
    if (probe_check()) return -ETIMEDOUT;
    task_yield();
    return probe_check();
}
static void probe_phase(void *ctx,const char *name,unsigned index,bool begin,int error)
{
    (void)ctx;
    if (begin) {
        phase_started=probe_now();
        phase_deadline=phase_started+(!strncmp(name,"marker",6) ? SAVAGE3D_PROBE_MARKER_US : SAVAGE3D_PROBE_PHASE_US);
    } else {
        phase_deadline=UINT64_MAX; /* cleanup/logging after a phase is not budget_phase (P3-4) */
        if (last_report.phase_count<RENDER3D_QUALIFICATION_PHASES) {
            uint64_t us=probe_now()-phase_started;
            last_report.phases[last_report.phase_count].name=name;
            last_report.phases[last_report.phase_count].us=us>UINT32_MAX ? UINT32_MAX : (uint32_t)us;
            last_report.phases[last_report.phase_count++].error=error;
        }
    }
    klog("[3d] probe phase=%s index=%u state=%s elapsed_us=%llu total_us=%llu error=%d",
        name,index,begin ? "begin" : "end",probe_now()-phase_started,probe_now()-probe_started,error);
}
/* The VFS request clock and deadline are microseconds; the block layer
 * converts the remainder to milliseconds itself (blkreq.c). */
static uint64_t marker_now(void *ctx) { (void)ctx; return probe_now(); }
static void marker_pause(void *ctx) { (void)ctx; task_sleep_ms(1); }
static int probe_marker(const struct pci_func *pci,bool clear,bool *previous)
{
    const char *name=clear ? "marker_clear" : "marker_begin";
    probe_phase(0,name,0,true,0);
    uint64_t until=probe_started+SAVAGE3D_PROBE_TOTAL_US;
    if (until>phase_deadline) until=phase_deadline;
    struct vfs_request request={.deadline=until,.now=marker_now,.pause=marker_pause,
        .operation="3d-probe-marker",.site="native",.background=true};
    struct fs_request_scope scope;
    fs_request_enter(&scope,&request);
    int e=display_attempt_marker(marker_vfs,"3d-s3savage",pci->vendor,pci->device,0,clear,previous,0);
    fs_request_leave(&scope);
    probe_phase(0,name,0,false,e);
    return e;
}
void savage3d_bind_vfs(struct vfs *vfs) { marker_vfs=vfs; }
static uint32_t memory_read(void *ctx,bool alias,uint32_t offset,unsigned bytes)
{
    (void)ctx;
    volatile uint8_t *p=alias ? aperture+offset : linear+offset-device.heap.first;
    if (bytes==2) return *(volatile uint16_t *)p;
    return *(volatile uint32_t *)p;
}
static void memory_write(void *ctx,bool alias,uint32_t offset,uint32_t value,unsigned bytes)
{
    (void)ctx;
    volatile uint8_t *p=alias ? aperture+offset : linear+offset-device.heap.first;
    if (bytes==2) *(volatile uint16_t *)p=(uint16_t)value;
    else *(volatile uint32_t *)p=value;
}
static int enter(void *ctx,uint32_t generation)
{ (void)ctx; return fbdev_render3d_enter(generation); }
static void leave(void *ctx) { (void)ctx; fbdev_render3d_leave(); }
static void job_yield(void *ctx) { (void)ctx; fbdev_render3d_yield(); }
static bool safe_extent(uint32_t first,uint32_t bytes)
{
    if (first<0x100000 || !bytes || bytes>UINT32_MAX-first) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *m=&g_boot.e820[i];
        if (m->type==CBI_E820_RAM && first<m->base+m->length && (uint64_t)first+bytes>m->base) return false;
    }
    return !(first<g_boot.kernel_end && (uint64_t)first+bytes>g_boot.kernel_start);
}
static int available_heap(const struct fb_device *fb,struct render3d_extent *heap,
                           uint32_t *free_bytes,uint32_t *largest_bytes)
{
    struct registry_inventory_copy inventory;
    registry_inventory(&inventory);
    uint32_t run=0,largest_first=heap->first;
    *free_bytes=*largest_bytes=0;
    uint32_t end=heap->first+heap->bytes;
    for (uint32_t at=heap->first;at<end;at+=4096) {
        if (probe_active && !((at-heap->first)%65536)) { int e=probe_checkpoint(0); if (e) return e; }
        uint64_t physical=(uint64_t)fb->phys+at;
        bool used=inventory.truncated;
        for (unsigned i=0;i<inventory.resource_count;i++) {
            const struct resource *r=&inventory.resources[i];
            if (r->type==RES_MMIO && r->state!=RS_RELEASED && physical<r->end && physical+4096>r->start) used=true;
        }
        if (used) run=0;
        else {
            *free_bytes+=4096; run+=4096;
            if (run>*largest_bytes) { *largest_bytes=run; largest_first=at+4096-run; }
        }
    }
    *heap=(struct render3d_extent){largest_first,*largest_bytes};
    return 0;
}
static bool release_proof(int handle,gen_t generation)
{
    if (device.engine && (!device.engine->cpu_safe || device.engine->event_pending)) return false;
    if (handle==claims[1] && generation==generations[1] && aperture) {
        vmm_revoke_mmio((void *)aperture,SAVAGE3D_ALIAS_BYTES); aperture=0;
    }
    if (handle==claims[0] && generation==generations[0] && linear) {
        vmm_revoke_mmio((void *)linear,device.heap.bytes); linear=0;
    }
    return true;
}
static int release_claims(void)
{
    int error=0;
    for (int i=1;i>=0;i--) if (claims[i]>=0) {
        const struct resource *r=registry_get(claims[i]);
        /* The registry has no release operation for an unactivated claim.
         * An unexpected activation failure must retain that reservation. */
        int e=r && r->state==RS_CLAIMED ? -EIO : registry_quiesce(claims[i],generations[i],release_proof);
        if (!e) e=registry_release(claims[i],generations[i]);
        if (e) { registry_quarantine(claims[i],generations[i]); error=e; }
        else claims[i]=-1;
    }
    return error;
}
static void lost_notify(void *ctx)
{
    (void)ctx;
    for (unsigned i=0;i<2;i++) if (claims[i]>=0) registry_quarantine(claims[i],generations[i]);
}
static int cache_transfer(bool take)
{
    int e=fbdev_render3d_cache(device.generation,savage2d_cache_transfer,device.engine,take);
    if (!e) cache_borrowed=take;
    else if (!take && cache_borrowed && e==-EBUSY) cache_return_pending=true;
    return e;
}
static int claim_heap(const struct fb_device *fb,uint32_t alias)
{
    uint32_t bases[]={fb->phys+device.heap.first,alias},sizes[]={device.heap.bytes,SAVAGE3D_ALIAS_BYTES};
    int e=0;
    for (unsigned i=0;!e && i<2;i++) {
        claims[i]=registry_claim(RES_MMIO,bases[i],bases[i]+sizes[i],"savage-3d",false);
        if (claims[i]<0) { e=claims[i]; break; }
        generations[i]=registry_get(claims[i])->generation;
        e=registry_activate(claims[i],generations[i]);
    }
    if (!e) {
        if (!linear_window) {
            linear_capacity=16u*1024*1024;
            linear_window=vmm_reserve_mmio(linear_capacity);
        }
        if (!aperture_window) aperture_window=vmm_reserve_mmio(SAVAGE3D_ALIAS_BYTES);
        linear=linear_window ? vmm_replace_mmio(linear_window,linear_capacity,bases[0],sizes[0]) : 0;
        aperture=aperture_window ? vmm_replace_mmio(aperture_window,SAVAGE3D_ALIAS_BYTES,alias,SAVAGE3D_ALIAS_BYTES) : 0;
        if (!linear || !aperture) e=-ENOMEM;
    }
    return e;
}
static void return_heap(void *ctx)
{
    (void)ctx;
    if (!cache_borrowed) { cache_return_pending=false; return; }
    if (stopping || probe_active || device.lost) return;
    if (device.consumer) for (unsigned i=0;i<R3D_CONTEXTS;i++)
        if (device.consumer->contexts[i].live && device.consumer->contexts[i].hardware) return;
    /* No live hardware context or referenced job remains. Pin the display
     * across revocation and cache return; contention defers until next attach
     * or detach/stop instead of releasing an unproved reservation. */
    if (fbdev_render3d_prepare(device.generation)) { cache_return_pending=true; return; }
    cache_return_pending=false;
    int e=release_claims();
    if (!e) e=cache_transfer(false);
    if (e && (claims[0]>=0 || claims[1]>=0)) {
        device.lost=true; lost_notify(0);
    }
    fbdev_render3d_prepared();
    klog("[3d] runtime state=release generation=%u triangles=%llu copies=%llu scanouts=%llu downloads=%llu download_pixels=%llu uploads=%llu upload_pixels=%llu upload_skips=%llu error=%d",
        device.generation,device.triangles-runtime_triangles,device.copies-runtime_copies,device.scanouts-runtime_scanouts,
        device.downloads-runtime_downloads,device.download_pixels-runtime_download_pixels,device.uploads-runtime_uploads,
        device.upload_pixels-runtime_upload_pixels,device.upload_skips-runtime_upload_skips,e);
}
static int reserve_heap(void *ctx)
{
    (void)ctx;
    if (device.lost || !device.qualified) return -ENODEV;
    cache_return_pending=false;
    if (cache_borrowed && linear && aperture) return 0;
    /* A passing desktop draw, batch or cursor write holds the drawing domain
     * for a moment: retry for at most 100 ms (1 ms sleeps, no lock held but
     * the preparation pin) instead of turning the GL OPEN into a permanent
     * software context (f7-15 review). The pin keeps the mode stable. */
    int e=-EBUSY;
    for (unsigned attempt=0;attempt<100 && e==-EBUSY;attempt++) {
        if (attempt) task_sleep_ms(1);
        e=fbdev_render3d_prepare(device.generation);
    }
    if (e) return e;
    e=-EBUSY;
    for (unsigned attempt=0;attempt<100 && e==-EBUSY;attempt++) {
        if (attempt) task_sleep_ms(1);
        e=cache_transfer(true);
    }
    struct render3d_extent heap={0}; uint32_t required=0,free_bytes=0,largest=0;
    if (!e) e=savage3d_mode_heap(device.engine,&pending_fb,g_boot.display.vram_bytes,&heap,&required);
    if (!e) e=available_heap(&pending_fb,&heap,&free_bytes,&largest);
    /* A service's allocation addresses cannot change behind its back. */
    if (!e && (heap.first!=device.heap.first || heap.bytes!=device.heap.bytes || largest<required)) e=-EAGAIN;
    if (!e) e=claim_heap(&pending_fb,layout_key.alias);
    if (e) {
        int released=release_claims();
        if (released) { device.lost=true; lost_notify(0); }
        else if (cache_borrowed) (void)cache_transfer(false);
    }
    fbdev_render3d_prepared();
    if (!e) {
        runtime_triangles=device.triangles; runtime_copies=device.copies; runtime_scanouts=device.scanouts;
        runtime_downloads=device.downloads; runtime_download_pixels=device.download_pixels;
        runtime_uploads=device.uploads; runtime_upload_pixels=device.upload_pixels; runtime_upload_skips=device.upload_skips;
        klog("[3d] runtime state=reserve generation=%u width=%u height=%u heap_first=%u heap_bytes=%u texture_bytes=%u transfer_bound_us=%u revalidate_bound_us=%u mode_ready_bound_us=%u",
            device.generation,device.mode_width,device.mode_height,device.heap.first,device.heap.bytes,device.texture_bytes,
            SAVAGE3D_READBACK_US,SAVAGE3D_REVALIDATE_US,SAVAGE3D_MODE_READY_US);
    }
    return e;
}
int savage3d_owner_stop(struct savage2d *engine)
{
    if (device.engine!=engine) return 0;
    /* Mode revokes all old resources/grants. Only raster evidence survives. */
    stopping=true;
    device.qualified=false; engine->render3d_qualified=false;
    if (device.consumer) {
        int e=render3d_close(device.consumer);
        if (e) { stopping=false; return e; }
        device.consumer=0;
    }
    stopping=false;
    if (device.lost || engine->quarantined || !engine->cpu_safe || engine->event_pending) {
        layout_key.valid=false; return -EIO;
    }
    int e=release_claims();
    if (!e) cache_borrowed=cache_return_pending=false; /* 2D stop retires this cache. */
    else layout_key.valid=false;
    return e;
}
static void qualification(void);
int savage3d_owner_prepare(uint32_t generation)
{
    if (layout_key.valid && generation!=device.generation && !revalidation_pending && !probe_active) {
        /* The full-screen lease can reach GL before the new mode's 2D
         * marker worker or first presentation. Wait without presenter for
         * that activation, then nominate the cached short check ourselves. */
        uint64_t started=probe_now(),until=started+SAVAGE3D_MODE_READY_US;
        int ready=-EBUSY;
        for (unsigned tries=0;tries<SAVAGE3D_MODE_READY_US/1000 && ready==-EBUSY;tries++) {
            ready=fbdev_render3d_prepare(generation);
            if (!ready) fbdev_render3d_prepared();
            else if (ready==-EBUSY) {
                if (probe_now()>=until) { ready=-ETIMEDOUT; break; }
                task_sleep_ms(1);
            }
        }
        klog("[3d] probe phase=mode_ready generation=%u waited_us=%llu bound_us=%u result=%d",
            generation,probe_now()-started,SAVAGE3D_MODE_READY_US,ready);
        if (ready) return -ENODEV;
        const struct fb_device *fb=fbdev_get();
        if (!fb || fb->generation!=generation) return -ENODEV;
        savage3d_boot_diagnostic(device.engine,fb);
    }
    if (revalidation_pending && generation==pending_fb.generation && !probe_active) {
        revalidation_pending=false;
        qualification();
    }
    /* A qualification still finishing (marker clear, claim/cache return)
     * admits nothing yet; the native refresh attaches once it is done. */
    if (!device.qualified || marker_fresh || probe_active || device.lost || generation!=device.generation) return -ENODEV;
    return 0;
}
int savage3d_owner_attach(struct render3d_service *s,const struct render3d_host *host,uint32_t generation)
{
    int e=savage3d_owner_prepare(generation); if (e) return e;
    if (device.consumer) return -EBUSY;
    render3d_init(s,host,generation,savage3d_backend(),&device,device.heap);
    device.consumer=s; return 0;
}
static void *host_alloc(void *ctx,size_t bytes) { (void)ctx; return kmalloc(bytes); }
static void host_free(void *ctx,void *p) { (void)ctx; kfree(p); }
static uint64_t host_now(void *ctx)
{ (void)ctx; return device.engine->io.now_us(device.engine->io.ctx); }
static int scratch_create(struct render3d_service **out,unsigned width,unsigned height,bool full,bool hardware)
{
    /* The existing inventory-derived pool must contain both mip chains and
     * their guards. This is preflight, never a larger VRAM claim or limit. */
    if (full && hardware && device.texture_bytes<480u*1024) return -ENOMEM;
    struct render3d_service *s=kmalloc(sizeof(*s)); if (!s) return -ENOMEM;
    const struct render3d_host host={0,host_alloc,host_free,host_now,0};
    render3d_init(s,&host,device.generation,0,0,(struct render3d_extent){0});
    s->allocation_checkpoint=probe_checkpoint;
    uint32_t context;
    int e=render3d_create_context(s,1,R3D_SOFTWARE,&context);
    const enum render3d_kind kinds[]={device.color565_testing ? R3D_COLOR565 : R3D_COLOR,
        device.depth24_testing ? R3D_D24S8 : R3D_D16,R3D_TEXTURE,
        device.color565_testing ? R3D_LINEAR565 : R3D_LINEAR_COLOR,R3D_TEXTURE};
    uint32_t at=device.heap.first,tex_at=device.heap.first+device.heap.bytes-device.texture_bytes;
    for (unsigned i=0;!e && i<(full ? 5u : 4u);i++) {
        e=probe_checkpoint(0); if (e) break;
        uint32_t resource;
        uint32_t *pixels=(void *)s->ring[2];
        if (full && kinds[i]==R3D_TEXTURE) {
            unsigned base=i==2 ? 256 : 128,levels=i==2 ? 9 : 8;
            /* The copied texture outlives this bounded staging allocation.
             * Never use a 64 KiB command-ring slot for a 256-square image. */
            pixels=kmalloc(base*base*4*2);
            if (!pixels) { e=-ENOMEM; break; }
            struct render3d_texture_desc desc={.format=R3D_ARGB8888,.width=base,.height=base,.levels=levels};
            unsigned offset=0;
            for (unsigned level=0,side=base;level<levels;level++,side=side>1 ? side/2 : 1) {
                for (unsigned y=0;y<side && !e;y++) {
                    e=probe_checkpoint(0); if (e) break;
                    for (unsigned x=0;x<side;x++)
                    pixels[offset+y*side+x]=0xff000000u|((96+x*32/side+level*4)<<16)|((112+y*32/side+level*4)<<8)|(128+level*4+i);
                }
                desc.image[level]=(struct render3d_texture_image){pixels+offset,side*side*4,side*4};
                offset+=side*side;
            }
            if (!e) e=render3d_create_texture(s,1,context,&desc,&resource);
            kfree(pixels);
        } else {
            if (i==2) for (unsigned j=0;j<32*16;j++) pixels[j]=(j%32)<16 ? 0xff4080c0u : 0xffc04080u;
            e=render3d_create_resource(s,1,context,kinds[i],i==2 ? 32 : width,i==2 ? 16 : height,
                i==2 ? pixels : 0,i==2 ? 32*16*4 : 0,&resource);
        }
        if (!e && hardware) {
            struct render3d_resource *r=&s->contexts[0].resources[i];
            r->hardware=true; r->offset=kinds[i]==R3D_TEXTURE ? tex_at : at;
            e=savage3d_backend()->layout(&device,r);
            if (kinds[i]==R3D_TEXTURE) tex_at+=r->reserved; else at+=r->reserved;
            if (at>device.heap.first+device.heap.bytes-device.texture_bytes || tex_at>device.heap.first+device.heap.bytes) e=-ENOMEM;
        }
    }
    if (e) { (void)render3d_close(s); kfree(s); return e; }
    *out=s; return 0;
}
static int fullsize_diagnostic(struct render3d_service **scratch)
{
    /* Minimum probe has completed/restored. Scratch allocation and scalar
     * reference rendering never occur inside presenter/engine ownership. */
    if (*scratch) { (void)render3d_close(*scratch); kfree(*scratch); *scratch=0; }
    struct render3d_service *reference=0;
    probe_phase(0,"fullsize_prepare",0,true,0);
    device.fullsize_testing=true;
    int e=scratch_create(scratch,device.mode_width,device.mode_height,true,true);
    device.fullsize_testing=false;
    if (!e) e=scratch_create(&reference,device.mode_width,device.mode_height,true,false);
    probe_phase(0,"fullsize_prepare",0,false,e);
    if (!e) {
        device.consumer=*scratch;
        e=savage3d_fullsize_probe(&device,reference);
        device.consumer=0;
    }
    if (reference) { (void)render3d_close(reference); kfree(reference); }
    klog("[3d] fullsize result=%s error=%d width=%u height=%u samples=%u reference=%08x hardware=%08x channel_error=%u depth_error=%u elapsed_us=%llu phase=%u guard=%u quarantined=%u",
        !e ? "qualified" : "refused",e,device.mode_width,device.mode_height,device.sample_count,
        device.reference_checksum,device.hardware_checksum,device.max_channel_error,device.max_depth_error,
        device.fullsize_us,device.failed_phase,device.guard_offset,device.lost);
    klog("[3d] precision result=%s error=%d delta_q16=%u filter_bits=%u lod_delta_q8=16 phase=%u samples=%u levels0=%u levels1=%u expected=%08x actual=%08x",
        device.perspective_qualified ? "qualified" : "refused",device.precision_error,
        device.sample_delta_q16,device.filter_fraction_bits,device.precision_phase,device.precision_samples,
        device.measured_filter_levels[0],device.measured_filter_levels[1],device.expected,device.actual);
    return e;
}
static int attachment_diagnostic(struct render3d_service **scratch,bool depth24)
{
    /* Never started after a budget overrun or an integrity failure. */
    if (!device.fullsize_qualified || device.lost) return -ENODEV;
    if (budget_reason) return -ETIMEDOUT;
    const char *prepare=depth24 ? "d24s8_prepare" : "rgb565_prepare";
    if (*scratch) { (void)render3d_close(*scratch); kfree(*scratch); *scratch=0; }
    struct render3d_service *reference=0;
    probe_phase(0,prepare,0,true,0);
    device.depth24_testing=depth24; device.color565_testing=!depth24; device.fullsize_testing=true;
    int e=scratch_create(scratch,device.mode_width,device.mode_height,true,true);
    device.fullsize_testing=false;
    if (!e) e=scratch_create(&reference,device.mode_width,device.mode_height,true,false);
    probe_phase(0,prepare,0,false,e);
    if (!e) {
        device.consumer=*scratch;
        e=depth24 ? savage3d_depth24_probe(&device,reference) : savage3d_color565_probe(&device,reference);
        device.consumer=0;
    }
    device.depth24_testing=device.color565_testing=false;
    if (reference) { (void)render3d_close(reference); kfree(reference); }
    klog("[3d] attachment format=%s result=%s error=%d width=%u height=%u depth_error=%u expected=%08x actual=%08x quarantined=%u budget=%s",
        depth24 ? "D24S8" : "RGB565",(depth24 ? device.depth24_qualified : device.color565_qualified) ? "qualified" : "refused",e,device.mode_width,device.mode_height,
        device.max_depth_error,device.expected,device.actual,device.lost,budget_reason ? budget_reason : "ok");
    return e;
}

static bool token(void)
{
    for (unsigned i=0;i<sizeof(g_boot.options) && g_boot.options[i];) {
        unsigned first=i;
        while (i<sizeof(g_boot.options) && g_boot.options[i] && g_boot.options[i]!=' ' &&
               g_boot.options[i]!='\r' && g_boot.options[i]!='\n' && g_boot.options[i]!='\t') i++;
        if (i-first==8 && !memcmp(g_boot.options+first,"3d=probe",8)) return true;
        if (i<sizeof(g_boot.options) && g_boot.options[i]) i++;
    }
    /* fx-selftest: the T boot forces the background qualification. */
    return (g_boot.flags&CBI_F_SELF_TEST)!=0;
}
/* fx-selftest: the qualification fills last_report completely before it
 * publishes probe_finished (release), so a copy taken after observing it
 * (acquire) is stable; before that only the scheduled/active/finished flags
 * are meaningful to the reader. No interrupt producer writes these fields. */
void render3d_qualification_report(struct render3d_qualification_report *out)
{
    bool finished=__atomic_load_n(&probe_finished,__ATOMIC_ACQUIRE);
    *out=last_report;
    /* The qualified backend serves the SuperSavage/IX only (qualification). */
    const struct pci_func *pci=display_driver_owner();
    out->present=pci && pci->vendor==0x5333 && pci->device==0x8c2e;
    out->scheduled=reported || probe_task || out->finished;
    out->active=probe_active;
    out->finished=finished && out->finished;
}
static void qualification(void)
{
    struct savage2d *engine=pending_engine;
    const struct fb_device *fb=&pending_fb;
    if (!layout_key.valid && desktop_generation!=fb->generation) {
        klog("[3d] probe result=blocked reason=mode_changed error=%d hardware=0 marker=clear elapsed_us=0",-ENODEV);
        last_report=(struct render3d_qualification_report){.scheduled=true,.finished=true,.error=-ENODEV,
            .result="blocked",.reason="mode_changed",.marker="clear"};
        return;
    }
    probe_active=true; budget_reason=0; checkpoint_count=0; revalidate_deadline=0;
    last_report=(struct render3d_qualification_report){.scheduled=true};
    optional_gate=optional_stop=false; optional_reason=0; phase_deadline=UINT64_MAX;
    probe_started=probe_now();
    probe_phase(0,"prepare",0,true,0);
    const struct pci_func *pci=display_driver_owner();
    klog("[3d] probe phase=identity pci=%04x:%04x revision=%u bar0=%08x bar1=%08x bar2=%08x active=%u quarantine=%u idle_mask=%08x idle_value=%08x elapsed_us=%llu",
        pci ? pci->vendor : 0,pci ? pci->device : 0,pci ? pci->revision : 0,
        pci ? pci->bar[0] : 0,pci ? pci->bar[1] : 0,pci ? pci->bar[2] : 0,
        engine->active,engine->quarantined,engine->idle_mask,engine->idle_value,probe_now()-probe_started);
    uint32_t required=0;
    struct render3d_extent heap={0};
    int e;
    /* The first desktop frame may finish while the 2D marker CLEAR is still
     * retiring. Wait here, never in .record, within the preparation budget. */
    do {
        e=probe_checkpoint(0); if (e) break;
        e=fbdev_render3d_prepare(fb->generation);
        if (e==-EBUSY) task_sleep_ms(1);
    } while (e==-EBUSY);
    bool pinned=!e;
    /* Borrow only the revocable cache. No 3D write is possible before the
     * durable marker; this transition merely synchronizes the existing 2D. */
    if (!e) {
        device.engine=engine; device.generation=fb->generation;
        do {
            e=cache_transfer(true);
            if (e==-EBUSY) e=probe_checkpoint(0) ? -ETIMEDOUT : -EBUSY;
        } while (e==-EBUSY);
    }
    if (!e) e=savage3d_mode_heap(engine,fb,g_boot.display.vram_bytes,&heap,&required);
    uint32_t free_bytes=0,largest_bytes=0;
    if (heap.bytes && fb) {
        int inventory=available_heap(fb,&heap,&free_bytes,&largest_bytes);
        if (!e) e=inventory;
        if (!e && largest_bytes<required) e=-ENOMEM;
    }
    last_report.heap_bytes=heap.bytes; last_report.required=required;
    last_report.free_vram=free_bytes; last_report.largest_vram=largest_bytes;
    const char *reason="mode_heap";
    uint32_t alias=pci ? pci->bar[2]&~15u : 0;
    if (!e && (!pci || pci->vendor!=0x5333 || pci->device!=0x8c2e || (pci->command&3)!=3 ||
        (pci->bar[2]&7) || !alias || (alias&0xffffffu) ||
        !safe_extent(alias,SAVAGE3D_ALIAS_BYTES) || !fb ||
        fb->phys!=(pci->bar[1]&~15u) || fb->phys>UINT32_MAX-heap.first || !safe_extent(fb->phys+heap.first,heap.bytes))) {
        e=-ENODEV; reason="identity_alias";
    }
    klog("[3d] probe phase=preflight vram=%u scanout=%u offscreen_end=%u cursor=%u free_vram=%u largest_vram=%u required=%u host_free_pages=%u host_reserved_pages=%u elapsed_us=%llu",
        g_boot.display.vram_bytes,fb ? fb->size : 0,fb ? engine->rows*fb->pitch : 0,
        engine->cursor_offset,free_bytes,largest_bytes,required,pmm_free_count(),pmm_total_usable()-pmm_free_count(),probe_now()-probe_started);
    if (!e && !marker_vfs) { e=-ENODEV; reason="marker_store"; }
    if (!e) {
        device=(struct savage3d_device){.engine=engine,.generation=fb->generation,.heap=heap,
            .mode_width=fb->width,.mode_height=fb->height,
            .texture_bytes=SAVAGE3D_TEXTURE_MIN,
            .memory={0,memory_read,memory_write,enter,leave},.job_yield=job_yield,.lost_notify=lost_notify,
            .probe_checkpoint=probe_checkpoint,.probe_phase=probe_phase,
            .reserve=reserve_heap,.unreserve=return_heap,.detach_owner=return_heap};
        /* Budget optional D24/S8 from this same claimed heap before fixing
         * the texture pool. Keep its existing minimum and the scratch/guards;
         * if it does not fit, preserve the D16 budget and decline D24. */
        uint32_t rows=768;
        uint32_t cp=4096*rows;
        uint32_t depth24_required=3*((cp+4095)&~4095u)+4*8192u+24576u;
        if (heap.bytes>=depth24_required+480u*1024)
            device.texture_bytes=(heap.bytes-depth24_required)&~4095u;
        else if (heap.bytes>=2*cp+2048*rows+4*8192u+24576u+480u*1024)
            device.texture_bytes=(heap.bytes-(2*cp+2048*rows+4*8192u+24576u))&~4095u;
        else if (heap.bytes>=required+224u*1024)
            device.texture_bytes=480u*1024;
        e=claim_heap(fb,alias); reason="claim_mapping";
    }
    struct render3d_service *scratch=0;
    if (!e) { e=scratch_create(&scratch,64,64,false,true); reason="scratch"; }
    if (!e) e=probe_checkpoint(0);
    probe_phase(0,"prepare",0,false,e);
    if (!e) {
        bool previous=false;
        e=probe_marker(pci,false,&previous);
        marker_retained=previous;
        marker_unknown=e && !previous; /* failed commit/close may have left BEGIN */
        if (previous) e=-EACCES;
        marker_fresh=!e; reason=previous ? "retained_marker" : "marker_begin";
    }
    if (!e) {
        e=probe_checkpoint(0);
    }
    if (!e) {
        klog("[3d] probe phase=armed heap_first=%u heap_bytes=%u bar2_mapped=%u marker=durable elapsed_us=%llu",
            heap.first,heap.bytes,SAVAGE3D_ALIAS_BYTES,probe_now()-probe_started);
        device.consumer=scratch;
        if (layout_key.valid) revalidate_deadline=probe_now()+SAVAGE3D_REVALIDATE_US;
        probe_phase(0,"minimum",0,true,0);
        e=savage3d_probe(&device); reason="engine_probe";
        if (!e) e=probe_checkpoint(0);
        probe_phase(0,"minimum",0,false,e);
        device.consumer=0;
        revalidate_deadline=0;
        if (optional_reason) {
            /* The cached revalidation missed its 50 ms deadline. A restored,
             * idle engine refuses this mode cleanly; the marker is cleared. */
            reason=optional_reason;
            if (engine_restored()) { optional_reason=0; e=-ETIMEDOUT; }
            else { budget_reason=optional_reason; optional_reason=0; }
        }
        if (!e) {
            bool same=layout_key.valid && layout_key.vendor==pci->vendor && layout_key.id==pci->device &&
                layout_key.revision==pci->revision && layout_key.lfb==fb->phys && layout_key.alias==alias &&
                layout_key.vram==g_boot.display.vram_bytes && layout_key.cursor==engine->cursor_offset;
            if (!same) {
                memset(targets,0,sizeof(targets));
                layout_key.valid=false;
                /* Quake III's 640x480 first, then the desktop-mode lease size,
                 * D24/S8 (its default 24/8 request) before RGB565: an overrun
                 * that stops the optional gates keeps what the T23 plays. */
                const unsigned widths[]={640,1024,800},heights[]={480,768,600};
                optional_gate=true;
                for (unsigned i=0;i<3 && !budget_reason && !device.lost && !optional_stop;i++) {
                    device.mode_width=widths[i]; device.mode_height=heights[i];
                    device.sample_count=device.reference_checksum=device.hardware_checksum=0;
                    device.max_channel_error=device.max_depth_error=device.failed_phase=0;
                    device.expected=device.actual=device.guard_offset=0; device.fullsize_us=0;
                    device.precision_error=0; device.precision_phase=device.precision_samples=0;
                    device.fullsize_qualified=device.perspective_qualified=device.depth24_qualified=device.color565_qualified=false;
                    (void)fullsize_diagnostic(&scratch);
                    bool more=optional_contained("fullsize");
                    for (unsigned depth24=2;more && depth24-->0;) {
                        (void)attachment_diagnostic(&scratch,depth24);
                        more=optional_contained(depth24 ? "d24s8" : "rgb565");
                    }
                    save_target(i);
                }
                optional_gate=false;
                if (!budget_reason && !device.lost) {
                    layout_key.vendor=pci->vendor; layout_key.id=pci->device; layout_key.revision=pci->revision;
                    layout_key.lfb=fb->phys; layout_key.alias=alias; layout_key.vram=g_boot.display.vram_bytes;
                    layout_key.cursor=engine->cursor_offset; layout_key.generation++;
                    layout_key.valid=true;
                }
            } else klog("[3d] probe phase=revalidate layout=%u generation=%u pitch=%u width=%u height=%u bound_us=%u elapsed_us=%llu",
                layout_key.generation,fb->generation,fb->pitch,fb->width,fb->height,SAVAGE3D_REVALIDATE_US,probe_now()-probe_started);
            select_target(fb);
            if (device.lost) { layout_key.valid=false; e=-EIO; }
        }
    }
    if (!e) e=probe_check();
    if (budget_reason) { e=-ETIMEDOUT; reason=budget_reason; }
    if (e) device.qualified=device.fullsize_qualified=device.perspective_qualified=
        device.depth24_qualified=device.color565_qualified=false;
    revalidate_deadline=0;
    if (marker_fresh && !device.lost && !budget_reason) {
        int clear=probe_marker(pci,true,0);
        if (clear) { e=clear; reason="marker_clear"; }
        else marker_fresh=false;
        if (probe_check()) { e=-ETIMEDOUT; reason=budget_reason; }
        if (e) device.qualified=device.fullsize_qualified=device.perspective_qualified=
            device.depth24_qualified=device.color565_qualified=false;
    }
    if (scratch) {
        if (device.lost) {
            /* Retain shadows/metadata too: a possibly running writer is not
             * an excuse to recycle any referenced object. */
            device.consumer=scratch; render3d_device_lost(scratch);
            scratch->contexts[0].hardware=true;
        } else { (void)render3d_close(scratch); kfree(scratch); }
    }
    engine->render3d_qualified=device.qualified && !e;
    if (!device.lost && device.engine) {
        int released=release_claims();
        if (released) { device.lost=true; engine->quarantined=true; engine->active=false; }
        else if (cache_borrowed) {
            int returned=cache_transfer(false);
            if (returned) klog("[3d] probe phase=cache_return error=%d",returned);
        }
    }
    if (pinned) fbdev_render3d_prepared();
    probe_active=false;
    device.probe_checkpoint=0; device.probe_phase=0;
    {
        struct render3d_qualification_report *r=&last_report;
        r->qualified=!e && engine->render3d_qualified; r->lost=device.lost; r->error=e;
        r->result=!e ? "qualified" : device.lost ? "failed" : "blocked";
        r->reason=!e ? "pixel_depth_copy_guards_restore" : reason;
        r->marker=marker_fresh || marker_retained ? "retained" : marker_unknown ? "unknown" : "clear";
        r->elapsed_us=probe_now()-probe_started; r->packets=device.packets; r->triangles=device.triangles;
        r->copies=device.copies; r->failed_phase=device.failed_phase; r->texture_bytes=device.texture_bytes;
        r->fullsize=device.fullsize_qualified; r->perspective=device.perspective_qualified;
        r->depth24=device.depth24_qualified; r->color565=device.color565_qualified;
        r->finished=true;
    }
    klog("[3d] probe result=%s reason=%s error=%d hardware=%u phase=%u expected=%08x actual=%08x guard=%u status=%08x packets=%llu triangles=%llu copies=%llu marker=%s elapsed_us=%llu",
        !e ? "qualified" : device.lost ? "failed" : "blocked",!e ? "pixel_depth_copy_guards_restore" : reason,e,
        engine->render3d_qualified,device.failed_phase,device.expected,device.actual,device.guard_offset,
        engine->status,device.packets,device.triangles,device.copies,
        marker_fresh || marker_retained ? "retained" : marker_unknown ? "unknown" : "clear",probe_now()-probe_started);
}
static bool probe_ready(void *ctx) { (void)ctx; return desktop_ready; }
static void probe_main(void *ctx)
{
    (void)ctx;
    (void)kwait_wait_until(&probe_wait,probe_ready,0,UINT64_MAX);
    qualification();
    __atomic_store_n(&probe_finished,true,__ATOMIC_RELEASE);
}
/* A task cannot reap itself: its return reaches task_exit and leaves a
 * zombie (stack and task allocations). A later display notification on
 * another task takes the pointer exactly once and reaps that zombie. */
static void reap_probe(void)
{
    if (!__atomic_load_n(&probe_finished,__ATOMIC_ACQUIRE)) return;
    struct task *t=__atomic_exchange_n(&probe_task,(struct task *)0,__ATOMIC_ACQ_REL);
    if (!t) return;
    if (t!=g_current && t->state==T_ZOMBIE) { task_reap(t); return; }
    __atomic_store_n(&probe_task,t,__ATOMIC_RELEASE); /* still exiting */
}
void savage3d_desktop_presented(uint32_t generation)
{
    desktop_generation=generation;
    reap_probe();
    if (!probe_task || desktop_ready) return;
    desktop_ready=true;
    kwait_wake_all(&probe_wait);
}
void savage3d_boot_diagnostic(struct savage2d *engine,const struct fb_device *fb)
{
    reap_probe();
    if (cache_return_pending) return_heap(0);
    if (!token() || !engine || !engine->active || !fb ||
        (reported && scheduled_generation==fb->generation) || probe_task ||
        marker_fresh || marker_retained || marker_unknown || device.lost) return;
    reported=true; scheduled_generation=fb->generation;
    pending_engine=engine; pending_fb=*fb;
    const struct pci_func *pci=display_driver_owner();
    bool same=layout_key.valid && pci && layout_key.vendor==pci->vendor && layout_key.id==pci->device &&
        layout_key.revision==pci->revision && layout_key.lfb==fb->phys && layout_key.alias==(pci->bar[2]&~15u) &&
        layout_key.vram==g_boot.display.vram_bytes && layout_key.cursor==engine->cursor_offset;
    if (same) { revalidation_pending=true; return; }
    layout_key.valid=false;
    desktop_ready=desktop_generation==fb->generation;
    kwait_init(&probe_wait);
    probe_finished=false;
    probe_task=task_create_kernel("savage-3d-probe",probe_main,0,P_IDLE);
    if (!probe_task) { klog("[3d] probe result=blocked reason=worker_memory error=%d",-ENOMEM); return; }
    klog("[3d] probe phase=queued elapsed_us=0 budget_us=%u after=desktop_frame priority=idle",SAVAGE3D_PROBE_TOTAL_US);
    task_start(probe_task);
}
