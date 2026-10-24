/* SPDX-License-Identifier: GPL-2.0-only -- production printer, authored content. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libnsfb.h>
#include "utils/errors.h"
#include "netsurf/plotters.h"
#include "netsurf/browser_window.h"
#include "netsurf/content.h"
#include "apps/libprint/dialog.h"
#include "sdk/netsurf/port/printing.h"
struct nsfb_s { unsigned w,h;uint32_t *pixels; };
static struct nsfb_s screen,*current=&screen;
static size_t charged;
static int fail_alloc,fail_draw,draws;
static int offsets[16];
struct hlcache_handle *browser_window_get_content(struct browser_window *bw)
{ return bw?(struct hlcache_handle *)1:NULL; }
const char *browser_window_get_title(struct browser_window *bw) { assert(bw);return "long page"; }
int content_get_width(struct hlcache_handle *c) { assert(c);return 400; }
int content_get_height(struct hlcache_handle *c) { assert(c);return 1400; }
int ciuki_image_reserve(size_t n) { assert(!charged);if(fail_alloc)return -1;charged=n;return 0; }
void ciuki_image_release(size_t n) { assert(n==charged);charged=0; }
void ciuki_ui_keepalive(void) {}
nsfb_t *nsfb_new(enum nsfb_type_e type) { assert(type==NSFB_SURFACE_RAM);return calloc(1,sizeof(struct nsfb_s)); }
int nsfb_set_geometry(nsfb_t *n,int w,int h,enum nsfb_format_e format)
{ assert(format==NSFB_FMT_XRGB8888);n->w=(unsigned)w;n->h=(unsigned)h;return 0; }
int nsfb_init(nsfb_t *n) { n->pixels=calloc((size_t)n->w*n->h,4);return n->pixels?0:-1; }
int nsfb_free(nsfb_t *n) { free(n->pixels);free(n);return 0; }
int nsfb_get_buffer(nsfb_t *n,uint8_t **p,int *stride) { *p=(uint8_t *)n->pixels;*stride=(int)n->w*4;return 0; }
nsfb_t *framebuffer_set_surface(nsfb_t *n) { nsfb_t *old=current;current=n;return old; }
const struct plotter_table fb_plotters={0};
bool content_redraw(struct hlcache_handle *c,struct content_redraw_data *d,const struct rect *clip,const struct redraw_context *ctx)
{
    assert(c && current!=&screen && !ctx->interactive && ctx->plot==&fb_plotters);
    assert(clip->x0==0 && clip->y0==0 && clip->x1==(int)current->w && clip->y1==(int)current->h);
    assert(d->scale>0 && d->x==0 && d->background_colour==0xffffff);
    assert(draws<16);offsets[draws++]=d->y;
    for(unsigned y=0;y<current->h;y++)for(unsigned x=0;x<current->w;x++)
        current->pixels[(size_t)y*current->w+x]=d->y<-100?0x0000ff:0xff0000;
    return !fail_draw;
}
static unsigned offered;
void cp_ui_document(struct cp_ui *p,struct du_ui *u,const char *title,void *bw,
                    int (*render)(void *,FILE *,unsigned,const struct cp_page *),void (*release)(void *))
{ assert(p && u && !strcmp(title,"long page") && bw && render==ciuki_print_render && release);release(bw);offered++; }
static char *read_all(FILE *f)
{ assert(!fflush(f));long size=ftell(f);assert(size>=0);char *s=calloc((size_t)size+1,1);assert(s);rewind(f);assert(fread(s,1,(size_t)size,f)==(size_t)size);return s; }
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    struct cp_page p={144,216,0,12,18,12,18,2,0,0};
    FILE *f=tmpfile();assert(f && !ciuki_print_render((void *)1,f,CP_POSTSCRIPT,&p));
    assert(draws==3 && offsets[0]==0 && offsets[1]==-180 && offsets[2]==-360 && !charged && current==&screen);
    char *text=read_all(f);assert(strstr(text,"/PageSize [144 216]") && strstr(text,"%%Page: 3 3") && strstr(text,"0000ff"));free(text);fclose(f);
    p.first=p.last=2;draws=0;f=tmpfile();assert(f && !ciuki_print_render((void *)1,f,CP_POSTSCRIPT,&p));
    text=read_all(f);assert(draws==1 && offsets[0]==-180 && strstr(text,"%%Page: 2 2") && !strstr(text,"%%Page: 1 1"));free(text);fclose(f);
    p.landscape=1;draws=0;f=tmpfile();assert(f && !ciuki_print_render((void *)1,f,CP_POSTSCRIPT,&p));
    text=read_all(f);assert(strstr(text,"/PageSize [216 144]") && strstr(text,"12 18 translate 192 108 scale"));free(text);fclose(f);
    p=(struct cp_page){612,792,0,36,36,36,36,1,1,1};draws=0;f=tmpfile();assert(f && !ciuki_print_render((void *)1,f,CP_PCL,&p));
    assert(ftell(f)>100 && draws==1 && !charged && current==&screen);fclose(f);
    f=tmpfile();assert(f && !fseek(f,CP_MAX_BYTES-16,SEEK_SET));
    assert(ciuki_print_render((void *)1,f,CP_POSTSCRIPT,&p)==-1 && errno==EFBIG && !charged && current==&screen);fclose(f);
    fail_alloc=1;f=tmpfile();assert(f && ciuki_print_render((void *)1,f,CP_PCL,&p)==-1 && errno==ENOMEM && !charged);fclose(f);fail_alloc=0;
    fail_draw=1;f=tmpfile();assert(f && ciuki_print_render((void *)1,f,CP_POSTSCRIPT,&p)==-1 && errno==EIO && !charged && current==&screen);fclose(f);fail_draw=0;
    f=tmpfile();assert(f && ciuki_print_render(NULL,f,CP_POSTSCRIPT,&p)==-1 && errno==EINVAL);assert(ciuki_print_render((void *)1,f,CP_TEXT,&p)==-1);fclose(f);
    static struct cp_ui print;static struct du_ui u;ciuki_print_document(&print,&u,(void *)1);ciuki_print_document(&print,&u,NULL);assert(offered==1);
    puts("PASS G26C document pagination: below viewport, inclusive range, margins/landscape, PS/PCL, allocation/redraw failure and surface/ledger cleanup");return 0;
}
