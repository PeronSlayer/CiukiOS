/* Production print spooler loop (apps/printers/spool.c) over the production
 * queue library, on the host: an idle worker must sleep, not rescan.
 * QEMU evidence (image 9fe0a02e, docs/validation/doom-launch-audio-2026-10-24.md):
 * the G31 loop's "1 s" idle sleep was refused with EINVAL, so printspool read
 * its folder back to back, 60-77 % of the CPU in every two-second window.
 * The runner builds spool.c with -Dmain=printspool_main, CP_ROOT in a scratch
 * folder, CP_RETRY_MS=300, CP_IDLE_RESCAN_MS=1000 (the 60 s safety rescan
 * scaled 60:1) and opendir/closedir/kill wrapped; ciuki_wait_word is
 * modelled with a private Linux futex (same compare-and-block contract,
 * EINTR on a caught signal).
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#undef main                             /* -Dmain=printspool_main is for spool.c */
#include "../../apps/libprint/print.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <ciuki/channel.h>         /* the runner's host shim: struct ciuki_timespec, ciuki_wait_word */

int printspool_main(void);
DIR *__real_opendir(const char *);
int __real_closedir(DIR *);
int __real_kill(pid_t, int);
static atomic_int muted;                /* a producer path that publishes without ringing */
int __wrap_kill(pid_t pid, int sig)
{
    if (sig == SIGCHLD && atomic_load(&muted)) return 0;
    return __real_kill(pid, sig);
}
static atomic_uint scans;               /* folder reads of the spool root */
static atomic_int inject;               /* publish a job right after the next scan */
static char target[512];
static atomic_int race_id;

DIR *__wrap_opendir(const char *p)
{
    if (!strcmp(p, CP_ROOT)) atomic_fetch_add(&scans, 1);
    return __real_opendir(p);
}
/* The lost wake-up window: the scan has finished and found nothing; a
 * producer publishes and rings before the worker goes to sleep. */
