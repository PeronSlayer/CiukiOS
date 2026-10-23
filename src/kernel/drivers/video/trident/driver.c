/* Ciuki lifecycle and UC MMIO transport; no upstream OS glue copied.
 * Register references: Linux v6.12 tridentfb.c and include/video/trident.h.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/vga_ports.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/timing.h>
#include "../../../fs/vfs.h"
#include <ciuki/swdib.h>
#include "engine.h"

static const struct display_match matches[]={
    {0x1023,0x9880,3,255},{0x1023,0x8400,3,255},{0x1023,0x8420,3,255},
    {0x1023,0x8500,3,255},{0x1023,0x8520,3,255},{0x1023,0x8600,3,255},
    {0x1023,0x8620,3,255},{0x1023,0x8820,3,255}
};
static struct tb_engine engine;
static void *window;
static uint8_t *registers;
static int mmio_claim=-1,port_claim=-1;
static gen_t mmio_generation,port_generation;
static bool authorized,started,blocked;
static bool access_saved;
static uint8_t saved_mmio,saved_engine;
static const char *recorded_reason;
static unsigned recorded_state=UINT32_MAX;
static struct vfs_table marker_table;
#define ATTEMPT_PATH "C:/system/settings/trident.cfg"

static bool probe(const struct pci_func *p)
{
    return p && p->vendor==0x1023 && tb_device(p->device) && p->class_code==3 &&
        !(p->header_type&0x7f) && (p->command&3u)==3u;
}
static bool intersects(uint64_t a,uint64_t bytes,uint64_t b,uint64_t end)
{ return a<end && b<a+bytes; }
static bool aperture(const struct pci_func *p,const struct fb_device *d)
{
    if ((p->bar[1]&7u) || !(p->bar[1]&~15u)) return false;
    uint32_t base=p->bar[1]&~15u;
    if ((base&0x3fffu) || base<0x100000 || base>UINT32_MAX-0x3000u) return false;
    uint64_t start=base+0x2000u,end=start+PAGE_SIZE;
    if (intersects(d->phys,d->size,start,end) ||
        intersects(g_boot.kernel_start,g_boot.kernel_end-g_boot.kernel_start,start,end)) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *e=&g_boot.e820[i];
        if (e->type==CBI_E820_RAM && (e->length>UINT64_MAX-e->base ||
            intersects(e->base,e->length,start,end))) return false;
    }
    for (unsigned i=0;i<pci_count();i++) {
        const struct pci_func *peer=pci_get(i);
        if (!peer) continue;
        for (unsigned n=0;n<6;n++) {
            if (peer==p && n==1) continue;
            uint32_t bar=peer->bar[n];
            if (bar && !(bar&1u) && (bar&~15u)>=base && (bar&~15u)<end) return false;
        }
    }
    return true;
}
static const struct pci_func *candidate(const struct fb_device *d)
{
    const struct pci_func *p=0;
    const struct display_driver *const table[]={&trident_blade_driver};
    if (!display_driver_select(d,table,1,&p) || !tb_format(tb_device(p->device),d) ||
        !aperture(p,d)) return 0;
    return p;
}
static bool eligible(const struct fb_device *d)
{
    const char *why=0;
    if (display_accel_disabled(&why)) { engine.reason=why; return false; }
    if (blocked || engine.quarantined) return false;
    if (!candidate(d) || d->width<16 || d->height<16) {
        engine.reason="unsupported_mode_or_aperture"; return false;
    }
    return true;
}
static uint64_t now_us(void *ctx)
{
    (void)ctx;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000;
    uint64_t c=ktime_cycles();
    return c/g_tsc_per_ms*1000+c%g_tsc_per_ms*1000/g_tsc_per_ms;
}
/* Only the engine page is mapped. Packed access preserves the donor's
 * unaligned XP ROP dword write without C alignment undefined behavior. */
