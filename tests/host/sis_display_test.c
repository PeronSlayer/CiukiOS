/* Independent SiS MMIO interpreter plus production registry/resource glue.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/mm.h>
#include <ciuki/swdib.h>
#include "../../src/kernel/drivers/video/sis/register.c"
#include "../../src/kernel/drivers/video/registry.c"
const struct display_driver s3savage_driver={0};
struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c); failures++; } } while (0)
static uint8_t pixels[64*64*4+64], expected[sizeof(pixels)], cursor_bytes[4096];
static struct fb_device display;
static struct pci_func cards[2];
static unsigned card_count, presenter, probes, claims, maps, revokes, yields;
static bool claim_failure, mapping_failure;
static struct resource resources[8];
static unsigned resource_count;
struct write_event { unsigned reg, width; uint32_t value; };
static struct {
    uint8_t seq[256], cr[256];
    uint32_t regs[SIS_MMIO_BYTES/4];
    struct write_event writes[4096];
    unsigned count, reads, fires, drains, wait_busy;
    bool busy, dead, frozen, hang_after_fire, corrupt, sticky_cursor, unlock_failed, reject_queue;
    uint64_t now;
} model;
static uint32_t getpixel(int x,int y)
{
    CHECK(x>=0 && y>=0 && x<(int)display.width && y<(int)display.height);
    uint32_t v=0; unsigned bytes=display.bpp/8;
    memcpy(&v,pixels+(unsigned)y*display.pitch+(unsigned)x*bytes,bytes); return v;
}
static void setpixel(int x,int y,uint32_t v)
{
    CHECK(x>=0 && y>=0 && x<(int)display.width && y<(int)display.height);
    memcpy(pixels+(unsigned)y*display.pitch+(unsigned)x*(display.bpp/8),&v,display.bpp/8);
}
static uint32_t rop(unsigned code,uint32_t p,uint32_t src,uint32_t dst)
{
    uint32_t out=0;
    for (unsigned bit=0;bit<32;bit++) {
        unsigned index=(((p>>bit)&1)<<2)|(((src>>bit)&1)<<1)|((dst>>bit)&1);
        out|=((code>>index)&1u)<<bit;
    }
    return out;
}
/* Register interpretation uses silicon encoding, never driver helpers or
 * swdib. Pixels therefore detect bad ROPs, pitch, depth, bases and directions. */
