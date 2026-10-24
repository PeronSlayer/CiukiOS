/* SPDX-License-Identifier: MIT
 * fx-15: the Windows 9x window-manager model (docs/design/
 * desktop-rendering-architecture.md, "fx-15"), every row against the
 * production desktop. Every frame on screen must equal a full redraw of
 * the current state (WM_PAINT semantics: what a change uncovers or alters
 * is invalidated and repainted before the next frame is shown).
 *
 * Presenters: the software upload, the 2D engine (screen COPY/FILL and
 * hardware pointer), the same engine with the f2-92 offscreen caches in
 * the T23 configuration of image e831e685 (caps=3, offscreen rows after
 * the scanout, pointer=hw), and the f2-101d command batches at 32 and
 * 16 bpp. The fx-2 occlusion harness (occlusion_test.c) supplies the
 * presenters, the full-composite comparison and the per-pixel oracle. */
#define DESKTOP_OCCLUSION_NO_MAIN
#include <ciuki/w32_diagnostics.h>
#include "occlusion_test.c"

enum { WM_SOFTWARE, WM_ENGINE, WM_CACHE, WM_BATCH32, WM_BATCH16, WM_MODES };
static const char *const wm_names[WM_MODES]={"software","engine","engine+cache","batch32","batch16"};
static unsigned wm_mode;
static int wm_shell;
static unsigned wm_frames;
static uint64_t wm_clock;

/* ---- fake processes: the launcher, the protocol and the kernel's reap ---- */
enum { APP_FREE, APP_SPAWNING, APP_STARTED, APP_WINDOW, APP_PAINTED };
struct fake_app { int slot, channel, surface, stage, hung, v3, w, h; uint32_t pid, salt; };
static struct fake_app apps[DESK_CLIENTS];
static int next_fd;
static uint32_t next_pid;
/* Descriptors 10..126, reused once the desktop has closed and unmapped them,
 * so long sequences keep launching programs. */
static int take_fd(void)
{
    for (unsigned n=0;n<117;n++) { int fd=10+(next_fd++-10)%117; if (fake_fd_free(fd)) return fd; }
    assert(0); return -1;
}
static int fds_left(void) { int n=0; for (int fd=10;fd<127;fd++) n+=fake_fd_free(fd); return n; }
static void apps_forget_dropped(void)
{
    /* The desktop dropped a client (close, crash, EPIPE): its process ends. */
    for (int k=0;k<DESK_CLIENTS;k++) {
        struct fake_app *a=&apps[k];
        if (a->stage==APP_FREE) continue;
        if (d.clients[a->slot].channel!=a->channel) a->stage=APP_FREE;
    }
}
static void wm_send(struct fake_app *a, struct ciuki_message *m)
{
    m->sender_pid=a->pid;
    desk_receive(&d,a->slot,m);
    apps_forget_dropped();
}
static void shell_message(struct ciuki_message *m, unsigned op, unsigned length)
{ desk_message(m,op,length); m->data[0]=DESK_SHELL_VERSION; m->data[2]=DESK_RESIZE_MINOR; }
/* The libdeskui startup order (apps/libdeskui/client.c window_open): HELLO
 * and CREATE_WINDOW with a fresh (zeroed) surface, the first frame later. */
static void app_create_window(struct fake_app *a)
{
    struct ciuki_message m;
    shell_message(&m,DESK_HELLO,4); wm_send(a,&m);
    if (a->stage==APP_FREE) return;
    /* libdeskui erases a fresh surface with the window class colour (fx-15). */
    a->surface=take_fd(); fake_surface(a->surface,(uint32_t)a->w,(uint32_t)a->h,desk_theme->face);
    static const char title[]="Program";
    unsigned t=sizeof(title)-1, n=16+t;
    shell_message(&m,DESK_CREATE_WINDOW,n+28); m.fd_count=1; m.fds[0]=a->surface;
    desk_put32(m.data+4,(uint32_t)a->w); desk_put32(m.data+8,(uint32_t)a->h); desk_put32(m.data+12,t);
    memcpy(m.data+16,title,t);
    desk_put32(m.data+n,DESK_RESIZABLE); desk_put32(m.data+n+4,64); desk_put32(m.data+n+8,48);
    desk_put32(m.data+n+12,2048); desk_put32(m.data+n+16,2048); desk_put32(m.data+n+20,1); desk_put32(m.data+n+24,1);
    wm_send(a,&m);
    if (a->stage!=APP_FREE) a->stage=APP_WINDOW;
}
static void app_paint(struct fake_app *a, unsigned x, unsigned y, unsigned w, unsigned h, uint32_t salt)
{
    struct desk_client *c=&d.clients[a->slot];
    if (!c->window || !c->pixels) return;
    uint32_t *p=(uint32_t *)c->pixels; unsigned cw=c->info.width, ch=c->info.height;
    if (x>=cw || y>=ch) return;
    if (x+w>cw) w=cw-x;
    if (y+h>ch) h=ch-y;
    for (unsigned yy=y;yy<y+h;yy++) for (unsigned xx=x;xx<x+w;xx++) p[yy*cw+xx]=(yy*92761u+xx*817u+salt)&0xffffffu;
    struct ciuki_message m; shell_message(&m,DESK_DAMAGE,24);
    desk_put32(m.data+4,x); desk_put32(m.data+8,y); desk_put32(m.data+12,w); desk_put32(m.data+16,h);
    wm_send(a,&m);
    if (a->stage==APP_WINDOW) a->stage=APP_PAINTED;
}
static void app_title(struct fake_app *a, const char *title)
{
    struct ciuki_message m; unsigned t=(unsigned)strlen(title);
    shell_message(&m,DESK_SET_TITLE,8+t); desk_put32(m.data+4,t); memcpy(m.data+8,title,t);
    wm_send(a,&m);
}
static void app_close(struct fake_app *a)
{ struct ciuki_message m; shell_message(&m,DESK_CLOSE,4); wm_send(a,&m); }
static void app_close_ack(struct fake_app *a, unsigned flags)
{ struct ciuki_message m; shell_message(&m,DESK_CLOSE_ACK,8); desk_put32(m.data+4,flags); wm_send(a,&m); }
static void app_exit(struct fake_app *a)
{
    /* The process dies: the kernel reaps it (waitpid), its channel closes. */
    uint32_t pid=a->pid;
    a->stage=APP_FREE;
    desk_child_exited(&d,pid,9);
    apps_forget_dropped();
}
static void app_answer_pings(void)
{
    for (int k=0;k<DESK_CLIENTS;k++) {
        struct fake_app *a=&apps[k];
        if (a->stage<APP_WINDOW || a->hung || a->v3 || !d.clients[a->slot].ping_pending) continue;
        struct desk_client *c=&d.clients[a->slot];
        struct ciuki_message m; shell_message(&m,DESK_PONG,8); desk_put32(m.data+4,c->serial); wm_send(a,&m);
    }
}
static struct fake_app *last_app;
static int next_size_w, next_size_h;
/* d->launch: the fx-8 asynchronous launcher (desktop.c launch_program):
 * the slot exists at once, AppStarting shows, the process comes later. */
static int fake_launch(struct desktop *dd, const struct desk_program *program)
{
    int k=0; while (k<DESK_CLIENTS && apps[k].stage!=APP_FREE) k++;
    if (k==DESK_CLIENTS || fds_left()<8) { errno=EAGAIN; return -1; }
    struct fake_app *a=&apps[k]; memset(a,0,sizeof(*a));
    a->channel=take_fd(); fake_channel(a->channel);
    a->slot=desk_add_client(dd,a->channel);
    if (a->slot<0) { (void)fake_ops.close(a->channel); errno=ENOSPC; return -1; }
    struct desk_client *c=&dd->clients[a->slot];
    snprintf(c->application,sizeof(c->application),"%s",program->name);
    c->program=desk_program_kind(program->name);
    c->awaiting_frame=desk_program_graphical(program->name);
    c->gfxdemo=!strcmp(program->name,"gfxdemo"); c->dosbox=!strcmp(program->name,"dosbox");
    c->launched_ns=c->launch_input_ns=dd->now_ns; c->spawn_pending=1;
    a->stage=APP_SPAWNING; a->pid=++next_pid; a->w=160+(int)(next_pid%5)*40; a->h=110+(int)(next_pid%3)*30;
    a->salt=next_pid*0x1f1f1fu;
    if (next_size_w) { a->w=next_size_w; a->h=next_size_h; next_size_w=next_size_h=0; }
    last_app=a;
    dd->launch_deferred=1;
    return 0;
}
static void app_spawned(struct fake_app *a)
{
    struct desk_client *c=&d.clients[a->slot];
    if (c->channel!=a->channel || !c->spawn_pending) { a->stage=APP_FREE; return; }
    c->spawn_pending=0; c->pid=a->pid; c->launched_ns=d.now_ns; a->stage=APP_STARTED;
    for (int k=0;k<DESK_CLIENTS;k++) if (!d.children[k].pid) { d.children[k].pid=a->pid; break; } /* launch_service */
    desk_cursor_update(&d);
}
static void app_step(struct fake_app *a)
{
    switch (a->stage) {
    case APP_SPAWNING: app_spawned(a); break;
    case APP_STARTED: app_create_window(a); break;
    case APP_WINDOW: app_paint(a,0,0,(unsigned)a->w,(unsigned)a->h,a->salt); break;
    default: break;
    }
}

/* A client scroll (ScrollWindowEx): retained rows move, the exposed strip
 * is new, the whole surface is reported. The engine's scroll detection
 * (compositor.c scroll_plan) turns it into a screen copy when exact. */
static void app_fill(struct fake_app *a, uint32_t colour)
{
    struct desk_client *c=&d.clients[a->slot]; uint32_t *p=(uint32_t *)c->pixels;
    for (unsigned k=0;k<c->info.width*c->info.height;k++) p[k]=colour;
    struct ciuki_message m; shell_message(&m,DESK_DAMAGE,24);
    desk_put32(m.data+12,c->info.width); desk_put32(m.data+16,c->info.height); wm_send(a,&m);
}
static void app_scroll(struct fake_app *a, int rows, uint32_t salt)
{
    struct desk_client *c=&d.clients[a->slot];
    if (!c->window || !c->pixels || a->stage<APP_PAINTED) return;
    uint32_t *p=(uint32_t *)c->pixels; unsigned w=c->info.width, h=c->info.height, n=(unsigned)(rows<0 ? -rows : rows);
    if (!n || n>=h) return;
    if (rows>0) { memmove(p,p+n*w,(size_t)(h-n)*w*4); for (unsigned k=(h-n)*w;k<h*w;k++) p[k]=(k*7631u+salt)&0xffffffu; }
    else { memmove(p+n*w,p,(size_t)(h-n)*w*4); for (unsigned k=0;k<n*w;k++) p[k]=(k*1483u+salt)&0xffffffu; }
    struct ciuki_message m; shell_message(&m,DESK_DAMAGE,24);
    desk_put32(m.data+12,w); desk_put32(m.data+16,h); wm_send(a,&m);
}
/* The graphics presenter (gfxdemo, DESK_GFX_FRAME): a scaled scene inside
 * the window plus an unscaled UI strip, one frame in flight. */
