/* Origin: X.Org xf86-video-neomagic, src/neo_cursor.c, commit
 * 6661bdd4551e4e63e983685464a277845aed3012. Ported packing, colours,
 * signed-position cropping and memory-address permutation; no X server glue. */
/**********************************************************************
Copyright 1998, 1999 by Precision Insight, Inc., Cedar Park, Texas.

                        All Rights Reserved

Permission to use, copy, modify, distribute, and sell this software and
its documentation for any purpose is hereby granted without fee,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation, and that the name of Precision Insight not be
used in advertising or publicity pertaining to distribution of the
software without specific, written prior permission.  Precision Insight
and its suppliers make no representations about the suitability of this
software for any purpose.  It is provided "as is" without express or
implied warranty.

PRECISION INSIGHT DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR ANY
SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
**********************************************************************/

/*
 * The original Precision Insight driver for
 * XFree86 v.3.3 has been sponsored by Red Hat.
 *
 * Authors:
 *   Jens Owen (jens@tungstengraphics.com)
 *   Kevin E. Martin (kevin@precisioninsight.com)
 *
 * Port to Xfree86 v.4.0
 *   1998, 1999 by Egbert Eich (Egbert.Eich@Physik.TU-Darmstadt.DE)
 */

#include <ciuki/kernel.h>
#include "neo.h"

int neo_hide(struct neo_engine *s)
{
    s->cursor_safe=s->io.hide(s->io.ctx);
    s->hidden=s->cursor_safe;
    if (!s->cursor_safe) s->cpu_safe=false;
    return s->cursor_safe ? 0 : -EIO;
}
static int cursor_fault(struct neo_engine *s)
{ neo_quarantine(s,"cursor_fault"); return -EIO; }
static bool cursor_available(const struct neo_engine *s)
{ return s->active && !s->quarantined && s->cursor; }
static uint32_t rgb(const struct neo_engine *s,uint32_t value)
{
    const struct fb_device *d=&s->fb;
    uint32_t r=(value>>d->red_pos)&((1u<<d->red_size)-1);
    uint32_t g=(value>>d->green_pos)&((1u<<d->green_size)-1);
    uint32_t b=(value>>d->blue_pos)&((1u<<d->blue_size)-1);
    r=(255*r+((1u<<d->red_size)-1)/2)/((1u<<d->red_size)-1);
    g=(255*g+((1u<<d->green_size)-1)/2)/((1u<<d->green_size)-1);
    b=(255*b+((1u<<d->blue_size)-1)/2)/((1u<<d->blue_size)-1);
    /* Neo cursor colour register order is R,G,B in increasing byte order. */
    return r|(g<<8)|(b<<16);
}
static bool checked(struct neo_engine *s,unsigned reg,uint32_t value)
{
    s->touched=true;
    s->io.write(s->io.ctx,NEO_CURSOR+reg,value);
    return s->io.read(s->io.ctx,NEO_CURSOR+reg)==value;
}
/* 64x64 layout: each row is 8 bytes of ~image & opaque, followed by
 * 8 bytes of opaque. Ciuki supplies MSB-left 32x32. Crop by samples, avoiding
 * the donor's platform-specific shifts and shift-by-32 corner cases. */
static int position(struct neo_engine *s)
{
    int32_t x=s->cursor_x, y=s->cursor_y;
    unsigned left=x<0 && x>-32 ? (unsigned)-x : 0;
    unsigned top=y<0 && y>-32 ? (unsigned)-y : 0;
    bool outside=x<=-32 || y<=-32 || x>=(int32_t)s->fb.width || y>=(int32_t)s->fb.height;
    if (neo_hide(s)) return cursor_fault(s);
    if (neo_idle(s)) return cursor_fault(s);
    memset(s->cursor,0,1024);
    if (!outside) for (unsigned row=0;row+top<32;row++) {
        uint32_t opaque=s->shape.mask[row+top], image=s->shape.image[row+top];
        for (unsigned col=0;col+left<32;col++) {
            uint32_t bit=1u<<(31-col-left);
            if (!(opaque&bit)) continue;
            uint8_t mask=(uint8_t)(0x80u>>(col%8));
            s->cursor[row*16+8+col/8]|=mask;
            if (!(image&bit)) s->cursor[row*16+col/8]|=mask;
        }
    }
    s->io.drain(s->io.ctx);
    uint32_t address=((s->cursor_offset>>10)&15u)<<8|((s->cursor_offset>>14)&255u);
    if (!checked(s,NEOREG_CURSMEMPOS,address) ||
        !checked(s,NEOREG_CURSX,outside || x<0 ? 0 : (uint32_t)x) ||
        !checked(s,NEOREG_CURSY,outside || y<0 ? 0 : (uint32_t)y)) return cursor_fault(s);
    if (s->shown && !outside) {
        if (!checked(s,NEOREG_CURSCNTL,NEO_CURS_ENABLE)) return cursor_fault(s);
        s->hidden=false;
    }
    return 0;
}
int neo_cursor_set(void *ctx,const struct accel_cursor *shape)
{
    struct neo_engine *s=ctx;
    if (!cursor_available(s)) return -EOPNOTSUPP;
    if (!shape) return -EINVAL;
    if (neo_hide(s)) return cursor_fault(s);
    s->shown=false; s->loaded=false; s->shape=*shape;
    s->cursor_x=s->cursor_y=0;
    if (!checked(s,NEOREG_CURSFGCOLOR,rgb(s,shape->foreground)) ||
        !checked(s,NEOREG_CURSBGCOLOR,rgb(s,shape->background))) return cursor_fault(s);
    int e=position(s);
    if (!e) s->loaded=true;
    return e;
}
int neo_cursor_move(void *ctx,int32_t x,int32_t y)
{
    struct neo_engine *s=ctx;
    if (!cursor_available(s)) return -EOPNOTSUPP;
    if (!s->loaded) return -EINVAL;
    s->cursor_x=x; s->cursor_y=y;
    return position(s);
}
int neo_cursor_show(void *ctx,bool show)
{
    struct neo_engine *s=ctx;
    if (!show) {
        s->shown=false;
        return neo_hide(s) ? cursor_fault(s) : 0;
    }
    if (!cursor_available(s)) return -EOPNOTSUPP;
    if (!s->loaded) return -EINVAL;
    s->shown=true;
    return position(s);
}
