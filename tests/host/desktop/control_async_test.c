/* SPDX-License-Identifier: MIT
 * f2-88: the Control Panel's Display Properties never blocks the UI thread on a host call.
 * Fixture: T23-like (1024x768, slow or absent accel info, default image wallpaper whose decode
 * takes "seconds"): the first frame comes before the decode, every UI-thread host call is
 * counted, and the image arrives through the worker poll only. */
#include "control.h"
#include "display_fixture.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t pixels[CP_WIDTH * CP_HEIGHT], portrait[256 * 256];
static struct cp_app a; static struct du_ui u;
static unsigned sync_decodes, begins, polls, engine_calls, pending_polls;
static int decode_fails;
static char begun_path[128];
static int sync_decode(void *c, const char *p, struct desk_image *o) { (void)c; (void)p; (void)o; sync_decodes++; return -1; }
static int begin(void *c, const char *p) { (void)c; begins++; snprintf(begun_path, sizeof(begun_path), "%s", p); return 0; }
static int poll_done(void *c, char *p, size_t n, struct desk_image *o)
{
    (void)c; polls++;
    if (pending_polls) { pending_polls--; return 0; }
    snprintf(p, n, "%s", begun_path);
    if (decode_fails) { memset(o, 0, sizeof(*o)); return 1; }
    o->width = 64; o->height = 48; o->pixels = malloc(64 * 48 * 4); assert(o->pixels);
    for (unsigned i = 0; i < 64 * 48; i++) o->pixels[i] = 0xff3060c0u;
    return 1;
}
static int display(void *c, struct ciuki_display_info *d) { (void)c; display_fixture(d); return 0; }
static int engine_slow(void *c, struct ciuki_accel_info *i) { (void)c; (void)i; engine_calls++; return -EAGAIN; }
static int load(void *c, char *t, size_t s, size_t *n) { (void)c; (void)t; (void)s; (void)n; return -1; }
static void attach(int page, int engine, int async)
{
    memset(&a, 0, sizeof(a)); sync_decodes = begins = polls = engine_calls = 0;
    a.host = (struct cp_host){.load = load, .display = display, .portrait = portrait, .screen_w = 1024, .screen_h = 768, .decode = sync_decode,
                              .engine = engine ? engine_slow : NULL};
    if (async) { a.host.decode_begin = begin; a.host.decode_poll = poll_done; }
    du_init(&u, pixels, CP_WIDTH, CP_HEIGHT); u.focused_window = 1;
    assert(!cp_attach(&a, &u, page));
    snprintf(a.cfg.wallpaper, sizeof(a.cfg.wallpaper), "/system/wallpapers/default.png");
    a.preview_valid = 0; a.preview_pending = 0; u.dirty = 1;
}
static void settle(unsigned ticks)
{
    uint64_t t = u.now;
    for (unsigned i = 0; i < ticks; i++) { t += 60000000ull; du_tick(&u, t); du_render(&u); }
}
int main(void)
{
    static const struct { int engine, async, fails; unsigned slow; } cases[] = {
        {0, 1, 0, 0}, {1, 1, 0, 5}, {1, 1, 0, 40}, {0, 1, 1, 3}};
    for (unsigned k = 0; k < sizeof(cases) / sizeof(*cases); k++) {
        pending_polls = cases[k].slow; decode_fails = cases[k].fails;
        attach(CP_DISPLAY, cases[k].engine, cases[k].async);
        du_render(&u);                       /* the first frame */
        assert(sync_decodes == 0);           /* the UI thread never decodes or reads the wallpaper */
        assert(a.preview_valid);             /* a preview (desktop colour) is on screen already */
        settle(1);                           /* the deferred start fires; the worker starts */
        assert(begins == 1 && a.decode_wait && sync_decodes == 0);
        unsigned before = polls;
        settle(cases[k].slow + 2);
        assert(polls > before);              /* polled, never waited on */
        assert(!a.decode_wait && sync_decodes == 0 && begins == 1);
        if (cases[k].fails) assert(!a.image.pixels && !strcmp(a.decode_failed, a.cfg.wallpaper));
        else assert(a.image.pixels && !strcmp(a.image_path, a.cfg.wallpaper) && a.preview_valid);
        du_render(&u); settle(3);
        assert(begins == 1);                 /* no retry loop after a finished or failed decode */
        cp_detach(&a); desk_image_free(&a.image);
    }
    attach(CP_DISPLAY, 1, 1); du_render(&u);
    assert(strstr(a.engine_line, "status unavailable"));
    cp_detach(&a); desk_image_free(&a.image);
    puts("PASS control async display: first frame before decode, bounded host calls, failed decode does not loop");
    return 0;
}
