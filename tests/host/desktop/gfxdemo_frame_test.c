/* SPDX-License-Identifier: MIT
 * Actual demo runtime + production toolkit, two-sided surface/channel replay. */
#include "replay_base.h"
#include "resize_transport.h"
#include "../../../apps/desktop/rows.h"
#include <assert.h>
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
    m.data[2]=0;assert(!desk_parse(&m,1,&info,&p));m.data[2]=1;
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
int main(void)
{
    scaling();packets();rt_reset();
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
    uint32_t *prepared=malloc(320*240*4);assert(prepared);memcpy(prepared,r->app.scene.pixels,320*240*4);
    int different=0;
    for (unsigned y=0;y<240;y++) if (memcmp(prepared+y*320,
        frozen+(size_t)(r->app.viewport.y+(int)y)*760+r->app.viewport.x,320*4)) different=1;
    assert(different);
    test_now+=1000000;demo_idle(r);assert(r->app.scene_ready);
    assert(!memcmp(prepared,r->app.scene.pixels,320*240*4));free(prepared);
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
    assert(d->clients[client].info.width==560 && d->clients[client].gfx_width==261);
    assert(du_window_turn(&r->window,test_now)>0);demo_idle(r);drain(client);test_now=d->next_frame_ns;
    assert(!desk_redraw(d));verify_scene(r,client);drain(client);assert(du_window_turn(&r->window,test_now)>0);
    assert(r->app.scene.width==261 && r->app.scene.height==196);
    /* Pause suppresses animation; hidden completion never changes FPS. */
    uint64_t frames=r->app.frames;r->app.paused=1;r->app.animation_due=0;r->app.scene_ready=0;
    test_now+=GFX_FRAME_NS;demo_idle(r);assert(!r->window.present_pending && r->app.frames==frames);
    r->app.paused=0;test_now+=GFX_FRAME_NS;demo_idle(r);assert(r->window.present_pending);
    d->clients[client].minimized=1;drain(client);assert(du_window_turn(&r->window,test_now)>0 && !r->window.present_pending);
    assert(r->app.frames==frames);d->clients[client].minimized=0;
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
    assert(upload_count && !d->stats.overruns);free(frozen);
    desk_drop_client(d,client);du_window_destroy(&r->window);desk_render_release(d);rt_close(3);
    assert(!rt_handles() && !rt_mappings() && !rt_surfaces());rt_reset();
    free(d->pixels);free(d);free(r);free(scanout);
    puts("PASS actual runtime completion FPS, frozen source, upload retry, resize commit, native scene, pause, hidden frames, backpressure, disable, no leaks/overruns");
    return 0;
}
