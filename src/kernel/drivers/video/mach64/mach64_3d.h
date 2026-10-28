/* Private Mach64 (Rage Pro / LT Pro / Mobility) 3D backend, f7-27.
 * Register programming: docs/design/f7-family-ati-mach64.md (pinned sources).
 * No client ever sees a VRAM address or a register. GPL-2.0-only. */
#ifndef CIUKI_MACH64_3D_H
#define CIUKI_MACH64_3D_H
#include "../render3d.h"
#include "engine.h"
struct vfs;
#define MACH64_3D_FEATURES (R3D_FEATURES | R3D_TEXTURE1)
#define MACH64_3D_TEXTURE_MIN (256u * 1024u)
/* Q2 scratch: one 256-square and one 128-square level plus their guards. */
#define MACH64_3D_FULL_TEXTURES (256u * 256u * 4u + 128u * 128u * 4u + 4u * 4096u)
#define MACH64_3D_MAX_TEXTURE 1024u      /* mach64_context.c:195-206, log2 10 */
#define MACH64_3D_QUALIFIED_TEXTURE 256u /* largest size the Q3 phases sample */
#define MACH64_3D_TRIANGLE_WRITES 31u
#define MACH64_3D_STATE_WRITES 32u
#define MACH64_3D_BATCH_WRITES (256u * MACH64_3D_TRIANGLE_WRITES)
#define MACH64_3D_SAVED 17u
#define MACH64_3D_PHASES 12u
#define MACH64_3D_QUALIFY_US (30u * 1000000u)
_Static_assert(MACH64_3D_BATCH_WRITES+MACH64_3D_STATE_WRITES<=M64_3D_WRITE_LIMIT,
    "Mach64 staging must fit the bounded transport");
/* Offsets are absolute VRAM bytes inside the claimed heap. */
struct mach64_3d_memory {
    void *ctx;
    uint32_t (*read)(void *, uint32_t, unsigned);
    void (*write)(void *, uint32_t, uint32_t, unsigned);
    int (*enter)(void *, uint32_t);
    void (*leave)(void *);
};
struct mach64_3d_saved {
    uint32_t value[MACH64_3D_SAVED];
    uint32_t texture[11];
    unsigned level;
    bool valid;
};
struct mach64_3d_device {
    struct m64_engine *engine;
    struct mach64_3d_memory memory;
    struct render3d_extent heap;
    uint32_t generation, texture_bytes, mode_width, mode_height;
    bool qualified, probing, locked, lost, minimum_qualified;
    bool fullsize_qualified, fullsize_testing;
    bool perspective_qualified, alpha_qualified, blend_qualified;
    uint32_t sample_delta_q16, filter_fraction_bits, precision_samples;
    uint32_t measured_filter_levels[2], precision_phase;
    int32_t precision_error, alpha_error, blend_error;
    uint32_t selfchecks;
    int32_t selfcheck_error;
    uint32_t sample_count, reference_checksum, hardware_checksum;
    uint32_t max_channel_error, max_depth_error, coverage_mismatches;
    uint64_t fullsize_us, qualification_started;
    uint64_t packets, triangles, copies, clears;
    uint32_t failed_phase, failed_register, expected, actual, guard_offset;
    struct m64_budget budget;
    struct mach64_3d_saved saved;
    uint32_t written_textures; /* TEX_n_OFF bitmask written in this window */
    struct m64_write writes[MACH64_3D_BATCH_WRITES + MACH64_3D_STATE_WRITES];
    struct render3d_service *consumer;
    void (*lost_notify)(void *);
};
/* Pure encoders, host-tested against the independent register decoder. */
uint32_t mach64_3d_fixed_ieee(uint32_t, unsigned);
uint32_t mach64_3d_ratio_ieee(uint64_t, uint64_t);
bool mach64_3d_on_grid(const struct render3d_vertex *);
int mach64_3d_triangle(const struct render3d_request *, unsigned, bool,
                       struct m64_write [MACH64_3D_TRIANGLE_WRITES], unsigned *);
uint32_t mach64_3d_pitch(const struct render3d_resource *);
uint32_t mach64_3d_padded_bytes(const struct render3d_resource *);
int mach64_3d_mode_heap(const struct m64_engine *, const struct fb_device *,
                        struct render3d_extent *, uint32_t *minimum, uint32_t *full);
const struct render3d_backend *mach64_3d_backend(void);
/* Boot qualification Q1 (minimum) and Q2/Q3 (mode size and precision).
 * Entered only after claims, mapping and a fresh durable marker. */
int mach64_3d_probe(struct mach64_3d_device *);
int mach64_3d_fullsize_probe(struct mach64_3d_device *, struct render3d_service *);
void mach64_3d_bind_vfs(struct vfs *);
void mach64_3d_boot_diagnostic(struct m64_engine *, const struct fb_device *);
int mach64_3d_owner_stop(struct m64_engine *);
int mach64_3d_owner_attach(struct render3d_service *, const struct render3d_host *, uint32_t);
#endif
