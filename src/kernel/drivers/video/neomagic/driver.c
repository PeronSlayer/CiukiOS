/* Native registry/lifecycle glue; SPDX-License-Identifier: GPL-2.0-only.
 * Engine/cursor origins and complete notices: engine.c, cursor.c, regs.h. */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/timing.h>
#include "../../../fs/vfs.h"
#include "neo.h"

static struct neo_engine engine;
static volatile uint8_t *registers;
static void *register_window, *cursor_window;
static uint8_t *cursor_mapping;
static uint32_t cursor_offset, mapped_base, mapped_fb;
static int reg_claim=-1, port_claim=-1, cursor_claim=-1;
static gen_t reg_generation, port_generation, cursor_generation;
static bool blocked, persisted;
static const char *reason="pending";
static const struct display_match matches[]={
    {0x10c8,0x0001,3,255}, {0x10c8,0x0002,3,255}, {0x10c8,0x0003,3,255},
    {0x10c8,0x0083,3,255}, {0x10c8,0x0004,3,255}, {0x10c8,0x0005,3,255},
    {0x10c8,0x0025,3,255}, {0x10c8,0x0006,3,255}, {0x10c8,0x0016,3,255}
};
static bool safe_extent(uint32_t base,uint32_t size)
{
    if (base<0x100000 || !size || size>UINT32_MAX-base) return false;
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
    /* Complete census table, but only the donor's four F/C engines qualify.
     * Read-only: no PCI configuration write, BAR sizing or engine access. */
    return p && p->vendor==0x10c8 && neo_chip(p->device) && p->class_code==3 &&
        !(p->header_type&127) && (p->command&3u)==3 && !(p->bar[0]&7u) &&
        !(p->bar[1]&7u) && (p->bar[1]&~15u) &&
        !((p->bar[1]&~15u)&(NEO_MMIO_BYTES-1u));
}
static const struct pci_func *qualified(const struct fb_device *d)
{
    const struct pci_func *p=NULL;
    const struct display_driver *const table[]={&neomagic_driver};
    if (display_driver_select(d,table,1,&p)!=&neomagic_driver || !probe(p) ||
        !neo_geometry(d) || d->size>neo_vram(p->device) ||
        d->size>g_boot.display.vram_bytes) return NULL;
    uint32_t mmio=p->bar[1]&~15u, fb=p->bar[0]&~15u;
    if (!safe_extent(mmio,NEO_MMIO_BYTES) || !safe_extent(fb,neo_vram(p->device)) ||
        (mmio<(uint64_t)fb+neo_vram(p->device) && (uint64_t)mmio+NEO_MMIO_BYTES>fb)) return NULL;
    return p;
}
static bool eligible(const struct fb_device *d)
{
    const char *why;
    if (display_accel_disabled(&why)) { reason=why; return false; }
    if (blocked || engine.quarantined) return false;
    if (!qualified(d)) { reason="unsupported_mode_or_owner"; return false; }
    return true;
}
static uint32_t read_register(void *ctx,unsigned reg)
{ (void)ctx; return *(volatile uint32_t *)(registers+reg); }
static void write_register(void *ctx,unsigned reg,uint32_t value)
{ (void)ctx; *(volatile uint32_t *)(registers+reg)=value; }
static uint64_t now_us(void *ctx)
{
    (void)ctx;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000;
    uint64_t cycles=ktime_cycles();
    return cycles/g_tsc_per_ms*1000+cycles%g_tsc_per_ms*1000/g_tsc_per_ms;
}
static void drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static bool hide_cursor(void *ctx)
{
    (void)ctx;
    if (port_claim<0 || !registry_valid(port_claim,port_generation)) return false;
    /* X.Org NeoHideCursor's independent path when the MMIO map disappears.
     * Only cursor GR82 and index latch are written; no unlock/panel/PLL. */
    uint8_t index=inb(0x3ce);
    outb(0x3ce,0x82); outb(0x3cf,0);
    bool hidden=inb(0x3cf)==0;
    outb(0x3ce,index);
    return hidden;
}
static int claim(enum res_type type,uint32_t base,uint32_t size,const char *owner,gen_t *generation)
{
    int h=registry_claim(type,base,base+size,owner,false);
    if (h<0) return h;
    const struct resource *r=registry_get((unsigned)h);
    if (!r) return -EIO;
    *generation=r->generation;
    if (registry_activate(h,*generation)) { registry_quarantine(h,*generation); return -EIO; }
    return h;
}
static bool unused_cursor_access(int h,gen_t generation)
{
    if (h!=cursor_claim || generation!=cursor_generation || engine.active || cursor_mapping) return false;
    if (cursor_window) vmm_revoke_mmio(cursor_window,PAGE_SIZE);
    return true; /* no cursor command or CPU store has used this new page */
}
/* Faults retain ownership. Clean stop revokes mappings and releases leases
 * before the next VBE mode can claim a larger framebuffer extent. */
