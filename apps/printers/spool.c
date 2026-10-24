/* SPDX-License-Identifier: MIT
 * Single desktop-owned background print service.
 *
 * Event driven, as the Windows spooler: a producer publishes a job and then
 * schedules it (AddJob/ScheduleJob), and the spooler starts printing it; it
 * does not rescan its folder while idle. Here producers ring a doorbell
 * (cp_service_notify, print.h) and the worker sleeps on a private wait word
 * until a ring, a stop request, the retry interval while a job is offline,
 * or the CP_IDLE_RESCAN_MS safety net (one folder read a minute) for a
 * producer path that published without ringing. The first G31 loop asked nanosleep for tv_nsec=1000000000 while
 * idle, which POSIX and the kernel refuse with EINVAL without sleeping
 * (src/kernel/proc/clock.c file_sleep_deadline): the "1 s" idle pass ran
 * back to back, 60-77 % of the CPU and C: held for 50-420 ms at a time in
 * QEMU (docs/validation/doom-launch-audio-2026-10-24.md). */
#include "../libprint/print.h"
#include <ciuki/channel.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
static volatile sig_atomic_t running=1;
static uint32_t doorbell;               /* bumped by every ring and stop request */
static void ring(void){__atomic_add_fetch(&doorbell,1,__ATOMIC_SEQ_CST);}
static void stop(int sig){(void)sig;running=0;ring();}
static void wake(int sig){(void)sig;ring();}
static int handle(int sig,void (*fn)(int))
{
    struct sigaction a;memset(&a,0,sizeof(a));a.sa_handler=fn;sigemptyset(&a.sa_mask);
    return sigaction(sig,&a,NULL);      /* no SA_RESTART: a ring ends the wait */
}
/* Sleeps until the doorbell moves past `seen` or retry_ms has passed. */
static void wait_ring(uint32_t seen,unsigned retry_ms)
{
    struct ciuki_timespec until,*deadline=NULL;
    if(retry_ms){
        struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
        int64_t ns=(int64_t)now.tv_nsec+(int64_t)(retry_ms%1000u)*1000000;
        until=(struct ciuki_timespec){.tv_sec=(int64_t)now.tv_sec+retry_ms/1000u+ns/1000000000,.tv_nsec=(int32_t)(ns%1000000000)};
        deadline=&until;
    }
    /* EAGAIN: rung after `seen` was read (no lost wake-up); EINTR: rung while
     * asleep; ETIMEDOUT: retry or safety rescan. Every outcome rescans once. */
    (void)ciuki_wait_word(&doorbell,seen,deadline,CLOCK_MONOTONIC);
}
int main(void)
{
    /* Handlers before the lock: producers ring as soon as the owner exists. */
    if(handle(SIGTERM,stop) || handle(SIGINT,stop) || handle(SIGCHLD,wake))return 1;
    if(cp_init(NULL) || cp_service_lock(NULL))return 1;
    while(running){
        uint32_t seen=__atomic_load_n(&doorbell,__ATOMIC_SEQ_CST);
        unsigned offline=0;int r=cp_service_next(NULL,&offline);
        if(r>0){
            /* Printing: the next bounded 1 KiB step after 1 ms, as before. */
            struct timespec pace={.tv_sec=0,.tv_nsec=1000000};nanosleep(&pace,NULL);
            continue;
        }
        /* Idle (no runnable job) sleeps until a ring or the safety rescan;
         * an offline job or an unreadable queue is retried after CP_RETRY_MS. */
        wait_ring(seen,offline || r<0 ? CP_RETRY_MS : CP_IDLE_RESCAN_MS);
    }
    cp_service_unlock(NULL);return 0;
}
