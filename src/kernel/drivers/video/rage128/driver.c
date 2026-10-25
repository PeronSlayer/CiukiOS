/*
	Copyright 2007-2011 Haiku, Inc.  All rights reserved.
	Distributed under the terms of the MIT license.

	Authors:
	Gerald Zajac
*/
/* Origin: Haiku r1beta5 src/add-ons/kernel/drivers/graphics/ati/driver.cpp,
 * Rage 128 entries of the PCI table only. CiukiOS resource/lifecycle glue is
 * new. Full notices: docs/licenses/haiku-rage128-NOTICES.txt. */
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/cpu.h>
#include <ciuki/timing.h>
#include "../../../fs/vfs.h"
#include "rage128.h"
#include "regs.h"

#include "../render3d.h"
static const struct render3d_family render_family = {
    .status=R3D_NOT_IMPLEMENTED, .potential_units=2, .potential_texture_size=1024
};

static const struct display_match matches[]={
    {0x1002,0x4c45,3,255}, {0x1002,0x4c46,3,255},
    {0x1002,0x4d46,3,255}, {0x1002,0x4d4c,3,255},
    {0x1002,0x5041,3,255}, {0x1002,0x5042,3,255}, {0x1002,0x5043,3,255},
    {0x1002,0x5044,3,255}, {0x1002,0x5045,3,255}, {0x1002,0x5046,3,255},
    {0x1002,0x5047,3,255}, {0x1002,0x5048,3,255}, {0x1002,0x5049,3,255},
    {0x1002,0x504a,3,255}, {0x1002,0x504b,3,255}, {0x1002,0x504c,3,255},
    {0x1002,0x504d,3,255}, {0x1002,0x504e,3,255}, {0x1002,0x504f,3,255},
    {0x1002,0x5050,3,255}, {0x1002,0x5051,3,255}, {0x1002,0x5052,3,255},
    {0x1002,0x5053,3,255}, {0x1002,0x5054,3,255}, {0x1002,0x5055,3,255},
    {0x1002,0x5056,3,255}, {0x1002,0x5057,3,255}, {0x1002,0x5058,3,255},
    {0x1002,0x5245,3,255}, {0x1002,0x5246,3,255}, {0x1002,0x5247,3,255},
    {0x1002,0x524b,3,255}, {0x1002,0x524c,3,255},
    {0x1002,0x5345,3,255}, {0x1002,0x5346,3,255}, {0x1002,0x5347,3,255},
    {0x1002,0x5348,3,255}, {0x1002,0x534b,3,255}, {0x1002,0x534c,3,255},
    {0x1002,0x534d,3,255}, {0x1002,0x534e,3,255},
    {0x1002,0x5446,3,255}, {0x1002,0x544c,3,255}, {0x1002,0x5452,3,255},
    {0x1002,0x5453,3,255}, {0x1002,0x5454,3,255}, {0x1002,0x5455,3,255}
};
static struct r128_engine engine={.cpu_safe=true,.cursor_hidden=true,.reason="pending"};
static volatile uint32_t *registers;
static volatile uint8_t *cursor_mapping;
static void *register_window, *cursor_window;
static int register_claim=-1, cursor_claim=-1;
static gen_t register_gen, cursor_gen;
static struct fb_device active_fb;
static int (*engine_stop)(void *);
static struct vfs *marker_vfs, *diagnostic_vfs;
static struct vfs_table marker_table;
static bool persisted, blocked, fault_recorded;
#define ATTEMPT_PATH "C:/system/settings/r128.cfg"

