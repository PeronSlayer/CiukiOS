/* Kernel-internal native renderer interface. No syscall/SDK ABI.
 * Trusted adapters copy user bytes and run ALL entries on the display worker.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_RENDER3D_INTERNAL_H
#define CIUKI_RENDER3D_INTERNAL_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <ciuki/abi.h>

#define R3D_CONTEXTS 32u
#define R3D_RESOURCES 256u
#define R3D_JOBS 4u
#define R3D_JOB_BYTES 65536u
#define R3D_VERTICES 768u
#define R3D_RAM_LIMIT (32u * 1024u * 1024u)
#define R3D_MAX_SIZE 1024u
#define R3D_PIXEL_BUDGET 65536u
#define R3D_TEXTURED CIUKI_R3D_TEXTURED
#define R3D_CLEAR_COLOR CIUKI_R3D_CLEAR_COLOR
#define R3D_CLEAR_DEPTH CIUKI_R3D_CLEAR_DEPTH
#define R3D_DEPTH_TEST CIUKI_R3D_DEPTH_TEST
#define R3D_DEPTH_WRITE CIUKI_R3D_DEPTH_WRITE
#define R3D_COPY CIUKI_R3D_COPY
#define R3D_FEATURES 63u
#define R3D_TEXTURE1 CIUKI_R3D_TEXTURE1
#define R3D_ALL_FEATURES 511u
#define R3D_STENCIL_TEST CIUKI_R3D_STENCIL_TEST
#define R3D_CLEAR_STENCIL CIUKI_R3D_CLEAR_STENCIL
#define R3D_DEPTH_FLAGS (R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_STENCIL_TEST|R3D_CLEAR_STENCIL)
#define R3D_REFS 5u
#define R3D_MIPS 11u

/* Zero state preserves the f7-02 minimum. Public wire ordinals have one source. */
enum render3d_compare {
    R3D_LESS = CIUKI_R3D_LESS,
    R3D_NEVER = CIUKI_R3D_NEVER,
    R3D_ALWAYS = CIUKI_R3D_ALWAYS,
    R3D_EQUAL = CIUKI_R3D_EQUAL,
    R3D_LEQUAL = CIUKI_R3D_LEQUAL,
    R3D_GREATER = CIUKI_R3D_GREATER,
    R3D_NOTEQUAL = CIUKI_R3D_NOTEQUAL,
    R3D_GEQUAL = CIUKI_R3D_GEQUAL
};
enum render3d_blend {
    R3D_ONE = CIUKI_R3D_ONE,
    R3D_ZERO = CIUKI_R3D_ZERO,
    R3D_SRC_ALPHA = CIUKI_R3D_SRC_ALPHA,
    R3D_INV_SRC_ALPHA = CIUKI_R3D_INV_SRC_ALPHA,
    R3D_DST_COLOR = CIUKI_R3D_DST_COLOR,
    R3D_SRC_COLOR = CIUKI_R3D_SRC_COLOR,
    R3D_INV_DST_COLOR = CIUKI_R3D_INV_DST_COLOR,
    R3D_INV_SRC_COLOR = CIUKI_R3D_INV_SRC_COLOR,
    R3D_DST_ALPHA = CIUKI_R3D_DST_ALPHA,
    R3D_INV_DST_ALPHA = CIUKI_R3D_INV_DST_ALPHA
};
enum render3d_combine {
    R3D_MODULATE = CIUKI_R3D_MODULATE,
    R3D_REPLACE = CIUKI_R3D_REPLACE,
    R3D_ADD = CIUKI_R3D_ADD,
    R3D_DECAL = CIUKI_R3D_DECAL
};
enum render3d_filter {
    R3D_NEAREST = CIUKI_R3D_NEAREST,
    R3D_BILINEAR = CIUKI_R3D_BILINEAR,
    R3D_NEAREST_MIP = CIUKI_R3D_NEAREST_MIP,
    R3D_BILINEAR_MIP = CIUKI_R3D_BILINEAR_MIP,
    R3D_TRILINEAR = CIUKI_R3D_TRILINEAR
};
enum render3d_format {
    R3D_ARGB8888 = CIUKI_R3D_TEX_ARGB8888,
    R3D_RGBA8888 = CIUKI_R3D_TEX_RGBA8888,
    R3D_RGB565 = CIUKI_R3D_TEX_RGB565,
    R3D_ARGB4444 = CIUKI_R3D_TEX_ARGB4444,
    R3D_L8 = CIUKI_R3D_TEX_L8,
    R3D_A8 = CIUKI_R3D_TEX_A8
};
#define render3d_sampler ciuki_r3d_sampler
#define render3d_state ciuki_r3d_state
struct render3d_texture_image {
    const void *pixels;
    size_t bytes;
    uint32_t pitch;
};
struct render3d_texture_desc {
    uint32_t format, width, height, levels;
    struct render3d_texture_image image[R3D_MIPS];
};

