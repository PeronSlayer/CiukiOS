/* Production register driver + production framebuffer renderer, ASan/UBSan.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main framebuffer_regression_main
#define savage2d_init boot_init
#include "fbdev_test.c"
#undef savage2d_init
#undef main
#include <ciuki/savage2d.h>
static unsigned binding_claims, binding_pci_count;
static struct pci_func binding_gpu;
static bool binding_allowed;
static struct resource binding_resource;
static int binding_claim(enum res_type t,uint32_t s,uint32_t e,const char *o,bool share)
{
    (void)t; (void)s; (void)e; (void)o; (void)share;
    binding_claims++;
    if (!binding_allowed) return -EBUSY;
    binding_resource=(struct resource){.state=RS_CLAIMED,.generation=43};
    return 1;
}
static const struct resource *binding_get(unsigned i)
{ return i==1 ? &binding_resource : registry_get(i); }
static int binding_activate(int h, gen_t gen)
{ CHECK(h==1 && gen==43); binding_resource.state=RS_ACTIVE; return 0; }
static int binding_quarantine(int h, gen_t gen)
{ CHECK(h==1 && gen==43); binding_resource.state=RS_QUARANTINED; return 0; }
static void *binding_reserve(uint32_t capacity)
{ CHECK(capacity==SAVAGE_MMIO_BYTES); return (void *)(uintptr_t)0x1000; }
#define registry_claim binding_claim
#define registry_get binding_get
#define registry_activate binding_activate
#define registry_quarantine binding_quarantine
#define vmm_reserve_mmio binding_reserve
#include "../../src/kernel/drivers/savage2d.c"
#undef registry_claim
#undef registry_get
#undef registry_activate
#undef registry_quarantine
#undef vmm_reserve_mmio

unsigned pci_count(void) { return binding_pci_count; }
const struct pci_func *pci_get(unsigned i) { return i<binding_pci_count ? &binding_gpu : NULL; }
void *vmm_map_mmio(uint32_t p, uint32_t n, bool uc) { (void)p; (void)n; (void)uc; return NULL; }
static uint8_t binding_window[SAVAGE_MMIO_BYTES];
void *vmm_replace_mmio(void *w,uint32_t c,uint32_t p,uint32_t n)
{ (void)w; CHECK(c==SAVAGE_MMIO_BYTES && n==c && p==(binding_gpu.bar[0]&~15u)); return binding_window; }
int udelay(uint32_t us) { CHECK(us<=500); return 0; }
void kwork_yield(void) { }
static kwork_fn queued_work;
static void *queued_arg;
static bool queue_allowed=true;
static unsigned boot_calls;
bool kwork_queue(kwork_fn fn, void *arg)
{ CHECK(!queued_work); if (!queue_allowed) return false; queued_work=fn; queued_arg=arg; return true; }

/* Durable settings boundary: distinguish cache bytes from committed bytes.
 * An interrupted attempt is tested by retaining only disk_marker. */
