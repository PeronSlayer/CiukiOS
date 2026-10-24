/* SPDX-License-Identifier: MIT
 * Desktop protocol v2.1 client glue: handshake, surface, back buffer,
 * coalesced damage, close requests, focus releases and the event loop. */
#include "internal.h"
#include "completion.h"
#include "clipboard.h"
#include <poll.h>
#include <ciuki/channel.h>
#include <ciuki/surface.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef __linux__
#define DU_PROCESS_LOCAL _Thread_local /* host replay models processes with threads */
#else
#define DU_PROCESS_LOCAL
#endif
static DU_PROCESS_LOCAL struct du_window *clip_window;
struct clip_saved { struct clip_saved *next; struct du_window *window; struct ciuki_message message; };
static DU_PROCESS_LOCAL struct clip_saved *clip_saved_head,*clip_saved_tail;
static DU_PROCESS_LOCAL unsigned clip_saved_count;
static DU_PROCESS_LOCAL uint32_t clip_waiting;
static int native_clip(void *ctx, struct desk_clip_wire *w, const void *data, struct desk_clip_wire *reply, void **out);
static int next_message(struct du_window *w, struct ciuki_message *m)
{
    struct clip_saved **at=&clip_saved_head,*previous=NULL;
    while (*at && (*at)->window!=w) { previous=*at; at=&(*at)->next; }
    if (*at) {
        struct clip_saved *p=*at; *m=p->message; *at=p->next;
        if (clip_saved_tail==p) clip_saved_tail=previous;
        free(p); clip_saved_count--; return 1;
    }
    return ciuki_channel_recv(w->channel,m,DONTWAIT);
}
static int queue(struct du_window *w, struct ciuki_message *m)
{
    if (w->count == 16) { errno = EAGAIN; return -1; }
    m->data[0] = DESK_SHELL_VERSION;
    m->data[2] = DESK_RESIZE_MINOR;
    w->tx[(w->head + w->count) % 16] = *m; w->count++;
    return 0;
}

static void dialog_sound(void *ctx, unsigned event)
{
    struct du_window *w = ctx; struct ciuki_message m;
    desk_message(&m,DESK_SOUND,8); m.data[0] = DESK_SHELL_VERSION; desk_put32(m.data+4,event); (void)queue(w,&m);
}

/* G15: F1 and Help menus open CiukiOS Help at the program's topic. Only the
 * desktop can start a window, so the request is a LAUNCH of "help" whose
 * argument is "topic:<id>" (the desktop accepts it from every client and
 * Help opens that topic); without a topic Help opens at its contents. */
#define HELP_LAUNCH_SERIAL 0x48454c50u
static int help_launch(struct du_ui *u, const char *topic, void *ctx)
{
    struct du_window *w = ctx; struct ciuki_message m;
    const char name[] = "help"; size_t n = sizeof(name) - 1, a = 0;
    char arg[DESK_ARG_MAX + 1];
    (void)u;
    if (!w) return -EINVAL;
    if (topic && *topic) {
        size_t t = strlen(topic);
        if (t > 40) return -EINVAL;
        for (size_t i = 0; i < t; i++)
            if (!((topic[i] >= 'a' && topic[i] <= 'z') || (topic[i] >= '0' && topic[i] <= '9') || topic[i] == '-')) return -EINVAL;
        a = (size_t)snprintf(arg, sizeof(arg), "topic:%s", topic);
    }
    desk_message(&m, DESK_LAUNCH, (unsigned)(12 + n + (a ? 4 + a : 0)));
    desk_put32(m.data + 4, HELP_LAUNCH_SERIAL); desk_put32(m.data + 8, (uint32_t)n); memcpy(m.data + 12, name, n);
    if (a) { desk_put32(m.data + 12 + n, (uint32_t)a); memcpy(m.data + 16 + n, arg, a); }
    return queue(w, &m) ? -errno : 0;
}

static int flush(struct du_window *w)
{
    while (w->count) {
        if (ciuki_channel_send(w->channel, &w->tx[w->head], DONTWAIT)) {
            if (errno == EAGAIN || errno == EINTR) return 0;
            w->error = errno; w->closed = 1; return -1;
        }
        w->head = (w->head + 1) % 16; w->count--;
    }
    return 0;
}

uint64_t du_now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000u + (unsigned)t.tv_nsec;
}

