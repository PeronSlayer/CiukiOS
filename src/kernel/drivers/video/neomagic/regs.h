/* Origin: Linux v6.12 include/video/neomagic.h, register definitions only. */
#ifndef CIUKI_NEO_REGS_H
#define CIUKI_NEO_REGS_H
/*
 * linux/include/video/neo_reg.h -- NeoMagic Framebuffer Driver
 *
 * Copyright (c) 2001  Denis Oliver Kropp <dok@convergence.de>
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License.  See the file COPYING in the main directory of this
 * archive for more details.
 */

#define NEO_BS0_BLT_BUSY        0x00000001
#define NEO_BS0_FIFO_AVAIL      0x00000002
#define NEO_BS0_FIFO_PEND       0x00000004

#define NEO_BC0_DST_Y_DEC       0x00000001
#define NEO_BC0_X_DEC           0x00000002
#define NEO_BC0_SRC_TRANS       0x00000004
#define NEO_BC0_SRC_IS_FG       0x00000008
#define NEO_BC0_SRC_Y_DEC       0x00000010
#define NEO_BC0_FILL_PAT        0x00000020
#define NEO_BC0_SRC_MONO        0x00000040
#define NEO_BC0_SYS_TO_VID      0x00000080

#define NEO_BC1_DEPTH8          0x00000100
#define NEO_BC1_DEPTH16         0x00000200
#define NEO_BC1_X_320           0x00000400
#define NEO_BC1_X_640           0x00000800
#define NEO_BC1_X_800           0x00000c00
#define NEO_BC1_X_1024          0x00001000
#define NEO_BC1_X_1152          0x00001400
#define NEO_BC1_X_1280          0x00001800
#define NEO_BC1_X_1600          0x00001c00
#define NEO_BC1_DST_TRANS       0x00002000
#define NEO_BC1_MSTR_BLT        0x00004000
#define NEO_BC1_FILTER_Z        0x00008000

#define NEO_BC2_WR_TR_DST       0x00800000

#define NEO_BC3_SRC_XY_ADDR     0x01000000
#define NEO_BC3_DST_XY_ADDR     0x02000000
#define NEO_BC3_CLIP_ON         0x04000000
#define NEO_BC3_FIFO_EN         0x08000000
#define NEO_BC3_BLT_ON_ADDR     0x10000000
#define NEO_BC3_SKIP_MAPPING    0x80000000

#define NEO_MODE1_DEPTH8        0x0100
#define NEO_MODE1_DEPTH16       0x0200
#define NEO_MODE1_DEPTH24       0x0300
#define NEO_MODE1_X_320         0x0400
#define NEO_MODE1_X_640         0x0800
#define NEO_MODE1_X_800         0x0c00
#define NEO_MODE1_X_1024        0x1000
#define NEO_MODE1_X_1152        0x1400
#define NEO_MODE1_X_1280        0x1800
#define NEO_MODE1_X_1600        0x1c00
#define NEO_MODE1_BLT_ON_ADDR   0x2000

/* These are offseted in MMIO space by par->CursorOff */
#define NEOREG_CURSCNTL		0x00
#define NEOREG_CURSX		0x04
#define NEOREG_CURSY		0x08
#define NEOREG_CURSBGCOLOR	0x0C
#define NEOREG_CURSFGCOLOR	0x10
#define NEOREG_CURSMEMPOS	0x14

#define NEO_CURS_DISABLE	0x00000000
#define NEO_CURS_ENABLE		0x00000001
#define NEO_ICON64_ENABLE	0x00000008
#define NEO_ICON128_ENABLE	0x0000000C
#define NEO_ICON_BLANK		0x00000010

#define NEO_GR01_SUPPRESS_VSYNC 0x10
#define NEO_GR01_SUPPRESS_HSYNC 0x20


/* Byte offsets of the upstream Neo2200 structure; no packed C overlay. */
enum { NEO_STAT=0, NEO_CONTROL=4, NEO_FG=12, NEO_BG=16, NEO_PITCH=20,
       NEO_SRC=36, NEO_DST=44, NEO_EXTENT=48, NEO_CURSOR=0x1000,
       NEO_UPLOAD=0x100000, NEO_MMIO_BYTES=0x200000 };
#endif
