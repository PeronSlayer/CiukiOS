/* Shared capability-driven encoder used by the GDI provider. LGPL-2.1-or-later */
#ifndef W32_GL_ENCODE_H
#define W32_GL_ENCODE_H
#include "minimum.h"
#include "../../libgl/vendor/include/GL/gl.h"
#include "../../libgl/software/extensions.h"
static int gl_r3d_compare(int f) {
  switch (f) {
  case GL_LESS: return R3D_LESS; case GL_NEVER: return R3D_NEVER;
  case GL_ALWAYS: return R3D_ALWAYS; case GL_EQUAL: return R3D_EQUAL;
  case GL_LEQUAL: return R3D_LEQUAL; case GL_GREATER: return R3D_GREATER;
  case GL_NOTEQUAL: return R3D_NOTEQUAL; case GL_GEQUAL: return R3D_GEQUAL;
  default: return -1;
  }
}
static int gl_r3d_blend(int f) {
  switch (f) {
  case GL_ONE: return R3D_ONE; case GL_ZERO: return R3D_ZERO;
  case GL_SRC_ALPHA: return R3D_SRC_ALPHA; case GL_ONE_MINUS_SRC_ALPHA: return R3D_INV_SRC_ALPHA;
  case GL_DST_COLOR: return R3D_DST_COLOR; case GL_SRC_COLOR: return R3D_SRC_COLOR;
  case GL_ONE_MINUS_DST_COLOR: return R3D_INV_DST_COLOR;
  case GL_ONE_MINUS_SRC_COLOR: return R3D_INV_SRC_COLOR;
  /* GDI's XRGB formats have no destination-alpha attachment (AlphaBits=0). */
  case GL_DST_ALPHA: return R3D_ONE; case GL_ONE_MINUS_DST_ALPHA: return R3D_ZERO;
  default: return -1;
  }
}
static int gl_r3d_combine(int f) {
  switch (f) {
  case GL_MODULATE: return R3D_MODULATE; case GL_REPLACE: return R3D_REPLACE;
  case GL_ADD: return R3D_ADD; case GL_DECAL: return R3D_DECAL;
  default: return -1;
  }
}
static inline int gl_hw_texture_rgb(unsigned internal) {
  return internal==3 || internal==GL_RGB || internal==0x84ed || internal==0x83f0 ||
    (internal>=0x804f && internal<=0x8054);
}
static inline int gl_hw_texture_rgba(unsigned internal) {
  return internal==4 || internal==GL_RGBA || internal==0x84ee ||
    (internal>=0x83f1 && internal<=0x83f3) || (internal>=0x8055 && internal<=0x805b);
}
static int gl_hw_texture_complete(const struct sw_texture *t) {
  if (!t || !t->image[0].pixels) return 0;
  if (!t->image[0].w || !t->image[0].h || (t->image[0].w&(t->image[0].w-1)) ||
      (t->image[0].h&(t->image[0].h-1))) return 0;
  if (t->min==GL_NEAREST || t->min==GL_LINEAR) return 1;
  unsigned w=t->image[0].w,h=t->image[0].h;
  for (unsigned i=0;i<SW_LEVELS;i++) {
    const struct sw_image *m=t->image+i;
    if (!m->pixels || m->w!=w || m->h!=h || m->internal!=t->image[0].internal) return 0;
    if (w==1 && h==1) return 1;
    w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
  }
  return 0;
}
/* Preserve the previously qualified exact-affine subset when the new
 * sample-space probe refuses perspective precision. */
