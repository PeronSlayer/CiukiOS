/* SPDX-License-Identifier: MIT
 * Production du_run/du_window_wait with model time and real pthread jobs.
 * Only the window turn and OS boundary are fixtures. No guest/image build. */
#include "replay_base.h"
#include "../../../apps/libdeskui/deskui.h"
#include "../../../apps/libdeskui/completion.h"
#include "../../../apps/libdeskui/job.h"
#include <poll.h>
#include <assert.h>
#include <stdio.h>
static uint64_t now_ns,deadline;
static unsigned calls,turns;
static int model,input,timer_events,peer;
static struct du_ui ui;
static struct du_window window;
static struct du_completion completion;
static struct du_job job;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int waiting,result,adopted;
uint64_t du_now(void)
{
    if(model)return now_ns;
    struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000000ull+(unsigned)t.tv_nsec;
}
uint64_t du_next_deadline(const struct du_ui *u) { (void)u;return deadline; }
int du_window_turn(struct du_window *w,uint64_t now)
{
    (void)now;turns++;
    if(w->closing || input)return 0;
    return 1;
}
static int event_poll(struct pollfd *fds,nfds_t n,int timeout)
{
    calls++;
    if(model==2){
        assert(n==2 && (fds[0].events&POLLOUT) && timeout==-1);
        fds[0].revents=POLLOUT;return 1;
    }
    if(model){
        assert(n==2 && fds[0].events==POLLIN && fds[1].events==POLLIN);
        if(deadline!=UINT64_MAX && now_ns<deadline){
            assert(timeout==5000);now_ns=deadline;deadline=UINT64_MAX;timer_events++;return 0;
        }
        assert(timeout==-1);now_ns=10000000000ull;input=1;fds[0].revents=POLLIN;return 1;
    }
    pthread_mutex_lock(&gate);waiting=1;pthread_cond_signal(&changed);pthread_mutex_unlock(&gate);
    return poll(fds,n,timeout);
}
#define poll event_poll
#include "native_run.inc"
#undef poll
static void work(void *ctx)
{
    (void)ctx;pthread_mutex_lock(&gate);
    while(!waiting)pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate);result=42;
}
static void idle(void *ctx)
{
    (void)ctx;
    if(du_job_ready(&job)){du_job_finish(&job);assert(result==42);adopted++;window.closing=1;}
}
void event_channel_fail(int);
static void setup(void)
{
    ui=(struct du_ui){0};window=(struct du_window){.channel=-1,.configured=1,.ui=&ui,.completion=&completion};
    assert(!du_completion_init(&completion));
    int pair[2];assert(!ciuki_channel_pair(pair));window.channel=pair[0];peer=pair[1];
    ui.wake=du_completion_notify;ui.wake_ctx=&completion;
    calls=turns=0;deadline=UINT64_MAX;input=0;
}
static void cleanup(void) { close(peer);close(window.channel);du_completion_destroy(&completion); }
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);assert(sizeof(struct ciuki_message)==288);
    model=1;now_ns=0;setup();assert(!du_run(&window,NULL,NULL));
    assert(now_ns==10000000000ull && calls==1 && turns==2);cleanup();
    now_ns=0;setup();deadline=5000000000ull;assert(!du_run(&window,NULL,NULL));
    assert(calls==2 && timer_events==1 && turns==3);cleanup();
    model=2;setup();window.closing=1;window.count=16;
    assert(!du_window_wait(&window,UINT64_MAX) && calls==1);cleanup();
    model=0;setup();waiting=result=adopted=0;
    assert(!du_job_start_ui(&job,work,NULL,&ui));int run_result=du_run(&window,idle,NULL);
    if(run_result)fprintf(stderr,"run=%d errno=%d calls=%u turns=%u adopted=%d fds=%d/%d\n",run_result,errno,calls,turns,adopted,completion.fd[0],completion.fd[1]);
    assert(!run_result);
    assert(adopted==1 && calls<=2);cleanup();
    /* Publication before registration remains readable. Coalescing/full send
     * leaves the previous notification; unexpected failure wakes via HUP. */
    setup();du_completion_notify(&completion);assert(!du_window_wait(&window,UINT64_MAX));
    du_completion_notify(&completion);event_channel_fail(EAGAIN);du_completion_notify(&completion);
    assert(!du_window_wait(&window,UINT64_MAX));
    event_channel_fail(ENOMEM);du_completion_notify(&completion);
    assert(du_window_wait(&window,UINT64_MAX)==-1 && errno==EIO);cleanup();
    assert(du_wait_ms(0,UINT64_MAX)==-1 && du_wait_ms(100,100)==0 && du_wait_ms(0,1000001)==2);
    assert(du_wait_ms(0,UINT64_MAX-1)==INT_MAX);
    puts("PASS native event waits: 10 s zero idle wakeups, exact timer, input/completion one turn, publication races, full/error/HUP");
    return 0;
}