static struct vfs settings_vfs;
static struct fat_volume settings_volume;
static char disk_marker[16], cache_marker[16];
static unsigned disk_size, cache_size, marker_commits;
static bool marker_exists, table_open, marker_open, marker_error, clear_error;
static unsigned marker_fault;
int vfs_table_init(struct vfs *v, struct vfs_table *t, unsigned drive)
{ CHECK(v==&settings_vfs && drive==2 && !table_open); if (marker_fault==1) return -FS_ENOMEM; table_open=true; t->vfs=v; return 0; }
void vfs_table_destroy(struct vfs_table *t)
{ CHECK(t==&attempt_table && table_open && !marker_open); table_open=false; }
int vfs_stat(struct vfs_table *t, const char *path, struct fat_entry *e)
{ CHECK(t==&attempt_table && !strcmp(path,ATTEMPT_PATH)); if (marker_fault==2) return -FS_EIO; e->size=disk_size; return marker_exists ? 0 : -FS_ENOENT; }
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share deny,uint8_t attr)
{
    CHECK(t==&attempt_table && !strcmp(path,ATTEMPT_PATH) && flags==(VFS_WRITE|VFS_CREATE) && deny==VFS_DENY_ALL && !attr);
    if (marker_error) return -FS_EIO;
    marker_open=true; memcpy(cache_marker,disk_marker,sizeof(cache_marker)); cache_size=disk_size; return 0;
}
int vfs_write(struct vfs_table *t,int fd,const void *buf,size_t n,size_t *done)
{
    CHECK(t==&attempt_table && fd==0 && marker_open && n==14 && !memcmp(buf,"accel=attempt\n",n));
    memcpy(cache_marker,buf,n); cache_size=n; *done=marker_fault==3 ? n-1 : n; return marker_fault==4 ? -FS_EIO : 0;
}
int vfs_truncate(struct vfs_table *t,int fd,uint64_t n)
{ CHECK(t==&attempt_table && fd==0 && marker_open && !n); cache_size=0; return 0; }
int vfs_commit(struct vfs_table *t,int fd)
{
    CHECK(t==&attempt_table && fd==0 && marker_open);
    if ((!cache_size && clear_error) || marker_fault==5) return -FS_EIO;
    memcpy(disk_marker,cache_marker,sizeof(disk_marker)); disk_size=cache_size; marker_exists=true; marker_commits++; return 0;
}
int vfs_close(struct vfs_table *t,int fd)
{ CHECK(t==&attempt_table && fd==0 && marker_open); marker_open=false; return marker_fault==6 ? -FS_EIO : 0; }

