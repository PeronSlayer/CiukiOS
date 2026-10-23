/* Production MMIO driver, registry and presenter, with the QEMU RV100 subset
 * plus Linux/X.Org R200/R300/R4xx cache, brush and host-data register models.
 * No hardware/guest execution.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/mm.h>
#include <ciuki/registry.h>
#undef _DEFAULT_SOURCE
#define DISPLAY_DRIVER_FIXTURE_REAL
#define main unused_fbdev_regressions
/* Keep the old framebuffer fixture's hardware boundary for its unused tests;
 * the driver and registry below use the larger resource model in this file. */
#define registry_get old_registry_get
#define registry_activate old_registry_activate
#define registry_quarantine old_registry_quarantine
#define registry_valid old_registry_valid
#define registry_quiesce old_registry_quiesce
#define registry_release old_registry_release
#define registry_claim old_registry_claim
#define vmm_map_framebuffer old_map_framebuffer
#define vmm_revoke_mmio old_revoke_mmio
#define vmm_reserve_mmio old_reserve_mmio
#include "fbdev_test.c"
#undef main
#undef registry_get
#undef registry_activate
#undef registry_quarantine
#undef registry_valid
#undef registry_quiesce
#undef registry_release
#undef registry_claim
#undef vmm_map_framebuffer
#undef vmm_revoke_mmio
#undef vmm_reserve_mmio
#include "../../src/kernel/drivers/video/radeon2d/driver.c"
#include "../../src/kernel/drivers/video/registry.c"
const struct display_driver s3savage_driver={0};

static struct {
    uint32_t regs[RADEON_REGSIZE/4];
    uint8_t vram[65536];
    struct { uint32_t reg,value; } trace[512];
    unsigned trace_count, launches, read_count, pauses, delays, cpu_drains;
    uint64_t us;
    bool busy, fifo_full, cache_busy, hang_launch, frozen, no_pixels, bad_cursor, stuck_hide;
    unsigned busy_reads;
    uint32_t required_cache, host[RADEON_UPLOAD_WORDS];
    unsigned host_count, host_width;
    unsigned row_us;
    bool host_pending, no_pattern, no_upload, bad_cache;
} model;
static uint16_t fixture_id=0x5159;
static uint32_t fixture_cache;
static struct resource resources[8];
static unsigned resource_count, claimed, released, revoked, pcis;
static struct pci_func pci;
static bool cursor_conflict, map_failure;
static unsigned marker_size, commits, closes, marker_ios, destroyed;
static int marker_error;
static bool short_write;
static struct vfs test_vfs;
static struct fat_volume test_volume;

uint64_t ktime_cycles(void) { return model.us; }
int udelay(uint32_t us) { model.us+=us; return 0; }
unsigned pci_count(void) { return pcis; }
const struct pci_func *pci_get(unsigned i) { return i<pcis ? &pci : 0; }
const struct resource *registry_get(unsigned i) { return i<resource_count ? &resources[i] : 0; }
bool registry_valid(int h, gen_t gen)
{ return h>=0 && (unsigned)h<resource_count && resources[h].generation==gen && resources[h].state==RS_ACTIVE; }
int registry_claim(enum res_type type, uint32_t first, uint32_t end, const char *owner, bool share)
{
    CHECK(!locks && type==RES_MMIO && !share && first<end);
    if (cursor_conflict && !strcmp(owner,"radeon-cursor")) return -EBUSY;
    CHECK(resource_count<ARRAY_SIZE(resources));
    unsigned n=resource_count++;
    resources[n]=(struct resource){.type=type,.start=first,.end=end,.owner=owner,.generation=n+1,.state=RS_CLAIMED};
    claimed++;
    return (int)n;
}
int registry_activate(int h, gen_t generation)
{
    CHECK(resources[h].generation==generation && resources[h].state==RS_CLAIMED);
    resources[h].state=RS_ACTIVE; return 0;
}
int registry_quarantine(int h, gen_t generation)
{ CHECK(resources[h].generation==generation); resources[h].state=RS_QUARANTINED; return 0; }
int registry_quiesce(int h, gen_t generation, registry_idle_proof_fn proof)
{
    CHECK(registry_valid(h,generation)); resources[h].state=RS_QUIESCING;
    if (!proof(h,generation)) { resources[h].state=RS_QUARANTINED; return -EIO; }
    return 0;
}
int registry_release(int h, gen_t generation)
{
    CHECK(resources[h].state==RS_QUIESCING && resources[h].generation==generation);
    resources[h].state=RS_RELEASED; released++; return 0;
}
void *vmm_map_mmio(uint32_t phys, uint32_t bytes, bool uncached)
{ CHECK(phys==0xe0000000u && bytes==RADEON_REGSIZE && uncached); return map_failure ? 0 : model.regs; }
void *vmm_reserve_mmio(uint32_t bytes)
{
    CHECK(bytes==RADEON_REGSIZE || bytes==RADEON_CURSOR_BYTES);
    if (map_failure) return 0;
    return bytes==RADEON_REGSIZE ? (void *)model.regs : model.vram+sizeof(model.vram)-RADEON_CURSOR_BYTES;
}
void *vmm_replace_mmio(void *window, uint32_t capacity, uint32_t phys, uint32_t bytes)
{
    CHECK(capacity==bytes);
    if (bytes==RADEON_REGSIZE) { CHECK(window==model.regs && phys==0xe0000000u); return window; }
    CHECK(window==model.vram+sizeof(model.vram)-RADEON_CURSOR_BYTES);
    CHECK(phys==device.phys+sizeof(model.vram)-RADEON_CURSOR_BYTES && bytes==RADEON_CURSOR_BYTES);
    return window;
}
void vmm_revoke_mmio(void *ptr, uint32_t bytes)
{ CHECK(ptr && (bytes==RADEON_REGSIZE || bytes==RADEON_CURSOR_BYTES)); revoked++; }
void grants_display_yield(void)
{
    CHECK(!locks && drawing==PRESENT_BUSY && accel_inflight);
    CHECK(!fbdev_console_begin());
    CHECK(fbdev_change_begin()==-EBUSY);
    model.pauses++;
}
void console_framebuffer_revoke(void) { }
void console_framebuffer_changed(const struct fb_device *d) { (void)d; }

