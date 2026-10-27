/* Run the production native client against an independent window/VM replay.
 * No QEMU or DOS execution claimed. SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "../../../apps/desktop/protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#undef ILL_ILLOPC
#undef FPE_INTDIV
#undef SEGV_MAPERR
#undef SEGV_ACCERR
#undef BUS_ADRALN
#include <signal.h>
#include <time.h>
#include <poll.h>
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#include <unistd.h>
#include "../../../apps/dosbox/program.h"
static int replay_close(int);
static int replay_clock(clockid_t, struct timespec *);
static int replay_poll(struct pollfd *,nfds_t,int);
#define close replay_close
#define clock_gettime replay_clock
#define poll replay_poll
#define main dosbox_main
#include "../../../apps/dosbox/dosbox.c"
#undef main
#undef close
#undef clock_gettime
#undef poll

static uint32_t screen[640 * 480];
static uint32_t guest_screen[640 * 480];
/* Clipboard transport/selection has its own G5 production fixture. This
 * lifecycle replay keeps it idle while checking G17 and G3 launch/close. */
void dos_clip_init(int channel,uint32_t *pixels) { assert(channel==3 && pixels==guest_screen); }
void dos_clip_vm(uint32_t id,uint32_t generation,uint32_t server) { (void)id; (void)generation; (void)server; }
int dos_clip_recv(struct ciuki_message *m) { return ciuki_channel_recv(3,m,DONTWAIT); }
int dos_clip_action(unsigned action) { assert(!action); return 0; }
int dos_clip_input(const struct desk_packet *p) { (void)p; return 0; }
void dos_clip_tick(int focused) { (void)focused; }
int dos_clip_marking(void) { return 0; }
int dos_clip_pasting(void) { return 0; }
int dos_clip_dirty(void) { return 0; }
void dos_clip_destroy(void) {}
void dos_clip_settings(const char *typed) { (void)typed; }
int dos_clip_service(void) { return 0; }

static unsigned surfaces;
static uint64_t clock_ms, enter_ms, first_frame_ms;
static unsigned title_set, finished_panel, close_requests;
static int phase, scenario, armed, entered, presents, damage_count, error_seen, closed;
static unsigned error_idle_waits;
static unsigned nonvideo, timeout_waits, state_waits, last_events, nonvideo_presents;
static int input_refused;
static int focus_loss;
static int fs_command,fs_active,fs_panel_seen;
static uint64_t fs_until;
/* fx-dosfull scenarios 33-37 (a kernel with CIUKI_DOSVM_GRAPHICS): 33 the
 * game enters full screen at its first graphics mode and the user's Alt+Enter
 * back to the window is remembered; 34 a remembered Window starts windowed,
 * the user's Alt+Enter into full screen is remembered as graphics full screen
 * and a mode-change return enters again; 35 the display refuses (QEMU f3-29):
 * one request, no notice, no setting; 36 a mode-change return enters again,
 * a watchdog return does not; 37 a busy display is retried after 1 s. */
static unsigned fs_calls,fs_user_in;
static int fs_hotkey,fs_error;
static uint64_t fs_first_call,fs_entered_at[4],fs_return_at;
static unsigned settings_phase, close_declined, f12_forwarded;
static unsigned session_registered, session_queries, session_answers, session_ends; /* G3 scenario 22 */
static unsigned save_full_notice;
static unsigned bar_damage[256], bar_updates, full_damage, settings_damage, notice_seen, loading_seen, back_seen;
static uint64_t last_bar_at;
/* fx-4b: scenario 20 waits for the background check of the DOS system
 * files with progress; 21 is refused for a checksum mismatch. */
static unsigned image_polls, image_paints, checking_seen, pongs, damaged_seen, pre_create_waits;
static int last_permille = -1, vm_created;
/* fx-23: scenario 22 is a CREATE that stays in the kernel for 6 s (the T23
 * trial's was 53 s) while it waits for a fresh check of the DOS system
 * files; 23 is closed by the user during that CREATE. */
