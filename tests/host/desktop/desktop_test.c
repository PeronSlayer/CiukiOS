/* SPDX-License-Identifier: MIT */
#include "fake.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct desktop d;
static void request(struct ciuki_message *m, unsigned op, unsigned n)
{ desk_message(m,op,n); m->sender_pid=42; }
static void create_message(struct ciuki_message *m, int surface, unsigned w, unsigned h)
{
    request(m,DESK_CREATE_WINDOW,20); m->fd_count=1; m->fds[0]=surface;
    desk_put32(m->data+4,w); desk_put32(m->data+8,h); desk_put32(m->data+12,4);
    memcpy(m->data+16,"Test",4);
}
static int client(int ch, int surf, unsigned w, unsigned h, uint32_t colour)
{
    fake_channel(ch); int i=desk_add_client(&d,ch); assert(i>=0);
    struct ciuki_message m; request(&m,DESK_HELLO,4); desk_receive(&d,i,&m);
    fake_surface(surf,w,h,colour); create_message(&m,surf,w,h); desk_receive(&d,i,&m);
    assert(d.clients[i].window); return i;
}
static void setup(unsigned w, unsigned h)
{ fake_reset(); assert(!desk_ignore_sigpipe()); desk_init(&d,&fake_ops,w,h); }
static void cleanup(void)
{ for (int i=0; i<DESK_CLIENTS; i++) desk_drop_client(&d,i); assert(!fake_live()); }
static void test_protocol(void)
{
    struct ciuki_message m; struct desk_packet p;
    struct ciuki_surface_info s={24,32,24,128,1,4096};
    for (unsigned op=DESK_HELLO; op<=DESK_CLOSED; op++) {
        int inbound=op<=DESK_PING || op==DESK_PONG;
        unsigned n=op==DESK_CREATE_WINDOW ? 20 : op==DESK_DAMAGE ? 20 :
                   op==DESK_MOVE || op==DESK_CONFIGURE ? 12 :
                   op==DESK_PING || op==DESK_PONG || op==DESK_FOCUS ? 8 :
                   op>=DESK_KEY && op<=DESK_BUTTON ? 44 : 4;
        request(&m,op,n);
        if (op==DESK_CREATE_WINDOW) create_message(&m,9,32,24);
        if (op==DESK_CONFIGURE) { desk_put32(m.data+4,32); desk_put32(m.data+8,24); }
        if (op>=DESK_KEY && op<=DESK_BUTTON) {
            struct ciuki_input_event e={.type=op==DESK_KEY ? CIUKI_INPUT_KEY : op==DESK_MOTION ? CIUKI_INPUT_MOTION : CIUKI_INPUT_BUTTON,
                .code=op==DESK_MOTION ? 0 : 1,.value=1,.monotonic_ns=0x123456789abcdef0ull};
            desk_event_message(&m,op,&e);
        }
        assert(desk_parse(&m,inbound,&s,&p));
        if (op>=DESK_KEY && op<=DESK_BUTTON) assert(p.event.monotonic_ns==0x123456789abcdef0ull);
        if (op!=DESK_PING && op!=DESK_PONG) assert(!desk_parse(&m,!inbound,&s,&p));
        struct ciuki_message bad=m;
        bad.length--; assert(!desk_parse(&bad,inbound,&s,&p));
        bad=m; bad.length++; assert(!desk_parse(&bad,inbound,&s,&p));
        bad=m; bad.data[0]=2; assert(!desk_parse(&bad,inbound,&s,&p));
        bad=m; bad.data[2]=1; assert(!desk_parse(&bad,inbound,&s,&p));
        bad=m; bad.data[255]=1; assert(!desk_parse(&bad,inbound,&s,&p));
        bad=m; bad.reserved=1; assert(!desk_parse(&bad,inbound,&s,&p));
        bad=m; bad.fd_count=5; assert(!desk_parse(&bad,inbound,&s,&p));
        bad=m; bad.fd_count=op==DESK_CREATE_WINDOW ? 0 : 1;
        assert(!desk_parse(&bad,inbound,&s,&p));
        bad=m; bad.fds[3]=8; assert(!desk_parse(&bad,inbound,&s,&p));
    }
    request(&m,99,4); assert(!desk_parse(&m,1,&s,&p));
    m.length=257; assert(!desk_parse(&m,1,&s,&p));
    create_message(&m,9,31,24); assert(!desk_parse(&m,1,&s,&p));
    create_message(&m,9,32,24); m.data[16]=0; assert(!desk_parse(&m,1,&s,&p));
    create_message(&m,9,32,24); m.fds[0]=-1; assert(!desk_parse(&m,1,&s,&p));
    for (unsigned field=0; field<6; field++) {
        struct ciuki_surface_info bad=s;
        uint32_t *fields=(uint32_t *)&bad; fields[field]=0;
        assert(!desk_surface_valid(&bad));
    }
    request(&m,DESK_DAMAGE,20);
    desk_put32(m.data+4,INT_MAX); assert(!desk_parse(&m,1,&s,&p));
    desk_put32(m.data+4,31); desk_put32(m.data+12,2); assert(!desk_parse(&m,1,&s,&p));
    desk_put32(m.data+12,1); assert(desk_parse(&m,1,&s,&p));
    request(&m,DESK_MOVE,12); desk_put32(m.data+4,INT_MIN); assert(!desk_parse(&m,1,&s,&p));
    struct ciuki_input_event e={.type=CIUKI_INPUT_TEXT,.code=0x10ffff};
    desk_event_message(&m,DESK_KEY,&e); assert(desk_parse(&m,0,NULL,&p));
    e.code=0xd800; desk_event_message(&m,DESK_KEY,&e); assert(!desk_parse(&m,0,NULL,&p));
    e.code=0x110000; desk_event_message(&m,DESK_KEY,&e); assert(!desk_parse(&m,0,NULL,&p));
    e.code=0; desk_event_message(&m,DESK_KEY,&e); assert(desk_parse(&m,0,NULL,&p));
    e.type=CIUKI_INPUT_BUTTON; e.code=4; desk_event_message(&m,DESK_BUTTON,&e); assert(!desk_parse(&m,0,NULL,&p));
    e.type=CIUKI_INPUT_RESYNC; e.code=0; e.value=8; desk_event_message(&m,DESK_KEY,&e); assert(!desk_parse(&m,0,NULL,&p));
    puts("PASS protocol: every opcode/direction, lengths, fds, versions, geometry, events");
}
static int covers(struct desk_box b,int x,int y)
{ return x>=b.x && y>=b.y && x<b.x+b.w && y<b.y+b.h; }
static void test_damage(void)
{
    struct desk_damage a={0};
    desk_damage_add(&a,(struct desk_box){-5,-5,10,10},64,64);
    desk_damage_add(&a,(struct desk_box){10,0,5,5},64,64);
    desk_damage_add(&a,(struct desk_box){5,0,5,5},64,64);
    assert(a.count==1 && a.boxes[0].x==0 && a.boxes[0].w==15);
    desk_damage_add(&a,(struct desk_box){INT_MAX,INT_MAX,INT_MAX,INT_MAX},64,64);
    assert(a.count==1);
    uint8_t changed[64][64]={{0}}; memset(&a,0,sizeof(a));
    unsigned state=123;
    for (int n=0; n<200; n++) {
        state=state*1664525u+1013904223u; int x=(int)(state%80)-8;
        state=state*1664525u+1013904223u; int y=(int)(state%80)-8;
        struct desk_box b={x,y,3,3}; desk_damage_add(&a,b,64,64);
        for (int yy=0; yy<64; yy++) for (int xx=0; xx<64; xx++) changed[yy][xx]|=(uint8_t)covers(b,xx,yy);
    }
    for (int y=0; y<64; y++) for (int x=0; x<64; x++) if (changed[y][x]) {
        int covered=0; for (unsigned i=0; i<a.count; i++) covered|=covers(a.boxes[i],x,y); assert(covered);
    }
    assert(a.count<=DESK_DAMAGE_MAX);
    memset(&a,0,sizeof(a));
    for (int n=0; n<33; n++) desk_damage_add(&a,(struct desk_box){n*3,0,1,1},128,128);
    assert(a.count==1 && a.boxes[0].w==97);
    struct desk_box f=desk_frame(2048,2048,0,32);
    assert(f.w==2054 && f.h==2075);
    puts("PASS damage/frame: clipping, signed extremes, transitive merge, saturation, coverage");
}
static void flush_all(void)
{ for (int i=0; i<DESK_CLIENTS; i++) desk_flush(&d,i); }
static void event(unsigned type,int code,int value,int value2)
{ struct ciuki_input_event e={.type=type,.code=code,.value=value,.value2=value2}; desk_input(&d,&e); }
static void pointer(int x,int y)
{ event(CIUKI_INPUT_MOTION,0,x-d.mouse_x,y-d.mouse_y); }
static void test_input(void)
{
    setup(640,480);
    int a=client(10,11,100,80,0x112233), b=client(12,13,100,80,0x445566);
    assert(d.focus==b); flush_all(); fake_sent=0;
    event(CIUKI_INPUT_KEY,0x1e,1,0); assert(d.keys[0x1e]);
    event(CIUKI_INPUT_KEY,0x11d,2,0); assert(d.keys[0x11d]);
    event(CIUKI_INPUT_KEY,0x200,1,0); assert(d.keys[0x200]);
    event(CIUKI_INPUT_TEXT,'a',0,0);
    struct ciuki_input_event e={.type=CIUKI_INPUT_RESYNC,.value=3,.lost_count=9}; desk_input(&d,&e);
    assert(!d.keys[0x1e] && !d.keys[0x11d] && !d.keys[0x200] && d.buttons==3 && d.drag==-1);
    flush_all(); assert(fake_sent==8); /* 4 events, 3 releases, RESYNC */
    for (unsigned i=0; i<fake_sent; i++) assert(fake_destinations[i]==12);
    event(CIUKI_INPUT_KEY,0x1e,1,0); desk_focus(&d,a); assert(!d.keys[0x1e] && d.focus==a);
    struct desk_box content=desk_content(&d.clients[a]);
    pointer(content.x+3,content.y+3); event(CIUKI_INPUT_BUTTON,1,1,0); assert(d.routed_buttons==1);
    pointer(639,479); event(CIUKI_INPUT_BUTTON,1,0,0); assert(!d.routed_buttons);
    pointer(d.clients[a].frame.x+8,d.clients[a].frame.y+8); event(CIUKI_INPUT_BUTTON,1,1,0); assert(d.drag==a);
    pointer(200,180); assert(d.clients[a].frame.x==192 && d.clients[a].frame.y==172);
    event(CIUKI_INPUT_BUTTON,1,0,0); assert(d.drag==-1);
    pointer(INT_MAX,INT_MAX); assert(d.mouse_x==639 && d.mouse_y==479);
    event(CIUKI_INPUT_MOTION,0,INT_MIN,INT_MIN); assert(!d.mouse_x && !d.mouse_y);
    struct desk_box close=desk_close_box(&d.clients[a]); pointer(close.x+1,close.y+1);
    event(CIUKI_INPUT_BUTTON,1,1,0); assert(!d.clients[a].window && d.focus==b);
    cleanup(); event(CIUKI_INPUT_KEY,0x1e,1,0); assert(!d.keys[0x1e]);
    event(CIUKI_INPUT_RESYNC,0,0,0); pointer(5,5); event(CIUKI_INPUT_BUTTON,1,1,0);
    puts("PASS focus/input: set-1 A/E0/Pause, TEXT, routing, RESYNC releases, drag/close, empty desktop");
}
static void test_lifecycle(void)
{
    setup(640,480); int survivor=client(10,11,100,80,0x123456);
    fake_block=1; struct ciuki_message m;
    for (unsigned n=0; n<DESK_TX_MAX+2; n++) { desk_message(&m,DESK_PING,8); desk_queue(&d,survivor,&m); }
    desk_flush(&d,survivor); assert(d.clients[survivor].window && d.clients[survivor].resync_pending);
    fake_block=0; flush_all(); flush_all();
    desk_heartbeat(&d,1000000000ull); desk_flush(&d,survivor);
    desk_heartbeat(&d,6000000000ull); assert(d.clients[survivor].unresponsive && d.clients[survivor].window);
    request(&m,DESK_PONG,8); desk_put32(m.data+4,d.clients[survivor].serial); desk_receive(&d,survivor,&m);
    assert(!d.clients[survivor].unresponsive);
    unsigned baseline=fake_live();
    for (int n=0; n<100; n++) {
        fake_sent=0;
        int victim=client(12,13,80,60,0xabcdef);
        for (int k=0; k<100; k++) { request(&m,DESK_PING,8); desk_put32(m.data+4,(uint32_t)k); desk_receive(&d,survivor,&m); desk_flush(&d,survivor); }
        fake_surface(14,8,8,0); create_message(&m,14,8,8); m.data[0]=99;
        desk_receive(&d,victim,&m);
        assert(!d.clients[victim].window && d.clients[survivor].window && fake_live()==baseline);
    }
    fake_channel(12); int i=desk_add_client(&d,12); request(&m,DESK_HELLO,4); desk_receive(&d,i,&m);
    fake_surface(13,8,8,0); create_message(&m,13,8,8); fake_fail_map=1; desk_receive(&d,i,&m);
    assert(!d.clients[i].window && fake_live()==baseline);
    fake_fail_map=0;
    i=client(12,13,8,8,0); fake_closed_peer=12;
    desk_flush(&d,i); assert(!d.clients[i].window && d.clients[survivor].window && fake_live()==baseline);
    fake_closed_peer=-1;
    request(&m,DESK_CLOSE,4); desk_receive(&d,survivor,&m); assert(!d.clients[survivor].window);
    cleanup();
    puts("PASS lifecycle: SIGPIPE/EPIPE isolation, backpressure, 5s heartbeat/recovery, 100 victim cycles, 100 replies/cycle, fd/map cleanup");
}
static void equivalent(uint32_t *reference, unsigned pixels)
{
    assert(!desk_redraw(&d));
    memcpy(reference,d.pixels,pixels*4);
    desk_composite(&d,(struct desk_box){0,0,(int32_t)d.width,(int32_t)d.height});
    assert(!memcmp(reference,d.pixels,pixels*4)); fake_presents=0;
}
static void test_compositor(void)
{
    setup(320,240);
    unsigned n=320*240; uint32_t *out=malloc((n+2)*4), *reference=malloc(n*4);
    assert(out && reference); out[0]=out[n+1]=0xfeedface; d.pixels=out+1;
    d.mouse_x=d.mouse_y=0;
    equivalent(reference,n);
    int a=client(10,11,100,80,0xff123456), b=client(12,13,120,100,0xabcdef);
    equivalent(reference,n);
    struct desk_box bc=desk_content(&d.clients[b]);
    assert(d.pixels[bc.y*d.width+bc.x]==0xabcdef);
    desk_focus(&d,a); equivalent(reference,n);
    desk_move(&d,a,-2048,2048); equivalent(reference,n);
    pointer(50,50); equivalent(reference,n);
    desk_drop_client(&d,a); equivalent(reference,n);
    d.damage.count=0; desk_damage_add(&d.damage,(struct desk_box){200,200,3,3},320,240);
    fake_fail_present=1; assert(desk_redraw(&d)==-1 && d.damage.count==1);
    fake_fail_present=0; assert(!desk_redraw(&d) && fake_presents==1);
    assert(fake_rects[0].width==3 && fake_rects[0].height==3 && fake_rects[0].src_x==200);
    assert(out[0]==0xfeedface && out[n+1]==0xfeedface);
    cleanup(); free(reference); free(out);
    puts("PASS compositor: incremental/full equivalence, z-order, clipping, cursor, present retry/damage bounds");
}
int main(void)
{
    test_protocol(); test_damage(); test_input(); test_lifecycle(); test_compositor();
    fake_reset(); puts("PASS desktop host model (guest qualification not_run)"); return 0;
}
