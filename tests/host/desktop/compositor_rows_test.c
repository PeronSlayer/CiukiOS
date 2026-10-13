/* SPDX-License-Identifier: MIT
 * Compare optimized rows with the unmodified shared theme painter. Include
 * the production unit to exercise its private frame cache directly. */
#include "../../../apps/desktop/compositor.c"
#include "../../../apps/libdeskui/deskui.h"
#include <assert.h>

enum { RW=192, RH=144 };
static uint32_t a[RW*RH], b[RW*RH], icon_pixels[16*16];
static int fail_allocation;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n)
{
    if (fail_allocation) { fail_allocation=0; return NULL; }
    return __real_malloc(n);
}

static void background(unsigned seed)
{
    for (unsigned i=0; i<RW*RH; i++) {
        seed=seed*1664525u+1013904223u;
        a[i]=b[i]=seed; /* includes X bytes, preserved by alpha-zero skips */
    }
}

static void frames(void)
{
    static const char *const themes[]={"Platinum","Ciuki Blue","Classic 98"};
    for (unsigned t=0; t<3; t++) {
        assert(!desk_theme_use(themes[t]));
        for (int k=0; k<90; k++) {
            struct desk_box f={k%11-5,k%13-6,38+k%21*7,28+k%17*5};
            if (k%9==0) f.w=5+k%4; /* fallback for narrow frames */
            struct desk_frame_style s={.title=k%2 ? "Ciuki \xc3\xa0 \xe2\x80\xa2 \xe2\x80\xa6" : "Caption",
                .icon=k%3 ? icon_pixels : NULL, .active=k&1, .alert=(k>>1)&1,
                .collapsed=(k>>2)&1, .buttons=DESK_TB_CLOSE|DESK_TB_MINIMIZE|DESK_TB_COLLAPSE,
                .hot=1u<<(k%3), .pressed=k%2 ? 1u<<(k%3) : 0};
            for (unsigned i=0; i<16*16; i++) icon_pixels[i]=i*812371u+(unsigned)k*73171u;
            for (int repeat=0; repeat<3; repeat++) {
                background((unsigned)k*3u+(unsigned)repeat);
                struct desk_paint p={a,RW,RH,repeat ? 7 : 0,repeat ? 5 : 0,
                    repeat ? 91 : RW,repeat ? 83 : RH};
                struct desk_box v;
                if (!desk_intersect(f,(struct desk_box){p.cx,p.cy,p.cw,p.ch},&v) ||
                    !desk_intersect(v,(struct desk_box){0,0,RW,RH},&v)) continue;
                p.cx=v.x; p.cy=v.y; p.cw=v.w; p.ch=v.h;
                struct desk_paint q=p; q.pixels=b;
                frame_border(&p,k%4,f,&s);
                desk_paint_frame_border(&q,f.x,f.y,f.w,f.h,&s);
                if (memcmp(a,b,sizeof(a))) {
                    for (unsigned i=0; i<RW*RH; i++) if (a[i]!=b[i]) {
                        fprintf(stderr,"frame mismatch theme=%s case=%d repeat=%d frame=%d,%d,%d,%d pixel=%u,%u got=%08x want=%08x\n",
                                themes[t],k,repeat,f.x,f.y,f.w,f.h,i%RW,i/RW,a[i],b[i]); break;
                    }
                    assert(!"frame mismatch");
                }
                f.x++; f.y++; /* cached caption moves over a different underlay */
            }
        }
    }
}

static void shadows(void)
{
    for (int k=0; k<96; k++) {
        background((unsigned)k);
        int x=k%19-8,y=k%13-6,w=25+k%11*7,h=20+k%17*5,size=k%20;
        struct desk_paint p={a,RW,RH,k%7,k%5,130,105}, q=p; q.pixels=b;
        desk_shadow_rows(&p,x,y,w,h,size,0x182736u);
        desk_paint_shadow(&q,x,y,w,h,size,0x182736u);
        assert(!memcmp(a,b,sizeof(a)));
    }
}

