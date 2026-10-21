/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdk/netsurf/port/surface.h"
static struct { struct ciuki_surface_info info;void *map;int live; } fds[64];
static int nextfd=4, fail_create, fail_send, closed, unmapped;
static struct ciuki_message sent[256], incoming;
static unsigned sends;
static int create(uint32_t w,uint32_t h,uint32_t format)
{
    if(fail_create) { errno=ENOMEM;return -1; }
    int fd=nextfd++;assert(fd<64);
    fds[fd].info=(struct ciuki_surface_info){.size=sizeof(struct ciuki_surface_info),.width=w,.height=h,.stride=w*4,
        .format=format,.allocation_bytes=(w*h*4+4095)&~4095u};
    fds[fd].live=1;fds[fd].map=calloc(1,fds[fd].info.allocation_bytes);assert(fds[fd].map);return fd;
}
static int info(int fd,struct ciuki_surface_info *out) { *out=fds[fd].info;return 0; }
static void *map(int fd,int prot) { (void)prot;return fds[fd].map; }
static int unmap(void *p,size_t n) { assert(n && p);free(p);unmapped++;return 0; }
static int closefd(int fd) { assert(fd>=3 && fd<64);if(fd!=3) { assert(fds[fd].live);fds[fd].live=0; }closed++;return 0; }
static int sendmsg(int fd,const struct ciuki_message *message,uint32_t flags)
{
    assert(fd==3 && flags==DONTWAIT);
    if(fail_send) { errno=EAGAIN;return -1; }
    assert(sends<256);sent[sends++]=*message;return 0;
}
static int recvmsg(int fd,struct ciuki_message *message,uint32_t flags)
{ assert(fd==3 && flags==DONTWAIT);*message=incoming;return 1; }
static struct ciuki_browser_ops ops={.transport={sendmsg,recvmsg,info,closefd},.create=create,.map=map,.unmap=unmap};
static struct desk_packet packet(unsigned op,uint32_t request,unsigned bytes)
{
    struct desk_packet result;
    desk_v3_message(&incoming,op,bytes,request,1,1);
    assert(desk_parse(&incoming,0,NULL,&result));return result;
}
static struct desk_packet result(unsigned op,unsigned request,int error)
{
    struct desk_packet out;
    desk_v3_message(&incoming,DESK_V3_RESULT,8,request,1,1);
    desk_put32(incoming.data+16,op);desk_put32(incoming.data+20,error);
    assert(du3_next(&(struct du3_connection){.channel=3,.transport={sendmsg,recvmsg,info,closefd}},&out)==1);
    return out;
}
static struct desk_packet configure(unsigned w,unsigned h,unsigned serial)
{
    struct desk_packet out;
    desk_v3_message(&incoming,DESK_CONFIGURE,32,0,1,1);
    desk_put32(incoming.data+16,w);desk_put32(incoming.data+20,h);desk_put32(incoming.data+24,1);desk_put32(incoming.data+28,serial);
    assert(desk_parse(&incoming,0,NULL,&out));return out;
}
int main(void)
{
    struct ciuki_browser_surface *s=calloc(1,sizeof(*s));assert(s);
    assert(!ciuki_browser_open(s,&ops,3,640,480));assert(sends==2 && sent[0].data[0]==3 && sent[1].fd_count==1);
    struct desk_packet p=result(DESK_CREATE_WINDOW,s->create_request,0);assert(!ciuki_browser_packet(s,&p) && s->ready);
    s->dirty=false;
    for(unsigned i=0;i<640*480;i++)s->current.back[i]=0x123456;
    s->current.back[2*640+1]=0x00ff0000;s->current.back[2*640+2]=0x000000ff;
    ciuki_browser_damage(s,(nsfb_bbox_t){1,2,3,3});assert(!ciuki_browser_present(s));
    assert(s->current.map[2*640+1]==0xff0000 && s->current.map[2*640+2]==0xff);
    assert(s->current.map[2*640]==0 && s->current.map[3*640+1]==0);
    assert(desk_get32(sent[sends-1].data+16)==1 && desk_get32(sent[sends-1].data+24)==2);
    ciuki_browser_damage(s,(nsfb_bbox_t){-3,4,10,6});ciuki_browser_damage(s,(nsfb_bbox_t){8,1,650,8});
    assert(s->damage.x0==0 && s->damage.x1==640 && s->damage.y0==1 && s->damage.y1==8);
    unsigned before=sends;assert(!ciuki_browser_present(s) && sends==before); /* mapped region still owned by server */
    p=result(DESK_DAMAGE,s->damage_request,0);ciuki_browser_packet(s,&p);
    fail_send=1;assert(ciuki_browser_present(s)==-1 && s->dirty);fail_send=0;
    assert(!ciuki_browser_present(s));
    int old=s->current.fd;
    p=configure(800,600,10);ciuki_browser_packet(s,&p);assert(s->pending.fd>=0 && s->current.fd==old && fds[old].live);
    unsigned swap=s->swap_request;
    p=configure(900,650,11);ciuki_browser_packet(s,&p);assert(s->swap_request==swap);
    p=result(DESK_V3_RESIZE,swap,0);ciuki_browser_packet(s,&p);
    assert(!fds[old].live && s->current.info.width==800 && s->swap_request);
    old=s->current.fd;swap=s->swap_request;
    p=result(DESK_V3_RESIZE,swap,-ENOMEM);ciuki_browser_packet(s,&p);assert(s->current.fd==old && s->pending.fd==-1);
    p=configure(400,300,9);ciuki_browser_packet(s,&p);assert(!s->swap_request && s->last_configure==11);
    fail_create=1;p=configure(1024,768,12);ciuki_browser_packet(s,&p);assert(s->current.fd==old && !s->swap_request);fail_create=0;
    p=configure(640,480,13);p.generation=2;ciuki_browser_packet(s,&p);assert(s->last_configure==12);
    s->configure=0;
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=50,.value2=100};ciuki_browser_input(s,&e);
    nsfb_event_t out;assert(ciuki_browser_event(s,&out) && out.value.vector.x==50 && out.value.vector.y==64);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_KEY,.code=0x1d,.value=1};ciuki_browser_input(s,&e);
    assert(ciuki_browser_event(s,&out) && out.value.keycode==NSFB_KEY_LCTRL);
    e.code=0x2e;e.value=2;ciuki_browser_input(s,&e);assert(ciuki_browser_event(s,&out) && out.value.keycode==NSFB_KEY_c);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_TEXT,.code=0x20ac};ciuki_browser_input(s,&e);
    assert(ciuki_browser_event(s,&out) && out.type==NSFB_EVENT_TEXT && (unsigned)out.value.keycode==0x20ac);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=2,.value=1};ciuki_browser_input(s,&e);
    assert(ciuki_browser_event(s,&out) && out.value.keycode==NSFB_KEY_MOUSE_3);
    ciuki_browser_release(s);unsigned releases=0;while(ciuki_browser_event(s,&out)) { assert(out.type==NSFB_EVENT_KEY_UP);releases++; }assert(releases==3);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_MOTION,.value=10,.value2=10};ciuki_browser_input(s,&e);assert(!ciuki_browser_event(s,&out));
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=1,.value=1};ciuki_browser_input(s,&e);assert(!ciuki_browser_event(s,&out));
    p=packet(DESK_PING,0,4);p.serial=123;ciuki_browser_packet(s,&p);assert(s->ping==123);
    assert(!du3_ui_progress(&s->connection,1,1,s->ping));assert(sent[sends-1].data[1]==DESK_PONG);
    ciuki_browser_close(s);assert(s->current.fd==-1 && s->pending.fd==-1 && s->connection.channel==-1);
    assert(unmapped==3 && closed==4);
    free(s);puts("NetSurf v3 surface: damage, colour, ownership, resize, stale/OOM, input and cleanup passed");fflush(stdout);
    return 0;
}
