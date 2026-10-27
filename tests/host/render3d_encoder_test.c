/* Capability mismatch must refuse before any provider submit. MIT */
#include <assert.h>
#include <stdio.h>
#include "../../sdk/win32/opengl32/encode.h"
int main(void)
{
    struct ciuki_r3d_caps caps={.features=127,.max_size=64,.max_vertices=3,
        .texture_units=2,.max_texture_size=1024,.depth_functions=255,
        .alpha_functions=255,.blend_factors=15,.combine_modes=15,.filters=31};
    struct gl_hw_command g={.state={.width=64,.height=64,.color_mask=~0u,
        .depth_test=1,.depth_write=1,.depth_func=GL_LEQUAL},.count=3};
    for (unsigned i=0;i<3;i++) {
        g.vertices[i]=(struct sw_vertex){.x=i==1?512:0,.y=i==2?512:0,.z=1000,
            .color={128,128,128,255},.qw={1048576,1048576},.uv={{16384,16384},{16384,16384}}};
    }
    struct render3d_request q;
    assert(!gl_hw_encode(&caps,&g,1,2,0,&q) && q.state.depth_func==R3D_LEQUAL);
    g.state.scissor=1; g.state.box[2]=g.state.box[3]=64;
    assert(!gl_hw_encode(&caps,&g,1,2,0,&q));
    g.state.box[2]=63; assert(gl_hw_encode(&caps,&g,1,2,0,&q)); g.state.scissor=0;
    caps.max_width=63; assert(gl_hw_encode(&caps,&g,1,2,0,&q)); caps.max_width=64;
    caps.max_height=63; assert(gl_hw_encode(&caps,&g,1,2,0,&q)); caps.max_height=64;
    caps.depth_functions&=~(1u<<R3D_LEQUAL); assert(gl_hw_encode(&caps,&g,1,2,0,&q)); caps.depth_functions=255;
    g.state.blend=1;g.state.sfactor=GL_SRC_ALPHA;g.state.dfactor=GL_ONE_MINUS_SRC_ALPHA;
    assert(!gl_hw_encode(&caps,&g,1,2,0,&q));
    caps.blend_factors=1;assert(gl_hw_encode(&caps,&g,1,2,0,&q));caps.blend_factors=15;
    g.state.dfactor=GL_DST_COLOR;assert(gl_hw_encode(&caps,&g,1,2,0,&q));g.state.blend=0;
    g.state.alpha_test=1;g.state.alpha_func=GL_GREATER;g.state.alpha_ref=128;
    assert(!gl_hw_encode(&caps,&g,1,2,0,&q));caps.alpha_functions=1;
    assert(gl_hw_encode(&caps,&g,1,2,0,&q));caps.alpha_functions=255;
    struct sw_texture tex={.wrap_s=GL_CLAMP_TO_EDGE,.wrap_t=GL_CLAMP_TO_EDGE,.min=GL_NEAREST,.mag=GL_NEAREST};
    uint32_t pixels[4]={~0u,~0u,~0u,~0u};tex.image[0]=(struct sw_image){.w=2,.h=2,.pixels=pixels,.internal=GL_RGBA};
    for (unsigned i=0;i<2;i++) g.state.unit[i]=(struct sw_unit){.enabled=1,.mode=GL_MODULATE,.texture=&tex};
    assert(!gl_hw_encode(&caps,&g,1,2,0,&q) && (q.flags&R3D_TEXTURE1));
    caps.texture_units=1;assert(gl_hw_encode(&caps,&g,1,2,0,&q));caps.texture_units=2;
    caps.filters=0;assert(gl_hw_encode(&caps,&g,1,2,0,&q));caps.filters=31;
    caps.combine_modes=0;assert(gl_hw_encode(&caps,&g,1,2,0,&q));caps.combine_modes=15;
    g.vertices[1].uv[1][0]++;assert(gl_hw_encode(&caps,&g,1,2,0,&q));g.vertices[1].uv[1][0]--;
    g.state.offset=1;assert(!gl_hw_encode(&caps,&g,1,2,0,&q) && q.vertices[0].z==1001);
    g.state.offset=-1001;assert(gl_hw_encode(&caps,&g,1,2,0,&q));g.state.offset=0;
    tex.min=GL_LINEAR_MIPMAP_LINEAR;tex.mag=GL_LINEAR;
    assert(gl_hw_encode(&caps,&g,1,2,0,&q)); /* incomplete mip chain */
    tex.image[1]=(struct sw_image){.w=1,.h=1,.pixels=pixels,.internal=GL_RGBA};
    tex.wrap_s=GL_REPEAT;assert(!gl_hw_encode(&caps,&g,1,2,0,&q));
    assert(q.state.sampler[0].filter==R3D_BILINEAR && q.state.sampler[0].repeat_u);
    caps.filters=1;assert(gl_hw_encode(&caps,&g,1,2,0,&q));caps.filters=31;
    caps.sampling_features=CIUKI_R3D_PROJECTIVE_BILINEAR;
    tex.min=tex.mag=GL_LINEAR; tex.wrap_s=tex.wrap_t=GL_CLAMP;
    assert(!gl_hw_encode(&caps,&g,1,2,0,&q));
    g.vertices[0].uv[0][0]=0; assert(gl_hw_encode(&caps,&g,1,2,0,&q)); g.vertices[0].uv[0][0]=16384;
    tex.wrap_s=tex.wrap_t=GL_REPEAT; tex.min=GL_LINEAR_MIPMAP_LINEAR;
    struct sw_vertex saved=g.vertices[2];
    g.vertices[2].x=g.vertices[1].x; g.vertices[2].y=g.vertices[1].y;
    g.vertices[1].qw[0]=g.vertices[1].qw[1]=524288;
    assert(!gl_hw_encode(&caps,&g,1,2,0,&q)); /* Degenerate draws cannot sample a mip. */
    g.vertices[2]=saved; g.vertices[1].qw[0]=g.vertices[1].qw[1]=1048576;
    g.vertices[0].x=16; g.vertices[2].x=INT32_MIN;
    assert(gl_hw_encode(&caps,&g,1,2,0,&q)); /* Reject all coordinates before mip-gradient arithmetic. */
    g.vertices[0].x=g.vertices[2].x=0;
    tex.min=GL_NEAREST;tex.mag=GL_NEAREST;
    uint32_t d24=0;g.state.depth24=&d24;assert(gl_hw_encode(&caps,&g,1,2,0,&q));
    caps.depth_formats=CIUKI_R3D_D24S8; caps.features=R3D_ALL_FEATURES;
    g.state.unit[0].enabled=g.state.unit[1].enabled=0;
    g.clear_mask=GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT; g.clear_depth=0x123457; g.clear_stencil=0xa5;
    uint8_t stencil=0; g.state.stencil=&stencil; g.state.stencil_test=1;
    g.state.stencil_func=GL_EQUAL; g.state.stencil_ref=17; g.state.stencil_mask=0x5a; g.state.stencil_write_mask=0xa5;
    g.state.stencil_fail=GL_KEEP; g.state.stencil_zfail=GL_INCR; g.state.stencil_zpass=GL_REPLACE;
    for (unsigned i=0;i<3;i++) g.vertices[i].z=0x123457;
    assert(!gl_hw_encode(&caps,&g,1,2,0,&q));
    assert(q.vertices[0].z==0x123457 && q.clear_depth==0x123457 && q.stencil.clear==0xa5 &&
      q.stencil.reference==17 && q.stencil.function==R3D_EQUAL && q.stencil.read_mask==0x5a &&
      q.stencil.write_mask==0xa5 && q.stencil.zfail==CIUKI_R3D_STENCIL_INCR &&
      q.stencil.zpass==CIUKI_R3D_STENCIL_REPLACE && (q.flags&R3D_STENCIL_TEST));
    caps.features&=~R3D_STENCIL_TEST; assert(gl_hw_encode(&caps,&g,1,2,0,&q)); caps.features=R3D_ALL_FEATURES;
    g.state.stencil_zpass=0xdead; assert(gl_hw_encode(&caps,&g,1,2,0,&q)); g.state.stencil_zpass=GL_KEEP;
    g.state.stencil_test=0; g.state.depth_test=0; g.clear_mask=0;
    assert(!gl_hw_encode(&caps,&g,1,2,0,&q) && q.depth==2 && q.state.depth_func==R3D_ALWAYS);
    g.vertices[0].z=16777216; assert(gl_hw_encode(&caps,&g,1,2,0,&q));
    puts("PASS D24/S8 encoder: exact 24-bit values, stencil state/masks, unsupported operations/caps, attachment typing without depth test");
    puts("PASS capability-driven encoder: two stages, depth/alpha/blend masks, filter/combiner mismatch, sampling boundary, D24 and offset refusal");
    return 0;
}
