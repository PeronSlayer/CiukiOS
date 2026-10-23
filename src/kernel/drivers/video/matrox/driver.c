/* Native Ciuki adapter for Linux v6.12 Matrox engine recipes in engine.c.
 * No upstream OS glue is copied. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/timing.h>
#include "../../../fs/vfs.h"
#include "matrox.h"

static struct matrox_engine engine;
static volatile uint8_t *registers;
static void *register_window, *cursor_window;
static int reg_claim=-1, cursor_claim=-1;
static gen_t reg_generation, cursor_generation;
static bool blocked, armed;
static const char *reason="pending";
static struct vfs_table marker_table;
static struct matrox_io activation_io;
#define MARKER_PATH "C:/system/settings/mga2d.cfg"
#define LEGACY_PATH "C:/system/settings/accel.cfg"

static uint32_t read32(void *ctx,unsigned reg)
{ (void)ctx; return *(volatile uint32_t *)(registers+reg); }
static void write32(void *ctx,unsigned reg,uint32_t value)
{ (void)ctx; *(volatile uint32_t *)(registers+reg)=value; }
static uint8_t read8(void *ctx,unsigned reg)
{ (void)ctx; return registers[reg]; }
static void write8(void *ctx,unsigned reg,uint8_t value)
{ (void)ctx; registers[reg]=value; }
static uint64_t now_us(void *ctx)
{
    (void)ctx;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000;
    return read_tsc_serialized()/(g_tsc_per_ms/1000 ? g_tsc_per_ms/1000 : 1);
}
static void drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static bool extent(uint32_t start,uint32_t bytes)
{
    if (start<0x100000 || !bytes || bytes>UINT32_MAX-start) return false;
    uint64_t end=(uint64_t)start+bytes;
    if (start<g_boot.kernel_end && end>g_boot.kernel_start) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *e=&g_boot.e820[i];
        if (e->type==CBI_E820_RAM && start<e->base+e->length && end>e->base) return false;
    }
    return true;
}
static bool overlap(uint32_t a,uint32_t an,uint32_t b,uint32_t bn)
{ return (uint64_t)a+an>b && (uint64_t)b+bn>a; }
static const struct pci_func *qualified(const struct fb_device *fb)
{
    const struct pci_func *p=display_driver_owner();
    if (display_driver_bound()!=&matrox_driver || !matrox_mode(fb,p) ||
        !p->bar[0] || !p->bar[1] || (p->bar[0]&7u) || (p->bar[1]&7u) ||
        (p->bar[0]&~15u)!=fb->phys || ((p->bar[1]&~15u)&(MATROX_MMIO_BYTES-1)) ||
        !extent(p->bar[1]&~15u,MATROX_MMIO_BYTES) ||
        !g_boot.display.vram_bytes || fb->size>g_boot.display.vram_bytes ||
        !extent(fb->phys,g_boot.display.vram_bytes) ||
        overlap(fb->phys,g_boot.display.vram_bytes,p->bar[1]&~15u,MATROX_MMIO_BYTES)) return 0;
    /* Recheck the exact active LFB lease even though registry selection
     * already established it; activation must not accept a stale mode. */
    const struct resource *r=fb->handle>=0 ? registry_get((unsigned)fb->handle) : 0;
    if (!r || !registry_valid(fb->handle,fb->generation) || r->type!=RES_MMIO ||
        r->state!=RS_ACTIVE || r->start!=fb->phys || r->end!=fb->phys+fb->size ||
        !r->owner || strncmp(r->owner,"fbdev",sizeof("fbdev"))) return 0;
    return p;
}
static bool eligible(const struct fb_device *fb)
{
    const char *why;
    if (display_accel_disabled(&why)) { reason=why; return false; }
    if (blocked || engine.quarantined) return false;
    if (!qualified(fb)) { reason="unsupported_mode_or_aperture"; return false; }
    return true;
}
/* A nonempty legacy record still blocks this adapter. Do not interpret or
 * clear an interrupted unkeyed attempt as permission to switch families. */
