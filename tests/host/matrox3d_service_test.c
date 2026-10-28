/* Real service with the register interpreter: fences, pool eviction and
 * unqualified/native fallback. SPDX-License-Identifier: GPL-2.0-only */
#define main matrox_core_fixture_main
#include "matrox3d_test.c"
#undef main
static const struct render3d_family family={.status=R3D_UNQUALIFIED,.backend=matrox3d_backend};
static const struct display_driver driver={.render3d=&family};
const struct display_driver *display_driver_bound(void) { return &driver; }
const struct fb_device *fbdev_get(void)
{ static const struct fb_device fb={.present=true,.generation=42}; return &fb; }
static void drain_jobs(void)
{ for (unsigned i=0;hardware.queued && i<10000;i++) assert(render3d_step(&hardware)>0); assert(!hardware.queued); }
int main(void)
{
    prepare(64,64,false);
    assert(!render3d_close(&hardware) && !render3d_close(&reference) && !allocations);
    device.fullsize_testing=false; device.fullsize_qualified=true;
    device.texture_bytes=MATROX3D_TEXTURE_MIN;
    render3d_init(&hardware,&host,42,matrox3d_backend(),&device,device.heap);
    uint32_t context,target,depth,copy,texture[6];
    assert(!render3d_create_context(&hardware,1,R3D_HARDWARE_REQUIRED,&context));
    assert(!render3d_create_resource(&hardware,1,context,R3D_COLOR,64,64,0,0,&target));
    assert(!render3d_create_resource(&hardware,1,context,R3D_D16,64,64,0,0,&depth));
    assert(!render3d_create_resource(&hardware,1,context,R3D_LINEAR_COLOR,64,64,0,0,&copy));
    uint32_t *pixels=malloc(128*128*4); assert(pixels);
    for (unsigned t=0;t<6;t++) {
        for (unsigned p=0;p<128*128;p++) pixels[p]=0xff204080u+t*0x10101u;
        assert(!render3d_create_resource(&hardware,1,context,R3D_TEXTURE,128,128,pixels,128*128*4,&texture[t]));
    }
    free(pixels);
    uint64_t last=0;
    for (unsigned t=0;t<12;t++) {
        scene(); request.flags=R3D_FEATURES; request.target=target; request.depth=depth;
        request.copy_target=copy; request.texture=texture[t%6];
        request.state.flat=1; request.flat_color[0]=UINT32_MAX;
        struct render3d_fence fence;
        unsigned writes=model.writes;
        assert(!render3d_submit(&hardware,1,context,&request,sizeof(request),&fence));
        assert(model.writes==writes && fence.sequence>last); last=fence.sequence;
        assert(render3d_wait(&hardware,1,context,fence,0)==-ETIMEDOUT);
        drain_jobs(); assert(!render3d_wait(&hardware,1,context,fence,0));
        struct render3d_fence stale=fence; stale.generation++;
        assert(render3d_wait(&hardware,1,context,stale,0)==-EINVAL);
        struct render3d_resource *r=hardware.contexts[0].resources;
        assert(((uint32_t *)r[0].shadow)[16*64+16]==0xff204080u+(t%6)*0x10101u);
        assert(!memcmp(r[0].shadow,r[2].shadow,64*64*4));
        unsigned resident=0;
        for (unsigned i=3;i<9;i++) resident+=r[i].offset!=0;
        assert(resident<=3); /* six immutable textures, only three pool slots */
        assert(!r[3+(t+1)%6].references && !engine.quarantined);
    }
    assert(!render3d_close(&hardware) && !allocations && !model.locks);
    puts("Matrox service: 12 idle fences, stale generation refusal, six textures evict/reupload through three slots, copy equality PASS");

    device.qualified=false;
    render3d_init(&hardware,&host,42,matrox3d_backend(),&device,device.heap);
    struct ciuki_r3d_caps caps; render3d_backend_caps(&hardware,&caps); assert(!caps.features);
    assert(render3d_create_context(&hardware,1,R3D_HARDWARE_REQUIRED,&context)==-EOPNOTSUPP);
    assert(!render3d_create_context(&hardware,1,R3D_AUTO,&context));
    assert(!hardware.contexts[0].hardware);
    assert(!render3d_create_resource(&hardware,1,context,R3D_COLOR,16,16,0,0,&target));
    memset(&request,0,sizeof(request)); request.target=target; request.flags=R3D_CLEAR_COLOR;
    request.clear_color=0xff123456;
    struct render3d_fence fence; unsigned writes=model.writes;
    assert(!render3d_submit(&hardware,1,context,&request,sizeof(request),&fence)); drain_jobs();
    assert(!render3d_wait(&hardware,1,context,fence,0) && writes==model.writes);
    assert(((uint32_t *)hardware.contexts[0].resources[0].shadow)[0]==request.clear_color);
    assert(!render3d_close(&hardware) && !allocations);
    /* Missing owner attachment is also a native software snapshot, with no
     * family programming hidden in the generic adapter. */
    assert(!render3d_native_init(&hardware,&host));
    render3d_backend_caps(&hardware,&caps); assert(!caps.features && writes==model.writes);
    assert(!render3d_close(&hardware));
    free(model.ram); model.ram=0;
    puts("Matrox unqualified AUTO/native fallback, zero MMIO writes and hardware-required refusal PASS");
    return 0;
}
