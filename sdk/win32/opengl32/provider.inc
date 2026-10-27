/* Shared render3d provider over the copied W32_RENDER3D wire: the GDI
 * personality (sdk/win32/gdi32/render3d.c) and the native library
 * (sdk/libgl/native/provider.c) include it. The includer defines
 * GL_PROVIDER_ALLOC/FREE/COPY/ZERO and gl_provider_exchange(), which performs
 * one copied kernel exchange and returns negative errno. No device names.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
struct provider_context {
  unsigned id, color, depth, linear, width, height, depth_bits, stencil_bits, rgb565;
  struct ciuki_r3d_caps caps;
  unsigned sequence_lo, sequence_hi, ordinal_lo, ordinal_hi;
};
static struct provider_context contexts[4];
static struct provider_context *lookup(unsigned id) {
  for (unsigned i=0;i<4;i++) if (id && contexts[i].id==id) return contexts+i;
  return 0;
}
static int query_caps(const struct gl_hw_format *f,struct ciuki_r3d_caps *c) {
  if (!gl_hw_format_supported(f)) return 0;
  struct ciuki_w32_render3d_args a={.action=CIUKI_R3D_QUERY,.width=f->width,
    .height=f->height,.kind=f->depth_bits==24 ? CIUKI_R3D_D24S8 : CIUKI_R3D_D16,
    .levels=f->stencil_bits,.format=f->rgb565 ? CIUKI_R3D_RGB565 : CIUKI_R3D_ARGB8888,.bytes=sizeof(*c)};
  return !gl_provider_exchange(&a,c) && c->version==CIUKI_R3D_VERSION && c->features &&
    c->max_size>=f->width && c->max_size>=f->height;
}
static int query(unsigned dc,const struct gl_hw_format *f,struct gl_hw_caps *out) {
  (void)dc;
  struct ciuki_r3d_caps c={0};
  if (!query_caps(f,&c)) return 0;
  *out=(struct gl_hw_caps){GL_HW_VERSION,c.generation,c.max_size,c.max_vertices,c.texture_units,c.texture_bytes,c.max_pixel_work};
  return 1;
}
static struct ciuki_w32_render3d_args args(struct provider_context *c,unsigned action) {
  struct ciuki_w32_render3d_args a={.action=action,.context=c->id,.generation=c->caps.generation};
  return a;
}
static int close_context(unsigned id) {
  struct provider_context *c=lookup(id);
  if (!c) return -EINVAL;
  struct ciuki_w32_render3d_args a=args(c,CIUKI_R3D_CLOSE);
  int e=gl_provider_exchange(&a,0); GL_PROVIDER_ZERO(c,sizeof(*c)); return e;
}
static void close_provider(unsigned id) { (void)close_context(id); }
static int open_provider(unsigned dc,const struct gl_hw_format *f,unsigned *out) {
  (void)dc;
  struct provider_context *c=0;
  for (unsigned i=0;i<4;i++) if (!contexts[i].id) { c=contexts+i; break; }
  if (!c || !query_caps(f,&c->caps)) return -EOPNOTSUPP;
  struct ciuki_w32_render3d_args a=args(c,CIUKI_R3D_OPEN);
  int e=gl_provider_exchange(&a,0);
  if (e) return e;
  c->id=a.context; c->width=f->width; c->height=f->height;
  c->depth_bits=f->depth_bits; c->stencil_bits=f->stencil_bits; c->rgb565=f->rgb565;
  unsigned *handles[]={&c->color,&c->depth,&c->linear};
  const unsigned kinds[]={f->rgb565 ? R3D_COLOR565 : R3D_COLOR,
    f->depth_bits==24 ? R3D_D24S8 : R3D_D16,f->rgb565 ? R3D_LINEAR565 : R3D_LINEAR_COLOR};
  for (unsigned i=0;i<3;i++) {
    a=args(c,CIUKI_R3D_RESOURCE); a.kind=kinds[i]; a.width=f->width; a.height=f->height;
    e=gl_provider_exchange(&a,0);
    if (e) { close_provider(c->id); return e; }
    *handles[i]=a.resource;
  }
  *out=c->id; return 0;
}
static int accept(unsigned id,const struct sw_state *s,unsigned mask,const struct sw_vertex *v,unsigned count) {
  struct provider_context *c=lookup(id);
  if (!c || count>3) return 0;
  struct gl_hw_command *g=GL_PROVIDER_ALLOC(sizeof(*g));
  struct render3d_request *r=GL_PROVIDER_ALLOC(sizeof(*r));
  if (!g || !r) { GL_PROVIDER_FREE(g,sizeof(*g)); GL_PROVIDER_FREE(r,sizeof(*r)); return 0; }
  g->state=*s; g->clear_mask=mask; g->count=count; g->clear_depth=s->depth24 ? 16777215u : 65535u;
  if (v && count) GL_PROVIDER_COPY(g->vertices,v,count*sizeof(*v));
  int e=gl_hw_encode(&c->caps,g,c->color,c->depth,0,r);
  GL_PROVIDER_FREE(g,sizeof(*g)); GL_PROVIDER_FREE(r,sizeof(*r)); return e ? -1 : 1;
}
static int transfer(unsigned id,struct sw_state *s,unsigned action) {
  struct provider_context *c=lookup(id);
  if (!c || s->width!=c->width || s->height!=c->height || !s->color ||
      (c->depth_bits==24 ? !s->depth24 || s->depth : !s->depth || s->depth24) ||
      !!s->stencil!=!!c->stencil_bits || s->color2 || !!s->rgb565!=!!c->rgb565) return -EOPNOTSUPP;
  unsigned n=c->width*c->height;
  struct ciuki_w32_render3d_args a=args(c,action);
  a.resource=c->color; a.bytes=n*4;
  int e=gl_provider_exchange(&a,s->color);
  if (e) return e;
  a=args(c,action); a.resource=c->depth;
  if (c->depth_bits==16) { a.bytes=n*2; return gl_provider_exchange(&a,s->depth); }
  a.bytes=n*4;
  uint32_t *packed=GL_PROVIDER_ALLOC(a.bytes);
  if (!packed) return -ENOMEM;
  if (action==CIUKI_R3D_UPLOAD) for (unsigned i=0;i<n;i++)
    packed[i]=(s->depth24[i]&0xffffffu)|(s->stencil ? (unsigned)s->stencil[i]<<24 : 0);
  e=gl_provider_exchange(&a,packed);
  if (!e && action==CIUKI_R3D_READ) for (unsigned i=0;i<n;i++) {
    s->depth24[i]=packed[i]&0xffffffu;
    if (s->stencil) s->stencil[i]=packed[i]>>24;
  }
  GL_PROVIDER_FREE(packed,a.bytes); return e;
}
static int load(unsigned id,const struct sw_state *s) {
  struct sw_state copy=*s; return transfer(id,&copy,CIUKI_R3D_UPLOAD);
}
static int read_provider(unsigned id,struct sw_state *s) { return transfer(id,s,CIUKI_R3D_READ); }
static int texture(unsigned id,const struct sw_texture *s,unsigned *out) {
  struct provider_context *c=lookup(id);
  if (!c || !s || !s->image[0].pixels) return -EINVAL;
  unsigned w=s->image[0].w,h=s->image[0].h,bytes=0,levels=0;
  if (!w || !h || w>c->caps.max_texture_size || h>c->caps.max_texture_size) return -EOPNOTSUPP;
  for (unsigned i=0;i<R3D_MIPS;i++) {
    const struct sw_image *m=s->image+i;
    if (!m->pixels || m->w!=w || m->h!=h) break;
    bytes+=w*h*4; levels++;
    if (w==1 && h==1) break;
    w=w>1?w/2:1; h=h>1?h/2:1;
  }
  if (!bytes || bytes>c->caps.texture_bytes) return -ENOMEM;
  void *data=GL_PROVIDER_ALLOC(bytes);
  if (!data) return -ENOMEM;
  unsigned at=0;
  for (unsigned i=0;i<levels;i++) {
    unsigned n=s->image[i].w*s->image[i].h*4;
    uint32_t *pixels=(void *)((uint8_t *)data+at);
    GL_PROVIDER_COPY(pixels,s->image[i].pixels,n);
    if (gl_hw_texture_rgb(s->image[i].internal))
      for (unsigned j=0;j<n/4;j++) pixels[j]|=0xff000000u;
    at+=n;
  }
  struct ciuki_w32_render3d_args a=args(c,CIUKI_R3D_TEXTURE);
  a.width=s->image[0].w; a.height=s->image[0].h; a.levels=levels;
  a.format=R3D_ARGB8888; a.bytes=bytes;
  int e=gl_provider_exchange(&a,data); GL_PROVIDER_FREE(data,bytes);
  if (!e) *out=a.resource;
  return e;
}
static void free_texture(unsigned id,unsigned handle) {
  struct provider_context *c=lookup(id);
  if (!c) return;
  struct ciuki_w32_render3d_args a=args(c,CIUKI_R3D_FREE); a.resource=handle;
  (void)gl_provider_exchange(&a,0);
}
static int finish_provider(unsigned id) {
  struct provider_context *c=lookup(id);
  if (!c) return -EINVAL;
  if (!c->ordinal_lo && !c->ordinal_hi) return 0;
  struct ciuki_w32_render3d_args a=args(c,CIUKI_R3D_WAIT);
  a.sequence_lo=c->sequence_lo; a.sequence_hi=c->sequence_hi;
  a.ordinal_lo=c->ordinal_lo; a.ordinal_hi=c->ordinal_hi; a.timeout_ms=100;
  return gl_provider_exchange(&a,0);
}
static int submit_request(struct provider_context *c,struct render3d_request *r) {
  /* Keep the probed one-outstanding-job admission; journal still owns recovery. */
  int e=finish_provider(c->id);
  if (e) return e;
  struct ciuki_w32_render3d_args a=args(c,CIUKI_R3D_SUBMIT); a.bytes=sizeof(*r);
  e=gl_provider_exchange(&a,r);
  if (!e) { c->sequence_lo=a.sequence_lo; c->sequence_hi=a.sequence_hi;
    c->ordinal_lo=a.ordinal_lo; c->ordinal_hi=a.ordinal_hi; }
  return e;
}
static int submit(unsigned id,const struct gl_hw_command *g) {
  struct provider_context *c=lookup(id);
  if (!c) return -EINVAL;
  struct render3d_request *r=GL_PROVIDER_ALLOC(sizeof(*r));
  if (!r) return -ENOMEM;
  int e=gl_hw_encode(&c->caps,g,c->color,c->depth,0,r);
  if (!e) e=submit_request(c,r);
  GL_PROVIDER_FREE(r,sizeof(*r)); return e;
}
static int present(unsigned id,unsigned token,unsigned generation,unsigned width,unsigned height,unsigned bpp,unsigned pitch) {
  struct provider_context *c=lookup(id);
  if (!c || generation!=c->caps.generation || width!=c->width || height!=c->height ||
      bpp!=32 || pitch!=width*4) return 0;
  struct render3d_request *r=GL_PROVIDER_ALLOC(sizeof(*r));
  if (!r) return -ENOMEM;
  r->flags=R3D_COPY; r->target=c->color; r->copy_target=c->linear;
  int e=submit_request(c,r); GL_PROVIDER_FREE(r,sizeof(*r));
  if (e) return e;
  struct ciuki_w32_render3d_args a=args(c,CIUKI_R3D_PRESENT);
  a.resource=c->linear; a.token=token; a.width=width; a.height=height; a.pitch=pitch; a.bytes=pitch*height;
  a.sequence_lo=c->sequence_lo; a.sequence_hi=c->sequence_hi;
  a.ordinal_lo=c->ordinal_lo; a.ordinal_hi=c->ordinal_hi;
  e=gl_provider_exchange(&a,0);
  return e ? e : 1;
}
/* f7-15: the owned linear copy of the last present is the front buffer of a
 * GPU swap. The kernel reads it back from VRAM only on this request. */
static int read_presented(unsigned id,uint32_t *pixels,unsigned width,unsigned height) {
  struct provider_context *c=lookup(id);
  if (!c || !pixels || width!=c->width || height!=c->height || c->rgb565) return -EOPNOTSUPP;
  struct ciuki_w32_render3d_args a=args(c,CIUKI_R3D_READ);
  a.resource=c->linear; a.bytes=width*height*4;
  return gl_provider_exchange(&a,pixels);
}
static const struct gl_hw_driver provider={GL_HW_VERSION,sizeof(provider),query,open_provider,
  close_provider,accept,load,read_provider,texture,free_texture,submit,finish_provider,present,
  read_presented};
