/* Production glue for the existing Mach64 2D owner (f7-27); no independent
 * PCI claim, no BAR sizing, no PCI command write. Register formats are
 * independently implemented from the pinned sources of
 * docs/design/f7-family-ati-mach64.md. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/mm.h>
#include <ciuki/desktop.h>
#include <ciuki/display_driver.h>
#include "mach64_3d.h"
#include "registers3d.h"
static struct mach64_3d_device device;
static struct vfs *marker_vfs;
static volatile uint8_t *heap_map;
static void *heap_window;
static uint32_t heap_capacity;
static int claim=-1;
static gen_t claim_generation;
static bool reported, marker_fresh;
void mach64_3d_bind_vfs(struct vfs *vfs) { marker_vfs=vfs; }
static uint32_t memory_read(void *ctx,uint32_t offset,unsigned bytes)
{
    (void)ctx;
    volatile uint8_t *p=heap_map+offset-device.heap.first;
    if (bytes==2) return *(volatile uint16_t *)p;
    return *(volatile uint32_t *)p;
}
static void memory_write(void *ctx,uint32_t offset,uint32_t value,unsigned bytes)
{
    (void)ctx;
    volatile uint8_t *p=heap_map+offset-device.heap.first;
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
/* Registry snapshot first: every live MMIO reservation (quarantine too) is
 * excluded page-wise; a truncated inventory leaves no free page. */
static void available_heap(const struct fb_device *fb,struct render3d_extent *heap,
                           uint32_t *free_bytes,uint32_t *largest_bytes)
{
    struct registry_inventory_copy inventory;
    registry_inventory(&inventory);
    uint32_t run=0,largest_first=heap->first,end=heap->first+heap->bytes;
    *free_bytes=*largest_bytes=0;
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
    if (device.engine && !device.engine->cpu_safe) return false;
    if (handle==claim && generation==claim_generation && heap_map) {
        vmm_revoke_mmio((void *)heap_map,device.heap.bytes); heap_map=0;
    }
    return true;
}
static int release_claim(void)
{
    if (claim<0) return 0;
    const struct resource *r=registry_get(claim);
    /* The registry cannot release an unactivated claim; retain it. */
    int e=r && r->state==RS_CLAIMED ? -EIO : registry_quiesce(claim,claim_generation,release_proof);
    if (!e) e=registry_release(claim,claim_generation);
    if (e) registry_quarantine(claim,claim_generation);
    else claim=-1;
    return e;
}
static void lost_notify(void *ctx)
{
    (void)ctx;
    if (claim>=0) registry_quarantine(claim,claim_generation);
}
int mach64_3d_owner_stop(struct m64_engine *engine)
{
    if (device.engine!=engine) return 0;
    /* Under presenter, before the 2D lease/mappings can be revoked. */
    device.qualified=device.minimum_qualified=device.fullsize_qualified=device.perspective_qualified=false;
    device.alpha_qualified=device.blend_qualified=false;
    engine->render3d_qualified=false;
    if (device.consumer) {
        int e=render3d_close(device.consumer);
        if (e) return e;
        device.consumer=0;
    }
    if (device.lost) return 0; /* quarantined claims stay retained */
    if (engine->quarantined || !engine->cpu_safe) return -EIO;
    int e=release_claim();
    if (!e) device.lost=true; /* this generation cannot be reattached */
    return e;
}
int mach64_3d_owner_attach(struct render3d_service *s,const struct render3d_host *host,uint32_t generation)
{
    if (!device.qualified || marker_fresh || device.lost || generation!=device.generation) return -ENODEV;
    if (device.consumer) return -EBUSY;
    render3d_init(s,host,generation,mach64_3d_backend(),&device,device.heap);
    device.consumer=s; return 0;
}
/* Probe surfaces and the service exceed the class-heap request limit.
 * Match the shared owner's page-backed, charged allocation contract. */
