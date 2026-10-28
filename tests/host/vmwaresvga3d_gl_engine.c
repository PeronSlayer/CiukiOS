/* Existing H1 copied-wire harness on the SVGA3D host model (f7-31): real
 * GL/GDI/wire/service/backend, independently decoded FIFO. GPL-2.0-only.
 * Legacy gl_savage_* names are fixture hooks in quake3.c. */
#include "vmwaresvga3d_model.h"
#include <ciuki/kernel.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../sdk/win32/opengl32/encode.h"
#ifndef SVGA_H1_HOST
#define SVGA_H1_HOST SVGA_HOST_VMWARE
#endif
#define CHECK(x) assert(x)
static struct render3d_service scratch3d,reference3d;
#define gpu3d svga_model.device
#include "../../src/kernel/drivers/video/render3d_transport.h"
static unsigned wire_pid=7, lease_token, lease_generation, wire_presented;
static uint32_t frame_pixels[1024*768];
static unsigned mode_width=640,mode_height=480;
/* Native registry attachment is covered by vmwaresvga3d_owner_test.c. H1
 * exercises the copied Win32 transport, not a second registry. */
const struct display_driver *display_driver_bound(void) { return 0; }
const struct fb_device *fbdev_get(void) { return &svga_model.fb; }
static unsigned work_submissions,work_vertices[2],work_refusals;
static uint64_t work_peak,work_total;
void gl_kernel_work_reset(void) {
    work_submissions=work_refusals=0; work_peak=work_total=0;
    memset(work_vertices,0,sizeof(work_vertices));
}
void gl_kernel_work_check(unsigned w,unsigned h) {
    CHECK(work_submissions==2 && work_vertices[0]==12 && work_vertices[1]==3);
    CHECK(!work_refusals && work_peak==4*(uint64_t)w*h && work_total==5*(uint64_t)w*h);
}
void gl_kernel_mode(unsigned w,unsigned h) { mode_width=w; mode_height=h; }
uint64_t gl_kernel_fragments(void) { return svga_model.fragments; }
static int present_wire(void *ctx,uint32_t pid,const struct ciuki_w32_render3d_args *a,
                        const void *data,uint32_t bytes)
{
    (void)ctx;
    if (pid!=7 || a->token!=lease_token || a->generation!=lease_generation) return -EACCES;
    memcpy(frame_pixels,data,bytes); wire_presented++; return 0;
}
void gl_kernel_setup(int qualified)
{
    svga_model_init(mode_width,mode_height,SVGA_H1_HOST);
    struct render3d_host host={0,svga_model_alloc,svga_model_free,svga_model_now,0};
    if (qualified) {
        svga_model_resources(&scratch3d,64,64,true,false);
        gpu3d.consumer=&scratch3d;
        CHECK(!vmsvga3d_probe(&gpu3d)); CHECK(!render3d_close(&scratch3d));
        gpu3d.fullsize_testing=true;
        svga_model_resources(&scratch3d,mode_width,mode_height,true,true);
        svga_model_resources(&reference3d,mode_width,mode_height,false,true);
        gpu3d.fullsize_testing=false; gpu3d.consumer=&scratch3d;
        int e=vmsvga3d_fullsize_probe(&gpu3d,&reference3d);
        if (e) fprintf(stderr,"SVGA3D H1 qualification error=%d precision=%d phase=%u expected=%08x actual=%08x\n",
            e,gpu3d.precision_error,gpu3d.precision_phase,gpu3d.expected,gpu3d.actual);
        CHECK(!e && gpu3d.perspective_qualified); CHECK(!render3d_close(&scratch3d) && !render3d_close(&reference3d));
        gpu3d.consumer=0; gpu3d.qualified=true; /* Q4 for this boot */
    }
    render3d_init(&scratch3d,&host,42,vmsvga3d_backend(),&gpu3d,
        (struct render3d_extent){gpu3d.heap.first+VMSVGA3D_SCRATCH_BYTES,gpu3d.heap.bytes-VMSVGA3D_SCRATCH_BYTES});
    gpu3d.consumer=&scratch3d; wire_pid=7;
    lease_token=1234; lease_generation=42; wire_presented=0;
}
void gl_kernel_teardown(void)
{
    CHECK(!render3d_close(&scratch3d)); CHECK(!scratch3d.ram_bytes);
    CHECK(!svga_model_live()); svga_model_destroy();
}
void gl_kernel_revoke(void) { lease_token++; }
void gl_kernel_process_exit(void) { render3d_process_exit(&scratch3d,7); }
unsigned gl_kernel_draws(void) { return (unsigned)svga_model.draws; }
unsigned gl_kernel_copies(void) { return (unsigned)svga_model.copies; }
unsigned gl_kernel_writes(void) { return (unsigned)svga_model.fifo_writes; }
unsigned gl_kernel_presents(void) { return wire_presented; }
const uint32_t *gl_kernel_pixels(void) { return frame_pixels; }
int w32_render3d_host_exchange(struct ciuki_w32_render3d_args *a,void *payload)
{
    struct ciuki_w32_render3d_args copied=*a;
    void *data=copied.bytes ? calloc(1,copied.bytes) : 0;
    bool output=a->action==CIUKI_R3D_QUERY || a->action==CIUKI_R3D_READ;
    if (copied.bytes && !data) return -ENOMEM;
    if (copied.bytes && !output && a->action!=CIUKI_R3D_PRESENT) {
        CHECK(payload); memcpy(data,payload,copied.bytes);
    }
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

/* H1 measurements run after the real per-boot qualification on the model. */
static unsigned admission_live,admission_class,admission_scope;
unsigned gl_kernel_h1_scenarios(void) { return 3; }
void gl_savage_scope(unsigned s) { admission_scope=s; }
static uint64_t admission_gpu,admission_cpu,admission_world,admission_entities;
static uint64_t admission_world_ok,admission_entities_ok,admission_refused[6];
static unsigned admission_switches,admission_previous;
void gl_savage_class(unsigned which) { admission_class=which; }
void gl_savage_begin(void) {
    admission_live=1; admission_gpu=admission_cpu=admission_world=admission_entities=0;
    admission_world_ok=admission_entities_ok=admission_switches=admission_previous=0;
    memset(admission_refused,0,sizeof(admission_refused));
}
void gl_host_draw(const struct sw_state *s,const struct sw_vertex *v,int hardware) {
    if (!admission_live) return;
    static struct gl_hw_command g;
    static struct render3d_request q;
    struct ciuki_r3d_caps caps; vmsvga3d_backend()->query_caps(&gpu3d,&caps);
    g=(struct gl_hw_command){.state=*s,.count=3}; memcpy(g.vertices,v,3*sizeof(*v));
    bool eligible=!gl_hw_encode(&caps,&g,1,2,0,&q);
    if (admission_class==1) { admission_world++; admission_world_ok+=eligible; }
    if (admission_class==2) { admission_entities++; admission_entities_ok+=eligible; }
    if (!eligible) {
        unsigned reason=s->alpha_test ? 0 : s->blend && (s->sfactor==GL_DST_COLOR || s->dfactor==GL_DST_COLOR) ? 1 :
            s->fog ? 2 : s->offset ? 3 : 4;
        admission_refused[reason]++;
    } else if (!hardware) admission_refused[5]++;
    if (hardware) admission_gpu++; else admission_cpu++;
    unsigned current=hardware ? 1 : 2;
    if (admission_previous && current!=admission_previous) admission_switches++;
    admission_previous=current;
}
void gl_savage_end(unsigned w,unsigned h,unsigned frame,unsigned gpu_draws,uint64_t gpu_fragments,
                   uint64_t cpu_draws,uint64_t cpu_fragments) {
    admission_live=0;
    bool vmware=SVGA_H1_HOST==SVGA_HOST_VMWARE;
    printf("GL_VMWARESVGA_H1 host=%s scope=%s width=%u height=%u frame=%u gpu_draws=%u gpu_fragments=%llu cpu_draws=%llu cpu_fragments=%llu "
        "eligible_world=%llu/%llu eligible_entities=%llu/%llu queued_gpu=%llu direct_cpu=%llu switches=%u "
        "refused_alpha=%llu refused_blend=%llu refused_fog=%llu refused_offset=%llu refused_combine=%llu context_software=%llu "
        "batches=%llu surfaces_left=%u\n",
        vmware ? "vmware" : "vbox-gl",admission_scope==2 ? "prefix" : admission_scope ? "complete-multipass" : "complete",
        w,h,frame,gpu_draws,(unsigned long long)gpu_fragments,(unsigned long long)cpu_draws,(unsigned long long)cpu_fragments,
        (unsigned long long)admission_world_ok,(unsigned long long)admission_world,
        (unsigned long long)admission_entities_ok,(unsigned long long)admission_entities,
        (unsigned long long)admission_gpu,(unsigned long long)admission_cpu,admission_switches,
        (unsigned long long)admission_refused[0],(unsigned long long)admission_refused[1],
        (unsigned long long)admission_refused[2],(unsigned long long)admission_refused[3],
        (unsigned long long)admission_refused[4],(unsigned long long)admission_refused[5],
        (unsigned long long)svga_model.batches,svga_model_live());
    CHECK(admission_world && admission_entities);
    CHECK(admission_world_ok*100>=95*admission_world && admission_entities_ok*100>=95*admission_entities);
    CHECK(admission_switches<=1 && !svga_model_live());
    CHECK(!admission_refused[0] && !admission_refused[1] && !admission_refused[2] && !admission_refused[3]);
    if (vmware || admission_scope==2) {
        /* Every combiner (incl. the HUD's ADD) qualified: whole frames on the model. */
        unsigned expected=admission_scope==2 ? 2202u : admission_scope ? 4600u : 3000u;
        CHECK(gpu_draws==expected && gpu_fragments && !cpu_draws && !admission_switches);
        CHECK(!admission_refused[4] && !admission_refused[5]);
    } else {
        /* VirtualBox GL backend: ADD is unqualified (TEXTURE_OPS 0); one such
         * state retires the whole GL context, including buffered draws. */
        CHECK(!gpu_draws && !gpu_fragments && cpu_draws==(admission_scope ? 4600u : 3000u) && admission_refused[4]);
    }
}
