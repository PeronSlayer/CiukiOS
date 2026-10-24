/* G30 scheduler service: the worker step, the frame-thread launch path through
 * the launcher seam, failure reporting, a full queue, and the real worker
 * thread binding with stubbed launcher. SPDX-License-Identifier: MIT */
#include "../../../apps/desktop/schedsvc.h"
#include "../../../apps/desktop/desktop.h"
#include "../../../apps/systools/systools.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---- stubs for the desktop launcher and notices ---- */
static char launched[16][96]; static unsigned launched_count; static int launch_result;
static char notices[8][128]; static int notice_errors[8]; static unsigned notice_count;
int desk_task_launch(struct desktop *d, const char *name, const char *argument)
{
    (void)d;
    snprintf(launched[launched_count++ % 16], 96, "%s|%s", name, argument ? argument : "");
    return launch_result ? launch_result : (int)(100 + launched_count);
}
void desk_shell_notify_error(struct desktop *d, const char *title, int error)
{
    (void)d; snprintf(notices[notice_count % 8], 128, "%s", title); notice_errors[notice_count++ % 8] = error;
}
void desk_launch_origin(struct desktop *d, unsigned origin, const char *label)
{ (void)label; memset(&d->launch_request, 0, sizeof(d->launch_request)); d->launch_request.origin = origin; }
static void finish(struct desk_schedsvc *svc)
{ struct desktop d = {0}; d.schedsvc = svc; for (unsigned pid = 101; pid < 150; pid++) desk_schedsvc_exited(&d, pid, 0); }
static unsigned wakes;
static void wake(void *ctx) { (void)ctx; __atomic_fetch_add(&wakes, 1, __ATOMIC_RELAXED); }

