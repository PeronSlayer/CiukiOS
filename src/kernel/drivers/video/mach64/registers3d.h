/* Mach64 setup-engine register map used by the f7-27 3D backend.
 * Independently written from the pinned sources listed in
 * docs/design/f7-family-ati-mach64.md; no upstream text is copied.
 * Mesa 7.11.2 mach64_reg.h (and the DRM's identical mach64_drv.h table)
 * number registers from the block-1 base: block 1 is 0x000-0x3ff and block 0
 * is 0x400-0x7ff (mach64_reg.h:42-50). The 2D driver's BAR2 page places block
 * 0 at 0xc00 (registers.h), so every page offset is the Mesa offset + 0x800
 * (X.Org atipreinit.c:1939, Block1Base = Block0Base - 0x400).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_MACH64_REGISTERS3D_H
#define CIUKI_MACH64_REGISTERS3D_H
#define M64_PAGE(mesa) ((mesa) + 0x800u)
/* Block 1: triangle setup engine (mach64_reg.h:214-215,283,343-372). */
#define M64_VERTEX_1_S          M64_PAGE(0x240u)
#define M64_VERTEX_2_S          M64_PAGE(0x260u)
#define M64_VERTEX_3_S          M64_PAGE(0x280u)
#define M64_VERTEX_STRIDE       0x20u
/* S,T,W,SPEC_ARGB,Z,ARGB,X_Y at +0,4,8,0xc,0x10,0x14,0x18 of each vertex. */
#define M64_ONE_OVER_AREA       M64_PAGE(0x29cu) /* follows VERTEX_3_X_Y */
#define M64_VERTEX_3_SECONDARY  M64_PAGE(0x2a0u) /* S,T,W */
#define M64_ONE_OVER_AREA_UC    M64_PAGE(0x300u)
#define M64_SETUP_CNTL          M64_PAGE(0x304u)
#define M64_VERTEX_1_SECONDARY  M64_PAGE(0x328u) /* S,T,W */
#define M64_VERTEX_2_SECONDARY  M64_PAGE(0x334u) /* S,T,W */
/* Block 0 (mach64_reg.h:72-387; the 2D names in registers.h agree). */
#define M64_BUS_CNTL            M64_PAGE(0x4a0u)
#define M64_BUS_EXT_REG_EN      0x08000000u      /* mach64_drv.h:215 */
#define M64_CONFIG_CHIP_ID_LOW  0x4e0u           /* aux block 0 at BAR2+0x400 */
#define M64_DST_OFF_PITCH       M64_PAGE(0x500u)
#define M64_DST_Y_X             M64_PAGE(0x50cu)
#define M64_DST_HEIGHT_WIDTH    M64_PAGE(0x518u)
#define M64_DST_CNTL            M64_PAGE(0x530u)
#define M64_Z_OFF_PITCH         M64_PAGE(0x548u)
#define M64_Z_CNTL              M64_PAGE(0x54cu)
#define M64_ALPHA_TST_CNTL      M64_PAGE(0x550u)
#define M64_SRC_OFF_PITCH       M64_PAGE(0x580u)
#define M64_SRC_Y_X             M64_PAGE(0x58cu)
#define M64_SRC_HEIGHT1_WIDTH1  M64_PAGE(0x598u)
#define M64_TEX_0_OFF           M64_PAGE(0x5c0u) /* TEX_n_OFF = +4n, n<=10 */
#define M64_SCALE_3D_CNTL       M64_PAGE(0x5fcu)
#define M64_SC_LEFT             M64_PAGE(0x6a0u)
#define M64_SC_RIGHT            M64_PAGE(0x6a4u)
#define M64_SC_LEFT_RIGHT       M64_PAGE(0x6a8u)
#define M64_SC_TOP              M64_PAGE(0x6acu)
#define M64_SC_BOTTOM           M64_PAGE(0x6b0u)
#define M64_SC_TOP_BOTTOM       M64_PAGE(0x6b4u)
#define M64_DP_FRGD_CLR         M64_PAGE(0x6c4u)
#define M64_DP_WRITE_MASK       M64_PAGE(0x6c8u)
#define M64_DP_PIX_WIDTH        M64_PAGE(0x6d0u)
#define M64_DP_MIX              M64_PAGE(0x6d4u)
#define M64_DP_SRC              M64_PAGE(0x6d8u)
#define M64_CLR_CMP_CNTL        M64_PAGE(0x708u)
#define M64_FIFO_STAT           M64_PAGE(0x710u)
#define M64_GUI_STAT            M64_PAGE(0x738u)
#define M64_TEX_SIZE_PITCH      M64_PAGE(0x770u)
#define M64_TEX_CNTL            M64_PAGE(0x774u)
#define M64_SECONDARY_TEX_OFF   M64_PAGE(0x778u)
#define M64_CONFIG_CHIP_ID      M64_PAGE(0x4e0u)
/* Field values (mach64_reg.h line numbers in the design document). */
#define M64_DATATYPE_RGB565     4u
#define M64_DATATYPE_ARGB8888   6u
#define M64_Z_EN                0x1u
#define M64_Z_MASK_EN           0x100u
#define M64_ALPHA_TEST_EN       0x1u
#define M64_ALPHA_DST_ZERO      (0u << 8)
#define M64_ALPHA_DST_SRCALPHA  (4u << 8)
#define M64_ALPHA_DST_DSTALPHA  (6u << 8)
#define M64_SCALE_BASE          0x0100001bu /* dynamic expand, 2D dither table, reset, round, mip off */
#define M64_FCN_TEXTURE         (2u << 6)
#define M64_FCN_SHADE           (3u << 6)
#define M64_TEX_CACHE_SPLIT     (1u << 9)
#define M64_ALPHA_FOG_EN_ALPHA  (1u << 11)
#define M64_BLEND_SRC_ONE       (1u << 16)
#define M64_TEX_LIGHT_REPLACE   (0u << 22)
#define M64_TEX_LIGHT_MODULATE  (1u << 22)
#define M64_TEX_LIGHT_DECAL     (2u << 22)
#define M64_BILINEAR_TEX_EN     (1u << 25)
#define M64_TEX_BLEND_LINEAR    (2u << 26)
#define M64_TEX_BLEND_TRILINEAR (3u << 26)
#define M64_TEX_MAP_AEN         (1u << 30)
#define M64_TEX_CNTL_BASE       0x40800200u /* combine modulate, cache flush, 4K cache */
#define M64_TEX_CACHE_FLUSH     (1u << 23)
#define M64_TEXTURE_COMPOSITE   (1u << 8)
#define M64_COMP_BLEND_BILINEAR (1u << 11)
#define M64_COMP_FILTER_BILINEAR (1u << 12)
#define M64_COMP_ALPHA          (1u << 13)
#define M64_TEXTURE_CLAMP_S     (1u << 17)
#define M64_TEXTURE_CLAMP_T     (1u << 18)
#define M64_SEC_TEX_CLAMP_S     (1u << 24)
#define M64_SEC_TEX_CLAMP_T     (1u << 25)
#define M64_SECONDARY_STW       (1u << 31)
#define M64_DP_SRC_3D           0x505u     /* background/foreground 3D, mono one */
#define M64_DP_MIX_3D           0x70007u   /* background/foreground source */
#define M64_DP_SRC_FILL         0x101u     /* foreground colour, mono one */
#define M64_DP_SRC_BLIT         0x300u     /* background colour, foreground blit */
#define M64_DP_MIX_COPY         0x70003u   /* background destination, foreground source */
#define M64_DST_2D              0x23u      /* left-to-right, top-to-bottom, last pel */
#define M64_DST_3D              0x3u
#endif