static void text(void)
{
    static const char *const themes[]={"Platinum","Ciuki Blue","Classic 98"};
    static const char sample[]="ABC abc 012 \xc3\x80\xc3\xa0\xc3\xbf \xe2\x80\xa2 \xe2\x80\xa6 \xf0\x9f\x90\xb6";
    for (unsigned t=0; t<3; t++) for (int font=0; font<DESK_FONTS; font++) for (int n=0; n<8; n++) {
        assert(!desk_theme_use(themes[t])); background((unsigned)(font*8+n));
        struct du_canvas c={a,RW,RH,{n%3*17,n%4*11,115,85}};
        struct desk_paint p={b,RW,RH,c.clip.x,c.clip.y,c.clip.w,c.clip.h};
        int x=n*13-20,y=n*9-8;
        du_text_font(&c,font,x,y,sample,0xac3218u);
        desk_paint_text(&p,font,x,y+desk_font_cap(font),sample,0xac3218u);
        assert(!memcmp(a,b,sizeof(a)));
    }
    /* Independent channel oracle for every alpha and channel value; the
     * three lanes use different inputs so cross-lane carries are detected. */
    for (unsigned alpha=0; alpha<256; alpha++) for (unsigned value=0; value<256; value++) {
        uint32_t src=(alpha<<24)|(value<<16)|((255-value)<<8)|((value*73)&255u);
        uint32_t old=0xfeedba98u, got=old;
        struct du_canvas c={&got,1,1,{0,0,1,1}};
        du_icon_blit(&c,0,0,&src,1,DU_ICON_NORMAL);
        uint32_t expected=alpha ? desk_mix(src,old,alpha) : old;
        assert(got==expected);
    }
}

static void visibility(void)
{
    static struct desktop d;
    uint32_t pixel=0;
    int windows[DESK_CLIENTS];
    struct desk_box boxes[DESK_DAMAGE_MAX], clip={5,9,140,107};
    for (unsigned k=0; k<96; k++) {
        unsigned count=k%(DESK_CLIENTS+1);
        for (unsigned i=0; i<count; i++) {
            struct desk_client *c=&d.clients[i]; windows[i]=(int)i;
            c->pixels=&pixel; c->internal=i%5==1; c->collapsed=(i+k)%7==0;
            c->frame=desk_frame(43,31,(int)((i*37+k)%140)-10,(int)((i*17+k)%110)-20);
            c->info.width=i%3 ? 43 : 17; c->info.height=i%4 ? 31 : 11;
        }
        unsigned n=visible_boxes(&d,clip,windows,count,boxes);
        assert(n<=DESK_DAMAGE_MAX);
        int fallback=n==1 && !memcmp(&boxes[0],&clip,sizeof(clip));
        for (int y=0; y<RH; y++) for (int x=0; x<RW; x++) {
            int covered=0, hidden=0;
            for (unsigned i=0; i<n; i++) covered+=desk_contains(boxes[i],x,y);
            assert(covered<=1);
            if (!desk_contains(clip,x,y)) { assert(!covered); continue; }
            for (unsigned i=0; i<count; i++) {
                struct desk_client *c=&d.clients[i];
                struct desk_box well=desk_content(c);
                hidden|=!c->internal && !c->collapsed && desk_contains(well,x,y) &&
                    desk_contains((struct desk_box){well.x,well.y,(int)c->info.width,(int)c->info.height},x,y);
            }
            assert(fallback ? covered==1 : covered==!hidden);
        }
    }
    /* Sixteen separated tiny opaque holes exceed the rectangle budget. */
    for (int i=0; i<DESK_CLIENTS; i++) {
        struct desk_client *c=&d.clients[i];
        windows[i]=i; c->internal=c->collapsed=0; c->pixels=&pixel;
        c->info.width=c->info.height=2;
        c->frame=desk_frame(2,2,10+i*7,5+i*5);
    }
    unsigned n=visible_boxes(&d,clip,windows,DESK_CLIENTS,boxes);
    assert(n==1 && !memcmp(&boxes[0],&clip,sizeof(clip)));
}

int main(void)
{
    frames(); shadows(); text(); visibility();
    struct desktop d={0}; desk_render_release(&d);
    background(37);
    struct desk_paint p={a,RW,RH,0,0,RW,RH}, q=p; q.pixels=b;
    struct desk_box f={0,0,150,120};
    struct desk_frame_style style={.title="No memory",.active=1,.buttons=DESK_TB_CLOSE};
    fail_allocation=1;
    frame_border(&p,0,f,&style); assert(!fail_allocation);
    desk_paint_frame_border(&q,f.x,f.y,f.w,f.h,&style);
    assert(!memcmp(a,b,sizeof(a)));
    puts("PASS compositor rows: frame cache invalidation/moves/clips/allocation fallback, shadow oracle, fonts/UTF-8, every icon alpha, opaque visibility/fragmentation fallback");
    return 0;
}
