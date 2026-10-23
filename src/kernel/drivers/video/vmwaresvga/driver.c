/* CiukiOS registry/resource adaptation of the MIT SVGA II protocol.
 * No X.Org, QEMU or VirtualBox implementation code imported.
 * Origin and decisions: docs/design/vmware-svga.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "engine.h"
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/swdib.h>
#include "../../../fs/vfs.h"
static struct vmsvga_engine engine={.cpu_safe=true,.reason="pending"};
static struct vmsvga_ports ports;
static struct vmsvga_io io;
static int claims[2]={-1,-1};
static gen_t generations[2];
static void *fifo_window;
static uint32_t fifo_capacity, fifo_bytes;
static bool authorized, marker_owned, blocked, logged, logged_quarantine;
static struct vfs_table marker_table;
#define MARKER "C:/system/settings/vmsvga.cfg"
static const struct display_match matches[]={{0x15ad,0x0405,3,255}};
static bool probe(const struct pci_func *p)
{
    return p && p->vendor==0x15ad && p->device==0x0405 && p->class_code==3 &&
        !(p->header_type&0x7fu) && (p->command&3u)==3u &&
        (p->bar[0]&3u)==1u && (p->bar[0]&~3u)>=0x100u &&
        (p->bar[0]&~3u)<=0xfff0u && !(p->bar[1]&7u) && !(p->bar[2]&7u) &&
        (p->bar[1]&~15u) && (p->bar[2]&~15u);
}
static const struct pci_func *owner(const struct fb_device *fb)
{
    const struct pci_func *p=0;
    const struct display_driver *const table[]={&vmwaresvga_driver};
    return display_driver_select(fb,table,1,&p) ? p : 0;
}
static bool eligible(const struct fb_device *fb)
{
    const char *why=0;
    if (blocked || engine.quarantined || display_accel_disabled(&why)) return false;
    if (!owner(fb) || !fb->mapped || !fb->width || !fb->height ||
        (fb->bpp!=16 && fb->bpp!=24 && fb->bpp!=32)) {
        engine.reason="unsupported_framebuffer"; return false;
    }
    return true;
}
static bool safe_extent(uint32_t base,uint32_t bytes,const struct fb_device *fb)
{
    if (base<0x100000 || !bytes || bytes>UINT32_MAX-base || (base&4095u)) return false;
    uint64_t end=(uint64_t)base+bytes;
    if (base<g_boot.kernel_end && end>g_boot.kernel_start) return false;
    if (base<(uint64_t)fb->phys+g_boot.display.vram_bytes && end>fb->phys) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *e=&g_boot.e820[i];
        if (e->type==CBI_E820_RAM && base<e->base+e->length && end>e->base) return false;
    }
    return true;
}
static bool resources_valid(void *ctx)
{
    (void)ctx;
    if (!ports.fifo) return false;
    for (unsigned i=0;i<2;i++)
        if (claims[i]<0 || !registry_valid(claims[i],generations[i])) return false;
    return registry_valid(engine.fb.handle,engine.fb.generation);
}
static bool stopped_proof(int handle,gen_t generation)
{
    for (unsigned i=0;i<2;i++) if (claims[i]==handle && generations[i]==generation)
        return engine.cpu_safe && !engine.pending && !ports.fifo;
    return false;
}
static int release_resources(void)
{
    if (!engine.cpu_safe || engine.pending) return -EIO;
    if (ports.fifo) { vmm_revoke_mmio(fifo_window,fifo_bytes); ports.fifo=0; }
    int e=0;
    for (unsigned i=0;i<2;i++) if (claims[i]>=0) {
        const struct resource *r=registry_get((unsigned)claims[i]);
        int result=0;
        if (r && r->state==RS_ACTIVE) result=registry_quiesce(claims[i],generations[i],stopped_proof);
        if (!result) result=registry_release(claims[i],generations[i]);
        if (result) { registry_quarantine(claims[i],generations[i]); e=-EIO; }
        else claims[i]=-1;
    }
    return e;
}
static int runtime_stop(void *ctx)
{
    struct vmsvga_engine *s=ctx;
    if (s->cpu_safe && !ports.fifo && claims[0]<0 && claims[1]<0) return 0;
    /* Core stop is intentionally called once here: ops->stop is the single
     * authority, including mode transitions and failed-marker cleanup. */
    if (s->cursor) {
        if (!s->quarantined) {
            int hidden=s->ops.cursor_show(s,false);
            if (hidden) return hidden;
        }
        else vmsvga_engine_quarantine(s,s->reason);
    }
    int e=s->ops.sync(s);
    if (e) return e;
    s->active=false; s->cursor_loaded=false;
    return release_resources();
}
static int marker_io(struct vfs *vfs,bool clear,bool *previous)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    int e=vfs_table_init(vfs,&marker_table,2);
    if (e) return e;
    if (!clear) {
        struct fat_entry entry;
        e=vfs_stat(&marker_table,MARKER,&entry);
        if (!e && entry.size) { *previous=true; goto out; }
        if (e && e!=-FS_ENOENT) goto out;
    }
    if (vfs->volumes[2]->readonly) { e=-FS_EROFS; goto out; }
    int fd=vfs_open(&marker_table,MARKER,VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { e=fd; goto out; }
    if (clear) e=vfs_truncate(&marker_table,fd,0);
    else {
        static const char text[]="driver=vmwaresvga\npci=15ad:0405\nstate=attempt\n";
        size_t done=0;
        e=vfs_write(&marker_table,fd,text,sizeof(text)-1,&done);
        if (!e && done!=sizeof(text)-1) e=-FS_EIO;
    }
    if (!e) e=vfs_commit(&marker_table,fd);
    int closed=vfs_close(&marker_table,fd);
    if (!e) e=closed;
out:
    vfs_table_destroy(&marker_table); return e;
}
static int attempt_begin(struct vfs *vfs)
{
    authorized=false;
    if (!eligible(fbdev_get())) return -EOPNOTSUPP;
    bool previous=false;
    int e=marker_io(vfs,false,&previous);
    if (e || previous) {
        blocked=true; engine.reason=previous ? "previous_attempt" : "marker_unavailable";
        return e ? e : -EIO;
    }
    authorized=marker_owned=true; return 0;
}
static int attempt_end(struct vfs *vfs)
{
    if (!marker_owned || engine.quarantined || !engine.cpu_safe) return -EIO;
    bool previous=false;
    int e=marker_io(vfs,true,&previous);
    if (e) { blocked=true; vmsvga_engine_quarantine(&engine,"marker_clear_failed"); }
    else marker_owned=false;
    return e;
}
static int attempt_failed(struct vfs *vfs)
{ (void)vfs; authorized=false; blocked=true; return 0; /* retain durable marker */ }
static const struct accel_ops *fallback(void)
{
    if (!engine.initialized) return engine.cpu_safe ? 0 : &engine.ops;
    if (engine.cpu_safe && !ports.fifo && claims[0]<0 && claims[1]<0) return 0;
    vmsvga_engine_quarantine(&engine,engine.reason);
    /* Use the same stop authority as a mode change. An idle proof also
     * revokes the private register mapping before software takes over. */
    (void)engine.ops.stop(&engine);
    return engine.cpu_safe ? 0 : &engine.ops;
}
/* Activation comparison never dispatches through the software fallback.
 * Scratch is RAM; scanout is saved/restored only while the presenter owns it. */