static int window_open(struct du_window *w, struct du_ui *u, int channel, int width, int height,
                       const char *title, int min_width, int min_height)
{
    memset(w, 0, sizeof(*w)); w->surface = w->old_surface = -1; w->channel = channel; w->ui = u;
    w->resizable = min_width > 0 && min_height > 0;
    size_t len = strlen(title);
    if (!len || len > DESK_TITLE_MAX || width <= 0 || height <= 0 ||
        min_width < 0 || min_height < 0 || (min_width == 0) != (min_height == 0) ||
        min_width > width || min_height > height) { errno = EINVAL; return -1; }
    (void)desk_theme_poll(DESK_SETTINGS_PATH,du_now());
    w->dpi=desk_appearance_active ? desk_appearance.dpi : 96;
    desk_theme_logical(1);
    int physical_w=(width*(int)w->dpi+48)/96, physical_h=(height*(int)w->dpi+48)/96;
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) return -1;
    w->surface = ciuki_surface_create((uint32_t)physical_w, (uint32_t)physical_h, CIUKI_SURFACE_XRGB8888);
    if (w->surface < 0) return -1;
    if (ciuki_surface_info(w->surface, &w->info) || !desk_surface_valid(&w->info)) goto fail;
    w->pixels = ciuki_surface_map(w->surface, PROT_READ | PROT_WRITE);
    if (w->pixels == MAP_FAILED) { w->pixels = NULL; goto fail; }
    /* fx-15: WM_ERASEBKGND with the window class brush. The desktop shows
     * the window from CREATE_WINDOW on, before the first frame (startup,
     * file reads): the erased face, never a black (zeroed) surface. The
     * first present then damages only what differs from it. The shell's
     * theme is read first, as the first turn would. */
    (void)desk_theme_poll(DESK_SETTINGS_PATH, du_now());
    for (size_t k = 0, n = (size_t)w->info.stride / 4 * w->info.height; k < n; k++) w->pixels[k] = DU_FACE;
    w->back = malloc((size_t)width * (size_t)height * 4);
    if (!w->back) { errno = ENOMEM; goto fail; }
    du_init(u, w->back, width, height);
    u->help_open = help_launch; u->help_ctx = w;
    w->completion=malloc(sizeof(*w->completion));
    if (!w->completion) goto fail;
    if (du_completion_init(w->completion)) { free(w->completion); w->completion=NULL; goto fail; }
    u->wake=du_completion_notify; u->wake_ctx=w->completion;
    u->now = du_now();
    u->sound = dialog_sound; u->sound_ctx = w;
    struct ciuki_message m;
    desk_message(&m, DESK_HELLO, 4); queue(w, &m);
    desk_message(&m, DESK_CREATE_WINDOW, 44 + (unsigned)len);
    desk_put32(m.data + 4, (uint32_t)physical_w); desk_put32(m.data + 8, (uint32_t)physical_h); desk_put32(m.data + 12, (uint32_t)len);
    memcpy(m.data + 16, title, len); m.fd_count = 1; m.fds[0] = w->surface;
    if (w->resizable) {
        unsigned n = 16 + (unsigned)len;
        desk_put32(m.data + n, DESK_RESIZABLE);
        desk_put32(m.data + n + 4, (uint32_t)(min_width*(int)w->dpi+48)/96); desk_put32(m.data + n + 8, (uint32_t)(min_height*(int)w->dpi+48)/96);
        desk_put32(m.data + n + 12, CIUKI_SURFACE_DIMENSION_MAX); desk_put32(m.data + n + 16, CIUKI_SURFACE_DIMENSION_MAX);
        desk_put32(m.data + n + 20, 1); desk_put32(m.data + n + 24, 1);
    }
    queue(w, &m);
    /* G3: this window answers QUERY_END_SESSION (registration: serial 0, AGREE). */
    desk_message(&m, DESK_SESSION_ANSWER, 12); desk_put32(m.data + 8, DU_SESSION_AGREE); queue(w, &m);
    desk_message(&m,DESK_INPUT_SETTINGS,16);queue(w,&m);
    clip_window=w; du_clip_bind(native_clip,w);
    desk_message(&m, DESK_POWER_REPLY, 12); desk_put32(m.data+8,1); queue(w,&m);    return flush(w);
fail: { int error = errno ? errno : EINVAL; du_window_destroy(w); errno = error; return -1; }
}
int du_window_open(struct du_window *w, struct du_ui *u, int ch, int width, int height, const char *title)
{ return window_open(w,u,ch,width,height,title,0,0); }
int du_window_open_resizable(struct du_window *w, struct du_ui *u, int ch, int width, int height,
                             const char *title, int min_width, int min_height)
{ return window_open(w,u,ch,width,height,title,min_width,min_height); }