enum render3d_policy { R3D_AUTO, R3D_SOFTWARE, R3D_HARDWARE_REQUIRED };
enum render3d_kind {
    R3D_COLOR = CIUKI_R3D_RESOURCE_COLOR,
    R3D_D16 = CIUKI_R3D_RESOURCE_D16,
    R3D_TEXTURE = CIUKI_R3D_RESOURCE_TEXTURE,
    R3D_LINEAR_COLOR = CIUKI_R3D_RESOURCE_LINEAR_COLOR,
    R3D_D24S8 = CIUKI_R3D_RESOURCE_D24S8,
    R3D_COLOR565 = CIUKI_R3D_RESOURCE_COLOR565,
    R3D_LINEAR565 = CIUKI_R3D_RESOURCE_LINEAR565
};
static inline uint32_t render3d_pack565(uint32_t p)
{ return ((p>>8)&0xf800)|((p>>5)&0x7e0)|((p>>3)&31); }
static inline uint32_t render3d_expand565(uint32_t p)
{
    unsigned r=p>>11&31,g=p>>5&63,b=p&31;
    return 0xff000000u|((r<<3|r>>2)<<16)|((g<<2|g>>4)<<8)|(b<<3|b>>2);
}
enum render3d_status { R3D_SOFTWARE_ONLY, R3D_NOT_IMPLEMENTED, R3D_UNQUALIFIED };
struct render3d_backend;
/* Potential features are research metadata, NEVER application capabilities. */
struct render3d_family {
    enum render3d_status status;
    unsigned potential_units, potential_texture_size;
    const struct render3d_backend *(*backend)(void);
};
struct render3d_caps {
    uint32_t version, features, hardware_features, max_size, texture_units;
    uint32_t ring_bytes, max_job_bytes, max_jobs, ram_budget;
    uint32_t hardware_max_size;
    uint32_t texture_heap_bytes;
};
/* Screen X/Y Q16.16 [0,1024]; normalized Z Q0.16 [0,65535];
 * D24S8 uses normalized Z 0..16777215 instead;
 * reciprocal W Q16.16 [1/256,256]; U/V Q16.16 [-16,16].
 * RGBA is packed 0xAARRGGBB. No kernel FP or implicit API-origin conversion. */
#define render3d_vertex ciuki_r3d_vertex
/* Typed, fully copied list. Unknown bits and nonzero unused vertices fail.
 * Zero state selects D16 LESS, Gouraud, nearest/clamp, MODULATE.
 * texture/vertices.u,v select stage 0; texture1/uv1 select stage 1.
 * flat_color is explicit per triangle, independent of provoking-vertex rules.
 * COPY copies to an owned offscreen presentation buffer, never scanout. */
#define render3d_request ciuki_r3d_draw
/* sequence orders the device; ordinal is a consecutive context receipt.
 * Completed receipts remain queryable after ring reuse without retaining
 * unbounded jobs. Completed identity is generation + owned context + ordinal;
 * sequence is then informational (ignored, including zero). Pending receipts
 * require the exact queued sequence/ordinal pair. No exact completed pair
 * authentication is promised. Context/process ownership is always checked. */
