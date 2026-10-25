/* Native registry/lifecycle adaptation. SPDX-License-Identifier: GPL-2.0-only
 * Register fragments and their donor notices live in hardware.c/engine.c.
 * Transport approval is intentionally explicit, never a build variant. */
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/timing.h>
#include "../../../fs/vfs.h"
#include "nv2d.h"
#include "matches.h"

#include "../render3d.h"
static const struct render3d_family render_family = {
    .status=R3D_NOT_IMPLEMENTED, .potential_units=4, .potential_texture_size=4096
};

/* f2-104 amendment 1 approves NV4-NV2x PIO; f2-105 amendment 1
 * approves the NV30/NV40 VRAM ring. DM9 remains mandatory for every
 * enabled transport, before publishing native ops. */
static const bool transport_reviewed=true;
static const bool ring_transport_reviewed=true;
static struct nv2d engine;
static volatile uint8_t *registers, *tail_mapping;
static void *register_window, *tail_window;
static int register_claim=-1, tail_claim=-1;
static gen_t register_generation, tail_generation;
static uint32_t register_phys, tail_phys, tail_bytes;
static struct fb_device activation_fb;
static bool persisted, blocked, diagnostic_marker;
static struct vfs *diagnostic_vfs;
static struct vfs_table marker_table;
static const char *declined="pending";
static bool report_pending;

static bool probe(const struct pci_func *p)
{
    if (!p || p->vendor!=0x10de || p->class_code!=3 || p->subclass ||
        (p->header_type&0x7f) || !(p->command&2) ||
        (p->bar[0]&15) || (p->bar[1]&7) ||
        !(p->bar[0]&~15u) || !(p->bar[1]&~15u) ||
        (p->bar[0]&~15u)==(p->bar[1]&~15u)) return false;
    for (unsigned i=0;i<ARRAY_SIZE(nv2d_matches);i++)
        if (p->device==nv2d_matches[i].device) return true;
    return false;
}
static bool safe_extent(uint32_t start, uint32_t bytes)
{
    uint64_t end=(uint64_t)start+bytes;
    if (start<0x100000 || !bytes || end>UINT32_MAX ||
        (start<g_boot.kernel_end && end>g_boot.kernel_start)) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *e=&g_boot.e820[i];
        uint64_t limit=e->base+e->length;
        if (limit<e->base) return false;
        if (e->type==CBI_E820_RAM && start<limit && end>e->base) return false;
    }
    return true;
}
static bool owns(const struct fb_device *f)
{
    const struct pci_func *p=display_driver_owner();
    unsigned displays=0;
    for (unsigned i=0;i<pci_count();i++) {
        const struct pci_func *candidate=pci_get(i);
        if (candidate && candidate->class_code==3) displays++;
    }
    if (displays!=1 || !f || !probe(p) || (p->bar[1]&~15u)!=f->phys ||
        !nv2d_format(f) || !g_boot.display.vram_bytes || f->size>g_boot.display.vram_bytes ||
        !safe_extent(f->phys,g_boot.display.vram_bytes) ||
        !safe_extent(p->bar[0]&~15u,NV2D_MMIO_BYTES) ||
        ((uint64_t)(p->bar[0]&~15u)<(uint64_t)f->phys+g_boot.display.vram_bytes &&
         (uint64_t)f->phys<(uint64_t)(p->bar[0]&~15u)+NV2D_MMIO_BYTES) ||
        f->handle<0 || !registry_valid(f->handle,f->generation)) return false;
    const struct resource *r=registry_get((unsigned)f->handle);
    return r && r->type==RES_MMIO && r->state==RS_ACTIVE && r->owner &&
        !strncmp(r->owner,"fbdev",sizeof("fbdev")) &&
        r->start==f->phys && r->end==(uint64_t)f->phys+f->size;
}
static bool eligible(const struct fb_device *f)
{
    const char *reason;
    if (display_accel_disabled(&reason)) declined=reason;
    else if (blocked || engine.quarantined) declined="session_quarantined";
    else if (!owns(f)) declined="ownership_or_format";
    else if (!transport_reviewed) declined="transport_review_required";
    else {
        const struct pci_func *p=display_driver_owner();
        if (p->device==0x01a0 || p->device==0x01f0 || p->device==0x00a0 ||
            (p->device&0xfff0)==0x0240)
            declined="uma_ownership_unproved";
        else if (!ring_transport_reviewed &&
                 (nv2d_architecture(p->device)>=0x30 || (p->device&0xfff0)==0x00f0))
            declined="ring_transport_review_required";
        else return true;
    }
    report_pending=true;
    return false;
}
static bool io_valid(void *ctx)
{
    (void)ctx;
    const struct fb_device *f=fbdev_get();
    const struct resource *r=register_claim>=0 ? registry_get((unsigned)register_claim) : NULL;
    const struct resource *t=tail_claim>=0 ? registry_get((unsigned)tail_claim) : NULL;
    return registers && tail_mapping && r && t &&
        r->start==register_phys && r->end==(uint64_t)register_phys+NV2D_MMIO_BYTES &&
        t->start==tail_phys && t->end==(uint64_t)tail_phys+tail_bytes &&
        f->present && f->phys==activation_fb.phys &&
        f->generation==activation_fb.generation &&
        registry_valid(activation_fb.handle,activation_fb.generation) &&
        registry_valid(register_claim,register_generation) &&
        registry_valid(tail_claim,tail_generation);
}
static uint32_t read32(void *ctx, uint32_t reg)
{ (void)ctx; return *(volatile uint32_t *)(registers+reg); }
static void write32(void *ctx, uint32_t reg, uint32_t value)
{ (void)ctx; *(volatile uint32_t *)(registers+reg)=value; }
static uint8_t read8(void *ctx, uint32_t reg)
{ (void)ctx; return registers[reg]; }
static void write8(void *ctx, uint32_t reg, uint8_t value)
{ (void)ctx; registers[reg]=value; }
static uint64_t now_us(void *ctx)
{
    (void)ctx;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000;
    uint64_t cycles=ktime_cycles();
    return cycles/g_tsc_per_ms*1000+(cycles%g_tsc_per_ms)*1000/g_tsc_per_ms;
}
static void delay_us(void *ctx, unsigned us)
{
    (void)ctx;
    /* udelay is itself bounded; the outer wait has a clock and poll cap. */
    (void)udelay(us);
}
static void drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static const struct nv2d_io io={
    .read=read32,.write=write32,.read8=read8,.write8=write8,
    .now=now_us,.delay=delay_us,.drain=drain,.valid=io_valid
};