static void release_old(struct du_window *w)
{
    if (w->old_pixels) munmap(w->old_pixels, w->old_info.allocation_bytes);
    if (w->old_surface >= 0) close(w->old_surface);
    free(w->old_back);
    w->old_pixels = w->old_back = NULL; w->old_surface = -1;
    w->replacement = w->swap_sent = 0;
}
static void discard_replacement(struct du_window *w)
{
    munmap(w->pixels,w->info.allocation_bytes); close(w->surface); free(w->back);
    w->surface = w->old_surface; w->info = w->old_info; w->pixels = w->old_pixels; w->back = w->old_back;
    w->old_surface = -1; w->old_pixels = w->old_back = NULL; w->replacement = w->swap_sent = 0;
    du_resize(w->ui,w->back,w->old_logical_width,w->old_logical_height);
}
static int replacement(struct du_window *w)
{
    if (!w->request_serial || w->swap_sent) return 0;
    if (w->replacement) discard_replacement(w);
    /* Even a same-size proposal must acknowledge its serial with a surface. */
    int fd = ciuki_surface_create(w->request_width,w->request_height,w->info.format);
    if (fd < 0) return -1;
    struct ciuki_surface_info info; uint32_t *pixels = NULL, *back = NULL;
    if (ciuki_surface_info(fd,&info) || !desk_surface_valid(&info)) goto fail;
    pixels = ciuki_surface_map(fd,PROT_READ|PROT_WRITE);
    if (pixels == MAP_FAILED) { pixels = NULL; goto fail; }
    int logical_w=((int)info.width*96+(int)w->dpi/2)/(int)w->dpi;
    int logical_h=((int)info.height*96+(int)w->dpi/2)/(int)w->dpi;
    back = malloc((size_t)logical_w*logical_h*4);
    if (!back) { errno = ENOMEM; goto fail; }
    w->old_logical_width=w->ui->canvas.width; w->old_logical_height=w->ui->canvas.height;
    w->old_surface = w->surface; w->old_info = w->info; w->old_pixels = w->pixels; w->old_back = w->back;
    w->surface = fd; w->info = info; w->pixels = pixels; w->back = back;
    w->damage_count=0;
    w->replacement = 1; w->configure_serial = w->request_serial; w->request_serial = 0; w->damaged = 0;
    du_resize(w->ui,back,logical_w,logical_h);
    return 0;
fail: {
    int error = errno ? errno : EINVAL;
    if (pixels) munmap(pixels,info.allocation_bytes);
    free(back); close(fd); errno = error; return -1;
} }

static void add_damage(struct du_window *w, struct du_rect r)
{
    if (!w->damaged) w->damage_count=0;
    du_region_add(w->damages,&w->damage_count,r);
    if (!w->damaged) { w->damage = r; w->damaged = 1; return; }
    int x = w->damage.x < r.x ? w->damage.x : r.x, y = w->damage.y < r.y ? w->damage.y : r.y;
    int right = w->damage.x + w->damage.w > r.x + r.w ? w->damage.x + w->damage.w : r.x + r.w;
    int bottom = w->damage.y + w->damage.h > r.y + r.h ? w->damage.y + w->damage.h : r.y + r.h;
    w->damage = (struct du_rect){x, y, right - x, bottom - y};
}

/* Copy only changed pixels from the back buffer and damage their bounds:
 * the compositor never sees a half-rendered frame and repaints little. */
