/* Production native ports with a register/packet execution model. The model
 * decodes wire packets independently; swdib supplies the pixel oracle.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/kernel/drivers/video/i810/driver.h"
#include "../../src/kernel/drivers/video/i740/driver.h"
#include <ciuki/swdib.h>
#include "../../src/kernel/fs/vfs.h"

#define APERTURE 0xe0000000u
#define MMIO 0xf0000000u
#define RING_OFFSET 0x80000u
#define CURSOR_OFFSET 0x100000u
#define RING_BACKING 0x900000u
#define CURSOR_BACKING 0xa00000u
#define DMA_BACKING 0x600000u
static _Alignas(4096) uint8_t dma_bytes[I810_PRIVATE_PAGES*4096u];
static unsigned dma_allocations,dma_frees,dma_pins,dma_unpins,publications;
static bool dma_alloc_fail,dma_pin_fail,gtt_restore_fail;
static uint32_t aperture_bytes=64u<<20;
struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
static struct fb_device live;
static struct pci_func gpu;
static unsigned gpu_count=1;
const struct display_driver s3savage_driver={0};
static struct {
    _Alignas(8) uint8_t registers[0x80000], vram[2*1024*1024];
    uint8_t crtc[256], xrx[256], palette[6];
    uint32_t last[12], fifo[12];
    unsigned count, packets, reads, writes, drains, index_writes, pauses;
    unsigned palette_read, palette_write;
    uint64_t now;
    bool newer, frozen, stall, dead, corrupt, hide_fail, readback_fail, stall_after;
} model;
static uint8_t reference[32*32*4];
static struct resource resources[64];
static unsigned claims, releases, maps, revokes, slots;
static unsigned map_fail_at, claim_fail_at;
static bool in_probe;
static struct vfs volume_vfs;
static struct fat_volume volume;
static size_t marker_size, legacy_size;
static unsigned commits, closes, truncates, file_writes;
static bool marker_read_fail, commit_fail, close_fail, short_write;

static uint32_t raw(unsigned bits, uint32_t reg)
{ uint32_t v=0; assert(reg+bits/8<=sizeof(model.registers)); memcpy(&v,model.registers+reg,bits/8); return v; }
static void put(unsigned bits, uint32_t reg, uint32_t v)
{ assert(reg+bits/8<=sizeof(model.registers)); memcpy(model.registers+reg,&v,bits/8); }
static int position(uint32_t address)
{ return (int)(model.newer ? address-APERTURE : address); }
static void execute(const uint32_t *p)
{
    model.packets++;
    memcpy(model.last,p,(model.newer ? 6u : 12u)*4);
    if (model.stall_after) { model.stall=true; return; }
    if (model.corrupt) return; /* ignored command must fail native DM9 */
    unsigned bytes=live.bpp/8;
    bool solid=model.newer ? p[0]==0x50000003 : !!(p[5]&0x80000);
    bool pattern=!model.newer && (p[5]&0x40000) && !solid;
    unsigned extent=model.newer ? p[2] : p[11];
    unsigned width=extent&65535, height=extent>>16;
    int dst=position(model.newer ? p[3] : p[8]);
    int src=position(model.newer ? p[5] : p[7]);
    int xstep=model.newer ? ((p[1]&(1u<<30)) ? -1 : 1) : ((p[5]&0x100) ? -1 : 1);
    int pitch=model.newer ? (int16_t)p[1] : ((p[5]&0x200) ? -(int)live.pitch : (int)live.pitch);
    assert(width && width<=live.pitch && height<=live.height);
    if (model.newer) {
        assert((p[1]&(3u<<24))==(live.bpp==16 ? 1u<<24 : 2u<<24));
        assert(p[1]&(1u<<26));
        assert(solid ? p[0]==0x50000003 && p[5]==0 : p[0]==0x50c00004 && (p[4]&65535)==(p[1]&65535));
        assert(((p[1]>>16)&255)==(solid ? 0xf0 : 0xcc));
    } else {
        assert(p[0]==0x6000000a && p[1]==(live.pitch<<16|live.pitch));
        assert(!p[4] && !p[9] && !p[10]);
        assert(!(p[5]&0x1400)); /* no host-data or mono-source command */
        assert((p[5]&255)==(solid || pattern ? 0xf0 : 0xcc));
    }
    for (unsigned y=0;y<height;y++) for (unsigned x=0;x<width;x++) {
        int where=dst+(int)y*pitch+(int)x*xstep;
        assert(where>=0 && (unsigned)where<live.size);
        if (solid || pattern) {
            uint32_t colour=model.newer ? p[4] : p[2];
            if (pattern) {
                unsigned row=(unsigned)where/live.pitch, col=((unsigned)where%live.pitch)/bytes;
                assert(p[6]==CURSOR_OFFSET+1024);
                colour=model.vram[p[6]+(row&7)]&(0x80u>>(col&7)) ? p[3] : p[2];
                assert(((p[5]>>20)&7)==((unsigned)dst/live.pitch&7));
            }
            model.vram[where]=(uint8_t)(colour>>(x%bytes)*8);
        } else {
            int from=src+(int)y*pitch+(int)x*xstep;
            assert(from>=0 && (unsigned)from<live.size);
            model.vram[where]=model.vram[from];
        }
    }
}
static uint32_t read_reg(void *ctx, unsigned bits, uint32_t reg)
{
    (void)ctx; model.reads++;
    if (model.dead) return bits==8 ? 255 : bits==16 ? 65535 : UINT32_MAX;
    if (reg==0x3c9) { assert(model.palette_read<6); return model.palette[model.palette_read++]; }
    if (model.stall) {
        if (model.newer && reg==0x2090) return 0;
        if (!model.newer && reg==0x40010) return 0x80000000;
    }
    return raw(bits,reg);
}
static void write_reg(void *ctx, unsigned bits, uint32_t reg, uint32_t value)
{
    (void)ctx; model.writes++;
    if (reg==0x3c7 && value==4) model.palette_read=0;
    if (reg==0x3c8 && value==4) model.palette_write=0;
    if (reg==0x3c9) { assert(model.palette_write<6); model.palette[model.palette_write++]=(uint8_t)value; return; }
    if (model.hide_fail && model.newer && reg==0x70008 && !(value&0x1000)) return;
    if (model.readback_fail && (reg==0x70084 || reg==0x70088 || reg==0x204c)) return;
    if (gtt_restore_fail && reg>=0x10000 && reg<0x20000 && !(value&1u)) return;
    put(bits,reg,value);
    if (model.newer && reg==0x2040 && (raw(32,0x204c)&1) && !model.stall) {
        uint32_t packet[6], head=raw(32,0x2044)&0x1ffffc, size=(raw(32,0x204c)&0x1ff000)+4096;
        if (head==value) return;
        uint32_t start=raw(32,0x2048), offset=start-APERTURE;
        const uint8_t *ring=model.vram+RING_OFFSET;
        if (offset!=RING_OFFSET) {
            assert(offset==aperture_bytes-I810_PRIVATE_PAGES*4096u);
            assert(raw(32,0x10000+offset/1024)==(DMA_BACKING|1u));
            assert(dma_pins>dma_unpins && publications);
            ring=dma_bytes;
        }
        for (unsigned i=0;i<6;i++) memcpy(packet+i,ring+((head+i*4)&(size-1)),4);
        execute(packet);
        if (!model.stall) put(32,0x2044,value);
    }
    if (!model.newer && reg==0x1000) {
        assert(model.count<12);
        model.fifo[model.count++]=value;
        if (model.count==12) { execute(model.fifo); model.count=0; }
    }
}
static uint8_t read_index(void *ctx, uint16_t port, uint8_t index)
{
    (void)ctx; model.reads++;
    if (model.dead) return 255;
    assert(port==0x3d4 || port==0x3d6);
    uint8_t value=port==0x3d4 ? model.crtc[index] : model.xrx[index];
    if (model.stall && port==0x3d6 && index==0x20) value|=1;
    return value;
}
static void write_index(void *ctx, uint16_t port, uint8_t index, uint8_t value)
{
    (void)ctx; assert(port==0x3d6); model.index_writes++;
    if (model.hide_fail && index==0x80 && !(value&0x10)) return;
    if (model.readback_fail && index>=0xa0) return;
    model.xrx[index]=value;
}
static uint64_t now(void *ctx)
{ (void)ctx; if (!model.frozen) model.now+=25; return model.now; }
static void drain(void *ctx) { (void)ctx; model.drains++; }
static struct video_io fake_io(void)
{ return (struct video_io){&model,read_reg,write_reg,read_index,write_index,now,drain}; }
void video_hardware_io(struct video_native *s, void *mapped)
{ assert(mapped==model.registers); s->io=fake_io(); }
void klog(const char *fmt, ...) { (void)fmt; assert(!in_probe); }
const struct fb_device *fbdev_get(void) { return &live; }
unsigned fbdev_native_capabilities(void) { return 0; }
bool fbdev_cpu_ready(void) { return true; }
int fbdev_accel_probe(const struct fb_device *fb, int (*probe)(void *), void *ctx)
{ assert(fb==&live && !in_probe); in_probe=true; int e=probe(ctx); in_probe=false; return e; }
unsigned pci_count(void) { return gpu_count; }
const struct pci_func *pci_get(unsigned i) { return i<gpu_count ? &gpu : 0; }
int registry_claim(enum res_type type, uint32_t start, uint32_t end, const char *owner, bool share)
{
    assert(!in_probe && !share); claims++;
    if (claims==claim_fail_at) return -EBUSY;
    for (unsigned i=0;i<slots;i++) if (resources[i].type==type && resources[i].state!=RS_RELEASED &&
        start<resources[i].end && end>resources[i].start) return -EBUSY;
    assert(slots<64);
    unsigned n=slots++;
    resources[n]=(struct resource){type,start,end,owner,n+1,RS_CLAIMED,false};
    return (int)n;
}
const struct resource *registry_get(unsigned i) { return i<slots ? &resources[i] : 0; }
bool registry_valid(int h, gen_t g)
{ return h>=0 && (unsigned)h<slots && resources[h].generation==g && resources[h].state!=RS_RELEASED && resources[h].state!=RS_QUARANTINED; }
int registry_activate(int h, gen_t g) { assert(registry_valid(h,g)); resources[h].state=RS_ACTIVE; return 0; }
int registry_quiesce(int h, gen_t g, registry_idle_proof_fn proof)
{ assert(registry_valid(h,g) && proof(h,g)); resources[h].state=RS_QUIESCING; return 0; }
int registry_release(int h, gen_t g)
{ assert(registry_valid(h,g)); resources[h].state=RS_RELEASED; releases++; return 0; }
int registry_quarantine(int h, gen_t g)
{ assert(registry_valid(h,g)); resources[h].state=RS_QUARANTINED; return 0; }
void *vmm_reserve_mmio(uint32_t bytes) { assert(bytes); return (void *)(uintptr_t)1; }
void *vmm_replace_mmio(void *window, uint32_t capacity, uint32_t phys, uint32_t bytes)
{
    assert(window && capacity==bytes); maps++;
    if (maps==map_fail_at) return 0;
    if (phys==MMIO && bytes==0x80000) return model.registers;
    if (phys==RING_BACKING) return model.vram+RING_OFFSET;
    if (phys==CURSOR_BACKING && bytes==4096) return model.vram+CURSOR_OFFSET;
    assert(phys>=APERTURE && (uint64_t)phys+bytes<=APERTURE+sizeof(model.vram));
    return model.vram+(phys-APERTURE);
}
void vmm_revoke_mmio(void *p, uint32_t bytes) { assert(p && bytes && !in_probe); revokes++; }
int vfs_table_init(struct vfs *v, struct vfs_table *t, unsigned drive)
{ assert(!in_probe && v==&volume_vfs && drive==2); memset(t,0,sizeof(*t)); return 0; }
void vfs_table_destroy(struct vfs_table *t) { (void)t; assert(!in_probe); }
int vfs_stat(struct vfs_table *t, const char *path, struct fat_entry *entry)
{
    (void)t; assert(!in_probe);
    if (marker_read_fail) return -FS_EIO;
    entry->size=strstr(path,"/accel.cfg") ? legacy_size : marker_size;
    return 0;
}
int vfs_open(struct vfs_table *t, const char *p, unsigned flags, enum vfs_share share, uint8_t attr)
{ (void)t; (void)attr; assert(!in_probe && flags==(VFS_WRITE|VFS_CREATE) && share==VFS_DENY_ALL && !strstr(p,"/accel.cfg")); return 0; }
int vfs_write(struct vfs_table *t, int fd, const void *p, size_t n, size_t *done)
{ (void)t; (void)fd; assert(!in_probe && p && n); file_writes++; *done=n-(short_write ? 1 : 0); marker_size=*done; return 0; }
int vfs_truncate(struct vfs_table *t, int fd, uint64_t size)
{ (void)t; (void)fd; assert(!in_probe && !size); truncates++; marker_size=0; return 0; }
int vfs_commit(struct vfs_table *t, int fd)
{ (void)t; (void)fd; assert(!in_probe); commits++; return commit_fail ? -FS_EIO : 0; }
int vfs_close(struct vfs_table *t, int fd)
{ (void)t; (void)fd; assert(!in_probe); closes++; return close_fail ? -FS_EIO : 0; }

