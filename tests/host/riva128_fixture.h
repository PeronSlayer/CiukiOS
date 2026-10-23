/* Fake NV3 register device, independent pixel interpreter. Not hardware evidence.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
#include "../../src/kernel/drivers/video/riva128/nv3.h"
#define FAKE_BYTES (NV3_INSTANCE+NV3_INSTANCE_BYTES)
struct fake_write { uint32_t reg,value; unsigned width; };
struct fake_nv3 {
    uint32_t *regs;
    uint8_t cr[256],seq[256],cr_index,seq_index,attr_index,pan;
    uint8_t pixels[32*32*4],gold[32*32*4];
    struct fb_device fb;
    struct fake_write trace[16000];
    unsigned writes,drains,delays,pauses,reads,launches;
    uint64_t time;
    uint32_t starve,dead_register;
    unsigned busy;
    bool frozen,ignore_cursor,no_draw,corrupt_cursor,hang_launch;
};
static struct fake_nv3 fake;
static void trace(struct fake_nv3 *f,uint32_t r,uint32_t v,unsigned width)
{
    assert(f->writes<sizeof(f->trace)/sizeof(f->trace[0]));
    f->trace[f->writes++]=(struct fake_write){r,v,width};
}
static uint32_t f_read(void *ctx,uint32_t r)
{
    struct fake_nv3 *f=ctx; f->reads++;
    assert(!(r&3u) && r<FAKE_BYTES);
    if (f->dead_register && r==f->dead_register) return UINT32_MAX;
    if (r==NV3_PGRAPH+0x6b0 && f->hang_launch && f->launches) return 1;
    if (r==NV3_PGRAPH+0x6b0 && f->busy) { f->busy--; return 1; }
    if (f->corrupt_cursor && r>=NV3_CURSOR && r<NV3_INSTANCE+NV3_INSTANCE_BYTES)
        return f->regs[r/4]^1u;
    return f->regs[r/4];
}
static uint32_t native_pattern(struct fake_nv3 *f,uint32_t value)
{
    if (f->fb.bpp==32) return value&0xffffffu;
    if (f->fb.green_size==5) return value&0x7fffu;
    return ((value>>19)&31u)<<11 | ((value>>10)&63u)<<5 | ((value>>3)&31u);
}
static uint32_t f_pixel(struct fake_nv3 *f,unsigned x,unsigned y)
{
    uint32_t v=0;
    memcpy(&v,f->pixels+y*f->fb.pitch+x*(f->fb.bpp/8),f->fb.bpp/8);
    return v;
}
static void f_store(struct fake_nv3 *f,unsigned x,unsigned y,uint32_t v)
{ memcpy(f->pixels+y*f->fb.pitch+x*(f->fb.bpp/8),&v,f->fb.bpp/8); }
static void launch(struct fake_nv3 *f,uint32_t reg,uint32_t value)
{
    f->launches++;
    if (f->hang_launch) f->busy=NV3_POLL_CAP*4;
    if (f->no_draw) return;
    if (reg==NV3_BITMAP+0x404) {
        uint32_t xy=f->regs[(NV3_BITMAP+0x400)/4];
        unsigned x=xy>>16,y=xy&0xffff,w=value>>16,h=value&0xffff;
        assert(x+w<=f->fb.width && y+h<=f->fb.height);
        uint32_t rop=f->regs[(NV3_ROP+0x300)/4];
        assert(rop==0xcc || rop==0xf0);
        for (unsigned yy=y;yy<y+h;yy++) for (unsigned xx=x;xx<x+w;xx++) {
            uint32_t c=f->regs[(NV3_BITMAP+0x3fc)/4];
            if (rop==0xf0) {
                unsigned row=yy%8,bit=row%4*8+xx%8;
                uint32_t word=f->regs[(NV3_PATTERN+0x318+row/4*4)/4];
                c=native_pattern(f,f->regs[(NV3_PATTERN+((word>>bit)&1u ? 0x314 : 0x310))/4]);
            }
            f_store(f,xx,yy,c);
        }
    } else {
        uint32_t src=f->regs[(NV3_BLT+0x300)/4],dst=f->regs[(NV3_BLT+0x304)/4];
        unsigned sx=src&0xffff,sy=src>>16,dx=dst&0xffff,dy=dst>>16,w=value&0xffff,h=value>>16;
        assert(sx+w<=f->fb.width && sy+h<=f->fb.height && dx+w<=f->fb.width && dy+h<=f->fb.height);
        uint32_t saved[32*32];
        for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) saved[y*w+x]=f_pixel(f,sx+x,sy+y);
        for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) f_store(f,dx+x,dy+y,saved[y*w+x]);
    }
}
static void f_write(void *ctx,uint32_t reg,uint32_t value)
{
    struct fake_nv3 *f=ctx;
    assert(!(reg&3u) && reg<FAKE_BYTES);
    if (reg>=NV3_CURSOR && reg<NV3_INSTANCE+NV3_INSTANCE_BYTES)
        assert(!(f->cr[0x31]&1u)); /* hide before every replacement */
    trace(f,reg,value,32);
    if (reg!=NV3_PFIFO+0x400) f->regs[reg/4]=value; /* read-only runout status */
    if (reg==NV3_BITMAP+0x404 || reg==NV3_BLT+0x308) launch(f,reg,value);
}
static uint16_t f_read16(void *ctx,uint32_t reg)
{
    struct fake_nv3 *f=ctx; f->reads++;
    assert(reg>=NV3_FIFO && (reg-NV3_FIFO)%0x2000==0x10);
    return f->starve==UINT32_MAX || f->starve==reg-0x10 ? 0 : 0x100;
}
static uint8_t f_read8(void *ctx,uint32_t reg)
{
    struct fake_nv3 *f=ctx;
    if (reg==0x6013c0) return f->attr_index;
    if (reg==0x6013c1) { assert((f->attr_index&31)==0x13); return f->pan; }
    if (reg==0x6013da) return 0;
    if (reg==NV3_CRTC_INDEX) return f->cr_index;
    if (reg==NV3_CRTC_DATA) return f->cr[f->cr_index];
    if (reg==NV3_SEQ_INDEX) return f->seq_index;
    assert(reg==NV3_SEQ_DATA); return f->seq[f->seq_index];
}
static void f_write8(void *ctx,uint32_t reg,uint8_t value)
{
    struct fake_nv3 *f=ctx; trace(f,reg,value,8);
    if (reg==0x6013c0) { f->attr_index=value; return; }
    if (reg==NV3_CRTC_INDEX) f->cr_index=value;
    else if (reg==NV3_CRTC_DATA) {
        /* Extended CRTC writes require the sequencer unlock. */
        if (f->seq[6]==0x57 && !f->ignore_cursor) f->cr[f->cr_index]=value;
    } else if (reg==NV3_SEQ_INDEX) f->seq_index=value;
    else { assert(reg==NV3_SEQ_DATA); f->seq[f->seq_index]=value; }
}
static uint64_t f_now(void *ctx) { return ((struct fake_nv3 *)ctx)->time; }
static int f_delay(void *ctx,uint32_t us)
{ struct fake_nv3 *f=ctx; f->delays++; if (!f->frozen) f->time+=us; return 0; }
static void f_drain(void *ctx) { ((struct fake_nv3 *)ctx)->drains++; }
static struct nv3_io fake_io(void)
{ return (struct nv3_io){&fake,f_read,f_write,f_read16,f_read8,f_write8,f_now,f_delay,f_drain}; }
static void fake_reset(unsigned format)
{
    free(fake.regs); memset(&fake,0,sizeof(fake));
    fake.regs=calloc(1,FAKE_BYTES); assert(fake.regs);
    unsigned bytes=format==32 ? 4 : 2;
    fake.fb=(struct fb_device){.width=32,.height=32,.pitch=32*bytes,.size=32*32*bytes,
        .mapped=fake.pixels,.bpp=bytes*8,.present=true,.red_size=bytes==4 ? 8 : 5,
        .red_pos=bytes==4 ? 16 : format==15 ? 10 : 11,.green_size=bytes==4 ? 8 : format==15 ? 5 : 6,
        .green_pos=bytes==4 ? 8 : 5,.blue_size=bytes==4 ? 8 : 5,.blue_pos=0,
        .phys=0xe0000000,.handle=0,.generation=7};
    for (unsigned i=0;i<sizeof(fake.pixels);i++) fake.pixels[i]=(uint8_t)(i*97u+3u);
    fake.regs[(NV3_PFB+0x200)/4]=0x1001u | (bytes==4 ? 3u : 2u)<<8;
    fake.regs[NV3_PFB/4]=2; /* SGRAM, 4 MiB */
    fake.regs[(NV3_PFIFO+0x1014)/4]=0x10;
    fake.regs[(NV3_PFIFO+0x1214)/4]=0x10;
    fake.regs[(NV3_PFIFO+0x400)/4]=0x10;
    fake.seq[6]=0x99; fake.seq_index=4; fake.cr_index=0x12;
    fake.cr[0x31]=0x78;
    fake.attr_index=0x20;
    fake.regs[0x200/4]=0x11100000u;
}
static struct fb_device golden(void)
{
    memcpy(fake.gold,fake.pixels,fake.fb.size);
    struct fb_device g=fake.fb; g.mapped=fake.gold; return g;
}
static void same_pixels(void) { assert(!memcmp(fake.pixels,fake.gold,fake.fb.size)); }
static unsigned writes_to(uint32_t reg)
{
    unsigned n=0;
    for (unsigned i=0;i<fake.writes;i++) if (fake.trace[i].reg==reg) n++;
    return n;
}
static void last_write(uint32_t reg,uint32_t value)
{
    for (unsigned i=fake.writes;i;i--) if (fake.trace[i-1].reg==reg) {
        assert(fake.trace[i-1].value==value); return;
    }
    assert(0 && "missing register write");
}
static bool cancelled(void *ctx) { (void)ctx; return true; }