static bool transport(const struct pci_func *p,const struct fb_device *d)
{
    uint32_t base=p->bar[1]&~15u;
    if (registers) return mapped_base==base && mapped_fb==d->phys &&
        registry_valid(reg_claim,reg_generation) && registry_valid(port_claim,port_generation);
    port_claim=claim(RES_PORT,0x3ce,2,"neomagic-cursor-disable",&port_generation);
    if (port_claim<0) return false;
    reg_claim=claim(RES_MMIO,base,NEO_MMIO_BYTES,"neomagic",&reg_generation);
    if (reg_claim<0) return false;
    if (!register_window) register_window=vmm_reserve_mmio(NEO_MMIO_BYTES);
    registers=register_window ? vmm_replace_mmio(register_window,NEO_MMIO_BYTES,base,NEO_MMIO_BYTES) : NULL;
    if (!registers) { registry_quarantine(reg_claim,reg_generation); return false; }
    mapped_base=base; mapped_fb=d->phys;
    /* Cursor address permutation has twelve 1 KiB address bits. Keep the
     * cursor below 4 MiB also on NM2380; never trust guessed firmware VRAM. */
    uint32_t limit=neo_vram(p->device);
    if (limit>g_boot.display.vram_bytes) limit=g_boot.display.vram_bytes;
    if (limit>0x400000u) limit=0x400000u;
    if (limit>=PAGE_SIZE) {
        cursor_offset=PAGE_ALIGN_DOWN(limit-PAGE_SIZE);
        if (cursor_offset>=d->size && safe_extent(d->phys+cursor_offset,PAGE_SIZE)) {
            cursor_claim=claim(RES_MMIO,d->phys+cursor_offset,PAGE_SIZE,"neomagic-cursor",&cursor_generation);
            if (cursor_claim>=0) {
                if (!cursor_window) cursor_window=vmm_reserve_mmio(PAGE_SIZE);
                cursor_mapping=cursor_window ? vmm_replace_mmio(cursor_window,PAGE_SIZE,d->phys+cursor_offset,PAGE_SIZE) : NULL;
                if (!cursor_mapping) {
                    int e=registry_quiesce(cursor_claim,cursor_generation,unused_cursor_access);
                    if (!e) e=registry_release(cursor_claim,cursor_generation);
                    if (e) { registry_quarantine(cursor_claim,cursor_generation); return false; }
                    cursor_claim=-1;
                }
            }
        }
    }
    return true;
}
static bool stopped_access(int h,gen_t generation)
{
    if (engine.active || !engine.cpu_safe || !engine.hidden) return false;
    if (h==cursor_claim && generation==cursor_generation) {
        if (cursor_mapping) vmm_revoke_mmio(cursor_mapping,PAGE_SIZE);
        cursor_mapping=NULL; engine.cursor=NULL;
        return true;
    }
    if (h==reg_claim && generation==reg_generation) {
        if (registers) vmm_revoke_mmio((void *)registers,NEO_MMIO_BYTES);
        registers=NULL; engine.io.read=NULL;
        return true;
    }
    return h==port_claim && generation==port_generation;
}
static int release_stopped(int *h,gen_t generation)
{
    if (*h<0) return 0;
    int e=registry_quiesce(*h,generation,stopped_access);
    if (!e) e=registry_release(*h,generation);
    if (!e) *h=-1;
    return e;
}
static int runtime_stop(void *ctx)
{
    if (!engine.io.read) return engine.cpu_safe ? 0 : -EIO;
    int e=neo_stop(ctx);
    if (e) return e; /* unsafe writer retains every lease/mapping */
    if ((e=release_stopped(&cursor_claim,cursor_generation)) ||
        (e=release_stopped(&reg_claim,reg_generation)) ||
        (e=release_stopped(&port_claim,port_generation))) {
        blocked=true; engine.active=false; engine.quarantined=true;
        engine.reason="release_failed";
    }
    return e;
}
static const struct accel_ops *start(const struct fb_device *d,struct vfs *vfs)
{
    (void)vfs;
    if (!persisted || !eligible(d)) return NULL;
    const struct pci_func *p=qualified(d);
    if (!p || !transport(p,d)) { blocked=true; reason="resource_unavailable"; return NULL; }
    uint8_t *cursor=cursor_mapping && cursor_offset>=d->size &&
        registry_valid(cursor_claim,cursor_generation) ? cursor_mapping : NULL;
    const struct neo_io io={NULL,read_register,write_register,now_us,drain,hide_cursor};
    if (neo_setup(&engine,&io,d,cursor,cursor_offset)) { blocked=true; reason="setup_refused"; return NULL; }
    engine.ops.stop=runtime_stop;
    int e=fbdev_accel_probe(d,neo_dm9,&engine);
    if (e) { blocked=true; reason="dm9_failed"; return NULL; }
    reason="none";
    return &engine.ops;
}
static const struct accel_ops *fallback(void)
{
    if (!engine.io.read) return NULL; /* No writer touched. */
    if (!engine.quarantined) neo_quarantine(&engine,"activation_failed");
    /* Exactly one bounded proof by the fallback caller, never from a cached
     * cpu_ready query and never repeated per frame by sync on quarantine. */
    int hidden=neo_hide(&engine), idle=neo_idle(&engine);
    return hidden || idle || !engine.cpu_safe ? &engine.ops : NULL;
}
static void quarantine(const char *why)
{
    blocked=true; reason=why;
    if (engine.io.read) neo_quarantine(&engine,why);
    else { engine.active=false; engine.quarantined=true; engine.reason=why; }
}
static bool summary(bool *ready,bool *used,const char **why)
{
    *ready=engine.active && !engine.quarantined;
    *used=false;
    *why=engine.quarantined ? engine.reason : reason;
    return true;
}
static void snapshot(struct ciuki_accel_info *out)
{
    out->driver=CIUKI_ACCEL_DRIVER_LEGACY_NATIVE;
    out->capabilities=engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0;
    out->quarantined=engine.quarantined;
    out->operations=engine.operations; out->idle_waits=engine.waits;
    out->generation=engine.fb.generation;
    out->fallbacks=0;
}
static void record(void)
{
    static const char *last_reason;
    static unsigned last_state=UINT32_MAX;
    unsigned state=engine.active|engine.quarantined<<1|engine.cpu_safe<<2;
    const char *why=engine.quarantined ? engine.reason : reason;
    if (state==last_state && why==last_reason) return;
    last_state=state; last_reason=why;
    klog("[accel] driver=neomagic ready=%u quarantined=%u cpu_ready=%u reason=%s ops=%llu timeouts=%llu",
         engine.active,engine.quarantined,engine.cpu_safe,why,
         engine.operations,engine.timeouts);
}
/* Keep the Part A legacy marker fail-closed. A nonempty legacy marker from
 * ANY driver is retained. Keyed storage/migration remains Part B's job. */
