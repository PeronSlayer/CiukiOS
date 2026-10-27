/* SPDX-License-Identifier: MIT
 * Actual demo runtime + production toolkit, two-sided surface/channel replay. */
#include "replay_base.h"
#include "resize_transport.h"
#include "../../../apps/desktop/rows.h"
#include <assert.h>
#include <ciuki/present_abi.h>
static uint64_t test_now=1000000000ull;
static uint64_t frame_now(void) { return test_now; }
#define du_now frame_now
#define main gfxdemo_main
#include "../../../apps/gfxdemo/main.c"
#undef main
#undef du_now
uint32_t ciuki_syscall(uint32_t call,uint32_t b,uint32_t c,uint32_t e,uint32_t s,uint32_t i,uint32_t p)
{ (void)b;(void)c;(void)e;(void)s;(void)i;(void)p;assert(call==CIUKI_SYS_YIELD);return 0; }
static struct desktop *d;
static uint32_t *scanout;
static int fail_upload;
static unsigned upload_count;
static uint64_t upload_cost;
static uint64_t server_now(void) { return test_now; }
static int output(int display,int surface,const struct ciuki_rect *r)
{
    (void)display;(void)surface;upload_count++;
    test_now+=upload_cost;
    if (fail_upload) return -1;
    for (unsigned y=0;y<r->height;y++) memcpy(scanout+(r->dst_y+y)*1024+r->dst_x,
        d->pixels+(r->src_y+y)*1024+r->src_x,r->width*4);
    return 0;
}
static void drain(int client)
{
    d->now_ns=test_now;
    struct ciuki_message m;
    while (rt_pop(&m)) desk_receive(d,client,&m);
    desk_flush(d,client);
}
static void demo_idle(struct runtime *r)
{
    idle(r);assert(du_window_turn(&r->window,test_now)>0); /* flush the application's queued frame */
}
static void verify_scene(struct runtime *r,int client)
{
    struct desk_client *c=&d->clients[client];struct desk_box content=desk_content(c),box=c->gfx_box;
    for (int y=0;y<box.h;y++) for (int x=0;x<box.w;x++) {
        int sx=x*(int)c->gfx_width/box.w,sy=y*(int)c->gfx_height/box.h;
        uint32_t expected=c->pixels[(size_t)(box.y+sy)*c->info.width+box.x+sx]&0xffffffu;
        assert(d->pixels[(size_t)(content.y+box.y+y)*1024+content.x+box.x+x]==expected);
    }
    assert(!memcmp(scanout,d->pixels,1024*768*4));
    assert(r->app.scene.width<=640 && r->app.scene.height<=480);
}
static void scaling(void)
{
    uint32_t source[17*13],destination[93*70];
    for (unsigned i=0;i<17*13;i++) source[i]=0xab000000u+i*7919;
    for (unsigned ratio=1;ratio<=3;ratio++) for (unsigned x=0;x<5;x++) {
        unsigned dw=ratio*17,dh=ratio*13,w=dw-x-1,h=dh-2;
        for (unsigned i=0;i<93*70;i++) destination[i]=0xfeedface;
        desk_scale_rows(destination+1,93,source,17,17,13,dw,dh,x,1,w,h);
        for (unsigned y=0;y<h;y++) for (unsigned k=0;k<w;k++)
            assert(destination[1+y*93+k]==(source[((y+1)*13/dh)*17+(x+k)*17/dw]&0xffffffu));
        for (unsigned y=0;y<h;y++) assert(destination[y*93]==0xfeedface && destination[1+y*93+w]==0xfeedface);
    }
    desk_scale_rows(destination,93,source,17,17,13,43,29,3,2,39,26);
    for (unsigned y=0;y<26;y++) for (unsigned x=0;x<39;x++)
        assert(destination[y*93+x]==(source[((y+2)*13/29)*17+(x+3)*17/43]&0xffffffu));
    assert(!gfx_wait_ns(1000000000,1000500000));assert(gfx_wait_ns(1000000000,1001000000)==1000000);
    puts("PASS clipped 1x/2x/3x/arbitrary rows, XRGB padding and wait rounding");
}
static void packets(void)
{
    struct ciuki_surface_info info={24,760,660,760*4,1,(760*660*4+4095)&~4095u};
    struct ciuki_message m;struct desk_packet p;
    desk_message(&m,DESK_GFX_FRAME,64);m.data[0]=2;m.data[2]=1;
    desk_put32(m.data+4,9);desk_put32(m.data+8,60);desk_put32(m.data+12,66);
    desk_put32(m.data+16,640);desk_put32(m.data+20,480);desk_put32(m.data+24,320);desk_put32(m.data+28,240);
    desk_put32(m.data+40,320);desk_put32(m.data+44,240);
    assert(desk_parse(&m,1,&info,&p));assert(!desk_parse(&m,0,&info,&p));
    desk_put32(m.data+32,1);assert(!desk_parse(&m,1,&info,&p));desk_put32(m.data+32,0);
    desk_put32(m.data+8,UINT32_MAX);assert(!desk_parse(&m,1,&info,&p));desk_put32(m.data+8,60);
    m.fd_count=1;m.fds[0]=10;assert(!desk_parse(&m,1,&info,&p));m.fd_count=0;m.fds[0]=0;
    /* fx-doswin3: a v2.0 client may send the 64-byte form. */
    m.data[2]=0;assert(desk_parse(&m,1,&info,&p));m.data[2]=1;
    desk_put32(m.data+24,641);assert(!desk_parse(&m,1,&info,&p));
    desk_message(&m,DESK_GFX_FRAME,64);m.data[0]=2;m.data[2]=1;desk_put32(m.data+4,10);
    assert(desk_parse(&m,1,&info,&p));desk_put32(m.data+44,1);assert(!desk_parse(&m,1,&info,&p));
    desk_message(&m,DESK_GFX_FRAME,68);m.data[0]=2;m.data[2]=1;desk_put32(m.data+4,11);
    desk_put32(m.data+64,1);m.fd_count=1;m.fds[0]=10;
    assert(desk_parse(&m,1,&info,&p) && p.resize_serial==1);
    m.fd_count=0;m.fds[0]=0;assert(!desk_parse(&m,1,&info,&p));
    desk_message(&m,DESK_GFX_FRAME,84);m.data[0]=2;m.data[2]=1;desk_put32(m.data+4,12);
    assert(desk_parse(&m,1,&info,&p) && !p.resize_serial);
    desk_put32(m.data+72,99);assert(!desk_parse(&m,1,&info,&p));desk_put32(m.data+68,11);
    desk_put32(m.data+76,77);desk_put32(m.data+80,555);
    assert(desk_parse(&m,1,&info,&p) && p.timing_serial==11 && p.render_us==99 && p.submit_us==77 && p.wait_us==555);
    desk_put32(m.data+64,1);assert(!desk_parse(&m,1,&info,&p));m.fd_count=1;m.fds[0]=10;
    assert(desk_parse(&m,1,&info,&p) && p.resize_serial==1);
    desk_message(&m,DESK_GFX_PRESENTED,64);m.data[0]=2;m.data[2]=1;desk_put32(m.data+4,12);
    desk_put32(m.data+32,234);desk_put32(m.data+36,1);desk_put32(m.data+40,345);
    desk_put32(m.data+48,456);desk_put32(m.data+56,(uint32_t)DESK_FRAME_NS);
    assert(desk_parse(&m,0,NULL,&p) && p.convert_ns==(1ull<<32)+234 && p.copy_ns==345 && p.slot_ns==456 && p.refresh_ns==DESK_FRAME_NS);
    assert(!desk_parse(&m,1,&info,&p));desk_put32(m.data+56,0);assert(!desk_parse(&m,0,NULL,&p));
    puts("PASS frame protocol bounds, directions, replacement attachments and disable");
}
static void telemetry(void)
{
    int channel,test;unsigned seconds,page;
    char *argv[]={"gfxdemo","--channel-fd=7","--benchmark","10","--scene","tiles","--test=gfxdemo"};
    assert(!arguments(7,argv,&channel,&test,&seconds,&page));
    assert(channel==7 && test && seconds==10 && page==2);
    char *bad[]={"gfxdemo","--benchmark","0"};assert(arguments(3,bad,&channel,&test,&seconds,&page)==2);
    bad[2]="61";assert(arguments(3,bad,&channel,&test,&seconds,&page)==2);
    bad[2]="10x";assert(arguments(3,bad,&channel,&test,&seconds,&page)==2);
    assert(arguments(2,bad,&channel,&test,&seconds,&page)==2);
    struct runtime *r=calloc(1,sizeof(*r));assert(r);
    r->benchmark_seconds=10;r->app.page=1;r->app.scene.width=640;r->app.scene.height=480;
    benchmark_tick(r,1000000000ull);
    for (unsigned i=1;i<=500;i++) gfx_fps_frame(&r->fps,1000000000ull+i*20000000ull);
    benchmark_tick(r,10999999999ull);assert(!r->benchmark_done);
    benchmark_tick(r,11000000000ull);assert(r->benchmark_done && !r->benchmark_error);
    assert(r->fps.frames==500 && gfx_fps_p95(&r->fps)==20000000ull);
    gfx_fps_start(&r->fps,1000000000ull,10);r->benchmark_done=0;
    benchmark_tick(r,11000000000ull);assert(r->benchmark_done && r->benchmark_error);
    r->app.ui=&r->ui;gfx_advance(&r->app,1000000000ull);
    for (unsigned i=1;i<=60;i++) gfx_presented(&r->app,1000000000ull+i*GFX_FRAME_NS);
    gfx_advance(&r->app,2000000020ull);assert(r->app.fps10==600 && r->app.total_frames==60);
    gfx_advance(&r->app,3000000020ull);assert(!r->app.fps10 && !r->app.ms1000);
    free(r);
    puts("PASS timed benchmark arguments, exact deadline, completed-frame average/p95 and no-frame failure");
}
/* f7-14: full screen through the production desktop lease path and the
 * native GL present. The host library has no render3d transport, so frames
 * take the CPU W32_DISPLAY_FRAME path checked here like the kernel does. */
