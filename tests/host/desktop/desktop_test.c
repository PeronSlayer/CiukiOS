/* SPDX-License-Identifier: MIT */
#include "fake.h"
#include "../../../apps/libdeskui/deskui.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
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
{ for (int i=0; i<DESK_CLIENTS; i++) desk_drop_client(&d,i); desk_render_release(&d); assert(!fake_live()); }
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
        bad=m; bad.data[0]=3; assert(!desk_parse(&bad,inbound,&s,&p));
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
    desk_message(&m,DESK_CLOSE_REQUEST,4); assert(!desk_parse(&m,0,NULL,&p));
    m.data[0]=DESK_SHELL_VERSION;
    assert(desk_parse(&m,0,NULL,&p) && !desk_parse(&m,1,NULL,&p));
    m.data[2]=2; assert(!desk_parse(&m,0,NULL,&p));
    puts("PASS protocol: every opcode/direction, lengths, fds, versions, geometry, events");
}
static int covers(struct desk_box b,int x,int y)
{ return x>=b.x && y>=b.y && x<b.x+b.w && y<b.y+b.h; }
static void test_cursor_requests(void)
{
    struct ciuki_message m,bad; struct desk_packet p;
    for(unsigned shape=0;shape<DESK_CURSOR_COUNT;shape++) {
        request(&m,DESK_SET_CURSOR,8); m.data[0]=DESK_SHELL_VERSION; desk_put32(m.data+4,shape);
        assert(desk_parse(&m,1,NULL,&p) && p.flags==shape); assert(!desk_parse(&m,0,NULL,&p));
        bad=m; bad.data[0]=DESK_VERSION; assert(!desk_parse(&bad,1,NULL,&p));
        bad=m; bad.length=9; assert(!desk_parse(&bad,1,NULL,&p));
        bad=m; bad.fd_count=1; bad.fds[0]=10; assert(!desk_parse(&bad,1,NULL,&p));
    }
    desk_put32(m.data+4,DESK_CURSOR_COUNT); assert(!desk_parse(&m,1,NULL,&p));
    setup(640,480); int indices[2];
    for(int k=0;k<2;k++) {
        fake_channel(10+k); indices[k]=desk_add_client(&d,10+k); assert(indices[k]>=0);
        request(&m,DESK_HELLO,4); m.data[0]=DESK_SHELL_VERSION; desk_receive(&d,indices[k],&m);
        fake_surface(20+k,120,80,0x123456); create_message(&m,20+k,120,80); m.data[0]=DESK_SHELL_VERSION;
        desk_receive(&d,indices[k],&m); assert(d.clients[indices[k]].window);
    }
    int i=indices[0],j=indices[1];
    desk_move(&d,i,30,60); desk_move(&d,j,300,60);
    struct desk_box box=desk_content(&d.clients[i]); d.mouse_x=box.x+20; d.mouse_y=box.y+20;
    request(&m,DESK_SET_CURSOR,8); m.data[0]=DESK_SHELL_VERSION; desk_put32(m.data+4,DESK_CURSOR_BRUSH);
    desk_receive(&d,i,&m); assert(d.cursor_shape==DESK_CURSOR_BRUSH && d.clients[j].cursor==DESK_CURSOR_ARROW);
    d.power.state=1; desk_cursor_update(&d); assert(d.cursor_shape==DESK_CURSOR_ARROW);
    d.power.state=0; desk_cursor_update(&d); assert(d.cursor_shape==DESK_CURSOR_BRUSH);
    d.sh.notification.active=1; desk_cursor_update(&d); assert(d.cursor_shape==DESK_CURSOR_ARROW);
    d.sh.notification.active=0; desk_cursor_update(&d); assert(d.cursor_shape==DESK_CURSOR_BRUSH);
    box=desk_content(&d.clients[j]); d.mouse_x=box.x+20; d.mouse_y=box.y+20; desk_cursor_update(&d);
    assert(d.cursor_shape==DESK_CURSOR_ARROW);
    d.mouse_x=d.clients[i].frame.x+40; d.mouse_y=d.clients[i].frame.y+5; desk_cursor_update(&d); assert(d.cursor_shape==DESK_CURSOR_ARROW);
    m.sender_pid=43; desk_receive(&d,i,&m); assert(!d.clients[i].window && d.clients[j].window);
    cleanup(); puts("PASS cursor requests: v2 bounds/direction/length/fds, sending window only, power/notification override, inactive peers and chrome unchanged, sender rejection");
}
static void test_titles(void)
{
    struct ciuki_message m, bad; struct desk_packet p;
    for (unsigned n = 1; n <= DESK_TITLE_MAX; n += DESK_TITLE_MAX - 1) {
        request(&m, DESK_SET_TITLE, 8 + n); m.data[0] = DESK_SHELL_VERSION;
        desk_put32(m.data + 4, n); memset(m.data + 8, 'x', n);
        assert(desk_parse(&m, 1, NULL, &p) && strlen(p.title) == n);
        assert(!desk_parse(&m, 0, NULL, &p));
        bad=m; bad.data[0]=DESK_VERSION; assert(!desk_parse(&bad, 1, NULL, &p));
        bad=m; bad.length--; assert(!desk_parse(&bad, 1, NULL, &p));
        bad=m; bad.length++; assert(!desk_parse(&bad, 1, NULL, &p));
        bad=m; bad.fd_count=1; bad.fds[0]=9; assert(!desk_parse(&bad, 1, NULL, &p));
        bad=m; bad.data[8]=31; assert(!desk_parse(&bad, 1, NULL, &p));
        bad=m; bad.data[8]=127; assert(!desk_parse(&bad, 1, NULL, &p));
        bad=m; desk_put32(bad.data + 4, 0); assert(!desk_parse(&bad, 1, NULL, &p));
        bad=m; desk_put32(bad.data + 4, DESK_TITLE_MAX + 1); assert(!desk_parse(&bad, 1, NULL, &p));
    }
    setup(640, 480); desk_shell_enable(&d);
    int indices[2];
    for (int k = 0; k < 2; k++) {
        fake_channel(10 + k); indices[k]=desk_add_client(&d, 10 + k); assert(indices[k]>=0);
        request(&m, DESK_HELLO, 4); m.data[0]=DESK_SHELL_VERSION; desk_receive(&d, indices[k], &m);
        fake_surface(20 + k, 32, 24, 0xffc0c0c0u);
        create_message(&m, 20 + k, 32, 24); m.data[0]=DESK_SHELL_VERSION; desk_receive(&d, indices[k], &m);
        assert(d.clients[indices[k]].window);
    }
    request(&m, DESK_SET_TITLE, 21); m.data[0]=DESK_SHELL_VERSION;
    desk_put32(m.data + 4, 13); memcpy(m.data + 8, "Files - C:\\X", 13);
    /* The title above includes its terminator: reject it, then send the 12 bytes. */
    assert(!desk_parse(&m, 1, NULL, &p)); m.length=20; desk_put32(m.data + 4, 12);
    d.damage.count=0; desk_receive(&d, indices[0], &m);
    assert(!strcmp(d.clients[indices[0]].title, "Files - C:\\X") && !strcmp(d.clients[indices[1]].title, "Test"));
    int taskbar=0, frame=0;
    for (unsigned k=0; k<d.damage.count; k++) {
        taskbar |= covers(d.damage.boxes[k], 1, 479);
        frame |= covers(d.damage.boxes[k], d.clients[indices[0]].frame.x, d.clients[indices[0]].frame.y);
    }
    assert(taskbar && frame);
    d.damage.count=0; desk_receive(&d, indices[0], &m); assert(!d.damage.count);
    m.sender_pid=43; desk_receive(&d, indices[0], &m);
    assert(d.clients[indices[0]].channel<0 && d.clients[indices[1]].window);
    fake_channel(12); int empty=desk_add_client(&d, 12); assert(empty>=0);
    request(&bad, DESK_HELLO, 4); bad.data[0]=DESK_SHELL_VERSION; desk_receive(&d, empty, &bad);
    m.sender_pid=42; desk_receive(&d, empty, &m); assert(d.clients[empty].channel<0);
    cleanup();
    puts("PASS window titles: v2 bounds/direction/ASCII/fds, own window only, frame/taskbar damage, no-op updates, sender/pre-create rejection");
}
static void resize_create(struct ciuki_message *m,int fd)
{
    create_message(m,fd,320,240);m->data[0]=2;m->data[2]=1;m->length+=28;
    unsigned limits[]={DESK_RESIZABLE,200,160,600,480,8,8};
    for(unsigned k=0;k<7;k++) desk_put32(m->data+20+k*4,limits[k]);
}
static void test_resize_protocol(void)
{
    struct ciuki_surface_info s={24,320,240,1280,1,307200};
    struct ciuki_message m,bad; struct desk_packet p;
    resize_create(&m,9);assert(desk_parse(&m,1,&s,&p) && p.min_width==200 && p.step_height==8);
    assert(!desk_parse(&m,0,&s,&p));
    for(unsigned k=0;k<7;k++) {
        bad=m;desk_put32(bad.data+20+k*4,k==0?2:0);assert(!desk_parse(&bad,1,&s,&p));
    }
    bad=m;desk_put32(bad.data+28,241);assert(!desk_parse(&bad,1,&s,&p));
    bad=m;desk_put32(bad.data+32,2049);assert(!desk_parse(&bad,1,&s,&p));
    bad=m;desk_put32(bad.data+40,7);assert(!desk_parse(&bad,1,&s,&p));
    for(unsigned n=0;n<28;n++) m.data[20+n]=0;
    assert(desk_parse(&m,1,&s,&p)); /* v2.1 fixed windows */
    for(unsigned op=DESK_CREATE_WINDOW;op<=DESK_CONFIGURE;op++) {
        if(op!=DESK_CREATE_WINDOW && op!=DESK_DAMAGE && op!=DESK_CONFIGURE) continue;
        int client=op!=DESK_CONFIGURE;
        if(op==DESK_CREATE_WINDOW) resize_create(&m,9);
        else {
            request(&m,op,op==DESK_DAMAGE?24:20);m.data[0]=2;m.data[2]=1;
            if(client) { m.fd_count=1;m.fds[0]=9;desk_put32(m.data+12,320);desk_put32(m.data+16,240);desk_put32(m.data+20,1); }
            else { desk_put32(m.data+4,320);desk_put32(m.data+8,240);desk_put32(m.data+12,1); }
        }
        assert(desk_parse(&m,client,&s,&p) && !desk_parse(&m,!client,&s,&p));
        bad=m;bad.length--;assert(!desk_parse(&bad,client,&s,&p));
        bad=m;bad.length++;assert(!desk_parse(&bad,client,&s,&p));
        bad=m;bad.data[2]=0;assert(!desk_parse(&bad,client,&s,&p));
        bad=m;bad.data[0]=1;assert(!desk_parse(&bad,client,&s,&p));
        bad=m;bad.data[2]=2;assert(!desk_parse(&bad,client,&s,&p));
        bad=m;bad.data[3]=1;assert(!desk_parse(&bad,client,&s,&p));
        bad=m;bad.fd_count=client?0:1;assert(!desk_parse(&bad,client,&s,&p));
        bad=m;bad.data[255]=1;assert(!desk_parse(&bad,client,&s,&p));
    }
    desk_put32(m.data+16,2);assert(!desk_parse(&m,0,NULL,&p));
    desk_put32(m.data+12,0);desk_put32(m.data+16,0);assert(!desk_parse(&m,0,NULL,&p));
    desk_put32(m.data+16,DESK_CONFIGURE_COMMITTED);assert(desk_parse(&m,0,NULL,&p));
    request(&m,DESK_DAMAGE,24);m.data[0]=2;m.data[2]=1;
    assert(desk_parse(&m,1,&s,&p));m.fd_count=1;m.fds[0]=9;assert(!desk_parse(&m,1,&s,&p));
    desk_put32(m.data+20,1);assert(!desk_parse(&m,1,&s,&p)); /* swap must damage full surface */
    puts("PASS resize protocol: v2.1 directions/lengths/attachments/min-max/steps/flags, v1/v2.0 isolation");
}
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
static void test_resize_title_buttons(void)
{
    setup(800,600);desk_shell_enable(&d);
    fake_channel(10);int i=desk_add_client(&d,10);assert(i>=0);
    struct ciuki_message m;request(&m,DESK_HELLO,4);m.data[0]=2;m.data[2]=1;desk_receive(&d,i,&m);
    fake_surface(11,320,240,0x112233);resize_create(&m,11);desk_receive(&d,i,&m);flush_all();
    struct desk_client *c=&d.clients[i];struct desk_box original=c->frame;
    struct desk_box zoom=desk_shade_box(c);
    pointer(zoom.x+1,zoom.y+1);assert(d.title_hot==i*4+3);
    event(CIUKI_INPUT_BUTTON,1,1,0);
    assert(c->zoomed && d.drag==-1 && d.title_down==i*4+3 && !d.routed_buttons);
    event(CIUKI_INPUT_BUTTON,1,0,0);assert(!d.title_down);
    zoom=desk_shade_box(c);pointer(zoom.x+1,zoom.y+1);
    event(CIUKI_INPUT_BUTTON,1,1,0);event(CIUKI_INPUT_BUTTON,1,0,0);
    assert(!c->zoomed && !memcmp(&original,&c->frame,sizeof(original)));
    pointer(c->frame.x+c->frame.w-8,c->frame.y+c->frame.h-8);
    assert(!d.title_hot);event(CIUKI_INPUT_BUTTON,1,1,0);
    assert(d.drag==i && d.resize_edges==(DESK_EDGE_RIGHT|DESK_EDGE_BOTTOM) && !d.title_down);
    event(CIUKI_INPUT_BUTTON,1,0,0);
    pointer(c->frame.x+60,c->frame.y+10);event(CIUKI_INPUT_BUTTON,1,1,0);
    assert(d.drag==i && !d.resize_edges && !d.title_down);
    pointer(d.mouse_x+16,d.mouse_y+12);event(CIUKI_INPUT_BUTTON,1,0,0);
    assert(c->frame.x==original.x+16 && c->frame.y==original.y+12);
    /* A short surface must not extend its corner grab region into Close. */
    c->frame=desk_frame(320,1,20,52);
    struct desk_box close=desk_close_box(c);pointer(close.x+close.w-1,close.y+close.h-1);
    assert(!desk_resize_hit(c,d.mouse_x,d.mouse_y) && d.title_hot==i*4+1);
    event(CIUKI_INPUT_BUTTON,1,1,0);
    assert(c->closing && d.drag==-1 && d.title_down==i*4+1);
    event(CIUKI_INPUT_BUTTON,1,0,0);assert(!d.title_down);cleanup();
    puts("PASS merged title/resize input: zoom hover/press/release, corner capture, title drag, short-frame Close priority");
}
static void test_resize(void)
{
    setup(800,600);desk_shell_enable(&d);
    fake_channel(10);int i=desk_add_client(&d,10);assert(i>=0);
    struct ciuki_message m;request(&m,DESK_HELLO,4);m.data[0]=2;m.data[2]=1;desk_receive(&d,i,&m);
    fake_surface(11,320,240,0x112233);resize_create(&m,11);desk_receive(&d,i,&m);flush_all();
    struct desk_client *c=&d.clients[i];struct desk_box f=c->frame;
    assert(c->resizable && fake_live()==3);
    c->awaiting_frame=1; /* A resize acknowledgement may be the first rendered frame. */
    int x[8]={f.x-6,f.x+f.w+6,f.x+100,f.x+100,f.x-6,f.x+f.w+6,f.x-6,f.x+f.w+6};
    int y[8]={f.y+100,f.y+100,f.y-6,f.y+f.h+6,f.y-6,f.y-6,f.y+f.h+6,f.y+f.h+6};
    unsigned edges[8]={1,2,4,8,5,6,9,10};
    for(int k=0;k<8;k++) assert(desk_resize_hit(c,x[k],y[k])==edges[k] && desk_hit(&d,x[k],y[k])==i);
    assert(!desk_resize_hit(c,f.x+100,f.y+100));
    pointer(f.x+f.w+2,f.y+f.h+2);event(CIUKI_INPUT_BUTTON,1,1,0);
    assert(d.drag==i && d.resize_edges==10 && !d.routed_buttons);
    pointer(d.mouse_x+83,d.mouse_y+51);event(CIUKI_INPUT_BUTTON,1,0,0);
    assert(d.drag==-1 && c->frame.w==406 && c->frame.h==315 && c->info.width==320 && c->surface==11);
    assert(c->resize_serial==1);
    assert(c->awaiting_frame); /* CREATE/CONFIGURE do not release scripted input. */
    flush_all();fake_surface(12,400,288,0x445566);
    request(&m,DESK_DAMAGE,24);m.data[0]=2;m.data[2]=1;m.fd_count=1;m.fds[0]=12;
    desk_put32(m.data+12,400);desk_put32(m.data+16,288);desk_put32(m.data+20,1);desk_receive(&d,i,&m);
    assert(!c->awaiting_frame && c->launch_frame_pending && c->surface==12 && c->info.width==400 && c->applied_serial==1 && fake_live()==3);flush_all();
    struct desk_box user=c->frame;desk_zoom(&d,i);
    assert(c->zoomed && !c->collapsed && c->frame.x==0 && c->frame.y==32 && c->frame.w<=800 && c->frame.h<=536);
    desk_collapse(&d,i);assert(c->collapsed);desk_collapse(&d,i);assert(!c->collapsed);
    desk_zoom(&d,i);assert(!c->zoomed && !memcmp(&user,&c->frame,sizeof(user)));
    /* Backpressure remembers a commit and the latest proposal separately. */
    flush_all();fake_block=1;desk_message(&m,DESK_PONG,8);
    for(int k=0;k<DESK_TX_MAX;k++) desk_queue(&d,i,&m);
    desk_resize(&d,i,desk_frame(600,480,20,52));assert(c->configure_pending);
    /* A slot opens while the old pending proposal waits: queueing the next
     * size must clear pending state, or it would be sent twice later. */
    c->tx_head=(c->tx_head+1)%DESK_TX_MAX;c->tx_count--;
    desk_resize(&d,i,desk_frame(592,472,20,52));assert(!c->configure_pending);
    /* Its acknowledgement arrives while the outbound FIFO is full. */
    uint32_t serial=c->resize_serial;
    fake_surface(13,592,472,0x778899);
    request(&m,DESK_DAMAGE,24);m.data[0]=2;m.data[2]=1;m.fd_count=1;m.fds[0]=13;
    desk_put32(m.data+12,592);desk_put32(m.data+16,472);desk_put32(m.data+20,serial);
    desk_receive(&d,i,&m);assert(c->commit_pending && fake_live()==3);
    desk_resize(&d,i,desk_frame(584,464,20,52));
    desk_collapse(&d,i); /* pending CONFIGURE must retain the full unshaded size */
    fake_block=0;flush_all();flush_all();
    int n=0,commits=0;for(unsigned k=0;k<fake_sent;k++) if(fake_messages[k].data[1]==DESK_CONFIGURE && fake_messages[k].length==20) {
        if(desk_get32(fake_messages[k].data+12)==c->resize_serial) { n++;assert(desk_get32(fake_messages[k].data+8)==464); }
        if(desk_get32(fake_messages[k].data+12)==serial && desk_get32(fake_messages[k].data+16)) commits++;
    }
    assert(n==1 && commits==1);cleanup();
    /* Fixed old clients and internal windows cannot acquire resize/zoom. */
    setup(800,600);i=client(10,11,320,240,0x112233);f=d.clients[i].frame;
    desk_resize(&d,i,desk_frame(600,480,0,32));desk_zoom(&d,i);assert(!memcmp(&f,&d.clients[i].frame,sizeof(f)));
    cleanup();puts("PASS resize WM: eight edges/margin/corner capture, live step limits, read-only swaps/cleanup, zoom restore/windowshade, backlog, fixed clients");
    for(int fault=0;fault<5;fault++) {
        setup(800,600);fake_channel(10);i=desk_add_client(&d,10);
        request(&m,DESK_HELLO,4);m.data[0]=2;m.data[2]=1;desk_receive(&d,i,&m);
        fake_surface(11,320,240,0x112233);resize_create(&m,11);desk_receive(&d,i,&m);
        desk_resize(&d,i,desk_frame(400,288,20,52));flush_all();
        if(fault==3) fake_channel(12); else fake_surface(12,fault==2?408:400,288,0x445566);
        request(&m,DESK_DAMAGE,24);m.data[0]=2;m.data[2]=1;m.fd_count=1;m.fds[0]=12;
        desk_put32(m.data+12,fault==2?408:400);desk_put32(m.data+16,288);desk_put32(m.data+20,fault==1?2:1);
        if(fault==0) m.data[2]=0; /* channel version changes */
        if(fault==4) fake_fail_map=1;
        desk_receive(&d,i,&m);assert(!d.clients[i].window && !fake_live());cleanup();
    }
    puts("PASS resize peer isolation: wrong version/serial/size/non-surface/map failure, every fd and mapping released");
}
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
    int wheel_x=d.mouse_x, wheel_y=d.mouse_y;
    event(CIUKI_INPUT_WHEEL,0,-1,0);
    assert(d.mouse_x==wheel_x && d.mouse_y==wheel_y);
    flush_all();
    struct ciuki_message wheel_message;
    struct ciuki_input_event wheel_event={.type=CIUKI_INPUT_WHEEL,.value=-1};
    struct desk_packet wheel_packet;
    desk_event_message(&wheel_message,DESK_MOTION,&wheel_event);
    assert(desk_parse(&wheel_message,0,NULL,&wheel_packet) && wheel_packet.event.type==CIUKI_INPUT_WHEEL && wheel_packet.event.value==-1);
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
    for (unsigned i=0;i<pixels;i++) if (reference[i]!=d.pixels[i]) {
        fprintf(stderr,"incremental mismatch x=%u y=%u expected=%06x actual=%06x shell=%d menu=%d about=%d focus=%d\n",
            i%d.width,i/d.width,reference[i],d.pixels[i],d.shell,d.menu_open,d.about,d.focus);
        assert(0);
    }
    fake_presents=0;
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
    desk_shell_enable(&d); equivalent(reference,n);
    pointer(40,224); event(CIUKI_INPUT_BUTTON,1,1,0); event(CIUKI_INPUT_BUTTON,1,0,0); equivalent(reference,n);
    event(CIUKI_INPUT_BUTTON,1,1,0); event(CIUKI_INPUT_BUTTON,1,0,0); equivalent(reference,n);
    desk_minimize(&d,b); equivalent(reference,n);
    desk_minimize(&d,b); equivalent(reference,n);
    desk_about(&d); equivalent(reference,n);
    desk_request_close(&d,d.about); equivalent(reference,n);
    d.damage.count=0; desk_damage_add(&d.damage,(struct desk_box){200,200,3,3},320,240);
    fake_fail_present=1; assert(desk_redraw(&d)==-1 && d.damage.count==1);
    fake_fail_present=0; assert(!desk_redraw(&d) && fake_presents==1);
    assert(fake_rects[0].width==3 && fake_rects[0].height==3 && fake_rects[0].src_x==200);
    assert(out[0]==0xfeedface && out[n+1]==0xfeedface);
    cleanup(); free(reference); free(out);
    puts("PASS compositor: incremental/full equivalence, z-order, clipping, cursor, present retry/damage bounds");
}
static unsigned term_calls, term_pid;
static int terminate(uint32_t pid, int signo)
{ assert(signo==SIGTERM); term_calls++; term_pid=pid; return 0; }
static void test_shell(void)
{
    setup(640,480); desk_shell_enable(&d);
    int a=client(10,11,100,80,0x112233), b=client(12,13,100,80,0x445566);
    desk_move(&d,a,40,60); desk_move(&d,b,40,60);
    assert(desk_hit(&d,50,90)==b);
    struct desk_box ab=desk_task_button(&d,a), bb=desk_task_button(&d,b);
    assert(ab.x+ab.w<=bb.x && bb.x+bb.w<=560 && ab.y>=448 && ab.h==26);
    flush_all(); fake_sent=0;
    event(CIUKI_INPUT_KEY,0x1e,1,0); pointer(50,90); event(CIUKI_INPUT_BUTTON,1,1,0);
    desk_minimize(&d,b); flush_all();
    assert(d.focus==a && d.clients[b].minimized && desk_hit(&d,50,90)==a);
    int key_up=0,button_up=0,unfocus=0;
    for (unsigned k=0; k<fake_sent; k++) if (fake_destinations[k]==12) {
        struct desk_packet p; assert(desk_parse(&fake_messages[k],0,NULL,&p));
        key_up+=p.opcode==DESK_KEY && p.event.type==CIUKI_INPUT_KEY && p.event.code==0x1e && !p.event.value;
        button_up+=p.opcode==DESK_BUTTON && !p.event.value;
        unfocus+=p.opcode==DESK_FOCUS && !p.focused;
    }
    assert(key_up==1 && button_up==1 && unfocus==1 && !d.routed_buttons && !d.keys[0x1e]);
    event(CIUKI_INPUT_BUTTON,1,0,0);
    pointer(bb.x+3,bb.y+3); event(CIUKI_INPUT_BUTTON,1,1,0); event(CIUKI_INPUT_BUTTON,1,0,0);
    assert(!d.clients[b].minimized && d.focus==b && desk_hit(&d,50,90)==b);
    desk_move(&d,b,INT_MAX,INT_MAX);
    assert(d.clients[b].frame.y+d.clients[b].frame.h==448);
    pointer(d.clients[b].frame.x+5,d.clients[b].frame.y+8); event(CIUKI_INPUT_BUTTON,1,1,0);
    assert(d.drag==b); event(CIUKI_INPUT_RESYNC,0,0,0); assert(d.drag<0);
    /* The Ciuki button at the left end of the top bar; its menu drops below the bar. */
    desk_focus(&d,a); pointer(40,16); event(CIUKI_INPUT_BUTTON,1,1,0);
    assert(d.menu_open && d.focus<0);
    { struct desk_box m=desk_menu_box(&d), s=desk_start_button(&d);
      assert(s.x==DESK_START_X && s.y+s.h<=DESK_BAR && m.x==s.x && m.y==DESK_BAR && m.y+m.h<=448); }
    event(CIUKI_INPUT_KEY,1,1,0); assert(!d.menu_open && d.focus==a);
    event(CIUKI_INPUT_BUTTON,1,0,0);
    /* Negotiated v2, request survives backlog and deadline survives EOF. */
    d.clients[a].version=DESK_SHELL_VERSION; d.children[0].pid=42;
    d.terminate=terminate; d.now_ns=100; term_calls=term_pid=0;
    fake_block=1;
    struct ciuki_message m; desk_message(&m,DESK_PONG,8);
    for (int k=0; k<DESK_TX_MAX; k++) desk_queue(&d,a,&m);
    desk_request_close(&d,a); assert(d.clients[a].close_pending && d.clients[a].window);
    fake_block=0; fake_sent=0; flush_all(); flush_all();
    int requested=0;
    for (unsigned k=0; k<fake_sent; k++) if (fake_messages[k].data[1]==DESK_CLOSE_REQUEST) {
        struct desk_packet p; assert(fake_messages[k].data[0]==2 && desk_parse(&fake_messages[k],0,NULL,&p)); requested++;
    }
    assert(requested==1); desk_drop_client(&d,a);
    d.now_ns=100+DESK_CLOSE_NS-1; desk_close_timeouts(&d); assert(!term_calls);
    d.now_ns++; desk_close_timeouts(&d); assert(term_calls==1 && term_pid==42);
    desk_close_timeouts(&d); assert(term_calls==1);
    desk_child_exited(&d,42,0); assert(!d.children[0].pid);
    desk_about(&d); int about=d.about; assert(about>=0 && d.clients[about].internal);
    desk_about(&d); assert(d.about==about);
    desk_minimize(&d,about); assert(d.clients[about].minimized);
    desk_about(&d); assert(!d.clients[about].minimized && d.focus==about);
    desk_request_close(&d,about); assert(d.about<0);
    cleanup();
    puts("PASS shell: hit/z-order, taskbar restore, releases, drag RESYNC, work area, v2 close/backpressure/EOF timeout, About lifecycle");
}
static void test_media_shell(void)
{
    setup(1024,768); strcpy(d.programs[0].name,"files"); d.program_count=1; desk_shell_enable(&d);
    for (int i=0;i<d.sh.icon_count;i++) assert(strcmp(d.sh.icons[i].label,"Pictures"));
    d.pictures_exists=1; d.sh.icon_count=0; desk_shell_apply(&d);
    int pictures=0;
    for (int i=0;i<d.sh.icon_count;i++) if (!strcmp(d.sh.icons[i].label,"Pictures")) {
        assert(!strcmp(d.sh.icons[i].argument,"/Documents/Pictures")); pictures++;
    }
    assert(pictures==1);
    strcpy(d.programs[1].name,"music"); strcpy(d.programs[2].name,"viewer"); d.program_count=3;
    assert(desk_program_graphical("music") && desk_program_graphical("viewer"));
    desk_menu_open(&d,SH_MENU_PROGRAMS,4,32);
    int music=0,viewer=0;
    for (int i=0;i<d.sh.menus[0].count;i++) {
        music+=!strcmp(d.sh.menus[0].items[i].label,"Music Player"); viewer+=!strcmp(d.sh.menus[0].items[i].label,"Image Viewer");
    }
    assert(music==1 && viewer==1);
    strcpy(d.programs[3].name,"regedit"); d.program_count=4;
    assert(!strcmp(desk_program_category("regedit"),"accessories"));
    assert(!strcmp(desk_program_icon_name("regedit"),"preferences-system"));
    desk_menu_open(&d,SH_MENU_ACCESSORIES,4,32); viewer=0;
    int registry=0;
    for (int i=0;i<d.sh.menus[0].count;i++) registry+=!strcmp(d.sh.menus[0].items[i].label,"Registry Editor");
    assert(registry==1);
    for (int i=0;i<d.sh.menus[0].count;i++) viewer+=!strcmp(d.sh.menus[0].items[i].label,"Image Viewer");
    assert(viewer==1); cleanup(); puts("PASS media shell: Programs entries, Accessories viewer and conditional Pictures");
}
static void test_graphics_menu(void)
{
    setup(1024,768); strcpy(d.programs[0].name,"gfxdemo"); d.program_count=1; desk_shell_enable(&d);
    assert(desk_program_graphical("gfxdemo"));
    assert(!strcmp(desk_program_category("gfxdemo"),"accessories"));
    assert(!strcmp(desk_program_icon_name("gfxdemo"),"applications-graphics"));
    desk_menu_open(&d,SH_MENU_ACCESSORIES,4,32); int graphics=0;
    for (int i=0;i<d.sh.menus[0].count;i++) graphics+=!strcmp(d.sh.menus[0].items[i].label,"Graphics Demo");
    assert(graphics==1); cleanup(); puts("PASS Graphics Demo in Accessories");
}
static unsigned graphics_samples;
static int graphics_snapshot(int display,uint32_t generation,struct ciuki_accel_info *out)
{
    (void)display;assert(generation==77);
    *out=(struct ciuki_accel_info){.driver=CIUKI_ACCEL_DRIVER_SAVAGE,.capabilities=1,.generation=77,
        .quarantined=1,.operations=0x123456789ull,.idle_waits=0xabcdef123ull,.fallbacks=5};return 0;
}
static int graphics_sample(struct desktop *desktop,unsigned index,uint64_t values[3])
{ assert(desktop==&d && index==2);graphics_samples++;values[0]=640*480*4;values[1]=1000000;values[2]=1;return 0; }
static struct desk_packet graphics_reply(int i)
{
    struct desk_client *c=&d.clients[i];assert(c->tx_count);
    struct ciuki_message m=c->tx[(c->tx_head+c->tx_count-1)%DESK_TX_MAX];struct desk_packet p;
    assert(desk_parse(&m,0,NULL,&p));c->tx_count=0;return p;
}
static void test_graphics_service(void)
{
    setup(640,480);int i=client(10,11,120,80,0);d.clients[i].version=2;d.clients[i].tx_count=0;
    d.accel_info=graphics_snapshot;d.accel_generation=77;d.lfb_bytes_per_pixel=4;d.gfx_benchmark=graphics_sample;
    struct ciuki_message m;request(&m,DESK_GFX_QUERY,8);m.data[0]=2;desk_put32(m.data+4,17);
    /* A title alone cannot acquire the desktop's launch-bound authority. */
    strcpy(d.clients[i].title,"Graphics Demo");desk_receive(&d,i,&m);
    struct desk_packet p=graphics_reply(i);assert(p.opcode==DESK_TASK_RESULT && p.result==-EPERM);
    d.clients[i].gfxdemo=1;desk_receive(&d,i,&m);p=graphics_reply(i);
    assert(p.opcode==DESK_GFX_INFO && p.serial==17 && p.width==640 && p.height==480 && p.bpp==32 &&
        p.accelerator.operations==0x123456789ull && p.accelerator.idle_waits==0xabcdef123ull && p.accelerator.quarantined==1);
    request(&m,DESK_GFX_BENCH,12);m.data[0]=2;desk_put32(m.data+4,18);desk_put32(m.data+8,2);
    graphics_samples=0;d.now_ns=1000000000ull;d.focus=i;desk_receive(&d,i,&m);p=graphics_reply(i);
    assert(p.opcode==DESK_GFX_RESULT && !p.result && p.benchmark[2]==1 && graphics_samples==1);
    desk_receive(&d,i,&m);p=graphics_reply(i);assert(p.result==-EBUSY && graphics_samples==1);
    d.now_ns+=100000000ull;d.clients[i].minimized=1;desk_receive(&d,i,&m);p=graphics_reply(i);assert(p.result==-EBUSY);
    d.clients[i].minimized=0;d.focus=-1;desk_receive(&d,i,&m);p=graphics_reply(i);assert(p.result==-EBUSY);
    d.focus=i;d.power.state=DESK_POWER_CLOSING;desk_receive(&d,i,&m);p=graphics_reply(i);assert(p.result==-EBUSY);
    d.power.state=0;d.clients[i].gfxdemo=0;desk_receive(&d,i,&m);p=graphics_reply(i);assert(p.result==-EPERM);
    struct desk_packet parsed;desk_put32(m.data+8,6);assert(!desk_parse(&m,1,NULL,&parsed));
    desk_put32(m.data+8,2);m.data[0]=1;assert(!desk_parse(&m,1,NULL,&parsed));
    cleanup();puts("PASS graphics service: launch authorization, 64-bit kernel counters, rate limit, focus/minimize/shutdown, protocol bounds");
}
static void test_menu(void)
{
    setup(320,240);
    uint8_t elf[52]={0}; memcpy(elf,"\177ELF\1\1\1",7);
    elf[16]=2; elf[18]=3; elf[20]=1; elf[40]=52;
    assert(desk_program_add(&d,"lua",elf,sizeof(elf)));
    assert(desk_program_add(&d,"demo",elf,sizeof(elf)) && !strcmp(d.programs[0].path,"/Programs/demo"));
    assert(!desk_program_add(&d,"demo",elf,sizeof(elf)));
    char longest[64]; memset(longest,'z',sizeof(longest)-1); longest[sizeof(longest)-1]=0;
    assert(desk_program_add(&d,longest,elf,sizeof(elf)));
    assert(strlen(d.programs[2].path)==strlen("/Programs/")+strlen(longest) &&
           !strcmp(d.programs[2].path+strlen("/Programs/"),longest));
    assert(!desk_program_add(&d,longest,elf,sizeof(elf)));
    const char *bad[]={"", ".", "..", ".hidden", "a/b", "a\\b", "a:b", "a b", "desktop", "DESKTOP", "\xff"};
    for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) assert(!desk_program_add(&d,bad[i],elf,sizeof(elf)));
    char huge[80]; memset(huge,'a',sizeof(huge)); huge[79]=0;
    assert(!desk_program_add(&d,huge,elf,sizeof(elf)));
    for (size_t n=0;n<sizeof(elf);n++) assert(!desk_program_add(&d,"truncated",elf,n));
    for (int k=0;k<7;k++) { uint8_t copy[52]; memcpy(copy,elf,52); copy[k]^=0xff; assert(!desk_program_add(&d,"invalid",copy,52)); }
    uint8_t copy[52]; memcpy(copy,elf,52); copy[4]=2; assert(!desk_program_add(&d,"elf64",copy,52));
    for (int i=31;i>=0;i--) { char name[20]; snprintf(name,sizeof(name),"app%02d",i); desk_program_add(&d,name,elf,52); }
    assert(d.program_count==DESK_MENU_MAX && !strcmp(d.programs[0].name,"app00") && !strcmp(d.programs[15].name,"app15"));
    desk_shell_enable(&d);
    struct desk_box menu=desk_menu_box(&d);
    /* The Start menu (shell.c) drops from the top bar; on a 240-line screen it moves up to end on the taskbar. */
    assert(menu.y>=0 && menu.y+menu.h==208 && menu.x==DESK_START_X && menu.x+menu.w<=320);
    for (int i=0;i<DESK_CLIENTS;i++) {
        d.clients[i].window=1;
        struct desk_box task=desk_task_button(&d,i);
        assert(task.x>=DESK_TASK_X && task.x+task.w<=desk_shell_tray_box(&d).x-8 && task.w>0);
    }
    for (int i=0;i<DESK_CLIENTS;i++) d.clients[i].window=0;
    cleanup();
    puts("PASS menu parser: bounded names, ELF filter, self/duplicate rejection, stable sort/cap, taskbar/menu layout");
}
static char ide_path[DESK_ARG_MAX+1];
static int ide_launch_count;
static int ide_launch(struct desktop *desktop,const struct desk_program *p)
{
    (void)desktop; assert(!strcmp(p->name,"ciukide-app"));
    snprintf(ide_path,sizeof(ide_path),"%s",p->path); ++ide_launch_count; return 123;
}
static void test_ide_launch(void)
{
    setup(640,480); int i=client(5,12,64,48,0x123456);
    d.clients[i].version=DESK_SHELL_VERSION; /* LAUNCH is a v2 client request. */
    d.launch=ide_launch; ide_launch_count=0;
    assert(desk_program_graphical("ciukide") && desk_program_kind("ciukide")==DESK_APP_IDE);
    assert(desk_program_graphical("ciukide-app") && !desk_program_kind("ciukide-app"));
    assert(desk_ide_launch(&d,"/Documents/Test/hello")==123);
    assert(!strcmp(ide_path,"/Documents/Test/hello"));
    const char *bad[]={"/System/kernel", "/Documents/../Programs/tcc", "/Documents/x/../../System/tcc",
        "/Documents//hello", "/Documents/./hello", "/Documents/hello/", "/Documents/x\\hello", "/Documents/x:hello"};
    for (unsigned b=0;b<sizeof(bad)/sizeof(bad[0]);++b) assert(desk_ide_launch(&d,bad[b])<0);
    assert(ide_launch_count==1);
    struct ciuki_message m; const char *name="ciukide-run", *path="/Documents/Test/hello";
    unsigned n=(unsigned)strlen(name),p=(unsigned)strlen(path);
    request(&m,DESK_LAUNCH,16+n+p); m.data[0]=DESK_SHELL_VERSION;
    desk_put32(m.data+4,44); desk_put32(m.data+8,n); memcpy(m.data+12,name,n);
    desk_put32(m.data+12+n,p); memcpy(m.data+16+n,path,p);
    desk_receive(&d,i,&m); assert(ide_launch_count==1 && d.clients[i].channel>=0); /* ordinary client denied */
    d.clients[i].program=DESK_APP_IDE; desk_receive(&d,i,&m); assert(ide_launch_count==2);
    d.power.state=1; assert(desk_ide_launch(&d,path)==-EBUSY);
    d.power.state=0; cleanup(); puts("PASS CiukIDE: desktop-owned identity, project path checks, ordinary launch callback");
}
static unsigned terminal_kill_pid, terminal_mix_count;
static int terminal_launch_test(struct desktop *desktop, const struct desk_program *p)
{ (void)desktop; assert(!strcmp(p->name,"ciuknote")); ide_launch_count++; return 123; }
static int terminal_kill(uint32_t pid, int sig)
{ assert(sig == SIGKILL); terminal_kill_pid = pid; return 0; }
static int terminal_mixer(struct desktop *desktop, uint32_t l, uint32_t r, uint32_t mute)
{ (void)desktop; assert(l == 20 && r == 30 && mute == 1); terminal_mix_count++; return 0; }
static int terminal_result(int client_index)
{
    unsigned start = fake_sent; desk_flush(&d, client_index);
    for (unsigned k = start; k < fake_sent; k++) if (fake_messages[k].data[1] == DESK_TASK_RESULT)
        return (int32_t)desk_get32(fake_messages[k].data + 12);
    assert(0); return -999;
}
static void test_terminal(void)
{
    setup(640,480); int i=client(5,12,64,48,0x123456);
    d.clients[i].version=DESK_SHELL_VERSION; desk_flush(&d,i);
    assert(desk_program_graphical("terminal") && desk_program_kind("terminal")==DESK_APP_TERMINAL);
    assert(!strcmp(desk_program_label("terminal"),"Terminal"));
    assert(!strcmp(desk_program_icon_name("terminal"),"utilities-terminal"));
    assert(!strcmp(desk_program_category("terminal"),"programs"));
    strcpy(d.programs[0].name,"terminal"); strcpy(d.programs[0].path,"/Programs/terminal");
    strcpy(d.programs[1].name,"ciuknote"); strcpy(d.programs[1].path,"/Programs/ciuknote");
    strcpy(d.programs[2].name,"lua"); strcpy(d.programs[2].path,"/Programs/lua"); d.program_count=3;
    desk_shell_enable(&d); desk_menu_open(&d,SH_MENU_PROGRAMS,4,32);
    int found=0; for(int k=0;k<d.sh.menus[0].count;k++)if(!strcmp(d.sh.menus[0].items[k].label,"Terminal"))found++;
    assert(found==1); desk_menu_close(&d);
    struct ciuki_message m; struct desk_packet packet;
    request(&m,DESK_TERMINAL_POWER,12); m.data[0]=DESK_SHELL_VERSION; desk_put32(m.data+4,9); desk_put32(m.data+8,CIUKI_POWER_OFF);
    assert(desk_parse(&m,1,NULL,&packet) && packet.flags==CIUKI_POWER_OFF);
    struct ciuki_message bad=m; desk_put32(bad.data+8,CIUKI_POWER_QUERY); assert(!desk_parse(&bad,1,NULL,&packet));
    bad=m; bad.length=8; assert(!desk_parse(&bad,1,NULL,&packet));
    desk_receive(&d,i,&m); assert(terminal_result(i)==-EPERM && !d.power.state);
    d.clients[i].program=DESK_APP_TERMINAL; d.power.state=DESK_POWER_BUSY;
    desk_receive(&d,i,&m); assert(terminal_result(i)==-EBUSY); d.power.state=0;
    /* Launch authority comes from the program identity; a title is insufficient. */
    d.launch=terminal_launch_test; ide_launch_count=0;
    request(&m,DESK_LAUNCH,20); m.data[0]=DESK_SHELL_VERSION; desk_put32(m.data+8,8); memcpy(m.data+12,"ciuknote",8);
    desk_receive(&d,i,&m); assert(terminal_result(i)==123 && ide_launch_count==1);
    request(&m,DESK_LAUNCH,15); m.data[0]=DESK_SHELL_VERSION; desk_put32(m.data+8,3); memcpy(m.data+12,"lua",3);
    desk_receive(&d,i,&m); assert(terminal_result(i)==-EPERM && ide_launch_count==1);
    d.clients[i].program=0; strcpy(d.clients[i].title,"Terminal");
    request(&m,DESK_LAUNCH,20); m.data[0]=DESK_SHELL_VERSION; desk_put32(m.data+8,8); memcpy(m.data+12,"ciuknote",8);
    desk_receive(&d,i,&m); assert(terminal_result(i)==-EPERM);
    d.clients[i].program=DESK_APP_TERMINAL; d.terminate=terminal_kill; d.self_pid=77;
    request(&m,DESK_END_TASK,16); m.data[0]=DESK_SHELL_VERSION; desk_put32(m.data+12,1);
    desk_receive(&d,i,&m); assert(terminal_result(i)==-EPERM && !terminal_kill_pid);
    desk_put32(m.data+12,77); desk_receive(&d,i,&m); assert(terminal_result(i)==-EPERM && !terminal_kill_pid);
    desk_put32(m.data+12,99); desk_receive(&d,i,&m); assert(terminal_result(i)==0 && terminal_kill_pid==99);
    d.clients[i].mixer=1; d.mixer_set=terminal_mixer;
    request(&m,DESK_MIXER_SET,20); m.data[0]=DESK_SHELL_VERSION; desk_put32(m.data+8,20); desk_put32(m.data+12,30); desk_put32(m.data+16,1);
    desk_receive(&d,i,&m); assert(terminal_result(i)==0 && terminal_mix_count==1);
    d.clients[i].mixer=0; desk_receive(&d,i,&m); assert(terminal_result(i)==-EPERM && terminal_mix_count==1);
    request(&m,DESK_TERMINAL_POWER,12); m.data[0]=DESK_SHELL_VERSION; desk_put32(m.data+8,CIUKI_POWER_REBOOT);
    desk_receive(&d,i,&m); assert(terminal_result(i)==0 && d.power.state==DESK_POWER_CLOSING && d.power.action==CIUKI_POWER_REBOOT);
    d.power.state=0; cleanup(); puts("PASS Terminal: menu, launch authorization, protected PIDs, mixer, protocol, desktop power path");
}
static void test_volume_steps(void)
{
    setup(640,480);desk_shell_enable(&d);d.sh.mixer_state=2;d.sh.volume_open=1;d.sh.volume=50;
    assert(desk_shell_key(&d,DU_KEY_UP,1) && d.sh.volume==51 && d.sh.want_mixer);
    d.sh.want_mixer=0;assert(desk_shell_key(&d,DU_KEY_UP,0) && d.sh.volume==51 && !d.sh.want_mixer);
    assert(desk_shell_key(&d,DU_KEY_DOWN,2) && d.sh.volume==50 && d.sh.want_mixer);
    d.sh.volume_open=0;d.sh.want_mixer=0;
    assert(desk_shell_key(&d,0x130,1) && d.sh.volume==51 && d.sh.want_mixer);
    assert(desk_shell_key(&d,0x12e,1) && d.sh.volume==50);
    assert(desk_shell_key(&d,0x120,1) && d.sh.muted);
    d.sh.want_mixer=0;assert(desk_shell_key(&d,0x120,2) && d.sh.muted && !d.sh.want_mixer);
    d.sh.volume=100;d.sh.want_mixer=0;assert(desk_shell_key(&d,0x130,1) && !d.sh.want_mixer);
    d.sh.volume=0;assert(desk_shell_key(&d,0x12e,1) && !d.sh.want_mixer);
    d.sh.volume_open=1;d.sh.volume=50;
    /* The popup follows the volume icon, including the measured clock width. */
    struct desk_box icon=desk_shell_tray_icon_box(&d,SH_TRAY_VOLUME), tray=desk_shell_tray_box(&d);
    int track_top=tray.y-SH_TRAY_GAP-168+30;
    d.mouse_x=icon.x+icon.w-92+46;d.mouse_y=track_top+20;
    assert(desk_shell_button(&d,CIUKI_BUTTON_LEFT,1));
    assert(d.sh.volume_drag && d.sh.volume==80 && d.sh.want_mixer);
    d.sh.want_mixer=0;d.mouse_y=track_top+30;desk_shell_motion(&d);assert(d.sh.volume==70 && d.sh.want_mixer);
    d.sh.want_mixer=0;desk_shell_motion(&d);assert(!d.sh.want_mixer);
    desk_shell_button(&d,CIUKI_BUTTON_LEFT,0);assert(!d.sh.volume_drag && !d.sh.want_mixer);
    cleanup();puts("PASS UI volume: tray steps during drag, unchanged/release silence, popup/extended keyboard, repeat and bounds");
}
static void test_dos_fullscreen_menu(void)
{
    setup(1024,768); desk_shell_enable(&d);
    int a=client(10,11,640,480,0x123456); d.clients[a].dosbox=1;
    fake_block=1; d.sh.menu_owner=a; desk_menu_open(&d,SH_MENU_TASK,20,700);
    int full=-1;
    for (int i=0;i<d.sh.menus[0].count;i++) if (d.sh.menus[0].items[i].action==SH_ACT_DOS_FULLSCREEN) full=i;
    assert(full>=0); d.sh.menus[0].hot=full;
    assert(desk_shell_key(&d,DU_KEY_ENTER,1));
    assert(d.clients[a].window && !d.clients[a].minimized);
    int sent=0;
    for (unsigned i=0;i<d.clients[a].tx_count;i++) {
        struct ciuki_message *m=&d.clients[a].tx[(d.clients[a].tx_head+i)%DESK_TX_MAX];
        if (m->data[1]==DESK_DOS_FULLSCREEN) {
            struct desk_packet packet; assert(desk_parse(m,0,NULL,&packet));
            assert(!desk_parse(m,1,NULL,&packet)); sent++;
        }
    }
    assert(sent==1);
    event(CIUKI_INPUT_KEY,0x38,1,0); event(CIUKI_INPUT_KEY,0x1D,1,0);
    event(CIUKI_INPUT_RESYNC,0,0,0); assert(!d.sh.modifiers);
    fake_block=0; cleanup();
    puts("PASS DOS full-screen menu: launch-bound command, preserved window/taskbar, direction validation, modifier resync");
}
/* f2-66: leaving a DOS full-screen session restores the desktop mode and sends exactly one RESYNC
 * (src/kernel/vm/dosvm_fullscreen.c dosvm_fullscreen_desktop_event). The display then refuses the next
 * mode change (awaiting_frame, -EBUSY) until a present whose surface and rectangle cover the whole display
 * (src/kernel/proc/grants.c full_frame). The desktop renders only when damaged (desk_frame_due), so the
 * resync must damage everything: on image 94542c30 nothing was presented and the second entry of
 * vm-fullscreen-qemu-t23 was refused reason=busy awaiting_frame=1. */
