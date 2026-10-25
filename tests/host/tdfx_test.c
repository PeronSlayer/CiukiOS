/* Production f2-117 engine/lifecycle, independent fake command processor.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/mtrr.h>
#include <ciuki/swdib.h>
#include "../../src/kernel/drivers/video/tdfx/engine.h"
#include "../../src/kernel/drivers/video/tdfx/registers.h"
#include "../../src/kernel/fs/vfs.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); exit(1); } } while (0)
struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
const struct display_driver s3savage_driver={0};
static struct {
    uint32_t regs[TDFX_MMIO_BYTES/4];
    struct { uint32_t reg,value; } writes[4096];
    unsigned count,reads,drains,native_draws,nops,pauses,delays;
    uint32_t status,config[64];
    uint64_t now;
    bool frozen,wedged,after_launch_busy,corrupt,drop_write;
    bool reject_delay,fail_hide,fail_show,fail_palette,fail_position;
    unsigned alternating;
    int pause_error;
} fake;
static uint8_t pixels[32*32*4+256],reference[sizeof(pixels)],saved[sizeof(pixels)];
static uint8_t cursor_page[PAGE_SIZE],cursor_saved[PAGE_SIZE];
static struct fb_device fb;
static struct pci_func pci[3];
static unsigned functions=1,config_reads,claims,maps,revokes,exclusive,logs;
static bool fail_claim,fail_map,fail_activate;
static struct resource resources[4];
static struct fat_volume volume;
static struct vfs vfs;
static size_t marker_durable,marker_pending;
static bool marker_stat_error,marker_commit_error,marker_close_error,marker_short;
static unsigned marker_writes,marker_commits,marker_closes;
static const struct accel_ops *live_ops;

static void check_path(const char *p)
{ CHECK(!strcmp(p,pci[0].device==5 ? "C:/system/settings/tdfx0005.cfg" : "C:/system/settings/tdfx0009.cfg")); }
int vfs_table_init(struct vfs *v,struct vfs_table *t,unsigned d)
{ CHECK(!exclusive && v==&vfs && d==2); memset(t,0,sizeof(*t)); marker_pending=marker_durable; return 0; }
void vfs_table_destroy(struct vfs_table *t) { (void)t; CHECK(!exclusive); }
int vfs_stat(struct vfs_table *t,const char *path,struct fat_entry *entry)
{ (void)t; CHECK(!exclusive); check_path(path); entry->size=marker_durable; return marker_stat_error ? -EIO : 0; }
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share share,uint8_t attr)
{ (void)t; CHECK(!exclusive && flags==(VFS_WRITE|VFS_CREATE) && share==VFS_DENY_ALL && !attr); check_path(path); return 7; }
int vfs_write(struct vfs_table *t,int fd,const void *data,size_t size,size_t *done)
{
    (void)t; CHECK(!exclusive && fd==7);
    const char *want=pci[0].device==5 ? "driver=tdfx pci=121a:0005 attempt\n" : "driver=tdfx pci=121a:0009 attempt\n";
    CHECK(size==strlen(want) && !memcmp(want,data,size)); marker_writes++;
    *done=marker_short ? size-1 : size; marker_pending=*done; return 0;
}
int vfs_truncate(struct vfs_table *t,int fd,uint64_t size)
{ (void)t; CHECK(!exclusive && fd==7 && !size); marker_pending=0; return 0; }
int vfs_commit(struct vfs_table *t,int fd)
{ (void)t; CHECK(!exclusive && fd==7); marker_commits++; if (marker_commit_error) return -EIO; marker_durable=marker_pending; return 0; }
int vfs_close(struct vfs_table *t,int fd)
{ (void)t; CHECK(!exclusive && fd==7); marker_closes++; return marker_close_error ? -EIO : 0; }

unsigned pci_count(void) { return functions; }
const struct pci_func *pci_get(unsigned i) { return i<functions ? &pci[i] : 0; }
const struct resource *registry_get(unsigned i) { return i<4 ? &resources[i] : 0; }
bool registry_valid(int h,gen_t g)
{ return h>=0 && h<4 && resources[h].generation==g && resources[h].state==RS_ACTIVE; }
int registry_claim(enum res_type t,uint32_t s,uint32_t end,const char *owner,bool share)
{
    CHECK(t==RES_MMIO && !share && s<end); claims++;
    if (fail_claim) return -EBUSY;
    for (unsigned i=0;i<4;i++) if (resources[i].owner && resources[i].state!=RS_RELEASED &&
        resources[i].start<end && resources[i].end>s) return -EBUSY;
    for (unsigned i=1;i<4;i++) if (!resources[i].owner || resources[i].state==RS_RELEASED) {
        resources[i]=(struct resource){.type=t,.start=s,.end=end,.owner=owner,.state=RS_CLAIMED,.generation=42+i};
        return i;
    }
    return -ENOSPC;
}
int registry_activate(int h,gen_t g)
{ CHECK(h>0 && h<4 && resources[h].generation==g); if (fail_activate) return -EIO; resources[h].state=RS_ACTIVE; return 0; }
int registry_quiesce(int h,gen_t g,registry_idle_proof_fn proof)
{
    CHECK(h>0 && h<4 && resources[h].generation==g);
    resources[h].state=RS_QUIESCING;
    if (proof(h,g)) return 0;
    resources[h].state=RS_QUARANTINED; return -EIO;
}
int registry_release(int h,gen_t g)
{ CHECK(h>0 && h<4 && resources[h].generation==g && resources[h].state==RS_QUIESCING); resources[h].state=RS_RELEASED; return 0; }
void *vmm_reserve_mmio(uint32_t n)
{ CHECK(n==TDFX_MMIO_BYTES || n==PAGE_SIZE); return n==PAGE_SIZE ? (void *)cursor_page : (void *)fake.regs; }
void *vmm_replace_mmio(void *p,uint32_t cap,uint32_t phys,uint32_t bytes)
{
    maps++; CHECK(cap==bytes && (cap==PAGE_SIZE || cap==TDFX_MMIO_BYTES));
    if (cap==TDFX_MMIO_BYTES) CHECK(phys==(pci[0].bar[0]&~15u));
    else CHECK(phys==fb.phys+g_boot.display.vram_bytes-PAGE_SIZE);
    return fail_map ? 0 : p;
}
void vmm_revoke_mmio(void *p,uint32_t bytes)
{ CHECK(p && (bytes==TDFX_MMIO_BYTES || bytes==PAGE_SIZE)); revokes++; }
const struct fb_device *fbdev_get(void) { return &fb; }
bool fbdev_cpu_ready(void) { return !live_ops || live_ops->cpu_ready(live_ops->ctx); }
unsigned fbdev_native_capabilities(void) { return live_ops ? live_ops->capabilities(live_ops->ctx) : 0; }
int fbdev_accel_probe(const struct fb_device *f,int (*probe)(void *),void *ctx)
{
    CHECK(!exclusive && f==&fb && marker_durable && marker_closes);
    exclusive++; int err=probe(ctx); exclusive--; return err;
}
void framebuffer_write_drain(void) { fake.drains++; }
void klog(const char *fmt,...) { (void)fmt; CHECK(!exclusive); logs++; }
uint32_t tdfx_host_config(const struct pci_func *p,unsigned off)
{ CHECK(p==&pci[0] && off%4==0 && off<256); config_reads++; return fake.config[off/4]; }
uint32_t tdfx_host_read(void *ctx,uint32_t reg)
{
    (void)ctx; CHECK(reg<TDFX_MMIO_BYTES && reg%4==0); fake.reads++;
    if (reg==STATUS) {
        if (fake.alternating) { fake.alternating--; return fake.status|((fake.alternating%3==0) ? STATUS_BUSY : 0); }
        return fake.status|(fake.wedged ? STATUS_BUSY : 0);
    }
    return fake.regs[reg/4];
}
/* Independent pixel processor: follows command direction in-place, so a
 * wrong overlap order corrupts the golden. Does not call swdib. */
