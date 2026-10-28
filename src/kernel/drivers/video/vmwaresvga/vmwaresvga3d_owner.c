/* Shared SVGA II owner glue for the SVGA3D backend: VRAM staging claim,
 * durable marker, per-boot Q1-Q4 and attachment. No independent PCI claim.
 * See docs/design/f7-family-vmware-svga3d.md. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/mm.h>
#include <ciuki/desktop.h>
#include <ciuki/display_driver.h>
#include "vmwaresvga3d.h"
#define MARKER_KEY "3d-vmwaresvga"
static struct vmsvga3d_device device;
static struct vfs *marker_vfs;
static volatile uint8_t *linear;
static void *linear_window;
static uint32_t linear_capacity;
static int claim=-1;
static gen_t claim_generation;
static bool reported, marker_fresh;
void vmsvga3d_bind_vfs(struct vfs *vfs) { marker_vfs=vfs; }
static volatile uint8_t *at(uint32_t offset) { return linear+offset-device.heap.first; }
static uint32_t memory_read(void *ctx,uint32_t offset,unsigned bytes)
{
    (void)ctx;
    if (bytes==2) return *(volatile uint16_t *)at(offset);
    return *(volatile uint32_t *)at(offset);
}
static void memory_write(void *ctx,uint32_t offset,uint32_t value,unsigned bytes)
{
    (void)ctx;
    if (bytes==2) *(volatile uint16_t *)at(offset)=(uint16_t)value;
    else *(volatile uint32_t *)at(offset)=value;
}
static int enter(void *ctx,uint32_t generation) { (void)ctx; return fbdev_render3d_enter(generation); }
static void leave(void *ctx) { (void)ctx; fbdev_render3d_leave(); }
static bool safe_extent(uint32_t first,uint32_t bytes)
{
    if (first<0x100000 || !bytes || bytes>UINT32_MAX-first) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *m=&g_boot.e820[i];
        if (m->length>UINT64_MAX-m->base) return false;
        if (m->type==CBI_E820_RAM && first<m->base+m->length && (uint64_t)first+bytes>m->base) return false;
    }
    return !(first<g_boot.kernel_end && (uint64_t)first+bytes>g_boot.kernel_start);
}
/* Largest page run of the planned extent free of every live MMIO claim. */
static void available_heap(const struct fb_device *fb,struct render3d_extent *heap,
                           uint32_t *free_bytes,uint32_t *largest_bytes)
{
    struct registry_inventory_copy inventory;
    registry_inventory(&inventory);
    uint32_t run=0,largest_first=heap->first;
    *free_bytes=*largest_bytes=0;
    uint32_t end=heap->first+heap->bytes;
    for (uint32_t page=heap->first;page<end;page+=4096) {
        uint64_t physical=(uint64_t)fb->phys+page;
        bool used=inventory.truncated;
        for (unsigned i=0;i<inventory.resource_count;i++) {
            const struct resource *r=&inventory.resources[i];
            if (r->type==RES_MMIO && r->state!=RS_RELEASED && physical<r->end && physical+4096>r->start) used=true;
        }
        if (used) run=0;
        else {
            *free_bytes+=4096; run+=4096;
            if (run>*largest_bytes) { *largest_bytes=run; largest_first=page+4096-run; }
        }
    }
    *heap=(struct render3d_extent){largest_first,*largest_bytes};
}
static bool release_proof(int handle,gen_t generation)
{
    if (device.engine && !device.engine->cpu_safe) return false;
    if (handle==claim && generation==claim_generation && linear) {
        vmm_revoke_mmio((void *)linear,device.heap.bytes); linear=0;
    }
    return true;
}
static int release_claim(void)
{
    if (claim<0) return 0;
    const struct resource *r=registry_get(claim);
    /* The registry has no release for an unactivated claim; keep it. */
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
/* Called by the 2D runtime stop under the presenter, before its FIFO and
 * register mappings can be revoked: close the consumer, destroy the host
 * context, then release the staging claim. */
int vmsvga3d_owner_stop(struct vmsvga_engine *engine)
{
    if (device.engine!=engine) return 0;
    if (claim<0 && !device.consumer && !device.context_live) return 0;
    device.qualified=device.fullsize_qualified=device.perspective_qualified=false;
    if (device.lost || engine->quarantined || !engine->cpu_safe) {
        /* Mark the consumer lost before close can collect its shadows. */
        if (device.consumer) {
            render3d_device_lost(device.consumer);
            (void)render3d_close(device.consumer);
        }
        else vmsvga3d_backend()->quarantine(&device);
        return -EIO;
    }
    if (device.consumer) {
        int e=render3d_close(device.consumer);
        if (e) return e;
        device.consumer=0;
    }
    int e=vmsvga3d_context(&device,false);
    if (!e) e=release_claim();
    if (!e) device.lost=true; /* this generation is never reattached */
    return e;
}
int vmsvga3d_owner_attach(struct render3d_service *s,const struct render3d_host *host,uint32_t generation)
{
    if (!vmsvga3d_backend()->qualified(&device,generation) || marker_fresh || device.lost) return -ENODEV;
    if (device.consumer) return -EBUSY;
    render3d_init(s,host,generation,vmsvga3d_backend(),&device,
        (struct render3d_extent){device.heap.first+VMSVGA3D_SCRATCH_BYTES,device.heap.bytes-VMSVGA3D_SCRATCH_BYTES});
    device.consumer=s; return 0;
}
/* Page-backed qualification storage, following the integrated Savage owner.
 * Include prefixes, page rounding and backend staging in the shared ceiling. */
static uint32_t probe_resident_bytes;
static void *host_alloc(void *ctx,size_t bytes)
{
    (void)ctx;
    if (!bytes || bytes>R3D_RAM_LIMIT-8) return 0;
    uint32_t extent=(uint32_t)bytes+8;
    uint32_t charge=(extent+PAGE_SIZE-1)&~(PAGE_SIZE-1);
    uint32_t backend=vmsvga3d_backend()->resident_bytes;
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
static uint64_t host_now(void *ctx) { (void)ctx; return device.engine->io.now(device.engine->io.ctx); }
/* Scratch services for the boot scenes: software services whose resources
 * are then placed, hardware-flagged, inside the claimed plan. */
static int scratch_create(struct render3d_service **out,unsigned width,unsigned height,bool full,bool hardware)
{
    if (full && hardware && device.texture_bytes<VMSVGA3D_TEXTURE_FULLSIZE) return -ENOMEM;
    struct render3d_service *s=host_alloc(0,sizeof(*s)); if (!s) return -ENOMEM;
    const struct render3d_host host={0,host_alloc,host_free,host_now,0};
    render3d_init(s,&host,device.generation,0,0,(struct render3d_extent){0});
    uint32_t context;
    int e=render3d_create_context(s,1,R3D_SOFTWARE,&context);
    const enum render3d_kind kinds[]={R3D_COLOR,R3D_D16,R3D_TEXTURE,R3D_LINEAR_COLOR,R3D_TEXTURE};
    uint32_t place=device.heap.first+VMSVGA3D_SCRATCH_BYTES;
    uint32_t texture_place=device.heap.first+device.heap.bytes-device.texture_bytes;
    for (unsigned i=0;!e && i<(full ? 5u : 4u);i++) {
        uint32_t resource;
        uint32_t *pixels=(void *)s->ring[2];
        if (full && kinds[i]==R3D_TEXTURE) {
            unsigned base=i==2 ? 256 : 128,levels=i==2 ? 9 : 8;
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
            r->hardware=true; r->offset=kinds[i]==R3D_TEXTURE ? texture_place : place;
            e=vmsvga3d_backend()->layout(&device,r);
            if (kinds[i]==R3D_TEXTURE) texture_place+=r->reserved; else place+=r->reserved;
        }
    }
    if (e) { (void)render3d_close(s); host_free(0,s); return e; }
    *out=s; return 0;
}
static void fullsize_diagnostic(struct render3d_service **scratch)
{
    /* Q1 is complete. Scratch allocation and scalar reference rendering
     * never occur inside presenter/engine ownership. */
    (void)render3d_close(*scratch); host_free(0,*scratch); *scratch=0;
    struct render3d_service *reference=0;
    device.fullsize_testing=true;
    int e=scratch_create(scratch,device.mode_width,device.mode_height,true,true);
    device.fullsize_testing=false;
    if (!e) e=scratch_create(&reference,device.mode_width,device.mode_height,true,false);
    if (!e) {
        device.consumer=*scratch;
        e=vmsvga3d_fullsize_probe(&device,reference);
        device.consumer=0;
    }
    if (reference) { (void)render3d_close(reference); host_free(0,reference); }
    if (e && !device.precision_error) device.precision_error=e;
    klog("[3d] fullsize driver=vmwaresvga result=%s error=%d width=%u height=%u samples=%u reference=%08x hardware=%08x channel_error=%u depth_error=%u elapsed_us=%llu phase=%u guard=%u quarantined=%u",
        device.fullsize_qualified ? "qualified" : "refused",e,device.mode_width,device.mode_height,device.sample_count,
        device.reference_checksum,device.hardware_checksum,device.max_channel_error,device.max_depth_error,
        device.fullsize_us,device.failed_phase,device.guard_offset,device.lost);
    klog("[3d] precision driver=vmwaresvga result=%s error=%d delta_q16=%u filter_bits=%u lod_delta_q8=16 phase=%u samples=%u levels0=%u levels1=%u expected=%08x actual=%08x",
        device.perspective_qualified ? "qualified" : "refused",device.precision_error,
        device.sample_delta_q16,device.filter_fraction_bits,device.precision_phase,device.precision_samples,
        device.measured_filter_levels[0],device.measured_filter_levels[1],device.expected,device.actual);
    klog("[3d] combiners driver=vmwaresvga texture_ops=%08x replace0=%u replace1=%u add=%u decal=%u",
        device.caps.texture_ops,!!(device.combine&VMSVGA3D_COMBINE_REPLACE0),!!(device.combine&VMSVGA3D_COMBINE_REPLACE1),
        !!(device.combine&VMSVGA3D_COMBINE_ADD),!!(device.combine&VMSVGA3D_COMBINE_DECAL));
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
static void destroy_context(void)
{
    if (!device.context_live || device.lost || !device.engine || device.engine->quarantined) return;
    if (!enter(0,device.generation)) { (void)vmsvga3d_context(&device,false); leave(0); }
}
/* Restore before clearing the durable marker. D-F2's 2D check is the
 * owner's existing saved/restored fill/copy comparison, with presenter
 * ownership. A lost FIFO cannot safely run restoration or a self-test. */
static int restore_owner(bool release)
{
    if (device.lost) return -EIO;
    if (release) destroy_context();
    int e=device.lost || (release && device.context_live) ? -EIO : 0;
    if (!e && device.batches) {
        e=enter(0,device.generation);
        if (!e) {
            e=device.engine->self_test ? device.engine->self_test(device.engine) : -EOPNOTSUPP;
            leave(0);
        }
    }
    if (!e && release) e=release_claim();
    if (e) vmsvga3d_backend()->quarantine(&device);
    return e;
}
/* Boot qualification on the current hook (record), gated by "3d=probe"
 * like the Savage and SiS owners until the f7-13 background job lands. */
void vmsvga3d_boot_diagnostic(struct vmsvga_engine *engine,const struct fb_device *fb)
{
    if (reported || !token() || !engine) return;
    reported=true;
    if (claim>=0 || device.consumer || device.lost) return;
    const struct pci_func *pci=display_driver_owner();
    struct vmsvga3d_caps caps={0}; const char *reason="identity";
    const char *marker_state="unarmed";
    int e=0;
    /* QEMU's vmware-svga reports no SVGA_CAP_3D: refuse with no port I/O. */
    if (!(engine->caps&SVGA_CAP_3D)) { e=-EOPNOTSUPP; reason="no_svga3d"; }
    bool pinned=false;
    if (!e && !fb) { e=-ENODEV; reason="no_framebuffer"; }
    if (!e) { e=fbdev_render3d_prepare(fb->generation); pinned=!e; reason="presenter_pin"; }
    if (!e) {
        /* SVGA_REG_DEV_CAP uses the shared index port: hold the engine. */
        e=enter(0,fb->generation); reason="presenter_enter";
        if (!e) { e=vmsvga3d_read_caps(engine,&caps,&reason); leave(0); }
    }
    klog("[3d] probe phase=identity driver=vmwaresvga pci=%04x:%04x revision=%u caps=%08x fifo_caps=%08x gb_objects=%u hwversion=%08x max_textures=%u max_texture=%ux%u texture_ops=%08x argb=%08x d16=%08x active=%u quarantine=%u reason=%s",
        pci ? pci->vendor : 0,pci ? pci->device : 0,pci ? pci->revision : 0,engine->caps,engine->fifo_caps,
        caps.gb_objects,caps.hwversion,caps.max_textures,caps.max_texture_width,caps.max_texture_height,caps.texture_ops,
        caps.argb_ops,caps.d16_ops,engine->active,engine->quarantined,reason);
    /* Both the firmware-reported and the device-reported VRAM must hold it. */
    uint32_t vram=g_boot.display.vram_bytes;
    if (engine->vram_size<vram) vram=engine->vram_size;
    uint32_t required=0,free_bytes=0,largest_bytes=0;
    struct render3d_extent heap={0};
    if (!e) { e=vmsvga3d_mode_heap(engine,fb,vram,&heap,&required); reason="mode_heap"; }
    if (!e) {
        available_heap(fb,&heap,&free_bytes,&largest_bytes);
        if (largest_bytes<required) e=-ENOMEM;
    }
    if (!e && (!pci || pci->vendor!=0x15ad || pci->device!=0x0405 || !engine->active || engine->quarantined ||
        !engine->cpu_safe || fb->phys!=(pci->bar[1]&~15u) || fb->phys>UINT32_MAX-heap.first ||
        !safe_extent(fb->phys+heap.first,heap.bytes))) { e=-ENODEV; reason="identity_heap"; }
    klog("[3d] probe phase=preflight driver=vmwaresvga vram=%u scanout=%u free_vram=%u largest_vram=%u required=%u host_free_pages=%u host_reserved_pages=%u",
        vram,fb ? fb->size : 0,free_bytes,largest_bytes,required,pmm_free_count(),pmm_total_usable()-pmm_free_count());
    if (!e && !marker_vfs) { e=-ENODEV; reason="marker_store"; }
    if (!e) {
        device=(struct vmsvga3d_device){.engine=engine,.generation=fb->generation,.heap=heap,.caps=caps,
            .mode_width=fb->width,.mode_height=fb->height,
            .texture_bytes=(heap.bytes-(required-VMSVGA3D_TEXTURE_MIN))&~4095u,
            .memory={0,memory_read,memory_write,enter,leave},.lost_notify=lost_notify};
        uint32_t physical=fb->phys+heap.first;
        claim=registry_claim(RES_MMIO,physical,physical+heap.bytes,"vmwaresvga-3d",false);
        if (claim<0) { e=claim; reason="claim"; }
        if (!e) {
            const struct resource *r=registry_get(claim);
            if (!r) e=-EIO;
            else { claim_generation=r->generation; e=registry_activate(claim,claim_generation); }
            reason="claim_activate";
        }
        if (!e) {
            if (!linear_window) { linear_capacity=heap.bytes; linear_window=vmm_reserve_mmio(linear_capacity); }
            linear=linear_window && heap.bytes<=linear_capacity ?
                vmm_replace_mmio(linear_window,linear_capacity,physical,heap.bytes) : 0;
            if (!linear) e=-ENOMEM;
            reason="mapping";
        }
    }
    struct render3d_service *scratch=0;
    if (!e) { e=scratch_create(&scratch,64,64,false,true); reason="scratch"; }
    if (!e) {
        bool previous=false;
        e=display_attempt_marker(marker_vfs,MARKER_KEY,pci->vendor,pci->device,0,false,&previous,0);
        if (previous) e=-EACCES;
        marker_state=previous || !e ? "retained" : "unknown";
        marker_fresh=!e; reason=previous ? "retained_marker" : "marker_begin";
    }
    if (!e) {
        klog("[3d] probe phase=armed driver=vmwaresvga heap_first=%u heap_bytes=%u texture_bytes=%u marker=durable",
            heap.first,heap.bytes,device.texture_bytes);
        device.qualification_started=host_now(0);
        device.consumer=scratch;
        e=vmsvga3d_probe(&device); reason="minimum_probe";
        device.consumer=0;
        if (!e) {
            fullsize_diagnostic(&scratch);
            if (!device.fullsize_qualified) { e=device.precision_error ? device.precision_error : -EIO; reason="fullsize_probe"; }
        }
    }
    bool ready=!e && device.minimum_passed && device.fullsize_qualified;
    if (!device.lost) {
        int restored=restore_owner(!ready);
        if (restored) { e=restored; reason="restore_2d_check"; }
    }
    if (marker_fresh && !device.lost) {
        int clear=display_attempt_marker(marker_vfs,MARKER_KEY,pci->vendor,pci->device,0,true,0,0);
        if (clear) {
            e=clear; reason="marker_clear";
            /* Successful qualification cannot publish if persistence fails.
             * Retain the marker and tear down the healthy host context. */
            if (ready) (void)restore_owner(true);
        }
        else { marker_fresh=false; marker_state="clear"; }
    }
    if (scratch) {
        if (device.lost) {
            /* Retain shadows/metadata: a possibly running host writer is
             * never an excuse to recycle a referenced object. */
            device.consumer=scratch; render3d_device_lost(scratch);
            scratch->contexts[0].hardware=true;
        } else { (void)render3d_close(scratch); host_free(0,scratch); }
    }
    /* Q4: admission for this boot, generation and mode only. */
    device.qualified=!e && !device.lost && device.minimum_passed && device.fullsize_qualified;
    if (pinned) fbdev_render3d_prepared();
    klog("[3d] probe driver=vmwaresvga result=%s reason=%s error=%d hardware=%u phase=%u expected=%08x actual=%08x guard=%u batches=%llu triangles=%llu copies=%llu surfaces=%llu/%llu combine=%x marker=%s",
        device.qualified ? "qualified" : device.lost ? "failed" : "blocked",device.qualified ? "q1_q2_q3_q4" : reason,e,
        device.qualified,device.failed_phase,device.expected,device.actual,device.guard_offset,
        device.batches,device.triangles,device.copies,device.defined,device.destroyed,device.combine,
        marker_state);
}
