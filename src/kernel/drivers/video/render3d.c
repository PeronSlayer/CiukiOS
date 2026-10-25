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
    if (bytes>R3D_RAM_LIMIT-sizeof(*s)-s->ram_bytes) return 0;
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
    if (heap.bytes && !(heap.first&4095u) && !(heap.bytes&4095u) && heap.first<=UINT32_MAX-heap.bytes) {
        s->heap_first=heap.first; s->heap_end=heap.first+heap.bytes;
    }
}
void render3d_query_caps(const struct render3d_service *s, struct render3d_caps *caps)
{
    *caps=(struct render3d_caps){1,R3D_FEATURES,usable(s) ? s->backend->features : 0,
        R3D_MAX_SIZE,1,R3D_JOBS*R3D_JOB_BYTES,R3D_JOB_BYTES,R3D_JOBS,R3D_RAM_LIMIT,usable(s) ? s->backend->max_size : 0u};
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
    uint32_t at=s->heap_first;
    for (;;) {
        if (at>s->heap_end || r->reserved>s->heap_end-at) return -ENOMEM;
        uint32_t next=at;
        for (unsigned i=0;i<R3D_CONTEXTS;i++) {
            struct render3d_context *c=&s->contexts[i]; if (!c->live || !c->resources) continue;
            for (unsigned j=0;j<R3D_RESOURCES;j++) {
                const struct render3d_resource *a=&c->resources[j];
                if (a->live && a->hardware && overlap(at,r->reserved,a->offset,a->reserved) && a->offset+a->reserved>next)
                    next=a->offset+a->reserved;
            }
        }
        if (next==at) { r->offset=at; return 0; }
        at=next;
    }
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
int render3d_create_resource(struct render3d_service *s, uint32_t p, uint32_t id,
    enum render3d_kind kind, uint32_t w, uint32_t h, const void *pixels, size_t bytes, uint32_t *out)
{
    if (s->busy) return -EBUSY;
    struct render3d_context *c=context(s,p,id);
    if (!c || c->dying || c->generation!=s->generation) return -ENOENT;
    if (c->failure) return c->failure;
    if (!out || kind<R3D_COLOR || kind>R3D_LINEAR_COLOR || !w || !h || w>R3D_MAX_SIZE || h>R3D_MAX_SIZE) return -EINVAL;
    if (kind==R3D_TEXTURE && ((w&(w-1)) || (h&(h-1)))) return -EINVAL;
    uint32_t pitch=w*(kind==R3D_D16 ? 2u : 4u), size=pitch*h;
    if (kind==R3D_TEXTURE ? (!pixels || bytes!=size) : (pixels || bytes)) return -EINVAL;
    if (!c->resources) {
        c->resources=allocate(s,R3D_RESOURCES*sizeof(*c->resources));
        if (!c->resources) return -ENOMEM;
    }
    struct render3d_resource *slot=0;
    for (unsigned i=0;i<R3D_RESOURCES;i++) if (!c->resources[i].live) { slot=&c->resources[i]; break; }
    if (!slot) return -EAGAIN;
    struct render3d_resource r={.kind=kind,.width=w,.height=h,.pitch=pitch,.bytes=size,.hardware=c->hardware};
    if (c->hardware) {
        if (!usable(s)) return -EIO;
        int e=s->backend->layout(s->device,&r);
        if (e && (e!=-EOPNOTSUPP || !software_before_submit(c))) return e;
        if (!c->hardware) { r.hardware=false; r.offset=r.reserved=0; }
        /* Layout must not change the canonical RAM extent or kind. */
        if (r.bytes!=size || r.pitch!=pitch || r.width!=w || r.height!=h || r.kind!=(uint32_t)kind) return -EINVAL;
        if (c->hardware) { e=reserve_vram(s,&r); if (e) return e; }
    }
    r.handle=handle(s); if (!r.handle) return -ENOSPC;
    r.shadow=allocate(s,size); if (!r.shadow) return -ENOMEM;
    if (pixels) memcpy(r.shadow,pixels,size);
    r.live=true; *slot=r; *out=r.handle; return 0;
}
int render3d_destroy_resource(struct render3d_service *s, uint32_t p, uint32_t id, uint32_t h)
{
    if (s->busy) return -EBUSY;
    struct render3d_context *c=context(s,p,id);
    struct render3d_resource *r=resource(c,h);
    if (!c || c->dying || !r || r->pending_free) return -ENOENT;
    r->pending_free=true; collect(s,c); return 0;
}
static void resources(struct render3d_context *c, const struct render3d_request *q, struct render3d_resource *r[4])
{ r[0]=resource(c,q->target); r[1]=resource(c,q->depth); r[2]=resource(c,q->texture); r[3]=resource(c,q->copy_target); }
static int validate(struct render3d_context *c, const struct render3d_request *q, struct render3d_resource *r[4], uint32_t *work)
{
    if (!q->flags || (q->flags&~R3D_FEATURES) || q->count>R3D_VERTICES || q->count%3 || q->clear_depth>65535) return -EINVAL;
    resources(c,q,r);
    if (!r[0] || r[0]->pending_free || r[0]->kind!=R3D_COLOR) return -EINVAL;
    if ((q->flags&(R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE)) ?
        (!r[1] || r[1]->pending_free || r[1]->kind!=R3D_D16 || r[1]->width!=r[0]->width || r[1]->height!=r[0]->height) : q->depth!=0) return -EINVAL;
    if ((q->flags&R3D_TEXTURED) ? (!r[2] || r[2]->pending_free || r[2]->kind!=R3D_TEXTURE || !q->count) : q->texture!=0) return -EINVAL;
    if ((q->flags&R3D_COPY) ? (!r[3] || r[3]->pending_free || r[3]==r[0] ||
        (r[3]->kind!=R3D_COLOR && r[3]->kind!=R3D_LINEAR_COLOR) ||
        r[3]->width!=r[0]->width || r[3]->height!=r[0]->height) : q->copy_target!=0) return -EINVAL;
    uint64_t pixels=0;
    for (unsigned i=0;i<q->count;i++) {
        const struct render3d_vertex *v=&q->vertices[i];
        if (v->x<0 || v->y<0 || v->x>(int32_t)(r[0]->width<<16) || v->y>(int32_t)(r[0]->height<<16) ||
            v->z>65535 || v->rhw<256 || v->rhw>16777216 || v->u<0 || v->u>65536 || v->v<0 || v->v>65536) return -EINVAL;
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
        if (memcmp(&q->vertices[i],&zero,sizeof(zero))) return -EINVAL;
    }
    /* Reject expensive geometry before side effects; clear/copy split in rows. */
    if (pixels>R3D_PIXEL_BUDGET) return -EAGAIN;
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
    struct render3d_resource *r[4]; uint32_t work;
    int e=validate(c,q,r,&work); if (e) return e;
    if (c->hardware) {
        if (!usable(s)) return -EIO;
        /* Unmeasured hardware: one tiny outstanding slice; byte limits are
         * never a claim of a calibrated 20 ms throughput budget. */
        if (s->queued || q->count>3 || work>4096 || r[0]->width>64 || r[0]->height>64) return -EAGAIN;
        e=s->backend->validate(s->device,q,r); if (e) return e;
    }
    for (unsigned i=0;i<4;i++) if (r[i]) r[i]->references++;
    struct render3d_fence f={s->generation,id,++s->sequence,++c->ordinal_submitted};
    s->jobs[slot]=(struct render3d_job){.context=id,.fence=f,.occupied=true};
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
    struct render3d_request *q=(void *)s->ring[slot]; struct render3d_resource *r[4]; resources(c,q,r);
    int e=0,done=1; s->busy=true;
    if (c->dying) e=-ECANCELED;
    else if (c->failure) e=c->failure;
    else if (c->hardware) {
        if (!usable(s)) e=-EIO;
        else {
            e=s->backend->enter(s->device);
            if (!e) {
                e=s->backend->execute(s->device,q,r);
                if (!e) e=s->backend->idle(s->device);
                if (e) render3d_device_lost(s);
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
    if (e && !c->failure) c->failure=e;
    c->completed=j->fence.sequence; c->ordinal_completed=j->fence.ordinal; c->queued--;
    if (!(s->quarantined && c->hardware)) for (unsigned i=0;i<4;i++) if (r[i]) r[i]->references--;
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