static uint32_t gfx_serial;
static void app_gfx(struct fake_app *a)
{
    struct desk_client *c=&d.clients[a->slot];
    if (!c->gfxdemo || !c->window || !c->pixels || c->gfx_pending || c->closing) return;
    uint32_t *p=(uint32_t *)c->pixels; unsigned w=c->info.width, h=c->info.height;
    unsigned sw=w/2-rnd(w/4), sh=h/2-rnd(h/4), vw=sw+rnd(w-sw), vh=sh+rnd(h-16-sh), x=rnd(w-vw+1), y=rnd(h-16-vh+1);
    for (unsigned yy=0;yy<sh;yy++) for (unsigned xx=0;xx<sw;xx++) p[(y+yy)*w+x+xx]=(yy*131u+xx*7u+gfx_serial*0x10101u)&0xffffffu;
    for (unsigned xx=0;xx<w;xx++) for (unsigned yy=h-16;yy<h;yy++) p[yy*w+xx]=(xx*3u+gfx_serial)&0xffffffu;
    struct ciuki_message m; shell_message(&m,DESK_GFX_FRAME,64);
    desk_put32(m.data+4,++gfx_serial); desk_put32(m.data+8,x); desk_put32(m.data+12,y);
    desk_put32(m.data+16,vw); desk_put32(m.data+20,vh); desk_put32(m.data+24,sw); desk_put32(m.data+28,sh);
    desk_put32(m.data+32,0); desk_put32(m.data+36,0); desk_put32(m.data+40,sw); desk_put32(m.data+44,sh);
    desk_put32(m.data+48,0); desk_put32(m.data+52,h-16); desk_put32(m.data+56,w); desk_put32(m.data+60,16);
    wm_send(a,&m);
    if (a->stage==APP_WINDOW) a->stage=APP_PAINTED;
}
/* ---- a v3 peer (the Win32 server, NetSurf): windows, popups, owners ---- */
static unsigned v3_generations[DESK_CLIENTS+1];
static int v3_live(void) { return v3_root>=0 && d.clients[v3_root].channel>=0 && d.clients[v3_root].version==DESK_V3_VERSION; }
static int v3_slot(unsigned id)
{
    for (int k=0;k<DESK_CLIENTS;k++) if (v3_live() && d.clients[k].v3_peer==(unsigned)v3_root+1 && d.clients[k].window &&
        d.clients[k].v3_id==id) return k;
    return -1;
}
static int v3_any(void)
{
    int list[DESK_CLIENTS], n=0;
    for (int k=0;k<DESK_CLIENTS;k++) if (v3_live() && d.clients[k].v3_peer==(unsigned)v3_root+1 && d.clients[k].window) list[n++]=k;
    return n ? list[rnd((unsigned)n)] : -1;
}
static int wm_v3_window(unsigned style, int owner, int x, int y, unsigned w, unsigned h)
{
    if (!v3_live()) { v3_peer(take_fd()); memset(v3_generations,0,sizeof(v3_generations)); }
    unsigned id=1; while (id<=DESK_CLIENTS && v3_slot(id)>=0) id++;
    int used=0; for (int k=0;k<DESK_CLIENTS;k++) used+=d.clients[k].channel>=0 || d.clients[k].internal;
    if (id>DESK_CLIENTS || used>=DESK_CLIENTS-2 || fds_left()<8) return -1;
    unsigned gen=++v3_generations[id];
    int fd=take_fd(); fake_surface(fd,w,h,0);
    struct ciuki_message m; v3msg(&m,DESK_CREATE_WINDOW,40,id,gen); m.fd_count=1; m.fds[0]=fd;
    desk_put32(m.data+16,style); desk_put32(m.data+20,owner>=0 ? d.clients[owner].v3_id : 0);
    desk_put32(m.data+24,owner>=0 ? d.clients[owner].v3_generation : 0);
    desk_put32(m.data+28,(uint32_t)x); desk_put32(m.data+32,(uint32_t)y);
    desk_put32(m.data+36,w); desk_put32(m.data+40,h); desk_put32(m.data+44,1); desk_put32(m.data+48,4); memcpy(m.data+52,"Wine",4);
    if (desk_v3_receive(&d,v3_root,&m)) { (void)fake_ops.close(fd); return -1; }
    int i=v3_slot(id); assert(i>=0); fill_pattern(i,id*0x070707u+gen);
    any_damage(i,0,0,w,h);
    return i;
}
static void wm_v3_create(void)
{
    unsigned style=rnd(4)==0 ? DESK_V3_POPUP : rnd(4)==0 ? DESK_V3_TOOL : DESK_V3_OVERLAPPED;
    if (!rnd(4)) style|=DESK_V3_TOPMOST;
    int owner=rnd(2) ? v3_any() : -1;
    int x=(int)rnd(d.width-60), y=(int)rnd(d.height-40);
    unsigned w=80+rnd(200), h=40+rnd(140);
    (void)wm_v3_window(style,owner,x,y,w,h);
}
static void wm_v3_place_at(int i, int x, int y, unsigned w, unsigned h, unsigned flags)
{
    struct desk_client *c=&d.clients[i]; struct ciuki_message m; v3msg(&m,DESK_V3_PLACE,28,c->v3_id,c->v3_generation);
    desk_put32(m.data+16,(uint32_t)x); desk_put32(m.data+20,(uint32_t)y); desk_put32(m.data+24,w); desk_put32(m.data+28,h);
    desk_put32(m.data+32,1); desk_put32(m.data+36,flags); desk_put32(m.data+40,c->v3_style&(DESK_V3_STYLES&~DESK_V3_MODAL));
    (void)desk_v3_receive(&d,v3_root,&m);
}
static void wm_v3_place(int i)
{
    struct desk_client *c=&d.clients[i]; struct ciuki_message m; v3msg(&m,DESK_V3_PLACE,28,c->v3_id,c->v3_generation);
    unsigned w=c->info.width+(rnd(3) ? 0 : rnd(40)), h=c->info.height+(rnd(3) ? 0 : rnd(30));
    desk_put32(m.data+16,(uint32_t)(rnd(2) ? c->frame.x : (int)rnd(d.width-60))); desk_put32(m.data+20,(uint32_t)(rnd(2) ? c->frame.y : (int)rnd(d.height-40)));
    desk_put32(m.data+24,w); desk_put32(m.data+28,h); desk_put32(m.data+32,1);
    desk_put32(m.data+36,rnd(3)==0 ? 2 : rnd(3)==0 ? 1 : 0); desk_put32(m.data+40,c->v3_style&(DESK_V3_STYLES&~DESK_V3_MODAL));
    (void)desk_v3_receive(&d,v3_root,&m);
}
static void wm_v3_activate(int i, unsigned focused)
{
    struct desk_client *c=&d.clients[i]; struct ciuki_message m; v3msg(&m,DESK_V3_ACTIVATE,8,c->v3_id,c->v3_generation);
    desk_put32(m.data+16,focused); desk_put32(m.data+20,focused); (void)desk_v3_receive(&d,v3_root,&m);
}
static void wm_v3_close(int i)
{
    struct desk_client *c=&d.clients[i]; struct ciuki_message m; v3msg(&m,DESK_CLOSE,0,c->v3_id,c->v3_generation);
    (void)desk_v3_receive(&d,v3_root,&m);
}
static void wm_v3_title(int i)
{
    struct desk_client *c=&d.clients[i]; struct ciuki_message m; static const char t[]="Wine window";
    v3msg(&m,DESK_SET_TITLE,4+(unsigned)(sizeof(t)-1),c->v3_id,c->v3_generation);
    desk_put32(m.data+16,(uint32_t)(sizeof(t)-1)); memcpy(m.data+20,t,sizeof(t)-1); (void)desk_v3_receive(&d,v3_root,&m);
}
/* desktop shortcuts appearing and disappearing (an installer, f2-95's worker) */
static unsigned link_variant;
static int fake_links(struct desktop *dd, struct desk_link_entry *out, unsigned cap)
{
    (void)dd; unsigned n=link_variant%3;
    if (n>cap) n=cap;
    for (unsigned k=0;k<n;k++) {
        memset(&out[k],0,sizeof(out[k]));
        snprintf(out[k].launch.label,sizeof(out[k].launch.label),"Game %u",k+link_variant);
        snprintf(out[k].launch.target,sizeof(out[k].launch.target),"C:\\GAMES\\G%u.EXE",k);
        out[k].launch.show=1; out[k].desktop=1;
        snprintf(out[k].source,sizeof(out[k].source),"/Documents/Desktop/g%u.lnk",k);
    }
    return (int)n;
}
/* ---- the desktop folder and the Files drag data object (fx-19) ----
 * A read-only folder: /Documents/Desktop empty, and the data object a Files
 * window writes before DESK_DRAG_BEGIN (two items). Drops are never made
 * where they would start a transfer; the drag image, targets, highlight,
 * taskbar hover and cancel are what this harness compares. */
static const char drag_object[]="X\nF/Documents/report.txt\nD/Documents/Games\n";
static unsigned drag_offset;
static int vfs_list(void *ctx, const char *path, unsigned flags, fm_emit emit, void *arg)
{
    (void)ctx; (void)flags;
    struct fm_meta dir={.type=FM_DIR};
    if (!strcmp(path,"/")) return emit("Documents",&dir,arg);
    if (!strcmp(path,"/Documents")) return emit("Desktop",&dir,arg);
    return 0;
}
static int vfs_stat(void *ctx, const char *path, struct fm_meta *meta)
{
    (void)ctx;
    if (!strcmp(path,"/") || !strcmp(path,"/Documents") || !strcmp(path,"/Documents/Desktop")) { *meta=(struct fm_meta){.type=FM_DIR}; return 0; }
    return -ENOENT;
}
static int vfs_refuse_path(void *ctx, const char *path) { (void)ctx; (void)path; return -EROFS; }
static int vfs_refuse_rename(void *ctx, const char *a, const char *b) { (void)ctx; (void)a; (void)b; return -EROFS; }
static int vfs_open(void *ctx, const char *path, int create)
{ (void)ctx; if (create || strcmp(path,DESK_DRAG_FILE)) return create ? -EROFS : -ENOENT; drag_offset=0; return 5; }
static int vfs_read(void *ctx, int fd, void *out, unsigned n)
{
    (void)ctx; (void)fd; unsigned left=(unsigned)sizeof(drag_object)-1-drag_offset; if (n>left) n=left;
    memcpy(out,drag_object+drag_offset,n); drag_offset+=n; return (int)n;
}
static int vfs_write(void *ctx, int fd, const void *in, unsigned n) { (void)ctx; (void)fd; (void)in; (void)n; return -EROFS; }
static int vfs_fd(void *ctx, int fd) { (void)ctx; (void)fd; return 0; }
static int vfs_metadata(void *ctx, const char *path, const struct fm_meta *meta) { (void)ctx; (void)path; (void)meta; return -EROFS; }
static const struct fm_vfs wm_vfs={NULL,vfs_list,vfs_stat,vfs_refuse_path,vfs_refuse_rename,vfs_refuse_path,vfs_refuse_path,
    vfs_open,vfs_read,vfs_write,vfs_fd,vfs_fd,vfs_metadata};
/* ---- input as the hardware delivers it (desk_input) ---- */
static void pointer_to(int x, int y)
{
    if (x<0) x=0;
    if (y<0) y=0;
    if (x>=(int)d.width) x=(int)d.width-1;
    if (y>=(int)d.height) y=(int)d.height-1;
    struct ciuki_input_event e={.type=CIUKI_INPUT_MOTION,.value=x-d.mouse_x,.value2=y-d.mouse_y,.monotonic_ns=d.now_ns};
    desk_input(&d,&e);
}
static void press(int code, int down)
{
    struct ciuki_input_event e={.type=CIUKI_INPUT_BUTTON,.code=code,.value=down,.monotonic_ns=d.now_ns};
    desk_input(&d,&e);
}
static void click(int x, int y, int code) { pointer_to(x,y); press(code,1); press(code,0); }
static void key(int code, int value)
{
    struct ciuki_input_event e={.type=CIUKI_INPUT_KEY,.code=code,.value=value,.monotonic_ns=d.now_ns};
    desk_input(&d,&e);
}
static void tap(int code) { key(code,1); key(code,0); }
/* ---- fx-19: selection, rubber band, drag and drop as the user does them ---- */
static int on_icon(int x, int y)
{
    for (int k=0;k<d.sh.icon_count;k++) if (desk_contains(d.sh.icons[k].box,x,y)) return 1;
    return 0;
}
static int empty_point(int *x, int *y)
{
    for (unsigned n=0;n<40;n++) {
        int px=(int)rnd(d.width), py=DESK_BAR+(int)rnd(d.height-2*DESK_BAR);
        if (desk_hit(&d,px,py)<0 && !on_icon(px,py) && !desk_start_contains(&d,px,py)) { *x=px; *y=py; return 1; }
    }
    return 0;
}
static void icon_point(int k, int *x, int *y) { struct desk_box b=d.sh.icons[k].box; *x=b.x+b.w/2; *y=b.y+20; }
static void app_accept_files(struct fake_app *a, unsigned on)
{ struct ciuki_message m; shell_message(&m,DESK_ACCEPT_FILES,8); desk_put32(m.data+4,on); wm_send(a,&m); }
/* A Files window starts a shell drag: the press reaches it (routed
 * button), it writes the data object and sends DESK_DRAG_BEGIN. */
static int files_drag_begin(struct fake_app *a, unsigned button)
{
    struct desk_client *c=&d.clients[a->slot];
    if (!c->window || c->minimized || c->program!=DESK_APP_FILES || d.sh.dnd.active) return 0;
    struct desk_box b=desk_content(c);
    if (b.w<20 || b.h<20) return 0;
    pointer_to(b.x+b.w/2,b.y+b.h/2);
    if (desk_hit(&d,d.mouse_x,d.mouse_y)!=a->slot) return 0;
    press(button==2 ? CIUKI_BUTTON_RIGHT : CIUKI_BUTTON_LEFT,1);
    struct ciuki_message m; shell_message(&m,DESK_DRAG_BEGIN,12); desk_put32(m.data+4,button); desk_put32(m.data+8,2); wm_send(a,&m);
    return d.sh.dnd.active;
}
static void serve_launches(void)
{
    /* desktop.c launch_pending: a Programs menu item runs at the next turn. */
    if (d.action>=0) {
        unsigned selected=(unsigned)d.action; d.action=-1;
        if (selected<d.program_count) (void)desk_task_launch(&d,d.programs[selected].name,NULL);
    }
}
/* One service turn of desktop.c's loop: heartbeat (PINGs, animation),
 * shell tick (tooltips, notifications), queued launches, client flushes. */
static void turn(uint64_t ns)
{
    wm_clock+=ns; d.now_ns=wm_clock;
    desk_heartbeat(&d,d.now_ns);
    if (d.shell) { desk_shell_tick(&d); desk_links_refresh(&d); }
    serve_launches();
    for (int k=0;k<DESK_CLIENTS;k++) desk_flush(&d,k);
    apps_forget_dropped();
    fake_sent=0;
    /* desktop.c shell_service serves Shut Down; the harness declines it. */
    if (d.power.state) desk_power_cancel(&d);
    d.sh.want_power=0;
}

