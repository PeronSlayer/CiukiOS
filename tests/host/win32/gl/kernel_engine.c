/* Real copied wire/validator/BCI emitter to independent fake silicon.
 * GPL-2.0-only. Fixture callbacks model only hardware and the lease boundary. */
#include "savage3d_backend_fixture.inc"
#include "../../../../src/kernel/drivers/video/render3d_transport.h"
static unsigned wire_pid=7, lease_token, lease_generation, wire_presented;
static unsigned wire_reads, wire_uploads, direct_presented;
static uint32_t frame_pixels[1024*768];
static unsigned mode_width=640,mode_height=480;
static bool t23_presenting;
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
    /* f7-15 transport: NULL pixels ask for the direct native path only. */
    if (!data) {
        if (!t23_presenting) return -EOPNOTSUPP;
        CHECK(bytes==device.width*device.height*4);
        int e=render3d_scanout(&scratch3d,pid,a,&device);
        if (!e) { wire_presented++; direct_presented++; }
        return e;
    }
    if (t23_presenting) {
        CHECK(bytes==device.width*device.height*4);
        struct fb_surface source={data,device.width,device.height,device.width*4,bytes};
        struct fb_rect dest={0,0,(int32_t)device.width,(int32_t)device.height};
        int e=render3d_scanout(&scratch3d,pid,a,&device);
        if (e==-EOPNOTSUPP) e=fbdev_present(&source,&dest);
        if (e) return e;
    }
    if (data!=frame_pixels) memcpy(frame_pixels,data,bytes); wire_presented++; return 0;
}
void gl_kernel_setup(int qualified)
{
    CHECK(!allocations3d);
    prepare(32); m.exact_clock=true; m.reg[0x48578/4]=0;
    memset(ram3d,0x7a,sizeof(ram3d)); draw3d=copy3d=writes3d=memory3d=count3d=attachment_fault3d=0;
    gpu3d=(struct savage3d_device){.engine=&test_engine,.heap={0x800000,sizeof(ram3d)},.generation=42,
        .memory={0,memory_read3d,memory_write3d,lock3d,unlock3d},.qualified=qualified};
    test_engine.render3d_qualified=qualified;
    test_engine.io.write32=write_engine3d;
    struct render3d_host host={0,allocate3d,free3d,clock3d,0};
    if (qualified==2) {
        gpu3d.texture_bytes=480*1024;
        m.exact_clock=true;
        gpu3d.mode_width=mode_width; gpu3d.mode_height=mode_height; gpu3d.fullsize_testing=true;
        fullsize_resources3d(&scratch3d,&host,mode_width,mode_height,true);
        fullsize_resources3d(&reference3d,&host,mode_width,mode_height,false);
        gpu3d.fullsize_testing=false; gpu3d.consumer=&scratch3d;
        CHECK(!savage3d_fullsize_probe(&gpu3d,&reference3d));
        CHECK(!render3d_close(&scratch3d) && !render3d_close(&reference3d)); gpu3d.consumer=0;
        gpu3d.color565_testing=gpu3d.fullsize_testing=true;
        fullsize_resources3d(&scratch3d,&host,mode_width,mode_height,true);
        fullsize_resources3d(&reference3d,&host,mode_width,mode_height,false);
        gpu3d.fullsize_testing=false; gpu3d.consumer=&scratch3d;
        CHECK(!savage3d_color565_probe(&gpu3d,&reference3d));
        CHECK(!render3d_close(&scratch3d) && !render3d_close(&reference3d)); gpu3d.consumer=0;
        /* Full D24 mode must fit the same heap; do not enlarge the model. */
        unsigned padded_h=(mode_height+15)&~15u;
        unsigned buffer=(((mode_width*4+127)&~127u)*padded_h+4095)&~4095u;
        if (3*(buffer+8192)<=gpu3d.heap.bytes-gpu3d.texture_bytes) {
            gpu3d.depth24_testing=gpu3d.fullsize_testing=true;
            fullsize_resources3d(&scratch3d,&host,mode_width,mode_height,true);
            fullsize_resources3d(&reference3d,&host,mode_width,mode_height,false);
            gpu3d.fullsize_testing=false; gpu3d.consumer=&scratch3d;
            int e=savage3d_depth24_probe(&gpu3d,&reference3d);
            if (e) fprintf(stderr,"D24 probe %ux%u error=%d expected=%08x actual=%08x depth_error=%u\n",
                mode_width,mode_height,e,gpu3d.expected,gpu3d.actual,gpu3d.max_depth_error);
            CHECK(!e);
            CHECK(!render3d_close(&scratch3d) && !render3d_close(&reference3d)); gpu3d.consumer=0;
        }
    }
    render3d_init(&scratch3d,&host,42,savage3d_backend(),&gpu3d,gpu3d.heap);
    gpu3d.consumer=&scratch3d; wire_pid=7;
    lease_token=1234; lease_generation=42; wire_presented=0;
}
/* T23 census/lease fixture; cached target evidence is controlled below.
 * Minimum recheck, copied GL traffic, BCI and scanout copies are production. */
