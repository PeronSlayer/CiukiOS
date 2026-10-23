/* Real registry and native lifecycle, with resource/VFS boundaries faked.
 * Includes only Ciuki adaptation glue to exercise the private marker path.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
#include "../../src/kernel/drivers/video/nv2d/nv2d.h"
/* Keep the complete production lifecycle; substitute only its I/O boundary. */
int nv2d_fixture_init(struct nv2d *,const struct nv2d_io *,const struct fb_device *,
                      uint16_t,uint32_t,volatile uint8_t *);
#define nv2d_engine_init nv2d_fixture_init
#include "../../src/kernel/drivers/video/nv2d/adapter.c"
#undef nv2d_engine_init
#define NV2D_ADAPTER_FIXTURE
#include "nv2d_fixture.h"
int nv2d_fixture_init(struct nv2d *s,const struct nv2d_io *io_arg,const struct fb_device *f,
                     uint16_t id,uint32_t vram,volatile uint8_t *tail_arg)
{
    assert(io_arg==&io && io_valid(NULL));
    return nv2d_engine_init(s,&model_io,f,id,vram,tail_arg);
}
struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
const struct display_driver s3savage_driver={0};
static struct fb_device fb;
static struct pci_func pci[2];
static unsigned functions=1, claims, maps, file_ops, opens, commits, closes, destroys;
static struct resource lease, extra[2];
static bool allow_maps;
static unsigned probes;
static struct vfs filesystem;
static struct fat_volume volume;
static bool exists, short_write;
static uint32_t file_size;
static int stat_error, init_error, open_error, commit_error, close_error, write_error, truncate_error;
static char last_path[64], stored[64];
unsigned pci_count(void) { return functions; }
const struct pci_func *pci_get(unsigned n) { return n<functions ? pci+n : NULL; }
bool registry_valid(int h,gen_t gen)
{
    const struct resource *r=h>=0 ? registry_get((unsigned)h) : NULL;
    return r && gen==r->generation && r->state==RS_ACTIVE;
}
const struct resource *registry_get(unsigned n)
{ return n==0 ? &lease : n<=2 ? &extra[n-1] : NULL; }
int registry_claim(enum res_type t,uint32_t a,uint32_t b,const char *owner,bool share)
{
    assert(t==RES_MMIO && !share);claims++;
    if (!allow_maps) return -EBUSY;
    assert(claims<=2);
    extra[claims-1]=(struct resource){.type=t,.start=a,.end=b,.owner=owner,.generation=9,.state=RS_CLAIMED};
    return (int)claims;
}
int registry_activate(int h,gen_t gen)
{ assert(h>0 && h<=2 && gen==9);extra[h-1].state=RS_ACTIVE;return 0; }
int registry_quarantine(int h,gen_t gen)
{ assert(h>0 && h<=2 && gen==9);extra[h-1].state=RS_QUARANTINED;return 0; }
void *vmm_reserve_mmio(uint32_t n)
{
    maps++;
    return n==NV2D_MMIO_BYTES ? (void *)m.regs : (void *)m.tail;
}
void *vmm_replace_mmio(void *w,uint32_t n,uint32_t p,uint32_t b)
{ assert(n==b && p);maps++;return w; }
const struct fb_device *fbdev_get(void) { return &fb; }
unsigned fbdev_native_capabilities(void)
{ return engine.ops.capabilities ? engine.ops.capabilities(&engine) : 0; }
bool fbdev_cpu_ready(void) { return !engine.touched || engine.cpu_safe; }
int fbdev_accel_probe(const struct fb_device *f,int (*fn)(void *),void *ctx)
{ assert(f==&fb && persisted && f->generation==activation_fb.generation);probes++;return fn(ctx); }
int fbdev_accel_diagnostic(struct vfs *v,uint64_t d,bool (*c)(void *),void *ctx,uint32_t *out)
{ (void)v;(void)d;(void)c;(void)ctx;*out=0;return -EOPNOTSUPP; }
void framebuffer_write_drain(void) { }
uint64_t ktime_cycles(void) { return 0; }
int udelay(uint32_t us) { (void)us;return 0; }
void klog(const char *fmt,...) { (void)fmt; }
int vfs_table_init(struct vfs *v,struct vfs_table *t,unsigned drive)
{ assert(v==&filesystem && t==&marker_table && drive==2);file_ops++;return init_error; }
void vfs_table_destroy(struct vfs_table *t) { assert(t==&marker_table);destroys++; }
int vfs_stat(struct vfs_table *t,const char *path,struct fat_entry *out)
{
    assert(t==&marker_table);file_ops++;
    char expected[64];snprintf(expected,sizeof(expected),"C:/system/settings/nv2d%04x.cfg",pci[0].device);
    assert(!strcmp(path,expected));
    if (stat_error) return stat_error;
    if (!exists) return -FS_ENOENT;
    out->size=file_size;return 0;
}
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share share,uint8_t attr)
{
    assert(t==&marker_table && flags==(VFS_WRITE|VFS_CREATE) && share==VFS_DENY_ALL && !attr);
    opens++;snprintf(last_path,sizeof(last_path),"%s",path);
    if (open_error) return open_error;
    exists=true;return 7;
}
int vfs_write(struct vfs_table *t,int fd,const void *data,size_t n,size_t *done)
{
    assert(t==&marker_table && fd==7 && n<sizeof(stored));
    if (write_error) return write_error;
    *done=short_write ? n-1 : n;memcpy(stored,data,*done);stored[*done]=0;file_size=(uint32_t)*done;
    return 0;
}
int vfs_truncate(struct vfs_table *t,int fd,uint64_t size)
{ assert(t==&marker_table && fd==7 && !size);if (!truncate_error) file_size=0;return truncate_error; }
int vfs_commit(struct vfs_table *t,int fd)
{ assert(t==&marker_table && fd==7);commits++;return commit_error; }
int vfs_close(struct vfs_table *t,int fd)
{ assert(t==&marker_table && fd==7);closes++;return close_error; }
static void baseline(void)
{
    memset(&g_boot,0,sizeof(g_boot)); memset(&engine,0,sizeof(engine)); memset(&m,0,sizeof(m));
    memset(extra,0,sizeof(extra));
    register_claim=tail_claim=-1;registers=tail_mapping=NULL;register_window=tail_window=NULL;
    claims=maps=file_ops=probes=opens=commits=closes=destroys=0;allow_maps=false;
    exists=short_write=false;file_size=0;
    stat_error=init_error=open_error=commit_error=close_error=write_error=truncate_error=0;
    m.valid=true;m.width=m.height=16;m.bytes=4;m.pitch=64;
    m.tail_offset=m.scratch_offset=(8<<20)-NV2D_TAIL_BYTES;
    m.regs[0x600860/4]=0x1000;m.regs[0x10020c/4]=8<<20;
    fb=(struct fb_device){.width=16,.height=16,.pitch=64,.size=16*16*4,.mapped=m.pixels,
        .phys=0xe0000000,.bpp=32,.red_size=8,.red_pos=16,.green_size=8,.green_pos=8,
        .blue_size=8,.present=true,.handle=0,.generation=9};
    lease=(struct resource){.type=RES_MMIO,.start=fb.phys,.end=fb.phys+fb.size,
        .owner="fbdev",.generation=9,.state=RS_ACTIVE};
    pci[0]=(struct pci_func){.vendor=0x10de,.device=0x0250,.class_code=3,.command=2,
        .bar={0xf0000000,0xe0000008}};
    functions=1;g_boot.display.vram_bytes=8<<20;
    filesystem.volumes[2]=&volume;volume.mounted=true;volume.readonly=false;
    blocked=persisted=diagnostic_marker=false;declined="pending";
    assert(display_driver_bind(&fb)==&nv2d_driver);
}
/* Configure the model's VRAM layout independently for each transport. */
static void device_baseline(uint16_t id)
{
    baseline();
    pci[0].device=id;
    uint16_t chip=id==0x00f1 ? 0x0040 : id;
    unsigned arch=nv2d_architecture(chip);
    m.regs[0x1800/4]=((uint32_t)chip<<16)|0x10de;
    m.tail_offset=(8<<20)-nv2d_tail_bytes(arch);
    m.scratch_offset=arch>=0x40 ? (8<<20)-576*1024 : m.tail_offset;
    assert(display_driver_bind(&fb)==&nv2d_driver);
}
static void selection(void)
{
    baseline();
    for (unsigned i=0;i<nv2d_driver.match_count;i++) {
        pci[0].device=nv2d_driver.matches[i].device;
        assert(probe(&pci[0]) && display_driver_bind(&fb)==&nv2d_driver);
    }
    const uint16_t refuse[]={0x0018,0x0021,0x0189,0x0329,0x016a,0x032f,0x00f5,0x0290,0xffff};
    for (unsigned i=0;i<ARRAY_SIZE(refuse);i++) {
        pci[0].device=refuse[i];assert(!probe(&pci[0]) && !display_driver_bind(&fb));
    }
    baseline();pci[0].vendor=0x12d2;assert(!probe(&pci[0]) && !display_driver_bind(&fb));
    baseline();pci[0].subclass=1;assert(!probe(&pci[0]));
    baseline();pci[0].command=0;assert(!display_driver_bind(&fb));
    baseline();pci[0].bar[1]+=0x100000;assert(!display_driver_bind(&fb));
    baseline();lease.generation=9;fb.generation++;assert(!display_driver_bind(&fb));
    baseline();lease.end--;assert(!display_driver_bind(&fb));
    baseline();lease.owner="foreign";assert(!display_driver_bind(&fb));
    baseline();functions=2;pci[1]=pci[0];assert(!display_driver_bind(&fb));
    baseline();g_boot.e820_count=1;g_boot.e820[0]=(struct ciuki_e820){.base=fb.phys,.length=8<<20,.type=CBI_E820_RAM};
    assert(!display_driver_bind(&fb));
    baseline();g_boot.e820_count=1;g_boot.e820[0]=(struct ciuki_e820){.base=0xf0000000,.length=NV2D_MMIO_BYTES,.type=CBI_E820_RAM};
    assert(!nv2d_driver.eligible(&fb)); /* BAR0 RAM alias is an extra port gate */
    baseline();assert(!claims && !maps && !file_ops && !m.traced);
}
static void policy_and_records(void)
{
    baseline();assert(transport_reviewed && ring_transport_reviewed);
    assert(eligible(&fb));
    struct ciuki_display_engine_info info;
    display_driver_info(&info);
    assert(!strcmp(info.driver_id,"nv2d") && !strcmp(info.display_name,"NVIDIA TNT-GeForce6"));
    assert(info.kind==CIUKI_ENGINE_SOFTWARE && !info.ready && !info.native_used && info.cpu_ready);
    const uint16_t approved[]={0x0322,0x0040,0x00f1};
    for (unsigned i=0;i<ARRAY_SIZE(approved);i++) {
        pci[0].device=approved[i];assert(eligible(&fb));
    }
    const uint16_t uma[]={0x00a0,0x01a0,0x01f0,0x0240,0x0242};
    for (unsigned i=0;i<ARRAY_SIZE(uma);i++) {
        pci[0].device=uma[i];assert(!eligible(&fb) && !strcmp(declined,"uma_ownership_unproved"));
    }
    g_boot.flags=CBI_F_SAFE_MODE;assert(!eligible(&fb) && !strcmp(declined,"safe_mode"));
    g_boot.flags=0;strcpy(g_boot.options,"accel=0");
    assert(!eligible(&fb) && !strcmp(declined,"boot_disabled"));
    assert(!claims && !maps && !file_ops);
}
static void activation(uint16_t id)
{
    device_baseline(id);allow_maps=true;exists=false;file_size=0;
    assert(!start(&fb,&filesystem) && !claims); /* marker first */
    assert(!attempt_begin(&filesystem));
    uint8_t saved[16*16*4];memcpy(saved,m.pixels,sizeof(saved));
    const struct accel_ops *ops=start(&fb,&filesystem);
    assert(ops==&engine.ops && engine.active && !engine.quarantined && engine.cpu_safe);
    assert(probes==1 && claims==2 && maps==4);
    assert(engine.dma==(nv2d_architecture(engine.device)>=0x30));
    assert(!engine.dma || (m.packets && engine.put==reg(NV2D_GET)/4));
    engine.ops.set_pause(&engine,pause_wait,&m);
    struct ciuki_display_engine_info info;
    display_driver_info(&info);
    assert(info.ready && info.kind==CIUKI_ENGINE_NATIVE && !info.native_used && info.cpu_ready);
    assert(!strcmp(info.display_name,"NVIDIA TNT-GeForce6"));
    assert(!memcmp(saved,m.pixels,sizeof(saved)) && io_valid(NULL));
    assert(!attempt_end(&filesystem) && !file_size);
    assert(!ops->stop(ops->ctx));
    assert(!attempt_begin(&filesystem) && start(&fb,&filesystem)==ops);
    assert(claims==2 && maps==4 && probes==2); /* exact reservations reused */
    assert(!attempt_end(&filesystem));
    fb.generation++; assert(!io_valid(NULL));fb.generation--;
    assert(!ops->stop(ops->ctx));
    device_baseline(id);allow_maps=true;exists=false;m.drop_draw=true;
    assert(!attempt_begin(&filesystem));
    assert(!start(&fb,&filesystem) && engine.quarantined && !engine.cpu_safe && file_size);
    assert(probes==1 && !strcmp(engine.reason,"native_comparison_failed"));
    display_driver_info(&info);
    assert(!info.ready && !info.cpu_ready);
    assert(attempt_end(&filesystem)==-EIO && !eligible(&fb));
    m.stuck=true;
    const struct accel_ops *guard=fallback();
    assert(guard && !guard->cpu_ready(guard->ctx));
    m.stuck=false;guard=fallback();
    assert(guard && guard->cpu_ready(guard->ctx) && engine.quarantined);
    unsigned methods=m.methods;
    struct fb_rect software={0,0,4,4};
    swdib_fill(&fb,&software,0x123456);
    assert(m.methods==methods && m.pixels[0]==0x56); /* CPU only after idle */
    assert(attempt_end(&filesystem)==-EIO && file_size);
}
static void activation_refusals(uint16_t id)
{
    /* Refusals reach their predicates rather than a transport constant. */
    device_baseline(id);allow_maps=true;exists=false;m.regs[0x600860/4]=0;
    assert(!attempt_begin(&filesystem) && !start(&fb,&filesystem) && !engine.touched);
    device_baseline(id);allow_maps=true;exists=false;m.regs[0x600800/4]=64;
    assert(!attempt_begin(&filesystem) && !start(&fb,&filesystem) && !engine.touched);
    device_baseline(id);fb.bpp=24;assert(!eligible(&fb) && !claims);
    device_baseline(id);allow_maps=true;exists=false;m.regs[0x3220/4]=1;
    assert(!attempt_begin(&filesystem) && !start(&fb,&filesystem) && !engine.touched);
    if (id==0x0040 || id==0x00f1) {
        device_baseline(id);allow_maps=true;g_boot.display.vram_bytes=4<<20;
        assert(!attempt_begin(&filesystem) && !start(&fb,&filesystem));
        assert(!strcmp(declined,"instance_extent_unproved") && !engine.touched && !m.traced && claims==1);
    }
    if (id==0x00f1) {
        device_baseline(id);allow_maps=true;m.regs[0x1800/4]=0x029010de;
        assert(!attempt_begin(&filesystem) && !start(&fb,&filesystem));
        assert(!strcmp(declined,"chipset_unproved") && !engine.touched && !m.traced && claims==1);
    }
}
static void markers(void)
{
    baseline();bool previous;
    assert(!marker(&filesystem,false,true,&previous) && !previous && !opens);
    assert(!marker(&filesystem,false,false,&previous) && !previous);
    assert(!strcmp(stored,"driver=nv2d\npci=10de:0250\nstate=attempt\n"));
    assert(opens==1 && commits==1 && closes==1 && file_size);
    unsigned n=opens;
    assert(!marker(&filesystem,false,false,&previous) && previous && n==opens);
    assert(!marker(&filesystem,false,true,&previous) && previous && n==opens);
    persisted=false;assert(!attempt_end(&filesystem) && n==opens); /* cannot clear an unowned marker */
    persisted=true;assert(!attempt_end(&filesystem) && !file_size && !persisted);
    assert(!marker(&filesystem,false,false,&previous));
    engine.quarantined=true;assert(attempt_end(&filesystem)==-EIO && file_size);
    engine.quarantined=false;blocked=true;assert(attempt_end(&filesystem)==-EIO);
    blocked=false;diagnostic_marker=true;assert(attempt_end(&filesystem)==-EIO);
    diagnostic_marker=false;exists=false;file_size=0;
    volume.readonly=true;assert(marker(&filesystem,false,false,&previous)==-FS_EROFS);
    volume.readonly=false;stat_error=-FS_EIO;
    assert(marker(&filesystem,false,false,&previous)==-FS_EIO);
    stat_error=0;init_error=-FS_EIO;
    assert(marker(&filesystem,false,false,&previous)==-FS_EIO);
    init_error=0;open_error=-FS_EIO;
    assert(marker(&filesystem,false,false,&previous)==-FS_EIO);
    open_error=0;short_write=true;
    assert(marker(&filesystem,false,false,&previous)==-FS_EIO);
    short_write=false;exists=false;commit_error=-FS_EIO;
    assert(marker(&filesystem,false,false,&previous)==-FS_EIO && file_size);
    commit_error=0;exists=false;close_error=-FS_EIO;
    assert(marker(&filesystem,false,false,&previous)==-FS_EIO && file_size);
    close_error=0;exists=false;write_error=-FS_EIO;
    assert(marker(&filesystem,false,false,&previous)==-FS_EIO);
    write_error=0;truncate_error=-FS_EIO;
    assert(marker(&filesystem,true,false,&previous)==-FS_EIO);
    assert(destroys && closes && !strcmp(last_path,"C:/system/settings/nv2d0250.cfg"));
}
int main(void)
{
    selection();policy_and_records();
    const uint16_t devices[]={0x0250,0x0322,0x0040,0x00f1};
    for (unsigned i=0;i<ARRAY_SIZE(devices);i++) {
        activation(devices[i]);activation_refusals(devices[i]);
    }
    markers();
    puts("nv2d adapter: exact PCI table, registry ownership/refusal, approved PIO/NV30/NV40/bridge DM9 pass/mismatch, independent refusals, generic display identity and durable marker failure paths PASS");
    return 0;
}
