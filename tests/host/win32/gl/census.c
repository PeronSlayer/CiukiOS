/* Opt-in census ABI, whole-context verdict and disabled-path accounting. MIT */
#include "../../../../sdk/win32/opengl32/internal.h"
#include "../../../../sdk/win32/opengl32/admission.h"
#include "../gdi32/support.h"
#undef NDEBUG /* TinyGL's private header disables its own assertions. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const char *setting;
static unsigned queries,rows,classes,recording;
static char summary[1024],last_class[1024];
static char last_clear[1024];
static char logical[1024];
static unsigned logical_used,next_part,last_record,physical_lines,max_physical;
unsigned gl_census_test_option(char *out,unsigned cap) {
  queries++;
  if (!setting) return 0;
  unsigned n=strlen(setting);
  if (n>=cap) return n;
  memcpy(out,setting,n+1); return n;
}
void test_gl_diagnostic(const char *line) {
  if (!recording) return;
  if (!strncmp(line,"GL_CENSUS_PART ",15)) {
    unsigned context,record,part,end; int at=0;
    unsigned n=strlen(line); assert(n<=240);
    physical_lines++; if (n>max_physical) max_physical=n;
    assert(sscanf(line,"GL_CENSUS_PART c=%u r=%u p=%u end=%u data=%n",&context,&record,&part,&end,&at)==4);
    assert(part==next_part && (!part || record==last_record) && context==0);
    assert(logical_used+strlen(line+at)<sizeof(logical));
    strcpy(logical+logical_used,line+at); logical_used+=strlen(line+at); last_record=record;
    if (end) { next_part=0; test_gl_diagnostic(logical); logical_used=0; }
    else next_part++;
    return;
  }
  if (!strncmp(line,"GL_CENSUS ",10)) {
    rows++; assert(strlen(line)<sizeof(summary)); strcpy(summary,line);
  } else if (!strncmp(line,"GL_CENSUS_CLASS ",16)) {
    classes++; assert(strlen(line)<sizeof(last_class)); strcpy(last_class,line);
  } else if (!strncmp(line,"GL_CENSUS_CLEAR ",16)) {
    assert(strlen(line)<sizeof(last_clear)); strcpy(last_clear,line);
  }
  puts(line);
}
void gl_census_tests(void) {
  struct gl_context c={0};
  long before=test_allocations;
  const char *invalid[]={0,"0","","x","10001","9999999999999"};
  for (unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
    setting=invalid[i]; gl_census_open(&c);
    assert(!c.census && test_allocations==before);
  }
  unsigned checked=queries;
  c.sw.width=c.sw.height=c.drawable_width=c.drawable_height=32;
  c.viewport[2]=c.viewport[3]=32; c.range[1]=1; c.depth_bits=16;
  c.tiny.software_state=&c;
  GLVertex point={0}; point.pc.W=point.tex_coord.W=point.tex_coord1.W=1;
  for (unsigned i=0;i<4;i++) point.color.v[i]=1;
  /* Real renderer entry, zero geometric work: disabled census must not query,
   * allocate or reach its scanning/encoding/logging functions per draw. */
  for (unsigned i=0;i<10000;i++) gl_triangle(&c.tiny,&point,&point,&point);
  assert(queries==checked && test_allocations==before && !rows && !classes);
  c.sw=(struct sw_state){.width=32,.height=32,.color_mask=~0u,
    .depth_test=1,.depth_func=GL_LESS,.depth_write=1};
  struct sw_vertex v[3]={{.x=0,.y=0,.z=1000,.qw={1048576,1048576},.color={255,255,255,255}},
    {.x=512,.y=0,.z=1000,.qw={1048576,1048576},.color={255,255,255,255}},
    {.x=0,.y=512,.z=1000,.qw={1048576,1048576},.color={255,255,255,255}}};
  unsigned reference_pixels[1024],covered=0; uint16_t reference_depth[1024];
  memset(reference_pixels,0,sizeof(reference_pixels));
  for (unsigned i=0;i<1024;i++) reference_depth[i]=65535;
  c.sw.color=reference_pixels; c.sw.depth=reference_depth;
  sw_triangle(&c.sw,v,v+1,v+2);
  for (unsigned i=0;i<1024;i++) covered+=reference_pixels[i]==~0u;
  assert(covered==496); /* Compare the census count with actual raster writes. */
  setting="2"; recording=1; gl_census_open(&c); assert(c.census);
  gl_census_draw(&c,v); c.frames++; gl_census_frame(&c); assert(!rows);
  gl_census_draw(&c,v); c.frames++; gl_census_frame(&c);
  assert(rows==1 && classes==1 && strstr(summary,"frames=2 draws=2 coverage_fragments=992 refused=0 hardware_frames=2 context_hw=1"));
  assert(strstr(last_class,"verdict=admit draws=2 coverage_fragments=992"));
  assert(strstr(last_class,"stage0=0,0,0,0,0,0,0,0,0 stage1=0,0,0,0,0,0,0,0,0"));
  assert(strstr(last_class,"depth=1,513,1 scissor=0,0,0,0,0 fog=0 offset=0 w_q20=0,0 uv_abs_q16=0"));
  c.sw.fog=1; gl_census_draw(&c,v); c.frames++; gl_census_frame(&c);
  c.sw.fog=0; gl_census_draw(&c,v); c.frames++; gl_census_frame(&c);
  assert(rows==2 && classes==3 && strstr(summary,"refused=1 hardware_frames=0 context_hw=0"));
  /* A later supported frame stays software under the context rule. */
  gl_census_draw(&c,v); c.frames++; gl_census_frame(&c);
  gl_census_close(&c);
  assert(rows==3 && classes==4 && strstr(summary,"hardware_frames=0 context_hw=0"));
  assert(test_allocations==before && !c.census);
  c.frames=0; setting="1"; gl_census_open(&c);
  unsigned pixel=~0u;
  struct sw_texture t[2]={{.wrap_s=GL_REPEAT,.wrap_t=GL_REPEAT,.min=GL_LINEAR,.mag=GL_LINEAR},
    {.wrap_s=GL_REPEAT,.wrap_t=GL_REPEAT,.min=GL_LINEAR,.mag=GL_LINEAR}};
  for (unsigned u=0;u<2;u++) {
    t[u].image[0]=(struct sw_image){.w=256u>>u,.h=256u>>u,.pixels=&pixel,.internal=GL_RGBA};
    c.sw.unit[u]=(struct sw_unit){.enabled=1,.mode=GL_MODULATE,.texture=&t[u]};
    for (unsigned i=0;i<3;i++) {
      v[i].qw[u]=524288u<<i;
      v[i].uv[u][0]=(int64_t)((128+(int)i*2)*65536)*v[i].qw[u]/1048576;
      v[i].uv[u][1]=-v[i].uv[u][0];
    }
  }
  c.sw.blend=1; c.sw.sfactor=GL_DST_COLOR; c.sw.dfactor=GL_ZERO;
  c.sw.scissor=1; c.sw.box[2]=c.sw.box[3]=32;
  c.sw.alpha_test=1; c.sw.alpha_func=GL_GEQUAL; c.sw.alpha_ref=128; c.sw.offset=-1;
  gl_census_draw(&c,v); c.frames++; gl_census_frame(&c);
  assert(strstr(last_class,"verdict=admit") && strstr(last_class,"stage0=1,256,256,9729,9729,10497,10497,8448"));
  assert(strstr(last_class,"stage1=1,128,128,9729,9729,10497,10497,8448") && strstr(last_class,"offset=-1 w_q20=524288,2097152 uv_abs_q16=8650752"));
  c.sw.sfactor=GL_SRC_ALPHA_SATURATE;
  gl_census_draw(&c,v); c.frames++; gl_census_frame(&c); assert(strstr(last_class,"verdict=blend-pair"));
  c.sw.sfactor=GL_DST_COLOR; t[0].image[0].w=512;
  gl_census_draw(&c,v); c.frames++; gl_census_frame(&c); assert(strstr(last_class,"verdict=texture-layout"));
  t[0].image[0].w=256; t[0].min=GL_LINEAR_MIPMAP_LINEAR;
  gl_census_draw(&c,v); c.frames++; gl_census_frame(&c); assert(strstr(last_class,"verdict=texture-layout"));
  t[0].min=GL_LINEAR; v[2].qw[0]=v[2].qw[1]=2097168;
  gl_census_draw(&c,v); c.frames++; gl_census_frame(&c); assert(strstr(last_class,"verdict=w-range"));
  v[2].qw[0]=v[2].qw[1]=2097152;
  c.sw.box[2]=31;
  gl_census_draw(&c,v); c.frames++; gl_census_frame(&c); assert(strstr(last_class,"verdict=scissor"));
  c.sw.box[2]=32;
  for (unsigned i=0;i<257;i++) { c.sw.offset=(int)i; gl_census_draw(&c,v); }
  c.frames++; gl_census_frame(&c);
  assert(strstr(summary,"draws=257") && strstr(summary,"overflow_draws=1 overflow_fragments=496"));
  gl_census_other(&c); c.frames++; gl_census_frame(&c);
  assert(strstr(summary,"other_calls=1"));
  c.sw.box[2]=31; gl_census_clear(&c,GL_COLOR_BUFFER_BIT);
  c.frames++; gl_census_frame(&c); assert(strstr(summary,"context_hw=0"));
  gl_census_close(&c); assert(test_allocations==before);
  /* Same production bound: five target-sized boxes form batches of 4+1,
   * despite raster coverage being less than half a box for each triangle. */
  c.sw=(struct sw_state){.width=32,.height=32,.color_mask=~0u,
    .depth_test=1,.depth_func=GL_LESS,.depth_write=1};
  for (unsigned i=0;i<3;i++) v[i]=(struct sw_vertex){.x=i==1 ? 512 : 0,
    .y=i==2 ? 512 : 0,.z=1000,.color={255,255,255,255}};
  gl_census_open(&c);
  for (unsigned i=0;i<5;i++) gl_census_draw(&c,v);
  c.frames++; gl_census_frame(&c);
  assert(strstr(summary,"draws=5 coverage_fragments=2480 refused=0 hardware_frames=1 context_hw=1"));
  assert(strstr(summary,"candidate_batches=2 candidate_pixel_work=5120 max_batch_work=4096"));
  /* Independent span fixture: ceil(1/16)*ceil(17/16)=2, not covered pixels
   * or ceil(max)-floor(min). Include both windings and translated vertices. */
  struct sw_vertex sub[3]={{.x=15,.y=15},{.x=16,.y=15},{.x=15,.y=32}};
  assert(gl_hw_triangle_work(sub)==2);
  struct sw_vertex swapped=sub[1]; sub[1]=sub[2]; sub[2]=swapped;
  assert(gl_hw_triangle_work(sub)==2);
  for (unsigned i=0;i<3;i++) { sub[i].x+=7; sub[i].y+=7; }
  assert(gl_hw_triangle_work(sub)==2);
  /* Vertex count remains an independent boundary even at zero pixel work. */
  const struct sw_vertex zero[3]={{0},{0},{0}};
  for (unsigned i=0;i<257;i++) gl_census_draw(&c,zero);
  c.frames++; gl_census_frame(&c);
  assert(strstr(summary,"candidate_batches=2 candidate_pixel_work=0 max_batch_work=0"));
  gl_census_clear(&c,GL_COLOR_BUFFER_BIT); gl_census_draw(&c,v);
  c.frames++; gl_census_frame(&c);
  assert(strstr(summary,"candidate_batches=2 candidate_pixel_work=1024 max_batch_work=1024"));
  gl_census_close(&c);
  /* Provider rejects attachments, regardless of GL_STENCIL_TEST. Check clear
   * and draw independently on fresh reference contexts. */
  for (unsigned clear=0;clear<2;clear++) {
    uint8_t stencil=0;
    c.stencil_bits=8; c.sw.stencil=&stencil; c.sw.stencil_test=0;
    gl_census_open(&c);
    if (clear) gl_census_clear(&c,GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    else gl_census_draw(&c,v);
    c.frames++; gl_census_frame(&c);
    assert(strstr(clear ? last_clear : last_class,"verdict=format"));
    assert(strstr(summary,"hardware_frames=0 context_hw=0"));
    assert(strstr(summary,"framebuffer=16,8,0"));
    assert(strstr(summary,"candidate_batches=0 candidate_pixel_work=0 max_batch_work=0"));
    gl_census_close(&c);
  }
  c.depth_bits=24; c.stencil_bits=8;
  uint32_t depth24=0; uint8_t stencil24=0;
  c.sw.depth24=&depth24; c.sw.stencil=&stencil24;
  c.sw.stencil_test=1; c.sw.stencil_func=GL_EQUAL; c.sw.stencil_ref=17;
  c.sw.stencil_mask=c.sw.stencil_write_mask=255;
  c.sw.stencil_fail=c.sw.stencil_zfail=GL_KEEP; c.sw.stencil_zpass=GL_REPLACE;
  gl_census_open(&c); gl_census_draw(&c,v); c.frames++; gl_census_frame(&c);
  assert(strstr(last_class,"verdict=admit") && strstr(summary,"framebuffer=24,8,0"));
  assert(strstr(last_class,"stencil=1,514,17,255,255,7680,7680,7681"));
  gl_census_close(&c);
  c.depth_bits=16; c.stencil_bits=0; c.sw.stencil=0; c.sw.depth24=0;
  assert(test_allocations==before);
  assert(!logical_used && !next_part && physical_lines>rows+classes && max_physical==240);
  setting=0; recording=0;
  puts("PASS census: default-off no allocation/scans/logs, N-frame histogram, exact coverage, persistent context refusal, work/vertex splits, D16/S8 draw+clear format refusal, cleanup");
  fflush(stdout);
}