static volatile int create_entered, create_release;
static unsigned create_polls, create_pongs, create_checking, create_pulses, create_queries;
static uint64_t create_started_ms, create_released_ms;
static void real_sleep(void) { struct timespec t = {.tv_nsec = 1000000}; nanosleep(&t, NULL); }
static int creating(void) { return create_job.started && !__atomic_load_n(&create_job.done, __ATOMIC_ACQUIRE); }
static char console[256]; static unsigned console_at;
static const struct desk_theme replay_theme = {.face = 0xcccccc, .ink = 0, .select = 0x2266aa};
const struct desk_theme *desk_theme = &replay_theme;
static int replay_close(int fd) { assert(fd == 7 || fd == 8 || fd == 3); return 0; }
static int replay_clock(clockid_t id, struct timespec *t)
{ assert(id == CLOCK_MONOTONIC); t->tv_sec = (time_t)(clock_ms / 1000); t->tv_nsec = (long)(clock_ms % 1000) * 1000000; return 0; }
static int replay_poll(struct pollfd *fds,nfds_t n,int timeout)
{
    assert(n==1 && fds[0].fd==3 && fds[0].events==POLLIN);
    if (timeout==200) { assert(scenario==20 && !vm_created); pre_create_waits++; clock_ms+=200; fds[0].revents=0; return 0; }
    if (timeout==DOSBOX_CREATE_FIRST_MS || timeout==DOSBOX_CREATE_POLL_MS) {
        /* The window thread waits for the CREATE worker: never a spin. */
        assert(create_job.started); create_polls++; fds[0].revents=0;
        if (scenario==27 || scenario==28) {
            clock_ms+=(uint64_t)timeout;
            if (scenario==27 && clock_ms-create_started_ms>=6000 && create_pongs) { create_release=1; create_released_ms=clock_ms; }
            if (!create_release) return 0;
        }
        for (unsigned i=0;i<5000 && !__atomic_load_n(&create_job.done,__ATOMIC_ACQUIRE);i++) real_sleep();
        assert(__atomic_load_n(&create_job.done,__ATOMIC_ACQUIRE));
        return 0;
    }
    assert(timeout==-1);error_idle_waits++;clock_ms+=10000;fds[0].revents=POLLIN;return 1;
}
int ciuki_surface_create(uint32_t w, uint32_t h, uint32_t f)
{ assert(w == 640 && h == 480 && f == CIUKI_SURFACE_XRGB8888); return 7+(int)surfaces++; }
void *ciuki_surface_map(int fd, int prot)
{ assert((fd == 7 || fd==8) && prot == (PROT_READ | PROT_WRITE)); return fd==7 ? guest_screen : screen; }
int ciuki_surface_info(int fd,struct ciuki_surface_info *info)
{
    (void)fd; *info=(struct ciuki_surface_info){sizeof(*info),640,480,2560,CIUKI_SURFACE_XRGB8888,sizeof(screen)};
    return 0;
}
int ciuki_channel_send(int fd, const struct ciuki_message *m, uint32_t flags)
{
    assert(fd == 3 && !flags);
    const struct ciuki_surface_info info = {sizeof(info), 640, 480, 2560, CIUKI_SURFACE_XRGB8888, sizeof(screen)};
    struct desk_packet p; assert(desk_parse(m, 1, &info, &p));
    if (p.opcode == DESK_DAMAGE) {
        damage_count++;
        unsigned x=desk_get32(m->data+4), y=desk_get32(m->data+8);
        unsigned w=desk_get32(m->data+12), h=desk_get32(m->data+16);
        if (w==400 && h==18) {
            assert(x==120 && y==272 && (!bar_updates || clock_ms-last_bar_at>=250));
            assert(clock_ms/1000<256 && ++bar_damage[clock_ms/1000]<=4);
            last_bar_at=clock_ms; bar_updates++;
        } else if (w==480 && h==280) {
            assert(x==80 && y==120); settings_damage++;
        } else if (fs_panel_seen && h==480) { assert(!y); full_damage++; }
        else if (presents && h!=480) {
            /* G17 scales the intact guest into the area below the toolbar.
             * Every incremental row must sample a changed guest row. */
            assert(y>=DOSBOX_TOOLBAR && y+h<=480);
            for (unsigned row=y;row<y+h;row++) {
                unsigned source=row==480 ? 479u : (row-DOSBOX_TOOLBAR)*480u/DOSBOX_VIEW_HEIGHT;
                if (scenario==0) assert(source<8 || (source>=16 && source<24) || (source>=264 && source<272));
                else assert(source==1 || source==17 || source==265);
            }
        } else { assert(screen[0] == replay_theme.face); full_damage++; }
    }
    /* G3 lifecycle packets and fx-23 CREATE worker packets share this channel. */
    if (p.opcode==DESK_CLOSE_ACK) { assert(scenario==12 && p.flags==DESK_CLOSE_DOS_ACTIVE); close_declined++; }
    if (p.opcode==DESK_SESSION_ANSWER) {
        if (!p.serial) { assert(p.flags==DESK_SESSION_AGREE && !session_registered); session_registered++; }
        else { assert((scenario==22 || scenario==26) && p.serial==5 && p.flags==(scenario==26 ? DESK_SESSION_AGREE : DESK_SESSION_DOS_ACTIVE) && session_registered); session_answers++; }
    }
    /* dosbox: EXIT with "Close on exit" off (30); a program that ended before
     * it was shown or in its first seconds (23, 24, 32). */
    if (p.opcode==DESK_SET_TITLE) {
        assert((scenario==23 || scenario==24 || scenario==30 || scenario==32) && !strcmp(p.title,"Finished - DOOM"));
        title_set++;
    }
    if (p.opcode==DESK_PONG && (scenario==27 || scenario==28)) { assert(creating() && desk_get32(m->data+4)==78); create_pongs++; }
    else if (p.opcode==DESK_PONG) { assert(scenario==20 && !vm_created && desk_get32(m->data+4)==77); pongs++; }
    if (p.opcode==DESK_DAMAGE && creating() && desk_get32(m->data+12)==400 && desk_get32(m->data+16)==18) create_pulses++;
    return 0;
}
int ciuki_channel_recv(int fd, struct ciuki_message *m, uint32_t flags)
{
    assert(fd == 3 && flags == DONTWAIT);
    if ((scenario==27 || scenario==28) && creating() && phase>=2 && !create_pongs && clock_ms-create_started_ms>=1000) {
        desk_message(m, DESK_PING, 8); desk_put32(m->data + 4, 78);
        m->data[0] = DESK_SHELL_VERSION; m->sender_pid = 999; return (int)m->length;
    }
    if (scenario==28 && creating() && create_pongs && clock_ms-create_started_ms>=2000) {
        desk_message(m, DESK_CLOSE_REQUEST, 4); phase++;
        m->data[0] = DESK_SHELL_VERSION; m->sender_pid = 999; return (int)m->length;
    }
    if (scenario==20 && !vm_created && image_polls==3 && !pongs) {
        desk_message(m, DESK_PING, 8); desk_put32(m->data + 4, 77);
        m->data[0] = DESK_SHELL_VERSION; m->sender_pid = 999; return (int)m->length;
    }
    if (!phase) {
        desk_message(m, DESK_CONFIGURE, 12); desk_put32(m->data + 4, 640); desk_put32(m->data + 8, 480);
        phase++;
    } else if (phase == 1) {
        desk_message(m, DESK_FOCUS, 8); desk_put32(m->data + 4, 1); phase++;
    } else if (focus_loss == 1) {
        desk_message(m, DESK_FOCUS, 8); desk_put32(m->data + 4, 0);
        clock_ms += 120001; focus_loss++;
    } else if (focus_loss == 2) {
        desk_message(m, DESK_FOCUS, 8); desk_put32(m->data + 4, 1); focus_loss++;
    } else if (scenario==8 && presents && !fs_command) {
        fs_command=1; desk_message(m,DESK_DOS_FULLSCREEN,4);
    } else if (scenario==11 && presents && !settings_phase++) {
        desk_message(m,DESK_KEY,44); desk_put32(m->data+16,CIUKI_INPUT_KEY);
        desk_put32(m->data+28,CIUKI_KEY_F12); desk_put32(m->data+32,1);
    } else if ((scenario==22 || scenario==26) && presents && clock_ms > enter_ms+650 && !session_queries) {
        desk_message(m,DESK_QUERY_END_SESSION,12); desk_put32(m->data+4,5); session_queries++;
    } else if ((scenario==22 || scenario==26) && session_answers==1 && !session_ends) {
        desk_message(m,DESK_END_SESSION,16); desk_put32(m->data+4,5); desk_put32(m->data+8,1); session_ends++;
    } else if (scenario==22 || scenario==26) { errno = EAGAIN; return -1; }
    else if (scenario==31 && clock_ms < enter_ms+20000) { errno = EAGAIN; return -1; } /* the program's return closes it */
    else if ((presents && clock_ms > (scenario==8 ? fs_until+3300 : scenario==32 ? enter_ms+2650 : scenario>=33 ? enter_ms+5000 : enter_ms+650)) ||
             (error_seen && clock_ms > 5000)) {
        desk_message(m, DESK_CLOSE_REQUEST, 4); phase++; close_requests++;
    } else { errno = EAGAIN; return -1; }
    m->data[0] = DESK_SHELL_VERSION; m->sender_pid = 999;
    return (int)m->length;
}
int ciuki_dosvm(uint32_t op, struct ciuki_dosvm_command *c)
{
    assert(c->version == 1 && c->size == sizeof(*c));
    if (op == CIUKI_DOSVM_IMAGE_QUERY) {
        assert(!c->id && !c->generation && !c->state && !c->changed && !c->launch.game);
        c->state = CIUKI_DOSVM_IMAGE_UNCHECKED;
        if (scenario==20) {
            assert(!vm_created);
            c->state = image_polls < 6 ? CIUKI_DOSVM_IMAGE_VERIFYING : CIUKI_DOSVM_IMAGE_VERIFIED;
            c->changed = image_polls < 6 ? image_polls * 180u : 1000u;
        }
        if (scenario==21) c->state = image_polls ? CIUKI_DOSVM_IMAGE_MISMATCH : CIUKI_DOSVM_IMAGE_UNCHECKED;
        if ((scenario==27 || scenario==28) && creating()) {
            /* CREATE joined a fresh check: 30/60/90 %, then verified. */
            create_queries++;
            if (create_queries<=3) { c->state=CIUKI_DOSVM_IMAGE_VERIFYING; c->changed=create_queries*300u; }
            else c->state=CIUKI_DOSVM_IMAGE_VERIFIED;
            return 0;
        }
        image_polls++;
        return 0;
    }
    if (op == CIUKI_DOSVM_CREATE) {
        assert(c->surface_fd == 7);
        assert(c->launch.game==(scenario==6 ? 0u : 2u));
        assert(c->launch.kernel==CIUKI_DOSVM_UNIFIED);
        assert(c->launch.memory==CIUKI_DOSVM_MEMORY_PAGED);
        if (scenario==20) assert(image_polls==7 && image_paints==6 && phase>=2);
        if (scenario == 2 || scenario==9 || scenario==21) { errno = scenario==2 ? ENOMEM : EIO; return -1; }
        if (scenario==27 || scenario==28) {
            create_started_ms=clock_ms; create_entered=1;
            for (unsigned i=0;i<20000 && !create_release;i++) real_sleep();
            assert(create_release);
            if (scenario==28) { errno=EINTR; return -1; }   /* the owner stopped */
        }
        c->id = 1; c->generation = 9; vm_created = 1; return 0;
    }
    assert(c->id == 1 && c->generation == 9);
    switch (op) {
    case CIUKI_DOSVM_BACKGROUND: assert(c->focused==0); return 0;
    case CIUKI_DOSVM_POLICY: assert(!c->policy.marking && c->policy.idle_sensitivity==50); return 0;
    case CIUKI_DOSVM_WAIT_PRESENT:
        assert(c->wait.timeout_ms==CIUKI_DOSVM_WAIT_MAX_MS);
        /* timeout_ms is a maximum, not the producer period. VIDEO/STATE
         * wake early; only an actual TIMEOUT consumes the full deadline. */
        clock_ms+=nonvideo==1 ? c->wait.timeout_ms : 15; c->wait.frame_ms=15;
        c->wait.events=nonvideo==1 ? CIUKI_DOSVM_EVENT_TIMEOUT : nonvideo==2 ? CIUKI_DOSVM_EVENT_STATE : CIUKI_DOSVM_EVENT_VIDEO;
        last_events=c->wait.events;
        timeout_waits+=last_events==CIUKI_DOSVM_EVENT_TIMEOUT;
        state_waits+=last_events==CIUKI_DOSVM_EVENT_STATE;
        return 0;
    case CIUKI_DOSVM_FOCUS:
        if (scenario==20 && !presents) assert(c->focused);  /* focus seen during the check */
        return 0;
    case CIUKI_DOSVM_FULLSCREEN:
        fs_calls++;
        if (scenario==19) { assert(!c->focused); fs_command=1; return 0; }
        if (scenario>=33) {
            assert(c->focused && !fs_active && fs_calls<=4);
            if (scenario==34) assert(fs_user_in);   /* a remembered Window never enters by itself */
            if (scenario==35) return 0;              /* DISPLAY_FULLSCREEN_WINDOWED: success, still windowed */
            if (scenario==37 && fs_calls==1) { fs_first_call=clock_ms; errno=EBUSY; return -1; }
            if (scenario==37) assert(clock_ms>=fs_first_call+DOSBOX_AUTO_RETRY_MS);
            fs_entered_at[fs_calls-1]=clock_ms;
            fs_active=1; fs_hotkey=0; fs_until=clock_ms+(scenario==36 ? 400u : scenario==33 ? 1000u : 100000u);
            return 0;
        }
        assert(scenario==8 && c->focused); fs_active=1; fs_until=clock_ms+100; return 0;
    case CIUKI_DOSVM_STATUS:
        c->state = clock_ms < 300 ? CIUKI_DOSVM_BOOTING : CIUKI_DOSVM_RUNNING;
        c->changed = armed ? 0 : CIUKI_DOSVM_VIDEO_READY;
        c->video_mode = 3;
        if (scenario == 1 && clock_ms >= 200) { c->state = CIUKI_DOSVM_STOPPED; c->reason = 2; strcpy(c->name, "Disk read error"); }
        if (entered && clock_ms > enter_ms + 500) {
            if (scenario == 3) { c->changed = CIUKI_DOSVM_LAUNCH_FAILED; strcpy(c->name, "Bad command or file name"); }
            else if (scenario != 5) { c->changed = CIUKI_DOSVM_VIDEO_READY; c->video_mode = scenario == 4 ? 3 : 19; }
        }
        if (scenario>=33) {
            if (c->video_mode==19) c->changed|=CIUKI_DOSVM_GRAPHICS;
            if (scenario==34 && !fs_user_in && entered && clock_ms>enter_ms+700) {
                /* The user's Alt+Enter in the window (the kernel's input path). */
                fs_user_in=1; fs_active=1; fs_hotkey=1; fs_until=clock_ms+300;
            }
            if (fs_active && clock_ms>=fs_until) {
                fs_active=0; if (!fs_return_at) fs_return_at=clock_ms;
                if (scenario==33) fs_hotkey=1;                     /* the user's Alt+Enter back */
                else if (scenario==34 || (scenario==36 && fs_calls==1)) fs_hotkey=0; /* a VBE mode change */
                else if (scenario==36) { fs_hotkey=0; fs_error=ETIMEDOUT; }   /* the first-frame watchdog */
            }
            if (fs_hotkey) c->changed|=CIUKI_DOSVM_FULLSCREEN_HOTKEY;
            if (fs_error) { c->changed|=CIUKI_DOSVM_FULLSCREEN_REFUSED; c->reason=(uint32_t)fs_error; }
        }
        if (fs_active && clock_ms>=fs_until) fs_active=0;
        if (fs_active) c->changed|=CIUKI_DOSVM_FULL_SCREEN;
        else if (scenario==8 && fs_command) { c->changed|=CIUKI_DOSVM_FULLSCREEN_REFUSED; c->reason=EIO; }
        if (clock_ms>=(scenario==17 ? 700u : 300u) && !entered) c->changed|=CIUKI_DOSVM_SHELL_READY;
        if ((scenario==18 || scenario==19) && presents) c->changed|=CIUKI_DOSVM_SAVE_FULL;
        if ((scenario==23 || scenario==24) && entered && clock_ms>enter_ms+20)
            c->changed=CIUKI_DOSVM_PROGRAM_RETURNED|CIUKI_DOSVM_SHELL_READY;
        /* 31: a game shown for 12 s returns (Close on exit closes the window);
         * 32: a game shown at 0.5 s returns at 2 s (Wolfenstein 3-D's memory
         * error after its signon screen): the window stays, "Finished - DOOM". */
        if ((scenario==31 && entered && clock_ms>enter_ms+12000) || (scenario==32 && entered && clock_ms>enter_ms+2000))
            c->changed|=CIUKI_DOSVM_PROGRAM_RETURNED|CIUKI_DOSVM_SHELL_READY;
        if (scenario==19 && presents && !fs_command) c->changed|=CIUKI_DOSVM_FULL_SCREEN;
        /* Decision 2026-10-24: EXIT at the prompt ends COMMAND, the wrapper writes
         * the control port and the VM stops with reason EXIT (0). */
        if ((scenario==29 || scenario==30) && presents && clock_ms>enter_ms+100) { c->state=CIUKI_DOSVM_STOPPED; c->reason=0; }
        return 0;
    case CIUKI_DOSVM_LAUNCH: assert(!armed); armed = 1; return 0;
    case CIUKI_DOSVM_INPUT:
        if (scenario==17) assert(clock_ms>=700);
        if (c->event.code==CIUKI_KEY_F12) { f12_forwarded++; return 0; }
        if (!input_refused++) { errno = EAGAIN; return -1; }
        if (c->event.code == CIUKI_KEY_ENTER && c->event.value == 1) {
            if (armed && scenario == 7 && !focus_loss) { focus_loss = 1; errno = EACCES; return -1; }
            if (armed) { entered = 1; enter_ms = clock_ms; }
            else { assert(console_at < sizeof(console) - 1); console[console_at++] = '\n'; }
        }
        return 0;
    case CIUKI_DOSVM_PRESENT_BANDS:
        if (scenario==0 && c->presentation.flags) { errno=EINVAL;return -1; } /* legacy kernel */
        if (!(last_events&CIUKI_DOSVM_EVENT_VIDEO)) { assert(c->presentation.redraw); nonvideo_presents++; }
        if (scenario != 6) assert(entered && clock_ms > enter_ms + (scenario==23 || scenario==24 ? 20u : 500u) && !error_seen);
        if (!presents) first_frame_ms=clock_ms;
        presents++;
        c->changed = 1;
        if(c->presentation.flags==CIUKI_DOSVM_PRESENT_SCANLINES) {
            memset(&c->scanlines,0,sizeof(c->scanlines));
            c->scanlines.bits[0]=(1u<<1)|(1u<<17);c->scanlines.bits[265/32]=1u<<(265%32);
        } else { c->presentation.bands[0] = 5; c->presentation.bands[1] = 2; }
        return 0;
    case CIUKI_DOSVM_CLOSE:
        if (scenario==25) { errno=EBUSY; return -1; }
        if (scenario==12 && !close_declined) { errno=EBUSY; return -1; }
        closed++; return 0;
    default: assert(0); return -1;
    }
}
/* Rendering primitives are already independently tested by deskui_test.
 * The replay observes panel text and keeps VM pixels distinguishable. */
