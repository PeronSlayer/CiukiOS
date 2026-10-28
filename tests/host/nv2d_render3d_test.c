/* f7-26: real family descriptor/service, fake existing 2D I/O boundary.
 * No fake 3D rasterizer or silicon-qualification claim. All 3D is software.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main nv2d_lifecycle_main
#include "nv2d_adapter_test.c"
#undef main
#include <stdlib.h>
#include "../../src/kernel/drivers/video/render3d_transport.h"

static struct render3d_service renderer;
static unsigned live_allocations, software_triangles;
static void *allocate_render(void *ctx,size_t bytes)
{
    (void)ctx;
    void *p=calloc(1,bytes);
    if (p) live_allocations++;
    return p;
}
static void free_render(void *ctx,void *p)
{ (void)ctx; assert(p && live_allocations); live_allocations--; free(p); }
static uint64_t render_clock(void *ctx) { (void)ctx; return 0; }
static const struct render3d_host render_host={NULL,allocate_render,free_render,render_clock,NULL};

/* These formerly weak hooks must remain unreachable even if linked.
 * This catches accidental resurrection of the incomplete boot scaffold. */
const struct render3d_backend *nv3d_backend(void) { abort(); }
void nv3d_boot_diagnostic(struct nv2d *s,const struct fb_device *f)
{ (void)s; (void)f; abort(); }
void nv3d_bind_vfs(struct vfs *v) { (void)v; abort(); }
int nv3d_owner_stop(struct nv2d *s) { (void)s; abort(); }

static void software_caps(void)
{
    const struct render3d_family *family=nv2d_driver.render3d;
    assert(family && family->status==R3D_SOFTWARE_ONLY && !family->backend);
    /* Even a supplied aligned extent is not an acceleration capability. */
    render3d_init_family(&renderer,&render_host,9,family,&engine,
                        (struct render3d_extent){0x400000,0x100000});
    assert(!renderer.backend && renderer.admission);
    struct render3d_caps caps;
    render3d_query_caps(&renderer,&caps);
    assert(caps.features==R3D_ALL_FEATURES && caps.texture_units==2);
    assert(!caps.hardware_features && !caps.hardware_max_size && !caps.texture_heap_bytes);
    struct ciuki_r3d_caps hardware,zero={0};
    memset(&hardware,0xa5,sizeof(hardware));
    render3d_backend_caps(&renderer,&hardware);
    assert(!memcmp(&hardware,&zero,sizeof(zero)));
    uint32_t id=0xdeadbeef;
    assert(render3d_create_context(&renderer,7,R3D_HARDWARE_REQUIRED,&id)==-EOPNOTSUPP);
    assert(id==0xdeadbeef && !live_allocations);
    for (unsigned policy=R3D_AUTO;policy<=R3D_SOFTWARE;policy++) {
        assert(!render3d_create_context(&renderer,7,policy,&id));
        assert(!renderer.contexts[0].hardware);
        assert(!render3d_destroy_context(&renderer,7,id));
    }
    assert(!render3d_close(&renderer) && !renderer.ram_bytes && !live_allocations);
    /* The production native attachment and Win32 query must expose the
     * same refusal; research metadata may not become a positive GL cap. */
    assert(!render3d_native_init(&renderer,&render_host));
    assert(!renderer.backend && !renderer.heap_first && !renderer.heap_end);
    struct render3d_transport transport={.service=&renderer};
    struct ciuki_w32_render3d_args args={.action=CIUKI_R3D_QUERY,
        .bytes=sizeof(hardware),.width=640,.height=480,
        .format=CIUKI_R3D_ARGB8888,.kind=CIUKI_R3D_D16};
    assert(!render3d_transport_dispatch(&transport,7,&args,&hardware,sizeof(hardware)));
    zero.version=CIUKI_R3D_VERSION; zero.generation=fb.generation;
    assert(!memcmp(&hardware,&zero,sizeof(zero)));
    args=(struct ciuki_w32_render3d_args){.action=CIUKI_R3D_OPEN,.generation=fb.generation};
    assert(render3d_transport_dispatch(&transport,7,&args,NULL,0)==-EOPNOTSUPP);
    assert(!render3d_close(&renderer) && !renderer.ram_bytes && !live_allocations);
}

static void all_candidates(void)
{
    for (unsigned i=0;i<nv2d_driver.match_count;i++) {
        device_baseline(nv2d_driver.matches[i].device);
        software_caps();
        strcpy(g_boot.options,"3d=probe");
        software_caps();
        g_boot.flags=CBI_F_SAFE_MODE;
        software_caps();
        g_boot.flags=0;
        strcpy(g_boot.options,"accel=0 3d=probe");
        software_caps();
        assert(!claims && !maps && !file_ops && !m.reads && !m.traced);
    }
    puts("PASS 168 family matches: AUTO/SOFTWARE, safe mode, accel=0 and 3d=probe; zero hardware caps, allocations, MMIO or markers; hardware-required refused");
}

