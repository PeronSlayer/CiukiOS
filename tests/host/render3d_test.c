/* Independent semantic/security fixtures for the production universal service.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/display_driver.h>
#include "../../src/kernel/drivers/video/render3d.h"
#include "../../src/kernel/drivers/video/render3d_transport.h"
#include "../../src/kernel/drivers/video/s3savage/savage3d.h"
static struct render3d_service service;
static uint64_t now;
static unsigned allocations,outstanding,fail_allocation,hardware_writes,entered,left,quarantines;
static bool qualify,invalid_layout,invalid_command,stuck;
struct ciuki_boot_info g_boot;
static struct pci_func probe_pci={.vendor=0x5333,.device=0x8c2e,.revision=1};
const struct pci_func *display_driver_owner(void) { return &probe_pci; }
uint32_t pmm_free_count(void) { return 1024; }
uint32_t pmm_total_usable(void) { return 2048; }
void klog(const char *fmt,...)
{
    char text[512]; va_list ap; va_start(ap,fmt); vsnprintf(text,sizeof(text),fmt,ap); va_end(ap);
    (void)text;
}
int savage2d_render3d_batch(struct savage2d *s,const uint32_t *words,unsigned n)
{ (void)s; (void)words; (void)n; assert(!"unqualified service emitted a batch"); return -EIO; }
int savage2d_render3d_packet(struct savage2d *s,const uint32_t *words,unsigned n)
{ (void)s; (void)words; (void)n; assert(!"unqualified service emitted a packet"); return -EIO; }
static void *alloc_mem(void *ctx,size_t bytes)
{
    (void)ctx; allocations++;
    void *p=allocations==fail_allocation ? NULL : malloc(bytes);
    if (p) outstanding++;
    return p;
}
static void free_mem(void *ctx,void *p) { (void)ctx; assert(p && outstanding); outstanding--; free(p); }
static uint64_t time_us(void *ctx) { (void)ctx; return now; }
static uint64_t row_clock(void *ctx) { (void)ctx; now+=1000; return now; }
static void cooperate(void *ctx) { (void)ctx; now+=1000; (void)render3d_step(&service); }
static struct render3d_host host={NULL,alloc_mem,free_mem,time_us,cooperate};
static bool qualified(void *ctx,uint32_t gen) { (void)ctx; return qualify && gen==17; }
static int layout(void *ctx,struct render3d_resource *r)
{ (void)ctx; if (r->width>64) return -EOPNOTSUPP; r->reserved=invalid_layout ? UINT32_MAX : (r->bytes+4095)&~4095u; return 0; }
static int validate_packet(void *ctx,const struct render3d_request *r,struct render3d_resource *const resources[R3D_REFS])
{ (void)ctx; (void)r; (void)resources; return invalid_command ? -EINVAL : 0; }
static int enter(void *ctx) { (void)ctx; entered++; return 0; }
static void leave(void *ctx) { (void)ctx; left++; }
static int execute(void *ctx,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    (void)ctx; hardware_writes++;
    struct render3d_job j={0};
    while (!render3d_scalar_slice(q,r,&j,NULL)) {}
    return 0;
}
static int idle(void *ctx) { (void)ctx; return stuck ? -EIO : 0; }
static void quarantine(void *ctx) { (void)ctx; quarantines++; }
/* Second independent family: no transport/type name occurs in the service. */
static const struct render3d_backend fake_family={.features=R3D_FEATURES,.qualified=qualified,.layout=layout,
    .validate=validate_packet,.enter=enter,.leave=leave,.execute=execute,.idle=idle,.quarantine=quarantine,.max_size=64};
