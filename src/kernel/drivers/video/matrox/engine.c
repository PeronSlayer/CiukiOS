// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Hardware accelerated Matrox Millennium I, II, Mystique, G100, G200 and G400
 *
 * (c) 1998-2002 Petr Vandrovec <vandrove@vc.cvut.cz>
 *
 * Version: 1.65 2002/08/14
 *
 * MTRR stuff: 1998 Tom Rini <trini@kernel.crashing.org>
 *
 * Contributors: "menion?" <menion@mindless.com>
 *                     Betatesting, fixes, ideas
 *
 *               "Kurt Garloff" <garloff@suse.de>
 *                     Betatesting, fixes, ideas, videomodes, videomodes timmings
 *
 *               "Tom Rini" <trini@kernel.crashing.org>
 *                     MTRR stuff, PPC cleanups, betatesting, fixes, ideas
 *
 *               "Bibek Sahu" <scorpio@dodds.net>
 *                     Access device through readb|w|l and write b|w|l
 *                     Extensive debugging stuff
 *
 *               "Daniel Haun" <haund@usa.net>
 *                     Testing, hardware cursor fixes
 *
 *               "Scott Wood" <sawst46+@pitt.edu>
 *                     Fixes
 *
 *               "Gerd Knorr" <kraxel@goldbach.isdn.cs.tu-berlin.de>
 *                     Betatesting
 *
 *               "Kelly French" <targon@hazmat.com>
 *               "Fernando Herrera" <fherrera@eurielec.etsit.upm.es>
 *                     Betatesting, bug reporting
 *
 *               "Pablo Bianucci" <pbian@pccp.com.ar>
 *                     Fixes, ideas, betatesting
 *
 *               "Inaky Perez Gonzalez" <inaky@peloncho.fis.ucm.es>
 *                     Fixes, enhandcements, ideas, betatesting
 *
 *               "Ryuichi Oikawa" <roikawa@rr.iiij4u.or.jp>
 *                     PPC betatesting, PPC support, backward compatibility
 *
 *               "Paul Womar" <Paul@pwomar.demon.co.uk>
 *               "Owen Waller" <O.Waller@ee.qub.ac.uk>
 *                     PPC betatesting
 *
 *               "Thomas Pornin" <pornin@bolet.ens.fr>
 *                     Alpha betatesting
 *
 *               "Pieter van Leuven" <pvl@iae.nl>
 *               "Ulf Jaenicke-Roessler" <ujr@physik.phy.tu-dresden.de>
 *                     G100 testing
 *
 *               "H. Peter Arvin" <hpa@transmeta.com>
 *                     Ideas
 *
 *               "Cort Dougan" <cort@cs.nmt.edu>
 *                     CHRP fixes and PReP cleanup
 *
 *               "Mark Vojkovich" <mvojkovi@ucsd.edu>
 *                     G400 support
 *
 * (following author is not in any relation with this code, but his code
 *  is included in this driver)
 *
 * Based on framebuffer driver for VBE 2.0 compliant graphic boards
 *     (c) 1998 Gerd Knorr <kraxel@cs.tu-berlin.de>
 *
 * (following author is not in any relation with this code, but his ideas
 *  were used when writing this driver)
 *
 *		 FreeVBE/AF (Matrox), "Shawn Hargreaves" <shawn@talula.demon.co.uk>
 *
 */

/* Origin: Linux v6.12 drivers/video/fbdev/matrox/matroxfb_accel.c.
 * Port of initialization, fill, directional copy and mono ILOAD recipes.
 * Ciuki glue, validation, bounded waits and cursor implementation are new.
 * Cursor register reference: matroxfb_DAC1064.h and MGA-G200 Specification,
 * 10582-MS-0301, pp. 3-304, 3-319--323, 4-72; see docs/design/matrox-2d.md. */

#include <ciuki/kernel.h>
#include "matrox.h"

#define WAIT_US 20000u
#define WAIT_POLLS 200000u
#define SLICE_US 1000u

