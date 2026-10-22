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
#include <time.h>
static uint32_t pixels[CP_WIDTH * CP_HEIGHT], portrait[256 * 256];
static struct cp_app a; static struct du_ui u;
static unsigned sync_decodes, begins, polls, engine_calls, pending_polls;
static int decode_fails;
static char begun_path[128];
static struct desk_image held;
static int sync_decode(void *c, const char *p, struct desk_image *o) { (void)c; (void)p; (void)o; sync_decodes++; return -1; }
static struct cp_preview_req last_req;
static unsigned render_jobs, image_jobs;
static int begin(void *c, const struct cp_preview_req *r)
{
    (void)c; begins++; snprintf(begun_path, sizeof(begun_path), "%s", r->path); last_req = *r;
    if (r->image.pixels) { render_jobs++; held = r->image; } else image_jobs++;
    return 0;
}
static int poll_done(void *c, struct cp_preview_done *o)
{
    (void)c; polls++;
    if (pending_polls) { pending_polls--; return 0; }
    memset(o->preview, 0, 0); o->req = last_req; o->req.image = (struct desk_image){0};
    if (held.pixels) { o->image = held; held = (struct desk_image){0}; o->rendered = 1; for (unsigned i = 0; i < 192 * 144; i++) o->preview[i] = 0xff112233u; return 1; }
    if (decode_fails) return 1;
    o->image.width = 64; o->image.height = 48; o->image.pixels = malloc(64 * 48 * 4); assert(o->image.pixels);
    for (unsigned i = 0; i < 64 * 48; i++) o->image.pixels[i] = 0xff3060c0u;
    o->rendered = 1; for (unsigned i = 0; i < 192 * 144; i++) o->preview[i] = 0xff445566u;
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
    /* f2-88b: image already decoded; a mode change is a render-only job (image moved to the worker),
     * the old preview stays until it is done, and the UI thread never resamples. */
    pending_polls = 0; decode_fails = 0; render_jobs = image_jobs = 0;
    attach(CP_DISPLAY, 0, 1); du_render(&u); settle(3);
    assert(a.image.pixels && image_jobs == 1 && render_jobs == 0 && a.preview_valid && a.preview[0] == 0xff445566u);
    a.cfg.wallpaper_mode = a.cfg.wallpaper_mode == CFG_WALL_FILL ? CFG_WALL_FIT : CFG_WALL_FILL; a.preview_valid = 0; u.dirty = 1;
    pending_polls = 3; du_render(&u);
    assert(render_jobs == 1 && a.decode_wait && !a.image.pixels && a.preview[0] == 0xff445566u);
    a.cfg.theme[0] = a.cfg.theme[0] == 'x' ? 'y' : 'x'; a.preview_valid = 0; u.dirty = 1;   /* a change while the job runs */
    du_render(&u); settle(1);
    assert(render_jobs == 1 && image_jobs == 1);   /* no second job, no stuck timer: still polled */
    settle(6);
    assert(!a.decode_wait && a.image.pixels && a.preview_valid && sync_decodes == 0);
    cp_detach(&a); desk_image_free(&a.image);
    /* The worker never finishes: every UI turn stays far below the 100 ms ping bound (no decode, no resample). */
    pending_polls = 1u << 30; decode_fails = 0;
    attach(CP_DISPLAY, 1, 1); du_render(&u);
    {
        struct timespec t0, t1; unsigned long long worst = 0;
        for (unsigned i = 0; i < 40; i++) {
            clock_gettime(CLOCK_MONOTONIC, &t0); settle(1); clock_gettime(CLOCK_MONOTONIC, &t1);
            unsigned long long ns = (unsigned long long)(t1.tv_sec - t0.tv_sec) * 1000000000ull + (unsigned long long)(t1.tv_nsec - t0.tv_nsec);
            if (ns > worst) worst = ns;
        }
        assert(a.decode_wait && worst < 100000000ull);
    }
    cp_detach(&a); desk_image_free(&a.image);
    /* f2-88b: the preview reaches the canvas by row copies, pixel-exact, in a few ms of UI time. */
    pending_polls = 0; attach(CP_DISPLAY, 0, 1); du_render(&u); settle(3);
    assert(a.preview_valid && a.preview[20 * 192 + 150] == 0xff445566u);
    {
        struct timespec t0, t1; u.dirty = 1;
        clock_gettime(CLOCK_MONOTONIC, &t0); du_render(&u); clock_gettime(CLOCK_MONOTONIC, &t1);
        assert(u.canvas.pixels[(52 + 20) * CP_WIDTH + 32 + 150] == 0xff445566u);
        assert((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec) < 50000000ll);   /* 5 ms target on the T23; 50 ms host/ASan ceiling */
    }
    cp_detach(&a); desk_image_free(&a.image);
    pending_polls = 0;
    attach(CP_DISPLAY, 1, 1); du_render(&u);
    assert(strstr(a.engine_line, "status unavailable"));
    cp_detach(&a); desk_image_free(&a.image);
    puts("PASS control async display: first frame before decode, bounded host calls, failed decode does not loop");
    return 0;
}