/* ---- frames ---- */
static unsigned seen_band, seen_drag, seen_drag_menu, seen_drop_lit, seen_multi, seen_files_drag;
static void wm_frame(const char *what)
{
    if (d.shell) {
        unsigned marked=0; for (int k=0;k<d.sh.icon_count;k++) marked+=desk_shell_icon_marked(&d,k)!=0;
        seen_band+=d.sh.band!=0; seen_drag+=d.sh.dnd.active && !d.sh.dnd.menu; seen_drag_menu+=d.sh.dnd.active && d.sh.dnd.menu;
        seen_drop_lit+=d.sh.dnd.active && d.sh.dnd.drop_index>=0; seen_multi+=marked>1; seen_files_drag+=d.sh.dnd.active && d.sh.dnd.source>=0;
    }
    snprintf(step_name,sizeof(step_name),"%s",what);
    if (wm_mode>=WM_BATCH32) batch_frame(); else redraw();
    /* A saver intentionally covers every application. Full-scene oracle still runs. */
    if (!d.animation.active && !d.saver_active) visible_oracle();
    frames_checked++; wm_frames++;
    if (wm_mode==WM_CACHE && !d.damage.count && !d.move_damage.count && !d.present_damage.count)
        for (unsigned k=0;k<64;k++) { wm_clock+=40000000ull; if (!desk_accel_cache_step(&d,wm_clock)) break; }
}
static const char *const wm_programs[]={"files","ciuknote","control","netsurf","dosbox","calc","tasks","terminal","gfxdemo"};
static void wm_begin(unsigned m, int shell)
{
    wm_mode=m; wm_shell=shell;
    mode=m==WM_SOFTWARE ? MODE_SOFTWARE : m==WM_ENGINE || m==WM_CACHE ? MODE_ENGINE : m==WM_BATCH32 ? MODE_BATCH32 : MODE_BATCH16;
    fake_reset(); struct desk_ops ops=fake_ops; ops.present=m>=WM_BATCH32 ? packed_upload : upload;
    desk_init(&d,&ops,640,480); d.display=7; d.mouse_x=600; d.mouse_y=12;
    pixel_bpp=m==WM_BATCH16 ? 16 : 32;
    /* T23 e831e685: 1024x768 with offscreen rows 768..1280 (two thirds of
     * the screen again): the background never fits, windows do. */
    unsigned rows=m==WM_CACHE ? d.height+d.height*2/3 : d.height+2;
    d.pixels=calloc((size_t)d.width*d.height,4); scanout=calloc((size_t)d.width*rows,4);
    assert(d.pixels && scanout);
    caps=0; shown=fail_copy=fail_cursor=fail_fill=fail_write=0; vram_first=vram_end=0;
    if (m==WM_ENGINE || m==WM_CACHE) {
        caps=3; d.accel=service;
        if (m==WM_CACHE) { vram_first=d.height; vram_end=rows; }
        desk_accel_reset(&d,55); assert(d.hw_cursor);
        if (m==WM_CACHE) assert(d.vram_end==rows);
    }
    if (m>=WM_BATCH32) {
        caps=1; vram_first=d.height; vram_end=d.height+2; d.display_info=format_info; d.lfb_bytes_per_pixel=pixel_bpp/8;
        d.accel=m==WM_BATCH16 ? service16 : service; d.commands=commands_info; d.batch=batch_service; d.fence=fence_service;
        desk_accel_reset(&d,55); assert(d.hw_cursor && d.hw_colour_cursor);
    }
    memset(apps,0,sizeof(apps)); next_fd=10; next_pid=100; wm_clock=1000000000ull; d.now_ns=wm_clock;
    d.launch=fake_launch; v3_root=-1; link_variant=0; gfx_serial=0;
    if (shell) d.v3_links_list=fake_links;
    for (unsigned k=0;k<sizeof(wm_programs)/sizeof(*wm_programs);k++) {
        snprintf(d.programs[k].name,sizeof(d.programs[k].name),"%s",wm_programs[k]);
        snprintf(d.programs[k].path,sizeof(d.programs[k].path),"/Programs/%s",wm_programs[k]);
    }
    d.program_count=sizeof(wm_programs)/sizeof(*wm_programs);
    if (shell) {
        desk_shell_enable(&d);
        snprintf(d.sh.cfg.wallpaper,sizeof(d.sh.cfg.wallpaper),"builtin:%s",desk_builtin_wallpapers[0].id);
        desk_shell_apply(&d); assert(!desk_shell_wallpaper_update(&d));
        d.sh.mixer_state=2;
        assert(!desk_folder_init(&d,&wm_vfs));
    }
    wm_frame("begin");
}
static void wm_end(void)
{
    desk_shell_drag_cancel(&d);
    for (int k=0;k<DESK_CLIENTS;k++) desk_drop_client(&d,k);
    desk_folder_destroy(&d); free(d.sh.band_original); d.sh.band_original=NULL; d.sh.band_capacity=0;
    desk_render_release(&d); free(d.pixels); free(scanout);
    free(d.sh.icons); free(d.sh.wall); free(d.sh.background); free(d.v3_links); free(d.sh.autoplay.workspace);
    d.sh.icons=NULL; d.sh.wall=NULL; d.sh.background=NULL; d.v3_links=NULL; d.sh.autoplay.workspace=NULL;
    vram_first=vram_end=0;
    assert(!fake_live());
}

