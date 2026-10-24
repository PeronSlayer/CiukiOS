/* SPDX-License-Identifier: MIT. The production selection publication path. */
#define CIUKI_SYSTEM_CLIPBOARD 1
#include "../../../apps/paint/paint.c"
#include <assert.h>
static struct clip_store shared;
static unsigned messages;
static int transport(void *ctx,struct desk_clip_wire *w,const void *data,struct desk_clip_wire *r,void **out)
{ return clip_request(ctx,1,w,data,r,out); }
void du_message(struct du_ui *u,int id,const char *title,const char *text,int icon,const char *const *buttons,unsigned count,int def,int cancel)
{ (void)u;(void)id;(void)title;(void)text;(void)icon;(void)buttons;(void)count;(void)def;(void)cancel; messages++; }
int main(void)
{
    clip_init(&shared); du_clip_bind(transport,&shared);
    for (unsigned i=0;i<sizeof(pal);i++) pal[i]=(i*7)%64;
    assert(img_alloc(&cv,3,2)); sel_on=1; selx=sely=0; selw=3; selh=2; bg=80;
    for (int i=0;i<6;i++) cv.pixels[i]=16+i; cv.pixels[5]=MASKED_PIXEL;
    assert(copy_selection() && !messages);
    cv.pixels[0]=90;
    void *dib=NULL; uint32_t n; assert(!du_clip_open() && !du_clip_get(8,&dib,&n) && !du_clip_close());
    uint32_t *rgb=NULL; unsigned width,height; assert(!clip_dib_decode(dib,n,&rgb,&width,&height) && width==3 && height==2);
    for (unsigned i=0;i<6;i++) {
        unsigned c=i==5 ? bg : 16+i;
        unsigned expected=(((pal[c*3]*255+31)/63)<<16)|(((pal[c*3+1]*255+31)/63)<<8)|((pal[c*3+2]*255+31)/63);
        assert(rgb[i]==expected);
        for (unsigned component=0;component<3;component++) {
            unsigned six=pal[c*3+component]; assert((((six*255+31)/63)*63+127)/255==six);
        }
    }
    free(rgb); free(dib);
    /* Paste actually enters Paint's floating selection through the shared
     * decoder and palette mapping, not the former private selection copy. */
    paste(); assert(floating && flt.w==3 && flt.h==2 && !messages);
    for (unsigned i=0;i<6;i++) {
        unsigned source=i==5 ? bg : 16+i, pasted=flt.pixels[i];
        assert(!memcmp(pal+source*3,pal+pasted*3,3));
    }
    cv.pixels[0]=90;

    struct desk_clip_wire w={.op=DESK_CLIP_OPEN},r; void *unused=NULL;
    assert(!clip_request(&shared,2,&w,NULL,&r,&unused));
    assert(!copy_selection() && messages==1 && cv.pixels[0]==90);
    w.op=DESK_CLIP_CLOSE; assert(!clip_request(&shared,2,&w,NULL,&r,&unused));
    img_free(&cv); img_free(&clip); img_free(&flt); img_free(&pool); du_clip_bind(NULL,NULL); clip_destroy(&shared);
    puts("PASS G5 Paint selection to shared DIB and shared DIB to floating selection, palette scaling round-trip, masked background, deep copy, failed Cut preserves source"); fflush(stdout);
}
