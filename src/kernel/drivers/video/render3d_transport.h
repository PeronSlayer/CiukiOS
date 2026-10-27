/* Copied worker dispatch; no user pointers or display authority. GPL-2.0-only */
#ifndef CIUKI_RENDER3D_TRANSPORT_H
#define CIUKI_RENDER3D_TRANSPORT_H
#include "render3d.h"
/* Refuse oversized/irrelevant payloads before allocating staging RAM. */
static inline bool render3d_wire_valid(const struct ciuki_w32_render3d_args *a)
{
    if (a->result || a->error || a->bytes>CIUKI_R3D_WIRE_LIMIT) return false;
    switch (a->action) {
    case CIUKI_R3D_QUERY: return a->bytes==sizeof(struct ciuki_r3d_caps);
    case CIUKI_R3D_SUBMIT: return a->bytes==sizeof(struct ciuki_r3d_draw);
    case CIUKI_R3D_OPEN: case CIUKI_R3D_CLOSE: case CIUKI_R3D_RESOURCE:
    case CIUKI_R3D_FREE: case CIUKI_R3D_WAIT: return !a->bytes;
    case CIUKI_R3D_READ: case CIUKI_R3D_UPLOAD: case CIUKI_R3D_PRESENT:
        /* Canonical color attachments can span the qualified display. The
         * dispatcher still checks the exact owned resource size/format. */
        return a->bytes && a->bytes<=R3D_MAX_SIZE*R3D_MAX_SIZE*4u;
    case CIUKI_R3D_TEXTURE: return a->bytes && a->levels && a->levels<=R3D_MIPS &&
        a->width && a->height && a->width<=1024 && a->height<=1024;
    default: return false;
    }
}
struct render3d_transport {
    struct render3d_service *service;
    void *present_ctx;
    /* NULL pixels: direct native path only, -EOPNOTSUPP when presentation
     * needs the CPU copy; the dispatcher then reads back and calls again. */
    int (*present)(void *, uint32_t, const struct ciuki_w32_render3d_args *, const void *, uint32_t);
};
int render3d_transport_dispatch(struct render3d_transport *, uint32_t,
    struct ciuki_w32_render3d_args *, void *, uint32_t);
void render3d_wire_caps(const struct render3d_service *, struct ciuki_r3d_caps *);
void render3d_filter_caps(const struct ciuki_r3d_caps *,
    const struct ciuki_w32_render3d_args *, struct ciuki_r3d_caps *);
int render3d_upload(struct render3d_service *, uint32_t, uint32_t, uint32_t, const void *, size_t);
void w32_render3d_poll(void);
#endif
