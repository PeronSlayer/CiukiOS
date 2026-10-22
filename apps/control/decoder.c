/* SPDX-License-Identifier: MIT */
#include "decoder.h"
#include <ciuki/channel.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __linux__
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#endif
#include <unistd.h>

#ifndef CP_THUMB_DIR
#define CP_THUMB_DIR "/System/cache/wallpaper/thumbnails"
#endif
#define PREVIEW_BYTES (192u * 144u * 4u)
/* One UI producer and one persistent worker. Messages wake sleepers; atomics
 * publish ownership. There is no mutex/futex on the completion path. */
static struct {
    int fd[2], started, state, cancelled, failed;
    pthread_t thread;
    struct cp_preview_req req;
    struct cp_preview_done done;
    uint32_t preview[192 * 144];
    cp_decoder_work work;
} d = {.fd = {-1, -1}};

static unsigned elapsed_ms(const struct timespec *a)
{
    struct timespec b; clock_gettime(CLOCK_MONOTONIC, &b);
    long long ms = (long long)(b.tv_sec - a->tv_sec) * 1000 + (b.tv_nsec - a->tv_nsec) / 1000000;
    return ms < 0 ? 0 : (unsigned)ms;
}
int cp_decoder_cancelled(void) { return __atomic_load_n(&d.cancelled, __ATOMIC_ACQUIRE); }
static int read_exact(int fd, void *data, size_t size)
{
    size_t n = 0;
    while (n < size) {
        if (cp_decoder_cancelled()) return -ECANCELED;
        size_t want = size - n > 65536 ? 65536 : size - n;
        ssize_t got = read(fd, (char *)data + n, want);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return got < 0 ? -errno : -EIO;
        n += (size_t)got;
    }
    return 0;
}
static int read_decode(const char *path, struct desk_image *out)
{
    char thumb[160];
    int system = cp_system_thumbnail(path, thumb, sizeof(thumb));
    if (system) {
        char full[256];
        snprintf(full, sizeof(full), "%s/%s", CP_THUMB_DIR, thumb);
        path = full;
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -errno;
        struct stat st;
        int r = fstat(fd, &st) ? -errno : st.st_size != PREVIEW_BYTES ? -EIO : 0;
        uint32_t *pixels = r ? NULL : malloc(PREVIEW_BYTES);
        if (!r && !pixels) r = -ENOMEM;
        if (!r) r = read_exact(fd, pixels, PREVIEW_BYTES);
        close(fd);
        if (r) { free(pixels); return r; }
        for (unsigned i = 0; i < 192 * 144; i++) pixels[i] |= 0xff000000u;
        *out = (struct desk_image){pixels, 192, 144};
        return 0; /* A missing/corrupt thumbnail NEVER falls back to its PNG. */
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    struct stat st;
    int r = fstat(fd, &st) ? -errno : !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > DESK_IMAGE_FILE_MAX ? -EFBIG : 0;
    size_t size = r ? 0 : (size_t)st.st_size;
    uint8_t *data = r ? NULL : malloc(size); /* one allocation, no realloc/copy ladder */
    if (!r && !data) r = -ENOMEM;
    if (!r) r = read_exact(fd, data, size);
    close(fd);
    if (!r && cp_decoder_cancelled()) r = -ECANCELED;
    if (!r) r = desk_image_decode(data, size, out);
    free(data);
    return r;
}
int cp_decoder_work_default(const struct cp_preview_req *req, struct cp_preview_done *out)
{
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    out->image = req->image;
    if (cp_decoder_cancelled()) return -ECANCELED;
    if (!out->image.pixels) {
        int r = read_decode(req->path, &out->image);
        out->decode_ms = elapsed_ms(&t0);
        if (r) return r;
    }
    if (cp_decoder_cancelled()) return -ECANCELED;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    desk_wallpaper_render_styled(d.preview, 192, 144, req->bar, req->bar, req->path, req->mode, req->colour,
                                 &out->image, req->portrait, req->font, req->label, req->aa);
    out->render_ms = elapsed_ms(&t0); out->rendered = 1;
    return 0;
}
static int send_token(int fd, unsigned stop)
{
    struct ciuki_message token = {0}; token.length = 4; token.data[0] = (uint8_t)stop;
    int r;
    do r = ciuki_channel_send(fd, &token, 0); while (r < 0 && errno == EINTR);
    return r;
}
static void *run(void *arg)
{
    (void)arg;
    for (;;) {
        struct ciuki_message token = {0};
        int r;
        do r = ciuki_channel_recv(d.fd[1], &token, 0); while (r < 0 && errno == EINTR);
        if (!r && token.length && token.data[0]) return NULL; /* orderly shutdown */
        if (r < 0 || !token.length) break;
        (void)__atomic_load_n(&d.state, __ATOMIC_ACQUIRE);
        struct cp_preview_req req = d.req;
        struct cp_preview_done done = {.req = req}; done.req.image = (struct desk_image){0};
        done.error = (d.work ? d.work : cp_decoder_work_default)(&req, &done);
        if (cp_decoder_cancelled()) done.error = -ECANCELED;
        if (done.error) { desk_image_free(&done.image); done.rendered = 0; }
        d.done = done;
        __atomic_store_n(&d.state, 2, __ATOMIC_RELEASE);
        if (send_token(d.fd[1], 0)) break;
    }
    /* A failed publication must wake the UI as HUP, rather than leave it
     * asleep forever with state=finished and no completion token. */
    __atomic_store_n(&d.failed, 1, __ATOMIC_RELEASE);
    close(d.fd[1]);
    d.fd[1] = -1; /* read by shutdown only after pthread_join */
    return NULL;
}
void cp_decoder_set_work(cp_decoder_work w) { d.work = w; } /* tests: only while stopped */
int cp_decoder_fd(void) { return d.fd[0]; }
int cp_decoder_begin(const struct cp_preview_req *req)
{
    if (__atomic_load_n(&d.failed, __ATOMIC_ACQUIRE)) return -EIO;
    if (__atomic_load_n(&d.state, __ATOMIC_ACQUIRE)) return -EBUSY;
    if (!d.started) {
        if (ciuki_channel_pair(d.fd)) return -errno;
        (void)fcntl(d.fd[0], F_SETFD, FD_CLOEXEC); (void)fcntl(d.fd[1], F_SETFD, FD_CLOEXEC);
        pthread_attr_t attr; int r = pthread_attr_init(&attr);
        if (!r) {
            r = pthread_attr_setstacksize(&attr, 65536);
            if (!r) r = pthread_create(&d.thread, &attr, run, NULL);
            pthread_attr_destroy(&attr);
        }
        if (r) { close(d.fd[0]); close(d.fd[1]); d.fd[0] = d.fd[1] = -1; return -r; }
        d.started = 1;
    }
    d.req = *req;
    __atomic_store_n(&d.cancelled, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&d.state, 1, __ATOMIC_RELEASE);
    if (send_token(d.fd[0], 0)) { __atomic_store_n(&d.state, 0, __ATOMIC_RELEASE); return -errno; }
    return 0;
}
int cp_decoder_poll(struct cp_preview_done *out)
{
    /* Called only after the completion endpoint is readable. No retry timer. */
    struct ciuki_message token;
    if (ciuki_channel_recv(d.fd[0], &token, DONTWAIT)) return 0;
    if (__atomic_load_n(&d.state, __ATOMIC_ACQUIRE) != 2) return 0;
    uint32_t *buf = out->preview;
    *out = d.done; out->preview = buf;
    if (cp_decoder_cancelled()) { desk_image_free(&out->image); out->error = -ECANCELED; out->rendered = 0; }
    if (out->rendered && buf) memcpy(buf, d.preview, PREVIEW_BYTES);
    memset(&d.done, 0, sizeof(d.done));
    __atomic_store_n(&d.state, 0, __ATOMIC_RELEASE);
    return 1;
}
void cp_decoder_cancel(void) { __atomic_store_n(&d.cancelled, 1, __ATOMIC_RELEASE); }
void cp_decoder_stop(void)
{
    if (!d.started) return;
    cp_decoder_cancel();
    if (!__atomic_load_n(&d.failed, __ATOMIC_ACQUIRE)) (void)send_token(d.fd[0], 1);
    pthread_join(d.thread, NULL); /* shutdown only, before freeing request assets */
    desk_image_free(&d.done.image); memset(&d.done, 0, sizeof(d.done));
    close(d.fd[0]); if (d.fd[1] >= 0) close(d.fd[1]); d.fd[0] = d.fd[1] = -1;
    d.started = 0; __atomic_store_n(&d.state, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&d.failed, 0, __ATOMIC_RELEASE);
}
