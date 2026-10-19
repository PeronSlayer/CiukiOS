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
    struct music_snapshot snapshot;
    struct music_player player;
};
static int cancel(void *ctx)
{
    struct music_runtime *r=ctx; return atomic_load(&r->closing);
}
static void publish(void *ctx, const struct music_snapshot *view)
{ struct music_runtime *r=ctx; pthread_mutex_lock(&r->lock); r->snapshot=*view; pthread_mutex_unlock(&r->lock); }
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
        (void)music_player_pump(&r->player);
        pthread_mutex_lock(&r->lock);
        r->snapshot=r->player.view;
        if (!r->closing && !r->count) {
            if (r->snapshot.state==MUSIC_PLAYING || r->snapshot.state==MUSIC_LOADING) {
                struct timespec until; clock_gettime(CLOCK_REALTIME,&until); until.tv_nsec+=2000000;
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