struct render3d_fence {
    uint32_t generation, context;
    uint64_t sequence, ordinal;
};
struct render3d_extent { uint32_t first, bytes; };
struct render3d_resource {
    uint32_t handle, kind, width, height, pitch, bytes, offset, reserved;
    uint32_t references;
    uint32_t copied_from; /* Invalidate when the completed tiled source changes. */
    /* stale: the resident device copy is newer than the canonical shadow
     * (backends with download only); the shadow is synchronized on demand. */
    bool live, pending_free, hardware, resident, stale;
    uint32_t format, levels, level_offset[R3D_MIPS];
    uint64_t last_use;
    void *shadow;
};
struct render3d_context {
    uint32_t handle, process, generation, queued;
    enum render3d_policy policy;
    uint64_t submitted, completed;
    uint64_t ordinal_submitted, ordinal_completed;
    int failure;
    bool live, hardware, dying;
    struct render3d_resource *resources;
};
struct render3d_job {
    uint32_t context, row, triangle, phase, column;
    struct render3d_fence fence;
    int result;
    bool occupied, complete;
    bool software;
};
struct fb_device;
struct render3d_service;
struct render3d_host;
/* Hooks share the display driver's existing owner: enter takes presenter,
 * then engine. leave reverses the order. Never allocate, persist or log
 * under them. qualification is read-only and generation-specific.
 * validate/layout have NO side effects. execute emits one bounded slice,
 * flush/idle prove completion, error quarantines the same 2D owner. */
struct render3d_backend {
    uint32_t features;
    bool (*qualified)(void *, uint32_t);
    int (*layout)(void *, struct render3d_resource *);
    int (*validate)(void *, const struct render3d_request *, struct render3d_resource *const [R3D_REFS]);
    int (*enter)(void *);
    void (*leave)(void *);
    int (*execute)(void *, const struct render3d_request *,
                   struct render3d_resource *const [R3D_REFS]);
    int (*idle)(void *);
    void (*quarantine)(void *);
    /* Optional owner attachment for the internal native consumer. */
    int (*attach)(struct render3d_service *, const struct render3d_host *, uint32_t);
    uint32_t max_size;
    void (*detach)(struct render3d_service *, void *);
    uint32_t (*texture_pool_bytes)(void *);
    const struct ciuki_r3d_caps *capabilities;
    void (*query_caps)(void *, struct ciuki_r3d_caps *);
    uint32_t resident_bytes; /* Backend-owned staging charged to the ceiling. */
    bool whole_context_fallback; /* Latch AUTO refusal for this context. */
    /* First/last hardware context: revocable owner heap, outside execution. */
    int (*prepare)(uint32_t); /* Optional short mode check before lease publication. */
    int (*reserve)(void *);
    void (*unreserve)(void *);
    int (*scanout)(void *, const struct render3d_resource *, const struct fb_device *);
    /* Optional, inside enter/leave: copy one resident attachment's device
     * pixels into its canonical shadow (bounded, may yield the presenter).
     * A backend with download leaves attachments device-resident after
     * execute; one without it keeps every shadow current in execute. */
    int (*download)(void *, struct render3d_resource *);
};
struct render3d_host {
    void *ctx;
    void *(*alloc)(void *, size_t);
    void (*free)(void *, void *);
    uint64_t (*now_us)(void *);
    void (*yield)(void *);
};
/* One owner-supplied disjoint VRAM heap. It must already exclude scanout,
 * cursor, firmware, COB and the 2D offscreen reservation. No BAR alias is
 * another heap. Unsupported AGP never receives an allocation. */