static void execute(void)
{
    unsigned command=model.regs[0x823c/4], code=(command>>8)&255;
    unsigned wh=model.regs[0x8218/4], w=wh&65535, h=wh>>16;
    unsigned srcxy=model.regs[0x8208/4], dstxy=model.regs[0x820c/4];
    int sx=srcxy>>16, sy=srcxy&65535, dx=dstxy>>16, dy=dstxy&65535;
    int xstep=1, ystep=1;
    CHECK(model.regs[0x8210/4]==0 && model.regs[0x8200/4]==0);
    CHECK((model.regs[0x8214/4]&65535)==display.pitch);
    CHECK((model.regs[0x8204/4]>>16)==(display.bpp==16 ? 0x8000u : 0xc000u));
    if (code==0xcc) {
        CHECK((model.regs[0x8204/4]&65535)==display.pitch);
        if (engine.generation==SIS_300) {
            xstep=command&0x10000 ? 1 : -1; ystep=command&0x20000 ? 1 : -1;
        } else {
            if (dy>sy) { ystep=-1; sy+=h-1; dy+=h-1; }
            if (dx>sx) { xstep=-1; sx+=w-1; dx+=w-1; }
        }
    }
    if (engine.generation==SIS_315) CHECK((command&0x30000)==(display.bpp==16 ? 0x10000u : 0x20000u));
    for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
        int px=dx+(int)x*xstep, py=dy+(int)y*ystep;
        uint32_t p=model.regs[0x821c/4], src=0;
        if (command&0x80) {
            unsigned row=(unsigned)py&7, shift=(row&3)*8;
            uint8_t pattern=model.regs[(row<4 ? 0x822c : 0x8230)/4]>>shift;
            p=(pattern&(0x80u>>((unsigned)px&7))) ? p : model.regs[0x8220/4];
        }
        if (code==0xcc) src=getpixel(sx+(int)x*xstep,sy+(int)y*ystep);
        setpixel(px,py,rop(code,p,src,getpixel(px,py)));
    }
    if (model.corrupt) pixels[0]^=1;
}
static uint32_t fake_read(void *ctx,unsigned reg,unsigned width)
{
    (void)ctx; model.reads++;
    CHECK(reg<SIS_MMIO_BYTES && (width==16 || width==32) && !(reg&(width/8-1)));
    unsigned status=engine.generation==SIS_300 ? 0x8240 : 0x85cc;
    if (reg==status && width==32) {
        if (model.dead) return UINT32_MAX;
        if (model.busy || model.wait_busy) { if (model.wait_busy) model.wait_busy--; return 0; }
        return engine.generation==SIS_300 ? 0xe00000ff : 0x80000000;
    }
    if (reg==0x8240 && width==16) return 255;
    uint32_t value=model.regs[reg/4]>>((reg&2)*8);
    return width==16 ? value&65535 : value;
}
static void fake_write(void *ctx,unsigned reg,unsigned width,uint32_t value)
{
    (void)ctx;
    CHECK(presenter==1);
    CHECK(model.count<ARRAY_SIZE(model.writes));
    if (model.count<ARRAY_SIZE(model.writes)) model.writes[model.count++]=(struct write_event){reg,width,value};
    if (model.sticky_cursor && reg==0x8500) return;
    if (width==32) model.regs[reg/4]=value;
    else { unsigned shift=(reg&2)*8; model.regs[reg/4]=(model.regs[reg/4]&~(65535u<<shift))|((value&65535)<<shift); }
    if (reg==0x8240) {
        model.fires++;
        execute();
        if (model.hang_after_fire) model.busy=true;
    }
}
static uint8_t fake_index_read(void *ctx,bool cr,uint8_t index)
{ (void)ctx; return (cr ? model.cr : model.seq)[index]; }
static void fake_index_write(void *ctx,bool cr,uint8_t index,uint8_t value)
{
    (void)ctx; CHECK(presenter==1);
    if (!cr && index==5) { if (!model.unlock_failed) model.seq[index]=value==0x86 ? 0xa1 : 0; return; }
    if (!cr && index==0x26 && model.reject_queue) return;
    (cr ? model.cr : model.seq)[index]=value;
}
static uint64_t fake_now(void *ctx)
{ (void)ctx; if (!model.frozen) model.now+=10; return model.now; }
static void fake_drain(void *ctx) { (void)ctx; model.drains++; }
static const struct sis_io fake_io={0,fake_read,fake_write,fake_index_read,fake_index_write,fake_now,fake_drain};
static int pause_wait(void *ctx)
{ (void)ctx; CHECK(presenter==1); yields++; return 0; }
static int cancel_wait(void *ctx) { (void)ctx; return -EINTR; }
unsigned pci_count(void) { return card_count; }
const struct pci_func *pci_get(unsigned i) { return i<card_count ? &cards[i] : 0; }
unsigned registry_count(void) { return resource_count; }
const struct resource *registry_get(unsigned i) { return i<resource_count ? &resources[i] : 0; }
bool registry_valid(int h,gen_t gen)
{ return h>=0 && (unsigned)h<resource_count && resources[h].generation==gen && resources[h].state==RS_ACTIVE; }
int registry_claim(enum res_type type,uint32_t first,uint32_t end,const char *name,bool shared)
{
    CHECK(!presenter && !shared); claims++;
    if (claim_failure) return -EBUSY;
    CHECK(resource_count<ARRAY_SIZE(resources));
    unsigned n=resource_count++;
    resources[n]=(struct resource){type,first,end,name,100+n,RS_CLAIMED,false};
    return (int)n;
}
int registry_activate(int h,gen_t gen)
{ CHECK(!presenter && resources[h].generation==gen); resources[h].state=RS_ACTIVE; return 0; }
int registry_quiesce(int h,gen_t gen,registry_idle_proof_fn proof)
{ CHECK(!presenter || (!engine.active && engine.cpu_safe)); if (!proof(h,gen)) return -EIO; resources[h].state=RS_QUIESCING; return 0; }
int registry_release(int h,gen_t gen)
{ CHECK((!presenter || (!engine.active && engine.cpu_safe)) && resources[h].generation==gen); resources[h].state=RS_RELEASED; return 0; }
int registry_quarantine(int h,gen_t gen)
{ CHECK(resources[h].generation==gen); resources[h].state=RS_QUARANTINED; return 0; }
void *vmm_reserve_mmio(uint32_t size)
{ CHECK(!presenter); return size==SIS_MMIO_BYTES ? (void *)model.regs : cursor_bytes; }
void *vmm_replace_mmio(void *window,uint32_t capacity,uint32_t phys,uint32_t size)
{ CHECK(!presenter && window && size<=capacity && phys); maps++; return mapping_failure ? 0 : window; }
void vmm_revoke_mmio(void *p,uint32_t size) { CHECK((!presenter || (!engine.active && engine.cpu_safe)) && p && size); revokes++; }
const struct fb_device *fbdev_get(void) { return &display; }
unsigned fbdev_native_capabilities(void) { return sis_capabilities(&engine); }
bool fbdev_cpu_ready(void) { return sis_cpu_ready(&engine); }
void framebuffer_write_drain(void) { fake_drain(0); }
uint64_t ktime_cycles(void) { return model.now; }
void klog(const char *fmt,...) { (void)fmt; CHECK(!presenter); }
int fbdev_accel_probe(const struct fb_device *d,int (*fn)(void *),void *ctx)
{
    CHECK(!presenter && d==&display); presenter=1; probes++;
    int e=fn(ctx);
    if (e) {
        const struct accel_ops *guard=fallback();
        CHECK(engine.cpu_safe || (guard && !guard->cpu_ready(guard->ctx)));
    }
    presenter=0; return e;
}
/* Durable marker fault model: files have independent legacy/new sizes. */
static struct fat_volume volume;
static struct vfs volume_fs;
static unsigned marker_size, legacy_size, commits, closes, destroys;
static int fs_failure;
int vfs_table_init(struct vfs *fs,struct vfs_table *table,unsigned drive)
{ (void)fs; (void)table; CHECK(!presenter && drive==2); return fs_failure==1 ? -FS_EIO : 0; }
void vfs_table_destroy(struct vfs_table *table) { (void)table; destroys++; }
int vfs_stat(struct vfs_table *table,const char *path,struct fat_entry *entry)
{
    (void)table; CHECK(!presenter);
    if (fs_failure==2) return -FS_EIO;
    entry->size=strstr(path,"accel.cfg") ? legacy_size : marker_size; return 0;
}
int vfs_open(struct vfs_table *table,const char *path,unsigned flags,enum vfs_share share,uint8_t attr)
{ (void)table; (void)attr; CHECK(!presenter && strstr(path,"sis") && flags==(VFS_WRITE|VFS_CREATE) && share==VFS_DENY_ALL); return fs_failure==3 ? -FS_EIO : 3; }
int vfs_write(struct vfs_table *table,int fd,const void *p,size_t size,size_t *done)
{
    (void)table; CHECK(!presenter && fd==3 && size && strstr(p,"pci=1039:") && strstr(p,"state=attempt"));
    *done=fs_failure==4 ? size-1 : size; marker_size=(unsigned)*done; return 0;
}
int vfs_truncate(struct vfs_table *table,int fd,uint64_t size)
{ (void)table; CHECK(!presenter && fd==3 && !size); marker_size=0; return 0; }
int vfs_commit(struct vfs_table *table,int fd)
{ (void)table; CHECK(!presenter && fd==3); commits++; return fs_failure==5 ? -FS_EIO : 0; }
int vfs_close(struct vfs_table *table,int fd)
{ (void)table; CHECK(!presenter && fd==3); closes++; return fs_failure==6 ? -FS_EIO : 0; }

