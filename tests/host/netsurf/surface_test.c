/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdk/netsurf/port/surface.h"
#include "sdk/netsurf/port/pointer.h"
static struct { struct ciuki_surface_info info;void *map;int live; } fds[64];
static int nextfd=4, fail_create, fail_send, closed, unmapped;
static unsigned fail_after;
static struct ciuki_message sent[256], incoming;
static unsigned sends;
static unsigned padding;
static int create(uint32_t w,uint32_t h,uint32_t format)
{
    if(fail_create) { errno=ENOMEM;return -1; }
    int fd=nextfd++;assert(fd<64);
    fds[fd].info=(struct ciuki_surface_info){.size=sizeof(struct ciuki_surface_info),.width=w,.height=h,.stride=w*4+padding,
        .format=format,.allocation_bytes=((w*4+padding)*h+4095)&~4095u};
    fds[fd].live=1;fds[fd].map=calloc(1,fds[fd].info.allocation_bytes);assert(fds[fd].map);return fd;
}
static int info(int fd,struct ciuki_surface_info *out) { *out=fds[fd].info;return 0; }
static void *map(int fd,int prot) { (void)prot;return fds[fd].map; }
static int unmap(void *p,size_t n) { assert(n && p);for(int i=4;i<nextfd;i++)if(fds[i].map==p)fds[i].map=NULL;free(p);unmapped++;return 0; }
static int closefd(int fd) { assert(fd>=3 && fd<64);if(fd!=3) { assert(fds[fd].live);free(fds[fd].map);fds[fd].map=NULL;fds[fd].live=0; }closed++;return 0; }
static int sendmsg(int fd,const struct ciuki_message *message,uint32_t flags)
{
    assert(fd==3 && flags==DONTWAIT);
    if(fail_send || (fail_after && sends>=fail_after)) { errno=EAGAIN;return -1; }
    /* fx-nsperf: the scroll fixture counts more messages than it keeps. */
    sent[sends%256]=*message;
    sends++;return 0;
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
    /* f2-99: semantic browser pointers use the existing system cursor API,
     * with no imported bitmap, duplicate sends or lost backpressure retry. */
    const unsigned shapes[]={GUI_POINTER_DEFAULT,GUI_POINTER_POINT,GUI_POINTER_CARET,GUI_POINTER_MENU,
        GUI_POINTER_UP,GUI_POINTER_DOWN,GUI_POINTER_LEFT,GUI_POINTER_RIGHT,GUI_POINTER_RU,GUI_POINTER_LD,
        GUI_POINTER_LU,GUI_POINTER_RD,GUI_POINTER_CROSS,GUI_POINTER_MOVE,GUI_POINTER_WAIT,GUI_POINTER_HELP,
        GUI_POINTER_NO_DROP,GUI_POINTER_NOT_ALLOWED,GUI_POINTER_PROGRESS};
    const unsigned cursors[]={0,10,3,0,6,6,5,5,8,8,7,7,11,9,1,21,4,4,2};
    for(unsigned n=0;n<sizeof(shapes)/sizeof(*shapes);n++)assert(ciuki_pointer_shape(shapes[n])==cursors[n]);
    assert(ciuki_pointer_shape(999)==DESK_CURSOR_ARROW);
    unsigned cursor_sends=sends;
    assert(!ciuki_browser_cursor(s,ciuki_pointer_shape(GUI_POINTER_POINT)) && sends==cursor_sends+1);
    struct desk_packet cp;assert(desk_parse(&sent[sends-1],1,NULL,&cp));
    assert(cp.opcode==DESK_SET_CURSOR && cp.flags==DESK_CURSOR_HAND && cp.index==0 && cp.result==0);
    assert(!ciuki_browser_cursor(s,DESK_CURSOR_HAND) && sends==cursor_sends+1);
    fail_send=1;assert(ciuki_browser_cursor(s,DESK_CURSOR_TEXT)==-1 && s->cursor_shape==DESK_CURSOR_HAND);
    fail_send=0;assert(!ciuki_browser_cursor(s,DESK_CURSOR_TEXT) && sends==cursor_sends+2);
    assert(ciuki_browser_cursor(s,DESK_CURSOR_COUNT)==-1 && errno==EINVAL);
    s->dirty=false;
    for(unsigned i=0;i<640*480;i++)s->current.back[i]=0x123456;
    s->current.back[2*640+1]=0x00ff0000;s->current.back[2*640+2]=0x000000ff;
    ciuki_browser_damage(s,(nsfb_bbox_t){1,2,3,3});assert(!ciuki_browser_present(s));
    assert(s->current.map[2*640+1]==0xff0000 && s->current.map[2*640+2]==0xff);
    assert(s->current.map[2*640]==0 && s->current.map[3*640+1]==0);
    assert(desk_get32(sent[sends-1].data+16)==1 && desk_get32(sent[sends-1].data+24)==2);
    ciuki_browser_damage(s,(nsfb_bbox_t){-3,4,10,6});ciuki_browser_damage(s,(nsfb_bbox_t){8,1,650,8});
    assert(s->damage.x0==0 && s->damage.x1==640 && s->damage.y0==1 && s->damage.y1==8);
    /* Deferred compositor: RESULT can arrive before any pixel consumption.
     * Private rendering remains invisible until the next explicit present. */
    unsigned before=sends;
    uint32_t saved=s->current.map[2*640+1];s->current.back[2*640+1]=0xabcdef;
    assert(s->current.map[2*640+1]==saved);
    p=result(DESK_DAMAGE,desk_get32(sent[sends-1].data+4),0);ciuki_browser_packet(s,&p);
    assert(s->current.map[2*640+1]==saved);
    assert(!ciuki_browser_present(s) && sends>before);
    /* This pixel lies in the gap between the reported rectangles. */
    assert(s->current.map[2*640+1]==saved);
    before=sends;
    s->current.back[2*640+1]=0x654321;
    assert(s->current.map[2*640+1]==saved);
    ciuki_browser_damage(s,(nsfb_bbox_t){1,2,2,3});
    assert(!ciuki_browser_present(s) && sends==before+1); /* no RESULT wait */
    assert(s->current.map[2*640+1]==0x654321);
    ciuki_browser_damage(s,(nsfb_bbox_t){1,2,2,3});
    s->current.back[2*640+1]=0xabcdef;
    fail_send=1;assert(ciuki_browser_present(s)==-1 && s->dirty);fail_send=0;
    assert(!ciuki_browser_present(s));
    assert(s->current.map[2*640+1]==0xabcdef && !s->retry_present);
    before=sends;
    ciuki_browser_damage(s,(nsfb_bbox_t){1,2,2,3});
    assert(!ciuki_browser_present(s) && sends==before); /* identical frame */
    /* Two distant updates preserve their exact regions even when private
     * rendering changed an unreported pixel in the covering box's gap. */
    s->current.back[20*640+10]=0x334455;s->current.back[200*640+600]=0x556677;
    saved=s->current.map[100*640+300];s->current.back[100*640+300]=0xfefefe;
    ciuki_browser_damage(s,(nsfb_bbox_t){10,20,11,21});
    ciuki_browser_damage(s,(nsfb_bbox_t){600,200,601,201});
    assert(!ciuki_browser_present(s) && sends==before+2);
    assert(s->current.map[100*640+300]==saved);
    assert(s->current.map[20*640+10]==0x334455 && s->current.map[200*640+600]==0x556677);
    puts("PASS F2-98b NetSurf: exact disjoint regions, unchanged frame sends nothing, backpressure retains copied-but-unsent damage");
    /* fx-nsperf, T23 trial 2ca3b0b6 ("pages render at 1-2 fps"): a 100-row
     * wheel step on a text page sent one DAMAGE per changed pixel run
     * (7,700-24,000 per frame on the frame bench's article). Text-like rows:
     * glyph-sized dark runs on white, 21-row lines with 5 blank rows. */
    {
        unsigned W=640,H=480,top=36,view=H-36-22;
        uint32_t *page=malloc((size_t)W*2000*4);assert(page);
        /* Start from a presented surface (earlier cases left unreported
         * private pixels on purpose). */
        memcpy(s->current.map,s->current.back,(size_t)W*H*4);
        srand(7);
        for(unsigned y=0;y<2000;y++)for(unsigned x=0;x<W;x++) {
            unsigned line=y%21,col=x%9;
            page[(size_t)y*W+x]=line<16 && x>8 && x<W-24 && col<5 && (rand()%3) ? 0x000000u+(unsigned)(rand()%64) : 0xffffffu;
        }
        for(unsigned y=0;y<view;y++)memcpy(s->current.back+(size_t)(top+y)*W,page+(size_t)y*W,W*4);
        ciuki_browser_damage(s,(nsfb_bbox_t){0,(int)top,(int)W,(int)(top+view)});
        assert(!ciuki_browser_present(s) && !s->dirty);
        assert(!memcmp(s->current.map+(size_t)top*W,s->current.back+(size_t)top*W,(size_t)view*W*4));
        /* The wheel step: fb_pan's copy (update of the moved box), the
         * exposed strip and the scroll bar, presented in one UI turn. */
        uint64_t messages=s->stats.messages;unsigned before_sends=sends;
        memmove(s->current.back+(size_t)top*W,s->current.back+(size_t)(top+100)*W,(size_t)(view-100)*W*4);
        ciuki_browser_update(s,(nsfb_bbox_t){0,(int)top,(int)W-16,(int)(top+view-100)});
        for(unsigned y=view-100;y<view;y++)memcpy(s->current.back+(size_t)(top+y)*W,page+(size_t)(100+y)*W,W*4);
        ciuki_browser_update(s,(nsfb_bbox_t){0,(int)(top+view-100),(int)W-16,(int)(top+view)});
        ciuki_browser_update(s,(nsfb_bbox_t){(int)W-16,(int)top,(int)W,(int)(top+view)});
        unsigned update_sends=sends-before_sends;
        while(s->dirty)assert(!ciuki_browser_present(s));
        printf("fx-nsperf scroll frame: %llu DAMAGE messages, %u sent by updates, %llu compared px, %llu copied px\n",
            (unsigned long long)(s->stats.messages-messages),update_sends,(unsigned long long)s->stats.compared_px,(unsigned long long)s->stats.copied_px);
        assert(s->stats.messages-messages>=1 && s->stats.messages-messages<=8);
        assert(!update_sends); /* updates are collected; the UI turn presents */
        assert(!memcmp(s->current.map+(size_t)top*W,s->current.back+(size_t)top*W,(size_t)view*W*4));
        /* A status-bar text change (22 rows of glyph runs) is one message. */
        messages=s->stats.messages;
        for(unsigned y=H-22+4;y<H-4;y++)for(unsigned x=4;x<300;x++)s->current.back[(size_t)y*W+x]=(x+y)%7<3?0x101010:0xc0c0c0;
        ciuki_browser_damage(s,(nsfb_bbox_t){0,(int)(H-22),(int)W,(int)H});
        assert(!ciuki_browser_present(s) && s->stats.messages-messages==1);
        const uint8_t *wire=sent[(sends-1)%256].data;
        assert(wire[1]==DESK_DAMAGE && desk_get32(wire+16)==4 && desk_get32(wire+20)==H-18 &&
               desk_get32(wire+24)==296 && desk_get32(wire+28)==14);
        /* Distant changes in one rectangle are separate bands; a refused
         * send keeps the copied band and resumes without sending a published
         * band again or comparing it again. */
        messages=s->stats.messages;
        s->current.back[(size_t)(top+10)*W+20]^=0xff;s->current.back[(size_t)(top+300)*W+400]^=0xff;
        ciuki_browser_damage(s,(nsfb_bbox_t){0,(int)top,(int)W,(int)(top+view)});
        fail_after=sends+1;
        assert(ciuki_browser_present(s)==-1 && s->dirty && s->retry_present && s->stats.messages-messages==1);
        fail_after=0;uint64_t compared=s->stats.compared_px;
        assert(!ciuki_browser_present(s) && !s->dirty && s->stats.messages-messages==2);
        assert(s->stats.compared_px-compared<(uint64_t)(view-10)*W);
        wire=sent[(sends-1)%256].data;
        assert(wire[1]==DESK_DAMAGE && desk_get32(wire+16)==400 && desk_get32(wire+20)==top+300 &&
               desk_get32(wire+24)==1 && desk_get32(wire+28)==1);
        assert(!memcmp(s->current.map,s->current.back,(size_t)H*W*4));
        free(page);
        puts("PASS fx-nsperf present: a text scroll frame and a status-bar change are a few DAMAGE messages, collected per UI turn; a refused send resumes");
    }
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
    ciuki_browser_release(s);unsigned releases=0;while(ciuki_browser_event(s,&out)) { assert(out.type==NSFB_EVENT_KEY_UP || (out.type==NSFB_EVENT_CONTROL && out.value.controlcode==NSFB_CONTROL_CANCEL));releases++; }assert(releases==3);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_MOTION,.value=10,.value2=10};ciuki_browser_input(s,&e);assert(!ciuki_browser_event(s,&out));
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_BUTTON,.code=1,.value=1};ciuki_browser_input(s,&e);assert(!ciuki_browser_event(s,&out));
    /* f6-20: a full input queue (UI thread behind a long layout) drops the
     * queued input and resynchronises like a focus loss; it never closes. */
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_MOTION,.value=50,.value2=100};ciuki_browser_input(s,&e);
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_KEY,.code=0x2a,.value=1};ciuki_browser_input(s,&e);
    /* Pointer motion coalesces into one trailing event; text does not. */
    for(unsigned i=0;i<10;i++) { e=(struct ciuki_input_event){.type=CIUKI_INPUT_MOTION,.value=60+(int)i,.value2=110};ciuki_browser_input(s,&e); }
    assert(s->count==3 && s->events[(s->head+2)%CIUKI_BROWSER_EVENTS].value.vector.x==69);
    while(s->count<CIUKI_BROWSER_EVENTS) { e=(struct ciuki_input_event){.type=CIUKI_INPUT_TEXT,.code='x'};ciuki_browser_input(s,&e); }
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_TEXT,.code='y'};ciuki_browser_input(s,&e);
    assert(!s->closed && !s->close_reason && s->overflows==1 && s->count==2 && !s->keys[0x2a] && !s->buttons);
    assert(ciuki_browser_event(s,&out) && out.type==NSFB_EVENT_KEY_UP && out.value.keycode==NSFB_KEY_LSHIFT);
    assert(ciuki_browser_event(s,&out) && out.type==NSFB_EVENT_CONTROL && out.value.controlcode==NSFB_CONTROL_CANCEL);
    assert(!ciuki_browser_event(s,&out));
    e=(struct ciuki_input_event){.type=CIUKI_INPUT_MOTION,.value=50,.value2=100};ciuki_browser_input(s,&e);
    assert(ciuki_browser_event(s,&out) && out.type==NSFB_EVENT_MOVE_ABSOLUTE && !s->closed);
    p=packet(DESK_PING,0,4);p.serial=123;ciuki_browser_packet(s,&p);assert(s->ping==123);
    assert(!du3_ui_progress(&s->connection,1,1,s->ping));assert(sent[sends-1].data[1]==DESK_PONG);
    /* f6-20: the desktop's close request is the recorded reason; a later
     * CLOSED or local close does not replace it. */
    p=packet(DESK_CLOSE_REQUEST,0,0);ciuki_browser_packet(s,&p);
    assert(s->closed && !strcmp(s->close_reason,"desktop-close-request"));
    p=packet(DESK_CLOSED,0,0);ciuki_browser_packet(s,&p);assert(!strcmp(s->close_reason,"desktop-close-request"));
    ciuki_browser_close(s);assert(s->current.fd==-1 && s->pending.fd==-1 && s->connection.channel==-1);
    assert(unmapped==3 && closed==4 && !strcmp(s->close_reason,"desktop-close-request"));
    /* A refused CREATE_WINDOW names the desktop's result. */
    assert(!ciuki_browser_open(s,&ops,3,640,480) && !s->closed && !s->close_reason);
    p=result(DESK_CREATE_WINDOW,s->create_request,-ENOSPC);assert(ciuki_browser_packet(s,&p)==-1);
    assert(s->closed && !strcmp(s->close_reason,"window-refused") && s->close_detail==-ENOSPC);
    ciuki_browser_close(s);
    /* The v3 protocol requires packed rows and rejects padded strides. */
    padding=64;
    assert(ciuki_browser_open(s,&ops,3,640,480)==-1 && s->closed && !strcmp(s->close_reason,"surface-open-failed"));
    free(s);puts("NetSurf v3 surface: damage, colour, ownership, resize, stale/OOM, input and cleanup passed");fflush(stdout);
    puts("PASS f2-99 browser cursor: all upstream semantic shapes, system bitmap index zero, unchanged suppression and backpressure retry");
    fflush(stdout);return 0;
}