static int gl_hw_affine_exact(const struct ciuki_r3d_caps *caps,
    const struct gl_hw_command *g,unsigned first,unsigned unit) {
  const struct sw_texture *t=g->state.unit[unit].texture;
  if (!(caps->sampling_features&(CIUKI_R3D_AFFINE_EXACT|CIUKI_R3D_PROJECTIVE_BILINEAR)) ||
      t->min!=GL_LINEAR || t->mag!=GL_LINEAR || t->image[0].w>64 || t->image[0].h>64 ||
      g->state.alpha_test || g->state.blend) return 0;
  int64_t x[3],y[3];
  for (unsigned i=0;i<3;i++) {
    const struct sw_vertex *v=&g->vertices[first+i];
    if ((v->x&15) || (v->y&15) || v->qw[unit]<524288 || v->qw[unit]>2097152 ||
        v->qw[unit]!=g->vertices[first].qw[unit]) return 0;
    for (unsigned axis=0;axis<2;axis++) {
      int64_t uv=(int64_t)v->uv[unit][axis]*1048576/v->qw[unit];
      if (uv < -65536 || uv>65536) return 0;
    }
    x[i]=v->x/16; y[i]=v->y/16;
  }
  int64_t area=(x[1]-x[0])*(y[2]-y[0])-(y[1]-y[0])*(x[2]-x[0]);
  int64_t magnitude=area<0 ? -area : area;
  if (magnitude<256 || magnitude>32768 || (magnitude&(magnitude-1))) return 0;
  for (unsigned axis=0;axis<2;axis++) {
    int64_t a=g->vertices[first].uv[unit][axis];
    int64_t b=g->vertices[first+1].uv[unit][axis]-a;
    int64_t c=g->vertices[first+2].uv[unit][axis]-a;
    int64_t dx=b*(y[2]-y[0])-c*(y[1]-y[0]);
    int64_t dy=c*(x[1]-x[0])-b*(x[2]-x[0]);
    if (dx%area || dy%area) return 0;
    dx/=area; dy/=area;
    int64_t qw=g->vertices[first].qw[unit];
    if ((dx+dy)%2 || (dx*1048576)%qw || (dy*1048576)%qw ||
        ((2*a+dx+dy)*1048576)%(2*qw)) return 0;
  }
  return 1;
}
/* A common integer REPEAT translation preserves the rational quotient. */
static int64_t gl_hw_uv_base(const struct gl_hw_command *g,unsigned first,unsigned unit,unsigned axis) {
  const struct sw_texture *t=g->state.unit[unit].texture;
  if ((axis ? t->wrap_t : t->wrap_s)!=GL_REPEAT) return 0;
  int64_t lo=INT64_MAX,hi=INT64_MIN;
  for (unsigned i=0;i<3;i++) {
    const struct sw_vertex *v=&g->vertices[first+i];
    if (v->qw[unit]<=0) return 0;
    int64_t a=(int64_t)v->uv[unit][axis]*1048576/v->qw[unit];
    if (a<lo) lo=a;
    if (a>hi) hi=a;
  }
  int64_t middle=(lo+hi)/2;
  return ((middle+(middle<0 ? -32768 : 32768))/65536)*65536;
}
/* After REPEAT rebasing, |UV|<=16; normalize shared reciprocal W to
 * [64,256] at ratio <=4. Q16 input recovery costs <0.005 texel at 256.
 * The remaining delta and filter/LOD widths must pass the silicon probe. */
