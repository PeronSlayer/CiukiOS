/* Native Ciuki adapter for Linux v6.12 cirrusfb engine.c/registers.h.
 * OS glue is original; donor notices are retained in those imported files.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mtrr.h>
#include <ciuki/timing.h>
#include "engine.h"
#include "../../../fs/vfs.h"

static const struct display_match matches[]={
    {0x1013,0x00a0,3,255},{0x1013,0x00a4,3,255},
    {0x1013,0x00ac,3,255},{0x1013,0x00b8,3,255}
};
static struct cirrus_engine engine;
static int port_claim=-1;
static gen_t port_generation;
static bool persisted, blocked;
static struct vfs_table marker_table;
static const char *reason="pending";
extern const struct display_driver cirrus_driver;

static bool probe(const struct pci_func *p)
{
    if (!p || p->vendor!=0x1013 || p->class_code!=3 || p->subclass ||
        (p->command&3)!=3 || (p->header_type&0x7f)) return false;
    for (unsigned i=0;i<ARRAY_SIZE(matches);i++)
        if (p->device==matches[i].device) return true;
    return false;
}
static const struct pci_func *owner(const struct fb_device *fb)
{
    const struct display_driver *const table[]={&cirrus_driver};
    const struct pci_func *p=0;
    return display_driver_select(fb,table,1,&p) ? p : 0;
}
static bool eligible(const struct fb_device *fb)
{
    const char *why;
    if (display_accel_disabled(&why)) { reason=why; return false; }
    if (engine.quarantined || blocked) { reason="quarantined"; return false; }
    const struct pci_func *p=owner(fb);
    if (!p || !cirrus_layout(fb) ||
        (p->device!=0x00b8 && (fb->bpp==32 || fb->pitch>4095 || fb->size>2u*1024*1024))) {
        reason="unsupported_framebuffer"; return false;
    }
    return true;
}

/* Save and restore each index in one short IRQ section; there are no waits
 * or engine commands with IF cleared. The exclusive port lease covers both
 * the sequencer and graphics index/data pairs, including their gap. */
