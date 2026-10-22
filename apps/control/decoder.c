/* SPDX-License-Identifier: MIT */
#include "decoder.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct {
    pthread_mutex_t lock; int state;                 /* 0 idle, 1 running, 2 finished */
    struct cp_preview_req req; struct cp_preview_done done;
    uint32_t *preview;                               /* worker-owned result buffer */
    cp_decoder_work work;
} d = {.lock = PTHREAD_MUTEX_INITIALIZER};

static unsigned elapsed_ms(const struct timespec *a)
{
    struct timespec b; clock_gettime(CLOCK_MONOTONIC, &b);
    long long ms = (long long)(b.tv_sec - a->tv_sec) * 1000 + (b.tv_nsec - a->tv_nsec) / 1000000;
    return ms < 0 ? 0 : (unsigned)ms;
}
static int read_decode(const char *path, struct desk_image *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = 1 << 16, n = 0; uint8_t *data = malloc(cap);
    while (data) {
        if (n == cap) { if (cap >= DESK_IMAGE_FILE_MAX) { free(data); data = NULL; break; } uint8_t *g = realloc(data, cap * 2); if (!g) { free(data); data = NULL; break; } data = g; cap *= 2; }
        size_t got = fread(data + n, 1, cap - n, f);
        if (!got) break;
        n += got;
    }
    fclose(f);
    int r = data ? desk_image_decode(data, n, out) : -1;
    free(data);
    return r;
}
int cp_decoder_work_default(const struct cp_preview_req *req, struct cp_preview_done *out)
{
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    out->image = req->image;
    if (!out->image.pixels) {
        if (read_decode(req->path, &out->image)) memset(&out->image, 0, sizeof(out->image));
        out->decode_ms = elapsed_ms(&t0);
    }
    if (!out->image.pixels) return 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    desk_wallpaper_render_styled(d.preview, 192, 144, req->bar, req->bar, req->path, req->mode, req->colour,
                                 &out->image, req->portrait, req->font, req->label, req->aa);
    out->render_ms = elapsed_ms(&t0); out->rendered = 1;
    return 0;
}
static void *run(void *arg)
{
    (void)arg;
    struct cp_preview_req req; struct cp_preview_done done;
    pthread_mutex_lock(&d.lock); req = d.req; cp_decoder_work work = d.work ? d.work : cp_decoder_work_default; pthread_mutex_unlock(&d.lock);
    memset(&done, 0, sizeof(done)); done.req = req; done.req.image = (struct desk_image){0};
    work(&req, &done);                               /* no lock held: the UI thread never waits on a job */
    pthread_mutex_lock(&d.lock); d.done = done; d.state = 2; pthread_mutex_unlock(&d.lock);
    return NULL;
}
void cp_decoder_test_lock(int hold) { if (hold) pthread_mutex_lock(&d.lock); else pthread_mutex_unlock(&d.lock); }
void cp_decoder_set_work(cp_decoder_work w) { d.work = w; }
int cp_decoder_begin(const struct cp_preview_req *req)
{
    pthread_t t; int r = 0;
    if (pthread_mutex_trylock(&d.lock)) return -EBUSY;
    if (d.state) r = -EBUSY;
    else if (!d.preview && !(d.preview = malloc(192 * 144 * 4))) r = -ENOMEM;
    else { d.req = *req; d.state = 1; }
    pthread_mutex_unlock(&d.lock);
    if (r) return r;
    r = pthread_create(&t, NULL, run, NULL);
    if (r) { pthread_mutex_lock(&d.lock); d.state = 0; pthread_mutex_unlock(&d.lock); return -r; }
    pthread_detach(t);
    return 0;
}
int cp_decoder_poll(struct cp_preview_done *out)
{
    int done = 0;
    if (pthread_mutex_trylock(&d.lock)) return 0;    /* the worker is publishing: next poll */
    if (d.state == 2) {
        uint32_t *buf = out->preview;
        *out = d.done; out->preview = buf;
        if (out->rendered && buf) memcpy(buf, d.preview, 192 * 144 * 4);
        memset(&d.done, 0, sizeof(d.done)); d.state = 0; done = 1;
    }
    pthread_mutex_unlock(&d.lock);
    return done;
}