static int gl_hw_projective_bilinear(const struct ciuki_r3d_caps *caps,
    const struct gl_hw_command *g,unsigned first,unsigned unit) {
  if (gl_hw_affine_exact(caps,g,first,unit)) return 1;
  const struct sw_texture *t=g->state.unit[unit].texture;
  if (!(caps->sampling_features&CIUKI_R3D_PROJECTIVE_BILINEAR) ||
      t->mag!=GL_LINEAR || t->image[0].w>256 || t->image[0].h>256 ||
      (t->min!=GL_LINEAR && t->min!=GL_LINEAR_MIPMAP_NEAREST &&
       t->min!=GL_LINEAR_MIPMAP_LINEAR)) return 0;
  int lo=INT32_MAX,hi=0;
  for (unsigned i=0;i<3;i++) {
    const struct sw_vertex *v=&g->vertices[first+i];
    if (v->x<0 || v->y<0 || (unsigned)v->x>g->state.width*16 ||
        (unsigned)v->y>g->state.height*16 || v->qw[unit]<1 || v->qw[unit]>268435456) return 0;
    if (v->qw[unit]<lo) lo=v->qw[unit];
    if (v->qw[unit]>hi) hi=v->qw[unit];
    for (unsigned axis=0;axis<2;axis++) {
      int64_t a=(int64_t)v->uv[unit][axis]*1048576/v->qw[unit];
      int64_t local=a-gl_hw_uv_base(g,first,unit,axis);
      if (a < -(int64_t)4096*65536 || a>(int64_t)4096*65536 ||
          local < -16*65536 || local>16*65536) return 0;
    }
  }
  if ((int64_t)hi>4*(int64_t)lo) return 0;
  if (t->min!=GL_LINEAR) {
    const struct sw_vertex *a=&g->vertices[first],*b=a+1,*c=a+2;
    int64_t dx=b->x-a->x,dy=b->y-a->y,ex=c->x-a->x,ey=c->y-a->y;
    int64_t area=dx*ey-dy*ex;
    if (!area) return (int64_t)hi<=4*(int64_t)lo; /* No fragments or LOD samples. */
    int64_t gx=(int64_t)(b->qw[unit]-a->qw[unit])*ey-(int64_t)(c->qw[unit]-a->qw[unit])*dy;
    int64_t gy=(int64_t)(c->qw[unit]-a->qw[unit])*dx-(int64_t)(b->qw[unit]-a->qw[unit])*ex;
    if (area<0) area=-area;
    if (gx<0) gx=-gx;
    if (gy<0) gy=-gy;
    /* Bound one-pixel reciprocal-W change. Finite-neighbour hardware LOD
     * versus exact quotient derivatives then differs by <1/16 octave. */
    if ((gx+gy)*16*32>area*lo) return 0;
    /* G=sum(abs(d lambda/dx)+abs(d lambda/dy)), R=ceil(Wmax/Wmin),
     * U=ceil(max(abs(local UV))). epsW<=2^-23 and Q16 UV recovery give
     * derivative error <= size*(33+2*U)/2^21*(R*G+1/32) texels/pixel.
     * Require <=1/256: log2(32/31)+log2(257/256)+2/256 <1/16. */
    int64_t perimeter=0;
    for (unsigned i=0;i<3;i++) {
      int64_t x=g->vertices[first+(i+1)%3].x-g->vertices[first+i].x;
      int64_t y=g->vertices[first+(i+1)%3].y-g->vertices[first+i].y;
      perimeter+=(x<0 ? -x : x)+(y<0 ? -y : y);
    }
    unsigned size=t->image[0].w>t->image[0].h ? t->image[0].w : t->image[0].h;
    unsigned magnitude=0,ratio=(unsigned)(((int64_t)hi+lo-1)/lo);
    for (unsigned i=0;i<3;i++) for (unsigned axis=0;axis<2;axis++) {
      const struct sw_vertex *v=&g->vertices[first+i];
      int64_t uv=(int64_t)v->uv[unit][axis]*1048576/v->qw[unit]-gl_hw_uv_base(g,first,unit,axis);
      unsigned n=(unsigned)(((uv<0 ? -uv : uv)+65535)/65536);
      if (n>magnitude) magnitude=n;
    }
    if ((int64_t)size*(33+2*magnitude)*(512*ratio*perimeter+area)>262144*area) return 0;
  }
  return 1;
}
static inline int gl_hw_scissor_full(const struct sw_state *s) {
  return !s->scissor || (s->box[0]<=0 && s->box[1]<=0 &&
    (int64_t)s->box[0]+s->box[2]>=s->width && (int64_t)s->box[1]+s->box[3]>=s->height);
}
static inline int gl_hw_clamp_interior(const struct ciuki_r3d_caps *caps,const struct gl_hw_command *g,unsigned unit) {
  const struct sw_texture *t=g->state.unit[unit].texture;
  unsigned margin=caps->sampling_features&CIUKI_R3D_PROJECTIVE_BILINEAR ? CIUKI_R3D_SAMPLE_DELTA_Q16/2 : 0;
  for (unsigned axis=0;axis<2;axis++) if ((axis ? t->wrap_t : t->wrap_s)==GL_CLAMP) {
    if (t->min!=GL_LINEAR || t->mag!=GL_LINEAR) return 0;
    unsigned size=axis ? t->image[0].h : t->image[0].w;
    for (unsigned i=0;i<g->count;i++) {
      const struct sw_vertex *v=g->vertices+i;
      if (v->qw[unit]<=0) return 0;
      int64_t uv=(int64_t)v->uv[unit][axis]*1048576/v->qw[unit];
      int64_t texel=uv*size;
      if (texel<32768-margin || texel>(int64_t)size*65536-32768+margin) return 0;
    }
  }
  return 1;
}
static int gl_hw_encode(const struct ciuki_r3d_caps *caps, const struct gl_hw_command *g, unsigned target,
    unsigned depth, unsigned copy, struct render3d_request *out) {
  const struct sw_state *s = &g->state;
  *out = (struct render3d_request){0};
  if (!s->width || !s->height || s->width > R3D_MAX_SIZE || s->height > R3D_MAX_SIZE ||
      g->count > caps->max_vertices || g->count>R3D_VERTICES || g->count % 3 || s->depth24 || s->rgb565 ||
      s->color_disabled || s->color2 || s->color_mask != UINT32_MAX || !gl_hw_scissor_full(s) ||
      (s->stencil_test && s->stencil) || s->fog ||
      (g->clear_mask & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT))) return -1;
  out->target = target;
  if (g->clear_mask & GL_COLOR_BUFFER_BIT) out->flags |= R3D_CLEAR_COLOR;
  if ((g->clear_mask & GL_DEPTH_BUFFER_BIT) && s->depth_write) out->flags |= R3D_CLEAR_DEPTH;
  if (g->count && s->depth_test) {
    out->flags |= R3D_DEPTH_TEST;
    if (s->depth_write) out->flags |= R3D_DEPTH_WRITE;
  }
  if (out->flags & (R3D_CLEAR_DEPTH | R3D_DEPTH_TEST | R3D_DEPTH_WRITE)) out->depth = depth;
  out->clear_color = g->clear_color;
  if (g->clear_depth > 65535) return -1;
  out->clear_depth = g->clear_depth;
  if (!caps->features || s->width>caps->max_size || s->height>caps->max_size ||
      (caps->max_width && s->width>caps->max_width) ||
      (caps->max_height && s->height>caps->max_height)) return -1;
  if (s->depth_test) {
    int f=gl_r3d_compare(s->depth_func);
    if (f<0 || !(caps->depth_functions & (1u<<f))) return -1;
    out->state.depth_func=(unsigned)f;
  }
  if (s->alpha_test) {
    int f=gl_r3d_compare(s->alpha_func);
    if (f<0 || s->alpha_ref<0 || s->alpha_ref>255 || !(caps->alpha_functions & (1u<<f))) return -1;
    out->state.alpha_test=1; out->state.alpha_func=(unsigned)f; out->state.alpha_ref=s->alpha_ref;
  }
  if (s->blend) {
    int a=gl_r3d_blend(s->sfactor),b=gl_r3d_blend(s->dfactor);
    if (a<0 || b<0 || a==R3D_SRC_COLOR || a==R3D_INV_SRC_COLOR ||
        b==R3D_DST_COLOR || b==R3D_INV_DST_COLOR ||
        !(caps->blend_factors & (1u<<a)) || !(caps->blend_factors & (1u<<b))) return -1;
    out->state.blend=1; out->state.source=a; out->state.destination=b;
  }
  for (unsigned u=0;u<2;u++) if (g->count && s->unit[u].enabled) {
    const struct sw_texture *t=s->unit[u].texture;
    int mode=gl_r3d_combine(s->unit[u].mode);
    if (!gl_hw_texture_complete(t)) return -1;
    unsigned internal=t->image[0].internal;
    bool rgb=gl_hw_texture_rgb(internal),rgba=gl_hw_texture_rgba(internal);
    /* RGB REPLACE leaves incoming alpha unchanged. With the upload's
     * canonical opaque texture alpha, DECAL expresses precisely that rule. */
    if (rgb && mode==R3D_REPLACE) mode=R3D_DECAL;
    unsigned filter=t->mag==GL_LINEAR ? R3D_BILINEAR : R3D_NEAREST;
    if (u>=caps->texture_units || (u && !s->unit[0].enabled) || mode<0 ||
        !(caps->combine_modes & (1u<<mode)) || !(caps->filters & (1u<<filter)) ||
        !t->image[0].pixels || (!rgb && !rgba) ||
        (t->wrap_s!=GL_CLAMP_TO_EDGE && t->wrap_s!=GL_REPEAT && t->wrap_s!=GL_CLAMP) ||
        (t->wrap_t!=GL_CLAMP_TO_EDGE && t->wrap_t!=GL_REPEAT && t->wrap_t!=GL_CLAMP) ||
        !gl_hw_clamp_interior(caps,g,u) ||
        (t->mag!=GL_NEAREST && t->mag!=GL_LINEAR) ||
        (t->min!=GL_NEAREST && t->min!=GL_LINEAR && t->min!=GL_NEAREST_MIPMAP_NEAREST &&
         t->min!=GL_NEAREST_MIPMAP_LINEAR && t->min!=GL_LINEAR_MIPMAP_NEAREST && t->min!=GL_LINEAR_MIPMAP_LINEAR) ||
        t->image[0].w>caps->max_texture_size || t->image[0].h>caps->max_texture_size) return -1;
    out->flags|=u ? R3D_TEXTURE1 : R3D_TEXTURED;
    if (u) out->texture1=g->texture[u]; else out->texture=g->texture[u];
    out->state.sampler[u].combine=mode;
    /* Constant sampling selects magnification even with a mipmapped MIN
     * filter. Qualified varying sampling preserves linear minification,
     * including mip selection; no potentially used mip state is discarded. */
    out->state.sampler[u].filter=filter;
    bool varying=false;
    for (unsigned i=0;i<g->count;i++) {
      const struct sw_vertex *v=&g->vertices[i],*a=&g->vertices[i-i%3];
      if (v->qw[u]!=a->qw[u] || v->uv[u][0]!=a->uv[u][0] || v->uv[u][1]!=a->uv[u][1]) varying=true;
    }
    if (varying && (t->min==GL_LINEAR_MIPMAP_NEAREST || t->min==GL_LINEAR_MIPMAP_LINEAR))
      out->state.sampler[u].filter=t->min==GL_LINEAR_MIPMAP_LINEAR ? R3D_TRILINEAR : R3D_BILINEAR_MIP;
    if (!(caps->filters&(1u<<out->state.sampler[u].filter))) return -1;
    out->state.sampler[u].repeat_u=t->wrap_s==GL_REPEAT;
    out->state.sampler[u].repeat_v=t->wrap_t==GL_REPEAT;
  }
  if (copy) { out->flags |= R3D_COPY; out->copy_target = copy; }
  if (g->count && !(out->flags & (R3D_TEXTURED|R3D_DEPTH_TEST))) {
    if (!(caps->depth_functions & (1u<<R3D_ALWAYS))) return -1;
    out->flags|=R3D_DEPTH_TEST; out->depth=depth; out->state.depth_func=R3D_ALWAYS;
  }
  if (!out->flags) return -1;
  for (unsigned i = 0; i < g->count; i++) {
    const struct sw_vertex *v = &g->vertices[i];
    struct render3d_vertex *r = &out->vertices[i];
    if (v->x < 0 || v->y < 0 || (unsigned)v->x > s->width * 16 ||
        (unsigned)v->y > s->height * 16 || v->z < 0 || v->z > 65535) return -1;
    r->x = v->x * 4096;
    r->y = ((int)s->height * 16 - v->y) * 4096;
    int64_t z=(int64_t)v->z+s->offset;
    /* Clamping vertices would change the depth plane. */
    if (z<0 || z>65535) return -1;
    r->z = (unsigned)z; r->rhw = 65536;
    for (unsigned k = 0; k < 4; k++) if (v->color[k] < 0 || v->color[k] > 255) return -1;
    r->color = (unsigned)v->color[3] << 24 | (unsigned)v->color[0] << 16 |
               (unsigned)v->color[1] << 8 | (unsigned)v->color[2];
    for (unsigned unit=0;unit<2;unit++) if (out->flags & (unit ? R3D_TEXTURE1 : R3D_TEXTURED)) {
      bool precise=gl_hw_projective_bilinear(caps,g,i-i%3,unit);
      if (v->qw[unit] < (precise ? 1 : 4096) || v->qw[unit] > 268435456) return -1;
      if (!precise && (v->qw[unit]&15)) return -1;
      /* Even a sub-Q16 difference can choose a different nearest texel.
       * Only the qualified smooth-filter subset relaxes constant sampling. */
      const struct sw_vertex *first = &g->vertices[i - i % 3];
      if ((v->qw[unit] != first->qw[unit] || v->uv[unit][0] != first->uv[unit][0] ||
          v->uv[unit][1] != first->uv[unit][1]) &&
          !precise) return -1;
      int64_t uq = (int64_t)v->uv[unit][0] * 1048576;
      int64_t tq = (int64_t)v->uv[unit][1] * 1048576;
      if (!precise && (uq % v->qw[unit] || tq % v->qw[unit])) return -1;
      int64_t u = uq / v->qw[unit], t = tq / v->qw[unit];
      if (precise && (caps->sampling_features&CIUKI_R3D_PROJECTIVE_BILINEAR)) {
        u-=gl_hw_uv_base(g,i-i%3,unit,0); t-=gl_hw_uv_base(g,i-i%3,unit,1);
      }
      if (u < -16*65536 || u > 16*65536 || t < -16*65536 || t > 16*65536) return -1;
      if (unit) { out->uv1[i][0]=(int32_t)u; out->uv1[i][1]=(int32_t)t; }
      else { r->u=(int32_t)u; r->v=(int32_t)t; }
      unsigned rw=((unsigned)v->qw[unit]+(precise ? 8 : 0))/16;
      if (precise && (caps->sampling_features&CIUKI_R3D_PROJECTIVE_BILINEAR)) {
        unsigned hi=0;
        for (unsigned j=i-i%3;j<i-i%3+3;j++) if ((unsigned)g->vertices[j].qw[unit]>hi) hi=g->vertices[j].qw[unit];
        rw=(unsigned)(((uint64_t)(unsigned)v->qw[unit]*16777216+hi/2)/hi);
      }
      if (unit && v->qw[unit]!=v->qw[0]) return -1;
      r->rhw=rw;
      if (!precise && ((u * r->rhw) % 65536 || (t * r->rhw) % 65536)) return -1;
    }
  }
  if (out->flags & ~caps->features) return -1;
  out->count = g->count;
  return 0;
}

#endif
