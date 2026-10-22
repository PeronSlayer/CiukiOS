/* SPDX-License-Identifier: MIT */
#include "player.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
struct music_runtime {
    pthread_t thread; pthread_mutex_t lock; pthread_cond_t changed;
    struct music_command commands[MUSIC_COMMANDS]; unsigned head, count;
    atomic_int closing; int done;
    uint64_t last_publish;
    unsigned ui_cycles; /* f4-37: below 1024, carried to the next report (UI thread) */
    struct music_snapshot snapshot;
    struct music_player player;
};
static int cancel(void *ctx)
{
    struct music_runtime *r=ctx; return atomic_load(&r->closing);
}
static int displayed_changed(const struct music_snapshot *a, const struct music_snapshot *b)
{
    if (a->state!=b->state || a->error!=b->error || a->current!=b->current || a->rate!=b->rate ||
        a->revision!=b->revision || a->playlist.count!=b->playlist.count || a->duration!=b->duration ||
        (!b->error && a->buffering!=b->buffering) || (b->error && strcmp(a->error_path,b->error_path))) return 1;
    if (b->rate && a->position/b->rate!=b->position/b->rate) return 1;
    return b->duration && a->position*1000/b->duration!=b->position*1000/b->duration;
}
static void publish(void *ctx, const struct music_snapshot *view)
{
    struct music_runtime *r=ctx;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    uint64_t now=(uint64_t)t.tv_sec*1000000000+t.tv_nsec;
    pthread_mutex_lock(&r->lock);
    /* Coalesce steady progress to two frames/s. State/buffering changes can
     * publish at 10 Hz; unchanged diagnostic counters never wake the view. */
    uint64_t interval=view->state==MUSIC_PLAYING && r->snapshot.state==MUSIC_PLAYING &&
        !view->buffering && !r->snapshot.buffering ? 500000000 : 100000000;
    if ((!r->snapshot.publication || now-r->last_publish>=interval) && displayed_changed(&r->snapshot,view)) {
        unsigned publication=r->snapshot.publication+1;
        r->snapshot=*view; r->snapshot.publication=publication ? publication : 1;
        r->last_publish=now; atomic_fetch_add(&r->player.ui_publishes,1);
    }
    pthread_mutex_unlock(&r->lock);
}
static void *worker(void *ctx)
{
    struct music_runtime *r=ctx;
    for (;;) {
        pthread_mutex_lock(&r->lock);
        int closing=r->closing, have=r->count!=0; struct music_command c={0};
        if (have) { c=r->commands[r->head]; r->head=(r->head+1)%MUSIC_COMMANDS; r->count--; }
        pthread_mutex_unlock(&r->lock);
        if (closing) break;
        if (have) (void)music_player_command(&r->player,&c);
        uint64_t begin=music_cycles();
        (void)music_player_pump(&r->player);
        publish(r,&r->player.view);
        r->player.pump_cycles+=music_cycles()-begin;
        pthread_mutex_lock(&r->lock);
        if (!r->closing && !r->count) {
            int playing=r->player.view.state==MUSIC_PLAYING || r->player.view.state==MUSIC_LOADING;
            if (playing || displayed_changed(&r->snapshot,&r->player.view)) {
                struct timespec until; clock_gettime(CLOCK_REALTIME,&until); until.tv_nsec+=playing ? 2000000 : 100000000;
                if (until.tv_nsec>=1000000000) { until.tv_sec++; until.tv_nsec-=1000000000; }
                pthread_cond_timedwait(&r->changed,&r->lock,&until);
            } else pthread_cond_wait(&r->changed,&r->lock);
        }
        pthread_mutex_unlock(&r->lock);
    }
    music_player_destroy(&r->player);
    pthread_mutex_lock(&r->lock); r->snapshot=r->player.view; r->done=1;
    pthread_mutex_unlock(&r->lock); return NULL;
}
struct music_runtime *music_runtime_start(const struct music_output *output)
{
    struct music_runtime *r=calloc(1,sizeof(*r)); if (!r) return NULL;
    atomic_init(&r->closing,0);
    int err=pthread_mutex_init(&r->lock,NULL); if (err) { free(r); errno=err; return NULL; }
    err=pthread_cond_init(&r->changed,NULL);
    if (err) { pthread_mutex_destroy(&r->lock); free(r); errno=err; return NULL; }
    music_player_init(&r->player,output,cancel,r);
    r->player.changed=publish;
    err=r->player.sync_error;
    if (!err) err=pthread_create(&r->thread,NULL,worker,r);
    if (err) { music_player_destroy(&r->player); pthread_cond_destroy(&r->changed); pthread_mutex_destroy(&r->lock); free(r); errno=err; return NULL; }
    return r;
}
int music_runtime_send(struct music_runtime *r, const struct music_command *c)
{
    if (c->path[MUSIC_PATH-1]) return -ENAMETOOLONG;
    pthread_mutex_lock(&r->lock);
    int err=r->closing ? -ECANCELED : r->count==MUSIC_COMMANDS ? -EBUSY : 0;
    if (!err) { r->commands[(r->head+r->count++)%MUSIC_COMMANDS]=*c; pthread_cond_signal(&r->changed); }
    pthread_mutex_unlock(&r->lock); return err;
}
void music_runtime_snapshot(struct music_runtime *r, struct music_snapshot *s)
{ pthread_mutex_lock(&r->lock); *s=r->snapshot; pthread_mutex_unlock(&r->lock); }
void music_runtime_repaint(struct music_runtime *r)
{ atomic_fetch_add(&r->player.ui_repaints,1); }
void music_runtime_ui_cycles(struct music_runtime *r, uint64_t cycles)
{
    cycles+=r->ui_cycles; r->ui_cycles=(unsigned)(cycles&1023u);
    atomic_fetch_add(&r->player.ui_kcycles,(unsigned)(cycles>>10));
}
void music_runtime_close(struct music_runtime *r)
{ pthread_mutex_lock(&r->lock); r->closing=1; r->count=0; pthread_cond_signal(&r->changed); pthread_mutex_unlock(&r->lock); }
int music_runtime_done(struct music_runtime *r)
{ pthread_mutex_lock(&r->lock); int done=r->done; pthread_mutex_unlock(&r->lock); return done; }
void music_runtime_destroy(struct music_runtime *r)
{
    if (!r) return;
    music_runtime_close(r); pthread_join(r->thread,NULL);
    pthread_cond_destroy(&r->changed); pthread_mutex_destroy(&r->lock); free(r);
}
