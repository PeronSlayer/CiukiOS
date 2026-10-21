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
static unsigned live,unmaps,resize_calls,sends;
static int nextfd=4,available;
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
{ assert(fd==3 && flags==DONTWAIT && m->data[0]==3);sends++;return 0; }
int fixture_recv(int fd,struct ciuki_message *m,uint32_t flags)
{ assert(fd==3 && flags==DONTWAIT);if(!available) { errno=EAGAIN;return -1; }*m=incoming;available=0;return 1; }
void ciuki_chrome_init(struct ciuki_browser_surface *s) { assert(s); }
void ciuki_chrome_resize(void) { resize_calls++; }
void ciuki_chrome_render(void) {}
void ciuki_chrome_release(void) {}
bool ciuki_chrome_input(const struct ciuki_input_event *e)
{ return e->type==CIUKI_INPUT_MOTION && e->value2<CIUKI_BROWSER_TOP; }
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
    puts("NetSurf registered Ciuki surface: constructor, viewport, content drag, resize and cleanup passed");fflush(stdout);return 0;
}