/* ---- randomized sequences over every row of the model ---- */
static struct fake_app *random_app(int stage)
{
    int list[DESK_CLIENTS], n=0;
    for (int k=0;k<DESK_CLIENTS;k++) if (apps[k].stage>=stage && apps[k].stage!=APP_FREE) list[n++]=k;
    return n ? &apps[list[rnd((unsigned)n)]] : NULL;
}
static int random_window(void)
{
    int list[DESK_CLIENTS], n=0;
    for (int k=0;k<DESK_CLIENTS;k++) if (d.clients[k].window) list[n++]=k;
    return n ? list[rnd((unsigned)n)] : -1;
}
static void random_point_on(struct desk_box b, int *x, int *y)
{ *x=b.x+(int)rnd((unsigned)(b.w>0 ? b.w : 1)); *y=b.y+(int)rnd((unsigned)(b.h>0 ? b.h : 1)); }
static void wm_fuzz(unsigned m, unsigned run)
{
    seed=run*104729u+m*7u+3u; fuzzing=1;
    wm_begin(m,1);
    char what[96];
    for (unsigned step=0;step<240;step++) {
        unsigned op=rnd(48);
        snprintf(what,sizeof(what),"fuzz run=%u step=%u op=%u",run,step,op);
        snprintf(step_name,sizeof(step_name),"%s",what);
        if (getenv("WM_TRACE")) fprintf(stderr,"%s %s focus=%d levels=%d\n",wm_names[m],what,d.focus,d.sh.levels);
        struct fake_app *a; int w, x, y;
        switch (op) {
        case 0: /* Start menu, a program from it */
            click(DESK_START_X+DESK_START_W/2,DESK_START_Y+DESK_START_H/2,CIUKI_BUTTON_LEFT); break;
        case 1: /* anywhere: menus, icons, task buttons, tray, windows */
            click((int)rnd(d.width),(int)rnd(d.height),rnd(4) ? CIUKI_BUTTON_LEFT : CIUKI_BUTTON_RIGHT); break;
        case 2: /* hover (menus, tooltips, title buttons) */
            pointer_to((int)rnd(d.width),(int)rnd(d.height)); break;
        case 3: if ((a=random_app(APP_SPAWNING))) app_step(a); break;
        case 4: if ((a=random_app(APP_PAINTED))) {
                    unsigned ww=1+rnd((unsigned)a->w), hh=1+rnd((unsigned)a->h);
                    app_paint(a,rnd((unsigned)a->w),rnd((unsigned)a->h),ww,hh,rnd(0xffffff));
                } break;
        case 5: if ((a=random_app(APP_WINDOW)) && !rnd(3)) app_close(a); break;
        case 6: if ((a=random_app(APP_SPAWNING)) && !rnd(3)) app_exit(a); break;
        case 7: if ((a=random_app(APP_WINDOW))) app_title(a,rnd(2) ? "Renamed program" : "Program"); break;
        case 8: /* drag a title bar */
            if ((w=random_window())>=0) {
                struct desk_box f=d.clients[w].frame; random_point_on((struct desk_box){f.x+30,f.y+4,f.w-110,DESK_TITLE-8},&x,&y);
                pointer_to(x,y); press(CIUKI_BUTTON_LEFT,1);
                for (unsigned k=0;k<1+rnd(4);k++) pointer_to(d.mouse_x+(int)rnd(81)-40,d.mouse_y+(int)rnd(61)-30);
                if (rnd(3)) press(CIUKI_BUTTON_LEFT,0);
            } break;
        case 9: /* title buttons and borders */
            if ((w=random_window())>=0) {
                struct desk_client *c=&d.clients[w];
                struct desk_box b=rnd(3)==0 ? desk_close_box(c) : rnd(2) ? desk_min_box(c) : desk_shade_box(c);
                if (b.w>0) { random_point_on(b,&x,&y); click(x,y,CIUKI_BUTTON_LEFT); }
            } break;
        case 10: /* Alt+Tab, Alt+Esc, Alt+F4, Ctrl+Esc, Esc, Win */
            x=(int)rnd(6); if (getenv("WM_TRACE")) fprintf(stderr," keys %d\n",x);
            switch (x) {
            case 0: key(0x38,1); tap(DU_KEY_TAB); if (rnd(2)) tap(DU_KEY_TAB); key(0x38,0); break;
            case 1: key(0x38,1); tap(DU_KEY_ESC); key(0x38,0); break;
            case 2: key(0x38,1); tap(DU_KEY_F4); key(0x38,0); break;
            case 3: key(0x1d,1); tap(DU_KEY_ESC); key(0x1d,0); break;
            case 4: tap(DU_KEY_ESC); break;
            default: tap(0x15b); break;
            } break;
        case 11: /* menu keyboard */
            tap(rnd(3)==0 ? DU_KEY_ENTER : rnd(2) ? DU_KEY_DOWN : DU_KEY_RIGHT); break;
        case 12: turn(16666667ull); break;
        case 13: turn(150000000ull+rnd(600)*1000000ull); break; /* tooltips, animations */
        case 14: if (!rnd(6)) turn(5100000000ull); break;        /* PING deadlines */
        case 15: if (!rnd(3)) desk_shell_notify(&d,"Notice","Something happened",DU_DLG_ICON_INFO,SH_NOTIFY_DETAILS); break;
        case 16: if (!rnd(4)) desk_internal_open(&d,rnd(2) ? DESK_INTERNAL_RUN : DESK_INTERNAL_ABOUT); break;
        case 17: if (!rnd(4)) desk_shell_message(&d,"Programs","A message.",rnd(2)); break;
        case 18: /* double click (title zoom, icon open) */
            x=(int)rnd(d.width); y=(int)rnd(d.height);
            click(x,y,CIUKI_BUTTON_LEFT); click(x,y,CIUKI_BUTTON_LEFT); break;
        case 19: if ((a=random_app(APP_WINDOW))) a->hung=!a->hung; break;
        case 20: if ((a=random_app(APP_WINDOW)) && !rnd(2)) { app_close_ack(a,0); } break;
        case 21: /* resize from the bottom-right grip */
            if ((w=random_window())>=0 && d.clients[w].resizable && !d.clients[w].internal) {
                struct desk_box f=d.clients[w].frame;
                pointer_to(f.x+f.w-2,f.y+f.h-2); press(CIUKI_BUTTON_LEFT,1);
                pointer_to(d.mouse_x+(int)rnd(61)-30,d.mouse_y+(int)rnd(41)-20); press(CIUKI_BUTTON_LEFT,0);
            } break;
        case 24: if (!rnd(2)) wm_v3_create(); break;
        case 25: if ((w=v3_any())>=0) wm_v3_place(w); break;
        case 26: if ((w=v3_any())>=0) wm_v3_activate(w,rnd(4)!=0); break;
        case 27: if ((w=v3_any())>=0 && !rnd(3)) wm_v3_close(w); break;
        case 28: if ((w=v3_any())>=0) {
                     if (rnd(3)) { fill_pattern(w,rnd(0xffffff)); any_damage(w,0,0,d.clients[w].info.width,d.clients[w].info.height); }
                     else if (rnd(2)) v3_window_order(w,rnd(3)==0 ? DESK_V3_ORDER_BOTTOM : rnd(2) ? DESK_V3_ORDER_TOPMOST : DESK_V3_ORDER_NOTOPMOST);
                     else wm_v3_title(w);
                 } break;
        case 29: if (v3_live() && !rnd(12)) { if (rnd(2)) desk_drop_client(&d,v3_root); else desk_child_exited(&d,d.clients[v3_root].pid,9); } break;
        case 30: if ((a=random_app(APP_PAINTED))) app_scroll(a,(int)rnd(41)-20,rnd(0xffffff)); break;
        case 31: if ((a=random_app(APP_WINDOW))) app_gfx(a); break;
        case 32: if (!rnd(8)) { /* a DOS game ran full screen and returned the display (desktop.c display_poll) */
                     for (size_t k=0;k<(size_t)d.width*d.height;k++) scanout[k]=(uint32_t)(k*2654435761u);
                     desk_accel_reset(&d,55);
                     struct ciuki_input_event e={.type=CIUKI_INPUT_RESYNC,.monotonic_ns=d.now_ns}; desk_input(&d,&e);
                 } break;
        case 33: if (!rnd(4)) { link_variant++; d.v3_links_next=0; } break;
        case 34: if (!rnd(10)) { desk_theme_use(cfg_theme_names[rnd(CFG_THEMES)]); desk_theme_changed(&d); } break;
        case 35: if (!rnd(3)) { /* launch a program from Programs > the menu's action */
                     d.action=(int)rnd(d.program_count);
                 } break;
        case 36: /* fx-19: drag desktop icons (left or right button), over windows, taskbar, desktop */
            if (d.sh.icon_count && !d.sh.dnd.active) {
                icon_point((int)rnd((unsigned)d.sh.icon_count),&x,&y);
                if (desk_hit(&d,x,y)>=0) break;
                unsigned button=rnd(4) ? CIUKI_BUTTON_LEFT : CIUKI_BUTTON_RIGHT;
                pointer_to(x,y); press((int)button,1);
                for (unsigned k=0;k<2+rnd(5);k++) { pointer_to((int)rnd(d.width),(int)rnd(d.height)); turn(16666667ull); wm_frame(what); }
                if (!rnd(4)) tap(DU_KEY_ESC);
                if (rnd(5)) press((int)button,0);
            } break;
        case 37: /* fx-19: rubber band, plain, Ctrl or Shift; sometimes left open */
            if (!d.sh.dnd.active && empty_point(&x,&y)) {
                int mod=rnd(3)==0 ? 0x1d : rnd(2) ? 0x2a : 0;
                if (mod) key(mod,1);
                pointer_to(x,y); press(CIUKI_BUTTON_LEFT,1);
                for (unsigned k=0;k<1+rnd(4);k++) { pointer_to(d.mouse_x+(int)rnd(181)-90,d.mouse_y+(int)rnd(141)-70); turn(16666667ull); wm_frame(what); }
                if (rnd(5)) press(CIUKI_BUTTON_LEFT,0);
                if (mod) key(mod,0);
            } break;
        case 38: /* fx-19: keyboard selection on the desktop */
            if (!d.sh.dnd.active && empty_point(&x,&y)) {
                click(x,y,CIUKI_BUTTON_LEFT);
                switch (rnd(5)) {
                case 0: key(0x1d,1); tap(0x1e); key(0x1d,0); break;
                case 1: tap(rnd(2) ? DU_KEY_DOWN : DU_KEY_RIGHT); break;
                case 2: key(0x2a,1); tap(DU_KEY_DOWN); key(0x2a,0); break;
                case 3: key(0x1d,1); tap(DU_KEY_UP); tap(DU_KEY_SPACE); key(0x1d,0); break;
                default: tap(DU_KEY_ESC); break;
                }
            } break;
        case 39: /* fx-19: a Files window drags two items: image, targets, highlight */
            a=NULL;
            for (int k=0;k<DESK_CLIENTS && !a;k++) if (apps[k].stage!=APP_FREE && !strcmp(d.clients[apps[k].slot].application,"files")) a=&apps[k];
            if (!a) { (void)desk_task_launch(&d,"files",NULL); break; }
            /* the Files window finishes starting first */
            for (unsigned k=0;k<3 && a->stage!=APP_FREE && a->stage<APP_PAINTED;k++) app_step(a);
            if (a->stage!=APP_PAINTED) break;
            if (a) {
                unsigned button=rnd(4) ? 1 : 2;
                if (!files_drag_begin(a,button)) { press(button==2 ? CIUKI_BUTTON_RIGHT : CIUKI_BUTTON_LEFT,0); break; }
                for (unsigned k=0;k<2+rnd(5);k++) {
                    if (rnd(3)==0 && d.sh.icon_count>2) { icon_point(2,&x,&y); pointer_to(x,y); }   /* over the Trash */
                    else pointer_to((int)rnd(d.width),(int)rnd(d.height));
                    turn(16666667ull); wm_frame(what);
                }
                if (rnd(3)) tap(DU_KEY_ESC);
                else { pointer_to((int)rnd(d.width),(int)rnd(DESK_BAR)); press(button==2 ? CIUKI_BUTTON_RIGHT : CIUKI_BUTTON_LEFT,0); }
                if (d.sh.dnd.active) tap(DU_KEY_ESC);
            } break;
        case 40: if ((a=random_app(APP_PAINTED))) app_accept_files(a,rnd(2)); break;
        case 41: if (d.sh.dnd.active || d.sh.band) turn(1100000000ull); break; /* taskbar hover during a drag */
        case 42: /* G4: the taskbar menu on the empty bar or the top bar; an item by arrow, access key, click or none */
            if (!d.sh.dnd.active) {
                x=(int)rnd(d.width); y=rnd(2) ? (int)d.height-DESK_BAR+(int)rnd(DESK_BAR) : (int)rnd(DESK_BAR);
                click(x,y,CIUKI_BUTTON_RIGHT);
                if (d.sh.levels && d.sh.menus[0].kind==SH_MENU_TASKBAR) {
                    static const int letters[]={0x2e,0x23,0x2f,0x32,0x16,0x13};
                    switch (rnd(5)) {
                    case 0: tap(DU_KEY_DOWN); tap(DU_KEY_DOWN); wm_frame(what); tap(DU_KEY_ENTER); break;
                    case 1: tap(letters[rnd(6)]); break;
                    case 2: { struct sh_menu *menu=&d.sh.menus[0]; unsigned k=rnd((unsigned)menu->count); y=menu->box.y+3;
                              for (unsigned j=0;j<k;j++) y+=menu->items[j].flags&SH_ITEM_SEPARATOR ? SH_SEPARATOR : menu->row_h;
                              pointer_to(menu->box.x+20,y+4); wm_frame(what); press(CIUKI_BUTTON_LEFT,1); press(CIUKI_BUTTON_LEFT,0); } break;
                    default: tap(DU_KEY_ESC); break;
                    }
                }
            } break;
        case 43: /* G4: Win+D, Win+M, Shift+Win+M, Alt+M */
            if (!d.sh.dnd.active) { switch (rnd(4)) {
            case 0: key(0x15b,1); tap(0x20); key(0x15b,0); break;
            case 1: key(0x15c,1); tap(0x32); key(0x15c,0); break;
            case 2: key(0x2a,1); key(0x15b,1); tap(0x32); key(0x15b,0); key(0x2a,0); break;
            default: key(0x38,1); tap(0x32); key(0x38,0); break;
            } } break;
        case 44: /* G4: the taskbar's keyboard focus */
            if (!d.sh.dnd.active) { switch (rnd(6)) {
            case 0: key(0x15b,1); tap(DU_KEY_TAB); key(0x15b,0); break;
            case 1: key(0x1d,1); tap(DU_KEY_ESC); key(0x1d,0); tap(DU_KEY_ESC); break;
            case 2: tap(rnd(2) ? DU_KEY_TAB : DU_KEY_RIGHT); break;
            case 3: tap(DU_KEY_ENTER); break;
            case 4: tap(DU_KEY_ESC); break;
            default: tap(0x15d); break;
            } } break;
        case 45: /* G4: Alt+F6, the Menu key, Shift+F10, F3 */
            if (!d.sh.dnd.active) { switch (rnd(4)) {
            case 0: key(0x38,1); tap(0x40); key(0x38,0); break;
            case 1: tap(0x15d); break;
            case 2: key(0x2a,1); tap(DU_KEY_F10); key(0x2a,0); break;
            default: tap(0x3d); break;
            } } break;
        case 46: /* G4: the caption icon (Ciuki Blue draws one): menu, or a double click closes */
            if ((w=random_window())>=0 && !d.sh.dnd.active) {
                struct desk_box icon=desk_caption_icon_box(&d,&d.clients[w]);
                if (icon.w) { click(icon.x+8,icon.y+8,CIUKI_BUTTON_LEFT); if (!rnd(3)) click(icon.x+8,icon.y+8,CIUKI_BUTTON_LEFT); }
            } break;
        case 47: /* G4: Win+E, Win+R, Win+F, Win+Break, Win+F1 */
            if (!d.sh.dnd.active) {
                static const int chords[]={0x12,0x13,0x21,0x200,0x3b};
                key(0x15b,1); tap(chords[rnd(5)]); key(0x15b,0);
            } break;
        
        default: break;
        }
        app_answer_pings();
        apps_forget_dropped();
        if (rnd(3)) { turn(16666667ull); wm_frame(what); }
    }
    turn(16666667ull); wm_frame("fuzz final");
    fuzzing=0; wm_end();
}
/* ---- the model's rows, one deterministic scenario each (all presenters) ---- */
static char row_step[96];
static const char *row_name;
static void step(const char *what) { snprintf(row_step,sizeof(row_step),"%s: %s",row_name,what); turn(16666667ull); wm_frame(row_step); }
static struct fake_app *app_open(const char *name, int w, int h, int x, int y)
{
    next_size_w=w; next_size_h=h; last_app=NULL;
    int r=desk_task_launch(&d,name,NULL); assert(r==0 && last_app);
    struct fake_app *a=last_app;
    app_spawned(a); app_create_window(a); assert(a->stage==APP_WINDOW);
    if (x>=0) desk_move(&d,a->slot,x,y);
    app_paint(a,0,0,(unsigned)a->w,(unsigned)a->h,a->salt);
    return a;
}
static void row_create(unsigned m)
{
    /* Window created: CREATE_WINDOW shows frame and (erased) client area at
     * once, above everything it covers, the activation moves to it; the
     * first frame follows. Also during a drag and with the Start menu open. */
    row_name="create"; wm_begin(m,1);
    struct fake_app *a=app_open("files",300,200,160,60); step("first window");
    struct desk_box icon=d.sh.icons[0].box; click(icon.x+icon.w/2,icon.y+20,CIUKI_BUTTON_LEFT);
    assert(d.focus<0 && d.sh.icon_selected==0); step("desktop icon selected, outline shown");
    next_size_w=260; next_size_h=180; assert(!desk_task_launch(&d,"ciuknote",NULL)); struct fake_app *b=last_app;
    step("AppStarting: slot without a window");
    app_spawned(b); step("process created");
    d.cascade=7; /* the cascade slot clear of the icon column */
    app_create_window(b); assert(d.focus==b->slot); step("window created over the desktop icon (outline cleared)");
    app_paint(b,0,0,(unsigned)b->w,(unsigned)b->h,b->salt); step("first frame");
    turn(600000000ull); click(icon.x+icon.w/2,icon.y+20,CIUKI_BUTTON_LEFT); assert(d.focus<0); step("icon selected again");
    struct desk_box fa=d.clients[a->slot].frame; click(fa.x+fa.w/2,fa.y+8,CIUKI_BUTTON_LEFT); assert(d.focus==a->slot);
    step("a window activated: the icon's focus outline goes");
    struct desk_box f=d.clients[a->slot].frame; pointer_to(f.x+60,f.y+8); press(CIUKI_BUTTON_LEFT,1);
    pointer_to(f.x+90,f.y+30); pointer_to(f.x+110,f.y+50);
    next_size_w=200; next_size_h=150; assert(!desk_task_launch(&d,"calc",NULL)); struct fake_app *c=last_app;
    app_spawned(c); app_create_window(c); step("window created while another is dragged");
    pointer_to(f.x+130,f.y+60); press(CIUKI_BUTTON_LEFT,0); step("drag ends");
    app_paint(c,0,0,(unsigned)c->w,(unsigned)c->h,c->salt); step("its first frame");
    click(DESK_START_X+DESK_START_W/2,DESK_START_Y+DESK_START_H/2,CIUKI_BUTTON_LEFT); assert(d.sh.levels); step("Start menu open");
    next_size_w=240; next_size_h=160; assert(!desk_task_launch(&d,"terminal",NULL)); struct fake_app *e=last_app;
    app_spawned(e); app_create_window(e); app_paint(e,0,0,(unsigned)e->w,(unsigned)e->h,e->salt); step("window created under the open menu");
    tap(DU_KEY_ESC); step("menu closed");
    wm_end();
}
static void row_stretched(unsigned m)
{
    /* Maximise before the client re-lays out: the old backing is stretched
     * over the well; its updates must repaint every stretched texel. */
    row_name="stretched damage"; wm_begin(m,1);
    struct fake_app *a=app_open("files",220,140,60,60); step("window");
    desk_zoom(&d,a->slot); assert(d.clients[a->slot].zoomed); step("maximised, backing stretched");
    for (unsigned k=0;k<12;k++) turn(16666667ull);
    app_paint(a,0,0,30,20,0x515151u); step("client update while stretched (top-left)");
    app_paint(a,100,60,120,80,0x343434u); step("client update while stretched (bottom-right)");
    desk_zoom(&d,a->slot); step("restored");
    wm_end();
}
static void row_scroll_overlays(unsigned m)
{
    /* ScrollWindowEx while something is drawn above the window: the
     * minimise outline (DrawAnimatedRects) and the taskbar. A screen copy
     * must neither carry nor cover those pixels. */
    row_name="scroll under overlays"; wm_begin(m,1);
    struct fake_app *a=app_open("netsurf",260,200,200,100); app_fill(a,desk_theme->face); step("a window with a plain (erased) page");
    struct fake_app *b=app_open("files",514,276,40,60); step("a wide window above it");
    struct desk_box min=desk_min_box(&d.clients[b->slot]); click(min.x+2,min.y+2,CIUKI_BUTTON_LEFT);
    assert(d.clients[b->slot].minimized && d.animation.active); step("minimise outline starts");
    for (unsigned k=0;k<8;k++) {
        /* The outline sweeps over the plain page: the outline's own damage
         * lies in the page, and a translation of plain pixels "matches" the
         * screen everywhere but under the outline (scroll_plan). */
        turn(25000000ull);
        snprintf(row_step,sizeof(row_step),"%s: outline over a plain page, step %u",row_name,k); wm_frame(row_step);
    }
    for (unsigned k=0;k<4;k++) { app_scroll(a,k&1 ? -9 : 13,k*0x050505u); step("content scrolls"); }
    int v=wm_v3_window(DESK_V3_OVERLAPPED,-1,100,180,300,260); assert(v>=0); step("v3 window reaching under the taskbar");
    for (unsigned k=0;k<4;k++) {
        uint32_t *p=(uint32_t *)d.clients[v].pixels; unsigned w=d.clients[v].info.width, h=d.clients[v].info.height;
        memmove(p,p+11*w,(size_t)(h-11)*w*4); for (unsigned q=(h-11)*w;q<h*w;q++) p[q]=(q*31u+k)&0xffffffu;
        any_damage(v,0,0,w,h); step("v3 window scrolls under the taskbar");
    }
    wm_end();
}
static void row_move(unsigned m)
{
    /* Moves: drag (coalesced), keyboard move, client DESK_MOVE, a window
     * taller than the work area dragged across the taskbar. */
    row_name="move"; wm_begin(m,1);
    struct fake_app *a=app_open("files",300,200,60,60); struct fake_app *b=app_open("ciuknote",240,160,200,140); step("two windows");
    struct desk_box f=d.clients[a->slot].frame; pointer_to(f.x+60,f.y+8); press(CIUKI_BUTTON_LEFT,1);
    for (unsigned k=0;k<6;k++) { pointer_to(d.mouse_x+17,d.mouse_y+9); if (k&1) step("dragged under the other window"); }
    press(CIUKI_BUTTON_LEFT,0); step("drop");
    desk_geometry_begin(&d,b->slot,0); for (unsigned k=0;k<4;k++) { tap(DU_KEY_LEFT); step("keyboard move"); } tap(DU_KEY_ENTER); step("keyboard move ends");
    struct ciuki_message msg; shell_message(&msg,DESK_MOVE,12); desk_put32(msg.data+4,10); desk_put32(msg.data+8,40); wm_send(a,&msg); step("client DESK_MOVE");
    int v=wm_v3_window(DESK_V3_OVERLAPPED,-1,60,40,300,410); assert(v>=0); step("window taller than the work area");
    for (unsigned k=0;k<5;k++) { desk_move(&d,v,d.clients[v].frame.x+23,d.clients[v].frame.y); step("tall window moved across the taskbar"); }
    wm_end();
}
/* fx-26: a drag while other things repaint (a DOS window playing, the
 * dragged program's own update, the clock, a notification sliding in): the
 * window moves by BitBlt with the damaged parts cut out and repainted
 * (Windows 98 SetWindowPos without SWP_NOCOPYBITS), never a software
 * recomposition of both outer boxes per frame; every frame still equals a
 * full redraw. */
