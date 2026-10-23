/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright 1998-2009 VIA Technologies, Inc. All Rights Reserved.
 * Copyright 2001-2008 S3 Graphics, Inc. All Rights Reserved.
 * Copyright 2009 Jonathan Corbet <corbet@lwn.net>
 * Origin: Linux v6.12 drivers/video/fbdev/via/{via-core.c,chip.h,hw.h}:
 * device IDs, BAR selection and firmware UMA-size decoding.
 * CiukiOS registry/resource/marker glue; no Linux compatibility layer.
 */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/timing.h>
#include "../../../fs/vfs.h"
#include "engine.h"
#include "regs.h"

static const struct display_match matches[]={
    {0x1106,0x3122,3,255}, {0x1106,0x7205,3,255},
    {0x1106,0x3108,3,255}, {0x1106,0x3118,3,255}
};
static struct uc_engine engine;
static volatile uint8_t *registers;
static void *register_window, *scratch_window;
static uint8_t *scratch_mapping;
static int register_claim=-1, scratch_claim=-1;
static gen_t register_gen, scratch_gen;
static int (*engine_stop)(void *);
static bool blocked, marker_persisted, marker_retained;
static const char *decision="pending";
static struct vfs_table marker_table;
#define MARKER_PATH "C:/system/settings/via2d.cfg"
#define LEGACY_PATH "C:/system/settings/accel.cfg"

bool uc_pci_supported(const struct pci_func *p)
{
    if (!p || p->vendor!=0x1106 || p->class_code!=3 || p->subclass ||
        (p->header_type&127u) || !(p->command&2u)) return false;
    for (unsigned i=0;i<ARRAY_SIZE(matches);i++)
        if (p->device==matches[i].device) return true;
    return false;
}
static const struct pci_func *owner_of(const struct fb_device *fb)
{
    const struct display_driver *table[]={&unichrome_driver};
    const struct pci_func *p=0;
    return display_driver_select(fb,table,1,&p) ? p : 0;
}
static bool physical(uint32_t start, uint32_t bytes, const struct fb_device *fb)
{
    if (start<0x100000 || !bytes || bytes>UINT32_MAX-start) return false;
    uint64_t end=(uint64_t)start+bytes;
    if ((start<g_boot.kernel_end && end>g_boot.kernel_start) ||
        (start<(uint64_t)fb->phys+fb->size && end>fb->phys)) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *r=&g_boot.e820[i];
        if (r->type==CBI_E820_RAM && start<r->base+r->length && end>r->base) return false;
    }
    return true;
}
static bool eligible(const struct fb_device *fb)
{
    const char *why;
    if (display_accel_disabled(&why)) { decision=why; return false; }
    if (blocked || engine.quarantined) { decision="session_quarantined"; return false; }
    const struct pci_func *p=owner_of(fb);
    if (!p) { decision="ownership_unproved"; return false; }
    uint32_t base=p->bar[1]&~15u;
    uint64_t aperture_end=(uint64_t)fb->phys+g_boot.display.vram_bytes;
    if ((p->bar[1]&15u) || (base&(UC_MMIO_BYTES-1)) ||
        aperture_end>UINT32_MAX ||
        (base<aperture_end && (uint64_t)base+UC_MMIO_BYTES>fb->phys) ||
        !physical(base,UC_MMIO_BYTES,fb)) { decision="register_bar_invalid"; return false; }
    if (!fb->mapped || fb->width<8 || fb->height<8 || fb->width>4096 || fb->height>4096 ||
        (fb->bpp!=16 && fb->bpp!=32) || !fb->pitch || (fb->pitch&0xffffc007u)) {
        decision="mode_unsupported"; return false;
    }
    return true;
}