static uint8_t indexed_read(uint16_t port, uint8_t index)
{
    uint32_t flags=irq_save();
    uint8_t old=inb(port);
    outb(port,index); uint8_t value=inb(port+1); outb(port,old);
    irq_restore(flags);
    return value;
}
static uint8_t read_reg(void *ctx, uint8_t index)
{ (void)ctx; return indexed_read(0x3ce,index); }
static void write_reg(void *ctx, uint8_t index, uint8_t value)
{
    (void)ctx;
    uint32_t flags=irq_save();
    uint8_t old=inb(0x3ce);
    outb(0x3ce,index); outb(0x3cf,value); outb(0x3ce,old);
    irq_restore(flags);
}
static uint64_t now_us(void *ctx)
{
    (void)ctx;
    if (!g_cpu_tsc || !g_tsc_per_ms) return g_ticks*1000;
    uint64_t cycles=ktime_cycles();
    return cycles/g_tsc_per_ms*1000+cycles%g_tsc_per_ms*1000/g_tsc_per_ms;
}
static void drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static bool access_valid(void *ctx)
{
    (void)ctx;
    if (port_claim<0 || !registry_valid(port_claim,port_generation) ||
        !registry_valid(engine.fb.handle,engine.fb.generation)) return false;
    const struct resource *ports=registry_get((unsigned)port_claim);
    const struct resource *fb=registry_get((unsigned)engine.fb.handle);
    return ports && ports->state==RS_ACTIVE && fb && fb->state==RS_ACTIVE &&
        fb->type==RES_MMIO && fb->start==engine.fb.phys &&
        fb->end==engine.fb.phys+engine.fb.size;
}
static bool port_idle(int h, gen_t generation)
{
    return h==port_claim && generation==port_generation &&
        (!engine.touched || engine.cpu_safe) && !engine.active;
}
static void release_port(void)
{
    if (port_claim<0) return;
    const struct resource *r=registry_get((unsigned)port_claim);
    if (r && r->state==RS_ACTIVE && registry_quiesce(port_claim,port_generation,port_idle)) return;
    if (!registry_release(port_claim,port_generation)) port_claim=-1;
}
static int stop(void *ctx)
{
    struct cirrus_engine *s=ctx;
    int e=s->ops.sync(s);
    s->active=false;
    if (!s->cpu_safe) return e ? e : -EIO;
    release_port();
    return port_claim<0 ? 0 : -EIO;
}
static uint32_t pixel_read(const struct fb_device *d, unsigned x, unsigned y)
{
    volatile uint8_t *p=d->mapped+y*d->pitch+x*(d->bpp/8);
    uint32_t value=0;
    for (unsigned b=0;b<d->bpp/8;b++) value|=(uint32_t)p[b]<<(8*b);
    return value;
}
static void pixel_write(const struct fb_device *d, unsigned x, unsigned y, uint32_t value)
{
    volatile uint8_t *p=d->mapped+y*d->pitch+x*(d->bpp/8);
    for (unsigned b=0;b<d->bpp/8;b++) p[b]=(uint8_t)(value>>(8*b));
}
struct activation { const struct fb_device *fb; uint16_t device; };
static int activation_probe(void *ctx)
{
    const struct activation *a=ctx;
    const struct fb_device *d=a->fb;
    struct cirrus_io io={0,read_reg,write_reg,now_us,drain,access_valid};
    int e=cirrus_engine_start(&engine,d,&io,a->device);
    engine.ops.stop=stop;
    if (e) return e;
    uint32_t saved[16];
    unsigned x=d->width-8, y=d->height-2;
    for (unsigned i=0;i<16;i++) saved[i]=pixel_read(d,x+i%8,y+i/8);
    struct fb_rect r={(int32_t)x,(int32_t)y,4,2};
    uint32_t mask=d->bpp==32 ? UINT32_MAX : (1u<<d->bpp)-1;
    uint32_t colour=(saved[0]^0x005aa536)&mask;
    e=engine.ops.fill_rect(&engine,&r,colour);
    if (!e) e=engine.ops.copy_rect(&engine,&r,(int32_t)x+4,(int32_t)y);
    if (!e) for (unsigned i=0;i<16;i++)
        if (pixel_read(d,x+i%8,y+i/8)!=colour) { e=-EIO; break; }
    /* A reset may stop a partially executed copy. Restore only after the
     * independent idle proof, while drawing exclusion still holds. */
    if (!engine.ops.sync(&engine) && engine.cpu_safe) {
        for (unsigned i=0;i<16;i++) pixel_write(d,x+i%8,y+i/8,saved[i]);
        framebuffer_write_drain();
    } else e=-EIO;
    if (e) cirrus_engine_quarantine(&engine,"activation_comparison_failed");
    return e;
}
static const struct accel_ops *start(const struct fb_device *fb, struct vfs *vfs)
{
    (void)vfs;
    if (!persisted || !eligible(fb)) return 0;
    const struct pci_func *p=owner(fb);
    if (!p) return 0;
    port_claim=registry_claim(RES_PORT,0x3c4,0x3d0,"cirrus-2d",false);
    if (port_claim<0) { reason="port_claim_failed"; return 0; }
    port_generation=registry_get((unsigned)port_claim)->generation;
    if (registry_activate(port_claim,port_generation)) {
        /* Activation failed before register access. Keep the reservation
         * quarantined; no engine exists and CPU drawing is still safe. */
        registry_quarantine(port_claim,port_generation);
        port_claim=-1; blocked=true; reason="port_activate_failed"; return 0;
    }
    /* Never unlock extensions or take an existing native cursor away from
     * another owner. Both refusals issue no engine command. */
    if (indexed_read(0x3c4,6)!=0x12 || (indexed_read(0x3c4,0x12)&1)) {
        release_port(); reason="firmware_state_unsupported"; return 0;
    }
    struct activation a={fb,p->device};
    int e=fbdev_accel_probe(fb,activation_probe,&a);
    if (e) {
        reason=engine.reason;
        if (!engine.quarantined) { engine.cpu_safe=true; release_port(); }
        return 0;
    }
    reason="none";
    return &engine.ops;
}
static const struct accel_ops *fallback(void)
{
    if (port_claim<0) return 0;
    if (engine.ops.sync) {
        (void)engine.ops.sync(&engine);
        if (!engine.cpu_safe) return &engine.ops;
    }
    engine.active=false;
    release_port();
    return 0; /* Idle is proved even if resource release was refused. */
}

