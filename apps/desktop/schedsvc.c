/* SPDX-License-Identifier: MIT */
#include "schedsvc.h"
#include "desktop.h"
#include "../systools/systools.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct queued { char program[48], argument[161]; };
struct desk_schedsvc {
    struct desk_schedsvc_io io;
    struct ss_service service;
    pthread_mutex_t lock; pthread_cond_t changed; pthread_t thread;
    struct queued queue[DESK_SCHED_QUEUE]; unsigned head, count;
    int thread_started, stopping;
    struct desktop *desktop;
    volatile int32_t local_offset;      /* seconds, set by the frame thread */
};

/* ---- native file and clock (the defaults) ---- */
static int file_load(void *ctx, char *text, size_t cap, size_t *length)
{
    (void)ctx;
    FILE *f = fopen(SS_PATH, "rb");
    if (!f) return -(errno ? errno : ENOENT);
    size_t n = fread(text, 1, cap, f);
    int bad = ferror(f), more = !bad && n == cap && fgetc(f) != EOF;
    fclose(f);
    if (bad) return -EIO;
    if (more) return -EFBIG;
    *length = n;
    return 0;
}
static int file_save(void *ctx, const char *text, size_t length)
{
    (void)ctx;
    static const char temp[] = SS_PATH ".new";
    FILE *f = fopen(temp, "wb");
    if (!f) return -(errno ? errno : EIO);
    int e = fwrite(text, 1, length, f) == length ? 0 : -EIO;
    if (!e && fflush(f)) e = -EIO;
    if (!e && fsync(fileno(f))) e = -EIO;
    if (fclose(f) && !e) e = -EIO;
    if (!e && rename(temp, SS_PATH)) e = -(errno ? errno : EIO);
    if (e) remove(temp);
    return e;
}
static int64_t clock_minutes(void *ctx)
{
    struct desk_schedsvc *s = ctx;
    return ((int64_t)time(NULL) + s->local_offset) / 60;
}

/* ---- the service step over the queue ---- */
static int enqueue(void *ctx, const char *program, const char *argument)
{
    struct desk_schedsvc *s = ctx;
    pthread_mutex_lock(&s->lock);
    int full = s->count == DESK_SCHED_QUEUE;
    if (!full) {
        struct queued *q = &s->queue[(s->head + s->count++) % DESK_SCHED_QUEUE];
        snprintf(q->program, sizeof(q->program), "%s", program);
        snprintf(q->argument, sizeof(q->argument), "%s", argument);
    }
    pthread_mutex_unlock(&s->lock);
    return full ? -EAGAIN : 1;    /* the pid is not known yet: any positive value means accepted */
}
static int step_load(void *ctx, char *t, size_t cap, size_t *n) { struct desk_schedsvc *s = ctx; return s->io.load(s->io.ctx, t, cap, n); }
static int step_save(void *ctx, const char *t, size_t n) { struct desk_schedsvc *s = ctx; return s->io.save(s->io.ctx, t, n); }
static int64_t step_now(void *ctx) { struct desk_schedsvc *s = ctx; return s->io.now_minutes(s->io.ctx); }
int desk_schedsvc_step(struct desk_schedsvc *s)
{
    struct ss_host host = { s, step_now, step_load, step_save, enqueue };
    return ss_poll(&s->service, &host);
}
unsigned desk_schedsvc_drain(struct desk_schedsvc *s)
{
    unsigned launched = 0;
    for (;;) {
        struct queued q;
        pthread_mutex_lock(&s->lock);
        if (!s->count) { pthread_mutex_unlock(&s->lock); break; }
        q = s->queue[s->head]; s->head = (s->head + 1) % DESK_SCHED_QUEUE; s->count--;
        pthread_mutex_unlock(&s->lock);
        int pid = s->io.launch(s->io.ctx, q.program, q.argument);
        if (pid > 0) launched++;
        else if (s->io.failed) s->io.failed(s->io.ctx, q.program, pid ? -pid : EIO);
    }
    return launched;
}
struct desk_schedsvc *desk_schedsvc_create(const struct desk_schedsvc_io *io)
{
    struct desk_schedsvc *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->io = *io;
    pthread_mutex_init(&s->lock, NULL); pthread_cond_init(&s->changed, NULL);
    return s;
}
void desk_schedsvc_destroy(struct desk_schedsvc *s)
{
    if (!s) return;
    pthread_cond_destroy(&s->changed); pthread_mutex_destroy(&s->lock);
    free(s);
}

/* ---- the desktop binding ---- */
static int desktop_launch(void *ctx, const char *program, const char *argument)
{
    struct desk_schedsvc *s = ctx;
    return desk_task_launch(s->desktop, program, argument);
}
static void desktop_failed(void *ctx, const char *program, int error)
{
    struct desk_schedsvc *s = ctx;
    char title[80];
    snprintf(title, sizeof(title), "Scheduled task did not start: %.40s", program);
    desk_shell_notify_error(s->desktop, title, error);
}
static void *worker(void *arg)
{
    struct desk_schedsvc *s = arg;
    pthread_mutex_lock(&s->lock);
    while (!s->stopping) {
        pthread_mutex_unlock(&s->lock);
        int queued = desk_schedsvc_step(s);
        if (queued > 0 && s->desktop) desk_wake(s->desktop);
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += DESK_SCHED_POLL_MS / 1000;
        pthread_mutex_lock(&s->lock);
        while (!s->stopping && pthread_cond_timedwait(&s->changed, &s->lock, &until) != ETIMEDOUT) {}
    }
    pthread_mutex_unlock(&s->lock);
    return NULL;
}
int desk_schedsvc_start(struct desktop *d)
{
    if (!d || d->schedsvc) return 0;
    struct desk_schedsvc_io io = { NULL, file_load, file_save, clock_minutes, desktop_launch, desktop_failed };
    struct desk_schedsvc *s = desk_schedsvc_create(&io);
    if (!s) return ENOMEM;
    s->io.ctx = s; s->desktop = d; s->local_offset = d->local_offset;
    int e = pthread_create(&s->thread, NULL, worker, s);
    if (e) { desk_schedsvc_destroy(s); return e; }
    s->thread_started = 1;
    d->schedsvc = s;
    return 0;
}
void desk_schedsvc_tick(struct desktop *d)
{
    struct desk_schedsvc *s = d ? d->schedsvc : NULL;
    if (!s) return;
    s->local_offset = d->local_offset;
    if (d->power.state) return;        /* no new programs while shutting down */
    desk_schedsvc_drain(s);
}
void desk_schedsvc_stop(struct desktop *d)
{
    struct desk_schedsvc *s = d ? d->schedsvc : NULL;
    if (!s) return;
    pthread_mutex_lock(&s->lock); s->stopping = 1; pthread_cond_signal(&s->changed); pthread_mutex_unlock(&s->lock);
    if (s->thread_started) pthread_join(s->thread, NULL);
    d->schedsvc = NULL;
    desk_schedsvc_destroy(s);
}
