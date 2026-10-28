/* Real copied wire/validator/BCI emitter to independent fake silicon.
 * GPL-2.0-only. Fixture callbacks model only hardware and the lease boundary. */
#ifdef GL_KERNEL_FAMILY_RADEON
#include "radeon3d_model.inc"
#define family_backend radeon3d_backend
#else
#include "savage3d_backend_fixture.inc"
#define family_backend savage3d_backend
#endif
#include "../../../../src/kernel/drivers/video/render3d_transport.h"
static unsigned wire_pid=7, lease_token, lease_generation, wire_presented;
static uint32_t frame_pixels[1024*768];
static unsigned mode_width=640,mode_height=480;
static unsigned work_submissions,work_vertices[2],work_refusals;
static uint64_t work_peak,work_total;
void gl_kernel_work_reset(void) {
    work_submissions=work_refusals=0; work_peak=work_total=0;
    memset(work_vertices,0,sizeof(work_vertices));
}
void gl_kernel_work_check(unsigned w,unsigned h) {
    CHECK(work_submissions==2 && work_vertices[0]==12 && work_vertices[1]==3);
    CHECK(!work_refusals && work_peak==4*(uint64_t)w*h && work_total==5*(uint64_t)w*h);
    printf("PASS production pixel-work split: %ux%u five full-target boxes, batches=4+1, peak=%llu total=%llu, zero wire refusals\n",
        w,h,(unsigned long long)work_peak,(unsigned long long)work_total);
}
void gl_kernel_mode(unsigned w,unsigned h) { mode_width=w; mode_height=h; }
uint64_t gl_kernel_fragments(void) { return fragments3d; }
static int present_wire(void *ctx,uint32_t pid,const struct ciuki_w32_render3d_args *a,
                        const void *data,uint32_t bytes)
{
    (void)ctx;
    if (pid!=7 || a->token!=lease_token || a->generation!=lease_generation) return -EACCES;
    memcpy(frame_pixels,data,bytes); wire_presented++; return 0;
}
void gl_kernel_setup(int qualified)
{
    CHECK(!allocations3d);
#ifdef GL_KERNEL_FAMILY_RADEON
    prepare(32);
    test_engine.group_id=RADEON_R200;
    gpu3d=(struct radeon3d_device){.engine=&test_engine,.heap={0x400000,sizeof(ram3d)},.generation=42,
        .memory={0,memory_read3d,memory_write3d,lock3d,unlock3d},.qualified=qualified,.r200=true};
    test_engine.render3d_qualified=qualified;
#else
    prepare(32); m.reg[0x48578/4]=0;
    memset(ram3d,0x7a,sizeof(ram3d)); draw3d=copy3d=writes3d=memory3d=count3d=0;
    gpu3d=(struct savage3d_device){.engine=&test_engine,.heap={0x800000,sizeof(ram3d)},.generation=42,
        .memory={0,memory_read3d,memory_write3d,lock3d,unlock3d},.qualified=qualified};
    test_engine.render3d_qualified=qualified;
    test_engine.io.write32=write_engine3d;
#endif
    struct render3d_host host={0,allocate3d,free3d,clock3d,0};
    if (qualified==2) {
        gpu3d.texture_bytes=480*1024;
        m.exact_clock=true;
        gpu3d.mode_width=mode_width; gpu3d.mode_height=mode_height; gpu3d.fullsize_testing=true;
        fullsize_resources3d(&scratch3d,&host,mode_width,mode_height,true);
        fullsize_resources3d(&reference3d,&host,mode_width,mode_height,false);
        gpu3d.fullsize_testing=false; gpu3d.consumer=&scratch3d;
#ifdef GL_KERNEL_FAMILY_RADEON
        CHECK(!radeon3d_fullsize_probe(&gpu3d,&reference3d));
#else
        CHECK(!savage3d_fullsize_probe(&gpu3d,&reference3d));
#endif
        CHECK(!render3d_close(&scratch3d) && !render3d_close(&reference3d)); gpu3d.consumer=0;
    }
    render3d_init(&scratch3d,&host,42,family_backend(),&gpu3d,gpu3d.heap);
    gpu3d.consumer=&scratch3d; wire_pid=7;
    lease_token=1234; lease_generation=42; wire_presented=0;
}
void gl_kernel_teardown(void)
{
    CHECK(!render3d_close(&scratch3d)); CHECK(!allocations3d && !scratch3d.ram_bytes);
    finish(); CHECK(!failures);
}
void gl_kernel_revoke(void) { lease_token++; }
void gl_kernel_process_exit(void) { render3d_process_exit(&scratch3d,7); }
unsigned gl_kernel_draws(void) { return draw3d; }
unsigned gl_kernel_copies(void) { return copy3d; }
unsigned gl_kernel_writes(void) { return writes3d; }
unsigned gl_kernel_presents(void) { return wire_presented; }
const uint32_t *gl_kernel_pixels(void) { return frame_pixels; }
int w32_render3d_host_exchange(struct ciuki_w32_render3d_args *a,void *payload)
{
    /* Independent RAM snapshot is the syscall staging seam. No process
     * pointer reaches the service; READ/QUERY publish only on success. */
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

/* H1 measurements run after the real per-boot fake-silicon qualification. */
static unsigned admission_live,admission_class,admission_scope;
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
    struct ciuki_r3d_caps caps; family_backend()->query_caps(&gpu3d,&caps);
    g=(struct gl_hw_command){.state=*s,.count=3}; memcpy(g.vertices,v,3*sizeof(*v));
    bool eligible=!gl_hw_encode(&caps,&g,1,2,0,&q);
    if (admission_class==1) { admission_world++; admission_world_ok+=eligible; }
    if (admission_class==2) { admission_entities++; admission_entities_ok+=eligible; }
    if (!eligible) {
        unsigned reason=s->alpha_test ? 0 : s->blend && (s->sfactor==GL_DST_COLOR || s->dfactor==GL_DST_COLOR) ? 1 :
            s->fog ? 2 : s->offset ? 3 : 4;
        admission_refused[reason]++;
    } else if (!hardware) admission_refused[5]++; /* context already software */
    if (hardware) admission_gpu++; else admission_cpu++;
    unsigned current=hardware ? 1 : 2;
    if (admission_previous && current!=admission_previous) admission_switches++;
    admission_previous=current;
}
void gl_savage_end(unsigned w,unsigned h,unsigned frame,unsigned gpu_draws,uint64_t gpu_fragments,
                   uint64_t cpu_draws,uint64_t cpu_fragments) {
    admission_live=0;
    printf(
#ifdef GL_KERNEL_FAMILY_RADEON
        "GL_RADEON_H1 scope="
#else
        "GL_SAVAGE_H1 scope="
#endif
        "%s width=%u height=%u frame=%u gpu_draws=%u gpu_fragments=%llu cpu_draws=%llu cpu_fragments=%llu "
        "eligible_world=%llu/%llu eligible_entities=%llu/%llu queued_gpu=%llu direct_cpu=%llu switches=%u queued_switches=%u "
        "refused_alpha=%llu refused_blend=%llu refused_fog=%llu refused_offset=%llu refused_numeric=%llu context_software=%llu\n",
        admission_scope ? "complete-multipass" : "complete",w,h,frame,gpu_draws,(unsigned long long)gpu_fragments,(unsigned long long)cpu_draws,(unsigned long long)cpu_fragments,
        (unsigned long long)admission_world_ok,(unsigned long long)admission_world,
        (unsigned long long)admission_entities_ok,(unsigned long long)admission_entities,
        (unsigned long long)admission_gpu,(unsigned long long)admission_cpu,gpu_draws ? admission_switches : 0,admission_switches,
        (unsigned long long)admission_refused[0],(unsigned long long)admission_refused[1],
        (unsigned long long)admission_refused[2],(unsigned long long)admission_refused[3],
        (unsigned long long)admission_refused[4],(unsigned long long)admission_refused[5]);
    CHECK(admission_world && admission_entities);
    CHECK(admission_world_ok*100>=95*admission_world && admission_entities_ok*100>=95*admission_entities);
    CHECK(admission_switches<=1);
    CHECK(gpu_draws==(admission_scope ? 4600u : 3000u) && gpu_fragments && !cpu_draws && !admission_switches);
    for (unsigned i=0;i<6;i++) CHECK(!admission_refused[i]);
}