struct mmio32 { uint32_t value; } __attribute__((packed));
static uint8_t read8(void *ctx,unsigned r) { (void)ctx; return *(volatile uint8_t *)(registers+(r-0x2000)); }
static uint32_t read32(void *ctx,unsigned r)
{ (void)ctx; return ((volatile struct mmio32 *)(registers+(r-0x2000)))->value; }
static void write8(void *ctx,unsigned r,uint8_t v)
{ (void)ctx; *(volatile uint8_t *)(registers+(r-0x2000))=v; }
static void write32(void *ctx,unsigned r,uint32_t v)
{ (void)ctx; ((volatile struct mmio32 *)(registers+(r-0x2000)))->value=v; }
static uint8_t cr(uint8_t index)
{ outb(0x3d4,index); return inb(0x3d5); }
static void cr_write(uint8_t index,uint8_t value)
{ outb(0x3d4,index); outb(0x3d5,value); }
static bool firmware_ready(const struct fb_device *d)
{
    if (!(inb(0x3cc)&1u)) return false;
    uint8_t index=inb(0x3d4);
    bool good=!cr(0x0c) && !cr(0x0d) && !(cr(0x1e)&0x20u) && !(cr(0x27)&7u);
    uint32_t pitch8=cr(0x13)|((cr(0x29)&0x30u)<<4);
    outb(0x3d4,index);
    return good && pitch8*8==d->pitch;
}
/* Linux enable_mmio and X.Org's GraphEngReg setup describe these access
 * bits. Save/restore only engine access, keeping VBE's scanout untouched.
 * Called under exclusive activation/stop ownership with active port leases. */
