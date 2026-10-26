/* Opt-in process-local GL state census; never grants hardware authority.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "encode.h"
#include "internal.h"
#define CENSUS_BINS 256
struct census_key {
  int stage[2][9]; /* enabled, width, height, min, mag, S/T wrap, env, internal */
  int blend, source, destination, alpha, alpha_func, alpha_ref;
  int depth, depth_func, depth_mask, fog, offset;
  int scissor[5];
  unsigned w_class, uv_class, reason;
  unsigned width, height;
};
struct census_bin {
  struct census_key key;
  uint64_t draws, fragments;
  unsigned w_min, w_max, uv_max;
};
struct gl_census {
  unsigned period, frames, bins, pinned, frame_bad, frame_open, partial;
  uint64_t draws, fragments, refused, hardware_frames, overflow_draws, overflow_fragments, other_calls;
  uint64_t clear_reasons[16];
  struct census_bin bin[CENSUS_BINS];
  struct gl_hw_command command;
  struct render3d_request request;
};
enum { C_ADMIT, C_FORMAT, C_SCISSOR, C_MASK, C_FOG, C_OFFSET, C_BLEND, C_TEXTURE, C_SAMPLER,
       C_STAGE, C_W, C_UV, C_MIP, C_STATE, C_PRIMITIVE };
static const char *const reasons[]={"admit","format","scissor","color-mask","fog","offset-range","blend-pair","texture-layout",
  "sampler","stage-env-or-q","w-range","uv-range","mip-gradient","state-or-coverage","non-triangle"};
static unsigned option(void) {
  char value[16]={0};
#ifdef W32_HOST_MODEL
  extern unsigned gl_census_test_option(char *,unsigned) __attribute__((weak));
  if (!gl_census_test_option || !gl_census_test_option(value,sizeof(value))) return 0;
#else
  extern void *W32API GetModuleHandleA(const char *);
  extern void *W32API GetProcAddress(void *,const char *);
  void *module=GetModuleHandleA("kernel32.dll");
  DWORD (W32API *get)(const char *,char *,DWORD)=module ? GetProcAddress(module,"GetEnvironmentVariableA") : 0;
  if (!get || get("CIUKI_GL_CENSUS",value,sizeof(value))>=sizeof(value)) return 0;
#endif
  unsigned n=0;
  for (unsigned i=0;value[i];i++) {
    if (value[i]<'0' || value[i]>'9' || n>1000) return 0;
    n=n*10+(unsigned)(value[i]-'0');
  }
  return n<=10000 ? n : 0;
}
void gl_census_open(struct gl_context *c) {
  unsigned n=option();
  if (!n) return;
  c->census=gl_alloc(sizeof(*c->census));
  if (c->census) c->census->period=n;
}
/* The reference is the fully qualified Savage profile, independent of the
 * installed adapter. The same encoder used by GDI makes the final verdict. */