/* Preserve the part A anonymous marker, including interrupted legacy
 * attempts. Only this boot's successfully committed attempt may clear it.
 * Every call is worker context, outside the presenter lock. */
static int marker(struct vfs *vfs, bool clear)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    int e=vfs_table_init(vfs,&marker_table,2); if (e) return e;
    if (!clear) {
        struct fat_entry entry;
        e=vfs_stat(&marker_table,"C:/system/settings/accel.cfg",&entry);
        if (!e && entry.size) { e=-EIO; blocked=true; goto done; }
        if (e && e!=-FS_ENOENT) goto done;
    }
    if (vfs->volumes[2]->readonly) { e=-FS_EROFS; goto done; }
    int fd=vfs_open(&marker_table,"C:/system/settings/accel.cfg",VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { e=fd; goto done; }
    if (clear) e=vfs_truncate(&marker_table,fd,0);
    else {
        const char text[]="accel=attempt\ndriver=cirrus\n";
        size_t written=0;
        e=vfs_write(&marker_table,fd,text,sizeof(text)-1,&written);
        if (!e && written!=sizeof(text)-1) e=-FS_EIO;
    }
    if (!e) e=vfs_commit(&marker_table,fd);
    int closed=vfs_close(&marker_table,fd); if (!e) e=closed;
done:
    vfs_table_destroy(&marker_table);
    return e;
}
static int attempt_begin(struct vfs *vfs)
{
    if (!eligible(fbdev_get()) || persisted || engine.active) return -EIO;
    int e=marker(vfs,false);
    if (e) { blocked=true; reason="marker_unavailable"; }
    else persisted=true;
    return e;
}
static int attempt_end(struct vfs *vfs)
{
    if (!persisted || !engine.active || engine.quarantined) return -EIO;
    int e=marker(vfs,true);
    if (!e) persisted=false;
    else { blocked=true; reason="marker_clear_failed"; }
    return e;
}
static int attempt_failed(struct vfs *vfs)
{ (void)vfs; blocked=true; return 0; }
static void quarantine(const char *why)
{ blocked=true; cirrus_engine_quarantine(&engine,why); reason=why; }
static bool summary(bool *ready, bool *used, const char **why)
{
    *ready=engine.active && !engine.quarantined;
    *used=false; /* Generic presenter counts establish actual use. */
    *why=engine.quarantined ? engine.reason : reason;
    return true;
}
static void snapshot(struct ciuki_accel_info *out)
{
    *out=(struct ciuki_accel_info){.driver=CIUKI_ACCEL_DRIVER_LEGACY_NATIVE,
        .capabilities=engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0,
        .quarantined=engine.quarantined,.generation=engine.fb.generation,
        .operations=engine.operations,.idle_waits=engine.waits};
}
static void record(void)
{
    klog("[accel] driver=cirrus ready=%u quarantined=%u cpu_ready=%u ops=%llu waits=%llu timeouts=%llu status=%02x reason=%s",
        engine.active,engine.quarantined,engine.cpu_safe,engine.operations,
        engine.waits,engine.timeouts,engine.status,engine.quarantined ? engine.reason : reason);
}
const struct display_driver cirrus_driver={
    .id="cirrus",.name="Cirrus Logic GD54xx",.matches=matches,
    .match_count=ARRAY_SIZE(matches),.framebuffer_bar=0,.probe=probe,.eligible=eligible,
    .start=start,.fallback=fallback,.attempt_begin=attempt_begin,.attempt_end=attempt_end,
    .attempt_failed=attempt_failed,.quarantine=quarantine,.summary=summary,
    .snapshot=snapshot,.record=record
};