static void setup(enum sis_generation generation,unsigned bpp)
{
    memset(&model,0,sizeof(model)); memset(&g_boot,0,sizeof(g_boot));
    memset(&engine,0,sizeof(engine)); memset(resources,0,sizeof(resources));
    engine.cpu_safe=true; engine.reason="pending";
    memset(pixels,0xa5,sizeof(pixels)); memset(cursor_bytes,0xcc,sizeof(cursor_bytes));
    display=(struct fb_device){.width=32,.height=32,.pitch=32*(bpp/8)+8,.size=32*(32*(bpp/8)+8),
        .phys=0xe0000000,.mapped=pixels,.bpp=bpp,.red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,
        .green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,.blue_size=bpp==16 ? 5 : 8,
        .present=true,.handle=0,.generation=42};
    resources[0]=(struct resource){RES_MMIO,display.phys,display.phys+display.size,"fbdev",42,RS_ACTIVE,false};
    resource_count=1; card_count=1;
    cards[0]=(struct pci_func){.vendor=0x1039,.device=generation==SIS_300 ? 0x0300 : 0x0315,.class_code=3,.command=3,
        .bar={0xe0000008,0xd0000000,0x1001}};
    g_boot.display.vram_bytes=16*1024*1024;
    model.seq[6]=bpp==16 ? 0x08 : 0x10; model.seq[0x20]=0x81;
    authorized=marker_owned=blocked=false;
    reg_claim=port_claim=scratch_claim=-1;
    registers=0; reg_window=cursor_window=0; cursor_mapping=cursor_bytes;
    presenter=probes=claims=maps=revokes=yields=0; claim_failure=mapping_failure=false;
    marker_size=legacy_size=commits=closes=destroys=0; fs_failure=0;
    memset(&volume,0,sizeof(volume)); memset(&volume_fs,0,sizeof(volume_fs));
    volume.mounted=true; volume_fs.volumes[2]=&volume;
    CHECK(eligible(&display));
}
static const struct accel_ops *activate(void)
{
    CHECK(!attempt_begin(&volume_fs) && marker_size && commits==1);
    CHECK(map_resources() && claims==3 && maps==2);
    const struct accel_ops *a=start_with_io(&display,&fake_io);
    CHECK(a==&ops && probes==1 && engine.cpu_safe && engine.active);
    CHECK(!attempt_end(&volume_fs) && !marker_size && commits==2);
    return a;
}
static bool cancelled(void *ctx) { return *(bool *)ctx; }
static void lifecycle_and_identity(enum sis_generation generation)
{
    setup(generation,32);
    const struct display_driver *d=display_driver_bind(&display);
    CHECK(d==(generation==SIS_300 ? &sis300_driver : &sis315_driver));
    struct ciuki_display_engine_info info;
    memset(native_count,0,sizeof(native_count));
    memset(software_count,0,sizeof(software_count));
    display_driver_info(&info);
    CHECK(!info.ready && !info.native_used && !strcmp(info.display_name,d->name));
    activate(); presenter=1;
    display_driver_refresh(); display_driver_info(&info);
    CHECK(info.ready && !info.native_used && info.pci_vendor==0x1039);
    CHECK(!strcmp(info.driver_id,generation==SIS_300 ? "sis300" : "sis315"));
    bool cancel=false;
    CHECK(!cursor_test(&ops,100,cancelled,&cancel) && !engine.cursor_enabled);
    unsigned n=model.fires;
    CHECK(cursor_test(&ops,0,cancelled,&cancel)==-ETIMEDOUT && !engine.cursor_enabled && model.fires==n);
    cancel=true; CHECK(cursor_test(&ops,100,cancelled,&cancel)==-EINTR && !engine.cursor_enabled);
    display_driver_count(CIUKI_ENGINE_FILL,true); display_driver_info(&info);
    CHECK(info.native_used && info.native[CIUKI_ENGINE_FILL]==1);
    CHECK(!ops.stop(&engine) && !engine.configured && !registers && !cursor_mapping);
    CHECK(reg_claim<0 && port_claim<0 && scratch_claim<0 && revokes==2);
    presenter=0;
    display.generation++; resources[0].generation=display.generation;
    CHECK(eligible(&display) && !attempt_begin(&volume_fs) && map_resources());
    CHECK(start_with_io(&display,&fake_io)==&ops);
    CHECK(!attempt_end(&volume_fs)); presenter=1;
    CHECK(!ops.stop(&engine)); presenter=0;
}
static void compare_pixels(void) { CHECK(!memcmp(pixels,expected,sizeof(pixels))); }
static void expect_write(unsigned n,unsigned reg,unsigned width,uint32_t value)
{ CHECK(n<model.count && model.writes[n].reg==reg && model.writes[n].width==width && model.writes[n].value==value); }
static void commands(enum sis_generation generation,unsigned bpp)
{
    setup(generation,bpp); activate();
    memcpy(expected,pixels,sizeof(pixels));
    struct fb_device reference=display; reference.mapped=expected;
    presenter=1; struct fb_rect r={3,5,7,6};
    unsigned n=model.count;
    uint32_t colour=swdib_pack(&display,0x123456);
    CHECK(!ops.fill_rect(&engine,&r,colour)); swdib_fill(&reference,&r,colour); compare_pixels();
    expect_write(n,0x8206,16,bpp==16 ? 0x8000 : 0xc000);
    expect_write(n+1,0x8214,32,((generation==SIS_300 ? 0xffffu : 0x0fffu)<<16)|display.pitch);
    expect_write(n+2,0x8210,32,0); expect_write(n+3,0x821c,32,colour);
    expect_write(n+4,0x820c,32,0x00030005); expect_write(n+5,0x8218,32,0x00060007);
    expect_write(n+6,0x823c,32,0xf000|(generation==SIS_300 ? 0x30000 : bpp==16 ? 0x10000 : 0x20000));
    expect_write(n+7,0x8240,32,0);
    const uint8_t pattern[8]={0x80,0x40,0x20,0x10,8,4,2,1};
    CHECK(!ops.pattern_rect(&engine,&r,pattern,colour,0)); swdib_pattern(&reference,&r,pattern,colour,0); compare_pixels();
    for (int dy=-3;dy<=3;dy++) for (int dx=-3;dx<=3;dx++) {
        struct fb_rect source={5,5,12,12}; n=model.count;
        CHECK(!ops.copy_rect(&engine,&source,5+dx,5+dy));
        swdib_copy(&reference,&source,5+dx,5+dy); compare_pixels();
        unsigned cmd=0xcc00;
        unsigned sx=5, sy=5, tx=(unsigned)(5+dx), ty=(unsigned)(5+dy);
        if (generation==SIS_300) {
            if (dx<=0) cmd|=0x10000; else { sx+=11; tx+=11; }
            if (dy<=0) cmd|=0x20000; else { sy+=11; ty+=11; }
        } else cmd|=bpp==16 ? 0x10000 : 0x20000;
        expect_write(n+4,0x8200,32,0); expect_write(n+5,0x8218,32,0x000c000c);
        expect_write(n+6,0x8208,32,(sx<<16)|sy); expect_write(n+7,0x820c,32,(tx<<16)|ty);
        expect_write(n+8,0x823c,32,cmd); expect_write(n+9,0x8240,32,0);
    }
    /* Inversion preserves X, and running twice is an exact byte identity. */
    memcpy(expected,pixels,sizeof(pixels));
    CHECK(!sis_invert(&engine,&r));
    for (int y=r.y;y<r.y+r.height;y++) for (int x=r.x;x<r.x+r.width;x++) {
        unsigned offset=(unsigned)y*display.pitch+(unsigned)x*(bpp/8);
        uint32_t v=0; memcpy(&v,expected+offset,bpp/8); v^=bpp==16 ? 0xffff : 0xffffff;
        memcpy(expected+offset,&v,bpp/8);
    }
    compare_pixels(); CHECK(!sis_invert(&engine,&r));
    for (int y=r.y;y<r.y+r.height;y++) for (int x=r.x;x<r.x+r.width;x++) {
        unsigned offset=(unsigned)y*display.pitch+(unsigned)x*(bpp/8);
        uint32_t v=0; memcpy(&v,expected+offset,bpp/8); v^=bpp==16 ? 0xffff : 0xffffff;
        memcpy(expected+offset,&v,bpp/8);
    }
    compare_pixels();
    struct fb_rect bad={INT32_MAX,1,8,8}, empty={0,0,0,1}; n=model.count;
    CHECK(sis_fill(&engine,&bad,0)==-EOPNOTSUPP && model.count==n);
    CHECK(!sis_fill(&engine,&empty,0) && model.count==n);
    CHECK(sis_copy(&engine,&r,-1,0)==-EOPNOTSUPP && model.count==n);
    CHECK(!sis_sync(&engine) && engine.cpu_safe);
    presenter=0;
}
static void cursors(enum sis_generation generation,unsigned bpp)
{
    setup(generation,bpp); activate(); presenter=1;
    struct accel_cursor shape={.foreground=bpp==16 ? 0xf800 : 0xff0000,.background=bpp==16 ? 0x07e0 : 0x00ff00};
    shape.mask[0]=0xc0000000; shape.image[0]=0x80000001;
    engine.cursor_loaded=false;
    CHECK(sis_cursor_show(&engine,true)==-EINVAL);
    CHECK(!sis_cursor_set(&engine,&shape));
    CHECK(!engine.cursor_enabled && engine.cursor_loaded);
    CHECK(cursor_bytes[0]==0x3f && cursor_bytes[8]==0x80);
    for (unsigned y=0;y<64;y++) for (unsigned b=0;b<8;b++) {
        if (y || b) { CHECK(cursor_bytes[y*16+b]==0xff); CHECK(cursor_bytes[y*16+8+b]==0); }
    }
    CHECK(model.regs[0x8504/4]==0x00ff00 && model.regs[0x8508/4]==0xff0000);
    CHECK((model.regs[0x8500/4]&(generation==SIS_300 ? 0xffffu : 0xfffffu))==engine.cursor_offset/1024);
    CHECK(!sis_cursor_move(&engine,-7,-11));
    CHECK(model.regs[0x850c/4]==(7u<<16) && model.regs[0x8510/4]==(11u<<16));
    unsigned n=model.count; CHECK(sis_cursor_move(&engine,INT32_MIN,0)==-EOPNOTSUPP && model.count==n);
    CHECK(!sis_cursor_show(&engine,true) && engine.cursor_enabled);
    CHECK(!sis_cursor_set(&engine,&shape) && !engine.cursor_enabled);
    CHECK(!sis_cursor_show(&engine,true));
    sis_quarantine(&engine,"host_fault"); CHECK(!engine.cursor_enabled && !sis_capabilities(&engine));
    CHECK(!sis_sync(&engine) && engine.cpu_safe);
    CHECK(!sis_stop(&engine) && !engine.active && !engine.configured);
    presenter=0;
}
static void failures_and_probe(void)
{
    for (unsigned gen=0;gen<2;gen++) {
        setup(gen,32);
        unsigned reads=model.reads;
        CHECK(display_driver_bind(&display)==(gen==SIS_300 ? &sis300_driver : &sis315_driver));
        CHECK(!model.count && reads==model.reads && !claims && !maps && !probes);
        const unsigned ids300[]={0x0300,0x5300,0x6300};
        const unsigned ids315[]={0x0310,0x0315,0x0325,0x5315,0x6325,0x0330,0x6330};
        for (unsigned i=0;i<(gen ? ARRAY_SIZE(ids315) : ARRAY_SIZE(ids300));i++) {
            cards[0].device=(gen ? ids315 : ids300)[i]; CHECK(sis_pci_supported(&cards[0],gen));
            CHECK(!sis_pci_supported(&cards[0],!gen));
        }
        cards[0].device=0x6342; CHECK(!display_driver_bind(&display));
        cards[0].device=gen ? 0x0315 : 0x0300;
        cards[1]=cards[0]; card_count=2; CHECK(!eligible(&display)); card_count=1;
        display.generation++; CHECK(!eligible(&display)); display.generation--;
        display.phys++; CHECK(!eligible(&display)); display.phys--;
        g_boot.e820_count=1; g_boot.e820[0]=(struct ciuki_e820){0xd0000000,0x10000,1,0}; CHECK(!eligible(&display));
        g_boot.e820_count=0; g_boot.flags=CBI_F_SAFE_MODE; CHECK(!eligible(&display)); g_boot.flags=0;
        memcpy(g_boot.options,"accel=0",8); CHECK(!eligible(&display)); g_boot.options[0]=0;
        cards[0].bar[2]=0; CHECK(!eligible(&display)); cards[0].bar[2]=0x1001;
        display.bpp=24; CHECK(!eligible(&display)); display.bpp=32;
        CHECK(eligible(&display));
        claim_failure=true; CHECK(!map_resources() && !engine.touched); claim_failure=false;
        mapping_failure=true; CHECK(!map_resources() && !engine.touched && reg_claim<0 && port_claim<0 && scratch_claim<0); mapping_failure=false;

        setup(gen,32); activate(); presenter=1;
        model.busy=true; model.frozen=true; reads=model.reads;
        sis_pause(&engine,pause_wait,0);
        CHECK(sis_sync(&engine)==-ETIMEDOUT && model.reads-reads<=SIS_POLL_LIMIT+4 && yields);
        CHECK(engine.quarantined && !engine.cpu_safe && !sis_capabilities(&engine));
        struct fb_rect r={0,0,1,1}; unsigned n=model.count;
        CHECK(sis_fill(&engine,&r,1)==-EIO && model.count==n);
        CHECK(fallback()==&ops && !engine.cpu_safe);
        model.busy=false; CHECK(!sis_sync(&engine) && engine.cpu_safe && fallback()==0);
        CHECK(!sis_stop(&engine)); presenter=0;

        setup(gen,32); activate(); presenter=1;
        model.hang_after_fire=true;
        CHECK(sis_copy(&engine,&r,1,1)==-ETIMEDOUT && engine.quarantined && !engine.cpu_safe);
        model.busy=false; model.hang_after_fire=false; CHECK(!sis_sync(&engine));
        presenter=0;

        setup(gen,32); activate(); presenter=1;
        model.dead=true; CHECK(sis_sync(&engine)==-EIO && !engine.cpu_safe);
        presenter=0;
        setup(gen,32); activate(); presenter=1;
        model.busy=true; sis_pause(&engine,cancel_wait,0); CHECK(sis_sync(&engine)==-EINTR && !engine.cpu_safe);
        presenter=0;
        setup(gen,32); activate(); presenter=1;
        CHECK(!sis_cursor_set(&engine,&(struct accel_cursor){0}) && !sis_cursor_show(&engine,true));
        model.sticky_cursor=true; sis_quarantine(&engine,"host"); CHECK(!engine.cpu_safe);
        CHECK(sis_sync(&engine)==-EIO && !engine.cpu_safe); presenter=0;
        setup(gen,32); CHECK(!attempt_begin(&volume_fs)); model.corrupt=true;
        CHECK(!start_with_io(&display,&fake_io) && engine.quarantined && marker_size && !attempt_failed(&volume_fs));
        CHECK(!eligible(&display));
    }
    for (unsigned failure=1;failure<=6;failure++) {
        setup(SIS_315,32); fs_failure=(int)failure;
        CHECK(attempt_begin(&volume_fs)<0 && !authorized && blocked && !model.count && !probes);
        CHECK(failure==1 || destroys==1);
    }
    setup(SIS_315,32); legacy_size=9; CHECK(attempt_begin(&volume_fs)<0 && legacy_size==9 && !commits);
    setup(SIS_315,32); marker_size=9; CHECK(attempt_begin(&volume_fs)<0 && marker_size==9 && !commits);
    setup(SIS_315,32); volume.readonly=true; CHECK(attempt_begin(&volume_fs)<0 && !authorized);
    setup(SIS_315,32); CHECK(!attempt_begin(&volume_fs)); model.unlock_failed=true;
    CHECK(!start_with_io(&display,&fake_io) && !model.fires && engine.cpu_safe && marker_size);
    setup(SIS_315,32); CHECK(!attempt_begin(&volume_fs)); model.seq[0x26]=0x80;
    CHECK(!start_with_io(&display,&fake_io) && !model.fires && engine.cpu_safe);
    setup(SIS_300,32); CHECK(!attempt_begin(&volume_fs)); model.seq[0x27]=0x80;
    CHECK(!start_with_io(&display,&fake_io) && !model.fires && engine.cpu_safe);
    setup(SIS_315,32); CHECK(!attempt_begin(&volume_fs)); model.reject_queue=true;
    CHECK(!start_with_io(&display,&fake_io) && engine.quarantined && !model.fires);
}
int main(void)
{
    for (unsigned gen=0;gen<2;gen++) for (unsigned bpp=16;bpp<=32;bpp+=16) { commands(gen,bpp); cursors(gen,bpp); }
    for (unsigned gen=0;gen<2;gen++) lifecycle_and_identity(gen);
    failures_and_probe();
    printf("SiS 300 / 315 / 330: %u checks, %u failures (register sequences, pixels, ownership, markers, cursor, quarantine)\n",checks,failures);
    return failures ? 1 : 0;
}
