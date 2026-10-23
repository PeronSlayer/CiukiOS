/*
	Copyright 2007-2011 Haiku, Inc.  All rights reserved.
	Distributed under the terms of the MIT license.

	Authors:
	Gerald Zajac
*/
/* Native lifecycle glue; SPDX-License-Identifier: GPL-2.0-only.
 * PCI table origin: Haiku r1beta5 src/add-ons/kernel/drivers/graphics/ati/driver.cpp.
 * Full donor permission notice: docs/licenses/haiku-mach64-NOTICES.txt. */
#include <ciuki/kernel.h>
#include <ciuki/display_driver.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/timing.h>
#include "engine.h"
#include "registers.h"
#include "../../../fs/vfs.h"

static const struct display_match matches[]={
    {0x1002,0x4742,3,255},{0x1002,0x4744,3,255},{0x1002,0x4749,3,255},
    {0x1002,0x4750,3,255},{0x1002,0x4751,3,255},
    {0x1002,0x4c4d,3,255},{0x1002,0x4c4e,3,255},
    {0x1002,0x4c52,3,255},{0x1002,0x4c53,3,255}
};
extern const struct display_driver mach64_driver;
static struct m64_engine engine;
static volatile uint8_t *registers;
static void *register_window,*cursor_window;
static uint8_t *cursor_mapping;
static uint32_t cursor_offset,register_phys,cursor_phys;
static int register_claim=-1,cursor_claim=-1;
static gen_t register_generation,cursor_generation;
static bool blocked,marker_owned;
static const char *reason="pending";
static bool recorded,last_active,last_quarantined,last_safe;
static const char *last_reason;

static bool safe_extent(uint32_t base,uint32_t size)
{
    if (base<0x100000 || !size || base>UINT32_MAX-size) return false;
    uint64_t end=(uint64_t)base+size;
    if (base<g_boot.kernel_end && end>g_boot.kernel_start) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *e=&g_boot.e820[i];
        if (e->type==CBI_E820_RAM && base<e->base+e->length && end>e->base) return false;
    }
    return true;
}
static bool probe(const struct pci_func *p)
{
    if (!p || p->vendor!=0x1002 || p->class_code!=3 || p->subclass ||
        (p->header_type&0x7f) || !(p->command&2u)) return false;
    bool known=false;
    for (unsigned i=0;i<ARRAY_SIZE(matches);i++) if (matches[i].device==p->device) known=true;
    /* Firmware BAR2 only. The donor's guessed legacy MMIO alias is not a
     * validated resource. Never size a BAR or enable PCI decode here. */
    uint32_t base=p->bar[2]&~15u;
    return known && !(p->bar[0]&7u) && !(p->bar[2]&7u) &&
        !(base&(PAGE_SIZE-1)) && safe_extent(base,PAGE_SIZE);
}
static const struct pci_func *candidate(const struct fb_device *d)
{
    const struct display_driver *const table[]={&mach64_driver};
    const struct pci_func *p=0;
    return display_driver_select(d,table,1,&p)==&mach64_driver ? p : 0;
}
static bool eligible(const struct fb_device *d)
{
    const char *why=0;
    if (display_accel_disabled(&why)) { reason=why; return false; }
    if (engine.quarantined) { reason=engine.reason; return false; }
    if (blocked) return false;
    if (!m64_format(d)) { reason="format_unsupported"; return false; }
    if (!candidate(d)) { reason="ownership_unproved"; return false; }
    return true;
}
static uint32_t hw_read(void *ctx,unsigned reg)
{ (void)ctx; return *(volatile uint32_t *)(registers+reg); }
static void hw_write(void *ctx,unsigned reg,uint32_t value)
{ (void)ctx; *(volatile uint32_t *)(registers+reg)=value; }
static uint64_t hw_now(void *ctx)
{
    (void)ctx;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000;
    uint64_t c=ktime_cycles();
    return c/g_tsc_per_ms*1000+c%g_tsc_per_ms*1000/g_tsc_per_ms;
}
static void hw_drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static struct m64_io io={0,hw_read,hw_write,hw_now,hw_drain};

/* File I/O only in the marker worker, outside the presenter. Any existing
 * record (including the legacy unkeyed attempt) prevents activation. */