static int full_frame_presents(void)
{
    int full=0;
    for (unsigned i=0;i<fake_presents;i++) {
        const struct ciuki_rect *r=&fake_rects[i];
        full+=fake_present_surfaces[i]==d.output && !r->src_x && !r->src_y && !r->dst_x && !r->dst_y &&
              r->width==d.width && r->height==d.height;
    }
    return full;
}
static void test_dos_fullscreen_restore_frame(void)
{
    const uint64_t later=UINT64_MAX/2;   /* past any frame deadline: only damage decides */
    setup(1024,768); desk_shell_enable(&d);
    uint32_t *pixels=calloc((size_t)1024*768,4); assert(pixels); d.pixels=pixels;   /* the output surface's mapping */
    int a=client(10,11,640,480,0x123456); d.clients[a].dosbox=1;
    assert(d.focus==a); flush_all();
    assert(!desk_redraw(&d)); fake_presents=0;
    /* Settled (f2-65): no damage, no frame due, nothing presented; a key for the DOS window changes no pixel. */
    assert(!desk_frame_due(&d,later) && !desk_redraw(&d) && fake_presents==0);
    event(CIUKI_INPUT_KEY,0x1e,1,0); event(CIUKI_INPUT_KEY,0x1e,0,0); flush_all();
    assert(!desk_frame_due(&d,later) && !desk_redraw(&d) && fake_presents==0);
    /* The restore's resync: the next frame is due and is one complete frame of the output surface. */
    event(CIUKI_INPUT_RESYNC,0,0,0); flush_all();
    if (!desk_frame_due(&d,later)) {
        fprintf(stderr,"resync: no frame due (damage=%u present_damage=%u cursor_dirty=%d)\n",
                d.damage.count,d.present_damage.count,d.cursor_dirty);
        assert(!"a resync must make a frame due");
    }
    assert(!desk_redraw(&d) && fake_presents==1 && full_frame_presents()==1);
    /* Then settled again. */
    fake_presents=0;
    assert(!desk_frame_due(&d,later) && !desk_redraw(&d) && fake_presents==0);
    cleanup(); free(pixels);
    puts("PASS DOS full-screen restore: settled desktop presents nothing, RESYNC presents one full frame");
}
int main(void)
{
    test_protocol(); test_titles(); test_cursor_requests(); test_resize_protocol(); test_damage();
    test_resize(); test_resize_title_buttons(); test_input(); test_lifecycle();
    test_compositor(); test_shell(); test_media_shell(); test_graphics_menu();
    test_graphics_service(); test_menu(); test_ide_launch(); test_terminal();
    test_volume_steps(); test_dos_fullscreen_menu(); test_dos_fullscreen_restore_frame();
    fake_reset(); puts("PASS desktop host model (guest qualification not_run)"); return 0;
}