#define ATTEMPT_PATH "C:/system/settings/accel.cfg"
static struct vfs_table marker_table;
static int marker(struct vfs *vfs,bool clear)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    int e=vfs_table_init(vfs,&marker_table,2);
    if (e) return e;
    if (!clear) {
        struct fat_entry entry;
        e=vfs_stat(&marker_table,ATTEMPT_PATH,&entry);
        if (!e && entry.size) { reason="previous_attempt"; e=-EIO; goto out; }
        if (e && e!=-FS_ENOENT) goto out;
    }
    if (vfs->volumes[2]->readonly) { e=-FS_EROFS; goto out; }
    int fd=vfs_open(&marker_table,ATTEMPT_PATH,VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { e=fd; goto out; }
    if (clear) e=vfs_truncate(&marker_table,fd,0);
    else {
        static const char value[]="accel=attempt\ndriver=neomagic\n";
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
    const char *why;
    if (display_accel_disabled(&why)) { reason=why; return -EOPNOTSUPP; }
    if (blocked || engine.quarantined || engine.active) return -EIO;
    persisted=false;
    int e=marker(vfs,false);
    if (e) { blocked=true; if (!reason || !strncmp(reason,"pending",sizeof("pending"))) reason="marker_unavailable"; }
    else persisted=true;
    return e;
}
static int attempt_end(struct vfs *vfs)
{
    if (!persisted || !engine.active || engine.quarantined) return -EIO;
    int e=marker(vfs,true);
    if (!e) persisted=false;
    return e;
}
static int attempt_failed(struct vfs *vfs)
{ (void)vfs; persisted=false; blocked=true; return 0; } /* retain evidence */
static int check_begin(struct vfs *vfs,bool *retained)
{
    if (!retained) return -EINVAL;
    *retained=blocked || engine.quarantined;
    return *retained ? -EIO : attempt_begin(vfs);
}
static int cursor_test(const struct accel_ops *ops,uint64_t deadline,bool (*cancel)(void *),void *ctx)
{
    if (!ops || !ops->cursor_set || !engine.cursor) return -EOPNOTSUPP;
    struct accel_cursor transparent={0};
    int e=0;
    for (unsigned step=0;step<4 && !e;step++) {
        if (g_ticks>=deadline || (cancel && cancel(ctx))) { e=g_ticks>=deadline ? -ETIMEDOUT : -EINTR; break; }
        if (!step) e=ops->cursor_set(ops->ctx,&transparent);
        else if (step==1) e=ops->cursor_move(ops->ctx,-1,-1);
        else e=ops->cursor_show(ops->ctx,step==2);
    }
    int hidden=ops->cursor_show(ops->ctx,false);
    return e ? e : hidden;
}
const struct display_driver neomagic_driver={
    .id="neomagic",.attempt_files={ATTEMPT_PATH},.name="NeoMagic MagicGraph",.matches=matches,.match_count=ARRAY_SIZE(matches),
    .framebuffer_bar=0,.probe=probe,.eligible=eligible,.start=start,.fallback=fallback,
    .attempt_begin=attempt_begin,.attempt_end=attempt_end,.attempt_failed=attempt_failed,
    .record=record,.quarantine=quarantine,.summary=summary,.snapshot=snapshot,
    .check_begin=check_begin,.cursor_test=cursor_test
};