struct model {
    uint32_t reg[SAVAGE_MMIO_BYTES/4], packet[16];
    uint8_t cr[256], cursor_page[4096], colour[2][3];
    unsigned words, expected, packets, fault_count, busy, cr_writes, colour_at;
    uint8_t cr_sequence[128];
    bool stuck, frozen, bad_readback, fail_after_submit, partial, locked, dead, wrong_offset, delay_failed, bci_locked;
    uint8_t misc, seq[256], seq_index;
    unsigned status_reads, delays, reset_phase;
    bool contradictory;
    bool control_reserved;
    uint64_t reset_set_us, reset_clear_us, last_status_us;
    uint32_t status_value;
    uint64_t us;
};
static struct model m;
static struct savage2d test_engine;
static void marker_before_register(void)
{
    if (initializing)
        CHECK(disk_size==14 && marker_commits==1 && !marker_open && !table_open && locks==1);
}
static void execute_packet(struct model *p)
{
    CHECK(p->packet[1]==0);
    unsigned bpp=(p->packet[2]>>16)&255, stride=p->packet[2]&65535;
    CHECK(bpp==device.bpp && stride==device.pitch/(bpp/8));
    CHECK((p->packet[2]&0x0f000000u)==0);
    bool copying=(p->packet[0]&0x1e0u)==0x140u, pattern=(p->packet[0]&3u)==3u;
    unsigned index=copying ? 5 : pattern ? 5 : 4;
    uint32_t origin=p->packet[index], dimensions=p->packet[index+1];
    unsigned w=dimensions&4095,h=dimensions>>16;
    unsigned bytes=bpp/8;
    int32_t sx=0,sy=0,dx=origin&4095,dy=origin>>16;
    if (copying) {
        CHECK(p->packet[3]==0 && p->packet[4]==p->packet[2]);
        sx=origin&4095; sy=origin>>16;
        dx=p->packet[6]&4095; dy=p->packet[6]>>16;
        w=p->packet[7]&4095; h=p->packet[7]>>16;
    }
    CHECK(((p->packet[0]>>16)&255u)==(pattern ? 0xf0u : 0xccu));
    for (unsigned y=0; y<h; y++) for (unsigned x=0; x<w; x++) {
        int32_t px=dx+((p->packet[0]&XP) ? (int32_t)x : -(int32_t)x);
        int32_t py=dy+((p->packet[0]&YP) ? (int32_t)y : -(int32_t)y);
        CHECK(px>=0 && py>=0 && (unsigned)px<device.width && (unsigned)py<device.height);
        uint8_t *dst=device.mapped+(unsigned)py*device.pitch+(unsigned)px*bytes;
        uint32_t colour=p->packet[3];
        if (copying) {
            int32_t ax=sx+((p->packet[0]&XP) ? (int32_t)x : -(int32_t)x);
            int32_t ay=sy+((p->packet[0]&YP) ? (int32_t)y : -(int32_t)y);
            CHECK(ax>=0 && ay>=0 && (unsigned)ax<device.width && (unsigned)ay<device.height);
            memcpy(dst,device.mapped+(unsigned)ay*device.pitch+(unsigned)ax*bytes,bytes);
        } else {
            if (pattern) {
                unsigned shift=((unsigned)py&3u)*8;
                uint8_t row=(uint8_t)(p->packet[7+((unsigned)py&7u)/4]>>shift);
                colour=(row&(0x80u>>((unsigned)px&7u))) ? p->packet[3] : p->packet[4];
            }
            for (unsigned b=0;b<bytes;b++) dst[b]=(uint8_t)(colour>>(b*8));
        }
        if (p->partial && x==w/2) return;
    }
}
static uint32_t model_read(void *ctx,uint32_t offset)
{
    struct model *p=ctx;
    trace("r32",offset,offset);
    CHECK(offset<SAVAGE_MMIO_BYTES && !(offset&3u));
    marker_before_register();
    if (offset==SAVAGE_STATUS) {
        CHECK(p->reset_phase==2 && p->reset_set_us>=10000 && p->reset_clear_us>=10000);
        if (p->status_reads) CHECK(p->us-p->last_status_us>=POLL_US);
        p->last_status_us=p->us; p->status_reads++;
        if (p->dead) return UINT32_MAX;
        if (p->wrong_offset) return 0;
        if (p->stuck) return 0;
        if (p->busy) { p->busy--; return 0x00e00001u; }
        if (p->contradictory && p->status_reads>1) return 0x004e0000;
        return p->status_value ? p->status_value : 0x00e00000u;
    }
    if (offset>=0x8100 && offset<0x8180) CHECK(!(p->reg[BCI_CONTROL/4]&8));
    if (offset==BCI_CONTROL && p->control_reserved) return p->reg[offset/4]|0x4000;
    if (offset==0x816c && p->bad_readback) return p->reg[offset/4]^16u;
    return p->reg[offset/4];
}
static void model_write(void *ctx,uint32_t offset,uint32_t value)
{
    struct model *p=ctx;
    marker_before_register();
    trace("w32",0x40000000u|offset,offset);
    CHECK(offset<SAVAGE_MMIO_BYTES && !(offset&3u));
    if (offset>=SAVAGE_BCI && offset<SAVAGE_BCI+64) {
        unsigned at=(offset-SAVAGE_BCI)/4;
        CHECK(at==p->words && at<16);
        CHECK(drains>0); /* CPU WC writes drained before command */
        p->packet[at]=value;
        if (!at) {
            CHECK((value&0xfc00u)==(DEST_NEW|(value&COLOR)));
            p->expected=(value&3u)==3u ? 9 : (value&0x1e0u)==0x140u ? 8 : 6;
        }
        if (++p->words==p->expected) {
            execute_packet(p); p->packets++; p->words=0; p->busy=3;
            if (p->fail_after_submit) p->stuck=true;
        }
    } else {
        if (offset>=0x8100 && offset<0x8180) CHECK(!(p->reg[BCI_CONTROL/4]&8));
        if (offset==BCI_CONTROL && p->bci_locked) return;
        p->reg[offset/4]=value;
    }
}
static uint8_t model_cr_read(void *ctx,uint8_t index)
{
    struct model *p=ctx;
    marker_before_register();
    trace("crr",0x80000000u|index,index);
    if (index==0x45) p->colour_at=0;
    return p->cr[index];
}
static void model_cr_write(void *ctx,uint8_t index,uint8_t value)
{
    struct model *p=ctx;
    marker_before_register();
    trace("crw",0xc0000000u|index,index);
    if (p->cr_writes<sizeof(p->cr_sequence)) p->cr_sequence[p->cr_writes]=index;
    p->cr_writes++;
    if (index==0x4a || index==0x4b) {
        CHECK(p->colour_at<3);
        p->colour[index==0x4b][p->colour_at++]=value;
    }
    if (p->locked) return;
    if (index==0x66) {
        CHECK((p->cr[index]&1u)==(value&1u));
        p->reset_phase=value&2u ? 1 : 2;
        if (value&2u) { p->reset_set_us=0; p->reset_clear_us=0; p->status_reads=0; }
    }
    p->cr[index]=value;
}
static uint64_t model_now(void *ctx)
{
    struct model *p=ctx;
    if (!p->frozen) p->us+=100;
    return p->frozen ? 0 : p->us;
}
static int model_delay(void *ctx,uint32_t us)
{
    struct model *p=ctx; p->delays++;
    if (initializing) CHECK(disk_size==14 && marker_commits==1 && !fbdev_console_begin() && locks==1);
    if (p->delay_failed) return -EFAULT;
    p->us+=us;
    if (p->reset_phase==1) p->reset_set_us+=us;
    else if (p->reset_phase==2) p->reset_clear_us+=us;
    return 0;
}
static uint8_t model_read8(void *ctx,uint32_t offset)
{
    struct model *p=ctx; trace("r8",0x10000000u|offset,offset);
    marker_before_register();
    CHECK(offset==0x83cc || offset==0x83c5);
    return offset==0x83cc ? p->misc : p->seq[p->seq_index];
}
static void model_write8(void *ctx,uint32_t offset,uint8_t value)
{
    struct model *p=ctx; trace("w8",0x20000000u|offset,offset);
    marker_before_register();
    if (offset==0x83c4) p->seq_index=value;
    else if (offset==0x83c5) p->seq[p->seq_index]=value;
    else { CHECK(offset==0x83c2); p->misc=value; }
}
static void model_drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static void model_fault(void *ctx) { ((struct model *)ctx)->fault_count++; }
static struct savage_io model_io(void)
{
    return (struct savage_io){&m,model_read,model_write,model_cr_read,model_cr_write,
        model_now,model_delay,model_drain,model_fault,model_read8,model_write8};
}
const struct accel_ops *boot_init(const struct fb_device *d, struct vfs *vfs)
{ boot_calls++; struct savage_io io=model_io(); return init_with_io(d,vfs,&io); }
static uint8_t *storage;
static void prepare(unsigned bpp)
{
    memset(&m,0,sizeof(m)); memset(&test_engine,0,sizeof(test_engine));
    m.cr[0x66]=1;
    initialized=true; changing=false; drawing=0; console_rows=8;
    g_boot.flags=0; acceleration=NULL;
    device=(struct fb_device){.width=96,.height=64,.pitch=128*(bpp/8),.size=64*128*(bpp/8),
        .phys=0xe0000000,.bpp=bpp,.red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,
        .green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,.blue_size=bpp==16 ? 5 : 8,
        .present=true,.handle=0,.generation=42};
    storage=malloc(device.size+32); CHECK(storage!=NULL);
    memset(storage,0xa5,device.size+32); device.mapped=storage+16;
    reservation=(struct resource){.state=RS_ACTIVE,.generation=42};
    struct savage_io io=model_io();
    CHECK(!savage2d_start(&test_engine,&io,&device,true,m.cursor_page,0x100000));
    acceleration=&test_engine.ops;
}
static void finish(void)
{
    for (unsigned i=0;i<16;i++) CHECK(storage[i]==0xa5 && storage[16+device.size+i]==0xa5);
    for (unsigned y=0;y<device.height;y++) for (unsigned x=device.width*(device.bpp/8);x<device.pitch;x++)
        CHECK(device.mapped[y*device.pitch+x]==0xa5);
    free(storage); acceleration=NULL;
}
static struct ciuki_accel_request request(unsigned op)
{ return (struct ciuki_accel_request){.size=sizeof(struct ciuki_accel_request),.operation=op,.generation=42}; }
static void commands(unsigned bpp)
{
    prepare(bpp);
    CHECK(m.reg[0x48c18/4]==8 && m.reg[0x48c0c/4]==0);
    CHECK(m.reg[0x816c/4]==(test_engine.descriptor|1u));
    CHECK((m.cr[0x50]&0x30u)==(bpp==16 ? 0x10u : 0x30u) && m.cr[0x66]==1);
    struct ciuki_accel_request q=request(CIUKI_ACCEL_FILL);
    q.rect=(struct ciuki_rect){0,0,-5,-3,30,20}; q.colour=0x123456;
    uint8_t *expected=malloc(device.size); CHECK(expected!=NULL);
    memcpy(expected,device.mapped,device.size);
    struct fb_rect r={-5,-3,30,20}; reference_fill(&device,expected,&r,q.colour);
    CHECK(!fbdev_accel_request(&q)); CHECK(!memcmp(device.mapped,expected,device.size));
    q=request(CIUKI_ACCEL_PATTERN); q.rect=(struct ciuki_rect){0,0,3,4,20,15};
    q.colour=0xabcdef; q.background=0x643210;
    for (unsigned i=0;i<8;i++) q.pattern[i]=(uint8_t)(0x81u^(i*19));
    for (unsigned y=4;y<19;y++) for (unsigned x=3;x<23;x++)
        reference_pixel(&device,expected+y*device.pitch+x*(bpp/8),q.pattern[y&7]&(0x80u>>(x&7)) ? q.colour : q.background);
    CHECK(!fbdev_accel_request(&q)); CHECK(!memcmp(device.mapped,expected,device.size));
    for (int direction=0;direction<8;direction++) {
        for (unsigned y=0;y<device.height;y++) for (unsigned x=0;x<device.width;x++)
            reference_pixel(&device,device.mapped+y*device.pitch+x*(bpp/8),(x*37u+y*9983u)&0xffffffu);
        memcpy(expected,device.mapped,device.size);
        uint8_t before[64*512]; memcpy(before,device.mapped,device.size);
        int dx=(direction&1) ? 5 : -5,dy=(direction&2) ? 4 : -4;
        if (direction&4) dy=0;
        q=request(CIUKI_ACCEL_COPY); q.rect=(struct ciuki_rect){20,20,20+dx,20+dy,48,25};
        for (unsigned y=0;y<25;y++) for (unsigned x=0;x<48;x++)
            memcpy(expected+(20+dy+y)*device.pitch+(20+dx+x)*(bpp/8),before+(20+y)*device.pitch+(20+x)*(bpp/8),bpp/8);
        CHECK(!fbdev_accel_request(&q)); CHECK(!memcmp(device.mapped,expected,device.size));
        CHECK((m.packet[0]&XP)!=0 ? dx<=0 : dx>0);
        CHECK((m.packet[0]&YP)!=0 ? dy<=0 : dy>0);
    }
    unsigned packets=m.packets;
    q.generation--; CHECK(fbdev_accel_request(&q)==-EAGAIN); CHECK(m.packets==packets);
    q.generation=42; q.rect.src_x=INT32_MAX; CHECK(fbdev_accel_request(&q)==-EINVAL); CHECK(m.packets==packets);
    q=request(CIUKI_ACCEL_CURSOR_SET); q.colour=0xffffff; q.background=0x101010;
    q.cursor_mask[0]=0xc0000000; q.cursor_image[0]=0x40000000;
    CHECK(!fbdev_accel_request(&q));
    CHECK(m.cursor_page[0]==0xff && m.cursor_page[1]==0x3f && m.cursor_page[2]==0 && m.cursor_page[3]==0x40);
    CHECK(m.cursor_page[4]==0xff && m.cursor_page[5]==0xff && !m.cursor_page[6] && !m.cursor_page[7]);
    CHECK(m.cr[0x4c]==4 && m.cr[0x4d]==0 && !(m.cr[0x55]&16u));
    q=request(CIUKI_ACCEL_CURSOR_MOVE); q.rect.dst_x=-5; q.rect.dst_y=511;
    unsigned at=m.cr_writes;
    CHECK(!fbdev_accel_request(&q)); CHECK(m.cr[0x4e]==5 && m.cr[0x4f]==0);
    CHECK(m.cr[0x46]==0 && m.cr[0x47]==0 && m.cr[0x48]==1 && m.cr[0x49]==255);
    CHECK(m.cr_sequence[at+5]==0x48);
    q=request(CIUKI_ACCEL_CURSOR_SHOW); q.colour=1;
    CHECK(!fbdev_accel_request(&q) && (m.cr[0x45]&1));
    CHECK(!test_engine.ops.stop(&test_engine) && !test_engine.active && !(m.cr[0x45]&1));
    free(expected); finish();
}
static void failures_and_fallback(void)
{
    prepare(32);
    struct ciuki_accel_request q=request(CIUKI_ACCEL_COPY); q.rect=(struct ciuki_rect){4,4,8,8,40,30};
    m.partial=true; m.fail_after_submit=true;
    CHECK(fbdev_accel_request(&q)==-EAGAIN && test_engine.quarantined && m.fault_count==1);
    CHECK(!fbdev_console_begin()); /* no CPU writer until idle proof */
    struct fb_rect r={0,0,10,10};
    CHECK(fbdev_fill(&r,0x102030)==-EIO);
    CHECK(test_engine.ops.stop(&test_engine)==-EIO);
    unsigned packets=m.packets;
    m.stuck=false; m.partial=false;
    CHECK(!fbdev_fill(&r,0x102030)); /* idle now proven, quarantined software fill */
    CHECK(m.packets==packets && m.fault_count==1 && test_engine.quarantined);
    CHECK(fbdev_console_begin()); fbdev_console_end();
    struct savage_io io=model_io();
    CHECK(savage2d_start(&test_engine,&io,&device,true,m.cursor_page,0x100000)==-EIO);
    finish();
    prepare(32); m.stuck=true; m.frozen=true;
    CHECK(test_engine.ops.sync(&test_engine)==-EIO && m.fault_count==1);
    finish();
    prepare(32);
    struct savage2d other={0}; io=model_io(); m.bad_readback=true;
    CHECK(savage2d_start(&other,&io,&device,false,NULL,0)==-EIO && other.quarantined);
    finish();
    prepare(32);io=model_io();other=(struct savage2d){0};
    CHECK(!savage2d_start(&other,&io,&device,false,NULL,0));
    CHECK(m.reg[0x816c/4]==(other.descriptor|9u) && other.ops.capabilities(&other)==1u);
    CHECK(!other.ops.stop(&other));
    m.cr[0x66]=0;other=(struct savage2d){0};
    CHECK(savage2d_start(&other,&io,&device,true,NULL,0)==-ENODEV);
    CHECK(!other.active && !other.quarantined && other.cpu_safe);
    device.bpp=24;CHECK(savage2d_start(&other,&io,&device,true,NULL,0)==-EINVAL);
    device.bpp=32;device.pitch++;CHECK(savage2d_start(&other,&io,&device,true,NULL,0)==-EINVAL);device.pitch--;
    finish();
    struct pci_func p={.class_code=3,.vendor=0x5333,.device=0x8c2e};
    CHECK(savage2d_supported(&p)); p.device=0x8a22; CHECK(savage2d_supported(&p));
    p.device=0x9102; CHECK(!savage2d_supported(&p)); p.device=0x8c2e;p.vendor=0x1234;CHECK(!savage2d_supported(&p));
    prepare(32);
    binding_gpu=(struct pci_func){.class_code=3,.vendor=0x5333,.device=0x8c2e,
        .command=2,.bar={0xe1000000,0xe0000000}};
    binding_pci_count=1;binding_claims=0;
    settings_volume=(struct fat_volume){.mounted=true}; settings_vfs.volumes[2]=&settings_volume;
    CHECK(!savage2d_init(&device,&settings_vfs) && binding_claims==1); /* exclusive ownership conflict */
    binding_gpu.command=0;CHECK(!savage2d_init(&device,&settings_vfs) && binding_claims==1);
    binding_gpu.command=2;binding_gpu.bar[0]|=4;CHECK(!savage2d_init(&device,&settings_vfs) && binding_claims==1); /* 64-bit BAR */
    binding_gpu.bar[0]=0xe1000000;binding_gpu.bar[1]=0xe2000000;CHECK(!savage2d_init(&device,&settings_vfs) && binding_claims==1);
    binding_gpu.bar[1]=device.phys;binding_pci_count=2;CHECK(!savage2d_init(&device,&settings_vfs) && binding_claims==1);
    binding_pci_count=0;finish();
}
static unsigned log_matches(const char *part)
{
    unsigned found=0;
    for (unsigned i=0;i<accel_count && i<128;i++) if (strstr(accel_log[i],part)) found++;
    return found;
}
/* Real MMIO dword glue and frozen production hw_now; VGA indexing is a
 * transport model because a byte array cannot implement indexed registers. */
