/*
 * Copyright 2007-2010 Haiku, Inc.  All rights reserved.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *     Gerald Zajac
 */
/* Origin: haiku/haiku r1beta5, src/add-ons/kernel/drivers/graphics/3dfx/driver.cpp
 * (PCI identity/BAR assignment) and accelerants/3dfx/3dfx_init.cpp (memory).
 * CiukiOS resource, marker, probe and validation glue; no Haiku OS host.
 * SPDX-License-Identifier: MIT
 */
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/swdib.h>
#include <ciuki/cpu.h>
#include <ciuki/timing.h>
#include "../../../fs/vfs.h"
#include "engine.h"
#include "registers.h"

static struct tdfx_engine engine={.cpu_safe=true,.reason="pending"};
static volatile uint8_t *registers;
static void *register_window, *cursor_window;
static uint8_t *cursor;
static int register_claim=-1, cursor_claim=-1;
static gen_t register_generation, cursor_generation;
static int (*engine_stop)(void *);
static bool authorized, blocked;
static struct vfs_table marker_table;
static const struct display_match matches[]={{0x121a,0x0005,3,255},{0x121a,0x0009,3,255}};

#ifdef TDFX_HOST
/* Host fake registers use exactly the production lifecycle and engine. */
extern uint32_t tdfx_host_config(const struct pci_func *,unsigned);
extern uint32_t tdfx_host_read(void *,uint32_t);
extern void tdfx_host_write(void *,uint32_t,uint32_t);
extern uint64_t tdfx_host_now(void *);
extern int tdfx_host_delay(void *,uint32_t);
#define config_read tdfx_host_config
#define hw_read tdfx_host_read
#define hw_write tdfx_host_write
#define hw_now tdfx_host_now
#define hw_delay tdfx_host_delay
#else
static uint32_t config_read(const struct pci_func *p,unsigned offset)
{
    uint32_t f=irq_save();
    outl(0xcf8,0x80000000u|((uint32_t)p->bus<<16)|((uint32_t)p->dev<<11)|
         ((uint32_t)p->fn<<8)|(offset&0xfcu));
    uint32_t value=inl(0xcfc); irq_restore(f); return value;
}
static uint32_t hw_read(void *ctx,uint32_t offset)
{ (void)ctx; return registers ? *(volatile uint32_t *)(registers+offset) : UINT32_MAX; }
static void hw_write(void *ctx,uint32_t offset,uint32_t value)
{ (void)ctx; if (registers) *(volatile uint32_t *)(registers+offset)=value; }
static uint64_t hw_now(void *ctx)
{
    (void)ctx;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000;
    uint64_t cycles=ktime_cycles();
    return cycles/g_tsc_per_ms*1000+cycles%g_tsc_per_ms*1000/g_tsc_per_ms;
}
static int hw_delay(void *ctx,uint32_t us) { (void)ctx; return udelay(us); }
#endif
static void drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static const struct tdfx_io io={.read=hw_read,.write=hw_write,.now=hw_now,.delay=hw_delay,.drain=drain};