static void launch(uint32_t cmd)
{
    CHECK(cmd&CMD_2D_GO); CHECK((cmd>>24)==0xcc);
    CHECK(fake.regs[DST_FORMAT/4]==(fb.pitch|(fb.bpp==16 ? 0x30000u : 0x50000u)));
    CHECK(fake.regs[DST_BASE_ADDR/4]==0 && fake.regs[SRC_BASE_ADDR/4]==0);
    unsigned size=fake.regs[DST_SIZE/4],w=size&0xffffu,h=size>>16;
    int dx=fake.regs[DST_XY/4]&0xffffu,dy=fake.regs[DST_XY/4]>>16;
    int sx=fake.regs[SRC_XY/4]&0xffffu,sy=fake.regs[SRC_XY/4]>>16;
    int xs=cmd&X_RIGHT_TO_LEFT ? -1 : 1,ys=cmd&Y_BOTTOM_TO_TOP ? -1 : 1;
    unsigned bytes=fb.bpp/8;
    for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
        int tx=dx+(int)x*xs,ty=dy+(int)y*ys;
        CHECK(tx>=0 && ty>=0 && tx<(int)fb.width && ty<(int)fb.height);
        if (cmd&STRETCH_CLIP1) {
            unsigned lo=fake.regs[CLIP1_MIN/4],hi=fake.regs[CLIP1_MAX/4];
            if (tx<(int)(lo&65535) || ty<(int)(lo>>16) || tx>=(int)(hi&65535) || ty>=(int)(hi>>16)) continue;
        }
        uint8_t *to=pixels+ty*fb.pitch+tx*bytes;
        if (fake.drop_write) continue;
        if ((cmd&15)==5) {
            uint32_t colour=fake.regs[COLOR_FORE/4]; memcpy(to,&colour,bytes);
        } else {
            CHECK((cmd&15)==1 || (cmd&15)==2);
            unsigned source=fake.regs[SRC_SIZE/4];
            int fromx=sx+(int)x*xs,fromy=sy+(int)y*ys;
            if ((cmd&15)==2) { fromx=sx+x*(source&65535)/w; fromy=sy+y*(source>>16)/h; }
            CHECK(fromx>=0 && fromy>=0 && fromx<(int)fb.width && fromy<(int)fb.height);
            uint8_t p[4]; memcpy(p,pixels+fromy*fb.pitch+fromx*bytes,bytes); memcpy(to,p,bytes);
        }
    }
    if (fake.corrupt) pixels[0]^=1;
    fake.native_draws++;
    if (fake.after_launch_busy) fake.wedged=true;
}
void tdfx_host_write(void *ctx,uint32_t reg,uint32_t value)
{
    (void)ctx; CHECK(reg<TDFX_MMIO_BYTES && reg%4==0);
    CHECK((fake.status&31u)>0 && fake.status!=UINT32_MAX); /* never submit to full FIFO */
    CHECK(fake.count<ARRAY_SIZE(fake.writes));
    fake.writes[fake.count].reg=reg; fake.writes[fake.count++].value=value;
    if ((fake.fail_hide && reg==VIDEO_PROC_CONFIG && !(value&CURSOR_ENABLE)) ||
        (fake.fail_show && reg==VIDEO_PROC_CONFIG && (value&CURSOR_ENABLE)) ||
        (fake.fail_palette && reg==HW_CURSOR_COLOR1) || (fake.fail_position && reg==HW_CURSOR_LOC)) return;
    fake.regs[reg/4]=value;
    if (reg==CMD_2D) launch(value);
    if (reg==CMD_3D) { CHECK(!value); fake.nops++; }
}
uint64_t tdfx_host_now(void *ctx) { (void)ctx; return fake.now; }
int tdfx_host_delay(void *ctx,uint32_t us)
{ (void)ctx; fake.delays++; if (!fake.frozen) fake.now+=us; return fake.reject_delay ? -EIO : 0; }
static void model_drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static const struct tdfx_io io={.read=tdfx_host_read,.write=tdfx_host_write,
    .now=tdfx_host_now,.delay=tdfx_host_delay,.drain=model_drain};