static uint8_t t23_ram[16*1024*1024];
/* Passing cached target evidence is a controlled fixture here. The render3d
 * fullsize/format suites execute the actual raster/precision gates at all
 * three dimensions; the production owner suite verifies recording and reuse.
 * H1 tests the subsequent lease, minimum check and real GL/BCI/presentation
 * path, without repeating that costly qualification matrix per replay. */
static void t23_scanout(unsigned w,unsigned h,unsigned generation)
{
    test_engine.io.write32=model_write;
    CHECK(!stop_engine(&test_engine));
    free(storage);
    device.width=w; device.height=h; device.pitch=w*4; device.size=device.pitch*h;
    device.generation=generation;
    storage=malloc(device.size+32); CHECK(storage);
    memset(storage,0xa5,device.size+32); device.mapped=storage+16;
    reservation.start=device.phys; reservation.end=device.phys+device.size; reservation.generation=generation;
    g_boot.display.vram_bytes=15532032;
    struct savage_io io=model_io();
    CHECK(!savage2d_start(&test_engine,&io,&device,true,m.cursor_page,15532032-4096));
    test_engine.io.write32=write_engine3d;
    acceleration=&test_engine.ops; m.exact_clock=true;
}
void gl_kernel_fullscreen_setup(unsigned w,unsigned h)
{
    gl_kernel_setup(0);
    CHECK(!render3d_close(&scratch3d)); gpu3d.consumer=0;
    t23_scanout(1024,768,42);
    ram_storage3d=t23_ram; memset(t23_ram,0x7a,sizeof(t23_ram));
    uint32_t required;
    CHECK(!savage3d_mode_heap(&test_engine,&device,15532032,&gpu3d.heap,&required));
    CHECK(gpu3d.heap.first==3145728 && gpu3d.heap.bytes==12382208);
    gpu3d.texture_bytes=2887680; gpu3d.qualified=true; test_engine.render3d_qualified=true;
    struct render3d_host host={0,allocate3d,free3d,clock3d,0};
    gpu3d.probe_checkpoint=0; gpu3d.probe_phase=0;
    gpu3d.color565_testing=gpu3d.depth24_testing=false;
    gpu3d.qualified=false;
    CHECK(!savage3d_backend()->qualified(&gpu3d,43));
    t23_scanout(w,h,43); gpu3d.generation=43; lease_generation=43;
    CHECK(!savage3d_mode_heap(&test_engine,&device,15532032,&gpu3d.heap,&required));
    gpu3d.fullsize_qualified=gpu3d.perspective_qualified=gpu3d.depth24_qualified=gpu3d.color565_qualified=false;
    render3d_init(&scratch3d,&host,43,0,0,(struct render3d_extent){0});
    unsigned c,rh; CHECK(!render3d_create_context(&scratch3d,1,R3D_SOFTWARE,&c));
    uint32_t tex[512]; for (unsigned i=0;i<512;i++) tex[i]=(i%32)<16 ? 0xff4080c0 : 0xffc04080;
    enum render3d_kind kinds[]={R3D_COLOR,R3D_D16,R3D_TEXTURE,R3D_LINEAR_COLOR};
    unsigned at=gpu3d.heap.first;
    for (unsigned i=0;i<4;i++) {
        CHECK(!render3d_create_resource(&scratch3d,1,c,kinds[i],i==2 ? 32 : 64,i==2 ? 16 : 64,
            i==2 ? tex : 0,i==2 ? sizeof(tex) : 0,&rh));
        struct render3d_resource *r=&scratch3d.contexts[0].resources[i]; r->hardware=true;
        r->offset=i==2 ? gpu3d.heap.first+gpu3d.heap.bytes-gpu3d.texture_bytes : at;
        CHECK(!savage3d_backend()->layout(&gpu3d,r)); if (i!=2) at+=r->reserved;
    }
    gpu3d.consumer=&scratch3d; CHECK(!savage3d_probe(&gpu3d));
    CHECK(!render3d_close(&scratch3d)); gpu3d.consumer=0;
    gpu3d.mode_width=w; gpu3d.mode_height=h;
    gpu3d.fullsize_qualified=gpu3d.perspective_qualified=gpu3d.depth24_qualified=gpu3d.color565_qualified=true;
    gpu3d.sample_delta_q16=CIUKI_R3D_SAMPLE_DELTA_Q16;
    gpu3d.filter_fraction_bits=CIUKI_R3D_FILTER_FRACTION_BITS;
    render3d_init(&scratch3d,&host,43,savage3d_backend(),&gpu3d,gpu3d.heap); gpu3d.consumer=&scratch3d;
    CHECK(savage3d_backend()->qualified(&gpu3d,43) && !savage3d_backend()->qualified(&gpu3d,42));
    t23_presenting=true;
    printf("GL_T23_LEASE desktop=1024x768 mode=%ux%u generation=42->43 heap_first=%u heap_bytes=%u retained_targets=1 minimum_recheck=1 PASS\n",
        w,h,gpu3d.heap.first,gpu3d.heap.bytes); fflush(stdout);
}
void gl_kernel_present_lease(const uint32_t *pixels,unsigned w,unsigned h)
{
    CHECK(t23_presenting && w==device.width && h==device.height);
    struct render3d_context *context=0; unsigned color=0,linear=0;
    for (unsigned i=0;i<R3D_CONTEXTS;i++) if (scratch3d.contexts[i].live && scratch3d.contexts[i].hardware) context=&scratch3d.contexts[i];
    CHECK(context && context->resources);
    for (unsigned i=0;i<R3D_RESOURCES;i++) {
        const struct render3d_resource *r=&context->resources[i];
        if (r->live && r->kind==R3D_COLOR) color=r->handle;
        if (r->live && r->kind==R3D_LINEAR_COLOR) linear=r->handle;
    }
    CHECK(color && linear);
    struct render3d_request q={.target=color,.copy_target=linear,.flags=R3D_COPY};
    struct render3d_fence fence;
    CHECK(!render3d_submit(&scratch3d,7,context->handle,&q,sizeof(q),&fence) && render3d_step(&scratch3d)==1);
    struct ciuki_w32_render3d_args a={.action=CIUKI_R3D_PRESENT,.token=lease_token,.generation=lease_generation,
        .context=context->handle,.resource=linear,.width=w,.height=h,.pitch=w*4,.bytes=w*h*4,
        .sequence_lo=(uint32_t)fence.sequence,.sequence_hi=fence.sequence>>32,
        .ordinal_lo=(uint32_t)fence.ordinal,.ordinal_hi=fence.ordinal>>32};
    unsigned before=wire_presented,front=scanout_copies3d,direct=direct_presented;
    struct render3d_transport t={&scratch3d,&scratch3d,present_wire};
    CHECK(!render3d_transport_dispatch(&t,7,&a,frame_pixels,w*h*4) && wire_presented==before+1 && scanout_copies3d==front+1);
    /* GPU-resident frame: presented directly; the linear copy is read here
     * only as the test oracle (production reads it back only for the CPU). */
    CHECK(direct_presented==direct+1);
    CHECK(!render3d_readback(&scratch3d,7,context->handle,linear,frame_pixels,w*h*4));
    a.generation--;
    CHECK(present_wire(0,7,&a,pixels,w*h*4)==-EACCES && wire_presented==before+1);
    /* glFinish leaves the hardware back buffer authoritative; the window DIB
     * is not refreshed until presentation. Compare scanout with the completed
     * owned linear copy staged by PRESENT, not that stale window surface. */
    unsigned mismatches=0;
    for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
        uint32_t got=0; memcpy(&got,device.mapped+y*device.pitch+x*4,4);
        if ((got&0xffffffu)!=(frame_pixels[y*w+x]&0xffffffu)) mismatches++;
    }
    CHECK(!mismatches);
}
/* f7-15 review P1: the real Quake III order. The context is created and
 * draws in the desktop generation; its first SwapBuffers takes the
 * full-screen lease, whose mode switch revokes that generation. Cached
 * target evidence is a controlled fixture as above; the minimum recheck,
 * wire, BCI, scanout and opengl32 recovery are production code. */
