/* Ciuki Intel display adapter and physical ownership boundary.
 * SPDX-License-Identifier: GPL-2.0-only
 * Register references: Haiku r1beta5 intel_extreme private header and
 * Linux v6.7 intel-gtt.c / gt/intel_engine_regs.h. No upstream C inclusion.
 */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/timing.h>
#include "../../../fs/vfs.h"
#include "intel2d.h"

#include "../render3d.h"
static const struct render3d_family render_family = {
    .status=R3D_NOT_IMPLEMENTED, .potential_units=4, .potential_texture_size=2048
};

#define MMIO_BYTES 0x80000u
#define GTT_BYTES 0x40000u
static struct intel_engine engine={.cpu_safe=true};
static const char *reason="pending";
static const struct pci_func *gpu, *bridge;
static const struct pci_func *memctrl;
static const struct intel_chip *chip;
static struct fb_device display;
static struct mm_pin dma_pin;
static uint32_t dma_phys, aperture, stolen_start, stolen_end;
static uint32_t original_pgtbl, plane_control, original_bsm, original_ggc, original_msac, original_ifp;
static uint32_t original_drb, original_tseg;
static unsigned gtt_bytes;
static unsigned cache_line;
static bool marker_written, blocked;
struct lease { int handle; gen_t generation; void *mapping; uint32_t bytes; };
static struct lease leases[4]={{.handle=-1},{.handle=-1},{.handle=-1},{.handle=-1}};

static uint32_t config(const struct pci_func *p, unsigned offset)
{
    uint32_t flags=irq_save();
    outl(0xcf8,0x80000000u | (uint32_t)p->bus<<16 | (uint32_t)p->dev<<11 |
        (uint32_t)p->fn<<8 | (offset&252u));
    uint32_t value=inl(0xcfc);
    irq_restore(flags); return value;
}
static bool primary(const struct pci_func *p)
{
    if (!p || p->vendor!=0x8086 || p->bus || p->dev!=2 || p->fn ||
        p->class_code!=3 || p->subclass || (p->header_type&127) || (p->command&6u)!=6u)
        return false;
    return p->device==0x3577 || p->device==0x2562 || p->device==0x3582 ||
        p->device==0x2572 || p->device==0x2582;
}
static bool probe(const struct pci_func *p) { return primary(p); }
static bool nonram(uint32_t start, uint32_t bytes)
{
    uint64_t end=(uint64_t)start+bytes;
    if (start<0x100000u || end>UINT32_MAX || !bytes) return false;
    if (start<g_boot.kernel_end && end>g_boot.kernel_start) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *e=&g_boot.e820[i];
        if (e->base>UINT64_MAX-e->length) return false;
        if (e->type==CBI_E820_RAM && start<e->base+e->length && end>e->base) return false;
    }
    return true;
}
static bool eligible(const struct fb_device *d)
{
    if (engine.quarantined || blocked) { reason="quarantined"; return false; }
    if (display_accel_disabled(&reason)) return false;
    if (!intel_mode_valid(d)) { reason="unsupported_mode"; return false; }
    gpu=0; bridge=0; memctrl=0;
    unsigned displays=0;
    for (unsigned i=0;i<pci_count();i++) {
        const struct pci_func *p=pci_get(i);
        if (!p) continue;
        if (p->class_code==3) { displays++; gpu=p; }
        if (!p->bus && !p->dev && !p->fn && p->vendor==0x8086) bridge=p;
        if (!p->bus && !p->dev && p->fn==1 && p->vendor==0x8086 && p->device==0x3584)
            memctrl=p;
    }
    if (displays!=1 || !primary(gpu)) { reason="ambiguous_owner"; return false; }
    chip=intel_chip_lookup(gpu->device);
    if (!chip || chip->refusal) { reason=chip ? chip->refusal : "unsupported_chip"; return false; }
    if (!bridge || bridge->device!=chip->bridge || (gpu->device==0x3582 && !memctrl)) {
        reason="host_bridge_mismatch"; return false;
    }
    bool bars=chip->gen2 ? (gpu->bar[0]&0x3ffffffu)==8u && gpu->bar[1] &&
        !(gpu->bar[1]&0x7ffffu) : gpu->bar[0] && !(gpu->bar[0]&0x7ffffu) &&
        (gpu->bar[2]&0x7ffffffu)==8u && gpu->bar[3] && !(gpu->bar[3]&0x3ffffu);
    if (!bars || (gpu->bar[chip->gen2 ? 0 : 2]&~15u)!=d->phys || !nonram(d->phys,d->size) ||
        !registry_valid(d->handle,d->generation)) { reason="framebuffer_owner"; return false; }
    const struct resource *r=registry_get((unsigned)d->handle);
    if (!r || r->state!=RS_ACTIVE || r->type!=RES_MMIO || r->start!=d->phys ||
        r->end!=(uint64_t)d->phys+d->size || !r->owner || strncmp(r->owner,"fbdev",sizeof("fbdev"))) {
        reason="framebuffer_lease"; return false;
    }
    uint32_t a,b,c,flags;
    cpuid(1,&a,&b,&c,&flags);
    cache_line=((b>>8)&255u)*8u;
    if (!(flags&(1u<<19)) || cache_line<16 || cache_line>256 || (cache_line&(cache_line-1))) {
        reason="cache_flush_unavailable"; return false;
    }
    reason="pending"; return true;
}
static uint32_t read_reg(void *ctx, uint32_t reg)
{
    (void)ctx;
    if (chip->gen2 && reg>=0x70000u)
        return *(volatile uint32_t *)((uint8_t *)leases[2].mapping+reg-0x70000u);
    return *(volatile uint32_t *)((uint8_t *)leases[0].mapping+reg);
}
static void write_reg(void *ctx, uint32_t reg, uint32_t value)
{ (void)ctx; *(volatile uint32_t *)((uint8_t *)leases[0].mapping+reg)=value; }
static uint32_t read_pte(void *ctx, unsigned i)
{ (void)ctx; return ((volatile uint32_t *)leases[1].mapping)[i]; }
static void write_pte(void *ctx, unsigned i, uint32_t pte)
{ (void)ctx; ((volatile uint32_t *)leases[1].mapping)[i]=pte; }
/* CLFLUSH runs only after the probe's CPUID.1:EDX[19] check above
 * (cache_flush_unavailable otherwise; the Pentium III has no CLFLUSH). One
 * noinline function under one name, so the kernel FPU/SIMD audit admits
 * exactly this instruction here (scripts/build_kernel.py). */
