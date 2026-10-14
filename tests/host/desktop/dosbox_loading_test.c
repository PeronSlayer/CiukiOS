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
static int input_refused;
static int focus_loss;
static int fs_command,fs_active,fs_panel_seen;
static uint64_t fs_until;
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
        if (fs_panel_seen && desk_get32(m->data+16)==480) assert(!desk_get32(m->data+8));
        else if (presents) {
            unsigned y = desk_get32(m->data + 8), h = desk_get32(m->data + 16);
            assert(h == 8 && (y == 0 || y == 16 || y == 264));
        } else assert(screen[0] == replay_theme.face);
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
    } else if ((presents && clock_ms > enter_ms + 650) || (error_seen && clock_ms > 5000)) {
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
        if (scenario == 2) { errno = ENOMEM; return -1; }
        c->id = 1; c->generation = 9; return 0;
    }
    assert(c->id == 1 && c->generation == 9);
    switch (op) {
    case CIUKI_DOSVM_FOCUS: return 0;
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
{ (void)font; du_text_fit(c,x,y,350,s,ink); }
int du_text_wrap(struct du_canvas *c,struct du_rect b,const char *s,uint32_t ink)
{ du_text_fit(c,b.x,b.y,b.w,s,ink); return 1; }
void du_progress_paint(struct du_canvas *c,struct du_rect b,int value,int total,uint64_t phase)
{ (void)c; (void)b; (void)value; (void)total; (void)phase; }
void du_well(struct du_canvas *c,struct du_rect b,uint32_t ink) { du_fill(c,b,ink); }
int main(int argc, char **argv)
{
    assert(argc == 2); scenario = atoi(argv[1]);
    char *launch[] = {"dosbox", "--name=DOOM", "\\DOOM\\DOOM.EXE", "-config", "CIUKI.CFG"};
    int result = dosbox_main(scenario == 6 ? 1 : 5, launch);
    if (scenario == 1 || scenario == 2 || scenario == 3 || scenario == 5)
        assert(result == 1 && error_seen && !presents && closed == (scenario != 2));
    else assert(!result && presents && !error_seen && closed == 1);
    if (scenario == 0 || scenario == 4 || scenario == 7) assert(armed && entered && console_at == 2);
    if (scenario == 6) assert(!armed && !entered);
    if (scenario==8) assert(fs_command && fs_panel_seen && !fs_active);
    assert(damage_count);
    puts("PASS dosbox loading replay");
    return 0;
}