void du_window_present(struct du_window *w)
{
    if (w->closed || w->swap_sent || w->present_pending) return;
    int width = (int)w->info.width, height = (int)w->info.height;
    if(w->dpi && w->dpi!=96) {
        int lw=w->ui->canvas.width, lh=w->ui->canvas.height;
        int x0=width,y0=height,x1=-1,y1=-1;
        for(int y=0;y<height;y++) for(int x=0;x<width;x++) {
            uint32_t value=w->back[(size_t)(((2*y+1)*lh)/(2*height))*lw+((2*x+1)*lw)/(2*width)];
            uint32_t *dst=w->pixels+(size_t)y*(w->info.stride/4)+x;
            if(*dst==value && !w->replacement) continue;
            *dst=value;
            if(x<x0) x0=x;
            if(y<y0) y0=y;
            if(x>x1) x1=x;
            if(y>y1) y1=y;
        }
        if(x1>=0) add_damage(w,(struct du_rect){x0,y0,x1-x0+1,y1-y0+1});
        return;
    }
    unsigned regions=w->present_rendered && !w->replacement ? w->ui->frame_count : 1;
    for (unsigned n=0;n<regions;n++) {
        struct du_rect r=w->present_rendered && !w->replacement ? w->ui->frames[n] : (struct du_rect){0,0,width,height};
        r=du_intersection(r,(struct du_rect){0,0,width,height});
        int left=r.x, right=r.x+r.w, top=r.y, bottom=r.y+r.h;
        int x0=width, x1=-1, y0=height, y1=-1;
        for (int y=top;y<bottom && left<right;y++) {
            uint32_t *src=w->back+(size_t)y*width, *dst=w->pixels+(size_t)y*width;
            if (!memcmp(src+left,dst+left,(size_t)(right-left)*4)) continue;
            int a=left,b=right-1;
            while (src[a]==dst[a]) a++;
            while (src[b]==dst[b]) b--;
            memcpy(dst+a,src+a,(size_t)(b-a+1)*4);
            if (a<x0) x0=a;
            if (b>x1) x1=b;
            if (y<y0) y0=y;
            y1=y;
        }
        if (x1>=0) add_damage(w,(struct du_rect){x0,y0,x1-x0+1,y1-y0+1});
    }
    if (w->replacement) {
        w->damage_count=0; w->damaged=0; add_damage(w,(struct du_rect){0,0,width,height});
    }
}

int du_window_queue(struct du_window *w, struct ciuki_message *m) { return queue(w, m); }

int du_window_ping(struct du_window *w, uint32_t serial)
{
    struct ciuki_message m;
    desk_message(&m, DESK_PING, 8); desk_put32(m.data + 4, serial);
    return queue(w, &m);
}

int du_window_move(struct du_window *w, int x, int y)
{
    struct ciuki_message m;
    desk_message(&m, DESK_MOVE, 12); desk_put32(m.data + 4, (uint32_t)x); desk_put32(m.data + 8, (uint32_t)y);
    return queue(w, &m);
}

static void power_answer(struct du_window *w, int answer)
{
    struct ciuki_message m; desk_message(&m,DESK_POWER_REPLY,12);
    desk_put32(m.data+4,w->power_serial); desk_put32(m.data+8,(uint32_t)answer);
    w->power_reply_pending=queue(w,&m) ? answer+1 : 0;
}
/* G3 session states: 0 none, 1 inside the query callback, 2 the program
 * asks the user, 3 agreed (waiting for END_SESSION). */
enum { SESSION_NONE, SESSION_QUERY, SESSION_ASKING, SESSION_AGREED };
static void session_answer(struct du_window *w, int answer)
{
    w->session = answer == DU_SESSION_AGREE ? SESSION_AGREED : answer == DU_SESSION_ASKING ? SESSION_ASKING : SESSION_NONE;
    struct ciuki_message m; desk_message(&m, DESK_SESSION_ANSWER, 12);
    desk_put32(m.data + 4, w->session_serial); desk_put32(m.data + 8, (uint32_t)answer);
    if (queue(w, &m)) w->session_reply_pending = answer + 1;
    else w->session_reply_pending = 0;
}
void du_window_session_reply(struct du_window *w, int agree)
{ if (w->session == SESSION_ASKING) session_answer(w, agree ? DU_SESSION_AGREE : DU_SESSION_VETO); }
void du_window_close_reply(struct du_window *w, int later)
{
    if (w->session == SESSION_QUERY || w->session == SESSION_ASKING) {
        if (!later) session_answer(w, DU_SESSION_VETO);   /* the user cancelled the prompt */
        return;
    }
    w->close_reply_pending=later ? 2 : 1;
}
void du_window_close(struct du_window *w)
{
    /* The program's own close during a query (its prompt answered Yes or
     * No) agrees: the window stays until END_SESSION. */
    if (w->session == SESSION_QUERY || w->session == SESSION_ASKING) { session_answer(w, DU_SESSION_AGREE); return; }
    if (w->session == SESSION_AGREED) return;
    if (!w->closing) w->closing = 1;
    du_release(w->ui);
}

int du_window_accept_files(struct du_window *w, int on)
{
    struct ciuki_message m;
    desk_message(&m, DESK_ACCEPT_FILES, 8); desk_put32(m.data + 4, on ? 1u : 0u);
    if (queue(w, &m)) return -1;
    w->accept_files = on != 0;
    return 0;
}

