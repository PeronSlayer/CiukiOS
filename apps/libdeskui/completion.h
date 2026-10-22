/* SPDX-License-Identifier: MIT
 * Private pollable notification transport. Results live in owner mailboxes;
 * tokens only wake the consumer. Join producers before destroying this object. */
#ifndef CIUKI_UI_COMPLETION_H
#define CIUKI_UI_COMPLETION_H
#include <ciuki/channel.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
struct du_completion {
    pthread_mutex_t lock;
    int fd[2];
};
static inline int du_completion_init(struct du_completion *c)
{
    c->fd[0]=c->fd[1]=-1;
    int error=pthread_mutex_init(&c->lock,NULL);
    if (error) { errno=error; return -1; }
    if (!ciuki_channel_pair(c->fd)) {
        if (!fcntl(c->fd[0],F_SETFD,FD_CLOEXEC) && !fcntl(c->fd[1],F_SETFD,FD_CLOEXEC)) return 0;
        int saved=errno; close(c->fd[0]); close(c->fd[1]); errno=saved;
    }
    pthread_mutex_destroy(&c->lock); return -1;
}
static inline void du_completion_notify(void *ctx)
{
    struct du_completion *c=ctx;
    struct ciuki_message token={0}; token.length=1;
    pthread_mutex_lock(&c->lock);
    /* Bounded send: EAGAIN means a previous token already wakes the UI.
     * EINTR is a failed publication too; HUP supplies a reliable wake. */
    if (c->fd[1]>=0 && ciuki_channel_send(c->fd[1],&token,DONTWAIT) && errno!=EAGAIN) {
        close(c->fd[1]); c->fd[1]=-1;
    }
    pthread_mutex_unlock(&c->lock);
}
static inline int du_completion_take(struct du_completion *c)
{
    struct ciuki_message token;
    int r=ciuki_channel_recv(c->fd[0],&token,DONTWAIT);
    if (r>0 || (r<0 && (errno==EAGAIN || errno==EINTR))) return 0;
    if (!r) errno=EIO;
    return -1;
}
static inline void du_completion_destroy(struct du_completion *c)
{
    if (c->fd[0]>=0) close(c->fd[0]);
    if (c->fd[1]>=0) close(c->fd[1]);
    pthread_mutex_destroy(&c->lock);
}
static inline int du_wait_ms(uint64_t now,uint64_t deadline)
{
    if (deadline==UINT64_MAX) return -1;
    if (deadline<=now) return 0;
    uint64_t ns=deadline-now, ms=ns/1000000u+(ns%1000000u!=0);
    return ms>INT_MAX ? INT_MAX : (int)ms;
}
#endif
