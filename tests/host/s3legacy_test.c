/* Independent register decoder and software pixel comparisons for f2-118.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/kernel/drivers/video/s3legacy/private.h"
#include <ciuki/swdib.h>
#include "../../src/kernel/fs/vfs.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
extern const struct display_driver s3trio64_driver,s3virge_driver;
const struct display_driver s3savage_driver={0};
static struct pci_func gpu[2];
static unsigned gpu_count=1;
static struct resource lease;
static struct fb_device fb;
static unsigned locks, probes, launches, access_calls, releases;
static bool access_fail, corrupt, previous, readonly, marker_fail, short_write, probe_busy;
static unsigned marker_commits,marker_writes,marker_clears;
static struct fat_volume volume;
static struct vfs vfs;
unsigned pci_count(void) { return gpu_count; }
const struct pci_func *pci_get(unsigned n) { return n<gpu_count ? &gpu[n] : 0; }
const struct resource *registry_get(unsigned n) { return n==0 ? &lease : 0; }
bool registry_valid(int h,gen_t g) { return h==0 && g==lease.generation && lease.state==RS_ACTIVE; }
const struct fb_device *fbdev_get(void) { return &fb; }
unsigned fbdev_native_capabilities(void) { return 1; }
bool fbdev_cpu_ready(void) { return true; }
void klog(const char *fmt,...) { (void)fmt; CHECK(!locks); }
int fbdev_accel_probe(const struct fb_device *f,int (*run)(void *),void *ctx)
{
    CHECK(!locks && f==&fb); probes++;
    if (probe_busy) return -EBUSY;
    locks++;
    int e=run(ctx);
    locks--;
    return e;
}
int vfs_table_init(struct vfs *v,struct vfs_table *t,unsigned d)
{ (void)t; CHECK(!locks && v==&vfs && d==2); return 0; }
void vfs_table_destroy(struct vfs_table *t) { (void)t; CHECK(!locks); }
int vfs_stat(struct vfs_table *t,const char *path,struct fat_entry *e)
{ (void)t; CHECK(!locks && !strcmp(path,"C:/system/settings/accel.cfg")); e->size=previous ? 12 : 0; return previous ? 0 : -FS_ENOENT; }
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share deny,uint8_t attr)
{ (void)t;(void)path;(void)attr; CHECK(!locks && flags==(VFS_WRITE|VFS_CREATE) && deny==VFS_DENY_ALL); return readonly ? -FS_EROFS : 1; }
int vfs_write(struct vfs_table *t,int fd,const void *data,size_t n,size_t *done)
{ (void)t; CHECK(!locks && fd==1 && strstr(data,"accel=attempt\n") && strstr(data,"driver=s3")); marker_writes++; *done=short_write ? n-1 : n; return 0; }
int vfs_commit(struct vfs_table *t,int fd)
{ (void)t; CHECK(!locks && fd==1); marker_commits++; return marker_fail ? -FS_EIO : 0; }
int vfs_close(struct vfs_table *t,int fd) { (void)t; CHECK(!locks && fd==1); return 0; }
int vfs_truncate(struct vfs_table *t,int fd,uint64_t n)
{ (void)t; CHECK(!locks && fd==1 && n==0); marker_clears++; return 0; }

struct event { unsigned reg,bits; uint32_t value; };
static struct fake {
    uint8_t pixels[640*32*3],cursor[4096],cr[256];
    uint32_t regs[0x10000/4];
    struct event trace[256]; unsigned count,reads,drains,pauses;
    struct event cr_trace[256]; unsigned cr_count;
    uint64_t us;
    bool trio,busy,frozen,wedged,hang_after_launch,delay_fail,hide_fail,move_fail;
    unsigned busy_polls;
    struct s3_device *device;
} f;
static uint32_t read_reg(void *ctx,unsigned reg,unsigned bits)
{
    struct fake *m=ctx; m->reads++;
    if (reg==(m->trio ? 0x9ae8u : 0x8504u)) {
        bool busy=m->busy || m->wedged || m->busy_polls;
        if (m->busy_polls) m->busy_polls--;
        return busy ? (m->trio ? 0x2ff : 0) : (m->trio ? 0 :
            (m->device->pci.device==0x8a10 || m->device->pci.device==0x8c01 || m->device->pci.device==0x8c03 ? 0xf000 : 0x3000));
    }
    (void)bits; return m->regs[reg/4];
}
static uint32_t pixel(unsigned x,unsigned y)
{
    unsigned b=f.device->fb.bpp/8;
    uint32_t v=0; memcpy(&v,f.pixels+y*fb.pitch+x*b,b); return v;
}
static void put(unsigned x,unsigned y,uint32_t v)
{
    unsigned b=f.device->fb.bpp/8;
    CHECK(x<fb.width && y<fb.height);
    memcpy(f.pixels+y*fb.pitch+x*b,&v,b);
}
static void execute(uint32_t cmd)
{
    struct s3_device *s=f.device;
    CHECK(!s->cpu_safe); launches++;
    if (corrupt || f.wedged) return;
    bool fill;
    int sx=0,sy=0,dx,dy,xstep,ystep;
    unsigned w,h;
    uint32_t colour;
    if (f.trio) {
        fill=(cmd&0xe000)==0x4000;
        w=f.regs[0x96e8/4]+1; h=f.regs[0xbee8/4]+1;
        sx=f.regs[0x86e8/4]; sy=f.regs[0x82e8/4];
        dx=fill ? sx : (int)f.regs[0x8ee8/4]; dy=fill ? sy : (int)f.regs[0x8ae8/4];
        xstep=cmd&0x20 ? 1 : -1; ystep=cmd&0x80 ? 1 : -1;
        colour=f.regs[0xa6e8/4];
        CHECK((cmd&0x11)==0x11 && f.regs[0xbae8/4]==(fill ? 0x27u : 0x67u));
    } else {
        fill=(cmd&(15u<<27))==(2u<<27);
        w=(f.regs[0xa504/4]>>16)+1; h=f.regs[0xa504/4]&65535;
        sx=f.regs[0xa508/4]>>16; sy=f.regs[0xa508/4]&65535;
        dx=f.regs[0xa50c/4]>>16; dy=f.regs[0xa50c/4]&65535;
        xstep=cmd&(1u<<25) ? 1 : -1; ystep=cmd&(1u<<26) ? 1 : -1;
        colour=f.regs[0xa4f4/4];
        CHECK(((cmd>>17)&255)==(fill ? 0xf0u : 0xccu));
        CHECK((cmd&0x1c)==(s->fb.bpp==16 ? 4u : 8u));
        CHECK(!(cmd&1)); /* no autoexecute, host-data or DMA mode */
    }
    CHECK(w && h && w<=fb.width && h<=fb.height);
    /* Execute in the submitted hardware direction, not via swdib/memmove.
     * A wrong overlap direction therefore corrupts the golden comparison. */
    for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
        int tx=dx+(int)x*xstep,ty=dy+(int)y*ystep;
        put(tx,ty,fill ? colour : pixel(sx+(int)x*xstep,sy+(int)y*ystep));
    }
    if (f.hang_after_launch) f.wedged=true;
}
static void write_reg(void *ctx,unsigned reg,uint32_t v,unsigned bits)
{
    struct fake *m=ctx;
    CHECK(m->count<256 && bits==(m->trio && reg!=0xa6e8 && reg!=0xaae8 ? 16u : 32u));
    m->trace[m->count++]=(struct event){reg,bits,v};
    if (m->trio && reg==0xbee8) {
        if (!(v&0xf000)) m->regs[reg/4]=v;
    } else m->regs[reg/4]=v;
    if ((m->trio && reg==0x9ae8) || (!m->trio && reg==0xa500 && (v&(15u<<27))!=(15u<<27))) execute(v);
}
static uint8_t cr_read(void *ctx,uint8_t r) { struct fake *m=ctx; return m->cr[r]; }
static void cr_write(void *ctx,uint8_t r,uint8_t v)
{
    struct fake *m=ctx;
    CHECK(m->cr_count<256);
    m->cr_trace[m->cr_count++]=(struct event){r,8,v};
    if ((r==0x45 && !(v&1) && m->hide_fail) || (r==0x47 && m->move_fail)) return;
    m->cr[r]=v;
}
static uint64_t now(void *ctx) { return ((struct fake *)ctx)->us; }
static int delay(void *ctx,unsigned us)
{ struct fake *m=ctx; if (!m->frozen) m->us+=us; return m->delay_fail ? -EFAULT : 0; }
static void drain(void *ctx) { ((struct fake *)ctx)->drains++; }
static int pause_hook(void *ctx) { struct fake *m=ctx; m->pauses++; return 0; }
static int cancel_hook(void *ctx) { struct fake *m=ctx; m->pauses++; return -EINTR; }
int s3_access_begin(struct s3_device *s)
{
    CHECK(!locks && marker_commits); access_calls++;
    if (access_fail) return -ENOMEM;
    f.device=s;
    s->io=(struct s3_io){&f,read_reg,write_reg,cr_read,cr_write,now,delay,drain};
    s->cursor=f.cursor; s->cursor_offset=0x3ff000;
    return 0;
}
int s3_access_end(struct s3_device *s)
{ CHECK(s->cpu_safe && s->cursor_hidden && !s->active); releases++; return 0; }
static struct s3_device device(const struct s3_backend *b)
{
    return (struct s3_device){.backend=b,.id=b==&s3trio64_backend ? "s3trio64" : "s3virge",
        .name="test",.reason="pending",.cpu_safe=true,.cursor_hidden=true,
        .reg_claim=-1,.port_claim=-1,.cursor_claim=-1};
}
static void setup(const struct s3_backend *b,unsigned bpp,uint16_t id)
{
    memset(&f,0,sizeof(f)); memset(&g_boot,0,sizeof(g_boot));
    memset(gpu,0,sizeof(gpu)); gpu_count=1;
    gpu[0]=(struct pci_func){.vendor=0x5333,.device=id,.class_code=3,.command=3,.bar={0xe0000000}};
    g_boot.display.vram_bytes=4*1024*1024;
    fb=(struct fb_device){.width=640,.height=32,.pitch=640*(bpp/8),.size=640*32*(bpp/8),
        .phys=0xe0000000,.mapped=f.pixels,.bpp=bpp,.present=true,.handle=0,.generation=42,
        .red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,
        .green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,.blue_size=bpp==16 ? 5 : 8};
    lease=(struct resource){.type=RES_MMIO,.state=RS_ACTIVE,.start=fb.phys,.end=fb.phys+fb.size,.owner="fbdev",.generation=42};
    f.trio=b==&s3trio64_backend;
    f.cr[0x50]=0x50; f.cr[0x67]=bpp==16 ? 0x50 : 0xd0;
    f.cr[0x13]=(fb.pitch/8)&255; f.cr[0x51]=((fb.pitch/8)>>4)&0x30;
    f.cr[0x58]=0x10; f.cr[0x59]=0xe0; f.cr[0x53]=8;
    f.cr[0x40]=f.cr[0x66]=f.cr[0x63]=1;
    memset(&vfs,0,sizeof(vfs)); memset(&volume,0,sizeof(volume));
    volume.mounted=true; vfs.volumes[2]=&volume;
    access_fail=corrupt=previous=readonly=marker_fail=short_write=probe_busy=false;
    launches=access_calls=probes=releases=marker_commits=marker_writes=marker_clears=0;
    CHECK(display_driver_bind(&fb)!=0);
}
static void activate(struct s3_device *s)
{
    CHECK(s3_attempt_begin(s,&vfs)==0);
    CHECK(s3_start(s,&fb,0)==&s->ops);
    CHECK(marker_writes==1 && probes==1 && launches==3 && s->cpu_safe && s->active);
    CHECK(s3_attempt_end(s,&vfs)==0 && marker_clears==1);
}
static void commands(const struct s3_backend *b,unsigned bpp,uint16_t id)
{
    setup(b,bpp,id); struct s3_device s=device(b); activate(&s);
    uint8_t golden[sizeof(f.pixels)];
    for (unsigned n=0;n<fb.size;n++) f.pixels[n]=(n*73+n/11)&255;
    memcpy(golden,f.pixels,fb.size);
    struct fb_device ref=fb; ref.mapped=golden;
    struct fb_rect r={9,3,9,5};
    f.count=0;
    CHECK(!s3_fill(&s,&r,0xa739)); swdib_fill(&ref,&r,0xa739);
    CHECK(!memcmp(golden,f.pixels,fb.size));
    CHECK(f.trace[f.count-1].reg==(f.trio ? 0x9ae8u : 0xa500u));
    const unsigned trio_fill[]={0xbee8,0xa6e8,0xbae8,0x86e8,0x82e8,0x96e8,0xbee8,0x9ae8};
    const unsigned virge_fill[]={0xa4f4,0xa504,0xa50c,0xa500};
    CHECK(f.count==(f.trio ? 8u : 4u));
    for (unsigned n=0;n<f.count;n++) CHECK(f.trace[n].reg==(f.trio ? trio_fill[n] : virge_fill[n]));
    CHECK(f.trace[f.count-1].value==(f.trio ? 0x40b1u :
        (0x10000120u|(0xf0u<<17)|(3u<<25)|(bpp==16 ? 4u : 8u))));
    for (unsigned dir=0;dir<4;dir++) {
        int dx=dir&1 ? 8 : 10,dy=dir&2 ? 2 : 4;
        f.count=0;
        CHECK(!s3_copy(&s,&r,dx,dy)); swdib_copy(&ref,&r,dx,dy);
        CHECK(!memcmp(golden,f.pixels,fb.size));
        uint32_t cmd=f.trace[f.count-1].value;
        const unsigned trio_copy[]={0xbee8,0xbae8,0x86e8,0x82e8,0x8ee8,0x8ae8,0x96e8,0xbee8,0x9ae8};
        const unsigned virge_copy[]={0xa504,0xa508,0xa50c,0xa500};
        CHECK(f.count==(f.trio ? 9u : 4u));
        for (unsigned n=0;n<f.count;n++) CHECK(f.trace[n].reg==(f.trio ? trio_copy[n] : virge_copy[n]));
        CHECK(!!(cmd&(f.trio ? 0x20u : 1u<<25))==(dx<=r.x));
        CHECK(!!(cmd&(f.trio ? 0x80u : 1u<<26))==(dy<=r.y));
    }
    unsigned count=f.count; r.width=0; CHECK(!s3_fill(&s,&r,1) && f.count==count);
    r.width=INT32_MAX; CHECK(s3_fill(&s,&r,1)==-EOPNOTSUPP && f.count==count);
    r.width=3; r.x=-1; CHECK(s3_copy(&s,&r,0,0)==-EOPNOTSUPP && f.count==count);
    r.x=0; CHECK(!s3_copy(&s,&r,0,r.y) && f.count==count);
    struct accel_cursor c={.foreground=0xf800,.background=0x7e0};
    c.mask[0]=0xf0000001; c.image[0]=0xafffffff;
    CHECK(!s3_cursor_set(&s,&c) && s.cursor_hidden);
    CHECK(f.cursor[0]==0x0f && f.cursor[1]==0xff && f.cursor[2]==0xa0 && f.cursor[3]==0);
    CHECK(f.cursor[4]==0xff && f.cursor[5]==0xfe && f.cursor[6]==0 && f.cursor[7]==1);
    CHECK(f.cursor[8]==255 && f.cursor[10]==0 && f.cursor[1024]==255 && f.cursor[1026]==0);
    f.cr_count=0;
    CHECK(!s3_cursor_move(&s,-2,-4) && f.cr[0x4e]==2 && f.cr[0x4f]==4);
    const unsigned move_order[]={0x4e,0x4f,0x47,0x46,0x49,0x48};
    CHECK(f.cr_count==6);
    for (unsigned n=0;n<6;n++) CHECK(f.cr_trace[n].reg==move_order[n]);
    CHECK(s3_cursor_move(&s,-1,0)==-EOPNOTSUPP);
    CHECK(s3_cursor_move(&s,INT32_MIN,0)==-EOPNOTSUPP);
    CHECK(!s3_cursor_show(&s,true) && (f.cr[0x45]&1));
    CHECK(!s3_sync(&s) && s.cpu_safe); /* cursor visibility is not an engine hazard */
    CHECK(!s3_cursor_set(&s,&c) && !(f.cr[0x45]&1));
    CHECK(!s3_cursor_test(&s.ops,1000,0,0) && s.cursor_hidden);
    CHECK(!s3_stop(&s) && releases==1 && s.cpu_safe);
    printf("PASS command goldens: %s %u bpp %04x\n",s.id,bpp,id);
}
static void faults(const struct s3_backend *b,uint16_t id)
{
    setup(b,16,id); struct s3_device s=device(b); activate(&s);
    f.count=0; f.busy=true; f.frozen=true;
    s3_pause(&s,pause_hook,&f);
    CHECK(s3_sync(&s)==-ETIMEDOUT && s.quarantined && !s.cpu_safe && f.pauses);
    CHECK(f.reads<5000 && s.timeouts==1 && s3_caps(&s)==0);
    CHECK(s3_fill(&s,&(struct fb_rect){0,0,2,2},1)==-EIO && f.count==0);
    CHECK(s3_fallback(&s)==&s.ops && !s.cpu_safe);
    CHECK(s3_stop(&s)==-ETIMEDOUT && releases==0);
    f.busy=false; CHECK(!s3_sync(&s) && s.cpu_safe && s.quarantined);
    CHECK(s3_attempt_begin(&s,&vfs)==-EIO);
    CHECK(!s3_stop(&s) && releases==1);
    setup(b,16,id); s=device(b); activate(&s);
    f.wedged=true;
    unsigned before=launches;
    CHECK(s3_copy(&s,&(struct fb_rect){0,0,4,2},1,1)==-ETIMEDOUT);
    CHECK(launches==before && !s.cpu_safe);
    setup(b,16,id); s=device(b); activate(&s);
    f.hang_after_launch=true;
    unsigned launched=launches;
    CHECK(s3_copy(&s,&(struct fb_rect){0,0,4,2},1,1)==-ETIMEDOUT);
    CHECK(launches==launched+1 && !s.cpu_safe && s.quarantined && s.copies==0);
    CHECK(s3_fallback(&s) && launches==launched+1 && !s.cpu_safe);
    f.wedged=false; CHECK(!s3_sync(&s) && s.cpu_safe && s.quarantined);
    setup(b,16,id); s=device(b); activate(&s);
    f.busy=true; s3_pause(&s,cancel_hook,&f);
    CHECK(s3_sync(&s)==-EINTR && s.quarantined && !s.cpu_safe && f.pauses==1);
    setup(b,16,id); s=device(b); activate(&s);
    f.delay_fail=true; f.busy=true;
    CHECK(s3_sync(&s)==-EFAULT && s.quarantined && !s.cpu_safe);
    setup(b,16,id); s=device(b); activate(&s);
    struct accel_cursor c={0}; CHECK(!s3_cursor_set(&s,&c)); CHECK(!s3_cursor_show(&s,true));
    f.hide_fail=true; s3_fault(&s,"test"); CHECK(!s.cpu_safe && s.quarantined);
    CHECK(s3_fallback(&s) && !s.cpu_safe); /* idle alone cannot prove overlay disabled */
    printf("PASS quarantine/timeout: %s\n",s.id);
}
static void lifecycle(const struct s3_backend *b,uint16_t id)
{
    setup(b,16,id); struct s3_device s=device(b);
    CHECK(s3_eligible(&fb,b) && !access_calls && !f.count && !launches);
    g_boot.flags|=CBI_F_SAFE_MODE;
    CHECK(s3_attempt_begin(&s,&vfs)==-EOPNOTSUPP && !marker_writes && !access_calls);
    g_boot.flags=0; memcpy(g_boot.options,"accel=0",8);
    CHECK(!s3_eligible(&fb,b) && s3_attempt_begin(&s,&vfs)==-EOPNOTSUPP);
    memset(g_boot.options,0,sizeof(g_boot.options));
    CHECK(!s3_start(&s,&fb,0) && !access_calls);
    previous=true; CHECK(s3_attempt_begin(&s,&vfs)==-EIO && !marker_writes);
    setup(b,16,id); s=device(b); volume.readonly=true;
    CHECK(s3_attempt_begin(&s,&vfs)==-FS_EROFS && !marker_writes && !access_calls);
    setup(b,16,id); s=device(b); marker_fail=true;
    CHECK(s3_attempt_begin(&s,&vfs)==-FS_EIO && !s.authorized && !access_calls);
    setup(b,16,id); s=device(b); short_write=true;
    CHECK(s3_attempt_begin(&s,&vfs)==-FS_EIO && !s.authorized && !marker_commits);
    setup(b,16,id); s=device(b); access_fail=true; CHECK(!s3_attempt_begin(&s,&vfs));
    CHECK(!s3_start(&s,&fb,0) && !launches && s.cpu_safe);
    setup(b,16,id); s=device(b); f.cr[0x0d]=1; CHECK(!s3_attempt_begin(&s,&vfs));
    CHECK(!s3_start(&s,&fb,0) && !launches && s.quarantined && s.cpu_safe);
    setup(b,16,id); s=device(b); corrupt=true; CHECK(!s3_attempt_begin(&s,&vfs));
    CHECK(!s3_start(&s,&fb,0) && s.quarantined && s.cpu_safe && !marker_clears);
    setup(b,16,id); s=device(b); f.cr[0x13]++; CHECK(!s3_attempt_begin(&s,&vfs));
    CHECK(!s3_start(&s,&fb,0) && !launches);
    setup(b,16,id); s=device(b); f.cr[0x55]=0x10; activate(&s);
    CHECK(s3_caps(&s)==1 && s3_cursor_set(&s,&(struct accel_cursor){0})==-EOPNOTSUPP);
    setup(b,16,id); s=device(b); f.cr[0x53]=0; activate(&s);
    CHECK(s.mmio_changed && f.cr[0x53]==8);
    setup(b,16,id); s=device(b); activate(&s); marker_fail=true;
    CHECK(s3_attempt_end(&s,&vfs)==-FS_EIO);
    struct ciuki_accel_info i; s3_snapshot(&s,&i); CHECK(i.driver==1 && i.generation==42);
    printf("PASS lifecycle/marker: %s\n",s.id);
}
static void ownership(void)
{
    setup(&s3trio64_backend,16,0x8811);
    unsigned count=f.count,reads=f.reads;
    CHECK(!strcmp(display_driver_bound()->id,"s3trio64"));
    CHECK(f.count==count && f.reads==reads); /* probe is PCI-only */
    gpu_count=2; gpu[1]=gpu[0]; CHECK(!display_driver_bind(&fb));
    gpu_count=1; gpu[0].command=1; CHECK(!display_driver_bind(&fb));
    gpu[0].command=3; lease.generation++; CHECK(!display_driver_bind(&fb));
    lease.generation=42; lease.end--; CHECK(!display_driver_bind(&fb));
    lease.end++; gpu[0].bar[0]+=0x1000000; CHECK(!display_driver_bind(&fb));
    gpu[0].bar[0]=fb.phys; gpu[0].device=0x8c2e; CHECK(!display_driver_bind(&fb));
    const uint16_t trio_ids[]={0x8811,0x8814,0x8901};
    const uint16_t virge_ids[]={0x5631,0x883d,0x8a01,0x8a10,0x8c01,0x8c03};
    for (unsigned n=0;n<ARRAY_SIZE(trio_ids);n++) {
        setup(&s3trio64_backend,16,trio_ids[n]);
        CHECK(!strcmp(display_driver_bound()->id,"s3trio64") && !f.count && !f.reads);
    }
    for (unsigned n=0;n<ARRAY_SIZE(virge_ids);n++) {
        setup(&s3virge_backend,16,virge_ids[n]);
        CHECK(!strcmp(display_driver_bound()->id,"s3virge") && !f.count && !f.reads);
    }
    setup(&s3trio64_backend,16,0x8811);
    struct s3_device original=device(&s3trio64_backend);
    original.fb=fb; original.fb.width=1600; original.fb.pitch=3200; original.pci=gpu[0];
    original.io=(struct s3_io){&f,read_reg,write_reg,cr_read,cr_write,now,delay,drain};
    f.cr[0x50]=0x91; f.cr[0x13]=144; f.cr[0x51]=0x10;
    CHECK(!s3trio64_backend.firmware(&original));
    original.pci.revision=0x40; CHECK(s3trio64_backend.firmware(&original));
    setup(&s3virge_backend,24,0x8a01); CHECK(!strcmp(display_driver_bound()->id,"s3virge"));
    CHECK(!s3_eligible(&fb,&s3virge_backend));
    fb.bpp=32; CHECK(!s3_eligible(&fb,&s3virge_backend));
    setup(&s3virge_backend,16,0x883d);
    struct s3_device vx=device(&s3virge_backend); f.cr[0x67]=0x40; activate(&vx);
    setup(&s3virge_backend,16,0x5631); struct s3_device s=device(&s3virge_backend); activate(&s);
    unsigned writes=f.count;
    CHECK(s3_copy(&s,&(struct fb_rect){0,0,26,2},1,1)==-EOPNOTSUPP && f.count==writes && !s.quarantined);
    puts("PASS registry ownership and family/errata refusals");
}
static void adapter(const struct s3_backend *b,uint16_t id,const char *name)
{
    setup(b,16,id);
    const struct display_driver *d=display_driver_bound();
    CHECK(d && !strcmp(d->name,name) && d->eligible(&fb));
    CHECK(!d->attempt_begin(&vfs));
    const struct accel_ops *ops=d->start(&fb,0);
    CHECK(ops && ops->ctx==f.device && !d->attempt_end(&vfs));
    struct ciuki_display_engine_info info;
    display_driver_info(&info);
    CHECK(!strcmp(info.display_name,name) && !strcmp(info.driver_id,d->id) && info.ready);
    CHECK(!ops->fill_rect(ops->ctx,&(struct fb_rect){0,0,2,2},0x1234));
    display_driver_count(CIUKI_ENGINE_FILL,true);
    display_driver_info(&info); CHECK(info.native_used);
    CHECK(!d->cursor_test(ops,1000,0,0));
    d->quarantine("fixture_fault"); CHECK(ops->capabilities(ops->ctx)==0);
    CHECK(display_driver_refresh());
    display_driver_info(&info); CHECK(info.quarantined && !info.ready && !strcmp(info.reason,"fixture_fault"));
    CHECK(d->fallback()==ops && ops->cpu_ready(ops->ctx));
    CHECK(d->attempt_begin(&vfs)==-EIO);
    CHECK(!ops->stop(ops->ctx));
    printf("PASS registry adapter identity/lifecycle/cursor/quarantine: %s\n",name);
}
int main(void)
{
    commands(&s3trio64_backend,16,0x8811);
    commands(&s3virge_backend,16,0x8a01);
    commands(&s3virge_backend,16,0x8a10);
    faults(&s3trio64_backend,0x8811); faults(&s3virge_backend,0x8a01);
    lifecycle(&s3trio64_backend,0x8811); lifecycle(&s3virge_backend,0x8a01);
    ownership();
    adapter(&s3trio64_backend,0x8811,"S3 Trio64");
    adapter(&s3virge_backend,0x8a01,"S3 ViRGE");
    puts("PASS f2-118 fake-register suite");
    return 0;
}