/* Claims stay pinned for the boot. A mode change may reuse the same exact
 * BAR0 and tail only after stop proves idle. Never release an unsafe writer
 * or create a new reservation per activation. */
static int map_claim(uint32_t phys, uint32_t bytes, const char *name,
                     int *claim, gen_t *generation, void **window,
                     volatile uint8_t **mapping)
{
    if (*claim>=0) {
        const struct resource *r=registry_get((unsigned)*claim);
        return registry_valid(*claim,*generation) && r && r->start==phys &&
            r->end==(uint64_t)phys+bytes && *mapping ? 0 : -EIO;
    }
    *claim=registry_claim(RES_MMIO,phys,phys+bytes,name,false);
    if (*claim<0) return -EBUSY;
    *generation=registry_get((unsigned)*claim)->generation;
    if (!*window) *window=vmm_reserve_mmio(bytes);
    *mapping=*window ? vmm_replace_mmio(*window,bytes,phys,bytes) : NULL;
    if (!*mapping || registry_activate(*claim,*generation)) {
        registry_quarantine(*claim,*generation); *mapping=NULL; return -EIO;
    }
    return 0;
}
static int activate_probe(void *ctx)
{
    uint32_t vram=*(uint32_t *)ctx;
    int e=nv2d_engine_init(&engine,&io,&activation_fb,display_driver_owner()->device,vram,tail_mapping);
    if (e) return e;
    return nv2d_compare(&engine);
}
static const struct accel_ops *start(const struct fb_device *f, struct vfs *vfs)
{
    (void)vfs;
    /* Enforce policy even if a caller bypassed eligibility/marker hooks. */
    if (!persisted || !eligible(f) || f->width<16 || f->height<16 || engine.active ||
        (engine.touched && !engine.cpu_safe)) return NULL;
    activation_fb=*f;
    register_phys=display_driver_owner()->bar[0]&~15u;
    if (map_claim(register_phys,NV2D_MMIO_BYTES,"nv2d-registers",&register_claim,
                  &register_generation,&register_window,&registers)) { declined="register_claim_failed"; return NULL; }
    uint16_t chip=nv2d_chipset(&io,display_driver_owner()->device);
    unsigned arch=nv2d_architecture(chip);
    if (!arch || (chip&0xfff0)==0x0240) { declined="chipset_unproved"; return NULL; }
    uint32_t vram=nv2d_vram(&io,arch);
    /* NV40 instance storage is at physical chip VRAM's top. Taking a smaller
     * firmware aperture would reserve the wrong address for that storage. */
    if (arch>=0x40 && vram!=g_boot.display.vram_bytes) {
        declined="instance_extent_unproved"; return NULL;
    }
    if (vram>g_boot.display.vram_bytes) vram=g_boot.display.vram_bytes;
    tail_bytes=nv2d_tail_bytes(arch);
    vram&=~(tail_bytes-1);
    if (vram<tail_bytes || f->size>vram-tail_bytes) {
        declined="vram_reservation_unavailable"; return NULL;
    }
    tail_phys=f->phys+vram-tail_bytes;
    if (!safe_extent(tail_phys,tail_bytes) ||
        map_claim(tail_phys,tail_bytes,"nv2d-vram",&tail_claim,
                  &tail_generation,&tail_window,&tail_mapping)) { declined="vram_claim_failed"; return NULL; }
    int e=fbdev_accel_probe(f,activate_probe,&vram);
    report_pending=true;
    if (e) { declined=engine.reason ? engine.reason : "activation_failed"; return NULL; }
    return &engine.ops;
}
static const struct accel_ops *fallback(void)
{
    if (!engine.touched) return NULL;
    /* Failure retains this nonwaiting guard. No reset is an idle proof. */
    if (nv2d_idle(&engine)) engine.cpu_safe=false;
    return &engine.ops;
}
static void quarantine(const char *reason)
{ blocked=true; report_pending=true; nv2d_quarantine(&engine,reason); }
static bool summary(bool *ready, bool *used, const char **reason)
{
    *ready=engine.active && !engine.quarantined; *used=false;
    *reason=engine.quarantined ? engine.reason : *ready ? "none" : declined;
    return true;
}
static void snapshot(struct ciuki_accel_info *out)
{
    *out=(struct ciuki_accel_info){.driver=CIUKI_ACCEL_DRIVER_LEGACY_NATIVE,
        .capabilities=engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0,
        .quarantined=engine.quarantined,.generation=activation_fb.generation,
        .operations=engine.operations,.idle_waits=engine.waits};
}
static void record(void)
{
    if (!report_pending && !engine.quarantined) return;
    report_pending=false;
    klog("[accel] driver=nv2d ready=%u quarantined=%u cpu_ready=%u reason=%s",
        engine.active,engine.quarantined,engine.cpu_safe,
        engine.quarantined ? engine.reason : engine.active ? "none" : declined);
}

