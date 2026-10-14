/* SPDX-License-Identifier: MIT
 * Production rasterizer, damage parser, compositor and client diff. Host RAM
 * upload is separate from the T23 134 MB/s directional bandwidth model. */
#define main compositor_main
#include "compositor_benchmark.c"
#undef main
#include "../../../apps/gfxdemo/gfxdemo.h"
#ifndef GFX_REFERENCE
#include "../../../apps/desktop/rows.h"
#endif
void gfx_host_lfb_row(uint32_t *,const uint32_t *,unsigned);
static struct desktop *demo_desktop;
static uint32_t *demo_scanout;
static uint64_t bytes_uploaded,upload_ticks;
static int demo_upload(int display,int output,const struct ciuki_rect *r)
{
    (void)display;(void)output;
    uint64_t begin=ticks();
    for (unsigned y=0;y<r->height;y++) {
        uint32_t *dst=demo_scanout+(size_t)(r->dst_y+(int)y)*W+r->dst_x;
        const uint32_t *src=demo_desktop->pixels+(size_t)(r->src_y+(int)y)*W+r->src_x;
        gfx_host_lfb_row(dst,src,r->width);
    }
    upload_ticks+=ticks()-begin; bytes_uploaded+=(uint64_t)r->width*r->height*4;
    return 0;
}
static void legacy_scale(uint32_t *dst,unsigned stride,const uint32_t *src,unsigned w,unsigned h)
{
    for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) dst[(size_t)y*stride+x]=src[(y*GFX_H/h)*GFX_W+x*GFX_W/w];
}
static void run_demo(const char *portrait,const char *store,const char *theme,unsigned ratio,unsigned page)
{
    /* 3x source/destination spans are measured on a larger host output. The
     * 1024x768 replay clips scanout exactly like a larger/off-screen window. */
    struct desktop *d=calloc(1,sizeof(*d));assert(d);setup(d,portrait,store,theme);desk_menu_close(d);
    for (int i=0;i<4;i++) { free((void *)d->clients[i].pixels);d->clients[i].pixels=NULL;d->clients[i].window=0; }
    unsigned cw=ratio==2 ? 760 : ratio*320+40,ch=ratio*240+180;
    fake_channel(90); fake_surface(91,cw,ch,0);
    int client=desk_add_client(d,90);assert(client>=0);
    struct ciuki_message m;desk_message(&m,DESK_HELLO,4);m.data[0]=2;m.data[2]=1;m.sender_pid=42;desk_receive(d,client,&m);
    desk_message(&m,DESK_CREATE_WINDOW,47);m.data[0]=2;m.data[2]=1;m.sender_pid=42;m.fd_count=1;m.fds[0]=91;
    desk_put32(m.data+4,cw);desk_put32(m.data+8,ch);desk_put32(m.data+12,3);memcpy(m.data+16,"gfx",3);
    desk_receive(d,client,&m);assert(d->clients[client].window);
    struct desk_client *c=&d->clients[client];c->gfxdemo=1;c->frame=desk_frame(cw,ch,8,40);
    struct gfx_app *app=calloc(1,sizeof(*app));struct du_ui *ui=calloc(1,sizeof(*ui));
    uint32_t *back=calloc(cw*ch,4),*scaled=calloc(320*ratio*240*ratio,4);assert(app&&ui&&back&&scaled);
    du_init(ui,back,(int)cw,(int)ch);gfx_attach(app,ui,d->portrait);app->page=page==3 ? 1 : page;
    struct du_window window={.ui=ui,.back=back,.pixels=(uint32_t *)c->pixels,.info=c->info};
    unsigned dw=320*ratio,dh=240*ratio;int x0=(int)(cw-dw)/2,y0=66;
#ifndef GFX_REFERENCE
    app->deferred=1;app->viewport=(struct du_rect){x0,y0,(int)dw,(int)dh};app->scene.width=320;app->scene.height=240;
    c->gfx_box=(struct desk_box){x0,y0,(int)dw,(int)dh};c->gfx_width=320;c->gfx_height=240;
#endif
    demo_desktop=d;demo_scanout=calloc(W*H,4);assert(demo_scanout);d->ops.present=demo_upload;
    d->time_ns=ns;d->now_ns=ns();assert(!desk_redraw(d));
    uint64_t measurements[6][SAMPLES];
    uint64_t uploaded[SAMPLES];
    for (unsigned sample=0;sample<SAMPLES;sample++) {
        uint64_t sum[6]={0};bytes_uploaded=0;
        for (unsigned frame=0;frame<24;frame++) {
            fake_sent=0;
            uint64_t phase=(sample*24ull+frame)*100000000; /* identical changing poses across revisions */
            uint64_t begin=ticks();
            if (page!=2) gfx_cube(&app->scene,phase,d->portrait,page==3);else gfx_sprites(&app->scene,phase,d->portrait);
            sum[0]+=ticks()-begin;
            /* Isolated scale microbenchmark; post-change this work belongs to
             * composition, so it must NOT be added again to pipeline totals. */
            begin=ticks();
#ifdef GFX_REFERENCE
            legacy_scale(scaled,dw,app->scene.pixels,dw,dh);
#else
            desk_scale_rows(scaled,dw,app->scene.pixels,320,320,240,dw,dh,0,0,dw,dh);
#endif
            sum[1]+=ticks()-begin;
            begin=ticks();
#ifdef GFX_REFERENCE
            for (unsigned y=0;y<dh;y++) memcpy(back+(size_t)(y0+(int)y)*cw+x0,scaled+(size_t)y*dw,dw*4);
            du_window_present(&window);
            struct du_rect dirty=window.damaged ? window.damage : (struct du_rect){0};window.damaged=0;
            desk_message(&m,DESK_DAMAGE,24);desk_put32(m.data+4,(uint32_t)dirty.x);desk_put32(m.data+8,(uint32_t)dirty.y);
            desk_put32(m.data+12,(uint32_t)dirty.w);desk_put32(m.data+16,(uint32_t)dirty.h);
#else
#ifdef GFX_PIPELINE_REFERENCE
            gfx_store_scene(app);struct du_rect dirty=app->scene_damage;
            for (int y=0;y<dirty.h;y++) {
                size_t at=(size_t)(y0+dirty.y+y)*cw+x0+dirty.x;
                desk_row_copy(window.pixels+at,back+at,(size_t)dirty.w);
            }
#else
            gfx_diff_scene(app,window.pixels,cw);struct du_rect dirty=app->scene_damage;
            gfx_submit_scene(app,window.pixels,cw);
#endif
            desk_message(&m,DESK_GFX_FRAME,64);desk_put32(m.data+4,frame+1);
#ifndef GFX_PIPELINE_REFERENCE
            m.length=84; /* include the production frame instrumentation */
#endif
            desk_put32(m.data+8,(uint32_t)x0);desk_put32(m.data+12,(uint32_t)y0);
            desk_put32(m.data+16,dw);desk_put32(m.data+20,dh);desk_put32(m.data+24,320);desk_put32(m.data+28,240);
            desk_put32(m.data+32,(uint32_t)dirty.x);desk_put32(m.data+36,(uint32_t)dirty.y);
            desk_put32(m.data+40,(uint32_t)dirty.w);desk_put32(m.data+44,(uint32_t)dirty.h);
#endif
            sum[2]+=ticks()-begin;
            m.data[0]=2;m.data[2]=1;m.sender_pid=42;
            begin=ticks();desk_receive(d,client,&m);sum[3]+=ticks()-begin;
            d->now_ns=ns();d->next_frame_ns=d->now_ns;upload_ticks=0;
            begin=ticks();assert(!desk_redraw(d));uint64_t render=ticks()-begin;
            sum[4]+=render-upload_ticks;sum[5]+=upload_ticks;
#ifndef GFX_REFERENCE
            assert(!c->gfx_pending);
            begin=ticks();desk_flush(d,client);
            for (unsigned i=0;i<fake_sent;i++) { struct desk_packet packet;assert(desk_parse(&fake_messages[i],0,NULL,&packet)); }
            sum[3]+=ticks()-begin;
#endif
            /* The completed display must match a clean full reconstruction. */
            if (!frame && !sample) {
                uint32_t *output=d->pixels,*expected=calloc(W*H,4);assert(expected);
                d->pixels=expected;desk_composite(d,screen);d->pixels=output;
                assert(!memcmp(output,expected,W*H*4));free(expected);
            }
        }
        for (unsigned stage=0;stage<6;stage++) measurements[stage][sample]=sum[stage]/24;
        uploaded[sample]=bytes_uploaded/24;
    }
    const char *names[]={"raster","scale_isolated","client_damage_copy","protocol","composite","upload_ram"};
    uint64_t total=0;
    for (unsigned stage=0;stage<6;stage++) {
        qsort(measurements[stage],SAMPLES,8,compare);
        uint64_t cycles=measurements[stage][SAMPLES/2];
        if (stage!=1 && stage!=5) total+=cycles;
#ifdef GFX_REFERENCE
        if (stage==1) total+=cycles;
#endif
        printf("GFX_STAGE revision=%s page=%s ratio=%u name=%s cycles=%llu nominal_1ghz_us=%llu\n",
#if defined(GFX_REFERENCE) || defined(GFX_PIPELINE_REFERENCE)
            "before",
#else
            "after",
#endif
            page==1 ? "cube" : page==3 ? "cube-gouraud" : "sprites",ratio,names[stage],(unsigned long long)cycles,(unsigned long long)(cycles/1000));
    }
    qsort(uploaded,SAMPLES,8,compare);
    uint64_t bytes=uploaded[SAMPLES/2],nominal=total/1000+(bytes+133)/134;
    printf("GFX_BUDGET page=%s ratio=%u window=%ux%u source=320x240 viewport=%ux%u upload_bytes=%llu nominal_upload_us=%llu nominal_frame_us=%llu p3_1130mhz_work_us=%llu budget_us=16666\n",
        page==1 ? "cube" : page==3 ? "cube-gouraud" : "sprites",ratio,cw,ch,dw,dh,
        (unsigned long long)bytes,(unsigned long long)((bytes+133)/134),(unsigned long long)nominal,
        (unsigned long long)(total/1130+(bytes+133)/134));
    desk_drop_client(d,client);assert(!fake_live());desk_render_release(d);du_release(ui);
    free(app);free(ui);free(back);free(scaled);free(demo_scanout);free((void *)d->portrait);free(d->pixels-1);
    free(d->sh.wall);free(d->sh.background);free(d->sh.icons);free(d);
}
int main(int argc,char **argv)
{
    assert(argc==4);setvbuf(stdout,NULL,_IOLBF,0);
    cpu_set_t allowed,one;assert(!sched_getaffinity(0,sizeof(allowed),&allowed));int cpu=0;
    while (cpu<CPU_SETSIZE && !CPU_ISSET(cpu,&allowed)) cpu++;
    assert(cpu<CPU_SETSIZE);CPU_ZERO(&one);CPU_SET(cpu,&one);assert(!sched_setaffinity(0,sizeof(one),&one));
    printf("GFX_CLOCK cpu=%d cycles=serialized_rdtsc samples=9 frames=24 upload_model=134_MB_s\n",cpu);
    for (unsigned page=1;page<=3;page++) for (unsigned ratio=2;ratio<=3;ratio++) run_demo(argv[1],argv[2],argv[3],ratio,page);
    puts("PASS Graphics Demo frame budget: production raster/damage/composition, exact reconstruction");
    return 0;
}