void *i810_dma_allocate(uint32_t *physical)
{
    assert(!in_probe); dma_allocations++;
    *physical=dma_alloc_fail ? 0 : DMA_BACKING;
    return *physical ? dma_bytes : 0;
}
void i810_dma_free(uint32_t physical)
{ assert(physical==DMA_BACKING && dma_pins==dma_unpins); dma_frees++; }
void i810_dma_publish(void) { publications++; }
uint32_t i810_aperture_bytes(const struct pci_func *p)
{ assert(p==&gpu); return aperture_bytes; }
int mm_pin(struct mm_pin *pin)
{
    assert(!in_probe && pin->first_pfn==DMA_BACKING/4096u &&
        pin->pages==I810_PRIVATE_PAGES && pin->direction==MM_PIN_BIDIRECTIONAL);
    if (dma_pin_fail) return -ENOMEM;
    dma_pins++; pin->ticket=dma_pins; return 0;
}
int mm_unpin(struct mm_pin *pin,uint32_t generation)
{ assert(pin->ticket && pin->generation==generation); dma_unpins++; pin->ticket=0; return 0; }
static void setup(bool newer, unsigned bpp)
{
    memset(&model,0,sizeof(model)); model.newer=newer;
    dma_allocations=dma_frees=dma_pins=dma_unpins=publications=0;
    dma_alloc_fail=dma_pin_fail=gtt_restore_fail=false; aperture_bytes=64u<<20;
    memset(dma_bytes,0xa5,sizeof(dma_bytes));
    memset(&g_boot,0,sizeof(g_boot));
    g_boot.kernel_start=0x100000; g_boot.kernel_end=0x200000;
    g_boot.e820_count=2;
    g_boot.e820[0]=(struct ciuki_e820){.base=0x100000,.length=0x700000,.type=CBI_E820_RAM};
    g_boot.e820[1]=(struct ciuki_e820){.base=0x800000,.length=0x800000,.type=2};
    g_boot.display.vram_bytes=sizeof(model.vram);
    live=(struct fb_device){.present=true,.width=32,.height=32,.pitch=32*(bpp/8),.size=32*32*(bpp/8),
        .phys=APERTURE,.mapped=model.vram,.bpp=(uint8_t)bpp,.red_size=bpp==16 ? 5 : 8,
        .red_pos=bpp==16 ? 11 : 16,.green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,
        .blue_size=bpp==16 ? 5 : 8,.blue_pos=0,.handle=0,.generation=1};
    gpu=(struct pci_func){.vendor=0x8086,.device=newer ? 0x7121 : 0x7800,.class_code=3,
        .command=newer ? 7 : 3,.bar={APERTURE,MMIO}};
    gpu_count=1; memset(resources,0,sizeof(resources)); slots=1;
    resources[0]=(struct resource){RES_MMIO,live.phys,live.phys+live.size,"fbdev",1,RS_ACTIVE,false};
    claims=releases=maps=revokes=map_fail_at=claim_fail_at=0;
    commits=closes=truncates=file_writes=0; marker_size=legacy_size=0;
    marker_read_fail=commit_fail=close_fail=short_write=false;
    memset(&volume_vfs,0,sizeof(volume_vfs)); memset(&volume,0,sizeof(volume));
    volume.mounted=true; volume_vfs.volumes[2]=&volume;
    model.crtc[0x13]=(uint8_t)(live.pitch/8);
    model.xrx[0x80]=0x80; model.xrx[0x81]=bpp==16 ? 5 : 6; model.xrx[0x20]=bpp==16 ? 0x10 : 0x20;
    model.xrx[0xa3]=CURSOR_OFFSET>>16;
    put(32,0x2048,APERTURE+RING_OFFSET); put(16,0x2090,0x7b);
    put(32,0x2020,0x800001); put(32,0x2080,0xc00000);
    put(32,0x10000+(RING_OFFSET/4096)*4,RING_BACKING|1);
    put(32,0x10000+(CURSOR_OFFSET/4096)*4,CURSOR_BACKING|1);
    put(32,0x10000,0xb00001);
    for (unsigned top=8192;top<=16384;top+=8192)
        for (unsigned i=0;i<I810_PRIVATE_PAGES;i++)
            put(32,0x10000+(top-I810_PRIVATE_PAGES+i)*4,0x1234000u+i*4096u);
    put(32,0x70008,(bpp==16 ? 5u : 6u)<<16|0x8000);
    put(32,0x70084,CURSOR_BACKING); put(8,0x3c6,0xe7); put(8,0x3c8,12);
    for (unsigned i=0;i<6;i++) model.palette[i]=(uint8_t)(17+i);
    for (unsigned i=0;i<sizeof(model.vram);i++) model.vram[i]=(uint8_t)(i*31+i/7);
    memcpy(reference,model.vram,live.size);
}
static struct fb_device refdev(void)
{ struct fb_device d=live; d.mapped=reference; return d; }
static void compare(void) { assert(!memcmp(model.vram,reference,live.size)); }
static int pause_wait(void *ctx) { assert(ctx==&model); model.pauses++; return 0; }
static int cancel_wait(void *ctx) { assert(ctx==&model); return -EINTR; }
static bool cancel(void *ctx) { assert(ctx==&model); return true; }
static void make_core(bool newer, struct i810_engine *a, struct i740_engine *b)
{
    if (newer) {
        memset(a,0,sizeof(*a)); a->native=(struct video_native){.fb=live,.io=fake_io(),.cpu_safe=true};
        a->ring=model.vram+RING_OFFSET; a->ring_phys=APERTURE+RING_OFFSET; a->ring_bytes=4096;
        a->cursor=model.vram+CURSOR_OFFSET; a->cursor_phys=CURSOR_BACKING;
        assert(!i810_engine_prepare(a));
    } else {
        memset(b,0,sizeof(*b)); b->native=(struct video_native){.fb=live,.io=fake_io(),.cpu_safe=true};
        b->cursor=model.vram+CURSOR_OFFSET; b->cursor_offset=CURSOR_OFFSET;
        assert(!i740_engine_prepare(b));
    }
}
static void core_tests(bool newer)
{
    static struct i810_engine a;
    static struct i740_engine b;
    for (unsigned bpp=16;bpp<=24;bpp+=8) {
        setup(newer,bpp); make_core(newer,&a,&b);
        struct video_native *s=newer ? &a.native : &b.native;
        const struct accel_ops *ops=&s->ops;
        struct fb_device d=refdev();
        assert(ops->capabilities(s)==3 && !ops->write_rect && !ops->offscreen);
        assert(!ops->sync(s) && ops->cpu_ready(s));
        for (unsigned i=0;i<100;i++) {
            struct fb_rect r={(int)(i*7%16),(int)(i*3%16),1+(int)(i*11%16),1+(int)(i*5%16)};
            uint32_t colour=swdib_pack(&live,i*72931);
            swdib_fill(&d,&r,colour); assert(!ops->fill_rect(s,&r,colour)); compare();
        }
        for (unsigned direction=0;direction<4;direction++) {
            struct fb_rect src={(direction&1) ? 4 : 0,(direction&2) ? 4 : 0,20,20};
            int x=(direction&1) ? 0 : 4, y=(direction&2) ? 0 : 4;
            swdib_copy(&d,&src,x,y); assert(!ops->copy_rect(s,&src,x,y)); compare();
            if (newer) {
                assert(!!(model.last[1]&(1u<<30))==!(direction&1));
                assert(((int16_t)model.last[1]<0)==!(direction&2));
            } else assert((model.last[5]&0x300)==((direction&1 ? 0u : 0x100u)|(direction&2 ? 0u : 0x200u)));
        }
        if (!newer) {
            uint8_t p[8]={0x81,0x42,0x24,0x18,0xa5,0x5a,0x3c,0xc3};
            struct fb_rect r={3,5,21,19};
            uint32_t fg=swdib_pack(&live,0x347bac), bg=swdib_pack(&live,0xc32167);
            swdib_pattern(&d,&r,p,fg,bg); assert(!ops->pattern_rect(s,&r,p,fg,bg)); compare();
        } else {
            /* Six dwords wrap across a 4-KiB ring without overwriting a guard. */
            a.tail=4080; put(32,0x2040,4080); put(32,0x2044,4080);
            struct fb_rect r={0,0,2,2}; swdib_fill(&d,&r,0x1234);
            assert(!ops->fill_rect(s,&r,0x1234) && a.tail==8); compare();
        }
        unsigned packets=model.packets, writes=model.writes;
        struct fb_rect bad={-1,0,2,2}, good={1,1,8,8};
        assert(ops->fill_rect(s,&bad,0)==-EOPNOTSUPP);
        assert(ops->copy_rect(s,&good,31,31)==-EOPNOTSUPP);
        assert(ops->copy_rect(s,NULL,0,0)==-EOPNOTSUPP);
        assert(model.packets==packets && model.writes==writes); compare();
        assert(ops->cursor_move(s,0,0)==-EINVAL && ops->cursor_show(s,true)==-EINVAL);
        struct accel_cursor cursor={.foreground=swdib_pack(&live,0xf79421),.background=swdib_pack(&live,0x236bad)};
        cursor.mask[0]=0xe0000000; cursor.image[0]=0xb0000000; /* image outside mask is ignored */
        cursor.mask[31]=1; cursor.image[31]=1;
        assert(!ops->cursor_set(s,&cursor)); compare();
        const uint8_t *image=model.vram+CURSOR_OFFSET;
        assert(image[0]==0x1f && image[8]==0xa0 && image[3]==255 && image[11]==0);
        assert(image[31*16+3]==0xfe && image[31*16+11]==1);
        assert(image[32*16]==255 && image[32*16+8]==0);
        uint32_t rgb=video_cursor_rgb(&live,cursor.foreground);
        assert(model.palette[3]==(rgb>>16) && model.palette[4]==((rgb>>8)&255) && model.palette[5]==(rgb&255));
        assert(!ops->cursor_move(s,-17,1024));
        if (newer) assert(raw(32,0x70088)==0x04008011);
        else assert(model.xrx[0xa4]==17 && model.xrx[0xa5]==0x80 && model.xrx[0xa6]==0 && model.xrx[0xa7]==4);
        writes=model.writes+model.index_writes;
        assert(ops->cursor_move(s,INT32_MIN,0)==-EOPNOTSUPP);
        assert(ops->cursor_move(s,2048,0)==-EOPNOTSUPP);
        assert(writes==model.writes+model.index_writes);
        assert(!ops->cursor_show(s,true)); assert(!ops->cursor_set(s,&cursor));
        assert(newer ? !(raw(32,0x70008)&0x1000) : !(model.xrx[0x80]&0x10));
        assert(video_cursor_test(ops,100,cancel,&model)==-EINTR);
        assert(!video_cursor_test(ops,100,NULL,NULL));
        assert(!ops->stop(s) && ops->cpu_ready(s) && !ops->capabilities(s));
        for (unsigned i=0;i<4096;i++) assert(model.vram[CURSOR_OFFSET+i]==(uint8_t)((CURSOR_OFFSET+i)*31+(CURSOR_OFFSET+i)/7));
        for (unsigned i=0;i<6;i++) assert(model.palette[i]==17+i);
        assert(raw(8,0x3c6)==0xe7 && raw(8,0x3c8)==12);
        if (newer) for (unsigned i=0;i<4096;i++) assert(model.vram[RING_OFFSET+i]==(uint8_t)((RING_OFFSET+i)*31+(RING_OFFSET+i)/7));
    }
}
static void timeout_tests(bool newer, const char *kind)
{
    static struct i810_engine a; static struct i740_engine b;
    setup(newer,16); make_core(newer,&a,&b);
    struct video_native *s=newer ? &a.native : &b.native;
    const struct accel_ops *ops=&s->ops;
    ops->set_pause(s,pause_wait,&model);
    if (!strcmp(kind,"dead")) model.dead=true;
    else if (!strcmp(kind,"cursor-fault")) {
        struct accel_cursor c={0}; assert(!ops->cursor_set(s,&c)); assert(!ops->cursor_show(s,true));
        model.hide_fail=true;
        assert(ops->cursor_show(s,false)==-EIO && s->quarantined && !ops->cpu_ready(s));
        assert(ops->sync(s)==-EIO && !ops->cpu_ready(s));
        model.hide_fail=false; assert(!ops->sync(s) && ops->cpu_ready(s)); return;
    } else if (!strcmp(kind,"submitted")) {
        model.stall_after=true;
        struct fb_rect r={0,0,8,8};
        assert(ops->copy_rect(s,&r,4,4)==-ETIMEDOUT);
        assert(model.packets==1 && s->quarantined && !ops->cpu_ready(s));
        unsigned packets=model.packets;
        assert(ops->copy_rect(s,&r,0,0)==-EIO && model.packets==packets);
        assert(ops->stop(s)==-ETIMEDOUT && s->touched); /* retain access guard */
        model.stall=model.stall_after=false;
        if (newer) put(32,0x2044,raw(32,0x2040));
        assert(!ops->sync(s) && ops->cpu_ready(s) && s->quarantined && !ops->capabilities(s));
        assert(model.packets==packets && !ops->stop(s)); return;
    } else {
        model.stall=true; model.frozen=!strcmp(kind,"frozen");
        if (!strcmp(kind,"cancel")) ops->set_pause(s,cancel_wait,&model);
    }
    int result=ops->sync(s);
    assert(result==(!strcmp(kind,"dead") ? -EIO : !strcmp(kind,"cancel") ? -EINTR : -ETIMEDOUT));
    assert(s->quarantined && !ops->cpu_ready(s) && !ops->capabilities(s));
    if (model.frozen) assert(model.reads<VIDEO_WAIT_POLLS*8);
    if (strcmp(kind,"dead") && strcmp(kind,"cancel")) assert(model.pauses);
    model.dead=model.stall=false;
    assert(!ops->sync(s) && ops->cpu_ready(s) && s->quarantined);
    assert(!ops->stop(s));
}
static void reservation_tests(void)
{
    setup(true,16);
    struct i810_engine e={.native={.fb=live,.io=fake_io()}};
    uint32_t phys;
    assert(i810_gtt_region(&e,RING_OFFSET,4096,&phys) && phys==RING_BACKING);
    unsigned writes=model.writes;
    put(32,0x10000+12,RING_BACKING|1); assert(!i810_gtt_region(&e,RING_OFFSET,4096,&phys)); put(32,0x10000+12,0);
    put(32,0x10000+RING_OFFSET/1024,0x700001); assert(!i810_gtt_region(&e,RING_OFFSET,4096,&phys));
    put(32,0x10000+RING_OFFSET/1024,0x800001); assert(!i810_gtt_region(&e,RING_OFFSET,4096,&phys));
    put(32,0x10000+RING_OFFSET/1024,0xc00001); assert(!i810_gtt_region(&e,RING_OFFSET,4096,&phys));
    put(32,0x10000+RING_OFFSET/1024,RING_BACKING|3); assert(!i810_gtt_region(&e,RING_OFFSET,4096,&phys));
    assert(!i810_gtt_region(&e,UINT32_MAX-4095,8192,&phys));
    assert(!i810_gtt_region(&e,RING_OFFSET+1,4096,&phys)); assert(model.writes==writes);
    assert(!video_reserved(UINT32_MAX-100,4096));
    assert(!video_reserved(g_boot.kernel_start,4096));
}
static const struct display_driver *bind_driver(bool newer)
{
    const struct display_driver *driver=display_driver_bind(&live);
    assert(driver==(newer ? &i810_driver : &i740_driver)); return driver;
}
static void production_tests(bool newer, const char *kind)
{
    setup(newer,24);
    const struct display_driver *d=bind_driver(newer);
    assert(!model.reads && !model.writes && !claims); /* read-only nomination */
    assert(d->eligible(&live));
    if (!strcmp(kind,"safe")) g_boot.flags|=CBI_F_SAFE_MODE;
    if (!strcmp(kind,"disabled")) strcpy(g_boot.options,"foo accel=0 bar");
    if (!strcmp(kind,"map-refusal")) map_fail_at=1;
    if (!strcmp(kind,"claim-refusal")) claim_fail_at=1;
    if (!strcmp(kind,"pitch-refusal")) model.crtc[0x13]++;
    if (!strcmp(kind,"offset-refusal")) { if (newer) put(32,0x70020,4); else model.crtc[0x0d]=1; }
    if (!strcmp(kind,"decode-refusal")) gpu.command=1;
    if (!strcmp(kind,"master-refusal")) gpu.command&=~4u;
    if (!strcmp(kind,"owner-refusal")) resources[0].owner="another-owner";
    if (!strcmp(kind,"ram-refusal")) g_boot.e820[0].base=MMIO;
    if (!strcmp(kind,"cursor-busy-refusal")) {
        if (newer) put(32,0x70008,raw(32,0x70008)|0x1000);
        else model.xrx[0x80]|=0x10;
    }
    if (!strcmp(kind,"ring-refusal")) put(32,0x204c,1);
    if (!strcmp(kind,"full-gtt-refusal")) put(32,0x10000+16383*4,0xb10001);
    if (!strcmp(kind,"beyond-aperture-refusal")) {
        aperture_bytes=32u<<20;
        put(32,0x10000+12000*4,0xb10001);
    }
    if (!strcmp(kind,"alloc-refusal")) dma_alloc_fail=true;
    if (!strcmp(kind,"pin-refusal")) dma_pin_fail=true;
    if (!strcmp(kind,"aperture-refusal")) aperture_bytes=0;
    if (!strcmp(kind,"aperture32")) aperture_bytes=32u<<20;
    if (!strcmp(kind,"firmware-ring-free")) put(32,0x2048,0);
    if (!strcmp(kind,"gtt-visible-refusal")) put(32,0x10000,g_boot.kernel_start|1);
    if (!strcmp(kind,"gtt-alias-refusal")) put(32,0x10000+12,DMA_BACKING|1);
    if (!strcmp(kind,"fence-refusal")) put(32,0x2000,1);
    if (!strcmp(kind,"probe-timeout")) model.stall_after=true;
    if (!strcmp(kind,"no-cursor")) { if (newer) put(32,0x70084,0); else model.xrx[0xa3]=0; }
    if (!strcmp(kind,"comparison")) model.corrupt=true;
    if (!strcmp(kind,"safe") || !strcmp(kind,"disabled")) {
        assert(!d->eligible(&live)); assert(d->attempt_begin(&volume_vfs));
        assert(!d->start(&live,NULL) && !claims && !model.writes && !file_writes); return;
    }
    assert(!d->attempt_begin(&volume_vfs) && marker_size && commits==1 && closes==1);
    uint8_t firmware_gtt[65536],firmware_ring[65536];
    uint32_t firmware_registers[4];
    memcpy(firmware_gtt,model.registers+0x10000,sizeof(firmware_gtt));
    memcpy(firmware_ring,model.vram+RING_OFFSET,sizeof(firmware_ring));
    for (unsigned i=0;i<4;i++) firmware_registers[i]=raw(32,0x2040+i*4);
    const struct accel_ops *ops=d->start(&live,NULL);
    if (!strcmp(kind,"probe-timeout")) {
        assert(!ops && marker_size && !releases);
        struct ciuki_accel_info info; d->snapshot(&info); assert(info.quarantined);
        const struct accel_ops *guard=d->fallback(); assert(guard && !guard->cpu_ready(guard->ctx));
        d->record(); assert(!releases && !revokes);
        model.stall=model.stall_after=false;
        if (newer) put(32,0x2044,raw(32,0x2040));
        guard=d->fallback(); assert(guard && guard->cpu_ready(guard->ctx));
        assert(!guard->stop(guard->ctx) && releases==claims && revokes==maps);
        assert(marker_size && !truncates); return;
    }
    if ((strstr(kind,"refusal") && strcmp(kind,"restore-refusal")) || !strcmp(kind,"comparison")) {
        assert(!ops);
        if (!strcmp(kind,"comparison")) { struct ciuki_accel_info info; d->snapshot(&info); assert(info.quarantined); }
        assert(!d->attempt_failed(&volume_vfs) && marker_size && !truncates);
        const struct accel_ops *guard=d->fallback(); assert(!guard || guard->cpu_ready(guard->ctx));
        assert(releases==claims-(claim_fail_at ? 1 : 0)); return;
    }
    assert(ops && model.packets>=4 && !memcmp(reference,model.vram,live.size)); /* saved pixels restored */
    if (newer && !strcmp(kind,"lease-loss")) {
        unsigned writes=model.writes,reads=model.reads;
        resources[0].state=RS_QUARANTINED;
        struct fb_rect r={0,0,4,4};
        assert(ops->fill_rect(ops->ctx,&r,0x1234)==-EIO);
        assert(!ops->cpu_ready(ops->ctx) && !ops->capabilities(ops->ctx));
        assert(model.writes==writes && model.reads==reads);
        assert(!dma_frees && !dma_unpins && !releases && marker_size);
        return;
    }
    assert(ops->capabilities(ops->ctx)==(!strcmp(kind,"no-cursor") ? 1u : 3u));
    if (!strcmp(kind,"no-cursor")) assert(ops->cursor_set(ops->ctx,&(struct accel_cursor){0})==-EOPNOTSUPP);
    assert(!d->attempt_end(&volume_vfs) && !marker_size && commits==2 && truncates==1);
    bool ready,used; const char *why; assert(d->summary(&ready,&used,&why) && ready && !strcmp(why,"active"));
    struct ciuki_display_engine_info info; display_driver_info(&info);
    assert(!strcmp(info.driver_id,d->id) && !strcmp(info.display_name,d->name));
    struct ciuki_accel_info legacy; d->snapshot(&legacy); assert(!legacy.driver && legacy.generation==live.generation);
    if (newer) {
        assert(dma_allocations==1 && dma_pins==1 && !dma_frees && !dma_unpins);
        uint32_t first=aperture_bytes/4096u-I810_PRIVATE_PAGES;
        for (unsigned i=0;i<I810_PRIVATE_PAGES;i++)
            assert(raw(32,0x10000+(first+i)*4)==((DMA_BACKING+i*4096u)|1u));
        assert(!memcmp(firmware_ring,model.vram+RING_OFFSET,sizeof(firmware_ring)));
        if (!strcmp(kind,"restore-refusal")) {
            gtt_restore_fail=true;
            assert(ops->stop(ops->ctx) && !ops->cpu_ready(ops->ctx));
            assert(!dma_frees && !dma_unpins && !releases && marker_size==0);
            return;
        }
    }
    d->quarantine("test_fault"); assert(!ops->capabilities(ops->ctx));
    const struct accel_ops *guard=d->fallback(); assert(guard && guard->cpu_ready(guard->ctx));
    assert(!guard->stop(guard->ctx) && releases==claims && revokes==maps);
    if (newer) {
        assert(dma_frees==1 && dma_unpins==1);
        assert(!memcmp(firmware_gtt,model.registers+0x10000,sizeof(firmware_gtt)));
        assert(!memcmp(firmware_ring,model.vram+RING_OFFSET,sizeof(firmware_ring)));
        for (unsigned i=0;i<4;i++) assert(raw(32,0x2040+i*4)==firmware_registers[i]);
    }
    d->record(); assert(releases==claims && revokes==maps);
    assert(!d->eligible(&live) && !d->start(&live,NULL));
}
static void marker_tests(bool newer, const char *kind)
{
    setup(newer,16); const struct display_driver *d=bind_driver(newer);
    if (!strcmp(kind,"legacy-marker")) legacy_size=8;
    else if (!strcmp(kind,"own-marker")) marker_size=8;
    else if (!strcmp(kind,"read-marker")) marker_read_fail=true;
    else if (!strcmp(kind,"commit-marker")) commit_fail=true;
    else if (!strcmp(kind,"close-marker")) close_fail=true;
    else if (!strcmp(kind,"short-marker")) short_write=true;
    else if (!strcmp(kind,"readonly-marker")) volume.readonly=true;
    assert(d->attempt_begin(&volume_vfs));
    assert(!d->start(&live,NULL) && !model.writes && !claims && !truncates);
    if (!strcmp(kind,"legacy-marker")) assert(legacy_size==8 && !file_writes);
    if (!strcmp(kind,"own-marker")) assert(marker_size==8 && !file_writes);
    assert(d->attempt_begin(&volume_vfs)); /* same-boot attempt never retries */
}
static void nomination_tests(bool newer)
{
    setup(newer,16); const struct display_driver *d=bind_driver(newer);
    unsigned count=d->match_count;
    for (unsigned i=0;i<count;i++) { gpu.device=d->matches[i].device; assert(display_driver_bind(&live)==d); }
    gpu.device=0x2582; assert(!display_driver_bind(&live));
    gpu.device=d->matches[0].device; gpu_count=2; assert(!display_driver_bind(&live)); gpu_count=1;
    gpu.bar[0]+=4096; assert(!display_driver_bind(&live)); gpu.bar[0]-=4096;
    gpu.command=2; assert(!display_driver_bind(&live)); gpu.command=newer ? 7 : 3;
    if (newer) {
        assert(display_driver_bind(&live)==d);
        gpu.command=3; assert(!d->eligible(&live)); gpu.command=7;
    }
    gpu.header_type=1; assert(!display_driver_bind(&live)); gpu.header_type=0;
    resources[0].state=RS_QUARANTINED; assert(!display_driver_bind(&live)); resources[0].state=RS_ACTIVE;
    g_boot.e820[0].base=APERTURE; assert(!display_driver_bind(&live));
    assert(!model.reads && !model.writes && !claims);
    setup(newer,32); assert(!d->eligible(&live));
}
int main(int argc, char **argv)
{
    assert(argc==3); bool newer=!strcmp(argv[1],"i810"); const char *test=argv[2];
    if (!strcmp(test,"core")) core_tests(newer);
    else if (!strcmp(test,"nomination")) nomination_tests(newer);
    else if (!strcmp(test,"reservation")) reservation_tests();
    else if (strstr(test,"marker")) marker_tests(newer,test);
    else if (!strcmp(test,"timeout") || !strcmp(test,"frozen") || !strcmp(test,"cancel") ||
        !strcmp(test,"dead") || !strcmp(test,"submitted") || !strcmp(test,"cursor-fault")) timeout_tests(newer,test);
    else production_tests(newer,test);
    printf("%s %s PASS\n",argv[1],test); return 0;
}
