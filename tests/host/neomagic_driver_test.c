#include "neomagic_fixture.h"
#include "../../src/kernel/drivers/video/neomagic/driver.c"
#include "../../src/kernel/drivers/video/registry.c"

struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
const struct display_driver s3savage_driver={0};
static struct pci_func pci;
static unsigned pci_functions, claims, activations, maps, probe_calls, revokes, releases;
static struct resource leases[8];
static bool claim_failed, map_failed, activate_failed, cursor_map_failed;
static bool retained_marker, read_failed, write_failed, short_write, commit_failed, close_failed;
static unsigned opens, writes, commits, closes, destroys, truncates;
static struct vfs filesystem;
static struct fat_volume volume;
uint64_t ktime_cycles(void) { return 0; }
void framebuffer_write_drain(void) { }
void klog(const char *format,...) { (void)format; }
unsigned pci_count(void) { return pci_functions; }
const struct pci_func *pci_get(unsigned i) { return i<pci_functions ? &pci : NULL; }
const struct resource *registry_get(unsigned i) { return i<claims+1 ? &leases[i] : NULL; }
bool registry_valid(int h,gen_t generation)
{ return h>=0 && h<(int)claims+1 && leases[h].generation==generation && leases[h].state==RS_ACTIVE; }
int registry_claim(enum res_type type,uint32_t a,uint32_t b,const char *owner_name,bool share)
{
    CHECK(!share && a<b);
    if (claim_failed) return -EBUSY;
    CHECK(claims+1<ARRAY_SIZE(leases));
    unsigned i=++claims;
    leases[i]=(struct resource){.type=type,.start=a,.end=b,.owner=owner_name,.generation=i+42,.state=RS_CLAIMED};
    return (int)i;
}
int registry_activate(int h,gen_t g)
{
    CHECK(h>0 && leases[h].generation==g); activations++;
    if (activate_failed) return -EIO;
    leases[h].state=RS_ACTIVE;
    return 0;
}
int registry_quarantine(int h,gen_t g)
{ CHECK(h>0 && leases[h].generation==g); leases[h].state=RS_QUARANTINED; return 0; }
int registry_quiesce(int h,gen_t g,registry_idle_proof_fn proof)
{
    CHECK(h>0 && leases[h].generation==g);
    if (leases[h].state!=RS_ACTIVE) return -EIO;
    leases[h].state=RS_QUIESCING;
    return proof(h,g) ? 0 : -EIO;
}
int registry_release(int h,uint32_t g)
{
    CHECK(h>0 && leases[h].generation==g && leases[h].state==RS_QUIESCING);
    leases[h].state=RS_RELEASED; releases++; return 0;
}
void vmm_revoke_mmio(void *mapped,uint32_t size)
{ CHECK(mapped && (size==PAGE_SIZE || size==NEO_MMIO_BYTES)); revokes++; }
void *vmm_reserve_mmio(uint32_t capacity)
{ CHECK(capacity==NEO_MMIO_BYTES || capacity==PAGE_SIZE); return capacity==PAGE_SIZE ? (void *)(fake.cursor+8) : (void *)fake.regs; }
void *vmm_replace_mmio(void *window,uint32_t capacity,uint32_t phys,uint32_t bytes)
{
    (void)phys; CHECK(bytes==capacity); maps++;
    return map_failed || (cursor_map_failed && capacity==PAGE_SIZE) ? NULL : window;
}
const struct fb_device *fbdev_get(void) { return &fake.fb; }
bool fbdev_cpu_ready(void) { return engine.cpu_safe; }
unsigned fbdev_native_capabilities(void) { return engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0; }
int fbdev_accel_probe(const struct fb_device *d,int (*fn)(void *),void *ctx)
{
    CHECK(d==&fake.fb && fn==neo_dm9 && ctx==&engine && persisted);
    CHECK(claims>=2 && !fake.writes && !fake.launches);
    probe_calls++;
    /* All register activity must occur inside this presenter exclusion.
     * Replace the physical transport with the independent fake registers. */
    engine.io=model_io();
    return fn(ctx);
}
int vfs_table_init(struct vfs *vfs,struct vfs_table *table,unsigned drive)
{ CHECK(vfs==&filesystem && table==&marker_table && drive==2); return 0; }
void vfs_table_destroy(struct vfs_table *table) { CHECK(table==&marker_table); destroys++; }
int vfs_stat(struct vfs_table *table,const char *path,struct fat_entry *entry)
{
    CHECK(table==&marker_table && !strcmp(path,ATTEMPT_PATH));
    if (read_failed) return -FS_EIO;
    entry->size=retained_marker ? 8 : 0;
    return retained_marker ? 0 : -FS_ENOENT;
}
int vfs_open(struct vfs_table *table,const char *path,unsigned flags,enum vfs_share share,uint8_t attrs)
{
    CHECK(table==&marker_table && !strcmp(path,ATTEMPT_PATH) && flags==(VFS_WRITE|VFS_CREATE));
    CHECK(share==VFS_DENY_ALL && !attrs); opens++; return 3;
}
int vfs_write(struct vfs_table *table,int fd,const void *data,size_t count,size_t *done)
{
    CHECK(table==&marker_table && fd==3 && count==strlen(data));
    CHECK(strstr(data,"driver=neomagic\n")); writes++;
    *done=short_write ? 1 : count;
    if (!write_failed) retained_marker=true;
    return write_failed ? -FS_EIO : 0;
}
int vfs_truncate(struct vfs_table *table,int fd,uint64_t bytes)
{ CHECK(table==&marker_table && fd==3 && !bytes); truncates++; retained_marker=false; return 0; }
int vfs_commit(struct vfs_table *table,int fd)
{ CHECK(table==&marker_table && fd==3); commits++; return commit_failed ? -FS_EIO : 0; }
int vfs_close(struct vfs_table *table,int fd)
{ CHECK(table==&marker_table && fd==3); closes++; return close_failed ? -FS_EIO : 0; }
static void setup(void)
{
    model_reset(24,32);
    memset(&engine,0,sizeof(engine)); memset(&g_boot,0,sizeof(g_boot)); memset(leases,0,sizeof(leases));
    blocked=persisted=false; reason="pending"; registers=NULL; cursor_mapping=NULL;
    register_window=cursor_window=NULL; reg_claim=port_claim=cursor_claim=-1;
    cursor_offset=mapped_base=mapped_fb=0; claims=activations=maps=probe_calls=0;
    revokes=releases=0;
    claim_failed=map_failed=activate_failed=cursor_map_failed=false;
    retained_marker=read_failed=write_failed=short_write=commit_failed=close_failed=false;
    opens=writes=commits=closes=destroys=truncates=0;
    leases[0]=(struct resource){.type=RES_MMIO,.start=fake.fb.phys,.end=fake.fb.phys+fake.fb.size,
        .owner="fbdev",.generation=42,.state=RS_ACTIVE};
    pci=(struct pci_func){.vendor=0x10c8,.device=5,.class_code=3,.command=3,
        .bar={fake.fb.phys,0xe1000000}};
    pci_functions=1;
    g_boot.display.vram_bytes=neo_vram(5);
    memset(&filesystem,0,sizeof(filesystem)); memset(&volume,0,sizeof(volume));
    volume.mounted=true; filesystem.volumes[2]=&volume;
}
static void activate(void)
{
    CHECK(eligible(&fake.fb));
    CHECK(!attempt_begin(&filesystem)); CHECK(persisted && retained_marker);
    CHECK(start(&fake.fb,NULL)==&engine.ops && probe_calls==1);
    CHECK(!attempt_end(&filesystem) && !persisted && !retained_marker);
}
static bool cancelled(void *ctx) { CHECK(ctx==&fake); return true; }
static void refusals(void)
{
    setup();
    CHECK(neomagic_driver.match_count==9 && neomagic_driver.framebuffer_bar==0 && !neomagic_driver.modes);
    CHECK(display_driver_bind(&fake.fb)==&neomagic_driver);
    CHECK(!fake.reads && !fake.writes && !claims);
    uint16_t ids[]={1,2,3,0x83,4,5,0x25,6,0x16};
    for (unsigned i=0;i<ARRAY_SIZE(ids);i++) {
        pci.device=ids[i]; CHECK(probe(&pci)==(i>=5));
        CHECK(!fake.reads && !claims);
    }
    pci.device=5; pci_functions=2; CHECK(!eligible(&fake.fb)); pci_functions=1;
    pci.bar[0]+=4096; CHECK(!eligible(&fake.fb)); pci.bar[0]=fake.fb.phys;
    pci.bar[1]=pci.bar[0]; CHECK(!eligible(&fake.fb)); pci.bar[1]=0xe1000000;
    pci.bar[1]|=4; CHECK(!eligible(&fake.fb)); pci.bar[1]=0xe1000000;
    pci.bar[1]+=4096; CHECK(!eligible(&fake.fb)); pci.bar[1]=0xe1000000;
    pci.command=1; CHECK(!eligible(&fake.fb)); pci.command=3;
    leases[0].generation++; CHECK(!eligible(&fake.fb)); leases[0].generation--;
    g_boot.e820_count=1; g_boot.e820[0]=(struct ciuki_e820){.base=pci.bar[1],.length=NEO_MMIO_BYTES,.type=CBI_E820_RAM};
    CHECK(!eligible(&fake.fb)); g_boot.e820_count=0;
    g_boot.display.vram_bytes=1; CHECK(!eligible(&fake.fb)); g_boot.display.vram_bytes=neo_vram(5);
    fake.fb.bpp=32; CHECK(!eligible(&fake.fb)); fake.fb.bpp=24;
    g_boot.flags=CBI_F_SAFE_MODE; CHECK(attempt_begin(NULL)==-EOPNOTSUPP); CHECK(!claims && !opens);
    g_boot.flags=0; strcpy(g_boot.options,"xaccel=0"); CHECK(eligible(&fake.fb));
    strcpy(g_boot.options,"accel=0"); CHECK(!eligible(&fake.fb) && attempt_begin(&filesystem)==-EOPNOTSUPP);
    CHECK(!claims && !fake.reads && !fake.writes && !opens);
    setup(); CHECK(!start(&fake.fb,NULL) && !claims && !fake.reads);
    setup(); retained_marker=true; CHECK(attempt_begin(&filesystem)<0 && blocked && !claims && !opens);
    setup(); read_failed=true; CHECK(attempt_begin(&filesystem)<0 && blocked && !claims && !opens);
    setup(); volume.readonly=true; CHECK(attempt_begin(&filesystem)<0 && !opens);
    setup(); short_write=true; CHECK(attempt_begin(&filesystem)<0 && closes==1 && !commits && !persisted);
    setup(); write_failed=true; CHECK(attempt_begin(&filesystem)<0 && closes==1 && !commits && !persisted);
    setup(); commit_failed=true; CHECK(attempt_begin(&filesystem)<0 && closes==1 && !persisted);
    setup(); close_failed=true; CHECK(attempt_begin(&filesystem)<0 && commits==1 && !persisted);
    setup(); CHECK(!attempt_begin(&filesystem)); claim_failed=true;
    CHECK(!start(&fake.fb,NULL) && !fake.writes && !fallback());
    setup(); CHECK(!attempt_begin(&filesystem)); map_failed=true;
    CHECK(!start(&fake.fb,NULL) && !fake.writes && !fallback());
    setup(); CHECK(!attempt_begin(&filesystem)); activate_failed=true;
    CHECK(!start(&fake.fb,NULL) && !fake.writes && !fallback());
}
static void lifecycle(void)
{
    setup(); activate();
    CHECK(claims==3 && maps==2 && engine.cursor && engine.cpu_safe);
    bool ready,used; const char *why;
    CHECK(summary(&ready,&used,&why) && ready && !used && !strcmp(why,"none"));
    struct ciuki_display_engine_info info;
    display_driver_bind(&fake.fb); display_driver_refresh(); display_driver_info(&info);
    CHECK(!strcmp(info.display_name,"NeoMagic MagicGraph") && !strcmp(info.driver_id,"neomagic"));
    CHECK(info.kind==CIUKI_ENGINE_NATIVE && info.ready && !info.native_used);
    struct ciuki_accel_info old={0}; snapshot(&old);
    CHECK(old.capabilities==3 && old.generation==42 && !old.quarantined);
    g_ticks=5;
    CHECK(cursor_test(&engine.ops,5,NULL,NULL)==-ETIMEDOUT && engine.hidden);
    CHECK(cursor_test(&engine.ops,10,cancelled,&fake)==-EINTR && engine.hidden);
    g_ticks=0;
    fake.stuck=true;
    struct fb_rect r={0,0,1,1};
    CHECK(engine.ops.fill_rect(&engine,&r,0)==-ETIMEDOUT && !engine.cpu_safe);
    CHECK(fallback()==&engine.ops && !engine.cpu_safe);
    fake.stuck=false;
    CHECK(!fallback() && engine.cpu_safe && engine.quarantined);
    CHECK(!eligible(&fake.fb) && !start(&fake.fb,NULL));
    CHECK(attempt_begin(&filesystem)<0); /* no same-boot reprobe */
    setup(); CHECK(!attempt_begin(&filesystem)); fake.corrupt=true;
    CHECK(!start(&fake.fb,NULL) && engine.quarantined && retained_marker);
    CHECK(!attempt_failed(&filesystem) && retained_marker);
    CHECK(attempt_end(&filesystem)<0 && !truncates);
    setup(); CHECK(!attempt_begin(&filesystem)); fake.after_launch_stuck=true;
    CHECK(!start(&fake.fb,NULL) && !engine.cpu_safe && retained_marker);
    CHECK(engine.hidden && engine.cursor_safe && !engine.cpu_safe); /* cursor hide alone is insufficient */
    CHECK(fallback()==&engine.ops);
    setup(); activate();
    fake.writes=0; commit_failed=true; persisted=true; retained_marker=true;
    CHECK(attempt_end(&filesystem)<0);
    CHECK(!engine.ops.stop(&engine)); quarantine("marker_clear_failed");
    CHECK(!fallback() && engine.cpu_safe && !eligible(&fake.fb));
    CHECK(revokes==2 && releases==3 && !registers && !cursor_mapping);
    setup(); activate();
    CHECK(!engine.ops.stop(&engine) && releases==3 && revokes==2);
    CHECK(!engine.ops.sync(&engine) && !engine.ops.capabilities(&engine));
    /* A clean mode transition can rebind with a fresh framebuffer lease. */
    fake.fb.generation=43; leases[0].generation=43;
    fake.writes=fake.launches=0;
    CHECK(!attempt_begin(&filesystem) && start(&fake.fb,NULL)==&engine.ops);
    CHECK(probe_calls==2 && engine.fb.generation==43 && claims==6);
    setup(); g_boot.display.vram_bytes=fake.fb.size; activate();
    CHECK(!engine.cursor && engine.ops.capabilities(&engine)==1 && claims==2);
    setup(); cursor_map_failed=true; activate();
    CHECK(!engine.cursor && cursor_claim<0 && engine.ops.capabilities(&engine)==1 && releases==1);
    CHECK(!engine.ops.stop(&engine) && releases==3);
}
int main(void)
{
    refusals(); lifecycle();
    puts("NeoMagic registry/lifecycle: ownership, refusals, durable marker, DM9, identity and guard passed");
    return 0;
}
