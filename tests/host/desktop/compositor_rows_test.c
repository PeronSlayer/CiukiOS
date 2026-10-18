/* SPDX-License-Identifier: MIT
 * Compare optimized rows with the unmodified shared theme painter. Include
 * the production unit to exercise its private frame cache directly. */
#include <string.h>
static unsigned caption_visits,shadow_visits;
#define DESK_PROFILE(name, ...) do { if (!strcmp(name,"captions")) caption_visits++; if (!strcmp(name,"shadows")) shadow_visits++; __VA_ARGS__; } while (0)
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
    struct desktop d={.width=RW,.height=RH};
    render_cache_prepare(&d); assert(d.render_cache);
    static const char *const themes[]={"Platinum","Ciuki Blue"};
    for (unsigned t=0; t<2; t++) {
        assert(!desk_theme_use(themes[t]));
        for (int k=0; k<90; k++) {
            struct desk_box f={k%11-5,k%13-6,38+k%21*7,28+k%17*5};
            if (k%9==0) f.w=5+k%4; /* fallback for narrow frames */
            struct desk_frame_style s={.title=k%2 ? "Ciuki \xc3\xa0 \xe2\x80\xa2 \xe2\x80\xa6" : "Caption",
                .icon=k%3 ? icon_pixels : NULL, .active=k&1, .alert=(k>>1)&1,
                .collapsed=(k>>2)&1, .buttons=DESK_TB_CLOSE|DESK_TB_MINIMIZE|DESK_TB_COLLAPSE,
                .hot=1u<<(k%3), .pressed=k%2 ? 1u<<(k%3) : 0};
            for (unsigned i=0; i<16*16; i++) icon_pixels[i]=i*812371u+(unsigned)k*73171u;
            for (int repeat=0; repeat<2; repeat++) {
                background((unsigned)k*3u+(unsigned)repeat);
                struct desk_paint p={a,RW,RH,repeat ? 7 : 0,repeat ? 5 : 0,
                    repeat ? 91 : RW,repeat ? 83 : RH};
                struct desk_box v;
                if (!desk_intersect(f,(struct desk_box){p.cx,p.cy,p.cw,p.ch},&v) ||
                    !desk_intersect(v,(struct desk_box){0,0,RW,RH},&v)) continue;
                p.cx=v.x; p.cy=v.y; p.cw=v.w; p.ch=v.h;
                struct desk_paint q=p; q.pixels=b;
                frame_border(&d,&p,k%DESK_CLIENTS,f,&s);
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
    desk_render_release(&d);
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
    static const char *const themes[]={"Platinum","Ciuki Blue"};
    static const char sample[]="ABC abc 012 \xc3\x80\xc3\xa0\xc3\xbf \xe2\x80\xa2 \xe2\x80\xa6 \xf0\x9f\x90\xb6";
    for (unsigned t=0; t<2; t++) for (int font=0; font<DESK_FONTS; font++) for (int n=0; n<8; n++) {
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

static unsigned coverage[RW*RH];
static void cover(struct desktop *d, int index, struct desk_box box)
{
    (void)d; (void)index;
    for (int y=box.y;y<box.y+box.h;y++) for (int x=box.x;x<box.x+box.w;x++) coverage[y*RW+x]++;
}
static void visibility(void)
{
    static struct desktop d;
    uint32_t pixel=0;
    int windows[DESK_CLIENTS];
    struct desk_box clip={5,9,140,107};
    for (unsigned k=0;k<112;k++) {
        unsigned count=k<96 ? k%(DESK_CLIENTS+1) : DESK_CLIENTS;
        for (unsigned i=0;i<count;i++) {
            struct desk_client *c=&d.clients[i]; windows[i]=(int)i;
            c->pixels=&pixel; c->internal=i%5==1; c->collapsed=(i+k)%7==0;
            c->frame=k<96 ? desk_frame(43,31,(int)((i*37+k)%140)-10,(int)((i*17+k)%110)-20) :
                desk_frame(2,2,10+i*7,5+i*5);
        }
        memset(coverage,0,sizeof(coverage));
        visible_paint(&d,clip,windows,count,cover,-1);
        for (int y=0;y<RH;y++) for (int x=0;x<RW;x++) {
            int hidden=0;
            for (unsigned i=0;i<count;i++) {
                struct desk_box f=d.clients[i].frame;
                int pad=y<f.y+desk_theme->radius ? desk_theme->radius+1 : 0;
                hidden|=desk_contains((struct desk_box){f.x+pad,f.y,f.w-2*pad,f.h},x,y);
            }
            assert(coverage[y*RW+x]==(unsigned)(desk_contains(clip,x,y) && !hidden));
        }
    }
}

static int uploads, fail_present;
static void dirty_rows(void)
{
    struct desktop d={.width=RW,.height=RH};
    d.rows=calloc(RH,sizeof(*d.rows)); assert(d.rows);
    struct desk_box dirty[DESK_DAMAGE_MAX];
    for (unsigned i=0;i<DESK_DAMAGE_MAX;i++) {
        dirty[i]=(struct desk_box){(int)i*6,5+(int)i%3,3,20};
        rows_add(&d,dirty[i]);
    }
    for (int clipped=0;clipped<2;clipped++) {
        struct desk_box cut=clipped ? (struct desk_box){7,5,1,20} : (struct desk_box){0};
        memset(coverage,0,sizeof(coverage)); uint32_t y=0;
        while (y<RH) {
            struct desk_box boxes[DESK_DAMAGE_MAX+1]; unsigned n=row_boxes(&d,&y,cut,boxes);
            for (unsigned i=0;i<n;i++) cover(&d,0,boxes[i]);
        }
        for (int y=0;y<RH;y++) for (int x=0;x<RW;x++) {
            int wanted=0;
            for (unsigned i=0;i<DESK_DAMAGE_MAX;i++) wanted|=desk_contains(dirty[i],x,y);
            wanted&=!desk_contains(cut,x,y);
            assert(coverage[y*RW+x]==(unsigned)wanted);
        }
    }
    free(d.rows);
}
static int upload(int display, int output, const struct ciuki_rect *r)
{ (void)display; (void)output; (void)r; uploads++; return fail_present ? -1 : 0; }
static int accelerator(int display, const struct ciuki_accel_request *q)
{ (void)display; (void)q; return 0; }
static void presentation(void)
{
    struct desktop d={.width=RW,.height=RH,.pixels=a,.ops={.present=upload}};
    struct desk_box full={0,0,RW,RH};
    for (unsigned caps=0;caps<4;caps++) for (int active=0;active<2;active++) {
        background(731); a[17]^=1;
        d.accel=active ? accelerator : NULL; d.accel_caps=caps; d.scanout_valid=0;
        uploads=fail_present=0; assert(!present_box(&d,full) && uploads==1);
        /* Presentation never writes a second RAM frame. */
        assert(a[17]!=b[17]);
        d.accel_caps=0; a[17]^=1; uploads=0;
        assert(!present_box(&d,full) && uploads==1);
    }
    d.accel=accelerator; d.accel_caps=1; d.scanout_valid=0; fail_present=1;
    background(923); a[17]^=1;
    assert(present_box(&d,full)==-1 && a[17]!=b[17]);
}

static uint64_t clock_ns, present_cost;
static int timing_failure;
static uint64_t clock_read(void) { return clock_ns; }
static int timing_present(int display, int output, const struct ciuki_rect *r)
{
    (void)display; (void)output; (void)r;
    clock_ns+=present_cost;
    return timing_failure ? -1 : 0;
}
static void pacing_latency(void)
{
    struct desk_ops ops={.present=timing_present};
    struct desktop d; desk_init(&d,&ops,64,48);
    d.pixels=calloc(64*48,4); assert(d.pixels); d.time_ns=clock_read;
    clock_ns=1000000000ull; d.now_ns=clock_ns;
    present_cost=5000000; timing_failure=0;
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=1,.monotonic_ns=990000000};
    desk_input(&d,&e); assert(!desk_redraw(&d));
    /* Includes 10 ms input wait and 5 ms presentation. Deadline is anchored
     * to render start, not to the 5 ms later completion. */
    assert(d.frame_stats.frame_ns==15000000 && d.stats.completed==1);
    assert(d.next_frame_ns==1000000000ull+DESK_FRAME_NS);
    assert(d.stats.frame_p95_ns==16000000 && d.stats.frame_max_ns==15000000);
    clock_ns=d.next_frame_ns; d.now_ns=clock_ns; e.monotonic_ns=clock_ns-2000000;
    desk_input(&d,&e); timing_failure=1; present_cost=1000000;
    assert(desk_redraw(&d)==-1 && d.stats.completed==1 && d.frame_input_ns==e.monotonic_ns);
    clock_ns=d.next_frame_ns; d.now_ns=clock_ns; timing_failure=0; present_cost=0;
    assert(!desk_redraw(&d) && d.stats.completed==2);
    assert(d.frame_stats.frame_ns==clock_ns-e.monotonic_ns && !d.frame_input_ns);
    assert(d.stats.frame_max_ns==d.frame_stats.frame_ns);
    desk_stats_sample(&d); assert(d.sample.completed==2 && !d.stats.completed);
    clock_ns=d.next_frame_ns; present_cost=40000000;
    desk_damage_add(&d.damage,(struct desk_box){0,0,64,48},64,48);
    uint64_t begin=clock_ns; assert(!desk_redraw(&d));
    assert(d.frame_stats.overruns==1 && d.frame_stats.dropped==2);
    assert(d.next_frame_ns==begin+3*DESK_FRAME_NS && d.next_frame_ns>clock_ns);
    assert(d.stats.completed==1 && d.stats.frame_p95_ns==41000000);
    /* Long waits retain their exact maximum beyond the histogram range. */
    clock_ns=d.next_frame_ns; d.frame_input_ns=clock_ns-300000000ull; present_cost=0;
    desk_damage_add(&d.damage,(struct desk_box){0,0,1,1},64,48); assert(!desk_redraw(&d));
    assert(d.stats.frame_max_ns==300000000 && d.stats.frame_p95_ns==300000000);
    /* Idle slots are not dropped frames, and rendering does not re-anchor
     * the absolute phase when a late wake still fits the remaining budget. */
    uint64_t phase=d.next_frame_ns;
    clock_ns=phase+1000*DESK_FRAME_NS+1000000ull;present_cost=1000000;
    desk_damage_add(&d.damage,(struct desk_box){0,0,1,1},64,48);assert(!desk_redraw(&d));
    assert(!d.frame_stats.overruns && !d.frame_stats.dropped);
    assert(d.next_frame_ns==phase+1001*DESK_FRAME_NS);
    phase=d.next_frame_ns;clock_ns=phase+1000*DESK_FRAME_NS-1000;present_cost=5000000;
    desk_damage_add(&d.damage,(struct desk_box){0,0,1,1},64,48);assert(!desk_redraw(&d));
    assert(!d.frame_stats.overruns && !d.frame_stats.dropped);
    assert(d.next_frame_ns==phase+1001*DESK_FRAME_NS);
    desk_render_release(&d); free(d.pixels);
}

static void opaque_client_updates(void)
{
    struct desktop *d=calloc(1,sizeof(*d));assert(d);struct desk_ops ops={0};desk_init(d,&ops,RW,RH);
    d->pixels=a;d->hw_cursor=1;d->mouse_x=d->mouse_y=0;
    uint32_t pixels[100*64];for (unsigned i=0;i<100*64;i++) pixels[i]=i*7919&0xffffff;
    struct desk_client *c=&d->clients[0];c->window=1;c->order=1;c->frame=desk_frame(100,64,40,35);
    c->info.width=100;c->info.height=64;c->pixels=pixels;
    struct desk_box all={0,0,RW,RH},content=desk_content(c),dirty={content.x+10,content.y+2,70,40};
    desk_composite(d,all);
    for (unsigned y=2;y<42;y++) for (unsigned x=10;x<80;x++) pixels[y*100+x]^=0xa5abcdefu;
    caption_visits=shadow_visits=0;desk_composite(d,dirty);assert(!caption_visits && !shadow_visits);
    d->pixels=b;desk_composite(d,all);assert(!memcmp(a,b,sizeof(a)));
    /* A higher opaque window, including its shadow, blocks the shortcut. */
    struct desk_client *upper=&d->clients[1];upper->window=1;upper->order=2;upper->frame=desk_frame(20,16,65,50);
    uint32_t overlay[20*16];memset(overlay,0x62,sizeof(overlay));upper->pixels=overlay;upper->info.width=20;upper->info.height=16;
    d->pixels=a;desk_composite(d,all);pixels[5*100+25]^=0xabcdef;
    caption_visits=shadow_visits=0;desk_composite(d,dirty);assert(caption_visits);
    d->pixels=b;desk_composite(d,all);assert(!memcmp(a,b,sizeof(a)));
    desk_render_release(d);free(d);
}
int main(void)
{
    opaque_client_updates();
    frames(); shadows(); text(); visibility(); dirty_rows(); presentation(); pacing_latency();
    struct desktop d={.width=RW,.height=RH};
    background(37);
    struct desk_paint p={a,RW,RH,0,0,RW,RH}, q=p; q.pixels=b;
    struct desk_box f={0,0,150,120};
    struct desk_frame_style style={.title="No memory",.active=1,.buttons=DESK_TB_CLOSE};
    fail_allocation=1;
    render_cache_prepare(&d); assert(!fail_allocation && !d.render_cache && d.render_cache_prepared);
    frame_border(&d,&p,0,f,&style);
    desk_paint_frame_border(&q,f.x,f.y,f.w,f.h,&style);
    assert(!memcmp(a,b,sizeof(a)));
    puts("PASS compositor rows: frame cache invalidation/moves/clips/allocation fallback, shadow oracle, fonts/UTF-8, every icon alpha, opaque visibility/fragmentation fallback");
    return 0;
}
