/* Production registry + driver lifecycle, with fake PCI/resources and disk.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "vmwaresvga_fixture.h"
#include "../../src/kernel/drivers/video/vmwaresvga/driver.c"
/* The 2D lifecycle fixture does not exercise the independent 3D owner
 * (tests/host/vmwaresvga3d_owner_test.c does). */
static unsigned stops3d;
const struct render3d_backend *vmsvga3d_backend(void) { return 0; }
void vmsvga3d_bind_vfs(struct vfs *v) { (void)v; }
void vmsvga3d_boot_diagnostic(struct vmsvga_engine *s,const struct fb_device *fb) { (void)s; (void)fb; }
int vmsvga3d_owner_stop(struct vmsvga_engine *s) { CHECK(s==&engine); stops3d++; return 0; }
const struct display_driver s3savage_driver={0};
static struct resource resources[4];
static struct pci_func gpu;
static unsigned gpu_count, claim_count, probes, commits, marker_size, disk_step;
static unsigned disk_failure, claim_failure;
static bool readonly_marker, stale, probe_unsafe, in_probe;
static struct fat_volume volume;
static struct vfs fs;
unsigned pci_count(void) { return gpu_count; }
const struct pci_func *pci_get(unsigned i) { return i<gpu_count ? &gpu : 0; }
const struct resource *registry_get(unsigned i) { return i<ARRAY_SIZE(resources) ? &resources[i] : 0; }
bool registry_valid(int h,gen_t gen)
{ return h>=0 && (unsigned)h<ARRAY_SIZE(resources) && resources[h].generation==gen && resources[h].state==RS_ACTIVE && !stale; }
int registry_claim(enum res_type type,uint32_t start,uint32_t end,const char *name,bool share)
{
    CHECK(!share && start<end); claim_count++;
    if (claim_failure==claim_count) return -EBUSY;
    for (unsigned i=1;i<ARRAY_SIZE(resources);i++) if (!resources[i].owner || resources[i].state==RS_RELEASED) {
        gen_t gen=resources[i].generation+1;
        resources[i]=(struct resource){type,start,end,name,gen,RS_CLAIMED,false}; return (int)i;
    }
    CHECK(0); return -ENOSPC;
}
int registry_activate(int h,gen_t gen)
{ CHECK(resources[h].state==RS_CLAIMED && resources[h].generation==gen); resources[h].state=RS_ACTIVE; return 0; }
int registry_quiesce(int h,gen_t gen,registry_idle_proof_fn proof)
{
    CHECK(resources[h].state==RS_ACTIVE && resources[h].generation==gen);
    resources[h].state=RS_QUIESCING;
    if (!proof(h,gen)) { resources[h].state=RS_QUARANTINED; return -EIO; } return 0;
}
int registry_release(int h,gen_t gen)
{
    CHECK(resources[h].generation==gen && (resources[h].state==RS_CLAIMED || resources[h].state==RS_QUIESCING));
    resources[h].state=RS_RELEASED; return 0;
}
int registry_quarantine(int h,gen_t gen)
{ CHECK(resources[h].generation==gen); resources[h].state=RS_QUARANTINED; return 0; }
void *vmm_reserve_mmio(uint32_t size) { CHECK(size==sizeof(fake.fifo)); return fake.fifo; }
void *vmm_replace_mmio(void *win,uint32_t capacity,uint32_t phys,uint32_t size)
{ CHECK(win==fake.fifo && capacity>=size && phys==0xe8000000 && size==sizeof(fake.fifo)); return win; }
void vmm_revoke_mmio(void *win,uint32_t bytes)
{ CHECK(win==fake.fifo && bytes==sizeof(fake.fifo) && engine.cpu_safe && !engine.pending); }
void framebuffer_write_drain(void) { fake_drain(&fake); }
void klog(const char *fmt,...) { (void)fmt; CHECK(!in_probe); }
struct vmsvga_io vmsvga_port_io(struct vmsvga_ports *p)
{ CHECK(p->base==0x4560); return fake_io(); }
const struct fb_device *fbdev_get(void) { return &fake.fb; }
unsigned fbdev_native_capabilities(void)
{ return engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0; }
bool fbdev_cpu_ready(void) { return engine.cpu_safe; }
int fbdev_accel_probe(const struct fb_device *fb,int (*fn)(void *),void *ctx)
{
    CHECK(fb==&fake.fb && commits && marker_size && !in_probe); probes++; in_probe=true;
    int e=fn(ctx); in_probe=false;
    if (e) { const struct accel_ops *guard=fallback(); probe_unsafe=guard && !guard->cpu_ready(guard->ctx); }
    return e;
}
static int disk(void)
{ CHECK(!in_probe); disk_step++; return disk_failure==disk_step ? -FS_EIO : 0; }
int vfs_table_init(struct vfs *vfs,struct vfs_table *table,unsigned drive)
{ CHECK(vfs==&fs && table==&marker_table && drive==2); return disk(); }
void vfs_table_destroy(struct vfs_table *table) { CHECK(table==&marker_table && !in_probe); }
int vfs_stat(struct vfs_table *table,const char *path,struct fat_entry *entry)
{ CHECK(table==&marker_table && !strcmp(path,MARKER)); int e=disk(); entry->size=marker_size; return e; }
int vfs_open(struct vfs_table *table,const char *path,unsigned flags,enum vfs_share share,uint8_t attr)
{ CHECK(table==&marker_table && !strcmp(path,MARKER) && flags==(VFS_WRITE|VFS_CREATE) && share==VFS_DENY_ALL && !attr); int e=disk(); return e ? e : 1; }
int vfs_write(struct vfs_table *table,int fd,const void *data,size_t bytes,size_t *done)
{
    CHECK(table==&marker_table && fd==1 && bytes && strstr(data,"driver=vmwaresvga\npci=15ad:0405"));
    int e=disk(); if (!e) marker_size=(unsigned)bytes; *done=e ? 0 : bytes; return e;
}
int vfs_truncate(struct vfs_table *table,int fd,uint64_t size)
{ CHECK(table==&marker_table && fd==1 && !size && !readonly_marker); int e=disk(); if (!e) marker_size=0; return e; }
int vfs_commit(struct vfs_table *table,int fd)
{ CHECK(table==&marker_table && fd==1); int e=disk(); if (!e) commits++; return e; }
int vfs_close(struct vfs_table *table,int fd) { CHECK(table==&marker_table && fd==1); return disk(); }
static void setup(bool extended)
{
    fake_setup(32,extended); fake.engine=&engine;
    memset(&engine,0,sizeof(engine)); engine.cpu_safe=true; engine.reason="pending";
    claims[0]=claims[1]=-1; authorized=marker_owned=blocked=logged=logged_quarantine=false;
    ports=(struct vmsvga_ports){0}; fifo_window=0; fifo_capacity=fifo_bytes=0;
    claim_count=claim_failure=disk_failure=disk_step=commits=marker_size=probes=0;
    stale=probe_unsafe=readonly_marker=false;
    memset(resources,0,sizeof(resources));
    resources[0]=(struct resource){RES_MMIO,fake.fb.phys,fake.fb.phys+fake.fb.size,"fbdev",42,RS_ACTIVE,false};
    gpu=(struct pci_func){.vendor=0x15ad,.device=0x0405,.class_code=3,.command=3,
        .bar={0x4561,0xe0000000,0xe8000000}}; gpu_count=1;
    g_boot.display.vram_bytes=8*1024*1024;
    memset(&volume,0,sizeof(volume)); volume.mounted=true;
    memset(&fs,0,sizeof(fs)); fs.volumes[2]=&volume;
}
static const struct accel_ops *activate(void)
{
    CHECK(display_driver_bind(&fake.fb)==&vmwaresvga_driver);
    CHECK(eligible(&fake.fb) && !attempt_begin(&fs) && authorized);
    CHECK(marker_size && !fake.writes && !fake.packets);
    const struct accel_ops *ops=start(&fake.fb,0);
    CHECK(ops && probes==1 && !authorized && fake.packets==4 && engine.cpu_safe);
    CHECK(!attempt_end(&fs) && !marker_size && !marker_owned);
    return ops;
}
static void lifecycle(void)
{
    setup(false);
    uint8_t saved[sizeof(fake.pixels)];
    for (unsigned i=0;i<sizeof(saved);i++) saved[i]=fake.pixels[i]=(uint8_t)(i*131+7);
    const struct accel_ops *ops=activate();
    CHECK(!memcmp(saved,fake.pixels,sizeof(saved))); /* activation restores exactly */
    struct ciuki_display_engine_info info;
    display_driver_refresh(); display_driver_info(&info);
    CHECK(!strcmp(info.driver_id,"vmwaresvga") && !strcmp(info.display_name,"VMware SVGA II"));
    CHECK(info.kind==CIUKI_ENGINE_NATIVE && info.pci_vendor==0x15ad && info.pci_device==0x0405 && !info.native_used);
    struct fb_rect r={0,0,4,4}; CHECK(!ops->fill_rect(ops->ctx,&r,0x123456));
    display_driver_count(CIUKI_ENGINE_FILL,true); display_driver_info(&info);
    CHECK(info.native_used && info.native[CIUKI_ENGINE_FILL]);
    CHECK(!ops->stop(ops->ctx) && !ports.fifo && claims[0]<0 && claims[1]<0 && engine.cpu_safe);
    CHECK(!attempt_begin(&fs)); CHECK(start(&fake.fb,0)); CHECK(!attempt_end(&fs)); /* fresh mode lease */
    CHECK(!engine.ops.stop(&engine));
}
static void refusal(void)
{
    setup(false); CHECK(!start(&fake.fb,0) && !fake.writes && !claim_count); /* no durable authorization */
    const uint16_t pairs[][2]={{0x1234,0x1111},{0x1013,0xb8},{0x15ad,0x0710},{0x80ee,0xbeef}};
    for (unsigned i=0;i<ARRAY_SIZE(pairs);i++) {
        setup(false); gpu.vendor=pairs[i][0]; gpu.device=pairs[i][1];
        CHECK(!display_driver_bind(&fake.fb) && !probe(&gpu) && !fake.writes && !claim_count);
    }
    setup(false); gpu_count=2; CHECK(!display_driver_bind(&fake.fb));
    setup(false); gpu.bar[1]+=4096; CHECK(!display_driver_bind(&fake.fb));
    setup(false); gpu.bar[2]|=4; CHECK(!display_driver_bind(&fake.fb));
    setup(false); stale=true; CHECK(!display_driver_bind(&fake.fb));
    setup(false); gpu.command=2; CHECK(!display_driver_bind(&fake.fb));
    setup(false); g_boot.flags|=CBI_F_SAFE_MODE; CHECK(!eligible(&fake.fb) && attempt_begin(&fs) && !claim_count && !disk_step);
    setup(false); strcpy(g_boot.options,"quiet accel=0"); CHECK(!eligible(&fake.fb) && attempt_begin(&fs) && !fake.writes);
    setup(false); marker_size=1; CHECK(attempt_begin(&fs) && blocked && !authorized && marker_size==1 && !claim_count && !fake.writes);
    for (unsigned fail=1;fail<=6;fail++) {
        setup(false); disk_failure=fail; CHECK(attempt_begin(&fs) && !authorized && !claim_count && !fake.writes);
    }
    setup(false); volume.readonly=true; CHECK(attempt_begin(&fs) && !authorized && !fake.writes);
    for (unsigned fail=1;fail<=2;fail++) {
        setup(false); CHECK(!attempt_begin(&fs)); claim_failure=fail;
        CHECK(!start(&fake.fb,0) && !fake.packets && !ports.fifo && engine.cpu_safe);
    }
    setup(false); CHECK(!attempt_begin(&fs));
    g_boot.e820_count=1; g_boot.e820[0]=(struct ciuki_e820){.base=0xe8000000,.length=0x10000,.type=CBI_E820_RAM};
    CHECK(!start(&fake.fb,0) && !ports.fifo && !fake.fifo_writes);
    setup(true); fake.regs[SVGA_REG_CAPABILITIES]&=~VMSVGA_CAP_FILL;
    CHECK(!attempt_begin(&fs) && !start(&fake.fb,0) && !fake.writes && !fake.fifo_writes && !ports.fifo && engine.cpu_safe);
}
static void failures(void)
{
    setup(false); CHECK(!attempt_begin(&fs)); fake.corrupt_pixels=true;
    CHECK(!start(&fake.fb,0) && engine.quarantined && engine.cpu_safe && !fallback());
    CHECK(marker_size && !eligible(&fake.fb)); CHECK(!engine.ops.stop(&engine));
    setup(false); CHECK(!attempt_begin(&fs)); fake.wedge=true;
    CHECK(!start(&fake.fb,0) && engine.quarantined && !engine.cpu_safe && probe_unsafe && fallback()==&engine.ops);
    CHECK(engine.ops.stop(&engine) && ports.fifo && claims[0]>=0 && claims[1]>=0);
    CHECK(marker_size && attempt_end(&fs) && marker_size);
    fake.wedge=false; CHECK(!engine.ops.stop(&engine) && !ports.fifo && engine.cpu_safe && !eligible(&fake.fb));
    setup(false); (void)activate();
    /* A new marker clear failure quarantines and retains an idle-safe path. */
    marker_owned=true; marker_size=20; disk_failure=disk_step+4;
    CHECK(attempt_end(&fs) && engine.quarantined && blocked && engine.cpu_safe && !fallback());
    CHECK(!engine.ops.stop(&engine));
}
int main(void)
{
    CHECK(vmwaresvga_driver.render3d->status==R3D_UNQUALIFIED && vmwaresvga_driver.render3d->backend==vmsvga3d_backend);
    CHECK(!strcmp(vmwaresvga_driver.attempt_files[1],"C:/system/settings/accel-3d-vmwaresvga-15ad-%04x.cfg"));
    lifecycle(); refusal(); failures();
    CHECK(stops3d); /* every runtime stop first stops the 3D owner */
    puts("vmwaresvga registry/lifecycle: PASS (identity, claims, markers, activation, quarantine)"); return 0;
}