static bool engine_access(bool enable)
{
    uint8_t seq_index=inb(0x3c4),cr_saved_index=inb(0x3d4);
    outb(0x3c4,0x0b); (void)inb(0x3c5); /* select new register bank */
    outb(0x3c4,0x0e); uint8_t mode=inb(0x3c5); outb(0x3c5,0x80);
    outb(0x3c4,0x11); uint8_t protect=inb(0x3c5); outb(0x3c5,0x92);
    if (enable) { saved_mmio=cr(0x39); saved_engine=cr(0x36); access_saved=true; }
    uint8_t mmio=enable ? saved_mmio|1u : saved_mmio;
    uint8_t graphics=enable ? saved_engine|0x80u : saved_engine;
    cr_write(0x39,mmio); cr_write(0x36,graphics);
    bool good=cr(0x39)==mmio && cr(0x36)==graphics;
    outb(0x3c4,0x11); outb(0x3c5,protect);
    outb(0x3c4,0x0e); outb(0x3c5,mode);
    outb(0x3c4,seq_index); outb(0x3d4,cr_saved_index);
    if (!enable && good) access_saved=false;
    return good;
}
static bool access_valid(void *ctx)
{
    (void)ctx;
    return registers && registry_valid(mmio_claim,mmio_generation) &&
        registry_valid(port_claim,port_generation) && engine.fb &&
        registry_valid(engine.fb->handle,engine.fb->generation);
}
static bool unmap_idle(int handle,gen_t generation)
{
    (void)handle; (void)generation;
    if (engine.active || !engine.cpu_safe) return false;
    if (access_saved && !engine_access(false)) return false;
    if (registers) { vmm_revoke_mmio(registers,PAGE_SIZE); registers=0; }
    return true;
}
static bool ports_idle(int handle,gen_t generation)
{ (void)handle; (void)generation; return !registers && !engine.active && engine.cpu_safe; }
static int release_resources(void)
{
    if (!engine.cpu_safe || engine.active) return -EIO;
    if (mmio_claim>=0) {
        const struct resource *r=registry_get(mmio_claim);
        if (r && r->state==RS_CLAIMED && registry_activate(mmio_claim,mmio_generation)) return -EIO;
        int err=registry_quiesce(mmio_claim,mmio_generation,unmap_idle);
        if (!err) err=registry_release(mmio_claim,mmio_generation);
        if (err) return err;
        mmio_claim=-1;
    }
    if (port_claim>=0) {
        const struct resource *r=registry_get(port_claim);
        if (r && r->state==RS_CLAIMED && registry_activate(port_claim,port_generation)) return -EIO;
        int err=registry_quiesce(port_claim,port_generation,ports_idle);
        if (!err) err=registry_release(port_claim,port_generation);
        if (err) return err;
        port_claim=-1;
    }
    return 0;
}
static int stop(void *ctx)
{
    struct tb_engine *e=ctx;
    if (!registers) return e->cpu_safe ? 0 : -EIO;
    int err=e->ops.sync(ctx);
    e->active=false;
    if (!err) err=release_resources();
    if (err) { blocked=true; tb_quarantine(e,"stop_failed"); }
    else { started=false; if (!e->quarantined) e->reason="stopped"; }
    return err;
}
static int compare(const struct fb_device *d,const struct fb_device *ref)
{
    int err=tb_idle(&engine);
    if (err || !engine.cpu_safe) return err ? err : -EIO;
    for (unsigned y=0;y<16;y++)
        if (memcmp(d->mapped+y*d->pitch,ref->mapped+y*ref->pitch,ref->pitch)) return -EIO;
    return 0;
}
static int activation_check(void *ctx)
{
    struct tb_engine *e=ctx;
    const struct fb_device *d=e->fb;
    uint8_t saved[16*16*4],reference[16*16*4];
    struct fb_device ref=*d; ref.width=ref.height=16;
    ref.pitch=16*(d->bpp/8); ref.size=16*ref.pitch; ref.mapped=reference;
    if (!engine_access(true)) { tb_quarantine(e,"engine_enable_failed"); return -EIO; }
    int err=tb_idle(e);
    if (err) return err;
    for (unsigned y=0;y<16;y++) memcpy(saved+y*ref.pitch,d->mapped+y*d->pitch,ref.pitch);
    memcpy(reference,saved,ref.size);
    err=tb_initialize(e);
    struct fb_rect block={0,0,16,16},seed={0,0,4,4},source={0,0,12,12};
    if (!err) {
        uint32_t c=swdib_pack(d,0x36a5e7),s=swdib_pack(d,0xcb7123);
        swdib_fill(&ref,&block,c); swdib_fill(&ref,&seed,s);
        framebuffer_write_drain();
        err=e->ops.fill_rect(e,&block,c);
        if (!err) err=e->ops.fill_rect(e,&seed,s);
        if (!err) err=compare(d,&ref);
    }
    if (!err) {
        swdib_copy(&ref,&source,4,4);
        err=e->ops.copy_rect(e,&source,4,4);
        if (!err) err=compare(d,&ref);
    }
    if (!err) {
        source.x=source.y=4;
        swdib_copy(&ref,&source,0,0);
        err=e->ops.copy_rect(e,&source,0,0);
        if (!err) err=compare(d,&ref);
    }
    /* Always prove idle again before restoring saved pixels, including a
     * failed initialization or read-back. A failed proof retains the guard. */
    int idle=tb_idle(e);
    if (!idle) {
        for (unsigned y=0;y<16;y++) memcpy(d->mapped+y*d->pitch,saved+y*ref.pitch,ref.pitch);
        framebuffer_write_drain();
    }
    if (!err) err=idle;
    if (err) tb_quarantine(e,"native_comparison_failed");
    return err;
}
static const struct accel_ops *fallback(void)
{
    if (!started) return 0;
    engine.active=false;
    if (registers) (void)tb_idle(&engine);
    return &engine.ops;
}
static const struct accel_ops *start(const struct fb_device *d,struct vfs *vfs)
{
    (void)vfs;
    if (!authorized || !eligible(d) || d->width<16 || d->height<16) return 0;
    authorized=false;
    const struct pci_func *p=candidate(d);
    if (!p) return 0;
    struct tb_io io={0,read8,read32,write8,write32,now_us,access_valid};
    tb_construct(&engine,tb_device(p->device),d,&io);
    port_claim=registry_claim(RES_PORT,CIUKI_VGA_COLOR_FIRST,CIUKI_VGA_COLOR_END,"trident-vga",false);
    if (port_claim<0) { engine.reason="port_claim_failed"; return 0; }
    port_generation=registry_get(port_claim)->generation;
    if (registry_activate(port_claim,port_generation) || !firmware_ready(d)) {
        engine.reason="firmware_scanout_mismatch"; (void)release_resources(); return 0;
    }
    uint32_t base=(p->bar[1]&~15u)+0x2000u;
    mmio_claim=registry_claim(RES_MMIO,base,base+PAGE_SIZE,"trident-mmio",false);
    if (mmio_claim<0) { engine.reason="mmio_claim_failed"; (void)release_resources(); return 0; }
    mmio_generation=registry_get(mmio_claim)->generation;
    if (!window) window=vmm_reserve_mmio(PAGE_SIZE);
    if (window) registers=vmm_replace_mmio(window,PAGE_SIZE,base,PAGE_SIZE);
    if (!registers || registry_activate(mmio_claim,mmio_generation)) {
        engine.reason="mmio_mapping_failed"; (void)release_resources(); return 0;
    }
    engine.ops.stop=stop; started=true;
    if (fbdev_accel_probe(d,activation_check,&engine)) {
        blocked=true;
        if (!access_saved && !engine.quarantined) {
            /* Exclusive drawing was refused before the callback. No engine
             * access or command occurred, so this is a clean start failure. */
            engine.reason="activation_unavailable"; started=false;
            (void)release_resources();
        } else tb_quarantine(&engine,"activation_failed");
        return 0;
    }
    return &engine.ops;
}
static int marker(struct vfs *vfs,bool clear)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    int err=vfs_table_init(vfs,&marker_table,2);
    if (err) return err;
    struct fat_entry entry;
    if (!clear) {
        err=vfs_stat(&marker_table,ATTEMPT_PATH,&entry);
        if (!err && entry.size) { err=-FS_EIO; engine.reason="previous_attempt"; goto out; }
        if (err && err!=-FS_ENOENT) goto out;
    }
    if (vfs->volumes[2]->readonly) { err=-FS_EROFS; goto out; }
    int fd=vfs_open(&marker_table,ATTEMPT_PATH,VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { err=fd; goto out; }
    if (clear) err=vfs_truncate(&marker_table,fd,0);
    else {
        const char attempt[]="driver=trident\nstatus=attempt\n";
        size_t done=0;
        err=vfs_write(&marker_table,fd,attempt,sizeof(attempt)-1,&done);
        if (!err && done!=sizeof(attempt)-1) err=-FS_EIO;
    }
    if (!err) err=vfs_commit(&marker_table,fd);
    int closed=vfs_close(&marker_table,fd);
    if (!err) err=closed;
out:
    vfs_table_destroy(&marker_table);
    return err;
}
static int attempt_begin(struct vfs *vfs)
{
    const char *why=0;
    if (blocked || engine.quarantined || display_accel_disabled(&why)) return -EIO;
    authorized=false;
    int err=marker(vfs,false);
    if (err) { blocked=true; if (!engine.reason) engine.reason="marker_unavailable"; }
    else authorized=true;
    return err;
}
static int attempt_end(struct vfs *vfs)
{
    if (!started || !engine.active || engine.quarantined || !engine.cpu_safe) return -EIO;
    return marker(vfs,true);
}
static int attempt_failed(struct vfs *vfs)
{ (void)vfs; blocked=true; authorized=false; return 0; /* retain durable marker */ }
static void quarantine(const char *why) { blocked=true; tb_quarantine(&engine,why); }
static void record(void)
{
    unsigned state=engine.active | (engine.quarantined<<1) | (engine.cpu_safe<<2);
    if (recorded_reason==engine.reason && recorded_state==state) return;
    recorded_reason=engine.reason; recorded_state=state;
    klog("[accel] driver=trident ready=%u quarantined=%u cpu_ready=%u status=%08x reason=%s",engine.active,engine.quarantined,engine.cpu_safe,engine.status,engine.reason ? engine.reason : "pending");
}
static bool summary(bool *ready,bool *used,const char **reason)
{
    *ready=engine.active && !engine.quarantined; *used=false;
    *reason=engine.reason ? engine.reason : "pending"; return true;
}
static void snapshot(struct ciuki_accel_info *out)
{
    memset(out,0,sizeof(*out)); out->driver=engine.active ? CIUKI_ACCEL_DRIVER_LEGACY_NATIVE : 0;
    out->capabilities=engine.active && !engine.quarantined ? 1 : 0;
    out->quarantined=engine.quarantined;
    out->generation=engine.fb ? engine.fb->generation : 0;
    out->operations=engine.operations; out->idle_waits=engine.waits;
}
const struct display_driver trident_blade_driver={
    .id="trident",.attempt_files={ATTEMPT_PATH},.name="Trident Blade / CyberBlade",.matches=matches,
    .match_count=ARRAY_SIZE(matches),.framebuffer_bar=0,
    .probe=probe,.eligible=eligible,.start=start,.fallback=fallback,
    .attempt_begin=attempt_begin,.attempt_end=attempt_end,.attempt_failed=attempt_failed,
    .record=record,.quarantine=quarantine,.summary=summary,.snapshot=snapshot
};
