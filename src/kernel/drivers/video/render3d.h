/* Kernel-internal native renderer interface. No syscall/SDK ABI.
 * Trusted adapters copy user bytes and run ALL entries on the display worker.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_RENDER3D_INTERNAL_H
#define CIUKI_RENDER3D_INTERNAL_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define R3D_CONTEXTS 32u
#define R3D_RESOURCES 256u
#define R3D_JOBS 4u
#define R3D_JOB_BYTES 65536u
#define R3D_VERTICES 192u
#define R3D_RAM_LIMIT (32u * 1024u * 1024u)
#define R3D_MAX_SIZE 1024u
#define R3D_PIXEL_BUDGET 65536u
#define R3D_TEXTURED 1u
#define R3D_CLEAR_COLOR 2u
#define R3D_CLEAR_DEPTH 4u
#define R3D_DEPTH_TEST 8u
#define R3D_DEPTH_WRITE 16u
#define R3D_COPY 32u
#define R3D_FEATURES 63u

enum render3d_policy { R3D_AUTO, R3D_SOFTWARE, R3D_HARDWARE_REQUIRED };
enum render3d_kind { R3D_COLOR = 1, R3D_D16, R3D_TEXTURE, R3D_LINEAR_COLOR };
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
};
/* Screen X/Y Q16.16 [0,1024]; normalized Z Q0.16 [0,65535];
 * reciprocal W Q16.16 [1/256,256]; U/V Q16.16 [0,1], clamp + nearest.
 * RGBA is packed 0xAARRGGBB. No kernel FP or implicit API-origin conversion. */
struct render3d_vertex {
    int32_t x, y;
    uint32_t z, rhw, color;
    int32_t u, v;
};
/* Typed, fully copied list. Unknown bits and nonzero unused vertices fail.
 * D16 LESS, Gouraud, nearest/clamp, one-stage MODULATE are the minimum subset.
 * COPY copies to an owned offscreen presentation buffer, never scanout. */
struct render3d_request {
    uint32_t flags, target, depth, texture, copy_target;
    uint32_t clear_color, clear_depth, count;
    struct render3d_vertex vertices[R3D_VERTICES];
};
/* sequence orders the device; ordinal is a consecutive context receipt.
 * Completed receipts remain queryable after ring reuse without retaining
 * unbounded jobs. Context/process ownership authorizes the receipt. */
struct render3d_fence {
    uint32_t generation, context;
    uint64_t sequence, ordinal;
};
struct render3d_extent { uint32_t first, bytes; };
struct render3d_resource {
    uint32_t handle, kind, width, height, pitch, bytes, offset, reserved;
    uint32_t references;
    bool live, pending_free, hardware, resident;
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
    uint32_t context, row, triangle, phase;
    struct render3d_fence fence;
    int result;
    bool occupied, complete;
};
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
    int (*validate)(void *, const struct render3d_request *, struct render3d_resource *const [4]);
    int (*enter)(void *);
    void (*leave)(void *);
    int (*execute)(void *, const struct render3d_request *,
                   struct render3d_resource *const [4]);
    int (*idle)(void *);
    void (*quarantine)(void *);
    /* Optional owner attachment for the internal native consumer. */
    int (*attach)(struct render3d_service *, const struct render3d_host *, uint32_t);
    uint32_t max_size;
    void (*detach)(struct render3d_service *, void *);
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
    struct render3d_host host;
    const struct render3d_backend *backend;
    void *device;
    uint32_t generation, next_handle, ram_bytes, heap_first, heap_end;
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
void render3d_query_caps(const struct render3d_service *, struct render3d_caps *);
int render3d_create_context(struct render3d_service *, uint32_t, enum render3d_policy, uint32_t *);
int render3d_destroy_context(struct render3d_service *, uint32_t, uint32_t);
int render3d_create_resource(struct render3d_service *, uint32_t, uint32_t,
    enum render3d_kind, uint32_t, uint32_t, const void *, size_t, uint32_t *);
int render3d_destroy_resource(struct render3d_service *, uint32_t, uint32_t, uint32_t);
int render3d_submit(struct render3d_service *, uint32_t, uint32_t,
                   const struct render3d_request *, size_t, struct render3d_fence *);
int render3d_step(struct render3d_service *);
int render3d_wait(struct render3d_service *, uint32_t, uint32_t, struct render3d_fence, uint32_t);
int render3d_readback(struct render3d_service *, uint32_t, uint32_t, uint32_t, void *, size_t);
void render3d_process_exit(struct render3d_service *, uint32_t);
int render3d_close(struct render3d_service *);
/* Native/Win32 trusted adapter boundary: uses the already bound display
 * nomination, never chooses a family or grants a presentation lease. */
int render3d_native_init(struct render3d_service *, const struct render3d_host *);
/* Private fbdev exclusion; successful enter must pair with leave on the
 * same task. No allocation, file I/O or logging inside this interval. */
int fbdev_render3d_enter(uint32_t);
void fbdev_render3d_leave(void);
int fbdev_render3d_prepare(uint32_t);
void fbdev_render3d_prepared(void);
void render3d_device_lost(struct render3d_service *);
/* Private software slice; returns 1 when complete, 0 for another slice. */
int render3d_scalar_slice(const struct render3d_request *, struct render3d_resource *const [4],
                          struct render3d_job *, const struct render3d_host *);
#endif
