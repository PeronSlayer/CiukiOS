/* SPDX-License-Identifier: MIT
 * Actual registered nsfb surface, with OS boundaries replaced by v3 fixtures. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <libnsfb.h>
#include <libnsfb_plot.h>
#include "sdk/netsurf/port/surface.h"
#include "sdk/netsurf/port/budget.h"
void (*ciuki_ui_pump)(void);   /* budget.c's hook; the surface installs its pump */
static unsigned live,unmaps,resize_calls,sends,pongs;
static struct ciuki_message fifo[128];
static unsigned fifo_head,fifo_count;
static int nextfd=4,available,recv_fault;
static struct ciuki_surface_info info[32];
static void *maps[32];
static struct ciuki_message incoming;
int fixture_create(uint32_t w,uint32_t h,uint32_t format)
{
    int fd=nextfd++;assert(fd<32);live++;
    info[fd]=(struct ciuki_surface_info){.size=sizeof(info[fd]),.width=w,.height=h,.stride=w*4,.format=format,.allocation_bytes=(w*h*4+4095)&~4095u};
    maps[fd]=calloc(1,info[fd].allocation_bytes);assert(maps[fd]);return fd;
}
int fixture_info(int fd,struct ciuki_surface_info *out) { *out=info[fd];return 0; }
void *fixture_map(int fd,int prot) { (void)prot;return maps[fd]; }
int fixture_unmap(void *p,size_t n) { assert(p && n);unmaps++;free(p);return 0; }
int fixture_close(int fd) { if(fd!=3) { assert(live);live--; }return 0; }
int fixture_send(int fd,const struct ciuki_message *m,uint32_t flags)
{ assert(fd==3 && flags==DONTWAIT && m->data[0]==3);sends++;if(m->data[1]==DESK_PONG)pongs++;return 0; }
int fixture_recv(int fd,struct ciuki_message *m,uint32_t flags)
{
    assert(fd==3 && flags==DONTWAIT);
    if(recv_fault==1)return 0;                       /* desktop closed the channel */
    if(recv_fault==2) { errno=EIO;return -1; }
    if(fifo_count) { *m=fifo[fifo_head];fifo_head=(fifo_head+1)%128;fifo_count--;return 1; }
    if(!available) { errno=EAGAIN;return -1; }*m=incoming;available=0;return 1;
}
void ciuki_chrome_init(struct ciuki_browser_surface *s) { assert(s); }
void ciuki_chrome_resize(void) { resize_calls++; }
void ciuki_chrome_render(void) {}
void ciuki_chrome_release(void) {}
bool ciuki_chrome_input(const struct ciuki_input_event *e)
{ return e->type==CIUKI_INPUT_MOTION && e->value2<CIUKI_BROWSER_TOP; }
/* The real dispatcher and deskui are exercised by frontend_test.c. */
void ciuki_chrome_dispatch(struct ciuki_browser_surface *s,const struct ciuki_input_event *e)
{
    if(e->type==CIUKI_INPUT_MOTION) { s->mouse_x=e->value;s->mouse_y=e->value2; }
    bool drag=s->buttons && (e->type==CIUKI_INPUT_MOTION || e->type==CIUKI_INPUT_BUTTON);
    if(drag || !ciuki_chrome_input(e))ciuki_browser_input(s,e);
}
static void result(unsigned op,uint32_t request)
{
    desk_v3_message(&incoming,DESK_V3_RESULT,8,request,1,1);
    desk_put32(incoming.data+16,op);available=1;
}
static void event(unsigned op,unsigned type,int code,int value,int value2)
{
    desk_v3_message(&incoming,op,40,0,1,1);
    desk_put32(incoming.data+28,type);desk_put32(incoming.data+40,code);
    desk_put32(incoming.data+44,value);desk_put32(incoming.data+48,value2);available=1;
}
/* A desktop that sent several packets before the client's next read. */
static void enqueue(unsigned op,unsigned type,int code,int value,int value2)
{
    assert(fifo_count<128);
    event(op,type,code,value,value2);available=0;
    fifo[(fifo_head+fifo_count++)%128]=incoming;
}
extern int (*ciuki_browser_wait)(int,int);
static unsigned waits;
static int wait_input(int channel,int timeout)
{
    assert(channel==3 && timeout==10000);waits++;
    enqueue(DESK_MOTION,CIUKI_INPUT_MOTION,0,70,110);return 1;
}
int main(void)
{
    ciuki_browser_channel=3;
    nsfb_t *fb=nsfb_new(NSFB_SURFACE_CIUKI);assert(fb);
    assert(!nsfb_set_geometry(fb,640,422,NSFB_FMT_XRGB8888));assert(!nsfb_init(fb));
    struct ciuki_browser_surface *s=ciuki_browser_active;assert(s && sends==2 && live==1);
    result(DESK_CREATE_WINDOW,s->create_request);nsfb_event_t e;assert(!nsfb_event(fb,&e,0));assert(s->ready);
    event(DESK_MOTION,CIUKI_INPUT_MOTION,0,50,100);assert(nsfb_event(fb,&e,0) && e.type==NSFB_EVENT_MOVE_ABSOLUTE && e.value.vector.y==64);
    event(DESK_BUTTON,CIUKI_INPUT_BUTTON,1,1,0);assert(nsfb_event(fb,&e,0) && e.type==NSFB_EVENT_KEY_DOWN);
    /* Drag remains content-owned while crossing over the toolbar. */
    event(DESK_MOTION,CIUKI_INPUT_MOTION,0,60,5);assert(nsfb_event(fb,&e,0) && e.type==NSFB_EVENT_MOVE_ABSOLUTE && e.value.vector.y==-31);
    event(DESK_BUTTON,CIUKI_INPUT_BUTTON,1,0,0);assert(nsfb_event(fb,&e,0) && e.type==NSFB_EVENT_KEY_UP && !s->buttons);
    event(DESK_MOTION,CIUKI_INPUT_MOTION,0,60,5);assert(!nsfb_event(fb,&e,0));
    desk_v3_message(&incoming,DESK_CONFIGURE,32,0,1,1);
    desk_put32(incoming.data+16,800);desk_put32(incoming.data+20,600);desk_put32(incoming.data+24,1);desk_put32(incoming.data+28,9);available=1;
    assert(!nsfb_event(fb,&e,0) && s->swap_request && live==2);
    result(DESK_V3_RESIZE,s->swap_request);assert(nsfb_event(fb,&e,0) && e.type==NSFB_EVENT_RESIZE && e.value.resize.w==800 && e.value.resize.h==542);
    assert(live==1 && resize_calls==1);
    uint8_t *pixels;int stride;nsfb_get_buffer(fb,&pixels,&stride);assert(pixels==(uint8_t *)s->current.back+36*stride && stride==3200);
    nsfb_free(fb);assert(!ciuki_browser_active && !live && unmaps==2);
    /* f6-19: a Programs-menu launch has no --channel-fd; the registered surface
     * opens the desktop channel the launcher mapped at fd 3 (the 1.0.0 alpha
     * T23 trial died with "Unable to initialise framebuffer" here). */
    ciuki_browser_channel=-1;sends=0;
    fb=nsfb_new(NSFB_SURFACE_CIUKI);assert(fb);
    assert(!nsfb_set_geometry(fb,640,422,NSFB_FMT_XRGB8888));assert(!nsfb_init(fb));
    s=ciuki_browser_active;assert(s && s->connection.channel==CIUKI_BROWSER_CHANNEL_FD && sends==2 && live==1);
    result(DESK_CREATE_WINDOW,s->create_request);assert(!nsfb_event(fb,&e,0) && s->ready);
    nsfb_free(fb);assert(!ciuki_browser_active && !live && ciuki_browser_channel==-1);
    /* A static browser has one ten-second wait, then dispatches the new input
     * on the following turn. The frontend passes the real scheduler timeout. */
    fb=nsfb_new(NSFB_SURFACE_CIUKI);assert(fb);
    assert(!nsfb_set_geometry(fb,640,422,NSFB_FMT_XRGB8888));assert(!nsfb_init(fb));
    s=ciuki_browser_active;result(DESK_CREATE_WINDOW,s->create_request);assert(!nsfb_event(fb,&e,0));
    ciuki_browser_wait=wait_input;
    assert(!nsfb_event(fb,&e,10000) && waits==1);
    assert(nsfb_event(fb,&e,0) && e.type==NSFB_EVENT_MOVE_ABSOLUTE && waits==1);
    ciuki_browser_wait=NULL;nsfb_free(fb);assert(!ciuki_browser_active && !live);
    puts("PASS F2-98c NetSurf chrome: 10 s idle wait, input dispatched in one turn");
    /* f6-20: one UI turn drains the whole channel (not one input packet per
     * turn) and coalesces consecutive pointer motion around input edges. */
    fb=nsfb_new(NSFB_SURFACE_CIUKI);assert(fb);
    assert(!nsfb_set_geometry(fb,640,422,NSFB_FMT_XRGB8888));assert(!nsfb_init(fb));
    s=ciuki_browser_active;result(DESK_CREATE_WINDOW,s->create_request);assert(!nsfb_event(fb,&e,0) && s->ready);
    assert(ciuki_ui_pump);
    for(int i=0;i<3;i++)enqueue(DESK_MOTION,CIUKI_INPUT_MOTION,0,50+i,100);
    enqueue(DESK_BUTTON,CIUKI_INPUT_BUTTON,1,1,0);
    for(int i=0;i<2;i++)enqueue(DESK_MOTION,CIUKI_INPUT_MOTION,0,70+i,110);
    assert(nsfb_event(fb,&e,0) && e.type==NSFB_EVENT_MOVE_ABSOLUTE && e.value.vector.x==52 && !fifo_count && s->count==2);
    assert(nsfb_event(fb,&e,0) && e.type==NSFB_EVENT_KEY_DOWN && e.value.keycode==NSFB_KEY_MOUSE_1);
    assert(nsfb_event(fb,&e,0) && e.type==NSFB_EVENT_MOVE_ABSOLUTE && e.value.vector.x==71 && e.value.vector.y==74);
    enqueue(DESK_BUTTON,CIUKI_INPUT_BUTTON,1,0,0);
    assert(nsfb_event(fb,&e,0) && e.type==NSFB_EVENT_KEY_UP && !s->buttons);
    /* Keepalive during a long layout/paint: the pump reads everything the
     * desktop queued (100 motions and a PING) without processing it. */
    for(int i=0;i<100;i++)enqueue(DESK_MOTION,CIUKI_INPUT_MOTION,0,100+i,120);
    desk_v3_message(&fifo[(fifo_head+fifo_count++)%128],DESK_PING,4,0,1,1);
    desk_put32(fifo[(fifo_head+fifo_count-1)%128].data+16,77);
    unsigned before_pongs=pongs;
    ciuki_ui_pump();
    assert(!fifo_count && s->held_count==2 && s->pumps==1 && !s->ping && pongs==before_pongs && !s->count);
    assert(nsfb_event(fb,&e,0) && e.type==NSFB_EVENT_MOVE_ABSOLUTE && e.value.vector.x==199);
    assert(!s->held_count && pongs==before_pongs+1 && !s->closed);
    nsfb_free(fb);assert(!ciuki_browser_active && !live && !ciuki_ui_pump);
    puts("PASS f6-20 channel drain: whole channel per UI turn, motion coalesced, keepalive pump holds packets");
    /* f6-20: every QUIT carries the surface's reason; EAGAIN never quits. */
    static const struct { int fault; unsigned op; const char *reason; int detail; } quits[]={
        {1,0,"channel-eof",0},{2,0,"channel-error",EIO},
        {0,DESK_CLOSE_REQUEST,"desktop-close-request",0},{0,DESK_CLOSED,"desktop-closed",0}};
    for(unsigned i=0;i<sizeof(quits)/sizeof(*quits);i++) {
        fb=nsfb_new(NSFB_SURFACE_CIUKI);assert(fb);
        assert(!nsfb_set_geometry(fb,640,422,NSFB_FMT_XRGB8888));assert(!nsfb_init(fb));
        s=ciuki_browser_active;result(DESK_CREATE_WINDOW,s->create_request);
        assert(!nsfb_event(fb,&e,0) && s->ready && !s->closed && !s->close_reason);
        assert(!nsfb_event(fb,&e,0) && !s->closed);
        if(quits[i].op) { desk_v3_message(&incoming,quits[i].op,0,0,1,1);available=1; }
        recv_fault=quits[i].fault;
        assert(nsfb_event(fb,&e,0) && e.type==NSFB_EVENT_CONTROL && e.value.controlcode==NSFB_CONTROL_QUIT);
        assert(s->closed && !strcmp(s->close_reason,quits[i].reason) && s->close_detail==quits[i].detail);
        recv_fault=0;nsfb_free(fb);assert(!ciuki_browser_active && !live);
    }
    puts("NetSurf registered Ciuki surface: constructor, viewport, content drag, resize and cleanup passed");
    puts("PASS f6-20 surface QUIT reasons: channel EOF/error, desktop close request and CLOSED");
    puts("PASS f6-19 launch without --channel-fd: the surface opens the inherited desktop channel at fd 3");fflush(stdout);return 0;
}