__attribute__((noinline)) static void intel2d_clflush_range(const void *p, unsigned bytes)
{
    uintptr_t end=(uintptr_t)p+bytes;
    for (uintptr_t at=(uintptr_t)p&~(uintptr_t)(cache_line-1);at<end;at+=cache_line)
        __asm__ volatile("clflush (%0)" :: "r"(at) : "memory");
}
static void clean(const void *p, unsigned bytes)
{
    intel2d_clflush_range(p,bytes);
    cpu_write_drain();
}
static uint64_t now(void *ctx);
static void relax(void *ctx);
static bool live(void *ctx);
static int publish(void *ctx, const void *p, unsigned bytes, struct intel_wait *wait)
{
    (void)ctx;
    if (bytes) clean(p,bytes);
    framebuffer_write_drain();
    if (chip->gen2) {
        /* CLFLUSH alone is insufficient on 8xx (Linux intel-gtt.c).
         * This kernel has one executing CPU; do not enable an AP here. */
        wbinvd();
        const struct intel_io io={.read=read_reg,.write=write_reg,.now=now,.relax=relax,.valid=live};
        return intel_hic_flush(&io,wait,engine.pause,engine.pause_arg);
    }
    *(volatile uint32_t *)leases[2].mapping=1; /* pre-enabled IFPADDR only */
    cpu_write_drain();
    return 0;
}
static uint32_t status(void *ctx, volatile uint32_t *p)
{ (void)ctx; clean((const void *)p,4); return *p; }
static uint64_t now(void *ctx)
{
    (void)ctx;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000;
    uint64_t cycles=ktime_cycles();
    return cycles/g_tsc_per_ms*1000 + cycles%g_tsc_per_ms*1000/g_tsc_per_ms;
}
static void relax(void *ctx) { (void)ctx; __asm__ volatile("pause" ::: "memory"); }
static bool live(void *ctx)
{
    (void)ctx;
    const struct fb_device *d=fbdev_get();
    if (!d || !d->present || d->phys!=display.phys || d->pitch!=display.pitch ||
        d->width!=display.width || d->height!=display.height || d->bpp!=display.bpp ||
        d->generation!=display.generation || d->handle!=display.handle ||
        !registry_valid(d->handle,d->generation)) return false;
    for (unsigned i=0;i<4;i++) if (!registry_valid(leases[i].handle,leases[i].generation)) return false;
    if (config(gpu,0)!=((uint32_t)chip->device<<16|0x8086u) ||
        config(bridge,0)!=((uint32_t)chip->bridge<<16|0x8086u) ||
        (config(gpu,4)&6u)!=6u || config(gpu,0x10)!=gpu->bar[0] ||
        config(bridge,0x50)!=original_ggc) return false;
    if (chip->gen2) {
        if (config(gpu,0x14)!=gpu->bar[1] ||
            config(bridge,gpu->device==0x2562 ? 0x9c : 0x60)!=original_tseg ||
            config(gpu->device==0x2562 ? bridge : memctrl,
                   gpu->device==0x2562 ? 0x60 : 0x40)!=original_drb ||
            (memctrl && config(memctrl,0)!=0x35848086u)) return false;
    } else if (config(gpu,0x18)!=gpu->bar[2] || config(gpu,0x1c)!=gpu->bar[3] ||
        config(gpu,0x5c)!=original_bsm || config(gpu,0x60)!=original_msac ||
        config(bridge,0x60)!=original_ifp) return false;
    if (read_reg(0,INTEL_PGTBL)!=original_pgtbl ||
        (read_reg(0,0x2098)&65535u)!=65535u || /* no asynchronous ISR HWS writes */
        read_reg(0,0x70180)!=plane_control || read_reg(0,0x70184) ||
        read_reg(0,0x70188)!=display.pitch || (read_reg(0,0x71180)&(1u<<31)) ||
        (read_reg(0,0x204c)&1u) || (!chip->gen2 && (read_reg(0,0x205c)&1u)) ||
        (read_reg(0,0x210c)&1u) || (read_reg(0,0x211c)&1u)) return false;
    for (unsigned i=0;i<8;i++) if (read_reg(0,0x2000+i*4)&1u) return false;
    return true;
}
static bool release_proof(int h, gen_t generation)
{
    if (engine.mapped || engine.touched || !engine.cpu_safe) return false;
    for (unsigned i=0;i<4;i++) if (leases[i].handle==h && leases[i].generation==generation) {
        if (leases[i].mapping) vmm_revoke_mmio(leases[i].mapping,leases[i].bytes);
        leases[i].mapping=0; return true;
    }
    return false;
}
static int release_all(void)
{
    if (engine.mapped || engine.touched || !engine.cpu_safe) return -EIO;
    int error=0;
    for (unsigned i=0;i<4;i++) if (leases[i].handle>=0) {
        int e=registry_quiesce(leases[i].handle,leases[i].generation,release_proof);
        if (!e) e=registry_release(leases[i].handle,leases[i].generation);
        if (e) error=e;
        else leases[i].handle=-1;
    }
    if (error) { blocked=true; reason="release_failed"; return error; }
    if (dma_pin.ticket && mm_unpin && mm_unpin(&dma_pin,dma_pin.generation)) {
        blocked=true; reason="unpin_failed"; return -EIO;
    }
    if (dma_phys) for (unsigned i=0;i<INTEL_DMA_PAGES;i++) mm_free(dma_phys+i*4096u,MM_DMA);
    dma_phys=0; return 0;
}
static int runtime_stop(void *ctx)
{
    int e=intel_engine_stop(ctx);
    if (!e) e=release_all();
    return e;
}
static int acquire(unsigned i, uint32_t phys, unsigned bytes)
{
    if (!nonram(phys,bytes)) return -ENODEV;
    struct lease *l=&leases[i];
    l->handle=registry_claim(RES_MMIO,phys,phys+bytes,"intel2d",false);
    if (l->handle<0) return l->handle;
    l->generation=registry_get((unsigned)l->handle)->generation; l->bytes=bytes;
    int e=registry_activate(l->handle,l->generation);
    if (e) return e;
    /* No CPU access is needed to the private aperture; GTT is the GPU alias. */
    if (i!=3) l->mapping=vmm_map_mmio(phys,bytes,true);
    return i!=3 && !l->mapping ? -ENOMEM : 0;
}
static bool stolen_valid(void)
{
    static const unsigned sizes[]={0,1,4,8,16,32,48,64};
    unsigned gms=(original_ggc>>20)&15u;
    unsigned size=0;
    if (chip->gen2) {
        unsigned tseg=0;
        unsigned esmramc=original_tseg>>(gpu->device==0x2562 ? 16 : 8)&255u;
        if (gpu->device==0x2562) {
            size=gms==2 ? 512u<<10 : gms==3 ? 1u<<20 : gms==4 ? 8u<<20 : 0;
            if (esmramc&1u) {
                if ((esmramc&6u)==4u) tseg=512u<<10;
                else if ((esmramc&6u)==6u) tseg=1u<<20;
                else return false;
            }
        } else {
            if (gms<=5) size=sizes[gms]<<20;
            if (esmramc&1u) tseg=1u<<20;
        }
        uint64_t tom=(uint64_t)(original_drb>>24)*(32u<<20);
        if (!size || tom>UINT32_MAX || tom<size+tseg) return false;
        stolen_start=(uint32_t)tom-size-tseg;
    } else {
        if (gms<ARRAY_SIZE(sizes)) size=sizes[gms]<<20;
        stolen_start=original_bsm&0xfff00000u;
    }
    if (!size || !stolen_start || stolen_start>UINT32_MAX-size ||
        !nonram(stolen_start,size)) return false;
    stolen_end=stolen_start+size;
    /* PMM reservations must not name kernel/modules/tables in stolen DRAM. */
    const struct pmm_reservation *reserved;
    unsigned n=pmm_reservations(&reserved);
    for (unsigned i=0;i<n;i++) if (stolen_start<reserved[i].end && stolen_end>reserved[i].start) return false;
    return true;
}
static bool scanout_valid(void)
{
    plane_control=read_reg(0,0x70180);
    unsigned format=display.bpp==16 ? 5u : 6u;
    /* Only ordinary enabled linear plane A; no stereo/keying/doubling. */
    if (!(plane_control&(1u<<31)) || ((plane_control>>26)&15u)!=format ||
        (plane_control&((1u<<25)|(1u<<22)|(1u<<20)|(1u<<10))) ||
        read_reg(0,0x70184) || read_reg(0,0x70188)!=display.pitch ||
        (read_reg(0,0x71180)&(1u<<31))) return false;
    unsigned pages=(display.size+4095u)/4096u;
    for (unsigned i=0;i<pages;i++) {
        uint32_t p=read_pte(0,i), address=p&~4095u;
        if (!(p&1u) || (p&0xffeu) || address<stolen_start || address>=stolen_end ||
            address>=stolen_end-4096u ||
            (address>=(original_pgtbl&~4095u) &&
             (uint64_t)address<(uint64_t)(original_pgtbl&~4095u)+gtt_bytes)) return false;
        /* Conservative contiguous stolen scanout, without aliases/tiling. */
        if (i && address!=(read_pte(0,i-1)&~4095u)+4096u) return false;
    }
    return true;
}
static int activation(void *ctx)
{
    struct intel_engine *s=ctx;
    int e=intel_engine_start(s);
    if (e) goto failed;
    /* Disjoint saved blocks: exercise genuine FILL and COPY before binding.
     * Every LFB read/restore is after breadcrumb + idle. */
    uint8_t saved[32], expected[32];
    unsigned bytes=display.bpp/8, count=8u*bytes;
    uint8_t *pixels=display.mapped+(display.height-1)*display.pitch;
    memcpy(saved,pixels,count); memcpy(expected,saved,count);
    uint32_t color=0;
    memcpy(&color,saved,bytes);
    color^=display.bpp==16 ? 0x5aa5u : 0x005aa53cu;
    struct fb_rect r={0,(int32_t)display.height-1,4,1};
    e=s->ops.fill_rect(s,&r,color);
    if (!e) e=s->ops.copy_rect(s,&r,4,r.y);
    for (unsigned x=0;x<8;x++) for (unsigned j=0;j<bytes;j++)
        expected[x*bytes+j]=(uint8_t)(color>>(j*8));
    if (!e && memcmp(pixels,expected,count)) e=-EIO;
    if (e) {
        intel_engine_quarantine(s,"pixel_probe_failed");
        if (intel_engine_stop(s)) goto failed;
    }
    if (s->cpu_safe) { memcpy(pixels,saved,count); framebuffer_write_drain(); }
    if (!e) { s->operations=0; return 0; } /* qualification is not a user draw */
failed:
    reason=s->reason ? s->reason : "activation_failed";
    if (s->mapped) (void)intel_engine_stop(s);
    return e;
}
static const struct accel_ops *start(const struct fb_device *d, struct vfs *vfs)
{
    (void)vfs;
    if (!marker_written || !eligible(d) || dma_phys || d->width<8) return 0;
    display=*d;
    original_ggc=config(bridge,0x50);
    if (chip->gen2) {
        original_tseg=config(bridge,gpu->device==0x2562 ? 0x9c : 0x60);
        original_drb=config(gpu->device==0x2562 ? bridge : memctrl,gpu->device==0x2562 ? 0x60 : 0x40);
        aperture=(original_ggc&(1u<<16)) ? 64u<<20 : 128u<<20;
    } else {
        original_bsm=config(gpu,0x5c); original_msac=config(gpu,0x60);
        original_ifp=config(bridge,0x60);
        aperture=(original_msac&(2u<<16)) ? 128u<<20 : 256u<<20;
    }
    gtt_bytes=chip->gen2 ? aperture/1024u : GTT_BYTES;
    if ((d->phys&(aperture-1u)) || (uint64_t)d->phys+aperture>UINT32_MAX || !stolen_valid() ||
        (chip->gen2 ? !(original_ggc&(4u<<16)) : !(original_ifp&1u) || (original_ifp&4094u))) {
        reason="firmware_memory_unqualified"; return 0;
    }
    uint32_t mmadr=gpu->bar[chip->gen2 ? 1 : 0];
    int e=acquire(0,mmadr,chip->gen2 ? 0x10000u : MMIO_BYTES);
    if (!e) e=acquire(1,chip->gen2 ? mmadr+0x10000u : gpu->bar[3],gtt_bytes);
    if (!e) e=acquire(2,chip->gen2 ? mmadr+0x70000u : original_ifp&~4095u,
                       chip->gen2 ? 0x2000u : 4096u);
    if (e) goto refused;
    original_pgtbl=read_reg(0,INTEL_PGTBL);
    if (!(original_pgtbl&1u) || !nonram(original_pgtbl&~4095u,gtt_bytes) || !scanout_valid()) {
        e=-ENODEV; reason="scanout_unqualified"; goto refused;
    }
    dma_phys=pmm_alloc_contig(INTEL_DMA_PAGES);
    if (!dma_phys) { e=-ENOMEM; goto refused; }
    mm_claim(dma_phys,INTEL_DMA_PAGES,MM_DMA);
    dma_pin=(struct mm_pin){.generation=display.generation,.first_pfn=dma_phys/4096u,
        .pages=INTEL_DMA_PAGES,.address_mask=UINT32_MAX,.alignment=4096,.direction=MM_PIN_BIDIRECTIONAL};
    if (!mm_pin || !mm_unpin || (e=mm_pin(&dma_pin))) { e=-ENOMEM; goto refused; }
    uint32_t private_gpu=aperture-INTEL_DMA_BYTES;
    e=acquire(3,d->phys+private_gpu,INTEL_DMA_BYTES);
    if (e) goto refused;
    struct intel_io io={0,read_reg,write_reg,read_pte,write_pte,publish,status,now,relax,live};
    e=intel_reserve(&engine,d,&io,P2V(dma_phys),dma_phys,aperture/4096u,gpu->device);
    if (e) goto refused;
    engine.ops.stop=runtime_stop;
    e=fbdev_accel_probe(d,activation,&engine);
    if (e) goto refused;
    reason="none"; return &engine.ops;
refused:
    if (!engine.mapped && !engine.touched) (void)release_all();
    if (!reason || !strncmp(reason,"pending",sizeof("pending"))) reason="transport_refused";
    return 0;
}
static const struct accel_ops *fallback(void)
{
    if (engine.mapped) (void)runtime_stop(&engine);
    return !engine.cpu_safe ? &engine.ops : 0;
}

