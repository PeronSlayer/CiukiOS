/* Source-backed transport/layout helpers; no silicon qualification claim.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SAVAGE3D_PRIVATE_H
#define CIUKI_SAVAGE3D_PRIVATE_H
#include "../render3d.h"
#include "savage2d.h"
uint32_t savage3d_fixed_ieee(uint32_t, unsigned);
/* Canonical serializer fixture; hardware variant reverses integer depth. */
int savage3d_triangle_packet(const struct render3d_vertex [3], uint32_t [22]);
int savage3d_hw_triangle_packet(const struct render3d_vertex [3], uint32_t [22]);
/* Pro/SuperSavage texture subtiles are 8x8 (Savage4 differs). The legacy
 * pack wrapper covers one ARGB8888 level; mip helpers cover tiny tails. */
int savage3d_texture_offset(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *);
int savage3d_pack_texture(void *, size_t, const uint32_t *, uint32_t, uint32_t);
int savage3d_mip_offset(const struct render3d_resource *, unsigned, unsigned, unsigned, uint32_t *);
uint32_t savage3d_texture_bytes(const struct render3d_resource *);
/* Checked plan only; callers must subtract every registry reservation first. */
struct savage3d_plan {
    struct render3d_extent color[2], depth, texture;
    uint32_t color_pitch, depth_pitch, free_bytes, largest_bytes;
};
int savage3d_plan_vram(uint32_t, const struct render3d_extent *, unsigned,
                      uint32_t, uint32_t, struct savage3d_plan *);
const struct render3d_backend *savage3d_backend(void);
#define SAVAGE3D_ALIAS_BYTES (1024u * 8192u)
#define SAVAGE3D_BATCH_WORDS (256u * 28u)
#define SAVAGE3D_TEXTURE_MIN (256u * 1024u)
#define SAVAGE3D_PROBE_TOTAL_US 5000000u
#define SAVAGE3D_PROBE_PHASE_US 500000u
#define SAVAGE3D_PROBE_MARKER_US 1000000u
/* One total normal-job readback deadline, including all attachment guards. */
#define SAVAGE3D_READBACK_US 200000u
#define SAVAGE3D_REVALIDATE_US 50000u
#define SAVAGE3D_MODE_READY_US 2000000u
/* Linear offsets are absolute VRAM bytes; aperture offsets are alias-local.
 * Only the owner creates these callbacks/mappings. No client sees them. */
struct savage3d_memory {
    void *ctx;
    uint32_t (*read)(void *, bool aperture, uint32_t, unsigned);
    void (*write)(void *, bool aperture, uint32_t, uint32_t, unsigned);
    int (*enter)(void *, uint32_t);
    void (*leave)(void *);
};
struct savage3d_device {
    struct savage2d *engine;
    struct savage3d_memory memory;
    /* Optional, between bounded windows of an entered job (memory.ctx):
     * releases only the presenter mutex and yields; the claim is kept. */
    void (*job_yield)(void *);
    struct render3d_extent heap;
    uint32_t generation;
    uint32_t texture_bytes;
    bool qualified, probing, locked, lost;
    bool fullsize_qualified, fullsize_testing, perspective_qualified;
    bool depth24_qualified, depth24_testing, color565_qualified, color565_testing;
    uint32_t sample_delta_q16, filter_fraction_bits, precision_samples;
    uint32_t measured_filter_levels[2];
    int32_t precision_error;
    uint32_t precision_phase;
    uint32_t mode_width, mode_height;
    uint32_t sample_count, reference_checksum, hardware_checksum;
    uint32_t max_channel_error, max_depth_error;
    uint64_t fullsize_us;
    uint32_t commands[SAVAGE3D_BATCH_WORDS];
    struct render3d_service *consumer;
    void (*lost_notify)(void *);
    uint64_t packets, triangles, copies, scanouts;
    uint64_t slice_started, readback_until;
    uint32_t failed_phase, expected, actual, guard_offset;
    /* Private qualification callbacks: called only outside engine ownership. */
    void *probe_ctx;
    int (*probe_checkpoint)(void *);
    void (*probe_phase)(void *, const char *, unsigned, bool, int);
    bool probe_transfers;
    int (*reserve)(void *);
    void (*unreserve)(void *);
    void (*detach_owner)(void *);
};
int savage3d_mode_heap(const struct savage2d *, const struct fb_device *, uint32_t,
                       struct render3d_extent *, uint32_t *required);
/* Source-complete positive qualification, entered only after claims and a
 * fresh durable operation marker. All scratch is inside device.heap. */
int savage3d_probe(struct savage3d_device *);
int savage3d_color565_probe(struct savage3d_device *, struct render3d_service *);
int savage3d_depth24_probe(struct savage3d_device *, struct render3d_service *);
int savage3d_fullsize_probe(struct savage3d_device *, struct render3d_service *);
void savage3d_bind_vfs(struct vfs *);
void savage3d_boot_diagnostic(struct savage2d *, const struct fb_device *);
void savage3d_desktop_presented(uint32_t);
int savage3d_owner_stop(struct savage2d *);
int savage3d_owner_prepare(uint32_t);
int savage3d_owner_attach(struct render3d_service *, const struct render3d_host *, uint32_t);
#endif
