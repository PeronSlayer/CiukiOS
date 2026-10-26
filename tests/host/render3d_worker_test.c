/* Actual production worker queue, staging ownership, deadlines and cleanup.
 * Hardware nomination and user address space are host boundaries. GPL-2.0-only */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/desktop.h>
#include <ciuki/display.h>
#include <ciuki/w32_core.h>
#define CIUKI_CPU_H
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
#include "../../src/kernel/core/render3d_transport.c"
volatile uint64_t g_ticks;
static unsigned allocations, dispatches, sleep_count;
static bool stop_worker, fail_alloc, fail_read, fail_write;
static struct process process={.pid=7,.state=PROC_LIVE};
static struct proc_thread thread={.process=&process};
static struct fb_device device={.present=true,.generation=42};
static unsigned char user_memory[sizeof(struct ciuki_r3d_caps)];
void *desktop_buffer_alloc(uint32_t n,enum mm_owner owner) {
    (void)owner;
    if (fail_alloc) return 0;
    void *p=malloc(n); if (p) allocations++; return p;
}
void desktop_buffer_free(void *p,uint32_t n,enum mm_owner owner) {
    (void)n;(void)owner;if (p) { assert(allocations);allocations--;free(p); }
}
struct process *proc_find(uint32_t p) { return p==7 ? &process : 0; }
const struct fb_device *fbdev_get(void) { return &device; }
int display_worker_start(void) { return 0; }
int ua_read(const struct uaddr *u,void *out,uint32_t address,uint32_t n) {
    (void)u;
    if (fail_read || address!=1 || n>sizeof(user_memory)) return -EFAULT;
    memcpy(out,user_memory,n);return 0;
}
int ua_write(const struct uaddr *u,uint32_t address,const void *data,uint32_t n) {
    (void)u;
    if (fail_write || address!=1 || n>sizeof(user_memory)) return -EFAULT;
    memcpy(user_memory,data,n);return 0;
}
void task_sleep_ms(uint32_t ms) { g_ticks+=ms;sleep_count++;if (!stop_worker) { dispatches++;w32_render3d_poll(); } }
int render3d_native_init(struct render3d_service *s,const struct render3d_host *h) {
    render3d_init(s,h,device.generation,0,0,(struct render3d_extent){0});return 0;
}
int display_present_render3d(void *x,uint32_t p,const struct ciuki_w32_render3d_args *a,const void *v,uint32_t b) {
    (void)x;(void)p;(void)a;(void)v;(void)b;assert(false);return -EACCES;
}
static struct ciuki_w32_render3d_args query(void) {
    return (struct ciuki_w32_render3d_args){.action=CIUKI_R3D_QUERY,.address=1,.bytes=sizeof(struct ciuki_r3d_caps),
        .kind=CIUKI_R3D_D16,.format=CIUKI_R3D_ARGB8888,.width=64,.height=64};
}
int main(void) {
    struct ciuki_w32_render3d_args a=query();
    assert(!w32_render3d_call(&thread,&a) && !a.error && a.result && dispatches==1);
    struct ciuki_r3d_caps c;memcpy(&c,user_memory,sizeof(c));
    assert(c.version==1 && c.generation==42 && !c.features && allocations==1 && !staging && !pending);
    fail_write=true;a=query();w32_render3d_call(&thread,&a);assert(a.error==EFAULT && !pending && !staging);fail_write=false;
    stop_worker=true;a=query();unsigned before=sleep_count;
    w32_render3d_call(&thread,&a);assert(a.error==ETIMEDOUT && sleep_count-before<=2000);
    assert(!pending && !staging && allocations==1);stop_worker=false;
    fail_alloc=true;a=query();w32_render3d_call(&thread,&a);assert(a.error==ENOMEM);fail_alloc=false;
    device.generation++;a=query();w32_render3d_call(&thread,&a);
    memcpy(&c,user_memory,sizeof(c));assert(!a.error && c.generation==43 && allocations==1);
    uint32_t ctx,res;
    assert(!render3d_create_context(service,7,R3D_SOFTWARE,&ctx));
    assert(!render3d_create_resource(service,7,ctx,R3D_COLOR,8,8,0,0,&res));
    process.state=PROC_STOPPING;w32_render3d_poll();assert(!service->ram_bytes && allocations==1);
    process.state=PROC_LIVE;
    assert(!render3d_close(service));desktop_buffer_free(service,sizeof(*service),MM_IO_REQUEST);service=0;
    assert(!allocations && !thread.cleanup && !thread.operation);
    puts("PASS production worker: copied caps/software gate, output fault, bounded stalled queue, allocation refusal, mode generation, process death, zero allocations");
    return 0;
}
