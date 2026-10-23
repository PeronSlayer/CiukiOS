/*
	Haiku S3 Trio64 driver adapted from the X.org S3 driver.

	Copyright 2001	Ani Joshi <ajoshi@unixbox.com>

	Copyright 2008 Haiku, Inc.  All rights reserved.
	Distributed under the terms of the MIT license.

	Authors:
	Gerald Zajac 2008
*/
/* Origin: Haiku r1beta5, src/add-ons/accelerants/s3/trio64_{draw,init,mode}.cpp
 * and trio64.h; a1f00118db8b858feb73f64ac4dc3d038b82bf9b.
 * Native glue: bounded synchronous execution, firmware mode preserved. */
#include "../s3legacy/private.h"
#include "registers.h"
static bool accept(const struct pci_func *p)
{
    return p && p->vendor==0x5333 && p->class_code==3 &&
        (p->device==0x8811 || p->device==0x8814 || p->device==0x8901);
}
static bool format(const struct fb_device *f)
{
    /* The chosen donor exposes packed RGB565, not packed RGB24/32. */
    if (f->bpp!=16 || f->red_size!=5 || f->red_pos!=11 || f->green_size!=6 ||
        f->green_pos!=5 || f->blue_size!=5 || f->blue_pos || f->pitch!=f->width*2) return false;
    return f->width==640 || f->width==800 || f->width==1024 ||
        f->width==1152 || f->width==1280 || f->width==1600;
}
static bool firmware(struct s3_device *s)
{
    struct s3_io *i=&s->io;
    unsigned width=s->fb.width;
    uint8_t cr50=width==640 ? 0x40 : width==800 ? 0x80 : width==1024 ? 0 :
        width==1152 ? 1 : width==1280 ? 0xc0 : 0x81;
    /* Shared PCI ID: revision bit 6 distinguishes Trio64V+. No mode writes. */
    bool original=s->pci.device==0x8811 && !(s->pci.revision&0x40);
    return !(original && width>=1600) &&
        (i->cr_read(i->ctx,0x40)&1) &&
        ((unsigned)i->cr_read(i->ctx,0x13)|((unsigned)(i->cr_read(i->ctx,0x51)&0x30)<<4))*8==s->fb.pitch &&
        (i->cr_read(i->ctx,0x58)&0x10) &&
        i->cr_read(i->ctx,0x59)==(s->fb.phys>>24) &&
        i->cr_read(i->ctx,0x5a)==((s->fb.phys>>16)&255) &&
        (i->cr_read(i->ctx,0x50)&0xf1)==(cr50|0x10) &&
        (i->cr_read(i->ctx,0x67)&0xf0)==0x50 &&
        i->cr_read(i->ctx,0x0c)==0 && i->cr_read(i->ctx,0x0d)==0 &&
        !(i->cr_read(i->ctx,0x31)&0x30) && !(i->cr_read(i->ctx,0x51)&3);
}
static bool available(struct s3_device *s,unsigned slots)
{
    uint32_t v=s->io.read(s->io.ctx,GP_STAT,16);
    if (v==0xffff) return false;
    return slots ? !(v&(0x100u>>slots)) : !(v&(GP_BUSY|0xffu));
}
static void w(struct s3_device *s,unsigned reg,uint32_t value,unsigned bits)
{ s->io.write(s->io.ctx,reg,value,bits); }
static int configure(struct s3_device *s)
{
    int e=s3_wait(s,6); if (e) return e;
    /* No engine reset and no clock, panel, pitch or scanout changes. */
    w(s,MULTIFUNC_CNTL,0x500c,16);
    w(s,MULTIFUNC_CNTL,SCISSORS_L,16);
    w(s,MULTIFUNC_CNTL,SCISSORS_T,16);
    w(s,MULTIFUNC_CNTL,SCISSORS_R|(s->fb.width-1),16);
    w(s,MULTIFUNC_CNTL,SCISSORS_B|(s->fb.height-1),16);
    w(s,WRT_MASK,0xffff,32);
    return 0;
}
static int fill(struct s3_device *s,const struct fb_rect *r,uint32_t colour)
{
    int e=s3_wait(s,3); if (e) return e;
    w(s,MULTIFUNC_CNTL,0xa000,16);
    w(s,FRGD_COLOR,colour,32);
    w(s,FRGD_MIX,FSS_FRGDCOL|7,16);
    if ((e=s3_wait(s,5))) return e;
    w(s,CUR_X,r->x,16); w(s,CUR_Y,r->y,16);
    w(s,CUR_WIDTH,r->width-1,16); w(s,MULTIFUNC_CNTL,r->height-1,16);
    s3_launch(s);
    w(s,CMD,CMD_RECT|DRAW|INC_X|INC_Y|WRTDATA,16);
    return s3_wait(s,0);
}
static int copy(struct s3_device *s,const struct fb_rect *r,int32_t dx,int32_t dy)
{
    int e=s3_wait(s,2); if (e) return e;
    int sx=r->x,sy=r->y;
    unsigned cmd=CMD_BITBLT|DRAW|INC_X|INC_Y|WRTDATA;
    if (sx<dx) { sx+=r->width-1; dx+=r->width-1; cmd&=~INC_X; }
    if (sy<dy) { sy+=r->height-1; dy+=r->height-1; cmd&=~INC_Y; }
    w(s,MULTIFUNC_CNTL,0xa000,16); w(s,FRGD_MIX,FSS_BITBLT|7,16);
    if ((e=s3_wait(s,7))) return e;
    w(s,CUR_X,sx,16); w(s,CUR_Y,sy,16);
    w(s,DESTX_DIASTP,dx,16); w(s,DESTY_AXSTP,dy,16);
    w(s,CUR_WIDTH,r->width-1,16); w(s,MULTIFUNC_CNTL,r->height-1,16);
    s3_launch(s); w(s,CMD,cmd,16);
    return s3_wait(s,0);
}
const struct s3_backend s3trio64_backend={accept,format,firmware,available,configure,fill,copy,false};
static const struct display_match matches[]={
    {0x5333,0x8811,3,255},{0x5333,0x8814,3,255},{0x5333,0x8901,3,255}
};
S3_ADAPTER(s3trio64_driver,"s3trio64","S3 Trio64",s3trio64_backend,matches);
