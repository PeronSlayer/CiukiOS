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

/* This replay covers DOSBox loading and session handling. Clipboard behaviour
 * has its own production-client conformance test. */
void dos_clip_init(int channel, uint32_t *pixels) { (void)channel; (void)pixels; }
void dos_clip_vm(uint32_t id, uint32_t generation, uint32_t server) { (void)id; (void)generation; (void)server; }
int dos_clip_recv(struct ciuki_message *message) { (void)message; return 0; }
int dos_clip_action(unsigned action) { (void)action; return 0; }
int dos_clip_input(const struct desk_packet *packet) { (void)packet; return 0; }
void dos_clip_tick(int focused) { (void)focused; }
int dos_clip_marking(void) { return 0; }
int dos_clip_dirty(void) { return 0; }
void dos_clip_destroy(void) { }

static uint32_t screen[640 * 480];
static uint64_t clock_ms, enter_ms, first_frame_ms;
static int phase, scenario, armed, entered, presents, damage_count, error_seen, closed;
static unsigned error_idle_waits;
static unsigned nonvideo, timeout_waits, state_waits, last_events, nonvideo_presents;
static int input_refused;
static int focus_loss;
static int fs_command,fs_active,fs_panel_seen;
static uint64_t fs_until;
static unsigned settings_phase, close_declined, f12_forwarded;
static unsigned session_registered, session_queries, session_answers, session_ends; /* G3 scenario 22 */
static unsigned save_full_notice;
static unsigned bar_damage[256], bar_updates, full_damage, settings_damage, notice_seen, loading_seen, back_seen;
static uint64_t last_bar_at;
/* fx-4b: scenario 20 waits for the background check of the DOS system
 * files with progress; 21 is refused for a checksum mismatch. */
