/* SPDX-License-Identifier: GPL-2.0 */
/* Origin: Linux v6.1 drivers/gpu/drm/radeon/radeon_reg.h (selected R100/R200
 * 3D definitions; GPL-2.0, Copyright 2000 ATI Technologies Inc. and VA Linux
 * Systems Inc., notice retained in docs/licenses/radeon2d-NOTICES.txt) and
 * Mesa 7.11.2 src/mesa/drivers/dri/r200/r200_reg.h (R200 register addresses
 * and bits, MIT, same notice file). Line references are in
 * docs/design/f7-family-ati-radeon.md. No CP, GART, TCL or Hyper-Z fields. */
#ifndef CIUKI_RADEON3D_REGS_H
#define CIUKI_RADEON3D_REGS_H
/* Status and caches. */
#define RADEON_RBBM_STATUS                  0x0e40u
#define RADEON_RBBM_FIFOCNT_MASK            0x007fu
#define RADEON_RBBM_ACTIVE                  (1u << 31)
#define RADEON_RB3D_ZCACHE_CTLSTAT          0x3254u
#define RADEON_RB3D_ZC_FLUSH_ALL            0x5u
#define RADEON_RB3D_DSTCACHE_CTLSTAT        0x325cu
#define RADEON_RB3D_DC_FLUSH_ALL            0xfu
#define RADEON_RB3D_DC_BUSY                 (1u << 31)
/* 2D registers reused for clears and the conversion copy (same values as
 * registers.h; the owner restores them after every job). */
