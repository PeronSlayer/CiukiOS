/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "fake.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#ifdef F125_WRAP_ALLOC
static int fail_malloc;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n)
{
    if (fail_malloc) { fail_malloc=0; errno=ENOMEM; return NULL; }
    return __real_malloc(n);
}
#endif
static struct desktop d;
static void motion(int dx);
#ifdef F125_BASELINE
static uint64_t present_time;
#endif
static uint64_t stamp(void);
static int measured_present(int display, int output, const struct ciuki_rect *rect)
{
#ifdef F125_BASELINE
    uint64_t begin=stamp(); int e=fake_ops.present(display,output,rect);
    present_time+=stamp()-begin; return e;
#else
    return fake_ops.present(display,output,rect);
#endif
}
#ifndef F125_BASELINE
static uint32_t *reference, *lfb;
static int fail_after=-1;
static unsigned checked_presents;
static unsigned pending_inputs;
static int read_inputs(int fd, struct ciuki_input_event *out, uint32_t capacity)
{
    (void)fd;
    if (!pending_inputs) { errno=EAGAIN; return -1; }
    unsigned n=pending_inputs<capacity ? pending_inputs : capacity;
    for (unsigned i=0;i<n;i++) out[i]=(struct ciuki_input_event){.type=CIUKI_INPUT_MOTION,.value=(pending_inputs-i)%2 ? 1 : -1};
    pending_inputs-=n; return (int)n;
}
static int publish(int display, int output, const struct ciuki_rect *r)
{
    /* Every rectangle sees the entire completed output frame. */
    assert(!pending_inputs);
    if (memcmp(d.pixels,reference,(size_t)d.width*d.height*4)) {
        for (size_t k=0;k<(size_t)d.width*d.height;k++) if (d.pixels[k]!=reference[k]) {
            fprintf(stderr,"completed output mismatch at %zu,%zu got=%06x expected=%06x presents=%u scene_damage=%u cursor=%d,%d selected=%d menus=%d realtime=%llu\n",
                k%d.width,k/d.width,d.pixels[k],reference[k],checked_presents,d.damage.count,
                d.mouse_x,d.mouse_y,d.sh.icon_selected,d.sh.levels,(unsigned long long)d.realtime_second); break;
        }
        assert(!"incomplete output frame");
    }
    checked_presents++;
    if (fail_after==0) { errno=EINTR; return -1; }
    if (fail_after>0) fail_after--;
    for (unsigned y=0;y<r->height;y++) memcpy(lfb+(r->dst_y+y)*d.width+r->dst_x,
        d.pixels+(r->src_y+y)*d.width+r->src_x,r->width*4);
    return fake_ops.present(display,output,r);
}
static void expected(void)
{
    uint32_t *out=d.pixels; d.pixels=reference;
    desk_composite(&d,(struct desk_box){0,0,(int)d.width,(int)d.height}); d.pixels=out;
}
static void redraw(void) { fake_presents=0; expected(); assert(!desk_redraw(&d)); }
static int switch_mode(struct desktop *desk, uint16_t id) { desk->display_pending=id; return 0; }
static void shadow_equivalence(void)
{
    uint32_t *a=malloc(96*96*4),*b=malloc(96*96*4); assert(a && b);
    for (int n=0;n<32;n++) {
        int x=n-12,y=n/2-4,w=48,h=40,size=n%10;
        struct desk_paint p={a,96,96,n%7,n%9,70,72};
        for (int i=0;i<96*96;i++) a[i]=b[i]=(uint32_t)(i*10293)&0xffffffu;
        desk_paint_shadow(&p,x,y,w,h,size,0x182736);
        /* Independent pixel reference: evaluate the original falloff over
         * the whole canvas, with no span/bounding-box optimization. */
        for (int yy=0;yy<96;yy++) for (int xx=0;xx<96;xx++) {
            if (!size || xx<p.cx || xx>=p.cx+p.cw || yy<p.cy || yy>=p.cy+p.ch ||
                (xx>=x && xx<x+w && yy>=y && yy<y+h)) continue;
            int ox=x+size/3,oy=y+size/2;
            int dx=xx<ox ? ox-xx : xx>=ox+w ? xx-(ox+w-1) : 0;
            int dy=yy<oy ? oy-yy : yy>=oy+h ? yy-(oy+h-1) : 0;
            if (dx>=size || dy>=size) continue;
            unsigned alpha=72u*(unsigned)(size-dx)*(unsigned)(size-dy)/(unsigned)(size*size);
            alpha=alpha*alpha/72u; b[yy*96+xx]=desk_mix(0x182736,b[yy*96+xx],alpha);
        }
        assert(!memcmp(a,b,96*96*4));
    }
    free(a); free(b);
}
static void assertions(void)
{
    shadow_equivalence();
    reference=malloc((size_t)d.width*d.height*4); lfb=malloc((size_t)d.width*d.height*4);
    assert(reference && lfb); memcpy(lfb,d.pixels,(size_t)d.width*d.height*4);
    d.ops.present=publish;
    d.damage.count=0; d.mouse_x=600; d.mouse_y=500; d.cursor_dirty=1;
    redraw();
    d.now_ns=10000000;
    uint64_t rebuilds=d.sh.background_rebuilds;
    for (int n=0;n<16;n++) motion(4);
    assert(!d.damage.count); redraw();
    assert(d.frame_stats.scene_px==0 && d.frame_stats.damage_px==374 && d.frame_stats.presents==2);
    assert(d.frame_stats.lfb_bytes==1496 && d.sh.background_rebuilds==rebuilds);
    assert(!memcmp(lfb,reference,(size_t)d.width*d.height*4));
    pending_inputs=600; uint64_t inputs=d.inputs;
    assert(desk_drain_input(&d,read_inputs)==600 && d.inputs==inputs+600 && !pending_inputs);
    redraw(); assert(d.frame_stats.scene_px==0);
    motion(-500); motion(-400); expected(); fake_presents=0; fail_after=1;
    assert(desk_redraw(&d)==-1 && d.present_damage.count);
    /* A newer input/frame must retain the earlier unpresented footprint. */
    motion(80); fail_after=-1; redraw();
    assert(!memcmp(lfb,reference,(size_t)d.width*d.height*4));
    struct ciuki_input_event edge={.type=CIUKI_INPUT_MOTION,.value=1023-d.mouse_x,.value2=767-d.mouse_y};
    desk_input(&d,&edge); redraw();
    assert(d.cursor_box.w==1 && d.cursor_box.h==1 && d.frame_stats.damage_px<=374);
    assert(!memcmp(lfb,reference,(size_t)d.width*d.height*4));
    edge.value=600-d.mouse_x; edge.value2=500-d.mouse_y; desk_input(&d,&edge); redraw();
    d.sh.cfg.clock_seconds=0; d.realtime_second=65; d.damage.count=0;
    desk_shell_clock_damage(&d,64); assert(!d.damage.count);
    d.sh.cfg.clock_seconds=1; desk_shell_clock_damage(&d,64); redraw();
    assert(d.frame_stats.damage_px==2150 && fake_presents==1);
    for (int n=0;n<20;n++) desk_damage_add(&d.damage,(struct desk_box){300+n,300,2,2},d.width,d.height);
    redraw(); assert(fake_presents==1 && d.frame_stats.damage_px==42);
    d.sh.icon_selected=0; desk_damage_add(&d.damage,d.sh.icons[0].box,d.width,d.height); redraw();
    desk_menu_open(&d,SH_MENU_START,4,32); redraw(); desk_menu_close(&d); redraw();
    assert(!memcmp(lfb,reference,(size_t)d.width*d.height*4));
    /* Title-button hover adds bounded scene damage, without a full present. */
    struct desk_box close=desk_close_box(&d.clients[0]);
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=close.x-d.mouse_x,.value2=close.y-d.mouse_y};
    desk_input(&d,&e); redraw();
    assert(d.frame_stats.scene_px<4096 && d.frame_stats.damage_px<8192);
    assert(!memcmp(lfb,reference,(size_t)d.width*d.height*4));
    d.next_frame_ns=1000000; d.cursor_dirty=1;
    assert(!desk_frame_due(&d,999999) && desk_frame_due(&d,1000000));
    redraw(); checked_presents=0; fake_presents=0; assert(!desk_redraw(&d) && !checked_presents);
    /* Display success cannot reach the panel before all new frame pixels. */
    struct desk_client *c=&d.clients[0]; c->pid=42; c->program=DESK_APP_CONTROL; c->version=DESK_SHELL_VERSION;
    d.display_set=switch_mode; d.display_mode=280;
    struct ciuki_message m; desk_message(&m,DESK_SETTINGS,24); m.data[0]=DESK_SHELL_VERSION;
    desk_put32(m.data+4,123); desk_put32(m.data+8,DESK_SETTINGS_DISPLAY); desk_put32(m.data+12,283); m.sender_pid=42;
    unsigned sent=fake_sent; desk_receive(&d,0,&m);
    assert(d.display_reply_mode==283 && fake_sent==sent);
    d.display_mode=283; desk_damage_add(&d.damage,(struct desk_box){0,0,(int)d.width,(int)d.height},d.width,d.height);
    fake_fail_present=1; /* publish honours its own failure injection */
    fail_after=0; expected(); assert(desk_redraw(&d)==-1 && !desk_display_frame_commit(&d,-1));
    assert(fake_sent==sent); fail_after=-1; fake_fail_present=0; redraw();
    assert(desk_display_frame_commit(&d,0)); desk_display_reply(&d,0); desk_flush(&d,0);
    assert(!d.display_reply_mode && fake_sent>sent);
    struct desk_packet p; assert(desk_parse(&fake_messages[fake_sent-1],0,NULL,&p) && p.serial==123 && !p.result);
    /* System telemetry is opt-in and cannot be supplied in the client direction. */
    desk_message(&m,DESK_SYSTEM_QUERY,8); m.data[0]=DESK_SHELL_VERSION; m.sender_pid=42;
    desk_receive(&d,0,&m); desk_flush(&d,0);
    assert(desk_parse(&fake_messages[fake_sent-1],0,NULL,&p) && p.result==-EPERM && p.request==DESK_SYSTEM_QUERY);
    c->taskman=1; d.sample.frames=120; d.sample.lfb_bytes=1ull<<40; d.sample_ns=2000000000;
    desk_message(&m,DESK_SYSTEM_QUERY,8); m.data[0]=DESK_SHELL_VERSION; m.sender_pid=42;
    desk_receive(&d,0,&m); desk_flush(&d,0);
    assert(desk_parse(&fake_messages[fake_sent-1],0,NULL,&p) && p.performance[3]==1ull<<40 && p.performance[0]==120);
    assert(!desk_parse(&fake_messages[fake_sent-1],1,NULL,&p));
    d.now_ns=2000000; edge.monotonic_ns=1000000; edge.value=edge.value2=0;
    desk_input(&d,&edge); redraw(); assert(d.frame_stats.input_lag_ns==1000000);
