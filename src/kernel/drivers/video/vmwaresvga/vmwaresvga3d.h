/* Private SVGA3D (legacy VGPU9 fixed-function) backend for the VMware SVGA II
 * owner. No client ever sees VRAM offsets, surface ids or FIFO state.
 * Design and pinned sources: docs/design/f7-family-vmware-svga3d.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VMWARESVGA3D_H
#define CIUKI_VMWARESVGA3D_H
#include "engine.h"
#include "../render3d.h"
#include "vmwaresvga3d_protocol.h"

#define VMSVGA3D_TEXTURE_MIN (256u * 1024u)
#define VMSVGA3D_TEXTURE_FULLSIZE (480u * 1024u) /* both Q2/Q3 mip chains + guards */
#define VMSVGA3D_HEAP_EXTRA (8u * 1024u * 1024u)  /* texture pool cap above the plan */
#define VMSVGA3D_VERTEX_WORDS 9u             /* X,Y,Z,W, D3DCOLOR, U0,V0, U1,V1 */
#define VMSVGA3D_VERTEX_BYTES (R3D_VERTICES * VMSVGA3D_VERTEX_WORDS * 4u)
/* Backend-owned staging: guard, vertex buffer image, guard; below the
 * service heap, inside the same claimed VRAM extent. */
#define VMSVGA3D_SCRATCH_BYTES (((VMSVGA3D_VERTEX_BYTES + 4095u) & ~4095u) + 8192u)
#define VMSVGA3D_BATCH_WORDS 2048u
#define VMSVGA3D_WAIT_US 250000u
#define VMSVGA3D_DEFINE_WAIT_US 1000000u
#define VMSVGA3D_QUALIFY_US (30u * 1000000u)
#define VMSVGA3D_SLICE_US 20000u
#define VMSVGA3D_CID 1u
#define VMSVGA3D_SID_BASE 16u
/* Fixed per-job surface roles; every one is destroyed in the same batch. */
enum vmsvga3d_role { VMSVGA3D_SID_COLOR, VMSVGA3D_SID_DEPTH, VMSVGA3D_SID_TEX0,
    VMSVGA3D_SID_COPY, VMSVGA3D_SID_TEX1, VMSVGA3D_SID_VERTEX, VMSVGA3D_SIDS };
/* Combine encodings beyond MODULATE, each admitted only after its boot
 * phase passes (REPLACE0 is algebraic; the others need SVGA3D_DEVCAP_TEXTURE_OPS). */
#define VMSVGA3D_COMBINE_REPLACE0 1u  /* MODULATE with white operands */
#define VMSVGA3D_COMBINE_REPLACE1 2u  /* SELECTARG1 */
#define VMSVGA3D_COMBINE_ADD 4u       /* ADD */
#define VMSVGA3D_COMBINE_DECAL 8u     /* BLENDTEXTUREALPHA + SELECTARG1 */

/* Snapshot of the FIFO caps block and hardware version. Values are host
 * answers, not claims: admission still needs this boot's qualification. */
struct vmsvga3d_caps {
    uint32_t hwversion, three_d, max_textures, max_texture_width, max_texture_height;
    uint32_t texture_ops, argb_ops, d16_ops, d16_buffer;
    uint32_t max_primitives, max_vertex_index, max_context_ids, max_surface_ids;
    uint32_t seen[3]; /* devcap indices 0..95 present in the records */
    bool valid, gb_objects;
};
/* Owner-only access to the claimed VRAM staging extent. Offsets are VRAM
 * offsets (the SVGA_GMR_FRAMEBUFFER guest pointer space). */
struct vmsvga3d_memory {
    void *ctx;
    uint32_t (*read)(void *, uint32_t, unsigned);
    void (*write)(void *, uint32_t, uint32_t, unsigned);
    int (*enter)(void *, uint32_t);
    void (*leave)(void *);
};
struct vmsvga3d_device {
    struct vmsvga_engine *engine;
    struct vmsvga3d_memory memory;
    struct render3d_extent heap;   /* scratch first, then the service heap */
    struct vmsvga3d_caps caps;
    uint32_t generation, texture_bytes, mode_width, mode_height;
    uint32_t fence, combine, combine_trial;
    bool qualified, probing, locked, lost, context_live;
    bool minimum_passed, fullsize_qualified, fullsize_testing, perspective_qualified;
    uint64_t batches, triangles, copies, dmas, defined, destroyed;
    uint64_t slice_started, qualification_started;
    uint32_t failed_phase, expected, actual, guard_offset;
    uint32_t sample_delta_q16, filter_fraction_bits, precision_samples;
    uint32_t measured_filter_levels[2], precision_phase;
    int32_t precision_error;
    uint32_t sample_count, reference_checksum, hardware_checksum;
    uint32_t max_channel_error, max_depth_error;
    uint64_t fullsize_us;
    const char *refusal;
    struct render3d_service *consumer;
    void (*lost_notify)(void *);
    uint32_t commands[VMSVGA3D_BATCH_WORDS];
};

const struct render3d_backend *vmsvga3d_backend(void);
/* Integer-only IEEE-754 binary32 conversions, round-to-nearest-even. */
uint32_t vmsvga3d_ratio_ieee(uint64_t numerator, uint64_t denominator, bool negative);
uint32_t vmsvga3d_fixed_ieee(int32_t q16);
/* Homogeneous POSITION FLOAT4 + D3DCOLOR + two FLOAT2 texcoords. */
int vmsvga3d_vertex(const struct render3d_request *, unsigned, uint32_t [VMSVGA3D_VERTEX_WORDS]);
/* Screen-to-clip projection (D3D row-vector convention, pixel-centre shift). */
void vmsvga3d_projection(uint32_t width, uint32_t height, uint32_t [16]);
/* Capability gates and devcaps: the bounded SVGA_FIFO_3D_CAPS record parse
 * plus the 3D hardware version, or, on SVGA_CAP_GBOBJECTS hosts, the
 * SVGA_REG_DEV_CAP index/value register. The latter uses the index port:
 * the caller must own the engine (render3d enter). Returns 0 or a negative
 * errno; *reason names the refusing gate. */
int vmsvga3d_read_caps(const struct vmsvga_engine *, struct vmsvga3d_caps *, const char **reason);
uint32_t vmsvga3d_staging_bytes(const struct render3d_resource *);
/* Builds one complete job batch (defines, DMA, state, clear, draw, copy,
 * readback, destroys) into d->commands; pure apart from that buffer. */
int vmsvga3d_encode(struct vmsvga3d_device *, const struct render3d_request *,
                    struct render3d_resource *const [R3D_REFS], unsigned *);
int vmsvga3d_context(struct vmsvga3d_device *, bool define);
int vmsvga3d_mode_heap(const struct vmsvga_engine *, const struct fb_device *, uint32_t,
                       struct render3d_extent *, uint32_t *);
int vmsvga3d_probe(struct vmsvga3d_device *);
int vmsvga3d_fullsize_probe(struct vmsvga3d_device *, struct render3d_service *);
void vmsvga3d_bind_vfs(struct vfs *);
void vmsvga3d_boot_diagnostic(struct vmsvga_engine *, const struct fb_device *);
int vmsvga3d_owner_stop(struct vmsvga_engine *);
int vmsvga3d_owner_attach(struct render3d_service *, const struct render3d_host *, uint32_t);
#endif
