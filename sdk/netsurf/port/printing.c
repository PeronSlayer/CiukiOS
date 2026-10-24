/* SPDX-License-Identifier: GPL-2.0-only
 * Full-layout, paginated raster output. Browser content/plotters remain on the
 * UI owner; only a completed raw file reaches the G31 publication worker. */
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <libnsfb.h>
#include "utils/errors.h"
#include "netsurf/browser_window.h"
#include "netsurf/content.h"
#include "netsurf/plotters.h"
#include "content/hlcache.h"
struct fbtk_bitmap;
#include "framebuffer/framebuffer.h"
#include "apps/libprint/dialog.h"
#include "image_memory.h"
#include "budget.h"
#include "printing.h"

static unsigned paper(unsigned w,unsigned h)
{ return w==612 && h==792?2:w==612 && h==1008?3:w==595 && h==842?26:0; }
static int selected(const struct cp_page *p,unsigned page)
{ return (!p->first || page>=p->first) && (!p->last || page<=p->last); }
static int page_output(FILE *f,unsigned driver,const struct cp_page *p,unsigned w,unsigned h,
                       unsigned pw,unsigned ph,const uint8_t *pixels,int stride,unsigned number)
{
    long begin=ftell(f);
    uint64_t bound=driver==CP_POSTSCRIPT?(uint64_t)pw*ph*6+ph+512:
        (uint64_t)((pw*75/72+7)/8+32)*(ph*75/72)+512;
    if(begin<0)return -1;
    /* Reserve page headers/trailers too, before writing any raster bytes. */
    if((uint64_t)begin+bound>CP_MAX_BYTES) { errno=EFBIG;return -1; }
    if(driver==CP_POSTSCRIPT) {
        fprintf(f,"%%%%Page: %u %u\ngsave %u %u translate %u %u scale\n%u %u 8 [%u 0 0 -%u 0 %u]\n"
                  "{currentfile %u string readhexstring pop} false 3 colorimage\n",
                number,number,p->left,h-p->top-ph,pw,ph,pw,ph,pw,ph,ph,pw*3);
        for(unsigned y=0;y<ph;y++) {
            const uint32_t *row=(const uint32_t *)(pixels+(size_t)y*stride);
            for(unsigned x=0;x<pw;x++)fprintf(f,"%06x",row[x]&0xffffffu);
            fputc('\n',f);
        }
        fputs("grestore showpage\n",f);
    } else {
        unsigned dw=pw*75/72,dh=ph*75/72,bytes=(dw+7)/8;
        unsigned char row[319]; /* G31 maximum paper width, 2448 points. */
        if(!dw || !dh || bytes>sizeof(row)) { errno=EINVAL;return -1; }
        fprintf(f,"\033&l%uO\033&l%uA\033*p%ux%uY\033*t75R\033*r%uS\033*r%uT\033*r1A\033*b0M",
                p->landscape,paper(p->width,p->height),p->left*300/72,p->top*300/72,dw,dh);
        for(unsigned y=0;y<dh;y++) {
            memset(row,0,bytes);
            const uint32_t *source=(const uint32_t *)(pixels+(size_t)((uint64_t)y*ph/dh)*stride);
            for(unsigned x=0;x<dw;x++) {
                uint32_t c=source[(uint64_t)x*pw/dw];
                unsigned lum=((c>>16&255)*299+(c>>8&255)*587+(c&255)*114)/1000;
                if(lum<128)row[x/8]|=(unsigned char)(128u>>(x%8));
            }
            fprintf(f,"\033*b%uW",bytes);fwrite(row,1,bytes,f);
        }
        fputs("\033*rC\f",f);
    }
    (void)w;
    long at=ftell(f);
    if(ferror(f)) { errno=EIO;return -1; }
    if(at<0)return -1;
    if((unsigned long)at>CP_MAX_BYTES) { errno=EFBIG;return -1; }
    return 0;
}
int ciuki_print_render(void *context,FILE *f,unsigned driver,const struct cp_page *p)
{
    struct browser_window *bw=context;
    struct hlcache_handle *content=browser_window_get_content(bw);
    if(!content || !cp_page_valid(p) || (driver!=CP_POSTSCRIPT && driver!=CP_PCL) ||
       (driver==CP_PCL && !paper(p->width,p->height))) { errno=EINVAL;return -1; }
    unsigned w=p->landscape?p->height:p->width,h=p->landscape?p->width:p->height;
    unsigned pw=w-p->left-p->right,ph=h-p->top-p->bottom;
    int cw=content_get_width(content),ch=content_get_height(content);
    if(cw<=0 || ch<=0) { errno=EINVAL;return -1; }
    /* Scale the complete laid-out document to the paper width. Unlike the
     * upstream print engine this needs no unimplemented HTML content clone and
     * never reformats/mutates the live document. CSS paged media is separate. */
    double scale=(double)pw/cw,total=(double)ch*scale;
    if(total>INT_MAX || total/ph>10000) { errno=EFBIG;return -1; }
    size_t charge=(size_t)pw*ph*4;
    if(ciuki_image_reserve(charge)) { errno=ENOMEM;return -1; }
    nsfb_t *ram=nsfb_new(NSFB_SURFACE_RAM),*old=NULL;
    bool switched=false;
    int error=ENOMEM,result=-1,stride=0;uint8_t *pixels=NULL;
    if(!ram || nsfb_set_geometry(ram,(int)pw,(int)ph,NSFB_FMT_XRGB8888) || nsfb_init(ram))goto done;
    if(nsfb_get_buffer(ram,&pixels,&stride) || !pixels || stride<(int)pw*4) { error=EIO;goto done; }
    old=framebuffer_set_surface(ram);switched=true;
    if(driver==CP_POSTSCRIPT)fprintf(f,"%%!PS-Adobe-3.0\n<< /PageSize [%u %u] >> setpagedevice\n",w,h);
    else fputs("\033E",f);
    struct rect clip={0,0,(int)pw,(int)ph};
    struct redraw_context ctx={.interactive=false,.background_images=true,.plot=&fb_plotters};
    unsigned number=0;
    for(int offset=0;offset<total;) {
        number++;
        if(p->last && number>p->last)break;
        if(selected(p,number)) {
            for(unsigned y=0;y<ph;y++)memset(pixels+(size_t)y*stride,255,(size_t)pw*4);
            struct content_redraw_data data={.x=0,.y=-offset,.width=(int)pw,.height=(int)total,
                .background_colour=0xffffff,.scale=(float)scale};
            if(!content_redraw(content,&data,&clip,&ctx)) { error=EIO;goto done; }
            if(page_output(f,driver,p,w,h,pw,ph,pixels,stride,number)) { error=errno?errno:EIO;goto done; }
        }
        ciuki_ui_keepalive();
        if((unsigned)(INT_MAX-offset)<ph)break;
        offset+=(int)ph;
    }
    fputs(driver==CP_POSTSCRIPT?"%%EOF\n":"\033E",f);
    if(ferror(f)) { error=EIO;goto done; }
    result=0;
done:
    if(switched)framebuffer_set_surface(old);
    if(ram)nsfb_free(ram);
    ciuki_image_release(charge);
    if(result)errno=error;
    return result;
}
static void release_window(void *unused) { (void)unused; }
void ciuki_print_document(struct cp_ui *p,struct du_ui *u,struct browser_window *bw)
{
    if(!bw || !browser_window_get_content(bw))return;
    cp_ui_document(p,u,browser_window_get_title(bw),bw,ciuki_print_render,release_window);
}
