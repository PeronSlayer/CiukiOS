/* Actual render3d scalar engine: GL origin, fixed-point and strict fallback.
 * SPDX-License-Identifier: MIT */
#include "../../../../sdk/win32/opengl32/minimum.h"
#include "../../../../sdk/libgl/vendor/include/GL/gl.h"
#include "../../../../sdk/libgl/software/extensions.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct gl_hw_command command;
static struct render3d_request request;
static unsigned color[64], native[64], texels[4] = {0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
static uint16_t depth[64], native_depth[64];
static struct sw_texture texture;
static struct sw_vertex vertex(int x, int y, int z, unsigned c, int u, int t) {
  return (struct sw_vertex){.x=x*16,.y=y*16,.z=z,.iw=1048576,
    .qw={1048576,1048576},.uv={{u,t},{0,0}},
    .color={(int)(c>>16&255),(int)(c>>8&255),(int)(c&255),(int)(c>>24)},.fog=255};
}
int main(void) {
  setvbuf(stdout,0,_IONBF,0);
  texture=(struct sw_texture){.used=1,.target=GL_TEXTURE_2D,.wrap_s=GL_CLAMP_TO_EDGE,
    .wrap_t=GL_CLAMP_TO_EDGE,.min=GL_NEAREST,.mag=GL_NEAREST,
    .image={{.w=2,.h=2,.pixels=texels,.internal=GL_RGBA}}};
  command.state=(struct sw_state){.width=8,.height=8,.color=color,.depth=depth,
    .depth_test=1,.depth_write=1,.depth_func=GL_LESS,.color_mask=UINT32_MAX};
  command.count=3; command.texture[0]=3;
  struct render3d_resource target={.width=8,.height=8,.pitch=32,.shadow=native},
    z={.width=8,.height=8,.pitch=16,.shadow=native_depth},
    tex={.width=2,.height=2,.pitch=8,.shadow=texels};
  struct render3d_resource *resources[4]={&target,&z,&tex,0};
  unsigned comparisons=0;
  for (unsigned textured=0;textured<2;textured++)
    for (unsigned reverse=0;reverse<2;reverse++) {
      memset(color,0,sizeof(color)); memset(native,0,sizeof(native));
      memset(depth,255,sizeof(depth)); memset(native_depth,255,sizeof(native_depth));
      command.state.unit[0]=(struct sw_unit){.enabled=(int)textured,.mode=GL_MODULATE,.texture=&texture};
      command.vertices[0]=vertex(0,0,1000,0xffff8040,0,0);
      command.vertices[1]=vertex(8,0,1100,0xff4080ff,65536,0);
      command.vertices[2]=vertex(0,8,1200,0xff80ff40,0,65536);
      if (textured) for (unsigned i=0;i<3;i++) {
        command.vertices[i].uv[0][0]=16384;
        command.vertices[i].uv[0][1]=49152;
      }
      if (reverse) { struct sw_vertex v=command.vertices[1]; command.vertices[1]=command.vertices[2]; command.vertices[2]=v; }
      assert(!gl_hw_minimum_encode(&command,1,2,0,&request));
      assert(request.vertices[0].x==0 && request.vertices[0].y==8*65536);
      assert(request.vertices[0].z==1000 && request.vertices[0].rhw==65536);
      assert(request.depth==2 && request.target==1 && request.texture==(textured?3u:0u));
      struct render3d_job job={0};
      while (!render3d_scalar_slice(&request,resources,&job,0)) {}
      sw_triangle(&command.state,command.vertices,command.vertices+1,command.vertices+2);
      unsigned coverage=0;
      for (unsigned p=0;p<64;p++) {
        assert((depth[p]!=65535)==(native_depth[p]!=65535));
        if (depth[p]==65535) continue;
        coverage++;
        int dz=(int)depth[p]-native_depth[p]; assert(dz>=-1 && dz<=1);
        for (unsigned shift=0;shift<32;shift+=8) {
          int dc=(int)(color[p]>>shift&255)-(int)(native[p]>>shift&255);
          assert(dc>=-2 && dc<=2);
        }
        comparisons++;
      }
      assert(coverage==28);
    }
  /* A sub-Q16 interpolation error can cross an entire texel boundary. */
  command.vertices[0]=vertex(0,0,1000,0xffffffff,32767,0);
  command.vertices[1]=vertex(8,0,1000,0xffffffff,32771,0);
  command.vertices[2]=vertex(0,8,1000,0xffffffff,32771,0);
  /* Independently demonstrate the rejected native request's full-channel
   * error, rather than relying only on the encoder's admission decision. */
  request=(struct render3d_request){.target=1,.depth=2,.texture=3,.count=3,
    .flags=R3D_TEXTURED|R3D_DEPTH_TEST|R3D_DEPTH_WRITE};
  for (unsigned i=0;i<3;i++) request.vertices[i]=(struct render3d_vertex){
    .x=command.vertices[i].x*4096,.y=(128-command.vertices[i].y)*4096,
    .z=1000,.rhw=65536,.color=UINT32_MAX,.u=command.vertices[i].uv[0][0]};
  memset(color,0,sizeof(color)); memset(native,0,sizeof(native));
  memset(depth,255,sizeof(depth)); memset(native_depth,255,sizeof(native_depth));
  struct render3d_job boundary={0};
  while (!render3d_scalar_slice(&request,resources,&boundary,0)) {}
  sw_triangle(&command.state,command.vertices,command.vertices+1,command.vertices+2);
  assert(color[7*8]==0xffff0000 && native[7*8]==0xff00ff00);
  assert(gl_hw_minimum_encode(&command,1,2,0,&request));
  /* Constant coordinates survive only when native conversion is lossless. */
  for (unsigned i=0;i<3;i++) command.vertices[i].uv[0][0]=32767;
  assert(!gl_hw_minimum_encode(&command,1,2,0,&request));
  for (unsigned i=0;i<3;i++) command.vertices[i].qw[0]=1048592;
  assert(gl_hw_minimum_encode(&command,1,2,0,&request));
  for (unsigned i=0;i<3;i++) command.vertices[i].qw[0]=1048576;
  command.vertices[2].qw[0]=524288;
  assert(gl_hw_minimum_encode(&command,1,2,0,&request));
  command.vertices[2].qw[0]=1048576;
  for (unsigned i=0;i<3;i++) {
    command.vertices[i].qw[0]=524288;
    command.vertices[i].uv[0][0]=16384;
  }
  assert(!gl_hw_minimum_encode(&command,1,2,0,&request));
  command.vertices[2].uv[0][1]=1;
  assert(gl_hw_minimum_encode(&command,1,2,0,&request));
  command.vertices[2].uv[0][1]=0;
  /* Refuse every unexpressible state rather than silently omit it. */
  command.state.blend=1; assert(gl_hw_minimum_encode(&command,1,2,0,&request)); command.state.blend=0;
  command.state.depth_func=GL_LEQUAL; assert(gl_hw_minimum_encode(&command,1,2,0,&request)); command.state.depth_func=GL_LESS;
  command.state.unit[1].enabled=1; assert(gl_hw_minimum_encode(&command,1,2,0,&request)); command.state.unit[1].enabled=0;
  texture.min=GL_LINEAR_MIPMAP_LINEAR; assert(gl_hw_minimum_encode(&command,1,2,0,&request)); texture.min=GL_NEAREST;
  command.state.offset=1; assert(gl_hw_minimum_encode(&command,1,2,0,&request)); command.state.offset=0;
  command.state.unit[0].enabled=0; command.state.depth_test=0;
  assert(gl_hw_minimum_encode(&command,1,2,0,&request));
  command.count=0; command.clear_mask=GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT;
  command.clear_color=0x12345678; command.clear_depth=65535;
  assert(!gl_hw_minimum_encode(&command,1,2,4,&request));
  assert(request.flags==(R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH|R3D_COPY) && request.copy_target==4);
  printf("PASS render3d minimum: %u independent origin/coverage/channel/depth comparisons, nearest boundary rejection (255-channel error), strict state rejection, clear and offscreen copy\n",comparisons);
}