static uint32_t cfg_read(const struct pci_func *p, unsigned reg)
{
#ifdef UC_HOST_TEST
    extern uint32_t uc_test_cfg(const struct pci_func *,unsigned);
    return uc_test_cfg(p,reg);
#else
    /* CF8 address selection is not a PCI configuration write. Preserve it
     * and serialize the pair against every other uniprocessor PCI reader. */
    uint32_t flags=irq_save();
    uint32_t previous=inl(0xcf8);
    outl(0xcf8,0x80000000u | (uint32_t)p->bus<<16 | (uint32_t)p->dev<<11 |
                (uint32_t)p->fn<<8 | (reg&0xfcu));
    uint32_t result=inl(0xcfc);
    outl(0xcf8,previous); irq_restore(flags);
    return result;
#endif
}
static uint32_t uma_size(const struct pci_func *gpu)
{
    uint16_t nb=gpu->device==0x3122 ? 0x3123 : gpu->device==0x7205 ? 0x3205 :
                gpu->device==0x3108 ? 0x3204 : 0x3259;
    const struct pci_func *found=0;
    for (unsigned i=0;i<pci_count();i++) {
        const struct pci_func *p=pci_get(i);
        if (p && p->vendor==0x1106 && p->device==nb && p->class_code==6 && p->fn==3) {
            if (found) return 0;
            found=p;
        }
    }
    if (!found) return 0;
    uint32_t raw=cfg_read(found,gpu->device==0x3122 || gpu->device==0x7205 ? 0xe0 : 0xa0);
    if (raw==UINT32_MAX) return 0;
    unsigned code=(raw>>12)&7u;
    return code>=4 && code<=6 ? (16u<<20)<<(code-4) : 0;
}
static uint32_t io_read(void *ctx, unsigned reg)
{
    (void)ctx;
#ifdef UC_HOST_TEST
    extern uint32_t uc_test_read(unsigned);
    return uc_test_read(reg);
#else
    return *(volatile uint32_t *)(registers+reg);
#endif
}
static void io_write(void *ctx, unsigned reg, uint32_t value)
{
    (void)ctx;
#ifdef UC_HOST_TEST
    extern void uc_test_write(unsigned,uint32_t);
    uc_test_write(reg,value);
#else
    *(volatile uint32_t *)(registers+reg)=value;
#endif
}
static uint8_t io_indexed(void *ctx, bool sequencer, uint8_t index)
{
    (void)ctx;
#ifdef UC_HOST_TEST
    extern uint8_t uc_test_indexed(bool,uint8_t);
    return uc_test_indexed(sequencer,index);
#else
    /* VGA MMIO aliases avoid claiming global VGA ports. The index is the
     * only write: preserve it so firmware/DOS readers see their old latch. */
    unsigned port=sequencer ? 0x83c4 : 0x83d4;
    uint32_t flags=irq_save();
    uint8_t old=registers[port]; registers[port]=index;
    uint8_t value=registers[port+1]; registers[port]=old;
    irq_restore(flags);
    return value;
#endif
}
static void io_indexed_write(void *ctx, bool sequencer, uint8_t index, uint8_t value)
{
    (void)ctx;
#ifdef UC_HOST_TEST
    extern void uc_test_indexed_write(bool,uint8_t,uint8_t);
    uc_test_indexed_write(sequencer,index,value);
#else
    unsigned port=sequencer ? 0x83c4 : 0x83d4;
    uint32_t flags=irq_save();
    uint8_t old=registers[port]; registers[port]=index;
    registers[port+1]=value; registers[port]=old;
    irq_restore(flags);
#endif
}
static uint64_t io_now(void *ctx)
{
    (void)ctx;
    if (g_cpu_tsc && g_tsc_per_ms) {
        uint64_t cycles=ktime_cycles();
        return cycles/g_tsc_per_ms*1000+(cycles%g_tsc_per_ms)*1000/g_tsc_per_ms;
    }
    return g_ticks*1000;
}
static void io_drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static bool io_valid(void *ctx)
{
    (void)ctx;
    return registers && registry_valid(register_claim,register_gen) &&
        (!scratch_mapping || registry_valid(scratch_claim,scratch_gen)) && engine.fb &&
        registry_valid(engine.fb->handle,engine.generation);
}
static const struct uc_io native_io={0,io_read,io_write,io_indexed,io_now,io_drain,io_valid,io_indexed_write};

static void release_claim(int *handle, gen_t generation)
{
    if (*handle>=0) (void)registry_release(*handle,generation);
    *handle=-1;
}
/* Called only before hardware was touched or by the authoritative successful
 * stop. An uncertain engine retains mappings, leases and its rejecting ops. */
