/*
 * Copyright 1996-1997  David J. McKay
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * DAVID J. MCKAY BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* Rewritten with reference from mga driver and 3.3.4 NVIDIA driver by
   Jarno Paananen <jpaana@s2.org> */

/* CiukiOS NV3 adapter and resource/marker ownership. SPDX-License-Identifier: GPL-2.0-only
 * Register-map origin: X.Org xf86-video-nv riva_setup.c, riva_driver.c,
 * 0b6619fec39fada995646fb95bb25e72d3c9a491. See README.md and nvidia-notice.h. */
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/timing.h>
#include <ciuki/swdib.h>
#include "nv3.h"
#include "../../../fs/vfs.h"

static struct nv3_engine engine;
static struct accel_ops ops;
static const char *reason="pending";
static bool authorized, attempted, marker_pending, blocked, reported;
static bool report_ready,report_quarantined;
static const char *report_reason;
/* Logical INSTANCE offsets select BAR1+12 MiB; all other offsets select BAR0.
 * Claim BAR0 once, but map only small UC windows for registers we actually use. */
static struct window {
    uint32_t offset, bytes;
    void *reservation;
    uint8_t *mapping;
} windows[]={
    {0,0x1000,0,0}, {NV3_PFIFO,0x2000,0,0}, {0xc0000,0x1000,0,0},
    {NV3_PFB,0x1000,0,0}, {NV3_PGRAPH,0x2000,0,0},
    {0x601000,0x3000,0,0}, {0x680000,0x1000,0,0},
    {NV3_FIFO,0x10000,0,0}, {NV3_INSTANCE,NV3_INSTANCE_BYTES,0,0}
};
static int claims[3]={-1,-1,-1};
static gen_t generations[3];
static bool release_safe=true;
static const struct display_match matches[]={{0x12d2,0x0018,3,255}};
static bool probe(const struct pci_func *p)
{
    return p && p->vendor==0x12d2 && p->device==0x0018 && p->class_code==3 &&
        !p->subclass && !(p->header_type&0x7f) && (p->command&2u) &&
        !(p->command&4u) && !(p->bar[0]&7u) && !(p->bar[1]&7u) &&
        (p->bar[0]&~15u) && (p->bar[1]&~15u);
}
static bool extent(uint32_t base,uint32_t bytes)
{
    uint64_t end=(uint64_t)base+bytes;
    if (base<0x100000 || end>UINT32_MAX) return false;
    if (base<g_boot.kernel_end && end>g_boot.kernel_start) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *e=&g_boot.e820[i];
        if (e->type==CBI_E820_RAM && base<e->base+e->length && end>e->base) return false;
    }
    return true;
}
static const struct pci_func *eligible_owner(const struct fb_device *fb)
{
    const char *disabled;
    if (display_accel_disabled(&disabled)) { reason=disabled; return 0; }
    if (engine.quarantined || blocked) { reason=engine.reason ? engine.reason : "attempt_blocked"; return 0; }
    const struct pci_func *p=0;
    const struct display_driver *const table[]={&riva128_driver};
    if (display_driver_select(fb,table,1,&p)!=&riva128_driver) { reason="framebuffer_owner"; return 0; }
    if (!nv3_mode_supported(fb)) { reason="unsupported_mode"; return 0; }
    uint32_t a=p->bar[0]&~15u,b=p->bar[1]&~15u;
    if ((a&0xfffu) || (b&0x7fffffu) || !extent(a,0x1000000) || !extent(b,0x1000000) ||
        ((uint64_t)a<b+0x1000000ull && (uint64_t)b<a+0x1000000ull)) {
        reason="invalid_aperture"; return 0;
    }
    return p;
}
static bool eligible(const struct fb_device *fb) { return eligible_owner(fb)!=0; }
static volatile uint8_t *address(uint32_t offset,unsigned bytes)
{
    for (unsigned i=0;i<ARRAY_SIZE(windows);i++) {
        const struct window *w=&windows[i];
        if (w->mapping && offset>=w->offset && offset-w->offset<=w->bytes-bytes)
            return w->mapping+(offset-w->offset);
    }
    return 0;
}
static uint32_t read32(void *ctx,uint32_t reg)
{ (void)ctx; volatile uint8_t *p=address(reg,4); return p ? *(volatile uint32_t *)p : UINT32_MAX; }
static void write32(void *ctx,uint32_t reg,uint32_t value)
{ (void)ctx; volatile uint8_t *p=address(reg,4); if (p) *(volatile uint32_t *)p=value; }
static uint16_t read16(void *ctx,uint32_t reg)
{ (void)ctx; volatile uint8_t *p=address(reg,2); return p ? *(volatile uint16_t *)p : UINT16_MAX; }
static uint8_t read8(void *ctx,uint32_t reg)
{ (void)ctx; volatile uint8_t *p=address(reg,1); return p ? *p : UINT8_MAX; }
static void write8(void *ctx,uint32_t reg,uint8_t value)
{ (void)ctx; volatile uint8_t *p=address(reg,1); if (p) *p=value; }
static uint64_t now_us(void *ctx)
{
    (void)ctx;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000u;
    uint64_t t=ktime_cycles();
    return t/g_tsc_per_ms*1000u + t%g_tsc_per_ms*1000u/g_tsc_per_ms;
}
static int delay_us(void *ctx,uint32_t us) { (void)ctx; return udelay(us); }
static void drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static const struct nv3_io io={0,read32,write32,read16,read8,write8,now_us,delay_us,drain};
static const struct nv3_io *transport=&io;
static void unmap(void)
{
    for (unsigned i=0;i<ARRAY_SIZE(windows);i++) if (windows[i].mapping) {
        vmm_revoke_mmio(windows[i].mapping,windows[i].bytes);
        windows[i].mapping=0;
    }
}
static bool idle_proof(int handle,gen_t generation)
{ (void)handle; (void)generation; return release_safe; }
static int release(void)
{
    if (!release_safe) return -EIO;
    unmap();
    int error=0;
    for (unsigned i=0;i<ARRAY_SIZE(claims);i++) if (claims[i]>=0) {
        int e=registry_quiesce(claims[i],generations[i],idle_proof);
        if (!e) e=registry_release(claims[i],generations[i]);
        if (e) error=e; else claims[i]=-1;
    }
    return error;
}
static int claim(unsigned index,uint32_t base,uint32_t bytes,const char *owner)
{
    claims[index]=registry_claim(RES_MMIO,base,base+bytes,owner,false);
    if (claims[index]<0) return -EBUSY;
    const struct resource *r=registry_get((unsigned)claims[index]);
    if (!r) return -EIO;
    generations[index]=r->generation;
    return registry_activate(claims[index],generations[index]);
}
static bool map_window(struct window *w,uint32_t phys)
{
    if (!w->reservation) w->reservation=vmm_reserve_mmio(w->bytes);
    w->mapping=w->reservation ? vmm_replace_mmio(w->reservation,w->bytes,phys,w->bytes) : 0;
    return w->mapping!=0;
}
static int adapter_stop(void *ctx)
{
    struct nv3_engine *s=ctx;
    int e=s->touched ? s->ops.stop(ctx) : 0;
    if (e) return e;
    release_safe=s->cpu_safe;
    /* From here the writer is proved stopped. An access-lease bookkeeping
     * failure must not turn a revoked mapping into an unsafe register poll. */
    s->touched=false; s->configured=false;
    e=release();
    if (e) { blocked=true; s->quarantined=true; s->reason=reason="resource_release_failed"; }
    else if (!s->quarantined) attempted=false;
    return e;
}
static int adapter_sync(void *ctx)
{ return engine.touched ? engine.ops.sync(ctx) : engine.cpu_safe ? 0 : -EIO; }
static int adapter_cursor_show(void *ctx,bool visible)
{ return engine.touched ? engine.ops.cursor_show(ctx,visible) : visible ? -EIO : 0; }
static const struct accel_ops *fallback(void)
{
    if (!engine.touched) {
        engine.cpu_safe=true;
        release_safe=true;
        if (!release()) return 0;
        blocked=true; reason="resource_release_failed";
        return 0; /* No writer was submitted; CPU remains safe. */
    }
    /* Quarantine is sticky, but idle proof can later make CPU fallback safe.
     * Keep the ops until stop releases the mappings and claims. */
    if (engine.ops.sync) (void)engine.ops.sync(&engine);
    return &ops;
}
static void quarantine(const char *why)
{
    blocked=true; reason=why;
    if (engine.touched) nv3_engine_quarantine(&engine,why);
    else { engine.active=false; engine.quarantined=true; engine.reason=why; }
}
/* Saves/restores under the presenter's exclusive drawing claim. This is a
 * native-only comparison: the fake/real device, never swdib, writes scanout. */
