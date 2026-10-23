/* Production registry and family adapter, fake PCI/MMIO/VFS boundaries.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/registry.h>
#include "../../src/kernel/drivers/video/rage128/driver.c"
#include "../../src/kernel/drivers/video/registry.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
const struct display_driver s3savage_driver={0};
struct ciuki_boot_info g_boot;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
volatile uint64_t g_ticks;
static struct resource resources[8];
static unsigned resource_count, pci_total, claims, maps, revokes, probes, commits, closes, writes, checks;
static unsigned marker_size, failures, groups;
static bool exclusive, cursor_unavailable;
static struct pci_func gpu;
static struct fb_device fb;
static uint32_t mmio[R128_MMIO_BYTES/4];
static uint8_t cursor_page[4096], screen[32*128];
static struct vfs filesystem;
static struct fat_volume volume;
static char marker_text[128];
static int marker_error;
static bool short_write;
unsigned pci_count(void) { return pci_total; }
const struct pci_func *pci_get(unsigned i) { return i<pci_total ? &gpu : NULL; }
const struct fb_device *fbdev_get(void) { return &fb; }
bool fbdev_cpu_ready(void) { return engine.cpu_safe; }
unsigned fbdev_native_capabilities(void) { return engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0; }
void framebuffer_write_drain(void) { }
void klog(const char *fmt, ...) { (void)fmt; CHECK(!exclusive); }
const struct resource *registry_get(unsigned i) { return i<resource_count ? &resources[i] : NULL; }
bool registry_valid(int h, gen_t generation)
{ return h>=0 && (unsigned)h<resource_count && resources[h].generation==generation && resources[h].state==RS_ACTIVE; }
int registry_claim(enum res_type type, uint32_t start, uint32_t end, const char *name, bool share)
{
    CHECK(!exclusive && type==RES_MMIO && !share && start<end); claims++;
    if (failures==1 || (cursor_unavailable && !strcmp(name,"rage128-cursor"))) return -EBUSY;
    for (unsigned i=0;i<resource_count;i++)
        CHECK(end<=resources[i].start || start>=resources[i].end || resources[i].state==RS_RELEASED);
    CHECK(resource_count<8);
    unsigned i=resource_count++;
    resources[i]=(struct resource){type,start,end,name,100+i,RS_CLAIMED,false};
    return (int)i;
}
int registry_activate(int h, gen_t gen)
{
    CHECK(resources[h].generation==gen);
    if (failures==2) return -EIO;
    resources[h].state=RS_ACTIVE; return 0;
}
int registry_quiesce(int h, gen_t gen, registry_idle_proof_fn fn)
{
    CHECK(registry_valid(h,gen));
    resources[h].state=RS_QUIESCING;
    if (!fn(h,gen)) { resources[h].state=RS_QUARANTINED; return -EIO; }
    resources[h].state=RS_CLAIMED; return 0;
}
int registry_release(int h, uint32_t gen)
{ CHECK(resources[h].generation==gen && resources[h].state==RS_CLAIMED); resources[h].state=RS_RELEASED; return 0; }
int registry_quarantine(int h, gen_t gen)
{ CHECK(resources[h].generation==gen); resources[h].state=RS_QUARANTINED; return 0; }
void *vmm_reserve_mmio(uint32_t size)
{ CHECK(!exclusive); return size==R128_MMIO_BYTES ? (void *)mmio : cursor_page; }
void *vmm_replace_mmio(void *window, uint32_t capacity, uint32_t phys, uint32_t bytes)
{
    (void)phys; CHECK(!exclusive && capacity==bytes); maps++;
    return failures==3 ? NULL : window;
}
void vmm_revoke_mmio(void *window, uint32_t size)
{ CHECK(window && (size==4096 || size==R128_MMIO_BYTES)); revokes++; }
int fbdev_accel_probe(const struct fb_device *d, int (*fn)(void *), void *ctx)
{
    CHECK(!exclusive && d==&fb); exclusive=true; probes++;
    int e=fn(ctx);
    if (e) (void)rage128_driver.fallback(); /* real fbdev publication discipline */
    exclusive=false; return e;
}
static void file_io(void) { CHECK(!exclusive); }
int vfs_table_init(struct vfs *v, struct vfs_table *t, unsigned drive)
{ file_io(); CHECK(v==&filesystem && drive==2); t->vfs=v; return marker_error==1 ? -FS_EIO : 0; }
void vfs_table_destroy(struct vfs_table *t) { file_io(); (void)t; }
int vfs_stat(struct vfs_table *t, const char *path, struct fat_entry *entry)
{
    file_io(); (void)t; CHECK(!strcmp(path,"C:/system/settings/r128.cfg"));
    if (marker_error==2) return -FS_EIO;
    entry->size=marker_size; return marker_size ? 0 : -FS_ENOENT;
}
int vfs_open(struct vfs_table *t, const char *path, unsigned flags, enum vfs_share share, uint8_t attr)
{
    file_io(); (void)t; (void)attr; CHECK(!strcmp(path,ATTEMPT_PATH) && flags==(VFS_WRITE|VFS_CREATE) && share==VFS_DENY_ALL);
    return marker_error==3 ? -FS_EIO : 1;
}
int vfs_write(struct vfs_table *t, int fd, const void *text, size_t size, size_t *done)
{
    file_io(); (void)t; CHECK(fd==1 && size<sizeof(marker_text)); writes++;
    memcpy(marker_text,text,size); marker_text[size]=0;
    *done=short_write ? size-1 : size;
    marker_size=(unsigned)*done;
    return marker_error==4 ? -FS_EIO : 0;
}
int vfs_commit(struct vfs_table *t, int fd)
{ file_io(); (void)t; CHECK(fd==1); commits++; return marker_error==5 ? -FS_EIO : 0; }
int vfs_close(struct vfs_table *t, int fd)
{ file_io(); (void)t; CHECK(fd==1); closes++; return marker_error==6 ? -FS_EIO : 0; }
int vfs_truncate(struct vfs_table *t, int fd, uint64_t size)
{ file_io(); (void)t; CHECK(fd==1 && !size); if (marker_error==7) return -FS_EIO; marker_size=0; return 0; }
/* The generic saved-block/presenter implementation is covered by the part A
 * suite. This boundary verifies the family's H hook selects that exact path. */
