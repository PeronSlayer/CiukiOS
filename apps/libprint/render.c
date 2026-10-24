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

/* ---- G28b: formatted documents ------------------------------------------------------------------------------------------ */

void cp_doc_free(struct cp_doc *d)
{
    for (unsigned i = 0; i < d->nimages; i++) free(d->images[i].pixels);
    free(d->images); free(d->ops); free(d->text); free(d->plain);
    memset(d, 0, sizeof(*d));
}
/* The Windows-1252 byte of the next UTF-8 character of s (advancing *i), or '?' when the page code has none. */
static unsigned cp1252_next(const unsigned char *s, size_t n, size_t *i)
{
    static const struct { unsigned cp, byte; } special[] = {{0x20ac, 0x80}, {0x201a, 0x82}, {0x0192, 0x83}, {0x201e, 0x84}, {0x2026, 0x85}, {0x2020, 0x86}, {0x2021, 0x87},
        {0x02c6, 0x88}, {0x2030, 0x89}, {0x0160, 0x8a}, {0x2039, 0x8b}, {0x0152, 0x8c}, {0x017d, 0x8e}, {0x2018, 0x91}, {0x2019, 0x92}, {0x201c, 0x93}, {0x201d, 0x94},
        {0x2022, 0x95}, {0x2013, 0x96}, {0x2014, 0x97}, {0x02dc, 0x98}, {0x2122, 0x99}, {0x0161, 0x9a}, {0x203a, 0x9b}, {0x0153, 0x9c}, {0x017e, 0x9e}, {0x0178, 0x9f}};
    unsigned c = s[*i], len = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1, cp = c;
    if (len > 1 && *i + len <= n) { cp = c & (0xffu >> (len + 1)); for (unsigned k = 1; k < len; k++) cp = cp << 6 | (s[*i + k] & 63); }
    else if (len > 1) len = 1;
    *i += len;
    if (cp < 0x80 || (cp >= 0xa0 && cp <= 0xff)) return cp;
    for (unsigned k = 0; k < sizeof(special) / sizeof(*special); k++) if (special[k].cp == cp) return special[k].byte;
    return '?';
}
static void ps_text(FILE *f, const char *s, size_t n)
{
    const unsigned char *u = (const unsigned char *)s;
    fputc('(', f);
    for (size_t i = 0; i < n;) {
        unsigned b = cp1252_next(u, n, &i);
        if (b >= 0x80 && b < 0xa0) {   /* ISOLatin1Encoding has nothing here: the nearest plain form */
            const char *r = b == 0x91 || b == 0x92 ? "'" : b == 0x93 || b == 0x94 ? "\"" : b == 0x96 || b == 0x97 ? "-" : b == 0x95 ? "\267" : b == 0x85 ? "..." : "?";
            for (; *r; r++) { unsigned c = (unsigned char)*r; if (c >= 0x80) fprintf(f, "\\%03o", c); else fputc((int)c, f); }
        } else if (b == '(' || b == ')' || b == '\\') { fputc('\\', f); fputc((int)b, f); }
        else if (b < 32 || b >= 127) fprintf(f, "\\%03o", b);
        else fputc((int)b, f);
    }
    fputc(')', f);
}
static void pt2(FILE *f, int twips)   /* twips as points with two decimals */
{
    int a = twips < 0 ? -twips : twips;
    fprintf(f, "%s%d.%02d", twips < 0 ? "-" : "", a / 20, (a % 20) * 5);
}
static unsigned image_step(const struct cp_image *im)
{
    unsigned step = 1;
    while ((uint64_t)(im->width / step) * (im->height / step) > 1200000u) step++;
    return step;
}
static int doc_valid(const struct cp_doc *d)
{
    if (!d->pages || d->pages > 100000 || (d->nops && !d->ops)) return 0;
    uint32_t last = 1;
    for (unsigned i = 0; i < d->nops; i++) {
        const struct cp_op *o = &d->ops[i];
        if (o->page < last || o->page > d->pages) return 0;
        last = o->page;
        if (o->kind == CP_OP_TEXT && ((uint64_t)o->offset + o->length > d->text_bytes || o->family > 2 || o->size < 2 || o->size > 1000)) return 0;
        if (o->kind == CP_OP_IMAGE && (o->offset >= d->nimages || !d->images[o->offset].pixels || !d->images[o->offset].width || !d->images[o->offset].height)) return 0;
        if (o->kind > CP_OP_IMAGE) return 0;
    }
    return 1;
}
static void pcl_image(FILE *f, const struct cp_image *im, int x, int y, int w, int h)
{
    unsigned dw = (unsigned)((int64_t)w * 75 / 1440), dh = (unsigned)((int64_t)h * 75 / 1440);
    if (!dw || !dh || dw > 600) return;
    fprintf(f, "\033*p%dx%dY\033*t75R\033*r%uS\033*r%uT\033*r1A\033*b0M", (int)((int64_t)x * 300 / 1440), (int)((int64_t)y * 300 / 1440), dw, dh);
    unsigned rowbytes = (dw + 7) / 8; unsigned char row[75];
    if (rowbytes > sizeof(row)) { fputs("\033*rC", f); return; }
    for (unsigned yy = 0; yy < dh; yy++) {
        memset(row, 0, rowbytes);
        unsigned sy = (unsigned)((uint64_t)yy * im->height / dh);
        for (unsigned xx = 0; xx < dw; xx++) {
            uint32_t c = im->pixels[(size_t)sy * im->width + (unsigned)((uint64_t)xx * im->width / dw)];
            unsigned lum = ((c >> 16 & 255) * 299 + (c >> 8 & 255) * 587 + (c & 255) * 114) / 1000;
            if (lum < 128) row[xx / 8] |= (unsigned char)(128u >> (xx % 8));
        }
        fprintf(f, "\033*b%uW", rowbytes); fwrite(row, 1, rowbytes, f);
    }
    fputs("\033*rC", f);
}
int cp_submit_doc(const char *r, unsigned printer, const char *title, const struct cp_doc *d, const struct cp_page *p, const char *target)
{
    if (!d || !doc_valid(d)) { errno = EINVAL; return -1; }
    struct cp_page page; unsigned driver, w, h;
    if (options(r, &printer, p, &page, &driver)) return -1;
    if (driver == CP_TEXT) {
        if (!d->plain) { errno = EINVAL; return -1; }
        return cp_submit_text(r, printer, title, d->plain, d->plain_bytes, &page, target);
    }
    geometry(&page, &w, &h);
    char temp[CP_PATH + 96]; FILE *f = cp_tempfile(r, temp, sizeof(temp));
    if (!f) return -1;
    static const char *const ps_face[3][4] = {{"Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique"},
                                              {"Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic"},
                                              {"Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique"}};
    static const unsigned pcl_face[3] = {4148, 4101, 3};
    if (driver == CP_POSTSCRIPT) {
        fprintf(f, "%%!PS-Adobe-3.0\n<< /PageSize [%u %u] >> setpagedevice\n", w, h);
        fputs("/relatin { exch findfont dup length dict begin { 1 index /FID ne {def} {pop pop} ifelse } forall /Encoding ISOLatin1Encoding def currentdict end definefont pop } def\n", f);
        for (unsigned fam = 0; fam < 3; fam++) for (unsigned st = 0; st < 4; st++) fprintf(f, "/%s /F%u relatin\n", ps_face[fam][st], fam * 4 + st);
    } else fprintf(f, "\033E\033&l%uO\033&l%uA\033&l6D\033&l0E\033&u300D\033(19U", page.landscape, pcl_paper(&page));
    unsigned i = 0; int any = 0;
    for (unsigned pg = 1; pg <= d->pages; pg++) {
        int active = selected(&page, pg);
        unsigned cur_font = ~0u, cur_size = 0; uint32_t cur_rgb = 0xffffffffu;
        int open = 0;
        for (; i < d->nops && d->ops[i].page == pg; i++) {
            const struct cp_op *o = &d->ops[i];
            if (!active) continue;
            open = 1;
            if (o->kind == CP_OP_TEXT) {
                unsigned st = (o->flags & 1 ? 1u : 0u) | (o->flags & 2 ? 2u : 0u), font = o->family * 4 + st;
                if (driver == CP_POSTSCRIPT) {
                    if (font != cur_font || o->size != cur_size) { fprintf(f, "/F%u findfont ", font); fprintf(f, "%u.%u scalefont setfont\n", o->size / 2u, (o->size % 2u) * 5u); cur_font = font; cur_size = o->size; }
                    if (o->rgb != cur_rgb) { fprintf(f, "%.3f %.3f %.3f setrgbcolor\n", (o->rgb >> 16 & 255) / 255.0, (o->rgb >> 8 & 255) / 255.0, (o->rgb & 255) / 255.0); cur_rgb = o->rgb; }
                    pt2(f, o->x); fputc(' ', f); pt2(f, (int)(h * 20) - o->y); fputs(" moveto ", f);
                    ps_text(f, d->text + o->offset, o->length); fputs(" show\n", f);
                } else {
                    fprintf(f, "\033(s%dp%u.%uv%us%ub%uT", o->family == 2 ? 0 : 1, o->size / 2u, (o->size % 2u) * 5u, (o->flags & 2) != 0, (o->flags & 1) ? 3 : 0, pcl_face[o->family]);
                    fprintf(f, "\033*p%dx%dY", (int)((int64_t)o->x * 300 / 1440), (int)((int64_t)o->y * 300 / 1440));
                    const unsigned char *u = (const unsigned char *)d->text + o->offset;
                    for (size_t k = 0; k < o->length;) { unsigned b = cp1252_next(u, o->length, &k); fputc((int)b, f); }
                }
            } else if (o->kind == CP_OP_RULE) {
                if (driver == CP_POSTSCRIPT) {
                    fprintf(f, "%.3f %.3f %.3f setrgbcolor ", (o->rgb >> 16 & 255) / 255.0, (o->rgb >> 8 & 255) / 255.0, (o->rgb & 255) / 255.0); cur_rgb = o->rgb;
                    pt2(f, o->x); fputc(' ', f); pt2(f, (int)(h * 20) - o->y - o->h); fputc(' ', f); pt2(f, o->w); fputc(' ', f); pt2(f, o->h); fputs(" rectfill\n", f);
                } else {
                    unsigned rw = (unsigned)((int64_t)o->w * 300 / 1440), rh = (unsigned)((int64_t)o->h * 300 / 1440);
                    fprintf(f, "\033*p%dx%dY\033*c%uA\033*c%uB\033*c0P", (int)((int64_t)o->x * 300 / 1440), (int)((int64_t)o->y * 300 / 1440), rw ? rw : 1, rh ? rh : 1);
                }
            } else {
                const struct cp_image *im = &d->images[o->offset];
                if (driver == CP_POSTSCRIPT) {
                    unsigned step = image_step(im), iw = im->width / step, ih = im->height / step;
                    if (!iw || !ih) continue;
                    fputs("gsave ", f); pt2(f, o->x); fputc(' ', f); pt2(f, (int)(h * 20) - o->y - o->h); fputs(" translate ", f); pt2(f, o->w); fputc(' ', f); pt2(f, o->h);
                    fprintf(f, " scale\n%u %u 8 [%u 0 0 -%u 0 %u]\n{currentfile %u string readhexstring pop} false 3 colorimage\n", iw, ih, iw, ih, ih, iw * 3);
                    for (unsigned y = 0; y < ih; y++) { for (unsigned x = 0; x < iw; x++) fprintf(f, "%06x", im->pixels[(size_t)(y * step) * im->width + x * step] & 0xffffffu); fputc('\n', f); }
                    fputs("grestore\n", f);
                    cur_rgb = 0xffffffffu;
                } else pcl_image(f, im, o->x, o->y, o->w, o->h);
            }
        }
        if (active) { fputs(driver == CP_POSTSCRIPT ? "showpage\n" : "\f", f); any = 1; }
        (void)open;
    }
    if (driver == CP_POSTSCRIPT) fputs("%%EOF\n", f); else fputs("\033E", f);
    if (!any) { /* the page range selected nothing: still a valid, empty job */ }
    return finish(f, temp, r, printer, title, &page, target);
}