#define RADEON_SRC_PITCH_OFFSET             0x1428u
#define RADEON_DST_PITCH_OFFSET             0x142cu
#define RADEON_DP_GUI_MASTER_CNTL           0x146cu
#define RADEON_DP_BRUSH_FRGD_CLR            0x147cu
#define RADEON_DP_WRITE_MSK                 0x16ccu
#define RADEON_DP_CNTL                      0x16c0u
#define RADEON_SRC_Y_X                      0x1434u
#define RADEON_DST_Y_X                      0x1438u
#define RADEON_DST_WIDTH_HEIGHT             0x1598u
#define RADEON_DST_HEIGHT_WIDTH             0x143cu
#define RADEON_GMC_SRC_PITCH_OFFSET_CNTL    (1u << 0)
#define RADEON_GMC_DST_PITCH_OFFSET_CNTL    (1u << 1)
#define RADEON_GMC_CLR_CMP_CNTL_DIS         (1u << 28)
#define RADEON_GMC_BRUSH_SOLID_COLOR        0x000000d0u
#define RADEON_GMC_BRUSH_NONE               (15u << 4)
#define RADEON_GMC_SRC_DSTCOLOR             0x00003000u
#define RADEON_DP_SRC_SOURCE_MEMORY         (2u << 24)
#define RADEON_ROP3_P                       0x00f00000u
#define RADEON_ROP3_S                       0x00cc0000u
#define RADEON_DST_X_LEFT_TO_RIGHT          1u
#define RADEON_DST_Y_TOP_TO_BOTTOM          2u
#define RADEON_DATATYPE_RGB565              4u
#define RADEON_DATATYPE_ARGB8888            6u
/* Common 3D/raster registers (R100 and R200 share these addresses). */
#define RADEON_PP_MISC                      0x1c14u
#define RADEON_REF_ALPHA_MASK               0x000000ffu
#define RADEON_ALPHA_TEST_OP_SHIFT          8
#define RADEON_PP_FOG_COLOR                 0x1c18u
#define RADEON_RB3D_BLENDCNTL               0x1c20u
#define RADEON_COMB_FCN_ADD_CLAMP           (0u << 12)
#define RADEON_SRC_BLEND_SHIFT              16
#define RADEON_DST_BLEND_SHIFT              24
#define RADEON_BLEND_GL_ZERO                32u
#define RADEON_BLEND_GL_ONE                 33u
#define RADEON_BLEND_GL_SRC_COLOR           34u
#define RADEON_BLEND_GL_ONE_MINUS_SRC_COLOR 35u
#define RADEON_BLEND_GL_DST_COLOR           36u
#define RADEON_BLEND_GL_ONE_MINUS_DST_COLOR 37u
#define RADEON_BLEND_GL_SRC_ALPHA           38u
#define RADEON_BLEND_GL_ONE_MINUS_SRC_ALPHA 39u
#define RADEON_BLEND_GL_DST_ALPHA           40u
#define RADEON_BLEND_GL_ONE_MINUS_DST_ALPHA 41u
#define RADEON_RB3D_DEPTHOFFSET             0x1c24u
#define RADEON_RB3D_DEPTHPITCH              0x1c28u
#define RADEON_DEPTHPITCH_MASK              0x00001ff8u
#define RADEON_RB3D_ZSTENCILCNTL            0x1c2cu
#define RADEON_DEPTH_FORMAT_16BIT_INT_Z     (0u << 0)
#define RADEON_Z_TEST_SHIFT                 4
#define RADEON_STENCIL_TEST_ALWAYS          (7u << 12)
#define RADEON_Z_WRITE_ENABLE               (1u << 30)
#define RADEON_PP_CNTL                      0x1c38u
#define RADEON_SCISSOR_ENABLE               (1u << 1)
#define RADEON_TEX_0_ENABLE                 (1u << 4)
#define RADEON_TEX_1_ENABLE                 (1u << 5)
#define RADEON_TEX_BLEND_0_ENABLE           (1u << 12)
#define RADEON_TEX_BLEND_1_ENABLE           (1u << 13)
#define RADEON_ALPHA_TEST_ENABLE            (1u << 23)
#define RADEON_ANTI_ALIAS_NONE              (0u << 24)
#define RADEON_RB3D_CNTL                    0x1c3cu
#define RADEON_ALPHA_BLEND_ENABLE           (1u << 0)
#define RADEON_PLANE_MASK_ENABLE            (1u << 1)
#define RADEON_Z_ENABLE                     (1u << 8)
#define RADEON_COLOR_FORMAT_ARGB8888        (6u << 10)
#define RADEON_RB3D_COLOROFFSET             0x1c40u
#define RADEON_COLOROFFSET_MASK             0xfffffff0u
#define RADEON_RE_WIDTH_HEIGHT              0x1c44u
#define RADEON_RB3D_COLORPITCH              0x1c48u
#define RADEON_COLORPITCH_MASK              0x00001ff8u
#define RADEON_SE_CNTL                      0x1c4cu
#define RADEON_BFACE_SOLID                  (3u << 1)
#define RADEON_FFACE_SOLID                  (3u << 3)
#define RADEON_DIFFUSE_SHADE_GOURAUD        (2u << 8)
#define RADEON_ALPHA_SHADE_GOURAUD          (2u << 10)
#define RADEON_VTX_PIX_CENTER_OGL           (1u << 27)
#define RADEON_ROUND_MODE_ROUND             (1u << 28)
#define RADEON_ROUND_PREC_16TH_PIX          (0u << 30)
#define RADEON_SE_COORD_FMT                 0x1c50u /* R100 only */
#define RADEON_VTX_XY_PRE_MULT_1_OVER_W0    (1u << 0)
#define RADEON_VTX_Z_PRE_MULT_1_OVER_W0     (1u << 1)
#define RADEON_TEX1_W_ROUTING_USE_W0        (0u << 26)
#define R200_RE_CNTL                        0x1c50u /* R200 only */
#define R200_SCISSOR_ENABLE_RE              0x2u
#define R200_PERSPECTIVE_ENABLE             0x8u
#define RADEON_RB3D_STENCILREFMASK          0x1d7cu
#define RADEON_RB3D_ROPCNTL                 0x1d80u
#define RADEON_ROP_COPY                     (12u << 8)
#define RADEON_RB3D_PLANEMASK               0x1d84u
#define RADEON_SE_CNTL_STATUS               0x2140u /* R200: SE_VAP_CNTL_STATUS */
#define RADEON_TCL_BYPASS                   (1u << 8)
#define RADEON_SE_VF_CNTL                   0x2084u
#define RADEON_VF_PRIM_TYPE_TRIANGLE_LIST   4u
#define RADEON_VF_PRIM_WALK_DATA            (3u << 4)
#define RADEON_VF_RADEON_MODE               (1u << 8)
#define RADEON_VF_NUM_VERTICES_SHIFT        16
#define RADEON_SE_PORT_DATA0                0x2000u
#define RADEON_RE_TOP_LEFT                  0x26c0u
#define RADEON_RE_MISC                      0x26c4u
#define RADEON_STIPPLE_BIG_BIT_ORDER        (1u << 16)
/* R100 vertex format and textures. */
#define RADEON_SE_VTX_FMT                   0x2080u
#define RADEON_SE_VTX_FMT_W0                0x00000001u
#define RADEON_SE_VTX_FMT_PKCOLOR           0x00000008u
#define RADEON_SE_VTX_FMT_ST0               0x00000080u
#define RADEON_SE_VTX_FMT_ST1               0x00000100u
#define RADEON_SE_VTX_FMT_Z                 0x80000000u
#define RADEON_PP_TXFILTER_0                0x1c54u
#define RADEON_PP_TXFORMAT_0                0x1c58u
#define RADEON_PP_TXOFFSET_0                0x1c5cu
#define RADEON_PP_TXCBLEND_0                0x1c60u
#define RADEON_PP_TXABLEND_0                0x1c64u
#define RADEON_PP_TFACTOR_0                 0x1c68u
#define RADEON_PP_TEX_UNIT_STRIDE           0x18u
#define RADEON_PP_TEX_SIZE_0                0x1d04u
#define RADEON_PP_TEX_PITCH_0               0x1d08u
#define RADEON_PP_TEX_STRIDE                0x8u
#define RADEON_PP_BORDER_COLOR_0            0x1d40u
#define RADEON_MAG_FILTER_LINEAR            (1u << 0)
#define RADEON_MIN_FILTER_SHIFT             1
#define RADEON_MIN_FILTER_NEAREST           0u
#define RADEON_MIN_FILTER_LINEAR            1u
#define RADEON_MIN_FILTER_NEAREST_MIP_NEAREST 2u
#define RADEON_MIN_FILTER_LINEAR_MIP_NEAREST 6u
#define RADEON_MIN_FILTER_LINEAR_MIP_LINEAR 7u
#define RADEON_MAX_MIP_LEVEL_SHIFT          16
#define RADEON_CLAMP_S_WRAP                 (0u << 23)
#define RADEON_CLAMP_S_CLAMP_LAST           (2u << 23)
#define RADEON_CLAMP_T_WRAP                 (0u << 27)
#define RADEON_CLAMP_T_CLAMP_LAST           (2u << 27)
#define RADEON_TXFORMAT_RGB565              (4u << 0)
#define RADEON_TXFORMAT_ARGB4444            (5u << 0)
#define RADEON_TXFORMAT_ARGB8888            (6u << 0)
#define RADEON_TXFORMAT_ALPHA_IN_MAP        (1u << 6)
#define RADEON_TXFORMAT_WIDTH_SHIFT         8
#define RADEON_TXFORMAT_HEIGHT_SHIFT        12
#define RADEON_TXFORMAT_ST_ROUTE_SHIFT      24
#define RADEON_TXFORMAT_PERSPECTIVE_ENABLE  (1u << 31)
#define RADEON_TEX_VSIZE_SHIFT              16
/* R100 combiner arguments (PP_TXCBLEND colour 5-bit slots at 0/5/10). */
#define RADEON_COLOR_ARG_ZERO               0u
#define RADEON_COLOR_ARG_CURRENT_COLOR      2u
#define RADEON_COLOR_ARG_CURRENT_ALPHA      3u
#define RADEON_COLOR_ARG_T0_COLOR           10u
#define RADEON_COLOR_ARG_T0_ALPHA           11u
#define RADEON_COLOR_ARG_T1_COLOR           12u
#define RADEON_COLOR_ARG_T1_ALPHA           13u
#define RADEON_COLOR_ARG_B_SHIFT            5
#define RADEON_COLOR_ARG_C_SHIFT            10
#define RADEON_COMP_ARG_B                   (1u << 16)
#define RADEON_BLEND_CTL_ADD                (0u << 18)
#define RADEON_BLEND_CTL_BLEND              (3u << 18)
#define RADEON_SCALE_1X                     (0u << 21)
#define RADEON_CLAMP_TX                     (1u << 23)
/* R100 alpha arguments (PP_TXABLEND 4-bit slots at 0/4/8). */
#define RADEON_ALPHA_ARG_ZERO               0u
#define RADEON_ALPHA_ARG_CURRENT_ALPHA      1u
#define RADEON_ALPHA_ARG_T0_ALPHA           5u
#define RADEON_ALPHA_ARG_T1_ALPHA           6u
#define RADEON_ALPHA_ARG_B_SHIFT            4
#define RADEON_ALPHA_ARG_C_SHIFT            8
/* R200 vertex path. */
#define R200_SE_VAP_CNTL                    0x2080u
#define R200_VAP_VF_MAX_VTX_NUM             (9u << 18)
#define R200_SE_VTX_FMT_0                   0x2088u
#define R200_VTX_Z0                         (1u << 0)
#define R200_VTX_W0                         (1u << 1)
#define R200_VTX_PK_RGBA                    1u
#define R200_VTX_COLOR_0_SHIFT              11
#define R200_SE_VTX_FMT_1                   0x208cu
#define R200_VTX_TEX0_COMP_CNT_SHIFT        0
#define R200_VTX_TEX1_COMP_CNT_SHIFT        3
#define R200_SE_VTE_CNTL                    0x20b0u
#define R200_VTX_XY_FMT                     0x00000100u
#define R200_VTX_Z_FMT                      0x00000200u
#define R200_SE_VTX_STATE_CNTL              0x2180u
#define R200_RE_SCISSOR_TL_0                0x1cd8u
#define R200_RE_SCISSOR_BR_0                0x1cdcu
#define R200_RB3D_DEPTHXY_OFFSET            0x1d60u
#define R200_RE_AUX_SCISSOR_CNTL            0x26f0u
/* R200 textures (unit stride 0x20 for filter/format, 0x18 for offsets,
 * 0x10 for combiners). */
