/*
	Haiku S3 Virge driver adapted from the X.org Virge driver.

	Copyright (C) 1994-1999 The XFree86 Project, Inc.	All Rights Reserved.

	Copyright 2007-2008 Haiku, Inc.  All rights reserved.
	Distributed under the terms of the MIT license.

	Authors:
	Gerald Zajac 2007-2008
*/
/* Origin: Haiku r1beta5, src/add-ons/accelerants/s3/virge_{draw,init,mode}.cpp
 * and virge.h; a1f00118db8b858feb73f64ac4dc3d038b82bf9b.
 * Native glue: bounded synchronous execution, firmware mode preserved. */
#define S3_RENDER_FAMILY (&render_family)
#include "../s3legacy/private.h"
#include "registers.h"
#include "../render3d.h"
static const struct render3d_family render_family = {
    .status=R3D_NOT_IMPLEMENTED, .potential_units=1, .potential_texture_size=256
};
static bool accept(const struct pci_func *p)
{
    if (!p || p->vendor!=0x5333 || p->class_code!=3) return false;
    return p->device==0x5631 || p->device==0x883d || p->device==0x8a01 ||
        p->device==0x8a10 || p->device==0x8c01 || p->device==0x8c03;
}
static bool format(const struct fb_device *f)
{
    /* The donor exposes RGB565. X.org's packed RGB24 needs STREAMS
     * scanout ownership; do not infer that from a 24-bpp engine opcode. */
    return f->bpp==16 && f->pitch<=0xfff8 && !(f->pitch&7) &&
        f->red_size==5 && f->red_pos==11 && f->green_size==6 && f->green_pos==5 &&
        f->blue_size==5 && f->blue_pos==0;
}
static bool firmware(struct s3_device *s)
{
    struct s3_io *i=&s->io;
    uint8_t mode=i->cr_read(i->ctx,0x67);
    bool rgb565=(mode&0xf0)==0x50 || (s->pci.device==0x883d && (mode&0xf0)==0x40);
    /* STREAMS and a panned desktop require a separate scanout contract. */
    return (mode&0x0c)==0 && rgb565 &&
        (i->cr_read(i->ctx,s->pci.device==0x883d ? 0x63 : 0x66)&1) &&
        ((unsigned)i->cr_read(i->ctx,0x13)|((unsigned)(i->cr_read(i->ctx,0x51)&0x30)<<4))*8==s->fb.pitch &&
        (i->cr_read(i->ctx,0x58)&0x10) &&
        i->cr_read(i->ctx,0x59)==(s->fb.phys>>24) &&
        i->cr_read(i->ctx,0x5a)==((s->fb.phys>>16)&255) &&
        !i->cr_read(i->ctx,0x0c) && !i->cr_read(i->ctx,0x0d) &&
        !(i->cr_read(i->ctx,0x69)&0x0f);
}
static bool available(struct s3_device *s,unsigned slots)
{
    uint32_t v=s->io.read(s->io.ctx,SUBSYS_STAT_REG,32);
    if (v==UINT32_MAX) return false;
    bool gx2=s->pci.device==0x8a10 || s->pci.device==0x8c01 || s->pci.device==0x8c03;
    return slots ? (gx2 ? ((v>>9)&0x60) : ((v>>8)&0x1f))>=slots :
        (v&0x3f00)==0x3000;
}
static void w(struct s3_device *s,unsigned reg,uint32_t value)
{ s->io.write(s->io.ctx,reg,value,32); }
static int configure(struct s3_device *s)
{
    int e=s3_wait(s,6); if (e) return e;
    w(s,CMD_SET,CMD_NOP); /* disable autoexecute, no reset */
    w(s,SRC_BASE,0); w(s,DEST_BASE,0);
    w(s,DEST_SRC_STR,s->fb.pitch|(s->fb.pitch<<16));
    w(s,CLIP_L_R,s->fb.width); w(s,CLIP_T_B,s->fb.height);
    return 0;
}
static unsigned common(const struct s3_device *s)
{ return DRAW|(s->fb.bpp==16 ? DST_16BPP : DST_24BPP); }
static int fill(struct s3_device *s,const struct fb_rect *r,uint32_t colour)
{
    int e=s3_wait(s,4); if (e) return e;
    w(s,PAT_FG_CLR,colour);
    w(s,RWIDTH_HEIGHT,((uint32_t)(r->width-1)<<16)|(unsigned)r->height);
    w(s,RDEST_XY,((uint32_t)r->x<<16)|(unsigned)r->y);
    s3_launch(s);
    w(s,CMD_SET,common(s)|(0xf0u<<17)|CMD_RECT|CMD_XP|CMD_YP);
    return s3_wait(s,0);
}
static int copy(struct s3_device *s,const struct fb_rect *r,int32_t dx,int32_t dy)
{
    /* X.org 211e2831 s3v_accel.c: original ViRGE/VX BLT_BUG.
     * Punt the documented hazardous width band before any submission. */
    int lo=s->fb.bpp==16 ? 26 : 16, hi=s->fb.bpp==16 ? 32 : 22;
    if ((s->pci.device==0x5631 || s->pci.device==0x883d) && r->width>=lo && r->width<=hi)
        return -EOPNOTSUPP;
    unsigned cmd=common(s)|(0xccu<<17)|CMD_BITBLT|CMD_XP|CMD_YP;
    int sx=r->x,sy=r->y;
    if (sx<dx) { sx+=r->width-1; dx+=r->width-1; cmd&=~CMD_XP; }
    if (sy<dy) { sy+=r->height-1; dy+=r->height-1; cmd&=~CMD_YP; }
    int e=s3_wait(s,4); if (e) return e;
    w(s,RWIDTH_HEIGHT,((uint32_t)(r->width-1)<<16)|(unsigned)r->height);
    w(s,RSRC_XY,((uint32_t)sx<<16)|(unsigned)sy);
    w(s,RDEST_XY,((uint32_t)dx<<16)|(unsigned)dy);
    s3_launch(s); w(s,CMD_SET,cmd);
    return s3_wait(s,0);
}
const struct s3_backend s3virge_backend={accept,format,firmware,available,configure,fill,copy,true};
static const struct display_match matches[]={
    {0x5333,0x5631,3,255},{0x5333,0x883d,3,255},{0x5333,0x8a01,3,255},
    {0x5333,0x8a10,3,255},{0x5333,0x8c01,3,255},{0x5333,0x8c03,3,255}
};
S3_ADAPTER(s3virge_driver,"s3virge","S3 ViRGE",s3virge_backend,matches);