struct render3d_service {
    /* Optional trusted background allocation pacing; never a wire callback. */
    int (*allocation_checkpoint)(void *);
    struct render3d_host host;
    const struct render3d_backend *backend;
    void *device;
    uint32_t generation, next_handle, ram_bytes, heap_first, heap_end;
    uint32_t texture_first;
    uint64_t sequence;
    bool admission, quarantined, busy;
    unsigned head, queued;
    struct render3d_context contexts[R3D_CONTEXTS];
    struct render3d_job jobs[R3D_JOBS];
    /* Exactly 256 KiB staging; slot stride is aligned for request records. */
    _Alignas(8) unsigned char ring[R3D_JOBS][R3D_JOB_BYTES];
};
void render3d_init(struct render3d_service *, const struct render3d_host *, uint32_t,
                   const struct render3d_backend *, void *, struct render3d_extent);
/* Pass the descriptor of the already bound display owner; no PCI re-probe. */
void render3d_init_family(struct render3d_service *, const struct render3d_host *, uint32_t,
                         const struct render3d_family *, void *, struct render3d_extent);
void render3d_backend_caps(const struct render3d_service *, struct ciuki_r3d_caps *);
void render3d_query_caps(const struct render3d_service *, struct render3d_caps *);
int render3d_create_context(struct render3d_service *, uint32_t, enum render3d_policy, uint32_t *);
int render3d_destroy_context(struct render3d_service *, uint32_t, uint32_t);
int render3d_create_resource(struct render3d_service *, uint32_t, uint32_t,
    enum render3d_kind, uint32_t, uint32_t, const void *, size_t, uint32_t *);
/* Texture creation copies every row/mip. Updates create a new immutable
 * handle, then retire the old one; queued references survive retirement.
 * RGBA8888 is byte-ordered R,G,B,A. Other multi-byte input is little-endian.
 * Shadows/readback are packed ARGB8888, with consecutive canonical mips. */
int render3d_create_texture(struct render3d_service *, uint32_t, uint32_t,
    const struct render3d_texture_desc *, uint32_t *);
int render3d_texture_shape(const struct render3d_resource *);
int render3d_validate_state(const struct render3d_request *);
int render3d_destroy_resource(struct render3d_service *, uint32_t, uint32_t, uint32_t);
int render3d_submit(struct render3d_service *, uint32_t, uint32_t,
                   const struct render3d_request *, size_t, struct render3d_fence *);
int render3d_step(struct render3d_service *);
int render3d_wait(struct render3d_service *, uint32_t, uint32_t, struct render3d_fence, uint32_t);
/* Synchronizes a device-resident attachment first (only when it is stale). */
int render3d_readback(struct render3d_service *, uint32_t, uint32_t, uint32_t, void *, size_t);
void render3d_process_exit(struct render3d_service *, uint32_t);
int render3d_close(struct render3d_service *);
/* Native/Win32 trusted adapter boundary: uses the already bound display
 * nomination, never chooses a family or grants a presentation lease. */
int render3d_native_prepare(void);
int render3d_scanout(struct render3d_service *, uint32_t, const struct ciuki_w32_render3d_args *, const struct fb_device *);
int render3d_native_init(struct render3d_service *, const struct render3d_host *);
int render3d_native_refresh(struct render3d_service *);
/* Private fbdev exclusion; successful enter must pair with leave on the
 * same task. No allocation, file I/O or logging inside this interval. */
int fbdev_render3d_enter(uint32_t);
void fbdev_render3d_leave(void);
/* Inside enter/leave only: yields the presenter mutex, keeps the job's claim. */
void fbdev_render3d_yield(void);
int fbdev_render3d_prepare(uint32_t);
void fbdev_render3d_prepared(void);
int fbdev_render3d_cache(uint32_t, int (*)(void *, bool), void *, bool);
void render3d_device_lost(struct render3d_service *);
/* Private software slice; returns 1 when complete, 0 for another slice. */
int render3d_scalar_slice(const struct render3d_request *, struct render3d_resource *const [R3D_REFS],
                          struct render3d_job *, const struct render3d_host *);
int render3d_scalar_sample_slice(const struct render3d_request *, struct render3d_resource *const [R3D_REFS],
                                 struct render3d_job *, const struct render3d_host *);
#endif