static bool probe(const struct pci_func *p)
{
    if (!p || p->vendor!=0x1002 || p->class_code!=3 || p->subclass ||
        (p->header_type&0x7f) || !(p->command&2u)) return false;
    for (unsigned i=0;i<ARRAY_SIZE(matches);i++) if (p->device==matches[i].device) return true;
    return false;
}
static bool safe_extent(uint32_t base, uint32_t bytes)
{
    if (base<0x100000 || !bytes || bytes>UINT32_MAX-base) return false;
    uint64_t end=(uint64_t)base+bytes;
    if (base<g_boot.kernel_end && end>g_boot.kernel_start) return false;
    for (unsigned i=0;i<g_boot.e820_count;i++) {
        const struct ciuki_e820 *e=&g_boot.e820[i];
        if (e->type==CBI_E820_RAM && base<e->base+e->length && end>e->base) return false;
    }
    return true;
}
static bool eligible(const struct fb_device *d)
{
    const char *reason;
    if (display_accel_disabled(&reason)) { engine.reason=reason; return false; }
    if (engine.quarantined || blocked) return false;
    const struct display_driver *const table[]={&rage128_driver};
    const struct pci_func *p=0;
    if (!display_driver_select(d,table,1,&p) || !probe(p)) {
        engine.reason="framebuffer_not_owned"; return false;
    }
    uint32_t base=p->bar[2]&~15u;
    if ((p->bar[2]&15u) || (base&(R128_MMIO_BYTES-1)) ||
        !safe_extent(base,R128_MMIO_BYTES) ||
        (base<(uint64_t)d->phys+d->size && (uint64_t)base+R128_MMIO_BYTES>d->phys)) {
        engine.reason="invalid_register_bar"; return false;
    }
    if (!r128_format(d)) { engine.reason="unsupported_mode"; return false; }
    return true;
}
static uint32_t read_reg(void *ctx, unsigned reg)
{ (void)ctx; return registers[reg/4]; }
static void write_reg(void *ctx, unsigned reg, uint32_t value)
{ (void)ctx; registers[reg/4]=value; }
static uint64_t now_us(void *ctx)
{
    (void)ctx;
    if (g_cpu_tsc && g_tsc_per_ms) {
        uint64_t cycles=read_tsc_serialized();
        return (cycles/g_tsc_per_ms)*1000+(cycles%g_tsc_per_ms)*1000/g_tsc_per_ms;
    }
    return g_ticks*1000;
}
static void drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static bool access_valid(void *ctx)
{
    (void)ctx;
    const struct fb_device *d=fbdev_get();
    return registers && registry_valid(register_claim,register_gen) && d && d->present &&
        d->generation==active_fb.generation && d->handle==active_fb.handle &&
        registry_valid(d->handle,d->generation) && d->phys==active_fb.phys &&
        d->pitch==active_fb.pitch && d->size==active_fb.size &&
        d->mapped==active_fb.mapped && d->width==active_fb.width &&
        d->height==active_fb.height && d->bpp==active_fb.bpp &&
        (!cursor_mapping || registry_valid(cursor_claim,cursor_gen));
}
static bool release_proof(int handle, gen_t generation)
{
    (void)generation;
    if (!engine.cpu_safe || !engine.cursor_hidden) return false;
    if (handle==cursor_claim && cursor_mapping) {
        vmm_revoke_mmio((void *)cursor_mapping,PAGE_SIZE); cursor_mapping=0;
    }
    if (handle==register_claim && registers) {
        vmm_revoke_mmio((void *)registers,R128_MMIO_BYTES); registers=0;
    }
    return true;
}
static void release_claim(int *handle, gen_t generation)
{
    if (*handle<0) return;
    const struct resource *r=registry_get((unsigned)*handle);
    if (r && r->generation==generation) {
        int e=0;
        if (r->state==RS_ACTIVE) e=registry_quiesce(*handle,generation,release_proof);
        if (!e) e=registry_release(*handle,generation);
        if (e) { (void)registry_quarantine(*handle,generation); return; }
    }
    *handle=-1;
}
static void release_access(void)
{
    release_claim(&cursor_claim,cursor_gen);
    release_claim(&register_claim,register_gen);
    engine.cursor=0;
}
static int stop_driver(void *ctx)
{
    struct r128_engine *s=ctx;
    int e=engine_stop ? engine_stop(s) : -EIO;
    if (e) return e;
    release_access();
    s->touched=false; s->cursor_loaded=false;
    if (!s->quarantined) s->reason="stopped";
    return register_claim<0 && cursor_claim<0 ? 0 : -EIO;
}
static int setup(void *ctx)
{
    (void)ctx;
    struct r128_io io={0,read_reg,write_reg,now_us,drain,access_valid};
    int e=r128_engine_init(&engine,&active_fb,io,cursor_mapping,engine.cursor_offset);
    if (engine.touched) { engine_stop=engine.ops.stop; engine.ops.stop=stop_driver; }
    return e;
}
static const struct accel_ops *start(const struct fb_device *d, struct vfs *unused)
{
    (void)unused;
    if (!persisted || !eligible(d)) return 0;
    const struct pci_func *p=display_driver_owner();
    if (!p || !probe(p)) return 0;
    active_fb=*d;
    uint32_t base=p->bar[2]&~15u;
    if (register_claim>=0 || cursor_claim>=0) { engine.reason="retained_resources"; return 0; }
    register_claim=registry_claim(RES_MMIO,base,base+R128_MMIO_BYTES,"rage128-registers",false);
    if (register_claim<0) { engine.reason="register_claim_failed"; return 0; }
    register_gen=registry_get((unsigned)register_claim)->generation;
    if (registry_activate(register_claim,register_gen)) { engine.reason="register_activate_failed"; goto clean; }
    if (!register_window) register_window=vmm_reserve_mmio(R128_MMIO_BYTES);
    registers=register_window ? vmm_replace_mmio(register_window,R128_MMIO_BYTES,base,R128_MMIO_BYTES) : 0;
    if (!registers) { engine.reason="register_map_failed"; goto clean; }
    uint32_t memory=read_reg(0,R128_CONFIG_MEMSIZE);
    if (!memory || memory==UINT32_MAX || memory>0x08000000u ||
        d->size>memory || d->size>g_boot.display.vram_bytes) {
        engine.reason="invalid_vram_extent"; goto clean;
    }
    /* A failed optional reservation disables only the cursor. Never overlap
     * the active framebuffer lease or infer memory through destructive probes. */
    uint32_t extent=memory<g_boot.display.vram_bytes ? memory : g_boot.display.vram_bytes;
    if (extent>=PAGE_SIZE && safe_extent(d->phys,extent)) {
        uint32_t offset=(extent-PAGE_SIZE)&~(PAGE_SIZE-1u);
        if (offset>=PAGE_ALIGN_UP(d->size)) {
            uint32_t phys=d->phys+offset;
            cursor_claim=registry_claim(RES_MMIO,phys,phys+PAGE_SIZE,"rage128-cursor",false);
            if (cursor_claim>=0) {
                cursor_gen=registry_get((unsigned)cursor_claim)->generation;
                if (!registry_activate(cursor_claim,cursor_gen)) {
                    if (!cursor_window) cursor_window=vmm_reserve_mmio(PAGE_SIZE);
                    cursor_mapping=cursor_window ? vmm_replace_mmio(cursor_window,PAGE_SIZE,phys,PAGE_SIZE) : 0;
                }
                if (!cursor_mapping) release_claim(&cursor_claim,cursor_gen);
                else engine.cursor_offset=offset;
            }
        }
    }
    /* All engine writes, including setup, happen under presenter + drawing
     * exclusion. The donor's destructive reset is deliberately not imported. */
    if (!fbdev_accel_probe(d,setup,0)) return &engine.ops;
    if (engine.touched) return 0;
clean:
    release_access();
    return 0;
}
static const struct accel_ops *fallback(void)
{
    if (!engine.touched) { release_access(); return 0; }
    /* Explicit bounded proof, before the generic software engine can run.
     * An unsafe guard remains installed if it fails. Never replay a COPY. */
    (void)engine.ops.sync(&engine);
    return &engine.ops;
}
static void quarantine(const char *reason) { r128_quarantine(&engine,reason); }
static bool summary(bool *ready, bool *used, const char **reason)
{
    *ready=engine.active && !engine.quarantined; *used=false;
    *reason=engine.reason ? engine.reason : "pending";
    return true;
}
static void snapshot(struct ciuki_accel_info *out)
{
    out->driver=engine.touched ? CIUKI_ACCEL_DRIVER_LEGACY_NATIVE : CIUKI_ACCEL_DRIVER_NONE;
    out->quarantined=engine.quarantined;
    out->operations=engine.operations; out->idle_waits=engine.idle_waits; out->fallbacks=engine.fallbacks;
}

