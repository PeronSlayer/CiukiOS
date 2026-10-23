/*
 *	drivers/video/aty/radeon_base.c
 *
 *	framebuffer driver for ATI Radeon chipset video boards
 *
 *	Copyright 2003	Ben. Herrenschmidt <benh@kernel.crashing.org>
 *	Copyright 2000	Ani Joshi <ajoshi@kernel.crashing.org>
 *
 *	i2c bits from Luca Tettamanti <kronos@kronoz.cjb.net>
 *
 *	Special thanks to ATI DevRel team for their hardware donations.
 *
 *	...Insert GPL boilerplate here...
 *
 *	Significant portions of this driver apdated from XFree86 Radeon
 *	driver which has the following copyright notice:
 *
 *	Copyright 2000 ATI Technologies Inc., Markham, Ontario, and
 *                     VA Linux Systems Inc., Fremont, California.
 *
 *	All Rights Reserved.
 *
 *	Permission is hereby granted, free of charge, to any person obtaining
 *	a copy of this software and associated documentation files (the
 *	"Software"), to deal in the Software without restriction, including
 *	without limitation on the rights to use, copy, modify, merge,
 *	publish, distribute, sublicense, and/or sell copies of the Software,
 *	and to permit persons to whom the Software is furnished to do so,
 *	subject to the following conditions:
 *
 *	The above copyright notice and this permission notice (including the
 *	next paragraph) shall be included in all copies or substantial
 *	portions of the Software.
 *
 *	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * 	EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *	MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *	NON-INFRINGEMENT.  IN NO EVENT SHALL ATI, VA LINUX SYSTEMS AND/OR
 *	THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *	WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *	DEALINGS IN THE SOFTWARE.
 *
 *	XFree86 driver authors:
 *
 *	   Kevin E. Martin <martin@xfree86.org>
 *	   Rickard E. Faith <faith@valinux.com>
 *	   Alan Hourihane <alanh@fairlite.demon.co.uk>
 *
 */


/* Native PCI/resource/marker adapter; no upstream OS glue imported.
 * PCI whitelist origin: Linux v6.12 drivers/video/fbdev/aty/radeon_base.c,
 * radeonfb_pci_table (discrete primary R100/R200/R300/R420 variants).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/timing.h>
#include "../../../fs/vfs.h"
#include "engine.h"

/* Discrete primary functions from Linux v6.12 radeonfb_pci_table.
 * Shared list prevents the registry match and generation selection diverging. */
#define RADEON_CHIPS(X) \
    X(0x4c59, R100) \
    X(0x4c5a, R100) \
    X(0x5159, R100) \
    X(0x515a, R100) \
    X(0x4242, R200) \
    X(0x4243, R200) \
    X(0x5148, R200) \
    X(0x514c, R200) \
    X(0x514d, R200) \
    X(0x4c57, R100) \
    X(0x4c58, R100) \
    X(0x5157, R100) \
    X(0x5158, R100) \
    X(0x4c64, R200) \
    X(0x4c65, R200) \
    X(0x4c66, R200) \
    X(0x4c67, R200) \
    X(0x4966, R200) \
    X(0x4967, R200) \
    X(0x5c61, R200) \
    X(0x5c63, R200) \
    X(0x5960, R200) \
    X(0x5961, R200) \
    X(0x5962, R200) \
    X(0x5964, R200) \
    X(0x4144, R300) \
    X(0x4145, R300) \
    X(0x4146, R300) \
    X(0x4147, R300) \
    X(0x4e44, R300) \
    X(0x4e45, R300) \
    X(0x4e46, R300) \
    X(0x4e47, R300) \
    X(0x4e50, R300) \
    X(0x4e51, R300) \
    X(0x4e52, R300) \
    X(0x4e53, R300) \
    X(0x4e54, R300) \
    X(0x4e56, R300) \
    X(0x4150, R300) \
    X(0x4151, R300) \
    X(0x4152, R300) \
    X(0x4153, R300) \
    X(0x4154, R300) \
    X(0x4156, R300) \
    X(0x4148, R300) \
    X(0x4149, R300) \
    X(0x414a, R300) \
    X(0x414b, R300) \
    X(0x4e48, R300) \
    X(0x4e49, R300) \
    X(0x4e4a, R300) \
    X(0x4e4b, R300) \
    X(0x3e50, R300) \
    X(0x3e54, R300) \
    X(0x3150, R300) \
    X(0x3154, R300) \
    X(0x5b60, R300) \
    X(0x5b62, R300) \
    X(0x5b63, R300) \
    X(0x5b64, R300) \
    X(0x5b65, R300) \
    X(0x5460, R300) \
    X(0x5464, R300) \
    X(0x4a48, R4XX) \
    X(0x4a49, R4XX) \
    X(0x4a4a, R4XX) \
    X(0x4a4b, R4XX) \
    X(0x4a4c, R4XX) \
    X(0x4a4d, R4XX) \
    X(0x4a4e, R4XX) \
    X(0x4a50, R4XX) \
    X(0x5548, R4XX) \
    X(0x5549, R4XX) \
    X(0x554a, R4XX) \
    X(0x554b, R4XX) \
    X(0x5551, R4XX) \
    X(0x5552, R4XX) \
    X(0x5554, R4XX) \
    X(0x5d57, R4XX) \
    X(0x5144, R100) \
    X(0x5145, R100) \
    X(0x5146, R100) \
    X(0x5147, R100)
