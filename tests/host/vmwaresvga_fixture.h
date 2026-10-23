/* Independent protocol model, not emulator code. The parser applies raw
 * protocol bytes; swdib is used only by the comparison side of the tests. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/kernel/drivers/video/vmwaresvga/engine.h"
#include <ciuki/swdib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%u: %s\n",__FILE__,__LINE__,#x); assert(x); } } while (0)
struct svga_fake {
    uint32_t regs[64], fifo[16384/4];
    uint8_t pixels[32*32*4], cursor_mask[128];
    uint32_t cursor_rgb[1024], last[1100];
    unsigned last_words, packets, writes, fifo_writes, drains, busy_reads, pauses;
    unsigned remain, delayed, syncs, definitions, hides, shows;
    struct { unsigned reg; uint32_t value; } trace[128];
    unsigned trace_count;
    unsigned sequence, last_data, last_drain;
    uint64_t clock;
    bool wedge, frozen, revoked, corrupt_pixels, suppress_hide;
    bool early_parse, configured, traces;
    struct vmsvga_engine *engine;
    struct fb_device fb;
};
static struct svga_fake fake;
struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
static uint32_t pop_word(struct svga_fake *f,uint32_t *at)
{
    CHECK(*at>=f->fifo[SVGA_FIFO_MIN] && *at<f->fifo[SVGA_FIFO_MAX] && !(*at&3));
    uint32_t value=f->fifo[*at/4];
    *at+=4;
    if (*at==f->fifo[SVGA_FIFO_MAX]) *at=f->fifo[SVGA_FIFO_MIN];
    return value;
}
static void consume(struct svga_fake *f)
{
    uint32_t at=f->fifo[SVGA_FIFO_STOP],next=f->fifo[SVGA_FIFO_NEXT_CMD];
    unsigned count=0;
    while (at!=next) {
        CHECK(count<1100); f->last[count++]=pop_word(f,&at);
    }
    f->last_words=count;
    if (!count) return;
    CHECK(f->configured && f->engine && !f->engine->cpu_safe);
    uint32_t *p=f->last;
    unsigned bytes=f->fb.bpp/8;
    if (p[0]==2) {
        CHECK(count==6);
        CHECK((uint64_t)p[2]+p[4]<=f->fb.width && (uint64_t)p[3]+p[5]<=f->fb.height);
        for (unsigned y=0;y<p[5];y++) for (unsigned x=0;x<p[4];x++)
            for (unsigned b=0;b<bytes;b++) f->pixels[(p[3]+y)*f->fb.pitch+(p[2]+x)*bytes+b]=(uint8_t)(p[1]>>(b*8));
    } else if (p[0]==3) {
        CHECK(count==7);
        uint8_t snapshot[sizeof(f->pixels)]; memcpy(snapshot,f->pixels,sizeof(snapshot));
        CHECK((uint64_t)p[1]+p[5]<=f->fb.width && (uint64_t)p[3]+p[5]<=f->fb.width);
        CHECK((uint64_t)p[2]+p[6]<=f->fb.height && (uint64_t)p[4]+p[6]<=f->fb.height);
        for (unsigned y=0;y<p[6];y++) for (unsigned x=0;x<p[5];x++)
            memcpy(f->pixels+(p[4]+y)*f->fb.pitch+(p[3]+x)*bytes,
                   snapshot+(p[2]+y)*f->fb.pitch+(p[1]+x)*bytes,bytes);
    } else if (p[0]==19) {
        CHECK(count==1064 && !p[1] && !p[2] && !p[3] && p[4]==32 && p[5]==32 && p[6]==1 && p[7]==32);
        CHECK(!f->engine->cursor_visible);
        for (unsigned i=0;i<128;i++) f->cursor_mask[i]=(uint8_t)(p[8+i/4]>>((i%4)*8));
        memcpy(f->cursor_rgb,p+40,sizeof(f->cursor_rgb)); f->definitions++;
    } else CHECK(0);
    if (f->corrupt_pixels) f->pixels[0]^=1;
    f->fifo[SVGA_FIFO_STOP]=at; f->packets++;
}
static uint32_t fake_read(void *ctx,unsigned reg)
{
    struct svga_fake *f=ctx; CHECK(reg<64);
    if (reg==SVGA_REG_BUSY && f->regs[reg] && f->configured) {
        f->busy_reads++;
        if (!f->wedge && !f->remain--) { consume(f); f->regs[reg]=0; }
    }
    return f->regs[reg];
}
static void fake_write(void *ctx,unsigned reg,uint32_t value)
{
    struct svga_fake *f=ctx; CHECK(reg<64); f->writes++;
    CHECK(f->trace_count<ARRAY_SIZE(f->trace));
    f->trace[f->trace_count].reg=reg; f->trace[f->trace_count++].value=value;
    f->sequence++;
    if (reg==SVGA_REG_CURSOR_ON) {
        if (!value) { f->hides++; if (f->suppress_hide) return; }
        else f->shows++;
    }
    f->regs[reg]=value;
    if (reg==SVGA_REG_CONFIG_DONE) { f->configured=!!value; f->traces=false; }
    if (reg==SVGA_REG_ENABLE) { CHECK(value==1); f->traces=!f->configured; }
    if (reg==SVGA_REG_SYNC) { f->syncs++; f->regs[SVGA_REG_BUSY]=1; f->remain=f->delayed; }
}
static uint32_t fake_fifo_read(void *ctx,unsigned off)
{ CHECK(!(off&3) && off<sizeof(fake.fifo)); return ((struct svga_fake *)ctx)->fifo[off/4]; }
static void fake_fifo_write(void *ctx,unsigned off,uint32_t value)
{
    struct svga_fake *f=ctx; CHECK(!(off&3) && off<sizeof(f->fifo)); f->fifo_writes++;
    if (off==4*SVGA_FIFO_NEXT_CMD && f->engine && f->engine->initialized) {
        CHECK(f->last_drain>f->last_data && !f->engine->cpu_safe); f->early_parse=true;
    }
    if (off>=f->fifo[SVGA_FIFO_MIN] && off>=16) f->last_data=++f->sequence;
    if (off==4*SVGA_FIFO_CURSOR_ON && !value && f->suppress_hide) return;
    f->fifo[off/4]=value;
}
static void fake_drain(void *ctx)
{ struct svga_fake *f=ctx; f->drains++; f->last_drain=++f->sequence; }
static uint64_t fake_now(void *ctx) { return ((struct svga_fake *)ctx)->clock; }
static int fake_delay(void *ctx,unsigned us)
{ struct svga_fake *f=ctx; if (!f->frozen) f->clock+=us; return 0; }
static bool fake_valid(void *ctx) { return !((struct svga_fake *)ctx)->revoked; }
static struct vmsvga_io fake_io(void)
{
    return (struct vmsvga_io){&fake,fake_read,fake_write,fake_fifo_read,fake_fifo_write,
        fake_drain,fake_now,fake_delay,fake_valid};
}
static void fake_setup(unsigned bpp,bool extended)
{
    memset(&fake,0,sizeof(fake)); memset(&g_boot,0,sizeof(g_boot)); g_ticks=0;
    fake.fb=(struct fb_device){.width=32,.height=32,.pitch=32*(bpp/8),.size=32*32*(bpp/8),
        .phys=0xe0000000,.mapped=fake.pixels,.bpp=bpp,.present=true,.handle=0,.generation=42,
        .red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,
        .green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,.blue_size=bpp==16 ? 5 : 8};
    fake.regs[SVGA_REG_ID]=SVGA_ID_2; fake.regs[SVGA_REG_ENABLE]=1;
    fake.regs[SVGA_REG_WIDTH]=32; fake.regs[SVGA_REG_HEIGHT]=32;
    fake.regs[SVGA_REG_BITS_PER_PIXEL]=bpp; fake.regs[SVGA_REG_BYTES_PER_LINE]=fake.fb.pitch;
    fake.regs[SVGA_REG_FB_START]=fake.fb.phys; fake.regs[SVGA_REG_FB_SIZE]=8*1024*1024;
    fake.regs[SVGA_REG_RED_MASK]=bpp==16 ? 0xf800 : 0xff0000;
    fake.regs[SVGA_REG_GREEN_MASK]=bpp==16 ? 0x7e0 : 0xff00;
    fake.regs[SVGA_REG_BLUE_MASK]=bpp==16 ? 0x1f : 0xff;
    fake.regs[SVGA_REG_CAPABILITIES]=VMSVGA_CAP_FILL|SVGA_CAP_RECT_COPY|SVGA_CAP_CURSOR|SVGA_CAP_CURSOR_BYPASS_2;
    fake.regs[SVGA_REG_MEM_START]=0xe8000000;
    fake.regs[SVGA_REG_MEM_SIZE]=sizeof(fake.fifo);
    if (extended) {
        fake.regs[SVGA_REG_CAPABILITIES]|=SVGA_CAP_EXTENDED_FIFO|SVGA_CAP_ALPHA_CURSOR|SVGA_CAP_3D;
        fake.regs[SVGA_REG_MEM_REGS]=SVGA_FIFO_NUM_REGS;
        fake.fifo[SVGA_FIFO_CAPABILITIES]=SVGA_FIFO_CAP_CURSOR_BYPASS_3|SVGA_FIFO_CAP_RESERVE|SVGA_FIFO_CAP_FENCE;
    }
    fake.traces=true;
}