/* Same durable commit/close discipline as part A's existing adapter, in a
 * separate file. A nonempty or unreadable marker is never cleared on begin;
 * the legacy adapter's accel.cfg is never touched or migrated. */
static int marker_read(struct vfs *vfs, bool *retained)
{
    *retained=false;
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    int e=vfs_table_init(vfs,&marker_table,2); if (e) return e;
    struct fat_entry entry;
    e=vfs_stat(&marker_table,ATTEMPT_PATH,&entry);
    if (!e) *retained=entry.size!=0;
    else if (e==-FS_ENOENT) e=0;
    vfs_table_destroy(&marker_table);
    return e;
}
static int marker_write(struct vfs *vfs, bool clear)
{
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    if (vfs->volumes[2]->readonly) return -FS_EROFS;
    int e=vfs_table_init(vfs,&marker_table,2); if (e) return e;
    int fd=vfs_open(&marker_table,ATTEMPT_PATH,VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
    if (fd<0) { e=fd; goto out; }
    if (clear) e=vfs_truncate(&marker_table,fd,0);
    else {
        const struct pci_func *p=display_driver_owner();
        char text[128];
        int length=ksnprintf(text,sizeof(text),"driver=rage128\npci=%04x:%04x\nattempt=1\n",
            p ? p->vendor : 0,p ? p->device : 0);
        size_t done=0;
        e=vfs_write(&marker_table,fd,text,(size_t)length,&done);
        if (!e && done!=(size_t)length) e=-FS_EIO;
    }
    if (!e) e=vfs_commit(&marker_table,fd);
    int closed=vfs_close(&marker_table,fd); if (!e) e=closed;
out:
    vfs_table_destroy(&marker_table);
    return e;
}
static int attempt_begin(struct vfs *vfs)
{
    const char *reason;
    if (display_accel_disabled(&reason)) { engine.reason=reason; return -EOPNOTSUPP; }
    if (blocked || engine.quarantined || engine.active) return -EIO;
    bool retained;
    int e=marker_read(vfs,&retained);
    if (!e && retained) { e=-EIO; engine.reason="previous_attempt"; }
    if (!e) e=marker_write(vfs,false);
    if (e) { blocked=true; if (!retained) engine.reason="marker_unavailable"; return e; }
    marker_vfs=vfs; persisted=true;
    return 0;
}
static int attempt_end(struct vfs *vfs)
{
    if (engine.quarantined || blocked || !engine.cpu_safe) return -EIO;
    if (!persisted) return 0; /* Already cleared by the native-only diagnostic. */
    int e=marker_write(vfs,true);
    if (!e) persisted=false;
    return e;
}
static int attempt_failed(struct vfs *vfs)
{
    (void)vfs; blocked=true; /* Keep the original durable attempt intact. */
    return 0;
}
static void record(void)
{
    if (!engine.quarantined || fault_recorded) return;
    fault_recorded=true;
    int e=marker_write(marker_vfs,false);
    klog("[accel] driver=rage128 quarantined reason=%s cpu_ready=%u marker_error=%d",
        engine.reason,engine.cpu_safe,e);
}
static int cursor_test(const struct accel_ops *ops, uint64_t deadline, bool (*cancel)(void *), void *ctx)
{
    if (!ops || !(ops->capabilities(ops->ctx)&2u)) return -EOPNOTSUPP;
    struct accel_cursor transparent={0};
    int e=0;
    for (unsigned step=0;step<4 && !e;step++) {
        if (g_ticks>=deadline || (cancel && cancel(ctx))) { e=g_ticks>=deadline ? -ETIMEDOUT : -EINTR; break; }
        if (!step) e=ops->cursor_show(ops->ctx,false);
        else if (step==1) e=ops->cursor_set(ops->ctx,&transparent);
        else if (step==2) e=ops->cursor_move(ops->ctx,-2,-4);
        else e=ops->cursor_show(ops->ctx,true);
    }
    int hidden=ops->cursor_show(ops->ctx,false);
    return e ? e : hidden;
}
static int check_begin(struct vfs *vfs, bool *retained)
{
    if (!(g_boot.flags&CBI_F_HW_CHECK) || !retained) return -EPERM;
    const char *reason;
    if (display_accel_disabled(&reason) || blocked || engine.quarantined) return -EIO;
    int e=marker_read(vfs,retained);
    if (!e && *retained) { engine.reason="previous_attempt"; return -EIO; }
    if (!e) diagnostic_vfs=vfs;
    return e;
}
static int selftest(uint64_t deadline, void (*line)(void *, const char *), void *ctx,
                    struct display_selftest_result *out)
{
    *out=(struct display_selftest_result){.first=-1};
    if (!diagnostic_vfs) return out->error=-EPERM;
    uint32_t tested=0;
    out->tried=1;
    int e=fbdev_accel_diagnostic(diagnostic_vfs,deadline,0,0,&tested);
    out->completed=1;
    out->restored=engine.cpu_safe;
    if (!e && (tested&3u)!=3u) e=-EIO;
    if (!e) { out->passed=1; out->first=0; }
    if (e==-ETIMEDOUT) out->timeouts=1;
    out->error=e;
    if (line) {
        char text[128];
        ksnprintf(text,sizeof(text),"Native-only reference: commands=%x cursor=%u cpu_ready=%u error=%d",
            tested&15u,!!(tested&DISPLAY_CHECK_CURSOR),engine.cpu_safe,e);
        line(ctx,text);
    }
    return e;
}
const struct display_driver rage128_driver={
    .render3d=&render_family,
    .id="rage128", .attempt_files={ATTEMPT_PATH}, .name="ATI Rage 128 / Rage 128 Pro",
    .matches=matches,.match_count=ARRAY_SIZE(matches),.framebuffer_bar=0,
    .probe=probe,.eligible=eligible,.start=start,.fallback=fallback,
    .attempt_begin=attempt_begin,.attempt_end=attempt_end,.attempt_failed=attempt_failed,
    .record=record,.quarantine=quarantine,.summary=summary,.snapshot=snapshot,
    .check_begin=check_begin,.selftest=selftest,.cursor_test=cursor_test
};