#define CHIP_MATCH(id, group) {0x1002,id,3,255},
static const struct display_match matches[] = { RADEON_CHIPS(CHIP_MATCH) };
#undef CHIP_MATCH
static enum radeon_group chip_group(uint16_t device)
{
    switch (device) {
#define CHIP_GROUP(id, group) case id: return RADEON_##group;
        RADEON_CHIPS(CHIP_GROUP)
#undef CHIP_GROUP
    default: return RADEON_UNKNOWN;
    }
}
#undef RADEON_CHIPS
static struct radeon_engine engine;
static uint8_t *registers;
static void *register_window, *cursor_window;
static bool prepared, authorized, attempt_blocked;
static int reg_claim=-1, cursor_claim=-1;
static gen_t reg_generation, cursor_generation;
static const char *decision="pending";
static bool safe_range(uint32_t start, uint32_t bytes)
{
    if (start<0x100000u || !bytes || bytes>UINT32_MAX-start) return false;
    uint64_t end=(uint64_t)start+bytes;
    if (start<g_boot.kernel_end && end>g_boot.kernel_start) return false;
    for (unsigned i=0; i<g_boot.e820_count; i++) {
        const struct ciuki_e820 *e=&g_boot.e820[i];
        if (e->length>UINT64_MAX-e->base ||
            (e->type==CBI_E820_RAM && start<e->base+e->length && end>e->base)) return false;
    }
    return true;
}
static bool probe(const struct pci_func *p)
{
    if (!p || p->vendor!=0x1002 || p->class_code!=3 || p->subclass || p->fn || p->revision==0xff ||
        (p->header_type & 0x7fu) || !(p->command & 2u) || (p->command & 4u) ||
        (p->bar[0] & 7u) || (p->bar[2] & 15u)) return false;
    bool found=false;
    for (unsigned i=0; i<ARRAY_SIZE(matches); i++) if (matches[i].device==p->device) found=true;
    uint32_t base=p->bar[2] & ~15u;
    if (!found || (base & (RADEON_REGSIZE-1u)) || !safe_range(base,RADEON_REGSIZE)) return false;
    uint32_t fb=p->bar[0] & ~15u, bytes=g_boot.display.vram_bytes;
    return safe_range(fb,bytes) && !(base<(uint64_t)fb+bytes && (uint64_t)base+RADEON_REGSIZE>fb);
}
static bool eligible(const struct fb_device *d)
{
    const char *reason;
    if (display_accel_disabled(&reason)) { decision=reason; return false; }
    if (engine.quarantined || attempt_blocked) { decision="same_boot_blocked"; return false; }
    if (!radeon_format(d,0) || d->width<8 || d->height<4) { decision="unsupported_format"; return false; }
    /* Reuse the generic exact active fbdev lease, ambiguity and BAR proof. */
    const struct display_driver *table[]={&radeon2d_driver};
    const struct pci_func *owner=0;
    if (!display_driver_select(d,table,1,&owner)) { decision="ownership_unproved"; return false; }
    return true;
}
static uint32_t hw_read(void *ctx, uint32_t reg)
{ (void)ctx; return *(volatile uint32_t *)(registers+reg); }
static void hw_write(void *ctx, uint32_t reg, uint32_t value)
{ (void)ctx; *(volatile uint32_t *)(registers+reg)=value; }
static uint64_t hw_now(void *ctx)
{
    (void)ctx;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000u;
    uint64_t cycles=ktime_cycles();
    return cycles/g_tsc_per_ms*1000u + cycles%g_tsc_per_ms*1000u/g_tsc_per_ms;
}
static int hw_delay(void *ctx, uint32_t us) { (void)ctx; return udelay(us); }
static void hw_drain(void *ctx) { (void)ctx; cpu_write_drain(); }
static bool access_idle(int handle, gen_t generation)
{
    if (engine.active || (prepared && (!engine.cpu_safe || !engine.cursor_hidden))) return false;
    if (handle==cursor_claim && generation==cursor_generation) {
        if (engine.cursor) vmm_revoke_mmio(engine.cursor,RADEON_CURSOR_BYTES);
        engine.cursor=0; return true;
    }
    if (handle==reg_claim && generation==reg_generation) {
        if (registers) vmm_revoke_mmio(registers,RADEON_REGSIZE);
        registers=0; return true;
    }
    return false;
}
static int release_access(int *handle, gen_t generation)
{
    if (*handle<0) return 0;
    int e=registry_quiesce(*handle,generation,access_idle);
    if (!e) e=registry_release(*handle,generation);
    if (!e) *handle=-1;
    return e;
}
static int release_resources(void)
{
    int e=release_access(&cursor_claim,cursor_generation);
    if (!e) e=release_access(&reg_claim,reg_generation);
    if (!e) prepared=false;
    return e;
}
static int runtime_stop(void *ctx)
{
    struct radeon_engine *s=ctx;
    s->active=false;
    int e=radeon_engine_idle(s);
    if (!e) e=radeon_cursor_hide(s);
    if (e) { s->cpu_safe=false; radeon_engine_quarantine(s,"stop_unproved"); return e; }
    return release_resources();
}
static const struct accel_ops *fallback(void)
{
    if (!prepared) return 0;
    /* No reset and no optimistic CPU fallback. A recovered engine stays
     * quarantined; only the cached CPU-safety flag changes after proof. */
    if (!radeon_engine_idle(&engine) && radeon_cursor_hide(&engine)) engine.cpu_safe=false;
    return &engine.ops;
}
static const struct accel_ops *start_with_io(const struct fb_device *d, struct vfs *vfs, const struct radeon_io *io)
{
    if (vfs || !authorized || !eligible(d)) { authorized=false; return 0; }
    authorized=false;
    if (reg_claim>=0 || cursor_claim>=0) { decision="access_retained"; return 0; }
    const struct pci_func *p=display_driver_owner();
    if (!probe(p)) { decision="ownership_unproved"; return 0; }
    uint32_t base=p->bar[2] & ~15u;
    reg_claim=registry_claim(RES_MMIO,base,base+RADEON_REGSIZE,"radeon2d",false);
    if (reg_claim<0) { decision="register_claim_failed"; return 0; }
    reg_generation=registry_get((unsigned)reg_claim)->generation;
    if (registry_activate(reg_claim,reg_generation)) {
        registry_quarantine(reg_claim,reg_generation); decision="register_activation_failed"; return 0;
    }
    if (!register_window) register_window=vmm_reserve_mmio(RADEON_REGSIZE);
    registers=register_window ? vmm_replace_mmio(register_window,RADEON_REGSIZE,base,RADEON_REGSIZE) : 0;
    if (!registers) { decision="register_mapping_failed"; (void)release_resources(); return 0; }
    if (radeon_engine_prepare(&engine,d,io,chip_group(p->device))) {
        decision="scanout_unqualified"; (void)release_resources(); return 0;
    }
    prepared=true;
    /* Firmware and device memory size must agree on every reserved byte.
     * Cursor allocation never overlaps the active framebuffer lease. */
    uint32_t vram=io->read(io->ctx,CNFG_MEMSIZE);
    if (vram>g_boot.display.vram_bytes) vram=g_boot.display.vram_bytes;
    if (vram>engine.local_bytes) vram=(uint32_t)engine.local_bytes;
    if ((engine.group->features & RADEON_CURSOR) && vram>=RADEON_CURSOR_BYTES) {
        uint32_t off=(vram-RADEON_CURSOR_BYTES) & ~(RADEON_CURSOR_BYTES-1u);
        if (off>=d->size && off<=0x07fff000u && safe_range(d->phys+off,RADEON_CURSOR_BYTES)) {
            cursor_claim=registry_claim(RES_MMIO,d->phys+off,d->phys+off+RADEON_CURSOR_BYTES,"radeon-cursor",false);
            if (cursor_claim>=0) {
                cursor_generation=registry_get((unsigned)cursor_claim)->generation;
                if (registry_activate(cursor_claim,cursor_generation)) {
                    registry_quarantine(cursor_claim,cursor_generation);
                    decision="cursor_activation_failed";
                    if (!release_access(&reg_claim,reg_generation)) prepared=false;
                    return 0;
                }
                /* A private UC VRAM lease, not the active scanout mapper:
                 * that mapper deliberately accepts only g_boot.fb_phys. */
                if (!cursor_window) cursor_window=vmm_reserve_mmio(RADEON_CURSOR_BYTES);
                engine.cursor=cursor_window ? vmm_replace_mmio(cursor_window,RADEON_CURSOR_BYTES,
                    d->phys+off,RADEON_CURSOR_BYTES) : 0;
                if (!engine.cursor) (void)release_access(&cursor_claim,cursor_generation);
                else engine.cursor_offset=off;
            }
        }
    }
    engine.ops.stop=runtime_stop;
    int e=fbdev_accel_probe(d,radeon_engine_activate,&engine);
    if (e) {
        if (!engine.quarantined) radeon_engine_quarantine(&engine,"probe_exclusion_failed");
        (void)fallback(); decision=engine.reason; return 0;
    }
    decision="none";
    return &engine.ops;
}
static const struct accel_ops *start(const struct fb_device *d, struct vfs *vfs)
{
    const struct radeon_io io={0,hw_read,hw_write,hw_now,hw_delay,hw_drain};
    return start_with_io(d,vfs,&io);
}