static uint32_t probe_resident_bytes;
static void *host_alloc(void *ctx,size_t bytes)
{
    (void)ctx;
    if (!bytes || bytes>R3D_RAM_LIMIT-8) return 0;
    uint32_t extent=(uint32_t)bytes+8;
    uint32_t charge=(extent+PAGE_SIZE-1)&~(PAGE_SIZE-1);
    uint32_t backend=mach64_3d_backend()->resident_bytes;
    if (backend>R3D_RAM_LIMIT-probe_resident_bytes ||
        charge>R3D_RAM_LIMIT-probe_resident_bytes-backend) return 0;
    probe_resident_bytes+=charge;
    uint32_t *p=desktop_buffer_alloc(extent,MM_IO_REQUEST);
    if (!p) { probe_resident_bytes-=charge; return 0; }
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
{ (void)ctx; return device.engine->io.now(device.engine->io.ctx); }
/* Scratch resources are allocated before any lock. Index 4 is the second
 * texture at mode size, or the software coverage reference for Q1. */
static int scratch_create(struct render3d_service **out,unsigned width,unsigned height,bool full,bool hardware)
{
    if (full && hardware && device.texture_bytes<MACH64_3D_FULL_TEXTURES) return -ENOMEM;
    struct render3d_service *s=host_alloc(0,sizeof(*s)); if (!s) return -ENOMEM;
    const struct render3d_host host={0,host_alloc,host_free,host_now,0};
    render3d_init(s,&host,device.generation,0,0,(struct render3d_extent){0});
    uint32_t context;
    int e=render3d_create_context(s,1,R3D_SOFTWARE,&context);
    const enum render3d_kind kinds[]={R3D_COLOR,R3D_D16,R3D_TEXTURE,R3D_LINEAR_COLOR,full ? R3D_TEXTURE : R3D_COLOR};
    uint32_t at=device.heap.first,tex_at=device.heap.first+device.heap.bytes-device.texture_bytes;
    for (unsigned i=0;!e && i<5;i++) {
        uint32_t resource;
        if (kinds[i]==R3D_TEXTURE) {
            unsigned w=full ? (i==2 ? 256 : 128) : 32,h=full ? w : 16;
            uint32_t *pixels=host_alloc(0,w*h*4);
            if (!pixels) { e=-ENOMEM; break; }
            for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++)
                pixels[y*w+x]=full ? 0xff000000u|((96+x*32/w)<<16)|((112+y*32/h)<<8)|(128+i) :
                    x<16 ? 0xff4080c0u : 0xffc04080u;
            struct render3d_texture_desc desc={.format=R3D_ARGB8888,.width=w,.height=h,.levels=1,
                .image={{pixels,w*h*4,w*4}}};
            e=render3d_create_texture(s,1,context,&desc,&resource);
            host_free(0,pixels);
        } else e=render3d_create_resource(s,1,context,kinds[i],width,height,0,0,&resource);
        bool laid_out=hardware && (full || i<4);
        if (!e && laid_out) {
            struct render3d_resource *r=&s->contexts[0].resources[i];
            r->hardware=true; r->offset=kinds[i]==R3D_TEXTURE ? tex_at : at;
            e=mach64_3d_backend()->layout(&device,r);
            if (kinds[i]==R3D_TEXTURE) tex_at+=r->reserved; else at+=r->reserved;
        }
    }
    if (e) { (void)render3d_close(s); host_free(0,s); return e; }
    *out=s; return 0;
}
static void fullsize_diagnostic(struct render3d_service **scratch)
{
    /* Q1 completed and restored. Allocation and scalar rendering never run
     * inside presenter/engine ownership. */
    (void)render3d_close(*scratch); host_free(0,*scratch); *scratch=0;
    struct render3d_service *reference=0;
    device.fullsize_testing=true;
    int e=scratch_create(scratch,device.mode_width,device.mode_height,true,true);
    device.fullsize_testing=false;
    if (!e) e=scratch_create(&reference,device.mode_width,device.mode_height,true,false);
    if (!e) {
        device.consumer=*scratch;
        e=mach64_3d_fullsize_probe(&device,reference);
        device.consumer=0;
    }
    if (reference) { (void)render3d_close(reference); host_free(0,reference); }
    klog("[3d] fullsize result=%s error=%d width=%u height=%u samples=%u reference=%08x hardware=%08x channel_error=%u depth_error=%u elapsed_us=%llu phase=%u guard=%u quarantined=%u",
        !e ? "qualified" : "refused",e,device.mode_width,device.mode_height,device.sample_count,
        device.reference_checksum,device.hardware_checksum,device.max_channel_error,device.max_depth_error,
        device.fullsize_us,device.failed_phase,device.guard_offset,device.lost);
    klog("[3d] precision result=%s error=%d alpha=%d blend=%d delta_q16=%u filter_bits=%u phase=%u samples=%u levels0=%u levels1=%u expected=%08x actual=%08x",
        device.perspective_qualified ? "qualified" : "refused",device.precision_error,device.alpha_error,device.blend_error,
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
/* Read-only identity under presenter + engine ownership: the 2D-validated
 * CONFIG_CHIP_ID must also appear at the X.Org/Linux auxiliary-aperture
 * location (BAR2+0x400 block 0, atyfb_base.c:3458-3459), proving the 4 KiB
 * page decodes both layouts, and block 1 must already be enabled. */
static int register_preflight(struct m64_engine *engine,uint32_t generation,const struct pci_func *pci,
                              uint32_t *chip,uint32_t *chip_low,uint32_t *bus,uint32_t *fifo)
{
    int e=fbdev_render3d_enter(generation);
    if (e) return e;
    e=m64_render3d_read(engine,M64_CONFIG_CHIP_ID,chip);
    if (!e) e=m64_render3d_read(engine,M64_CONFIG_CHIP_ID_LOW,chip_low);
    if (!e) e=m64_render3d_read(engine,M64_BUS_CNTL,bus);
    if (!e) e=m64_render3d_read(engine,M64_FIFO_STAT,fifo);
    fbdev_render3d_leave();
    if (e) return -EIO;
    if ((*chip&0xffffu)!=pci->device || *chip_low!=*chip) return -ENODEV;
    if (!(*bus&M64_BUS_EXT_REG_EN)) return -EOPNOTSUPP;
    if (*fifo&0x80000000u) return -EIO;
    return 0;
}
void mach64_3d_boot_diagnostic(struct m64_engine *engine,const struct fb_device *fb)
{
    if (reported || !token()) return;
    reported=true;
    const struct pci_func *pci=display_driver_owner();
    klog("[3d] probe phase=identity driver=mach64 pci=%04x:%04x revision=%u bar0=%08x bar2=%08x command=%04x active=%u quarantine=%u vram=%u mem_cntl=%08x firmware_vram=%u",
        pci ? pci->vendor : 0,pci ? pci->device : 0,pci ? pci->revision : 0,
        pci ? pci->bar[0] : 0,pci ? pci->bar[2] : 0,pci ? pci->command : 0,
        engine->active,engine->quarantined,engine->vram_bytes,engine->mem_cntl,g_boot.display.vram_bytes);
    uint32_t minimum=0,full=0;
    struct render3d_extent heap={0};
    const char *reason="mode_heap";
    int e=!engine->active || engine->quarantined || !engine->cpu_safe ? -ENODEV :
        mach64_3d_mode_heap(engine,fb,&heap,&minimum,&full);
    uint32_t free_bytes=0,largest_bytes=0;
    if (!e) {
        available_heap(fb,&heap,&free_bytes,&largest_bytes);
        if (largest_bytes<minimum) e=-ENOMEM;
    }
    bool fits_full=!e && largest_bytes>=full;
    if (!e && (!pci || pci->vendor!=0x1002 || !fb || fb->generation==0 ||
        fb->phys>UINT32_MAX-heap.first || !safe_extent(fb->phys+heap.first,heap.bytes))) {
        e=-ENODEV; reason="identity";
    }
    klog("[3d] probe phase=preflight vram=%u scanout=%u bpp=%u mode=%ux%u cursor=%u free_vram=%u largest_vram=%u minimum=%u full=%u fullsize=%u host_free_pages=%u",
        engine->vram_bytes,fb ? fb->size : 0,fb ? fb->bpp : 0,fb ? fb->width : 0,fb ? fb->height : 0,
        engine->cursor ? engine->cursor_offset : 0,free_bytes,largest_bytes,minimum,full,fits_full,pmm_free_count());
    bool pinned=false;
    if (!e) { e=fbdev_render3d_prepare(fb->generation); pinned=!e; reason="presenter_pin"; }
    uint32_t chip=0,chip_low=0,bus=0,fifo=0;
    if (!e) {
        e=register_preflight(engine,fb->generation,pci,&chip,&chip_low,&bus,&fifo);
        reason=e==-EOPNOTSUPP ? "block1_disabled" : e==-ENODEV ? "aperture_layout" : "register_preflight";
        klog("[3d] probe phase=registers chip=%08x chip_aux=%08x bus_cntl=%08x fifo=%08x error=%d",chip,chip_low,bus,fifo,e);
    }
    if (!e && !marker_vfs) { e=-ENODEV; reason="marker_store"; }
    if (!e) {
        uint32_t nontexture=fits_full ? full-MACH64_3D_FULL_TEXTURES : minimum-MACH64_3D_TEXTURE_MIN;
        /* Field stores, not a compound literal: the device holds the batch
         * staging array and must never be built on the kernel stack. */
        memset(&device,0,sizeof(device));
        device.engine=engine; device.generation=fb->generation; device.heap=heap;
        device.mode_width=fb->width; device.mode_height=fb->height;
        device.texture_bytes=(heap.bytes-nontexture)&~4095u;
        device.memory=(struct mach64_3d_memory){0,memory_read,memory_write,enter,leave};
        device.lost_notify=lost_notify;
        uint32_t phys=fb->phys+heap.first;
        claim=registry_claim(RES_MMIO,phys,phys+heap.bytes,"mach64-3d",false);
        if (claim<0) { e=claim; reason="claim"; claim=-1; }
        else {
            claim_generation=registry_get(claim)->generation;
            e=registry_activate(claim,claim_generation); reason="claim_activate";
        }
        if (!e) {
            if (!heap_window) { heap_capacity=heap.bytes; heap_window=vmm_reserve_mmio(heap_capacity); }
            heap_map=heap_window && heap_capacity>=heap.bytes ? vmm_replace_mmio(heap_window,heap_capacity,phys,heap.bytes) : 0;
            if (!heap_map) e=-ENOMEM;
            reason="mapping";
        }
    }
    struct render3d_service *scratch=0;
    if (!e) { e=scratch_create(&scratch,64,64,false,true); reason="scratch"; }
    if (!e) {
        bool previous=false;
        e=display_attempt_marker(marker_vfs,"3d-mach64",pci->vendor,pci->device,0,false,&previous,0);
        if (previous) e=-EACCES;
        marker_fresh=!e; reason=previous ? "retained_marker" : "marker_begin";
    }
    if (!e) {
        klog("[3d] probe phase=armed heap_first=%u heap_bytes=%u texture_pool=%u marker=durable",
            heap.first,heap.bytes,device.texture_bytes);
        device.consumer=scratch;
        e=mach64_3d_probe(&device); reason="minimum_probe";
        device.consumer=0;
        if (!e && !fits_full) { e=-ENOMEM; reason="fullsize_heap"; }
        else if (!e) {
            fullsize_diagnostic(&scratch);
            if (device.lost) e=-EIO;
            else if (!device.qualified) { e=-EILSEQ; reason="fullsize_probe"; }
        }
    }
    if (marker_fresh && !device.lost) {
        int clear=display_attempt_marker(marker_vfs,"3d-mach64",pci->vendor,pci->device,0,true,0,0);
        if (clear) { e=clear; reason="marker_clear"; device.qualified=false; }
        else marker_fresh=false;
    }
    if (scratch) {
        if (device.lost) {
            /* Retain shadows and metadata: a possibly running writer is not
             * a reason to recycle any referenced object. */
            device.consumer=scratch; render3d_device_lost(scratch);
            scratch->contexts[0].hardware=true;
        } else { (void)render3d_close(scratch); host_free(0,scratch); }
    }
    engine->render3d_qualified=device.qualified && !e;
    if (!engine->render3d_qualified) device.qualified=false;
    if (e && !device.lost && device.engine && claim>=0) {
        int released=release_claim();
        if (released) { device.lost=true; m64_quarantine(engine,"render3d_release"); }
    }
    if (pinned) fbdev_render3d_prepared();
    klog("[3d] probe result=%s reason=%s error=%d hardware=%u phase=%u register=%03x expected=%08x actual=%08x guard=%u coverage=%u packets=%llu triangles=%llu clears=%llu copies=%llu alpha=%u blend=%u perspective=%u selfchecks=%u selfcheck_error=%d marker=%s",
        !e ? "qualified" : device.lost ? "failed" : "blocked",!e ? "q1_q2_q3_restore" : reason,e,
        engine->render3d_qualified,device.failed_phase,device.failed_register,device.expected,device.actual,
        device.guard_offset,device.coverage_mismatches,device.packets,device.triangles,device.clears,device.copies,
        device.alpha_qualified,device.blend_qualified,device.perspective_qualified,device.selfchecks,device.selfcheck_error,
        marker_fresh ? "retained" : "clear");
}
