/* Production GDI query -> copied syscall -> actual display worker loop.
 * Deterministic UP context switches model only the scheduler, not the loop or
 * enqueue/predicate path. No direct poll from the caller. GPL-2.0-only */
#define _XOPEN_SOURCE 700
#include <ucontext.h>
#include <time.h>
#include <stdlib.h>
/* Kernel ABI deliberately owns these spellings in this mixed host fixture. */
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#define DISPLAY_WORKER_SCHEDULING
#define main frozen_display_main
#include "display_test.c"
#undef main
#include <ciuki/w32_core.h>
#include "../../src/kernel/core/render3d_transport.c"
#include "../../sdk/win32/opengl32/backend.h"

static ucontext_t caller_context, worker_context;
static unsigned char worker_stack[128*1024];
static bool in_worker, runnable;
static uint64_t wake_at;
static struct kwait *sleep_queue;
static kwait_cond_fn sleep_condition;
static void *sleep_argument, *payload;
static unsigned switches_to_worker, notifications, caller_sleeps, native_inits;
static bool inject_before_wait;
static unsigned immediate_waits;
static struct proc_thread caller_thread;

static void run_worker(void)
{
    in_worker=true; switches_to_worker++;
    assert(!swapcontext(&caller_context,&worker_context));
    in_worker=false;
}
static void park_worker(uint64_t deadline)
{
    wake_at=deadline; runnable=false;
    assert(!swapcontext(&worker_context,&caller_context));
}
bool scheduling_wait(struct kwait *q,kwait_cond_fn cond,void *arg,uint64_t deadline)
{
    assert(in_worker);
    if (inject_before_wait) { inject_before_wait=false; display_worker_wake(); }
    if (cond(arg)) { immediate_waits++; return true; }
    sleep_queue=q; sleep_condition=cond; sleep_argument=arg;
    park_worker(deadline);
    sleep_queue=0;
    return cond(arg);
}
void scheduling_wake(struct kwait *q)
{
    notifications++;
    if (q==sleep_queue && sleep_condition(sleep_argument)) runnable=true;
}
void scheduling_sleep(uint32_t ms)
{
    if (in_worker) { park_worker(g_ticks+ms); return; }
    caller_sleeps++; g_ticks+=ms;
    if (runnable || deadline_passed(wake_at)) run_worker();
}
static void worker_entry(void) { watchdog_main(0); }
void *desktop_buffer_alloc(uint32_t n,enum mm_owner owner)
{ (void)owner; return calloc(1,(n+PAGE_SIZE-1)&~(PAGE_SIZE-1)); }
void desktop_buffer_free(void *p,uint32_t n,enum mm_owner owner)
{ (void)n; (void)owner; free(p); }
int ua_read(const struct uaddr *u,void *out,uint32_t address,uint32_t n)
{ (void)u; assert(address==1 && payload); memcpy(out,payload,n); return 0; }
int ua_write(const struct uaddr *u,uint32_t address,const void *data,uint32_t n)
{ (void)u; assert(address==1 && payload); memcpy(payload,data,n); return 0; }
int render3d_native_init(struct render3d_service *s,const struct render3d_host *h)
{
    native_inits++;
    render3d_init(s,h,fbdev_get()->generation,0,0,(struct render3d_extent){0}); return 0;
}
int display_present_render3d(void *ctx,uint32_t p,const struct ciuki_w32_render3d_args *a,const void *v,uint32_t b)
{ (void)ctx;(void)p;(void)a;(void)v;(void)b; assert(false); return -EACCES; }
int w32_render3d_host_exchange(struct ciuki_w32_render3d_args *a,void *data)
{
    payload=data; a->address=data ? 1 : 0;
    w32_render3d_call(&caller_thread,a);
    return a->error ? -(int)a->error : a->result ? 0 : -EIO;
}
void *gdi_alloc(uint32_t n) { return calloc(1,n); }
void gdi_free(void *p,uint32_t n) { (void)n; free(p); }
extern const struct gl_hw_driver *w32_gdi_render3d_native(unsigned);
static double seconds(void)
{ struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t)); return t.tv_sec+t.tv_nsec/1e9; }

int main(void)
{
    fixture(); desktop.state=PROC_LIVE; caller_thread.process=&desktop;
    assert(!display_worker_start());
    assert(!getcontext(&worker_context));
    worker_context.uc_stack.ss_sp=worker_stack;
    worker_context.uc_stack.ss_size=sizeof(worker_stack);
    makecontext(&worker_context,worker_entry,0);
    run_worker(); /* Actually park in the production worker's 100 ms wait. */
    assert(wake_at==g_ticks+100 && !runnable);
    const struct gl_hw_driver *gdi=w32_gdi_render3d_native(GL_HW_VERSION);
    struct gl_hw_format f={640,480,16,0,0}; struct gl_hw_caps caps;
    uint64_t before=g_ticks;
    assert(!gdi->query(1,&f,&caps));
    assert(g_ticks-before<=1 && native_inits==1 && notifications);
    unsigned sleeps=caller_sleeps, switches=switches_to_worker;
    double start=seconds(); before=g_ticks;
    for (unsigned i=0;i<100000;i++) {
        f.width=i&1 ? 64 : 640; f.height=i&1 ? 64 : 480;
        assert(!gdi->query(1,&f,&caps));
    }
    double us=(seconds()-start)*1e6/100000;
    assert(g_ticks==before && sleeps==caller_sleeps && switches==switches_to_worker);
    printf("PASS real display worker/GDI: 100000 unqualified probes, %.3f host us/call, 0 scheduled ticks/call\n",us);
    device.generation++; before=g_ticks;
    assert(!gdi->query(1,&f,&caps));
    assert(native_inits==2 && g_ticks-before<=1);
    /* Mutations always enqueue and wake the already sleeping actual loop. */
    struct ciuki_w32_render3d_args a={.action=CIUKI_R3D_OPEN,.generation=device.generation};
    before=g_ticks;
    assert(w32_render3d_host_exchange(&a,0)==-EOPNOTSUPP && g_ticks-before<=1);
    /* Enqueue between poll and wait: publish-before-notify is persistent. */
    unsigned immediate=immediate_waits;
    inject_before_wait=true; run_worker();
    assert(!inject_before_wait && immediate_waits==immediate+1 && !worker_has_request(0));
    assert(!render3d_close(service)); rfree(0,service); service=0;
    free(aperture); free(window); aperture=window=0;
    assert(!resident_bytes);
    return 0;
}