int du_window_set_title(struct du_window *w, const char *title)
{
    size_t n = title ? strlen(title) : 0;
    if (!n || n > DESK_TITLE_MAX) { errno = EINVAL; return -1; }
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)title[i] < 32 || (unsigned char)title[i] > 126) { errno = EINVAL; return -1; }
    struct ciuki_message msg;
    desk_message(&msg, DESK_SET_TITLE, 8 + (unsigned)n);
    desk_put32(msg.data + 4, (uint32_t)n); memcpy(msg.data + 8, title, n);
    return queue(w, &msg);
}

int du_window_turn(struct du_window *w, uint64_t now)
{
    if (w->closed) return 0;
    if (flush(w)) return -1;
    for (unsigned n = 0; n < 32 && w->count < 14; n++) {
        struct ciuki_message m;
        int r = next_message(w,&m);
        if (r < 0) { if (errno == EAGAIN || errno == EINTR) break; w->error = errno; w->closed = 1; return -1; }
        if (!r) { w->closed = 1; du_release(w->ui); return 0; }
        struct desk_packet p;
        if (!m.sender_pid || (w->server_pid && m.sender_pid != w->server_pid) ||
            m.data[0] != DESK_SHELL_VERSION || m.data[2] != DESK_RESIZE_MINOR || !desk_parse(&m, 0, NULL, &p)) {
            for (unsigned i=0;i<m.fd_count && i<4;i++) close(m.fds[i]);
            w->error = EIO; w->closed = 1; return -1;
        }
        for (unsigned i=0;i<m.fd_count && i<4;i++) close(m.fds[i]);
        w->server_pid = m.sender_pid;
        if (now > w->ui->now) w->ui->now = now;
        switch (p.opcode) {
        case DESK_CLIPBOARD_EVENT: du_clip_event(&p.clip); break;
        case DESK_CONFIGURE:
            if (p.flags & DESK_CONFIGURE_COMMITTED) {
                if ((!w->configured ? p.serial != 0 : !w->swap_sent || p.serial != w->configure_serial) ||
                    p.width != w->info.width || p.height != w->info.height) goto protocol_error;
                if (w->configured) release_old(w);
                w->configured = 1; w->ui->dirty = 1;
            } else {
                if (!w->configured || !w->resizable || p.serial <= w->last_serial) goto protocol_error;
                w->last_serial = w->request_serial = p.serial;
                w->request_width = p.width; w->request_height = p.height;
            }
            break;
        case DESK_INPUT_STATE:
            if(w->ui->input_layout!=p.input_settings[0])w->ui->input_dead=0;
            w->ui->input_layout=p.input_settings[0];w->ui->input_blink_ms=p.input_settings[3];
            w->ui->input_wheel_lines=p.input_settings[6];w->ui->input_double_ms=p.input_settings[9];
            w->ui->input_caret_on=1;w->ui->input_caret_deadline=0;w->ui->dirty=1;break;
        case DESK_POWER_EVENT: {
            w->power_serial=p.serial;
            power_answer(w,2); (void)flush(w); /* handler owns the query from here */
            int agree=w->power_event ? w->power_event(w,p.index,p.flags,w->user) : 1;
            power_answer(w,p.index==0 && !agree ? 0 : 1);
            break;
        }        case DESK_PING:
            if (w->power_reply_pending) power_answer(w,w->power_reply_pending-1);
            if (w->session_reply_pending) session_answer(w, w->session_reply_pending - 1);   /* answer before PONG */
            desk_message(&m, DESK_PONG, 8); desk_put32(m.data + 4, p.serial); queue(w, &m); break;
        case DESK_PONG: break;
        case DESK_CLOSED: w->closed = 1; du_release(w->ui); return 0;
        case DESK_CLOSE_REQUEST:
            if (w->closing || w->session) break;   /* already closing, or the session owns the window */
            if (!w->close_request || w->close_request(w, w->user)) du_window_close(w);
            else du_window_close_reply(w,1);
            break;
        case DESK_QUERY_END_SESSION: {
            w->session_serial = p.serial; w->session_flags = p.flags;
            if (w->closing) { session_answer(w, DU_SESSION_AGREE); break; }
            w->session = SESSION_QUERY;
            int answer = w->query_end_session ? w->query_end_session(w, p.flags, w->user) :
                         !w->close_request || w->close_request(w, w->user) ? DU_SESSION_AGREE : DU_SESSION_ASKING;
            /* The callback may already have answered (its own close, a veto). */
            if (w->session == SESSION_QUERY)
                session_answer(w, answer == DU_SESSION_VETO || answer == DU_SESSION_ASKING ? answer : DU_SESSION_AGREE);
            break;
        }
        case DESK_END_SESSION:
            if (p.serial != w->session_serial) break;
            w->session = SESSION_NONE; w->session_reply_pending = 0;
            if (p.focused) { if (!w->closing) w->closing = 1; du_release(w->ui); }   /* exit without asking again */
            break;
        case DESK_FOCUS: du_window_focus(w->ui, p.focused != 0); break;
        case DESK_DROP_FILES:
            if (w->accept_files) du_emit(w->ui, (struct du_event){.type = DU_EV_DROPFILES, .x = (int)(((int64_t)p.x*2+1)*w->ui->canvas.width/(2*w->info.width)), .y = (int)(((int64_t)p.y*2+1)*w->ui->canvas.height/(2*w->info.height)), .index = (int)p.serial});
            if (w->message) w->message(w, &p, w->user);
            break;
        case DESK_KEY: case DESK_MOTION: case DESK_BUTTON:
            /* Repair remembered input on the desktop's backlog RESYNC too. */
            if(p.event.type==CIUKI_INPUT_MOTION && w->dpi && w->dpi!=96) {
                p.event.value=(int)(((int64_t)p.event.value*2+1)*w->ui->canvas.width/(2*w->info.width));
                p.event.value2=(int)(((int64_t)p.event.value2*2+1)*w->ui->canvas.height/(2*w->info.height));
            }
            du_input(w->ui, &p.event); break;
        default: if (w->message) w->message(w, &p, w->user); break;
        }
    }
    if (w->power_reply_pending && w->count<16) power_answer(w,w->power_reply_pending-1);
    if (w->session_reply_pending && w->count<16) session_answer(w, w->session_reply_pending - 1);
    if (w->close_reply_pending && w->count<16) {
        struct ciuki_message m; desk_message(&m,DESK_CLOSE_ACK,8);
        desk_put32(m.data+4,(uint32_t)(w->close_reply_pending-1));
        if (!queue(w,&m)) w->close_reply_pending=0;
    }
    if (!w->closing && !w->present_pending && replacement(w)) { w->error = errno; w->closed = 1; return -1; }
    if (w->closing) {
        if (w->closing == 1 && w->count < 16) {
            if (clip_window==w) du_clip_release(); /* render while the channel is still live */
            struct ciuki_message m; desk_message(&m, DESK_CLOSE, 4);
            if (!queue(w, &m)) w->closing = 2;
        }
        if (flush(w)) return -1;
        if (w->closing == 2 && !w->count) w->closed = 1;
        return w->closed ? 0 : 1;
    }
    du_tick(w->ui, now);
    unsigned cursor=du_cursor_at(w->ui);
    if (w->configured && cursor!=w->cursor_sent && w->count<16) {
        struct ciuki_message m; desk_message(&m,DESK_SET_CURSOR,8); desk_put32(m.data+4,cursor);
        if (!queue(w,&m)) w->cursor_sent=cursor;
    }
    /* ui-look: follow the shell's theme setting (read at once, then a stat every 2 s). */
    if (desk_theme_poll(DESK_SETTINGS_PATH, now)) w->ui->dirty = 1;
    if (w->configured && !w->swap_sent && !w->present_pending && w->ui->dirty) { du_render(w->ui);
        int mode=w->present_rendered; w->present_rendered=1;
        du_window_present(w); w->present_rendered=mode; }
    while (w->configured && w->damaged && w->count < 16 && !w->manual_damage) {
        struct ciuki_message m; desk_message(&m, DESK_DAMAGE, 24);
        struct du_rect damage=w->replacement || !w->damage_count ? w->damage : w->damages[0];
        desk_put32(m.data + 4, (uint32_t)damage.x); desk_put32(m.data + 8, (uint32_t)damage.y);
        desk_put32(m.data + 12, (uint32_t)damage.w); desk_put32(m.data + 16, (uint32_t)damage.h);
        if (w->replacement) {
            desk_put32(m.data + 20,w->configure_serial); m.fd_count = 1; m.fds[0] = w->surface;
            /* Send swaps directly after the FIFO drains: an EAGAIN surface
             * remains unsent and can be superseded by the next CONFIGURE. */
            if (flush(w)) return -1;
            if (!w->count) {
                m.data[0] = DESK_SHELL_VERSION; m.data[2] = DESK_RESIZE_MINOR;
                if (!ciuki_channel_send(w->channel,&m,DONTWAIT)) { w->swap_sent = 1; w->damaged = 0; w->damage_count=0; }
                else if (errno != EAGAIN && errno != EINTR) { w->error = errno; w->closed = 1; return -1; }
            }
            break; /* The pending swap (including EAGAIN) is retried next turn. */
        } else if (!queue(w, &m)) {
            if (w->damage_count) {
                w->damage_count--;
                memmove(w->damages,w->damages+1,w->damage_count*sizeof(*w->damages));
            }
            w->damaged=w->damage_count!=0;
        } else break;
    }
    return flush(w) ? -1 : 1;
protocol_error:
    w->error = EIO; w->closed = 1; return -1;
}