#define R200_PP_TXFILTER_0                  0x2c00u
#define R200_PP_TXFORMAT_0                  0x2c04u
#define R200_PP_TXFORMAT_X_0                0x2c08u
#define R200_PP_TXSIZE_0                    0x2c0cu
#define R200_PP_TXPITCH_0                   0x2c10u
#define R200_PP_BORDER_COLOR_0              0x2c14u
#define R200_PP_TXMULTI_CTL_0               0x2c1cu
#define R200_PP_TEX_UNIT_STRIDE             0x20u
#define R200_PP_CNTL_X                      0x2cc4u
#define R200_PP_TXOFFSET_0                  0x2d00u
#define R200_PP_TXOFFSET_STRIDE             0x18u
#define R200_PP_TFACTOR_0                   0x2ee0u
#define R200_PP_TXCBLEND_0                  0x2f00u
#define R200_PP_TXCBLEND2_0                 0x2f04u
#define R200_PP_TXABLEND_0                  0x2f08u
#define R200_PP_TXABLEND2_0                 0x2f0cu
#define R200_PP_BLEND_STRIDE                0x10u
#define R200_TEXCOORD_NONPROJ               (0u << 16)
#define R200_LOD_BIAS_CORRECTION            0x00600000u
/* R200 combiner arguments (5-bit slots at 0/5/10 for colour and alpha). */
#define R200_TXC_ARG_ZERO                   0u
#define R200_TXC_ARG_DIFFUSE_COLOR          4u
#define R200_TXC_ARG_DIFFUSE_ALPHA          5u
#define R200_TXC_ARG_R0_COLOR               10u
#define R200_TXC_ARG_R0_ALPHA               11u
#define R200_TXC_ARG_R1_COLOR               12u
#define R200_TXC_ARG_R1_ALPHA               13u
#define R200_TXA_ARG_ZERO                   0u
#define R200_TXA_ARG_DIFFUSE_ALPHA          4u
#define R200_TXA_ARG_R0_ALPHA               10u
#define R200_TXA_ARG_R1_ALPHA               12u
#define R200_TX_ARG_B_SHIFT                 5
#define R200_TX_ARG_C_SHIFT                 10
#define R200_TX_COMP_ARG_B                  (1u << 20)
#define R200_TX_OP_MADD                     (0u << 28)
#define R200_TX_OP_LERP                     (3u << 28)
#define R200_TX_CLAMP_0_1                   (1u << 12)
#define R200_TX_OUTPUT_REG_R0               (1u << 16)
#endif