static void row_move_damaged(unsigned m)
{
    row_name="move with other damage"; wm_begin(m,1);
    struct fake_app *a=app_open("files",300,200,60,60), *b=app_open("ciuknote",240,160,420,300); step("two windows");
    struct desk_box f=d.clients[a->slot].frame; pointer_to(f.x+60,f.y+8); press(CIUKI_BUTTON_LEFT,1);
    uint64_t copies=d.stats.accel_copies, copied=d.stats.copy_bytes;
    for (unsigned k=0;k<12;k++) {
        app_paint(b,10,10,60,20,k);                       /* another window repaints every frame */
        if (k%4==1) app_paint(a,5,30,40,10,k);             /* the dragged program updates itself */
        if (k==5) { uint64_t previous=d.realtime_second; d.realtime_second++; desk_shell_clock_damage(&d,previous); }
        if (k==7) desk_shell_notify(&d,"Volume","Volume: 40%",DU_DLG_ICON_INFO,SH_NOTIFY_VOLUME);
        pointer_to(d.mouse_x+(k<6 ? 13 : -9),d.mouse_y+7);
        if (k==3) pointer_to(d.mouse_x+5,d.mouse_y+2);    /* two motions in one frame */
        step("dragged while other windows paint");
    }
    press(CIUKI_BUTTON_LEFT,0); step("drop");
    /* The move was copied (RAM; engine presenters also in scanout). */
    assert(d.stats.copy_bytes>copied);
    if (m==WM_ENGINE || m==WM_CACHE) assert(d.stats.accel_copies>copies);
    wm_end();
}
static void row_show_hide(unsigned m)
{
    /* ShowWindow: SW_HIDE exposes, SW_SHOW/activation paints, also when the
     * shown window was already the highest in the stacking. */
    row_name="show/hide"; wm_begin(m,1);
    struct fake_app *a=app_open("files",320,220,40,50); (void)a;
    int v=wm_v3_window(DESK_V3_OVERLAPPED,-1,120,90,260,170); assert(v>=0); step("v3 window over a native one");
    struct desk_box f=d.clients[v].frame;
    wm_v3_place_at(v,f.x,f.y,d.clients[v].info.width,d.clients[v].info.height,2); assert(d.clients[v].minimized==2); step("SW_HIDE");
    wm_v3_activate(v,1); assert(!d.clients[v].minimized); step("activated again (SetForegroundWindow on a hidden window)");
    wm_v3_place_at(v,f.x,f.y,d.clients[v].info.width,d.clients[v].info.height,1); step("SW_MINIMIZE");
    wm_v3_activate(v,1); step("restored by activation");
    wm_v3_place_at(v,f.x+40,f.y+20,d.clients[v].info.width,d.clients[v].info.height,0); step("SetWindowPos move");
    int palette=wm_v3_window(DESK_V3_TOOL|DESK_V3_TOPMOST,-1,360,200,160,100); assert(palette>=0); step("topmost tool palette");
    struct desk_box fa=d.clients[a->slot].frame; click(fa.x+fa.w/2,fa.y+8,CIUKI_BUTTON_LEFT); assert(d.focus==a->slot); step("a normal window activated under it");
    struct desk_box pf=d.clients[palette].frame;
    wm_v3_place_at(palette,pf.x,pf.y,d.clients[palette].info.width,d.clients[palette].info.height,2); step("palette hidden (SW_HIDE)");
    wm_v3_activate(palette,1); assert(!d.clients[palette].minimized && d.focus==palette); step("palette shown by activation, still the highest window");
    int popup=wm_v3_window(DESK_V3_POPUP|DESK_V3_TOPMOST,v,f.x+30,f.y+60,120,80); assert(popup>=0); step("owned popup (menu)");
    wm_v3_close(popup); step("popup destroyed: saved bits restored by repaint");
    wm_end();
}
static void row_minimize(unsigned m)
{
    row_name="minimise/restore"; wm_begin(m,1);
    struct fake_app *a=app_open("files",320,220,40,50); struct fake_app *b=app_open("calc",200,140,200,120); (void)a;
    struct desk_box min=desk_min_box(&d.clients[b->slot]); click(min.x+2,min.y+2,CIUKI_BUTTON_LEFT); step("minimised");
    for (unsigned k=0;k<8;k++) { turn(25000000ull); wm_frame("minimise outline"); }
    struct desk_box t=desk_task_button(&d,b->slot); click(t.x+5,t.y+5,CIUKI_BUTTON_LEFT); assert(!d.clients[b->slot].minimized); step("restored from its task button");
    for (unsigned k=0;k<8;k++) { turn(25000000ull); wm_frame("restore outline"); }
    struct desk_box z=desk_shade_box(&d.clients[b->slot]); click(z.x+2,z.y+2,CIUKI_BUTTON_LEFT); step("maximised");
    for (unsigned k=0;k<8;k++) { turn(25000000ull); wm_frame("zoom outline"); }
    desk_collapse(&d,a->slot); step("shaded"); desk_collapse(&d,a->slot); step("unshaded");
    wm_end();
}
static void row_close(unsigned m)
{
    /* WM_CLOSE: the caption and task button say "Closing..." while the
     * program decides; a declined close restores both; DESK_CLOSE exposes. */
    row_name="close"; wm_begin(m,1);
    struct fake_app *a=app_open("files",320,220,40,50); struct fake_app *b=app_open("ciuknote",260,170,140,110);
    key(0x38,1); tap(DU_KEY_F4); key(0x38,0); assert(d.clients[b->slot].closing); step("Alt+F4: Closing...");
    app_close_ack(b,0); assert(!d.clients[b->slot].closing); step("the program declined the close");
    struct desk_box x=desk_close_box(&d.clients[b->slot]); click(x.x+2,x.y+2,CIUKI_BUTTON_LEFT); step("close button");
    app_close(b); step("window destroyed, the one beneath exposed");
    app_close(a); step("last window destroyed");
    wm_end();
}
static void row_death(unsigned m)
{
    row_name="process death"; wm_begin(m,1);
    struct fake_app *a=app_open("files",320,220,40,50); struct fake_app *b=app_open("calc",200,140,150,100); (void)a;
    app_exit(b); step("a program crashed (reaped)");
    int owner=wm_v3_window(DESK_V3_OVERLAPPED,-1,200,120,260,170); int owned=wm_v3_window(DESK_V3_OVERLAPPED,owner,260,180,180,110);
    assert(owner>=0 && owned>=0); step("Win32 owner and owned window");
    desk_drop_client(&d,v3_root); step("the Win32 server's channel broke");
    wm_end();
}
static void row_activation(unsigned m)
{
    /* WM_ACTIVATE/WM_SETFOCUS/WM_KILLFOCUS: captions, task buttons, the
     * caret and focus ring of the Run dialog, the desktop icon outline. */
    row_name="activation"; wm_begin(m,1);
    int run=desk_internal_open(&d,DESK_INTERNAL_RUN); assert(run>=0); step("Run dialog focused (caret)");
    struct fake_app *a=app_open("files",300,200,200,150); assert(d.focus==a->slot); step("a program window takes the focus");
    struct desk_box c=d.clients[run].frame; click(c.x+c.w/2,c.y+8,CIUKI_BUTTON_LEFT); assert(d.focus==run); step("Run dialog clicked");
    key(0x38,1); tap(DU_KEY_TAB); key(0x38,0); step("Alt+Tab");
    key(0x38,1); tap(DU_KEY_ESC); key(0x38,0); step("Alt+Esc");
    click(600,300,CIUKI_BUTTON_LEFT); step("desktop clicked: no active window");
    wm_end();
}
static void row_caption(unsigned m)
{
    row_name="caption"; wm_begin(m,1);
    struct fake_app *a=app_open("files",320,220,40,50); struct fake_app *b=app_open("calc",200,140,260,160); (void)b;
    app_title(a,"Files - Documents"); step("WM_SETTEXT");
    a->hung=1; turn(1100000000ull); turn(5100000000ull); assert(d.clients[a->slot].unresponsive); step("Not responding");
    a->hung=0; app_answer_pings(); step("responding again");
    struct desk_box x=desk_close_box(&d.clients[b->slot]); pointer_to(x.x+2,x.y+2); step("title button hot");
    press(CIUKI_BUTTON_LEFT,1); step("title button pressed"); pointer_to(300,300); press(CIUKI_BUTTON_LEFT,0); step("released elsewhere");
    wm_end();
}
static void row_menus(unsigned m)
{
    row_name="menus"; wm_begin(m,1);
    struct fake_app *a=app_open("files",320,220,40,50); (void)a;
    click(DESK_START_X+DESK_START_W/2,DESK_START_Y+DESK_START_H/2,CIUKI_BUTTON_LEFT); step("Start menu");
    struct sh_menu *menu=&d.sh.menus[0];
    for (int k=0;k<menu->count;k++) if (menu->items[k].action==SH_ACT_SUBMENU) {
        int y=menu->box.y+3; for (int j=0;j<k;j++) y+=menu->items[j].flags&SH_ITEM_SEPARATOR ? SH_SEPARATOR : menu->row_h;
        pointer_to(menu->box.x+40,y+4); step("submenu opened by hover"); break;
    }
    tap(DU_KEY_DOWN); step("menu key down"); tap(DU_KEY_RIGHT); step("menu key right"); tap(DU_KEY_LEFT); step("menu key left");
    tap(DU_KEY_ESC); tap(DU_KEY_ESC); step("menus closed");
    click(560,300,CIUKI_BUTTON_RIGHT); step("desktop context menu"); click(560,10+DESK_BAR,CIUKI_BUTTON_LEFT); step("dismissed");
    struct desk_box t=desk_task_button(&d,a->slot); click(t.x+4,t.y+4,CIUKI_BUTTON_RIGHT); step("task button menu"); tap(DU_KEY_ESC); step("dismissed");
    wm_end();
}
static void row_tooltip(unsigned m)
{
    row_name="tooltip"; wm_begin(m,1);
    (void)app_open("files",320,220,40,50); (void)app_open("calc",200,140,300,60); (void)app_open("ciuknote",220,140,200,200);
    struct fake_app *a=app_open("terminal",220,140,120,250);
    struct desk_box t=desk_task_button(&d,a->slot); pointer_to(t.x+t.w/2,t.y+t.h/2); step("hover the last task button");
    turn(700000000ull); assert(d.sh.tooltip); step("tooltip shown");
    struct fake_app *b=app_open("tasks",200,140,260,160); (void)b; wm_frame("tooltip: task buttons re-laid out (same turn)");
    turn(16666667ull); step("tooltip re-anchored");
    pointer_to(300,300); step("tooltip hidden");
    wm_end();
}
static void row_popups(unsigned m)
{
    row_name="tray popups"; wm_begin(m,1);
    struct fake_app *a=app_open("files",400,260,200,40); (void)a;
    struct desk_box v=desk_shell_tray_icon_box(&d,SH_TRAY_VOLUME); click(v.x+4,v.y+4,CIUKI_BUTTON_LEFT); assert(d.sh.volume_open); step("volume popup");
    tap(DU_KEY_UP); step("volume changed"); tap(DU_KEY_ESC); step("volume popup closed");
    desk_shell_notify(&d,"Network","Cable connected",DU_DLG_ICON_INFO,SH_NOTIFY_DETAILS); step("notification");
    for (unsigned k=0;k<12;k++) { turn(16666667ull); wm_frame("notification slides in"); }
    turn(8100000000ull); step("notification expired");
    wm_end();
}
static void row_switcher(unsigned m)
{
    row_name="switcher"; wm_begin(m,1);
    (void)app_open("files",300,200,40,50); (void)app_open("calc",200,140,260,160); (void)app_open("ciuknote",260,170,120,220);
    key(0x38,1); tap(DU_KEY_TAB); step("Alt+Tab switcher"); tap(DU_KEY_TAB); step("next"); key(0x38,0); step("switched");
    wm_end();
}
static void row_internal(unsigned m)
{
    row_name="built-in windows"; wm_begin(m,1);
    int about=desk_internal_open(&d,DESK_INTERNAL_ABOUT); assert(about>=0); step("About");
    struct desk_box c=desk_content(&d.clients[about]); click(c.x+8+80,c.y+12,CIUKI_BUTTON_LEFT); step("About > System");
    struct fake_app *a=app_open("files",280,140,340,290); step("a window opens: About counts it");
    app_close(a); step("a window closes");
    desk_shell_message(&d,"Programs","A message.",1); step("message box");
    desk_shell_message(&d,"Programs","A second message replaces the first.",0); step("message replaced");
    wm_end();
}
static void row_fault(unsigned m)
{
    /* G13: the native program fault dialog is a message window with a Details panel. */
    row_name="native fault dialog"; wm_begin(m,1);
    struct ciuki_w32_fault f={.kind=W32_REPORT_NATIVE_FAULT,.pid=42,.tid=43,.code=14u|6u<<8,.eip=0x401234,.offset=0x10,.valid=3};
    strcpy(f.exe,"crasher"); f.stack[0]=0xdeadbeef; f.stack[1]=0x01020304;
    desk_w32_fault_dialog(&d,&f); step("illegal operation dialog");
    tap(DU_KEY_TAB); step("Details panel opens");
    tap(DU_KEY_TAB); step("Details panel closes");
    tap(DU_KEY_ENTER); step("Close");
    wm_end();
}
static void row_icons(unsigned m)
{
    row_name="desktop icons"; wm_begin(m,1);
    link_variant=1; d.v3_links_next=0; turn(16666667ull); step("a shortcut appears on the desktop");
    struct desk_box icon=d.sh.icons[0].box; click(icon.x+icon.w/2,icon.y+20,CIUKI_BUTTON_LEFT); step("icon selected");
    link_variant=2; d.v3_links_next=0; turn(16666667ull); step("shortcuts change while an icon is selected");
    link_variant=3; d.v3_links_next=0; turn(16666667ull); step("shortcuts removed");
    wm_end();
}
static void row_fullscreen(unsigned m)
{
    /* A DOS game full screen: the display returns (display_poll, RESYNC). */
    row_name="full-screen return"; wm_begin(m,1);
    (void)app_open("dosbox",320,200,60,60); step("DOS window");
    for (size_t k=0;k<(size_t)d.width*d.height;k++) scanout[k]=(uint32_t)(k*2654435761u);
    desk_accel_reset(&d,55); struct ciuki_input_event e={.type=CIUKI_INPUT_RESYNC,.monotonic_ns=d.now_ns}; desk_input(&d,&e);
    step("display returned: everything repainted");
    wm_end();
}
static void row_gfx(unsigned m)
{
    row_name="graphics presenter"; wm_begin(m,1);
    struct fake_app *a=app_open("gfxdemo",320,240,60,60);
    for (unsigned k=0;k<6;k++) { app_gfx(a); step("scaled scene frame"); }
    desk_zoom(&d,a->slot); step("maximised"); app_gfx(a); step("frame while stretched");
    wm_end();
}
static void row_topbar(unsigned m)
{
    /* Win32 tool windows and popups may cover the top bar (it is drawn under
     * the windows); their updates there must reach the screen. */
    row_name="top bar overlap"; wm_begin(m,1);
    int v=wm_v3_window(DESK_V3_TOOL,-1,300,4,240,80); assert(v>=0); step("tool window over the top bar");
    for (unsigned k=0;k<3;k++) { fill_pattern(v,k*0x0a0a0au); any_damage(v,0,0,d.clients[v].info.width,d.clients[v].info.height); step("tool window updates"); }
    int p=wm_v3_window(DESK_V3_POPUP,-1,40,2,160,60); assert(p>=0); fill_pattern(p,0x404040u); any_damage(p,0,0,160,60); step("popup over the top bar");
    wm_end();
}
static void row_theme(unsigned m)
{
    row_name="theme"; wm_begin(m,1);
    (void)app_open("files",300,200,40,50);
    for (unsigned k=0;k<CFG_THEMES;k++) { desk_theme_use(cfg_theme_names[k]); desk_theme_changed(&d); step("theme changed"); }
    wm_end();
}
static void row_strips(unsigned m)
{
    /* An exposure repaints any strip; the result must equal a whole repaint
     * (text with negative bearings, ascenders above the capital line). */
    row_name="exposure strips"; wm_begin(m,1);
    (void)desk_internal_open(&d,DESK_INTERNAL_ABOUT); (void)app_open("files",300,200,330,200);
    desk_shell_notify(&d,"Notice","Typography: jumping quickly, glyphs dip below",DU_DLG_ICON_INFO,SH_NOTIFY_DETAILS);
    click(DESK_START_X+DESK_START_W/2,DESK_START_Y+DESK_START_H/2,CIUKI_BUTTON_LEFT); step("scene");
    uint32_t *full=calloc((size_t)d.width*d.height,4), *parts=calloc((size_t)d.width*d.height,4);
    struct desktop *o=malloc(sizeof(d)); assert(full && parts && o);
    *o=d; o->hw_cursor=1; o->pixels=full; desk_composite(o,(struct desk_box){0,0,(int)d.width,(int)d.height});
    for (int width=1;width<=5;width+=2) for (int x0=0;x0<width;x0++) {
        memcpy(parts,full,(size_t)d.width*d.height*4);
        for (int x=x0;x<(int)d.width;x+=width*2) for (int y=0;y<(int)d.height;y++) for (int k=0;k<width && x+k<(int)d.width;k++) parts[(size_t)y*d.width+x+k]=0xff00ffu;
        *o=d; o->hw_cursor=1; o->pixels=parts;
        for (int x=x0;x<(int)d.width;x+=width*2) desk_composite(o,(struct desk_box){x,0,width,(int)d.height});
        for (size_t k=0;k<(size_t)d.width*d.height;k++) if (parts[k]!=full[k]) {
            fprintf(stderr,"%s %s: strip width %d repaints pixel %zu,%zu as %06x, a whole repaint gives %06x\n",
                    wm_names[wm_mode],row_name,width,k%d.width,k/d.width,parts[k],full[k]);
            abort();
        }
        *o=d; o->hw_cursor=1; o->pixels=parts; memcpy(parts,full,(size_t)d.width*d.height*4);
        for (int y=x0;y<(int)d.height;y+=width*2) for (int x=0;x<(int)d.width;x++) for (int k=0;k<width && y+k<(int)d.height;k++) parts[(size_t)(y+k)*d.width+x]=0xff00ffu;
        for (int y=x0;y<(int)d.height;y+=width*2) desk_composite(o,(struct desk_box){0,y,(int)d.width,width});
        for (size_t k=0;k<(size_t)d.width*d.height;k++) if (parts[k]!=full[k]) {
            fprintf(stderr,"%s %s: band height %d repaints pixel %zu,%zu as %06x, a whole repaint gives %06x\n",
                    wm_names[wm_mode],row_name,width,k%d.width,k/d.width,parts[k],full[k]);
            abort();
        }
    }
    free(full); free(parts); free(o);
    wm_end();
}
static int fake_lease(struct desktop *dd, uint32_t pid, int acquire, uint32_t w, uint32_t h, uint32_t bpp, uint32_t generation)
{
    (void)pid; (void)bpp;
    if (acquire) dd->v3_lease_tuple=(struct desk_lease_tuple){.token=7,.generation=generation,.mode=0x101,.scale=1,
        .surface={w,h,w,w*h,1},.native_width=640,.native_height=480,.native_bpp=32};
    return 0;
}
static void row_lease(unsigned m)
{
    /* A Win32 DirectDraw program owns the display (f5-08 lease); when it
     * ends or loses activation the whole desktop is repainted. */
    if (m!=WM_SOFTWARE) return; /* the lease resets the engine generation, which the engine models pin */
    row_name="display lease"; wm_begin(m,1); d.v3_lease=fake_lease;
    (void)app_open("files",300,200,40,50);
    int v=wm_v3_window(DESK_V3_OVERLAPPED,-1,200,120,240,160); assert(v>=0 && d.focus==v); step("Win32 window");
    struct desk_client *c=&d.clients[v]; struct ciuki_message msg; v3msg(&msg,DESK_V3_LEASE,16,c->v3_id,c->v3_generation);
    desk_put32(msg.data+16,1); desk_put32(msg.data+20,320); desk_put32(msg.data+24,200); desk_put32(msg.data+28,8);
    assert(!desk_v3_receive(&d,v3_root,&msg) && d.v3_lease_state==1);
    for (size_t k=0;k<(size_t)d.width*d.height;k++) scanout[k]=(uint32_t)(k*40503u);
    assert(!desk_redraw(&d)); /* composition waits while leased */
    v3msg(&msg,DESK_V3_LEASE,16,c->v3_id,c->v3_generation);
    assert(!desk_v3_receive(&d,v3_root,&msg) && !d.v3_lease_state); step("lease released: desktop repainted");
    wm_end();
}
static void row_selection(unsigned m)
{
    /* fx-19 selection: click, Ctrl and Shift clicks, the rubber band (and a
     * band ended by an icon rebuild: the merge failure of fx-15b), keyboard
     * selection; each changes icon looks and the focus outline. */
    row_name="selection"; wm_begin(m,1);
    (void)app_open("files",280,180,330,60);
    link_variant=2; d.v3_links_next=0; turn(16666667ull); step("two shortcuts on the desktop");
    int x,y; icon_point(0,&x,&y); click(x,y,CIUKI_BUTTON_LEFT); step("click selects");
    turn(600000000ull); icon_point(1,&x,&y); key(0x1d,1); click(x,y,CIUKI_BUTTON_LEFT); key(0x1d,0); step("Ctrl+click adds");
    turn(600000000ull); icon_point(3,&x,&y); key(0x2a,1); click(x,y,CIUKI_BUTTON_LEFT); key(0x2a,0); step("Shift+click selects the range");
    pointer_to(150,300); press(CIUKI_BUTTON_LEFT,1); step("band starts");
    pointer_to(60,200); step("band grows"); pointer_to(20,90); step("band covers icons");
    press(CIUKI_BUTTON_LEFT,0); step("band ends");
    pointer_to(160,320); press(CIUKI_BUTTON_LEFT,1); pointer_to(40,150); step("second band");
    link_variant=3; d.v3_links_next=0; turn(16666667ull); step("icons rebuilt during the band: the band ends");
    press(CIUKI_BUTTON_LEFT,0); step("release");
    click(200,400,CIUKI_BUTTON_LEFT); key(0x1d,1); tap(0x1e); key(0x1d,0); step("Ctrl+A");
    tap(DU_KEY_DOWN); step("arrow"); key(0x2a,1); tap(DU_KEY_DOWN); key(0x2a,0); step("Shift+arrow");
    key(0x1d,1); tap(DU_KEY_UP); tap(DU_KEY_SPACE); key(0x1d,0); step("Ctrl+arrow, Ctrl+Space");
    wm_end();
}
static void row_drag(unsigned m)
{
    /* fx-19 drag and drop: the drag image follows the pointer over windows,
     * the taskbar and the desktop; the drop target is highlighted; hovering a
     * task button restores its window; Esc and the right-drag menu end it. */
    row_name="drag and drop"; wm_begin(m,1);
    struct fake_app *files=app_open("files",280,180,330,50);
    struct fake_app *note=app_open("ciuknote",240,150,330,270); app_accept_files(note,1);
    struct fake_app *calc=app_open("calc",200,120,150,200);
    struct desk_box min=desk_min_box(&d.clients[calc->slot]); click(min.x+2,min.y+2,CIUKI_BUTTON_LEFT);
    for (unsigned k=0;k<8;k++) turn(25000000ull);
    step("three windows, one minimised");
    int x,y; icon_point(0,&x,&y); pointer_to(x,y); press(CIUKI_BUTTON_LEFT,1);
    pointer_to(x+12,y+9); step("icon drag starts: drag image");
    pointer_to(400,120); step("image over a window");
    struct desk_box t=desk_task_button(&d,calc->slot); pointer_to(t.x+6,t.y+6); step("image over a task button");
    turn(1100000000ull); step("hover restores the minimised window");
    pointer_to(220,330); step("image over the desktop");
    tap(DU_KEY_ESC); step("Esc cancels");
    press(CIUKI_BUTTON_LEFT,0); step("release after cancel");
    icon_point(1,&x,&y); pointer_to(x,y); press(CIUKI_BUTTON_RIGHT,1); pointer_to(x+40,y+60); pointer_to(600,430); step("right drag");
    press(CIUKI_BUTTON_RIGHT,0); assert(d.sh.dnd.menu); step("right-drag menu"); tap(DU_KEY_ESC); step("menu cancelled");
    icon_point(1,&x,&y); pointer_to(x,y); press(CIUKI_BUTTON_LEFT,1); pointer_to(x+50,y+30); pointer_to(200,300); step("reposition drag");
    press(CIUKI_BUTTON_LEFT,0); step("icon dropped at a new place");
    assert(files_drag_begin(files,1)); step("Files drags two items");
    icon_point(2,&x,&y); pointer_to(x,y); step("over the Trash: target highlighted");
    pointer_to(200,300); step("over the desktop: highlight removed");
    struct desk_box nb=desk_content(&d.clients[note->slot]); pointer_to(nb.x+30,nb.y+30); step("over a WM_DROPFILES window");
    press(CIUKI_BUTTON_LEFT,0); step("dropped on the window (DROPFILES, no transfer)");
    assert(files_drag_begin(files,2)); pointer_to(200,10); step("right drag from Files over the top bar");
    tap(DU_KEY_ESC); press(CIUKI_BUTTON_RIGHT,0); step("cancelled");
    wm_end();
}
static void row_startup(unsigned m)
{
    row_name="startup programs"; wm_begin(m,1);
    struct fake_app *a=app_open("files",300,180,80,100); step("startup program shows a window");
    struct fake_app *b=app_open("ciuknote",350,240,210,160); step("next startup program activates");
    app_close(b); step("startup program closes and exposes previous window");
    app_close(a); step("startup batch windows closed");
    wm_end();
}
static int saver_monitor_error;
static unsigned saver_monitor_calls;
static int saver_monitor(int state)
{
    assert(state==-1 || state==1 || state==2);
    saver_monitor_calls++; return saver_monitor_error;
}
static struct ciuki_screen_control saver_shared;
static int saver_control(struct ciuki_screen_control *q)
{
    assert(q->version==CIUKI_SCREEN_VERSION && q->size==sizeof(*q));
    if (q->action==CIUKI_SCREEN_CONFIG) {
        saver_shared=*q; saver_shared.sequence++;
    } else if (q->action==CIUKI_SCREEN_STATUS) {
        saver_shared.running=(unsigned)q->value;
        if (q->sequence==saver_shared.sequence) saver_shared.flags=saver_shared.command=0;
    } else assert(!q->action);
    *q=saver_shared; return 0;
}
static void row_saver(unsigned m)
{
    row_name="G10 saver"; wm_begin(m,1);
    (void)app_open("ciuknote",240,160,100,100); step("application before saver");
    uint32_t *portrait=malloc(256*256*4); assert(portrait);
    for (unsigned i=0;i<256*256;i++) portrait[i]=i*31u&0xffffffu;
    d.portrait=portrait;
    assert(desk_saver_start(&d,3)==-EINVAL);
    for (unsigned k=0;k<3;k++) {
        assert(!desk_saver_start(&d,k)); step("preview start");
        for (unsigned frame=0;frame<3;frame++) { turn(50000000ull); wm_frame("animated saver"); }
        unsigned sent=fake_sent;
        struct ciuki_input_event e={.type=CIUKI_INPUT_BUTTON,.code=CIUKI_BUTTON_LEFT,.value=1};
        desk_input(&d,&e); assert(!d.saver_active); step("input dismisses and restores desktop");
        for (unsigned n=sent;n<fake_sent;n++) assert(fake_messages[n].data[1]!=DESK_BUTTON);
    }
    d.sh.cfg.saver_enabled=1; d.sh.cfg.saver_seconds=1;
    d.now_ns=wm_clock; d.saver_idle=d.now_ns; turn(999000000ull); assert(!d.saver_active);
    turn(1000000ull); assert(d.saver_active); wm_frame("idle activation");
    tap(DU_KEY_ESC); step("keyboard dismisses");
    d.v3_lease_state=1; d.saver_idle=d.now_ns-2000000000ull;
    desk_saver_tick(&d); assert(!d.saver_active); assert(desk_saver_start(&d,0)==-EBUSY);
    d.v3_lease_state=0; d.sh.cfg.saver_enabled=0;
    d.monitor_power=saver_monitor; saver_monitor_error=0; saver_monitor_calls=0;
    d.sh.cfg.monitor_seconds=1; d.saver_idle=d.now_ns-2000000000ull;
    desk_saver_tick(&d); assert(d.monitor_off && saver_monitor_calls==1);
    uint64_t before_present=d.presents;
    desk_damage_add(&d.damage,(struct desk_box){0,0,640,480},640,480);
    assert(!desk_redraw(&d) && d.presents==before_present && d.damage.count);

    saver_monitor_error=-EIO;
    struct ciuki_input_event motion={.type=CIUKI_INPUT_MOTION,.value=1};
    desk_input(&d,&motion); assert(d.monitor_off);
    saver_monitor_error=0; desk_input(&d,&motion); assert(!d.monitor_off);
    step("monitor wake repaints scene");
    saver_monitor_error=-EOPNOTSUPP; d.saver_idle=d.now_ns-2000000000ull;
    desk_saver_tick(&d); assert(!d.monitor_off && d.monitor_failed);
    unsigned calls=saver_monitor_calls; desk_saver_tick(&d); assert(calls==saver_monitor_calls);
    d.sh.cfg.monitor_seconds=0;
    d.screen_control=saver_control; memset(&saver_shared,0,sizeof(saver_shared));
    desk_saver_config(&d); assert(saver_shared.timeout==1 && !saver_shared.enabled);
    saver_shared.enabled=1; saver_shared.timeout=120;
    saver_shared.command=0xf140; saver_shared.argument=2;
    saver_shared.flags=CIUKI_SCREEN_PERSIST; saver_shared.sequence++;
    d.saver_poll=0; desk_saver_tick(&d);
    assert(d.saver_active && d.saver_kind==2 && d.sh.want_save);
    assert(!saver_shared.flags && !saver_shared.command);
    step("shared SC_SCREENSAVE request");
    desk_input(&d,&motion); desk_saver_tick(&d); assert(!saver_shared.running);
    saver_shared.running=1; d.saver_poll=0; desk_saver_tick(&d);
    assert(d.saver_external_running && desk_saver_start(&d,0)==-EBUSY);
    saver_shared.running=0; d.saver_poll=0; desk_saver_tick(&d);
    assert(!d.saver_external_running);
    saver_shared.inhibited=1; d.saver_poll=0; desk_saver_tick(&d);
    assert(d.saver_display_inhibited && desk_saver_start(&d,0)==-EBUSY);
    saver_shared.inhibited=0; d.saver_poll=0; desk_saver_tick(&d);
    assert(!d.saver_display_inhibited);
    d.sh.cfg.saver_enabled=0; d.screen_control=NULL;
    d.portrait=NULL; free(portrait);
    wm_end();
}
static void row_network_tools(unsigned m)
{
    row_name="G26 browser windows and Internet dialogs";wm_begin(m,1);
    int window=wm_v3_window(DESK_V3_OVERLAPPED,-1,160,72,260,180);assert(window>=0);
    struct desk_client *c=&d.clients[window];
    struct ciuki_message request;const char *url="http://example.test/download";unsigned n=(unsigned)strlen(url);
    v3msg(&request,DESK_LAUNCH,4+n,c->v3_id,c->v3_generation);
    desk_put32(request.data+16,n);memcpy(request.data+20,url,n);
    snprintf(d.clients[v3_root].application,sizeof(d.clients[v3_root].application),"other");
    assert(desk_v3_receive(&d,v3_root,&request)==-EPERM);step("untrusted launch refused");
    snprintf(d.clients[v3_root].application,sizeof(d.clients[v3_root].application),"netsurf");
    assert(!desk_v3_receive(&d,v3_root,&request));assert(last_app);step("new browser launch queued");
    struct fake_app *child=last_app;app_spawned(child);app_create_window(child);step("new browser window visible");
    app_paint(child,0,0,(unsigned)child->w,(unsigned)child->h,0x13579);step("new browser first frame");
    /* Application widgets/dialogs publish damage through the common client
     * path. Their state transitions are exercised by G26's app host tests. */
    app_paint(child,12,24,120,64,0x24680);step("download Save dialog opens");
    app_paint(child,12,24,120,64,0x13579);step("Save dialog closes");
    struct fake_app *panel=app_open("control",320,220,220,100);step("Internet Properties opens");
    app_paint(panel,20,50,240,120,0xabcdef);step("Internet Properties apply status");
    wm_end();
}
/* ---- G4: the taskbar menu, window arrangement and the Windows-key shortcuts ---- */
static void win_chord(int code) { key(0x15b,1); tap(code); key(0x15b,0); }
static void bar_right_click(int top) { click(top ? 300 : (int)d.width-60,top ? 10 : (int)d.height-16,CIUKI_BUTTON_RIGHT); }
static void menu_item_click(int k)
{
    struct sh_menu *menu=&d.sh.menus[0]; int y=menu->box.y+3;
    for (int j=0;j<k;j++) y+=menu->items[j].flags&SH_ITEM_SEPARATOR ? SH_SEPARATOR : menu->row_h;
    pointer_to(menu->box.x+20,y+menu->row_h/2); press(CIUKI_BUTTON_LEFT,1); press(CIUKI_BUTTON_LEFT,0);
}
static void row_taskbar_menu(unsigned m)
{
    /* The empty taskbar and the empty top bar open the Windows 98 taskbar
     * menu (TrackPopupMenu at the cursor, above the bar when it would leave
     * the screen); hot rows and the menu's box are repainted; an item runs
     * through the mouse or its access key and the menu goes. */
    row_name="taskbar menu"; wm_begin(m,1);
    struct fake_app *a=app_open("files",300,200,40,60); struct fake_app *b=app_open("calc",220,150,260,140); (void)a; (void)b;
    bar_right_click(0); assert(d.sh.levels && d.sh.menus[0].kind==SH_MENU_TASKBAR); step("menu on the empty taskbar");
    pointer_to(d.sh.menus[0].box.x+20,d.sh.menus[0].box.y+30); step("hot row"); pointer_to(d.sh.menus[0].box.x+20,d.sh.menus[0].box.y+60); step("next hot row");
    tap(DU_KEY_ESC); step("dismissed");
    bar_right_click(1); assert(d.sh.levels); step("menu on the top bar");
    click(500,300,CIUKI_BUTTON_LEFT); step("dismissed by a click");
    bar_right_click(0); menu_item_click(0); assert(!d.sh.levels); step("Cascade Windows");
    bar_right_click(0); assert(d.sh.menus[0].count==7); step("menu with Undo Cascade");
    menu_item_click(1); step("Tile Windows Horizontally");
    bar_right_click(0); tap(0x2f); assert(!d.sh.levels); step("Tile Windows Vertically by its access key");
    bar_right_click(0); tap(0x32); step("Minimize All Windows by its access key");
    bar_right_click(0); tap(0x16); step("Undo Minimize All by its access key");
    bar_right_click(0); tap(0x13); step("Properties launches the Control Panel");
    wm_end();
}
static void row_arrange(unsigned m)
{
    /* CascadeWindows / TileWindows: SetWindowPos on every window in one
     * pass (EndDeferWindowPos): the old and the new boxes of all of them
     * are repainted; maximised and shaded windows are restored first; a
     * window under a modal question, a minimised one and a topmost one keep
     * their place; Undo puts everything back. */
    row_name="arrange"; wm_begin(m,1);
    struct fake_app *a=app_open("files",300,200,40,60), *b=app_open("calc",220,150,260,140), *c=app_open("ciuknote",260,180,120,220);
    struct fake_app *e=app_open("terminal",240,160,360,80);
    int v=wm_v3_window(DESK_V3_OVERLAPPED|DESK_V3_TOPMOST,-1,400,300,200,150); assert(v>=0); step("four windows and a topmost Win32 window");
    desk_zoom(&d,c->slot); desk_collapse(&d,e->slot); step("one maximised, one shaded");
    assert(desk_arrange(&d,DESK_ARRANGE_CASCADE)>=3); step("Cascade Windows");
    assert(desk_arrange_undo(&d)>0); step("Undo Cascade");
    assert(desk_arrange(&d,DESK_ARRANGE_TILE_V)>=3); step("Tile Windows Vertically");
    for (unsigned k=0;k<8;k++) { app_paint(a,0,0,(unsigned)a->w,(unsigned)a->h,k*0x090909u); app_paint(b,0,0,40,40,k); turn(25000000ull); wm_frame("windows repaint while tiled"); }
    assert(desk_arrange(&d,DESK_ARRANGE_TILE_H)>=3); step("Tile Windows Horizontally");
    app_paint(a,0,0,(unsigned)a->w,(unsigned)a->h,0x3a3a3a); step("the first frame after the resize");
    desk_minimize(&d,b->slot); assert(desk_arrange(&d,DESK_ARRANGE_CASCADE)>=1); step("Cascade with a minimised window");
    desk_focus(&d,a->slot); desk_shell_message(&d,"Question","A modal question.",0); step("a window with a modal question");
    assert(desk_arrange(&d,DESK_ARRANGE_TILE_V)>=1); step("Tile: the owner and its question stay");
    desk_request_close(&d,d.focus); step("question closed");
    app_close(c); assert(desk_arrange_undo(&d)>=0); step("Undo after a window closed");
    wm_end();
}
static void row_minimize_all(unsigned m)
{
    /* Minimize All Windows, Show Desktop (Win+D toggles), Win+M and
     * Shift+Win+M, Alt+M: windows leave to the taskbar at once (no
     * caption animation), the desktop is active, the undo brings them back
     * with the window that was active. */
    row_name="minimize all"; wm_begin(m,1);
    struct fake_app *a=app_open("files",300,200,40,60), *b=app_open("calc",220,150,260,140), *c=app_open("ciuknote",260,180,120,220);
    (void)a; (void)b; (void)c;
    desk_shell_message(&d,"Owned","A dialog.",0); step("a dialog owned by the active window");
    win_chord(0x20); assert(d.focus<0 && !d.sh.levels); step("Win+D");
    win_chord(0x20); assert(d.focus>=0); step("Win+D again");
    win_chord(0x32); step("Win+M");
    key(0x2a,1); win_chord(0x32); key(0x2a,0); step("Shift+Win+M");
    click(DESK_START_X+DESK_START_W/2,DESK_START_Y+DESK_START_H/2,CIUKI_BUTTON_LEFT); assert(d.sh.levels); win_chord(0x20); assert(!d.sh.levels); step("Win+D with the Start menu open");
    key(0x38,1); tap(0x32); key(0x38,0); step("Alt+M on the desktop");
    desk_arrange_undo(&d); step("Undo Minimize All");
    int v=wm_v3_window(DESK_V3_OVERLAPPED,-1,300,200,180,120); assert(v>=0); step("a Win32 window");
    win_chord(0x20); step("Win+D with a Win32 window"); win_chord(0x20); step("and back");
    wm_end();
}
static void row_win_keys(unsigned m)
{
    /* The other Windows-key shortcuts, the taskbar's keyboard focus and
     * the access keys: each opens or moves something the screen shows. */
    row_name="windows keys"; wm_begin(m,1);
    struct fake_app *a=app_open("files",300,200,40,60), *b=app_open("calc",220,150,260,140), *c=app_open("ciuknote",260,180,120,220);
    (void)a; (void)b; (void)c;
    win_chord(0x13); assert(d.focus>=0 && d.clients[d.focus].internal==DESK_INTERNAL_RUN); step("Win+R");
    desk_request_close(&d,d.focus); step("Run closed");
    win_chord(0x21); step("Win+F: Find"); desk_request_close(&d,d.focus);
    win_chord(0x12); step("Win+E"); win_chord(0x200); step("Win+Break"); win_chord(0x3b); step("Win+F1");
    win_chord(DU_KEY_TAB); assert(d.sh.taskbar_key); step("Win+Tab: a task button in focus");
    win_chord(DU_KEY_TAB); step("Win+Tab again"); tap(DU_KEY_ENTER); step("Enter activates");
    key(0x1d,1); tap(DU_KEY_ESC); key(0x1d,0); assert(d.sh.levels); step("Ctrl+Esc");
    tap(DU_KEY_ESC); assert(d.sh.taskbar_key && d.sh.task_pick==DESK_CLIENTS); step("Esc: the Start button in focus");
    tap(DU_KEY_TAB); step("Tab: the first task button"); tap(DU_KEY_RIGHT); step("Right");
    tap(0x15d); assert(d.sh.levels); step("Menu key: the button's menu"); tap(DU_KEY_ESC); step("menu closed");
    tap(DU_KEY_HOME); tap(DU_KEY_ENTER); assert(d.sh.levels); step("Enter on Start"); tap(0x13); step("Run by its access key");
    desk_request_close(&d,d.focus); step("Run closed");
    key(0x38,1); tap(0x40); key(0x38,0); step("Alt+F6");
    click(600,300,CIUKI_BUTTON_LEFT); tap(0x15d); assert(d.sh.levels); step("Menu key on the desktop"); tap(DU_KEY_ESC);
    key(0x2a,1); tap(DU_KEY_F10); key(0x2a,0); step("Shift+F10"); tap(DU_KEY_ESC);
    tap(0x3d); step("F3: Find"); desk_request_close(&d,d.focus);
    wm_end();
}
static void row_caption_icon(unsigned m)
{
    /* The caption icon of the Ciuki Blue theme: a click opens the window
     * menu under it, a double click closes the window. */
    row_name="caption icon"; wm_begin(m,1);
    assert(!desk_theme_use("Ciuki Blue")); desk_theme_changed(&d); step("Ciuki Blue");
    struct fake_app *a=app_open("files",300,200,40,60), *b=app_open("calc",220,150,260,140); (void)b;
    struct desk_box icon=desk_caption_icon_box(&d,&d.clients[a->slot]); assert(icon.w==16);
    click(icon.x+8,icon.y+8,CIUKI_BUTTON_LEFT); assert(d.sh.levels && d.sh.menus[0].kind==SH_MENU_TASK); step("the window menu under the icon");
    tap(0x2d); step("Maximize by its access key"); desk_zoom(&d,a->slot); step("restored");
    icon=desk_caption_icon_box(&d,&d.clients[a->slot]);
    click(icon.x+8,icon.y+8,CIUKI_BUTTON_LEFT); click(icon.x+8,icon.y+8,CIUKI_BUTTON_LEFT); step("double click: Closing");
    app_close(a); step("destroyed");
    assert(!desk_theme_use("Platinum")); desk_theme_changed(&d); step("Platinum");
    wm_end();
}
static void (*const rows[])(unsigned)={row_create,row_stretched,row_scroll_overlays,row_move,row_show_hide,row_minimize,
    row_close,row_death,row_activation,row_caption,row_menus,row_tooltip,row_popups,row_switcher,row_internal,row_icons,
    row_fullscreen,row_gfx,row_topbar,row_theme,row_strips,row_lease,row_selection,row_drag,
    row_taskbar_menu, row_arrange, row_minimize_all, row_win_keys, row_caption_icon, row_startup,
    row_move_damaged, row_fault, row_saver, row_network_tools};
