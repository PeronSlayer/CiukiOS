/* Display-worker copied-wire adapter. GPL-2.0-only */
#include <ciuki/kernel.h>
#include "render3d_transport.h"
static int finish(struct render3d_service *s, uint32_t p, uint32_t id,
                  struct render3d_fence f, uint32_t ms)
{
    if (ms>100) ms=100;
    uint64_t start=s->host.now_us(s->host.ctx);
    for (unsigned i=0;i<1024;i++) {
        int e=render3d_wait(s,p,id,f,0);
        if (e!=-ETIMEDOUT) return e;
        if (!ms || s->host.now_us(s->host.ctx)-start>=(uint64_t)ms*1000) break;
        e=render3d_step(s);
        if (e<0 && e!=-EAGAIN && e!=-EBUSY) return e;
    }
    return -ETIMEDOUT;
}
int render3d_transport_dispatch(struct render3d_transport *t, uint32_t p,
    struct ciuki_w32_render3d_args *a, void *data, uint32_t bytes)
{
    struct render3d_service *s=t->service;
    if (!p || !s || bytes!=a->bytes || (bytes && !data) || bytes>CIUKI_R3D_WIRE_LIMIT) return -EINVAL;
    if (a->action!=CIUKI_R3D_QUERY && a->generation!=s->generation) return -ENODEV;
    struct render3d_fence f={a->generation,a->context,
        (uint64_t)a->sequence_hi<<32|a->sequence_lo,(uint64_t)a->ordinal_hi<<32|a->ordinal_lo};
    switch (a->action) {
    case CIUKI_R3D_QUERY: {
        if (bytes!=sizeof(struct ciuki_r3d_caps)) return -EINVAL;
        struct render3d_caps c; render3d_query_caps(s,&c);
        struct ciuki_r3d_caps out={0};
        out.version=CIUKI_R3D_VERSION; out.generation=s->generation;
        if (c.hardware_features && a->width && a->height &&
            a->width<=c.hardware_max_size && a->height<=c.hardware_max_size &&
            a->format==CIUKI_R3D_ARGB8888 && a->kind==CIUKI_R3D_D16 && !a->levels) {
            const struct ciuki_r3d_caps *native=s->backend->capabilities;
            if (native) {
                out=*native; out.generation=s->generation;
                out.features &= c.hardware_features;
                if (out.max_size>c.hardware_max_size) out.max_size=c.hardware_max_size;
                out.texture_bytes=c.texture_heap_bytes;
            }
        }
        memcpy(data,&out,sizeof(out)); return 0;
    }
    case CIUKI_R3D_OPEN:
        if (bytes) return -EINVAL;
        return render3d_create_context(s,p,R3D_HARDWARE_REQUIRED,&a->context);
    case CIUKI_R3D_CLOSE:
        if (bytes) return -EINVAL;
        return render3d_destroy_context(s,p,a->context);
    case CIUKI_R3D_RESOURCE:
        if (bytes || a->kind==R3D_TEXTURE) return -EINVAL;
        return render3d_create_resource(s,p,a->context,a->kind,a->width,a->height,0,0,&a->resource);
    case CIUKI_R3D_FREE:
        if (bytes) return -EINVAL;
        return render3d_destroy_resource(s,p,a->context,a->resource);
    case CIUKI_R3D_UPLOAD:
        return render3d_upload(s,p,a->context,a->resource,data,bytes);
    case CIUKI_R3D_READ:
        return render3d_readback(s,p,a->context,a->resource,data,bytes);
    case CIUKI_R3D_TEXTURE: {
        if (!a->levels || a->levels>R3D_MIPS || a->format>R3D_A8 ||
            !a->width || !a->height || a->width>1024 || a->height>1024) return -EINVAL;
        struct render3d_texture_desc d={.format=a->format,.width=a->width,.height=a->height,.levels=a->levels};
        uint32_t w=a->width,h=a->height,at=0,bpp=a->format<=R3D_RGBA8888 ? 4 : a->format<=R3D_ARGB4444 ? 2 : 1;
        for (unsigned i=0;i<a->levels;i++) {
            uint32_t n=w*h*bpp;
            if (n>bytes-at) return -EINVAL;
            d.image[i]=(struct render3d_texture_image){(uint8_t *)data+at,n,w*bpp};
            at+=n; w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
        }
        if (at!=bytes) return -EINVAL;
        return render3d_create_texture(s,p,a->context,&d,&a->resource);
    }
    case CIUKI_R3D_SUBMIT: {
        int e=render3d_submit(s,p,a->context,data,bytes,&f);
        if (e) return e;
        a->sequence_lo=(uint32_t)f.sequence; a->sequence_hi=f.sequence>>32;
        a->ordinal_lo=(uint32_t)f.ordinal; a->ordinal_hi=f.ordinal>>32;
        return 0;
    }
    case CIUKI_R3D_WAIT:
        if (bytes) return -EINVAL;
        return finish(s,p,a->context,f,a->timeout_ms);
    case CIUKI_R3D_PRESENT: {
        /* Completed tiled-to-linear copy first; callback re-proves the existing
         * lease for each bounded scanout write. No raw front address in wire. */
        if (!t->present || !a->token || bytes!=a->width*a->height*4u ||
            !a->width || !a->height || a->width>64 || a->height>64 ||
            a->pitch!=a->width*4u) return -EOPNOTSUPP;
        int e=finish(s,p,a->context,f,100);
        if (!e) e=render3d_readback(s,p,a->context,a->resource,data,bytes);
        if (e) return e;
        /* Require the owned linear destination, never an arbitrary texture. */
        bool linear=false;
        for (unsigned i=0;i<R3D_CONTEXTS;i++) {
            struct render3d_context *c=&s->contexts[i];
            if (c->live && !c->dying && c->handle==a->context && c->process==p && c->resources)
                for (unsigned j=0;j<R3D_RESOURCES;j++) {
                    struct render3d_resource *r=&c->resources[j];
                    if (r->live && !r->pending_free && r->handle==a->resource &&
                        r->kind==R3D_LINEAR_COLOR && r->width==a->width && r->height==a->height) linear=true;
                }
        }
        return linear ? t->present(t->present_ctx,p,a,data,bytes) : -EINVAL;
    }
    default: return -EINVAL;
    }
}
