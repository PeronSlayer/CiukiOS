/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static unsigned pcl_paper(const struct cp_page *p)
{
    if(p->width==612 && p->height==792)return 2;
    if(p->width==612 && p->height==1008)return 3;
    if(p->width==595 && p->height==842)return 26;
    return 0;
}
static int options(const char *r,unsigned *printer,const struct cp_page *p,struct cp_page *out,unsigned *driver)
{
    struct cp_config *c=malloc(sizeof(*c));if(!c)return -1;int e=cp_config_load(r,c);
    if(!e && *printer==CP_PRINTERS)*printer=c->default_printer;
    if(!e && (*printer>=CP_PRINTERS || !c->printers[*printer].present)){errno=EINVAL;e=-1;}
    if(!e){*out=p?*p:c->printers[*printer].page;*driver=c->printers[*printer].driver;if(!cp_page_valid(out) || (*driver==CP_PCL && !pcl_paper(out))){errno=EINVAL;e=-1;}}
    free(c);return e;
}
static int selected(const struct cp_page *p,unsigned n){return (!p->first || n>=p->first) && (!p->last || n<=p->last);}
static void geometry(const struct cp_page *p,unsigned *w,unsigned *h){*w=p->landscape?p->height:p->width;*h=p->landscape?p->width:p->height;}
static void ps_begin(FILE *f,const struct cp_page *p,unsigned w,unsigned h)
{fprintf(f,"%%!PS-Adobe-3.0\n<< /PageSize [%u %u] >> setpagedevice\n/Courier findfont 10 scalefont setfont\n",w,h);(void)p;}
static void ps_line(FILE *f,const char *line,unsigned n,unsigned x,unsigned y)
{
    fprintf(f,"%u %u moveto (",x,y);
    for(unsigned i=0;i<n;i++){unsigned c=(unsigned char)line[i];if(c=='(' || c==')' || c=='\\')fputc('\\',f);fputc((int)c,f);}fputs(") show\n",f);
}
static int finish(FILE *f,const char *temp,const char *r,unsigned printer,const char *title,const struct cp_page *p,const char *target)
{
    int id=-1;if(!ferror(f) && !fflush(f))id=cp_submit_file(r,printer,title,temp,p,target);cp_tempclose(f,temp);return id;
}
int cp_submit_text(const char *r,unsigned printer,const char *title,const void *v,size_t bytes,const struct cp_page *p,const char *target)
{
    if((!v && bytes)||bytes>CP_MAX_BYTES){errno=EINVAL;return -1;}
    struct cp_page page;unsigned driver,w,h;if(options(r,&printer,p,&page,&driver))return -1;geometry(&page,&w,&h);
    if(page.left+page.right>=w || page.top+page.bottom+12>h){errno=EINVAL;return -1;}
    unsigned cols=(w-page.left-page.right)/6,rows=(h-page.top-page.bottom)/12;
    if(!cols || cols>408 || !rows){errno=EINVAL;return -1;}
    char temp[CP_PATH+96];FILE *f=cp_tempfile(r,temp,sizeof(temp));if(!f)return -1;
    if(driver==CP_POSTSCRIPT)ps_begin(f,&page,w,h);
    if(driver==CP_PCL)fprintf(f,"\033E\033&l%uO\033&l%uA\033&l6D\033&l0E\033&u300D\033(s0p10h12v0s0b3T",page.landscape,pcl_paper(&page));
    const unsigned char *s=v;size_t at=0;unsigned number=1,row=0;int active=selected(&page,number),any=0;
    while(at<bytes || (!bytes && !any)){
        char line[409];unsigned n=0;int eject=0;
        while(at<bytes && n<cols){unsigned c=s[at++];if(c=='\r'){if(at<bytes && s[at]=='\n')at++;break;}if(c=='\n')break;if(c=='\f'){eject=1;break;}if(c=='\t'){unsigned spaces=8-n%8;while(spaces-- && n<cols)line[n++]=' ';}else line[n++]=(char)(c>=32 && c<127?c:'?');}
        if(active){
            if(driver==CP_POSTSCRIPT)ps_line(f,line,n,page.left,h-page.top-10-row*12);
            else if(driver==CP_PCL){fprintf(f,"\033*p%ux%uY",page.left*300/72,(page.top+10+row*12)*300/72);fwrite(line,1,n,f);}
            else{if(!row){for(unsigned t=0;t<page.top/12;t++)fputs("\r\n",f);}for(unsigned x=0;x<page.left/6;x++)fputc(' ',f);fwrite(line,1,n,f);fputs("\r\n",f);}
        }
        row++;any=1;
        if(row>=rows || eject){if(active)fputs(driver==CP_POSTSCRIPT?"showpage\n":"\f",f);number++;row=0;active=selected(&page,number);}
    }
    if(row && active)fputs(driver==CP_POSTSCRIPT?"showpage\n":"\f",f);
    if(driver==CP_POSTSCRIPT)fputs("%%EOF\n",f);if(driver==CP_PCL)fputs("\033E",f);
    return finish(f,temp,r,printer,title,&page,target);
}
int cp_submit_image(const char *r,unsigned printer,const char *title,const uint32_t *pixels,unsigned width,unsigned height,const struct cp_page *p,const char *target)
{
    if(!pixels || !width || !height || width>4096 || height>4096 || (uint64_t)width*height>CP_MAX_BYTES/4){errno=EINVAL;return -1;}
    struct cp_page page;unsigned driver,w,h;if(options(r,&printer,p,&page,&driver))return -1;
    if(driver==CP_TEXT){errno=EINVAL;return -1;}geometry(&page,&w,&h);
    if(page.left+page.right>=w || page.top+page.bottom>=h){errno=EINVAL;return -1;}
    unsigned dw=w-page.left-page.right,dh=h-page.top-page.bottom;
    if((uint64_t)dw*height>(uint64_t)dh*width)dw=(unsigned)((uint64_t)dh*width/height);else dh=(unsigned)((uint64_t)dw*height/width);
    if(!dw || !dh){errno=EINVAL;return -1;}
    char temp[CP_PATH+96];FILE *f=cp_tempfile(r,temp,sizeof(temp));if(!f)return -1;
    if(selected(&page,1)){
        if(driver==CP_POSTSCRIPT){
            ps_begin(f,&page,w,h);fprintf(f,"gsave %u %u translate %u %u scale\n%u %u 8 [%u 0 0 -%u 0 %u]\n{currentfile %u string readhexstring pop} false 3 colorimage\n",page.left,h-page.top-dh,dw,dh,width,height,width,height,height,width*3);
            for(unsigned y=0;y<height;y++){for(unsigned x=0;x<width;x++)fprintf(f,"%06x",pixels[(size_t)y*width+x]&0xffffffu);fputc('\n',f);}fputs("grestore showpage\n%%EOF\n",f);
        }else{
            /* PCL 5 standard 75 dpi raster, fit in the printable point bounds. */
            dw=dw*75/72;dh=dh*75/72;
            fprintf(f,"\033E\033&l%uO\033&l%uA\033*p%ux%uY\033*t75R\033*r%uS\033*r%uT\033*r1A\033*b0M",page.landscape,pcl_paper(&page),page.left*300/72,page.top*300/72,dw,dh);
            unsigned rowbytes=(dw+7)/8;unsigned char row[319];
            for(unsigned y=0;y<dh;y++){
                memset(row,0,rowbytes);unsigned sy=(unsigned)((uint64_t)y*height/dh);
                for(unsigned x=0;x<dw;x++){uint32_t c=pixels[(size_t)sy*width+(unsigned)((uint64_t)x*width/dw)];unsigned lum=((c>>16&255)*299+(c>>8&255)*587+(c&255)*114)/1000;if(lum<128)row[x/8]|=(unsigned char)(128u>>(x%8));}
                fprintf(f,"\033*b%uW",rowbytes);fwrite(row,1,rowbytes,f);
            }
            fputs("\033*rC\f\033E",f);
        }
    }
    return finish(f,temp,r,printer,title,&page,target);
}