/* ---- fake io ---- */
struct fake { char file[SS_TEXT]; int have, saves; int64_t now; };
static int fl(void *c, char *t, size_t cap, size_t *n) { struct fake *f = c; if (!f->have) return -ENOENT; *n = strlen(f->file); assert(*n < cap); memcpy(t, f->file, *n); return 0; }
static int fs(void *c, const char *t, size_t n) { struct fake *f = c; memcpy(f->file, t, n); f->file[n] = 0; f->have = 1; f->saves++; return 0; }
static int64_t fn(void *c) { return ((struct fake *)c)->now; }
static int fk(void *c, const char *p, const char *a) { (void)c; return desk_task_launch(NULL, p, a); }
static unsigned failed_count; static char failed_name[48]; static int failed_error;
static void ff(void *c, const char *p, int e) { (void)c; failed_count++; snprintf(failed_name, sizeof(failed_name), "%s", p); failed_error = e; }
static struct st_task task(const char *name, const char *command, int kind, unsigned minute, struct st_date start, int64_t created)
{
    struct st_task t; memset(&t, 0, sizeof(t));
    snprintf(t.name, sizeof(t.name), "%s", name); snprintf(t.command, sizeof(t.command), "%s", command);
    t.kind = kind; t.minute_of_day = minute; t.start = start; t.created = created; t.last_run = -1;
    return t;
}
static void test_step_and_drain(void)
{
    struct st_date day = { 2026, 10, 24 };
    int64_t base = st_minutes(day, 8 * 60);
    struct st_store s; st_init(&s);
    struct st_task a = task("report", "/Programs/hwreport", ST_DAILY, 9 * 60, day, base);
    struct st_task b = task("files", "files /mnt/c", ST_DAILY, 9 * 60 + 5, day, base);
    struct st_task c = task("boot", "sysmon", ST_AT_START, 0, (struct st_date){ 1970, 1, 1 }, base);
    assert(st_add(&s, &a) == 0 && st_add(&s, &b) == 0 && st_add(&s, &c) == 0);
    static struct fake f; memset(&f, 0, sizeof(f));
    assert(st_save(&s, f.file, sizeof(f.file)) > 0); f.have = 1; f.now = base + 1;
    struct desk_schedsvc_io io = { &f, fl, fs, fn, fk, ff };
    struct desk_schedsvc *svc = desk_schedsvc_create(&io);
    assert(svc);
    assert(desk_schedsvc_step(svc) == 1 && launched_count == 0);         /* the worker only queues */
    assert(desk_schedsvc_drain(svc) == 1 && launched_count == 1 && !strcmp(launched[0], "sysmon|"));
    assert(desk_schedsvc_drain(svc) == 0);
    finish(svc);
    f.now = base + 9 * 60;                                              /* both daily tasks are due at 09:05 */
    assert(desk_schedsvc_step(svc) == 2 && desk_schedsvc_drain(svc) == 2);
    assert(!strcmp(launched[1], "hwreport|") && !strcmp(launched[2], "files|/mnt/c"));
    assert(f.saves >= 2 && desk_schedsvc_step(svc) == 0);               /* last runs were recorded and written */
    finish(svc);
    f.now += 1440;
    launch_result = -ENOENT;                                            /* the launcher refuses: the user is told */
    assert(desk_schedsvc_step(svc) == 2 && desk_schedsvc_drain(svc) == 0 && failed_count == 2 && failed_error == ENOENT);
    launch_result = 0;
    desk_schedsvc_destroy(svc);
    /* more due tasks than the queue holds: the rest stay due and run on the next step */
    struct st_store many; st_init(&many);
    for (unsigned i = 0; i < 10; i++) {
        char name[16]; snprintf(name, sizeof(name), "t%u", i);
        struct st_task t = task(name, "calc", ST_DAILY, 9 * 60, day, base);
        assert(st_add(&many, &t) == 0);
    }
    memset(&f, 0, sizeof(f)); assert(st_save(&many, f.file, sizeof(f.file)) > 0); f.have = 1; f.now = base + 120;
    svc = desk_schedsvc_create(&io);
    unsigned before = launched_count;
    int queued = desk_schedsvc_step(svc);
    assert(queued == DESK_SCHED_QUEUE && desk_schedsvc_drain(svc) == DESK_SCHED_QUEUE);
    assert(desk_schedsvc_step(svc) == 2 && desk_schedsvc_drain(svc) == 2 && launched_count == before + 10);
    desk_schedsvc_destroy(svc);
    puts("PASS G30 scheduler service: queue, launch through the launcher, failure notice, full queue");
}
static unsigned stop_calls;
static int transient_stop(uint32_t pid, int signal_number)
{ assert(pid == 901 && signal_number == SIGTERM); if (!stop_calls++) { errno=EAGAIN; return -1; } return 0; }
static int async_launch(void *ctx, const char *program, const char *argument)
{ (void)ctx; (void)program; (void)argument; return 0; }
static void test_conditions_and_results(void)
{
    struct st_date day = {2026, 10, 24}; int64_t base = st_minutes(day, 9 * 60);
    struct st_store store; st_init(&store);
    struct st_task t = task("conditional", "calc", ST_AT_START, 0, day, base);
    t.conditions = ST_IDLE | ST_AC_ONLY | ST_STOP_IDLE; t.idle_minutes = 5;
    assert(!st_add(&store, &t));
    static struct fake f; memset(&f, 0, sizeof(f)); f.have = 1; f.now = base;
    assert(st_save(&store, f.file, sizeof(f.file)) > 0);
    struct desk_schedsvc_io io = {&f, fl, fs, fn, async_launch, ff};
    struct desk_schedsvc *svc = desk_schedsvc_create(&io); assert(svc);
    static struct desktop d; memset(&d, 0, sizeof(d)); d.schedsvc = svc; desk_schedsvc_bind(svc, &d);
    desk_schedsvc_conditions(svc, 4, 1); assert(!desk_schedsvc_step(svc));
    desk_schedsvc_conditions(svc, 5, 0); assert(!desk_schedsvc_step(svc));
    desk_schedsvc_conditions(svc, 5, 1); assert(desk_schedsvc_step(svc) == 1);
    assert(desk_schedsvc_drain(svc) == 1); /* zero means asynchronous spawn acceptance */
    st_load(&store, f.file); assert(store.t[0].last_result == ST_RUNNING);
    assert(!desk_schedsvc_step(svc));
    desk_schedsvc_completed(&d, 1, 900);
    desk_schedsvc_exited(&d, 900, 23 << 8);
    assert(!desk_schedsvc_step(svc)); st_load(&store, f.file); assert(store.t[0].last_result == 23);
    desk_schedsvc_conditions(svc, 0, 0); /* explicit Run bypasses these conditions */
    assert(desk_schedsvc_run(&d, "conditional") == 1 && desk_schedsvc_step(svc) == 1);
    assert(desk_schedsvc_drain(svc) == 1);
    desk_schedsvc_completed(&d, 2, -ENOENT); assert(!desk_schedsvc_step(svc));
    st_load(&store, f.file); assert(store.t[0].last_result == -ENOENT);
    /* A failed signal is retried; only the child exit is a last result. */
    assert(desk_schedsvc_run(&d, "conditional") == 1 && desk_schedsvc_step(svc) == 1);
    assert(desk_schedsvc_drain(svc) == 1); desk_schedsvc_completed(&d, 3, 901);
    d.terminate=transient_stop; stop_calls=0;
    assert(!desk_schedsvc_step(svc)); desk_schedsvc_drain(svc); assert(stop_calls==0);
    desk_schedsvc_conditions(svc, 5, 1); assert(!desk_schedsvc_step(svc));
    desk_schedsvc_conditions(svc, 0, 0);
    assert(!desk_schedsvc_step(svc)); desk_schedsvc_drain(svc); assert(stop_calls==1);
    desk_schedsvc_drain(svc); assert(stop_calls==2);
    desk_schedsvc_exited(&d,901,SIGTERM); assert(!desk_schedsvc_step(svc));
    st_load(&store,f.file); assert(store.t[0].last_result==128+SIGTERM);
    desk_schedsvc_destroy(svc);
    puts("PASS G30B scheduler queue: conditions, deferred spawn, exit result and failed manual launch");
}
static struct desktop desktop;
static void test_thread_binding(void)
{
    memset(&desktop, 0, sizeof(desktop));
    desktop.wake = wake;
    desktop.local_offset = 3600;
    unsigned before = launched_count;
    assert(desk_schedsvc_start(&desktop) == 0 && desktop.schedsvc && desk_schedsvc_start(&desktop) == 0);   /* idempotent */
    desk_schedsvc_tick(&desktop);                                      /* the real file does not exist: an empty schedule */
    desktop.power.state = 1;
    desk_schedsvc_tick(&desktop);
    desktop.power.state = 0;
    assert(launched_count == before);
    desk_schedsvc_stop(&desktop);
    assert(!desktop.schedsvc);
    desk_schedsvc_stop(&desktop);                                      /* NULL-safe */
    desk_schedsvc_tick(&desktop);
    puts("PASS G30 scheduler service: worker thread starts, ticks and stops");
}
int main(void)
{
    test_step_and_drain();
    test_conditions_and_results();
    test_thread_binding();
    puts("PASS G30 desktop scheduler service");
    return 0;
}
