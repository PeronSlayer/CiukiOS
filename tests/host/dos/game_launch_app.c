/* Full DOS game launch, application side: the production /Programs/dosbox
 * (apps/dosbox/dosbox.c, unchanged) as the desktop starts it from
 * Programs > Games ("--name=DOOM" "\DOOM\DOOM.EXE -config CIUKI.CFG"), with
 * a scripted desktop (CONFIGURE, FOCUS, then DESK_CLOSED once a game frame
 * reached the window) and every CIUKI_DOSVM call served by the production
 * kernel boundary in game_launch_kernel.c. The kernel's clock is the
 * process clock; poll() lets kernel time pass. SPDX-License-Identifier: MIT */
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
static int launch_close(int);
static int launch_clock(clockid_t, struct timespec *);
static int launch_poll(struct pollfd *, nfds_t, int);
#define close launch_close
#define clock_gettime launch_clock
#define poll launch_poll
#define main game_dosbox_main
#include "../../../apps/dosbox/dosbox.c"
#undef main
#undef close
#undef clock_gettime
#undef poll

uint64_t game_now_ms(void);
void game_sleep(unsigned ms);
int game_dosvm(uint32_t op, struct ciuki_dosvm_command *c);
extern int game_mode;
int game_prompt_ready(void);
static uint64_t close_requested_at, window_closed_at;
uint64_t game_close_requested(void) { return close_requested_at; }
uint64_t game_window_closed(void) { return window_closed_at; }

uint32_t game_guest_screen[CIUKI_DOSVM_WIDTH * CIUKI_DOSVM_HEIGHT];
static uint32_t window_screen[CIUKI_DOSVM_WIDTH * CIUKI_DOSVM_HEIGHT];
static unsigned surfaces, phase, frames, failed_wait;
static char seen_text[32][96];
static unsigned seen_count;

/* G5 clipboard transport has its own fixture; the launch keeps it idle. */
void dos_clip_init(int channel, uint32_t *pixels) { assert(channel == 3 && pixels == game_guest_screen); }
void dos_clip_vm(uint32_t id, uint32_t generation, uint32_t server) { (void)id; (void)generation; (void)server; }
int dos_clip_recv(struct ciuki_message *m) { return ciuki_channel_recv(3, m, DONTWAIT); }
int dos_clip_action(unsigned action) { assert(!action); return 0; }
int dos_clip_input(const struct desk_packet *p) { (void)p; return 0; }
void dos_clip_tick(int focused) { (void)focused; }
int dos_clip_marking(void) { return 0; }
int dos_clip_pasting(void) { return 0; }
int dos_clip_dirty(void) { return 0; }
void dos_clip_destroy(void) {}
void dos_clip_settings(const char *typed) { (void)typed; }
int dos_clip_service(void) { assert(0); return 0; }

static const struct desk_theme launch_theme = {.face = 0xcccccc, .ink = 0, .select = 0x2266aa};
const struct desk_theme *desk_theme = &launch_theme;
int ciuki_dosvm(uint32_t op, struct ciuki_dosvm_command *c)
{ int e = game_dosvm(op, c); if (e) { errno = -e; return -1; } return 0; }
static int launch_close(int fd) { assert(fd == 7 || fd == 8 || fd == 3); return 0; }
static int launch_clock(clockid_t id, struct timespec *t)
{
    assert(id == CLOCK_MONOTONIC);
    uint64_t ms = game_now_ms();
    t->tv_sec = (time_t)(ms / 1000); t->tv_nsec = (long)(ms % 1000) * 1000000; return 0;
}
static int desktop_pending(void)
{
    if (phase < 2 || (phase == 2 && (failed_wait || game_now_ms() > 120000))) return 1;
    if (phase != 2 || window_closed_at) return 0;
    return game_mode == 0 ? frames != 0 : game_mode == 2 && game_prompt_ready();
}
static int launch_poll(struct pollfd *fds, nfds_t n, int timeout)
{
    assert(n == 1 && fds[0].fd == 3 && fds[0].events == POLLIN);
    if (desktop_pending()) { fds[0].revents = POLLIN; return 1; }
    if (timeout < 0) { failed_wait = 1; fds[0].revents = POLLIN; return 1; } /* failure panel: the user closes it */
    /* While CREATE runs on the DOS Box worker, its system call holds the
     * kernel CPU: this sleep starts when it returns. */
    game_sleep((unsigned)(timeout ? timeout : 1));
    fds[0].revents = 0; return 0;
}
int ciuki_surface_create(uint32_t w, uint32_t h, uint32_t f)
{ assert(w == 640 && h == 480 && f == CIUKI_SURFACE_XRGB8888); return 7 + (int)surfaces++; }
void *ciuki_surface_map(int fd, int prot)
{ assert((fd == 7 || fd == 8) && prot == (PROT_READ | PROT_WRITE)); return fd == 7 ? game_guest_screen : window_screen; }
int ciuki_surface_info(int fd, struct ciuki_surface_info *info)
{
    (void)fd; *info = (struct ciuki_surface_info){sizeof(*info), 640, 480, 2560, CIUKI_SURFACE_XRGB8888, sizeof(window_screen)};
    return 0;
}
/* A game frame reached the window: below the G17 toolbar, the colour the
 * guest drew (its first non-black pixel in the guest surface). */
