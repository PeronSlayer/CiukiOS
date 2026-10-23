/* Registry + production NV3 lifecycle with fake register/VFS/resource services.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "riva128_fixture.h"
#include "../../src/kernel/drivers/video/riva128/driver.c"
#include "../../src/kernel/drivers/video/registry.c"

struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
static struct pci_func cards[2];
static unsigned card_count,resource_count,claim_calls,releases,maps,revokes,probe_calls;
static int claim_fail_after,map_fail_after;
static bool exclusive,fail_probe,fail_release;
static struct resource resources[16];
static struct vfs fs;
static struct fat_volume volume;
static char marker_data[128];
static size_t marker_size,legacy_size;
static unsigned commits,closes,opens,truncates,fs_calls;
static bool marker_exists,committed,short_write;
static int stat_error,write_error,commit_error,close_error,truncate_error,open_error,table_error;
static struct nv3_io device_io;
const struct display_driver s3savage_driver={0};
unsigned pci_count(void) { return card_count; }
const struct pci_func *pci_get(unsigned i) { return i<card_count ? &cards[i] : 0; }
const struct resource *registry_get(unsigned i) { return i<resource_count ? &resources[i] : 0; }
bool registry_valid(int h,gen_t gen)
{ return h>=0 && (unsigned)h<resource_count && resources[h].generation==gen && resources[h].state==RS_ACTIVE; }
int registry_claim(enum res_type type,uint32_t start,uint32_t end,const char *owner,bool share)
{
    claim_calls++; assert(marker_size && committed); assert(start<end && !share);
    if (claim_fail_after==0) return -EBUSY;
    if (claim_fail_after>0) claim_fail_after--;
    for (unsigned i=0;i<resource_count;i++) if (resources[i].state!=RS_RELEASED &&
        start<resources[i].end && end>resources[i].start) return -EBUSY;
    assert(resource_count<ARRAY_SIZE(resources));
    unsigned h=resource_count++;
    resources[h]=(struct resource){type,start,end,owner,h+7,RS_CLAIMED,false};
    return (int)h;
}
int registry_activate(int h,gen_t gen)
{ assert(resources[h].generation==gen); resources[h].state=RS_ACTIVE; return 0; }
int registry_quiesce(int h,gen_t gen,registry_idle_proof_fn fn)
{
    assert(resources[h].generation==gen);
    if (fail_release) return -EIO;
    resources[h].state=RS_QUIESCING;
    return fn(h,gen) ? 0 : -EIO;
}
int registry_release(int h,gen_t gen)
{ assert(resources[h].generation==gen); resources[h].state=RS_RELEASED; releases++; return 0; }
void *vmm_reserve_mmio(uint32_t bytes) { return calloc(1,bytes); }
void *vmm_replace_mmio(void *window,uint32_t capacity,uint32_t phys,uint32_t bytes)
{
    maps++; assert(capacity==bytes && phys>=0x100000 && window);
    if (map_fail_after==0) return 0;
    if (map_fail_after>0) map_fail_after--;
    return window;
}
void vmm_revoke_mmio(void *window,uint32_t bytes)
{ assert(window && bytes); revokes++; }
void framebuffer_write_drain(void) { fake.drains++; }
uint64_t ktime_cycles(void) { return 0; }
int udelay(uint32_t us) { fake.time+=us; return 0; }
void klog(const char *fmt,...) { (void)fmt; }
const struct fb_device *fbdev_get(void) { return &fake.fb; }
bool fbdev_cpu_ready(void) { return engine.cpu_safe; }
unsigned fbdev_native_capabilities(void) { return engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0; }
int fbdev_accel_probe(const struct fb_device *fb,int (*callback)(void *),void *ctx)
{
    assert(fb==&fake.fb && !exclusive && marker_size && committed);
    probe_calls++;
    if (fail_probe) return -EBUSY;
    exclusive=true; int e=callback(ctx); exclusive=false; return e;
}
static void owned_write(void *ctx,uint32_t reg,uint32_t v)
{
    /* Commands, setup and activation all hold the probe's exclusive drawing
     * claim. Teardown/diagnostic calls below simulate the same ownership. */
    assert(exclusive); f_write(ctx,reg,v);
}
static void file_call(void) { assert(!exclusive); fs_calls++; }
int vfs_table_init(struct vfs *vfs,struct vfs_table *table,unsigned drive)
{ file_call(); assert(drive==2); table->vfs=vfs; return table_error; }
void vfs_table_destroy(struct vfs_table *table) { file_call(); table->vfs=0; }
int vfs_stat(struct vfs_table *table,const char *path,struct fat_entry *entry)
{
    file_call(); assert(table->vfs==&fs);
    if (stat_error) return stat_error;
    if (!strcmp(path,LEGACY_MARKER)) {
        if (!legacy_size) return -FS_ENOENT;
        entry->size=legacy_size; return 0;
    }
    assert(!strcmp(path,MARKER));
    if (!marker_exists) return -FS_ENOENT;
    entry->size=marker_size; return 0;
}
int vfs_open(struct vfs_table *table,const char *path,unsigned flags,enum vfs_share share,uint8_t attr)
{
    file_call(); (void)table; assert(!strcmp(path,MARKER) && flags==(VFS_WRITE|VFS_CREATE) && share==VFS_DENY_ALL && !attr);
    opens++; marker_exists=true; return open_error ? open_error : 5;
}
int vfs_write(struct vfs_table *table,int fd,const void *data,size_t size,size_t *done)
{
    file_call(); (void)table; assert(fd==5 && size<sizeof(marker_data));
    if (write_error) return write_error;
    marker_size=*done=short_write ? size-1 : size;
    memcpy(marker_data,data,marker_size); marker_data[marker_size]=0; return 0;
}
int vfs_truncate(struct vfs_table *table,int fd,uint64_t size)
{ file_call(); (void)table; assert(fd==5 && !size); truncates++; if (truncate_error) return truncate_error; marker_size=0; return 0; }
int vfs_commit(struct vfs_table *table,int fd)
{ file_call(); (void)table; assert(fd==5); commits++; if (commit_error) return commit_error; committed=true; return 0; }
int vfs_close(struct vfs_table *table,int fd)
{ file_call(); (void)table; assert(fd==5); closes++; return close_error; }
static void reset(unsigned format)
{
    for (unsigned i=0;i<ARRAY_SIZE(windows);i++) {
        free(windows[i].reservation); windows[i].reservation=windows[i].mapping=0;
    }
    fake_reset(format); memset(&engine,0,sizeof(engine)); memset(&ops,0,sizeof(ops));
    reason="pending"; authorized=attempted=marker_pending=blocked=reported=false;
    release_safe=true; for (unsigned i=0;i<3;i++) { claims[i]=-1; generations[i]=0; }
    device_io=fake_io(); device_io.write32=owned_write; transport=&device_io;
    memset(&g_boot,0,sizeof(g_boot)); g_boot.display.vram_bytes=4u<<20;
    g_boot.kernel_start=0x100000; g_boot.kernel_end=0x200000;
    g_boot.e820_count=1; g_boot.e820[0]=(struct ciuki_e820){.base=0,.length=0x10000000,.type=CBI_E820_RAM};
    memset(cards,0,sizeof(cards)); cards[0]=(struct pci_func){.vendor=0x12d2,.device=0x18,.class_code=3,.command=2,
        .bar={0xf0000000,0xe0000000}}; card_count=1;
    memset(resources,0,sizeof(resources)); resource_count=1;
    resources[0]=(struct resource){RES_MMIO,fake.fb.phys,fake.fb.phys+fake.fb.size,"fbdev",7,RS_ACTIVE,false};
    memset(&fs,0,sizeof(fs)); memset(&volume,0,sizeof(volume)); volume.mounted=true; fs.volumes[2]=&volume;
    memset(marker_data,0,sizeof(marker_data)); marker_size=legacy_size=0; marker_exists=committed=short_write=false;
    commits=closes=opens=truncates=fs_calls=0;
    stat_error=write_error=commit_error=close_error=truncate_error=open_error=table_error=0;
    claim_fail_after=map_fail_after=-1; claim_calls=releases=maps=revokes=probe_calls=0;
    fail_probe=fail_release=exclusive=false;
    bound=0; owner=0; software_reason="pending";
    memset(&cached_snapshot,0,sizeof(cached_snapshot));
    memset(native_count,0,sizeof(native_count)); memset(software_count,0,sizeof(software_count));
}
static const struct accel_ops *activate(void)
{
    assert(display_driver_bind(&fake.fb)==&riva128_driver);
    assert(riva128_driver.eligible(&fake.fb));
    assert(!riva128_driver.attempt_begin(&fs));
    assert(marker_size && commits==1 && closes==1 && !claim_calls);
    assert(strstr(marker_data,"driver=riva128\npci=12d2:0018\n"));
    return riva128_driver.start(&fake.fb,0);
}
static void nomination(void)
{
    reset(32); const struct pci_func *found=0;
    const struct display_driver *const table[]={&riva128_driver};
    assert(display_driver_select(&fake.fb,table,1,&found)==&riva128_driver && found==&cards[0]);
    assert(!fake.writes && !claim_calls && !fs_calls);
    cards[0].vendor=0x10de; cards[0].device=0x20;
    assert(!display_driver_select(&fake.fb,table,1,&found)); /* TNT is not NV3 */
    reset(32); card_count=2; cards[1]=cards[0]; assert(!eligible(&fake.fb));
    reset(32); cards[0].bar[1]+=0x800000; assert(!eligible(&fake.fb));
    reset(32); resources[0].generation++; assert(!eligible(&fake.fb));
    reset(32); cards[0].command|=4; assert(!probe(&cards[0]));
    reset(32); cards[0].subclass=2; assert(!probe(&cards[0]));
    reset(32); cards[0].bar[0]=0x4000000; assert(!eligible(&fake.fb));
    reset(32); cards[0].bar[0]=0xfffff000; assert(!eligible(&fake.fb));
    reset(32); cards[0].bar[0]=cards[0].bar[1]; assert(!eligible(&fake.fb));
    reset(32); fake.fb.bpp=24; assert(!eligible(&fake.fb));
    reset(32); g_boot.flags|=CBI_F_SAFE_MODE; assert(attempt_begin(&fs)==-EIO);
    assert(!opens && !claim_calls && !fake.writes);
    reset(32); memcpy(g_boot.options,"accel=0",8); assert(attempt_begin(&fs)==-EIO);
    assert(!opens && !claim_calls && !fake.writes);
    reset(32); assert(!start(&fake.fb,0) && !fake.writes && !claim_calls);
}
static void markers(void)
{
    reset(32); marker_exists=true; marker_size=9; assert(attempt_begin(&fs)==-EIO && blocked && !opens);
    reset(32); legacy_size=7; assert(attempt_begin(&fs)==-EIO && !opens && legacy_size==7);
    reset(32); stat_error=-FS_EIO; assert(attempt_begin(&fs)==-FS_EIO && !opens);
    reset(32); volume.readonly=true; assert(attempt_begin(&fs)==-FS_EROFS && !opens);
    reset(32); table_error=-FS_ENOMEM; assert(attempt_begin(&fs)==-FS_ENOMEM && !opens);
    reset(32); open_error=-FS_EIO; assert(attempt_begin(&fs)==-FS_EIO && !authorized);
    reset(32); short_write=true; assert(attempt_begin(&fs)==-FS_EIO && !authorized && closes==1 && !commits);
    reset(32); write_error=-FS_EIO; assert(attempt_begin(&fs)==-FS_EIO && !authorized && closes==1);
    reset(32); commit_error=-FS_EIO; assert(attempt_begin(&fs)==-FS_EIO && !authorized && closes==1);
    reset(32); close_error=-FS_EIO; assert(attempt_begin(&fs)==-FS_EIO && !authorized);
    assert(!fake.writes && !claim_calls);
}
static void success(unsigned format)
{
    reset(format); memcpy(fake.gold,fake.pixels,fake.fb.size);
    const struct accel_ops *o=activate();
    assert(o && probe_calls==1 && claim_calls==3 && maps==9 && !revokes);
    same_pixels(); /* DM9 must restore the saved block, including reserved bits. */
    assert(!attempt_end(&fs) && !marker_size && truncates==1 && commits==2);
    struct ciuki_display_engine_info info;
    display_driver_info(&info);
    assert(!strcmp(info.driver_id,"riva128") && !strcmp(info.display_name,"NVIDIA Riva 128"));
    assert(info.ready && !info.native_used && info.capabilities==3 && info.pci_vendor==0x12d2 && info.pci_device==0x18);
    display_driver_count(CIUKI_ENGINE_FILL,true); display_driver_info(&info); assert(info.native_used);
    exclusive=true; assert(!o->stop(o->ctx)); exclusive=false;
    assert(releases==3 && revokes==9 && engine.cpu_safe && !engine.touched);
    assert(!riva128_driver.modes && !riva128_driver.mode_adapter);
    /* A normal mode stop permits a new authorized activation, unlike quarantine. */
    assert(!attempt_begin(&fs));
    o=start(&fake.fb,0); assert(o && !attempt_end(&fs));
    exclusive=true; assert(!o->stop(o->ctx)); exclusive=false;
    assert(releases==6 && revokes==18 && engine.cpu_safe);
}
static void activation_failures(void)
{
    reset(32); fake.no_draw=true; assert(!activate());
    assert(engine.quarantined && marker_size && !truncates && !attempt_failed(&fs));
    assert(!eligible(&fake.fb) && attempt_end(&fs)==-EIO);
    exclusive=true; const struct accel_ops *guard=fallback(); exclusive=false;
    assert(guard && guard->cpu_ready(guard->ctx) && guard->capabilities(guard->ctx)==0);
    reset(32); fake.hang_launch=true; assert(!activate());
    assert(engine.quarantined && !engine.cpu_safe && marker_size);
    exclusive=true; guard=fallback(); assert(guard && !guard->cpu_ready(guard->ctx));
    assert(guard->stop(guard->ctx)==-ETIMEDOUT && !releases && !revokes);
    fake.busy=0; fake.hang_launch=false;
    assert(!guard->sync(guard->ctx) && guard->cpu_ready(guard->ctx));
    assert(!guard->stop(guard->ctx)); exclusive=false;
    assert(releases==3 && revokes==9);
    reset(32); claim_fail_after=0; assert(!activate() && !fake.writes && !maps);
    reset(32); claim_fail_after=1; assert(!activate() && releases==1 && !fake.writes);
    reset(32); claim_fail_after=2; assert(!activate() && releases==2 && !fake.launches);
    reset(32); map_fail_after=4; assert(!activate() && releases==2 && revokes==4 && !fake.writes);
    reset(32); fail_probe=true; assert(!activate() && releases==2 && !fake.writes);
    reset(32); g_boot.display.vram_bytes=8u<<20; assert(!activate() && !fake.launches && releases==2);
    reset(32); fake.pan=1; assert(!activate() && !fake.launches && releases==3);
}
static void teardown_failure(void)
{
    reset(32); const struct accel_ops *o=activate(); assert(o);
    truncate_error=-FS_EIO; assert(attempt_end(&fs)==-FS_EIO && marker_size);
    exclusive=true;
    assert(!o->stop(o->ctx)); quarantine("marker_clear_failed");
    assert(!fallback() && engine.cpu_safe && engine.quarantined);
    exclusive=false; assert(!maps || revokes==maps);
    reset(32); o=activate(); assert(o && !attempt_end(&fs));
    fail_release=true; exclusive=true;
    assert(o->stop(o->ctx)==-EIO && engine.cpu_safe && !engine.touched);
    assert(!o->sync(o->ctx) && !o->cursor_show(o->ctx,false));
    exclusive=false;
    assert(revokes==9 && !releases && engine.quarantined);
}
int main(void)
{
    nomination(); markers(); success(32); success(16); success(15); activation_failures(); teardown_failure();
    reset(32); for (unsigned i=0;i<ARRAY_SIZE(windows);i++) free(windows[i].reservation);
    free(fake.regs);
    puts("NV3 adapter: PCI/lease refusal, DM9 native-only/restoration, identity, durable markers, claims, quarantine and stop PASS");
    return 0;
}