static int marker(struct vfs *vfs,bool clear)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    int e=vfs_table_init(vfs,&marker_table,2); if (e) return e;
    if (!clear) {
        const char *paths[]={LEGACY_PATH,MARKER_PATH};
        for (unsigned i=0;i<ARRAY_SIZE(paths);i++) {
            struct fat_entry entry;
            e=vfs_stat(&marker_table,paths[i],&entry);
            if (!e && entry.size) { e=-EIO; goto done; }
            if (e && e!=-FS_ENOENT) goto done;
        }
    }
    if (vfs->volumes[2]->readonly) { e=-FS_EROFS; goto done; }
    int fd=vfs_open(&marker_table,MARKER_PATH,VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { e=fd; goto done; }
    if (clear) e=vfs_truncate(&marker_table,fd,0);
    else {
        const struct pci_func *p=display_driver_owner();
        char text[80];
        int n=ksnprintf(text,sizeof(text),"driver=matrox pci=%04x:%04x revision=%02x attempt\n",
                         p->vendor,p->device,p->revision);
        size_t written=0;
        e=vfs_write(&marker_table,fd,text,(size_t)n,&written);
        if (!e && written!=(size_t)n) e=-FS_EIO;
    }
    if (!e) e=vfs_commit(&marker_table,fd);
    int closed=vfs_close(&marker_table,fd);
    if (!e) e=closed;
done:
    vfs_table_destroy(&marker_table);
    return e;
}
static int attempt_begin(struct vfs *vfs)
{
    if (!eligible(fbdev_get()) || armed) return -EIO;
    int e=marker(vfs,false);
    if (e) { blocked=true; reason="retained_or_unavailable_marker"; }
    else armed=true;
    return e;
}
static int attempt_end(struct vfs *vfs)
{
    if (!armed || !engine.active || engine.quarantined || !engine.cpu_safe) return -EIO;
    int e=marker(vfs,true); armed=false;
    if (e) { blocked=true; reason="marker_clear_failed"; }
    return e;
}
static int attempt_failed(struct vfs *vfs)
{
    (void)vfs; blocked=true; armed=false;
    reason=engine.reason ? engine.reason : "start_failed";
    return 0; /* keep the committed nonempty attempt; never clear on failure */
}
static bool cursor_idle(int handle,gen_t gen)
{
    (void)handle; (void)gen;
    if (!engine.cpu_safe || !engine.cursor_hidden) return false;
    if (engine.cursor) vmm_revoke_mmio((void *)engine.cursor,PAGE_SIZE);
    engine.cursor=0;
    return true;
}
static bool registers_idle(int handle,gen_t gen)
{
    (void)handle; (void)gen;
    if (!engine.cpu_safe || !engine.cursor_hidden || engine.active) return false;
    if (registers) vmm_revoke_mmio((void *)registers,MATROX_MMIO_BYTES);
    registers=0;
    return true;
}
static int release_claim(int *handle,gen_t gen,registry_idle_proof_fn proof)
{
    if (*handle<0) return 0;
    int e=registry_quiesce(*handle,gen,proof);
    if (!e) e=registry_release(*handle,gen);
    if (!e) *handle=-1;
    return e;
}
static int runtime_sync(void *ctx)
{
    (void)ctx;
    if (!registers) { framebuffer_write_drain(); return engine.cpu_safe ? 0 : -EIO; }
    /* Installed only in the adapter's copy, not engine.ops. */
    return engine.ops.sync(&engine);
}
static int runtime_stop(void *ctx)
{
    (void)ctx;
    engine.active=false; engine.shape=false;
    int e=registers ? matrox_engine_prove(&engine) : engine.cpu_safe ? 0 : -EIO;
    if (e) {
        if (registers) matrox_engine_quarantine(&engine,"stop_unproved");
        else { engine.quarantined=true; engine.reason="stop_unproved"; }
        blocked=true; return e;
    }
    e=release_claim(&cursor_claim,cursor_generation,cursor_idle);
    if (!e) e=release_claim(&reg_claim,reg_generation,registers_idle);
    if (e) { blocked=true; reason="release_failed"; return e; }
    engine.touched=false;
    reason=engine.quarantined ? engine.reason : "stopped";
    return 0;
}
static struct accel_ops runtime_ops;
static const struct accel_ops *fallback(void)
{
    if (!registers) return engine.cpu_safe ? 0 : engine.touched ? &runtime_ops : 0;
    engine.active=false;
    if (matrox_engine_prove(&engine)) blocked=true;
    return &runtime_ops; /* a safe guard still allows synchronized swdib */
}
static void quarantine(const char *why)
{
    blocked=true; reason=why;
    if (registers) matrox_engine_quarantine(&engine,why);
    else { engine.active=false; engine.quarantined=true; engine.reason=why; }
}
static uint32_t pixel_read(const struct fb_device *fb,unsigned x,unsigned y)
{
    volatile uint8_t *p=fb->mapped+y*fb->pitch+x*(fb->bpp/8);
    uint32_t v=0;
    for (unsigned b=0;b<fb->bpp/8;b++) v|=(uint32_t)p[b]<<(b*8);
    return v;
}
static int activate(void *ctx)
{
    const struct fb_device *fb=ctx;
    volatile uint8_t *cursor=engine.cursor;
    int e=matrox_engine_init(&engine,fb,&activation_io,cursor,engine.cursor_offset);
    runtime_ops=engine.ops; runtime_ops.stop=runtime_stop; runtime_ops.sync=runtime_sync;
    if (e) return e;
    /* Native-only saved block, under fbdev's presenter and drawing claim.
     * A changed marker is required: an ignored engine cannot pass. */
    uint8_t saved[2*2*4];
    unsigned bytes=fb->bpp/8;
    for (unsigned y=0;y<2;y++) for (unsigned b=0;b<2*bytes;b++) saved[y*2*bytes+b]=fb->mapped[y*fb->pitch+b];
    uint32_t colour=fb->bpp==16 ? (pixel_read(fb,0,0)^0x5aa5u)&(fb->green_size==5 ? 0x7fffu : 0xffffu) :
        (pixel_read(fb,0,0)^0x5aa55au)&0xffffffu;
    struct fb_rect block={0,0,2,2};
    e=engine.ops.fill_rect(&engine,&block,colour);
    if (!e) for (unsigned y=0;y<2;y++) for (unsigned x=0;x<2;x++)
        if (pixel_read(fb,x,y)!=colour) e=-EIO;
    if (!e || !matrox_engine_prove(&engine)) {
        for (unsigned y=0;y<2;y++) for (unsigned b=0;b<2*bytes;b++) fb->mapped[y*fb->pitch+b]=saved[y*2*bytes+b];
        framebuffer_write_drain();
    }
    if (e) matrox_engine_quarantine(&engine,"native_probe_failed");
    return e;
}
static const struct accel_ops *start_with_io(const struct fb_device *fb,struct vfs *vfs,const struct matrox_io *io)
{
    (void)vfs;
    if (!armed || !eligible(fb) || fb->width<2 || fb->height<2 || registers) return 0;
    const struct pci_func *p=qualified(fb);
    if (!p || !io) return 0;
    activation_io=*io;
    uint32_t base=p->bar[1]&~15u;
    reg_claim=registry_claim(RES_MMIO,base,base+MATROX_MMIO_BYTES,"matrox-registers",false);
    if (reg_claim<0) { reason="mmio_claim_failed"; return 0; }
    reg_generation=registry_get((unsigned)reg_claim)->generation;
    if (!register_window) register_window=vmm_reserve_mmio(MATROX_MMIO_BYTES);
    registers=register_window ? vmm_replace_mmio(register_window,MATROX_MMIO_BYTES,base,MATROX_MMIO_BYTES) : 0;
    engine.cpu_safe=true; engine.cursor_hidden=true;
    if (!registers || registry_activate(reg_claim,reg_generation)) {
        reason="mmio_mapping_failed";
        (void)release_claim(&reg_claim,reg_generation,registers_idle);
        return 0;
    }
    /* Reserve a full page disjoint from the exact active fbdev lease; never
     * steal visible pixels or guess VRAM by probing. DAC base is 24 bits. */
    uint32_t usable=g_boot.display.vram_bytes;
    if (usable>16u*1024*1024) usable=16u*1024*1024;
    bool second_head=(p->device==0x0525 || p->device==0x2527) &&
        (activation_io.read32(activation_io.ctx,M_C2CTL)&1u);
    if (!second_head && usable>=PAGE_SIZE && PAGE_ALIGN_DOWN(usable)-PAGE_SIZE>=fb->size) {
        uint32_t offset=PAGE_ALIGN_DOWN(usable)-PAGE_SIZE, phys=fb->phys+offset;
        cursor_claim=registry_claim(RES_MMIO,phys,phys+PAGE_SIZE,"matrox-cursor",false);
        if (cursor_claim>=0) {
            cursor_generation=registry_get((unsigned)cursor_claim)->generation;
            if (!cursor_window) cursor_window=vmm_reserve_mmio(PAGE_SIZE);
            engine.cursor=cursor_window ? vmm_replace_mmio(cursor_window,PAGE_SIZE,phys,PAGE_SIZE) : 0;
            if (!engine.cursor || registry_activate(cursor_claim,cursor_generation))
                (void)release_claim(&cursor_claim,cursor_generation,cursor_idle);
            else engine.cursor_offset=offset;
        }
    }
    int e=fbdev_accel_probe(fb,activate,(void *)fb);
    if (e) {
        blocked=true; reason=engine.quarantined ? engine.reason : "activation_excluded";
        if (!engine.touched || !matrox_engine_prove(&engine)) (void)runtime_stop(&engine);
        return 0;
    }
    reason="ready";
    return &runtime_ops;
}
static const struct accel_ops *start(const struct fb_device *fb,struct vfs *vfs)
{
    const struct matrox_io io={0,read32,write32,read8,write8,now_us,drain};
    return start_with_io(fb,vfs,&io);
}
static bool summary(bool *ready,bool *used,const char **why)
{
    *ready=engine.active && !engine.quarantined; *used=false;
    *why=engine.quarantined ? engine.reason : reason;
    return true;
}
static void snapshot(struct ciuki_accel_info *out)
{
    *out=(struct ciuki_accel_info){.driver=CIUKI_ACCEL_DRIVER_LEGACY_NATIVE,
        .capabilities=engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0,
        .quarantined=engine.quarantined,.generation=engine.fb.generation,
        .operations=engine.operations,.idle_waits=engine.waits,.fallbacks=engine.failures};
}
static void record(void)
{
    const struct pci_func *p=display_driver_owner();
    const char *chip=p && p->device==0x0525 ? (p->revision<=0x80 ? "G400" : "G450") :
        p && p->device==0x2527 ? "G550" : "G200";
    klog("[accel] driver=matrox family=%s ready=%u cpu_safe=%u quarantined=%u reason=%s",
         chip,engine.active,engine.cpu_safe,engine.quarantined,engine.quarantined ? engine.reason : reason);
}
static int cursor_test(const struct accel_ops *ops,uint64_t deadline,bool (*cancel)(void *),void *ctx)
{
    if (!ops || !(ops->capabilities(ops->ctx)&2u)) return -EOPNOTSUPP;
    struct accel_cursor transparent={0};
    int e=0;
    for (unsigned i=0;i<4 && !e;i++) {
        if (g_ticks>=deadline || (cancel && cancel(ctx))) { e=g_ticks>=deadline ? -ETIMEDOUT : -EINTR; break; }
        if (!i) e=ops->cursor_show(ops->ctx,false);
        else if (i==1) e=ops->cursor_set(ops->ctx,&transparent);
        else if (i==2) e=ops->cursor_move(ops->ctx,0,0);
        else e=ops->cursor_show(ops->ctx,true);
    }
    int hidden=ops->cursor_show(ops->ctx,false);
    return e ? e : hidden;
}
static const struct display_match matches[]={
    {0x102b,0x0520,3,255},{0x102b,0x0521,3,255},
    {0x102b,0x0525,3,255},{0x102b,0x2527,3,255}
};
const struct display_driver matrox_driver={
    .id="matrox",.attempt_files={LEGACY_PATH,MARKER_PATH},.name="Matrox G200/G400/G450/G550",.matches=matches,
    .match_count=ARRAY_SIZE(matches),.framebuffer_bar=0,.probe=matrox_probe,.eligible=eligible,
    .start=start,.fallback=fallback,.attempt_begin=attempt_begin,.attempt_end=attempt_end,
    .attempt_failed=attempt_failed,.record=record,.quarantine=quarantine,
    .summary=summary,.snapshot=snapshot,.cursor_test=cursor_test
};
