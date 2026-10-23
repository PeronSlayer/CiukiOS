/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/font.h>
#include <parserutils/charset/codec.h>
#include <libnsfb.h>
#include <libnsfb_plot.h>
#include "utils/errors.h"
#include "utils/utf8.h"
#include "netsurf/plot_style.h"
#include "netsurf/layout.h"
#include "framebuffer/font.h"
#include "sdk/netsurf/port/charset.h"
#include "sdk/netsurf/port/font.h"
#include "sdk/netsurf/port/font_work.h"
static size_t measure_calls,processed_bytes;
int __real_ciuki_font_measure(struct ciuki_font_face *,unsigned,const char *,size_t,int32_t *);
int __wrap_ciuki_font_measure(struct ciuki_font_face *f,unsigned size,const char *s,size_t n,int32_t *out)
{ measure_calls++;processed_bytes+=n;return __real_ciuki_font_measure(f,size,s,n,out); }
static void conversion(const char *from,const char *bytes,size_t n,const char *expected)
{
    char *out=NULL;size_t size=999;
    assert(ciuki_charset(bytes,n,from,"UTF-8",&out,&size)==NSERROR_OK);
    assert(size==strlen(expected) && !memcmp(out,expected,size+1));free(out);
}
int main(void)
{
    conversion("UTF-8","h\xc3\xa9\xe2\x82\xac",6,"h\xc3\xa9\xe2\x82\xac");
    conversion("ISO-8859-1","caf\xe9",4,"caf\xc3\xa9");
    conversion("ISO-8859-15","\xa4",1,"\xe2\x82\xac");
    conversion("Windows-1252","\x80\x93x\x94",4,"\xe2\x82\xac\xe2\x80\x9cx\xe2\x80\x9d");
    char *out=(void *)1;size_t n;
    assert(ciuki_charset("x",1,"UTF-16","UTF-8",&out,&n)==NSERROR_BAD_ENCODING && !out);
    assert(ciuki_charset("\xc0\xaf",2,"UTF-8","UTF-8",&out,&n)==NSERROR_BAD_ENCODING);
    assert(ciuki_charset("\xe2\x82",2,"UTF-8","UTF-8",&out,&n)==NSERROR_BAD_ENCODING);
    assert(ciuki_charset("\xed\xa0\x80",3,"UTF-8","UTF-8",&out,&n)==NSERROR_BAD_ENCODING);
    assert(ciuki_charset("x",8*1024*1024+1,"UTF-8","UTF-8",&out,&n)==NSERROR_BAD_SIZE);
    assert(ciuki_charset("\xe2\x82\xac",3,"UTF-8","ISO-8859-1",&out,&n)==NSERROR_BAD_ENCODING);
    assert(utf8_to_html("<&\xe2\x82\xac","ISO-8859-1",5,&out)==NSERROR_OK);
    assert(!strcmp(out,"&#x00003c;&#x000026;&#x0020ac;"));free(out);
    /* Same parserutils decoder used by the HTML/CSS filters: split inside
     * every multibyte sequence, using a four-byte UCS4 output buffer. */
    parserutils_charset_codec *codec;assert(parserutils_charset_codec_create("UTF-8",&codec)==PARSERUTILS_OK);
    const uint8_t *source=(const uint8_t *)"\xe2";size_t left=1;uint32_t data;uint8_t *dest=(void *)&data;size_t room=4;
    assert(parserutils_charset_codec_decode(codec,&source,&left,&dest,&room)==PARSERUTILS_OK && left==0 && room==4);
    source=(const uint8_t *)"\x82\xac";left=2;
    assert(parserutils_charset_codec_decode(codec,&source,&left,&dest,&room)==PARSERUTILS_OK && room==0);
    assert(((uint8_t *)&data)[2]==0x20 && ((uint8_t *)&data)[3]==0xac);
    parserutils_charset_codec_destroy(codec);
    assert(fb_font_init());
    plot_font_style_t style={.family=PLOT_FONT_FAMILY_SANS_SERIF,.size=12*PLOT_STYLE_SCALE,.weight=400,.foreground=0xff};
    const char *text="Ciuki caf\xc3\xa9 \xe2\x82\xac";size_t length=strlen(text);int width;
    assert(fb_font_width(&style,text,length,&width)==NSERROR_OK && width>60);
    /* f6-23: exact byte/size/style keys, not pointer identity or an approximate
     * per-character sum. A cache hit avoids SDK locking and measurement. */
    size_t before=measure_calls;
    char copy[80];strcpy(copy,text);int cached_width;
    assert(fb_font_width(&style,copy,length,&cached_width)==NSERROR_OK && cached_width==width);
    assert(measure_calls==before && ciuki_font_work_read().hits);
    plot_font_style_t other=style;other.flags=FONTF_ITALIC;other.weight=700;
    assert(fb_font_width(&other,copy,length,&cached_width)==NSERROR_OK);
    assert(measure_calls==before+1);
    other=style;other.size+=PLOT_STYLE_SCALE;
    assert(fb_font_width(&other,copy,length,&cached_width)==NSERROR_OK);
    assert(measure_calls==before+2);
    copy[0]='X';assert(fb_font_width(&style,copy,length,&cached_width)==NSERROR_OK);
    assert(measure_calls==before+3);
    size_t offset;int actual;
    assert(fb_font_position(&style,text,length,9999,&offset,&actual)==NSERROR_OK && offset==length && actual==width);
    assert(fb_font_position(&style,text,length,-1,&offset,&actual)==NSERROR_OK && !offset && !actual);
    assert(framebuffer_layout_table->split(&style,text,length,width/2,&offset,&actual)==NSERROR_OK && offset>0 && offset<length);
    int measured;assert(fb_font_width(&style,text,offset,&measured)==NSERROR_OK && measured==actual);
    nsfb_t *fb=nsfb_new(NSFB_SURFACE_RAM);assert(fb);
    assert(!nsfb_set_geometry(fb,240,60,NSFB_FMT_XRGB8888) && !nsfb_init(fb));
    uint8_t *raw;int stride;nsfb_get_buffer(fb,&raw,&stride);memset(raw,0,(size_t)stride*60);
    nsfb_bbox_t clip={10,4,150,45};nsfb_plot_set_clip(fb,&clip);
    assert(ciuki_font_plot(fb,&style,8,30,text,length)==NSERROR_OK);
    unsigned painted=0;
    for(int y=0;y<60;y++)for(int x=0;x<240;x++) {
        uint32_t pixel=((uint32_t *)(raw+(size_t)y*stride))[x];
        if(x<10 || x>=150 || y<4 || y>=45)assert(!pixel);
        else if(pixel)painted++;
    }
    assert(painted>50);
    style.size*=2;int wide;assert(fb_font_width(&style,text,length,&wide)==NSERROR_OK && wide>width*3/2);
    assert(fb_font_width(&style,"\xf0\x9f\x90\x95",4,&wide)==NSERROR_OK && wide>0); /* missing-glyph fallback */
    char long_run[8193];memset(long_run,'x',8192);long_run[8192]=0;
    measure_calls=processed_bytes=0;
    assert(framebuffer_layout_table->split(&style,long_run,8192,5,&offset,&actual)==NSERROR_OK && offset==8192);
    assert(measure_calls==1 && processed_bytes==8192);
    measure_calls=processed_bytes=0;
    assert(fb_font_position(&style,long_run,8192,32768,&offset,&actual)==NSERROR_OK);
    assert(measure_calls<=16 && processed_bytes<=16*8192);
    assert(fb_font_width(&style,long_run,offset,&measured)==NSERROR_OK && measured==actual);
    for(unsigned i=1;i<8192;i+=2)long_run[i]=' ';
    measure_calls=processed_bytes=0;
    assert(framebuffer_layout_table->split(&style,long_run,8192,32768,&offset,&actual)==NSERROR_OK);
    assert(measure_calls<=16 && processed_bytes<=16*8192);
    assert(fb_font_width(&style,long_run,offset,&measured)==NSERROR_OK && measured==actual);
    assert(fb_font_width(&style,long_run,8193,&measured)==NSERROR_BAD_SIZE);
    nsfb_free(fb);assert(fb_font_finalise());
    /* fx-15: the body faces opened on the start-up worker are adopted at the
     * first use: no open on the UI thread, identical metrics. */
    ciuki_font_prefetch();
    struct ciuki_font_work pre=ciuki_font_work_read();
    int prefetched;assert(fb_font_width(&style,text,length,&prefetched)==NSERROR_OK);
    plot_font_style_t bold=style;bold.weight=700;bold.size=12*PLOT_STYLE_SCALE;style.size=12*PLOT_STYLE_SCALE;
    assert(fb_font_width(&style,text,length,&prefetched)==NSERROR_OK && prefetched==width);
    int heavy;assert(fb_font_width(&bold,text,length,&heavy)==NSERROR_OK && heavy>=width);
    assert(ciuki_font_work_read().opens==pre.opens);
    plot_font_style_t serif=style;serif.family=PLOT_FONT_FAMILY_SERIF;
    int other_width;assert(fb_font_width(&serif,text,length,&other_width)==NSERROR_OK);
    assert(ciuki_font_work_read().opens==pre.opens+1);
    assert(fb_font_finalise());
    puts("NetSurf charset/font: four encodings, errors, split codec, metrics, positions, scale and clipped paint passed");
    fflush(stdout);
    return 0;
}