/* A driver+PCI-specific record cannot clear the frozen legacy marker. */
static int marker(struct vfs *vfs, bool clear, bool readonly, bool *previous)
{
    const struct pci_func *p=display_driver_owner();
    if (previous) *previous=false;
    if (!p || !vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    char path[64], value[64];
    ksnprintf(path,sizeof(path),"C:/system/settings/nv2d%04x.cfg",p->device);
    ksnprintf(value,sizeof(value),"driver=nv2d\npci=10de:%04x\nstate=attempt\n",p->device);
    int e=vfs_table_init(vfs,&marker_table,2);
    if (e) return e;
    struct fat_entry entry;
    e=vfs_stat(&marker_table,path,&entry);
    if (!e && entry.size && !clear) { if (previous) *previous=true; goto done; }
    if (e && e!=-FS_ENOENT) goto done;
    e=0;
    if (readonly) goto done;
    if (vfs->volumes[2]->readonly) { e=-FS_EROFS; goto done; }
    int fd=vfs_open(&marker_table,path,VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { e=fd; goto done; }
    if (clear) e=vfs_truncate(&marker_table,fd,0);
    else {
        size_t wrote=0, bytes=strlen(value);
        e=vfs_write(&marker_table,fd,value,bytes,&wrote);
        if (!e && wrote!=bytes) e=-FS_EIO;
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
    if (!eligible(fbdev_get())) return -EOPNOTSUPP;
    bool previous=false;
    int e=marker(vfs,false,false,&previous);
    if (e || previous) {
        blocked=true; declined=previous ? "previous_attempt" : "marker_unavailable";
        return e ? e : -EIO;
    }
    persisted=true; return 0;
}
static int attempt_end(struct vfs *vfs)
{
    if (blocked || engine.quarantined || diagnostic_marker) return -EIO;
    if (!persisted) return 0; /* outer H-page cleanup after diagnostic clear */
    int e=marker(vfs,true,false,NULL);
    if (!e) persisted=false;
    return e;
}
static int attempt_failed(struct vfs *vfs)
{
    (void)vfs;
    blocked=true; /* Retain the committed marker, without a destructive rewrite. */
    return 0;
}
static int cursor_test(const struct accel_ops *ops, uint64_t deadline,
                       bool (*cancel)(void *), void *ctx)
{
    struct accel_cursor cursor={0};
    int e=0;
    for (unsigned step=0;step<4 && !e;step++) {
        if (g_ticks>=deadline || (cancel && cancel(ctx))) { e=g_ticks>=deadline ? -ETIMEDOUT : -EINTR; break; }
        if (!step) e=ops->cursor_show(ops->ctx,false);
        else if (step==1) e=ops->cursor_set(ops->ctx,&cursor);
        else if (step==2) e=ops->cursor_move(ops->ctx,-1,-1);
        else e=ops->cursor_show(ops->ctx,true);
    }
    int hidden=ops->cursor_show(ops->ctx,false);
    return e ? e : hidden;
}
static int check_begin(struct vfs *vfs, bool *retained)
{
    if (!retained || !(g_boot.flags&CBI_F_HW_CHECK)) return -EPERM;
    *retained=false;
    if (!eligible(fbdev_get())) return -EOPNOTSUPP;
    int e=marker(vfs,false,true,retained);
    if (e || *retained) { diagnostic_marker=*retained; return e ? e : -EIO; }
    diagnostic_vfs=vfs; return 0;
}
static int selftest(uint64_t deadline, void (*line)(void *, const char *), void *ctx,
                    struct display_selftest_result *out)
{
    if (!out) return -EINVAL;
    *out=(struct display_selftest_result){.first=-1,.restored=true};
    uint32_t tested=0;
    int e=diagnostic_vfs ? fbdev_accel_diagnostic(diagnostic_vfs,deadline,NULL,NULL,&tested) : -EPERM;
    out->error=e; out->tried=4;
    for (unsigned i=0;i<4;i++) if (tested&(1u<<i)) out->passed++;
    out->completed=out->passed; out->timeouts=e==-ETIMEDOUT;
    out->first=out->passed ? 0 : -1;
    out->restored=!engine.touched || engine.cpu_safe;
    if (line) line(ctx,e ? "Native comparison failed/refused; no software replay" : "Native FILL/COPY/PATTERN/WRITE and cursor readback passed");
    return e;
}
const struct display_driver nv2d_driver={
    .render3d=&render_family,
    .id="nv2d",.attempt_files={"C:/system/settings/nv2d%04x.cfg"},.name="NVIDIA TNT-GeForce6",.matches=nv2d_matches,
    .match_count=ARRAY_SIZE(nv2d_matches),.framebuffer_bar=1,.probe=probe,.eligible=eligible,
    .start=start,.fallback=fallback,.attempt_begin=attempt_begin,.attempt_end=attempt_end,
    .attempt_failed=attempt_failed,.record=record,.quarantine=quarantine,.summary=summary,
    .snapshot=snapshot,.check_begin=check_begin,.selftest=selftest,.cursor_test=cursor_test
};
