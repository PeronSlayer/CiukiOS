/* SPDX-License-Identifier: MIT. Windows 9x DOS window Mark/Copy/Paste. */
#include "clipboard.h"
#include "autorun.h"
#include "clipboard_settings.h"
#include "../libdeskui/clipboard.h"
#include "../libdeskui/clipstore.h"
#include <ciuki/channel.h>
#include <ciuki/dosvm.h>
#include <ciuki/surface.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
static struct {
    int channel,marking,dragging,text_mode,quick_edit,exclusive,x0,y0,x1,y1,mouse_x,mouse_y,dirty;
    uint32_t id,generation,server,*pixels,*snapshot;
    unsigned columns,rows,cell_height;
    unsigned char *cells,*paste; unsigned paste_bytes,paste_at,key_at,key_count;
    struct dosbox_key keys[16];
    uint64_t next_key, settings_key;
    int settings_bound, guest_open;
    struct ciuki_message saved[1024]; unsigned head,count;
} state;
static uint64_t now(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static void command(struct ciuki_dosvm_command *c)
{ memset(c,0,sizeof(*c)); c->version=1; c->size=sizeof(*c); c->id=state.id; c->generation=state.generation; }
static int send(struct ciuki_message *m, uint64_t deadline)
{
    while (ciuki_channel_send(state.channel,m,DONTWAIT)) {
        if (errno!=EAGAIN && errno!=EINTR) return -errno;
        if (now()>=deadline) return -ETIMEDOUT;
        struct pollfd p={state.channel,POLLOUT,0}; poll(&p,1,10);
    }
    return 0;
}
static int rpc(void *ctx,struct desk_clip_wire *w,const void *data,struct desk_clip_wire *r,void **out)
{
    (void)ctx; int fd=-1;
    if (w->bytes) {
        unsigned width,height; desk_clip_dims(w->bytes,&width,&height);
        fd=ciuki_surface_create(width,height,CIUKI_SURFACE_XRGB8888); if (fd<0) return -errno;
        struct ciuki_surface_info info;
        if (ciuki_surface_info(fd,&info)) { int e=errno; close(fd); return -e; }
        void *map=ciuki_surface_map(fd,PROT_READ|PROT_WRITE);
        if (!map || map==MAP_FAILED) { close(fd); return -ENOMEM; }
        memcpy(map,data,w->bytes); munmap(map,info.allocation_bytes);
    }
    struct ciuki_message m; int e=desk_clip_message(&m,2,1,w,fd); uint64_t deadline=now()+3000000000ull;
    if (!e) e=send(&m,deadline);
    if (fd>=0) close(fd);
    if (e) return e;
    while (now()<deadline) {
        int n=ciuki_channel_recv(state.channel,&m,DONTWAIT);
        if (n<0 && (errno==EAGAIN || errno==EINTR)) { struct pollfd p={state.channel,POLLIN,0}; poll(&p,1,10); continue; }
        if (n!=1) return -EIO;
        struct desk_packet p;
        int valid=m.sender_pid && (!state.server || state.server==m.sender_pid) && desk_parse(&m,0,NULL,&p);
        if (!valid) { for (unsigned i=0;i<m.fd_count && i<4;i++) close(m.fds[i]); return -EIO; }
        if (p.opcode==DESK_CLIPBOARD_EVENT) {
            if (p.clip.kind==DESK_CLIP_REPLY && p.clip.serial==w->serial) {
                *r=p.clip; *out=NULL;
                if (m.fd_count) {
                    struct ciuki_surface_info info;
                    if (ciuki_surface_info(m.fds[0],&info) || !desk_surface_valid(&info) || p.clip.bytes>info.allocation_bytes) e=-EIO;
                    else {
                        void *map=ciuki_surface_map(m.fds[0],PROT_READ);
                        if (!map || map==MAP_FAILED) e=-ENOMEM;
                        else { *out=malloc(p.clip.bytes); if (!*out) e=-ENOMEM; else memcpy(*out,map,p.clip.bytes); munmap(map,info.allocation_bytes); }
                    }
                }
                for (unsigned i=0;i<m.fd_count;i++) close(m.fds[i]);
                return e ? e : r->result;
            }
            for (unsigned i=0;i<m.fd_count;i++) close(m.fds[i]);
        } else if (p.opcode==DESK_PING) {
            desk_message(&m,DESK_PONG,8); m.data[0]=2; desk_put32(m.data+4,p.serial); e=send(&m,deadline); if (e) return e;
        } else {
            if (state.count==1024) { for (unsigned i=0;i<m.fd_count;i++) close(m.fds[i]); return -EBUSY; }
            state.saved[(state.head+state.count++)%1024]=m;
        }
    }
    return -ETIMEDOUT;
}
void dos_clip_init(int channel,uint32_t *pixels)
{ memset(&state,0,sizeof(state)); state.channel=channel; state.pixels=pixels; du_clip_bind(rpc,NULL); }
void dos_clip_settings(const char *program)
{
    struct dosbox_clipboard_settings s;
    state.settings_key=dosbox_clipboard_key(program); state.settings_bound=1;
    dosbox_clipboard_load(state.settings_key,&s); state.quick_edit=s.quick_edit; state.exclusive=s.exclusive;
}
void dos_clip_vm(uint32_t id,uint32_t generation,uint32_t server)
{ state.id=id; state.generation=generation; state.server=server; }
int dos_clip_recv(struct ciuki_message *m)
{ if (state.count) { *m=state.saved[state.head]; state.head=(state.head+1)%1024; state.count--; return 1; } return ciuki_channel_recv(state.channel,m,DONTWAIT); }
static void resume(void)
{
    if (state.snapshot) memcpy(state.pixels,state.snapshot,CIUKI_DOSVM_WIDTH*CIUKI_DOSVM_HEIGHT*4);
    free(state.snapshot); state.snapshot=NULL; free(state.cells); state.cells=NULL;
    struct ciuki_dosvm_command c; command(&c); (void)ciuki_dosvm(CIUKI_DOSVM_MARK,&c); state.marking=state.dragging=0; state.dirty=1;
}
static int copy(void)
{
    if (!state.marking) return -EINVAL;
    int x=state.x0<state.x1 ? state.x0 : state.x1,y=state.y0<state.y1 ? state.y0 : state.y1;
    unsigned w=abs(state.x1-state.x0)+1,h=abs(state.y1-state.y0)+1;
    void *data=NULL; uint32_t n=0; unsigned f=DESK_CF_DIB; int e=0;
    if (state.text_mode) {
        unsigned char *s=malloc((w+2)*h+1); if (!s) return -ENOMEM;
        for (unsigned row=0;row<h;row++) {
            unsigned used=w;
            while (used && (state.cells[(y+row)*state.columns+x+used-1]==' ' || !state.cells[(y+row)*state.columns+x+used-1])) used--;
            for (unsigned col=0;col<used;col++) { unsigned char c=state.cells[(y+row)*state.columns+x+col]; s[n++]=c ? c : ' '; }
            if (row+1<h) { s[n++]='\r'; s[n++]='\n'; }
        }
        s[n++]=0; data=s; f=DESK_CF_OEMTEXT;
    } else e=clip_dib_encode(state.snapshot+(size_t)y*640+x,640,w,h,&data,&n);
    if (!e) e=du_clip_open();
    if (!e) {
        e=du_clip_empty(); if (!e) e=du_clip_put(f,data,n);
        if (!e && state.text_mode) { unsigned char locale[4]; desk_put32(locale,0x409); e=du_clip_put(16,locale,4); }
        int closed=du_clip_close(); if (!e) e=closed;
    }
    free(data); if (!e) resume(); return e;
}
int dos_clip_action(unsigned action)
{
    if (action==4 || action==5) {
        struct dosbox_clipboard_settings s={(unsigned)state.quick_edit,(unsigned)state.exclusive};
        if (action==4) s.quick_edit=!s.quick_edit; else s.exclusive=!s.exclusive;
        int e=state.settings_bound ? dosbox_clipboard_save(state.settings_key,&s) : 0;
        if (!e) { state.quick_edit=s.quick_edit; state.exclusive=s.exclusive; }
        return e;
    }
    if (!state.id) return -ESRCH;
    if (!action) { if (state.marking) resume(); return 0; }
    if (action==2) return copy();
    if (action==1) {
        if (state.marking) return 0;
        struct ciuki_dosvm_command c; command(&c); c.focused=1;
        if (ciuki_dosvm(CIUKI_DOSVM_MARK,&c)) return -errno;
        state.marking=1; state.snapshot=malloc(640*480*4); state.cells=malloc(CIUKI_DOSVM_TEXT_MAX);
        if (!state.snapshot || !state.cells) { resume(); return -ENOMEM; }
        memcpy(state.snapshot,state.pixels,640*480*4);
        command(&c); c.text.buffer=(uint32_t)(uintptr_t)state.cells; c.text.capacity=CIUKI_DOSVM_TEXT_MAX;
        state.text_mode=!ciuki_dosvm(CIUKI_DOSVM_TEXT,&c);
        state.columns=state.text_mode ? c.text.columns : 640; state.rows=state.text_mode ? c.text.rows : 480; state.cell_height=c.text.cell_height;
        state.x0=state.y0=state.x1=state.y1=0; state.dirty=1; return 0;
    }
    if (action!=3) return -EINVAL;
    if (state.marking) resume();
    void *wide=NULL; uint32_t n; int e=du_clip_open(); if (e) return e;
    e=du_clip_get(13,&wide,&n); du_clip_close(); if (e) return e;
    struct clip_store *s=malloc(sizeof(*s)); if (!s) { free(wide); return -ENOMEM; }
    clip_init(s); struct desk_clip_wire w={.op=DESK_CLIP_OPEN},r; void *out=NULL;
    clip_request(s,1,&w,NULL,&r,&out); w.op=DESK_CLIP_EMPTY; clip_request(s,1,&w,NULL,&r,&out);
    w=(struct desk_clip_wire){.op=DESK_CLIP_PUT,.format=13,.bytes=n}; e=clip_request(s,1,&w,wide,&r,&out); free(wide);
    if (!e) { w=(struct desk_clip_wire){.op=DESK_CLIP_GET,.format=7}; e=clip_request(s,1,&w,NULL,&r,&out); }
    if (!e) { free(state.paste); state.paste=out; state.paste_bytes=r.bytes; state.paste_at=state.key_at=state.key_count=0; }
    else free(out);
    clip_destroy(s); free(s); return e;
}
static void point(int x,int y,int *col,int *row)
{
    int height=state.text_mode ? (int)(state.rows*state.cell_height) : 480;
    if (height>480) height=480; if (height<1) height=480;
    int top=(480-height)/2;
    *col=(int64_t)x*state.columns/640; *row=(int64_t)(y-top)*state.rows/height;
    if (*col<0) *col=0; if (*col>=(int)state.columns) *col=state.columns-1;
    if (*row<0) *row=0; if (*row>=(int)state.rows) *row=state.rows-1;
}
int dos_clip_input(const struct desk_packet *p)
{
    if (p->opcode==DESK_MOTION && p->event.type==CIUKI_INPUT_MOTION) { state.mouse_x=p->event.value; state.mouse_y=p->event.value2; }
    if (!state.marking && state.quick_edit && !state.exclusive && p->opcode==DESK_BUTTON && p->event.code==1 && p->event.value)
        (void)dos_clip_action(1);
    if (!state.marking) return 0;
    if (p->opcode==DESK_KEY) { if (p->event.value && p->event.code==CIUKI_KEY_ENTER) (void)copy(); else if (p->event.value && p->event.code==CIUKI_KEY_ESCAPE) resume(); return 1; }
    if (p->opcode==DESK_BUTTON && p->event.code==1) {
        state.dragging=p->event.value;
        if (state.dragging) { point(state.mouse_x,state.mouse_y,&state.x0,&state.y0); state.x1=state.x0; state.y1=state.y0; state.dirty=1; }
        return 1;
    }
    if (p->opcode==DESK_MOTION) { if (state.dragging) { point(state.mouse_x,state.mouse_y,&state.x1,&state.y1); state.dirty=1; } return 1; }
    return p->opcode==DESK_BUTTON;
}
int dos_clip_marking(void) { return state.marking; }
int dos_clip_pasting(void) { return state.paste!=NULL; }
int dos_clip_dirty(void)
{
    if (!state.dirty) return 0;
    state.dirty=0;
    if (state.marking && state.snapshot) {
        memcpy(state.pixels,state.snapshot,640*480*4);
        int left=state.x0<state.x1 ? state.x0 : state.x1, right=state.x0>state.x1 ? state.x0 : state.x1;
        int top=state.y0<state.y1 ? state.y0 : state.y1, bottom=state.y0>state.y1 ? state.y0 : state.y1;
        int height=state.text_mode ? (int)(state.rows*state.cell_height) : 480;
        if (height>480 || height<1) height=480;
        int border=(480-height)/2;
        left=left*640/state.columns; right=(right+1)*640/state.columns;
        top=border+top*height/state.rows; bottom=border+(bottom+1)*height/state.rows;
        for (int y=top;y<bottom;y++) for (int x=left;x<right;x++) state.pixels[y*640+x]^=0xffffff;
    }
    return 1;
}
void dos_clip_tick(int focused)
{
    if (!focused || state.marking || !state.paste || now()<state.next_key) return;
    if (state.key_at==state.key_count) {
        unsigned c=state.paste_at<state.paste_bytes ? state.paste[state.paste_at++] : 0;
        if (!c) { free(state.paste); state.paste=NULL; return; }
        if (c=='\r' && state.paste_at<state.paste_bytes && state.paste[state.paste_at]=='\n') state.paste_at++;
        int shift=0; unsigned code=c=='\r' || c=='\n' ? CIUKI_KEY_ENTER : c=='\t' ? CIUKI_KEY_TAB : dosbox_key_for((char)c,&shift);
        state.key_at=state.key_count=0;
        if (code) {
            if (shift) state.keys[state.key_count++]=(struct dosbox_key){.code=CIUKI_KEY_LEFTSHIFT,.down=1};
            state.keys[state.key_count++]=(struct dosbox_key){.code=code,.down=1}; state.keys[state.key_count++]=(struct dosbox_key){.code=code};
            if (shift) state.keys[state.key_count++]=(struct dosbox_key){.code=CIUKI_KEY_LEFTSHIFT};
        } else {
            static const unsigned keypad[]={0x52,0x4f,0x50,0x51,0x4b,0x4c,0x4d,0x47,0x48,0x49};
            state.keys[state.key_count++]=(struct dosbox_key){.code=CIUKI_KEY_LEFTALT,.down=1};
            unsigned digits[3]={c/100,(c/10)%10,c%10}; unsigned first=c>=100 ? 0 : c>=10 ? 1 : 2;
            for (unsigned i=first;i<3;i++) { state.keys[state.key_count++]=(struct dosbox_key){.code=keypad[digits[i]],.down=1}; state.keys[state.key_count++]=(struct dosbox_key){.code=keypad[digits[i]]}; }
            state.keys[state.key_count++]=(struct dosbox_key){.code=CIUKI_KEY_LEFTALT};
        }
    }
    struct ciuki_dosvm_command c; command(&c); struct dosbox_key *k=&state.keys[state.key_at];
    c.event.type=CIUKI_INPUT_KEY; c.event.code=k->code; c.event.value=k->down;
    if (!ciuki_dosvm(CIUKI_DOSVM_INPUT,&c)) state.key_at++;
    else if (errno!=EAGAIN) { free(state.paste); state.paste=NULL; }
    state.next_key=now()+1000000;
}
void dos_clip_destroy(void)
{
    if (state.guest_open) { (void)du_clip_close(); state.guest_open=0; }
    if (state.marking) resume(); free(state.paste); state.paste=NULL;
    while (state.count) { struct ciuki_message *m=&state.saved[state.head]; for (unsigned i=0;i<m->fd_count;i++) close(m->fds[i]); state.head=(state.head+1)%1024; state.count--; }
    du_clip_bind(NULL,NULL);
}

/* WAIT_PRESENT delivers an upcall only when the guest requests the clipboard.
 * The buffer is transfer storage for this call, never a retained mirror. */
static int dos_clip_execute(unsigned op,unsigned format,unsigned char *buffer,uint32_t *bytes)
{
    void *data=NULL; int e=0;
    switch (op) {
    case 1: e=du_clip_open(); if (!e) state.guest_open=1; break;
    case 2: e=state.guest_open ? du_clip_empty() : -EPERM; break;
    case 3: e=state.guest_open ? du_clip_put(format,buffer,*bytes) : -EPERM; break;
    case 4: case 5:
        e=state.guest_open ? du_clip_get(format,&data,bytes) : -EPERM;
        if (!e && *bytes>CIUKI_DOSVM_CLIP_MAX) e=-ENOSPC;
        if (!e && op==5 && *bytes) memcpy(buffer,data,*bytes);
        free(data); break;
    case 8: e=state.guest_open ? du_clip_close() : -EPERM; if (!e) state.guest_open=0; break;
    case 9: *bytes=CIUKI_DOSVM_CLIP_MAX; break;
    case CIUKI_DOSVM_CLIP_SCREEN:
        e=du_clip_open();
        if (!e) { e=du_clip_empty(); if (!e) e=du_clip_put(DESK_CF_OEMTEXT,buffer,*bytes);
            if (!e) { unsigned char locale[4]={9,4,0,0}; e=du_clip_put(DESK_CF_LOCALE,locale,4); }
            int closed=du_clip_close(); if (!e) e=closed; }
        break;
    default: e=-EINVAL; break;
    }
    return e;
}
int dos_clip_service(void)
{
    unsigned char *buffer=malloc(CIUKI_DOSVM_CLIP_MAX);
    if (!buffer) return -ENOMEM;
    struct ciuki_dosvm_command c; command(&c);
    c.clip.buffer=(uint32_t)(uintptr_t)buffer; c.clip.capacity=CIUKI_DOSVM_CLIP_MAX;
    if (ciuki_dosvm(CIUKI_DOSVM_CLIP_REQUEST,&c)) { int e=-errno; free(buffer); return e; }
    unsigned op=c.clip.op,format=c.clip.format; uint32_t serial=c.clip.serial,bytes=c.clip.bytes;
    int e=dos_clip_execute(op,format,buffer,&bytes);
    command(&c); c.clip.serial=serial; c.clip.bytes=e ? 0 : bytes; c.clip.result=e;
    if (op==5 && !e) { c.clip.buffer=(uint32_t)(uintptr_t)buffer; c.clip.capacity=bytes; }
    int completed=ciuki_dosvm(CIUKI_DOSVM_CLIP_COMPLETE,&c);
    int result=completed ? -errno : 0;
    if (completed && op==1 && !e) { (void)du_clip_close(); state.guest_open=0; }
    free(buffer); return result;
}
