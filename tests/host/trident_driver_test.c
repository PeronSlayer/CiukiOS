/* Production lifecycle/registry with only I/O, clock and VFS substituted.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "trident_fixture.h"
#include <ciuki/cpu.h>
static uint8_t port_in(uint16_t);
static void port_out(uint16_t,uint8_t);
#define inb port_in
#define outb port_out
#include "../../src/kernel/drivers/video/trident/driver.c"
#undef inb
#undef outb
#include "../../src/kernel/drivers/video/registry.c"

struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
const struct display_driver s3savage_driver={0};
static struct tb_fixture f;
static struct resource leases[3];
static struct pci_func pci;
static unsigned pci_functions,claims,releases,maps,unmaps,port_writes,pixel_probes,commits,closes,drains;
static bool presenter_held,map_fail,claim_fail,probe_fail,corrupt_engine,wedge_engine;
static unsigned marker_bytes;
static int stat_error,open_error,write_error,commit_error,close_error,truncate_error;
static bool short_write;
static uint8_t cr_index,cr_regs[256],seq_index,seq_regs[256];
static struct fat_volume volume;
static struct vfs volume_vfs;
static uint8_t mapped_page[PAGE_SIZE];

static uint8_t port_in(uint16_t p)
{
    if (p==0x3cc) return 1;
    if (p==0x3c4) return seq_index;
    if (p==0x3c5) return seq_regs[seq_index];
    if (p==0x3d4) return cr_index;
    assert(p==0x3d5); return cr_regs[cr_index];
}
static void port_out(uint16_t p,uint8_t v)
{
    port_writes++;
    if (p==0x3c4) { seq_index=v; return; }
    if (p==0x3c5) { assert(seq_index==0x0e || seq_index==0x11); seq_regs[seq_index]=v; return; }
    if (p==0x3d4) { cr_index=v; return; }
    assert(p==0x3d5 && (cr_index==0x39 || cr_index==0x36)); cr_regs[cr_index]=v;
}
uint64_t ktime_cycles(void) { return 0; }
void klog(const char *fmt,...) { (void)fmt; }
void framebuffer_write_drain(void) { drains++; }
unsigned pci_count(void) { return pci_functions; }
const struct pci_func *pci_get(unsigned n) { return n<pci_functions ? &pci : NULL; }
const struct resource *registry_get(unsigned n) { return n<3 ? &leases[n] : NULL; }
bool registry_valid(int h,gen_t g)
{ return h>=0 && h<3 && leases[h].generation==g && leases[h].state==RS_ACTIVE; }
int registry_claim(enum res_type type,uint32_t a,uint32_t b,const char *owner_name,bool share)
{
    (void)share; assert(!presenter_held); claims++;
    if (claim_fail) return -EBUSY;
    unsigned h=type==RES_PORT ? 1 : 2;
    leases[h]=(struct resource){.type=type,.start=a,.end=b,.owner=owner_name,.generation=7,.state=RS_CLAIMED};
    return h;
}
int registry_activate(int h,gen_t g)
{ assert(h>=1 && h<3 && g==leases[h].generation && leases[h].state==RS_CLAIMED); leases[h].state=RS_ACTIVE; return 0; }
int registry_quiesce(int h,gen_t g,registry_idle_proof_fn proof)
{
    assert(g==leases[h].generation && leases[h].state==RS_ACTIVE);
    leases[h].state=RS_QUIESCING;
    if (!proof(h,g)) { leases[h].state=RS_QUARANTINED; return -EIO; }
    return 0;
}
int registry_release(int h,gen_t g)
{ assert(g==leases[h].generation && leases[h].state==RS_QUIESCING); leases[h].state=RS_RELEASED; releases++; return 0; }
void *vmm_reserve_mmio(uint32_t size) { assert(size==PAGE_SIZE); return mapped_page; }
void *vmm_replace_mmio(void *w,uint32_t cap,uint32_t phys,uint32_t bytes)
{ assert(w==mapped_page && cap==PAGE_SIZE && bytes==PAGE_SIZE && phys==0xf0002000); maps++; return map_fail ? NULL : mapped_page; }
void vmm_revoke_mmio(void *p,uint32_t bytes) { assert(p==mapped_page && bytes==PAGE_SIZE); unmaps++; }
const struct fb_device *fbdev_get(void) { return &f.fb; }
unsigned fbdev_native_capabilities(void) { return engine.active && !engine.quarantined ? 1 : 0; }
bool fbdev_cpu_ready(void) { return !started || engine.cpu_safe; }
int fbdev_accel_probe(const struct fb_device *d,int (*check)(void *),void *ctx)
{
    assert(d==&f.fb && ctx==&engine && !presenter_held); pixel_probes++;
    if (probe_fail) return -EBUSY;
    engine.io=fixture_io(&f); f.corrupt=corrupt_engine; f.busy_after=wedge_engine;
    presenter_held=true;
    int err=check(ctx);
    if (err) (void)fallback();
    presenter_held=false;
    return err;
}
int vfs_table_init(struct vfs *v,struct vfs_table *t,unsigned drive)
{ assert(!presenter_held && v==&volume_vfs && t==&marker_table && drive==2); return 0; }
void vfs_table_destroy(struct vfs_table *t) { assert(!presenter_held && t==&marker_table); }
int vfs_stat(struct vfs_table *t,const char *path,struct fat_entry *entry)
{ (void)t; assert(!strcmp(path,ATTEMPT_PATH) && !presenter_held); entry->size=marker_bytes; return stat_error ? stat_error : marker_bytes ? 0 : -FS_ENOENT; }
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share share,uint8_t attr)
{ (void)t; assert(!presenter_held && !strcmp(path,ATTEMPT_PATH) && flags==(VFS_WRITE|VFS_CREATE) && share==VFS_DENY_ALL && !attr); return open_error ? open_error : 4; }
int vfs_write(struct vfs_table *t,int fd,const void *p,size_t bytes,size_t *done)
{ (void)t; assert(!presenter_held && fd==4 && !memcmp(p,"driver=trident\n",sizeof("driver=trident\n")-1)); *done=short_write ? bytes-1 : bytes; marker_bytes=*done; return write_error; }
int vfs_truncate(struct vfs_table *t,int fd,uint64_t size)
{ (void)t; assert(!presenter_held && fd==4 && !size); if (!truncate_error) marker_bytes=0; return truncate_error; }
int vfs_commit(struct vfs_table *t,int fd)
{ (void)t; assert(!presenter_held && fd==4); commits++; return commit_error; }
int vfs_close(struct vfs_table *t,int fd)
{ (void)t; assert(!presenter_held && fd==4); closes++; return close_error; }

static void reset(uint16_t id)
{
    fixture_reset(&f,tb_device(id),32,1024);
    memset(&g_boot,0,sizeof(g_boot));
    g_boot.display.vram_bytes=f.fb.size;
    memset(&engine,0,sizeof(engine)); engine.cpu_safe=true;
    authorized=started=blocked=access_saved=false; window=NULL; registers=NULL; mmio_claim=port_claim=-1;
    claims=releases=maps=unmaps=port_writes=pixel_probes=commits=closes=drains=0;
    map_fail=claim_fail=probe_fail=corrupt_engine=wedge_engine=presenter_held=false;
    memset(native_count,0,sizeof(native_count)); memset(software_count,0,sizeof(software_count));
    marker_bytes=0; stat_error=open_error=write_error=commit_error=close_error=truncate_error=0; short_write=false;
    memset(leases,0,sizeof(leases)); leases[0]=(struct resource){RES_MMIO,f.fb.phys,f.fb.phys+f.fb.size,"fbdev",1,RS_ACTIVE,false};
    pci=(struct pci_func){.vendor=0x1023,.device=id,.command=3,.class_code=3,.bar={f.fb.phys,0xf0000000}};
    pci_functions=1; memset(cr_regs,0,sizeof(cr_regs)); cr_regs[0x39]=1; cr_regs[0x36]=0x80;
    cr_regs[0x13]=f.fb.pitch/8; cr_index=0xab;
    memset(seq_regs,0,sizeof(seq_regs)); seq_regs[0x0e]=0xc0; seq_regs[0x11]=0x90; seq_index=0x77;
    memset(&volume,0,sizeof(volume)); volume.mounted=true;
    memset(&volume_vfs,0,sizeof(volume_vfs)); volume_vfs.volumes[2]=&volume;
}
static void activate(void)
{
    assert(display_driver_bind(&f.fb)==&trident_blade_driver);
    assert(trident_blade_driver.eligible(&f.fb));
    assert(!trident_blade_driver.attempt_begin(&volume_vfs) && authorized && commits==1 && closes==1);
    assert(trident_blade_driver.start(&f.fb,NULL)==&engine.ops);
    assert(engine.active && engine.cpu_safe && !engine.quarantined);
    /* fx-1: colour VGA range only, so an LPT1 at 3BCh cannot conflict. */
    assert(leases[1].type==RES_PORT && leases[1].start==0x3c0 && leases[1].end==0x3e0);
    assert(cr_index==0xab && seq_index==0x77 && seq_regs[0x0e]==0xc0 && seq_regs[0x11]==0x90 && pixel_probes==1 && marker_bytes);
    assert(!trident_blade_driver.attempt_end(&volume_vfs) && !marker_bytes && commits==2);
}
static void selection_tests(void)
{
    reset(0x8400); const struct pci_func *owner_p=NULL;
    const struct display_driver *const table[]={&trident_blade_driver};
    assert(display_driver_select(&f.fb,table,1,&owner_p)==&trident_blade_driver && owner_p==&pci);
    assert(eligible(&f.fb) && !claims && !port_writes && !f.traces);
    pci.device=0x8500; assert(eligible(&f.fb));
    pci.device=0x9750; assert(!probe(&pci) && !eligible(&f.fb));
    pci.device=0x8820; pci.vendor=0x1106; assert(!probe(&pci));
    pci.vendor=0x1023; pci.command=2; assert(!probe(&pci)); pci.command=3;
    pci.class_code=2; assert(!probe(&pci)); pci.class_code=3;
    pci.bar[0]+=PAGE_SIZE; assert(!eligible(&f.fb)); pci.bar[0]=f.fb.phys;
    pci_functions=2; assert(!eligible(&f.fb)); pci_functions=1;
    pci.bar[1]|=4; assert(!eligible(&f.fb)); pci.bar[1]=0xffffe000; assert(!eligible(&f.fb));
    pci.bar[1]=f.fb.phys; assert(!eligible(&f.fb)); pci.bar[1]=0xf0000000;
    g_boot.e820_count=1; g_boot.e820[0]=(struct ciuki_e820){.base=0xf0002000,.length=PAGE_SIZE,.type=CBI_E820_RAM};
    assert(!eligible(&f.fb)); g_boot.e820_count=0;
    g_boot.kernel_start=0xf0002000; g_boot.kernel_end=0xf0003000; assert(!eligible(&f.fb));
    g_boot.kernel_start=g_boot.kernel_end=0;
    leases[0].generation++; assert(!eligible(&f.fb)); leases[0].generation--;
    f.fb.width=15; assert(!eligible(&f.fb)); f.fb.width=32;
    g_boot.flags=CBI_F_SAFE_MODE; assert(!eligible(&f.fb) && attempt_begin(&volume_vfs)); g_boot.flags=0;
    strcpy(g_boot.options,"accel=0"); assert(!eligible(&f.fb) && attempt_begin(&volume_vfs));
    assert(!claims && !f.traces && !port_writes && !commits);
}
static void marker_tests(void)
{
    reset(0x9880); assert(!start(&f.fb,NULL) && !claims);
    marker_bytes=5; assert(attempt_begin(&volume_vfs) && blocked && !authorized && !claims);
    reset(0x9880); volume.readonly=true; assert(attempt_begin(&volume_vfs) && !claims);
    for (unsigned phase=0;phase<6;phase++) {
        reset(0x9880);
        if (!phase) stat_error=-FS_EIO;
        else if (phase==1) open_error=-FS_EIO;
        else if (phase==2) write_error=-FS_EIO;
        else if (phase==3) short_write=true;
        else if (phase==4) commit_error=-FS_EIO;
        else close_error=-FS_EIO;
        assert(attempt_begin(&volume_vfs) && blocked && !authorized);
        assert(!start(&f.fb,NULL) && !claims && !f.traces);
    }
    reset(0x9880); activate(); commit_error=-FS_EIO;
    assert(attempt_end(&volume_vfs)); quarantine("marker_clear_failed");
    assert(!engine.active && engine.quarantined && !engine.cpu_safe);
    assert(fallback()==&engine.ops && engine.cpu_safe && !engine.ops.capabilities(&engine));
    assert(!engine.ops.stop(&engine) && releases==2 && unmaps==1);
    assert(!eligible(&f.fb));
}
int main(void)
{
    selection_tests(); marker_tests();
    for (unsigned kind=0;kind<2;kind++) {
        reset(kind ? 0x8820 : 0x9880);
        uint8_t original[16*16*4];
        for (unsigned y=0;y<16;y++) memcpy(original+y*64,f.pixels+y*f.fb.pitch,64);
        activate(); assert(f.commands==4 && drains && claims==2);
        for (unsigned y=0;y<16;y++) assert(!memcmp(original+y*64,f.pixels+y*f.fb.pitch,64));
        struct ciuki_display_engine_info info;
        display_driver_info(&info);
        assert(!strcmp(info.driver_id,"trident") && !strcmp(info.display_name,"Trident Blade / CyberBlade"));
        assert(info.ready && info.kind==CIUKI_ENGINE_NATIVE && !info.native_used);
        struct fb_rect r={1,1,3,3}; assert(!engine.ops.fill_rect(&engine,&r,1));
        display_driver_count(CIUKI_ENGINE_FILL,true); display_driver_info(&info); assert(info.native_used);
        assert(!engine.ops.stop(&engine) && releases==2 && unmaps==1 && !registers);
        assert(!engine.ops.stop(&engine) && releases==2);
    }
    reset(0x9880); cr_regs[0x39]=0x26; cr_regs[0x36]=0x12;
    activate(); assert(cr_regs[0x39]==0x27 && cr_regs[0x36]==0x92);
    assert(!engine.ops.stop(&engine) && cr_regs[0x39]==0x26 && cr_regs[0x36]==0x12);
    reset(0x9880); assert(!attempt_begin(&volume_vfs)); cr_regs[0x0d]=1;
    assert(!start(&f.fb,NULL) && !maps && releases==1);
    reset(0x9880); assert(!attempt_begin(&volume_vfs)); cr_regs[0x13]=1;
    assert(!start(&f.fb,NULL) && !maps);
    reset(0x9880); assert(!attempt_begin(&volume_vfs)); map_fail=true;
    assert(!start(&f.fb,NULL) && releases==2 && !f.traces);
    reset(0x9880); assert(!attempt_begin(&volume_vfs)); claim_fail=true;
    assert(!start(&f.fb,NULL) && !maps && !f.traces);
    reset(0x9880); assert(!attempt_begin(&volume_vfs)); probe_fail=true;
    assert(!start(&f.fb,NULL) && !engine.quarantined && !fallback() && engine.cpu_safe && releases==2 && !f.traces);
    reset(0x9880); assert(!attempt_begin(&volume_vfs)); corrupt_engine=true;
    assert(!start(&f.fb,NULL) && engine.quarantined && !eligible(&f.fb));
    assert(fallback()==&engine.ops && engine.cpu_safe && !engine.ops.capabilities(&engine));
    reset(0x8820); assert(!attempt_begin(&volume_vfs)); wedge_engine=true;
    assert(!start(&f.fb,NULL) && engine.quarantined && !engine.cpu_safe);
    assert(fallback()==&engine.ops && !engine.cpu_safe && !releases && !unmaps && marker_bytes);
    assert(engine.ops.stop(&engine)==-ETIMEDOUT && !releases && !unmaps);
    assert(!attempt_failed(&volume_vfs) && marker_bytes && !eligible(&f.fb));
    f.busy=false;
    assert(fallback()==&engine.ops && engine.cpu_safe && engine.quarantined);
    assert(!engine.ops.stop(&engine) && releases==2 && unmaps==1);
    reset(0x9880); registers=mapped_page;
    write32(NULL,0x2127,0x12345678); assert(read32(NULL,0x2127)==0x12345678);
    write8(NULL,0x2125,0x7f); assert(read8(NULL,0x2125)==0x7f);
    puts("Trident production registry/lifecycle: PCI ownership, no-command probe, firmware refusal, durable markers, native read-back activation, identity, quarantine, unsafe guard and resource stop PASS");
}