static void release_resources(void)
{
    if (scratch_mapping) vmm_revoke_mmio(scratch_mapping,UC_SCRATCH_BYTES);
    scratch_mapping=0;
    release_claim(&scratch_claim,scratch_gen);
    if (registers) vmm_revoke_mmio((void *)registers,UC_MMIO_BYTES);
    registers=0;
    release_claim(&register_claim,register_gen);
}
static int claim(uint32_t base, uint32_t bytes, const char *name, gen_t *gen)
{
    int h=registry_claim(RES_MMIO,base,base+bytes,name,false);
    if (h<0) return h;
    const struct resource *r=registry_get((unsigned)h);
    if (!r) { *gen=0; return -EIO; }
    *gen=r->generation;
    int err=registry_activate(h,*gen);
    if (err) { (void)registry_release(h,*gen); return err; }
    return h;
}
static int stop_adapter(void *ctx)
{
    if (!registers && engine.cpu_safe) return 0;
    int err=engine_stop(ctx);
    if (!err) {
        release_resources();
        /* A subsequent mode may activate again; same-boot quarantine stays. */
        engine.fb=0; engine.scratch=0;
    }
    return err;
}
static const struct accel_ops *start(const struct fb_device *fb, struct vfs *vfs)
{
    (void)vfs; /* Marker I/O is exclusively in the worker hooks. */
    if (!eligible(fb) || !marker_persisted || marker_retained || registers) return 0;
    const struct pci_func *p=owner_of(fb);
    if (!p) return 0;
    uint32_t base=p->bar[1]&~15u;
    register_claim=claim(base,UC_MMIO_BYTES,"unichrome",&register_gen);
    if (register_claim<0) { decision="register_claim_failed"; return 0; }
    if (!register_window) register_window=vmm_reserve_mmio(UC_MMIO_BYTES);
    registers=register_window ? vmm_replace_mmio(register_window,UC_MMIO_BYTES,base,UC_MMIO_BYTES) : 0;
    if (!registers) { release_resources(); decision="register_map_failed"; return 0; }
    uint32_t size=uma_size(p), offset=0;
    if (fb->size<=UINT32_MAX-4095u) offset=(fb->size+4095u)&~4095u;
    uint64_t scratch_phys=(uint64_t)fb->phys+offset;
    if (offset && size && g_boot.display.vram_bytes &&
        offset<=size && UC_SCRATCH_BYTES<=size-offset &&
        offset<=g_boot.display.vram_bytes && UC_SCRATCH_BYTES<=g_boot.display.vram_bytes-offset &&
        scratch_phys<=UINT32_MAX && physical((uint32_t)scratch_phys,UC_SCRATCH_BYTES,fb) &&
        !((uint32_t)scratch_phys<base+UC_MMIO_BYTES && scratch_phys+UC_SCRATCH_BYTES>base)) {
        scratch_claim=claim((uint32_t)scratch_phys,UC_SCRATCH_BYTES,"unichrome-upload",&scratch_gen);
        if (scratch_claim>=0) {
            if (!scratch_window) scratch_window=vmm_reserve_mmio(UC_SCRATCH_BYTES);
            scratch_mapping=scratch_window ? vmm_replace_mmio(scratch_window,UC_SCRATCH_BYTES,
                (uint32_t)scratch_phys,UC_SCRATCH_BYTES) : 0;
            if (!scratch_mapping) release_claim(&scratch_claim,scratch_gen);
        }
    }
    int err=uc_engine_init(&engine,fb,&native_io,scratch_mapping,offset);
    engine_stop=engine.ops.stop;
    if (engine_stop) engine.ops.stop=stop_adapter;
    if (!err) err=fbdev_accel_probe(fb,uc_engine_probe,&engine);
    if (err) {
        blocked=true; decision=engine.reason ? engine.reason : "start_failed";
        /* A clean scanout refusal changed only access configuration. Use
         * the sole stop hook to restore it after idle, before releasing the
         * claims. A quarantine or failed proof keeps the rejecting guard. */
        if (!engine.quarantined && engine.cpu_safe && engine.ops.stop && !engine.ops.stop(&engine)) return 0;
        if (engine.touched || !engine.cpu_safe) {
            uc_engine_quarantine(&engine,decision);
            /* fallback keeps this engine, even if idle is proved later. */
        } else release_resources();
        return 0;
    }
    decision="none";
    return &engine.ops;
}
static const struct accel_ops *fallback(void)
{
    if (!registers || !engine.ops.sync) return 0;
    (void)engine.ops.sync(&engine);
    return &engine.ops;
}
static void quarantine(const char *why)
{
    blocked=true; decision=why;
    if (registers) uc_engine_quarantine(&engine,why);
    else { engine.quarantined=true; engine.active=false; engine.verified_write=false; engine.reason=why; }
}

