/* Production glue for the existing Matrox owner; no independent PCI claim.
 * Algorithms/register formats are independently implemented from the pinned
 * sources in f7-3d-acceleration-contract.md. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/mm.h>
#include <ciuki/desktop.h>
#include <ciuki/display_driver.h>
#include "matrox3d.h"
static struct matrox3d_device device;
static struct vfs *marker_vfs;
static volatile uint8_t *linear;
static void *linear_window;
static uint32_t linear_capacity;
static int claims[1]={-1};
static gen_t generations[1];
static bool reported, marker_fresh;
void matrox3d_bind_vfs(struct vfs *vfs) { marker_vfs=vfs; }
static uint32_t memory_read(void *ctx,uint32_t offset,unsigned bytes)
{
    (void)ctx;
    volatile uint8_t *p=linear+offset-device.heap.first;
    if (bytes==2) return *(volatile uint16_t *)p;
    return *(volatile uint32_t *)p;
}
static void memory_write(void *ctx,uint32_t offset,uint32_t value,unsigned bytes)
{
    (void)ctx;
    volatile uint8_t *p=linear+offset-device.heap.first;
    if (bytes==2) *(volatile uint16_t *)p=(uint16_t)value;
    else *(volatile uint32_t *)p=value;
}
static int enter(void *ctx,uint32_t generation)
{ (void)ctx; return fbdev_render3d_enter(generation); }
static void leave(void *ctx) { (void)ctx; fbdev_render3d_leave(); }
static bool safe_extent(uint32_t first,uint32_t bytes)
{
    if (first<0x100000 || !bytes || bytes>UINT32_MAX-first) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *m=&g_boot.e820[i];
        if (m->type==CBI_E820_RAM && first<m->base+m->length && (uint64_t)first+bytes>m->base) return false;
    }
    return !(first<g_boot.kernel_end && (uint64_t)first+bytes>g_boot.kernel_start);
}
static void available_heap(const struct fb_device *fb,struct render3d_extent *heap,
                           uint32_t *free_bytes,uint32_t *largest_bytes)
{
    struct registry_inventory_copy inventory;
    registry_inventory(&inventory);
    uint32_t run=0,largest_first=heap->first;
    *free_bytes=*largest_bytes=0;
    uint32_t end=heap->first+heap->bytes;
    for (uint32_t at=heap->first;at<end;at+=4096) {
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
}
static bool release_proof(int handle,gen_t generation)
{
    if (device.engine && (!device.engine->cpu_safe)) return false;
    if (handle==claims[0] && generation==generations[0] && linear) {
        vmm_revoke_mmio((void *)linear,device.heap.bytes); linear=0;
    }
    return true;
}
static int release_claims(void)
{
    int error=0;
    for (int i=0;i>=0;i--) if (claims[i]>=0) {
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
    for (unsigned i=0;i<1;i++) if (claims[i]>=0) registry_quarantine(claims[i],generations[i]);
}
int matrox3d_owner_stop(struct matrox_engine *engine)
{
    if (device.engine!=engine) return 0;
    /* Called under presenter before its 2D mappings/lease can be revoked. */
    device.qualified=device.fullsize_qualified=device.perspective_qualified=false;
    if (device.consumer) {
        int e=render3d_close(device.consumer);
        if (e) return e;
        device.consumer=0;
    }
    if (device.lost || engine->quarantined || !engine->cpu_safe) return -EIO;
    int e=release_claims();
    if (!e) device.lost=true; /* old generation cannot be reattached */
    return e;
}
int matrox3d_owner_attach(struct render3d_service *s,const struct render3d_host *host,uint32_t generation)
{
    if (!device.qualified || !s || !host || marker_fresh || device.lost || generation!=device.generation) return -ENODEV;
    if (device.consumer) return -EBUSY;
    render3d_init(s,host,generation,matrox3d_backend(),&device,device.heap);
    device.consumer=s; return 0;
}
/* Probe buffers exceed the small heap; charge page-backed allocations and
 * family residency together under the existing service RAM ceiling. */