static int activation_check(void *ctx)
{
    const struct fb_device *fb=ctx;
    uint32_t ram=nv3_detect_vram(transport);
    if (!ram || ram!=g_boot.display.vram_bytes || fb->size>ram-0x8000u) {
        reason="vram_mismatch"; return -EOPNOTSUPP;
    }
    uint32_t base=fb->phys+ram-0x8000u;
    if (claim(2,base,0x8000,"riva128-reserved")) { reason="vram_claim_failed"; return -EOPNOTSUPP; }
    int e=nv3_engine_init(&engine,transport,fb,ram);
    if (engine.ops.sync) {
        ops=engine.ops; ops.stop=adapter_stop; ops.sync=adapter_sync;
        ops.cursor_show=adapter_cursor_show;
    }
    if (e) { reason=engine.quarantined ? engine.reason : "scanout_refused"; return e; }
    release_safe=false;
    uint8_t saved[16*16*4],reference[16*16*4];
    struct fb_device ref=*fb;
    unsigned row_bytes=16*(fb->bpp/8);
    ref.width=ref.height=16; ref.pitch=row_bytes; ref.size=16*row_bytes; ref.mapped=reference;
    for (unsigned y=0;y<16;y++) memcpy(saved+y*row_bytes,fb->mapped+y*fb->pitch,row_bytes);
    memcpy(reference,saved,ref.size);
    struct fb_rect block={0,0,16,16},seed={0,0,4,4},source={0,0,12,12};
    const uint8_t p[8]={0x81,0x62,0x24,0x18,0xc3,0xa5,0x42,0x99};
    uint32_t a=swdib_pack(fb,0x36a5e7),b=swdib_pack(fb,0xcb7123);
    for (unsigned step=0;step<4 && !e;step++) {
        framebuffer_write_drain();
        if (!step) {
            swdib_fill(&ref,&block,a); swdib_fill(&ref,&seed,b);
            e=engine.ops.fill_rect(&engine,&block,a);
            if (!e) e=engine.ops.fill_rect(&engine,&seed,b);
        } else if (step==1 || step==2) {
            int dst=step==1 ? 4 : 0;
            source.x=source.y=step==2 ? 4 : 0;
            swdib_copy(&ref,&source,dst,dst);
            e=engine.ops.copy_rect(&engine,&source,dst,dst);
        } else {
            swdib_pattern(&ref,&block,p,a,b);
            e=engine.ops.pattern_rect(&engine,&block,p,a,b);
        }
        if (!e) e=engine.ops.sync(&engine);
        if (!e && !engine.cpu_safe) e=-EIO;
        for (unsigned y=0;y<16 && !e;y++)
            if (memcmp(reference+y*row_bytes,fb->mapped+y*fb->pitch,row_bytes)) e=-EIO;
    }
    if (!e) e=nv3_engine_cursor_test(&engine,transport->now_us(transport->ctx)+NV3_WAIT_US,0,0);
    if (!engine.ops.sync(&engine) && engine.cpu_safe) {
        for (unsigned y=0;y<16;y++) memcpy(fb->mapped+y*fb->pitch,saved+y*row_bytes,row_bytes);
        framebuffer_write_drain();
    } else e=-EIO;
    if (e) quarantine("native_comparison_failed");
    return e;
}
static const struct accel_ops *start(const struct fb_device *fb,struct vfs *unused)
{
    (void)unused;
    const struct pci_func *p=eligible_owner(fb);
    if (!p || !authorized || attempted) { if (p) reason="attempt_not_persisted"; return 0; }
    authorized=false; attempted=true;
    uint32_t reg=p->bar[0]&~15u,frame=p->bar[1]&~15u;
    release_safe=true;
    if (claim(0,reg,0x1000000,"riva128-registers") ||
        claim(1,frame+0xc00000,NV3_INSTANCE_BYTES,"riva128-instance")) {
        reason="register_claim_failed"; (void)release(); return 0;
    }
    for (unsigned i=0;i<ARRAY_SIZE(windows);i++) {
        struct window *w=&windows[i];
        uint32_t physical=(w->offset==NV3_INSTANCE ? frame : reg)+w->offset;
        if (!map_window(w,physical)) { reason="register_map_failed"; (void)release(); return 0; }
    }
    int e=fbdev_accel_probe(fb,activation_check,(void *)fb);
    if (e) { (void)fallback(); return 0; }
    reason="none";
    return &ops;
}
#define MARKER "C:/system/settings/riva128.cfg"
#define LEGACY_MARKER "C:/system/settings/accel.cfg"
static int marker(struct vfs *vfs,bool clear,bool *previous)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    static struct vfs_table table; /* worker-only, too large for kernel stack */
    int e=vfs_table_init(vfs,&table,2);
    if (e) return e;
    struct fat_entry entry;
    if (!clear) {
        const char *paths[]={LEGACY_MARKER,MARKER};
        for (unsigned i=0;i<ARRAY_SIZE(paths);i++) {
            e=vfs_stat(&table,paths[i],&entry);
            if (!e && entry.size) { *previous=true; goto done; }
            if (e && e!=-FS_ENOENT) goto done;
        }
    }
    if (vfs->volumes[2]->readonly) { e=-FS_EROFS; goto done; }
    int fd=vfs_open(&table,MARKER,VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { e=fd; goto done; }
    if (clear) e=vfs_truncate(&table,fd,0);
    else {
        const char data[]="driver=riva128\npci=12d2:0018\nstate=attempt\n";
        size_t written=0;
        e=vfs_write(&table,fd,data,sizeof(data)-1,&written);
        if (!e && written!=sizeof(data)-1) e=-FS_EIO;
    }
    if (!e) e=vfs_commit(&table,fd);
    int closed=vfs_close(&table,fd);
    if (!e) e=closed;
done:
    vfs_table_destroy(&table);
    return e;
}
static int attempt_begin(struct vfs *vfs)
{
    if (!eligible(fbdev_get()) || attempted || marker_pending) return -EIO;
    bool previous=false;
    int e=marker(vfs,false,&previous);
    if (e || previous) {
        blocked=true; reason=previous ? "previous_attempt" : "marker_unavailable";
        return e ? e : -EIO;
    }
    authorized=marker_pending=true;
    return 0;
}
static int attempt_end(struct vfs *vfs)
{
    if (!marker_pending || !engine.active || engine.quarantined) return -EIO;
    bool previous=false;
    int e=marker(vfs,true,&previous);
    if (!e) marker_pending=false;
    return e;
}
static int attempt_failed(struct vfs *vfs)
{ (void)vfs; authorized=false; blocked=true; return 0; /* retain committed marker */ }
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
    out->capabilities=engine.active && !engine.quarantined ? 3 : 0;
    out->quarantined=engine.quarantined;
    out->generation=engine.fb.generation;
    out->operations=engine.operations; out->idle_waits=engine.waits; out->fallbacks=engine.fallbacks;
}
static void record(void)
{
    const char *why=engine.quarantined ? engine.reason : reason;
    if (reported && report_ready==engine.active && report_quarantined==engine.quarantined && report_reason==why) return;
    klog("[accel] riva128 pci=12d2:0018 ready=%u quarantined=%u cpu_ready=%u reason=%s",
         engine.active,engine.quarantined,engine.cpu_safe,why);
    reported=true; report_ready=engine.active; report_quarantined=engine.quarantined; report_reason=why;
}
static int cursor_test(const struct accel_ops *selected,uint64_t deadline,bool (*cancel)(void *),void *ctx)
{
    if (selected!=&ops) return -EOPNOTSUPP;
    if (g_ticks>=deadline) return -ETIMEDOUT;
    uint64_t left=deadline-g_ticks;
    if (left>NV3_WAIT_US/1000) left=NV3_WAIT_US/1000;
    return nv3_engine_cursor_test(&engine,now_us(0)+left*1000u,cancel,ctx);
}
const struct display_driver riva128_driver={
    .id="riva128",.name="NVIDIA Riva 128",.matches=matches,.match_count=ARRAY_SIZE(matches),
    .framebuffer_bar=1,.probe=probe,.eligible=eligible,.start=start,.fallback=fallback,
    .attempt_begin=attempt_begin,.attempt_end=attempt_end,.attempt_failed=attempt_failed,
    .quarantine=quarantine,.summary=summary,.snapshot=snapshot,.record=record,.cursor_test=cursor_test
};
