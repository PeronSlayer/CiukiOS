/* SPDX-License-Identifier: MIT
 * The production worker with real blocking host channels on one allowed CPU.
 * Kernel scheduler policy is tested separately by control_display_wait_test.c. */
#define _GNU_SOURCE
#include "decoder.h"
#include <ciuki/channel.h>
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#include <poll.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
void cp_channel_host_cleanup(void);
void cp_channel_host_fail_completion(void);
static sem_t release_job, started;
_Static_assert(sizeof(struct ciuki_message) == 288, "host channel transport size");
static int work(const struct cp_preview_req *req, struct cp_preview_done *out)
{
    (void)req; sem_post(&started); sem_wait(&release_job);
    out->decode_ms = 7; out->rendered = 0; return 0;
}
static unsigned long long now_ns(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec; }
static unsigned long long system_us(void)
{ struct rusage r; assert(!getrusage(RUSAGE_THREAD, &r)); return (unsigned long long)r.ru_stime.tv_sec * 1000000ull + r.ru_stime.tv_usec; }
static void take(struct cp_preview_done *out)
{
    struct pollfd p = {.fd = cp_decoder_fd(), .events = POLLIN};
    assert(poll(&p, 1, 2000) == 1 && p.revents == POLLIN);
    assert(cp_decoder_poll(out) == 1);
}
int main(int argc, char **argv)
{
    assert(argc == 3); assert(!chdir(argv[1]));
    cpu_set_t allowed, one; CPU_ZERO(&one);
    assert(!sched_getaffinity(0, sizeof(allowed), &allowed));
    for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &allowed)) { CPU_SET(i, &one); break; }
    assert(!sched_setaffinity(0, sizeof(one), &one)); /* inherited by worker */
    struct cp_preview_req req = {0}; struct cp_preview_done done; uint32_t pv[192 * 144];
    snprintf(req.path, sizeof(req.path), "/x.png");
    sem_init(&release_job, 0, 0); sem_init(&started, 0, 0);
    cp_decoder_set_work(work);
    int initial = cp_decoder_begin(&req); if (initial) fprintf(stderr, "worker start error=%d errno=%d\n", initial, errno); assert(!initial); sem_wait(&started);
    assert(cp_decoder_begin(&req) == -EBUSY);
    struct pollfd p = {.fd = cp_decoder_fd(), .events = POLLIN};
    unsigned long long cpu = system_us(), t = now_ns();
    assert(poll(&p, 1, 250) == 0); /* one wait, zero UI turns, no completion timer */
    unsigned long long cpu_wait = system_us() - cpu;
    assert(now_ns() - t >= 240000000ull && cpu_wait <= 2000);
    printf("single CPU: UI system time while blocked=%llu us / 250 ms\n", cpu_wait);
    sem_post(&release_job); memset(&done, 0, sizeof(done)); done.preview = pv; take(&done);
    assert(done.decode_ms == 7 && !strcmp(done.req.path, "/x.png"));
    assert(poll(&p, 1, 20) == 0); /* readiness consumed, no repeated completion */
    assert(!cp_decoder_begin(&req)); sem_wait(&started);
    t = now_ns(); cp_decoder_cancel(); assert(now_ns() - t < CP_DECODER_UI_SLICE_NS);
    sem_post(&release_job); take(&done); assert(done.error == -ECANCELED);
    cp_decoder_stop(); cp_channel_host_cleanup(); assert(cp_decoder_fd() == -1);
    cp_decoder_set_work(NULL);
    /* Every system image is served from its raw thumbnail. The fixture has
     * no /system/wallpapers originals; missing/corrupt thumbs must fail. */
    for (unsigned i = 1; i <= 3; i++) {
        memset(&req, 0, sizeof(req)); snprintf(req.path, sizeof(req.path), "/system/wallpapers/Ciuk%u.png", i);
        req.mode = CFG_WALL_FILL;
        assert(!cp_decoder_begin(&req)); take(&done);
        assert(!done.error && done.rendered && done.image.width == 192 && done.image.height == 144);
        desk_image_free(&done.image);
    }
    assert(!cp_system_thumbnail("/user/Ciuk1.png", (char[64]){0}, 64));
    assert(cp_system_thumbnail("/SYSTEM/WALLPAPERS/CIUK1.PNG", (char[64]){0}, 64));
    assert(!unlink("thumbnails/Ciuk1-192x144.raw"));
    snprintf(req.path, sizeof(req.path), "/system/wallpapers/Ciuk1.png");
    assert(!cp_decoder_begin(&req)); take(&done); assert(done.error && !done.image.pixels && !done.rendered);
    FILE *f = fopen("thumbnails/Ciuk2-192x144.raw", "wb"); assert(f); fputs("bad", f); fclose(f);
    snprintf(req.path, sizeof(req.path), "/system/wallpapers/Ciuk2.png");
    assert(!cp_decoder_begin(&req)); take(&done); assert(done.error && !done.image.pixels);
    /* A user-supplied uncompressed BMP exercises the actual read/decode path. */
    snprintf(req.path, sizeof(req.path), "user.bmp");
    assert(!cp_decoder_begin(&req)); take(&done);
    assert(!done.error && done.image.width == 2 && done.image.height == 2 && done.rendered);
    desk_image_free(&done.image);
    /* The full system asset, selected through a user path, is a real PNG
     * decode on the small persistent stack; system paths above never do it. */
    assert(strlen(argv[2]) < sizeof(req.path));
    snprintf(req.path, sizeof(req.path), "%s", argv[2]);
    assert(!cp_decoder_begin(&req)); take(&done);
    assert(!done.error && done.rendered && done.image.width == 1448 && done.image.height == 1086);
    req.image = done.image; /* render-only transfer returns ownership exactly once */
    assert(!cp_decoder_begin(&req)); take(&done);
    assert(!done.error && done.rendered && done.image.pixels == req.image.pixels);
    desk_image_free(&done.image);
    req.image = (struct desk_image){0}; snprintf(req.path, sizeof(req.path), "user.bmp");
    cp_channel_host_fail_completion(); assert(!cp_decoder_begin(&req));
    p = (struct pollfd){.fd = cp_decoder_fd(), .events = POLLIN};
    assert(poll(&p, 1, 2000) == 1 && (p.revents & POLLHUP));
    cp_decoder_stop(); cp_channel_host_cleanup(); assert(cp_decoder_fd() == -1);
    sem_destroy(&release_job); sem_destroy(&started);
    puts("PASS control decoder: blocking endpoint, single CPU, cancellation, thumbnail-only system paths, user decode, shutdown");
    return 0;
}
