/* Host-programmed trapezoid 3D backend for the G200/G400 family. Register
 * meanings come from the pinned sources in docs/design/f7-family-matrox-g.md;
 * no silicon qualification claim. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_MATROX3D_PRIVATE_H
#define CIUKI_MATROX3D_PRIVATE_H
#include "../render3d.h"
#include "matrox.h"
#define MATROX3D_TEXTURE_MIN (256u * 1024u)
#define MATROX3D_BATCH_WORDS 96u
#define MATROX3D_ENCODINGS 2u
/* 3D register offsets beyond registers.h (Mesa mgaregs.h, Linux mga_drv.h). */
#define M3D_ZORG 0x1C0C
#define M3D_AR0 0x1C60
#define M3D_AR2 0x1C68
#define M3D_AR5 0x1C74
#define M3D_AR6 0x1C78
#define M3D_SGN 0x1C58
#define M3D_LEN 0x1C5C
#define M3D_YDST 0x1C90
#define M3D_FXLEFT 0x1CA8
#define M3D_FXRIGHT 0x1CAC
#define M3D_DR0 0x1CC0
#define M3D_DR2 0x1CC8
#define M3D_DR3 0x1CCC
#define M3D_DR4 0x1CD0
#define M3D_DR6 0x1CD8
#define M3D_DR7 0x1CDC
#define M3D_DR8 0x1CE0
#define M3D_DR10 0x1CE8
#define M3D_DR11 0x1CEC
#define M3D_DR12 0x1CF0
#define M3D_DR14 0x1CF8
#define M3D_DR15 0x1CFC
#define M3D_TMR0 0x2C00
#define M3D_TEXORG 0x2C24
#define M3D_TEXWIDTH 0x2C28
#define M3D_TEXHEIGHT 0x2C2C
#define M3D_TEXCTL 0x2C30
#define M3D_TEXTRANS 0x2C34
#define M3D_TEXTRANSHIGH 0x2C38
#define M3D_TEXCTL2 0x2C3C
#define M3D_TEXFILTER 0x2C58
#define M3D_TEXBORDERCOL 0x2C5C
#define M3D_ALPHASTART 0x2C70
#define M3D_ALPHAXINC 0x2C74
#define M3D_ALPHAYINC 0x2C78
#define M3D_ALPHACTRL 0x2C7C
#define M3D_TEXORG1 0x2CA4
#define M3D_TEXORG2 0x2CA8
#define M3D_TEXORG3 0x2CAC
#define M3D_TEXORG4 0x2CB0
#define M3D_TDUALSTAGE0 0x2CF8
#define M3D_TDUALSTAGE1 0x2CFC
/* SGN sub-pixel fields (G400 spec page 3-172). */
#define M3D_SGN_BL (1u<<11)
#define M3D_SGN_SBE (1u<<12)
#define M3D_SGN_SSE (1u<<13)
#define M3D_SGN_US (1u<<14)
#define M3D_SGN_CENTERSNAP (1u<<15)
#define M3D_SGN_SUBPIXEN (1u<<16)
/* One register write; a burst is a bounded list of pairs. */
struct matrox3d_write { uint16_t reg; uint32_t value; };
/* Per-trapezoid setup produced by validation and emitted by execute. */
struct matrox3d_trap {
    uint32_t sgn, ydst, fxleft, fxright, ar0, ar2, ar5, ar6, len;
    uint32_t dr0, dr2, dr3, dr[9], alpha[3], tmr[2][9];
    bool bottom;
};
/* Absolute VRAM offsets; only the owner creates these callbacks. */
struct matrox3d_memory {
    void *ctx;
    uint32_t (*read)(void *, uint32_t, unsigned);
    void (*write)(void *, uint32_t, uint32_t, unsigned);
    int (*enter)(void *, uint32_t);
    void (*leave)(void *);
};
struct matrox3d_device {
    struct matrox_engine *engine;
    struct matrox3d_memory memory;
    struct render3d_extent heap;
    uint32_t generation;
    uint32_t texture_bytes;
    bool g400, qualified, probing, locked, lost;
    bool fullsize_qualified, fullsize_testing, perspective_qualified;
    unsigned encoding;
    uint32_t sample_delta_q16, filter_fraction_bits, precision_samples;
    uint32_t measured_filter_levels[2];
    int32_t precision_error;
    uint32_t precision_phase, precision_x, precision_y;
    uint32_t mode_width, mode_height;
    uint32_t sample_count, reference_checksum, hardware_checksum;
    uint32_t max_channel_error, max_depth_error;
    uint64_t fullsize_us;
    struct render3d_service *consumer;
    void (*lost_notify)(void *);
    uint64_t writes, triangles, copies, clears;
    uint64_t slice_started;
    uint32_t failed_phase, expected, actual, guard_offset;
};
/* Pure setup for one triangle (first vertex index), both encodings. */
int matrox3d_setup(const struct render3d_request *, unsigned first, unsigned encoding,
                   struct matrox3d_trap out[2], unsigned *count);
uint32_t matrox3d_texture_bytes(const struct render3d_resource *, bool g400);
unsigned matrox3d_texture_levels(const struct render3d_resource *, bool g400);
int matrox3d_mode_heap(const struct matrox_engine *, const struct fb_device *, uint32_t vram,
                       uint32_t cursor_offset, struct render3d_extent *, uint32_t *required);
const struct render3d_backend *matrox3d_backend(void);
int matrox3d_probe(struct matrox3d_device *);
int matrox3d_fullsize_probe(struct matrox3d_device *, struct render3d_service *);
void matrox3d_bind_vfs(struct vfs *);
void matrox3d_boot_diagnostic(struct matrox_engine *, const struct fb_device *);
int matrox3d_owner_stop(struct matrox_engine *);
int matrox3d_owner_attach(struct render3d_service *, const struct render3d_host *, uint32_t);
#endif
