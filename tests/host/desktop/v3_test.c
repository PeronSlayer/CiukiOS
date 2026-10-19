/* SPDX-License-Identifier: MIT
 * Production protocol/server with fake kernel surfaces. No guest evidence. */
#include "fake.h"
#include "../../../apps/libdeskui/v3.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/w32_diagnostics.h>
static struct desktop d;
static uint32_t token;
static uint32_t portrait[256*256];
static void msg(struct ciuki_message *m, unsigned op, unsigned n, unsigned id, unsigned gen)
{ desk_v3_message(m,op,n,++token,id,gen); m->sender_pid=42; }
static void check_packet(struct ciuki_message *m, int direction, const struct ciuki_surface_info *s)
{
    struct desk_packet p; if (!desk_parse(m,direction,s,&p)) fprintf(stderr,"bad fixture opcode=%u length=%u dir=%d\n",m->data[1],m->length,direction); assert(desk_parse(m,direction,s,&p));
    struct ciuki_message bad=*m;
    bad.length--; assert(!desk_parse(&bad,direction,s,&p));
    bad=*m; bad.length++; assert(!desk_parse(&bad,direction,s,&p));
    bad=*m; bad.data[2]=1; assert(!desk_parse(&bad,direction,s,&p));
    bad=*m; bad.data[3]=1; assert(!desk_parse(&bad,direction,s,&p));
    bad=*m; bad.reserved=1; assert(!desk_parse(&bad,direction,s,&p));
    bad=*m; bad.data[255]=1; assert(!desk_parse(&bad,direction,s,&p));
    bad=*m; bad.fd_count++; assert(!desk_parse(&bad,direction,s,&p));
    bad=*m; bad.fds[3]=99; assert(!desk_parse(&bad,direction,s,&p));
    bad=*m; bad.data[1]=255; assert(!desk_parse(&bad,direction,s,&p));
    if (m->data[1]!=DESK_PING && m->data[1]!=DESK_PONG && m->data[1]!=DESK_V3_ACTIVATE) assert(!desk_parse(m,!direction,s,&p));
}
static void protocol(void)
{
    struct ciuki_surface_info s={24,32,32,128,1,4096}; struct ciuki_message m;
    desk_v3_message(&m,DESK_HELLO,0,0,0,0); check_packet(&m,1,&s);
    const unsigned ops[]={DESK_CREATE_WINDOW,DESK_DAMAGE,DESK_V3_RESIZE,DESK_V3_PLACE,DESK_SET_TITLE,
        DESK_SET_CURSOR,DESK_V3_CURSOR_BITMAP,DESK_V3_POINTER,DESK_V3_WARP,DESK_V3_LEASE,
        DESK_V3_CRASH,DESK_V3_RESTART,DESK_CLOSE,DESK_CLOSED,DESK_CLOSE_REQUEST,DESK_PING,DESK_PONG,
        DESK_CONFIGURE,DESK_V3_RESULT,DESK_V3_ACTIVATE,DESK_V3_LEASE_EVENT,DESK_KEY,DESK_MOTION,DESK_BUTTON,DESK_V3_RELATIVE_EVENT,DESK_V3_POINTER_STATE,DESK_CLOSE_ACK};
    for (unsigned k=0;k<sizeof(ops)/sizeof(*ops);k++) {
        unsigned op=ops[k], n=0; int direction=1;
        msg(&m,op,0,1,1); uint8_t *b=m.data+16;
        switch (op) {
        case DESK_CREATE_WINDOW: n=37; desk_put32(b+20,32); desk_put32(b+24,32); desk_put32(b+28,1); desk_put32(b+32,1); b[36]='W'; m.fd_count=1; m.fds[0]=9; break;
        case DESK_V3_PLACE: n=28; desk_put32(b+8,32); desk_put32(b+12,32); desk_put32(b+16,1); break;
        case DESK_V3_RESIZE: n=12; desk_put32(b,32); desk_put32(b+4,32); desk_put32(b+8,1); m.fd_count=1; m.fds[0]=9; break;
        case DESK_DAMAGE: n=16; desk_put32(b+8,32); desk_put32(b+12,32); break;
        case DESK_SET_TITLE: n=5; desk_put32(b,1); b[4]='W'; break;
        case DESK_SET_CURSOR: n=12; break;
        case DESK_V3_CURSOR_BITMAP: n=12; desk_put32(b,1); m.fd_count=1; m.fds[0]=9; break;
        case DESK_V3_POINTER: n=20; break;
        case DESK_V3_WARP: n=8; break;
        case DESK_V3_LEASE: n=16; break;
        case DESK_V3_CRASH: n=13; desk_put32(b,0xc0000005); desk_put32(b+8,1); b[12]='M'; break;
        case DESK_V3_RESTART: case DESK_CLOSE_ACK: n=4; break;
        case DESK_V3_POINTER_STATE: n=4; direction=0; break;
        case DESK_CONFIGURE: n=32; direction=0; desk_put32(b,32); desk_put32(b+4,32); desk_put32(b+8,1); desk_put32(b+12,1); break;
        case DESK_V3_RESULT: n=8; direction=0; desk_put32(b,DESK_DAMAGE); break;
        case DESK_V3_ACTIVATE: n=8; direction=0; break;
        case DESK_V3_LEASE_EVENT: n=20; direction=0; desk_put32(b+4,1); desk_put32(b+8,320); desk_put32(b+12,200); desk_put32(b+16,8); break;
        case DESK_PING: case DESK_PONG: n=4; break;
        case DESK_CLOSED: case DESK_CLOSE_REQUEST: direction=0; break;
        case DESK_KEY: case DESK_MOTION: case DESK_BUTTON: case DESK_V3_RELATIVE_EVENT:
            n=40; direction=0;
            desk_put32(b+12,op==DESK_KEY ? CIUKI_INPUT_KEY : op==DESK_BUTTON ? CIUKI_INPUT_BUTTON : CIUKI_INPUT_MOTION);
            if (op==DESK_BUTTON) desk_put32(b+24,1);
            break;
        default: break;
        }
        m.length=16+n; check_packet(&m,direction,&s);
    }
    const uint8_t good[]={0xe7,0x8a,0xac,0x20,0xc3,0xa8}; assert(desk_utf8_valid(good,sizeof(good)));
    const uint8_t bad[][4]={{0xc0,0x80,0,0},{0xed,0xa0,0x80,0},{0xf4,0x90,0x80,0x80},{0x80,0,0,0}};
    assert(!desk_utf8_valid(bad[0],2)); assert(!desk_utf8_valid(bad[1],3)); assert(!desk_utf8_valid(bad[2],4)); assert(!desk_utf8_valid(bad[3],1));
    assert(!desk_utf8_valid(good,2));
    msg(&m,DESK_SET_TITLE,4,1,1); struct desk_packet p; assert(desk_parse(&m,1,&s,&p) && !p.title[0]);
    msg(&m,DESK_CREATE_WINDOW,36,1,1); m.fd_count=1; m.fds[0]=9;
    desk_put32(m.data+36,32); desk_put32(m.data+40,32); desk_put32(m.data+44,1);
    assert(desk_parse(&m,1,&s,&p) && !p.title[0]);
    puts("PASS every v3 message: direction, length, reserved bytes, descriptors, UTF-8");
}
static int peer(int fd, unsigned pid)
{
    fake_channel(fd); int root=desk_add_client(&d,fd); assert(root>=0);
    struct ciuki_message m; desk_v3_message(&m,DESK_HELLO,0,0,0,0); m.sender_pid=pid;
    assert(!desk_v3_receive(&d,root,&m)); return root;
}
static int create(int root, int fd, unsigned id, unsigned generation, unsigned style, unsigned owner)
{
    fake_surface(fd,200,100,0x7498b0);
    struct ciuki_message m; msg(&m,DESK_CREATE_WINDOW,40,id,generation); m.sender_pid=d.clients[root].pid;
    m.fd_count=1; m.fds[0]=fd;
    desk_put32(m.data+16,style); desk_put32(m.data+20,owner); desk_put32(m.data+24,owner ? 1 : 0);
    desk_put32(m.data+28,80+id*20); desk_put32(m.data+32,100+id*24);
    desk_put32(m.data+36,200); desk_put32(m.data+40,100); desk_put32(m.data+44,1); desk_put32(m.data+48,4); memcpy(m.data+52,"Test",4);
    assert(!desk_v3_receive(&d,root,&m));
    for (int i=0;i<DESK_CLIENTS;i++) if (d.clients[i].v3_peer==(unsigned)root+1 && d.clients[i].v3_id==id) return i;
    assert(0); return -1;
}
static void setup(void)
{
    fake_reset(); desk_init(&d,&fake_ops,800,600); d.portrait=portrait;
    fake_surface(1,800,600,0); d.output=1; d.pixels=fake_ops.map(1,PROT_READ); assert(d.pixels);
    desk_shell_enable(&d); token=0;
}
static void cleanup(void)
{
    for (int i=0;i<DESK_CLIENTS;i++) desk_drop_client(&d,i);
    desk_render_release(&d); fake_ops.unmap(d.pixels,(800*600*4+4095)&~4095u); fake_ops.close(1);
    free(d.sh.icons); free(d.sh.wall); free(d.sh.background); free(d.v3_links);
    assert(!fake_live());
}
static void flush(void)
{ for (int i=0;i<DESK_CLIENTS;i++) desk_flush(&d,i); }
static void key(int code, int value)
{ struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=code,.value=value}; desk_input(&d,&e); }
static unsigned leases, revokes, restart_count, terminated;
static int lease_error;
static int lease(struct desktop *p,uint32_t pid,int acquire,uint32_t w,uint32_t h,uint32_t bpp,uint32_t gen)
{ assert(p==&d && pid==42 && w==320 && h==200 && bpp==8 && gen); if (lease_error) return lease_error; if (acquire) leases++; else revokes++; return 0; }
static int restart(struct desktop *p,uint32_t pid,uint32_t flags)
{ assert(p==&d && pid==42 && flags==2); restart_count++; return 0; }
static int terminate(uint32_t pid,int signal)
{ assert(pid==42 && signal==SIGKILL); terminated++; return 0; }
static void request_lease(int root,unsigned id)
{
    struct ciuki_message m; msg(&m,DESK_V3_LEASE,16,id,1);
    desk_put32(m.data+16,1); desk_put32(m.data+20,320); desk_put32(m.data+24,200); desk_put32(m.data+28,8);
    assert(!desk_v3_receive(&d,root,&m));
}
static void ownership(void)
{
    setup(); int root=peer(10,42), a=create(root,20,1,1,0,0), b=create(root,21,2,1,DESK_V3_POPUP,0);
    flush(); assert(d.clients[a].channel==d.clients[b].channel && fake_closed==0);
    struct ciuki_message m; msg(&m,DESK_CLOSE,0,1,1); assert(!desk_v3_receive(&d,root,&m)); flush();
    assert(!d.clients[a].window && d.clients[b].window && d.clients[root].channel==10);
    fake_surface(22,200,100,0); msg(&m,DESK_CREATE_WINDOW,40,1,1); m.fd_count=1; m.fds[0]=22;
    desk_put32(m.data+36,200); desk_put32(m.data+40,100); desk_put32(m.data+44,1); desk_put32(m.data+48,4); memcpy(m.data+52,"Test",4);
    assert(desk_v3_receive(&d,root,&m)==-ENOENT); flush();
    a=create(root,23,1,2,DESK_V3_TOPMOST,0); assert(d.clients[a].order>d.clients[b].order);
    desk_focus(&d,b); assert(d.clients[a].order>d.clients[b].order);
    int other=peer(11,84); msg(&m,DESK_CLOSE,0,2,1); m.sender_pid=84;
    assert(desk_v3_receive(&d,other,&m)==-ENOENT); assert(d.clients[b].window);
    msg(&m,DESK_DAMAGE,16,2,1); m.sender_pid=84;
    assert(desk_v3_receive(&d,root,&m)==-EINVAL); assert(d.clients[root].channel<0 && !d.clients[b].window);
    cleanup();
    setup(); root=peer(10,42); a=create(root,20,1,1,0,0); b=create(root,21,2,1,DESK_V3_TOOL|DESK_V3_MODAL,1);
    assert(desk_modal_target(&d,a)==b); desk_drop_client(&d,a); assert(!d.clients[b].window); cleanup();
    setup(); root=peer(10,42); a=create(root,20,1,1,0,0); flush();
    fake_block=1;
    for (unsigned k=0;k<DESK_TX_MAX+1;k++) { msg(&m,DESK_PING,4,1,1); desk_put32(m.data+16,k); (void)desk_v3_receive(&d,root,&m); }
    assert(d.clients[root].v3_failed); desk_flush(&d,root); fake_block=0; assert(!d.clients[a].window); cleanup();
    puts("PASS v3 multiwindow: ownership, generations, topmost, modal owner death, transport loss and backpressure");
}
static void input_and_lease(void)
{
    setup(); int root=peer(10,42), a=create(root,20,1,1,0,0), b=create(root,21,2,1,0,0); desk_focus(&d,a); flush();
    struct ciuki_message m; msg(&m,DESK_V3_POINTER,20,1,1);
    desk_put32(m.data+16,DESK_V3_CAPTURE|DESK_V3_CLIP|DESK_V3_RELATIVE|DESK_V3_HIDDEN);
    desk_put32(m.data+28,30); desk_put32(m.data+32,20); assert(!desk_v3_receive(&d,root,&m));
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=123,.value2=-41,.sequence=88,.source=1};
    desk_input(&d,&e); flush(); int relative=0;
    for (unsigned k=0;k<fake_sent;k++) if (fake_messages[k].data[1]==DESK_V3_RELATIVE_EVENT) {
        struct desk_packet p; assert(desk_parse(&fake_messages[k],0,NULL,&p)); assert(p.event.value==123 && p.event.value2==-41 && p.event.sequence==88); relative++;
    }
    assert(relative==1 && d.v3_cursor_hidden);
    struct desk_box clip=d.clients[a].v3_clip; assert(desk_contains(clip,d.mouse_x,d.mouse_y));
    msg(&m,DESK_V3_WARP,8,1,1); desk_put32(m.data+16,9999); assert(!desk_v3_receive(&d,root,&m)); assert(d.mouse_x==clip.x+clip.w-1);
    unsigned before=fake_sent; desk_focus(&d,b); flush(); assert(!d.v3_pointer_window); assert(fake_sent>before);
    d.v3_lease=lease; desk_focus(&d,a); request_lease(root,1); assert(d.v3_lease_state==1);
    unsigned presents=fake_presents; assert(!desk_redraw(&d) && fake_presents==presents);
    key(0x38,1); key(0x0f,1); assert(!d.v3_lease_state && revokes==1); key(0x0f,0); key(0x38,0); flush();
    before=fake_sent; desk_focus(&d,a); assert(d.v3_lease_state==1 && leases==2); flush();
    int lease_at=-1, active_at=-1;
    for (unsigned k=before;k<fake_sent;k++) {
        if (fake_messages[k].data[1]==DESK_V3_LEASE_EVENT && desk_get32(fake_messages[k].data+16)==1) lease_at=(int)k;
        if (fake_messages[k].data[1]==DESK_V3_ACTIVATE && desk_get32(fake_messages[k].data+16)==1) active_at=(int)k;
    }
    assert(lease_at>=0 && active_at>lease_at);
    key(0x38,1); key(0x1c,1); key(0x1c,0); key(0x38,0); assert(!d.v3_lease_state); flush();
    /* Reacquire, then Ctrl+Alt+Del must revoke even when tasks is absent. */
    d.clients[a].v3_lease_wanted=0; request_lease(root,1);
    key(0x1d,1); key(0x38,1); key(0x153,1); key(0x153,0); key(0x38,0); key(0x1d,0); assert(!d.v3_lease_state);
    flush(); d.clients[a].v3_lease_wanted=0;
    cleanup();
    setup(); root=peer(10,42); a=create(root,20,1,1,0,0); d.v3_lease=lease; request_lease(root,1);
    lease_error=-EBUSY; desk_v3_revoke(&d); assert(d.v3_lease_state==2 && !desk_redraw(&d));
    lease_error=0; desk_v3_revoke(&d); assert(!d.v3_lease_state); cleanup();
    struct ciuki_display_info info={.mode_count=3,.modes={{0x111,640,480,16,1280,1,0},{0x112,640,400,32,2560,1,0},{0x113,320,200,8,320,1,0}}};
    assert(desk_v3_mode(&info,320,200,8)==0x113); info.mode_count=2; assert(desk_v3_mode(&info,320,200,8)==0x112);
    assert(desk_v3_mode(&info,4096,200,8)==-EINVAL);
    puts("PASS v3 raw deltas, scaled clipping/warp, focus release, hotkeys, lease revocation/regrant/failure rollback");
}
static void links(void)
{
    struct desk_launch_record r={.label="Game",.target="C:\\Games\\A game.exe",.arguments="-data \"A B\"",.directory="C:\\Games",.icon="C:\\Games\\icon.ico",.show=3,.icon_index=-2}, out;
    uint8_t bytes[DESK_LINK_BYTES]; unsigned n=desk_link_encode(&r,bytes,sizeof(bytes)); assert(n && desk_link_decode(bytes,n,&out)); assert(!memcmp(&r,&out,sizeof(r)));
    assert(!desk_link_decode(bytes,n-1,&out)); bytes[36]=1; assert(!desk_link_decode(bytes,n,&out)); bytes[36]=0; desk_put32(bytes+16,0xffffffff); assert(!desk_link_decode(bytes,n,&out));
    assert(desk_uninstall_command("\"C:\\Program Files\\Game\\uninstall.exe\" /remove \"A B\"",&out));
    assert(!strcmp(out.target,"C:\\Program Files\\Game\\uninstall.exe") && !strcmp(out.arguments,"/remove \"A B\""));
    assert(!desk_uninstall_command("\"C:\\bad.exe /remove",&out)); assert(!desk_uninstall_command("uninstall.exe",&out)); assert(!desk_uninstall_command("C:\\Program Files\\uninstall.exe",&out));
    puts("PASS counted links retain launch metadata; malformed links and ambiguous uninstall paths fail closed");
}
static unsigned link_stale, link_started;
static int list_links(struct desktop *p,struct desk_link_entry *out,unsigned cap)
{
    assert(p==&d && cap==DESK_LINKS);
    out[0]=(struct desk_link_entry){.launch={.label="Installed Game",.target="C:\\Games\\game.exe",.show=1},.source="/Windows/Desktop/Game.lnk",.desktop=1};
    memset(out[0].launch.arguments,'x',300); out[1]=out[0]; out[1].desktop=0; return 2;
}
static int read_link(struct desktop *p,const char *path,struct desk_launch_record *out)
{
    assert(p==&d && !strcmp(path,"/Windows/Desktop/Game.lnk"));
    if (link_stale) return -ENOENT;
    *out=d.v3_links[0].launch; return 0;
}
static int launch_link(struct desktop *p,const struct desk_launch_record *r)
{ assert(p==&d && strlen(r->arguments)==300); link_started++; return 91; }
static void shell_links(void)
{
    setup(); d.v3_links_list=list_links; d.v3_link_read=read_link; d.v3_launch=launch_link;
    desk_links_refresh(&d); assert(d.v3_link_count==2 && d.sh.icons[d.sh.icon_count-1].action==SH_ACT_WIN32_LINK);
    assert(desk_link_launch(&d,0)==91 && link_started==1); link_stale=1;
    assert(desk_link_launch(&d,0)==-ENOENT && link_started==1);
    link_stale=0; desk_internal_open(&d,DESK_INTERNAL_PROGRAMS);
    key(0x1c,1); key(0x1c,0); assert(link_started==2);
    cleanup(); puts("PASS shell link discovery, desktop icon, long launch arguments, stale source re-read");
}
static void resize_and_cursor(void)
{
    setup(); int root=peer(10,42), a=create(root,20,1,1,0,0); flush();
    struct ciuki_message m; msg(&m,DESK_V3_PLACE,28,1,1);
    desk_put32(m.data+16,200); desk_put32(m.data+20,120); desk_put32(m.data+24,240); desk_put32(m.data+28,120); desk_put32(m.data+32,2);
    assert(!desk_v3_receive(&d,root,&m)); flush(); assert(d.clients[a].info.width==200 && desk_content(&d.clients[a]).w==480);
    assert(desk_redraw(&d)>=0);
    uint32_t *back=(uint32_t *)d.clients[a].pixels;
    for (unsigned y=20;y<30;y++) for (unsigned x=100;x<110;x++) back[y*200+x]=0xff00aa;
    msg(&m,DESK_DAMAGE,16,1,1); desk_put32(m.data+16,100); desk_put32(m.data+20,20); desk_put32(m.data+24,10); desk_put32(m.data+28,10);
    assert(!desk_v3_receive(&d,root,&m)); d.now_ns+=DESK_FRAME_NS; assert(desk_redraw(&d)>=0);
    uint32_t *partial=malloc(800*600*4); assert(partial); memcpy(partial,d.pixels,800*600*4);
    desk_theme_changed(&d); d.now_ns+=DESK_FRAME_NS; assert(desk_redraw(&d)>=0); assert(!memcmp(partial,d.pixels,800*600*4)); free(partial);
    fake_surface(21,240,120,0x22aa33); msg(&m,DESK_V3_RESIZE,12,1,1); m.fd_count=1; m.fds[0]=21;
    desk_put32(m.data+16,240); desk_put32(m.data+20,120); desk_put32(m.data+24,d.clients[a].resize_serial);
    assert(!desk_v3_receive(&d,root,&m)); assert(d.clients[a].surface==21 && d.clients[a].info.width==240); flush();
    fake_surface(22,240,120,0); m.fds[0]=22; assert(desk_v3_receive(&d,root,&m)==-ENOENT); flush();
    fake_surface(23,32,32,0xffef5720); msg(&m,DESK_V3_CURSOR_BITMAP,12,1,1); m.fd_count=1; m.fds[0]=23;
    desk_put32(m.data+16,1); desk_put32(m.data+20,4); desk_put32(m.data+24,7); assert(!desk_v3_receive(&d,root,&m));
    msg(&m,DESK_SET_CURSOR,12,1,1); desk_put32(m.data+20,1); assert(!desk_v3_receive(&d,root,&m));
    struct desk_box b=desk_content(&d.clients[a]); d.mouse_x=b.x+40; d.mouse_y=b.y+20; desk_cursor_update(&d);
    assert(d.v3_cursor_id==1 && d.clients[a].v3_hot_x[0]==4 && d.clients[a].v3_hot_y[0]==7);
    msg(&m,DESK_DAMAGE,16,1,1); desk_put32(m.data+24,240); desk_put32(m.data+28,120); assert(!desk_v3_receive(&d,root,&m));
    flush(); cleanup(); puts("PASS scaled placement, atomic resize acknowledgement, stale resize, custom cursor cache");
}
static struct ciuki_message bridge_wire;
static unsigned bridge_sends;
static int bridge_block;
static int bridge_send(int fd,const struct ciuki_message *m,uint32_t flags)
{ assert(fd==10 && flags==DONTWAIT); if (bridge_block) { errno=EAGAIN; return -1; } bridge_wire=*m; bridge_sends++; return 0; }
static int bridge_recv(int fd,struct ciuki_message *m,uint32_t flags)
{ assert(fd==10 && flags==DONTWAIT); *m=bridge_wire; return 1; }
static void bridge(void)
{
    struct du3_connection c; struct du3_transport ops={bridge_send,bridge_recv,fake_ops.info,fake_ops.close};
    assert(!du3_connect(&c,10,&ops)); assert(bridge_sends==1 && bridge_wire.data[1]==DESK_HELLO);
    uint8_t title[8]={0}; desk_put32(title,4); memcpy(title+4,"Test",4);
    assert(du3_request(&c,DESK_SET_TITLE,1,1,title,8,-1)==1);
    bridge_block=1; assert(!du3_request(&c,DESK_CLOSE,1,1,NULL,0,-1) && errno==EAGAIN && c.request==1); bridge_block=0;
    unsigned before=bridge_sends;
    desk_v3_message(&bridge_wire,DESK_PING,4,0,1,1); desk_put32(bridge_wire.data+16,99);
    struct desk_packet p; assert(du3_next(&c,&p)==1 && p.serial==99 && bridge_sends==before);
    assert(!du3_ui_progress(&c,1,1,99)); assert(bridge_wire.data[1]==DESK_PONG);
    bridge_wire.data[255]=1; assert(du3_next(&c,&p)<0 && c.disconnected);
    puts("PASS user-mode bridge tokens, backpressure, malformed server, explicit guest UI PONG");
}
static unsigned ended;
static int end_process(uint32_t pid,int sig) { assert(pid==42 && sig==SIGKILL); ended++; return 0; }
static void close_and_native_tasks(void)
{
    setup(); int root=peer(10,42), a=create(root,20,1,1,0,0), b=create(root,21,2,1,0,0); flush();
    d.children[0].pid=42; desk_request_close(&d,a); assert(d.clients[a].closing && !d.children[0].close_ns);
    struct ciuki_message m; msg(&m,DESK_CLOSE_ACK,4,1,1); assert(!desk_v3_receive(&d,root,&m)); assert(!d.clients[a].closing && d.clients[b].window);
    msg(&m,DESK_SET_TITLE,7,1,1); desk_put32(m.data+16,3); memcpy(m.data+20,"A\xc3\xa8",3); assert(!desk_v3_receive(&d,root,&m));
    fake_channel(11); int tasks=desk_add_client(&d,11); assert(tasks>=0); d.clients[tasks].version=2; d.clients[tasks].hello=1; d.clients[tasks].pid=84;
    desk_task_windows(&d,tasks,123); flush(); int found=0;
    for (unsigned k=0;k<fake_sent;k++) if (fake_messages[k].data[1]==DESK_WINDOW_INFO) {
        struct desk_packet p; assert(desk_parse(&fake_messages[k],0,NULL,&p)); if (!strcmp(p.title,"A?")) found++;
    }
    assert(found==1); d.terminate=end_process; assert(!desk_task_end(&d,d.clients[a].window_id,42) && ended==1);
    msg(&m,DESK_V3_LEASE,16,1,1); assert(!desk_v3_receive(&d,root,&m)); flush(); cleanup();
    puts("PASS window Close/decline without process timer, native ASCII task view, End Task independent of guest UI, idempotent lease release");
}
static void frame(const char *folder,const char *theme,const char *name)
{
    desk_theme_use(theme); d.now_ns+=DESK_FRAME_NS; desk_theme_changed(&d); desk_redraw(&d);
    char path[1024]; snprintf(path,sizeof(path),"%s/%s-%s.xrgb",folder,theme,name);
    FILE *f=fopen(path,"wb"); assert(f && fwrite(d.pixels,4,800*600,f)==800*600); fclose(f);
}
static int task_launch_record(struct desktop *p,const struct desk_program *program)
{ assert(p==&d && !strcmp(program->name,"tasks")); return 90; }
static void frames(const char *folder)
{
    const char *themes[]={"ciuki-blue","platinum"};
    for (unsigned t=0;t<2;t++) {
        const unsigned styles[]={0,DESK_V3_POPUP,DESK_V3_TOOL,DESK_V3_TOPMOST};
        const char *names[]={"overlapped","popup","tool","topmost"};
        for (unsigned k=0;k<4;k++) {
            setup(); int root=peer(10,42); int a=create(root,20,1,1,styles[k],0);
            if (k==3) { int b=create(root,21,2,1,0,0); desk_focus(&d,b); assert(d.clients[a].order>d.clients[b].order); }
            flush(); frame(folder,themes[t],names[k]); cleanup();
        }
        setup(); d.v3_links_list=list_links; d.v3_link_read=read_link; d.v3_launch=launch_link;
        desk_links_refresh(&d); desk_internal_open(&d,DESK_INTERNAL_PROGRAMS); frame(folder,themes[t],"programs-folder"); cleanup();
        setup(); int root=peer(10,42); create(root,20,1,1,0,0); create(root,21,2,1,0,0); flush();
        key(0x38,1); key(0x0f,1); frame(folder,themes[t],"alt-tab"); key(0x0f,0); key(0x38,0); flush();
        frame(folder,themes[t],"alt-tab-committed");
        d.v3_lease=lease; desk_focus(&d,2); request_lease(root,2); flush();
        key(0x38,1); key(0x1c,1); key(0x1c,0); key(0x38,0); assert(!d.v3_lease_state);
        frame(folder,themes[t],"alt-enter");
        request_lease(root,2); d.launch=task_launch_record; d.program_count=1; strcpy(d.programs[0].name,"tasks"); strcpy(d.programs[0].path,"/Programs/tasks");
        key(0x1d,1); key(0x38,1); key(0x153,1); key(0x153,0); key(0x38,0); key(0x1d,0); assert(!d.v3_lease_state);
        frame(folder,themes[t],"ctrl-alt-del"); d.clients[2].v3_lease_wanted=0;
        struct ciuki_message m; msg(&m,DESK_V3_CRASH,20,2,1); desk_put32(m.data+16,0xc0000005); desk_put32(m.data+20,0x401234); desk_put32(m.data+24,8); memcpy(m.data+28,"game.exe",8);
        d.terminate=terminate; assert(!desk_v3_receive(&d,root,&m)); frame(folder,themes[t],"crash");
        assert(strstr(d.sh.message_text,"game.exe has performed an illegal operation and will be shut down."));
        key(0x0f,1);key(0x0f,0);assert(d.v3_details_open);frame(folder,themes[t],"crash-details");
        assert(strstr(d.v3_details,"Exception: c0000005")&&strstr(d.v3_details,"????????"));
        key(0x1c,1); key(0x1c,0); assert(terminated==t+1);
        d.v3_restart=restart; msg(&m,DESK_V3_RESTART,4,2,1); desk_put32(m.data+16,2); assert(!desk_v3_receive(&d,root,&m)); frame(folder,themes[t],"restart");
        key(0x1c,1); key(0x1c,0); assert(restart_count==t+1); flush(); cleanup();
        for(unsigned reason=1;reason<=7;reason++){
            setup();struct ciuki_w32_fault fault={.kind=W32_REPORT_REFUSE,.code=reason,.pid=42};
            strcpy(fault.exe,"fixture.exe");strcpy(fault.module,"fixture.dll");desk_w32_fault_dialog(&d,&fault);
            assert(d.v3_dialog_kind==3);char name[32];snprintf(name,sizeof(name),"refuse-%u",reason);frame(folder,themes[t],name);
            key(0x1c,1);key(0x1c,0);assert(!d.v3_dialog_kind);flush();cleanup();
        }
    }
}
int main(int argc,char **argv)
{
    assert(argc==3); FILE *f=fopen(argv[1],"rb"); assert(f && fread(portrait,4,256*256,f)==256*256); fclose(f);
    protocol(); ownership(); input_and_lease(); resize_and_cursor(); bridge(); links(); shell_links(); close_and_native_tasks(); frames(argv[2]);
    puts("PASS desktop v3 integration (host only)"); return 0;
}
