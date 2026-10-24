/* SPDX-License-Identifier: MIT. Real desktop store/glue, fake kernel I/O. */
#include "../../../apps/desktop/clipboard.h"
#include "../../../apps/libdeskui/clipstore.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static struct { void *data; struct ciuki_surface_info info; } surfaces[32];
static struct ciuki_message received[128]; static void *payloads[128];
static unsigned count,live; static int blocked;
int ciuki_surface_create(uint32_t w,uint32_t h,uint32_t format)
{
    for (unsigned i=1;i<32;i++) if (!surfaces[i].data) {
        unsigned bytes=(w*h*4+4095)&~4095u; surfaces[i].data=calloc(1,bytes); assert(surfaces[i].data);
        surfaces[i].info=(struct ciuki_surface_info){sizeof(struct ciuki_surface_info),w,h,w*4,format,bytes}; live++; return i;
    }
    errno=ENOMEM; return -1;
}
static int info(int fd,struct ciuki_surface_info *out) { assert(fd>0 && fd<32 && surfaces[fd].data); *out=surfaces[fd].info; return 0; }
static void *map(int fd,int prot) { (void)prot; assert(surfaces[fd].data); return surfaces[fd].data; }
static int unmap(void *p,size_t n) { (void)p;(void)n; return 0; }
static int close_fd(int fd) { assert(surfaces[fd].data); free(surfaces[fd].data); surfaces[fd].data=NULL; live--; return 0; }
static int send(int fd,const struct ciuki_message *m,uint32_t flags)
{
    (void)fd;(void)flags;
    if (blocked) { errno=EAGAIN; return -1; }
    assert(count<128); received[count]=*m; struct desk_packet p; assert(desk_parse(m,0,NULL,&p));
    if (m->fd_count) { payloads[count]=malloc(p.clip.bytes); assert(payloads[count]); memcpy(payloads[count],surfaces[m->fds[0]].data,p.clip.bytes); }
    count++; return 0;
}
void desk_queue(struct desktop *d,int i,const struct ciuki_message *m) { assert(!send(d->clients[i].channel,m,0)); }
struct desk_box desk_outer(const struct desk_client *c) { return c->frame; }
void desk_drop_client(struct desktop *d,int i) { desk_clip_drop(d,i); d->clients[i].channel=-1; }
static struct desk_clip_wire submit(struct desktop *d,int client,unsigned op,unsigned f,unsigned flags,const void *data,unsigned bytes)
{
    static unsigned serial; struct desk_clip_wire w={.serial=++serial,.op=op,.format=f,.flags=flags,.bytes=bytes};
    struct ciuki_message m; struct desk_packet p; int fd=-1;
    if (bytes) { uint32_t width,height; desk_clip_dims(bytes,&width,&height); fd=ciuki_surface_create(width,height,1); memcpy(surfaces[fd].data,data,bytes); }
    assert(!desk_clip_message(&m,d->clients[client].version,1,&w,fd)); assert(desk_parse(&m,1,NULL,&p));
    desk_clip_receive(d,client,&m,&p); desk_clip_tick(d);
    struct desk_clip_wire r={0};
    for (unsigned i=0;i<count;i++) { assert(desk_parse(&received[i],0,NULL,&p)); if (p.clip.serial==serial) r=p.clip; }
    return r;
}
static void wire_submit(struct desktop *d,int client,struct desk_clip_wire *w,const void *data)
{
    int fd=-1; if (w->bytes) { uint32_t width,height; desk_clip_dims(w->bytes,&width,&height);
        fd=ciuki_surface_create(width,height,1); memcpy(surfaces[fd].data,data,w->bytes); }
    struct ciuki_message m; struct desk_packet p;
    assert(!desk_clip_message(&m,d->clients[client].version,1,w,fd) && desk_parse(&m,1,NULL,&p));
    desk_clip_receive(d,client,&m,&p); desk_clip_tick(d);
}
static void test_owner_display(struct desktop *d)
{
    assert(!submit(d,0,DESK_CLIP_OPEN,0,0,NULL,0).result);
    assert(!submit(d,0,DESK_CLIP_EMPTY,0,0,NULL,0).result);
    assert(!submit(d,0,DESK_CLIP_PUT,0x80,DESK_CLIP_DELAYED,NULL,0).result);
    assert(!submit(d,0,DESK_CLIP_CLOSE,0,0,NULL,0).result);
    uint8_t request[8]={128}; struct desk_clip_wire w={.serial=200,.op=DESK_CLIP_DISPLAY,.format=0x30c,.bytes=8};
    unsigned before=count; wire_submit(d,1,&w,request);
    struct desk_packet p; assert(count==before+1 && desk_parse(&received[before],0,NULL,&p));
    unsigned nonce=p.clip.serial;
    assert(p.clip.kind==DESK_CLIP_DISPLAY_EVENT && nonce && p.clip.token==2 && payloads[before] && !memcmp(payloads[before],request,8));
    d->clients[2].channel=12; d->clients[2].version=2;
    uint8_t response[8]={0,0,0,0,'d','o','g',0};
    w=(struct desk_clip_wire){.serial=300,.op=DESK_CLIP_DISPLAY_REPLY,.format=0x30c,.flags=nonce,.token=2,.bytes=8};
    wire_submit(d,2,&w,response); assert(desk_parse(&received[count-1],0,NULL,&p) && p.clip.result==-ESRCH);
    wire_submit(d,0,&w,response); int found=0;
    for (unsigned i=before;i<count;i++) { assert(desk_parse(&received[i],0,NULL,&p));
        if (p.clip.kind==DESK_CLIP_REPLY && p.clip.serial==200) { assert(!p.clip.result && p.clip.bytes==8 && !memcmp(payloads[i],response,8)); found++; } }
    assert(found==1 && !live);
    w=(struct desk_clip_wire){.serial=201,.op=DESK_CLIP_DISPLAY,.format=0x30c,.bytes=8}; wire_submit(d,1,&w,request);
    assert(desk_parse(&received[count-1],0,NULL,&p)); nonce=p.clip.serial;
    d->now_ns+=DESK_CLIP_RENDER_NS; desk_clip_tick(d);
    assert(desk_parse(&received[count-1],0,NULL,&p) && p.clip.serial==201 && p.clip.result==-ETIMEDOUT);
    w=(struct desk_clip_wire){.serial=301,.op=DESK_CLIP_DISPLAY_REPLY,.format=0x30c,.flags=nonce,.token=2,.bytes=8};
    wire_submit(d,0,&w,response); assert(desk_parse(&received[count-1],0,NULL,&p) && p.clip.result==-ESRCH);
    w=(struct desk_clip_wire){.serial=203,.op=DESK_CLIP_DISPLAY,.format=0x30c,.bytes=8}; wire_submit(d,1,&w,request);
    assert(desk_parse(&received[count-1],0,NULL,&p)); unsigned old_nonce=p.clip.serial;
    desk_drop_client(d,1); d->clients[1].channel=14; d->clients[1].version=3;
    wire_submit(d,1,&w,request); assert(desk_parse(&received[count-1],0,NULL,&p)); nonce=p.clip.serial; assert(nonce!=old_nonce);
    w=(struct desk_clip_wire){.serial=302,.op=DESK_CLIP_DISPLAY_REPLY,.format=0x30c,.flags=old_nonce,.token=2,.bytes=8};
    wire_submit(d,0,&w,response); assert(desk_parse(&received[count-1],0,NULL,&p) && p.clip.result==-ESRCH);
    w.flags=nonce; w.serial=303; wire_submit(d,0,&w,response);
    assert(desk_parse(&received[count-1],0,NULL,&p) && p.clip.serial==203 && !p.clip.result);
    w=(struct desk_clip_wire){.serial=202,.op=DESK_CLIP_DISPLAY,.format=0x30c,.bytes=8}; wire_submit(d,1,&w,request);
    desk_drop_client(d,0); desk_clip_tick(d);
    assert(desk_parse(&received[count-1],0,NULL,&p) && p.clip.serial==202 && p.clip.result==-ETIMEDOUT);
    puts("PASS G5b owner-display forwarding, payload copies, sender authentication, deadline, late reply, client-slot reuse and owner-exit cancellation");
}
int main(void)
{
    struct desktop *d=calloc(1,sizeof(*d)); assert(d);
    d->ops=(struct desk_ops){send,info,map,unmap,close_fd,NULL}; d->width=640; d->height=480; d->focus=-1;
    for (int i=0;i<DESK_CLIENTS;i++) d->clients[i].channel=-1;
    d->clients[0].channel=10; d->clients[0].version=2;
    d->clients[1].channel=11; d->clients[1].version=3;
    assert(!submit(d,0,DESK_CLIP_OPEN,0,0,NULL,0).result);
    assert(!submit(d,0,DESK_CLIP_EMPTY,0,0,NULL,0).result);
    assert(!submit(d,0,DESK_CLIP_PUT,7,DESK_CLIP_DELAYED,NULL,0).result);
    assert(!submit(d,0,DESK_CLIP_PUT,16,DESK_CLIP_DELAYED,NULL,0).result);
    assert(!submit(d,0,DESK_CLIP_CLOSE,0,0,NULL,0).result);
    assert(!submit(d,1,DESK_CLIP_OPEN,0,0,NULL,0).result);
    unsigned before=count;
    assert(!submit(d,1,DESK_CLIP_GET,13,0,NULL,0).serial);
    assert(count==before+1); struct desk_packet p; assert(desk_parse(&received[count-1],0,NULL,&p) && p.clip.kind==DESK_CLIP_RENDER);
    assert(!submit(d,0,DESK_CLIP_PUT,7,DESK_CLIP_RENDERED,"x",2).result);
    desk_clip_tick(d); assert(desk_parse(&received[count-1],0,NULL,&p) && p.clip.kind==DESK_CLIP_RENDER && p.clip.format==16);
    const unsigned char locale[]={9,4,0,0};
    assert(!submit(d,0,DESK_CLIP_PUT,16,DESK_CLIP_RENDERED,locale,4).result);
    int found=0; for (unsigned i=before;i<count;i++) if (payloads[i]) { assert(!memcmp(payloads[i],"x\0\0",4)); found=1; } assert(found);
    assert(!submit(d,1,DESK_CLIP_CLOSE,0,0,NULL,0).result);
    assert(!submit(d,1,DESK_CLIP_OPEN,0,0,NULL,0).result);
    blocked=1; before=count; (void)submit(d,1,DESK_CLIP_GET,13,0,NULL,0); assert(count==before && !live);
    blocked=0; desk_clip_tick(d); assert(count==before+1 && !live);
    assert(!submit(d,1,DESK_CLIP_CLOSE,0,0,NULL,0).result);
    d->pixels=malloc(640*480*4); assert(d->pixels); for (unsigned i=0;i<640*480;i++) d->pixels[i]=i&0xffffff;
    d->focus=0; d->clients[0].frame=(struct desk_box){10,20,100,80}; assert(desk_clip_key(d,CIUKI_KEY_PRINTSCREEN,1,1)==1);
    struct desk_clip_wire w={.serial=1,.op=DESK_CLIP_OPEN},r; void *out=NULL;
    assert(!desk_clip_local(d,&w,NULL,&r,&out)); w=(struct desk_clip_wire){.serial=1,.op=DESK_CLIP_GET,.format=8};
    assert(!desk_clip_local(d,&w,NULL,&r,&out)); uint32_t *pixels; unsigned width,height;
    assert(!clip_dib_decode(out,r.bytes,&pixels,&width,&height) && width==100 && height==80 && pixels[0]==d->pixels[20*640+10]);
    free(pixels); free(out); out=NULL; w=(struct desk_clip_wire){.serial=1,.op=DESK_CLIP_CLOSE}; assert(!desk_clip_local(d,&w,NULL,&r,&out));
    assert(desk_clip_key(d,CIUKI_KEY_PRINTSCREEN,0,1)==1);
    assert(!desk_clip_key(d,CIUKI_KEY_A,1,0));
    assert(desk_clip_key(d,0x54,1,0)==1); desk_clip_drop(d,0); desk_clip_drop(d,1); test_owner_display(d); desk_clip_destroy(d);
    for (unsigned i=0;i<count;i++) free(payloads[i]); free(d->pixels); free(d); assert(!live);
    puts("PASS G5 desktop v2/v3 shared store, delayed render, full queue retry, surface lifetime, Print Screen and Alt+Print Screen clipping"); fflush(stdout);
    return 0;
}