int vfs_table_init(struct vfs *v, struct vfs_table *t, unsigned drive)
{ CHECK(!locks && v==&test_vfs && drive==2); t->vfs=v; marker_ios++; return marker_error==1 ? -EIO : 0; }
void vfs_table_destroy(struct vfs_table *t) { CHECK(!locks && t==&marker_table); destroyed++; }
int vfs_stat(struct vfs_table *t, const char *path, struct fat_entry *entry)
{ CHECK(!locks && t==&marker_table && !strcmp(path,ATTEMPT_PATH)); entry->size=marker_size; return marker_error==2 ? -EIO : 0; }
int vfs_open(struct vfs_table *t, const char *path, unsigned flags, enum vfs_share share, uint8_t attr)
{ (void)flags; (void)share; (void)attr; CHECK(!locks && t==&marker_table && !strcmp(path,ATTEMPT_PATH)); return marker_error==3 ? -EIO : 0; }
int vfs_write(struct vfs_table *t, int fd, const void *data, size_t n, size_t *done)
{
    CHECK(!locks && t==&marker_table && fd==0 && n==14 && !memcmp(data,"accel=attempt\n",n));
    *done=short_write ? n-1 : n; marker_size=(unsigned)*done;
    return marker_error==4 ? -EIO : 0;
}
int vfs_truncate(struct vfs_table *t, int fd, uint64_t size)
{ CHECK(!locks && t==&marker_table && fd==0 && !size); if (marker_error==7) return -EIO; marker_size=0; return 0; }
int vfs_commit(struct vfs_table *t, int fd)
{ CHECK(!locks && t==&marker_table && fd==0); commits++; return marker_error==5 ? -EIO : 0; }
int vfs_close(struct vfs_table *t, int fd)
{ CHECK(!locks && t==&marker_table && fd==0); closes++; return marker_error==6 ? -EIO : 0; }

static uint32_t m_read(void *ctx, uint32_t reg)
{
    (void)ctx; CHECK(!(reg&3u) && reg<RADEON_REGSIZE); model.read_count++;
    if (reg==RBBM_STATUS) {
        if (model.busy_reads) { model.busy_reads--; return 64u | GUI_ACTIVE; }
        return (model.fifo_full ? 0u : 64u) | (model.busy || model.host_pending ? GUI_ACTIVE : 0u);
    }
    if (reg==DSTCACHE_CTLSTAT) return model.cache_busy ? RB2D_DC_BUSY : 0;
    if (reg==DP_GUI_MASTER_CNTL) return model.regs[reg/4] & 0xf800000fu;
    if (reg==CUR_OFFSET && model.bad_cursor) return UINT32_MAX;
    return model.regs[reg/4];
}
/* QEMU pitch units and launch paths, deliberately independent of swdib.
 * Test-only byte traversal preserves each programmed axis direction. */
