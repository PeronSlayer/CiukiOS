/* Deterministic production rendering and benchmark error semantics.
 * SPDX-License-Identifier: MIT */
#include "acc_test.h"
#include "../../../apps/gfxdemo/gfxdemo.h"
#include "../../../apps/gfxdemo/benchmark.h"
#include <ciuki/gl.h>
static struct gfx_app app;
static struct du_ui ui;
static uint32_t portrait[256*256],pixels[760*640],scratch[640*480],screen[640*480];
static struct ciuki_accel_info status={.driver=CIUKI_ACCEL_DRIVER_LEGACY_NATIVE,.capabilities=1,.generation=9};
static uint64_t clock_ns;
static int failure,upload_failure,stopped,coarse;
static unsigned hardware,uploads;
static uint64_t now(void *ctx)
{ (void)ctx;if(stopped)return 100;clock_ns+=coarse ? 250000 : 1000000;return coarse ? clock_ns/1000000*1000000 : clock_ns; }
static int info(void *ctx,struct ciuki_accel_info *out) { (void)ctx;*out=status;return 0; }
static int upload(void *ctx,const struct ciuki_rect *r)
{
    (void)ctx;uploads++;
    if (upload_failure) return -EIO;
    for (unsigned y=0;y<r->height;y++) memcpy(screen+((unsigned)r->dst_y+y)*640+(unsigned)r->dst_x,
        scratch+((unsigned)r->src_y+y)*640+(unsigned)r->src_x,r->width*4);
    return 0;
}
static int accel(void *ctx,const struct ciuki_accel_request *q)
{
    (void)ctx; hardware++;
    if (failure==1) { status.quarantined=1;status.capabilities=0;return -EIO; }
    if (failure!=2) status.operations++;
    else status.fallbacks++;
    if (failure==3) status.generation++;
    if (q->operation==CIUKI_ACCEL_COPY) {
        for (unsigned y=0;y<256;y++) memmove(screen+y*640+256,screen+y*640,256*4);
    } else for (unsigned y=0;y<q->rect.height;y++) for (unsigned x=0;x<q->rect.width;x++)
        screen[y*640+x]=q->operation==CIUKI_ACCEL_FILL || (q->pattern[y&7]&(0x80u>>(x&7))) ? q->colour : q->background;
    return 0;
}
static void benchmarks(void)
{
    struct gfx_bench_target t={.pixels=scratch,.width=640,.height=480,.bpp=32,.generation=9,
        .now=now,.info=info,.accel=accel,.upload=upload};
    struct gfx_bench_result r;
    for (unsigned op=0;op<3;op++) {
        for (unsigned p=0;p<640*480;p++) scratch[p]=screen[p]=p*7919u;
        assert(!gfx_bench_sample(&t,op,&r) && r.operations==8 && r.ns==8000000);
        uint32_t expected[640*480];memcpy(expected,screen,sizeof(expected));
        assert(!gfx_bench_sample(&t,op+3,&r) && !r.operations && r.ns==8000000);
        assert(!memcmp(expected,screen,sizeof(expected)));
        assert(r.bytes==(op==1 ? 256ull*256 : 640ull*480)*4*8);
    }
    t.bpp=16;assert(!gfx_bench_sample(&t,3,&r) && r.bytes==640ull*480*2*8);
    t.bpp=24;assert(!gfx_bench_sample(&t,3,&r) && r.bytes==640ull*480*3*8);t.bpp=32;
    assert(gfx_bench_sample(&t,6,&r)==-EINVAL);
    unsigned before=hardware; status.capabilities=0;
    assert(gfx_bench_sample(&t,0,&r)==-ENODEV && hardware==before); status.capabilities=1;
    status.quarantined=1;assert(gfx_bench_sample(&t,0,&r)==-ENODEV);status.quarantined=0;
    failure=1;assert(gfx_bench_sample(&t,0,&r)==-EIO && !r.bytes);status.quarantined=0;status.capabilities=1;
    failure=2;assert(gfx_bench_sample(&t,0,&r)==-EIO && !r.ns);
    failure=3;assert(gfx_bench_sample(&t,0,&r)==-EIO);failure=0;status.generation=9;
    upload_failure=1;assert(gfx_bench_sample(&t,3,&r)==-EIO);upload_failure=0;
    coarse=1;assert(!gfx_bench_sample(&t,3,&r) && r.ns>=8000000 && r.bytes>640ull*480*4*8);coarse=0;
    before=uploads;stopped=1;assert(gfx_bench_sample(&t,3,&r)==-EAGAIN && uploads-before==64);stopped=0;
    t.width=320;assert(gfx_bench_sample(&t,1,&r)==-EOPNOTSUPP);
    assert(hardware>24 && uploads>40);
    puts("PASS fake surfaces: fill/copy/pattern, 16/24/32-bpp accounting, fallback, quarantine, mode change, upload error, stopped clock");
}
static void select_page(unsigned page)
{
    struct du_widget *w=du_get(&ui,GFX_TABS);
    for (int x=w->box.x;x<w->box.x+w->box.w;x++)
        if (du_tabs_hit(w,x,w->box.y+4)==(int)page) { at_click(&ui,x,w->box.y+4);return; }
    assert(0);
}
static unsigned requests,last_index;
static int request(void *ctx,unsigned op,unsigned index) { (void)ctx;assert(op==DESK_GFX_BENCH);requests++;last_index=index;return 0; }
int main(int argc,char **argv)
{
    assert(argc==3);FILE *f=fopen(argv[1],"rb");assert(f && fread(portrait,4,256*256,f)==256*256 && !fclose(f));
    benchmarks();du_init(&ui,pixels,760,640);gfx_attach(&app,&ui,portrait);
    gfx_advance(&app,1000000000ull);
    assert(!app.frames); /* timer ticks cannot claim presented frames */
    for (unsigned i=1;i<=75;i++) { gfx_advance(&app,1000000000ull+i*GFX_FRAME_NS);gfx_presented(&app,1000000000ull+i*GFX_FRAME_NS); }
    assert(!app.fps10); /* benchmark page does not publish scene FPS */
    struct desk_packet p={.opcode=DESK_GFX_INFO_ENGINE,.width=1024,.height=768,.bpp=32,
        .accelerator={.driver=CIUKI_ACCEL_DRIVER_LEGACY_NATIVE,.capabilities=3,.operations=123,.generation=9}};
    strcpy(p.engine.driver_id,"fake");
    gfx_receive(&app,&p);p.opcode=DESK_SYSTEM_INFO;p.performance[0]=120;p.performance[4]=60000000;p.performance[5]=120000000;gfx_receive(&app,&p);
    for(unsigned i=0;i<6;i++) app.results[i]=(struct gfx_result){.bytes=640ull*480*4*4,.ns=8000000+i*1000000,.samples=4,.operations=i<3 ? 4 : 0};
    du_render(&ui);assert(!app.scene.error);at_save(argv[2],"benchmark",&ui);
    select_page(1);assert(app.page==1);du_render(&ui);assert(!app.scene.error);at_save(argv[2],"cube-flat",&ui);
    at_click_widget(&ui,GFX_SHADE);assert(app.gouraud);du_render(&ui);assert(!app.scene.error);at_save(argv[2],"cube-gouraud",&ui);
    select_page(2);assert(app.page==2);du_render(&ui);assert(!app.scene.error);at_save(argv[2],"sprites",&ui);
    uint64_t phase=app.elapsed_ns;at_click_widget(&ui,GFX_PAUSE);gfx_advance(&app,4000000000ull);assert(app.elapsed_ns==phase);
    at_click_widget(&ui,GFX_PAUSE);gfx_advance(&app,4100000000ull);assert(app.elapsed_ns==phase+100000000);
    /* A long stall yields one frame, with no catch-up animation burst. */
    uint64_t frames=app.frames;gfx_advance(&app,4110000000ull);assert(app.frames<=frames+1);
    select_page(0);assert(!app.page);app.request=request;at_click_widget(&ui,GFX_RUN);assert(app.bench_running && requests==1 && !last_index);
    p=(struct desk_packet){.opcode=DESK_GFX_RESULT,.index=0,.benchmark={1000000,2000000,1}};
    for(unsigned i=0;i<4;i++) gfx_receive(&app,&p);assert(app.bench_index==1 && app.results[0].samples==4 && requests==1);
    p.index=1;p.result=-ENODEV;gfx_receive(&app,&p);assert(app.bench_index==2 && app.results[1].error==-ENODEV);
    app.bench_running=0;app.accelerator=(struct ciuki_accel_info){0};strcpy(app.engine.driver_id,"swdib");app.results[0].error=-ENODEV;
    du_render(&ui);assert(!app.scene.error);at_save(argv[2],"software-only",&ui);
    du_resize(&ui,pixels,560,360);du_render(&ui);select_page(1);du_render(&ui);
    assert(app.scene.width==261 && app.scene.height==196);
    du_resize(&ui,pixels,760,360);du_render(&ui);
    assert(app.scene.width==320 && app.scene.height==240 && app.viewport.h>=240);
    p=(struct desk_packet){.opcode=DESK_GFX_INFO,.width=800,.height=600,.bpp=16,.accelerator={.generation=10}};
    app.bench_running=1;gfx_receive(&app,&p);assert(!app.bench_running && !app.results[0].samples && !app.results[0].error);
    ciuki_gl_close();du_release(&ui);puts("PASS deterministic pages, flat/Gouraud, exact Ciuki texture, pause/resume, fixed-step pacing, asynchronous benchmark progression, minimum size");return 0;
}
