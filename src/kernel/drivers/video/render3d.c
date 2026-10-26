/* Family-neutral copied submission, lifetime and admission service.
 * See docs/design/f7-3d-acceleration-contract.md. GPL-2.0-only. */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include "render3d.h"
_Static_assert(sizeof(struct render3d_request) <= R3D_JOB_BYTES, "job bound");
_Static_assert(sizeof(((struct render3d_service *)0)->ring) == 262144, "ring bound");

static struct render3d_context *context(struct render3d_service *s, uint32_t p, uint32_t h)
{
    for (unsigned i=0;i<R3D_CONTEXTS;i++) {
        struct render3d_context *c=&s->contexts[i];
        if (c->live && c->handle==h && c->process==p) return c;
    }
    return 0;
}
static struct render3d_resource *resource(struct render3d_context *c, uint32_t h)
{
    if (!h || !c || !c->resources) return 0;
    for (unsigned i=0;i<R3D_RESOURCES;i++)
        if (c->resources[i].live && c->resources[i].handle==h) return &c->resources[i];
    return 0;
}
static void *allocate(struct render3d_service *s, uint32_t bytes)
{
    uint32_t backend=s->backend ? s->backend->resident_bytes : 0;
    if (backend>R3D_RAM_LIMIT-sizeof(*s) || s->ram_bytes>R3D_RAM_LIMIT-sizeof(*s)-backend ||
        bytes>R3D_RAM_LIMIT-sizeof(*s)-backend-s->ram_bytes) return 0;
    void *p=s->host.alloc(s->host.ctx,bytes);
    if (p) { s->ram_bytes+=bytes; memset(p,0,bytes); }
    return p;
}
static void release(struct render3d_service *s, void *p, uint32_t bytes)
{ if (p) { s->host.free(s->host.ctx,p); s->ram_bytes-=bytes; } }
static uint32_t handle(struct render3d_service *s)
{ return s->next_handle==UINT32_MAX ? 0 : ++s->next_handle; }
static bool usable(const struct render3d_service *s)
{
    const struct render3d_backend *b=s->backend;
    return s->admission && !s->quarantined && s->heap_end>s->heap_first && b && b->qualified &&
        b->layout && b->validate && b->enter && b->leave && b->execute &&
        b->idle && b->quarantine && b->qualified(s->device,s->generation);
}
void render3d_init(struct render3d_service *s, const struct render3d_host *h,
    uint32_t generation, const struct render3d_backend *b, void *device, struct render3d_extent heap)
{
    memset(s,0,sizeof(*s)); s->host=*h; s->generation=generation;
    s->backend=b; s->device=device; s->admission=generation && h->alloc && h->free && h->now_us;
    /* Registry owns/proves this heap; a zero/unaligned/wrapped extent admits
     * software only, with no device allocation or implicit scanout overlap. */
    if (heap.first && heap.bytes && !(heap.first&4095u) && !(heap.bytes&4095u) && heap.first<=UINT32_MAX-heap.bytes) {
        s->heap_first=heap.first; s->heap_end=heap.first+heap.bytes;
        if (b && b->texture_pool_bytes) {
            uint32_t n=b->texture_pool_bytes(device);
            if (!n || (n&4095u) || n>=heap.bytes) s->heap_end=s->heap_first;
            else s->texture_first=s->heap_end-n;
        }
    }
}
void render3d_backend_caps(const struct render3d_service *s,struct ciuki_r3d_caps *caps)
{
    *caps=(struct ciuki_r3d_caps){0};
    if (!usable(s)) return;
    if (s->backend->query_caps) s->backend->query_caps(s->device,caps);
    else if (s->backend->capabilities) *caps=*s->backend->capabilities;
    else *caps=(struct ciuki_r3d_caps){.max_size=s->backend->max_size,
        .max_vertices=3,.max_pixel_work=4096};
}
void render3d_query_caps(const struct render3d_service *s, struct render3d_caps *caps)
{
    struct ciuki_r3d_caps hw; render3d_backend_caps(s,&hw);
    *caps=(struct render3d_caps){2,R3D_ALL_FEATURES,usable(s) ? s->backend->features : 0,
        R3D_MAX_SIZE,2,R3D_JOBS*R3D_JOB_BYTES,R3D_JOB_BYTES,R3D_JOBS,R3D_RAM_LIMIT,hw.max_size, s->texture_first ? s->heap_end-s->texture_first : 0u};
}
void render3d_init_family(struct render3d_service *s, const struct render3d_host *h,
    uint32_t generation, const struct render3d_family *family, void *owner, struct render3d_extent heap)
{
    const struct render3d_backend *b=family && family->status==R3D_UNQUALIFIED && family->backend ? family->backend() : 0;
    render3d_init(s,h,generation,b,owner,heap);
}
int render3d_create_context(struct render3d_service *s, uint32_t p, enum render3d_policy policy, uint32_t *out)
{
    if (!p || !out || policy>R3D_HARDWARE_REQUIRED || policy<R3D_AUTO) return -EINVAL;
    if (s->busy) return -EBUSY;
    if (!s->admission) return -ENODEV;
    bool hw=policy!=R3D_SOFTWARE && usable(s) && s->heap_end>s->heap_first &&
        (s->backend->features&R3D_FEATURES)==R3D_FEATURES;
    if (!hw && policy==R3D_HARDWARE_REQUIRED) return -EOPNOTSUPP;
    struct render3d_context *free=0; unsigned owned=0;
    for (unsigned i=0;i<R3D_CONTEXTS;i++) {
        if (s->contexts[i].live) owned+=s->contexts[i].process==p;
        else if (!free) free=&s->contexts[i];
    }
    if (owned>=4 || !free) return -EAGAIN;
    uint32_t id=handle(s); if (!id) return -ENOSPC;
    *free=(struct render3d_context){.handle=id,.process=p,.generation=s->generation,.policy=policy,.live=true,.hardware=hw};
    *out=id; return 0;
}
static void collect(struct render3d_service *s, struct render3d_context *c)
{
    if (c->resources) for (unsigned i=0;i<R3D_RESOURCES;i++) {
        struct render3d_resource *r=&c->resources[i];
        if (!r->live || !r->pending_free || r->references || (s->quarantined && r->hardware)) continue;
        release(s,r->shadow,r->bytes); memset(r,0,sizeof(*r));
    }
    if (c->dying && !c->queued) {
        if (s->quarantined && c->hardware) return;
        release(s,c->resources,R3D_RESOURCES*sizeof(*c->resources)); memset(c,0,sizeof(*c));
    }
}
int render3d_destroy_context(struct render3d_service *s, uint32_t p, uint32_t id)
{
    if (s->busy) return -EBUSY;
    struct render3d_context *c=context(s,p,id); if (!c || c->dying) return -ENOENT;
    c->dying=true;
    if (c->resources) for (unsigned i=0;i<R3D_RESOURCES;i++) c->resources[i].pending_free=true;
    collect(s,c); return 0;
}
static bool overlap(uint32_t a, uint32_t n, uint32_t b, uint32_t m)
{ return (uint64_t)a+n>b && (uint64_t)b+m>a; }
static int reserve_vram(struct render3d_service *s, struct render3d_resource *r)
{
    if (!r->reserved || (r->reserved&4095u)) return -EINVAL;
    uint32_t at=r->kind==R3D_TEXTURE && s->texture_first ? s->texture_first : s->heap_first;
    uint32_t end=r->kind!=R3D_TEXTURE && s->texture_first ? s->texture_first : s->heap_end;
    for (unsigned probes=0;probes<64;probes++) {
        if (at>end || r->reserved>end-at) return -ENOMEM;
        uint32_t next=at;
        for (unsigned i=0;i<R3D_CONTEXTS;i++) {
            struct render3d_context *c=&s->contexts[i]; if (!c->live || !c->resources) continue;
            for (unsigned j=0;j<R3D_RESOURCES;j++) {
                const struct render3d_resource *a=&c->resources[j];
                if (a->live && a->hardware && a->offset && overlap(at,r->reserved,a->offset,a->reserved) && a->offset+a->reserved>next)
                    next=a->offset+a->reserved;
            }
        }
        if (next==at) { r->offset=at; return 0; }
        at=next;
    }
    return -EAGAIN;
}
/* AUTO can select software for a resource/layout unsupported by the first
 * backend, but only before any frame has been submitted. Existing shadows
 * are still canonical initial data; no partially executed work is replayed. */