int du_window_wait(struct du_window *w, uint64_t deadline)
{
    for (struct clip_saved *p=clip_saved_head;p;p=p->next) if (p->window==w) return 0;
    struct du_ui *u=w->ui;
    if (w->closed || (w->closing==1 && w->count<16) || (w->configured && u->dirty &&
        !w->swap_sent && !w->present_pending && w->count<16)) return 0;
    struct pollfd fds[6]; unsigned n=0;
    fds[n++]=(struct pollfd){w->channel,POLLIN | ((w->count ||
        (w->configured && w->damaged && !w->manual_damage && !w->swap_sent)) ? POLLOUT : 0),0};
    if (w->completion) fds[n++]=(struct pollfd){w->completion->fd[0],POLLIN,0};
    for (unsigned i=0;i<u->wait_count && i<4;i++)
        if (u->wait_fds[i]>=0) fds[n++]=(struct pollfd){u->wait_fds[i],POLLIN,0};
    int r=poll(fds,n,du_wait_ms(du_now(),deadline));
    if (r<0) return errno==EINTR ? 0 : -1;
    for (unsigned i=0;i<n;i++) if (fds[i].revents&POLLNVAL) { errno=EBADF; return -1; }
    if (w->completion && fds[1].revents) {
        if (fds[1].revents&(POLLERR|POLLHUP)) { errno=EIO; return -1; }
        if (du_completion_take(w->completion)) return -1;
    }
    return 0;
}