static bool safe_extent(uint32_t base,uint32_t size)
{
    if (base<0x100000u || !size || size>UINT32_MAX-base) return false;
    uint64_t end=(uint64_t)base+size;
    if (base<g_boot.kernel_end && end>g_boot.kernel_start) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *r=&g_boot.e820[i];
        if (r->type==CBI_E820_RAM && base<r->base+r->length && end>r->base) return false;
    }
    return true;
}
static bool probe(const struct pci_func *p)
{
    if (!p || p->vendor!=0x121a || (p->device!=5 && p->device!=9) ||
        p->class_code!=3 || p->header_type || p->fn || !(p->command&2) ||
        (p->bar[0]&7u) || (p->bar[1]&7u)) return false;
    /* A second VSA die, including a non-display-class function, is not
     * silently treated as a single-chip adapter. Multifunction is refused. */
    for (unsigned i=0;i<pci_count();i++) {
        const struct pci_func *other=pci_get(i);
        if (other && other!=p && other->vendor==0x121a && other->device==9) return false;
    }
    uint32_t reg=p->bar[0]&~15u,fb=p->bar[1]&~15u;
    if ((reg&(PAGE_SIZE-1u)) || !safe_extent(reg,TDFX_MMIO_BYTES) ||
        !safe_extent(fb,g_boot.display.vram_bytes) ||
        (reg<(uint64_t)fb+g_boot.display.vram_bytes && fb<(uint64_t)reg+TDFX_MMIO_BYTES)) return false;
    if (p->device==9) {
        /* X.Org tdfxdefs.h: snooping/multifunction, enhanced-video SLI,
         * SLI LFB chip count/dispatch/read, and AA LFB dispatch/read. Read
         * only; never disable/reconfigure a multi-chip board to qualify. */
        if ((config_read(p,0x40)&0x10007800u) || (config_read(p,0x80)&3u) ||
            (config_read(p,0x8c)&0x1f000000u) || (config_read(p,0x94)&0x1c000000u)) return false;
    }
    return true;
}
static const struct pci_func *qualified(const struct fb_device *f)
{
    const char *reason;
    if (display_accel_disabled(&reason)) { engine.reason=reason; return 0; }
    if (blocked || engine.quarantined || !tdfx_layout(f)) return 0;
    const struct display_driver *table[]={&tdfx_driver};
    const struct pci_func *p=0;
    return display_driver_select(f,table,1,&p)==&tdfx_driver ? p : 0;
}
static bool eligible(const struct fb_device *f) { return qualified(f)!=0; }

static bool resource_idle(int handle,gen_t generation)
{
    if (!engine.cpu_safe) return false;
    if (handle==register_claim && generation==register_generation) {
        if (registers) vmm_revoke_mmio((void *)registers,TDFX_MMIO_BYTES);
        registers=0; return true;
    }
    if (handle==cursor_claim && generation==cursor_generation) {
        if (cursor) vmm_revoke_mmio(cursor,PAGE_SIZE);
        cursor=0; engine.cursor=0; return true;
    }
    return false;
}
static int release_access(void)
{
    if (cursor_claim>=0) {
        int err=registry_quiesce(cursor_claim,cursor_generation,resource_idle);
        if (!err) err=registry_release(cursor_claim,cursor_generation);
        if (err) return err;
        cursor_claim=-1;
    }
    if (register_claim>=0) {
        int err=registry_quiesce(register_claim,register_generation,resource_idle);
        if (!err) err=registry_release(register_claim,register_generation);
        if (err) return err;
        register_claim=-1;
    }
    return 0;
}
static int runtime_stop(void *ctx)
{
    int err=engine_stop ? engine_stop(ctx) : engine.cpu_safe ? 0 : -EIO;
    if (!err) err=release_access();
    if (err) { blocked=true; tdfx_engine_quarantine(&engine,"stop_failed"); }
    else { engine.io.read=0; engine.io.write=0; engine.shape=false; }
    return err;
}
static const struct accel_ops *fallback(void)
{
    if (!engine.cpu_safe && engine.ops.sync && registers) (void)engine.ops.sync(&engine);
    if (engine.cpu_safe) return 0;
    return &engine.ops;
}
static void quarantine(const char *reason)
{
    blocked=true; authorized=false;
    tdfx_engine_quarantine(&engine,reason);
}

/* Saved, disjoint 8x8 block; every draw is native. No software replay,
 * and no CPU read/restore after a failed idle proof. Presenter owns calls. */