static bool software_before_submit(struct render3d_context *c)
{
    if (c->policy!=R3D_AUTO || c->submitted || c->queued) return false;
    if (c->resources) for (unsigned i=0;i<R3D_RESOURCES;i++) {
        struct render3d_resource *r=&c->resources[i];
        if (r->resident || r->references) return false;
    }
    if (c->resources) for (unsigned i=0;i<R3D_RESOURCES;i++) {
        struct render3d_resource *r=&c->resources[i];
        r->hardware=false; r->offset=r->reserved=0;
    }
    c->hardware=false; return true;
}
/* Only completed, unbound textures can be evicted. Their immutable canonical
 * data remains charged in RAM; the next referencing job reuploads it. */
static int texture_reserve(struct render3d_service *s, struct render3d_resource *r)
{
    if (s->texture_first && r->reserved>s->heap_end-s->texture_first) return -EOPNOTSUPP;
    for (unsigned tries=0;tries<32;tries++) {
        int e=reserve_vram(s,r);
        if (e!=-ENOMEM) return e;
        struct render3d_resource *victim=0;
        for (unsigned i=0;i<R3D_CONTEXTS;i++) {
            struct render3d_context *c=&s->contexts[i];
            if (!c->live || !c->resources) continue;
            for (unsigned j=0;j<R3D_RESOURCES;j++) {
                struct render3d_resource *a=&c->resources[j];
                if (a!=r && a->live && a->hardware && a->kind==R3D_TEXTURE && a->offset && !a->references &&
                    (!victim || a->last_use<victim->last_use)) victim=a;
            }
        }
        if (!victim) return -ENOMEM;
        victim->offset=0; victim->resident=false;
    }
    return -EAGAIN;
}
static unsigned texel_bytes(uint32_t format)
{ return format<=R3D_RGBA8888 ? 4 : format<=R3D_ARGB4444 ? 2 : 1; }
int render3d_texture_shape(const struct render3d_resource *r)
{
    if (!r || r->kind!=R3D_TEXTURE || r->format>R3D_A8 || !r->levels || r->levels>R3D_MIPS ||
        !r->width || !r->height || r->width>1024 || r->height>1024 ||
        (r->width&(r->width-1)) || (r->height&(r->height-1))) return -EINVAL;
    uint32_t w=r->width,h=r->height,bytes=0;
    for (unsigned i=0;i<R3D_MIPS;i++) {
        if (i<r->levels) {
            if (r->level_offset[i]!=bytes) return -EINVAL;
            bytes+=w*h*4;
            if (w==1 && h==1 && i+1<r->levels) return -EINVAL;
            w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
        } else if (r->level_offset[i]) return -EINVAL;
    }
    return r->bytes==bytes && r->pitch==r->width*4 ? 0 : -EINVAL;
}
static int texture_description(const struct render3d_texture_desc *t,struct render3d_resource *r)
{
    if (!t || t->format>R3D_A8 || !t->levels || t->levels>R3D_MIPS) return -EINVAL;
    r->format=t->format; r->levels=t->levels; r->bytes=0;
    unsigned w=r->width,h=r->height,bpp=texel_bytes(t->format);
    uint64_t source_bytes=0;
    for (unsigned i=0;i<R3D_MIPS;i++) {
        const struct render3d_texture_image *im=&t->image[i];
        if (i>=t->levels) { if (im->pixels || im->bytes || im->pitch) return -EINVAL; continue; }
        if (!im->pixels || im->pitch<w*bpp || im->pitch>R3D_RAM_LIMIT ||
            im->bytes!=(uint64_t)im->pitch*h || im->bytes>UINTPTR_MAX-(uintptr_t)im->pixels) return -EINVAL;
        source_bytes+=im->bytes;
        if (source_bytes>R3D_RAM_LIMIT) return -EINVAL;
        r->level_offset[i]=r->bytes; r->bytes+=w*h*4;
        if (w==1 && h==1 && i+1<t->levels) return -EINVAL;
        w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
    }
    return render3d_texture_shape(r);
}
static void texture_decode(const struct render3d_texture_desc *t,struct render3d_resource *r)
{
    unsigned w=r->width,h=r->height,bpp=texel_bytes(t->format);
    for (unsigned i=0;i<t->levels;i++) {
        uint32_t *dst=(void *)((uint8_t *)r->shadow+r->level_offset[i]);
        for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
            const uint8_t *src=(const uint8_t *)t->image[i].pixels+y*t->image[i].pitch+x*bpp;
            uint32_t v=0; memcpy(&v,src,bpp);
            if (t->format==R3D_RGBA8888) v=((uint32_t)src[3]<<24)|((uint32_t)src[0]<<16)|((uint32_t)src[1]<<8)|src[2];
            else if (t->format==R3D_RGB565) {
                unsigned rr=(v>>11)&31,g=(v>>5)&63,b=v&31;
                v=0xff000000u|(((rr<<3)|(rr>>2))<<16)|(((g<<2)|(g>>4))<<8)|((b<<3)|(b>>2));
            } else if (t->format==R3D_ARGB4444) {
                v=((v&0xf000)<<16)|((v&0xf00)<<12)|((v&0xf0)<<8)|((v&15)<<4); v|=v>>4;
            } else if (t->format==R3D_L8) v=0xff000000u|v*0x010101u;
            else if (t->format==R3D_A8) v=(v<<24)|0xffffffu;
            dst[y*w+x]=v;
        }
        w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
    }
}
static int create_resource(struct render3d_service *s, uint32_t p, uint32_t id,
    enum render3d_kind kind, uint32_t w, uint32_t h, const struct render3d_texture_desc *t, uint32_t *out)
{
    if (s->busy) return -EBUSY;
    struct render3d_context *c=context(s,p,id);
    if (!c || c->dying || c->generation!=s->generation) return -ENOENT;
    if (c->failure) return c->failure;
    if (!out || kind<R3D_COLOR || kind>R3D_LINEAR_COLOR || !w || !h || w>R3D_MAX_SIZE || h>R3D_MAX_SIZE) return -EINVAL;
    if (kind==R3D_TEXTURE && ((w&(w-1)) || (h&(h-1)))) return -EINVAL;
    uint32_t pitch=w*(kind==R3D_D16 ? 2u : 4u), size=pitch*h;
    struct render3d_resource r={.kind=kind,.width=w,.height=h,.pitch=pitch,.bytes=size,.hardware=c->hardware};
    if (kind==R3D_TEXTURE) { int e=texture_description(t,&r); if (e) return e; size=r.bytes; }
    if (!c->resources) {
        c->resources=allocate(s,R3D_RESOURCES*sizeof(*c->resources));
        if (!c->resources) return -ENOMEM;
    }
    struct render3d_resource *slot=0;
    for (unsigned i=0;i<R3D_RESOURCES;i++) if (!c->resources[i].live) { slot=&c->resources[i]; break; }
    if (!slot) return -EAGAIN;
    if (c->hardware) {
        if (!usable(s)) return -EIO;
        int e=s->backend->layout(s->device,&r);
        if (e && (e!=-EOPNOTSUPP || !software_before_submit(c))) return e;
        if (!c->hardware) { r.hardware=false; r.offset=r.reserved=0; }
        /* Layout must not change the canonical RAM extent or kind. */
        if (r.bytes!=size || r.pitch!=pitch || r.width!=w || r.height!=h || r.kind!=(uint32_t)kind) return -EINVAL;
        /* Texture residency is established at submission, after validation.
         * Resource creation never overwrites an in-flight device allocation. */
        if (c->hardware && kind!=R3D_TEXTURE) { e=reserve_vram(s,&r); if (e) return e; }
    }
    r.handle=handle(s); if (!r.handle) return -ENOSPC;
    r.shadow=allocate(s,size); if (!r.shadow) return -ENOMEM;
    if (t) texture_decode(t,&r);
    r.live=true; *slot=r; *out=r.handle; return 0;
}
int render3d_create_texture(struct render3d_service *s,uint32_t p,uint32_t c,
    const struct render3d_texture_desc *t,uint32_t *out)
{
    if (!t) return -EINVAL;
    return create_resource(s,p,c,R3D_TEXTURE,t->width,t->height,t,out);
}
int render3d_create_resource(struct render3d_service *s,uint32_t p,uint32_t c,
    enum render3d_kind kind,uint32_t w,uint32_t h,const void *pixels,size_t bytes,uint32_t *out)
{
    if (kind==R3D_TEXTURE) {
        struct render3d_texture_desc t={.width=w,.height=h,.levels=1,
            .image={{pixels,bytes,w<=1024 ? w*4 : 0}}};
        return render3d_create_texture(s,p,c,&t,out);
    }
    if (pixels || bytes) return -EINVAL;
    return create_resource(s,p,c,kind,w,h,0,out);
}
int render3d_validate_state(const struct render3d_request *q)
{
    const struct render3d_state *a=&q->state;
    if (a->depth_func>R3D_GEQUAL || a->alpha_func>R3D_GEQUAL || a->alpha_ref>255 ||
        a->alpha_test>1 || a->blend>1 || a->flat>1 || a->source>R3D_INV_SRC_ALPHA || a->destination>R3D_INV_SRC_ALPHA)
        return -EINVAL;
    for (unsigned i=0;i<2;i++) {
        const struct render3d_sampler *t=&a->sampler[i];
        if (t->combine>R3D_DECAL || t->filter>R3D_TRILINEAR || t->repeat_u>1 || t->repeat_v>1) return -EINVAL;
    }
    return 0;
}
int render3d_destroy_resource(struct render3d_service *s, uint32_t p, uint32_t id, uint32_t h)
{
    if (s->busy) return -EBUSY;
    struct render3d_context *c=context(s,p,id);
    struct render3d_resource *r=resource(c,h);
    if (!c || c->dying || !r || r->pending_free) return -ENOENT;
    r->pending_free=true; collect(s,c); return 0;
}
static void resources(struct render3d_context *c, const struct render3d_request *q, struct render3d_resource *r[R3D_REFS])
{ r[0]=resource(c,q->target); r[1]=resource(c,q->depth); r[2]=resource(c,q->texture); r[3]=resource(c,q->copy_target); r[4]=resource(c,q->texture1); }
static int validate(struct render3d_context *c, const struct render3d_request *q, struct render3d_resource *r[R3D_REFS], uint32_t *work)
{
    if (!q->flags || (q->flags&~R3D_ALL_FEATURES) || q->count>R3D_VERTICES || q->count%3 || q->clear_depth>65535) return -EINVAL;
    if (render3d_validate_state(q)) return -EINVAL;
    resources(c,q,r);
    if (!r[0] || r[0]->pending_free || r[0]->kind!=R3D_COLOR) return -EINVAL;
    if ((q->flags&(R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE)) ?
        (!r[1] || r[1]->pending_free || r[1]->kind!=R3D_D16 || r[1]->width!=r[0]->width || r[1]->height!=r[0]->height) : q->depth!=0) return -EINVAL;
    if ((q->flags&R3D_TEXTURED) ? (!r[2] || r[2]->pending_free || r[2]->kind!=R3D_TEXTURE || !q->count) : q->texture!=0) return -EINVAL;
    if ((q->flags&R3D_TEXTURE1) ? (!(q->flags&R3D_TEXTURED) || !r[4] || r[4]->pending_free ||
        r[4]->kind!=R3D_TEXTURE || !q->count) : q->texture1!=0) return -EINVAL;
    if ((q->flags&R3D_COPY) ? (!r[3] || r[3]->pending_free || r[3]==r[0] ||
        (r[3]->kind!=R3D_COLOR && r[3]->kind!=R3D_LINEAR_COLOR) ||
        r[3]->width!=r[0]->width || r[3]->height!=r[0]->height) : q->copy_target!=0) return -EINVAL;
    uint64_t pixels=0;
    for (unsigned i=0;i<q->count;i++) {
        const struct render3d_vertex *v=&q->vertices[i];
        if (v->x<0 || v->y<0 || v->x>(int32_t)(r[0]->width<<16) || v->y>(int32_t)(r[0]->height<<16) ||
            v->z>65535 || v->rhw<256 || v->rhw>16777216 || v->u < -16*65536 || v->u>16*65536 || v->v < -16*65536 || v->v>16*65536 ||
            q->uv1[i][0] < -16*65536 || q->uv1[i][0]>16*65536 ||
            q->uv1[i][1] < -16*65536 || q->uv1[i][1]>16*65536) return -EINVAL;
        if (i%3==2) {
            int32_t minx=v->x,maxx=v->x,miny=v->y,maxy=v->y;
            for (unsigned j=i-2;j<i;j++) {
                const struct render3d_vertex *a=&q->vertices[j];
                if (a->x<minx) minx=a->x;
                if (a->x>maxx) maxx=a->x;
                if (a->y<miny) miny=a->y;
                if (a->y>maxy) maxy=a->y;
            }
            pixels+=(uint64_t)((maxx-minx+65535)>>16)*((maxy-miny+65535)>>16);
        }
    }
    for (unsigned i=q->count;i<R3D_VERTICES;i++) {
        static const struct render3d_vertex zero;
        if (memcmp(&q->vertices[i],&zero,sizeof(zero)) || q->uv1[i][0] || q->uv1[i][1]) return -EINVAL;
    }
    /* Reject expensive geometry before side effects; clear/copy split in rows. */
    if (pixels>UINT32_MAX) return -EAGAIN;
    *work=(uint32_t)pixels;
    return 0;
}
int render3d_submit(struct render3d_service *s, uint32_t p, uint32_t id,
    const struct render3d_request *input, size_t bytes, struct render3d_fence *out)
{
    if (s->busy) return -EBUSY;
    if (!input || !out || bytes!=sizeof(*input)) return -EINVAL;
    struct render3d_context *c=context(s,p,id);
    if (!c || c->dying || c->generation!=s->generation) return -ENOENT;
    if (!s->admission) return -ENODEV;
    if (c->failure) return c->failure;
    if (s->queued==R3D_JOBS || c->queued>=2 || s->sequence==UINT64_MAX) return -EAGAIN;
    /* Internal caller owns stable copied bytes until return. Snapshot before
     * validation; the same bytes are then immutable until fence completion. */
    unsigned slot=(s->head+s->queued)%R3D_JOBS;
    memcpy(s->ring[slot],input,bytes);
    struct render3d_request *q=(void *)s->ring[slot];
    struct render3d_resource *r[R3D_REFS]; uint32_t work;
    int e=validate(c,q,r,&work); if (e) return e;
    bool software=!c->hardware;
    if (software && work>R3D_PIXEL_BUDGET) return -EAGAIN;
    if (c->hardware) {
        if (!usable(s)) return -EIO;
        struct ciuki_r3d_caps hw; render3d_backend_caps(s,&hw);
        /* One outstanding hardware job, including after full-size qualification.
         * Dynamic limits belong to the qualified generation, never the PCI ID. */
        if (s->queued || q->count>hw.max_vertices || work>hw.max_pixel_work ||
            r[0]->width>hw.max_size || r[0]->height>hw.max_size) return -EAGAIN;
        if (q->flags&~s->backend->features) {
            if (c->policy!=R3D_AUTO) return -EOPNOTSUPP;
            software=true;
        }
        if (!software) {
            /* Pin both selected textures before eviction; a bound/in-flight
             * texture is never a victim, including a second stage. */
            for (unsigned i=0;i<R3D_REFS;i++) if (r[i]) r[i]->references++;
            for (unsigned i=0;i<R3D_REFS && !e;i++) if (r[i] && r[i]->kind==R3D_TEXTURE && !r[i]->offset)
                e=texture_reserve(s,r[i]);
            for (unsigned i=0;i<R3D_REFS;i++) if (r[i]) r[i]->references--;
            if (!e) e=s->backend->validate(s->device,q,r);
            if (e==-EOPNOTSUPP && c->policy==R3D_AUTO) { software=true; e=0; }
            if (e) return e;
        }
    }
    if (software && work>R3D_PIXEL_BUDGET) return -EAGAIN;
    for (unsigned i=0;i<R3D_REFS;i++) if (r[i]) {
        r[i]->references++;
        r[i]->last_use=s->sequence+1;
    }
    struct render3d_fence f={s->generation,id,++s->sequence,++c->ordinal_submitted};
    s->jobs[slot]=(struct render3d_job){.context=id,.fence=f,.occupied=true,.software=software};
    c->queued++; c->submitted=f.sequence; s->queued++; *out=f; return 0;
}
void render3d_device_lost(struct render3d_service *s)
{
    /* Permanent for the boot; do not reset, retry or release referenced VRAM. */
    if (!s->quarantined && s->backend && s->backend->quarantine) s->backend->quarantine(s->device);
    s->quarantined=true;
    for (unsigned i=0;i<R3D_CONTEXTS;i++) if (s->contexts[i].live && s->contexts[i].hardware) s->contexts[i].failure=-EIO;
}
static void rotate(struct render3d_service *s)
{
    unsigned slot=s->head, tail=(slot+s->queued)%R3D_JOBS;
    if (tail!=slot) {
        memcpy(s->ring[tail],s->ring[slot],sizeof(struct render3d_request));
        s->jobs[tail]=s->jobs[slot];
        memset(s->ring[slot],0,sizeof(struct render3d_request));
        s->jobs[slot].occupied=false;
    }
    s->head=(slot+1)%R3D_JOBS;
}
int render3d_step(struct render3d_service *s)
{
    if (s->busy) return -EBUSY;
    if (!s->queued) return 0;
    unsigned slot=s->head; struct render3d_job *j=&s->jobs[slot];
    struct render3d_context *c=0;
    for (unsigned i=0;i<R3D_CONTEXTS;i++) if (s->contexts[i].live && s->contexts[i].handle==j->context) c=&s->contexts[i];
    if (!c) return -EIO; /* internal inconsistency, never panic or touch MMIO */
    /* A context's later job cannot overtake its own partial clear/draw/copy. */
    for (unsigned i=0;i<R3D_JOBS;i++) if (s->jobs[i].occupied &&
        s->jobs[i].context==j->context && s->jobs[i].fence.sequence<j->fence.sequence) {
        rotate(s); return 1;
    }
    struct render3d_request *q=(void *)s->ring[slot]; struct render3d_resource *r[R3D_REFS]; resources(c,q,r);
    int e=0,done=1; s->busy=true;
    if (c->dying) e=-ECANCELED;
    else if (c->failure) e=c->failure;
    else if (c->hardware && !j->software) {
        if (!usable(s)) e=-EIO;
        else {
            e=s->backend->enter(s->device);
            if (!e) {
                e=s->backend->execute(s->device,q,r);
                int idle=s->backend->idle(s->device);
                if (!e) e=idle;
                /* A bounded CPU transfer can time out after completed engine
                 * work and successful restoration. Fail that context without
                 * quarantining an owner that still proves idle. */
                if (e && (e!=-ETIMEDOUT || idle)) render3d_device_lost(s);
                s->backend->leave(s->device);
            } else if (e!=-EAGAIN && e!=-EBUSY) render3d_device_lost(s);
        }
    } else done=render3d_scalar_slice(q,r,j,&s->host);
    s->busy=false;
    if (e==-EAGAIN || e==-EBUSY) return e;
    if (!done && !e) {
        /* Round-robin at every bounded scalar slice, without a second ring
         * or a large stack copy. A full ring rotates by changing head only. */
        rotate(s);
        return 1;
    }
    if (!e && j->software && c->hardware) for (unsigned i=0;i<R3D_REFS;i++)
        if (r[i] && r[i]->kind!=R3D_TEXTURE) r[i]->resident=false;
    if (e && !c->failure) c->failure=e;
    c->completed=j->fence.sequence; c->ordinal_completed=j->fence.ordinal; c->queued--;
    if (!(s->quarantined && c->hardware)) for (unsigned i=0;i<R3D_REFS;i++) if (r[i]) r[i]->references--;
    j->result=e; j->occupied=false; j->complete=true;
    memset(s->ring[slot],0,sizeof(s->ring[slot])); s->head=(s->head+1)%R3D_JOBS; s->queued--;
    collect(s,c); return e ? e : 1;
}
int render3d_wait(struct render3d_service *s, uint32_t p, uint32_t id, struct render3d_fence f, uint32_t timeout_ms)
{
    if (s->busy) return -EBUSY;
    struct render3d_context *c=context(s,p,id);
    if (!c || c->dying || f.context!=id || f.generation!=s->generation || f.generation!=c->generation ||
        !f.ordinal || f.ordinal>c->ordinal_submitted) return -EINVAL;
    /* Every ordinal <= ordinal_submitted identifies an actual context job.
     * Completed receipts are ordinal-only: sequence is informational once
     * complete. Pending pairs must match the immutable submission snapshot. */
    if (f.ordinal>c->ordinal_completed) {
        bool owned=false;
        for (unsigned i=0;i<R3D_JOBS;i++) if (s->jobs[i].occupied && s->jobs[i].context==id &&
            s->jobs[i].fence.ordinal==f.ordinal && s->jobs[i].fence.sequence==f.sequence) owned=true;
        if (!owned) return -EINVAL;
    }
    if (timeout_ms>100) timeout_ms=100;
    uint64_t start=s->host.now_us(s->host.ctx);
    /* Frozen clock/worker: bounded observations, timeout is not device loss. */
    for (unsigned i=0;i<1024;i++) {
        if (c->failure) return c->failure;
        if (c->ordinal_completed>=f.ordinal) return 0;
        if (!timeout_ms || s->host.now_us(s->host.ctx)-start>=(uint64_t)timeout_ms*1000) break;
        if (s->host.yield) s->host.yield(s->host.ctx);
    }
    return -ETIMEDOUT;
}
int render3d_readback(struct render3d_service *s, uint32_t p, uint32_t id, uint32_t h, void *out, size_t bytes)
{
    if (s->busy) return -EBUSY;
    struct render3d_context *c=context(s,p,id); struct render3d_resource *r=resource(c,h);
    if (!c || c->dying || !r || r->pending_free) return -ENOENT;
    if (c->failure) return c->failure;
    if (!out || bytes!=r->bytes) return -EINVAL;
    if (r->references) return -EAGAIN;
    /* Executing backends synchronize canonical shadows before completion.
     * A new resource's zero/texture shadow is also its defined initial data. */
    memcpy(out,r->shadow,bytes); return 0;
}
int render3d_upload(struct render3d_service *s, uint32_t p, uint32_t id,
    uint32_t h, const void *input, size_t bytes)
{
    if (s->busy) return -EBUSY;
    struct render3d_context *c=context(s,p,id); struct render3d_resource *r=resource(c,h);
    if (!c || c->dying || !r || r->pending_free) return -ENOENT;
    if (!s->admission || c->failure) return c->failure ? c->failure : -ENODEV;
    if (!input || bytes!=r->bytes || r->kind==R3D_TEXTURE) return -EINVAL;
    if (r->references) return -EAGAIN;
    memcpy(r->shadow,input,bytes); r->resident=false; return 0;
}
void render3d_process_exit(struct render3d_service *s, uint32_t p)
{
    for (unsigned i=0;i<R3D_CONTEXTS;i++) {
        struct render3d_context *c=&s->contexts[i]; if (!c->live || c->process!=p) continue;
        c->dying=true;
        if (c->resources) for (unsigned j=0;j<R3D_RESOURCES;j++) c->resources[j].pending_free=true;
        if (!s->busy) collect(s,c);
    }
}
int render3d_close(struct render3d_service *s)
{
    s->admission=false; /* synchronous close, no later enqueue */
    if (s->busy) return -EBUSY;
    /* Pending offscreen jobs are cancelled; an executing hook is synchronous
     * and must have returned with idle proof before this worker can close. */
    for (unsigned i=0;i<R3D_CONTEXTS;i++) if (s->contexts[i].live) {
        struct render3d_context *c=&s->contexts[i];
        c->dying=true;
        /* A cancelled last job can collect the whole context. Mark shadows
         * before draining so that collection cannot discard their pointers. */
        if (c->resources) for (unsigned j=0;j<R3D_RESOURCES;j++) c->resources[j].pending_free=true;
    }
    for (unsigned i=0;s->queued && i<R3D_JOBS*R3D_JOBS;i++) (void)render3d_step(s);
    if (s->queued) return -EIO;
    for (unsigned i=0;i<R3D_CONTEXTS;i++) if (s->contexts[i].live) {
        struct render3d_context *c=&s->contexts[i];
        collect(s,c);
    }
    if (!s->quarantined && s->backend && s->backend->detach) s->backend->detach(s,s->device);
    return s->quarantined ? -EIO : 0;
}
