/* H1 Quake III replay through the production GL/GDI/copied wire and the
 * independent Mach64 register model (f7-27). Legacy gl_savage_* names are
 * the existing fixture hooks of tests/host/win32/gl/quake3.c.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "mach64_3d_model.h"
#include <ciuki/kernel.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../sdk/win32/opengl32/encode.h"
#include "../../src/kernel/drivers/video/render3d_transport.h"
#define CHECK(x) assert(x)
#define gpu3d m64m.device
static struct render3d_service scratch3d,reference3d;
static unsigned wire_pid=7,lease_token,lease_generation,wire_presented;
static uint32_t frame_pixels[1024*768];
static unsigned mode_width=640,mode_height=480;
static int qualification_error;
/* Native registry attachment is covered by mach64_3d_owner_test.c. */
const struct display_driver *display_driver_bound(void) { return 0; }
const struct fb_device *fbdev_get(void) { return &m64m.engine.fb; }
static unsigned work_submissions,work_vertices[2],work_refusals;
static uint64_t work_peak,work_total;
void gl_kernel_work_reset(void)
{
    work_submissions=work_refusals=0; work_peak=work_total=0;
    memset(work_vertices,0,sizeof(work_vertices));
}
void gl_kernel_work_check(unsigned w,unsigned h)
{
    (void)w; (void)h;
    printf("mach64 pixel-work split: submissions=%u refusals=%u peak=%llu total=%llu\n",work_submissions,work_refusals,
        (unsigned long long)work_peak,(unsigned long long)work_total);
}
void gl_kernel_mode(unsigned w,unsigned h) { mode_width=w; mode_height=h; }
uint64_t gl_kernel_fragments(void) { return m64m.fragments; }
static int present_wire(void *ctx,uint32_t pid,const struct ciuki_w32_render3d_args *a,const void *data,uint32_t bytes)
{
    (void)ctx;
    if (pid!=7 || a->token!=lease_token || a->generation!=lease_generation) return -EACCES;
    memcpy(frame_pixels,data,bytes); wire_presented++; return 0;
}
void gl_kernel_setup(int qualified)
{
    m64m_init(mode_width,mode_height);
    struct render3d_host host={0,m64m_alloc,m64m_free,m64m_now,0};
    qualification_error=-ENODEV;
    struct render3d_extent heap; uint32_t minimum,full;
    int fit=mach64_3d_mode_heap(&m64m.engine,&m64m.engine.fb,&heap,&minimum,&full);
    /* The owner refuses Q2 when two mode-sized colour buffers, D16 and the
     * Q2 textures do not fit the heap above the scanout (8 MiB aperture). */
    if (qualified && !fit && heap.bytes>=full) {
        m64m_resources(&scratch3d,64,64,true,false);
        gpu3d.consumer=&scratch3d;
        qualification_error=mach64_3d_probe(&gpu3d);
        CHECK(!render3d_close(&scratch3d));
        if (!qualification_error && qualified==2) {
            gpu3d.fullsize_testing=true;
            m64m_resources(&scratch3d,mode_width,mode_height,true,true);
            m64m_resources(&reference3d,mode_width,mode_height,false,true);
            gpu3d.fullsize_testing=false; gpu3d.consumer=&scratch3d;
            qualification_error=mach64_3d_fullsize_probe(&gpu3d,&reference3d);
            CHECK(!render3d_close(&scratch3d) && !render3d_close(&reference3d));
        }
        gpu3d.consumer=0;
    }
    printf("GL_MACH64_SETUP width=%u height=%u heap=%u minimum=%u full=%u qualification=%d qualified=%u perspective=%u alpha=%u blend=%u\n",
        mode_width,mode_height,heap.bytes,minimum,full,qualification_error,gpu3d.qualified,gpu3d.perspective_qualified,
        gpu3d.alpha_qualified,gpu3d.blend_qualified);
    render3d_init(&scratch3d,&host,42,mach64_3d_backend(),&gpu3d,gpu3d.heap);
    gpu3d.consumer=&scratch3d; wire_pid=7;
    lease_token=1234; lease_generation=42; wire_presented=0;
}
void gl_kernel_teardown(void)
{
    CHECK(!render3d_close(&scratch3d)); CHECK(!scratch3d.ram_bytes);
    CHECK(!m64m.engine.quarantined); m64m_destroy();
}
void gl_kernel_revoke(void) { lease_token++; }
void gl_kernel_process_exit(void) { render3d_process_exit(&scratch3d,7); }
unsigned gl_kernel_draws(void) { return (unsigned)m64m.draws; }
unsigned gl_kernel_copies(void) { return (unsigned)m64m.blits; }
unsigned gl_kernel_writes(void) { return (unsigned)m64m.writes; }
unsigned gl_kernel_presents(void) { return wire_presented; }
const uint32_t *gl_kernel_pixels(void) { return frame_pixels; }
int w32_render3d_host_exchange(struct ciuki_w32_render3d_args *a,void *payload)
{
    /* Independent RAM snapshot is the syscall staging seam. */
    struct ciuki_w32_render3d_args copied=*a;
    void *data=copied.bytes ? calloc(1,copied.bytes) : 0;
    bool output=a->action==CIUKI_R3D_QUERY || a->action==CIUKI_R3D_READ;
    if (copied.bytes && !data) return -ENOMEM;
    if (copied.bytes && !output && a->action!=CIUKI_R3D_PRESENT) { CHECK(payload); memcpy(data,payload,copied.bytes); }
    struct render3d_transport t={&scratch3d,0,present_wire};
    int e=render3d_transport_dispatch(&t,wire_pid,&copied,data,copied.bytes);
    if (a->action==CIUKI_R3D_SUBMIT) {
        const struct render3d_request *q=data;
        if (e) work_refusals++;
        if (!e && q->count) {
            uint64_t work=0;
            for (unsigned i=0;i<q->count;i+=3) {
                int xmin=q->vertices[i].x,xmax=xmin,ymin=q->vertices[i].y,ymax=ymin;
                for (unsigned j=i+1;j<i+3;j++) {
                    int x=q->vertices[j].x,y=q->vertices[j].y;
                    if (x<xmin) xmin=x;
                    if (x>xmax) xmax=x;
                    if (y<ymin) ymin=y;
                    if (y>ymax) ymax=y;
                }
                work+=(uint64_t)((xmax-xmin+65535)>>16)*((ymax-ymin+65535)>>16);
            }
            if (work_submissions<2) work_vertices[work_submissions]=q->count;
            work_submissions++; work_total+=work;
            if (work>work_peak) work_peak=work;
        }
    }
    if (!e && output) memcpy(payload,data,copied.bytes);
    if (!e) *a=copied;
    free(data); return e;
}