static uint32_t resource_render(uint32_t context,enum render3d_kind kind)
{
    uint32_t id;
    assert(!render3d_create_resource(&renderer,7,context,kind,8,8,NULL,0,&id));
    return id;
}
static uint32_t texture_render(uint32_t context,uint32_t *pixels,unsigned size)
{
    struct render3d_texture_desc t={.format=R3D_ARGB8888,.width=size,.height=size};
    unsigned offset=0;
    for (unsigned w=size;w;w/=2) {
        t.image[t.levels++]=(struct render3d_texture_image){pixels+offset,w*w*4,w*4};
        offset+=w*w;
    }
    uint32_t id;
    assert(!render3d_create_texture(&renderer,7,context,&t,&id));
    return id;
}
static uint32_t memory_hash(const void *ptr,size_t size)
{
    const uint8_t *p=ptr;
    uint32_t hash=2166136261u;
    for (size_t i=0;i<size;i++) hash=(hash^p[i])*16777619u;
    return hash;
}
static void software_corpus(uint16_t device)
{
    device_baseline(device);
    allow_maps=true;
    assert(!attempt_begin(&filesystem));
    const struct accel_ops *ops=start(&fb,&filesystem);
    assert(ops && !nv2d_compare(&engine));
    assert(!attempt_end(&filesystem));
    /* A positively qualified 2D engine is still software 3D. */
    assert(engine.active && !engine.quarantined);
    strcpy(g_boot.options,"3d=probe");
    unsigned before_reads=m.reads,before_writes=m.traced,before_methods=m.methods;
    unsigned before_claims=claims,before_maps=maps,before_files=file_ops;
    uint32_t before_regs=memory_hash(m.regs,sizeof(m.regs));
    uint32_t before_pixels=memory_hash(m.pixels,sizeof(m.pixels));
    uint32_t before_tail=memory_hash(m.tail,sizeof(m.tail));
    record(); /* No 3D diagnostic may run on the active owner. */
    software_caps();
    render3d_init_family(&renderer,&render_host,9,nv2d_driver.render3d,&engine,
                        (struct render3d_extent){0,0});
    uint32_t context;
    assert(!render3d_create_context(&renderer,7,R3D_AUTO,&context));
    uint32_t color=resource_render(context,R3D_COLOR),depth=resource_render(context,R3D_D16);
    uint32_t copy=resource_render(context,R3D_LINEAR_COLOR);
    uint32_t texels[21],second[5];
    for (unsigned i=0;i<21;i++) texels[i]=0x80804020u+i*0x010203u;
    for (unsigned i=0;i<5;i++) second[i]=0xc0208040u+i*0x030201u;
    uint32_t texture=texture_render(context,texels,4),texture1=texture_render(context,second,2);
    uint32_t reference_color[64]={0},reference_copy[64]={0};
    uint16_t reference_depth[64]={0};
    struct render3d_resource reference[R3D_REFS]={
        {.kind=R3D_COLOR,.width=8,.height=8,.pitch=32,.bytes=256,.shadow=reference_color},
        {.kind=R3D_D16,.width=8,.height=8,.pitch=16,.bytes=128,.shadow=reference_depth},
        {.kind=R3D_TEXTURE,.width=4,.height=4,.pitch=16,.bytes=84,.shadow=texels,
         .format=R3D_ARGB8888,.levels=3,.level_offset={0,64,80}},
        {.kind=R3D_LINEAR_COLOR,.width=8,.height=8,.pitch=32,.bytes=256,.shadow=reference_copy},
        {.kind=R3D_TEXTURE,.width=2,.height=2,.pitch=8,.bytes=20,.shadow=second,
         .format=R3D_ARGB8888,.levels=2,.level_offset={0,16}}};
    static struct render3d_request q,saved;
    unsigned cases=0;
    for (unsigned src=0;src<=R3D_INV_DST_ALPHA;src++)
      for (unsigned dst=0;dst<=R3D_INV_DST_ALPHA;dst++) {
        if (src==R3D_SRC_COLOR || src==R3D_INV_SRC_COLOR ||
            dst==R3D_DST_COLOR || dst==R3D_INV_DST_COLOR) continue;
        unsigned n=cases++;
        q=(struct render3d_request){.flags=R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_COPY,
            .target=color,.depth=depth,.copy_target=copy,.count=3,.clear_color=0x60402010,.clear_depth=32768};
        if (n%3) { q.flags|=R3D_TEXTURED; q.texture=texture; }
        if (n%3==2) { q.flags|=R3D_TEXTURE1; q.texture1=texture1; }
        q.state.depth_func=n%8; q.state.alpha_test=n%2; q.state.alpha_func=(n/2)%8;
        q.state.alpha_ref=120; q.state.blend=1; q.state.source=src; q.state.destination=dst;
        q.state.flat=n%2; q.flat_color[0]=0xa060c080;
        for (unsigned stage=0;stage<2;stage++) {
            q.state.sampler[stage].filter=(n+stage)%5;
            q.state.sampler[stage].combine=(n/4+stage)%4;
            q.state.sampler[stage].repeat_u=n%2; q.state.sampler[stage].repeat_v=(n/2)%2;
        }
        q.vertices[0]=(struct render3d_vertex){0,0,12000,65536,0x8040c080,-65536,0};
        q.vertices[1]=(struct render3d_vertex){8*65536,0,35000,n%2 ? 32768u : 65536u,0xc08040c0,65536,0};
        q.vertices[2]=(struct render3d_vertex){0,8*65536,24000,n%2 ? 131072u : 65536u,0xffc08040,0,65536};
        q.uv1[0][0]=-65536; q.uv1[1][0]=65536; q.uv1[2][1]=65536;
        saved=q;
        struct render3d_fence fence;
        assert(!render3d_submit(&renderer,7,context,&q,sizeof(q),&fence));
        memset(&q,0,sizeof(q)); /* Submission must be an immutable snapshot. */
        unsigned steps=0;
        while (renderer.queued) { assert(render3d_step(&renderer)>=0); assert(++steps<100); }
        assert(!render3d_wait(&renderer,7,context,fence,1));
        struct render3d_resource *refs[R3D_REFS]={reference,reference+1,
            saved.flags&R3D_TEXTURED ? reference+2 : NULL,reference+3,
            saved.flags&R3D_TEXTURE1 ? reference+4 : NULL};
        struct render3d_job job={0};
        while (!render3d_scalar_slice(&saved,refs,&job,NULL)) {}
        uint32_t output[64]; uint16_t output_depth[64];
        assert(!render3d_readback(&renderer,7,context,copy,output,sizeof(output)));
        assert(!memcmp(output,reference_copy,sizeof(output)));
        assert(!render3d_readback(&renderer,7,context,depth,output_depth,sizeof(output_depth)));
        assert(!memcmp(output_depth,reference_depth,sizeof(output_depth)));
        assert(output[63]==0x60402010); /* Known untouched pixel, independent of oracle. */
        software_triangles++;
    }
    assert(cases==64);
    /* Malformed copied requests must refuse before queuing or touching I/O.
     * This fuzzes the public typed boundary; there is no NVIDIA packet API. */
    for (unsigned i=0;i<256;i++) {
        q=saved;
        switch (i%16) {
        case 0: q.flags|=0x80000000u; break;
        case 1: q.count=R3D_VERTICES+3; break;
        case 2: q.state.sampler[0].filter=5+i; break;
        case 3: q.state.depth_func=8+i; break;
        case 4: q.target=0; break;
        case 5: q.texture1=UINT32_MAX; break;
        case 6: q.vertices[0].x=-1; break;
        case 7: q.vertices[0].z=65536+i; break;
        case 8: q.uv1[0][1]=17*65536; break;
        case 9: q.vertices[0].rhw=0; break;
        case 10: q.state.flat=2+i; break;
        case 11: q.state.source=R3D_SRC_COLOR; break;
        case 12: q.state.alpha_ref=256+i; break;
        case 13: q.vertices[R3D_VERTICES-1].x=1; break;
        case 14: q.copy_target=color; break;
        case 15: q.count=1; break;
        }
        struct render3d_fence fence;
        unsigned ram=renderer.ram_bytes,live=live_allocations;
        assert(render3d_submit(&renderer,7,context,&q,sizeof(q),&fence)==-EINVAL);
        assert(!renderer.queued && renderer.ram_bytes==ram && live_allocations==live);
    }
    assert(!render3d_close(&renderer) && !renderer.ram_bytes && !live_allocations);
    assert(m.reads==before_reads && m.traced==before_writes && m.methods==before_methods);
    assert(claims==before_claims && maps==before_maps && file_ops==before_files);
    assert(memory_hash(m.regs,sizeof(m.regs))==before_regs);
    assert(memory_hash(m.pixels,sizeof(m.pixels))==before_pixels);
    assert(memory_hash(m.tail,sizeof(m.tail))==before_tail);
    assert(engine.active && !engine.quarantined && !engine.ops.stop(&engine));
    printf("PASS %04x active 2D: %u scalar/service cases + 256 malformed requests, gpu_triangles=0 cpu_triangles=%u; unchanged MMIO/VRAM/claims/markers, clean stop\n",
           device,cases,cases);
}
int main(void)
{
    all_candidates();
    const uint16_t devices[]={0x0020,0x0110,0x0170,0x0200,0x0250};
    for (unsigned i=0;i<ARRAY_SIZE(devices);i++) software_corpus(devices[i]);
    assert(software_triangles==320 && !live_allocations);
    return 0;
}
