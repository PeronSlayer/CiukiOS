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
static int gl_hw_texture_complete(const struct sw_texture *t) {
  if (!t || !t->image[0].pixels) return 0;
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
/* Sample-space admission: input quantization consumes <0.009 texel at
 * 64x64, |UV|<=4 and Q20 W>=0.5. Internal precision is boot-qualified.
 * Alpha-test discontinuities remain outside this smooth-filter proof. */
static int gl_hw_projective_bilinear(const struct ciuki_r3d_caps *caps,
    const struct gl_hw_command *g,unsigned first,unsigned unit) {
  if (gl_hw_affine_exact(caps,g,first,unit)) return 1;
  const struct sw_texture *t=g->state.unit[unit].texture;
  if (!(caps->sampling_features&CIUKI_R3D_PROJECTIVE_BILINEAR) ||
      t->mag!=GL_LINEAR || t->image[0].w>64 || t->image[0].h>64 ||
      (t->min!=GL_LINEAR && t->min!=GL_LINEAR_MIPMAP_NEAREST &&
       t->min!=GL_LINEAR_MIPMAP_LINEAR) || g->state.alpha_test) return 0;
  int lo=2097152,hi=0;
  for (unsigned i=0;i<3;i++) {
    const struct sw_vertex *v=&g->vertices[first+i];
    if (v->x<0 || v->y<0 || (unsigned)v->x>g->state.width*16 ||
        (unsigned)v->y>g->state.height*16 || v->qw[unit]<524288 || v->qw[unit]>2097152) return 0;
    if (v->qw[unit]<lo) lo=v->qw[unit];
    if (v->qw[unit]>hi) hi=v->qw[unit];
    for (unsigned axis=0;axis<2;axis++)
      if ((int64_t)v->uv[unit][axis]*1048576 < -(int64_t)4*65536*v->qw[unit] ||
          (int64_t)v->uv[unit][axis]*1048576 > (int64_t)4*65536*v->qw[unit]) return 0;
  }
  if (t->min!=GL_LINEAR) {
    const struct sw_vertex *a=&g->vertices[first],*b=a+1,*c=a+2;
    int64_t dx=b->x-a->x,dy=b->y-a->y,ex=c->x-a->x,ey=c->y-a->y;
    int64_t area=dx*ey-dy*ex;
    if (!area) return 0;
    int64_t gx=(int64_t)(b->qw[unit]-a->qw[unit])*ey-(int64_t)(c->qw[unit]-a->qw[unit])*dy;
    int64_t gy=(int64_t)(c->qw[unit]-a->qw[unit])*dx-(int64_t)(b->qw[unit]-a->qw[unit])*ex;
    if (area<0) area=-area;
    if (gx<0) gx=-gx;
    if (gy<0) gy=-gy;
    /* Bound one-pixel reciprocal-W change. Finite-neighbour hardware LOD
     * versus exact quotient derivatives then differs by <1/16 octave. */
    if ((gx+gy)*16*32>area*lo) return 0;
  }
  return hi<=4*lo;
}
static int gl_hw_encode(const struct ciuki_r3d_caps *caps, const struct gl_hw_command *g, unsigned target,
    unsigned depth, unsigned copy, struct render3d_request *out) {
  const struct sw_state *s = &g->state;
  *out = (struct render3d_request){0};
  if (!s->width || !s->height || s->width > R3D_MAX_SIZE || s->height > R3D_MAX_SIZE ||
      g->count > caps->max_vertices || g->count>R3D_VERTICES || g->count % 3 || s->depth24 || s->rgb565 ||
      s->color_disabled || s->color2 || s->color_mask != UINT32_MAX || s->scissor ||
      (s->stencil_test && s->stencil) || s->fog || s->offset ||
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
    if (a<0 || b<0 || !(caps->blend_factors & (1u<<a)) || !(caps->blend_factors & (1u<<b))) return -1;
    out->state.blend=1; out->state.source=a; out->state.destination=b;
  }
  for (unsigned u=0;u<2;u++) if (g->count && s->unit[u].enabled) {
    const struct sw_texture *t=s->unit[u].texture;
    int mode=gl_r3d_combine(s->unit[u].mode);
    if (!gl_hw_texture_complete(t)) return -1;
    unsigned internal=t->image[0].internal;
    bool rgb=internal==3 || internal==GL_RGB || (internal>=0x804f && internal<=0x8054);
    bool rgba=internal==4 || internal==GL_RGBA || (internal>=0x8055 && internal<=0x805b);
    unsigned filter=t->mag==GL_LINEAR ? R3D_BILINEAR : R3D_NEAREST;
    if (u>=caps->texture_units || (u && !s->unit[0].enabled) || mode<0 ||
        !(caps->combine_modes & (1u<<mode)) || !(caps->filters & (1u<<filter)) ||
        !t->image[0].pixels || (!rgb && !rgba) || (rgb && mode==R3D_REPLACE) ||
        (t->wrap_s!=GL_CLAMP_TO_EDGE && t->wrap_s!=GL_REPEAT) ||
        (t->wrap_t!=GL_CLAMP_TO_EDGE && t->wrap_t!=GL_REPEAT) ||
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
    r->z = (unsigned)v->z; r->rhw = 65536;
    for (unsigned k = 0; k < 4; k++) if (v->color[k] < 0 || v->color[k] > 255) return -1;
    r->color = (unsigned)v->color[3] << 24 | (unsigned)v->color[0] << 16 |
               (unsigned)v->color[1] << 8 | (unsigned)v->color[2];
    for (unsigned unit=0;unit<2;unit++) if (out->flags & (unit ? R3D_TEXTURE1 : R3D_TEXTURED)) {
      if (v->qw[unit] < 4096 || v->qw[unit] > 268435456) return -1;
      bool precise=gl_hw_projective_bilinear(caps,g,i-i%3,unit);
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
      if (u < -16*65536 || u > 16*65536 || t < -16*65536 || t > 16*65536) return -1;
      if (unit) { out->uv1[i][0]=(int32_t)u; out->uv1[i][1]=(int32_t)t; }
      else { r->u=(int32_t)u; r->v=(int32_t)t; }
      unsigned rw=((unsigned)v->qw[unit]+(precise ? 8 : 0))/16;
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