/* H1 counts after the real per-boot qualification on the model. A draw is
 * eligible when the shared GL encoder accepts it for the qualified caps AND
 * every vertex lies on the engine's quarter-pixel grid (validate). */
static unsigned admission_live,admission_class,admission_scope;
void gl_savage_scope(unsigned s) { admission_scope=s; }
static uint64_t admission_gpu,admission_cpu,admission_world,admission_entities;
static uint64_t admission_world_ok,admission_entities_ok,admission_refused[6];
static unsigned admission_switches,admission_previous;
void gl_savage_class(unsigned which) { admission_class=which; }
void gl_savage_begin(void)
{
    admission_live=1; admission_gpu=admission_cpu=admission_world=admission_entities=0;
    admission_world_ok=admission_entities_ok=admission_switches=admission_previous=0;
    memset(admission_refused,0,sizeof(admission_refused));
}
void gl_host_draw(const struct sw_state *s,const struct sw_vertex *v,int hardware)
{
    if (!admission_live) return;
    static struct gl_hw_command g;
    static struct render3d_request q;
    struct ciuki_r3d_caps caps; mach64_3d_backend()->query_caps(&gpu3d,&caps);
    g=(struct gl_hw_command){.state=*s,.count=3}; memcpy(g.vertices,v,3*sizeof(*v));
    bool encoded=!gl_hw_encode(&caps,&g,1,2,0,&q),grid=true;
    for (unsigned i=0;i<3;i++) if (v[i].x&3 || v[i].y&3) grid=false; /* Q4 GL to quarter pixels */
    bool add=false;
    for (unsigned i=0;i<2;i++) if (s->unit[i].enabled && s->unit[i].mode==GL_ADD) add=true;
    bool eligible=encoded && grid && !add;
    if (admission_class==1) { admission_world++; admission_world_ok+=eligible; }
    if (admission_class==2) { admission_entities++; admission_entities_ok+=eligible; }
    if (!eligible) {
        unsigned reason=add ? 4 : !grid ? 5 : s->alpha_test ? 0 : s->blend ? 1 : s->fog ? 2 : s->offset ? 3 : 4;
        admission_refused[reason]++;
    }
    if (hardware) admission_gpu++; else admission_cpu++;
    unsigned current=hardware ? 1 : 2;
    if (admission_previous && current!=admission_previous) admission_switches++;
    admission_previous=current;
}
void gl_savage_end(unsigned w,unsigned h,unsigned frame,unsigned gpu_draws,uint64_t gpu_fragments,
                   uint64_t cpu_draws,uint64_t cpu_fragments)
{
    admission_live=0;
    printf("GL_MACH64_H1 scope=%s width=%u height=%u frame=%u gpu_draws=%u gpu_fragments=%llu cpu_draws=%llu cpu_fragments=%llu "
        "eligible_world=%llu/%llu eligible_entities=%llu/%llu queued_gpu=%llu direct_cpu=%llu switches=%u "
        "refused_alpha=%llu refused_blend=%llu refused_fog=%llu refused_offset=%llu refused_combine=%llu refused_grid=%llu qualified=%u\n",
        admission_scope ? "complete-multipass" : "complete",w,h,frame,gpu_draws,(unsigned long long)gpu_fragments,
        (unsigned long long)cpu_draws,(unsigned long long)cpu_fragments,
        (unsigned long long)admission_world_ok,(unsigned long long)admission_world,
        (unsigned long long)admission_entities_ok,(unsigned long long)admission_entities,
        (unsigned long long)admission_gpu,(unsigned long long)admission_cpu,admission_switches,
        (unsigned long long)admission_refused[0],(unsigned long long)admission_refused[1],
        (unsigned long long)admission_refused[2],(unsigned long long)admission_refused[3],
        (unsigned long long)admission_refused[4],(unsigned long long)admission_refused[5],gpu3d.qualified);
    CHECK(admission_world && admission_entities && admission_switches<=1);
    /* Every triangle is drawn exactly once, by the engine or the CPU. */
    CHECK(admission_gpu+admission_cpu==(admission_scope ? 4600u : 3000u));
}
