/* The real sweep against a minimal BCI/VRAM transport. Experimental control
 * dependencies are regression scenarios, not claims about S3 silicon. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ciuki/kernel.h>
#include <ciuki/savage2d.h>
unsigned fs_host_contig_fail,fs_host_contig_live;
struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
uint64_t ktime_cycles(void) { return 0; }
int registry_quarantine(int handle,uint32_t generation) { (void)handle;(void)generation;return 0; }
int udelay(uint32_t us) { (void)us;return 0; }
void framebuffer_write_drain(void) {}
bool fbdev_rect_valid(const struct fb_rect *r) { return r && r->x>=0 && r->y>=0 && r->width>0 && r->height>0; }
void klog(const char *fmt,...) { (void)fmt; }
void task_yield(void) {}
struct chip {
    uint32_t reg[SAVAGE_MMIO_BYTES/4],packet[8];
    uint8_t cr[256],sr[256],ci,si,misc,vga;
    uint8_t scratch[SAVAGE_SWEEP_BYTES];
    uint64_t us;
    unsigned words, fills, lines;
    bool hang, busy;
    uint32_t offset;
    unsigned pitch,bpp;
};
static struct chip chip;
static uint32_t rd(void *ctx,uint32_t r)
{ struct chip *c=ctx; c->us++; return r==SAVAGE_STATUS ? c->busy ? 1 : 0x00e00000 : c->reg[r/4]; }
static uint16_t rd16(void *ctx,uint32_t r)
{
    struct chip *c=ctx;
    assert(c->cr[0x31]==0x0c && (c->cr[0x40]&1));
    c->us++; return (uint16_t)(c->reg[r/4]>>((r&2)*8));
}
static void wr16(void *ctx,uint32_t r,uint16_t v)
{ struct chip *c=ctx; unsigned s=(r&2)*8; c->reg[r/4]=(c->reg[r/4]&~(0xffffu<<s))|((uint32_t)v<<s); }
static void wr(void *ctx,uint32_t r,uint32_t v)
{
    struct chip *c=ctx; c->us++;
    if (r<SAVAGE_BCI || r>=SAVAGE_BCI+64) { c->reg[r/4]=v; return; }
    c->packet[c->words++]=v;
    if ((c->packet[0]&0xff000000u)==0x96000000u) {
        if (c->words==2) { c->reg[(0x8168+(c->packet[0]&1)*4)/4]=v; c->words=0; }
    } else if (c->words==4) {
        c->words=0; c->fills++;
        if (c->hang) { c->busy=true; return; }
        uint32_t gbd=c->reg[0x816c/4];
        unsigned bytes=c->bpp/8, stride=(gbd&0x1ff0)*bytes;
        unsigned x=c->packet[2]&4095,y=c->packet[2]>>16;
        unsigned pos=c->reg[0x8168/4]-c->offset+y*stride+x*bytes;
        if (gbd&0x03000000) pos^=2048; /* alternate destination layout */
        assert(pos+4<SAVAGE_SWEEP_BYTES);
        unsigned depth=c->cr[0x50]&0x30;
        unsigned lane=depth==0x10 ? 2 : depth==0x30 ? 4 : 1;
        if ((gbd&0x10000008u)==0x10000008u) lane=1; /* measured T23 config 0 */
        for (unsigned i=0;i<bytes;i++) c->scratch[pos+i]=(uint8_t)(c->packet[1]>>(8*(i%lane)));
    }
}
static uint8_t port_read(void *ctx,uint16_t p)
{
    struct chip *c=ctx;
    switch (p) { case 0x3d4:return c->ci; case 0x3d5:return c->cr[c->ci];
    case 0x3c4:return c->si;case 0x3c5:return c->sr[c->si];case 0x3cc:return c->misc;case 0x3c3:return c->vga; }
    return 0;
}
static void port_write(void *ctx,uint16_t p,uint8_t v)
{
    struct chip *c=ctx;
    switch(p) {case 0x3d4:c->ci=v;break;case 0x3d5:c->cr[c->ci]=v;break;
    case 0x3c4:c->si=v;break;case 0x3c5:c->sr[c->si]=v;break;case 0x3c2:c->misc=v;break;case 0x3c3:c->vga=v;break;}
}
static uint64_t now(void *ctx) { return ((struct chip *)ctx)->us; }
static int delay(void *ctx,uint32_t us) { ((struct chip *)ctx)->us+=us; return 0; }
static void drain(void *ctx) { (void)ctx; }
static void line(void *ctx,const char *s) {
    struct chip *c=ctx;
    if (!strncmp(s,"Savage bpp=",11)) c->lines++;
    else assert(strstr(s,"MM8136 wrote=0007") || strstr(s,"first exact configuration") || strstr(s,"stage=engine-restore"));
}
int main(void)
{
    for (unsigned depth=16;depth<=32;depth+=16) for (unsigned variant=0;variant<3;variant++) {
        memset(&chip,0,sizeof(chip)); chip.cr[0x66]=1; chip.cr[0x45]=0xc0;
        chip.bpp=depth;chip.pitch=1024*(depth/8);chip.offset=0x400000;
        chip.reg[0x8134/4]=0x00070027; chip.reg[0x816c/4]=0x10200409;
        chip.reg[0x8128/4]=0x87654321; chip.sr[0x26]=0x4f;
        memset(chip.scratch,0xa7,sizeof(chip.scratch));
        chip.hang=variant==1;
        struct fb_device d={.present=true,.width=1024,.height=768,.pitch=chip.pitch,
            .bpp=depth,.size=chip.pitch*768};
        struct savage_io io={.ctx=&chip,.read32=rd,.write32=wr,.read16=rd16,.write16=wr16,
            .read_port=port_read,.write_port=port_write,.now_us=now,.delay_us=delay,.drain=drain};
        struct savage2d s={0}; struct savage_sweep_result result;
        int e=savage2d_sweep_model(&s,&io,&d,true,chip.scratch,chip.offset,
            variant==2 ? 1 : 5000000,line,&chip,&result);
        if (variant==1) {
            assert(e && result.tried==1 && !result.restored && s.quarantined && chip.fills==1);
            printf("PASS Savage %ubpp timeout: one fill, no retry, unsafe state retained\n",depth);
        } else {
            assert(result.restored && chip.cr[0x45]==0xc0 && chip.cr[0x66]==1 && chip.sr[0x26]==0x4f);
            assert(chip.reg[0x8128/4]==0x87654321 && chip.reg[0x8134/4]==0x00070027);
            assert(chip.reg[0x816c/4]==0x10200409 && chip.reg[0x8168/4]==0);
            for (unsigned i=0;i<sizeof(chip.scratch);i++) assert(chip.scratch[i]==0xa7);
            if (!variant) {
                assert(!e && result.tried==SAVAGE_CONFIGS && chip.lines==SAVAGE_CONFIGS && result.first==1 && result.passed>0);
                printf("PASS Savage %ubpp matrix: tried=%u passed=%u first=%d VGA/engine/VRAM restored\n",depth,result.tried,result.passed,result.first);
            } else assert(e && !result.tried);
        }
        assert(!fs_host_contig_live);
    }
    puts("PASS Savage deadline: expired sweep stops before first fill and restores state");
    return 0;
}