static unsigned image_polls, image_paints, checking_seen, pongs, damaged_seen, pre_create_waits;
static int last_permille = -1, vm_created;
static char console[256]; static unsigned console_at;
static const struct desk_theme replay_theme = {.face = 0xcccccc, .ink = 0, .select = 0x2266aa};
const struct desk_theme *desk_theme = &replay_theme;
static int replay_close(int fd) { assert(fd == 7 || fd == 3); return 0; }
static int replay_clock(clockid_t id, struct timespec *t)
{ assert(id == CLOCK_MONOTONIC); t->tv_sec = (time_t)(clock_ms / 1000); t->tv_nsec = (long)(clock_ms % 1000) * 1000000; return 0; }
static int replay_poll(struct pollfd *fds,nfds_t n,int timeout)
{
    assert(n==1 && fds[0].fd==3 && fds[0].events==POLLIN);
    if (timeout==200) { assert(scenario==20 && !vm_created); pre_create_waits++; clock_ms+=200; fds[0].revents=0; return 0; }
    assert(timeout==-1);error_idle_waits++;clock_ms+=10000;fds[0].revents=POLLIN;return 1;
}
int ciuki_surface_create(uint32_t w, uint32_t h, uint32_t f)
{ assert(w == 640 && h == 480 && f == CIUKI_SURFACE_XRGB8888); return 7; }
void *ciuki_surface_map(int fd, int prot)
{ assert(fd == 7 && prot == (PROT_READ | PROT_WRITE)); return screen; }
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
        else if (presents && !(scenario>=11 && desk_get32(m->data+16)==480)) {
            if(scenario==0)assert(h==8 && (y==0 || y==16 || y==264));
            else assert(h==1 && (y==1 || y==17 || y==265));
        } else { assert(screen[0] == replay_theme.face); full_damage++; }
    }
    /* G3: a running program cannot be closed automatically: the desktop's warning. */
    if (p.opcode==DESK_CLOSE_ACK) { assert(scenario==12 && p.flags==DESK_CLOSE_DOS_ACTIVE); close_declined++; }
    if (p.opcode==DESK_SESSION_ANSWER) {
        if (!p.serial) { assert(p.flags==DESK_SESSION_AGREE && !session_registered); session_registered++; }
        else { assert(scenario==22 && p.serial==5 && p.flags==DESK_SESSION_DOS_ACTIVE && session_registered); session_answers++; }
    }
    if (p.opcode==DESK_PONG) { assert(scenario==20 && !vm_created && desk_get32(m->data+4)==77); pongs++; }
    return 0;
}
int ciuki_channel_recv(int fd, struct ciuki_message *m, uint32_t flags)
{
    assert(fd == 3 && flags == DONTWAIT);
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
    } else if (scenario==22 && presents && clock_ms > enter_ms+650 && !session_queries) {
        desk_message(m,DESK_QUERY_END_SESSION,12); desk_put32(m->data+4,5); session_queries++;
    } else if (scenario==22 && session_answers==1 && !session_ends) {
        desk_message(m,DESK_END_SESSION,16); desk_put32(m->data+4,5); desk_put32(m->data+8,1); session_ends++;
    } else if (scenario==22) { errno = EAGAIN; return -1; }
    else if ((presents && clock_ms > (scenario==8 ? fs_until+3300 : enter_ms+650)) || (error_seen && clock_ms > 5000)) {
        desk_message(m, DESK_CLOSE_REQUEST, 4); phase++;
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
        c->id = 1; c->generation = 9; vm_created = 1; return 0;
    }
    assert(c->id == 1 && c->generation == 9);
    switch (op) {
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
        if (scenario==19) { assert(!c->focused); fs_command=1; return 0; }
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
        if (fs_active && clock_ms>=fs_until) fs_active=0;
        if (fs_active) c->changed|=CIUKI_DOSVM_FULL_SCREEN;
        else if (scenario==8 && fs_command) { c->changed|=CIUKI_DOSVM_FULLSCREEN_REFUSED; c->reason=EIO; }
        if (clock_ms>=(scenario==17 ? 700u : 300u) && !entered) c->changed|=CIUKI_DOSVM_SHELL_READY;
        if ((scenario==18 || scenario==19) && presents) c->changed|=CIUKI_DOSVM_SAVE_FULL;
        if (scenario==19 && presents && !fs_command) c->changed|=CIUKI_DOSVM_FULL_SCREEN;
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
        if (scenario != 6) assert(entered && clock_ms > enter_ms + 500 && !error_seen);
        if (!presents) first_frame_ms=clock_ms;
        presents++;
        c->changed = 1;
        if(c->presentation.flags==CIUKI_DOSVM_PRESENT_SCANLINES) {
            memset(&c->scanlines,0,sizeof(c->scanlines));
            c->scanlines.bits[0]=(1u<<1)|(1u<<17);c->scanlines.bits[265/32]=1u<<(265%32);
        } else { c->presentation.bands[0] = 5; c->presentation.bands[1] = 2; }
        return 0;
    case CIUKI_DOSVM_CLOSE:
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
    if (!strncmp(s,"Checking the DOS system files: ",31)) { assert(scenario==20); checking_seen++; }
    if (!strcmp(s,"The DOS system files are damaged (checksum mismatch).")) damaged_seen++;
    if (!strcmp(s,"Running full screen — Alt+Enter to return")) fs_panel_seen++;
    if (!strcmp(s, "Unable to start") || !strcmp(s, "Program did not start") || !strcmp(s, "Startup timed out")) error_seen++;
}
const char *du_icon_name(const char *name) { assert(!strcmp(name, "program:dosbox")); return "utilities-terminal"; }
const struct du_icon *du_icon(const char *name, unsigned size)
{ assert(name && (size == 32 || size == 16)); return NULL; }
void du_icon_draw(struct du_canvas *c, int x, int y, const struct du_icon *icon, enum du_icon_mode mode)
{ (void)c; (void)x; (void)y; (void)icon; (void)mode; }
void desk_paint_frame(struct desk_paint *p,int x,int y,int w,int h,const struct desk_frame_style *s)
{ (void)p; (void)x; (void)y; (void)w; (void)h; (void)s; }
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
    if (total) {                       /* fx-4b determinate progress of the check */
        assert(scenario==20 && total==1000 && value>=0 && value<=1000 && value>last_permille && !phase);
        last_permille=value; image_paints++; return;
    }
    assert(!value); assert(!phase || phase==clock_ms);
}
void du_well(struct du_canvas *c,struct du_rect b,uint32_t ink) { du_fill(c,b,ink); }
int main(int argc, char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);
    assert(argc == 2 || argc == 3); scenario = atoi(argv[1]);
    if (argc==3) { nonvideo=(unsigned)atoi(argv[2]); assert(nonvideo==1 || nonvideo==2); }
    assert(scenario>=0 && scenario<=22);
    if (scenario>=13 && scenario<=16) {
        const char *options[]={"--dos-kernel=ciukidos","--dos-kernel=freedos-1.3","--dos-memory=locked","--dos-network=ipx"};
        char *refused[]={"dosbox",(char *)options[scenario-13]};
        assert(dosbox_main(2,refused)==2 && !phase && !closed && !presents);
        puts("PASS dosbox loading replay: retired kernel/settings CLI refused before VM/window creation");
        return 0;
    }
    char *launch[] = {"dosbox", "--name=DOOM", "\\DOOM\\DOOM.EXE", "-config", "CIUKI.CFG"};
    char *reference[]={"dosbox","--name=DOOM","--dos-kernel=unified","\\DOOM\\DOOM.EXE","-config","CIUKI.CFG"};
    int selected=scenario==9 || scenario==10;
    int result = dosbox_main(selected ? 6 : scenario == 6 ? 1 : 5, selected ? reference : launch);
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
    } else assert(!image_paints && !checking_seen && image_polls==(scenario==21 || scenario==9 ? 2u : 1u));
    if (scenario==21) { assert(damaged_seen==1); puts("PASS fx-4b mismatch: CREATE EIO named as damaged DOS system files"); }
    if (scenario == 1 || scenario == 2 || scenario == 3 || scenario == 5 || scenario==9 || scenario==21)
        assert(result == 1 && error_seen && !presents && closed == (scenario != 2 && scenario!=9 && scenario!=21));
    else assert(!result && presents && !error_seen && closed == 1);
    if (scenario == 0 || scenario == 4 || scenario == 7) assert(armed && entered && console_at == 1);
    if (scenario == 6) assert(!armed && !entered);
    if (scenario==8) assert(fs_command && fs_panel_seen && !fs_active && back_seen==1 && !notice_seen);
    else assert(!back_seen);
    if (scenario==18 || scenario==19) assert(save_full_notice==1 && presents && !error_seen);
    if (scenario==19) assert(fs_command);
    if (error_seen) { assert(error_seen==1 && error_idle_waits==1);
        puts("PASS DOS error window: one infinite wait for 10 s modeled idle, close in one turn"); }
    /* fullscreen's static page shares the title; fx-4b: six progress pages and the Preparing page */
    if (scenario!=6) assert(loading_seen==(scenario==8 ? 2u : scenario==20 ? 8u : 1u));
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
