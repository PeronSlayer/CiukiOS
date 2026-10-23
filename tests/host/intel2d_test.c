/* Independent ring consumer: literal Intel packet formats, not driver helpers.
 * Pixel goldens use swdib. SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
#include "../../src/kernel/drivers/video/intel2d/intel2d.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)
#define ENTRIES 256u
struct fake {
    struct intel_engine engine;
    uint32_t regs[0x2100/4], ptes[ENTRIES];
    uint8_t pixels[32*20*4+64], golden[32*20*4+64];
    uint32_t memory[INTEL_DMA_BYTES/4];
    uint32_t setup_reg[6], setup_value[6];
    uint64_t now;
    unsigned writes, pte_writes, publishes, tails, commands, fills, copies, stores, reads, pauses;
    unsigned delay, ticks, consumed, row_fail;
    unsigned hic_writes, hic_polls, hic_delay, cache_drains, fail_publish;
    bool hic_stuck, flush_fail, lost_idle_on_disable;
    bool freeze, hung, no_store, busy, stale, bad_tail, bad_gtt, published, cancel, stale_pause;
};
static struct fake f;
static unsigned checks;
static uint16_t fixture_device=0x2582;
static bool gen2(void) { return fixture_device!=0x2582; }
static unsigned completion_words(void) { return gen2() ? 124u : 74u; }
static uint32_t read32(void *ctx, uint32_t reg)
{
    struct fake *m=ctx; m->reads++;
    CHECK(reg/4<sizeof(m->regs)/4);
    return m->regs[reg/4];
}
static uint8_t *gpu_bytes(struct fake *m, uint32_t address, unsigned count)
{
    if (address<m->engine.fb.size) {
        CHECK(count<=m->engine.fb.size-address);
        return m->pixels+address;
    }
    CHECK(address>=m->engine.gpu && address-m->engine.gpu<=INTEL_DMA_BYTES-count);
    unsigned offset=address-m->engine.gpu;
    for (unsigned i=offset/4096;i<=(offset+count-1)/4096;i++)
        CHECK(m->ptes[m->engine.first+i]==0x01000001u+i*4096u);
    return (uint8_t *)m->memory+offset;
}
static void execute(struct fake *m)
{
    if (m->hung) return;
    unsigned at=m->consumed, tail=m->regs[0x2030/4];
    if (at==tail) return;
    CHECK(m->regs[0x203c/4]&1u);
    CHECK(m->regs[0x2080/4]==0x01010000u); /* physical HWS, not GTT */
    unsigned limit=20000;
    while (at!=tail) {
        CHECK(limit-- && !(at&3u));
        uint32_t *p=m->memory+at/4;
        uint32_t opcode=p[0];
        unsigned n=1;
        if (!opcode || opcode==0x02000000u || opcode==0x02000004u) { }
        else if ((opcode&~0x00300000u)==0x54000004u ||
                 (opcode&~0x00300000u)==0x54c00006u) {
            bool copy=(opcode&~0x00300000u)==0x54c00006u;
            n=copy ? 8 : 6;
            CHECK(at+n*4<=65536 && ((p[1]>>16)&255u)==(copy ? 0xccu : 0xf0u));
            unsigned bytes=(p[1]>>24)==3 ? 4 : 2;
            CHECK(bytes==m->engine.fb.bpp/8);
            unsigned x=p[2]&65535u,y=p[2]>>16;
            unsigned w=(p[3]&65535u)-x,h=(p[3]>>16)-y;
            unsigned pitch=p[1]&65535u;
            CHECK(w && h && pitch);
            for (unsigned row=0;row<h;row++) {
                uint8_t *dst=gpu_bytes(m,p[4]+(y+row)*pitch+x*bytes,w*bytes);
                if (copy) {
                    unsigned sp=p[6]&65535u;
                    uint8_t *src=gpu_bytes(m,p[7]+((p[5]>>16)+row)*sp+(p[5]&65535u)*bytes,w*bytes);
                    /* Deliberately forward copy: no memmove or overlap magic. */
                    for (unsigned i=0;i<w*bytes;i++) dst[i]=src[i];
                } else for (unsigned col=0;col<w;col++) for (unsigned b=0;b<bytes;b++)
                    dst[col*bytes+b]=(uint8_t)(p[5]>>(b*8));
            }
            if (copy) m->copies++; else m->fills++;
            m->commands++;
        } else if (opcode==0x10800001u) {
            n=3; CHECK(at+12<=65536);
            CHECK(p[1]==0x100u || p[1]==0x200u);
            if (!m->no_store) m->memory[65536/4+p[1]/4]=p[2];
            m->stores++;
        } else { fprintf(stderr,"unknown packet %08x at %u\n",opcode,at); abort(); }
        at=(at+n*4)%65536;
    }
    m->consumed=tail; m->regs[0x2034/4]=tail;
    if (!m->busy) {
        m->regs[0x209c/4]=gen2() ? 0 : 1u<<9;
        m->regs[0x2090/4]=0x01ffffc0u;
    }
}
static void write32(void *ctx, uint32_t reg, uint32_t value)
{
    struct fake *m=ctx;
    if (reg==0x70) {
        CHECK(gen2() && m->cache_drains>m->hic_writes && value==(0x1234u|1u<<31));
        m->hic_writes++; m->hic_polls=0; m->regs[reg/4]=value;
        if (!m->hic_delay && !m->hic_stuck) m->regs[reg/4]&=~(1u<<31);
        return;
    }
    if (m->writes<6) { m->setup_reg[m->writes]=reg; m->setup_value[m->writes]=value; }
    m->writes++;
    CHECK(reg==0x2030 || reg==0x2034 || reg==0x2038 || reg==0x203c || reg==0x2080);
    if (reg==0x203c && !value) {
        CHECK(m->regs[0x2030/4]==m->regs[0x2034/4]);
        CHECK(gen2() ? (m->regs[0x2090/4]&0x01ffffc0u)==0x01ffffc0u :
            (m->regs[0x209c/4]&(1u<<9))!=0);
        if (m->lost_idle_on_disable) m->regs[gen2() ? 0x2090/4 : 0x209c/4]=0;
    }
    m->regs[reg/4]=value;
    if (reg==0x2030 && (m->regs[0x203c/4]&1u)) {
        CHECK(m->published && !(value&7) && value<65536);
        m->published=false; m->tails++; m->ticks=0;
        m->regs[0x209c/4]=0;
        m->regs[0x2090/4]=0;
        if (m->row_fail && m->tails>=m->row_fail) m->hung=true;
        if (!m->delay) execute(m);
        if (m->bad_tail) m->regs[reg/4]^=8;
    }
}
static uint32_t pte(void *ctx, unsigned i)
{ struct fake *m=ctx; CHECK(i<ENTRIES); return m->ptes[i]; }
static void set_pte(void *ctx, unsigned i, uint32_t p)
{
    struct fake *m=ctx; CHECK(i>=m->engine.first && i<ENTRIES);
    if (!(p&1u)) CHECK(!(m->regs[0x203c/4]&1u) && (gen2() ?
        (m->regs[0x2090/4]&0x01ffffc0u)==0x01ffffc0u : (m->regs[0x209c/4]&(1u<<9))!=0));
    m->pte_writes++; if (!m->bad_gtt) m->ptes[i]=p;
}
static int publish(void *ctx, const void *p, unsigned bytes, struct intel_wait *wait)
{
    struct fake *m=ctx;
    if (bytes) CHECK((const uint8_t *)p>=(uint8_t *)m->memory &&
        (const uint8_t *)p+bytes<=(uint8_t *)m->memory+sizeof(m->memory));
    m->publishes++; m->published=true;
    if (m->flush_fail || m->publishes==m->fail_publish) return -ETIMEDOUT;
    if (gen2()) {
        m->cache_drains++;
        return intel_hic_flush(&m->engine.io,wait,m->engine.pause,m->engine.pause_arg);
    }
    return 0;
}
static uint32_t status(void *ctx, volatile uint32_t *p)
{ (void)ctx; return *p; }
static uint64_t now(void *ctx) { return ((struct fake *)ctx)->now; }
static void relax(void *ctx)
{
    struct fake *m=ctx;
    if (!m->freeze) m->now+=100;
    if ((m->regs[0x70/4]&(1u<<31)) && !m->hic_stuck && ++m->hic_polls>=m->hic_delay)
        m->regs[0x70/4]&=~(1u<<31);
    if (++m->ticks>=m->delay && (m->regs[0x203c/4]&1u)) execute(m);
}
static bool valid(void *ctx) { return !((struct fake *)ctx)->stale; }
static int pause_wait(void *ctx)
{ struct fake *m=ctx; m->pauses++; if (m->stale_pause) m->stale=true; return m->cancel ? -EINTR : 0; }
static void setup(unsigned bpp)
{
    memset(&f,0,sizeof(f));
    struct fb_device d={.width=32,.height=20,.pitch=32*(bpp/8),.size=20*32*(bpp/8),
        .phys=0xc0000000u,.mapped=f.pixels,.bpp=bpp,.present=true,.handle=1,.generation=42,
        .red_size=bpp==16?5:8,.red_pos=bpp==16?11:16,.green_size=bpp==16?6:8,
        .green_pos=bpp==16?5:8,.blue_size=bpp==16?5:8};
    f.regs[0x2020/4]=0x08000001;
    f.regs[0x209c/4]=gen2() ? 0 : 1u<<9;
    f.regs[0x2090/4]=0x01ffffc0u;
    f.regs[0x70/4]=0x1234u;
    f.regs[0x2080/4]=0x07000000; f.regs[0x2038/4]=0x00400000;
    for (unsigned i=0;i<ENTRIES-INTEL_DMA_PAGES;i++) f.ptes[i]=0x08000001u+i*4096;
    for (unsigned i=ENTRIES-INTEL_DMA_PAGES;i<ENTRIES;i++) f.ptes[i]=0xa5a50000; /* invalid saved bits */
    struct intel_io io={&f,read32,write32,pte,set_pte,publish,status,now,relax,valid};
    CHECK(!intel_reserve(&f.engine,&d,&io,f.memory,0x01000000,ENTRIES,fixture_device));
    CHECK(!f.writes && !f.pte_writes && !f.publishes);
    CHECK(!intel_engine_start(&f.engine));
    const uint32_t regs[]={0x2080,0x2030,0x2034,0x2038,0x203c,0x2030};
    const uint32_t values[]={0x01010000,0,0,(ENTRIES-25u)*4096u,0xf001,completion_words()*4u};
    CHECK(f.writes==6 && !memcmp(f.setup_reg,regs,sizeof(regs)) &&
        !memcmp(f.setup_value,values,sizeof(values)));
    CHECK(f.engine.cpu_safe && f.engine.ops.capabilities(&f.engine)==1);
    CHECK(!f.engine.ops.cursor_set && !f.engine.ops.pattern_rect && !f.engine.ops.write_rect && !f.engine.ops.offscreen);
}
static void restored(void)
{
    CHECK(!intel_engine_stop(&f.engine));
    CHECK(f.engine.cpu_safe && !f.engine.mapped && !f.engine.active);
    CHECK(f.regs[0x2030/4]==0 && f.regs[0x2034/4]==0 && f.regs[0x203c/4]==0);
    CHECK(f.regs[0x2038/4]==0x00400000 && f.regs[0x2080/4]==0x07000000);
    for (unsigned i=0;i<ENTRIES;i++)
        CHECK(f.ptes[i]==(i<ENTRIES-INTEL_DMA_PAGES ? 0x08000001u+i*4096 : 0xa5a50000));
    checks++;
}
static void seed(void)
{
    for (unsigned i=0;i<sizeof(f.pixels);i++) f.pixels[i]=(uint8_t)(i*73+i/11);
    memcpy(f.golden,f.pixels,sizeof(f.pixels));
}
static void golden(void)
{
    for (unsigned i=0;i<sizeof(f.pixels);i++) if (f.pixels[i]!=f.golden[i]) {
        fprintf(stderr,"golden at %u: got %02x expected %02x bpp=%u fills=%u copies=%u\n",
            i,f.pixels[i],f.golden[i],f.engine.fb.bpp,f.fills,f.copies);
        CHECK(false);
    }
    checks++;
}
static void drawing(void)
{
    for (unsigned bpp=16;bpp<=32;bpp+=16) {
        setup(bpp); seed();
        struct fb_device surface=f.engine.fb;
        surface.mapped=f.golden;
        struct fb_rect r={2,3,14,9};
        uint32_t colour=bpp==16 ? 0xba98 : 0xfedcba98;
        CHECK(!f.engine.ops.fill_rect(&f.engine,&r,colour));
        swdib_fill(&surface,&r,colour); golden();
        unsigned first=completion_words();
        CHECK(f.memory[first]==(bpp==32 ? 0x54300004u : 0x54000004u));
        const uint32_t expected[]={bpp==32 ? 0x54300004u : 0x54000004u,
            (bpp==32 ? 0x03f00000u : 0x01f00000u)|surface.pitch,
            0x00030002,0x000c0010,0,colour};
        CHECK(!memcmp(f.memory+first,expected,sizeof(expected)));
        if (gen2()) {
            CHECK(f.memory[first+6]==0x02000000u);
            for (unsigned i=0;i<12;i++) {
                const uint32_t flush[]={0x10800001u,0x200u,0,0x02000004u};
                CHECK(!memcmp(f.memory+first+7+i*4,flush,sizeof(flush)));
            }
            CHECK(f.memory[first+55]==0x02000000u);
        }
        unsigned bread=first+7+(gen2() ? 50 : 0);
        for (unsigned i=0;i<24;i++) {
            CHECK(f.memory[bread+i*3]==0x10800001u);
            CHECK(f.memory[bread+1+i*3]==(i<16 ? 0x200u : 0x100u));
            CHECK(f.memory[bread+2+i*3]==2);
        }
        CHECK(f.stores==(gen2() ? 72u : 48u) && f.fills==1);
        for (unsigned i=0;i<8;i++) {
            static const int offsets[8][2]={{2,0},{-2,0},{0,2},{0,-2},{2,2},{-2,-2},{2,-2},{-2,2}};
            seed(); r=(struct fb_rect){6,5,16,10};
            int dx=r.x+offsets[i][0], dy=r.y+offsets[i][1];
            CHECK(!f.engine.ops.copy_rect(&f.engine,&r,dx,dy));
            swdib_copy(&surface,&r,dx,dy); golden();
        }
        seed(); r=(struct fb_rect){0,0,32,20};
        CHECK(!f.engine.ops.copy_rect(&f.engine,&r,0,0));
        golden();
        unsigned tails=f.tails;
        r=(struct fb_rect){-1,0,2,1};
        CHECK(f.engine.ops.fill_rect(&f.engine,&r,3)==-EOPNOTSUPP && f.tails==tails);
        r=(struct fb_rect){0,0,2,1};
        CHECK(f.engine.ops.copy_rect(&f.engine,&r,31,0)==-EOPNOTSUPP && f.tails==tails);
        restored();
    }
}
static void timeouts(void)
{
    for (unsigned which=0;which<5;which++) {
        setup(32); f.hung=which==0 || which==1; f.freeze=which==1;
        f.no_store=which==2; f.busy=which==3;
        if (which==4) { f.delay=1000; f.cancel=true; f.engine.ops.set_pause(&f.engine,pause_wait,&f); }
        struct fb_rect r={0,0,8,4};
        CHECK(f.engine.ops.fill_rect(&f.engine,&r,0x102030)!=0);
        CHECK(f.engine.quarantined && !f.engine.cpu_safe && f.engine.mapped);
        unsigned ptes=f.pte_writes, tails=f.tails;
        CHECK(intel_engine_stop(&f.engine)!=0);
        CHECK(f.pte_writes==ptes && f.tails==tails && !f.engine.cpu_safe);
        CHECK(!f.engine.ops.capabilities(&f.engine));
        CHECK(f.engine.ops.fill_rect(&f.engine,&r,0)==-EIO && f.tails==tails);
        if (which==1) CHECK(f.reads<INTEL_POLL_LIMIT*30u);
        checks++;
    }
    setup(32); f.hung=true;
    CHECK(f.engine.ops.sync(&f.engine)==-ETIMEDOUT);
    /* Later observational recovery: no command replay. */
    unsigned tails=f.tails;
    f.hung=false; execute(&f);
    restored(); CHECK(f.tails==tails && f.engine.quarantined);
    CHECK(intel_engine_start(&f.engine)==-ENODEV);
    setup(32); f.row_fail=f.tails+2;
    struct fb_rect r={2,2,16,6};
    CHECK(f.engine.ops.copy_rect(&f.engine,&r,4,4)==-EAGAIN);
    CHECK(f.engine.quarantined && !f.engine.cpu_safe);
}
static void ring_cases(void)
{
    setup(32);
    f.engine.tail=65400; f.consumed=65400;
    f.regs[0x2030/4]=f.regs[0x2034/4]=65400;
    CHECK(!f.engine.ops.sync(&f.engine));
    CHECK(f.engine.tail==completion_words()*4u);
    for (unsigned i=65400/4;i<65536/4;i++) CHECK(!f.memory[i]);
    restored();
    setup(32); f.regs[0x2034/4]=f.engine.tail+8; f.hung=true;
    unsigned tails=f.tails;
    CHECK(f.engine.ops.sync(&f.engine)==-ETIMEDOUT && f.tails==tails);
    setup(32); f.regs[0x2034/4]=65536;
    CHECK(f.engine.ops.sync(&f.engine)==-EIO);
    setup(32); f.stale=true; tails=f.tails;
    CHECK(f.engine.ops.sync(&f.engine)==-EIO && f.tails==tails);
    setup(32); f.bad_tail=true;
    CHECK(f.engine.ops.sync(&f.engine)==-EIO && !f.engine.cpu_safe);
    setup(32); f.delay=30; f.engine.ops.set_pause(&f.engine,pause_wait,&f);
    CHECK(!f.engine.ops.sync(&f.engine) && f.pauses>=2 && f.engine.cpu_safe);
    restored();
    setup(32); f.ptes[f.engine.first]^=4096; tails=f.tails;
    CHECK(f.engine.ops.sync(&f.engine)==-EIO && f.tails==tails);
    setup(32); f.delay=30; f.stale_pause=true;
    f.engine.ops.set_pause(&f.engine,pause_wait,&f);
    CHECK(f.engine.ops.sync(&f.engine)==-EIO && !f.engine.cpu_safe && f.pauses==1);
    checks+=7;
}
static void refusal(void)
{
    setup(32); restored();
    struct fb_device d=f.engine.fb;
    struct intel_io io=f.engine.io;
    memset(&f.engine,0,sizeof(f.engine));
    unsigned writes=f.writes, ptes=f.pte_writes;
    f.ptes[ENTRIES-1]|=1;
    CHECK(intel_reserve(&f.engine,&d,&io,f.memory,0x01000000,ENTRIES,fixture_device)==-ENOSPC);
    CHECK(f.writes==writes && f.pte_writes==ptes);
    f.ptes[ENTRIES-1]&=~1u; d.bpp=24;
    CHECK(intel_reserve(&f.engine,&d,&io,f.memory,0x01000000,ENTRIES,fixture_device)==-EINVAL);
    d.bpp=32; d.pitch=0xffff;
    CHECK(intel_reserve(&f.engine,&d,&io,f.memory,0x01000000,ENTRIES,fixture_device)==-EINVAL);
    d.pitch=128; f.ptes[5]=0x01000001;
    CHECK(intel_reserve(&f.engine,&d,&io,f.memory,0x01000000,ENTRIES,fixture_device)==-EBUSY);
    f.ptes[5]=0x08005001;
    CHECK(!intel_reserve(&f.engine,&d,&io,f.memory,0x01000000,ENTRIES,fixture_device));
    f.regs[0x203c/4]=1;
    CHECK(intel_engine_start(&f.engine)==-EBUSY);
    CHECK(f.writes==writes && f.pte_writes==ptes);
    f.regs[0x203c/4]=0; f.bad_gtt=true;
    CHECK(intel_engine_start(&f.engine)==-EIO && f.engine.quarantined);
    CHECK(f.writes==writes); /* GTT readback failed before any engine write */
    f.bad_gtt=false; f.stale=true;
    unsigned before_restore=f.pte_writes;
    CHECK(intel_engine_stop(&f.engine)==-EIO && f.pte_writes==before_restore && !f.engine.cpu_safe);
    f.stale=false; CHECK(!intel_engine_stop(&f.engine));
    CHECK(f.engine.cpu_safe && !f.engine.mapped);
    checks+=7;
}
static void gen2_protocol(void)
{
    setup(32);
    /* Gen3 MODE_IDLE must neither be needed nor substitute for INSTDONE. */
    CHECK(!f.regs[0x209c/4] && f.hic_writes==3 && f.regs[0x70/4]==0x1234u);
    for (unsigned bit=6;bit<=24;bit++) {
        setup(32); f.busy=true; f.regs[0x209c/4]=1u<<9;
        CHECK(f.engine.ops.sync(&f.engine)==-ETIMEDOUT);
        /* A matching breadcrumb and HEAD == TAIL do not excuse one busy unit. */
        f.regs[0x209c/4]=1u<<9;
        f.regs[0x2090/4]=0x01ffffc0u&~(1u<<bit);
        unsigned tails=f.tails, ptes=f.pte_writes;
        CHECK(intel_engine_stop(&f.engine)==-ETIMEDOUT && !f.engine.cpu_safe);
        CHECK(f.tails==tails && f.pte_writes==ptes && f.engine.mapped);
        f.regs[0x2090/4]=0x01ffffc0u;
        restored(); CHECK(f.tails==tails);
    }
    setup(32); f.hic_delay=3;
    CHECK(!f.engine.ops.sync(&f.engine) && f.hic_polls==3);
    restored();
    setup(32); f.hic_delay=3; f.regs[0x70/4]|=1u<<31;
    unsigned hic_writes=f.hic_writes;
    CHECK(!f.engine.ops.sync(&f.engine) && f.hic_writes==hic_writes+1);
    restored();
    setup(32); f.hic_delay=30; f.engine.ops.set_pause(&f.engine,pause_wait,&f);
    CHECK(!f.engine.ops.sync(&f.engine) && f.pauses>=2);
    restored();
    setup(32); f.hic_delay=30; f.cancel=true; f.engine.ops.set_pause(&f.engine,pause_wait,&f);
    unsigned unpublished=f.tails;
    CHECK(f.engine.ops.sync(&f.engine)==-EINTR && f.engine.quarantined && f.tails==unpublished);
    CHECK(!f.engine.cpu_safe && f.engine.mapped && f.pauses==1);
    for (unsigned freeze=0;freeze<2;freeze++) {
        setup(32); f.hic_stuck=true; f.freeze=freeze!=0;
        unsigned tails=f.tails, ptes=f.pte_writes;
        CHECK(f.engine.ops.sync(&f.engine)==-ETIMEDOUT);
        CHECK(f.tails==tails && f.engine.quarantined && !f.engine.cpu_safe);
        CHECK(intel_engine_stop(&f.engine)==-ETIMEDOUT && f.pte_writes==ptes);
        CHECK(f.tails==tails && f.engine.mapped && !f.engine.cpu_safe);
        f.hic_stuck=false; f.regs[0x70/4]&=~(1u<<31);
        restored(); CHECK(f.tails==tails);
    }
    setup(32); f.hic_stuck=true; f.regs[0x70/4]|=1u<<31;
    hic_writes=f.hic_writes;
    CHECK(intel_engine_stop(&f.engine)==-ETIMEDOUT);
    CHECK(f.engine.mapped && f.engine.quarantined && !f.engine.cpu_safe && f.hic_writes==hic_writes);
    setup(32); f.no_store=true;
    unsigned stop_tails=f.tails, stop_ptes=f.pte_writes;
    CHECK(intel_engine_stop(&f.engine)==-ETIMEDOUT);
    CHECK(f.tails==stop_tails+1 && f.pte_writes==stop_ptes && f.engine.quarantined &&
        f.engine.mapped && !f.engine.cpu_safe && (f.regs[0x203c/4]&1u));
    setup(32); f.freeze=true; f.hic_delay=60000;
    struct fb_rect many_rows={0,0,8,4};
    stop_tails=f.tails;
    CHECK(f.engine.ops.copy_rect(&f.engine,&many_rows,2,2)==-EAGAIN);
    CHECK(f.tails==stop_tails+1 && f.engine.quarantined && !f.engine.cpu_safe);
    setup(32); f.lost_idle_on_disable=true;
    unsigned ptes=f.pte_writes;
    CHECK(intel_engine_stop(&f.engine)==-EIO && f.engine.mapped && !f.engine.cpu_safe);
    CHECK(f.pte_writes==ptes);
    setup(32); f.fail_publish=f.publishes+2; /* final GTT restoration flush */
    CHECK(intel_engine_stop(&f.engine)==-ETIMEDOUT && f.engine.mapped && !f.engine.cpu_safe);
    CHECK(f.engine.quarantined && !(f.regs[0x203c/4]&1u));
    unsigned restore_tails=f.tails;
    f.fail_publish=0; restored(); CHECK(f.tails==restore_tails);
    /* A flush transport failure also cannot publish TAIL or authorize CPU. */
    setup(32); f.flush_fail=true;
    unsigned tails=f.tails;
    CHECK(f.engine.ops.sync(&f.engine)==-ETIMEDOUT && f.tails==tails && !f.engine.cpu_safe);
    f.flush_fail=false; restored(); CHECK(f.tails==tails);
    setup(32); restored();
    struct fb_device d=f.engine.fb;
    struct intel_io io=f.engine.io;
    memset(&f.engine,0,sizeof(f.engine));
    CHECK(!intel_reserve(&f.engine,&d,&io,f.memory,0x01000000,ENTRIES,fixture_device));
    unsigned writes=f.writes; ptes=f.pte_writes;
    f.regs[0x2090/4]&=~(1u<<7);
    CHECK(intel_engine_start(&f.engine)==-EBUSY && f.writes==writes && f.pte_writes==ptes);
    f.regs[0x2090/4]=0x01ffffc0u; f.regs[0x70/4]|=1u<<31;
    CHECK(intel_engine_start(&f.engine)==-EBUSY && f.writes==writes && f.pte_writes==ptes);
    checks+=34;
}
static void gate_table(void)
{
    static const struct { uint16_t id; bool enabled; uint16_t bridge; } gates[]={
        {0x3577,false,0x3575},{0x2562,true,0x2560},{0x3582,true,0x3580},
        {0x2572,false,0x2570},{0x2582,true,0x2580}
    };
    setup(32); restored();
    struct fb_device d=f.engine.fb;
    struct intel_io io=f.engine.io;
    for (unsigned i=0;i<ARRAY_SIZE(gates);i++) {
        const struct intel_chip *c=intel_chip_lookup(gates[i].id);
        CHECK(c && c->bridge==gates[i].bridge && (!c->refusal)==gates[i].enabled);
        memset(&f.engine,0,sizeof(f.engine));
        unsigned writes=f.writes, ptes=f.pte_writes;
        CHECK(intel_reserve(&f.engine,&d,&io,f.memory,0x01000000,ENTRIES,gates[i].id)==
            (gates[i].enabled ? 0 : -ENODEV));
        CHECK(f.writes==writes && f.pte_writes==ptes);
    }
    CHECK(!intel_chip_lookup(0x2782) && !intel_chip_lookup(0x2592));
    checks+=5;
}
static void unqualified_chips(void)
{
    const uint16_t ids[]={0x3577,0x2572};
    setup(32); restored();
    struct fb_device d=f.engine.fb;
    struct intel_io io=f.engine.io;
    memset(&f.engine,0,sizeof(f.engine));
    /* A ready, disabled engine must not turn shared Gen2 completion evidence
     * into permission to publish through an unproved chipset interface. */
    static struct fake before;
    memcpy(&before,&f,sizeof(f));
    for (unsigned i=0;i<ARRAY_SIZE(ids);i++) {
        CHECK(intel_reserve(&f.engine,&d,&io,f.memory,0x01000000,ENTRIES,ids[i])==-ENODEV);
        CHECK(!memcmp(&before,&f,sizeof(f))); /* callbacks, GTT, HWS, ring, pixels */
        /* Refusal must also precede argument inspection and any callbacks. */
        CHECK(intel_reserve(0,0,0,0,0,0,ids[i])==-ENODEV);
        checks++;
    }
}
int main(void)
{
    const uint16_t enabled[]={0x2582,0x2562,0x3582};
    for (unsigned i=0;i<ARRAY_SIZE(enabled);i++) {
        fixture_device=enabled[i];
        drawing(); timeouts(); ring_cases(); refusal();
        if (gen2()) gen2_protocol();
    }
    gate_table();
    unqualified_chips();
    printf("intel2d: PASS (%u scenarios; register ordering, breadcrumbs, swdib goldens, guards)\n",checks);
    return 0;
}