void du_fill(struct du_canvas *c, struct du_rect r, uint32_t colour)
{ assert(c->pixels == screen); for (int y = r.y; y < r.y + r.h; y++) for (int x = r.x; x < r.x + r.w; x++) screen[y * 640 + x] = colour; }
void du_edge(struct du_canvas *c, struct du_rect r, enum du_edge e) { (void)c; (void)r; (void)e; }
void du_text_fit(struct du_canvas *c, int x, int y, int w, const char *s, uint32_t colour)
{
    (void)c; (void)x; (void)y; (void)w; (void)colour;
    assert(!strstr(s,"DOS kernel, memory and network"));
    if (!strcmp(s,"Full screen unavailable")) notice_seen++;
    if (!strcmp(s,"Back in the window")) back_seen++;   /* f3-61: the kernel ended full screen itself */
    if (!strcmp(s,"Game data is full")) save_full_notice++;
    if (!strncmp(s,"Loading ",8)) loading_seen++;
    if (!strncmp(s,"Checking the DOS system files: ",31)) { assert(scenario==20 || scenario==27 || scenario==28); checking_seen++; }
    if (!strcmp(s,"The DOS system files are damaged (checksum mismatch).")) damaged_seen++;
    if (!strcmp(s,"Running full screen — Alt+Enter to return")) fs_panel_seen++;
    if (!strcmp(s, "Unable to start") || !strcmp(s, "Program did not start") || !strcmp(s, "Startup timed out")) error_seen++;
}
int du_text_width(const char *s) { return (int)strlen(s)*7; }
const char *du_icon_name(const char *name) { assert(!strcmp(name, "program:dosbox")); return "utilities-terminal"; }
const struct du_icon *du_icon(const char *name, unsigned size)
{ assert(name && (size == 32 || size == 16)); return NULL; }
void du_icon_draw(struct du_canvas *c, int x, int y, const struct du_icon *icon, enum du_icon_mode mode)
{ (void)c; (void)x; (void)y; (void)icon; (void)mode; }
void desk_paint_frame(struct desk_paint *p,int x,int y,int w,int h,const struct desk_frame_style *s)
{ (void)p; (void)x; (void)y; (void)w; (void)h; if (s->title && !strcmp(s->title,"Finished - DOOM")) finished_panel++; }
struct du_rect du_intersection(struct du_rect a,struct du_rect b) { (void)b; return a; }
void du_text_font(struct du_canvas *c,int font,int x,int y,const char *s,uint32_t ink)
{ (void)font; du_text_fit(c,x,y,350,s,ink); if (!strcmp(s,"DOS kernel, memory and network")) c->pixels[y*640+x]=0x123456; }
int du_text_wrap(struct du_canvas *c,struct du_rect b,const char *s,uint32_t ink)
{
    du_text_fit(c,b.x,b.y,b.w,s,ink); return 1;
}
void du_progress_paint(struct du_canvas *c,struct du_rect b,int value,int total,uint64_t phase)
{
    (void)c; assert(b.x==120 && b.y==272 && b.w==400 && b.h==18);
    if (total && creating()) {         /* fx-23: the check CREATE waits for */
        assert((scenario==27 || scenario==28) && total==1000 && value>last_permille && !phase);
        last_permille=value; create_checking++; return;
    }
    if (total) {                       /* fx-4b determinate progress of the check */
        assert(scenario==20 && total==1000 && value>=0 && value<=1000 && value>last_permille && !phase);
        last_permille=value; image_paints++; return;
    }
    assert(!value); assert(!phase || phase==clock_ms);
}
void du_well(struct du_canvas *c,struct du_rect b,uint32_t ink) { du_fill(c,b,ink); }
int du_clipboard_set_n(const char *s,size_t n) { (void)s;(void)n; assert(0); return -1; }
const char *du_clipboard_get(void) { assert(0); return ""; }
int main(int argc, char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    assert(argc == 2 || argc == 3); scenario = atoi(argv[1]);
    if (argc==3) { nonvideo=(unsigned)atoi(argv[2]); assert(nonvideo==1 || nonvideo==2); }
    assert(scenario>=0 && scenario<=37);
    if (scenario>=13 && scenario<=16) {
        const char *options[]={"--dos-kernel=ciukidos","--dos-kernel=freedos-1.3","--dos-memory=locked","--dos-network=ipx"};
        char *refused[]={"dosbox",(char *)options[scenario-13]};
        assert(dosbox_main(2,refused)==2 && !phase && !closed && !presents);
        puts("PASS dosbox loading replay: retired kernel/settings CLI refused before VM/window creation");
        return 0;
    }
    char *launch[] = {"dosbox", "--name=DOOM", "\\DOOM\\DOOM.EXE", "-config", "CIUKI.CFG"};
    char *reference[]={"dosbox","--name=DOOM","--dos-kernel=unified","\\DOOM\\DOOM.EXE","-config","CIUKI.CFG"};
    struct dosbox_profile profile;
    if (scenario==34) assert(!dosbox_profile_usage_save("\\DOOM\\DOOM.EXE",DOSBOX_SCREEN_WINDOW));
    if ((scenario>=24 && scenario<=26) || scenario==30) {
        dosbox_profile_default(&profile,"\\DOOM\\DOOM.EXE -config CIUKI.CFG");
        if (scenario==24 || scenario==30) profile.close_on_exit=0; else profile.warn=0;
        assert(!dosbox_profile_save(&profile));
    }
    int selected=scenario==9 || scenario==10;
    int result = dosbox_main(selected ? 6 : scenario == 6 ? 1 : 5, selected ? reference : launch);
    if ((scenario>=24 && scenario<=26) || scenario==30) {
        char path[192]; assert(!dosbox_profile_path(profile.key,path,sizeof(path)) && !unlink(path));
        assert(!rmdir(DOSBOX_SETTINGS_DIR));
    }
    if (scenario==23 || scenario==24 || scenario==25 || scenario==26) {
        assert(!result && session_registered==1 && !error_seen && !close_declined);
        /* A program that returned 20 ms after its command, before any frame:
         * its text screen is shown in a "Finished - DOOM" window, whether or
         * not "Close on exit" is on (Windows 98 shows the program's output). */
        if (scenario==23 || scenario==24) assert(presents && closed==1 && title_set==1 && close_requests==1);
        if (scenario==25) assert(presents && !closed);
        if (scenario==26) assert(session_queries==1 && session_answers==1 && session_ends==1 && closed==1);
        printf("PASS G17 DOS program lifecycle: scenario=%d short EXEC keeps a Finished window, retain and per-program termination warning\n",scenario);
        return 0;
    }
    if (scenario==31 || scenario==32) {
        assert(!result && session_registered==1 && !error_seen && !close_declined && presents && closed==1);
        if (scenario==31) assert(!title_set && !close_requests && clock_ms>enter_ms+12000);
        else assert(title_set==1 && close_requests==1);
        printf("PASS G17 DOS program lifecycle: scenario=%d %s\n",scenario,scenario==31 ?
               "a program shown for 12 s closes its window on return (Close on exit)" :
               "a program that returned in its first seconds keeps the Finished - DOOM window");
        puts("PASS dosbox loading replay scenario"); return 0;
    }
    if (scenario==29 || scenario==30) {
        assert(!result && session_registered==1 && !error_seen && !close_declined && presents && closed==1);
        if (scenario==29) assert(!title_set && !finished_panel);
        else assert(title_set==1 && finished_panel>=1);
        printf("PASS EXIT at the DOS prompt: scenario=%d %s\n",scenario,
               scenario==29 ? "Close on exit on closes the window" : "Close on exit off keeps the Finished - DOOM window");
        puts("PASS dosbox loading replay scenario"); return 0;
    }
    if (scenario==28) {
        assert(!result && creating() && create_pongs==1 && !closed && !presents);
        create_release=1; du_job_finish(&create_job);
        printf("PASS fx-23 close during CREATE: window closed after %llu ms of CREATE, PING answered, no VM left\n",
               (unsigned long long)(clock_ms-create_started_ms));
        puts("PASS dosbox loading replay scenario=28"); return 0;
    }
    if (scenario==27) {
        assert(create_pongs==1 && create_checking==3 && last_permille==900 && create_pulses>=8);
        assert(create_polls>=60 && create_polls<=62);
        printf("PASS fx-23 slow CREATE: %llu ms in the kernel, PING answered, %u check paints to %d/1000, %u pulses, %u polls\n",
               (unsigned long long)(create_released_ms-create_started_ms),create_checking,last_permille,create_pulses,create_polls);
    } else assert(!create_pongs && !create_checking && create_polls<=1);
    if (scenario==11) assert(f12_forwarded==1 && !settings_damage);
    if (scenario==12) assert(close_declined==1);
    assert(session_registered==1);
    if (scenario==22) {
        assert(session_queries==1 && session_answers==1 && session_ends==1 && closed==1);
        puts("PASS G3 DOS session: registered, a running program answers DOS_ACTIVE (no Ctrl+C), END_SESSION closes the VM");
    }
    if (scenario==20) {
        assert(image_polls==7 && image_paints==6 && checking_seen>=6 && pongs==1 && pre_create_waits==6 && last_permille==900);
        printf("PASS fx-4b verify wait: %u progress paints to %d/1000, PING answered during the check, focus forwarded, %u ms\n",
               image_paints,last_permille,pre_create_waits*200u);
    } else assert(!image_paints && (scenario==22 || scenario==27 || !checking_seen) && image_polls==(scenario==21 || scenario==9 ? 2u : 1u));
    if (scenario==21) { assert(damaged_seen==1); puts("PASS fx-4b mismatch: CREATE EIO named as damaged DOS system files"); }
    if (scenario == 1 || scenario == 2 || scenario == 3 || scenario == 5 || scenario==9 || scenario==21)
        assert(result == 1 && error_seen && !presents && closed == (scenario != 2 && scenario!=9 && scenario!=21));
    else assert(!result && presents && !error_seen && closed == 1);
    if (scenario == 0 || scenario == 4 || scenario == 7) assert(armed && entered && console_at == 1);
    if (scenario == 6) assert(!armed && !entered);
    if (scenario==8) assert(fs_command && fs_panel_seen && !fs_active && back_seen==1 && !notice_seen);
    else assert(!back_seen || scenario==36);
    if (scenario<33) assert(fs_calls==(scenario==8 ? 1u : scenario==19 ? (unsigned)fs_command : 0u));
    {
        /* fx-dosfull: what the DOS Box remembered for \DOOM\DOOM.EXE. */
        struct dosbox_profile stored; dosbox_profile_default(&stored,"\\DOOM\\DOOM.EXE");
        int loaded=dosbox_profile_load(&stored);
        if (scenario==8) {
            /* The window menu's Full screen at a text frame: the whole session. */
            assert(loaded==1 && stored.screen==DOSBOX_SCREEN_FULL && !stored.command[0]);
        } else if (scenario==33) {
            /* Entered by itself at the graphics mode; Alt+Enter back: Window. */
            assert(fs_calls==1 && fs_entered_at[0]>enter_ms+500 && fs_entered_at[0]<=enter_ms+600 && fs_panel_seen);
            assert(loaded==1 && stored.screen==DOSBOX_SCREEN_WINDOW && !stored.command[0] && !notice_seen);
        } else if (scenario==34) {
            assert(fs_user_in && fs_calls==1 && loaded==1 && stored.screen==DOSBOX_SCREEN_GRAPHICS && !stored.command[0]);
        } else if (scenario==35) {
            assert(fs_calls==1 && !fs_panel_seen && !notice_seen && !loaded && presents>3);
        } else if (scenario==36) {
            /* Re-entry waits for the focus to settle after the kernel's return. */
            assert(fs_calls==2 && fs_entered_at[1]>=fs_return_at+DOSBOX_AUTO_RETURN_MS && back_seen==1 && !loaded);
        } else if (scenario==37) {
            assert(fs_calls==2 && !loaded && fs_panel_seen);
        } else assert(!loaded);
        if (loaded==1) { char path[192]; assert(!dosbox_profile_path(stored.key,path,sizeof(path)) && !unlink(path)); }
        (void)rmdir(DOSBOX_SETTINGS_DIR);
    }
    if (scenario>=33) printf("PASS fx-dosfull scenario=%d full-screen requests=%u (33 auto entry, Alt+Enter back remembered; 34 remembered Window, Alt+Enter remembered; 35 refusal: one request, window, nothing stored; 36 mode change re-enters, watchdog does not; 37 busy retried)\n",scenario,fs_calls);
    if (scenario==18 || scenario==19) assert(save_full_notice==1 && presents && !error_seen);
    if (scenario==19) assert(fs_command);
    if (error_seen) { assert(error_seen==1 && error_idle_waits==1);
        puts("PASS DOS error window: one infinite wait for 10 s modeled idle, close in one turn"); }
    /* fullscreen's static page shares the title; fx-4b: six progress pages and the Preparing page */
    if (scenario!=6) assert(loading_seen==(scenario==8 || scenario>=33 ? 1u+fs_panel_seen : scenario==20 ? 8u : scenario==27 ? 6u : 1u));
    assert(damage_count);
    if (scenario==0 && !nonvideo) {
        assert(first_frame_ms<=3000);
        printf("f3-63 client startup model: first_frame_ms=%llu (ready=300ms, guest first-frame work=500ms, CREATE excluded)\n",
               (unsigned long long)first_frame_ms);
    }
    if (nonvideo) {
        assert((nonvideo==1 ? timeout_waits : state_waits)>=4);
        assert(nonvideo_presents==(scenario==0 ? 1u : scenario==8 ? 2u : 0u));
        printf("PASS no-VIDEO events: TIMEOUT=%u STATE=%u redraw_presents=%u scenario=%d\n",timeout_waits,state_waits,nonvideo_presents,scenario);
    }
    printf("PASS dosbox loading replay scenario=%d full_damage=%u bar_updates=%u max_bar_per_second<=4 settings_damage=%u error_paints=%d notice_paints=%u loading_static=%u\n",
           scenario,full_damage,bar_updates,settings_damage,error_seen,notice_seen,loading_seen);
    return 0;
}