static int activation_probe(void *arg)
{
    const struct fb_device *f=arg;
    uint32_t offset=cursor ? (g_boot.display.vram_bytes-PAGE_SIZE)&~(PAGE_SIZE-1u) : 0;
    int err=tdfx_engine_start(&engine,&io,f,cursor,offset);
    engine_stop=engine.ops.stop; engine.ops.stop=runtime_stop;
    if (err) return err;
    if (f->width<8 || f->height<8) return -EOPNOTSUPP;
    uint8_t saved[8*8*4],golden[sizeof(saved)];
    unsigned bytes=f->bpp/8u;
    struct fb_device ref=*f; ref.width=ref.height=8; ref.pitch=8*bytes;
    ref.size=8*ref.pitch; ref.mapped=golden;
    err=engine.ops.sync(&engine); if (err || !engine.cpu_safe) return -EIO;
    for (unsigned y=0;y<8;y++) memcpy(saved+y*ref.pitch,f->mapped+y*f->pitch,ref.pitch);
    memcpy(golden,saved,ref.size);
    struct fb_rect block={0,0,8,8},seed={0,0,3,3},a={0,0,6,6},b={2,2,6,6};
    swdib_fill(&ref,&block,swdib_pack(f,0x3197db));
    err=engine.ops.fill_rect(&engine,&block,swdib_pack(f,0x3197db));
    if (!err) {
        swdib_fill(&ref,&seed,swdib_pack(f,0xe52364));
        err=engine.ops.fill_rect(&engine,&seed,swdib_pack(f,0xe52364));
    }
    for (unsigned step=0;step<3 && !err;step++) {
        if (step) {
            const struct fb_rect *s=step==1 ? &a : &b;
            int dst=step==1 ? 2 : 0;
            swdib_copy(&ref,s,dst,dst); err=engine.ops.copy_rect(&engine,s,dst,dst);
        }
        if (!err) err=engine.ops.sync(&engine);
        if (!err && engine.cpu_safe) for (unsigned y=0;y<8;y++)
            if (memcmp(f->mapped+y*f->pitch,golden+y*ref.pitch,ref.pitch)) { err=-EIO; break; }
    }
    int idle=engine.ops.sync(&engine);
    if (!idle && engine.cpu_safe) {
        for (unsigned y=0;y<8;y++) memcpy(f->mapped+y*f->pitch,saved+y*ref.pitch,ref.pitch);
        framebuffer_write_drain();
    } else err=-EIO;
    if (err) quarantine("activation_comparison_failed");
    return err;
}
static const struct accel_ops *start(const struct fb_device *f,struct vfs *unused)
{
    (void)unused;
    const struct pci_func *p=qualified(f);
    if (!p || !authorized || register_claim>=0) return 0;
    authorized=false; /* consume exactly one durable authorization */
    uint32_t base=p->bar[0]&~15u;
    register_claim=registry_claim(RES_MMIO,base,base+TDFX_MMIO_BYTES,"tdfx-registers",false);
    if (register_claim<0) { engine.reason="register_claim_failed"; return 0; }
    register_generation=registry_get(register_claim)->generation;
    if (!register_window) register_window=vmm_reserve_mmio(TDFX_MMIO_BYTES);
    registers=register_window ? vmm_replace_mmio(register_window,TDFX_MMIO_BYTES,base,TDFX_MMIO_BYTES) : 0;
    if (!registers || registry_activate(register_claim,register_generation)) goto clean_failure;
    uint32_t vram=tdfx_vram(&io,p->device);
    if (!vram || g_boot.display.vram_bytes>vram ||
        (p->device==9 && (hw_read(0,0x20020c)!=0 || hw_read(0,0x200210)!=0))) {
        engine.reason="memory_or_topology_unsupported"; goto clean_failure;
    }
    uint32_t offset=(g_boot.display.vram_bytes-PAGE_SIZE)&~(PAGE_SIZE-1u);
    if (g_boot.display.vram_bytes>=PAGE_SIZE && offset>=f->size &&
        safe_extent(f->phys+offset,PAGE_SIZE)) {
        cursor_claim=registry_claim(RES_MMIO,f->phys+offset,f->phys+offset+PAGE_SIZE,"tdfx-cursor",false);
        if (cursor_claim>=0) {
            cursor_generation=registry_get(cursor_claim)->generation;
            if (!cursor_window) cursor_window=vmm_reserve_mmio(PAGE_SIZE);
            cursor=cursor_window ? vmm_replace_mmio(cursor_window,PAGE_SIZE,f->phys+offset,PAGE_SIZE) : 0;
            if (!cursor || registry_activate(cursor_claim,cursor_generation)) goto clean_failure;
        }
    }
    if (fbdev_accel_probe(f,activation_probe,(void *)f)) {
        blocked=true;
        if (engine.touched && !engine.quarantined) quarantine("activation_failed");
        if (engine.cpu_safe) (void)release_access();
        return 0;
    }
    return &engine.ops;
clean_failure:
    if (!engine.reason || !strncmp(engine.reason,"pending",sizeof("pending"))) engine.reason="resource_unavailable";
    (void)release_access();
    return 0;
}
static const char *marker_path(const struct pci_func *p)
{ return p->device==5 ? "C:/system/settings/tdfx0005.cfg" : "C:/system/settings/tdfx0009.cfg"; }
static int marker(struct vfs *vfs,const struct pci_func *p,bool clear)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -EIO;
    int err=vfs_table_init(vfs,&marker_table,2); if (err) return err;
    const char *path=marker_path(p);
    if (!clear) {
        struct fat_entry entry;
        err=vfs_stat(&marker_table,path,&entry);
        if (!err && entry.size) { engine.reason="previous_attempt"; err=-EIO; goto out; }
        if (err && err!=-FS_ENOENT) goto out;
    }
    if (vfs->volumes[2]->readonly) { err=-FS_EROFS; goto out; }
    int fd=vfs_open(&marker_table,path,VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { err=fd; goto out; }
    if (clear) err=vfs_truncate(&marker_table,fd,0);
    else {
        const char *text=p->device==5 ? "driver=tdfx pci=121a:0005 attempt\n" : "driver=tdfx pci=121a:0009 attempt\n";
        size_t done=0; err=vfs_write(&marker_table,fd,text,strlen(text),&done);
        if (!err && done!=strlen(text)) err=-EIO;
    }
    if (!err) err=vfs_commit(&marker_table,fd);
    int closed=vfs_close(&marker_table,fd); if (!err) err=closed;
out:
    vfs_table_destroy(&marker_table); return err;
}
static int attempt_begin(struct vfs *vfs)
{
    authorized=false;
    const struct pci_func *p=qualified(fbdev_get()); if (!p) return -EOPNOTSUPP;
    int err=marker(vfs,p,false);
    if (err) { blocked=true; if (strncmp(engine.reason,"previous_attempt",sizeof("previous_attempt"))) engine.reason="marker_unavailable"; }
    else authorized=true;
    return err;
}
static int attempt_end(struct vfs *vfs)
{
    const struct pci_func *p=display_driver_owner();
    if (!p || !engine.active || engine.quarantined) return -EIO;
    return marker(vfs,p,true);
}
static int attempt_failed(struct vfs *vfs)
{ (void)vfs; authorized=false; blocked=true; return 0; /* retain committed marker */ }
static bool summary(bool *ready,bool *used,const char **reason)
{
    *ready=engine.active && !engine.quarantined; *used=false;
    *reason=engine.reason; return true;
}
static void snapshot(struct ciuki_accel_info *out)
{
    *out=(struct ciuki_accel_info){.driver=CIUKI_ACCEL_DRIVER_LEGACY_NATIVE};
    out->capabilities=engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0;
    out->quarantined=engine.quarantined; out->generation=engine.fb.generation;
    out->operations=engine.operations; out->idle_waits=engine.idle_waits; out->fallbacks=engine.fallbacks;
}
static void record(void)
{
    static bool recorded;
    static unsigned last_state;
    static const char *last_reason;
    unsigned state=(engine.active ? 1u : 0u)|(engine.quarantined ? 2u : 0u)|(engine.cpu_safe ? 4u : 0u);
    /* fbdev calls this after ordinary presentation too. Report state
     * changes only; serial logging must not dominate every desktop frame. */
    if (recorded && state==last_state && last_reason==engine.reason) return;
    recorded=true; last_state=state; last_reason=engine.reason;
    klog("[accel] driver=tdfx ready=%u quarantine=%u cpu_ready=%u status=%08x reason=%s",
        engine.active && !engine.quarantined,engine.quarantined,engine.cpu_safe,engine.status,engine.reason);
}
const struct display_driver tdfx_driver={
    .id="tdfx",.name="3dfx Voodoo3 / VSA-100",.matches=matches,
    .match_count=ARRAY_SIZE(matches),.framebuffer_bar=1,
    .probe=probe,.eligible=eligible,.start=start,.fallback=fallback,
    .attempt_begin=attempt_begin,.attempt_end=attempt_end,.attempt_failed=attempt_failed,
    .record=record,.quarantine=quarantine,.summary=summary,.snapshot=snapshot,
    .cursor_test=tdfx_cursor_check
};