int du_run(struct du_window *w, void (*idle)(void *), void *arg)
{
    int r;
    while ((r = du_window_turn(w, du_now())) > 0) {
        if (idle) idle(arg);
        if (du_window_wait(w,du_next_deadline(w->ui))) return -1;
    }
    return r;
}

void du_window_destroy(struct du_window *w)
{
    if (clip_window==w) { du_clip_release(); du_clip_bind(NULL,NULL); clip_window=NULL; }
    struct clip_saved **at=&clip_saved_head,*previous=NULL;
    while (*at) {
        struct clip_saved *p=*at;
        if (p->window!=w) { previous=p; at=&p->next; continue; }
        for (unsigned i=0;i<p->message.fd_count;i++) close(p->message.fds[i]);
        *at=p->next; if (clip_saved_tail==p) clip_saved_tail=previous;
        free(p); clip_saved_count--;
    }
    if (w->completion) {
        if (w->ui) { w->ui->wake=NULL; w->ui->wake_ctx=NULL; }
        du_completion_destroy(w->completion); free(w->completion); w->completion=NULL;
    }
    if (w->back && w->ui) du_dialog_close(w->ui);
    release_old(w);
    if (w->pixels) { munmap(w->pixels, w->info.allocation_bytes); w->pixels = NULL; }
    free(w->back); w->back = NULL;
    if (w->surface >= 0) { close(w->surface); w->surface = -1; }
    if (w->channel >= 0) { close(w->channel); w->channel = -1; }
}

/* Synchronous clipboard RPC pumps transport/UI progress without running the
 * application's event callback recursively. Calls originate on the UI thread. */