static int marker_io(struct vfs *vfs, bool clear, bool *previous)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    int err=vfs_table_init(vfs,&marker_table,2);
    if (err) return err;
    if (!clear) {
        const char *paths[]={LEGACY_PATH,MARKER_PATH};
        for (unsigned i=0;i<ARRAY_SIZE(paths);i++) {
            struct fat_entry entry;
            err=vfs_stat(&marker_table,paths[i],&entry);
            if (!err && entry.size) { *previous=true; goto done; }
            if (err && err!=-FS_ENOENT) goto done;
        }
    }
    if (vfs->volumes[2]->readonly) { err=-FS_EROFS; goto done; }
    int fd=vfs_open(&marker_table,MARKER_PATH,VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { err=fd; goto done; }
    if (clear) err=vfs_truncate(&marker_table,fd,0);
    else {
        const struct pci_func *p=owner_of(fbdev_get());
        char text[96];
        int count=ksnprintf(text,sizeof(text),"driver=unichrome\npci=%04x:%04x\nattempt=1\n",
                            p ? p->vendor : 0,p ? p->device : 0);
        size_t wrote=0;
        err=count<0 || (unsigned)count>=sizeof(text) ? -FS_EIO :
            vfs_write(&marker_table,fd,text,(size_t)count,&wrote);
        if (!err && wrote!=(size_t)count) err=-FS_EIO;
    }
    if (!err) err=vfs_commit(&marker_table,fd);
    int close=vfs_close(&marker_table,fd);
    if (!err) err=close;
done:
    vfs_table_destroy(&marker_table);
    return err;
}
static int attempt_begin(struct vfs *vfs)
{
    marker_persisted=false;
    if (!eligible(fbdev_get())) return -EIO;
    bool previous=false;
    int err=marker_io(vfs,false,&previous);
    if (err || previous) {
        blocked=true; marker_retained=previous;
        decision=previous ? "previous_attempt" : "marker_unavailable";
        return err ? err : -EIO;
    }
    marker_persisted=true;
    return 0;
}
static int attempt_end(struct vfs *vfs)
{
    if (!marker_persisted || marker_retained || engine.quarantined ||
        (registers && !engine.cpu_safe)) return -EIO;
    bool previous=false;
    int err=marker_io(vfs,true,&previous);
    marker_persisted=false;
    if (err) { blocked=true; decision="marker_clear_failed"; }
    return err;
}
static int attempt_failed(struct vfs *vfs)
{ (void)vfs; blocked=true; marker_persisted=false; return 0; /* retain committed file */ }
static bool summary(bool *ready, bool *used, const char **reason)
{
    *ready=engine.active && !engine.quarantined;
    *used=engine.operations!=0; *reason=engine.quarantined ? engine.reason : decision;
    return true;
}
static void snapshot(struct ciuki_accel_info *out)
{
    memset(out,0,sizeof(*out));
    out->driver=CIUKI_ACCEL_DRIVER_LEGACY_NATIVE;
    out->quarantined=engine.quarantined;
    out->capabilities=engine.active && !engine.quarantined ? 1u : 0u;
    out->generation=engine.generation;
    out->operations=engine.operations;
}
static void record(void)
{
    klog("[display] driver=unichrome ready=%u cpu_ready=%u quarantined=%u reason=%s operations=%llu timeouts=%llu",
        engine.active,engine.cpu_safe,engine.quarantined,engine.quarantined ? engine.reason : decision,
        engine.operations,engine.timeouts);
}
static int check_begin(struct vfs *vfs, bool *retained)
{
    if (!(g_boot.flags&CBI_F_HW_CHECK) || !retained) return -EPERM;
    *retained=false;
    if (!eligible(fbdev_get())) return -EIO;
    int err=marker_io(vfs,false,retained);
    marker_retained=*retained; marker_persisted=!err && !*retained;
    return err;
}
static int selftest(uint64_t deadline, void (*line)(void *,const char *),void *ctx,
                    struct display_selftest_result *out)
{
    *out=(struct display_selftest_result){.first=-1,.restored=true};
    /* No private configuration sweep exists. Actual drawing comparison is
     * the generic native-only DM9 diagnostic, including activation proof. */
    if (g_ticks>=deadline) { out->timeouts=1; return out->error=-ETIMEDOUT; }
    out->tried=1;
    if (!eligible(fbdev_get())) return out->error=-EOPNOTSUPP;
    if (marker_retained) { out->skipped=1; return out->error=-EOPNOTSUPP; }
    out->passed=out->completed=1; out->first=0;
    if (line) line(ctx,"Resource preflight only; native pixels are checked by the generic DM9 comparison");
    return 0;
}
const struct display_driver unichrome_driver={
    .id="unichrome",.name="VIA UniChrome / UniChrome Pro",.matches=matches,
    .match_count=ARRAY_SIZE(matches),.framebuffer_bar=0,.probe=uc_pci_supported,.eligible=eligible,
    .start=start,.fallback=fallback,.attempt_begin=attempt_begin,.attempt_end=attempt_end,
    .attempt_failed=attempt_failed,.quarantine=quarantine,.summary=summary,.snapshot=snapshot,
    .record=record,.check_begin=check_begin,.selftest=selftest
};