int __wrap_closedir(DIR *d)
{
    int r = __real_closedir(d);
    if (atomic_exchange(&inject, 0)) {
        int id = cp_submit_raw(NULL, CP_PRINTERS, "race", "R", 1, NULL, target);
        assert(id > 0); atomic_store(&race_id, id);
    }
    return r;
}
int ciuki_wait_word(uint32_t *w, uint32_t expected, const struct ciuki_timespec *deadline, int clock)
{
    (void)clock;
    struct timespec rel, *wait = NULL;
    if (deadline) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t ns = (deadline->tv_sec - now.tv_sec) * 1000000000LL + (deadline->tv_nsec - now.tv_nsec);
        if (ns <= 0) { errno = ETIMEDOUT; return -1; }
        rel = (struct timespec){ .tv_sec = ns / 1000000000LL, .tv_nsec = ns % 1000000000LL };
        wait = &rel;
    }
    return syscall(SYS_futex, w, FUTEX_WAIT_PRIVATE, expected, wait, NULL, 0) ? -1 : 0;
}
static uint64_t now_ms(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u; }
static void pause_ms(unsigned ms)
{ struct timespec t = { ms / 1000u, (long)(ms % 1000u) * 1000000L }; while (nanosleep(&t, &t) && errno == EINTR) {} }
static int exists(const char *p) { struct stat st; return !stat(p, &st); }
static unsigned slot_gone(unsigned id, unsigned limit_ms)
{
    char p[600]; snprintf(p, sizeof(p), "%s/j%03u", CP_ROOT, id);
    uint64_t start = now_ms();
    while (exists(p)) { if (now_ms() - start > limit_ms) return UINT32_MAX; pause_ms(2); }
    return (unsigned)(now_ms() - start);
}
static void *worker(void *arg) { (void)arg; return (void *)(intptr_t)printspool_main(); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char owner[600], missing[600], late[600];
    snprintf(owner, sizeof(owner), "%s/%s", CP_ROOT, CP_WORKER_OWNER);
    snprintf(target, sizeof(target), "%s.out", CP_ROOT);
    pthread_t t;
    assert(!pthread_create(&t, NULL, worker, NULL));
    /* Only the worker thread takes the doorbell and stop signals, as the
     * single-threaded printspool process does. */
    sigset_t block; sigemptyset(&block); sigaddset(&block, SIGCHLD); sigaddset(&block, SIGTERM); sigaddset(&block, SIGINT);
    assert(!pthread_sigmask(SIG_BLOCK, &block, NULL));
    uint64_t start = now_ms();
    while (!exists(owner)) { assert(now_ms() - start < 2000 && "worker did not start"); pause_ms(2); }
    pause_ms(100);

    unsigned before = atomic_load(&scans); pause_ms(2500);
    unsigned idle = atomic_load(&scans) - before;
    printf("idle: %u folder reads in 2500 ms (safety rescan every %u ms)\n", idle, (unsigned)CP_IDLE_RESCAN_MS);
    assert(idle >= 2 && idle <= 3 && "an idle spooler waits for a job; only the safety rescan reads its folder");

    /* A job published without a ring is found by the safety rescan. */
    atomic_store(&muted, 1);
    before = atomic_load(&scans);
    int quiet = cp_submit_raw(NULL, CP_PRINTERS, "unrung", "Q", 1, NULL, target);
    assert(quiet > 0);
    unsigned quiet_ms = slot_gone((unsigned)quiet, 3000);
    atomic_store(&muted, 0);
    printf("unrung: job %d printed after %u ms, %u folder reads\n", quiet, quiet_ms, atomic_load(&scans) - before);
    assert(quiet_ms <= CP_IDLE_RESCAN_MS + 300 && atomic_load(&scans) - before <= 3);

    before = atomic_load(&scans);
    int id = cp_submit_raw(NULL, CP_PRINTERS, "wake", "ABC", 3, NULL, target);
    assert(id > 0);
    unsigned ms = slot_gone((unsigned)id, 1000);
    printf("ring: job %d printed after %u ms, %u folder reads\n", id, ms, atomic_load(&scans) - before);
    assert(ms <= 500 && "a submitted job wakes the worker");

    /* Publication between the worker's empty scan and its sleep. */
    atomic_store(&inject, 1); assert(!kill(getpid(), SIGCHLD));
    start = now_ms();
    while (atomic_load(&inject)) { assert(now_ms() - start < 1000); pause_ms(1); }
    ms = slot_gone((unsigned)atomic_load(&race_id), 1000);
    printf("race: job published after the empty scan printed after %u ms\n", ms);
    assert(ms <= 500 && "no lost wake-up between the scan and the sleep");

    /* Offline output: retried every CP_RETRY_MS, never in a loop. */
    snprintf(missing, sizeof(missing), "%s.missing", CP_ROOT);
    snprintf(late, sizeof(late), "%s/out.prn", missing);
    before = atomic_load(&scans);
    id = cp_submit_raw(NULL, CP_PRINTERS, "offline", "OFF", 3, NULL, late);
    assert(id > 0); pause_ms(1000);
    unsigned offline = atomic_load(&scans) - before;
    struct cp_job j; assert(!cp_job_read(NULL, (unsigned)id, &j) && j.state == CP_OFFLINE);
    printf("offline: %u folder reads in 1000 ms (retry %u ms)\n", offline, (unsigned)CP_RETRY_MS);
    assert(offline >= 2 && offline <= 6 && "offline jobs are retried on the retry interval");
    assert(!mkdir(missing, 0700));      /* no ring: the retry timer must find it */
    ms = slot_gone((unsigned)id, 2000);
    printf("offline: printed %u ms after the port appeared\n", ms);
    assert(ms <= 1000);

    FILE *f = fopen(late, "rb"); assert(f); char text[8] = {0}; assert(fread(text, 1, sizeof(text), f) == 3); fclose(f);
    assert(!memcmp(text, "OFF", 3));
    assert(!kill(getpid(), SIGTERM));
    void *result; assert(!pthread_join(t, &result) && !result && !exists(owner));
    puts("PASS idle spooler sleeps; ring, race and offline retry print; stop releases worker.lock");
    return 0;
}
