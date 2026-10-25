/* SPDX-License-Identifier: MIT */
#include "schedsvc.h"
#include "worker_role.h"
#include "desktop.h"
#include "../systools/systools.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

struct queued { char program[48], argument[161]; unsigned token; };
struct desk_schedsvc {
    struct desk_schedsvc_io io;
    struct ss_service service;
    pthread_mutex_t lock; pthread_cond_t changed; pthread_t thread;
    struct queued queue[DESK_SCHED_QUEUE]; unsigned head, count;
    int thread_started, stopping;
    struct desktop *desktop;
    struct { unsigned token; int pid, result, done, stop; } active[ST_MAX];
    char manual[DESK_SCHED_QUEUE][32]; unsigned manual_head, manual_count;
    unsigned next_token, launching_token, idle_minutes; int ac;

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
    return ((int64_t)time(NULL) + __atomic_load_n(&s->local_offset, __ATOMIC_RELAXED)) / 60;
}

/* ---- the service step over the queue ---- */
static int enqueue(void *ctx, const char *program, const char *argument)
{
    struct desk_schedsvc *s = ctx;
    pthread_mutex_lock(&s->lock);
    unsigned slot = ST_MAX;
    for (unsigned i = 0; i < ST_MAX; i++) if (!s->active[i].token) { slot = i; break; }
    int full = s->count == DESK_SCHED_QUEUE || slot == ST_MAX;
    unsigned token = 0;
    if (!full) {
        struct queued *q = &s->queue[(s->head + s->count++) % DESK_SCHED_QUEUE];
        token = ++s->next_token; if (!token || token > 0x7fffffffu) token = s->next_token = 1;
        q->token = token;
        memset(&s->active[slot], 0, sizeof(s->active[slot])); s->active[slot].token = token;
        snprintf(q->program, sizeof(q->program), "%s", program);
        snprintf(q->argument, sizeof(q->argument), "%s", argument);
    }
    pthread_mutex_unlock(&s->lock);
    return full ? -EAGAIN : (int)token;    /* the pid is not known yet: any positive value means accepted */
}
static int step_load(void *ctx, char *t, size_t cap, size_t *n) { struct desk_schedsvc *s = ctx; return s->io.load(s->io.ctx, t, cap, n); }
static int step_save(void *ctx, const char *t, size_t n) { struct desk_schedsvc *s = ctx; return s->io.save(s->io.ctx, t, n); }
static int64_t step_now(void *ctx) { struct desk_schedsvc *s = ctx; return s->io.now_minutes(s->io.ctx); }
void desk_schedsvc_bind(struct desk_schedsvc *s, struct desktop *d) { s->desktop = d; }
void desk_schedsvc_conditions(struct desk_schedsvc *s, unsigned idle_minutes, int ac)
{
    pthread_mutex_lock(&s->lock); s->idle_minutes = idle_minutes; s->ac = ac; pthread_mutex_unlock(&s->lock);
}
static void step_conditions(void *ctx, struct st_conditions *c)
{
    struct desk_schedsvc *s = ctx; pthread_mutex_lock(&s->lock);
    c->idle_minutes = s->idle_minutes; c->ac = s->ac; pthread_mutex_unlock(&s->lock);
}
static int step_result(void *ctx, int token, int *result)
{
    struct desk_schedsvc *s = ctx; int done = 0;
    pthread_mutex_lock(&s->lock);
    for (unsigned i = 0; i < ST_MAX; i++) if (s->active[i].token == (unsigned)token && s->active[i].done) {
        *result = s->active[i].result; s->active[i].token = 0; done = 1; break;
    }
    pthread_mutex_unlock(&s->lock); return done;
}
static void step_stop(void *ctx, int token)
{
    struct desk_schedsvc *s = ctx; pthread_mutex_lock(&s->lock);
    for (unsigned i = 0; i < ST_MAX; i++) if (s->active[i].token == (unsigned)token) s->active[i].stop = 1;
    pthread_mutex_unlock(&s->lock);
}
void desk_schedsvc_completed(struct desktop *d, unsigned token, int result)
{
    struct desk_schedsvc *s = d ? d->schedsvc : NULL; if (!s) return;
    pthread_mutex_lock(&s->lock);
    for (unsigned i = 0; i < ST_MAX; i++) if (s->active[i].token == token) {
        if (result > 0) s->active[i].pid = result;
        else { s->active[i].result = result ? result : -EIO; s->active[i].done = 1; }
        break;
    }
    pthread_cond_signal(&s->changed); pthread_mutex_unlock(&s->lock);
}
void desk_schedsvc_exited(struct desktop *d, uint32_t pid, int status)
{
    struct desk_schedsvc *s = d ? d->schedsvc : NULL; if (!s || !pid) return;
    pthread_mutex_lock(&s->lock);
    for (unsigned i = 0; i < ST_MAX; i++) if (s->active[i].token && s->active[i].pid == (int)pid) {
        s->active[i].result = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -EIO;
        s->active[i].done = 1;
    }
    pthread_cond_signal(&s->changed); pthread_mutex_unlock(&s->lock);
}
int desk_schedsvc_run(struct desktop *d, const char *name)
{
    struct desk_schedsvc *s = d ? d->schedsvc : NULL;
    if (!s || !name || !*name || strlen(name) >= 32) return -EINVAL;
    pthread_mutex_lock(&s->lock);
    int full = s->manual_count == DESK_SCHED_QUEUE;
    if (!full) snprintf(s->manual[(s->manual_head + s->manual_count++) % DESK_SCHED_QUEUE], 32, "%s", name);
    pthread_cond_signal(&s->changed); pthread_mutex_unlock(&s->lock);
    return full ? -EAGAIN : 1;
}
static int step_manual(void *ctx, char name[32])
{
    struct desk_schedsvc *s = ctx; pthread_mutex_lock(&s->lock);
    int have = s->manual_count != 0;
    if (have) { memcpy(name, s->manual[s->manual_head], 32); s->manual_head = (s->manual_head + 1) % DESK_SCHED_QUEUE; s->manual_count--; }
    pthread_mutex_unlock(&s->lock); return have;
}
int desk_schedsvc_step(struct desk_schedsvc *s)
{
    struct ss_host host = { s, step_now, step_load, step_save, enqueue, step_conditions, step_result, step_stop, step_manual };
    return ss_poll(&s->service, &host);
}
unsigned desk_schedsvc_drain(struct desk_schedsvc *s)
{
    unsigned launched = 0;
    for (unsigned i = 0; i < ST_MAX; i++) {
        pthread_mutex_lock(&s->lock);
        int pid = s->active[i].stop && !s->active[i].done ? s->active[i].pid : 0;
        unsigned token = s->active[i].token;
        pthread_mutex_unlock(&s->lock);
        if (pid > 0 && s->desktop && s->desktop->terminate) {
            int e = s->desktop->terminate((uint32_t)pid, SIGTERM), error = errno;
            pthread_mutex_lock(&s->lock);
            if (s->active[i].token == token && s->active[i].pid == pid) {
                if (!e) s->active[i].stop = 0;
                else if (error == ESRCH) { s->active[i].result = -ESRCH; s->active[i].done = 1; }
            }
            pthread_mutex_unlock(&s->lock);
        }
    }
    for (;;) {
        struct queued q;
        pthread_mutex_lock(&s->lock);
        if (!s->count) { pthread_mutex_unlock(&s->lock); break; }
        q = s->queue[s->head]; s->head = (s->head + 1) % DESK_SCHED_QUEUE; s->count--;
        pthread_mutex_unlock(&s->lock);
        int cancelled = 0;
        pthread_mutex_lock(&s->lock);
        for (unsigned i = 0; i < ST_MAX; i++) if (s->active[i].token == q.token && s->active[i].stop) {
            s->active[i].done = 1; s->active[i].result = -EINTR; cancelled = 1;
        }
        pthread_mutex_unlock(&s->lock);
        if (cancelled) continue;
        s->launching_token = q.token;
        int pid = s->io.launch(s->io.ctx, q.program, q.argument);
        pthread_mutex_lock(&s->lock);
        for (unsigned i = 0; i < ST_MAX; i++) if (s->active[i].token == q.token) {
            if (pid > 0) s->active[i].pid = pid;
            else if (pid < 0) { s->active[i].result = pid; s->active[i].done = 1; }
        }
        pthread_mutex_unlock(&s->lock);
        if (pid >= 0) launched++;
        else if (s->io.failed) s->io.failed(s->io.ctx, q.program, pid ? -pid : EIO);
    }
    return launched;
}
struct desk_schedsvc *desk_schedsvc_create(const struct desk_schedsvc_io *io)
{
    struct desk_schedsvc *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->io = *io; s->ac = 2;
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
    desk_launch_origin(s->desktop, DESK_LAUNCH_SCHEDULE, NULL);
    s->desktop->launch_request.serial = s->launching_token;
    int result = desk_task_launch(s->desktop, program, argument);
    return !result && !s->desktop->launch_deferred ? -EIO : result;
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
    desk_worker_role("scheduler");
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
        if (!s->stopping) (void)pthread_cond_timedwait(&s->changed, &s->lock, &until);
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
    s->io.ctx = s; desk_schedsvc_bind(s, d); s->local_offset = d->local_offset;
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
    __atomic_store_n(&s->local_offset, d->local_offset, __ATOMIC_RELAXED);
    unsigned idle = d->now_ns >= d->last_input_ns ? (unsigned)((d->now_ns - d->last_input_ns) / 60000000000ull) : 0;
    desk_schedsvc_conditions(s, idle, d->sh.power_summary.valid ? d->sh.power_summary.ac : 2);
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
