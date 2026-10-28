/* The real sweep against a minimal BCI/VRAM transport. Experimental control
 * dependencies are regression scenarios, not claims about S3 silicon. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <ciuki/kernel.h>
#include "../../src/kernel/drivers/video/s3savage/savage2d.h"
#include <ciuki/registry.h>
unsigned fs_host_contig_fail,fs_host_contig_live;
struct ciuki_boot_info g_boot;
volatile uint64_t g_ticks;
bool g_cpu_tsc;
uint64_t g_tsc_per_ms;
uint64_t ktime_cycles(void) { return 0; }
int registry_quarantine(int handle,uint32_t generation) { (void)handle;(void)generation;return 0; }
const struct resource *registry_get(unsigned handle) { (void)handle;assert(0);return 0; }
int registry_quiesce(int h,gen_t g,registry_idle_proof_fn proof)
{ (void)h;(void)g;(void)proof;assert(0);return -EIO; }
int registry_release(int h,uint32_t g) { (void)h;(void)g;assert(0);return -EIO; }
int registry_offer_takeover(int h,gen_t g,const char *owner,registry_idle_proof_fn proof)
{ (void)h;(void)g;(void)owner;(void)proof;assert(0);return -EIO; }
void vmm_revoke_mmio(void *p,uint32_t bytes) { (void)p;(void)bytes;assert(0); }
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
    unsigned words, fills, lines, checkpoints, reset_edges, reset_delay;
    unsigned active_id, durable_id, result_lines, fail_checkpoint;
    bool hang, busy, stuck, descriptor_hang, freeze, delay_fail, result_pending;
    bool config3, checkpoint_failed, supersavage, setup, winner_pending, wait_2d;
    unsigned winners, durable_winners;
    uint64_t line_cost;
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
    assert(!c->result_pending || c->checkpoint_failed); /* cleanup allowed after sink failure */
    /* Production setup now fences the queued descriptor before the sweep.
     * This transport executes synchronously; a busy engine must not publish
     * the tag beyond WAIT_2D. The delayed-parser model lives in savage2d_test. */
    if (!c->words && v==0xc0020000u) { c->wait_2d=true; return; }
    if (!c->words && (v&0xffff0000u)==0x98000000u) {
        assert(c->wait_2d && (v&0xffffu)); c->wait_2d=false;
        if (!c->busy) c->reg[SAVAGE_EVENT_STATUS/4]=v&0xffffu;
        return;
    }
    c->packet[c->words++]=v;
    if ((c->packet[0]&0xff000000u)==0x96000000u) {
        if (c->words==2) {
            c->reg[(0x8168+(c->packet[0]&1)*4)/4]=v; c->words=0;
            if (c->descriptor_hang && !c->setup && c->active_id==4) { c->descriptor_hang=false; c->busy=true; }
        }
    } else if (c->words==4) {
        c->words=0; c->fills++;
        assert(!c->checkpoint_failed);
        assert(c->active_id==c->durable_id);
        uint32_t desc=c->reg[0x816c/4];
        assert((desc&0x10000000) && !(desc&0x03000000)); /* excluded BEFORE submission */
        if (c->hang && c->active_id==4) { c->hang=false; c->busy=true; return; }
        uint32_t gbd=c->reg[0x816c/4];
        unsigned bytes=c->bpp/8, stride=(gbd&0x1ff0)*bytes;
        unsigned x=c->packet[2]&4095,y=c->packet[2]>>16;
        unsigned pos=c->reg[0x8168/4]-c->offset+y*stride+x*bytes;
        if (gbd&0x03000000) pos^=2048; /* alternate destination layout */
        assert(pos+4<SAVAGE_SWEEP_BYTES);
        unsigned depth=c->cr[0x50]&0x30;
        unsigned lane=depth==0x10 ? 2 : depth==0x30 ? 4 : 1;
        if (c->supersavage && (gbd&0x10000008u)==0x10000008u) lane=1; /* measured T23 config 0 */
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
    switch(p) {case 0x3d4:c->ci=v;break;case 0x3d5:
        if (c->ci==0x66 && ((v^c->cr[c->ci])&2)) {
            if (v&2) { c->reset_edges++;c->reset_delay=0; }
            else {
                assert(c->reset_delay>=10000 || c->delay_fail);
                if (!c->stuck) { c->busy=false;c->words=0; }
            }
        }
        c->cr[c->ci]=v;break;
    case 0x3c4:c->si=v;break;case 0x3c5:c->sr[c->si]=v;break;case 0x3c2:c->misc=v;break;case 0x3c3:c->vga=v;break;}
}
static uint64_t now(void *ctx) { return ((struct chip *)ctx)->freeze ? 0 : ((struct chip *)ctx)->us; }
static int delay(void *ctx,uint32_t us) {
    struct chip *c=ctx;c->us+=us;
    if (c->cr[0x66]&2) c->reset_delay+=us;
    return c->delay_fail && c->reset_edges>1 ? -EIO : 0;
}
static void drain(void *ctx) { (void)ctx; }
static void line(void *ctx,const char *s) {
    struct chip *c=ctx;
    c->us+=c->line_cost;
    if (!strncmp(s,"Savage bpp=",11)) {
        unsigned id;assert(sscanf(s,"Savage bpp=%*u config=%u",&id)==1);
        assert(strstr(s,"bit3=") && strstr(s,"BW_DISABLE=") && strstr(s,"stride=") &&
            strstr(s,"CR50=") && strstr(s,"tile=") && strstr(s,"depth=") && strstr(s,"descriptor="));
        c->lines++;c->active_id=id;
        c->setup=strstr(s,"phase=setup")!=0;
        if (strstr(s,"first_exact=1")) {
            assert(strstr(s,"result=pass") && !c->winners);
            c->winners++;c->winner_pending=true;
        }
        if (strstr(s,"result=begin")) c->durable_id=UINT32_MAX;
        else { c->result_pending=true;c->result_lines++; }
        if (id==3) {
            assert(strstr(s,"bit3=0 BW_DISABLE=0 stride=pixels") && strstr(s,"tile=0") &&
                strstr(s,"result=skip reason=block_write_unqualified"));
            c->config3=true;
        }
    }
    else assert(strstr(s,"MM8136 wrote=0007") || strstr(s,"stage=engine-restore"));
}
static int checkpoint(void *ctx)
{
    struct chip *c=ctx;c->checkpoints++;
    if (c->checkpoints==c->fail_checkpoint) { c->checkpoint_failed=true;return -EIO; }
    if (c->winner_pending) { c->durable_winners++;c->winner_pending=false; }
    c->durable_id=c->active_id;c->result_pending=false;return 0;
}
int main(void)
{
    for (unsigned depth=16;depth<=32;depth+=16) for (unsigned variant=0;variant<12;variant++) {
        memset(&chip,0,sizeof(chip)); chip.cr[0x66]=1; chip.cr[0x45]=0xc0;
        chip.bpp=depth;chip.pitch=1024*(depth/8);chip.offset=0x400000;
        chip.supersavage=variant!=10;
        if (variant==11) { chip.pitch=depth==16 ? 8160 : 8192;chip.offset=0x800000; }
        chip.reg[0x8134/4]=0x00070027; chip.reg[0x816c/4]=0x10200409;
        chip.reg[0x8128/4]=0x87654321; chip.sr[0x26]=0x4f;
        memset(chip.scratch,0xa7,sizeof(chip.scratch));
        /* Fault config 4 after config 1's winner, so recovery/cutoff tests
         * still prove that an earlier exact result survives the failure. */
        chip.hang=variant==1 || variant==3 || variant==6 || variant==7;
        chip.stuck=variant==3;chip.descriptor_hang=variant==4;
        chip.fail_checkpoint=variant==5 ? 2 : variant==9 ? 5 : 0;
        chip.freeze=variant==6;chip.delay_fail=variant==7;
        chip.line_cost=variant==8 ? 1000 : 0;
        struct fb_device d={.present=true,.width=1024,.height=768,.pitch=chip.pitch,
            .bpp=depth,.size=chip.pitch*768};
        struct savage_io io={.ctx=&chip,.read32=rd,.write32=wr,.read16=rd16,.write16=wr16,
            .read_port=port_read,.write_port=port_write,.now_us=now,.delay_us=delay,.drain=drain};
        struct savage2d s={0}; struct savage_sweep_result result;
        int e=savage2d_sweep_model(&s,&io,&d,chip.supersavage,chip.scratch,chip.offset,
            variant==2 ? 1 : variant==8 ? 30000 : 5000000,line,checkpoint,&chip,&result);
        if (variant==3 || variant==7) {
            assert(e && result.tried==3 && !result.restored && s.quarantined && chip.fills==3);
            assert(chip.reset_edges==2 && !(chip.cr[0x66]&2));
            printf("PASS Savage %ubpp failed recovery variant=%u: no further fill, reset deasserted, unsafe state retained\n",depth,variant);
        } else {
            assert(result.restored && chip.cr[0x45]==0xc0 && chip.cr[0x66]==1 && chip.sr[0x26]==0x4f);
            assert(chip.reg[0x8128/4]==0x87654321 && chip.reg[0x8134/4]==0x00070027);
            assert(chip.reg[0x816c/4]==0x10200409 && chip.reg[0x8168/4]==0);
            for (unsigned i=0;i<sizeof(chip.scratch);i++) assert(chip.scratch[i]==0xa7);
            if (variant==11) {
                unsigned tried=depth==16 ? 0 : 12;
                assert(!e && result.tried==tried && result.completed==SAVAGE_CONFIGS && result.skipped==SAVAGE_CONFIGS-tried);
                printf("PASS Savage %ubpp padded pitch=%u: bounded destination/control overlap exclusions, tried=%u skip=%u\n",
                    depth,chip.pitch,result.tried,result.skipped);
            } else if (variant==0 || variant==1 || variant==4 || variant==6 || variant==10) {
                unsigned tried=variant==10 ? 6 : 12;
                bool timeout=variant==1 || variant==4 || variant==6;
                assert(result.tried==tried && result.completed==SAVAGE_CONFIGS && result.skipped==SAVAGE_CONFIGS-tried && chip.config3);
                assert(chip.lines==chip.checkpoints && result.passed>0);
                assert(result.timeouts==timeout && chip.reset_edges==1u+timeout);
                assert((e==-ETIMEDOUT)==timeout && result.first==(chip.supersavage ? 1 : 0));
                assert(chip.winners==1 && chip.durable_winners==1);
                printf("PASS Savage %ubpp matrix variant=%u: tried=%u passed=%u skip=%u timeout=%u done=%u checkpoint-before-submit, state restored\n",
                    depth,variant,result.tried,result.passed,result.skipped,result.timeouts,result.completed);
            } else if (variant==5 || variant==2) assert(e && !result.tried && !chip.fills);
            else if (variant==9) assert(e && result.tried==2 && chip.fills==2 && result.first==1 && result.passed==1 &&
                chip.winners==1 && !chip.durable_winners);
            else assert(e && result.tried && result.completed<SAVAGE_CONFIGS && result.passed);
        }
        assert(!fs_host_contig_live);
    }
    puts("PASS Savage deadlines/checkpoints: expired or failed pre-command checkpoint issues no fill; slow evidence truncates and retains earlier results");
    return 0;
}

/* fx-r3dalloc moved large driver buffers to fs_contig_alloc; this harness
 * links no fs_port.c, so it supplies zeroed host memory. */
__attribute__((weak)) void *fs_contig_alloc(size_t n) { return calloc(1, n ? n : 1); }
__attribute__((weak)) void fs_contig_free(void *p, size_t n) { (void)n; free(p); }
