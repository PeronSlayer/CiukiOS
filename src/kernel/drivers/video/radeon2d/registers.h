/*
 * Copyright 2000 ATI Technologies Inc., Markham, Ontario, and
 *                VA Linux Systems Inc., Fremont, California.
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation on the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT.  IN NO EVENT SHALL ATI, VA LINUX SYSTEMS AND/OR
 * THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* SPDX-License-Identifier: GPL-2.0 */
/* Origin: Linux v6.12, include/video/radeon.h; selected register definitions. */
#ifndef CIUKI_RADEON2D_REGISTERS_H
#define CIUKI_RADEON2D_REGISTERS_H
#define RADEON_REGSIZE			0x4000
#define CRTC_GEN_CNTL                          0x0050
#define CRTC_OFFSET                            0x0224
#define CRTC_OFFSET_CNTL                       0x0228
#define CRTC_PITCH                             0x022C
#define MC_FB_LOCATION                         0x0148
#define CNFG_MEMSIZE                           0x00F8
#define SURFACE_CNTL                           0x0B00
#define RB3D_CNTL			       0x1C3C
#define RBBM_STATUS                            0x0E40
#define DSTCACHE_CTLSTAT                       0x1714
#define RB2D_DC_FLUSH_ALL			   (RB2D_DC_FLUSH_2D | RB2D_DC_FREE_2D)
#define RB2D_DC_FLUSH_2D			   (1 << 0)
#define RB2D_DC_FREE_2D				   (1 << 2)
#define RB2D_DC_BUSY                                (1u << 31)
#define RB2D_DSTCACHE_MODE		       0x3428
#define RB2D_DC_AUTOFLUSH_ENABLE (1u << 8)
#define RB2D_DC_DC_DISABLE_IGNORE_PE (1u << 17)
#define DEFAULT_PITCH_OFFSET                   0x16E0
#define DST_PITCH_OFFSET                       0x142C
#define SRC_PITCH_OFFSET                       0x1428
#define DEFAULT_SC_TOP_LEFT                    0x16EC
#define DEFAULT_SC_BOTTOM_RIGHT                0x16E8
#define DEFAULT_SC_RIGHT_MAX			   (0x1fff << 0)
#define DEFAULT_SC_BOTTOM_MAX			   (0x1fff << 16)
#define DP_GUI_MASTER_CNTL                     0x146C
#define DP_BRUSH_FRGD_CLR                      0x147C
#define DP_WRITE_MSK                           0x16CC
#define DP_CNTL                                0x16C0
#define SRC_Y_X                                0x1434
#define DST_Y_X				       0x1438
#define DST_WIDTH_HEIGHT		       0x1598
#define DST_HEIGHT_WIDTH		       0x143c
#define GMC_CLR_CMP_CNTL_DIS      		   (1 << 28)
#define GMC_BRUSH_SOLID_COLOR                       0x000000d0
#define GMC_BRUSH_NONE            		   (15   <<  4)
#define GMC_SRC_DSTCOLOR                           0x00003000
#define DP_SRC_SOURCE_MEMORY			   (2    << 24)
#define ROP3_P                			   0x00f00000
#define ROP3_S                			   0x00cc0000
#define DST_X_LEFT_TO_RIGHT                        0x00000001
#define DST_Y_TOP_TO_BOTTOM                        0x00000002
#define GUI_ACTIVE			0x80000000
#define CUR_OFFSET                             0x0260
#define CUR_HORZ_VERT_OFF                      0x0268
#define CUR_HORZ_VERT_POSN                     0x0264
#define CUR_CLR0                               0x026C
#define CUR_CLR1                               0x0270
#define CUR_LOCK                                   0x80000000
#define CRTC_CUR_EN                                0x00010000
#define CP_CSQ_CNTL 0x0740u /* same upstream register reference */
/* Origin: X.Org ATI xf86-video-ati-6.14.6 src/radeon_reg.h;
 * notice retained in cursor.c, engine.c and docs/licenses/radeon2d-NOTICES.txt. */
#define DP_BRUSH_BKGD_CLR 0x1478u
#define BRUSH_DATA0 0x1480u
#define BRUSH_DATA1 0x1484u
#define BRUSH_Y_X 0x1474u
#define GMC_BRUSH_8X8_MONO_FG_BG (0u << 4)
#define GMC_BYTE_MSB_TO_LSB (0u << 14)
#define GMC_DST_CLIPPING (1u << 3)
#define DP_SRC_SOURCE_HOST_DATA (3u << 24)
#define SC_TOP_LEFT 0x16ecu
#define SC_BOTTOM_RIGHT 0x16f0u
#define RBBM_GUICNTL 0x172cu
#define HOST_DATA0 0x17c0u
#define HOST_DATA7 0x17dcu
#define HOST_DATA_LAST 0x17e0u
#endif