static void wm_rows(unsigned m)
{
    /* WM_ROW=<index> runs one row (the per-row evidence against older code).
     * Main keeps indexes 0..28; fx-26 move-with-other-damage is index 29. */
    const char *one=getenv("WM_ROW");
    for (unsigned k=0;k<sizeof(rows)/sizeof(*rows);k++) if (!one || (unsigned)atoi(one)==k) rows[k](m);
}
static void wm_dump(void)
{
    fprintf(stderr,"focus=%d levels=%d tooltip=%d switcher=%d notification=%d volume=%d anim=%d drag=%d copy=%d mouse=%d,%d icon_sel=%d\n",
        d.focus,d.sh.levels,d.sh.tooltip,d.sh.switcher,d.sh.notification.active,d.sh.volume_open,d.animation.active,d.drag,d.copy_pending,
        d.mouse_x,d.mouse_y,d.sh.icon_selected);
    fprintf(stderr,"fx-19 band=%d %d,%d-%d,%d dnd=%d menu=%d image=%d,%d,%dx%d kind=%d target=%d buttons=%u icon_drag=%d dragged=%d icons=%d:",d.sh.band,d.sh.band_x0,d.sh.band_y0,
        d.sh.band_x1,d.sh.band_y1,d.sh.dnd.active,d.sh.dnd.menu,d.sh.dnd.image.x,d.sh.dnd.image.y,d.sh.dnd.image.w,d.sh.dnd.image.h,d.sh.dnd.kind,d.sh.dnd.target,d.buttons,d.sh.icon_drag,d.sh.icon_dragged,d.sh.icon_count);
    for (int k=0;k<d.sh.icon_count;k++) fprintf(stderr," [%d %d,%d sel=%d]",k,d.sh.icons[k].box.x,d.sh.icons[k].box.y,d.sh.icons[k].selected);
    fprintf(stderr,"\n");
    fprintf(stderr,"cursor shape=%u loaded=%u hw=%d hw_shown=%d dirty=%d shown=%d at=%d,%d v3=%u hidden=%u retry=%u fallbacks=%u\n",d.cursor_shape,d.cursor_loaded,
        d.hw_cursor,d.hw_cursor_shown,d.cursor_dirty,shown,cursor_x,cursor_y,d.v3_cursor_id,d.v3_cursor_hidden,d.hw_cursor_retries,d.hw_cursor_fallbacks);
    for (int l=0;l<d.sh.levels;l++) fprintf(stderr," menu %d kind=%d box=%d,%d,%dx%d\n",l,d.sh.menus[l].kind,
        d.sh.menus[l].box.x,d.sh.menus[l].box.y,d.sh.menus[l].box.w,d.sh.menus[l].box.h);
    for (int k=0;k<DESK_CLIENTS;k++) { const struct desk_client *c=&d.clients[k];
        if (c->channel<0 && !c->internal) continue;
        fprintf(stderr," slot %d win=%d id=%u order=%llu frame=%d,%d,%dx%d min=%d col=%d zoom=%d int=%d v=%u pix=%d spawn=%d await=%d closing=%d unresp=%d title=%s stack=%u/%llu style=%u info=%ux%u owner=%u\n",
            k,c->window,c->window_id,(unsigned long long)c->order,c->frame.x,c->frame.y,c->frame.w,c->frame.h,c->minimized,c->collapsed,c->zoomed,
            c->internal,c->version,c->pixels!=NULL,c->spawn_pending,c->awaiting_frame,c->closing,c->unresponsive,c->title,
            d.stack_window[k],(unsigned long long)d.stack_order[k],c->v3_style,c->info.width,c->info.height,c->owner_id); }
}
static void report(int sig)
{ (void)sig; fprintf(stderr,"aborted in mode %s at %s\n",wm_names[wm_mode],step_name); wm_dump(); signal(SIGABRT,SIG_DFL); }
int main(int argc, char **argv)
{
    signal(SIGABRT,report);
    unsigned runs=argc>1 ? (unsigned)atoi(argv[1]) : 8;
    int only=argc>2 ? atoi(argv[2]) : -1;
    setvbuf(stdout,NULL,_IONBF,0);
    (void)command_redraw; (void)covered_scenario; (void)restack_scenario; (void)internal_scenario; (void)fuzz; (void)wm_shell;
    const char *rows_only=getenv("WM_ROWS_ONLY");
    const char *mode_only=getenv("WM_MODE");
    for (unsigned m=0;m<WM_MODES;m++) {
        if (mode_only && (unsigned)atoi(mode_only)!=m) continue;
        if (only<0) { wm_rows(m); printf("fx-15 %s: every row of the window-manager model identical to a full redraw\n",wm_names[m]); }
        if (rows_only) continue;
        for (unsigned run=0;run<runs;run++) if (only<0 || (unsigned)only==run) wm_fuzz(m,run);
        printf("fx-15 %s: %u randomized sequences identical to a full redraw\n",wm_names[m],runs);
    }
    printf("fx-15b fx-19 states compared: band=%u drag=%u files_drag=%u drag_menu=%u drop_highlight=%u multi_selection=%u frames\n",
           seen_band,seen_drag,seen_files_drag,seen_drag_menu,seen_drop_lit,seen_multi);
    printf("PASS fx-15 window model: %u frames identical to a full redraw\n",wm_frames);
    return 0;
}
