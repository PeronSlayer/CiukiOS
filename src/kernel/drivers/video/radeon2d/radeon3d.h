/* Radeon R100/R200 fixed-function 3D backend, private to the family owner.
 * Source-backed register programming (docs/design/f7-family-ati-radeon.md);
 * no silicon qualification claim. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_RADEON3D_PRIVATE_H
#define CIUKI_RADEON3D_PRIVATE_H
#include "../render3d.h"
#include "engine.h"
#define RADEON3D_TEXTURE_MIN (256u * 1024u)
#define RADEON3D_GUARD 4096u
#define RADEON3D_WORDS_PER_TRIANGLE 28u /* SE_VF_CNTL + 3 x 9 vertex dwords */
#define RADEON3D_STATE_MAX 64u
/* Absolute VRAM byte offsets; only the owner creates these callbacks. */
struct radeon3d_memory {
    void *ctx;
    uint32_t (*read)(void *, uint32_t, unsigned);
    void (*write)(void *, uint32_t, uint32_t, unsigned);
    int (*enter)(void *, uint32_t);
    void (*leave)(void *);
};
struct radeon3d_device {
    struct radeon_engine *engine;
    struct radeon3d_memory memory;
    struct render3d_extent heap;
    uint32_t generation, texture_bytes;
    bool r200, tcl_bypass;
    bool qualified, probing, locked, lost;
    bool fullsize_qualified, fullsize_testing, perspective_qualified;
    uint32_t sample_delta_q16, filter_fraction_bits, precision_samples;
    uint32_t measured_filter_levels[2];
    int32_t precision_error;
    uint32_t precision_phase;
    uint32_t mode_width, mode_height;
    uint32_t sample_count, reference_checksum, hardware_checksum;
    uint32_t max_channel_error, max_depth_error;
    uint64_t fullsize_us;
    struct render3d_service *consumer;
    void (*lost_notify)(void *);
    uint64_t writes, triangles, copies, fifo_waits;
    uint64_t slice_started;
    uint32_t failed_phase, expected, actual, guard_offset;
    uint32_t saved[RADEON3D_STATE_MAX], saved_2d[6];
};
/* Vertex serialization fixtures (no hardware access). */
uint32_t radeon3d_fixed_ieee(uint32_t value, unsigned fractional);
uint32_t radeon3d_signed_ieee(int32_t value);
uint32_t radeon3d_depth_ieee(uint32_t depth);
int radeon3d_triangle_words(const struct render3d_request *, unsigned first, bool r200,
                            uint32_t words[RADEON3D_WORDS_PER_TRIANGLE]);
/* Mesa miptree layout: 32-byte aligned rows, consecutive levels. */
uint32_t radeon3d_texture_bytes(const struct render3d_resource *);
int radeon3d_mip_offset(const struct render3d_resource *, unsigned level, unsigned x, unsigned y, uint32_t *);
uint32_t radeon3d_color_pitch(uint32_t width);
uint32_t radeon3d_depth_pitch(uint32_t width);
int radeon3d_mode_heap(const struct radeon_engine *, const struct fb_device *, uint32_t vram,
                       uint32_t cursor_offset, struct render3d_extent *, uint32_t *required);
const struct render3d_backend *radeon3d_backend(void);
/* Positive qualification, entered only after claims and a fresh marker. */
int radeon3d_probe(struct radeon3d_device *);
int radeon3d_fullsize_probe(struct radeon3d_device *, struct render3d_service *reference);
/* Register images saved/restored around every job (R100 / R200 lists). */
unsigned radeon3d_state_registers(bool r200, const uint16_t **registers);
void radeon3d_bind_vfs(struct vfs *);
void radeon3d_boot_diagnostic(struct radeon_engine *, const struct fb_device *);
int radeon3d_owner_stop(struct radeon_engine *);
int radeon3d_owner_attach(struct render3d_service *, const struct render3d_host *, uint32_t);
#endif