static uint32_t rd(struct matrox_engine *s, unsigned reg)
{ return s->io.read32(s->io.ctx,reg); }
static void wr(struct matrox_engine *s, unsigned reg, uint32_t value)
{ s->io.write32(s->io.ctx,reg,value); }
static bool dac_write(struct matrox_engine *s, uint8_t reg, uint8_t value)
{
    uint8_t old=s->io.read8(s->io.ctx,M_DAC_INDEX);
    s->io.write8(s->io.ctx,M_DAC_INDEX,reg);
    s->io.write8(s->io.ctx,M_DAC_DATA,value);
    bool ok=s->io.read8(s->io.ctx,M_DAC_DATA)==value;
    s->io.write8(s->io.ctx,M_DAC_INDEX,old);
    return ok;
}
/* Every packet fits entirely in known free entries. Never write into a full
 * FIFO and rely on PCI retries, including for ILOAD data. A second STATUS
 * observation after the empty FIFO orders posted commands before idle proof. */
static int poll(struct matrox_engine *s, unsigned slots, uint32_t clear)
{
    uint64_t start=s->io.now_us(s->io.ctx), slice=start;
    s->waits++;
    for (unsigned i=0;i<WAIT_POLLS;i++) {
        uint32_t fifo=rd(s,M_FIFOSTATUS), status=rd(s,M_STATUS);
        if (fifo==UINT32_MAX || status==UINT32_MAX) return -EIO;
        bool available=slots==M_FIFO_EMPTY ? (fifo&M_FIFO_EMPTY)!=0 : (fifo&255u)>=slots;
        if (available && !(status&clear)) {
            status=rd(s,M_STATUS);
            if (status==UINT32_MAX) return -EIO;
            if (!(status&clear)) return 0;
        }
        uint64_t now=s->io.now_us(s->io.ctx);
        if (now-start>=WAIT_US) return -ETIMEDOUT;
        if (s->pause && now-slice>=SLICE_US) {
            int e=s->pause(s->pause_arg);
            if (e) return e;
            slice=now;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    return -ETIMEDOUT;
}
void matrox_engine_quarantine(struct matrox_engine *s,const char *reason)
{
    s->active=false; s->quarantined=true; s->cpu_safe=false;
    s->reason=reason ? reason : "engine_fault";
    /* Direct DAC registers are outside the drawing FIFO. No reset, waiting
     * or framebuffer write here. A failed disable keeps the CPU guard. */
    s->cursor_hidden=dac_write(s,XCURCTRL,0);
    s->shape=false;
}
static int fault(struct matrox_engine *s,int error)
{
    if (error==-EOPNOTSUPP) error=-EIO; /* never report a submitted call as a clean punt */
    s->failures++;
    matrox_engine_quarantine(s,error==-ETIMEDOUT ? "engine_timeout" : "engine_fault");
    return error;
}
int matrox_engine_prove(struct matrox_engine *s)
{
    s->cpu_safe=false;
    if (!s->touched) { s->cpu_safe=true; return 0; }
    if (!dac_write(s,XCURCTRL,0)) return -EIO;
    s->cursor_hidden=true;
    int e=poll(s,M_FIFO_EMPTY,M_BUSY);
    if (!e) { s->io.drain(s->io.ctx); s->cpu_safe=true; }
    return e;
}
static int sync_engine(void *ctx)
{
    struct matrox_engine *s=ctx;
    s->io.drain(s->io.ctx);
    if (s->quarantined) return matrox_engine_prove(s);
    int e=poll(s,M_FIFO_EMPTY,M_BUSY);
    if (e) return fault(s,e);
    s->cpu_safe=true;
    return 0;
}
static int begin(struct matrox_engine *s,unsigned slots)
{
    if (!s->active || s->quarantined) return -ENODEV;
    s->io.drain(s->io.ctx);
    int e=poll(s,slots,0);
    return e ? fault(s,e) : 0;
}
static void layout(struct matrox_engine *s)
{ wr(s,M_MACCESS,s->maccess); wr(s,M_PITCH,s->pitch); }
static bool rect_ok(const struct matrox_engine *s,const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>=0 && r->height>=0 &&
        (uint64_t)(uint32_t)r->x+(uint32_t)r->width<=s->fb.width &&
        (uint64_t)(uint32_t)r->y+(uint32_t)r->height<=s->fb.height;
}
static uint32_t hw_colour(const struct matrox_engine *s,uint32_t c)
{ return s->fb.bpp==16 ? (c&65535u)*0x10001u : c; }
static int fill(void *ctx,const struct fb_rect *r,uint32_t colour)
{
    struct matrox_engine *s=ctx;
    if (!rect_ok(s,r)) return -EOPNOTSUPP;
    if (!r->width || !r->height) return 0;
    int e=begin(s,6); if (e) return e;
    layout(s);
    wr(s,M_DWGCTL,M_DWG_TRAP|M_DWG_SOLID|M_DWG_ARZERO|M_DWG_SGNZERO|M_DWG_SHIFTZERO|M_DWG_REPLACE);
    wr(s,M_FCOL,hw_colour(s,colour));
    wr(s,M_FXBNDRY,((uint32_t)(r->x+r->width)<<16)|(uint32_t)r->x);
    s->cpu_safe=false;
    wr(s,M_YDSTLEN|M_EXEC,((uint32_t)r->y<<16)|(uint32_t)r->height);
    e=sync_engine(s); if (!e) s->operations++;
    return e;
}
static int copy(void *ctx,const struct fb_rect *r,int32_t dx,int32_t dy)
{
    struct matrox_engine *s=ctx;
    if (!r) return -EOPNOTSUPP;
    struct fb_rect dst={dx,dy,r->width,r->height};
    if (!rect_ok(s,r) || !rect_ok(s,&dst)) return -EOPNOTSUPP;
    if (!r->width || !r->height) return 0;
    bool reverse=dy>r->y || (dy==r->y && dx>r->x);
    int e=begin(s,reverse ? 9 : 8); if (e) return e;
    layout(s);
    wr(s,M_DWGCTL,M_DWG_BITBLT|M_DWG_SHIFTZERO|M_DWG_BFCOL|M_DWG_REPLACE|(reverse ? 0 : M_DWG_SGNZERO));
    if (reverse) wr(s,M_SGN,5);
    wr(s,M_AR5,reverse ? 0u-s->pitch : s->pitch);
    uint32_t start=(uint32_t)r->y*s->pitch+(uint32_t)r->x, end;
    if (reverse) {
        end=start+(uint32_t)(r->height-1)*s->pitch;
        start=end+(uint32_t)r->width-1; dy+=r->height-1;
    } else end=start+(uint32_t)r->width-1;
    wr(s,M_AR0,end); wr(s,M_AR3,start);
    wr(s,M_FXBNDRY,((uint32_t)(dx+r->width-1)<<16)|(uint32_t)dx);
    s->cpu_safe=false;
    wr(s,M_YDSTLEN|M_EXEC,((uint32_t)dy<<16)|(uint32_t)r->height);
    e=sync_engine(s); if (!e) s->operations++;
    return e;
}
int matrox_mono(struct matrox_engine *s,const struct fb_rect *r,const uint8_t *data,
                 uint32_t stride,uint32_t size,uint32_t fg,uint32_t bg)
{
    if (!rect_ok(s,r) || r->width>256) return -EOPNOTSUPP;
    if (!r->width || !r->height) return 0;
    unsigned rowbytes=((unsigned)r->width+7)/8;
    if (!data || stride<rowbytes || (uint64_t)(r->height-1)*stride+rowbytes>size) return -EOPNOTSUPP;
    unsigned words=(rowbytes+3)/4;
    for (int y=0;y<r->height;y++) {
        int e=begin(s,9); if (e) return e;
        layout(s);
        wr(s,M_DWGCTL,M_DWG_ILOAD|M_DWG_SGNZERO|M_DWG_SHIFTZERO|M_DWG_BMONOWF|M_DWG_REPLACE);
        wr(s,M_FCOL,hw_colour(s,fg)); wr(s,M_BCOL,hw_colour(s,bg));
        wr(s,M_FXBNDRY,((uint32_t)(r->x+r->width-1)<<16)|(uint32_t)r->x);
        wr(s,M_AR0,(uint32_t)r->width-1); wr(s,M_AR3,0); wr(s,M_AR5,0);
        /* G400-class bus FIFO has only 16 entries. Keep the EXEC plus all
         * data words in one reserved packet; never stall after launch. */
        e=begin(s,1+words); if (e) return e;
        s->cpu_safe=false;
        wr(s,M_YDSTLEN|M_EXEC,((uint32_t)(r->y+y)<<16)|1u);
        for (unsigned i=0;i<words;i++) {
            uint32_t word=0;
            for (unsigned b=0;b<4 && i*4+b<rowbytes;b++) word|=(uint32_t)data[(unsigned)y*stride+i*4+b]<<(8*b);
            wr(s,0,word); /* only validated source bytes, tail zero padded */
        }
        e=sync_engine(s); if (e) return e;
    }
    s->operations++;
    return 0;
}
static unsigned channel(uint32_t c,unsigned size,unsigned pos)
{
    unsigned max=(1u<<size)-1u;
    return (((c>>pos)&max)*255u+max/2)/max;
}
static int cursor_show(void *ctx,bool show)
{
    struct matrox_engine *s=ctx;
    if (show && (!s->active || s->quarantined)) return -ENODEV;
    if (show && (!s->cursor || !s->shape)) return -EINVAL;
    if (!dac_write(s,XCURCTRL,show ? XCUR_XWIN : 0)) return fault(s,-EIO);
    s->cursor_hidden=!show;
    return 0;
}
static int cursor_set(void *ctx,const struct accel_cursor *cursor)
{
    struct matrox_engine *s=ctx;
    if (!cursor || !s->cursor) return -EOPNOTSUPP;
    if (!s->active || s->quarantined) return -ENODEV;
    int e=sync_engine(s); if (e) return e;
    e=cursor_show(s,false); if (e) return e;
    /* The plane's leftmost pixel is bit 63 of each 64-bit slice. Plane 0
     * precedes plane 1, and rows below/right of the legacy 32x32 are clear. */
    for (unsigned y=0;y<64;y++) for (unsigned b=0;b<16;b++) {
        unsigned shift=(b&7)*8;
        uint32_t row=y<32 ? (b<8 ? cursor->image[y]&cursor->mask[y] : cursor->mask[y]) : 0;
        uint64_t plane=(uint64_t)row<<32;
        s->cursor[y*16+b]=(uint8_t)(plane>>shift);
    }
    s->io.drain(s->io.ctx);
    for (unsigned index=0;index<2;index++) {
        uint32_t c=index ? cursor->foreground : cursor->background;
        unsigned base=index ? XCURCOL1 : XCURCOL0;
        if (!dac_write(s,base,(uint8_t)channel(c,s->fb.red_size,s->fb.red_pos)) ||
            !dac_write(s,base+1,(uint8_t)channel(c,s->fb.green_size,s->fb.green_pos)) ||
            !dac_write(s,base+2,(uint8_t)channel(c,s->fb.blue_size,s->fb.blue_pos))) return fault(s,-EIO);
    }
    if (!dac_write(s,XCURADDL,(uint8_t)(s->cursor_offset>>10)) ||
        !dac_write(s,XCURADDH,(uint8_t)(s->cursor_offset>>18))) return fault(s,-EIO);
    s->shape=true;
    return 0;
}
static int cursor_move(void *ctx,int32_t x,int32_t y)
{
    struct matrox_engine *s=ctx;
    if (!s->active || s->quarantined) return -ENODEV;
    if (!s->cursor) return -EOPNOTSUPP;
    if (!s->shape) return -EINVAL;
    uint32_t pos=0;
    if (x>-32 && y>-32 && x<(int32_t)s->fb.width && y<(int32_t)s->fb.height)
        pos=((uint32_t)(y+64)<<16)|(uint32_t)(x+64);
    int e=poll(s,0,M_VSYNC); if (e) return fault(s,e);
    wr(s,M_CURSOR_POS,pos);
    return rd(s,M_CURSOR_POS)==pos ? 0 : fault(s,-EIO);
}
static int stop(void *ctx)
{
    struct matrox_engine *s=ctx;
    s->active=false; s->shape=false;
    int e=matrox_engine_prove(s);
    return e ? fault(s,e) : 0;
}
static bool cpu_ready(void *ctx) { return ((struct matrox_engine *)ctx)->cpu_safe; }
static unsigned capabilities(void *ctx)
{
    struct matrox_engine *s=ctx;
    return s->active && !s->quarantined ? 1u|(s->cursor ? 2u : 0u) : 0;
}
static void set_pause(void *ctx,int (*pause_fn)(void *),void *arg)
{ struct matrox_engine *s=ctx; s->pause=pause_fn; s->pause_arg=arg; }
bool matrox_probe(const struct pci_func *p)
{
    return p && p->vendor==0x102b && p->class_code==3 && !p->subclass &&
        !(p->header_type&0x7f) && (p->command&2u) &&
        (p->device==0x0520 || p->device==0x0521 || p->device==0x0525 || p->device==0x2527);
}
bool matrox_mode(const struct fb_device *d,const struct pci_func *p)
{
    if (!d || !d->present || !d->mapped || !matrox_probe(p) ||
        !d->width || !d->height || d->width>4000 || d->height>4000 ||
        (d->bpp!=16 && d->bpp!=24 && d->bpp!=32) ||
        !d->pitch || d->pitch%(d->bpp/8) || (uint64_t)d->pitch*d->height>d->size ||
        (uint64_t)d->width*(d->bpp/8)>d->pitch ||
        d->size>(p->device==0x0520 || p->device==0x0521 ? 16u : 32u)*1024*1024) return false;
    unsigned pitch=d->pitch/(d->bpp/8), align=d->bpp==24 ? 64 : 32;
    if (pitch>8191 || pitch%align || d->blue_pos || d->green_pos!=(d->bpp==16 ? 5 : 8)) return false;
    return d->bpp==16 ? d->blue_size==5 && d->red_size==5 &&
        ((d->green_size==6 && d->red_pos==11) || (d->green_size==5 && d->red_pos==10)) :
        d->red_size==8 && d->green_size==8 && d->blue_size==8 && d->red_pos==16;
}
int matrox_engine_init(struct matrox_engine *s,const struct fb_device *fb,
                        const struct matrox_io *io,volatile uint8_t *cursor,uint32_t offset)
{
    if (s->quarantined) return -ENODEV;
    if (!io || !io->read32 || !io->write32 || !io->read8 || !io->write8 || !io->now_us || !io->drain ||
        !fb || (fb->bpp!=16 && fb->bpp!=24 && fb->bpp!=32) || !fb->pitch ||
        (cursor && ((offset&1023u) || offset>16u*1024*1024-1024))) return -EINVAL;
    s->io=*io; s->fb=*fb; s->cursor=cursor; s->cursor_offset=offset;
    s->pitch=fb->pitch/(fb->bpp/8);
    s->maccess=fb->bpp==16 ? (fb->green_size==5 ? 0xc0000001u : 0x40000001u) : fb->bpp==24 ? 3u : 2u;
    s->ops=(struct accel_ops){.ctx=s,.sync=sync_engine,.copy_rect=copy,.fill_rect=fill,
        .cursor_set=cursor_set,.cursor_move=cursor_move,.cursor_show=cursor_show,
        .stop=stop,.capabilities=capabilities,.cpu_ready=cpu_ready,.set_pause=set_pause};
    /* Before the first write, treat firmware activity as unsafe as well. */
    s->cpu_safe=false; s->touched=true;
    int e=matrox_engine_prove(s); if (e) return fault(s,e);
    e=poll(s,10,0); if (e) return fault(s,e);
    s->cpu_safe=false;
    wr(s,M_PITCH,s->pitch); wr(s,M_YDSTORG,0); wr(s,M_PLNWT,UINT32_MAX);
    wr(s,M_SRCORG,0); wr(s,M_DSTORG,0); wr(s,M_OPMODE,4);
    wr(s,M_CXBNDRY,0xffff0000u); wr(s,M_YTOP,0); wr(s,M_YBOT,0x01ffffffu);
    wr(s,M_MACCESS,s->maccess);
    e=sync_engine(s); if (e) return e;
    s->active=true; s->reason="ready";
    return 0;
}