static uint32_t guest_colour(void)
{
    for (unsigned i = 0; i < 640 * 480; i++) if (game_guest_screen[i]) return game_guest_screen[i];
    return 0;
}
static int window_has_game_frame(void)
{
    uint32_t colour = guest_colour();
    if (!colour || colour == launch_theme.face) return 0;
    for (unsigned i = DOSBOX_TOOLBAR * 640; i < 640 * 480; i++) if (window_screen[i] == colour) return 1;
    return 0;
}
int ciuki_channel_send(int fd, const struct ciuki_message *m, uint32_t flags)
{
    assert(fd == 3 && !flags);
    const struct ciuki_surface_info info = {sizeof(info), 640, 480, 2560, CIUKI_SURFACE_XRGB8888, sizeof(window_screen)};
    struct desk_packet p; assert(desk_parse(m, 1, &info, &p));
    if (p.opcode == DESK_CLOSE && !window_closed_at) {
        window_closed_at = game_now_ms(); if (!window_closed_at) window_closed_at = 1;
        printf("app: window closed (DESK_CLOSE) at t=%llu ms\n", (unsigned long long)game_now_ms());
    }
    if (p.opcode == DESK_DAMAGE && window_has_game_frame()) {
        if (!frames) printf("app: first guest frame in the window at t=%llu ms\n", (unsigned long long)game_now_ms());
        frames++;
    }
    return 0;
}
int ciuki_channel_recv(int fd, struct ciuki_message *m, uint32_t flags)
{
    assert(fd == 3 && flags == DONTWAIT);
    if (phase == 0) { desk_message(m, DESK_CONFIGURE, 12); desk_put32(m->data + 4, 640); desk_put32(m->data + 8, 480); phase++; }
    else if (phase == 1) { desk_message(m, DESK_FOCUS, 8); desk_put32(m->data + 4, 1); phase++; }
    else if (phase == 2 && failed_wait) { desk_message(m, DESK_CLOSED, 4); phase++; }
    else if (phase == 2 && game_now_ms() > 120000) { printf("app: window still open after 120 s\n"); desk_message(m, DESK_CLOSED, 4); phase++; }
    else if (phase == 2 && game_mode == 0 && frames) { desk_message(m, DESK_CLOSED, 4); phase++; }
    else if (phase == 2 && game_mode == 2 && game_prompt_ready()) {
        /* The close button at the idle prompt: "Closing..." on the caption. */
        close_requested_at = game_now_ms(); printf("desktop: close request at t=%llu ms\n", (unsigned long long)close_requested_at);
        desk_message(m, DESK_CLOSE_REQUEST, 4); phase++;
    }
    else { errno = EAGAIN; return -1; }
    m->data[0] = DESK_SHELL_VERSION; m->sender_pid = 999;
    return (int)m->length;
}
unsigned game_desktop_frames(void) { return frames; }
/* Panels are observed by their text; pixels are not checked here. */
void du_fill(struct du_canvas *c, struct du_rect r, uint32_t colour)
{
    for (int y = r.y; y < r.y + r.h; y++) for (int x = r.x; x < r.x + r.w; x++)
        if (x >= 0 && y >= 0 && x < 640 && y < 480) c->pixels[y * 640 + x] = colour;
}
void du_edge(struct du_canvas *c, struct du_rect r, enum du_edge e) { (void)c; (void)r; (void)e; }
void du_text_fit(struct du_canvas *c, int x, int y, int w, const char *s, uint32_t colour)
{
    (void)c; (void)x; (void)y; (void)w; (void)colour;

    for (unsigned i = 0; i < seen_count; i++) if (!strncmp(seen_text[i], s, sizeof(seen_text[i]) - 1)) return;
    if (seen_count < 32) snprintf(seen_text[seen_count++], sizeof(seen_text[0]), "%s", s);
    printf("app: window text \"%s\" at t=%llu ms\n", s, (unsigned long long)game_now_ms());
}
const char *du_icon_name(const char *name) { (void)name; return "utilities-terminal"; }
const struct du_icon *du_icon(const char *name, unsigned size) { (void)name; (void)size; return NULL; }
void du_icon_draw(struct du_canvas *c, int x, int y, const struct du_icon *icon, enum du_icon_mode mode)
{ (void)c; (void)x; (void)y; (void)icon; (void)mode; }
void desk_paint_frame(struct desk_paint *p, int x, int y, int w, int h, const struct desk_frame_style *s)
{ (void)p; (void)x; (void)y; (void)w; (void)h; (void)s; }
struct du_rect du_intersection(struct du_rect a, struct du_rect b) { (void)b; return a; }
void du_text_font(struct du_canvas *c, int font, int x, int y, const char *s, uint32_t ink)
{ (void)font; du_text_fit(c, x, y, 350, s, ink); }
int du_text_wrap(struct du_canvas *c, struct du_rect b, const char *s, uint32_t ink)
{ du_text_fit(c, b.x, b.y, b.w, s, ink); return 1; }
void du_progress_paint(struct du_canvas *c, struct du_rect b, int value, int total, uint64_t phase_ms)
{ (void)c; (void)b; (void)value; (void)total; (void)phase_ms; }
void du_well(struct du_canvas *c, struct du_rect b, uint32_t ink) { du_fill(c, b, ink); }
int du_clipboard_set_n(const char *s, size_t n) { (void)s; (void)n; assert(0); return -1; }
const char *du_clipboard_get(void) { assert(0); return ""; }
void game_debug_surfaces(void)
{
    unsigned guest = 0, window = 0; uint32_t gcolour = 0, wcolour = 0;
    for (unsigned i = 0; i < 640 * 480; i++) {
        if (game_guest_screen[i]) { guest++; if (!gcolour) gcolour = game_guest_screen[i]; }
        if (i >= DOSBOX_TOOLBAR * 640 && window_screen[i] && window_screen[i] != launch_theme.face) { window++; if (!wcolour) wcolour = window_screen[i]; }
    }
    printf("frame: guest nonzero=%u first=%06x row20[0]=%06x window nonface=%u first=%06x\n",
           guest, gcolour, game_guest_screen[20 * 640], window, wcolour);
}