static const struct render3d_backend *fake_backend(void) { return &fake_family; }
static const struct render3d_family fake_descriptor={R3D_UNQUALIFIED,1,1024,fake_backend};
static void init(const struct render3d_backend *b)
{
    assert(!outstanding);
    allocations=fail_allocation=hardware_writes=entered=left=quarantines=0; now=0;
    qualify=invalid_layout=invalid_command=stuck=false;
    if (b==&fake_family) render3d_init_family(&service,&host,17,&fake_descriptor,NULL,(struct render3d_extent){0x300000,0xc00000});
    else render3d_init(&service,&host,17,b,NULL,(struct render3d_extent){0x300000,0xc00000});
}
static uint32_t create(uint32_t p,enum render3d_policy policy)
{ uint32_t id; assert(!render3d_create_context(&service,p,policy,&id)); return id; }
static uint32_t image(uint32_t p,uint32_t c,enum render3d_kind kind,unsigned w,unsigned h,const void *pixels)
{ uint32_t id; assert(!render3d_create_resource(&service,p,c,kind,w,h,pixels,pixels ? w*h*4 : 0,&id)); return id; }
static struct render3d_request clear(uint32_t target,uint32_t color)
{ return (struct render3d_request){.flags=R3D_CLEAR_COLOR,.target=target,.clear_color=color}; }
static struct render3d_fence submit(uint32_t p,uint32_t c,const struct render3d_request *q)
{ struct render3d_fence f; assert(!render3d_submit(&service,p,c,q,sizeof(*q),&f)); return f; }
static void drain(void)
{ unsigned steps=0; while (service.queued) { assert(render3d_step(&service)>=0); assert(++steps<10000); } }
static struct render3d_vertex v(int x,int y,uint32_t z,uint32_t color,int u,int t)
{ return (struct render3d_vertex){x*65536,y*65536,z,65536,color,u*65536,t*65536}; }
static void security(void)
{
    init(&fake_family); struct render3d_caps caps; render3d_query_caps(&service,&caps);
    assert(caps.features==R3D_ALL_FEATURES && !caps.hardware_features && caps.ring_bytes==262144);
    uint32_t c=create(10,R3D_AUTO),foreign=create(11,R3D_SOFTWARE),other=create(10,R3D_SOFTWARE);
    uint32_t t=image(10,c,R3D_COLOR,16,16,NULL), d=image(10,c,R3D_D16,16,16,NULL);
    uint32_t a=image(11,foreign,R3D_COLOR,16,16,NULL), z=image(10,other,R3D_COLOR,16,16,NULL);
    struct render3d_request q=clear(t,0x12345678); struct render3d_fence f={0};
    unsigned saved=allocations;
    assert(render3d_submit(&service,11,c,&q,sizeof(q),&f)==-ENOENT);
    assert(render3d_submit(&service,10,c,&q,sizeof(q)-1,&f)==-EINVAL);
    q.target=a; assert(render3d_submit(&service,10,c,&q,sizeof(q),&f)==-EINVAL);
    q.target=z; assert(render3d_submit(&service,10,c,&q,sizeof(q),&f)==-EINVAL);
    q.target=UINT32_MAX; assert(render3d_submit(&service,10,c,&q,sizeof(q),&f)==-EINVAL);
    q.target=t; q.flags|=0x80000000; assert(render3d_submit(&service,10,c,&q,sizeof(q),&f)==-EINVAL);
    q=clear(t,0); q.count=1; assert(render3d_submit(&service,10,c,&q,sizeof(q),&f)==-EINVAL);
    q.count=3; q.vertices[0]=v(-1,0,1,0,0,0);
    assert(render3d_submit(&service,10,c,&q,sizeof(q),&f)==-EINVAL);
    q.vertices[0]=v(0,0,65536,0,0,0); assert(render3d_submit(&service,10,c,&q,sizeof(q),&f)==-EINVAL);
    q.vertices[0]=v(0,0,1,0,0,0); q.vertices[0].rhw=0;
    assert(render3d_submit(&service,10,c,&q,sizeof(q),&f)==-EINVAL);
    q=clear(t,0); q.vertices[R3D_VERTICES-1].x=1;
    assert(render3d_submit(&service,10,c,&q,sizeof(q),&f)==-EINVAL);
    q=clear(t,0); q.depth=d; assert(render3d_submit(&service,10,c,&q,sizeof(q),&f)==-EINVAL);
    assert(!hardware_writes && allocations==saved && !service.queued);
    assert(render3d_create_resource(&service,10,c,R3D_COLOR,UINT32_MAX,16,NULL,0,&z)==-EINVAL);
    uint32_t pixels[16]={0};
    assert(render3d_create_resource(&service,10,c,R3D_TEXTURE,4,4,pixels,63,&z)==-EINVAL);
    assert(render3d_create_resource(&service,10,c,R3D_TEXTURE,3,4,pixels,48,&z)==-EINVAL);
    assert(!render3d_destroy_resource(&service,10,c,t));
    assert(render3d_destroy_resource(&service,10,c,t)==-ENOENT);
    q=clear(t,0); assert(render3d_submit(&service,10,c,&q,sizeof(q),&f)==-EINVAL);
    assert(!render3d_close(&service) && !service.ram_bytes);
    puts("typed ownership, stale/forged handles, extents, enums and malformed streams PASS");
}
static void software(void)
{
    init(savage3d_backend()); uint32_t c=create(1,R3D_AUTO),id;
    assert(render3d_create_context(&service,2,R3D_HARDWARE_REQUIRED,&id)==-EOPNOTSUPP);
    uint32_t t=image(1,c,R3D_COLOR,8,8,NULL),depth=image(1,c,R3D_D16,8,8,NULL),copy=image(1,c,R3D_COLOR,8,8,NULL);
    uint32_t texels[4]={0xff808080,0xff808080,0xff808080,0xff808080};
    uint32_t texture=image(1,c,R3D_TEXTURE,2,2,texels); texels[0]=0; /* snapshot */
    struct render3d_request q={.flags=R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_TEXTURED|R3D_COPY,
        .target=t,.depth=depth,.texture=texture,.copy_target=copy,.clear_color=0xff010203,.clear_depth=65535,.count=6};
    /* Two triangles exactly tile an 8x8 square. Independent constant-color
     * result detects missing shared edges and incorrect MODULATE/nearest. */
    q.vertices[0]=v(0,0,30000,0xff804020,0,0); q.vertices[1]=v(8,0,30000,0xff804020,1,0); q.vertices[2]=v(0,8,30000,0xff804020,0,1);
    q.vertices[3]=v(8,0,30000,0xff804020,1,0); q.vertices[4]=v(8,8,30000,0xff804020,1,1); q.vertices[5]=v(0,8,30000,0xff804020,0,1);
    struct render3d_fence f=submit(1,c,&q); memset(&q,0,sizeof(q));
    uint32_t out[64]; assert(render3d_readback(&service,1,c,copy,out,sizeof(out))==-EAGAIN);
    assert(!render3d_wait(&service,1,c,f,100)); assert(!hardware_writes);
    assert(!render3d_readback(&service,1,c,copy,out,sizeof(out)));
    for (unsigned i=0;i<64;i++) assert(out[i]==0xff402010);
    /* Different colors and reversed submission order expose shared-edge
     * double coverage even when identical colors/depth would hide it. */
    struct render3d_request edgeq={.flags=R3D_CLEAR_COLOR,.target=copy,.count=6};
    edgeq.vertices[0]=v(0,0,0,0xffff0000,0,0); edgeq.vertices[1]=v(8,0,0,0xffff0000,0,0); edgeq.vertices[2]=v(0,8,0,0xffff0000,0,0);
    edgeq.vertices[3]=v(8,0,0,0xff00ff00,0,0); edgeq.vertices[4]=v(8,8,0,0xff00ff00,0,0); edgeq.vertices[5]=v(0,8,0,0xff00ff00,0,0);
    f=submit(1,c,&edgeq); assert(!render3d_wait(&service,1,c,f,100));
    uint32_t first_order[64]; assert(!render3d_readback(&service,1,c,copy,first_order,sizeof(first_order)));
    for (unsigned i=0;i<3;i++) { struct render3d_vertex tmp=edgeq.vertices[i]; edgeq.vertices[i]=edgeq.vertices[i+3]; edgeq.vertices[i+3]=tmp; }
    f=submit(1,c,&edgeq); assert(!render3d_wait(&service,1,c,f,100));
    assert(!render3d_readback(&service,1,c,copy,out,sizeof(out)) && !memcmp(out,first_order,sizeof(out)));
    for (unsigned y=0;y<8;y++) for (unsigned x=0;x<8;x++) assert(out[y*8+x]==(x+y<7 ? 0xffff0000u : 0xff00ff00u));
    /* Far triangle cannot overwrite near; equal LESS also fails. */
    q=(struct render3d_request){.flags=R3D_DEPTH_TEST|R3D_DEPTH_WRITE,.target=t,.depth=depth,.count=3};
    q.vertices[0]=v(0,0,40000,0xffffffff,0,0); q.vertices[1]=v(8,0,40000,0xffffffff,0,0); q.vertices[2]=v(0,8,40000,0xffffffff,0,0);
    f=submit(1,c,&q); assert(!render3d_wait(&service,1,c,f,100));
    assert(!render3d_readback(&service,1,c,t,out,sizeof(out))); for (unsigned i=0;i<64;i++) assert(out[i]==0xff402010);
    /* Gouraud at pixel (.5,.5): weights 3/4,1/8,1/8. */
    q=(struct render3d_request){.flags=R3D_CLEAR_COLOR,.target=t,.count=3};
    q.vertices[0]=v(0,0,0,0xffff0000,0,0); q.vertices[1]=v(4,0,0,0xff00ff00,0,0); q.vertices[2]=v(0,4,0,0xff0000ff,0,0);
    f=submit(1,c,&q); assert(!render3d_wait(&service,1,c,f,100));
    assert(!render3d_readback(&service,1,c,t,out,sizeof(out)) && out[0]==0xffbf2020 && out[63]==0);
    /* Perspective fixture: unequal W moves a sample from texel 0 to 1. */
    uint32_t split[4]={0xffff0000,0xff00ff00,0xffff0000,0xff00ff00};
    uint32_t tex2=image(1,c,R3D_TEXTURE,2,2,split);
    q=(struct render3d_request){.flags=R3D_TEXTURED|R3D_CLEAR_COLOR,.target=t,.texture=tex2,.count=3};
    q.vertices[0]=v(0,0,0,UINT32_MAX,0,0); q.vertices[1]=v(8,0,0,UINT32_MAX,1,0); q.vertices[2]=v(0,8,0,UINT32_MAX,0,0);
    q.vertices[1].rhw=4*65536;
    f=submit(1,c,&q); assert(!render3d_wait(&service,1,c,f,100));
    assert(!render3d_readback(&service,1,c,t,out,sizeof(out)) && out[2]==0xff00ff00);
    assert(!render3d_close(&service) && !service.ram_bytes);
    puts("scalar clear, shared edges, Gouraud, immutable texture, perspective MODULATE, D16 and offscreen copy PASS");
}
static void queues(void)
{
    init(NULL); uint32_t c[3],t[3];
    for (unsigned i=0;i<3;i++) { c[i]=create(i+1,R3D_SOFTWARE); t[i]=image(i+1,c[i],R3D_COLOR,32,32,NULL); }
    struct render3d_request q=clear(t[0],1), q2=clear(t[1],2);
    struct render3d_fence a=submit(1,c[0],&q), b=submit(1,c[0],&q),z;
    assert(render3d_submit(&service,1,c[0],&q,sizeof(q),&z)==-EAGAIN);
    submit(2,c[1],&q2); submit(2,c[1],&q2); assert(service.queued==4);
    q.target=t[2]; assert(render3d_submit(&service,3,c[2],&q,sizeof(q),&z)==-EAGAIN);
    /* First context's first half clear then other context makes progress;
     * its own second clear cannot overtake the first. */
    assert(render3d_step(&service)==1);
    assert(render3d_step(&service)==1);
    assert(render3d_step(&service)==1);
    assert(((uint32_t *)service.contexts[1].resources[0].shadow)[0]==2);
    assert(render3d_wait(&service,1,c[0],a,0)==-ETIMEDOUT && !service.quarantined);
    service.host.yield=NULL;
    assert(render3d_wait(&service,1,c[0],a,1000)==-ETIMEDOUT && !service.quarantined);
    service.host.yield=cooperate;
    assert(render3d_wait(&service,2,c[1],a,0)==-EINVAL);
    z=b; z.generation++; assert(render3d_wait(&service,1,c[0],z,0)==-EINVAL);
    z=b; z.sequence=UINT64_MAX; assert(render3d_wait(&service,1,c[0],z,0)==-EINVAL);
    assert(!render3d_wait(&service,1,c[0],b,100)); drain();
    /* Wrap the four slots many times and cross 16-bit event-tag range. */
    service.sequence=65534;
    for (unsigned i=0;i<16;i++) { q=clear(t[0],i); z=submit(1,c[0],&q); drain(); assert(!render3d_wait(&service,1,c[0],z,0)); }
    assert(z.sequence==65550);
    assert(!render3d_wait(&service,1,c[0],a,0)); /* receipt survives slot reuse */
    z=a; z.ordinal=UINT64_MAX; assert(render3d_wait(&service,1,c[0],z,0)==-EINVAL);
    q=clear(t[0],3); z=submit(1,c[0],&q);
    assert(!render3d_destroy_resource(&service,1,c[0],t[0]));
    assert(service.contexts[0].resources[0].live); drain(); assert(!service.contexts[0].resources[0].live);
    q=clear(t[1],3); submit(2,c[1],&q); render3d_process_exit(&service,2);
    assert(render3d_step(&service)==-ECANCELED && !service.queued);
    assert(!service.contexts[1].live);
    q=clear(t[2],7); submit(3,c[2],&q);
    service.host.now_us=row_clock;
    assert(render3d_step(&service)==1 && service.queued==1 && service.jobs[service.head].row==1);
    service.host.now_us=time_us; drain();
    assert(!render3d_close(&service) && !service.ram_bytes);
    puts("ring saturation/wrap, per-context order/fairness, timeout, deferred free and process death PASS");
}
static void hardware(void)
{
    init(&fake_family); qualify=true; struct render3d_caps caps; render3d_query_caps(&service,&caps); assert(caps.hardware_features==R3D_FEATURES);
    uint32_t c=create(1,R3D_HARDWARE_REQUIRED), t=image(1,c,R3D_COLOR,16,16,NULL), t2=image(1,c,R3D_COLOR,16,16,NULL);
    assert(service.contexts[0].resources[0].offset==0x300000 && service.contexts[0].resources[1].offset==0x301000);
    struct render3d_request q=clear(t,0xff123456); struct render3d_fence f;
    invalid_command=true; assert(render3d_submit(&service,1,c,&q,sizeof(q),&f)==-EINVAL && !hardware_writes);
    invalid_command=false; f=submit(1,c,&q); q.target=t2;
    assert(render3d_submit(&service,1,c,&q,sizeof(q),&f)==-EAGAIN && !hardware_writes);
    drain(); assert(hardware_writes==1 && entered==1 && left==1);
    assert(!render3d_wait(&service,1,c,f,0));
    invalid_layout=true; uint32_t r;
    assert(render3d_create_resource(&service,1,c,R3D_COLOR,16,16,NULL,0,&r)==-EINVAL);
    invalid_layout=false;
    assert(!render3d_close(&service) && !service.ram_bytes);
    init(&fake_family); qualify=true; c=create(1,R3D_AUTO);
    image(1,c,R3D_COLOR,16,16,NULL);
    t=image(1,c,R3D_COLOR,128,128,NULL);
    assert(!service.contexts[0].hardware && !service.contexts[0].resources[0].hardware);
    q=clear(t,0xabcdef); submit(1,c,&q); drain(); assert(!hardware_writes);
    assert(!render3d_close(&service));
    /* Quarantine retains every possibly referenced allocation, no retry. */
    init(&fake_family); qualify=true; c=create(1,R3D_AUTO); t=image(1,c,R3D_COLOR,16,16,NULL);
    q=clear(t,1); f=submit(1,c,&q); stuck=true;
    assert(render3d_step(&service)==-EIO && service.quarantined && quarantines==1);
    assert(render3d_wait(&service,1,c,f,1)==-EIO);
    assert(render3d_submit(&service,1,c,&q,sizeof(q),&f)==-EIO && hardware_writes==1);
    unsigned bytes=service.ram_bytes; render3d_process_exit(&service,1);
    assert(render3d_close(&service)==-EIO && service.ram_bytes==bytes && service.contexts[0].resources[0].references==1);
    /* Fixture teardown represents reboot AFTER fake engine destruction;
     * production has no same-boot release override. */
    free_mem(NULL,service.contexts[0].resources[0].shadow); free_mem(NULL,service.contexts[0].resources); memset(&service,0,sizeof(service));
    puts("second family qualification, VRAM reservations, no-write rejection, idle fences and quarantine retention PASS");
}
static void limits(void)
{
    init(NULL); uint32_t c=create(1,R3D_SOFTWARE), id;
    fail_allocation=allocations+1;
    assert(render3d_create_resource(&service,1,c,R3D_COLOR,32,32,NULL,0,&id)==-ENOMEM && !service.ram_bytes);
    fail_allocation=0;
    for (unsigned i=0;i<R3D_RESOURCES;i++) image(1,c,R3D_COLOR,1,1,NULL);
    assert(render3d_create_resource(&service,1,c,R3D_COLOR,1,1,NULL,0,&id)==-EAGAIN);
    create(1,R3D_SOFTWARE); create(1,R3D_SOFTWARE); create(1,R3D_SOFTWARE);
    assert(render3d_create_context(&service,1,R3D_SOFTWARE,&id)==-EAGAIN);
    for (unsigned i=4;i<R3D_CONTEXTS;i++) create(i,R3D_SOFTWARE);
    assert(render3d_create_context(&service,100,R3D_SOFTWARE,&id)==-EAGAIN);
    service.next_handle=UINT32_MAX;
    assert(render3d_destroy_context(&service,100,0)==-ENOENT);
    assert(!render3d_close(&service) && !service.ram_bytes);
    init(NULL); service.next_handle=UINT32_MAX;
    assert(render3d_create_context(&service,1,R3D_SOFTWARE,&id)==-ENOSPC);
    assert(!render3d_close(&service));
    puts("allocation failure, global/process/resource budgets and non-recycled handles PASS");
}
static void layouts(void)
{
    assert(savage3d_fixed_ieee(65536,16)==0x3f800000);
    assert(savage3d_fixed_ieee(32768,16)==0x3f000000);
    assert(savage3d_fixed_ieee(0x1000001,0)==0x4b800000); /* tie even */
    assert(savage3d_fixed_ieee(0x1000003,0)==0x4b800002); /* tie odd */
    assert(savage3d_fixed_ieee(UINT32_MAX,0)==0x4f800000);
    struct render3d_vertex tri[3]={v(1,2,32768,0xff112233,0,1),v(3,4,0,0xff445566,1,0),v(5,6,16384,0xff778899,1,1)};
    const uint32_t golden[22]={0x800300c8,0x3f800000,0x40000000,0x3f000000,0x3f800000,0xff112233,0,0x3f800000,
        0x40400000,0x40800000,0,0x3f800000,0xff445566,0x3f800000,0,
        0x40a00000,0x40c00000,0x3e800000,0x3f800000,0xff778899,0x3f800000,0x3f800000};
    uint32_t packet[22]; assert(!savage3d_triangle_packet(tri,packet) && !memcmp(packet,golden,sizeof(golden)));
    tri[2].u=-1; memset(packet,0x55,sizeof(packet)); assert(savage3d_triangle_packet(tri,packet)==-EINVAL && packet[0]==0x55555555);
    /* Literal hardware packet: Mesa sample biases and rational integer-D16
     * reversal. Constants checked independently with host IEEE binary32. */
    tri[0]=v(0,0,0,0xff112233,0,1); tri[1]=v(1,1,32768,0xff445566,1,0); tri[2]=v(64,64,65535,0xff778899,1,1);
    const uint32_t hardware_golden[22]={0x800300c8,0xbf000000,0xbec00000,0x3f800000,0x3f800000,0xff112233,0,0x3f800000,
        0x3f000000,0x3f200000,0x3effff00,0x3f800000,0xff445566,0x3f800000,0,
        0x427e0000,0x427e8000,0,0x3f800000,0xff778899,0x3f800000,0x3f800000};
    assert(!savage3d_hw_triangle_packet(tri,packet) && !memcmp(packet,hardware_golden,sizeof(hardware_golden)));
    tri[2].z=16384; assert(!savage3d_hw_triangle_packet(tri,packet) && packet[17]==0x3f3fffc0);
    unsigned w=64,h=32; uint32_t *src=malloc(w*h*4),*dst=malloc(w*h*4); assert(src && dst);
    for (unsigned i=0;i<w*h;i++) src[i]=i;
    assert(!savage3d_pack_texture(dst,w*h*4,src,w,h));
    assert(dst[0]==0 && dst[8]==64 && dst[16]==128 && dst[32]==256 && dst[512]==32 && dst[1024]==1024);
    unsigned char seen[8192]={0};
    for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
        uint32_t at; assert(!savage3d_texture_offset(w,h,x,y,&at)); assert(at<w*h*4 && !seen[at]); seen[at]=1; assert(dst[at/4]==y*w+x);
    }
    free(src); free(dst);
    struct render3d_extent reserve[3]={{0,3u*1024*1024},{16u*1024*1024-4096,4096},{15u*1024*1024+512*1024,512*1024-4096}};
    struct savage3d_plan plan;
    assert(!savage3d_plan_vram(16u*1024*1024,reserve,3,1024,768,&plan));
    assert(plan.color[0].bytes==3u*1024*1024 && plan.depth.bytes==1536u*1024 && plan.texture.bytes==4u*1024*1024);
    assert(plan.free_bytes==1024*1024 && plan.largest_bytes==1024*1024);
    assert(savage3d_plan_vram(4u*1024*1024,reserve,1,1024,768,&plan)==-ENOMEM);
    reserve[1]=reserve[0]; assert(savage3d_plan_vram(16u*1024*1024,reserve,2,1024,768,&plan)==-EINVAL);
    puts("independent IEEE/vertex packet and Pro texture fixtures; checked 16 MiB memory plan PASS");
}
static void diagnostic(void)
{
    struct savage2d engine={.chip=SAVAGE_SUPER,.rows=2048,.height=768,.cursor_offset=16*1024*1024-4096};
    struct fb_device fb={.width=1024,.height=768,.pitch=4096,.size=3145728,.bpp=32};
    struct render3d_extent heap; uint32_t required;
    assert(!savage3d_mode_heap(&engine,&fb,16*1024*1024,&heap,&required));
    assert(heap.first==8*1024*1024 && heap.bytes==8*1024*1024-4096 && required==8183808);
    engine.cursor_offset=15532032-4096;
    assert(savage3d_mode_heap(&engine,&fb,15532032,&heap,&required)==-ENOMEM);
    assert(heap.first==8388608 && heap.bytes==7139328 && required==8183808);
    engine.cursor_offset=16*1024*1024-4096;
    assert(savage3d_mode_heap(&engine,&fb,8*1024*1024,&heap,&required)==-EOPNOTSUPP);
    engine.cursor_offset=heap.first+4096;
    assert(savage3d_mode_heap(&engine,&fb,16*1024*1024,&heap,&required)==-ENOMEM);
    /* T23 census: 1280 cached rows after the 768-row desktop. Borrowing
     * them is explicit; the checked layout never silently overlaps 2D. */
    static const unsigned widths[]={1024,1024,640,640},heights[]={768,768,480,480},bpps[]={32,16,32,16};
    for (unsigned i=0;i<4;i++) {
        fb.width=widths[i]; fb.height=heights[i]; fb.bpp=bpps[i];
        fb.pitch=fb.width*(fb.bpp/8); fb.size=fb.pitch*fb.height;
        engine.height=fb.height; engine.rows=fb.height; engine.cursor_offset=15532032-4096;
        assert(!savage3d_mode_heap(&engine,&fb,15532032,&heap,&required));
        assert(heap.first==fb.size && heap.first+heap.bytes==engine.cursor_offset && heap.bytes>=required);
        printf("T23 plan %ux%ux%u scanout=%u heap_first=%u heap_bytes=%u D16_required=%u PASS\n",
            fb.width,fb.height,fb.bpp,fb.size,heap.first,heap.bytes,required);
    }
    puts("unchanged 2D lease, guarded full-mode buffer + minimum texture admission PASS");
}
static void queued_close(void)
{
    /* A full ring, two ordered jobs per context, a partial scalar slice,
     * shared job references, pending and unused resources, and a lazy context. */
    for (unsigned cycle=0;cycle<100;cycle++) {
        init(NULL);
        for (unsigned p=1;p<=2;p++) {
            uint32_t c=create(p,R3D_SOFTWARE),t=image(p,c,R3D_COLOR,32,32,NULL);
            image(p,c,R3D_D16,32,32,NULL);
            struct render3d_request q=clear(t,cycle);
            submit(p,c,&q); submit(p,c,&q);
            if (p==1) assert(!render3d_destroy_resource(&service,p,c,t));
        }
        create(3,R3D_SOFTWARE);
        assert(service.queued==R3D_JOBS && outstanding==6);
        assert(render3d_step(&service)==1 && service.queued==R3D_JOBS);
        assert(!render3d_close(&service));
        assert(!outstanding && !service.ram_bytes && !service.queued);
        for (unsigned i=0;i<R3D_CONTEXTS;i++) assert(!service.contexts[i].live);
        assert(!render3d_close(&service) && !outstanding && !service.ram_bytes);
    }
    puts("queued close: 100 full-ring/partial-work cycles, zero allocations and RAM charge PASS");
}
static void completed_receipts(void)
{
    init(NULL);
    uint32_t a=create(1,R3D_SOFTWARE),b=create(1,R3D_SOFTWARE),c=create(2,R3D_SOFTWARE);
    uint32_t ta=image(1,a,R3D_COLOR,1,1,NULL),tb=image(1,b,R3D_COLOR,1,1,NULL),tc=image(2,c,R3D_COLOR,1,1,NULL);
    struct render3d_request q=clear(ta,1);
    struct render3d_fence fa=submit(1,a,&q);
    q.target=tb; struct render3d_fence fb=submit(1,b,&q);
    q.target=tc; struct render3d_fence fc=submit(2,c,&q);
    q.target=ta; struct render3d_fence last=submit(1,a,&q),f=last;
    /* Pending sequence/ordinal pairs still require the immutable snapshot. */
    f.sequence=fb.sequence; assert(render3d_wait(&service,1,a,f,0)==-EINVAL);
    drain();
    for (unsigned i=0;i<8;i++) { q.target=tb; submit(1,b,&q); drain(); }
    /* Completed identity is ordinal-only, explicitly ignoring sequence even
     * outside the old broad bounds. Foreign sequence conveys no authority. */
    const uint64_t informational[]={0,UINT64_MAX,fb.sequence,fc.sequence};
    for (unsigned i=0;i<4;i++) {
        f=fa; f.sequence=informational[i]; assert(!render3d_wait(&service,1,a,f,0));
        f=last; f.sequence=informational[i]; assert(!render3d_wait(&service,1,a,f,0));
    }
    f=fa; f.ordinal=0; assert(render3d_wait(&service,1,a,f,0)==-EINVAL);
    f=last; f.ordinal++; assert(render3d_wait(&service,1,a,f,0)==-EINVAL);
    f=fa; f.generation++; assert(render3d_wait(&service,1,a,f,0)==-EINVAL);
    assert(render3d_wait(&service,2,a,fa,0)==-EINVAL);
    assert(render3d_wait(&service,1,b,fa,0)==-EINVAL);
    assert(!render3d_close(&service) && !outstanding && !service.ram_bytes);
    puts("completed ordinal-only receipts, pending exact pairs, owner/generation bounds and ring reuse PASS");
}
static void scalar_numeric_bounds(void)
{
    init(NULL); uint32_t c=create(1,R3D_SOFTWARE);
    uint32_t target=image(1,c,R3D_COLOR,1,1,0),depth=image(1,c,R3D_D16,1,1,0);
    uint32_t *pixels=malloc(1024*1024*4); assert(pixels);
    for (unsigned i=0;i<1024*1024;i++) pixels[i]=UINT32_MAX;
    uint32_t tex=image(1,c,R3D_TEXTURE,1024,1024,pixels); free(pixels);
    struct render3d_request q={.flags=R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH|R3D_DEPTH_WRITE|R3D_TEXTURED|R3D_TEXTURE1,
        .target=target,.depth=depth,.texture=tex,.texture1=tex,.clear_depth=65535,.count=3};
    /* A four-unit Q32 area surrounding the pixel centre forces derivative
     * saturation, signed UV and extreme reciprocal-W products. */
    q.vertices[0]=(struct render3d_vertex){32768,32767,16384,256,UINT32_MAX,-16*65536,16*65536};
    q.vertices[1]=(struct render3d_vertex){32769,32769,16384,16777216,UINT32_MAX,16*65536,-16*65536};
    q.vertices[2]=(struct render3d_vertex){32767,32769,16384,256,UINT32_MAX,-16*65536,-16*65536};
    for (unsigned i=0;i<2;i++) q.state.sampler[i]=(struct render3d_sampler){R3D_MODULATE,R3D_TRILINEAR,1,1};
    for (unsigned i=0;i<3;i++) { q.uv1[i][0]=q.vertices[i].v; q.uv1[i][1]=q.vertices[i].u; }
    (void)submit(1,c,&q); drain();
    uint32_t color; uint16_t z;
    assert(!render3d_readback(&service,1,c,target,&color,sizeof(color)) && color==UINT32_MAX);
    assert(!render3d_readback(&service,1,c,depth,&z,sizeof(z)) && z==16384);
    assert(!render3d_close(&service) && !outstanding);
    puts("scalar tiny-area derivatives, maximum texture/UV/reciprocal-W products and signed wrap PASS");
}
static void canonical_rgb565(void)
{
    init(NULL); uint32_t c=create(1,R3D_SOFTWARE),h,pixel;
    const unsigned kinds[]={R3D_COLOR565,R3D_LINEAR565};
    for (unsigned k=0;k<2;k++) {
        assert(!render3d_create_resource(&service,1,c,kinds[k],1,1,NULL,0,&h));
        assert(!render3d_readback(&service,1,c,h,&pixel,4) && pixel==0xff000000u);
        pixel=0x00123456;
        assert(!render3d_upload(&service,1,c,h,&pixel,4));
        assert(!render3d_readback(&service,1,c,h,&pixel,4) && pixel==0xff103452u);
    }
    assert(!render3d_close(&service));
    puts("RGB565 canonical shadows: creation, immediate upload/read quantization and absent alpha PASS");
}
int main(int argc,char **argv)
{
    canonical_rgb565();
    if (argc==2 && !strcmp(argv[1],"queued-close")) queued_close();
    else if (argc==2 && !strcmp(argv[1],"completed-receipts")) completed_receipts();
    else { security(); software(); queues(); hardware(); limits(); layouts(); diagnostic(); queued_close(); completed_receipts(); scalar_numeric_bounds(); }
    assert(!outstanding);
    puts("universal 3D host model PASS"); fflush(stdout); return 0;
}