/* Preserve the part A global legacy record, including nonempty records from
 * any other driver. Keying/migration belongs to part B, not this family. */
#define ATTEMPT_PATH "C:/system/settings/accel.cfg"
static struct vfs_table marker_table;
static int marker(struct vfs *vfs, bool clear)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    int e=vfs_table_init(vfs,&marker_table,2);
    if (e) return e;
    if (!clear) {
        struct fat_entry entry;
        e=vfs_stat(&marker_table,ATTEMPT_PATH,&entry);
        if (!e && entry.size) { decision="previous_attempt"; e=-EIO; goto out; }
        if (e && e!=-FS_ENOENT) goto out;
    }
    if (vfs->volumes[2]->readonly) { e=-FS_EROFS; goto out; }
    int fd=vfs_open(&marker_table,ATTEMPT_PATH,VFS_WRITE | VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { e=fd; goto out; }
    if (clear) e=vfs_truncate(&marker_table,fd,0);
    else {
        const char value[]="accel=attempt\n";
        size_t done=0;
        e=vfs_write(&marker_table,fd,value,sizeof(value)-1,&done);
        if (!e && done!=sizeof(value)-1) e=-FS_EIO;
    }
    if (!e) e=vfs_commit(&marker_table,fd);
    int closed=vfs_close(&marker_table,fd);
    if (!e) e=closed;
out:
    vfs_table_destroy(&marker_table);
    return e;
}
static int attempt_begin(struct vfs *vfs)
{
    const char *reason;
    authorized=false;
    if (display_accel_disabled(&reason)) { decision=reason; return -EOPNOTSUPP; }
    if (engine.quarantined || attempt_blocked) return -EIO;
    int e=marker(vfs,false);
    if (e) { attempt_blocked=true; if (strncmp(decision,"previous_attempt",sizeof("previous_attempt"))) decision="marker_unavailable"; }
    else authorized=true;
    return e;
}
static int attempt_end(struct vfs *vfs)
{
    if (!prepared || !engine.active || engine.quarantined) return -EIO;
    int e=marker(vfs,true);
    if (e) { attempt_blocked=true; decision="marker_clear_failed"; }
    return e;
}
static int attempt_failed(struct vfs *vfs)
{ (void)vfs; authorized=false; attempt_blocked=true; return 0; }
static void quarantine(const char *reason)
{
    attempt_blocked=true; authorized=false;
    if (prepared) radeon_engine_quarantine(&engine,reason);
    else { engine.quarantined=true; decision=reason; }
}
static bool summary(bool *ready, bool *used, const char **reason)
{
    *ready=prepared && engine.active && !engine.quarantined;
    *used=false; /* Only the generic completed presenter counters establish YES. */
    *reason=engine.quarantined && engine.reason ? engine.reason : decision;
    return true;
}
static void snapshot(struct ciuki_accel_info *out)
{
    memset(out,0,sizeof(*out));
    out->driver=CIUKI_ACCEL_DRIVER_LEGACY_NATIVE;
    out->capabilities=prepared ? engine.ops.capabilities(&engine) : 0;
    out->quarantined=engine.quarantined;
    out->generation=engine.fb.generation;
    out->operations=engine.commands;
    out->idle_waits=engine.waits;
}
static void record(void)
{
    klog("[accel] driver=radeon2d ready=%u quarantined=%u cpu_ready=%u subgroup=%s verified=%x commands=%llu waits=%llu timeouts=%llu reason=%s",
        prepared && engine.active,engine.quarantined,engine.cpu_safe,engine.group ? engine.group->name : "unknown",engine.verified,engine.commands,engine.waits,
        engine.timeouts,engine.quarantined && engine.reason ? engine.reason : decision);
}
static int cursor_test(const struct accel_ops *ops, uint64_t deadline, bool (*cancel)(void *), void *ctx)
{
    struct accel_cursor transparent={0};
    int e=0;
    for (unsigned step=0; step<4 && !e; step++) {
        if (g_ticks>=deadline || (cancel && cancel(ctx))) { e=g_ticks>=deadline ? -ETIMEDOUT : -EINTR; break; }
        if (!step) e=ops->cursor_show(ops->ctx,false);
        else if (step==1) e=ops->cursor_set(ops->ctx,&transparent);
        else if (step==2) e=ops->cursor_move(ops->ctx,0,0);
        else e=ops->cursor_show(ops->ctx,true);
    }
    int hidden=ops->cursor_show(ops->ctx,false);
    return e ? e : hidden;
}
const struct display_driver radeon2d_driver={
    .id="radeon2d", .name="ATI Radeon R100/R200/R300/R4xx", .matches=matches,
    .match_count=ARRAY_SIZE(matches), .framebuffer_bar=0, .probe=probe, .eligible=eligible,
    .start=start, .fallback=fallback, .attempt_begin=attempt_begin, .attempt_end=attempt_end,
    .attempt_failed=attempt_failed, .quarantine=quarantine, .summary=summary,
    .snapshot=snapshot, .record=record, .cursor_test=cursor_test
};