static void glue_stalled_clock(uint32_t status)
{
    prepare(32);
    uint8_t *window = calloc(1, SAVAGE_MMIO_BYTES); CHECK(window != NULL);
    memcpy(window + SAVAGE_STATUS, &status, sizeof(status));
    registers = window; g_ticks = 0; g_boot.flags = CBI_F_VERBOSE;
    tracing = true; trace_last = UINT32_MAX; step_count = accel_count = 0;
    struct savage_io io = model_io(); io.read32=hw_read; io.write32=hw_write; io.now_us=hw_now;
    struct savage2d e = {0};
    CHECK(savage2d_start(&e, &io, &device, true, NULL, 0) == -EIO);
    CHECK(e.quarantined && !e.touched && !e.active && e.idle_waits == 1 && e.timeouts == 1);
    CHECK(step_count>=14);
    CHECK(!strcmp(step_log[0], "[accel] savage step op=crw reg=00038"));
    CHECK(!strcmp(step_log[step_count-1], "[accel] savage step op=r32 reg=48c60"));
    CHECK(log_matches("savage status reg=48c60 raw=")==1 && !e.idle_mask);
    tracing = false; registers = NULL; g_boot.flags = 0; free(window);
    finish();
}

static void boot_fixture_accel(bool retain_marker)
{
    /* Allocate the actual framebuffer, but make initialization entirely late. */
    prepare(32); acceleration=NULL;
    memset(&m,0,sizeof(m)); m.cr[0x66]=1;
    engine=(struct savage2d){0}; attempt_blocked=false; reg_claim=cursor_claim=-1;
    registers=NULL; register_window=cursor_window=NULL; accel_vfs=NULL; accel_queued=false;
    binding_allowed=true; binding_claims=0; binding_pci_count=1;
    binding_gpu=(struct pci_func){.class_code=3,.vendor=0x5333,.device=0x8c2e,.command=2,
        .bar={0xe1000000,device.phys}};
    settings_volume=(struct fat_volume){.mounted=true};
    memset(&settings_vfs,0,sizeof(settings_vfs)); settings_vfs.volumes[2]=&settings_volume;
    marker_commits=marker_fault=0; marker_error=clear_error=false;
    if (!retain_marker) { disk_size=0; marker_exists=false; memset(disk_marker,0,sizeof(disk_marker)); }
    CHECK(!queued_work && !table_open && !marker_open);
    queue_allowed=true; g_boot.flags=CBI_F_VERBOSE; step_count=accel_count=0;
}
static void run_accel_work(void)
{
    CHECK(queued_work && m.status_reads==0 && !binding_claims);
    kwork_fn fn=queued_work; void *arg=queued_arg; queued_work=NULL;
    fn(arg); CHECK(!accel_queued && !table_open && !marker_open && !locks);
}
static void software_desktop(void)
{
    CHECK(fbdev_get()->present);
    struct fb_rect r={16,16,10,10}; unsigned packets=m.packets;
    CHECK(!fbdev_fill(&r,0x123456));
    uint8_t expected[4]; reference_pixel(&device,expected,0x123456);
    CHECK(!memcmp(device.mapped+16*device.pitch+16*4,expected,4));
    if (!acceleration) {
        CHECK(m.packets==packets);
        struct ciuki_accel_request q=request(CIUKI_ACCEL_QUERY);
        CHECK(fbdev_accel_request(&q)==0);
    }
}
static void deferred_boots(void)
{
    /* Every failure reaches the actual software framebuffer presenter, while
     * the queued callback runs the production marker and register decisions. */
    for (unsigned variant=0;variant<11;variant++) {
        boot_fixture_accel(false);
        if (variant==0) { m.stuck=true; m.frozen=true; }
        if (variant==1) m.dead=true;
        if (variant==2) m.wrong_offset=true;
        if (variant==3) m.locked=true;
        if (variant==4) m.busy=24;
        if (variant==5) { m.reg[BCI_CONTROL/4]=8; m.bci_locked=true; }
        if (variant==6) m.delay_failed=true;
        if (variant==7) { m.busy=8; m.status_value=0x004e0000; }
        if (variant==8) m.status_value=0x00a00000; /* Linux-only cannot prove idle */
        if (variant==9) m.contradictory=true;
        if (variant==10) m.control_reserved=true;
        fbdev_accel_schedule(&settings_vfs); fbdev_accel_schedule(&settings_vfs);
        CHECK(queued_work && !m.cr_writes && !binding_claims);
        software_desktop(); /* desktop can draw even before the callback */
        run_accel_work();
        bool ready=variant==4 || variant==7 || variant==10;
        CHECK(!!acceleration==ready && marker_commits==(ready ? 2u : 1u));
        CHECK(disk_size==(ready ? 0u : 14u) && m.status_reads<=WAIT_POLLS+1);
        CHECK(log_matches(ready ? "[accel] savage ready" : "[accel] savage quarantined reason=")==1);
        if (variant==3 || variant==6) CHECK(!m.status_reads);
        else CHECK(log_matches("savage status reg=48c60 raw=")==1);
        if (!ready) software_desktop();
        if (variant==9) CHECK(log_matches("quarantined reason=status_contradiction")==1);
        printf("deferred savage boot variant=%u ready=%u status_reads=%u marker_bytes=%u desktop=visible\n",
            variant,ready,m.status_reads,disk_size);
        finish();
    }
    /* Simulated hard reset retains only the committed attempt, including on
     * a read-only second mount. Absolutely no register/claim follows it. */
    boot_fixture_accel(false);
    bool previous=false; CHECK(!attempt_marker(&settings_vfs,false,&previous) && !previous && disk_size==14);
    finish(); boot_fixture_accel(true); settings_volume.readonly=true;
    fbdev_accel_schedule(&settings_vfs); run_accel_work();
    CHECK(!acceleration && !binding_claims && !m.cr_writes && !m.status_reads && !marker_commits);
    CHECK(log_matches("skipped after a failed attempt")==1 && log_matches("quarantined reason=previous_attempt")==1);
    software_desktop(); finish();
    for (unsigned variant=0;variant<5;variant++) {
        boot_fixture_accel(false);
        if (variant==0) settings_vfs.volumes[2]=NULL;
        if (variant==1) settings_volume.readonly=true;
        if (variant==2) marker_error=true;
        if (variant==3) { g_boot.flags|=CBI_F_SAFE_MODE; }
        if (variant==4) queue_allowed=false;
        fbdev_accel_schedule(&settings_vfs);
        if (queued_work) run_accel_work();
        CHECK(!acceleration && !binding_claims && !m.cr_writes && !m.status_reads);
        software_desktop(); finish();
    }
    boot_fixture_accel(false); clear_error=true;
    fbdev_accel_schedule(&settings_vfs); run_accel_work();
    CHECK(!acceleration && engine.quarantined && disk_size==14);
    CHECK(log_matches("quarantined reason=marker_clear_failed")==1);
    software_desktop(); finish();
    for (unsigned failure=1;failure<=6;failure++) {
        boot_fixture_accel(false); marker_fault=failure;
        fbdev_accel_schedule(&settings_vfs); run_accel_work();
        CHECK(!acceleration && !binding_claims && !m.cr_writes && !m.status_reads);
        CHECK(log_matches("quarantined reason=marker_unavailable")==1);
        software_desktop(); finish();
    }
    binding_allowed=false; binding_pci_count=0; queued_work=NULL; accel_vfs=NULL;
}
int main(void)
{
    CHECK(!framebuffer_regression_main());
    CHECK(boot_calls==0); /* framebuffer activation never enters the engine */
    commands(16); commands(32); failures_and_fallback();
    glue_stalled_clock(0);            /* never idle, frozen clock */
    glue_stalled_clock(UINT32_MAX);   /* master-aborted (dead) aperture */
    deferred_boots();
    printf("savage2d register/presenter model: %s (%u failures)\n",failures ? "FAIL" : "PASS",failures);
    return failures ? 1 : 0;
}
