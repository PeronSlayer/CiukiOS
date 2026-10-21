/* SPDX-License-Identifier: MIT
 * SDK font metrics and paint share the exact same whole-string advances. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/font.h>
#include <libnsfb.h>
#include <libnsfb_plot.h>
#include "utils/errors.h"
#include "utils/utf8.h"
#include "netsurf/plot_style.h"
#include "netsurf/layout.h"
#include "netsurf/utf8.h"
#include "framebuffer/font.h"
#include "font.h"

/* Four live faces cap caches at 1 MiB. Reopen on style changes; no web fonts. */
static struct { struct ciuki_font_face *face; unsigned key; } faces[4];
static unsigned next_face;
static struct ciuki_font_face *face_for(const plot_font_style_t *style)
{
    unsigned family=style->family==PLOT_FONT_FAMILY_SERIF?1:style->family==PLOT_FONT_FAMILY_MONOSPACE?2:0;
    unsigned key=family*4+(style->weight>=600?1:0)+((style->flags&(FONTF_ITALIC|FONTF_OBLIQUE))?2:0);
    static const char *family_names[]={"Sans","Serif","Mono"};
    static const char *style_names[]={"Regular","Bold","Italic","BoldItalic"};
    for (unsigned i=0;i<4;i++) if (faces[i].face && faces[i].key==key) return faces[i].face;
    unsigned i=next_face++%4;
    ciuki_font_close(faces[i].face); faces[i].face=NULL;
    char path[512];
#ifdef CIUKI_HOST
    snprintf(path,sizeof(path),"%s/assets/fonts/liberation/Liberation%s-%s.ttf",CIUKI_ROOT,family_names[family],style_names[key%4]);
#else
    snprintf(path,sizeof(path),"/System/fonts/Liberation%s-%s.ttf",family_names[family],style_names[key%4]);
#endif
    if (ciuki_font_open(path,&faces[i].face)) return NULL;
    faces[i].key=key;
    return faces[i].face;
}
static unsigned em(const plot_font_style_t *s)
{
    int64_t value=(int64_t)s->size*96*64/(72*PLOT_STYLE_SCALE);
    return value<64?64:value>8192?8192:(unsigned)value;
}
bool fb_font_init(void) { return true; }
bool fb_font_finalise(void)
{
    for(unsigned i=0;i<4;i++) { ciuki_font_close(faces[i].face); faces[i].face=NULL; }
    return true;
}
nserror fb_font_width(const plot_font_style_t *style,const char *s,size_t n,int *width)
{
    struct ciuki_font_face *face=face_for(style); int32_t advance;
    if (!face || ciuki_font_measure(face,em(style),s,n,&advance)) return NSERROR_NOMEM;
    *width=(advance+32)/64; return NSERROR_OK;
}
nserror fb_font_position(const plot_font_style_t *style,const char *s,size_t n,int x,size_t *offset,int *actual)
{
    int prior=0, width; size_t prev=0;
    *offset=0; *actual=0;
    for(size_t at=0;at<n;) {
        at=utf8_next(s,n,at);
        nserror err=fb_font_width(style,s,at,&width); if(err) return err;
        if(width>=x) {
            if ((int64_t)x-prior < (int64_t)width-x) { *offset=prev; *actual=prior; }
            else { *offset=at; *actual=width; }
            return NSERROR_OK;
        }
        prev=at; prior=width;
    }
    *offset=n; *actual=prior; return NSERROR_OK;
}
static nserror split(const plot_font_style_t *style,const char *s,size_t n,int x,size_t *offset,int *actual)
{
    size_t last=0; int last_width=0, width=0;
    for(size_t at=0;at<n;) {
        if(s[at]==' ' && at) { last=at; last_width=width; }
        at=utf8_next(s,n,at);
        nserror err=fb_font_width(style,s,at,&width); if(err) return err;
        if(width>x && last) { *offset=last; *actual=last_width; return NSERROR_OK; }
    }
    *offset=n; *actual=width; return NSERROR_OK;
}
struct paint_context { nsfb_t *surface; nsfb_colour_t colour; };
static int paint(void *context,int x,int y,const struct ciuki_font_bitmap *bitmap)
{
    struct paint_context *p=context;
    nsfb_bbox_t box={x,y,x+(int)bitmap->width,y+(int)bitmap->height};
    if (!bitmap->width || !bitmap->height) return 0;
    return nsfb_plot_glyph8(p->surface,&box,bitmap->coverage,bitmap->stride,p->colour)?0:-1;
}
nserror ciuki_font_plot(nsfb_t *surface,const plot_font_style_t *style,int x,int y,const char *s,size_t n)
{
    struct ciuki_font_face *face=face_for(style);
    struct paint_context p={surface,style->foreground};
    int32_t advance;
    return face && !ciuki_font_paint(face,em(style),s,n,x,y,paint,&p,&advance)?NSERROR_OK:NSERROR_NOMEM;
}
static nserror local(const char *s,size_t n,char **out)
{
    *out=malloc(n+1); if(!*out) return NSERROR_NOMEM;
    memcpy(*out,s,n); (*out)[n]=0; return NSERROR_OK;
}
static struct gui_layout_table layout={.width=fb_font_width,.position=fb_font_position,.split=split};
static struct gui_utf8_table utf8={.utf8_to_local=local,.local_to_utf8=local};
struct gui_layout_table *framebuffer_layout_table=&layout;
struct gui_utf8_table *framebuffer_utf8_table=&utf8;
