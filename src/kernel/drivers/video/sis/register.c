/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2001-2005 Thomas Winischhofer, Vienna, Austria.
 * Native SiS registry and resource glue.
 * PCI generation mapping: Linux v6.12 drivers/video/fbdev/sis/sis_main.h,
 * Thomas Winischhofer. Engine/cursor derivations keep notices in their files.
 * No BIOS interpreter, mode programming, AGP or host DMA is imported. */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/timing.h>
#include <ciuki/swdib.h>
#include "../../../fs/vfs.h"
#include "sis.h"

static const struct display_match matches300[] = {
    {0x1039,0x0300,3,255}, {0x1039,0x5300,3,255}, {0x1039,0x6300,3,255}
};
static const struct display_match matches315[] = {
    {0x1039,0x0310,3,255}, {0x1039,0x0315,3,255}, {0x1039,0x0325,3,255},
    {0x1039,0x5315,3,255}, {0x1039,0x6325,3,255}, {0x1039,0x0330,3,255},
    {0x1039,0x6330,3,255}
};
bool sis_pci_supported(const struct pci_func *p,enum sis_generation generation)
{
    if (!p || p->class_code!=3 || p->subclass || (p->header_type&0x7f) ||
        p->vendor!=0x1039 || (p->command&3)!=3 ||
        (p->bar[0]&7) || !(p->bar[0]&~15u) ||
        (p->bar[1]&15) || !(p->bar[1]&~15u) ||
        ((p->bar[1]&~15u)&(SIS_MMIO_BYTES-1)) ||
        (p->bar[2]&3)!=1 || !(p->bar[2]&~3u) || (p->bar[2]&~3u)>0xff80) return false;
    const struct display_match *m=generation==SIS_300 ? matches300 : matches315;
    unsigned n=generation==SIS_300 ? ARRAY_SIZE(matches300) : ARRAY_SIZE(matches315);
    for (unsigned i=0;i<n;i++) if (p->device==m[i].device) return true;
    return false;
}
static bool probe300(const struct pci_func *p) { return sis_pci_supported(p,SIS_300); }
static bool probe315(const struct pci_func *p) { return sis_pci_supported(p,SIS_315); }
static struct sis_engine engine={.cpu_safe=true,.reason="pending"};
static const struct pci_func *gpu;
static bool authorized, marker_owned, blocked;
static int reg_claim=-1, port_claim=-1, scratch_claim=-1;
static gen_t reg_gen, port_gen, scratch_gen;
static void *reg_window, *cursor_window;
static volatile uint8_t *registers;
static uint8_t *cursor_mapping;
static uint16_t io_base;
static uint32_t claim_cursor_phys, claim_scratch_end;
static uint64_t record_tick, record_operations;
static bool record_seen, record_active, record_quarantined;
static const char *record_reason;
static int runtime_stop(void *);
static const struct accel_ops ops={
    .ctx=&engine,.sync=sis_sync,.copy_rect=sis_copy,.fill_rect=sis_fill,
    .pattern_rect=sis_pattern,.cursor_set=sis_cursor_set,.cursor_move=sis_cursor_move,
    .cursor_show=sis_cursor_show,.stop=runtime_stop,.capabilities=sis_capabilities,
    .cpu_ready=sis_cpu_ready,.set_pause=sis_pause
};
static bool safe_extent(uint32_t base,uint32_t size)
{
    if (base<0x100000 || !size || size>UINT32_MAX-base) return false;
    uint64_t end=(uint64_t)base+size;
    if (base<g_boot.kernel_end && end>g_boot.kernel_start) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *e=&g_boot.e820[i];
        if (e->length>UINT64_MAX-e->base) return false;
        if (e->type==CBI_E820_RAM && base<e->base+e->length && end>e->base) return false;
    }
    return true;
}
static bool eligible(const struct fb_device *d)
{
    const char *why=0;
    if (display_accel_disabled(&why)) { engine.reason=why; return false; }
    if (blocked || engine.quarantined) return false;
    const struct display_driver *const table[]={&sis300_driver,&sis315_driver};
    const struct pci_func *p=0;
    const struct display_driver *driver=display_driver_select(d,table,ARRAY_SIZE(table),&p);
    if (!driver || !sis_mode_supported(d) || d->width<16 || d->height<16) {
        engine.reason="unsupported_mode_or_owner"; return false;
    }
    uint32_t mmio=p->bar[1]&~15u, vram=g_boot.display.vram_bytes;
    if (!safe_extent(mmio,SIS_MMIO_BYTES) ||
        ((uint64_t)mmio< (uint64_t)d->phys+vram && (uint64_t)mmio+SIS_MMIO_BYTES>d->phys) ||
        !safe_extent(d->phys,vram)) {
        engine.reason="invalid_aperture"; return false;
    }
    /* Exact firmware VRAM extent; reserve a disjoint page for the cursor
     * and, on 315, a 512 KiB local MMIO command queue. No RAM reservations. */
    uint32_t queue=driver==&sis315_driver ? SIS_QUEUE_BYTES : 0;
    if (vram<queue+SIS_CURSOR_BYTES) { engine.reason="insufficient_vram"; return false; }
    uint32_t end=driver==&sis315_driver ? (vram-queue)&~(SIS_QUEUE_BYTES-1) : vram&~4095u;
    if (end<SIS_CURSOR_BYTES || end-SIS_CURSOR_BYTES<d->size ||
        ((end-SIS_CURSOR_BYTES)>>10)>(driver==&sis300_driver ? 0xffffu : 0xfffffu) || d->phys>UINT32_MAX-vram) {
        engine.reason="insufficient_vram"; return false;
    }
    gpu=p; engine.generation=driver==&sis300_driver ? SIS_300 : SIS_315;
    engine.cursor_offset=end-SIS_CURSOR_BYTES; engine.queue_offset=end;
    claim_cursor_phys=d->phys+engine.cursor_offset;
    claim_scratch_end=d->phys+end+queue;
    return true;
}
static uint32_t hw_read(void *ctx,unsigned reg,unsigned width)
{
    (void)ctx;
    if (width==16) return *(volatile uint16_t *)(registers+reg);
    return *(volatile uint32_t *)(registers+reg);
}
static void hw_write(void *ctx,unsigned reg,unsigned width,uint32_t value)
{
    (void)ctx;
    if (width==16) *(volatile uint16_t *)(registers+reg)=(uint16_t)value;
    else *(volatile uint32_t *)(registers+reg)=value;
}
static uint8_t hw_index_read(void *ctx,bool cr,uint8_t index)
{
    (void)ctx; uint16_t port=io_base+(cr ? 0x54 : 0x44);
    uint8_t saved=inb(port); outb(port,index); uint8_t value=inb(port+1); outb(port,saved);
    return value;
}
static void hw_index_write(void *ctx,bool cr,uint8_t index,uint8_t value)
{
    (void)ctx; uint16_t port=io_base+(cr ? 0x54 : 0x44);
    uint8_t saved=inb(port); outb(port,index); outb(port+1,value); outb(port,saved);
}
static uint64_t hw_now(void *ctx)
{
    (void)ctx;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000;
    uint64_t c=ktime_cycles();
    return c/g_tsc_per_ms*1000+c%g_tsc_per_ms*1000/g_tsc_per_ms;
}
static void hw_drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static bool release_proof(int handle,gen_t gen)
{
    (void)gen;
    if (!engine.cpu_safe || engine.active || engine.configured) return false;
    if (handle==reg_claim && registers) { vmm_revoke_mmio((void *)registers,SIS_MMIO_BYTES); registers=0; }
    if (handle==scratch_claim && cursor_mapping) { vmm_revoke_mmio(cursor_mapping,SIS_CURSOR_BYTES); cursor_mapping=0; engine.cursor=0; }
    return true;
}
static void release_claim(int *handle,gen_t gen)
{
    if (*handle<0) return;
    if (!registry_quiesce(*handle,gen,release_proof) && !registry_release(*handle,gen)) *handle=-1;
}
static void cleanup(void)
{
    if (engine.active || !engine.cpu_safe || engine.configured) return;
    release_claim(&scratch_claim,scratch_gen); release_claim(&reg_claim,reg_gen); release_claim(&port_claim,port_gen);
}
static int runtime_stop(void *ctx)
{
    int e=sis_stop(ctx);
    if (e) return e;
    /* The mode service must not enter firmware with native aliases or
     * claims still live. Resource proofs/revocation do no VFS or ATA I/O. */
    cleanup();
    if (scratch_claim>=0 || reg_claim>=0 || port_claim>=0) {
        sis_quarantine(&engine,"resource_release_failed"); return -EIO;
    }
    return 0;
}
static bool acquire(int *handle,gen_t *gen,enum res_type type,uint32_t start,uint32_t end,const char *name)
{
    *handle=registry_claim(type,start,end,name,false);
    if (*handle<0) return false;
    const struct resource *r=registry_get((unsigned)*handle);
    if (!r) return false;
    *gen=r->generation;
    return !registry_activate(*handle,*gen);
}
/* This worker callback always runs under fbdev's exclusive pixel claim. */
static int activation_probe(void *ctx)
{
    struct sis_engine *s=ctx;
    int e=sis_engine_start(s);
    if (e) {
        if (s->touched && s->cpu_safe) (void)sis_stop(s);
        return e;
    }
    uint8_t saved[16*16*4], reference[16*16*4];
    struct fb_device ref=s->fb;
    ref.width=ref.height=16; ref.pitch=16*(ref.bpp/8); ref.size=16*ref.pitch; ref.mapped=reference;
    for (unsigned y=0;y<16;y++) memcpy(saved+y*ref.pitch,s->fb.mapped+y*s->fb.pitch,ref.pitch);
    memcpy(reference,saved,ref.size);
    struct fb_rect r={0,0,16,16}, seed={0,0,4,4}, source={0,0,12,12};
    uint32_t fg=swdib_pack(&ref,0x36a5e7), bg=swdib_pack(&ref,0xcb7123);
    const uint8_t pattern[8]={0x81,0x42,0x24,0x18,0x18,0x24,0x42,0x81};
    for (unsigned step=0;step<5 && !e;step++) {
        s->io.drain(s->io.ctx);
        if (!step) { swdib_fill(&ref,&r,fg); e=sis_fill(s,&r,fg); }
        else if (step==1) { swdib_fill(&ref,&seed,bg); e=sis_fill(s,&seed,bg); }
        else if (step==2) { swdib_copy(&ref,&source,4,4); e=sis_copy(s,&source,4,4); }
        else if (step==3) { source.x=source.y=4; swdib_copy(&ref,&source,0,0); e=sis_copy(s,&source,0,0); }
        else { swdib_pattern(&ref,&r,pattern,fg,bg); e=sis_pattern(s,&r,pattern,fg,bg); }
        if (!e) for (unsigned y=0;y<16;y++) if (memcmp(reference+y*ref.pitch,s->fb.mapped+y*s->fb.pitch,ref.pitch)) { e=-EIO; break; }
    }
    if (!sis_sync(s) && s->cpu_safe) {
        for (unsigned y=0;y<16;y++) memcpy(s->fb.mapped+y*s->fb.pitch,saved+y*ref.pitch,ref.pitch);
        s->io.drain(s->io.ctx);
    } else e=-EIO;
    if (!e && s->cursor_ok) {
        const struct accel_cursor transparent={0};
        e=sis_cursor_set(s,&transparent);
        if (!e) e=sis_cursor_move(s,0,0);
        if (!e) e=sis_cursor_show(s,true);
        if (!sis_cursor_hide(s) && !e) e=-EIO;
    }
    if (e) {
        sis_quarantine(s,"native_comparison_failed");
        if (s->cpu_safe) (void)sis_stop(s);
    }
    return e;
}
static const struct accel_ops *fallback(void)
{
    if (engine.quarantined && !engine.cpu_safe && engine.io.read) (void)sis_sync(&engine);
    return engine.cpu_safe ? 0 : &ops;
}
static const struct accel_ops *start_with_io(const struct fb_device *d,const struct sis_io *io)
{
    if (!authorized || !eligible(d)) return 0;
    authorized=false; /* Consume one persisted authorization. */
    engine.fb=*d; engine.io=*io; engine.cursor=cursor_mapping;
    engine.cursor_ok=cursor_mapping!=0;
    if (fbdev_accel_probe(d,activation_probe,&engine)) {
        blocked=true;
        if (!engine.reason || !strncmp(engine.reason,"pending",sizeof("pending"))) engine.reason="start_failed";
        return 0;
    }
    return &ops;
}
static bool map_resources(void)
{
    if (reg_claim>=0 || port_claim>=0 || scratch_claim>=0) {
        engine.reason="resource_lease_retained"; return false;
    }
    io_base=gpu->bar[2]&~3u;
    if (!acquire(&port_claim,&port_gen,RES_PORT,io_base,io_base+0x80,"sis-ports") ||
        !acquire(&reg_claim,&reg_gen,RES_MMIO,gpu->bar[1]&~15u,(gpu->bar[1]&~15u)+SIS_MMIO_BYTES,"sis-registers") ||
        !acquire(&scratch_claim,&scratch_gen,RES_MMIO,claim_cursor_phys,claim_scratch_end,"sis-scratch")) {
        engine.reason="resource_conflict"; cleanup(); return false;
    }
    if (!reg_window) reg_window=vmm_reserve_mmio(SIS_MMIO_BYTES);
    if (!cursor_window) cursor_window=vmm_reserve_mmio(SIS_CURSOR_BYTES);
    registers=reg_window ? vmm_replace_mmio(reg_window,SIS_MMIO_BYTES,gpu->bar[1]&~15u,SIS_MMIO_BYTES) : 0;
    cursor_mapping=cursor_window ? vmm_replace_mmio(cursor_window,SIS_CURSOR_BYTES,claim_cursor_phys,SIS_CURSOR_BYTES) : 0;
    if (!registers || !cursor_mapping) {
        engine.reason="mapping_failed"; cleanup(); return false;
    }
    return true;
}
static const struct accel_ops *start(const struct fb_device *d,struct vfs *vfs)
{
    (void)vfs;
    if (!authorized || !eligible(d)) return 0;
    cleanup();
    if (!map_resources()) { authorized=false; blocked=true; return 0; }
    const struct sis_io io={0,hw_read,hw_write,hw_index_read,hw_index_write,hw_now,hw_drain};
    return start_with_io(d,&io);
}
static const char *marker_path(void)
{ return engine.generation==SIS_300 ? "C:/system/settings/sis300.cfg" : "C:/system/settings/sis315.cfg"; }
static struct vfs_table marker_table;
static int marker(struct vfs *vfs,bool clear)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    int e=vfs_table_init(vfs,&marker_table,2); if (e) return e;
    if (!clear) {
        const char *paths[]={"C:/system/settings/accel.cfg",marker_path()};
        for (unsigned i=0;i<ARRAY_SIZE(paths);i++) {
            struct fat_entry entry;
            e=vfs_stat(&marker_table,paths[i],&entry);
            if (!e && entry.size) { e=-EIO; goto out; }
            if (e && e!=-FS_ENOENT) goto out;
        }
    }
    if (vfs->volumes[2]->readonly) { e=-FS_EROFS; goto out; }
    int fd=vfs_open(&marker_table,marker_path(),VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { e=fd; goto out; }
    if (clear) e=vfs_truncate(&marker_table,fd,0);
    else {
        char text[80];
        int n=ksnprintf(text,sizeof(text),"driver=sis%u\npci=%04x:%04x\nstate=attempt\n",engine.generation==SIS_300 ? 300u : 315u,gpu->vendor,gpu->device);
        size_t done=0;
        e=n<0 || (unsigned)n>=sizeof(text) ? -FS_EIO : vfs_write(&marker_table,fd,text,(size_t)n,&done);
        if (!e && done!=(size_t)n) e=-FS_EIO;
    }
    if (!e) e=vfs_commit(&marker_table,fd);
    int closed=vfs_close(&marker_table,fd); if (!e) e=closed;
out:
    vfs_table_destroy(&marker_table);
    return e;
}
static int attempt_begin(struct vfs *vfs)
{
    if (!eligible(fbdev_get()) || authorized || marker_owned) return -EIO;
    int e=marker(vfs,false);
    if (e) { blocked=true; engine.reason="marker_unavailable_or_retained"; return e; }
    authorized=marker_owned=true;
    return 0;
}
static int attempt_end(struct vfs *vfs)
{
    if (!marker_owned || !engine.active || !engine.cpu_safe || engine.quarantined) return -EIO;
    int e=marker(vfs,true); if (!e) marker_owned=false;
    return e;
}
static int attempt_failed(struct vfs *vfs)
{ (void)vfs; authorized=false; blocked=true; return 0; }
static void quarantine(const char *why) { blocked=true; sis_quarantine(&engine,why); }
static bool summary(bool *ready,bool *used,const char **reason)
{ *ready=engine.active && !engine.quarantined; *used=false; *reason=engine.reason; return true; }
static void snapshot(struct ciuki_accel_info *out)
{
    memset(out,0,sizeof(*out)); out->driver=CIUKI_ACCEL_DRIVER_LEGACY_NATIVE;
    out->capabilities=sis_capabilities(&engine); out->quarantined=engine.quarantined;
    out->generation=engine.fb.generation; out->operations=engine.operations; out->idle_waits=engine.waits;
}
static void record(void)
{
    if (!record_seen || record_active!=engine.active || record_quarantined!=engine.quarantined ||
        record_reason!=engine.reason || (record_operations!=engine.operations && g_ticks-record_tick>=1000)) {
        klog("[accel] driver=sis%u ready=%u quarantined=%u cpu_ready=%u operations=%llu reason=%s",
            engine.generation==SIS_300 ? 300u : 315u,engine.active,engine.quarantined,engine.cpu_safe,engine.operations,engine.reason);
        record_seen=true; record_active=engine.active; record_quarantined=engine.quarantined;
        record_reason=engine.reason; record_tick=g_ticks; record_operations=engine.operations;
    }
    cleanup();
    if (engine.quarantined) {
        if (reg_claim>=0) registry_quarantine(reg_claim,reg_gen);
        if (scratch_claim>=0) registry_quarantine(scratch_claim,scratch_gen);
        if (port_claim>=0) registry_quarantine(port_claim,port_gen);
    }
}
static int cursor_test(const struct accel_ops *a,uint64_t deadline,bool (*cancel)(void *),void *ctx)
{
    if (a!=&ops || !engine.cursor_ok) return -EOPNOTSUPP;
    struct accel_cursor transparent={0};
    int e=0;
    for (unsigned i=0;i<4 && !e;i++) {
        if (g_ticks>=deadline || (cancel && cancel(ctx))) { e=g_ticks>=deadline ? -ETIMEDOUT : -EINTR; break; }
        if (!i) e=sis_cursor_show(&engine,false);
        else if (i==1) e=sis_cursor_set(&engine,&transparent);
        else if (i==2) e=sis_cursor_move(&engine,0,0);
        else e=sis_cursor_show(&engine,true);
    }
    if (!sis_cursor_hide(&engine)) { sis_quarantine(&engine,"cursor_disable_failed"); if (!e) e=-EIO; }
    return e;
}
#define SIS_DRIVER(id_,name_,matches_,probe_,marker_) { \
    .id=id_,.attempt_files={"C:/system/settings/accel.cfg",marker_},.name=name_,.matches=matches_,.match_count=ARRAY_SIZE(matches_),.framebuffer_bar=0, \
    .probe=probe_,.eligible=eligible,.start=start,.fallback=fallback, \
    .attempt_begin=attempt_begin,.attempt_end=attempt_end,.attempt_failed=attempt_failed, \
    .record=record,.quarantine=quarantine,.summary=summary,.snapshot=snapshot,.cursor_test=cursor_test }
const struct display_driver sis300_driver=SIS_DRIVER("sis300","SiS 300 / 540 / 630",matches300,probe300,"C:/system/settings/sis300.cfg");
const struct display_driver sis315_driver=SIS_DRIVER("sis315","SiS 315 / 330",matches315,probe315,"C:/system/settings/sis315.cfg");
