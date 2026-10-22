/* SPDX-License-Identifier: MIT
 * SDK font metrics and paint share the exact same whole-string advances. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
#include "budget.h"
#include "font_work.h"
#include "logline.h"

static struct ciuki_font_work work;
static uint64_t clock_ns(void)
{
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;
}
struct ciuki_font_work ciuki_font_work_read(void) { return work; }
void ciuki_font_work_report(struct ciuki_font_work before,uint64_t elapsed,bool local)
{
    static unsigned reports;
    if(reports>=16)return;
    reports++;
    ciuki_log_line("[netsurf] layout work elapsed_us=%llu class=%s pass=%u font_opens=%u font_open_us=%llu font_measures=%u font_measure_us=%llu width_hits=%u measured_bytes=%llu",
        (unsigned long long)(elapsed/1000),local?"local":"remote",reports,work.opens-before.opens,
        (unsigned long long)((work.open_ns-before.open_ns)/1000),work.measures-before.measures,
        (unsigned long long)((work.measure_ns-before.measure_ns)/1000),work.hits-before.hits,
        (unsigned long long)(work.bytes-before.bytes));
}
/* Exact whole-string SDK advances; fixed storage, no new font/glyph budget.
 * Hash collisions compare all bytes. Longer strings bypass the cache. */
#define WIDTH_SLOTS 128
#define WIDTH_BYTES 256
static struct {
    bool valid;
    unsigned key,size;
    size_t length;
    int width;
    char text[WIDTH_BYTES];
} widths[WIDTH_SLOTS];

/* Four live faces cap caches at 1 MiB. Reopen on style changes; no web fonts. */
static struct { struct ciuki_font_face *face; unsigned key; } faces[4];
static unsigned next_face;
static unsigned face_key(const plot_font_style_t *style)
{
    unsigned family=style->family==PLOT_FONT_FAMILY_SERIF?1:style->family==PLOT_FONT_FAMILY_MONOSPACE?2:0;
    return family*4+(style->weight>=600?1:0)+((style->flags&(FONTF_ITALIC|FONTF_OBLIQUE))?2:0);
}
static struct ciuki_font_face *face_for(const plot_font_style_t *style)
{
    unsigned key=face_key(style),family=key/4;
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
    uint64_t start=clock_ns();
    int error=ciuki_font_open(path,&faces[i].face);
    uint64_t end=clock_ns();
    work.opens++;if(end>=start)work.open_ns+=end-start;
    if(error)return NULL;
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
    memset(widths,0,sizeof(widths));next_face=0;
    return true;
}
nserror fb_font_width(const plot_font_style_t *style,const char *s,size_t n,int *width)
{
    *width=0;
    if(n>8192 || ciuki_layout_expired())return NSERROR_BAD_SIZE;
    unsigned key=face_key(style),size=em(style),slot=key*33u+size;
    for(size_t i=0;i<n;i++)slot=slot*33u+(unsigned char)s[i];
    slot%=WIDTH_SLOTS;
    if(n<=WIDTH_BYTES && widths[slot].valid && widths[slot].key==key && widths[slot].size==size &&
       widths[slot].length==n && !memcmp(widths[slot].text,s,n)) {
        *width=widths[slot].width;work.hits++;return NSERROR_OK;
    }
    struct ciuki_font_face *face=face_for(style); int32_t advance;
    if(!face)return NSERROR_NOMEM;
    uint64_t start=clock_ns();
    int error=ciuki_font_measure(face,size,s,n,&advance);
    uint64_t end=clock_ns();
    work.measures++;work.bytes+=n;if(end>=start)work.measure_ns+=end-start;
    if(error)return NSERROR_NOMEM;
    if(ciuki_layout_expired())return NSERROR_STOPPED;
    *width=(advance+32)/64;
    if(n<=WIDTH_BYTES) {
        widths[slot].valid=true;widths[slot].key=key;widths[slot].size=size;
        widths[slot].length=n;widths[slot].width=*width;memcpy(widths[slot].text,s,n);
    }
    return NSERROR_OK;
}
/* At most ceil(log2(8193))+2 complete measurements, rather than one
 * measurement per growing prefix. Boundaries are UTF-8 byte offsets. */
nserror fb_font_position(const plot_font_style_t *style,const char *s,size_t n,int x,size_t *offset,int *actual)
{
    if(n>8192)return NSERROR_BAD_SIZE;
    *offset=0;*actual=0;
    if(x<=0 || !n)return NSERROR_OK;
    uint16_t *ends=malloc((n+1)*sizeof(*ends));
    if(!ends)return NSERROR_NOMEM;
    size_t count=0;
    ends[count++]=0;
    for(size_t at=0;at<n;) { at=utf8_next(s,n,at);ends[count++]=at; }
    size_t lo=1,hi=count;int width=0;
    nserror err=NSERROR_OK;
    while(lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        err=fb_font_width(style,s,ends[mid],&width);if(err)goto done;
        if(width<x)lo=mid+1;else hi=mid;
    }
    if(lo==count)lo=count-1;
    err=fb_font_width(style,s,ends[lo],&width);if(err)goto done;
    int prior=0;
    if(lo) { err=fb_font_width(style,s,ends[lo-1],&prior);if(err)goto done; }
    if((int64_t)x-prior < (int64_t)width-x) { *offset=ends[lo-1];*actual=prior; }
    else { *offset=ends[lo];*actual=width; }
done:
    free(ends);return err;
}
static nserror split(const plot_font_style_t *style,const char *s,size_t n,int x,size_t *offset,int *actual)
{
    int width;
    nserror err=fb_font_width(style,s,n,&width);if(err)return err;
    *offset=n;*actual=width;
    if(width<=x || !n)return NSERROR_OK;
    uint16_t *spaces=malloc(n*sizeof(*spaces));if(!spaces)return NSERROR_NOMEM;
    size_t count=0;
    for(size_t at=1;at<n;at++)if(s[at]==' ')spaces[count++]=at;
    if(!count) { free(spaces);return NSERROR_OK; }
    size_t lo=0,hi=count;
    while(lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        err=fb_font_width(style,s,spaces[mid],&width);if(err)goto done;
        if(width<=x)lo=mid+1;else hi=mid;
    }
    *offset=spaces[lo?lo-1:0];
    err=fb_font_width(style,s,*offset,actual);
done:
    free(spaces);return err;
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
    ciuki_ui_keepalive(); /* f6-20: a long first paint still reads the desktop channel */
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