/* Each PCI owner has its own durable marker; file I/O is worker-only. */
static struct vfs_table marker_table;
static int marker(struct vfs *vfs, bool clear)
{
    if (!chip || !vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -EIO;
    char path[64];
    ksnprintf(path,sizeof(path),"C:/system/settings/intel2d-%04x.cfg",chip->device);
    struct vfs_table *table=&marker_table;
    int e=vfs_table_init(vfs,table,2);
    if (e) return e;
    if (!clear) {
        struct fat_entry entry;
        e=vfs_stat(table,path,&entry);
        if (!e && entry.size) { blocked=true; reason="previous_attempt"; e=-EIO; goto out; }
        if (e && e!=-FS_ENOENT) goto out;
    }
    if (vfs->volumes[2]->readonly) { e=-FS_EROFS; goto out; }
    int fd=vfs_open(table,path,VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { e=fd; goto out; }
    if (clear) e=vfs_truncate(table,fd,0);
    else {
        char text[64];
        int length=ksnprintf(text,sizeof(text),"driver=intel2d\npci=8086:%04x\naccel=attempt\n",chip->device);
        size_t done=0;
        e=vfs_write(table,fd,text,(size_t)length,&done);
        if (!e && done!=(size_t)length) e=-FS_EIO;
    }
    if (!e) e=vfs_commit(table,fd);
    int closed=vfs_close(table,fd);
    if (!e) e=closed;
out:
    vfs_table_destroy(table); return e;
}
static int attempt_begin(struct vfs *vfs)
{
    if (!eligible(fbdev_get())) return -ENODEV;
    int e=marker(vfs,false);
    marker_written=!e;
    if (e) { blocked=true; if (!strncmp(reason,"pending",sizeof("pending"))) reason="marker_unavailable"; }
    return e;
}
static int attempt_end(struct vfs *vfs)
{ int e=marker(vfs,true); if (!e) marker_written=false; return e; }
static int attempt_failed(struct vfs *vfs) { (void)vfs; blocked=true; return 0; }
static void quarantine(const char *why)
{ intel_engine_quarantine(&engine,why); reason=why; }
static bool summary(bool *ready, bool *used, const char **why)
{
    *ready=engine.active && !engine.quarantined;
    *used=engine.operations!=0; *why=engine.reason ? engine.reason : reason; return true;
}
static void snapshot(struct ciuki_accel_info *out)
{
    *out=(struct ciuki_accel_info){.driver=CIUKI_ACCEL_DRIVER_LEGACY_NATIVE,
        .capabilities=engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0,
        .quarantined=engine.quarantined,.generation=display.generation,
        .operations=engine.operations,.idle_waits=engine.waits,.fallbacks=engine.fallbacks};
}
static void record(void)
{
    if (engine.reason) reason=engine.reason;
    klog("[accel] driver=intel2d ready=%u native_used=%u quarantined=%u cpu_ready=%u reason=%s",
        engine.active,engine.operations!=0,engine.quarantined,engine.cpu_safe,reason);
}
static const struct display_match gen2_matches[]={
    {0x8086,0x3577,3,255},{0x8086,0x2562,3,255},
    {0x8086,0x3582,3,255},{0x8086,0x2572,3,255}
};
static const struct display_match gen3_matches[]={{0x8086,0x2582,3,255}};
#define ADAPTER(table,bar) \
    .render3d=&render_family,.id="intel2d",.name="Intel integrated graphics",.matches=table, \
    .attempt_files={"C:/system/settings/intel2d-%04x.cfg"}, \
    .match_count=ARRAY_SIZE(table),.framebuffer_bar=bar,.probe=probe,.eligible=eligible, \
    .start=start,.fallback=fallback,.attempt_begin=attempt_begin,.attempt_end=attempt_end, \
    .attempt_failed=attempt_failed,.record=record,.quarantine=quarantine,.summary=summary,.snapshot=snapshot
const struct display_driver intel2d_gen2_driver={ADAPTER(gen2_matches,0)};
const struct display_driver intel2d_gen3_driver={ADAPTER(gen3_matches,2)};