static unsigned lease_switch_w,lease_switch_h;
extern unsigned gl_test_generation,gl_test_token;
static void t23_qualify(unsigned generation,unsigned w,unsigned h)
{
    struct render3d_host host={0,allocate3d,free3d,clock3d,0};
    uint32_t required;
    CHECK(!savage3d_mode_heap(&test_engine,&device,15532032,&gpu3d.heap,&required));
    gpu3d.generation=generation; gpu3d.texture_bytes=2887680; gpu3d.qualified=false;
    gpu3d.probe_checkpoint=0; gpu3d.probe_phase=0; gpu3d.color565_testing=gpu3d.depth24_testing=false;
    gpu3d.fullsize_qualified=gpu3d.perspective_qualified=gpu3d.depth24_qualified=gpu3d.color565_qualified=false;
    render3d_init(&scratch3d,&host,generation,0,0,(struct render3d_extent){0});
    unsigned c,rh; CHECK(!render3d_create_context(&scratch3d,1,R3D_SOFTWARE,&c));
    uint32_t tex[512]; for (unsigned i=0;i<512;i++) tex[i]=(i%32)<16 ? 0xff4080c0 : 0xffc04080;
    enum render3d_kind kinds[]={R3D_COLOR,R3D_D16,R3D_TEXTURE,R3D_LINEAR_COLOR};
    unsigned at=gpu3d.heap.first;
    for (unsigned i=0;i<4;i++) {
        CHECK(!render3d_create_resource(&scratch3d,1,c,kinds[i],i==2 ? 32 : 64,i==2 ? 16 : 64,
            i==2 ? tex : 0,i==2 ? sizeof(tex) : 0,&rh));
        struct render3d_resource *r=&scratch3d.contexts[0].resources[i]; r->hardware=true;
        r->offset=i==2 ? gpu3d.heap.first+gpu3d.heap.bytes-gpu3d.texture_bytes : at;
        CHECK(!savage3d_backend()->layout(&gpu3d,r)); if (i!=2) at+=r->reserved;
    }
    gpu3d.consumer=&scratch3d; CHECK(!savage3d_probe(&gpu3d));
    CHECK(!render3d_close(&scratch3d)); gpu3d.consumer=0;
    gpu3d.mode_width=w; gpu3d.mode_height=h;
    gpu3d.fullsize_qualified=gpu3d.perspective_qualified=gpu3d.depth24_qualified=gpu3d.color565_qualified=true;
    gpu3d.sample_delta_q16=CIUKI_R3D_SAMPLE_DELTA_Q16;
    gpu3d.filter_fraction_bits=CIUKI_R3D_FILTER_FRACTION_BITS;
    render3d_init(&scratch3d,&host,generation,savage3d_backend(),&gpu3d,gpu3d.heap); gpu3d.consumer=&scratch3d;
}
void gl_kernel_desktop_setup(unsigned w,unsigned h)
{
    gl_kernel_setup(0);
    CHECK(!render3d_close(&scratch3d)); gpu3d.consumer=0;
    t23_scanout(1024,768,42);
    ram_storage3d=t23_ram; memset(t23_ram,0x7a,sizeof(t23_ram));
    t23_qualify(42,1024,768);
    lease_switch_w=w; lease_switch_h=h;
    gl_test_generation=lease_generation=42; gl_test_token=lease_token=1234;
    wire_reads=wire_uploads=direct_presented=0; t23_presenting=false;
}
void gl_test_lease_hook(void)
{
    if (!lease_switch_w) return;
    unsigned w=lease_switch_w,h=lease_switch_h; lease_switch_w=0;
    /* Owner stop under the mode change: the old service, its contexts and
     * VRAM are revoked; the new mode passes the short minimum recheck. */
    CHECK(!render3d_close(&scratch3d)); gpu3d.consumer=0;
    t23_scanout(w,h,43);
    t23_qualify(43,w,h);
    gl_test_generation=lease_generation=43; t23_presenting=true;
    printf("GL_T23_SWITCH desktop=1024x768 lease=%ux%u generation=42->43 heap_first=%u heap_bytes=%u PASS\n",
        w,h,gpu3d.heap.first,gpu3d.heap.bytes);
}
void gl_kernel_counters(unsigned out[5])
{ out[0]=wire_reads; out[1]=wire_uploads; out[2]=direct_presented; out[3]=memory3d; out[4]=gpu3d.generation; }
const uint8_t *gl_kernel_scanout(void) { return device.mapped; }
unsigned gl_kernel_scanout_pitch(void) { return device.pitch; }
void gl_kernel_teardown(void)
{
    CHECK(!render3d_close(&scratch3d)); CHECK(!allocations3d && !scratch3d.ram_bytes);
    finish(); t23_presenting=false; ram_storage3d=0; CHECK(!failures);
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
    if (!e && a->action==CIUKI_R3D_READ) wire_reads++;
    if (!e && a->action==CIUKI_R3D_UPLOAD) wire_uploads++;
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
    struct ciuki_r3d_caps caps; savage3d_backend()->query_caps(&gpu3d,&caps);
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
    printf("GL_SAVAGE_H1 scope=%s width=%u height=%u frame=%u gpu_draws=%u gpu_fragments=%llu cpu_draws=%llu cpu_fragments=%llu "
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