static unsigned host_frames,host_token=500,host_generation=9,lease_grants,lease_revokes;
static uint32_t host_pixel;
int32_t ciuki_gl_host_display_frame(struct ciuki_w32_display_args *a,const void *payload)
{
    assert(a->header.operation==W32_DISPLAY && a->header.size==sizeof(*a) && a->action==W32_DISPLAY_FRAME);
    if (a->token!=host_token || a->generation!=host_generation) { a->error=EACCES; return 0; }
    memcpy(&host_pixel,(const uint8_t *)payload+(384*1024+512)*4,4);
    host_frames++; a->result=1; return 0;
}
static int fake_lease(struct desktop *p,uint32_t pid,int acquire,uint32_t w,uint32_t h,uint32_t bpp,uint32_t gen)
{
    (void)pid;(void)gen; assert(p==d && bpp==32);
    if (!acquire) { lease_revokes++; return 0; }
    assert(w==1024 && h==768); lease_grants++;
    p->v3_lease_tuple=(struct desk_lease_tuple){.token=host_token,.generation=host_generation,.mode=0x118,.scale=1,
        .surface={w,h,w*4,w*h*4,CIUKI_PRESENT_XRGB8888},.native_width=1024,.native_height=768,.native_bpp=32};
    return 0;
}
static void full_screen(struct runtime *r,int client)
{
    d->v3_lease=fake_lease;d->focus=client;
    r->app.request=request;r->app.request_ctx=r;
    r->app.have_info=1;r->app.display_w=1024;r->app.display_h=768;
    unsigned sent=r->serial;
    du_post(&r->ui,DU_EV_ACTIVATE,GFX_FULLSCREEN,0);
    assert(r->app.full_state==GFX_FULL_REQUESTED);
    assert(du_window_turn(&r->window,test_now)>0);drain(client);
    assert(d->v3_lease_state==1 && lease_grants==1);
    assert(du_window_turn(&r->window,test_now)>0 && r->app.full_state==GFX_FULL_ACTIVE);
    assert(r->lease.token==host_token && r->lease.width==1024 && r->lease.height==768 && r->full_pixels);
    uint64_t frames=r->app.total_frames;
    for (unsigned k=0;k<3;k++) {
        test_now+=GFX_FRAME_NS;demo_idle(r);drain(client);
        assert(host_frames==k+1 && r->app.total_frames==frames+k+1 && host_pixel);
    }
    assert(r->serial==sent); /* no windowed frame under the lease */
    /* Focus loss (Alt+Tab) revokes; refocus reactivates and the demo
     * takes the new lease again (DirectDraw exclusive-mode behaviour). */
    desk_focus(d,-1);assert(!d->v3_lease_state && lease_revokes==1);drain(client);
    assert(du_window_turn(&r->window,test_now)>0 && r->app.full_state==GFX_WINDOWED && r->full_wanted);
    host_token=501;desk_focus(d,client);assert(d->v3_lease_state==1 && lease_grants==2);drain(client);
    assert(du_window_turn(&r->window,test_now)>0 && r->app.full_state==GFX_FULL_ACTIVE && r->lease.token==501);
    test_now+=GFX_FRAME_NS;demo_idle(r);drain(client);assert(host_frames==4);
    assert(r->app.gl.frames>=4 && r->app.gl.presents==4 && !r->app.gl.hardware_presents);
    assert(!strcmp(gfx_present_name(&r->app),"cpu-copy"));
    /* The window keeps its own surface: a status repaint renders no scene. */
    uint64_t rendered=r->app.gl.frames;r->ui.dirty=1;assert(du_window_turn(&r->window,test_now)>0);
    gfx_gl_status(&r->app);assert(r->app.gl.frames==rendered);
    /* Esc releases; the desktop revokes and the demo returns to its window. */
    for (int v=1;v>=0;v--) du_input(&r->ui,&(struct ciuki_input_event){.type=CIUKI_INPUT_KEY,.code=DU_KEY_ESC,.value=v});
    assert(du_window_turn(&r->window,test_now)>0);drain(client);
    assert(!d->v3_lease_state && lease_revokes==2);
    assert(du_window_turn(&r->window,test_now)>0 && r->app.full_state==GFX_WINDOWED && !r->full_pixels && !r->app.full_error);
    assert(!r->full_wanted);desk_focus(d,-1);desk_focus(d,client);assert(!d->v3_lease_state && lease_grants==2);drain(client);
    test_now=d->next_frame_ns;assert(!desk_redraw(d));drain(client);
    for (unsigned k=0;k<4 && r->serial==sent;k++) {
        assert(du_window_turn(&r->window,test_now)>0);demo_idle(r);drain(client);
        test_now=d->next_frame_ns;assert(!desk_redraw(d));drain(client);
    }
    assert(r->serial!=sent && host_frames==4);
    /* A refused request leaves the demo windowed with the error shown. */
    d->focus=-1;du_post(&r->ui,DU_EV_ACTIVATE,GFX_FULLSCREEN,0);
    assert(du_window_turn(&r->window,test_now)>0);drain(client);
    assert(du_window_turn(&r->window,test_now)>0 && r->app.full_state==GFX_WINDOWED && r->app.full_error==-EBUSY);
    d->focus=client;d->v3_lease=NULL;r->app.full_error=0;
    puts("PASS full screen: production lease request/grant, leased CPU frames counted as completions, no windowed frames, Alt+Tab revoke and refocus, Esc release and refusal");
}
int main(void)
{
    scaling();packets();telemetry();rt_reset();
    d=calloc(1,sizeof(*d));struct runtime *r=calloc(1,sizeof(*r));scanout=calloc(1024*768,4);assert(d&&r&&scanout);
    desk_init(d,&rt_ops,1024,768);d->pixels=calloc(1024*768,4);assert(d->pixels);
    d->ops.present=output;d->time_ns=server_now;d->hw_cursor=1;d->now_ns=test_now;d->mouse_x=d->mouse_y=0;
    int client=desk_add_client(d,3);assert(client>=0);
    assert(!du_window_open_resizable(&r->window,&r->ui,3,760,660,"Graphics Demo",560,360));
    drain(client);drain(client);gfx_attach(&r->app,&r->ui,NULL);r->app.time_ns=frame_now;r->app.deferred=1;r->app.page=1;r->window.manual_damage=1;
    r->window.message=message;r->window.user=r;r->next_info=UINT64_MAX;d->clients[client].gfxdemo=1;d->clients[client].awaiting_frame=1;
    assert(du_window_turn(&r->window,test_now)>0);demo_idle(r);assert(r->window.present_pending);drain(client);
    assert(d->clients[client].gfx_pending && !r->app.frames && !d->clients[client].awaiting_frame && d->clients[client].launch_frame_pending);
    uint32_t *frozen=malloc(r->window.info.width*r->window.info.height*4);assert(frozen);
    memcpy(frozen,r->window.pixels,r->window.info.width*r->window.info.height*4);
    /* The next frame is ready while the submitted surface remains frozen,
     * including when the clock has not ticked since submission. */
    demo_idle(r);assert(r->window.present_pending && r->app.scene_ready && !r->app.frames);
    assert(!memcmp(frozen,r->window.pixels,r->window.info.width*r->window.info.height*4));
    uint32_t *prepared=malloc(640*480*4);assert(prepared);memcpy(prepared,r->app.scene.pixels,640*480*4);
    int different=0;
    for (unsigned y=0;y<480;y++) if (memcmp(prepared+y*640,
        frozen+(size_t)(r->app.viewport.y+(int)y)*760+r->app.viewport.x,640*4)) different=1;
    assert(different);
    test_now+=1000000;demo_idle(r);assert(r->app.scene_ready);
    assert(!memcmp(prepared,r->app.scene.pixels,640*480*4));free(prepared);
    r->ui.dirty=1;fail_upload=1;assert(desk_redraw(d)<0);drain(client);
    test_now+=20000000;assert(du_window_turn(&r->window,test_now)>0);demo_idle(r);
    assert(!memcmp(frozen,r->window.pixels,r->window.info.width*r->window.info.height*4));assert(!r->app.frames);
    fail_upload=0;assert(!desk_redraw(d));drain(client);assert(du_window_turn(&r->window,test_now)>0);
    assert(!r->window.present_pending && r->app.frames==1);demo_idle(r);drain(client);
    assert(r->window.present_pending);
    /* Completion survives a saturated server reply queue with its original
     * timestamp; the producer stays frozen until that reply is delivered. */
    struct ciuki_message queued;desk_message(&queued,DESK_PONG,8);
    for (unsigned k=0;k<DESK_TX_MAX;k++) desk_queue(d,client,&queued);
    test_now=d->next_frame_ns;assert(!desk_redraw(d));verify_scene(r,client);
    assert(d->clients[client].gfx_reply_pending && d->clients[client].gfx_pending);
    uint64_t completed=d->clients[client].gfx_completed_ns;drain(client);drain(client);
    assert(!d->clients[client].gfx_reply_pending && completed==test_now);
    for (unsigned k=0;k<DESK_TX_MAX/32+1;k++) assert(du_window_turn(&r->window,test_now)>0);
    assert(!r->window.present_pending);
    assert(r->app.have_timing && r->app.wait_ns==test_now-r->submitted_ns && r->app.wait_ns>1000000 && r->app.refresh_ns==DESK_FRAME_NS);
    assert(d->gfx_sample.serial==r->frame_serial && d->gfx_sample.app_serial==r->timing_serial-1);
    assert(du_window_turn(&r->window,test_now)>0);demo_idle(r);drain(client);
    /* A CONFIGURE received while a frame is outstanding cannot replace the
     * mapped source until its completion. */
    desk_resize(d,client,desk_frame(560,360,20,40));drain(client);drain(client);
    assert(du_window_turn(&r->window,test_now)>0 && r->window.info.width==760);
    test_now=d->next_frame_ns;assert(!desk_redraw(d));drain(client);
    assert(du_window_turn(&r->window,test_now)>0 && r->window.info.width==560);demo_idle(r);drain(client);drain(client);
    assert(d->clients[client].info.width==560 && d->clients[client].gfx_width==226); /* f7-14: two control rows */
    assert(du_window_turn(&r->window,test_now)>0);demo_idle(r);drain(client);test_now=d->next_frame_ns;
    assert(!desk_redraw(d));verify_scene(r,client);drain(client);assert(du_window_turn(&r->window,test_now)>0);
    assert(r->app.scene.width==226 && r->app.scene.height==170);
    /* Pause suppresses animation; hidden completion never changes FPS. */
    uint64_t frames=r->app.total_frames;r->app.paused=1;r->app.animation_due=0;r->app.scene_ready=0;
    test_now+=GFX_FRAME_NS;demo_idle(r);assert(!r->window.present_pending && r->app.total_frames==frames);
    r->app.paused=0;test_now+=GFX_FRAME_NS;demo_idle(r);assert(r->window.present_pending);
    d->clients[client].minimized=1;drain(client);assert(du_window_turn(&r->window,test_now)>0 && !r->window.present_pending);
    assert(r->app.total_frames==frames);d->clients[client].minimized=0;
    /* Switching back restores the full-resolution ordinary client surface. */
    r->app.page=0;r->ui.dirty=1;assert(du_window_turn(&r->window,test_now)>0);demo_idle(r);drain(client);
    test_now=d->next_frame_ns;assert(!desk_redraw(d));drain(client);assert(du_window_turn(&r->window,test_now)>0);
    assert(!d->clients[client].gfx_width && !r->scene_active);
    /* Fast producer: prepared successors occupy consecutive 60 Hz slots.
     * Present time is part of wait, and is returned separately without an
     * additional frame copy. This is a mock clock, not hardware evidence. */
    if (test_now<r->app.last_ns) test_now=r->app.last_ns;
    d->next_frame_ns=test_now+DESK_FRAME_NS;
    r->app.page=1;r->ui.dirty=1;assert(du_window_turn(&r->window,test_now)>0);demo_idle(r);drain(client);
    upload_cost=200000;uint64_t previous=0;
    for (unsigned k=0;k<12;k++) {
        assert(r->window.present_pending);demo_idle(r);
        assert(r->app.scene_ready);
        test_now=d->next_frame_ns;uint64_t slot=test_now;
        assert(desk_frame_due(d,test_now) && !desk_redraw(d));drain(client);
        assert(du_window_turn(&r->window,test_now)>0 && !r->window.present_pending);
        assert(r->app.copy_ns && r->app.copy_ns<=2000000 && r->app.refresh_ns==DESK_FRAME_NS);
        if (previous) assert(slot-previous==DESK_FRAME_NS);
        previous=slot;demo_idle(r);drain(client);
    }
    upload_cost=0;
    full_screen(r,client);
    assert(upload_count && !d->stats.overruns);free(frozen);
    desk_drop_client(d,client);du_window_destroy(&r->window);desk_render_release(d);rt_close(3);
    assert(!rt_handles() && !rt_mappings() && !rt_surfaces());rt_reset();
    ciuki_gl_close();free(d->pixels);free(d);free(r);free(scanout);
    puts("PASS actual runtime completion FPS, frozen source, upload retry, resize commit, native scene, pause, hidden frames, backpressure, disable, no leaks/overruns");
    return 0;
}