#define ATTEMPT_PATH "C:/system/settings/accel.cfg"
static struct vfs_table marker_table;
static int marker_io(struct vfs *vfs,bool clear)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    int e=vfs_table_init(vfs,&marker_table,2);
    if (e) return e;
    if (!clear) {
        struct fat_entry entry;
        e=vfs_stat(&marker_table,ATTEMPT_PATH,&entry);
        if (!e && entry.size) { reason="previous_attempt"; e=-EIO; goto done; }
        if (e && e!=-FS_ENOENT) goto done;
    }
    if (vfs->volumes[2]->readonly) { e=-FS_EROFS; goto done; }
    int fd=vfs_open(&marker_table,ATTEMPT_PATH,VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { e=fd; goto done; }
    if (clear) e=vfs_truncate(&marker_table,fd,0);
    else {
        char text[64];
        const struct pci_func *p=candidate(fbdev_get());
        ksnprintf(text,sizeof(text),"driver=mach64\npci=1002:%04x\n",p ? p->device : 0);
        size_t bytes=strlen(text),written=0;
        e=vfs_write(&marker_table,fd,text,bytes,&written);
        if (!e && written!=bytes) e=-FS_EIO;
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
    if (!eligible(fbdev_get()) || marker_owned) return -EIO;
    int e=marker_io(vfs,false);
    if (e) { blocked=true; if (strncmp(reason,"previous_attempt",sizeof("previous_attempt"))) reason="marker_unavailable"; }
    else marker_owned=true;
    return e;
}
static int attempt_end(struct vfs *vfs)
{
    if (!marker_owned) return -EIO;
    int e=marker_io(vfs,true);
    if (e) { blocked=true; reason="marker_clear_failed"; }
    else marker_owned=false;
    return e;
}
static int attempt_failed(struct vfs *vfs)
{ (void)vfs; blocked=true; return 0; } /* retain this boot's marker */
static const struct accel_ops *fallback(void)
{
    if (!engine.touched) return 0;
    /* No caller pause during a failed start's proof: there is no registered
     * service ownership outside the activation callback. Still bounded. */
    int (*saved)(void *)=engine.pause; engine.pause=0;
    int e=m64_idle_proof(&engine); engine.pause=saved;
    return e ? &engine.ops : 0;
}
static int cursor_test(const struct accel_ops *,uint64_t,bool (*)(void *),void *);
static int activation(void *ctx)
{
    struct m64_engine *s=ctx;
    int e=m64_configure(s);
    if (e) return e;
    uint8_t saved[8*4*4];
    unsigned bytes=s->fb.bpp/8,row=8*bytes;
    for (unsigned y=0;y<4;y++) memcpy(saved+y*row,s->fb.mapped+y*s->fb.pitch,row);
    struct fb_rect left={0,0,4,4};
    uint32_t colour=0x005aa53cu & (s->fb.bpp==16 ? 0xffffu : 0xffffffu);
    e=s->ops.fill_rect(s,&left,colour);
    if (!e) e=s->ops.copy_rect(s,&left,4,0);
    if (!e) for (unsigned y=0;y<4;y++) for (unsigned x=0;x<8;x++) {
        uint32_t pixel=0;
        memcpy(&pixel,s->fb.mapped+y*s->fb.pitch+x*bytes,bytes);
        if (pixel!=colour) e=-EIO;
    }
    /* A claimed page alone does not qualify the optional native plane.
     * Exercise transparent SET/MOVE/SHOW with register readback first. */
    if (!e && s->cursor) e=cursor_test(&s->ops,g_ticks+1000,0,0);
    int idle=s->ops.sync(s);
    if (!idle && s->cpu_safe) {
        for (unsigned y=0;y<4;y++) memcpy(s->fb.mapped+y*s->fb.pitch,saved+y*row,row);
        framebuffer_write_drain();
    } else e=-EIO;
    if (e) m64_quarantine(s,"activation_comparison_failed");
    return e;
}
static const struct accel_ops *start(const struct fb_device *d,struct vfs *vfs)
{
    (void)vfs;
    if (!eligible(d) || !marker_owned || engine.active) return 0;
    const struct pci_func *p=candidate(d);
    uint32_t base=p->bar[2]&~15u;
    if (base<(uint64_t)d->phys+d->size && (uint64_t)base+PAGE_SIZE>d->phys) { reason="register_framebuffer_alias"; return 0; }
    if (register_claim>=0 && (base!=register_phys || !registry_valid(register_claim,register_generation))) { reason="register_lease_changed"; return 0; }
    if (register_claim<0) {
        register_claim=registry_claim(RES_MMIO,base,base+PAGE_SIZE,"mach64",false);
        if (register_claim<0) { reason="register_claim_failed"; return 0; }
        register_generation=registry_get((unsigned)register_claim)->generation; register_phys=base;
        if (!register_window) register_window=vmm_reserve_mmio(PAGE_SIZE);
        registers=register_window ? vmm_replace_mmio(register_window,PAGE_SIZE,base,PAGE_SIZE) : 0;
        if (!registers || registry_activate(register_claim,register_generation)) {
            registry_quarantine(register_claim,register_generation); reason="register_map_failed"; return 0;
        }
    }
    if ((hw_read(0,CONFIG_CHIP_ID)&0xffffu)!=p->device) { reason="chip_id_mismatch"; return 0; }
    unsigned mem=hw_read(0,MEM_CNTL)&15u;
    uint32_t vram=mem<8 ? (mem+1)*512*1024u : mem<12 ? (mem-3)*1024*1024u : (mem-7)*2048*1024u;
    if (vram>g_boot.display.vram_bytes) vram=g_boot.display.vram_bytes;
    /* Keep away from the top-of-aperture register alias, even with BAR2. */
    if (vram>0x7ff000u) vram=0x7ff000u;
    if (d->size>vram) { reason="framebuffer_exceeds_vram"; return 0; }
    uint32_t off=vram>=PAGE_SIZE ? (vram-PAGE_SIZE)&~(PAGE_SIZE-1u) : 0;
    uint32_t phys=d->phys+off;
    bool cursor_ok=(uint64_t)d->phys+off+PAGE_SIZE<=UINT32_MAX &&
        off>=d->size && safe_extent(phys,PAGE_SIZE) &&
        !((uint64_t)phys<base+PAGE_SIZE && (uint64_t)phys+PAGE_SIZE>base);
    if (cursor_claim>=0) cursor_ok=cursor_ok && phys==cursor_phys && registry_valid(cursor_claim,cursor_generation);
    if (cursor_ok && cursor_claim<0) {
        cursor_claim=registry_claim(RES_MMIO,phys,phys+PAGE_SIZE,"mach64-cursor",false);
        if (cursor_claim>=0) {
            cursor_generation=registry_get((unsigned)cursor_claim)->generation; cursor_phys=phys; cursor_offset=off;
            if (!cursor_window) cursor_window=vmm_reserve_mmio(PAGE_SIZE);
            cursor_mapping=cursor_window ? vmm_replace_mmio(cursor_window,PAGE_SIZE,phys,PAGE_SIZE) : 0;
            if (!cursor_mapping || registry_activate(cursor_claim,cursor_generation)) {
                registry_quarantine(cursor_claim,cursor_generation); cursor_mapping=0;
            }
        }
    }
    m64_attach(&engine,d,&io,cursor_ok ? cursor_mapping : 0,cursor_offset);
    int e=fbdev_accel_probe(d,activation,&engine);
    reason=engine.reason;
    if (e) { blocked=true; if (!engine.quarantined) reason="activation_refused"; return 0; }
    return &engine.ops;
}
static void quarantine(const char *why) { blocked=true; reason=why; m64_quarantine(&engine,why); }
static bool summary(bool *ready,bool *used,const char **why)
{
    *ready=engine.active && !engine.quarantined; *used=false;
    *why=engine.quarantined ? engine.reason : reason;
    return true;
}
static void snapshot(struct ciuki_accel_info *out)
{
    memset(out,0,sizeof(*out));
    out->driver=CIUKI_ACCEL_DRIVER_LEGACY_NATIVE;
    out->capabilities=engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0;
    out->quarantined=engine.quarantined;
    out->generation=engine.fb.generation;
    out->operations=engine.commands;
}
static void record(void)
{
    const char *why=engine.quarantined ? engine.reason : reason;
    if (recorded && last_active==engine.active && last_quarantined==engine.quarantined &&
        last_safe==engine.cpu_safe && last_reason==why) return;
    recorded=true; last_active=engine.active; last_quarantined=engine.quarantined;
    last_safe=engine.cpu_safe; last_reason=why;
    klog("[accel] mach64 ready=%u quarantine=%u cpu_ready=%u commands=%llu timeouts=%u reason=%s",
        engine.active,engine.quarantined,engine.cpu_safe,engine.commands,engine.timeouts,
        why);
}
static int cursor_test(const struct accel_ops *ops,uint64_t deadline,bool (*cancel)(void *),void *ctx)
{
    struct accel_cursor transparent={0};
    int e=0;
    for (unsigned step=0;step<4 && !e;step++) {
        if (g_ticks>=deadline || (cancel && cancel(ctx))) { e=g_ticks>=deadline ? -ETIMEDOUT : -EINTR; break; }
        if (!step) e=ops->cursor_show(ops->ctx,false);
        else if (step==1) e=ops->cursor_set(ops->ctx,&transparent);
        else if (step==2) e=ops->cursor_move(ops->ctx,-1,-1);
        else e=ops->cursor_show(ops->ctx,true);
    }
    int hidden=ops->cursor_show(ops->ctx,false);
    return e ? e : hidden;
}
const struct display_driver mach64_driver={
    .id="mach64",.attempt_files={ATTEMPT_PATH},.name="ATI Mach64 / Rage Mobility",.matches=matches,
    .match_count=ARRAY_SIZE(matches),.framebuffer_bar=0,.probe=probe,.eligible=eligible,
    .start=start,.fallback=fallback,.attempt_begin=attempt_begin,.attempt_end=attempt_end,
    .attempt_failed=attempt_failed,.record=record,.quarantine=quarantine,.summary=summary,
    .snapshot=snapshot,.cursor_test=cursor_test
};