static unsigned verdict(struct gl_census *c) {
  struct gl_hw_command *g=&c->command;
  const struct sw_state *s=&g->state;
  const struct ciuki_r3d_caps caps={.features=127,.max_size=1024,.max_width=1024,.max_height=1024,
    .max_vertices=R3D_VERTICES,.texture_units=2,.max_texture_size=256,.depth_functions=255,
    .alpha_functions=255,.blend_factors=1023,.combine_modes=15,.filters=31,
    .sampling_features=CIUKI_R3D_AFFINE_EXACT|CIUKI_R3D_PROJECTIVE_BILINEAR};
  if (!gl_hw_encode(&caps,g,1,2,0,&c->request)) return C_ADMIT;
  if (s->depth24 || s->rgb565 || (s->stencil_test && s->stencil)) return C_FORMAT;
  if (g->clear_mask&~(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT)) return C_STATE;
  if (!gl_hw_scissor_full(s)) return C_SCISSOR;
  if (s->color_disabled || s->color2 || s->color_mask!=~0u) return C_MASK;
  if (s->fog) return C_FOG;
  for (unsigned i=0;i<3;i++) if ((int64_t)g->vertices[i].z+s->offset<0 ||
      (int64_t)g->vertices[i].z+s->offset>65535) return C_OFFSET;
  int src=gl_r3d_blend(s->sfactor),dst=gl_r3d_blend(s->dfactor);
  if (s->blend && (src<0 || dst<0 || src==R3D_SRC_COLOR || src==R3D_INV_SRC_COLOR ||
      dst==R3D_DST_COLOR || dst==R3D_INV_DST_COLOR)) return C_BLEND;
  for (unsigned u=0;u<2;u++) if (s->unit[u].enabled) {
    const struct sw_texture *t=s->unit[u].texture;
    if (!gl_hw_texture_complete(t) || t->image[0].w>256 || t->image[0].h>256 ||
        (!gl_hw_texture_rgb(t->image[0].internal) && !gl_hw_texture_rgba(t->image[0].internal))) return C_TEXTURE;
    if (gl_r3d_combine(s->unit[u].mode)<0 || (u && !s->unit[0].enabled)) return C_STAGE;
    if ((t->wrap_s!=GL_REPEAT && t->wrap_s!=GL_CLAMP_TO_EDGE && t->wrap_s!=GL_CLAMP) ||
        (t->wrap_t!=GL_REPEAT && t->wrap_t!=GL_CLAMP_TO_EDGE && t->wrap_t!=GL_CLAMP) ||
        !gl_hw_clamp_interior(&caps,g,u) || t->mag!=GL_LINEAR ||
        (t->min!=GL_LINEAR && t->min!=GL_LINEAR_MIPMAP_NEAREST && t->min!=GL_LINEAR_MIPMAP_LINEAR)) return C_SAMPLER;
    int lo=INT32_MAX,hi=0;
    for (unsigned i=0;i<3;i++) {
      const struct sw_vertex *v=g->vertices+i;
      if (v->qw[u]<1 || v->qw[u]>268435456) return C_W;
      if (u && v->qw[u]!=v->qw[0]) return C_STAGE;
      if (v->qw[u]<lo) lo=v->qw[u];
      if (v->qw[u]>hi) hi=v->qw[u];
      for (unsigned k=0;k<2;k++) {
        int64_t a=(int64_t)v->uv[u][k]*1048576/v->qw[u],b=a-gl_hw_uv_base(g,0,u,k);
        if (a<-(int64_t)4096*65536 || a>(int64_t)4096*65536 || b < -16*65536 || b>16*65536) return C_UV;
      }
    }
    if ((int64_t)hi>4*(int64_t)lo) return C_W;
    if (!gl_hw_projective_bilinear(&caps,g,0,u)) return C_MIP;
  }
  return C_STATE;
}
static unsigned bucket(unsigned n) {
  unsigned b=0;
  while (n>1) { b++; n>>=1; }
  return b;
}
void gl_census_clear(struct gl_context *context,unsigned mask) {
  if (!mask) return;
  struct gl_census *c=context->census;
  c->command.state=context->sw; c->command.count=0;
  c->command.clear_mask=mask; c->command.clear_depth=65535;
  unsigned reason=verdict(c);
  c->clear_reasons[reason]++; c->frame_open=1;
  if (reason) c->frame_bad=1;
}
static uint64_t coverage(const struct sw_state *s,const struct sw_vertex *v) {
  int xmin=v[0].x,xmax=xmin,ymin=v[0].y,ymax=ymin;
  for (unsigned i=1;i<3;i++) {
    if (v[i].x<xmin) xmin=v[i].x;
    if (v[i].x>xmax) xmax=v[i].x;
    if (v[i].y<ymin) ymin=v[i].y;
    if (v[i].y>ymax) ymax=v[i].y;
  }
  int left=xmin<0 ? 0 : xmin/16,right=(xmax+15)/16;
  int bottom=ymin<0 ? 0 : ymin/16,top=(ymax+15)/16;
  if (right>(int)s->width) right=s->width;
  if (top>(int)s->height) top=s->height;
  int64_t area=(int64_t)(v[1].x-v[0].x)*(v[2].y-v[0].y)-(int64_t)(v[1].y-v[0].y)*(v[2].x-v[0].x);
  uint64_t n=0;
  for (int y=bottom;y<top;y++) for (int x=left;x<right;x++) {
    bool inside=area!=0;
    for (unsigned i=0;i<3;i++) {
      const struct sw_vertex *a=v+(i+1)%3,*b=v+(i+2)%3;
      if (area<0) { const struct sw_vertex *t=a; a=b; b=t; }
      int64_t w=(int64_t)(b->x-a->x)*(y*16+8-a->y)-(int64_t)(b->y-a->y)*(x*16+8-a->x);
      /* Bottom-left GL convention, equivalent to the target's top-left rule. */
      if (w<0 || (!w && !(b->y<a->y || (b->y==a->y && b->x<a->x)))) inside=false;
    }
    n+=inside;
  }
  return n;
}
void gl_census_draw(struct gl_context *context,const struct sw_vertex *v) {
  struct gl_census *c=context->census;
  struct census_key key={0};
  const struct sw_state *s=&context->sw;
  c->command.state=*s; c->command.count=3; c->command.clear_mask=0;
  memcpy(c->command.vertices,v,3*sizeof(*v));
  key.reason=verdict(c); key.blend=s->blend; key.source=s->sfactor; key.destination=s->dfactor;
  key.width=s->width; key.height=s->height;
  key.alpha=s->alpha_test; key.alpha_func=s->alpha_func; key.alpha_ref=s->alpha_ref;
  key.depth=s->depth_test; key.depth_func=s->depth_func; key.depth_mask=s->depth_write;
  key.fog=s->fog; key.offset=s->offset;
  key.scissor[0]=s->scissor;
  for (unsigned i=0;i<4;i++) key.scissor[i+1]=s->box[i];
  unsigned wmin=UINT32_MAX,wmax=0,uvmax=0;
  for (unsigned u=0;u<2;u++) if (s->unit[u].enabled) {
    const struct sw_texture *t=s->unit[u].texture;
    int *k=key.stage[u]; k[0]=1; k[7]=s->unit[u].mode;
    if (t) { k[1]=t->image[0].w; k[2]=t->image[0].h; k[3]=t->min; k[4]=t->mag; k[5]=t->wrap_s; k[6]=t->wrap_t; k[8]=t->image[0].internal; }
    for (unsigned i=0;i<3;i++) {
      unsigned w=v[i].qw[u]>0 ? (unsigned)v[i].qw[u] : 0;
      if (w<wmin) wmin=w;
      if (w>wmax) wmax=w;
      for (unsigned a=0;a<2;a++) if (w) {
        int64_t q=(int64_t)v[i].uv[u][a]*1048576/(int64_t)w;
        uint64_t mag=q<0 ? (uint64_t)-q : (uint64_t)q;
        unsigned m=mag>UINT32_MAX ? UINT32_MAX : (unsigned)mag;
        if (m>uvmax) uvmax=m;
      }
    }
  }
  if (wmin==UINT32_MAX) wmin=0;
  key.w_class=bucket(wmin)*32+bucket(wmax); key.uv_class=bucket(uvmax);
  uint64_t fragments=coverage(s,v);
  c->draws++; c->fragments+=fragments; c->frame_open=1;
  if (key.reason) { c->refused++; c->frame_bad=1; }
  unsigned index=0;
  while (index<c->bins && memcmp(&key,&c->bin[index].key,sizeof(key))) index++;
  if (index==CENSUS_BINS) { c->overflow_draws++; c->overflow_fragments+=fragments; return; }
  struct census_bin *b=&c->bin[index];
  if (index==c->bins) { c->bins++; b->key=key; b->w_min=wmin; }
  b->draws++; b->fragments+=fragments;
  if (wmin<b->w_min) b->w_min=wmin;
  if (wmax>b->w_max) b->w_max=wmax;
  if (uvmax>b->uv_max) b->uv_max=uvmax;
}
/* Bounded decimal log builder, also usable by the freestanding PE recipe. */
struct census_line { char data[768]; unsigned used; };
static void word(struct census_line *l,const char *s) {
  while (*s && l->used+1<sizeof(l->data)) l->data[l->used++]=*s++;
  l->data[l->used]=0;
}
static void number(struct census_line *l,uint64_t n) {
  char text[24]; unsigned used=0;
  do { text[used++]=(char)('0'+n%10); n/=10; } while (n);
  while (used) { char s[2]={text[--used],0}; word(l,s); }
}
static void field(struct census_line *l,const char *s,uint64_t n) { word(l,s); number(l,n); }
static void integer(struct census_line *l,int n) {
  if (n<0) word(l,"-");
  number(l,n<0 ? (uint64_t)-(int64_t)n : (unsigned)n);
}
static void debug_write(const char *line,unsigned bytes) {
#ifdef W32_HOST_MODEL
  const struct w32_services *s=w32_get_services();
  if (s->diagnostic) s->diagnostic(s->opaque,"info","opengl32.dll",line);
  (void)bytes;
#else
  /* Same bounded user debug sink as WinMM; W32_DIAGNOSTIC intentionally
   * filters informational text. Never change that protected boundary. */
  int result;
  __asm__ volatile("int $0x80" : "=a"(result) : "a"(CIUKI_SYS_DEBUG_WRITE),
    "b"((uintptr_t)line),"c"(bytes) : "memory","cc");
  (void)result;
#endif
}
static void emit(const struct gl_context *context,struct census_line *l) {
  static unsigned serial;
  unsigned record=++serial,offset=0,index=0;
  do {
    struct census_line part={0}; word(&part,"GL_CENSUS_PART");
    field(&part," c=",context->handle); field(&part," r=",record); field(&part," p=",index++);
    word(&part," end="); unsigned end_at=part.used; word(&part,"0 data=");
    unsigned n=l->used-offset,cap=CIUKI_DEBUG_WRITE_MAX-part.used;
    if (n>cap) n=cap;
    part.data[end_at]=offset+n==l->used ? '1' : '0';
    memcpy(part.data+part.used,l->data+offset,n); part.used+=n; part.data[part.used]=0;
    debug_write(part.data,part.used); offset+=n;
  } while (offset<l->used);
}
static void flush(struct gl_context *context) {
  struct gl_census *c=context->census;
  struct census_line l={0}; word(&l,"GL_CENSUS version=1 reference=savage-qualified");
  field(&l," frame=",context->frames+c->partial); field(&l," frames=",c->frames); field(&l," draws=",c->draws);
  field(&l," coverage_fragments=",c->fragments); field(&l," refused=",c->refused);
  field(&l," hardware_frames=",c->hardware_frames); field(&l," context_hw=",!c->pinned);
  field(&l," other_calls=",c->other_calls);
  field(&l," width=",context->sw.width); field(&l," height=",context->sw.height);
  field(&l," partial=",c->partial);
  field(&l," overflow_draws=",c->overflow_draws); field(&l," overflow_fragments=",c->overflow_fragments); emit(context,&l);
  for (unsigned i=0;i<sizeof(reasons)/sizeof(reasons[0]);i++) if (c->clear_reasons[i]) {
    l=(struct census_line){0}; word(&l,"GL_CENSUS_CLEAR verdict="); word(&l,reasons[i]);
    field(&l," calls=",c->clear_reasons[i]); emit(context,&l);
  }
  for (unsigned i=0;i<c->bins;i++) {
    const struct census_bin *b=c->bin+i; const struct census_key *k=&b->key;
    l=(struct census_line){0}; word(&l,"GL_CENSUS_CLASS verdict="); word(&l,reasons[k->reason]);
    field(&l," draws=",b->draws); field(&l," coverage_fragments=",b->fragments);
    field(&l," size=",k->width); field(&l,",",k->height);
    for (unsigned u=0;u<2;u++) {
      word(&l,u ? " stage1=" : " stage0=");
      for (unsigned j=0;j<9;j++) { if (j) word(&l,","); number(&l,(unsigned)k->stage[u][j]); }
    }
    field(&l," blend=",k->blend); field(&l,",",(unsigned)k->source); field(&l,",",(unsigned)k->destination);
    field(&l," alpha=",k->alpha); field(&l,",",(unsigned)k->alpha_func); field(&l,",",(unsigned)k->alpha_ref);
    field(&l," depth=",k->depth); field(&l,",",(unsigned)k->depth_func); field(&l,",",(unsigned)k->depth_mask);
    word(&l," scissor=");
    for (unsigned j=0;j<5;j++) { if (j) word(&l,","); integer(&l,k->scissor[j]); }
    field(&l," fog=",k->fog); word(&l," offset="); integer(&l,k->offset);
    field(&l," w_q20=",b->w_min); field(&l,",",b->w_max); field(&l," uv_abs_q16=",b->uv_max); emit(context,&l);
  }
  c->frames=c->bins=0; c->draws=c->fragments=c->refused=c->hardware_frames=0;
  c->overflow_draws=c->overflow_fragments=0; memset(c->bin,0,sizeof(c->bin));
  c->other_calls=0;
  c->partial=0;
  memset(c->clear_reasons,0,sizeof(c->clear_reasons));
}
void gl_census_frame(struct gl_context *context) {
  struct gl_census *c=context->census;
  c->pinned|=c->frame_bad; c->hardware_frames+=!c->pinned;
  c->frame_bad=c->frame_open=0;
  if (++c->frames==c->period) flush(context);
}
void gl_census_other(struct gl_context *context) {
  /* Non-triangle operations cannot be qualified by a triangle state census. */
  context->census->frame_bad=1; context->census->frame_open=1;
  context->census->other_calls++;
}
void gl_census_close(struct gl_context *context) {
  if (!context->census) return;
  if (context->census->frame_open) { context->census->partial=1; gl_census_frame(context); }
  if (context->census->frames) flush(context);
  gl_dealloc(context->census); context->census=0;
}
