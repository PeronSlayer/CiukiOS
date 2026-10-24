/* CONFIGURE supersession and allocation lifetime in production client glue.
 * Called from deskui_test; SPDX-License-Identifier: MIT */
#include "resize_transport.h"
#include "../../../apps/libdeskui/deskui.h"
#include <ciuki/surface.h>
#include "../../../apps/libdeskui/clipboard.h"
#include "../../../apps/libdeskui/clipstore.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static struct clip_store closing_clipboard;
static unsigned closing_rendered;
static int closing_transport(void *ctx,struct desk_clip_wire *w,const void *data,struct desk_clip_wire *r,void **out)
{ return clip_request(ctx,55,w,data,r,out); }
static void closing_producer(void *ctx,unsigned event,unsigned format)
{
    (void)format; struct du_window *w=ctx;
    assert(event==DESK_CLIP_RELEASE && w->closing==1 && !w->closed);
    static const unsigned char text[]={'o',0,'k',0,0,0};
    assert(!du_clip_open() && !du_clip_put(13,text,sizeof(text)) && !du_clip_close()); closing_rendered++;
}
static void configured(unsigned w,unsigned h,unsigned serial,int committed)
{
    struct ciuki_message m; desk_message(&m,DESK_CONFIGURE,20);
    m.data[0]=DESK_SHELL_VERSION; m.data[2]=DESK_RESIZE_MINOR;
    desk_put32(m.data+4,w);desk_put32(m.data+8,h);desk_put32(m.data+12,serial);
    desk_put32(m.data+16,committed?DESK_CONFIGURE_COMMITTED:0);rt_push(&m);
}
static void drain(void)
{ struct ciuki_message m; while(rt_pop(&m)) for(unsigned i=0;i<m.fd_count;i++) rt_close(m.fds[i]); }
static void open_window(struct du_window *w,struct du_ui *u)
{
    rt_reset(); assert(!du_window_open_resizable(w,u,3,400,300,"Resize",200,160)); drain();
    du_add(u,DU_EDIT,1,(struct du_rect){8,30,200,24},"Ciuki");du_focus(u,1);
    configured(400,300,0,1); assert(du_window_turn(w,1)==1); drain();
    assert(rt_surfaces()==1 && rt_mappings()==1 && rt_handles()==1);
}
static unsigned power_seen;
static int power_event(struct du_window *w,uint32_t event,uint32_t flags,void *ctx)
{ (void)w;(void)ctx;assert(flags==1 || event!=0);power_seen++;return event!=0; }
static void rt_power_tests(void)
{
    static struct du_window w;static struct du_ui u;open_window(&w,&u);w.power_event=power_event;
    for(unsigned event=0;event<=18;event+=event==0?4:event==4?3:event==7?3:8) {
        struct ciuki_message msg;desk_message(&msg,DESK_POWER_EVENT,16);msg.data[0]=DESK_SHELL_VERSION;msg.data[2]=DESK_RESIZE_MINOR;
        desk_put32(msg.data+4,event+1);desk_put32(msg.data+8,event);desk_put32(msg.data+12,event==0);
        rt_push(&msg);assert(du_window_turn(&w,event+10)==1);
        unsigned replies=0;
        while(rt_pop(&msg)) if(msg.data[1]==DESK_POWER_REPLY) {
            assert(desk_get32(msg.data+4)==event+1);
            assert(desk_get32(msg.data+8)==(replies?event!=0:2));replies++;
        }
        assert(replies==2);
    }
    assert(power_seen==5);du_window_destroy(&w);rt_reset();
}
void rt_client_tests(void)
{
    static struct du_window w; static struct du_ui u;
    rt_power_tests();
    open_window(&w,&u);
    u.mouse_x=15; u.mouse_y=40;
    assert(du_window_turn(&w,2)==1);
    struct ciuki_message cursor_request;
    assert(rt_pop(&cursor_request) && cursor_request.data[1]==DESK_SET_CURSOR && desk_get32(cursor_request.data+4)==DESK_CURSOR_TEXT);
    assert(du_window_turn(&w,3)==1 && !rt_pop(&cursor_request));
    u.mouse_y=10; rt_block=1; assert(du_window_turn(&w,4)==1 && w.count==1);
    rt_block=0; assert(du_window_turn(&w,5)==1 && rt_pop(&cursor_request));
    assert(cursor_request.data[1]==DESK_SET_CURSOR && desk_get32(cursor_request.data+4)==DESK_CURSOR_ARROW);
    uint32_t *old=w.pixels; int oldfd=w.surface;
    rt_block=1; configured(500,350,1,0); assert(du_window_turn(&w,2)==1);
    assert(w.replacement && !w.swap_sent && rt_surfaces()==2 && rt_mappings()==2);
    uint32_t snapshot=old[0];
    configured(600,440,2,0); assert(du_window_turn(&w,3)==1);
    assert(w.configure_serial==2 && w.info.width==600 && w.old_surface==oldfd && rt_surfaces()==2);
    assert(old[0]==snapshot && du_focused(&u)==1 && !strcmp(du_get(&u,1)->text,"Ciuki"));
    rt_block=0; assert(du_window_turn(&w,4)==1 && w.swap_sent);
    struct ciuki_message m; assert(rt_pop(&m) && m.fd_count==1 && desk_get32(m.data+20)==2);
    assert(ciuki_surface_map(m.fds[0],PROT_READ|PROT_WRITE)==MAP_FAILED);
    rt_close(m.fds[0]); assert(!rt_pop(&m));
    /* A newer request waits for the sent swap's confirmation: no third surface. */
    configured(700,460,3,0); assert(du_window_turn(&w,5)==1);
    assert(w.request_serial==3 && rt_surfaces()==2 && w.info.width==600 && old[0]==snapshot);
    configured(600,440,2,1); assert(du_window_turn(&w,6)==1);
    assert(w.info.width==700 && w.swap_sent && rt_surfaces()==2 && w.old_info.width==600);
    assert(rt_pop(&m) && desk_get32(m.data+20)==3); rt_close(m.fds[0]);
    configured(700,460,3,1); assert(du_window_turn(&w,7)==1); drain();
    assert(!w.replacement && rt_surfaces()==1 && rt_mappings()==1 && rt_handles()==1);
    assert(!du_file_dialog(&u,900,0,"Open","/","",NULL,NULL));
    assert(u.dialog.places && u.dialog.entries);
    du_window_destroy(&w); assert(!rt_surfaces() && !u.dialog.kind && !u.dialog.places && !u.dialog.entries);
    /* Cursor requests use the production FIFO and suppress unchanged shapes. */
    open_window(&w,&u); u.mouse_x=300; u.mouse_y=200;
    for (unsigned shape=1;shape<DESK_CURSOR_COUNT;shape++) {
        du_cursor(&u,shape); assert(du_window_turn(&w,shape+20)==1);
        assert(rt_pop(&m) && m.data[1]==DESK_SET_CURSOR && desk_get32(m.data+4)==shape);
        struct desk_packet p; assert(desk_parse(&m,1,NULL,&p) && p.flags==shape && !m.fd_count);
        assert(!rt_pop(&m)); assert(du_window_turn(&w,shape+40)==1 && !rt_pop(&m));
    }
    du_window_destroy(&w); assert(!rt_surfaces());
    /* Same-turn proposals allocate only the newest; disconnect cleans pending state. */
    open_window(&w,&u); rt_block=1;
    configured(500,350,1,0);configured(650,420,2,0);assert(du_window_turn(&w,2)==1);
    assert(w.configure_serial==2 && rt_surfaces()==2);
    rt_eof=1;assert(!du_window_turn(&w,3));du_window_destroy(&w);assert(!rt_surfaces());
    /* Allocation/map failure keeps the previous allocation valid until teardown. */
    for(int map=0;map<2;map++) {
        open_window(&w,&u); rt_fail_create=!map;rt_fail_map=map;
        configured(500,350,1,0);assert(du_window_turn(&w,2)==-1);
        assert(rt_surfaces()==1 && rt_handles()==1 && w.info.width==400);
        du_window_destroy(&w);assert(!rt_surfaces());
    }
    /* A normal close must render promised data before CLOSE drops the
     * transport; destructor-only rendering runs after w->closed is set. */
    open_window(&w,&u); clip_init(&closing_clipboard);
    du_clip_bind(closing_transport,&closing_clipboard); du_clip_producer_bind(closing_producer,&w);
    assert(!du_clip_open() && !du_clip_empty() && !du_clip_delay(13) && !du_clip_close());
    du_window_close(&w); assert(!du_window_turn(&w,100) && w.closed && closing_rendered==1);
    assert(!closing_clipboard.owner && closing_clipboard.items[0].format==13 && closing_clipboard.items[0].bytes==6);
    du_window_destroy(&w); assert(!rt_surfaces()); clip_destroy(&closing_clipboard);
    puts("PASS G5b native window close renders delayed data before closing its channel");
    rt_reset();
    puts("PASS deskui resize transport: supersession, backpressure, in-flight serials, read-only transfers, EOF/failure cleanup, bounded surfaces, cursor FIFO and unchanged-shape suppression");
}