static void m_launch(unsigned width, unsigned height)
{
    model.launches++;
    if (model.hang_launch) model.busy=true;
    if (model.no_pixels) return;
    uint32_t master=model.regs[DP_GUI_MASTER_CNTL/4], dir=model.regs[DP_CNTL/4];
    CHECK((model.regs[RB2D_DSTCACHE_MODE/4] & model.required_cache)==model.required_cache);
    if ((master & (3u<<24))==DP_SRC_SOURCE_HOST_DATA) {
        CHECK(!model.host_pending && height==1);
        model.host_pending=true; model.host_count=0; model.host_width=width;
        return;
    }
    unsigned datatype=(master>>8)&15u, bytes=datatype==3 || datatype==4 ? 2 : datatype==5 ? 3 : 4;
    uint32_t dstpo=model.regs[DST_PITCH_OFFSET/4], srcpo=model.regs[SRC_PITCH_OFFSET/4];
    unsigned dpitch=(dstpo & 0x3fc00000u)>>16, spitch=(srcpo & 0x3fc00000u)>>16;
    unsigned doff=(dstpo & 0x003fffffu)<<10, soff=(srcpo & 0x003fffffu)<<10;
    /* Hardware chip-local MC base; QEMU implements only the zero base. */
    uint32_t base=model.regs[MC_FB_LOCATION/4]<<16;
    CHECK(doff>=base && soff>=base); doff-=base; soff-=base;
    uint32_t dxy=model.regs[DST_Y_X/4], sxy=model.regs[SRC_Y_X/4];
    int dx=(int)(dxy&0x3fff), dy=(int)(dxy>>16), sx=(int)(sxy&0x3fff), sy=(int)(sxy>>16);
    for (unsigned row=0; row<height; row++) for (unsigned col=0; col<width; col++) {
        int ox=(dir&DST_X_LEFT_TO_RIGHT) ? (int)col : -(int)col;
        int oy=(dir&DST_Y_TOP_TO_BOTTOM) ? (int)row : -(int)row;
        size_t dst=doff+(size_t)(dy+oy)*dpitch+(size_t)(dx+ox)*bytes;
        CHECK(dst+bytes<=sizeof(model.vram));
        if ((master&0x00ff0000u)==ROP3_P) {
            uint32_t colour=model.regs[DP_BRUSH_FRGD_CLR/4];
            if (!(master & (15u<<4))) {
                if (model.no_pattern) continue;
                uint32_t origin=model.regs[BRUSH_Y_X/4];
                unsigned px=(unsigned)(dx+ox-(int)(origin&255u))&7u;
                unsigned py=(unsigned)(dy+oy-(int)((origin>>8)&255u))&7u;
                uint32_t data=model.regs[BRUSH_DATA0/4+py/4u]>>((py%4u)*8u);
                if (!(data & (0x80u>>px))) colour=model.regs[DP_BRUSH_BKGD_CLR/4];
            }
            for (unsigned i=0; i<bytes; i++) model.vram[dst+i]=(uint8_t)(colour>>(i*8));
        } else {
            CHECK((master&0x00ff0000u)==ROP3_S && (master&DP_SRC_SOURCE_MEMORY));
            size_t src=soff+(size_t)(sy+oy)*spitch+(size_t)(sx+ox)*bytes;
            CHECK(src+bytes<=sizeof(model.vram));
            for (unsigned i=0; i<bytes; i++) model.vram[dst+i]=model.vram[src+i];
        }
    }
}
static void m_host_complete(void)
{
    unsigned type=(model.regs[DP_GUI_MASTER_CNTL/4]>>8)&15u;
    unsigned bytes=type==3 || type==4 ? 2 : type==5 ? 3 : 4;
    CHECK(model.host_pending && model.host_count==(model.host_width*bytes+3u)/4u);
    uint32_t dxy=model.regs[DST_Y_X/4], tl=model.regs[SC_TOP_LEFT/4], br=model.regs[SC_BOTTOM_RIGHT/4];
    unsigned x=dxy&0xffffu, y=dxy>>16;
    unsigned pitch=(model.regs[DST_PITCH_OFFSET/4]&0x3fc00000u)>>16;
    for (unsigned p=0;p<model.host_width;p++) {
        if (model.no_pixels || model.no_upload || x+p<(tl&0xffffu) || x+p>=(br&0xffffu) ||
            y<(tl>>16) || y>=(br>>16)) continue;
        size_t dst=(size_t)y*pitch+(x+p)*bytes;
        CHECK(dst+bytes<=sizeof(model.vram));
        for (unsigned b=0;b<bytes;b++) {
            unsigned i=p*bytes+b;
            model.vram[dst+b]=(uint8_t)(model.host[i/4]>>((i%4)*8));
        }
    }
    model.host_pending=false;
    model.us+=model.row_us;
}
static void m_write(void *ctx, uint32_t reg, uint32_t value)
{
    (void)ctx;
    /* A failed start also permits the documented outside-presenter idle
     * proof, but never another draw or setup packet there. */
    CHECK((locks==1 && drawing==PRESENT_BUSY) ||
        (!locks && !engine.active && engine.quarantined &&
         (reg==DSTCACHE_CTLSTAT || reg==CRTC_GEN_CNTL)));
    CHECK(!(reg&3u) && reg<RADEON_REGSIZE);
    /* An unexpected CP/PLL/reset/WAIT_UNTIL write fails the fixture. */
    switch (reg) {
    case DSTCACHE_CTLSTAT: case CRTC_GEN_CNTL: case RB3D_CNTL: case RB2D_DSTCACHE_MODE:
    case DEFAULT_PITCH_OFFSET: case DST_PITCH_OFFSET: case SRC_PITCH_OFFSET:
    case DEFAULT_SC_TOP_LEFT: case DEFAULT_SC_BOTTOM_RIGHT: case SC_BOTTOM_RIGHT:
    case DP_GUI_MASTER_CNTL: case DP_WRITE_MSK: case DP_BRUSH_FRGD_CLR: case DP_BRUSH_BKGD_CLR:
    case DP_CNTL: case SRC_Y_X: case DST_Y_X: case DST_WIDTH_HEIGHT: case DST_HEIGHT_WIDTH:
    case BRUSH_DATA0: case BRUSH_DATA1: case BRUSH_Y_X: case RBBM_GUICNTL:
    case CUR_OFFSET: case CUR_CLR0: case CUR_CLR1: case CUR_HORZ_VERT_POSN: case CUR_HORZ_VERT_OFF:
        CHECK(!model.host_pending); break;
    default: CHECK(reg>=HOST_DATA0 && reg<=HOST_DATA_LAST); break;
    }
    if (model.trace_count<ARRAY_SIZE(model.trace)) model.trace[model.trace_count++]=(typeof(model.trace[0])){reg,value};
    if (!(reg==CRTC_GEN_CNTL && model.stuck_hide && !(value&CRTC_CUR_EN)) &&
        !(reg==RB2D_DSTCACHE_MODE && model.bad_cache)) model.regs[reg/4]=value;
    if (reg==CUR_HORZ_VERT_POSN) model.regs[reg/4]&=0x3fff0fffu;
    if (reg==CUR_HORZ_VERT_OFF) model.regs[reg/4]&=0x003f003fu;
    if (reg==DST_WIDTH_HEIGHT) m_launch(value>>16,value&0x3fffu);
    if (reg==DST_HEIGHT_WIDTH) m_launch(value&0x3fffu,value>>16);
    if (reg>=HOST_DATA0 && reg<=HOST_DATA_LAST) {
        CHECK(model.host_pending && model.host_count<RADEON_UPLOAD_WORDS);
        model.host[model.host_count++]=value;
        if (reg==HOST_DATA_LAST) m_host_complete();
    }
}
static uint64_t m_now(void *ctx) { (void)ctx; return model.us; }
static int m_delay(void *ctx, uint32_t us)
{ (void)ctx; model.delays++; if (!model.frozen) model.us+=us; return 0; }
static void m_drain(void *ctx) { (void)ctx; model.cpu_drains++; }
static const struct radeon_io io={0,m_read,m_write,m_now,m_delay,m_drain};
static void setup(unsigned bpp)
{
    bool rgb555=bpp==15;
    if (rgb555) bpp=16;
    CHECK(!locks);
    memset(&g_boot,0,sizeof(g_boot)); memset(&model,0,sizeof(model));
    memset(&engine,0,sizeof(engine)); memset(resources,0,sizeof(resources));
    memset(&test_vfs,0,sizeof(test_vfs)); memset(&test_volume,0,sizeof(test_volume));
    g_boot.display.vram_bytes=sizeof(model.vram); test_volume.mounted=true; test_vfs.volumes[2]=&test_volume;
    device=(struct fb_device){.width=32,.height=32,.pitch=bpp==24 ? 192 : bpp*4,.size=(bpp==24 ? 192 : bpp*4)*32,
        .phys=0xd0000000u,.mapped=model.vram,.bpp=bpp,.red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,
        .green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,.blue_size=bpp==16 ? 5 : 8,
        .mode_id=0x118,.present=true,.handle=0,.generation=42};
    resources[0]=(struct resource){.type=RES_MMIO,.start=device.phys,.end=device.phys+device.size,
        .owner="fbdev",.generation=42,.state=RS_ACTIVE};
    reservation=resources[0]; resource_count=1;
    pci=(struct pci_func){.vendor=0x1002,.device=fixture_id,.class_code=3,.command=3,
        .bar={0xd0000008u,0,0xe0000000u}};
    pcis=1; claimed=released=revoked=0;
    prepared=authorized=attempt_blocked=false; reg_claim=cursor_claim=-1; registers=0;
    register_window=cursor_window=0; decision="pending";
    marker_size=commits=closes=marker_ios=destroyed=0; marker_error=0; short_write=false;
    cursor_conflict=map_failure=false;
    model.regs[CRTC_GEN_CNTL/4]=(1u<<25)|((bpp==16 ? 4u : bpp==24 ? 5u : 6u)<<8);
    if (rgb555) {
        device.green_size=5; device.red_pos=10;
        model.regs[CRTC_GEN_CNTL/4]=(1u<<25)|(3u<<8);
    }
    model.regs[CRTC_PITCH/4]=device.pitch/(bpp/8u)/8u;
    model.regs[CNFG_MEMSIZE/4]=sizeof(model.vram);
    model.required_cache=fixture_cache;
    model.regs[RB2D_DSTCACHE_MODE/4]=fixture_cache ? 0x10u : 0;
    initialized=true; changing=false; acceleration=0; adapter=&radeon2d_driver;
    accel_initializing=true; accel_blocked=accel_inflight=false; drawing=console_rows=0;
    memset(native_count,0,sizeof(native_count)); memset(software_count,0,sizeof(software_count));
    CHECK(display_driver_bind(&device)==&radeon2d_driver);
}
static void activate(void)
{
    CHECK(!attempt_begin(&test_vfs)); CHECK(marker_size && authorized);
    CHECK(start_with_io(&device,0,&io)==&engine.ops);
    CHECK(prepared && engine.active && engine.cpu_safe && !engine.quarantined);
    CHECK(engine.group->cache_bits==fixture_cache && engine.cache_mode==(fixture_cache ? 0x20110u : 0));
    CHECK(engine.verified==(fixture_id==0x5159 ? RADEON_RECT : RADEON_RECT | RADEON_PATTERN | RADEON_WRITE));
    acceleration=&engine.ops; accel_initializing=false;
    CHECK(!attempt_end(&test_vfs) && !marker_size);
}
static void claim_draw(void)
{ kmutex_lock(&presenter); CHECK(present_begin(&device,&(struct fb_rect){0,0,(int32_t)device.width,(int32_t)device.height})); }
static void end_draw(void)
{ (void)present_end(&device,&(struct fb_rect){0,0,(int32_t)device.width,(int32_t)device.height},false); presenter_unlock(); }
static void stop(void)
{
    claim_draw(); CHECK(!engine.ops.stop(&engine)); acceleration=0; end_draw();
    CHECK(released==claimed && !registers && !engine.cursor && !prepared);
}
static void goldens(void)
{
    uint8_t expected[32*192];
    const unsigned formats[]={15,16,24,32};
    for (unsigned format=0; format<ARRAY_SIZE(formats); format++) {
        setup(formats[format]);
        for (unsigned i=0; i<device.size; i++) model.vram[i]=(uint8_t)(i*53u+17u);
        memcpy(expected,model.vram,device.size); activate(); CHECK(!memcmp(expected,model.vram,device.size));
        struct fb_device reference=device; reference.mapped=expected;
        claim_draw();
        struct fb_rect fill={1,2,23,19};
        uint32_t colour=swdib_pack(&device,0x9e43b7u);
        unsigned n=model.launches;
        unsigned trace=model.trace_count;
        CHECK(!engine.ops.fill_rect(&engine,&fill,colour)); swdib_fill(&reference,&fill,colour);
        CHECK(model.launches==n+1 && !memcmp(expected,model.vram,device.size));
        CHECK(model.regs[DST_WIDTH_HEIGHT/4]==((23u<<16)|19u));
        const uint32_t sequence[]={DSTCACHE_CTLSTAT,DP_GUI_MASTER_CNTL,DP_BRUSH_FRGD_CLR,
            DP_WRITE_MSK,DP_CNTL,DST_Y_X,DST_WIDTH_HEIGHT,DSTCACHE_CTLSTAT};
        CHECK(model.trace_count==trace+ARRAY_SIZE(sequence));
        for (unsigned i=0; i<ARRAY_SIZE(sequence); i++) CHECK(model.trace[trace+i].reg==sequence[i]);
        CHECK(model.regs[DP_GUI_MASTER_CNTL/4]==((engine.datatype<<8)|GMC_CLR_CMP_CNTL_DIS|GMC_BRUSH_SOLID_COLOR|ROP3_P));
        CHECK(model.regs[DST_PITCH_OFFSET/4]==((device.pitch/64u)<<22));
        for (unsigned dir=0; dir<4; dir++) {
            int sx=(dir&1) ? 8 : 2, sy=(dir&2) ? 8 : 2;
            int dx=(dir&1) ? 2 : 8, dy=(dir&2) ? 2 : 8;
            struct fb_rect src={sx,sy,17,17};
            n=model.launches;
            trace=model.trace_count;
            CHECK(!engine.ops.copy_rect(&engine,&src,dx,dy)); swdib_copy(&reference,&src,dx,dy);
            CHECK(model.launches==n+1 && !memcmp(expected,model.vram,device.size));
            CHECK(model.regs[DP_CNTL/4]==((dir&1 ? 1u : 0u)|(dir&2 ? 2u : 0u)));
            const uint32_t copy_sequence[]={DSTCACHE_CTLSTAT,DP_GUI_MASTER_CNTL,DP_WRITE_MSK,
                DP_CNTL,SRC_Y_X,DST_Y_X,DST_HEIGHT_WIDTH,DSTCACHE_CTLSTAT};
            CHECK(model.trace_count==trace+ARRAY_SIZE(copy_sequence));
            for (unsigned i=0; i<ARRAY_SIZE(copy_sequence); i++) CHECK(model.trace[trace+i].reg==copy_sequence[i]);
        }
        n=model.trace_count;
        CHECK(engine.ops.fill_rect(&engine,&(struct fb_rect){-1,0,1,1},0)==-EINVAL);
        CHECK(engine.ops.copy_rect(&engine,&fill,INT32_MAX,0)==-EINVAL && model.trace_count==n);
        CHECK((engine.ops.pattern_rect!=0)==(fixture_id!=0x5159));
        CHECK((engine.ops.write_rect!=0)==(fixture_id!=0x5159));
        end_draw();
        model.busy_reads=300; /* 1.5 ms: exercise the real service pause. */
        struct ciuki_accel_request sliced={.size=sizeof(sliced),.generation=42,
            .operation=CIUKI_ACCEL_FILL,.colour=0x314159,.rect={.width=1,.height=1}};
        CHECK(!fbdev_accel_request(&sliced) && model.pauses);
        memcpy(expected,model.vram,device.size);
        uint32_t tested=0;
        CHECK(!fbdev_accel_validate(1000,0,0,&tested) && tested==(fixture_id==0x5159 ? 3u : 15u));
        CHECK(!memcmp(expected,model.vram,device.size));
        struct ciuki_display_engine_info info;
        display_driver_info(&info);
        CHECK(!strcmp(info.driver_id,"radeon2d") && !strcmp(info.display_name,"ATI Radeon R100/R200/R300/R4xx"));
        CHECK(info.ready && info.native_used && info.capabilities==3);
        CHECK(!fbdev_fill(&(struct fb_rect){0,0,2,2},0x123456));
        display_driver_info(&info); CHECK(info.native_used && info.native[CIUKI_ENGINE_FILL]);
        stop();
    }
}
static bool cancelled(void *arg) { (void)arg; return true; }
static void cursors(void)
{
    const unsigned formats[]={15,16,24,32};
    for (unsigned format=0; format<ARRAY_SIZE(formats); format++) {
        setup(formats[format]); activate(); claim_draw();
        CHECK(engine.ops.cursor_show(&engine,true)==-EINVAL);
        struct accel_cursor shape={.foreground=swdib_pack(&device,0xffffff),.background=0};
        shape.mask[0]=0xc1000001u; shape.image[0]=0x81ffffffu;
        CHECK(!engine.ops.cursor_set(&engine,&shape) && engine.cursor_loaded && engine.cursor_hidden);
        CHECK(engine.cursor[0]==0x3e && engine.cursor[8]==0x81 && engine.cursor[3]==0xfe && engine.cursor[11]==1);
        CHECK(engine.cursor[4]==0xff && engine.cursor[12]==0 && engine.cursor[32*16]==0xff);
        CHECK(model.regs[CUR_CLR1/4]==0xffffff && !model.regs[CUR_CLR0/4]);
        CHECK(!engine.ops.cursor_move(&engine,123,231));
        CHECK(model.regs[CUR_HORZ_VERT_POSN/4]==((123u<<16)|231u));
        unsigned n=model.trace_count;
        CHECK(engine.ops.cursor_move(&engine,-1,0)==-EOPNOTSUPP && model.trace_count==n);
        CHECK(engine.ops.cursor_move(&engine,0,4096)==-EINVAL && model.trace_count==n);
        CHECK(!engine.ops.cursor_show(&engine,true) && !engine.cursor_hidden);
        CHECK(!engine.ops.cursor_set(&engine,&shape) && engine.cursor_hidden);
        CHECK(!cursor_test(&engine.ops,100,0,0) && engine.cursor_hidden);
        CHECK(cursor_test(&engine.ops,100,cancelled,0)==-EINTR && engine.cursor_hidden);
        CHECK(cursor_test(&engine.ops,0,0,0)==-ETIMEDOUT && engine.cursor_hidden);
        end_draw(); stop();
    }
}
static void faults(void)
{
    for (unsigned kind=0; kind<4; kind++) {
        setup(32); activate(); claim_draw();
        if (!kind) model.fifo_full=true;
        if (kind==1) model.busy=true;
        if (kind==2) model.cache_busy=true;
        if (kind==3) { model.busy=true; model.frozen=true; }
        unsigned n=model.launches;
        CHECK(engine.ops.sync(&engine)==-ETIMEDOUT && engine.quarantined && !engine.cpu_safe);
        CHECK(!engine.ops.capabilities(&engine) && model.launches==n && model.delays<=RADEON_POLLS);
        CHECK(!fbdev_console_begin());
        unsigned before_release=released;
        CHECK(engine.ops.stop(&engine)<0 && released==before_release && !engine.cpu_safe);
        CHECK(fallback()==&engine.ops && !engine.cpu_safe);
        model.fifo_full=model.busy=model.cache_busy=model.frozen=false;
        CHECK(fallback()==&engine.ops && engine.cpu_safe && engine.quarantined);
        CHECK(engine.ops.fill_rect(&engine,&(struct fb_rect){0,0,1,1},0)==-ENODEV);
        end_draw(); CHECK(!eligible(&device) && attempt_begin(&test_vfs)==-EIO); stop();
    }
    setup(32); activate();
    model.hang_launch=true;
    struct ciuki_accel_request q={.size=sizeof(q),.generation=42,.operation=CIUKI_ACCEL_COPY,
        .rect={.dst_x=4,.dst_y=4,.width=12,.height=12,.src_x=0,.src_y=0}};
    unsigned n=model.launches;
    CHECK(fbdev_accel_request(&q)==-EAGAIN && model.launches==n+1);
    CHECK(engine.quarantined && !engine.cpu_safe && !fbdev_console_begin());
    model.hang_launch=model.busy=false; claim_draw(); (void)fallback(); end_draw(); stop();

    setup(32); activate(); claim_draw(); model.bad_cursor=true;
    struct accel_cursor shape={0};
    CHECK(engine.ops.cursor_set(&engine,&shape)==-EIO && engine.quarantined);
    model.bad_cursor=false; (void)fallback(); end_draw(); stop();

    setup(32); activate(); claim_draw();
    CHECK(!engine.ops.cursor_set(&engine,&shape) && !engine.ops.cursor_show(&engine,true));
    model.stuck_hide=true; quarantine("injected");
    CHECK(engine.quarantined && !engine.cpu_safe && !engine.cursor_hidden);
    CHECK(fallback()==&engine.ops && !engine.cpu_safe && !released);
    model.stuck_hide=false; CHECK(fallback()==&engine.ops && engine.cpu_safe);
    end_draw(); stop();

    setup(32); activate();
    uint8_t saved[32*128]; memcpy(saved,model.vram,device.size);
    model.no_pixels=true;
    uint32_t tested=0;
    CHECK(fbdev_accel_validate(1000,0,0,&tested)==-EIO && !tested);
    CHECK(engine.quarantined && engine.cpu_safe && !memcmp(saved,model.vram,device.size));
    stop();

    setup(32); activate(); claim_draw(); model.regs[MC_FB_LOCATION/4]=1;
    n=model.launches;
    CHECK(engine.ops.fill_rect(&engine,&(struct fb_rect){0,0,1,1},0)==-EIO && model.launches==n);
    CHECK(fallback()==&engine.ops && !engine.cpu_safe && engine.unowned);
    end_draw();
    CHECK(!fbdev_console_begin() && !released); /* retain an unsafe access guard */
}
static void refusals(void)
{
    setup(32); unsigned writes=model.trace_count;
    CHECK(probe(&pci) && !model.trace_count && !claimed);
    pci.device=0x7142; CHECK(!probe(&pci)); pci.device=0x5159;
    pci.device=0x4336; CHECK(!probe(&pci)); pci.device=0x5159; /* unqualified UMA */
    pci.command|=4; CHECK(!probe(&pci)); pci.command=3;
    pci.revision=0xff; CHECK(!probe(&pci)); pci.revision=0;
    pci.bar[2]|=4; CHECK(!probe(&pci)); pci.bar[2]=0xe0000000u;
    pci.bar[2]=0xd0000000u; CHECK(!probe(&pci)); pci.bar[2]=0xe0000000u;
    pci.fn=1; CHECK(!probe(&pci)); pci.fn=0;
    resources[0].owner="other"; CHECK(!display_driver_bind(&device)); resources[0].owner="fbdev";
    resources[0].generation++; CHECK(!display_driver_bind(&device)); resources[0].generation--;
    g_boot.e820_count=1;
    g_boot.e820[0]=(struct ciuki_e820){.base=0xd0000000u,.length=65536,.type=CBI_E820_RAM};
    CHECK(!display_driver_bind(&device));
    g_boot.e820[0].base=0xe0000000u; CHECK(!probe(&pci)); g_boot.e820_count=0;
    pcis=2; CHECK(!display_driver_bind(&device)); pcis=1; CHECK(display_driver_bind(&device));
    struct radeon_engine temporary;
    model.regs[CRTC_OFFSET/4]=16; CHECK(radeon_engine_prepare(&temporary,&device,&io,RADEON_R100)==-EOPNOTSUPP);
    model.regs[CRTC_OFFSET/4]=0; model.regs[CRTC_OFFSET_CNTL/4]=0x8000;
    CHECK(radeon_engine_prepare(&temporary,&device,&io,RADEON_R100)==-EOPNOTSUPP);
    model.regs[CRTC_OFFSET_CNTL/4]=0; model.regs[CP_CSQ_CNTL/4]=1;
    CHECK(radeon_engine_prepare(&temporary,&device,&io,RADEON_R100)==-EOPNOTSUPP);
    model.regs[CP_CSQ_CNTL/4]=0; device.pitch++;
    CHECK(radeon_engine_prepare(&temporary,&device,&io,RADEON_R100)==-EOPNOTSUPP);
    CHECK(model.trace_count==writes && !claimed);

    setup(32); g_boot.flags=CBI_F_SAFE_MODE;
    CHECK(!eligible(&device) && attempt_begin(&test_vfs)==-EOPNOTSUPP && !claimed && !marker_ios);
    setup(32); strcpy(g_boot.options,"accel=0"); CHECK(!eligible(&device) && attempt_begin(&test_vfs)==-EOPNOTSUPP);
    setup(32); CHECK(!start_with_io(&device,0,&io) && !claimed && !model.trace_count);
    setup(32); marker_size=1; CHECK(attempt_begin(&test_vfs)==-EIO && marker_size==1 && !authorized && !commits);
    for (int err=1; err<=6; err++) {
        setup(32); marker_error=err;
        CHECK(attempt_begin(&test_vfs)<0 && !authorized && !claimed);
        CHECK(!start_with_io(&device,0,&io) && !model.trace_count);
    }
    setup(32); short_write=true; CHECK(attempt_begin(&test_vfs)<0 && !authorized);
    setup(32); test_volume.readonly=true; CHECK(attempt_begin(&test_vfs)==-FS_EROFS && !authorized);
    setup(32); activate(); marker_error=5;
    CHECK(attempt_end(&test_vfs)<0 && attempt_blocked);
    claim_draw(); quarantine("marker_clear_failed"); end_draw(); stop();
    setup(32); cursor_conflict=true; activate(); CHECK(engine.ops.capabilities(&engine)==1); stop();
    setup(32); map_failure=true; CHECK(!attempt_begin(&test_vfs));
    CHECK(!start_with_io(&device,0,&io) && claimed==released && !model.trace_count);
    setup(32); model.no_pixels=true; CHECK(!attempt_begin(&test_vfs));
    CHECK(!start_with_io(&device,0,&io) && engine.quarantined && engine.cpu_safe);
    CHECK(marker_size && attempt_end(&test_vfs)==-EIO);
    acceleration=&engine.ops; accel_initializing=false; stop();
}
static void optional_goldens(void)
{
    const unsigned formats[]={15,16,24,32};
    for (unsigned f=0;f<ARRAY_SIZE(formats);f++) {
        setup(formats[f]); activate(); claim_draw();
        uint8_t expected[32*192];
        struct fb_device ref=device; ref.mapped=expected;
        memcpy(expected,model.vram,device.size);
        const uint8_t pattern[8]={0x80,0x41,0x22,0x14,0x08,0x05,0x02,0x01};
        uint32_t fg=swdib_pack(&device,0xe72693), bg=swdib_pack(&device,0x12ab56);
        struct fb_rect r={3,5,19,13};
        model.trace_count=0;
        CHECK(!engine.ops.pattern_rect(&engine,&r,pattern,fg,bg));
        swdib_pattern(&ref,&r,pattern,fg,bg);
        CHECK(!memcmp(expected,model.vram,device.size));
        const uint32_t sequence[]={DSTCACHE_CTLSTAT,DP_GUI_MASTER_CNTL,DP_WRITE_MSK,
            DP_BRUSH_FRGD_CLR,DP_BRUSH_BKGD_CLR,BRUSH_DATA0,BRUSH_DATA1,DP_CNTL,
            BRUSH_Y_X,DST_Y_X,DST_HEIGHT_WIDTH,DSTCACHE_CTLSTAT};
        CHECK(model.trace_count==ARRAY_SIZE(sequence));
        for (unsigned i=0;i<ARRAY_SIZE(sequence);i++) CHECK(model.trace[i].reg==sequence[i]);
        CHECK(model.regs[BRUSH_DATA0/4]==0x14224180u && model.regs[BRUSH_DATA1/4]==0x01020508u);
        CHECK(!model.regs[BRUSH_Y_X/4]);
        unsigned n=model.trace_count;
        CHECK(engine.ops.pattern_rect(&engine,&r,0,fg,bg)==-EINVAL && model.trace_count==n);

        uint32_t upload[32*4];
        for (unsigned i=0;i<ARRAY_SIZE(upload);i++) upload[i]=0xef000000u+i*65713u;
        struct fb_rect dst={-2,-1,32,4}, clip={1,1,27,2};
        swdib_convert(&ref,&dst,upload,32*4);
        model.trace_count=0;
        n=model.launches;
        CHECK(!engine.ops.write_rect(&engine,&dst,&clip,upload,32*4));
        swdib_write(&ref,&dst,&clip,upload,32*4);
        CHECK(!memcmp(expected,model.vram,device.size) && model.launches==n+2 && !model.host_pending);
        unsigned words=(27u*(device.bpp/8u)+3u)/4u, terminals=0, launches=0;
        unsigned at=0;
        const uint32_t setup_sequence[]={DSTCACHE_CTLSTAT,DP_GUI_MASTER_CNTL,DP_WRITE_MSK,DP_CNTL,
            RBBM_GUICNTL,SC_TOP_LEFT,SC_BOTTOM_RIGHT,DST_Y_X,DST_HEIGHT_WIDTH};
        for (unsigned row=0;row<2;row++) {
            /* The second row starts with the previous completion's empty
             * FIFO proof, without an extra pre-submission flush. */
            unsigned first=row ? 1 : 0;
            for (unsigned i=first;i<ARRAY_SIZE(setup_sequence);i++)
                CHECK(model.trace[at++].reg==setup_sequence[i]);
            for (unsigned i=0;i<words;) {
                unsigned count=words-i>8 ? 8 : words-i;
                uint32_t first_reg=words-i>8 ? HOST_DATA0 : HOST_DATA_LAST-(count-1)*4;
                for (unsigned j=0;j<count;j++,i++) CHECK(model.trace[at++].reg==first_reg+j*4);
            }
            CHECK(model.trace[at++].reg==DSTCACHE_CTLSTAT);
        }
        CHECK(at==model.trace_count);
        for (unsigned i=0;i<model.trace_count;i++) {
            if (model.trace[i].reg==HOST_DATA_LAST) terminals++;
            if (model.trace[i].reg==DST_HEIGHT_WIDTH) launches++;
        }
        CHECK(terminals==2 && launches==2);
        CHECK((model.regs[DP_GUI_MASTER_CNTL/4] & (3u<<24))==DP_SRC_SOURCE_HOST_DATA);
        CHECK(model.regs[SC_BOTTOM_RIGHT/4]==((3u<<16)|28u));
        n=model.trace_count;
        CHECK(engine.ops.write_rect(&engine,&dst,&clip,upload,1)==-EINVAL && model.trace_count==n);
        CHECK(engine.ops.write_rect(&engine,&dst,&clip,model.vram,32*4)==-EOPNOTSUPP && model.trace_count==n);
        CHECK(engine.ops.write_rect(&engine,&dst,&clip,(void *)(UINTPTR_MAX-8u),32*4)==-EINVAL && model.trace_count==n);
        /* Hooks remain clean refusals until their individual comparisons pass. */
        engine.verified&=~RADEON_PATTERN;
        CHECK(engine.ops.pattern_rect(&engine,&r,pattern,fg,bg)==-EOPNOTSUPP && model.trace_count==n);
        engine.verified&=~RADEON_WRITE;
        CHECK(engine.ops.write_rect(&engine,&dst,&clip,upload,32*4)==-EOPNOTSUPP && model.trace_count==n);
        end_draw(); stop();
    }
    /* An empty FIFO accommodates the largest whole upload packet (56
     * slots); one word beyond that refuses without any hardware write. */
    setup(32);
    device.width=64; device.pitch=256; device.size=device.pitch*device.height;
    resources[0].end=device.phys+device.size; reservation=resources[0];
    model.regs[CRTC_PITCH/4]=8;
    activate(); claim_draw();
    uint32_t rows[64]; for (unsigned i=0;i<64;i++) rows[i]=i*7919u;
    struct fb_rect max={0,0,48,1}, too_wide={0,0,49,1};
    model.trace_count=0;
    CHECK(!engine.ops.write_rect(&engine,&max,&max,rows,sizeof(rows)));
    unsigned n=model.trace_count;
    CHECK(engine.ops.write_rect(&engine,&too_wide,&too_wide,rows,sizeof(rows))==-EOPNOTSUPP && model.trace_count==n);
    CHECK(!memcmp(model.vram,rows,48*4));
    end_draw();
    model.busy_reads=300;
    uint64_t native=native_count[CIUKI_ENGINE_WRITE], software=software_count[CIUKI_ENGINE_WRITE];
    CHECK(!fbdev_accel_write(device.generation,&max,&max,rows,sizeof(rows)) && model.pauses);
    CHECK(native_count[CIUKI_ENGINE_WRITE]==native+1);
    n=model.launches;
    CHECK(fbdev_accel_write(device.generation,&too_wide,&too_wide,rows,sizeof(rows))==1);
    CHECK(model.launches==n && software_count[CIUKI_ENGINE_WRITE]==software+1);
    CHECK(!memcmp(model.vram,rows,49*4));
    stop();
}
static void optional_faults(void)
{
    struct fb_rect r={0,0,8,2};
    const uint8_t pattern[8]={0x81,0x42,0x24,0x18,0x18,0x24,0x42,0x81};
    uint32_t rows[16]; for (unsigned i=0;i<16;i++) rows[i]=0x123456u+i;
    for (unsigned op=0;op<2;op++) {
        setup(32); activate(); claim_draw();
        unsigned n=model.launches;
        model.hang_launch=true;
        int e=op ? engine.ops.write_rect(&engine,&r,&r,rows,8*4) :
            engine.ops.pattern_rect(&engine,&r,pattern,0xff00,0x123456);
        CHECK(e==-ETIMEDOUT && engine.quarantined && !engine.cpu_safe && !model.host_pending);
        CHECK(model.launches==n+1 && !engine.ops.capabilities(&engine));
        CHECK(fallback()==&engine.ops && !engine.cpu_safe && !released);
        model.hang_launch=model.busy=false;
        CHECK(fallback()==&engine.ops && engine.cpu_safe && engine.quarantined);
        end_draw(); stop();
    }
    for (unsigned op=0;op<2;op++) {
        setup(32); activate();
        uint8_t saved[32*128]; memcpy(saved,model.vram,device.size);
        if (op) model.no_upload=true; else model.no_pattern=true;
        uint32_t tested=0;
        CHECK(fbdev_accel_validate(1000,0,0,&tested)==-EIO && tested==(op ? 7u : 3u));
        CHECK(engine.quarantined && engine.cpu_safe && !memcmp(saved,model.vram,device.size));
        stop();
    }
    setup(32); activate(); claim_draw();
    model.row_us=RADEON_WAIT_US;
    unsigned n=model.launches;
    CHECK(engine.ops.write_rect(&engine,&r,&r,rows,8*4)==-ETIMEDOUT);
    CHECK(model.launches==n+1 && engine.quarantined && engine.cpu_safe && !model.host_pending);
    end_draw(); stop();
    setup(32); activate();
    uint32_t tall[32]; for (unsigned i=0;i<32;i++) tall[i]=i;
    struct fb_rect column={0,0,1,32};
    /* Instant completions still yield at the row cap, without depending
     * on a working clock. The production pause rejects console/mode work. */
    model.frozen=true;
    CHECK(!fbdev_accel_write(device.generation,&column,&column,tall,4) && model.pauses);
    stop();
}
static void subgroup_gates(void)
{
    for (unsigned i=0;i<ARRAY_SIZE(matches);i++) {
        setup(32); pci.device=matches[i].device;
        CHECK(probe(&pci) && chip_group(pci.device)!=RADEON_UNKNOWN && !model.trace_count && !claimed);
        CHECK(display_driver_bind(&device)==&radeon2d_driver);
    }
    const uint16_t refused[]={0x4336,0x5834,0x5954,0x5955,0x5a62,0x5975, /* UMA/Xpress */
        0x4164,0x4170,0x4e64,0x4a69,0x5569, /* secondary functions */
        0x7142,0x7100,0x71c0,0x5e48,0xffff}; /* R500, unqualified RV410 */
    for (unsigned i=0;i<ARRAY_SIZE(refused);i++) {
        setup(32); pci.device=refused[i];
        CHECK(!probe(&pci) && !display_driver_bind(&device) && !model.trace_count && !claimed);
    }
    setup(32);
    struct radeon_engine temporary;
    CHECK(!radeon_engine_prepare(&temporary,&device,&io,chip_group(fixture_id)));
    CHECK(!temporary.verified && !temporary.ops.capabilities(&temporary));
    model.regs[MC_FB_LOCATION/4]=1; /* top precedes base */
    CHECK(radeon_engine_prepare(&temporary,&device,&io,chip_group(fixture_id))==-EOPNOTSUPP);
    model.regs[MC_FB_LOCATION/4]=0;
    model.regs[RB2D_DSTCACHE_MODE/4]=UINT32_MAX;
    CHECK(radeon_engine_prepare(&temporary,&device,&io,chip_group(fixture_id))==-EOPNOTSUPP);
    CHECK(!model.trace_count && !claimed);
    for (unsigned failure=0;failure<3;failure++) {
        setup(32);
        if (!failure) model.no_pattern=true;
        if (failure==1) model.no_upload=true;
        if (failure==2) model.bad_cache=true;
        if (failure==2 && !fixture_cache) continue;
        uint8_t saved[32*128]; memcpy(saved,model.vram,device.size);
        CHECK(!attempt_begin(&test_vfs));
        CHECK(!start_with_io(&device,0,&io) && engine.quarantined && !engine.active);
        CHECK(!engine.ops.capabilities(&engine) && engine.cpu_safe);
        CHECK(!memcmp(saved,model.vram,device.size));
        CHECK(marker_size && attempt_end(&test_vfs)==-EIO && !eligible(&device));
        acceleration=&engine.ops; accel_initializing=false; stop();
    }
    /* Pre-AVIVO cursor offsets are relative to VRAM, independently of the
     * 2D engine's chip-local MC base (X.Org radeon_cursor.c). */
    setup(32); model.regs[MC_FB_LOCATION/4]=0x01000100u; activate();
    CHECK(engine.local_base==0x01000000u && engine.cursor && engine.ops.capabilities(&engine)==3);
    claim_draw();
    struct accel_cursor shape={.foreground=0xffffff}; shape.mask[0]=shape.image[0]=0x80000000u;
    CHECK(!engine.ops.cursor_set(&engine,&shape) && !engine.ops.cursor_show(&engine,true));
    CHECK(model.regs[CUR_OFFSET/4]==sizeof(model.vram)-RADEON_CURSOR_BYTES);
    end_draw(); stop();
    setup(32); activate(); claim_draw();
    model.regs[RB2D_DSTCACHE_MODE/4]^=1u;
    unsigned n=model.launches;
    CHECK(engine.ops.fill_rect(&engine,&(struct fb_rect){0,0,1,1},0)==-EIO && model.launches==n);
    CHECK(engine.quarantined && !engine.cpu_safe && !released);
    end_draw();
}
int main(void)
{
    setbuf(stdout,0);
    goldens(); cursors(); faults(); refusals();
    setup(32);
    struct radeon_engine temporary;
    struct fb_device large=device; large.height=1024; large.size=large.height*large.pitch;
    model.regs[CNFG_MEMSIZE/4]=8u*1024u*1024u;
    CHECK(!radeon_engine_prepare(&temporary,&large,&io,RADEON_R100));
    CHECK(radeon_engine_prepare(&temporary,&large,&io,RADEON_R200)==-EOPNOTSUPP);
    model.regs[MC_FB_LOCATION/4]=127u<<16;
    CHECK(!radeon_engine_prepare(&temporary,&large,&io,RADEON_R200));
    CHECK(!model.trace_count && !claimed);
    const struct { uint16_t id; enum radeon_group group; uint32_t cache; } fixtures[]={
        {0x514c,RADEON_R200,0}, {0x4966,RADEON_R200,0}, {0x5961,RADEON_R200,0},
        {0x4e44,RADEON_R300,0x20100u}, {0x4e48,RADEON_R300,0x20100u},
        {0x4150,RADEON_R300,0x20100u}, {0x3e50,RADEON_R300,0x20100u},
        {0x4a49,RADEON_R4XX,0x20100u}, {0x5549,RADEON_R4XX,0x20100u}
    };
    for (unsigned i=0;i<ARRAY_SIZE(fixtures);i++) {
        fixture_id=fixtures[i].id; fixture_cache=fixtures[i].cache;
        CHECK(chip_group(fixture_id)==fixtures[i].group);
        goldens(); cursors(); faults(); optional_goldens(); optional_faults(); subgroup_gates();
        printf("subgroup %04x: F/C/Y/P/W/cursor, goldens, DM9, refusal and quarantine checked\n",fixture_id);
    }
    printf("radeon2d: register sequences, software goldens, native comparison, cursor, ownership, marker, quarantine: %u failures\n",failures);
    return failures ? 1 : 0;
}