static uint32_t probe_resident_bytes;
static void *host_alloc(void *ctx,size_t bytes)
{
    (void)ctx;
    if (!bytes || bytes>R3D_RAM_LIMIT-8) return 0;
    uint32_t extent=(uint32_t)bytes+8;
    uint32_t charge=(extent+PAGE_SIZE-1)&~(PAGE_SIZE-1);
    uint32_t backend=matrox3d_backend()->resident_bytes;
    if (backend>R3D_RAM_LIMIT-probe_resident_bytes ||
        charge>R3D_RAM_LIMIT-probe_resident_bytes-backend) return 0;
    uint32_t *p=desktop_buffer_alloc(extent,MM_IO_REQUEST);
    if (!p) return 0;
    probe_resident_bytes+=charge;
    p[0]=extent; p[1]=charge; return p+2;
}
static void host_free(void *ctx,void *data)
{
    (void)ctx;
    if (data) {
        uint32_t *p=(uint32_t *)data-2,charge=p[1];
        desktop_buffer_free(p,p[0],MM_IO_REQUEST);
        probe_resident_bytes-=charge;
    }
}
static uint64_t host_now(void *ctx)
{ (void)ctx; return device.engine->io.now_us(device.engine->io.ctx); }
static int scratch_create(struct render3d_service **out,unsigned width,unsigned height,bool full,bool hardware)
{
    /* The existing inventory-derived pool must contain both mip chains and
     * their guards. This is preflight, never a larger VRAM claim or limit. */
    if (full && hardware && device.texture_bytes<480u*1024) return -ENOMEM;
    struct render3d_service *s=host_alloc(0,sizeof(*s)); if (!s) return -ENOMEM;
    const struct render3d_host host={0,host_alloc,host_free,host_now,0};
    render3d_init(s,&host,device.generation,0,0,(struct render3d_extent){0});
    uint32_t context;
    int e=render3d_create_context(s,1,R3D_SOFTWARE,&context);
    const enum render3d_kind kinds[]={R3D_COLOR,R3D_D16,R3D_TEXTURE,R3D_LINEAR_COLOR,R3D_TEXTURE};
    uint32_t at=device.heap.first,tex_at=device.heap.first+device.heap.bytes-device.texture_bytes;
    for (unsigned i=0;!e && i<(full ? 5u : 4u);i++) {
        uint32_t resource;
        uint32_t *pixels=(void *)s->ring[2];
        if (full && kinds[i]==R3D_TEXTURE) {
            unsigned base=i==2 ? 256 : 128,levels=i==2 ? 9 : 8;
            /* The copied texture outlives this bounded staging allocation.
             * Never use a 64 KiB command-ring slot for a 256-square image. */
            pixels=host_alloc(0,base*base*4*2);
            if (!pixels) { e=-ENOMEM; break; }
            struct render3d_texture_desc desc={.format=R3D_ARGB8888,.width=base,.height=base,.levels=levels};
            unsigned offset=0;
            for (unsigned level=0,side=base;level<levels;level++,side=side>1 ? side/2 : 1) {
                for (unsigned y=0;y<side;y++) for (unsigned x=0;x<side;x++)
                    pixels[offset+y*side+x]=0xff000000u|((96+x*32/side+level*4)<<16)|((112+y*32/side+level*4)<<8)|(128+level*4+i);
                desc.image[level]=(struct render3d_texture_image){pixels+offset,side*side*4,side*4};
                offset+=side*side;
            }
            e=render3d_create_texture(s,1,context,&desc,&resource);
            host_free(0,pixels);
        } else {
            if (i==2) for (unsigned j=0;j<32*16;j++) pixels[j]=(j%32)<16 ? 0xff4080c0u : 0xffc04080u;
            e=render3d_create_resource(s,1,context,kinds[i],i==2 ? 32 : width,i==2 ? 16 : height,
                i==2 ? pixels : 0,i==2 ? 32*16*4 : 0,&resource);
        }
        if (!e && hardware) {
            struct render3d_resource *r=&s->contexts[0].resources[i];
            r->hardware=true; r->offset=kinds[i]==R3D_TEXTURE ? tex_at : at;
            e=matrox3d_backend()->layout(&device,r);
            if (kinds[i]==R3D_TEXTURE) tex_at+=r->reserved; else at+=r->reserved;
        }
    }
    if (e) { (void)render3d_close(s); host_free(0,s); return e; }
    *out=s; return 0;
}
static void fullsize_diagnostic(struct render3d_service **scratch)
{
    /* Minimum probe has completed/restored. Scratch allocation and scalar
     * reference rendering never occur inside presenter/engine ownership. */
    (void)render3d_close(*scratch); host_free(0,*scratch); *scratch=0;
    struct render3d_service *reference=0;
    device.fullsize_testing=true;
    int e=scratch_create(scratch,device.mode_width,device.mode_height,true,true);
    device.fullsize_testing=false;
    if (!e) e=scratch_create(&reference,device.mode_width,device.mode_height,true,false);
    if (!e) {
        device.consumer=*scratch;
        e=matrox3d_fullsize_probe(&device,reference);
        device.consumer=0;
    }
    if (reference) { (void)render3d_close(reference); host_free(0,reference); }
    klog("[3d] fullsize result=%s error=%d width=%u height=%u samples=%u reference=%08x hardware=%08x channel_error=%u depth_error=%u elapsed_us=%llu phase=%u guard=%u quarantined=%u",
        !e ? "qualified" : "refused",e,device.mode_width,device.mode_height,device.sample_count,
        device.reference_checksum,device.hardware_checksum,device.max_channel_error,device.max_depth_error,
        device.fullsize_us,device.failed_phase,device.guard_offset,device.lost);
    klog("[3d] precision result=%s error=%d delta_q16=%u filter_bits=%u lod_delta_q8=16 phase=%u samples=%u levels0=%u levels1=%u expected=%08x actual=%08x",
        device.perspective_qualified ? "qualified" : "refused",device.precision_error,
        device.sample_delta_q16,device.filter_fraction_bits,device.precision_phase,device.precision_samples,
        device.measured_filter_levels[0],device.measured_filter_levels[1],device.expected,device.actual);
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
    return false;
}
void matrox3d_boot_diagnostic(struct matrox_engine *engine,const struct fb_device *fb)
{
    if (reported || !token()) return;
    reported=true;
    const struct pci_func *pci=display_driver_owner();
    klog("[3d] probe phase=identity pci=%04x:%04x revision=%u bar0=%08x bar1=%08x bar2=%08x active=%u quarantine=%u",
        pci ? pci->vendor : 0,pci ? pci->device : 0,pci ? pci->revision : 0,
        pci ? pci->bar[0] : 0,pci ? pci->bar[1] : 0,pci ? pci->bar[2] : 0,
        engine->active,engine->quarantined);
    uint32_t required=0;
    struct render3d_extent heap={0};
    int e=matrox3d_mode_heap(engine,fb,g_boot.display.vram_bytes,engine->cursor_offset,&heap,&required);
    uint32_t free_bytes=0,largest_bytes=0;
    if (heap.bytes && fb) {
        available_heap(fb,&heap,&free_bytes,&largest_bytes);
        if (!e && largest_bytes<required) e=-ENOMEM;
    }
    const char *reason="mode_heap";
    if (!e && (!pci || !matrox_probe(pci) || (pci->command&3)!=3 ||
        (pci->bar[0]&7) || !fb || fb->phys!=(pci->bar[0]&~15u) ||
        fb->phys>UINT32_MAX-heap.first || !safe_extent(fb->phys+heap.first,heap.bytes) ||
        !engine->active || engine->quarantined ||
        ((pci->device==0x0525 || pci->device==0x2527) && (engine->io.read32(engine->io.ctx,M_C2CTL)&1u)))) {
        e=-ENODEV; reason="identity_heap";
    }
    klog("[3d] probe phase=preflight vram=%u scanout=%u offscreen_end=%u cursor=%u free_vram=%u largest_vram=%u required=%u host_free_pages=%u host_reserved_pages=%u",
        g_boot.display.vram_bytes,fb ? fb->size : 0,fb ? fb->size : 0,
        engine->cursor_offset,free_bytes,largest_bytes,required,pmm_free_count(),pmm_total_usable()-pmm_free_count());
    bool pinned=false;
    if (!e) { e=fbdev_render3d_prepare(fb->generation); pinned=!e; reason="presenter_pin"; }
    if (!e && !marker_vfs) { e=-ENODEV; reason="marker_store"; }
    if (!e) {
        device=(struct matrox3d_device){.engine=engine,.generation=fb->generation,.heap=heap,
            .mode_width=fb->width,.mode_height=fb->height,
            .texture_bytes=(heap.bytes-(required-MATROX3D_TEXTURE_MIN))&~4095u,
            .g400=pci->device==0x0525 || pci->device==0x2527,
            .memory={0,memory_read,memory_write,enter,leave},.lost_notify=lost_notify};
        uint32_t bases[]={fb->phys+heap.first}, sizes[]={heap.bytes};
        for (unsigned i=0;!e && i<1;i++) {
            claims[i]=registry_claim(RES_MMIO,bases[i],bases[i]+sizes[i],"matrox-3d",false);
            if (claims[i]<0) { e=claims[i]; reason="claim"; break; }
            generations[i]=registry_get(claims[i])->generation;
            e=registry_activate(claims[i],generations[i]); reason="claim_activate";
        }
        if (!e) {
            if (!linear_window || linear_capacity<heap.bytes) {
                if (!linear_window) { linear_capacity=heap.bytes; linear_window=vmm_reserve_mmio(linear_capacity); }
            }
            linear=linear_window ? vmm_replace_mmio(linear_window,linear_capacity,bases[0],heap.bytes) : 0;
            if (!linear) e=-ENOMEM;
            reason="mapping";
        }
    }
    struct render3d_service *scratch=0;
    if (!e) { e=scratch_create(&scratch,64,64,false,true); reason="scratch"; }
    if (!e) {
        bool previous=false;
        e=display_attempt_marker(marker_vfs,"3d-matrox",pci->vendor,pci->device,0,false,&previous,0);
        if (previous) e=-EACCES;
        marker_fresh=!e; reason=previous ? "retained_marker" : "marker_begin";
    }
    if (!e) {
        klog("[3d] probe phase=armed heap_first=%u heap_bytes=%u marker=durable",heap.first,heap.bytes);
        device.consumer=scratch;
        e=matrox3d_probe(&device); reason="engine_probe";
        device.consumer=0;
        if (!e) {
            fullsize_diagnostic(&scratch);
            if (device.lost) e=-EIO;
        }
    }
    if (marker_fresh && !device.lost) {
        int clear=display_attempt_marker(marker_vfs,"3d-matrox",pci->vendor,pci->device,0,true,0,0);
        if (clear) { e=clear; reason="marker_clear"; device.qualified=false; }
        else marker_fresh=false;
    }
    if (scratch) {
        if (device.lost) {
            /* Retain shadows/metadata too: a possibly running writer is not
             * an excuse to recycle any referenced object. */
            device.consumer=scratch; render3d_device_lost(scratch);
            scratch->contexts[0].hardware=true;
        } else { (void)render3d_close(scratch); host_free(0,scratch); }
    }
    if (e) device.qualified=false;
    if (e && !device.lost && device.engine) {
        int released=release_claims();
        if (released) { device.lost=true; engine->quarantined=true; engine->active=false; }
    }
    if (pinned) fbdev_render3d_prepared();
    klog("[3d] probe result=%s reason=%s error=%d hardware=%u phase=%u expected=%08x actual=%08x guard=%u status=%08x writes=%llu triangles=%llu copies=%llu marker=%s",
        !e ? "qualified" : device.lost ? "failed" : "blocked",!e ? "pixel_depth_copy_guards_restore" : reason,e,
        device.qualified,device.failed_phase,device.expected,device.actual,device.guard_offset,
        engine->io.read32(engine->io.ctx,M_STATUS),device.writes,device.triangles,device.copies,marker_fresh ? "retained" : "clear");
}
