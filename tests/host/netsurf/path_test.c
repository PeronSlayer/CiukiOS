/* fx-24: the framebuffer path plotter (sdk/netsurf/port/path.c) on a real
 * libnsfb RAM surface. Upstream's framebuffer front end draws nothing for a
 * path; these checks fail on it (every sample stays white).
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <libnsfb.h>
#include <libnsfb_plot.h>
#include "utils/errors.h"
#include "netsurf/types.h"
#include "netsurf/plot_style.h"
#include "netsurf/plotters.h"
#include "sdk/netsurf/port/path.h"
#define W 128
#define H 128
static nsfb_t *fb;
static uint8_t *pixels;
static int stride;
static const float identity[6]={1,0,0,1,0,0};
static void clear(void)
{
    nsfb_bbox_t all={0,0,W,H};
    assert(nsfb_plot_set_clip(fb,&all));
    assert(nsfb_plot_rectangle_fill(fb,&all,0xffffff));
}
static unsigned red(int x,int y) { return pixels[y*stride+x*4+2]; }
static unsigned green(int x,int y) { return pixels[y*stride+x*4+1]; }
static unsigned blue(int x,int y) { return pixels[y*stride+x*4]; }
static plot_style_t fill(colour c) { return (plot_style_t){.fill_colour=c,.stroke_colour=NS_TRANSPARENT}; }
static void rect_path(float *p,unsigned *n,float x0,float y0,float x1,float y1,bool reverse)
{
    float xs[4]={x0,x1,x1,x0},ys[4]={y0,y0,y1,y1};
    for(int i=0;i<4;i++){int k=reverse?3-i:i;p[(*n)++]=i?PLOTTER_PATH_LINE:PLOTTER_PATH_MOVE;p[(*n)++]=xs[k];p[(*n)++]=ys[k];}
    p[(*n)++]=PLOTTER_PATH_CLOSE;
}
int main(void)
{
    fb=nsfb_new(NSFB_SURFACE_RAM);assert(fb);
    assert(!nsfb_set_geometry(fb,W,H,NSFB_FMT_XRGB8888));assert(nsfb_init(fb)!=-1);
    assert(!nsfb_get_buffer(fb,&pixels,&stride));
    float p[256];unsigned n=0;
    /* 1. An integer-aligned square: exact colour inside, untouched outside. */
    clear();rect_path(p,&n,10,10,50,50,false);plot_style_t s=fill(0x0000ff);
    assert(ciuki_plot_path(fb,&s,p,n,identity)==NSERROR_OK);
    assert(red(10,10)==255 && green(10,10)==0 && blue(10,10)==0 && red(49,49)==255 && red(30,30)==255);
    assert(red(9,30)==255 && green(9,30)==255 && green(50,30)==255 && green(30,50)==255 && green(30,9)==255);
    /* 2. Anti-aliasing: a half-pixel edge gives half coverage. */
    clear();n=0;rect_path(p,&n,10.5f,10,50,50,false);assert(ciuki_plot_path(fb,&s,p,n,identity)==NSERROR_OK);
    assert(green(10,30)>=120 && green(10,30)<=135 && green(11,30)==0);
    /* 3. Non-zero rule: an inner subpath wound the other way is a hole;
     *    wound the same way it stays filled. */
    clear();n=0;rect_path(p,&n,10,10,70,70,false);rect_path(p,&n,30,30,50,50,true);
    assert(ciuki_plot_path(fb,&s,p,n,identity)==NSERROR_OK);
    assert(green(20,20)==0 && green(40,40)==255 && green(60,60)==0);
    clear();n=0;rect_path(p,&n,10,10,70,70,false);rect_path(p,&n,30,30,50,50,false);
    assert(ciuki_plot_path(fb,&s,p,n,identity)==NSERROR_OK);assert(green(40,40)==0);
    /* 4. Bezier circle (four cubic arcs, kappa 0.5523): area within 1 %. */
    clear();n=0;const float cx=64,cy=64,r=30,k=0.5523f*r;
    float circle[]={PLOTTER_PATH_MOVE,cx+r,cy,
        PLOTTER_PATH_BEZIER,cx+r,cy+k,cx+k,cy+r,cx,cy+r, PLOTTER_PATH_BEZIER,cx-k,cy+r,cx-r,cy+k,cx-r,cy,
        PLOTTER_PATH_BEZIER,cx-r,cy-k,cx-k,cy-r,cx,cy-r, PLOTTER_PATH_BEZIER,cx+k,cy-r,cx+r,cy-k,cx+r,cy,PLOTTER_PATH_CLOSE};
    assert(ciuki_plot_path(fb,&s,circle,sizeof(circle)/sizeof(*circle),identity)==NSERROR_OK);
    double area=0;for(int y=0;y<H;y++)for(int x=0;x<W;x++)area+=(255.0-green(x,y))/255.0;
    assert(fabs(area-3.14159265358979*r*r)/(3.14159265358979*r*r)<0.01);
    assert(green(64,64)==0 && green(64+31,64)==255 && green(64,64-31)==255);
    /* 5. Stroke: a 6-unit line covers rows 77-82; stroke and fill colours. */
    clear();float line[]={PLOTTER_PATH_MOVE,10,80,PLOTTER_PATH_LINE,110,80};
    plot_style_t t={.fill_colour=NS_TRANSPARENT,.stroke_colour=0x00ff00,.stroke_width=plot_style_int_to_fixed(6)};
    assert(ciuki_plot_path(fb,&t,line,6,identity)==NSERROR_OK);
    assert(green(60,77)==255 && red(60,77)==0 && red(60,82)==0 && red(60,76)==255 && red(60,83)==255);
    /* 6. Transform: x' = 2x + 10 maps the square [0,20] to [10,50]. */
    clear();n=0;rect_path(p,&n,0,0,20,20,false);const float scale2[6]={2,0,0,2,10,10};
    assert(ciuki_plot_path(fb,&s,p,n,scale2)==NSERROR_OK);
    assert(green(10,10)==0 && green(49,49)==0 && green(50,50)==255 && green(9,9)==255);
    /*    and the stroke width follows the transform's scale (3 units x2). */
    clear();t.stroke_width=plot_style_int_to_fixed(3);
    assert(ciuki_plot_path(fb,&t,line,6,(const float[6]){2,0,0,2,-10,-80})==NSERROR_OK);
    assert(red(60,77)==0 && red(60,82)==0 && red(60,76)==255 && red(60,83)==255);
    /* 7. The clip rectangle bounds every raster. */
    clear();nsfb_bbox_t left={0,0,30,H};assert(nsfb_plot_set_clip(fb,&left));
    n=0;rect_path(p,&n,10,10,50,50,false);assert(ciuki_plot_path(fb,&s,p,n,identity)==NSERROR_OK);
    assert(green(20,20)==0 && green(30,20)==255 && green(45,45)==255);
    /* 8. Transparent colours draw nothing; malformed paths draw nothing. */
    clear();n=0;rect_path(p,&n,10,10,50,50,false);plot_style_t none=fill(NS_TRANSPARENT);
    assert(ciuki_plot_path(fb,&none,p,n,identity)==NSERROR_OK && green(30,30)==255);
    struct ciuki_path_stats before=ciuki_path_stats();
    float bad[]={PLOTTER_PATH_MOVE,10,10,PLOTTER_PATH_LINE,50};
    assert(ciuki_plot_path(fb,&s,bad,5,identity)==NSERROR_OK && green(12,12)==255);
    float unknown[]={PLOTTER_PATH_MOVE,10,10,9,50,50};
    assert(ciuki_plot_path(fb,&s,unknown,6,identity)==NSERROR_OK);
    assert(ciuki_path_stats().rejected==before.rejected+2);
    /* 9. A line after CLOSE begins a new subpath at the closed one's start
     *    point (10,10), not at its last point (40,40) (SVG 1.1, 8.3.3). */
    clear();float reopen[]={PLOTTER_PATH_MOVE,10,10,PLOTTER_PATH_LINE,40,10,PLOTTER_PATH_LINE,40,40,PLOTTER_PATH_CLOSE,
                            PLOTTER_PATH_LINE,10,60,PLOTTER_PATH_LINE,40,60};
    assert(ciuki_plot_path(fb,&s,reopen,sizeof(reopen)/sizeof(*reopen),identity)==NSERROR_OK);
    assert(green(35,15)==0 && green(15,35)==0 && green(30,45)==0 && green(38,50)==255);
    nsfb_free(fb);
    struct ciuki_path_stats done=ciuki_path_stats();
    printf("NetSurf path plotter: fills=%u strokes=%u rejected=%u\n",done.fills,done.strokes,done.rejected);
    puts("NetSurf fx-24 path plotter: non-zero fills, anti-aliasing, Bezier area, strokes, transforms, clip and malformed paths passed");
    return 0;
}