static int pause_wait(void *ctx) { (void)ctx; fake.pauses++; return fake.pause_error; }

static void setup(unsigned bits)
{
    memset(&fake,0,sizeof(fake)); fake.status=31;
    memset(pixels,0x7b,sizeof(pixels)); memset(cursor_page,0x96,sizeof(cursor_page));
    fb=(struct fb_device){.width=32,.height=32,.pitch=32*(bits/8)+8,
        .phys=0xe0000000u,.mapped=pixels,.present=true,.bpp=bits,.handle=0,.generation=42,
        .red_size=bits==16 ? 5 : 8,.red_pos=bits==16 ? 11 : 16,
        .green_size=bits==16 ? 6 : 8,.green_pos=bits==16 ? 5 : 8,.blue_size=bits==16 ? 5 : 8};
    fb.size=fb.height*fb.pitch;
    CHECK(fb.size<=sizeof(pixels));
    memset(&g_boot,0,sizeof(g_boot)); g_boot.display.vram_bytes=16u*1024u*1024u;
    resources[0]=(struct resource){.type=RES_MMIO,.start=fb.phys,.end=fb.phys+fb.size,
        .owner="fbdev",.generation=42,.state=RS_ACTIVE};
    pci[0]=(struct pci_func){.dev=1,.vendor=0x121a,.device=5,.class_code=3,.command=2,
        .bar={0xd0000000u,fb.phys|8u}};
    fake.regs[VIDEO_PROC_CONFIG/4]=1u|128u|(1u<<10)|((bits/8u-1)<<18);
    fake.regs[VIDEO_DESKTOP_OVERLAY_STRIDE/4]=fb.pitch;
    fake.regs[VIDEO_SCREEN_SIZE/4]=fb.width|(fb.height<<12);
    fake.regs[DRAM_INIT0/4]=(1u<<26)|(1u<<27); /* eight 2 MiB devices */
    volume.mounted=true; vfs.volumes[2]=&volume;
}
static void trace_reset(void) { fake.count=0; }
static void wrote(unsigned n,uint32_t reg,uint32_t value)
{ CHECK(n<fake.count && fake.writes[n].reg==reg && fake.writes[n].value==value); }
static struct tdfx_engine e;
static void start_core(bool cursor)
{ CHECK(!tdfx_engine_start(&e,&io,&fb,cursor ? cursor_page : 0,g_boot.display.vram_bytes-PAGE_SIZE)); CHECK(e.cpu_safe); trace_reset(); }
static void stretch_test(unsigned bits)
{
    setup(bits); start_core(false);
    CHECK(e.ops.stretch_rect && (e.ops.command_features(&e)&CIUKI_CAP_STRETCH) &&
          (e.ops.command_bits(&e)&(1u<<CIUKI_CMD_SCALED)));
    struct swdib_target target={.device=fb,.identity=pixels,.kind=CIUKI_TARGET_SCANOUT,.rights=3};
    struct swdib_target oracle=target; oracle.device.mapped=reference; oracle.identity=reference;
    for (unsigned i=0;i<fb.size;i++) pixels[i]=(i*17u)&255;
    memcpy(reference,pixels,sizeof(pixels));
    struct ciuki_batch_clip clips[]={{3,4,8,4},{8,6,12,12}};
    struct ciuki_batch_record r={.opcode=CIUKI_CMD_SCALED,.record_bytes=128,.dst=1,.src=1,
        .sx=2,.sy=1,.sw=20,.sh=1,.dx=2,.dy=4,.dw=20,.dh=15,.filter=CIUKI_FILTER_NEAREST_EDGE,
        .clip_offset=8,.clip_count=2};
    struct swdib_command c={.record=&r,.src=&target,.dst=&target,.clips=clips};
    struct swdib_command ref=c; ref.src=ref.dst=&oracle;
    CHECK(!swdib_validate(&ref)); swdib_execute(&ref,0,0);
    CHECK(!e.ops.stretch_rect(&e,&c)); CHECK(!memcmp(pixels,reference,sizeof(pixels)));
    /* Refusal precedes every command/register write and all cursor changes. */
    unsigned count=fake.count;
    r.filter=CIUKI_FILTER_BILINEAR; CHECK(e.ops.stretch_rect(&e,&c)==-EOPNOTSUPP && fake.count==count);
    r.filter=CIUKI_FILTER_NEAREST_EDGE; r.sw=16;
    CHECK(e.ops.stretch_rect(&e,&c)==-EOPNOTSUPP && fake.count==count);
    r.sw=20; target.kind=CIUKI_TARGET_SYSTEM;
    CHECK(e.ops.stretch_rect(&e,&c)==-EOPNOTSUPP && fake.count==count);
    target.kind=CIUKI_TARGET_SCANOUT; r.sy=r.dy;
    CHECK(e.ops.stretch_rect(&e,&c)==-EOPNOTSUPP && fake.count==count);
    r.sy=1; fake.after_launch_busy=true;
    CHECK(e.ops.stretch_rect(&e,&c)==-ETIMEDOUT && !e.ops.command_bits(&e));
    CHECK(!e.ops.command_features(&e));
}
static void golden(unsigned bits)
{
    setup(bits); start_core(false);
    struct fb_device ref=fb; ref.mapped=reference;
    memcpy(reference,pixels,sizeof(pixels));
    for (unsigned i=0;i<16;i++) {
        struct fb_rect r={(int)i,(int)i,2,2}; uint32_t colour=swdib_pack(&fb,i*0x123415u);
        CHECK(!e.ops.fill_rect(&e,&r,colour)); swdib_fill(&ref,&r,colour);
        CHECK(!memcmp(pixels,reference,sizeof(pixels)));
    }
    for (unsigned axis=0;axis<4;axis++) {
        for (unsigned i=0;i<fb.size;i++) pixels[i]=reference[i]=(uint8_t)(i*29u+i/31u);
        struct fb_rect src={(axis&1) ? 5 : 1,(axis&2) ? 5 : 1,20,20};
        int dx=(axis&1) ? 1 : 5,dy=(axis&2) ? 1 : 5;
        CHECK(!e.ops.copy_rect(&e,&src,dx,dy)); swdib_copy(&ref,&src,dx,dy);
        CHECK(!memcmp(pixels,reference,sizeof(pixels)));
    }
    struct fb_rect single={31,31,1,1};
    CHECK(!e.ops.copy_rect(&e,&single,31,31));
    CHECK(e.cpu_safe && !e.quarantined && e.ops.capabilities(&e)==1);
}
static void trace_test(void)
{
    setup(32); start_core(false);
    struct fb_rect r={1,2,4,3}; CHECK(!e.ops.fill_rect(&e,&r,0x123456));
    CHECK(fake.count==7); wrote(0,DST_FORMAT,fb.pitch|0x50000);
    wrote(1,COLOR_BACK,0x123456); wrote(2,COLOR_FORE,0x123456);
    wrote(3,DST_SIZE,4|(3<<16)); wrote(4,DST_XY,1|(2<<16));
    wrote(5,CMD_2D,0xcc000105); wrote(6,CMD_3D,0);
    trace_reset(); CHECK(!e.ops.copy_rect(&e,&r,3,4)); CHECK(fake.count==7);
    wrote(2,SRC_XY,4|(4<<16)); wrote(3,DST_SIZE,4|(3<<16));
    wrote(4,DST_XY,6|(6<<16)); wrote(5,CMD_2D,0xcc00c101);
    trace_reset(); r.x=3; r.y=4; CHECK(!e.ops.copy_rect(&e,&r,1,2));
    wrote(2,SRC_XY,3|(4<<16)); wrote(4,DST_XY,1|(2<<16)); wrote(5,CMD_2D,0xcc000101);
    trace_reset(); CHECK(!e.ops.sync(&e)); CHECK(fake.count==1); wrote(0,CMD_3D,0);
}
static void refusal(void)
{
    setup(32); start_core(false); memcpy(saved,pixels,sizeof(saved));
    struct fb_rect bad[]={{-1,0,2,2},{0,-1,2,2},{31,0,2,2},{0,31,2,2},
        {INT32_MAX,0,INT32_MAX,1},{0,0,-1,1},{0,0,0,1}};
    for (unsigned i=0;i<ARRAY_SIZE(bad);i++) {
        CHECK(e.ops.fill_rect(&e,&bad[i],1)==-EOPNOTSUPP);
        CHECK(e.ops.copy_rect(&e,&bad[i],0,0)==-EOPNOTSUPP);
    }
    struct fb_rect r={0,0,2,2};
    CHECK(e.ops.copy_rect(&e,&r,INT32_MAX,0)==-EOPNOTSUPP);
    CHECK(e.ops.cursor_show(&e,true)==-EOPNOTSUPP);
    CHECK(!e.ops.pattern_rect && !e.ops.write_rect && !e.ops.offscreen);
    CHECK(!fake.count && !memcmp(saved,pixels,sizeof(saved)) && e.cpu_safe);
    struct fb_device f=fb; f.bpp=24; CHECK(!tdfx_layout(&f));
    f=fb; f.red_pos=0; CHECK(!tdfx_layout(&f));
    f=fb; f.pitch--; CHECK(!tdfx_layout(&f)); f=fb; f.width=4096; CHECK(!tdfx_layout(&f));
    f=fb; f.size--; CHECK(!tdfx_layout(&f));
    /* Read-only start refusals: exact pitch/format/base/scanout, cursor
     * region, endian and X11 cursor mode. None may submit a command. */
    uint32_t offsets[]={VIDEO_PROC_CONFIG,VIDEO_DESKTOP_START_ADDR,VIDEO_DESKTOP_OVERLAY_STRIDE,
        VIDEO_SCREEN_SIZE,MISC_INIT0,PCI_INIT0};
    for (unsigned i=0;i<ARRAY_SIZE(offsets);i++) {
        struct tdfx_engine fresh={0}; uint32_t old=fake.regs[offsets[i]/4];
        fake.regs[offsets[i]/4]=i==4 ? 1u<<30 : old^1;
        CHECK(tdfx_engine_start(&fresh,&io,&fb,0,0)==-EOPNOTSUPP && !fake.count);
        fake.regs[offsets[i]/4]=old;
    }
    struct tdfx_engine fresh={0};
    CHECK(tdfx_engine_start(&fresh,&io,&fb,cursor_page,fb.size-1)==-EOPNOTSUPP && !fake.count);
}
static void timeout_test(void)
{
    setup(32); start_core(false); struct fb_rect r={0,0,3,3};
    fake.status=0; fake.frozen=true; unsigned d=fake.delays;
    CHECK(e.ops.fill_rect(&e,&r,1)==-ETIMEDOUT);
    CHECK(fake.delays-d==TDFX_POLL_LIMIT && !fake.count && e.quarantined && !e.cpu_safe);
    fake.status=31; CHECK(!e.ops.sync(&e) && e.cpu_safe && e.quarantined && !e.ops.capabilities(&e));
    trace_reset(); CHECK(e.ops.fill_rect(&e,&r,1)==-EIO && !fake.count);
    fake.wedged=true; fake.frozen=false;
    CHECK(e.ops.sync(&e)==-ETIMEDOUT && !e.cpu_safe);
    fake.wedged=false; CHECK(!e.ops.sync(&e) && e.cpu_safe);
    fake.status=UINT32_MAX; CHECK(e.ops.sync(&e)==-EIO && !e.cpu_safe);
    fake.status=31; CHECK(!e.ops.sync(&e));
    fake.reject_delay=true; CHECK(e.ops.sync(&e)==-EIO && !e.cpu_safe);
}
static void quarantine_test(void)
{
    setup(32); start_core(true); struct fb_rect r={0,0,4,4}; fake.after_launch_busy=true;
    CHECK(e.ops.copy_rect(&e,&r,1,1)==-ETIMEDOUT);
    CHECK(fake.native_draws==1 && !e.cpu_safe && e.quarantined);
    unsigned draws=fake.native_draws; CHECK(e.ops.copy_rect(&e,&r,1,1)==-EIO && fake.native_draws==draws);
    CHECK(e.ops.stop(&e)==-ETIMEDOUT && !e.cpu_safe);
    fake.wedged=false; CHECK(!e.ops.sync(&e) && e.cpu_safe && e.quarantined);
    CHECK(tdfx_engine_start(&e,&io,&fb,0,0)==-EIO); /* no same-boot reactivation */
}
static void cursor_test(unsigned bits)
{
    setup(bits); start_core(true); memcpy(saved,pixels,sizeof(saved));
    CHECK(e.ops.cursor_show(&e,true)==-EINVAL);
    struct accel_cursor c={.foreground=swdib_pack(&fb,0xff0000),.background=swdib_pack(&fb,0x00ff00)};
    c.mask[0]=0xa0000001u; c.image[0]=0xc0000001u; c.mask[31]=0xffffffffu; c.image[31]=0x81234567u;
    fake.regs[VIDEO_PROC_CONFIG/4]|=CURSOR_ENABLE;
    CHECK(!e.ops.cursor_set(&e,&c) && e.hidden && !(fake.regs[VIDEO_PROC_CONFIG/4]&CURSOR_ENABLE));
    CHECK(cursor_page[0]==0x5f && cursor_page[3]==0xfe && cursor_page[8]==0x80 && cursor_page[11]==1);
    CHECK(cursor_page[31*16]==0 && cursor_page[31*16+8]==0x81 && cursor_page[31*16+11]==0x67);
    for (unsigned row=0;row<64;row++) for (unsigned col=0;col<8;col++) {
        if (row>=32 || col>=4) CHECK(cursor_page[row*16+col]==0xff && cursor_page[row*16+8+col]==0);
    }
    for (unsigned i=TDFX_CURSOR_BYTES;i<PAGE_SIZE;i++) CHECK(cursor_page[i]==0x96);
    CHECK(fake.regs[HW_CURSOR_COLOR0/4]==0x00ff00 && fake.regs[HW_CURSOR_COLOR1/4]==0xff0000);
    c.foreground=swdib_pack(&fb,0x123456);
    CHECK(!e.ops.cursor_set(&e,&c));
    CHECK(fake.regs[HW_CURSOR_COLOR1/4]==(bits==16 ? 0x103552u : 0x123456u));
    CHECK(!e.ops.cursor_move(&e,-1,-1)); CHECK(fake.regs[HW_CURSOR_LOC/4]==(62u|(62u<<16)));
    CHECK(!e.ops.cursor_show(&e,true) && !e.hidden);
    CHECK(!e.ops.cursor_move(&e,INT32_MIN,INT32_MAX) && e.hidden);
    CHECK(!e.ops.cursor_move(&e,0,0) && !e.hidden);
    CHECK(!e.ops.cursor_set(&e,&c) && e.hidden);
    CHECK(!memcmp(saved,pixels,sizeof(saved)));
    CHECK(!tdfx_cursor_check(&e.ops,1,0,0) && e.hidden);
    CHECK(tdfx_cursor_check(&e.ops,0,0,0)==-ETIMEDOUT && e.hidden);
    CHECK(!e.ops.cursor_move(&e,0,0) && !e.ops.cursor_show(&e,true));
    tdfx_engine_quarantine(&e,"test"); CHECK(e.hidden && !e.cpu_safe && !e.ops.capabilities(&e));
    CHECK(!e.ops.sync(&e) && e.cpu_safe);
}
static void cursor_faults(void)
{
    setup(32); start_core(true); struct accel_cursor c={.foreground=0xffffff};
    fake.fail_palette=true; CHECK(e.ops.cursor_set(&e,&c)==-EIO && !e.cpu_safe && e.quarantined);
    fake.fail_palette=false; CHECK(!e.ops.sync(&e));
    /* A fresh local engine is a distinct adapter instance for these faults. */
    memset(&e,0,sizeof(e)); start_core(true); CHECK(!e.ops.cursor_set(&e,&c));
    fake.fail_position=true; CHECK(e.ops.cursor_move(&e,1,1)==-EIO && !e.cpu_safe);
    fake.fail_position=false; CHECK(!e.ops.sync(&e));
    memset(&e,0,sizeof(e)); start_core(true); CHECK(!e.ops.cursor_set(&e,&c));
    CHECK(!e.ops.cursor_move(&e,1,1) && !e.ops.cursor_show(&e,true));
    fake.fail_hide=true; tdfx_engine_quarantine(&e,"test");
    CHECK(!e.cpu_safe && e.ops.sync(&e)==-EIO);
    fake.fail_hide=false; CHECK(!e.ops.sync(&e) && e.cpu_safe && e.hidden);
    memset(&e,0,sizeof(e)); start_core(true); CHECK(!e.ops.cursor_set(&e,&c));
    CHECK(!e.ops.cursor_move(&e,1,1)); fake.fail_show=true;
    CHECK(e.ops.cursor_show(&e,true)==-EIO && !e.cpu_safe && e.quarantined);
}
static void slices(void)
{
    setup(32); start_core(false); e.ops.set_pause(&e,pause_wait,0);
    fake.wedged=true; CHECK(e.ops.sync(&e)==-ETIMEDOUT && fake.pauses>0);
    fake.wedged=false; CHECK(!e.ops.sync(&e));
    fake.pause_error=-EINTR; fake.wedged=true;
    CHECK(e.ops.sync(&e)==-EINTR && !e.cpu_safe);
    fake.wedged=false; fake.pause_error=0; CHECK(!e.ops.sync(&e));
    fake.alternating=12; unsigned before=fake.reads;
    CHECK(!e.ops.sync(&e) && fake.reads-before>=15); /* three consecutive samples */
}
static void probe_test(void)
{
    setup(32); const struct pci_func *owner;
    const struct display_driver *table[]={&tdfx_driver};
    CHECK(display_driver_select(&fb,table,1,&owner)==&tdfx_driver && owner==&pci[0]);
    CHECK(!fake.count && !claims && !maps);
    pci[0].device=1; CHECK(!display_driver_bind(&fb)); pci[0].device=2; CHECK(!display_driver_bind(&fb));
    pci[0].device=3; CHECK(!display_driver_bind(&fb)); pci[0].device=5;
    fb.phys+=PAGE_SIZE; CHECK(!display_driver_bind(&fb)); fb.phys-=PAGE_SIZE;
    resources[0].generation++; CHECK(!display_driver_bind(&fb)); resources[0].generation--;
    resources[0].owner="other"; CHECK(!display_driver_bind(&fb)); resources[0].owner="fbdev";
    pci[0].command=0; CHECK(!display_driver_bind(&fb)); pci[0].command=2;
    pci[0].bar[0]|=4; CHECK(!display_driver_bind(&fb)); pci[0].bar[0]&=~4u;
    g_boot.e820_count=1; g_boot.e820[0]=(struct ciuki_e820){.base=pci[0].bar[0],.length=TDFX_MMIO_BYTES,.type=CBI_E820_RAM};
    CHECK(!display_driver_bind(&fb)); g_boot.e820_count=0;
    functions=2; pci[1]=pci[0]; CHECK(!display_driver_bind(&fb)); functions=1;
    pci[0].device=9;
    for (unsigned off=0;off<4;off++) {
        unsigned offsets[]={0x40,0x80,0x8c,0x94}; uint32_t bits[]={1u<<28,1u,1u<<24,1u<<26};
        fake.config[offsets[off]/4]=bits[off]; CHECK(!display_driver_bind(&fb)); fake.config[offsets[off]/4]=0;
    }
    CHECK(display_driver_bind(&fb)==&tdfx_driver && config_reads);
    pci[0].header_type=0x80; CHECK(!display_driver_bind(&fb)); pci[0].header_type=0;
    pci[1]=pci[0]; pci[1].class_code=0xff; functions=2; CHECK(!display_driver_bind(&fb)); functions=1;
    CHECK(!fake.count && !claims && !maps);
}
static void lifecycle(const char *name)
{
    setup(32); memcpy(saved,pixels,sizeof(saved)); memcpy(cursor_saved,cursor_page,sizeof(cursor_saved));
    if (!strcmp(name,"start9") || !strcmp(name,"bad_sli")) pci[0].device=9;
    CHECK(display_driver_bind(&fb)==&tdfx_driver);
    if (!strcmp(name,"safe_mode") || !strcmp(name,"accel_off")) {
        if (!strcmp(name,"safe_mode")) g_boot.flags|=CBI_F_SAFE_MODE; else memcpy(g_boot.options,"accel=0",8);
        CHECK(!tdfx_driver.eligible(&fb)); CHECK(tdfx_driver.attempt_begin(&vfs)<0);
        CHECK(!tdfx_driver.start(&fb,0)); CHECK(!claims && !maps && !fake.count && !marker_writes); return;
    }
    if (!strcmp(name,"retained")) marker_durable=33;
    if (!strcmp(name,"marker_read")) marker_stat_error=true;
    if (!strcmp(name,"marker_commit")) marker_commit_error=true;
    if (!strcmp(name,"marker_short")) marker_short=true;
    if (!strcmp(name,"marker_close")) marker_close_error=true;
    if (marker_durable || marker_stat_error || marker_commit_error || marker_short || marker_close_error) {
        CHECK(tdfx_driver.attempt_begin(&vfs)<0); CHECK(!tdfx_driver.start(&fb,0));
        CHECK(!fake.count && !claims && !maps);
        CHECK(!tdfx_driver.eligible(&fb)); return;
    }
    CHECK(tdfx_driver.eligible(&fb)); CHECK(!tdfx_driver.start(&fb,0)); /* no marker */
    CHECK(!tdfx_driver.attempt_begin(&vfs) && marker_durable && marker_commits==1 && !fake.count);
    if (!strcmp(name,"cursorless")) { fb.size=g_boot.display.vram_bytes; resources[0].end=fb.phys+fb.size; }
    if (!strcmp(name,"bad_vram")) fake.regs[DRAM_INIT0/4]=0;
    if (!strcmp(name,"bad_sli")) fake.regs[0x20020c/4]=1u<<26;
    if (!strcmp(name,"bad_layout")) fake.regs[VIDEO_DESKTOP_START_ADDR/4]=1024;
    if (!strcmp(name,"bad_pixels")) fake.drop_write=true;
    if (!strcmp(name,"unsafe_start")) fake.after_launch_busy=true;
    fail_claim=!strcmp(name,"claim_fail"); fail_map=!strcmp(name,"map_fail"); fail_activate=!strcmp(name,"activate_fail");
    live_ops=tdfx_driver.start(&fb,0);
    bool refuse=!strncmp(name,"bad_",4) || !strcmp(name,"unsafe_start") || fail_claim || fail_map || fail_activate;
    if (refuse) {
        CHECK(!live_ops);
        const struct accel_ops *guard=tdfx_driver.fallback();
        if (!strcmp(name,"unsafe_start")) {
            CHECK(guard && !guard->cpu_ready(guard->ctx) && !revokes);
            CHECK(!tdfx_driver.eligible(&fb)); fake.wedged=false;
            CHECK(!guard->sync(guard->ctx) && guard->cpu_ready(guard->ctx));
            CHECK(!tdfx_driver.fallback());
        } else { CHECK(!guard); CHECK(!memcmp(saved,pixels,sizeof(saved))); }
        CHECK(marker_durable); return;
    }
    CHECK(live_ops && !memcmp(saved,pixels,sizeof(saved)) && fake.native_draws==5);
    CHECK(live_ops->capabilities(live_ops->ctx)==(!strcmp(name,"cursorless") ? 1u : 3u));
    CHECK(!memcmp(cursor_saved,cursor_page,sizeof(cursor_saved))); /* setup didn't overwrite unknown cursor */
    tdfx_driver.record(); tdfx_driver.record(); CHECK(logs==1);
    struct ciuki_display_engine_info info;
    display_driver_refresh(); display_driver_info(&info);
    CHECK(info.ready && !info.native_used && !strcmp(info.driver_id,"tdfx") && !strcmp(info.display_name,tdfx_driver.name));
    CHECK(info.pci_vendor==0x121a && info.pci_device==pci[0].device);
    if (!strcmp(name,"marker_clear")) {
        marker_commit_error=true; CHECK(tdfx_driver.attempt_end(&vfs)<0 && marker_durable);
        CHECK(!live_ops->stop(live_ops->ctx));
        tdfx_driver.quarantine("marker_clear_failed"); CHECK(!tdfx_driver.eligible(&fb));
        CHECK(live_ops->cpu_ready(live_ops->ctx) && !tdfx_driver.fallback());
    } else CHECK(!tdfx_driver.attempt_end(&vfs) && !marker_durable);
    if (!strcmp(name,"diagnostic")) {
        exclusive++; CHECK(!tdfx_driver.cursor_test(live_ops,10,0,0)); exclusive--;
        CHECK(!(fake.regs[VIDEO_PROC_CONFIG/4]&CURSOR_ENABLE));
    }
    if (!strcmp(name,"stop_fault")) {
        fake.wedged=true; CHECK(live_ops->stop(live_ops->ctx)==-ETIMEDOUT && !revokes);
        CHECK(!live_ops->cpu_ready(live_ops->ctx) && !tdfx_driver.eligible(&fb));
        fake.wedged=false;
    }
    CHECK(!live_ops->stop(live_ops->ctx) && revokes==(!strcmp(name,"cursorless") ? 1u : 2u));
    CHECK(live_ops->cpu_ready(live_ops->ctx) && !live_ops->capabilities(live_ops->ctx));
    CHECK(!(fake.regs[VIDEO_PROC_CONFIG/4]&CURSOR_ENABLE));
    display_driver_info(&info); CHECK(!info.ready && info.kind==CIUKI_ENGINE_SOFTWARE && info.cpu_ready);
}
int main(int argc,char **argv)
{
    CHECK(argc==2); const char *s=argv[1];
    if (!strcmp(s,"goldens16")) golden(16);
    else if (!strcmp(s,"stretch16")) stretch_test(16);
    else if (!strcmp(s,"stretch32")) stretch_test(32);
    else if (!strcmp(s,"goldens32")) golden(32);
    else if (!strcmp(s,"trace")) trace_test();
    else if (!strcmp(s,"refusal")) refusal();
    else if (!strcmp(s,"timeouts")) timeout_test();
    else if (!strcmp(s,"quarantine")) quarantine_test();
    else if (!strcmp(s,"cursor16")) cursor_test(16);
    else if (!strcmp(s,"cursor32")) cursor_test(32);
    else if (!strcmp(s,"cursor_faults")) cursor_faults();
    else if (!strcmp(s,"slices")) slices();
    else if (!strcmp(s,"probe")) probe_test();
    else lifecycle(s);
    printf("tdfx %s: PASS\n",s); return 0;
}
