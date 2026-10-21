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
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#include <unistd.h>
static int replay_close(int);
static int replay_clock(clockid_t, struct timespec *);
static int replay_sleep(const struct timespec *, struct timespec *);
#define close replay_close
#define clock_gettime replay_clock
#define nanosleep replay_sleep
#define main dosbox_main
#include "../../../apps/dosbox/dosbox.c"
#undef main
#undef close
#undef clock_gettime
#undef nanosleep

static uint32_t screen[640 * 480];
static uint64_t clock_ms, enter_ms;
static int phase, scenario, armed, entered, presents, damage_count, error_seen, closed;
static unsigned nonvideo, timeout_waits, state_waits, last_events, nonvideo_presents;
static int input_refused;
static int focus_loss;
static int fs_command,fs_active,fs_panel_seen;
static uint64_t fs_until;
static unsigned settings_phase,settings_seen,settings_blur,settings_restore;
static unsigned network_rows,network_selections;
static uint64_t settings_hold_until, last_bar_at;
static unsigned bar_damage[256], bar_updates, full_damage, settings_damage, notice_seen, loading_seen;
static uint32_t settings_original[640 * 480];
static const struct {
    unsigned kernel, memory, key, saved_kernel, saved_memory;
    const char *memory_arg;
} settings_cases[] = {
    /* Initial preferences, panel key, expected preferences after closing. */
    {0, 0, CIUKI_KEY_2, 1, 0, NULL}, /* 11: legacy kernel-only setting */
    {0, 0, 0,           0, 0, NULL}, /* 12: close with panel open */
    {0, 1, CIUKI_KEY_2, 1, 1, NULL}, /* 13: kernel choice keeps locked memory */
    {1, 1, CIUKI_KEY_1, 0, 1, NULL}, /* 14: return to CiukiDOS, keep locked */
    {1, 1, CIUKI_KEY_3, 1, 0, NULL}, /* 15: paged memory keeps FreeDOS */
    {1, 0, CIUKI_KEY_4, 1, 1, NULL}, /* 16: locked memory keeps FreeDOS */
    {0, 0, CIUKI_KEY_2, 1, 1, "--dos-memory=locked"}, /* 17: CLI lock */
    {0, 1, CIUKI_KEY_2, 1, 0, "--dos-memory=paged"},  /* 18: CLI unlock */
    {0, 0, CIUKI_KEY_1, 0, 0, "--dos-memory=paged"},  /* 19: duplicate selection changes no pixels */
    {0, 0, CIUKI_KEY_6, 0, 0, NULL}, /* 20: enable IPX without changing kernel/memory */
    {1, 1, CIUKI_KEY_5, 1, 1, NULL}, /* 21: disable IPX, longest kernel/memory labels */
    {0, 0, CIUKI_KEY_7, 2, 0, NULL}, /* 22: unified kernel on row 7, network untouched */
};
static char console[256]; static unsigned console_at;
static const struct desk_theme replay_theme = {.face = 0xcccccc, .ink = 0, .select = 0x2266aa};
const struct desk_theme *desk_theme = &replay_theme;
static int replay_close(int fd) { assert(fd == 7 || fd == 3); return 0; }
static int replay_clock(clockid_t id, struct timespec *t)
{ assert(id == CLOCK_MONOTONIC); t->tv_sec = (time_t)(clock_ms / 1000); t->tv_nsec = (long)(clock_ms % 1000) * 1000000; return 0; }
static int replay_sleep(const struct timespec *t, struct timespec *unused)
{ assert(!unused && !t->tv_sec); clock_ms += (uint64_t)t->tv_nsec / 1000000; return 0; }
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
            assert(h == 8 && (y == 0 || y == 16 || y == 264));
        } else { assert(screen[0] == replay_theme.face); full_damage++; }
    }
    return 0;
}
int ciuki_channel_recv(int fd, struct ciuki_message *m, uint32_t flags)
{
    assert(fd == 3 && flags == DONTWAIT);
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
    } else if (scenario>=11 && presents && settings_phase<(scenario==12 ? 2u : scenario==19 ? 8u : 6u)) {
        if (clock_ms<settings_hold_until) { errno=EAGAIN; return -1; }
        unsigned step=settings_phase++;
        if (step&1) { settings_hold_until=clock_ms+1100; errno=EAGAIN; return -1; }
        if (!step) memcpy(settings_original,screen,sizeof(screen));
        desk_message(m,DESK_KEY,44);
        desk_put32(m->data+16,CIUKI_INPUT_KEY);
        desk_put32(m->data+28,step==0 ? CIUKI_KEY_F12 : step==2 || (scenario==19 && step==4) ? settings_cases[scenario-11].key :
                   scenario==16 ? CIUKI_KEY_F12 : CIUKI_KEY_ESCAPE);
        desk_put32(m->data+32,1);
    } else if (scenario==12 && settings_phase==2) {
        desk_message(m,DESK_CLOSE_REQUEST,4);
    } else if ((presents && clock_ms > (scenario==8 ? fs_until+3300 : enter_ms+650)) || (error_seen && clock_ms > 5000)) {
        desk_message(m, DESK_CLOSE_REQUEST, 4); phase++;
    } else { errno = EAGAIN; return -1; }
    m->data[0] = DESK_SHELL_VERSION; m->sender_pid = 999;
    return (int)m->length;
}
int ciuki_dosvm(uint32_t op, struct ciuki_dosvm_command *c)
{
    assert(c->version == 1 && c->size == sizeof(*c));
    if (op == CIUKI_DOSVM_CREATE) {
        assert(c->surface_fd == 7);
        assert(c->launch.game==(scenario==6 ? 0u : 2u));
        assert(c->launch.kernel==(scenario>=11 ? settings_cases[scenario-11].kernel :
                                 (scenario==9 || scenario==10) ? 1u : 0u));
        assert(c->launch.memory==(scenario==17 ? 1u : scenario==18 ? 0u :
                                 scenario>=11 ? settings_cases[scenario-11].memory : 0u));
        if (scenario == 2 || scenario==9) { errno = scenario==9 ? EIO : ENOMEM; return -1; }
        c->id = 1; c->generation = 9; return 0;
    }
    assert(c->id == 1 && c->generation == 9);
    switch (op) {
    case CIUKI_DOSVM_WAIT_PRESENT:
        assert(c->wait.timeout_ms>0 && c->wait.timeout_ms<=50);
        clock_ms+=c->wait.timeout_ms; c->wait.frame_ms=15;
        c->wait.events=nonvideo==1 ? CIUKI_DOSVM_EVENT_TIMEOUT : nonvideo==2 ? CIUKI_DOSVM_EVENT_STATE : CIUKI_DOSVM_EVENT_VIDEO;
        last_events=c->wait.events;
        timeout_waits+=last_events==CIUKI_DOSVM_EVENT_TIMEOUT;
        state_waits+=last_events==CIUKI_DOSVM_EVENT_STATE;
        return 0;
    case CIUKI_DOSVM_FOCUS:
        if (scenario>=11 && settings_phase) { if (c->focused) settings_restore++; else settings_blur++; }
        return 0;
    case CIUKI_DOSVM_FULLSCREEN:
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
        return 0;
    case CIUKI_DOSVM_LAUNCH: assert(!armed); armed = 1; return 0;
    case CIUKI_DOSVM_INPUT:
        if (!input_refused++) { errno = EAGAIN; return -1; }
        if (c->event.code == CIUKI_KEY_ENTER && c->event.value == 1) {
            if (armed && scenario == 7 && !focus_loss) { focus_loss = 1; errno = EACCES; return -1; }
            if (armed) { entered = 1; enter_ms = clock_ms; }
            else { assert(console_at < sizeof(console) - 1); console[console_at++] = '\n'; }
        }
        return 0;
    case CIUKI_DOSVM_PRESENT_BANDS:
        if (!(last_events&CIUKI_DOSVM_EVENT_VIDEO)) { assert(c->presentation.redraw); nonvideo_presents++; }
        if (scenario != 6) assert(entered && clock_ms > enter_ms + 500 && !error_seen);
        presents++;
        c->changed = 1; c->presentation.bands[0] = 5; c->presentation.bands[1] = 2;
        return 0;
    case CIUKI_DOSVM_CLOSE: closed++; return 0;
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
    if (!strcmp(s,"DOS kernel, memory and network")) settings_seen++;
    if (!strcmp(s,"Full screen unavailable")) notice_seen++;
    if (!strncmp(s,"Loading ",8)) loading_seen++;
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
    if (strstr(s,"5  Network: off (default)\n6  Network: IPX (LAN)\n7  CiukiDOS unified (preview)\n")) network_rows++;
    if (!strncmp(s,"Kernel: ",8)) {
        const char *network=strstr(s,". Network: ");
        unsigned ipx=scenario==21;
        if (settings_phase>=3 && (scenario==20 || scenario==21)) ipx=scenario==20;
        assert(network && !strcmp(network,ipx ? ". Network: IPX (LAN)." : ". Network: off (default)."));
        network_selections++;
    }
    du_text_fit(c,b.x,b.y,b.w,s,ink); return 1;
}
void du_progress_paint(struct du_canvas *c,struct du_rect b,int value,int total,uint64_t phase)
{ (void)c; assert(b.x==120 && b.y==272 && b.w==400 && b.h==18); assert(!value && !total); assert(!phase || phase==clock_ms); }
void du_well(struct du_canvas *c,struct du_rect b,uint32_t ink) { du_fill(c,b,ink); }
int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3); scenario = atoi(argv[1]);
    if (argc==3) { nonvideo=(unsigned)atoi(argv[2]); assert(nonvideo==1 || nonvideo==2); }
    assert(scenario>=0 && scenario<11+(int)(sizeof(settings_cases)/sizeof(settings_cases[0])));
    char *launch[] = {"dosbox", "--name=DOOM", "\\DOOM\\DOOM.EXE", "-config", "CIUKI.CFG",
                      scenario>=17 ? (char *)settings_cases[scenario-11].memory_arg : NULL};
    char *reference[]={"dosbox","--name=DOOM","--dos-kernel=freedos-1.3","\\DOOM\\DOOM.EXE","-config","CIUKI.CFG"};
    if (scenario>=11) {
        assert(!dosbox_settings_save(2,settings_cases[scenario-11].kernel,settings_cases[scenario-11].memory));
        assert(dosbox_setting_load(2)==settings_cases[scenario-11].kernel);
        assert(dosbox_memory_load(2)==settings_cases[scenario-11].memory);
    } else assert(!dosbox_settings_save(2,0,0));
    int selected=scenario==9 || scenario==10;
    assert(!dosbox_network_save(2,scenario==21 ? CIUKI_DOSVM_NETWORK_IPX : CIUKI_DOSVM_NETWORK_OFF));
    int result = dosbox_main(selected || launch[5] ? 6 : scenario == 6 ? 1 : 5, selected ? reference : launch);
    if (selected) assert(dosbox_setting_load(2)==1);
    if (scenario>=11) {
        assert(dosbox_setting_load(2)==settings_cases[scenario-11].saved_kernel);
        assert(dosbox_memory_load(2)==settings_cases[scenario-11].saved_memory);
        assert(dosbox_network_load(2)==(scenario==20 ? CIUKI_DOSVM_NETWORK_IPX : CIUKI_DOSVM_NETWORK_OFF));
        assert(settings_seen && settings_blur && (scenario==12 || settings_restore));
        if (scenario!=12) assert(!memcmp(screen,settings_original,sizeof(screen)));
        assert(settings_seen==(scenario==12 ? 1u : 2u));
        assert(network_rows==settings_seen && network_selections==settings_seen);
        assert(settings_damage==settings_seen+(scenario!=12)); /* Restore only the saved rectangle. */
    }
    if (scenario == 1 || scenario == 2 || scenario == 3 || scenario == 5 || scenario==9)
        assert(result == 1 && error_seen && !presents && closed == (scenario != 2 && scenario!=9));
    else assert(!result && presents && !error_seen && closed == 1);
    if (scenario == 0 || scenario == 4 || scenario == 7) assert(armed && entered && console_at == 2);
    if (scenario == 6) assert(!armed && !entered);
    if (scenario==8) assert(fs_command && fs_panel_seen && !fs_active && notice_seen==1);
    if (error_seen) assert(error_seen==1);
    if (scenario!=6) assert(loading_seen==(scenario==8 ? 2u : 1u)); /* fullscreen's static page shares the title */
    assert(damage_count);
    if (nonvideo) {
        assert((nonvideo==1 ? timeout_waits : state_waits)>10);
        assert(nonvideo_presents==(scenario==0 ? 1u : scenario==8 ? 2u : 0u));
        printf("PASS no-VIDEO events: TIMEOUT=%u STATE=%u redraw_presents=%u scenario=%d\n",timeout_waits,state_waits,nonvideo_presents,scenario);
    }
    printf("PASS dosbox loading replay scenario=%d full_damage=%u bar_updates=%u max_bar_per_second<=4 settings_paints=%u settings_damage=%u error_paints=%d notice_paints=%u loading_static=%u\n",
           scenario,full_damage,bar_updates,settings_seen,settings_damage,error_seen,notice_seen,loading_seen);
    return 0;
}
