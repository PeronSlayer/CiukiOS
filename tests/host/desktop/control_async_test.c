/* SPDX-License-Identifier: MIT
 * f2-88: the Control Panel's Display Properties never blocks the UI thread on a host call.
 * Fixture: T23-like (1024x768, slow or absent accel info, default image wallpaper whose decode
 * takes "seconds"): the first frame comes before the decode, every UI-thread host call is
 * counted, and the image arrives through the worker poll only. */
#include "decoder.h"
#include "display_fixture.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __linux__
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#endif
#include <unistd.h>
#include <poll.h>
void cp_channel_host_cleanup(void);
static int real_begin(void *c, const struct cp_preview_req *r) { (void)c; return cp_decoder_begin(r); }
static int real_take(void *c, struct cp_preview_done *r) { (void)c; return cp_decoder_poll(r); }
static void real_cancel(void *c) { (void)c; cp_decoder_cancel(); }
static uint32_t pixels[CP_WIDTH * CP_HEIGHT], portrait[256 * 256];
static struct cp_app a; static struct du_ui u;
static unsigned sync_decodes, begins, polls, engine_calls, pending_polls, cancels;
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
static void cancel(void *c) { (void)c; cancels++; }
static int display(void *c, struct ciuki_display_info *d) { (void)c; display_fixture(d); return 0; }
static int engine_slow(void *c, struct ciuki_accel_info *i) { (void)c; (void)i; engine_calls++; return -EAGAIN; }
static int load(void *c, char *t, size_t s, size_t *n) { (void)c; (void)t; (void)s; (void)n; return -1; }
static void attach(int page, int engine, int async)
{
    memset(&a, 0, sizeof(a)); sync_decodes = begins = polls = engine_calls = 0;
    a.host = (struct cp_host){.load = load, .display = display, .portrait = portrait, .screen_w = 1024, .screen_h = 768, .decode = sync_decode,
                              .engine = engine ? engine_slow : NULL};
    if (async) { a.host.decode_begin = begin; a.host.decode_poll = poll_done; a.host.decode_cancel = cancel; }
    du_init(&u, pixels, CP_WIDTH, CP_HEIGHT); u.focused_window = 1;
    assert(!cp_attach(&a, &u, page));
    snprintf(a.cfg.wallpaper, sizeof(a.cfg.wallpaper), "/system/wallpapers/default.png");
    a.preview_valid = 0; a.preview_pending = 0; u.dirty = 1;
}
static void settle(unsigned ticks)
{
    uint64_t t = u.now;
    for (unsigned i = 0; i < ticks; i++) { t += 60000000ull; du_tick(&u, t); if (a.decode_wait) cp_preview_ready(&a); du_render(&u); }
}
int main(int argc, char **argv)
{
    assert(argc == 3);
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
    /* Pending work has no timer: ticks cannot query the worker. Every tab
     * cancels once and accepts input immediately, even if work never returns. */
    pending_polls = 1u << 30; attach(CP_DISPLAY, 0, 1); du_render(&u); settle(1);
    unsigned before = polls; cancels = 0;
    for (unsigned i = 0; i < 500; i++) du_tick(&u, u.now + 1000000ull);
    assert(polls == before && du_next_deadline(&u) == UINT64_MAX);
    for (int tab = 1; tab < CP_DISPLAY_TABS; tab++) {
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        u.event(&u, &(struct du_event){.type=DU_EV_SELECT,.id=CP_DISPLAY_TABS_ID,.index=tab}, u.user);
        du_render(&u); clock_gettime(CLOCK_MONOTONIC, &t1);
        assert(a.display_tab == tab && cancels == 1);
        assert((t1.tv_sec-t0.tv_sec)*1000000000ll+t1.tv_nsec-t0.tv_nsec < 100000000ll);
    }
    pending_polls = 0; cp_preview_ready(&a);
    assert(!a.decode_wait && !a.image.pixels); /* cancelled result cannot reappear on another tab */
    cp_detach(&a);
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
    /* The merged asset job may publish the portrait while a preview is in
     * flight. Keep the image, but render again with the newly owned asset. */
    pending_polls = 0; decode_fails = 0; attach(CP_DISPLAY, 0, 1);
    du_render(&u); du_tick(&u,u.now+20000000ull); du_render(&u);
    assert(a.decode_wait && last_req.portrait == portrait);
    a.host.portrait = NULL; cp_preview_ready(&a);
    assert(a.image.pixels && !a.preview_valid);
    du_render(&u); assert(a.decode_wait && !last_req.portrait && last_req.image.pixels);
    cp_preview_ready(&a); du_render(&u); assert(a.preview_valid);
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
    /* Golden frames through the actual application/worker/thumbnail path. */
    assert(!chdir(argv[1]));
    FILE *f = fopen(argv[2], "rb"); assert(f && fread(portrait, 4, 256 * 256, f) == 256 * 256); fclose(f);
    for (unsigned i = 1; i <= 3; i++) {
        attach(CP_DISPLAY, 0, 0);
        a.host.decode_begin = real_begin; a.host.decode_poll = real_take; a.host.decode_cancel = real_cancel;
        snprintf(a.cfg.wallpaper, sizeof(a.cfg.wallpaper), "/system/wallpapers/Ciuk%u.png", i);
        du_render(&u); du_tick(&u, u.now + 20000000ull); du_render(&u);
        assert(a.decode_wait);
        struct pollfd p = {.fd = cp_decoder_fd(), .events = POLLIN};
        assert(poll(&p, 1, 3000) == 1 && p.revents == POLLIN);
        cp_preview_ready(&a); du_render(&u);
        assert(!a.decode_wait && a.preview_valid && a.image.width == 192 && a.image.height == 144);
        char name[64]; snprintf(name, sizeof(name), "control-thumbnail-%u.xrgb", i);
        f = fopen(name, "wb"); assert(f && fwrite(pixels, 4, CP_WIDTH * CP_HEIGHT, f) == CP_WIDTH * CP_HEIGHT); fclose(f);
        cp_decoder_stop(); cp_channel_host_cleanup(); cp_detach(&a);
    }
    puts("PASS control async display: first frame before decode, bounded host calls, failed decode does not loop");
    return 0;
}
