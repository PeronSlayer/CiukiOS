/* Actual native adapter, registry, fbdev and swdib; fake hardware/VFS only.
 * SPDX-License-Identifier: GPL-2.0-only */
#define DISPLAY_DRIVER_FIXTURE_REAL
#define main framebuffer_regression_main
/* Keep the existing framebuffer fixture's one-lease boundary. Driver MMIO
 * uses the separate lease model below; h=0 is the same active framebuffer. */
#define registry_get fixture_registry_get
#define registry_count fixture_registry_count
#define registry_claim fixture_registry_claim
#define registry_activate fixture_registry_activate
#define registry_quarantine fixture_registry_quarantine
#define registry_valid fixture_registry_valid
#define vmm_reserve_mmio fixture_vmm_reserve_mmio
#include "fbdev_test.c"
#undef main
#undef registry_get
#undef registry_count
#undef registry_claim
#undef registry_activate
#undef registry_quarantine
#undef registry_valid
#undef vmm_reserve_mmio
#include "mach64_fixture.h"
#include "../../src/kernel/fs/vfs.h"
static struct pci_func pci_gpu;
static unsigned pci_functions,claims,marker_writes,marker_clears,commits,closes,io_calls;
static bool mapping_failed,claim_failed,cursor_denied,marker_short,prior_marker;
static int marker_error,commit_error,close_error;
static struct resource leases[3];
static uint8_t lfb[32*32*4],original[sizeof(lfb)],cursor_page[4096];
static struct vfs volume;
static struct fat_volume mounted_volume;
static bool inside_probe;
static unsigned presenter_pauses;
static bool finish_on_pause;
unsigned pci_count(void) { return pci_functions; }
const struct pci_func *pci_get(unsigned i) { return i<pci_functions ? &pci_gpu : NULL; }
unsigned registry_count(void) { return 3; }
const struct resource *registry_get(unsigned i) { return !i ? &reservation : i<3 ? &leases[i] : NULL; }
bool registry_valid(int h,gen_t gen)
{
    const struct resource *r=h>=0 ? registry_get((unsigned)h) : NULL;
    return r && r->generation==gen && r->state!=RS_QUARANTINED && r->state!=RS_RELEASED;
}
int registry_claim(enum res_type t,uint32_t first,uint32_t last,const char *owner,bool share)
{
    assert(!locks && !share && t==RES_MMIO); claims++;
    unsigned n=strstr(owner,"cursor") ? 2 : 1;
    if (claim_failed || (n==2 && cursor_denied)) return -EBUSY;
    leases[n]=(struct resource){.type=t,.start=first,.end=last,.owner=owner,.generation=100+n,.state=RS_CLAIMED};
    return (int)n;
}
int registry_activate(int h,gen_t gen)
{ assert(h>0 && h<3 && leases[h].generation==gen && !locks); leases[h].state=RS_ACTIVE; return 0; }
int registry_quarantine(int h,gen_t gen)
{ assert(h>0 && h<3 && leases[h].generation==gen); leases[h].state=RS_QUARANTINED; return 0; }
void *vmm_reserve_mmio(uint32_t bytes)
{ assert(bytes==PAGE_SIZE && !locks); return mapping_failed ? NULL : (void *)model.reg; }
void *vmm_replace_mmio(void *window,uint32_t capacity,uint32_t phys,uint32_t bytes)
{
    assert(window && capacity==PAGE_SIZE && bytes==PAGE_SIZE && !locks);
    return phys==(pci_gpu.bar[2]&~15u) ? (void *)model.reg : cursor_page;
}
uint64_t ktime_cycles(void) { return model.clock; }
int vfs_table_init(struct vfs *v,struct vfs_table *t,unsigned drive)
{ (void)t; assert(v==&volume && drive==2 && !locks); io_calls++; return marker_error; }
void vfs_table_destroy(struct vfs_table *t) { (void)t; assert(!locks); }
int vfs_stat(struct vfs_table *t,const char *path,struct fat_entry *entry)
{ (void)t; assert(strstr(path,"accel.cfg") && !locks); entry->size=prior_marker ? 11 : 0; return 0; }
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share share,uint8_t attr)
{ (void)t; (void)flags; (void)share; (void)attr; assert(strstr(path,"accel.cfg") && !locks); return 1; }
int vfs_write(struct vfs_table *t,int fd,const void *data,size_t count,size_t *done)
{
    (void)t; assert(fd==1 && !locks && strstr(data,"driver=mach64\npci=1002:"));
    marker_writes++; *done=marker_short ? count-1 : count; return 0;
}
int vfs_truncate(struct vfs_table *t,int fd,uint64_t bytes)
{ (void)t; assert(fd==1 && !bytes && !locks); marker_clears++; return 0; }
int vfs_commit(struct vfs_table *t,int fd)
{ (void)t; assert(fd==1 && !locks); commits++; return commit_error; }
int vfs_close(struct vfs_table *t,int fd)
{ (void)t; assert(fd==1 && !locks); closes++; return close_error; }
const struct display_driver s3savage_driver={0};
#include "../../src/kernel/drivers/video/registry.c"
#include "../../src/kernel/drivers/video/mach64/driver.c"

