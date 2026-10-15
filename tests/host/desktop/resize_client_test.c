/* CONFIGURE supersession and allocation lifetime in production client glue.
 * Called from deskui_test; SPDX-License-Identifier: MIT */
#include "resize_transport.h"
#include "../../../apps/libdeskui/deskui.h"
#include <ciuki/surface.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
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
void rt_client_tests(void)
{
    struct du_window w; struct du_ui u;
    open_window(&w,&u);
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
    du_window_destroy(&w); assert(!rt_surfaces());
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
    rt_reset();
    puts("PASS deskui resize transport: supersession, backpressure, in-flight serials, read-only transfers, EOF/failure cleanup, bounded surfaces, cursor FIFO and unchanged-shape suppression");
}
