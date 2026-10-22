/* SPDX-License-Identifier: MIT
 * f2-88b: the preview worker never makes the UI thread wait. The UI loop answers the desktop's ping at the
 * start of each turn; it may call only cp_decoder_begin/poll, which must return at once while a job runs
 * and while the worker holds the hand-over lock. */
#include "decoder.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static sem_t release_job, started;
static int work(const struct cp_preview_req *req, struct cp_preview_done *out)
{
    (void)req; sem_post(&started); sem_wait(&release_job);
    out->decode_ms = 7; out->rendered = 0; return 0;
}
static unsigned long long now_ns(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec; }
int main(void)
{
    struct cp_preview_req req; struct cp_preview_done done; uint32_t pv[192 * 144];
    memset(&req, 0, sizeof(req)); snprintf(req.path, sizeof(req.path), "/x.png");
    sem_init(&release_job, 0, 0); sem_init(&started, 0, 0);
    cp_decoder_set_work(work);
    unsigned long long t = now_ns();
    assert(!cp_decoder_begin(&req));
    assert(now_ns() - t < CP_DECODER_UI_SLICE_NS);          /* begin: one slice */
    sem_wait(&started);                                     /* the job is running and blocked */
    assert(cp_decoder_begin(&req) == -EBUSY);               /* one job at a time, answered at once */
    for (unsigned i = 0; i < 1000; i++) {                   /* 1000 UI turns while the worker is busy */
        memset(&done, 0, sizeof(done)); done.preview = pv;
        t = now_ns(); assert(!cp_decoder_poll(&done));
        assert(now_ns() - t < CP_DECODER_UI_SLICE_NS);      /* poll: bounded, never waits for the job */
    }
    sem_post(&release_job);
    int got = 0;
    for (unsigned i = 0; i < 2000 && !got; i++) {           /* the worker publishes under the lock: poll may miss, never wait */
        memset(&done, 0, sizeof(done)); done.preview = pv;
        t = now_ns(); got = cp_decoder_poll(&done);
        assert(now_ns() - t < CP_DECODER_UI_SLICE_NS);
        if (!got) nanosleep(&(struct timespec){0, 1000000}, NULL);
    }
    assert(got == 1 && done.decode_ms == 7 && !strcmp(done.req.path, "/x.png"));
    assert(!cp_decoder_poll(&done));                        /* one result, once */
    /* The worker (or anything else) holds the lock: begin and poll answer at once instead of waiting. */
    cp_decoder_test_lock(1);
    t = now_ns(); assert(cp_decoder_begin(&req) == -EBUSY && !cp_decoder_poll(&done));
    assert(now_ns() - t < CP_DECODER_UI_SLICE_NS);
    cp_decoder_test_lock(0);
    puts("PASS control decoder: begin/poll bounded while the worker runs, one job at a time");
    return 0;
}