static uint32_t checked_read(void *ctx,unsigned r) { return model_read(ctx,r); }
static void checked_write(void *ctx,unsigned r,uint32_t v)
{
    if (r!=GEN_TEST_CNTL) assert(locks==1 && drawing==PRESENT_BUSY);
    if (inside_probe && drawing==PRESENT_BUSY) assert(!fbdev_console_begin());
    model_write(ctx,r,v);
}
static void setup_native(void)
{
    memset(&g_boot,0,sizeof(g_boot)); g_boot.display.vram_bytes=4*1024*1024;
    device=(struct fb_device){.width=32,.height=32,.pitch=128,.size=sizeof(lfb),.phys=0xe0000000,
        .mapped=lfb,.bpp=32,.red_size=8,.red_pos=16,.green_size=8,.green_pos=8,.blue_size=8,
        .rsvd_size=8,.rsvd_pos=24,.present=true,.handle=0,.generation=42};
    reservation=(struct resource){.type=RES_MMIO,.start=device.phys,.end=device.phys+device.size,
        .owner="fbdev",.state=RS_ACTIVE,.generation=42};
    pci_gpu=(struct pci_func){.vendor=0x1002,.device=0x4c4d,.class_code=3,.command=2,
        .bar={device.phys,0,0xe1000000}};
    pci_functions=1; claims=marker_writes=marker_clears=commits=closes=io_calls=0;
    mapping_failed=claim_failed=cursor_denied=marker_short=prior_marker=false;
    marker_error=commit_error=close_error=0;
    memset(leases,0,sizeof(leases)); memset(&engine,0,sizeof(engine));
    reason="pending"; blocked=marker_owned=false; registers=NULL; cursor_mapping=NULL;
    register_claim=cursor_claim=-1; register_window=cursor_window=NULL;
    cursor_offset=cursor_phys=register_phys=register_generation=cursor_generation=0;
    acceleration=NULL; adapter=NULL; bound=NULL; owner=NULL;
    initialized=true; changing=accel_queued=accel_initializing=accel_inflight=accel_blocked=false;
    drawing=console_rows=0; locks=0; g_ticks=0;
    accel_marker_task=NULL; accel_marker_request=ACCEL_MARKER_IDLE;
    memset(native_count,0,sizeof(native_count)); memset(software_count,0,sizeof(software_count));
    memset(&cached_snapshot,0,sizeof(cached_snapshot)); software_reason="pending";
    memset(&mounted_volume,0,sizeof(mounted_volume)); mounted_volume.mounted=true;
    memset(&volume,0,sizeof(volume)); volume.volumes[2]=&mounted_volume;
    for (unsigned i=0;i<sizeof(lfb);i++) lfb[i]=(uint8_t)(i*71+13);
    memcpy(original,lfb,sizeof(lfb)); memset(cursor_page,0,sizeof(cursor_page));
    model_reset(&device); io=model_io(); io.read=checked_read; io.write=checked_write;
    inside_probe=true;
    presenter_pauses=0; finish_on_pause=false;
    recorded=false; last_reason=NULL;
}
void grants_display_yield(void)
{
    assert(!locks && drawing==PRESENT_BUSY && accel_inflight); presenter_pauses++;
    assert(!fbdev_console_begin() && fbdev_change_begin()==-EBUSY);
    struct ciuki_accel_request q={.size=sizeof(q),.generation=42,.operation=CIUKI_ACCEL_CURSOR_SHOW};
    assert(fbdev_accel_request(&q)==-EBUSY);
    if (finish_on_pause) model.reg[GUI_STAT/4]=0;
}
void console_framebuffer_revoke(void) { }
void console_framebuffer_changed(const struct fb_device *d) { (void)d; }
static void activate_native(void)
{
    fbdev_accel_schedule(&volume);
    for (unsigned i=0;i<4 && accel_marker_request!=ACCEL_MARKER_IDLE;i++) accel_marker_run();
    assert(!locks);
}
static void selection(void)
{
    setup_native();
    for (unsigned i=0;i<ARRAY_SIZE(matches);i++) {
        pci_gpu.device=matches[i].device;
        assert(display_driver_bind(&device)==&mach64_driver);
    }
    assert(!claims && !model.count && !model.reads && !io_calls);
    pci_gpu.device=0x4758; assert(!display_driver_bind(&device));
    pci_gpu.device=0x4358; assert(!display_driver_bind(&device));
    pci_gpu.device=0x5245; assert(!display_driver_bind(&device));
    pci_gpu.device=0x4c4d;
    pci_gpu.bar[0]+=0x1000; assert(!display_driver_bind(&device)); pci_gpu.bar[0]-=0x1000;
    pci_gpu.bar[2]=0; assert(!display_driver_bind(&device)); pci_gpu.bar[2]=0xe1000004; assert(!display_driver_bind(&device));
    pci_gpu.bar[2]=0xe1000001; assert(!display_driver_bind(&device)); pci_gpu.bar[2]=0xe1000000;
    pci_functions=2; assert(!display_driver_bind(&device)); pci_functions=1;
    reservation.generation++; assert(!display_driver_bind(&device)); reservation.generation--;
    g_boot.e820_count=1; g_boot.e820[0]=(struct ciuki_e820){.base=pci_gpu.bar[2],.length=4096,.type=CBI_E820_RAM};
    assert(!display_driver_bind(&device));
    setup_native(); g_boot.flags=CBI_F_SAFE_MODE; activate_native();
    assert(!claims && !io_calls && !model.count);
    setup_native(); strcpy(g_boot.options,"accel=0"); activate_native();
    assert(!claims && !io_calls && !model.count);
    setup_native(); device.bpp=24; activate_native(); assert(!claims && !io_calls && !model.count);
}
static void lifecycle(void)
{
    setup_native(); activate_native();
    assert(acceleration==&engine.ops && engine.active && marker_writes==1 && marker_clears==1);
    assert(commits==2 && closes==2 && !marker_owned && claims==2);
    assert(!memcmp(lfb,original,sizeof(lfb)) && fbdev_cpu_ready());
    assert(fbdev_native_capabilities()==3);
    struct ciuki_display_engine_info info;
    display_driver_info(&info);
    assert(info.ready && !info.native_used && !strcmp(info.driver_id,"mach64") &&
        !strcmp(info.display_name,"ATI Mach64 / Rage Mobility") && info.pci_device==0x4c4d);
    struct ciuki_accel_request q={.size=sizeof(q),.generation=42,.operation=CIUKI_ACCEL_FILL,
        .rect={.width=8,.height=8},.colour=0x1256ab};
    assert(!fbdev_accel_request(&q)); display_driver_info(&info);
    assert(info.native_used && info.native[CIUKI_ENGINE_FILL]==1);
    /* Unsupported PATTERN is dispatched to the reference engine. */
    unsigned launched=model.launches;
    q.operation=CIUKI_ACCEL_PATTERN; q.pattern[0]=0x80;
    assert(!fbdev_accel_request(&q) && model.launches==launched);
    display_driver_info(&info); assert(info.software[CIUKI_ENGINE_PATTERN]==1);

    /* The native completion wait releases the presenter but retains the
     * engine/drawing claim, excluding modes, console and another request. */
    model.busy_after_launch=true; finish_on_pause=true;
    q.operation=CIUKI_ACCEL_FILL;
    assert(!fbdev_accel_request(&q) && presenter_pauses && !accel_inflight && !drawing);

    setup_native(); activate_native(); model.hang_launch=true;
    q.operation=CIUKI_ACCEL_COPY; q.rect=(struct ciuki_rect){.width=8,.height=8,.dst_x=1,.dst_y=1};
    uint8_t first=lfb[0];
    assert(fbdev_accel_request(&q)==-EAGAIN && lfb[0]==(uint8_t)(first^0x55));
    assert(model.launches==3 && engine.quarantined && !fbdev_cpu_ready());
    display_driver_info(&info); assert(info.quarantined && !info.ready && !info.cpu_ready);

    setup_native(); cursor_denied=true; activate_native();
    assert(acceleration && fbdev_native_capabilities()==1);
    setup_native(); prior_marker=true; activate_native();
    assert(!acceleration && !claims && !model.count && !marker_writes && !marker_clears && blocked);
    setup_native(); marker_short=true; activate_native(); assert(!acceleration && !claims && !marker_clears);
    setup_native(); commit_error=-EIO; activate_native(); assert(!acceleration && !claims && !marker_clears);
    setup_native(); close_error=-EIO; activate_native(); assert(!acceleration && !claims && !marker_clears);
    setup_native(); marker_error=-EIO; activate_native(); assert(!acceleration && !claims && !model.count);
    setup_native(); claim_failed=true; activate_native(); assert(!acceleration && !model.count && marker_owned);
    setup_native(); mapping_failed=true; activate_native(); assert(!acceleration && !model.count && marker_owned);
    setup_native(); model.reg[CONFIG_CHIP_ID/4]=0x5245; activate_native(); assert(!acceleration && !model.count);
    setup_native(); model.reg[CRTC_OFF_PITCH/4]|=1; activate_native(); assert(!acceleration && !model.count && fbdev_cpu_ready());
    setup_native(); model.reg[MEM_CNTL/4]=0; device.size=1024*1024; reservation.end=device.phys+device.size;
    activate_native(); assert(!acceleration && !model.count); /* mapped size exceeds real VRAM */
    setup_native(); model.hang_launch=true; activate_native();
    assert(acceleration==&engine.ops && engine.quarantined && !fbdev_cpu_ready());
    assert(!fbdev_console_begin() && !marker_clears);
    model.reg[GUI_STAT/4]=0;
    /* Still quarantined, but a new bounded proof can permit software access. */
    inside_probe=false;
    assert(!fallback() && fbdev_cpu_ready() && !eligible(&device));
}
static void diagnostics(void)
{
    setup_native(); g_boot.flags=CBI_F_HW_CHECK;
    uint32_t tested=0;
    assert(!fbdev_accel_diagnostic(&volume,1000,NULL,NULL,&tested));
    assert(tested==(3u|DISPLAY_CHECK_CURSOR) && !acceleration && engine.cpu_safe);
    assert(!memcmp(lfb,original,sizeof(lfb)) && !marker_owned);
    setup_native(); g_boot.flags=CBI_F_HW_CHECK; model.corrupt=true;
    assert(fbdev_accel_diagnostic(&volume,1000,NULL,NULL,&tested)==-EOPNOTSUPP);
    assert(engine.quarantined && !acceleration && !memcmp(lfb,original,sizeof(lfb)));
}
int main(void)
{
    selection(); lifecycle(); diagnostics();
    assert(!failures);
    puts("mach64 adapter: actual registry/fbdev activation, PCI/lease refusal, durable markers, native/punt counters and display identity, CPU exclusion, DM9 draw/cursor evidence and restoration PASS");
    return 0;
}