static int activation_probe(void *arg)
{
    struct vmsvga_engine *s=arg;
    uint8_t saved[16*16*4],gold[16*16*4];
    unsigned bytes=s->fb.bpp/8, row=16*bytes;
    struct fb_device ref=s->fb; ref.width=ref.height=16; ref.pitch=row;
    ref.size=16*row; ref.mapped=gold;
    if (s->ops.sync(s) || !s->cpu_safe) return -EIO;
    for (unsigned y=0;y<16;y++) memcpy(saved+y*row,s->fb.mapped+y*s->fb.pitch,row);
    memcpy(gold,saved,ref.size);
    int e=0;
    for (unsigned i=0;i<4 && !e;i++) {
        struct fb_rect r={0,0,i<2 ? 16 : 12,i<2 ? 16 : 12};
        framebuffer_write_drain();
        if (i<2) {
            if (i) r=(struct fb_rect){0,0,4,4};
            uint32_t c=swdib_pack(&ref,i ? 0xcb7123 : 0x36a5e7);
            swdib_fill(&ref,&r,c); e=s->ops.fill_rect(s,&r,c);
        } else {
            int dst=i==2 ? 4 : 0;
            if (i==3) r.x=r.y=4;
            swdib_copy(&ref,&r,dst,dst); e=s->ops.copy_rect(s,&r,dst,dst);
        }
        if (!e) e=s->ops.sync(s);
        if (!e && !s->cpu_safe) e=-EIO;
        if (!e) for (unsigned y=0;y<16;y++)
            if (memcmp(gold+y*row,s->fb.mapped+y*s->fb.pitch,row)) { e=-EIO; break; }
    }
    if (!s->ops.sync(s) && s->cpu_safe) {
        for (unsigned y=0;y<16;y++) memcpy(s->fb.mapped+y*s->fb.pitch,saved+y*row,row);
        framebuffer_write_drain();
    } else e=-EIO;
    if (e) vmsvga_engine_quarantine(s,"activation_comparison_failed");
    return e;
}
static const struct accel_ops *start(const struct fb_device *fb,struct vfs *vfs)
{
    (void)vfs;
    if (!authorized || !eligible(fb) || fb->width<16 || fb->height<16) return 0;
    authorized=false;
    const struct pci_func *p=owner(fb);
    if (!p) return 0;
    engine.fb=*fb;
    ports.base=(uint16_t)(p->bar[0]&~3u);
    claims[0]=registry_claim(RES_PORT,ports.base,ports.base+16u,"vmwaresvga-io",false);
    if (claims[0]<0) { engine.reason="port_claim_failed"; return 0; }
    generations[0]=registry_get((unsigned)claims[0])->generation;
    if (registry_activate(claims[0],generations[0])) goto failure;
    io=vmsvga_port_io(&ports);
    /* Read-only protocol/resource qualification before mapping FIFO memory. */
    fifo_bytes=io.read(io.ctx,SVGA_REG_MEM_SIZE);
    uint32_t base=p->bar[2]&~15u;
    if (io.read(io.ctx,SVGA_REG_ID)!=SVGA_ID_2 ||
        io.read(io.ctx,SVGA_REG_MEM_START)!=base || fifo_bytes<16384 ||
        fifo_bytes>VMSVGA_FIFO_LIMIT || (fifo_bytes&4095u) || !safe_extent(base,fifo_bytes,fb)) goto failure;
    claims[1]=registry_claim(RES_MMIO,base,base+fifo_bytes,"vmwaresvga-fifo",false);
    if (claims[1]<0) goto failure;
    generations[1]=registry_get((unsigned)claims[1])->generation;
    if (registry_activate(claims[1],generations[1])) goto failure;
    if (!fifo_window) { fifo_capacity=fifo_bytes; fifo_window=vmm_reserve_mmio(fifo_capacity); }
    if (fifo_bytes>fifo_capacity || !fifo_window) goto failure;
    ports.fifo=vmm_replace_mmio(fifo_window,fifo_capacity,base,fifo_bytes);
    if (!ports.fifo) goto failure;
    io.valid=resources_valid;
    int initialized=vmsvga_engine_init(&engine,&io,fb,base,fifo_bytes);
    engine.ops.stop=runtime_stop;
    if (initialized) goto failure;
    if (fbdev_accel_probe(fb,activation_probe,&engine)) {
        (void)fallback(); blocked=true; return 0;
    }
    return &engine.ops;
failure:
    if (!engine.reason || !strncmp(engine.reason,"none",5) || !strncmp(engine.reason,"pending",8))
        engine.reason="resource_or_protocol_refused";
    (void)release_resources(); return 0;
}
static void quarantine(const char *why)
{ blocked=true; vmsvga_engine_quarantine(&engine,why); }
static bool summary(bool *ready,bool *used,const char **why)
{ *ready=engine.active && !engine.quarantined; *used=!!(engine.fills+engine.copies); *why=engine.reason; return true; }
static void snapshot(struct ciuki_accel_info *out)
{
    memset(out,0,sizeof(*out)); out->driver=CIUKI_ACCEL_DRIVER_LEGACY_NATIVE;
    out->capabilities=engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0;
    out->quarantined=engine.quarantined; out->generation=engine.fb.generation;
    out->operations=(uint64_t)engine.fills+engine.copies;
    out->idle_waits=engine.packets;
}
static void record(void)
{
    if (logged && logged_quarantine==engine.quarantined) return;
    if (!engine.active && !blocked && !engine.quarantined && !engine.initialized) return;
    logged=true; logged_quarantine=engine.quarantined;
    klog("[accel] driver=vmwaresvga ready=%u quarantined=%u cpu_ready=%u reason=%s packets=%u",
        engine.active,engine.quarantined,engine.cpu_safe,engine.reason,engine.packets);
}
static int cursor_test(const struct accel_ops *ops,uint64_t until,bool (*cancel)(void *),void *ctx)
{
    if (ops!=&engine.ops) return -EOPNOTSUPP;
    return vmsvga_engine_cursor_test(&engine,until,cancel,ctx);
}
const struct display_driver vmwaresvga_driver={
    .id="vmwaresvga",.name="VMware SVGA II",.matches=matches,.match_count=1,.framebuffer_bar=1,
    .probe=probe,.eligible=eligible,.start=start,.fallback=fallback,
    .attempt_begin=attempt_begin,.attempt_end=attempt_end,.attempt_failed=attempt_failed,
    .record=record,.quarantine=quarantine,.summary=summary,.snapshot=snapshot,.cursor_test=cursor_test
};
