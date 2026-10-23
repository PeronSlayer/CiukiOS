/* Actual adapter, registry, durable-marker and lifecycle paths with fake I/O.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "matrox_fixture.h"
#include "../../src/kernel/drivers/video/matrox/driver.c"
#include "../../src/kernel/drivers/video/registry.c"

volatile uint64_t g_ticks;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
struct ciuki_boot_info g_boot;
const struct display_driver s3savage_driver={0};
static struct pci_func gpu[2];
static unsigned gpu_count, claims, maps, revokes, commits, closes, destroys, file_writes;
static struct resource resources[4];
static struct vfs volume;
static struct fat_volume disk;
static bool own_marker, legacy_marker, bad_commit, bad_close, short_write, bad_clear, refuse_probe;
static bool refuse_mmio, refuse_cursor;
static bool bad_mapping, bad_activation;
static bool probing;
unsigned pci_count(void) { return gpu_count; }
const struct pci_func *pci_get(unsigned n) { return n<gpu_count ? &gpu[n] : 0; }
const struct resource *registry_get(unsigned n) { return n<ARRAY_SIZE(resources) ? &resources[n] : 0; }
bool registry_valid(int n,gen_t gen)
{ return n>=0 && (unsigned)n<ARRAY_SIZE(resources) && resources[n].generation==gen && resources[n].state==RS_ACTIVE; }
int registry_claim(enum res_type type,uint32_t begin,uint32_t end,const char *who,bool share)
{
    assert(type==RES_MMIO && !share && begin<end); claims++;
    bool regs=!strcmp(who,"matrox-registers");
    if ((regs && refuse_mmio) || (!regs && refuse_cursor)) return -EBUSY;
    unsigned i=regs ? 1 : 2;
    assert(resources[i].state!=RS_ACTIVE);
    assert(end<=fake.fb.phys || begin>=fake.fb.phys+fake.fb.size);
    resources[i]=(struct resource){type,begin,end,who,42,RS_CLAIMED,false};
    return (int)i;
}
int registry_activate(int n,gen_t gen)
{ assert(resources[n].generation==gen); if (bad_activation) return -EIO; resources[n].state=RS_ACTIVE; return 0; }
int registry_quiesce(int n,gen_t gen,registry_idle_proof_fn proof)
{
    assert(!probing); assert(resources[n].generation==gen);
    resources[n].state=RS_QUIESCING;
    return proof(n,gen) ? 0 : -EIO;
}
int registry_release(int n,gen_t gen)
{ assert(resources[n].generation==gen && resources[n].state==RS_QUIESCING); resources[n].state=RS_RELEASED; return 0; }
void *vmm_reserve_mmio(uint32_t bytes)
{ return bytes==MATROX_MMIO_BYTES ? (void *)fake.regs : fake.cursor+16; }
void *vmm_replace_mmio(void *window,uint32_t capacity,uint32_t phys,uint32_t bytes)
{ assert(window && capacity==bytes && phys>=0x100000); maps++; return bad_mapping ? 0 : window; }
void vmm_revoke_mmio(void *window,uint32_t bytes)
{ assert(window && (bytes==PAGE_SIZE || bytes==MATROX_MMIO_BYTES)); revokes++; }
void framebuffer_write_drain(void) { fake.drains++; }
const struct fb_device *fbdev_get(void) { return &fake.fb; }
unsigned fbdev_native_capabilities(void)
{ return engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0; }
bool fbdev_cpu_ready(void) { return !engine.touched || engine.cpu_safe; }
int fbdev_accel_probe(const struct fb_device *fb,int (*fn)(void *),void *ctx)
{
    assert(fb==&fake.fb && !probing);
    if (refuse_probe) return -EBUSY;
    probing=true; int e=fn(ctx); probing=false; return e;
}
void klog(const char *fmt,...) { (void)fmt; }
int vfs_table_init(struct vfs *v,struct vfs_table *t,unsigned drive)
{ assert(!probing && v==&volume && t==&marker_table && drive==2); return 0; }
void vfs_table_destroy(struct vfs_table *t) { assert(t==&marker_table); destroys++; }
int vfs_stat(struct vfs_table *t,const char *path,struct fat_entry *entry)
{
    assert(t==&marker_table);
    entry->size=(!strcmp(path,LEGACY_PATH) ? legacy_marker : own_marker) ? 1 : 0;
    return 0;
}
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share share,uint8_t attr)
{
    assert(!probing && t==&marker_table && !strcmp(path,MARKER_PATH) &&
        flags==(VFS_WRITE|VFS_CREATE) && share==VFS_DENY_ALL && !attr); return 3;
}
int vfs_write(struct vfs_table *t,int fd,const void *data,size_t size,size_t *done)
{
    assert(!probing && t==&marker_table && fd==3 && size<80);
    assert(strstr(data,"driver=matrox pci=102b:") && strstr(data,"revision="));
    own_marker=true; *done=short_write ? size-1 : size; file_writes++; return 0;
}
int vfs_truncate(struct vfs_table *t,int fd,uint64_t size)
{ assert(!probing && t==&marker_table && fd==3 && !size); if (bad_clear) return -FS_EIO; own_marker=false; return 0; }
int vfs_commit(struct vfs_table *t,int fd)
{ assert(!probing && t==&marker_table && fd==3); commits++; return bad_commit ? -FS_EIO : 0; }
int vfs_close(struct vfs_table *t,int fd)
{ assert(!probing && t==&marker_table && fd==3); closes++; return bad_close ? -FS_EIO : 0; }

static void boot(void)
{
    fake_reset(32); memset(&engine,0,sizeof(engine));
    memset(resources,0,sizeof(resources)); memset(&g_boot,0,sizeof(g_boot));
    memset(&volume,0,sizeof(volume)); memset(&disk,0,sizeof(disk));
    memset(&runtime_ops,0,sizeof(runtime_ops));
    bound=0; owner=0; registers=0; register_window=cursor_window=0;
    reg_claim=cursor_claim=-1; blocked=armed=false; reason="pending";
    memset(native_count,0,sizeof(native_count)); memset(software_count,0,sizeof(software_count));
    claims=maps=revokes=commits=closes=destroys=file_writes=0; g_ticks=0;
    own_marker=legacy_marker=bad_commit=bad_close=short_write=bad_clear=refuse_probe=false;
    refuse_mmio=refuse_cursor=bad_mapping=bad_activation=false;
    g_boot.display.vram_bytes=8*1024*1024;
    g_boot.kernel_start=0x100000; g_boot.kernel_end=0x200000;
    g_boot.e820_count=1; g_boot.e820[0]=(struct ciuki_e820){.base=0,.length=128*1024*1024,.type=CBI_E820_RAM};
    gpu[0]=(struct pci_func){.vendor=0x102b,.device=0x0521,.class_code=3,.command=2,
        .bar={fake.fb.phys|8,0xf0000000}};
    gpu_count=1;
    resources[0]=(struct resource){RES_MMIO,fake.fb.phys,fake.fb.phys+fake.fb.size,"fbdev",42,RS_ACTIVE,false};
    volume.volumes[2]=&disk; disk.mounted=true;
}
static const struct accel_ops *activate_fake(void)
{
    struct matrox_io io=fake_io();
    assert(display_driver_bind(&fake.fb)==&matrox_driver);
    assert(!attempt_begin(&volume) && armed && own_marker);
    return start_with_io(&fake.fb,0,&io);
}
static void pci_paths(void)
{
    const uint16_t ids[]={0x0520,0x0521,0x0525,0x2527};
    for (unsigned i=0;i<ARRAY_SIZE(ids);i++) {
        boot(); gpu[0].device=ids[i];
        assert(display_driver_bind(&fake.fb)==&matrox_driver && eligible(&fake.fb));
        assert(!claims && !maps && !fake.writes && !fake.reads);
        assert(!matrox_driver.modes);
    }
    const uint16_t family_ids[]={0x0520,0x0521,0x0525,0x0525,0x2527};
    const uint8_t revs[]={0,0,0x80,0x81,0};
    for (unsigned i=0;i<ARRAY_SIZE(family_ids);i++) {
        boot(); gpu[0].device=family_ids[i]; gpu[0].revision=revs[i];
        if (i>=2) fake.credits=fake.capacity=16;
        assert(activate_fake() && !attempt_end(&volume) && !runtime_stop(&engine));
    }
    boot(); gpu[0].device=0x0532; assert(!display_driver_bind(&fake.fb));
    boot(); gpu[1]=gpu[0]; gpu_count=2; assert(!display_driver_bind(&fake.fb));
    boot(); gpu[0].bar[0]+=4096; assert(!display_driver_bind(&fake.fb));
    boot(); resources[0].generation=41; assert(!display_driver_bind(&fake.fb));
    boot(); gpu[0].command=0; assert(!display_driver_bind(&fake.fb));
    boot(); gpu[0].bar[1]=0x100000; assert(display_driver_bind(&fake.fb) && !eligible(&fake.fb));
    boot(); gpu[0].bar[1]=fake.fb.phys; assert(display_driver_bind(&fake.fb) && !eligible(&fake.fb));
    boot(); fake.fb.red_pos=0; assert(display_driver_bind(&fake.fb) && !eligible(&fake.fb));
    boot(); fake.fb.pitch-=4; assert(display_driver_bind(&fake.fb) && !eligible(&fake.fb));
    boot(); g_boot.flags|=CBI_F_SAFE_MODE; assert(display_driver_bind(&fake.fb) && attempt_begin(&volume) && !claims && !file_writes);
    boot(); memcpy(g_boot.options,"accel=0",8); assert(display_driver_bind(&fake.fb) && attempt_begin(&volume) && !claims && !file_writes);
    puts("Matrox registry: exact PCI IDs, probe side effects, active LFB ownership, mode/BAR/RAM refusal, safe mode and accel=0 PASS");
}
static void lifecycle(void)
{
    boot(); uint8_t saved[sizeof(fake.pixels)]; memcpy(saved,fake.pixels,sizeof(saved));
    const struct accel_ops *ops=activate_fake(); assert(ops && claims==2 && maps==2 && probing==false);
    assert(!memcmp(saved,fake.pixels,sizeof(saved)) && engine.cpu_safe && fake.launches==1);
    assert(!attempt_end(&volume) && !own_marker && commits==2 && closes==2 && destroys==2);
    struct ciuki_display_engine_info info;
    display_driver_refresh(); display_driver_info(&info);
    assert(info.kind==CIUKI_ENGINE_NATIVE && !info.native_used && info.cpu_ready);
    assert(!strcmp(info.driver_id,"matrox") && !strcmp(info.display_name,"Matrox G200/G400/G450/G550"));
    display_driver_count(CIUKI_ENGINE_FILL,true); display_driver_info(&info); assert(info.native_used);
    assert(!cursor_test(ops,100,0,0) && !fake.dac[XCURCTRL]);
    assert(!runtime_stop(&engine) && !registers && !engine.cursor && revokes==2);
    assert(!fallback() && resources[1].state==RS_RELEASED && resources[2].state==RS_RELEASED);
    boot(); refuse_cursor=true; ops=activate_fake(); assert(ops && !(ops->capabilities(ops->ctx)&2u));
    struct accel_cursor c={0}; assert(ops->cursor_set(ops->ctx,&c)==-EOPNOTSUPP);
    assert(!attempt_end(&volume) && !runtime_stop(&engine));
    boot(); gpu[0].device=0x0525; fake.regs[M_C2CTL/4]=1;
    ops=activate_fake(); assert(ops && claims==1 && !(ops->capabilities(ops->ctx)&2u));
    assert(!attempt_end(&volume) && !runtime_stop(&engine));
    boot(); fake.no_draw=true; assert(!activate_fake() && engine.quarantined && own_marker && !registers);
    assert(!attempt_failed(&volume) && own_marker && !eligible(&fake.fb));
    boot(); fake.wedge_launch=true; assert(!activate_fake() && engine.quarantined && !engine.cpu_safe);
    assert(registers && fallback() && !engine.cpu_safe && resources[1].state==RS_ACTIVE);
    fake.wedge=false; assert(fallback() && engine.cpu_safe && engine.quarantined);
    assert(!runtime_stop(&engine) && !registers);
    boot(); ops=activate_fake(); assert(ops); assert(!attempt_end(&volume));
    fake.wedge=true; quarantine("runtime_fault"); assert(!engine.cpu_safe && !eligible(&fake.fb));
    assert(runtime_stop(&engine) && registers && !engine.cpu_safe);
    fake.wedge=false; assert(!runtime_stop(&engine) && engine.cpu_safe && engine.quarantined);
    puts("Matrox adapter: saved native activation, identity/usage, cursor evidence, resource release, ignored/wedged engine and guarded fallback PASS");
}
static void marker_paths(void)
{
    boot(); own_marker=true; display_driver_bind(&fake.fb); assert(attempt_begin(&volume) && !file_writes && !claims);
    boot(); legacy_marker=true; display_driver_bind(&fake.fb); assert(attempt_begin(&volume) && !file_writes && legacy_marker);
    boot(); short_write=true; display_driver_bind(&fake.fb); assert(attempt_begin(&volume) && !commits && closes==1 && !claims);
    boot(); bad_commit=true; display_driver_bind(&fake.fb); assert(attempt_begin(&volume) && closes==1 && !claims);
    boot(); bad_close=true; display_driver_bind(&fake.fb); assert(attempt_begin(&volume) && !armed && !claims);
    boot(); disk.readonly=true; display_driver_bind(&fake.fb); assert(attempt_begin(&volume) && !file_writes && !claims);
    boot(); refuse_mmio=true; assert(!activate_fake() && own_marker && !registers);
    boot(); bad_mapping=true; assert(!activate_fake() && own_marker && !registers && resources[1].state==RS_RELEASED);
    boot(); bad_activation=true; assert(!activate_fake() && own_marker && !registers && revokes==1);
    boot(); fake.bad_dac=true; assert(!activate_fake() && own_marker && registers && !engine.cpu_safe);
    fake.bad_dac=false; assert(!runtime_stop(&engine));
    boot(); fake.wedge=true; assert(!activate_fake() && !fake.writes && !engine.cpu_safe && registers);
    fake.wedge=false; assert(!runtime_stop(&engine));
    boot(); display_driver_bind(&fake.fb); assert(!attempt_begin(&volume));
    resources[0].generation++; struct matrox_io io=fake_io();
    assert(!start_with_io(&fake.fb,0,&io) && !claims && !maps);
    boot(); refuse_probe=true; assert(!activate_fake() && own_marker && !registers && revokes==2);
    boot(); assert(activate_fake()); bad_clear=true; assert(attempt_end(&volume) && blocked && own_marker);
    quarantine("marker_clear_failed"); assert(!runtime_stop(&engine));
    puts("Matrox durable marker: retained legacy/own, read-only, short write, commit/close/clear failures and no I/O under presenter PASS");
}
int main(void) { pci_paths(); lifecycle(); marker_paths(); return 0; }