int fbdev_accel_diagnostic(struct vfs *v, uint64_t deadline, bool (*cancel)(void *), void *ctx, uint32_t *tested)
{
    CHECK(v==diagnostic_vfs && deadline>g_ticks && !cancel && !ctx); checks++;
    CHECK(!rage128_driver.attempt_begin(v));
    const struct accel_ops *ops=rage128_driver.start(&fb,NULL);
    CHECK(ops && !rage128_driver.attempt_end(v));
    CHECK(!rage128_driver.cursor_test(ops,deadline,NULL,NULL));
    *tested=3|DISPLAY_CHECK_CURSOR;
    CHECK(!ops->stop(ops->ctx));
    return 0;
}
static void fixture(void)
{
    engine=(struct r128_engine){.cpu_safe=true,.cursor_hidden=true,.reason="pending"};
    registers=NULL; cursor_mapping=NULL; register_window=cursor_window=NULL; engine_stop=NULL;
    register_claim=cursor_claim=-1; persisted=blocked=fault_recorded=false;
    marker_vfs=diagnostic_vfs=NULL; bound=NULL; owner=NULL;
    memset(native_count,0,sizeof(native_count)); memset(software_count,0,sizeof(software_count));
    memset(&g_boot,0,sizeof(g_boot)); memset(resources,0,sizeof(resources));
    memset(&filesystem,0,sizeof(filesystem)); memset(&volume,0,sizeof(volume));
    memset(mmio,0,sizeof(mmio));
    fb=(struct fb_device){.width=32,.height=32,.pitch=128,.size=sizeof(screen),.phys=0xe0000000,
        .mapped=screen,.present=true,.bpp=32,.red_size=8,.red_pos=16,.green_size=8,.green_pos=8,
        .blue_size=8,.handle=0,.generation=42};
    resources[0]=(struct resource){RES_MMIO,fb.phys,fb.phys+fb.size,"fbdev",42,RS_ACTIVE,false};
    resource_count=1; pci_total=1; claims=maps=revokes=probes=commits=closes=writes=checks=0;
    marker_size=failures=0; marker_error=0; short_write=cursor_unavailable=false;
    gpu=(struct pci_func){.vendor=0x1002,.device=0x5046,.class_code=3,.command=2,
        .bar={fb.phys|8,0,0xf0000000}};
    g_boot.display.vram_bytes=8*1024*1024;
    volume.mounted=true; filesystem.volumes[2]=&volume;
    mmio[0xf8/4]=g_boot.display.vram_bytes;
    mmio[0x50/4]=0x03000600; mmio[0x22c/4]=4;
    mmio[0x200/4]=3<<16; mmio[0x208/4]=31<<16; mmio[0x1740/4]=64;
    g_ticks=0; g_cpu_tsc=false;
}
static const struct accel_ops *activate(void)
{
    CHECK(display_driver_bind(&fb)==&rage128_driver);
    CHECK(rage128_driver.eligible(&fb));
    CHECK(!rage128_driver.attempt_begin(&filesystem));
    CHECK(marker_size && commits==1 && closes==1);
    CHECK(strstr(marker_text,"driver=rage128\npci=1002:5046\n"));
    const struct accel_ops *ops=rage128_driver.start(&fb,NULL);
    CHECK(ops && probes==1 && !engine.submitted);
    CHECK(!rage128_driver.attempt_end(&filesystem) && !marker_size);
    return ops;
}
static void discovery(void)
{
    fixture();
    unsigned table_count=rage128_driver.match_count;
    CHECK(table_count==47 && !rage128_driver.modes);
    for (unsigned i=0;i<table_count;i++) {
        gpu.device=rage128_driver.matches[i].device;
        CHECK(display_driver_bind(&fb)==&rage128_driver && rage128_driver.eligible(&fb));
    }
    gpu.device=0x5159; CHECK(!display_driver_bind(&fb)); /* separate Radeon family */
    gpu.device=0x4c4d; CHECK(!display_driver_bind(&fb)); /* separate Mach64 family */
    CHECK(!claims && !maps && !probes && !writes);
    fixture(); gpu.bar[0]=fb.phys+0x10000; CHECK(!display_driver_bind(&fb));
    fixture(); pci_total=2; CHECK(!display_driver_bind(&fb));
    fixture(); resources[0].generation++; CHECK(!display_driver_bind(&fb));
    fixture(); resources[0].end--; CHECK(!display_driver_bind(&fb));
    fixture(); gpu.command=0; CHECK(!display_driver_bind(&fb));
    fixture(); gpu.bar[2]|=4; CHECK(!rage128_driver.eligible(&fb));
    fixture(); gpu.bar[2]=fb.phys; CHECK(!rage128_driver.eligible(&fb));
    fixture(); g_boot.e820_count=1;
    g_boot.e820[0]=(struct ciuki_e820){.base=0xf0000000,.length=0x4000,.type=CBI_E820_RAM};
    CHECK(!rage128_driver.eligible(&fb));
    fixture(); fb.bpp=24; CHECK(!rage128_driver.eligible(&fb));
    fixture(); g_boot.flags=CBI_F_SAFE_MODE; CHECK(!rage128_driver.eligible(&fb));
    CHECK(rage128_driver.attempt_begin(&filesystem)==-EOPNOTSUPP && !writes && !claims);
    fixture(); memcpy(g_boot.options,"accel=0",8); CHECK(!rage128_driver.eligible(&fb));
    CHECK(rage128_driver.attempt_begin(&filesystem)==-EOPNOTSUPP && !writes && !claims);
    groups++;
}
static void lifecycle(void)
{
    fixture(); const struct accel_ops *ops=activate(); CHECK(ops->capabilities(ops->ctx)==3);
    struct ciuki_display_engine_info info;
    display_driver_refresh(); display_driver_info(&info);
    CHECK(info.kind==CIUKI_ENGINE_NATIVE && info.ready && !info.native_used && info.cpu_ready);
    CHECK(!strcmp(info.driver_id,"rage128") && !strcmp(info.display_name,rage128_driver.name));
    display_driver_count(CIUKI_ENGINE_FILL,true); display_driver_info(&info); CHECK(info.native_used);
    CHECK(!ops->stop(ops->ctx) && revokes==2 && !registers && !cursor_mapping);
    CHECK(resources[1].state==RS_RELEASED && resources[2].state==RS_RELEASED);
    CHECK(!rage128_driver.attempt_begin(&filesystem));
    ops=rage128_driver.start(&fb,NULL); CHECK(ops); CHECK(!rage128_driver.attempt_end(&filesystem));
    CHECK(!ops->stop(ops->ctx));
    fixture(); cursor_unavailable=true; ops=activate(); CHECK(ops->capabilities(ops->ctx)==1);
    struct accel_cursor cursor={0}; CHECK(ops->cursor_set(ops->ctx,&cursor)==-EOPNOTSUPP);
    CHECK(!ops->stop(ops->ctx));
    groups++;
}
static void marker_failures(void)
{
    for (int failure=1;failure<=6;failure++) {
        fixture(); CHECK(display_driver_bind(&fb)); marker_error=failure;
        CHECK(rage128_driver.attempt_begin(&filesystem)<0 && blocked && !claims && !maps);
        CHECK(!rage128_driver.start(&fb,NULL));
    }
    fixture(); CHECK(display_driver_bind(&fb)); short_write=true;
    CHECK(rage128_driver.attempt_begin(&filesystem)<0 && blocked && !claims);
    fixture(); marker_size=42; CHECK(display_driver_bind(&fb));
    CHECK(rage128_driver.attempt_begin(&filesystem)<0 && marker_size==42 && !writes);
    fixture(); volume.readonly=true; CHECK(display_driver_bind(&fb));
    CHECK(rage128_driver.attempt_begin(&filesystem)<0 && !claims);
    fixture(); const struct accel_ops *ops=activate();
    /* A clear failure is handled by the generic registry's stop/quarantine
     * policy; driver itself neither drops a guard nor clears a retained file. */
    persisted=true; marker_size=32; marker_error=7;
    CHECK(rage128_driver.attempt_end(&filesystem)<0 && marker_size==32);
    CHECK(!ops->stop(ops->ctx)); rage128_driver.quarantine("marker_clear_failed");
    CHECK(engine.quarantined && engine.cpu_safe);
    groups++;
}
static void setup_failures(void)
{
    for (unsigned failure=1;failure<=3;failure++) {
        fixture(); CHECK(display_driver_bind(&fb)); CHECK(!rage128_driver.attempt_begin(&filesystem));
        failures=failure;
        CHECK(!rage128_driver.start(&fb,NULL)); CHECK(!rage128_driver.fallback());
        CHECK(register_claim<0 && cursor_claim<0 && !engine.touched && engine.cpu_safe);
        CHECK(marker_size);
    }
    fixture(); CHECK(display_driver_bind(&fb)); CHECK(!rage128_driver.attempt_begin(&filesystem));
    mmio[0x224/4]=16;
    CHECK(!rage128_driver.start(&fb,NULL) && !rage128_driver.fallback());
    CHECK(!engine.touched && register_claim<0 && engine.cpu_safe);
    fixture(); CHECK(display_driver_bind(&fb)); CHECK(!rage128_driver.attempt_begin(&filesystem));
    mmio[0x1740/4]=0;
    CHECK(!rage128_driver.start(&fb,NULL));
    const struct accel_ops *guard=rage128_driver.fallback();
    CHECK(guard && !guard->cpu_ready(guard->ctx) && engine.quarantined && registers);
    CHECK(guard->stop(guard->ctx)<0 && register_claim>=0 && marker_size);
    mmio[0x1740/4]=64;
    guard=rage128_driver.fallback(); CHECK(guard->cpu_ready(guard->ctx));
    CHECK(!guard->stop(guard->ctx) && register_claim<0 && !registers);
    groups++;
}
static void quarantine_and_check(void)
{
    fixture(); const struct accel_ops *ops=activate();
    mmio[0x1740/4]=0;
    struct fb_rect r={0,0,2,2};
    CHECK(ops->copy_rect(ops->ctx,&r,1,1)==-ETIMEDOUT);
    CHECK(engine.quarantined && !engine.submitted && !engine.cpu_safe && !ops->capabilities(ops->ctx));
    CHECK(!rage128_driver.start(&fb,NULL));
    CHECK(rage128_driver.fallback()==ops && !ops->cpu_ready(ops->ctx));
    rage128_driver.record(); CHECK(marker_size && fault_recorded);
    mmio[0x1740/4]=64; CHECK(rage128_driver.fallback()==ops && ops->cpu_ready(ops->ctx));
    CHECK(!ops->stop(ops->ctx)); CHECK(!rage128_driver.eligible(&fb));
    fixture(); ops=activate(); fb.generation++;
    CHECK(ops->fill_rect(ops->ctx,&r,0)==-EIO && !engine.cpu_safe && register_claim>=0);
    CHECK(ops->stop(ops->ctx)<0); /* lease pinned on stale mode, never CPU fallback */
    fixture(); g_boot.flags=CBI_F_HW_CHECK; CHECK(display_driver_bind(&fb));
    bool retained=false; CHECK(!rage128_driver.check_begin(&filesystem,&retained) && !retained);
    struct display_selftest_result result;
    CHECK(!rage128_driver.selftest(100,NULL,NULL,&result));
    CHECK(result.passed==1 && result.first==0 && result.restored && checks==1 && !marker_size);
    CHECK(!rage128_driver.attempt_end(&filesystem));
    fixture(); g_boot.flags=CBI_F_HW_CHECK; marker_size=27; CHECK(display_driver_bind(&fb));
    CHECK(rage128_driver.check_begin(&filesystem,&retained)<0 && retained && marker_size==27 && !writes && !claims);
    groups++;
}
int main(void)
{
    discovery(); lifecycle(); marker_failures(); setup_failures(); quarantine_and_check();
    printf("rage128 registry/lifecycle: %u groups passed\n",groups);
    return 0;
}
