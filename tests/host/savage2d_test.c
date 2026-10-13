/* Production register driver + production framebuffer renderer, ASan/UBSan.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main framebuffer_regression_main
#include "fbdev_test.c"
#undef main
#include <ciuki/savage2d.h>
static unsigned binding_claims, binding_pci_count;
static struct pci_func binding_gpu;
static int binding_claim(enum res_type t,uint32_t s,uint32_t e,const char *o,bool share)
{ binding_claims++;return registry_claim(t,s,e,o,share); }
#define registry_claim binding_claim
#include "../../src/kernel/drivers/savage2d.c"
#undef registry_claim

unsigned pci_count(void) { return binding_pci_count; }
const struct pci_func *pci_get(unsigned i) { return i<binding_pci_count ? &binding_gpu : NULL; }
void *vmm_map_mmio(uint32_t p, uint32_t n, bool uc) { (void)p; (void)n; (void)uc; return NULL; }
void *vmm_replace_mmio(void *w,uint32_t c,uint32_t p,uint32_t n) { (void)w;(void)c;(void)p;(void)n;return NULL; }

struct model {
    uint32_t reg[SAVAGE_MMIO_BYTES/4], packet[16];
    uint8_t cr[256], cursor_page[4096], colour[2][3];
    unsigned words, expected, packets, fault_count, busy, cr_writes, colour_at;
    uint8_t cr_sequence[128];
    bool stuck, frozen, bad_readback, fail_after_submit, partial;
    uint64_t us;
};
static struct model m;
static struct savage2d test_engine;
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
    CHECK(offset<SAVAGE_MMIO_BYTES && !(offset&3u));
    if (offset==SAVAGE_STATUS) {
        if (p->stuck) return 0;
        if (p->busy) { p->busy--; return 0x00e00001u; }
        return 0x00e00000u;
    }
    if (offset==0x816c && p->bad_readback) return p->reg[offset/4]^16u;
    return p->reg[offset/4];
}
static void model_write(void *ctx,uint32_t offset,uint32_t value)
{
    struct model *p=ctx;
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
    } else p->reg[offset/4]=value;
}
static uint8_t model_cr_read(void *ctx,uint8_t index)
{
    struct model *p=ctx;
    if (index==0x45) p->colour_at=0;
    return p->cr[index];
}
static void model_cr_write(void *ctx,uint8_t index,uint8_t value)
{
    struct model *p=ctx;
    if (p->cr_writes<sizeof(p->cr_sequence)) p->cr_sequence[p->cr_writes]=index;
    p->cr_writes++;
    if (index==0x4a || index==0x4b) {
        CHECK(p->colour_at<3);
        p->colour[index==0x4b][p->colour_at++]=value;
    }
    p->cr[index]=value;
}
static uint64_t model_now(void *ctx)
{
    struct model *p=ctx;
    if (!p->frozen) p->us+=100;
    return p->us;
}
static void model_relax(void *ctx) { (void)ctx; }
static void model_drain(void *ctx) { (void)ctx; framebuffer_write_drain(); }
static void model_fault(void *ctx) { ((struct model *)ctx)->fault_count++; }
static struct savage_io model_io(void)
{
    return (struct savage_io){&m,model_read,model_write,model_cr_read,model_cr_write,
        model_now,model_relax,model_drain,model_fault};
}
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
    m.cr[0x66]=0;other=(struct savage2d){0};unsigned writes=m.cr_writes;
    CHECK(savage2d_start(&other,&io,&device,true,NULL,0)==-ENODEV);
    CHECK(m.cr_writes==writes && !other.active && !other.quarantined && other.cpu_safe);
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
    CHECK(!savage2d_init(&device) && binding_claims==1); /* exclusive ownership conflict */
    binding_gpu.command=0;CHECK(!savage2d_init(&device) && binding_claims==1);
    binding_gpu.command=2;binding_gpu.bar[0]|=4;CHECK(!savage2d_init(&device) && binding_claims==1); /* 64-bit BAR */
    binding_gpu.bar[0]=0xe1000000;binding_gpu.bar[1]=0xe2000000;CHECK(!savage2d_init(&device) && binding_claims==1);
    binding_gpu.bar[1]=device.phys;binding_pci_count=2;CHECK(!savage2d_init(&device) && binding_claims==1);
    binding_pci_count=0;finish();
}
/* T23 stall (2026-10-13): the production register glue with a clock that
 * never advances. The wait is bounded by WAIT_POLLS, so a silent stop on
 * hardware is one register access that never completes, not a software
 * loop. The breadcrumbs show the first engine-domain access: the status
 * read MM48C60, preceded only by CRTC accesses and no engine reset. */
static void glue_stalled_clock(uint32_t status)
{
    prepare(32);
    uint8_t *window = calloc(1, SAVAGE_MMIO_BYTES); CHECK(window != NULL);
    window[0x83d5] = 1;                                  /* CR66.0 reads set */
    memcpy(window + SAVAGE_STATUS, &status, sizeof(status));
    registers = window; g_ticks = 0; g_boot.flags = CBI_F_VERBOSE;
    tracing = true; trace_last = UINT32_MAX; step_count = 0;
    struct savage_io io = {0, hw_read, hw_write, hw_cr_read, hw_cr_write, hw_now, hw_relax, hw_drain, hw_fault};
    struct savage2d e = {0};
    CHECK(savage2d_start(&e, &io, &device, true, NULL, 0) == -EIO);
    CHECK(e.quarantined && !e.touched && !e.active && e.idle_waits == 1 && e.timeouts == 1);
    CHECK(step_count == 2);
    CHECK(!strcmp(step_log[0], "[accel] savage step op=crr reg=00066"));
    CHECK(!strcmp(step_log[1], "[accel] savage step op=r32 reg=48c60"));
    tracing = false; registers = NULL; g_boot.flags = 0; free(window);
    finish();
}
int main(void)
{
    CHECK(!framebuffer_regression_main());
    commands(16); commands(32); failures_and_fallback();
    glue_stalled_clock(0);            /* never idle, frozen clock */
    glue_stalled_clock(UINT32_MAX);   /* master-aborted (dead) aperture */
    printf("savage2d register/presenter model: %s (%u failures)\n",failures ? "FAIL" : "PASS",failures);
    return failures ? 1 : 0;
}