static int native_clip(void *ctx, struct desk_clip_wire *w, const void *data, struct desk_clip_wire *reply, void **out)
{
    struct du_window *window=ctx;
    if (clip_waiting) return -EBUSY;
    int fd=-1;
    if (w->bytes) {
        uint32_t width,height; desk_clip_dims(w->bytes,&width,&height);
        fd=ciuki_surface_create(width,height,CIUKI_SURFACE_XRGB8888); if (fd<0) return -errno;
        struct ciuki_surface_info info;
        if (ciuki_surface_info(fd,&info)) { int e=errno; close(fd); return -e; }
        void *map=ciuki_surface_map(fd,PROT_READ|PROT_WRITE);
        if (!map || map==MAP_FAILED) { close(fd); return -ENOMEM; }
        memcpy(map,data,w->bytes); munmap(map,info.allocation_bytes);
    }
    struct ciuki_message m;
    int error=desk_clip_message(&m,DESK_SHELL_VERSION,1,w,fd);
    m.data[2]=DESK_RESIZE_MINOR;
    if (error) { if (fd>=0) close(fd); return error; }
    uint64_t deadline=du_now()+3000000000ull;
    clip_waiting=w->serial;
    while (window->count && du_now()<deadline && !window->closed) { flush(window); if (window->count) { struct pollfd p={window->channel,POLLOUT,0}; poll(&p,1,10); } }
    if (window->count || window->closed) error=-EIO;
    while (!error && ciuki_channel_send(window->channel,&m,DONTWAIT)) {
        if (errno!=EAGAIN && errno!=EINTR) { error=-errno; break; }
        if (du_now()>=deadline) { error=-ETIMEDOUT; break; }
        struct pollfd p={window->channel,POLLOUT,0}; poll(&p,1,10);
    }
    if (fd>=0) close(fd);
    int done=0;
    while (!error && !done && !window->closed && du_now()<deadline) {
        struct ciuki_message incoming;
        int got=ciuki_channel_recv(window->channel,&incoming,DONTWAIT);
        if (got<0 && (errno==EAGAIN || errno==EINTR)) { struct pollfd p={window->channel,POLLIN,0}; poll(&p,1,10); continue; }
        if (got!=1) { error=-EIO; break; }
        struct desk_packet packet;
        if (!incoming.sender_pid || (window->server_pid && window->server_pid!=incoming.sender_pid) ||
            !desk_parse(&incoming,0,NULL,&packet)) {
            for (unsigned i=0;i<incoming.fd_count && i<4;i++) close(incoming.fds[i]); error=-EIO; break;
        }
        window->server_pid=incoming.sender_pid;
        if (packet.opcode==DESK_CLIPBOARD_EVENT) {
            du_clip_observe(packet.clip.sequence);
            if (packet.clip.kind==DESK_CLIP_REPLY && packet.clip.serial==w->serial) {
                *reply=packet.clip; *out=NULL; done=1; error=reply->result;
                if (!error && incoming.fd_count) {
                    struct ciuki_surface_info info;
                    if (ciuki_surface_info(incoming.fds[0],&info) || !desk_surface_valid(&info) || reply->bytes>info.allocation_bytes) error=-EIO;
                    else {
                        void *map=ciuki_surface_map(incoming.fds[0],PROT_READ);
                        if (!map || map==MAP_FAILED) error=-ENOMEM;
                        else { *out=malloc(reply->bytes); if (!*out) error=-ENOMEM; else memcpy(*out,map,reply->bytes); munmap(map,info.allocation_bytes); }
                    }
                }
            }
            /* EmptyClipboard destroys old producer resources before its
             * caller can publish new promises. Ordinary UI events remain
             * deferred; only this clipboard lifecycle callback is immediate. */
            if (packet.clip.kind==DESK_CLIP_DESTROY) du_clip_event(&packet.clip);
            if (packet.clip.kind==DESK_CLIP_RENDER) {
                struct clip_saved *saved=clip_saved_count<1024 ? malloc(sizeof(*saved)) : NULL;
                if (!saved) error=-ENOMEM;
                else {
                    *saved=(struct clip_saved){.window=window,.message=incoming}; saved->message.fd_count=0;
                    if (clip_saved_tail) clip_saved_tail->next=saved; else clip_saved_head=saved;
                    clip_saved_tail=saved; clip_saved_count++;
                }
            }
            for (unsigned i=0;i<incoming.fd_count;i++) close(incoming.fds[i]);
        } else if (packet.opcode==DESK_PING) {
            struct ciuki_message pong; desk_message(&pong,DESK_PONG,8); pong.data[0]=2; pong.data[2]=1; desk_put32(pong.data+4,packet.serial);
            if (ciuki_channel_send(window->channel,&pong,DONTWAIT) && errno!=EAGAIN && errno!=EINTR) error=-errno;
        } else {
            struct clip_saved *saved=clip_saved_count<1024 ? malloc(sizeof(*saved)) : NULL;
            if (!saved) { for (unsigned i=0;i<incoming.fd_count;i++) close(incoming.fds[i]); error=-ENOMEM; break; }
            *saved=(struct clip_saved){.window=window,.message=incoming};
            if (clip_saved_tail) clip_saved_tail->next=saved; else clip_saved_head=saved;
            clip_saved_tail=saved; clip_saved_count++;
        }
    }
    if (!error && !done) error=-ETIMEDOUT;
    clip_waiting=0; return error;
}
