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
static uint32_t physical_bytes, physical_peak;
static struct { void *p; uint32_t bytes; } blocks[9000];
static bool stop_worker, fail_alloc, fail_read, fail_write;
static struct process process={.pid=7,.state=PROC_LIVE};
static struct proc_thread thread={.process=&process};
static struct fb_device device={.present=true,.generation=42};
static unsigned char user_memory[R3D_MAX_SIZE*R3D_MAX_SIZE*4u];
static unsigned presents;
void *desktop_buffer_alloc(uint32_t n,enum mm_owner owner) {
    (void)owner;
    if (fail_alloc) return 0;
    uint32_t bytes=(n+PAGE_SIZE-1)&~(PAGE_SIZE-1);
    void *p=malloc(bytes);
    if (p) {
        unsigned i=0; while (i<9000 && blocks[i].p) i++; assert(i<9000);
        blocks[i].p=p; blocks[i].bytes=bytes;
        allocations++; physical_bytes+=bytes;
        if (physical_bytes>physical_peak) physical_peak=physical_bytes;
    }
    return p;
}
void desktop_buffer_free(void *p,uint32_t n,enum mm_owner owner) {
    (void)owner;
    if (p) {
        unsigned i=0; while (i<9000 && blocks[i].p!=p) i++; assert(i<9000);
        assert(blocks[i].bytes==((n+PAGE_SIZE-1)&~(PAGE_SIZE-1)));
        physical_bytes-=blocks[i].bytes; blocks[i].p=0;
        assert(allocations);allocations--;free(p);
    }
}
struct process *proc_find(uint32_t p) { return p==7 ? &process : 0; }
const struct fb_device *fbdev_get(void) { return &device; }
int display_worker_start(void) { return 0; }
void display_worker_wake(void) { }
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
static unsigned direct_presents;
int display_present_render3d(void *x,uint32_t p,const struct ciuki_w32_render3d_args *a,const void *v,uint32_t b) {
    (void)x;
    assert(p==7 && a->token==1234 && a->pitch==a->width*4 && b==a->width*a->height*4);
    /* f7-15 contract: NULL pixels first ask for the direct path only; this
     * presenter has none, so the dispatcher reads back and calls again. */
    if (!v) { direct_presents++; return -EOPNOTSUPP; }
    for (unsigned i=0;i<b;i++) assert(((const unsigned char *)v)[i]==(unsigned char)(i*37));
    presents++;return 0;
}
static struct ciuki_w32_render3d_args query(void) {
    return (struct ciuki_w32_render3d_args){.action=CIUKI_R3D_QUERY,.address=1,.bytes=sizeof(struct ciuki_r3d_caps),
        .kind=CIUKI_R3D_D16,.format=CIUKI_R3D_ARGB8888,.width=64,.height=64};
}
static bool qualified=true;
static bool is_qualified(void *ctx,uint32_t generation)
{ (void)ctx; return qualified && generation==device.generation; }
static int unused_layout(void *ctx,struct render3d_resource *r)
{ (void)ctx;(void)r;assert(false);return -EIO; }
static int unused_execute(void *ctx,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{ (void)ctx;(void)q;(void)r;assert(false);return -EIO; }
static int unused_enter(void *ctx) { (void)ctx;assert(false);return -EIO; }
static void unused_leave(void *ctx) { (void)ctx;assert(false); }
static void snapshots(void) {
    static const struct ciuki_r3d_caps caps={.version=CIUKI_R3D_VERSION,.max_size=64,.features=R3D_FEATURES,
        .depth_formats=CIUKI_R3D_D16,.color_formats=CIUKI_R3D_ARGB8888};
    static const struct render3d_backend backend={.features=R3D_FEATURES,.qualified=is_qualified,
        .layout=unused_layout,.validate=unused_execute,.enter=unused_enter,.leave=unused_leave,
        .execute=unused_execute,.idle=unused_enter,.quarantine=unused_leave,.max_size=64,.capabilities=&caps};
    service->backend=&backend; service->heap_first=4096; service->heap_end=8192;
    unsigned before=dispatches, sleeps=sleep_count;
    for (unsigned i=0;i<100;i++) {
        struct ciuki_w32_render3d_args a=query();
        a.width=i%3 ? 32 : 640;
        qualified=i%2;
        w32_render3d_call(&thread,&a);
        struct ciuki_r3d_caps c;memcpy(&c,user_memory,sizeof(c));
        assert(!a.error && c.generation==device.generation && !!c.features==(qualified && i%3));
    }
    assert(dispatches==before && sleep_count==sleeps);
    service->backend=0; service->heap_first=service->heap_end=0;
}
static int full_layout(void *ctx,struct render3d_resource *r) {
    (void)ctx; r->reserved=(r->bytes+4095)&~4095u; return 0;
}
static int full_enter(void *ctx) { (void)ctx; return 0; }
static void full_leave(void *ctx) { (void)ctx; }
static int full_validate(void *ctx,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS]) {
    (void)ctx;(void)q;(void)r;return 0;
}
static int full_execute(void *ctx,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS]) {
    (void)ctx;assert(q->flags==R3D_COPY && !q->count);
    memcpy(r[3]->shadow,r[0]->shadow,r[0]->bytes);return 0;
}
static void exchange(struct ciuki_w32_render3d_args *a) {
    a->result=a->error=0;
    w32_render3d_call(&thread,a);
    assert(!a->error && a->result && !pending && !staging);
}
static void full_transfers(void) {
    static const struct ciuki_r3d_caps caps={.version=CIUKI_R3D_VERSION,.max_size=1024,
        .max_width=1024,.max_height=1024,.depth_formats=CIUKI_R3D_D16,.color_formats=CIUKI_R3D_ARGB8888,.features=R3D_FEATURES,.max_pixel_work=4*1024*1024};
    static const struct render3d_backend backend={.features=R3D_FEATURES,.qualified=is_qualified,
        .layout=full_layout,.validate=full_validate,.enter=full_enter,.leave=full_leave,
        .execute=full_execute,.idle=full_enter,.quarantine=unused_leave,.max_size=1024,.capabilities=&caps};
    static const unsigned sizes[][2]={{640,480},{800,600},{1024,768},{1024,1024}};
    service->backend=&backend; service->heap_first=4096; service->heap_end=24*1024*1024;
    for (unsigned size=0;size<4;size++) {
        unsigned w=sizes[size][0],h=sizes[size][1],bytes=w*h*4;
        struct ciuki_w32_render3d_args a=query();a.width=w;a.height=h;exchange(&a);
        struct ciuki_r3d_caps c;memcpy(&c,user_memory,sizeof(c));assert(c.features);
        a=(struct ciuki_w32_render3d_args){.generation=device.generation,.action=CIUKI_R3D_OPEN};exchange(&a);
        uint32_t context=a.context,resource[3];
        const unsigned kinds[]={R3D_COLOR,R3D_D16,R3D_LINEAR_COLOR};
        for (unsigned i=0;i<3;i++) {
            a=(struct ciuki_w32_render3d_args){.generation=device.generation,.context=context,
                .action=CIUKI_R3D_RESOURCE,.kind=kinds[i],.width=w,.height=h};exchange(&a);resource[i]=a.resource;
            unsigned n=i==1 ? bytes/2 : bytes;
            for (unsigned j=0;j<n;j++) user_memory[j]=(unsigned char)(j*37);
            a.action=CIUKI_R3D_UPLOAD;a.address=1;a.bytes=n;exchange(&a);
            memset(user_memory,0,n);a.action=CIUKI_R3D_READ;exchange(&a);
            for (unsigned j=0;j<n;j++) assert(user_memory[j]==(unsigned char)(j*37));
            /* Enlarged ingress must still reject a short owned attachment. */
            a.bytes=n-1;a.result=0;w32_render3d_call(&thread,&a);assert(a.error==EINVAL);
        }
        struct render3d_request q={.flags=R3D_COPY,.target=resource[0],.copy_target=resource[2]};
        memcpy(user_memory,&q,sizeof(q));
        a=(struct ciuki_w32_render3d_args){.generation=device.generation,.context=context,
            .action=CIUKI_R3D_SUBMIT,.address=1,.bytes=sizeof(q)};exchange(&a);
        a.action=CIUKI_R3D_PRESENT;a.resource=resource[2];a.width=w;a.height=h;
        a.pitch=w*4;a.token=1234;a.bytes=bytes;a.address=0;exchange(&a);
        a=(struct ciuki_w32_render3d_args){.generation=device.generation,.context=context,
            .action=CIUKI_R3D_CLOSE};exchange(&a);
    }
    assert(presents==4 && direct_presents==4 && allocations==1);
    unsigned before=dispatches;
    for (unsigned action=CIUKI_R3D_UPLOAD;action<=CIUKI_R3D_PRESENT;action++) {
        if (action!=CIUKI_R3D_UPLOAD && action!=CIUKI_R3D_READ && action!=CIUKI_R3D_PRESENT) continue;
        struct ciuki_w32_render3d_args a={.action=action,.address=1,.bytes=CIUKI_R3D_WIRE_LIMIT+1};
        w32_render3d_call(&thread,&a);assert(a.error==EINVAL);
        a.error=0;a.bytes=R3D_MAX_SIZE*R3D_MAX_SIZE*4u+1;
        w32_render3d_call(&thread,&a);assert(a.error==EINVAL);
    }
    assert(dispatches==before);
    service->backend=0;service->heap_first=service->heap_end=0;
    puts("PASS production syscall/worker: qualified full-size color/D16 UPLOAD/READ, completed-copy PRESENT, exact resource sizes and wire ceiling");
}
static void budget(void) {
    uint32_t base=physical_bytes, count=0;
    static uint32_t pixels[32*32];
    bool full=false;
    for (unsigned i=0;i<R3D_CONTEXTS && !full;i++) {
        uint32_t ctx;
        assert(!render3d_create_context(service,100+i,R3D_SOFTWARE,&ctx));
        for (unsigned j=0;j<R3D_RESOURCES;j++) {
            uint32_t res;
            int e=render3d_create_resource(service,100+i,ctx,R3D_TEXTURE,32,32,pixels,sizeof(pixels),&res);
            assert(physical_bytes<=R3D_RAM_LIMIT);
            if (e) { assert(e==-ENOMEM); full=true; break; }
            count++;
        }
    }
    assert(full && count>3000 && count<4096);
    printf("PASS resident budget: %u small textures, peak=%u, limit=%u\n",count,physical_peak,R3D_RAM_LIMIT);
    /* Jobs and wire payloads share the very same resident limit. An otherwise
     * legal 4 MiB texture upload must fail before touching user memory. */
    struct ciuki_w32_render3d_args a={.action=CIUKI_R3D_TEXTURE,.address=1,.bytes=1024*1024*4,
        .width=1024,.height=1024,.levels=1};
    uint32_t before=physical_bytes;
    w32_render3d_call(&thread,&a);
    assert(a.error==ENOMEM && physical_bytes==before && physical_peak<=R3D_RAM_LIMIT);
    /* A failed close retains pinned shadows and their charge. */
    service->quarantined=true;
    service->contexts[0].hardware=true;
    struct render3d_resource *pinned=&service->contexts[0].resources[0];
    pinned->hardware=true;
    pinned->references=1;
    assert(render3d_close(service)==-EIO && pinned->shadow && physical_bytes>base);
    before=physical_bytes;
    a=query(); device.generation++; w32_render3d_call(&thread,&a);
    assert(a.error==EIO && physical_bytes==before);
    /* Test-only proof of idle; production retains quarantine indefinitely. */
    pinned->references=0; service->quarantined=false;
    assert(!render3d_close(service) && physical_bytes==base);
}
int main(int argc,char **argv) {
    struct ciuki_w32_render3d_args a=query();
    assert(!w32_render3d_call(&thread,&a) && !a.error && a.result && dispatches==1);
    struct ciuki_r3d_caps c;memcpy(&c,user_memory,sizeof(c));
    assert(c.version==CIUKI_R3D_VERSION && c.generation==42 && !c.features && allocations==1 && !staging && !pending);
    if (argc==2 && !strcmp(argv[1],"budget")) { budget(); goto done; }
    full_transfers();
    unsigned calls=dispatches, sleeps=sleep_count;
    for (unsigned i=0;i<10000;i++) {
        a=query(); w32_render3d_call(&thread,&a); assert(!a.error && a.result);
    }
    assert(dispatches==calls && sleep_count==sleeps);
    snapshots();
    fail_write=true;a=query();w32_render3d_call(&thread,&a);assert(a.error==EFAULT && !pending && !staging);fail_write=false;
    stop_worker=true;device.generation++;a=query();unsigned before=sleep_count;
    w32_render3d_call(&thread,&a);assert(a.error==ETIMEDOUT && sleep_count-before<=2000);
    assert(!pending && !staging && allocations==1);stop_worker=false;
    fail_alloc=true;a=query();w32_render3d_call(&thread,&a);assert(a.error==ENOMEM);fail_alloc=false;
    device.generation++;a=query();w32_render3d_call(&thread,&a);
    memcpy(&c,user_memory,sizeof(c));assert(!a.error && c.generation==device.generation && allocations==1);
    uint32_t ctx,res;
    assert(!render3d_create_context(service,7,R3D_SOFTWARE,&ctx));
    assert(!render3d_create_resource(service,7,ctx,R3D_COLOR,8,8,0,0,&res));
    process.state=PROC_STOPPING;w32_render3d_poll();assert(!service->ram_bytes && allocations==1);
    process.state=PROC_LIVE;
done:
    assert(!render3d_close(service));rfree(0,service);service=0;
    assert(!allocations && !physical_bytes && !resident_bytes && !thread.cleanup && !thread.operation);
    puts("PASS production worker: copied caps/software gate, output fault, bounded stalled queue, allocation refusal, mode generation, process death, zero allocations");
    return 0;
}
