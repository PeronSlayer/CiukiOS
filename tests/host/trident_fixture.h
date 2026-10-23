/* Independent local-VRAM register interpreter, not a call into swdib.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
#include "../../src/kernel/drivers/video/trident/engine.h"
struct tb_trace { unsigned reg,bits; uint32_t value; };
struct tb_fixture {
    struct fb_device fb;
    enum tb_kind kind;
    uint8_t pixels[8192*32],regs[512];
    struct tb_trace trace[1024];
    unsigned traces,reads,commands,busy_reads;
    bool busy,dead,busy_after,corrupt;
    uint64_t time,step;
};
static void store32(uint8_t *p,uint32_t v) { memcpy(p,&v,4); }
static uint32_t reg32(struct tb_fixture *f,unsigned r)
{ uint32_t v; memcpy(&v,f->regs+(r-0x2000),4); return v; }
static void tb_emulate(struct tb_fixture *f)
{
    unsigned sx=0,sy=0,dx,dy,w,h;
    bool is_fill,reverse_x=false,reverse_y=false;
    uint32_t colour;
    if (f->kind==TB_BLADE) {
        uint32_t cmd=reg32(f,0x2144),a=reg32(f,0x2108),b=reg32(f,0x210c);
        is_fill=(cmd&0xe0000000u)==0x20000000u;
        reverse_x=reverse_y=!!(cmd&2u);
        unsigned ax=a&0xffff,ay=a>>16,bx=b&0xffff,by=b>>16;
        dx=reverse_x ? bx : ax; dy=reverse_y ? by : ay;
        w=(ax>bx ? ax-bx : bx-ax)+1; h=(ay>by ? ay-by : by-ay)+1;
        uint32_t src=reg32(f,reverse_x ? 0x2104 : 0x2100);
        sx=src&0xffff; sy=src>>16;
        colour=reg32(f,0x2160);
    } else {
        uint32_t flags=reg32(f,0x2128),dim=reg32(f,0x2140),dst=reg32(f,0x2138),src=reg32(f,0x213c);
        is_fill=!!(flags&0x4000); reverse_x=!!(flags&0x200); reverse_y=!!(flags&0x100);
        w=dim>>16; h=dim&0xffff; dx=dst>>16; dy=dst&0xffff; sx=src>>16; sy=src&0xffff;
        if (reverse_x) { sx-=w-1; dx-=w-1; }
        if (reverse_y) { sy-=h-1; dy-=h-1; }
        colour=reg32(f,0x2158);
    }
    unsigned bytes=f->fb.bpp/8;
    assert(w && h && dx+w<=f->fb.width && dy+h<=f->fb.height);
    if (!is_fill) assert(sx+w<=f->fb.width && sy+h<=f->fb.height);
    if (!f->corrupt) for (unsigned j=0;j<h;j++) for (unsigned i=0;i<w;i++) {
        unsigned x=reverse_x ? w-1-i : i,y=reverse_y ? h-1-j : j;
        uint8_t *dst=f->pixels+(dy+y)*f->fb.pitch+(dx+x)*bytes;
        if (is_fill) memcpy(dst,&colour,bytes);
        else memcpy(dst,f->pixels+(sy+y)*f->fb.pitch+(sx+x)*bytes,bytes);
    }
    f->commands++;
    if (f->busy_after) f->busy=true;
}
static uint32_t fixture_status(struct tb_fixture *f)
{
    f->reads++;
    if (f->dead) return UINT32_MAX;
    bool busy=f->busy;
    if (f->busy_reads) { f->busy_reads--; busy=true; }
    return busy ? (f->kind==TB_XP ? 0x80u : 0x80000000u) : 0;
}
static uint8_t fixture_r8(void *ctx,unsigned r)
{ struct tb_fixture *f=ctx; return r==TB_STATUS ? fixture_status(f) : f->regs[r-0x2000]; }
static uint32_t fixture_r32(void *ctx,unsigned r)
{ struct tb_fixture *f=ctx; return r==TB_STATUS ? fixture_status(f) : reg32(f,r); }
static void fixture_write(struct tb_fixture *f,unsigned r,uint32_t v,unsigned bits)
{
    assert(f->traces<1024 && r>=0x2000 && r+bits/8<=0x2200);
    f->trace[f->traces++]=(struct tb_trace){r,bits,v};
    if (bits==8) f->regs[r-0x2000]=v;
    else store32(f->regs+(r-0x2000),v);
    if ((f->kind==TB_BLADE && r==0x210c) || (f->kind==TB_XP && r==0x2124 && bits==8)) tb_emulate(f);
}
static void fixture_w8(void *ctx,unsigned r,uint8_t v) { fixture_write(ctx,r,v,8); }
static void fixture_w32(void *ctx,unsigned r,uint32_t v) { fixture_write(ctx,r,v,32); }
static uint64_t fixture_now(void *ctx) { struct tb_fixture *f=ctx; f->time+=f->step; return f->time; }
static struct tb_io fixture_io(struct tb_fixture *f)
{ return (struct tb_io){f,fixture_r8,fixture_r32,fixture_w8,fixture_w32,fixture_now,NULL}; }
static void fixture_reset(struct tb_fixture *f,enum tb_kind kind,unsigned bpp,unsigned pitch)
{
    memset(f,0,sizeof(*f)); f->kind=kind;
    f->fb=(struct fb_device){.width=32,.height=32,.pitch=pitch,.size=pitch*32,
        .phys=0xe0000000,.mapped=f->pixels,.bpp=bpp,.present=true,.handle=0,.generation=1,
        .red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,
        .green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,
        .blue_size=bpp==16 ? 5 : 8,.blue_pos=0};
    for (unsigned i=0;i<f->fb.size;i++) f->pixels[i]=(i*73+i/11)&255;
}