#ifdef F125_WRAP_ALLOC
    desk_render_release(&d); expected();
    fail_malloc=1; fake_presents=0;
    assert(desk_redraw(&d)==-1 && errno==ENOMEM && !fake_presents);
    assert(!memcmp(d.pixels,reference,(size_t)d.width*d.height*4));
    assert(!desk_frame_due(&d,d.next_frame_ns-1));
    redraw(); assert(!memcmp(lfb,reference,(size_t)d.width*d.height*4));
#endif
    free(reference); free(lfb);
    puts("PASS f1-25: cursor overlay, cache, precise clock/hover, coalescing, input drain before render, complete output before present, retry after newer input, frame deadline/allocation failure, display frame reply, telemetry wire");
}
#endif
static uint64_t stamp(void)
{
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000000ull+(uint32_t)t.tv_nsec;
}
static void motion(int dx)
{
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=dx,.monotonic_ns=d.now_ns};
    desk_input(&d,&e);
}
static void run(const char *name, int kind)
{
    uint64_t pixels=0, calls=0, input_time=0, composite_time=0, present_sum=0, begin=stamp();
    for (int n=0;n<100;n++) {
        fake_presents=0;
        uint64_t input_begin=stamp();
        if (kind==0) for (int k=0;k<16;k++) motion(n%2 ? -4 : 4);
        if (kind==1) {
            d.realtime_second++;
#ifdef F125_BASELINE
            desk_damage_add(&d.damage,desk_taskbar(&d),d.width,d.height);
#else
            desk_shell_clock_damage(&d,d.realtime_second-1);
#endif
        }
        if (kind==2) {
            desk_menu_open(&d,SH_MENU_START,4,DESK_BAR);
            desk_menu_close(&d);
        }
        if (kind==3) desk_move(&d,0,100+((n+1)%2)*16,100);
        if (kind==4) desk_move(&d,0,20+((n+1)%2)*16,100);
        input_time+=stamp()-input_begin;
        uint64_t redraw_begin=stamp();
#ifdef F125_BASELINE
        present_time=0;
#endif
        assert(!desk_redraw(&d));
#ifdef F125_BASELINE
        composite_time+=stamp()-redraw_begin-present_time; present_sum+=present_time;
#else
        (void)redraw_begin;
        if (fake_presents) { composite_time+=d.frame_stats.composite_ns; present_sum+=d.frame_stats.present_ns; }
#endif
        for (unsigned k=0;k<fake_presents;k++) pixels+=(uint64_t)fake_rects[k].width*fake_rects[k].height;
        calls+=fake_presents;
    }
    printf("MODEL %s damage_px=%llu presents=%llu.%02llu lfb_bytes=%llu mean_ns=%llu composite_ns=%llu present_ns=%llu input_ns=%llu\n",name,
           (unsigned long long)(pixels/100),(unsigned long long)(calls/100),(unsigned long long)(calls%100),
           (unsigned long long)(pixels*4/100),(unsigned long long)((stamp()-begin)/100),
           (unsigned long long)(composite_time/100),(unsigned long long)(present_sum/100),(unsigned long long)(input_time/100));
}
int main(void)
{
    fake_reset(); desk_init(&d,&fake_ops,1024,768); desk_shell_enable(&d);
    d.ops.present=measured_present;
#ifndef F125_BASELINE
    d.time_ns=stamp;
#endif
    d.pixels=calloc((size_t)d.width*d.height,4); assert(d.pixels);
    strcpy(d.sh.cfg.wallpaper,"none"); d.sh.cfg.clock_seconds=1;
    assert(!desk_shell_wallpaper_update(&d));
    d.mouse_x=400; d.mouse_y=400;
    assert(!desk_redraw(&d));
    run("cursor-batch16",0); run("clock-second",1); run("menu-open-close",2);
    fake_channel(8); fake_surface(9,320,200,0x123456);
    struct desk_client *c=&d.clients[0]; c->channel=8;c->window=c->hello=1;c->order=1;
    c->surface=9; assert(!fake_ops.info(9,&c->info));c->pixels=fake_ops.map(9,1);
    c->frame=desk_frame(320,200,100,100);strcpy(c->title,"Window");
    desk_damage_add(&d.damage,desk_taskbar(&d),d.width,d.height);
    desk_damage_add(&d.damage,desk_outer(c),d.width,d.height);fake_presents=0;assert(!desk_redraw(&d));
    run("window-move16",3);
    desk_move(&d,0,20,100); fake_presents=0; assert(!desk_redraw(&d));
    run("window-over-labels",4);
#ifndef F125_BASELINE
    assertions();
    desk_render_release(&d);
#endif
    desk_drop_client(&d,0);free(d.pixels);free(d.sh.wall);
    assert(!fake_live());return 0;
}
