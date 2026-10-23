/*
	Haiku ATI video driver adapted from the X.org ATI driver which has the
	following copyright:

	Copyright 1999, 2000 ATI Technologies Inc., Markham, Ontario,
						  Precision Insight, Inc., Cedar Park, Texas, and
						  VA Linux Systems Inc., Fremont, California.

	Copyright 2009, 2011 Haiku, Inc.  All rights reserved.
	Distributed under the terms of the MIT license.

	Authors:
	Gerald Zajac
*/



/* Origin: Haiku r1beta5, src/add-ons/accelerants/ati/rage128.h.
 * CiukiOS: selected definitions only, unsigned high-bit constants. */
/*
 * Copyright 1999, 2000 ATI Technologies Inc., Markham, Ontario,
 *                      Precision Insight, Inc., Cedar Park, Texas, and
 *                      VA Linux Systems Inc., Fremont, California.
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
 * NON-INFRINGEMENT.  IN NO EVENT SHALL ATI, PRECISION INSIGHT, VA LINUX
 * SYSTEMS AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Authors:
 *   Rickard E. Faith <faith@valinux.com>
 *   Kevin E. Martin <martin@valinux.com>
 *   Gareth Hughes <gareth@valinux.com>
 *
 * References:
 *
 *   RAGE 128 VR/ RAGE 128 GL Register Reference Manual (Technical
 *   Reference Manual P/N RRG-G04100-C Rev. 0.04), ATI Technologies: April
 *   1999.
 *
 *   RAGE 128 Software Development Manual (Technical Reference Manual P/N
 *   SDK-G04000 Rev. 0.01), ATI Technologies: June 1999.
 *
 */


/* Additional fields: xf86-video-r128 d05022a1c398787d3f21d023a8251cb128ba9f9f,
 * src/r128_reg.h (CRTC cursor mode and PM4 buffer control). */
#ifndef CIUKI_R128_REGS_H
#define CIUKI_R128_REGS_H
#define R128_AUX_SC_CNTL				  0x1660
#define R128_CONFIG_MEMSIZE 			  0x00f8
#define R128_CRTC_GEN_CNTL				  0x0050
#		define R128_CRTC_CUR_EN 		  (1u << 16)
#		define R128_CRTC_EXT_DISP_EN	  (1u << 24)
#		define R128_CRTC_EN 			  (1u << 25)
#define R128_CRTC_H_TOTAL_DISP			  0x0200
#define R128_CRTC_OFFSET				  0x0224
#define R128_CRTC_OFFSET_CNTL			  0x0228
#define R128_CRTC_PITCH					  0x022c
#define R128_CRTC_V_TOTAL_DISP			  0x0208
#define R128_CUR_CLR0					  0x026c
#define R128_CUR_CLR1					  0x0270
#define R128_CUR_HORZ_VERT_OFF			  0x0268
#define R128_CUR_HORZ_VERT_POSN 		  0x0264
#define R128_CUR_OFFSET 				  0x0260
#		define R128_CUR_LOCK			  (1u << 31)
#define R128_DEFAULT_OFFSET 			  0x16e0
#define R128_DEFAULT_PITCH				  0x16e4
#define R128_DEFAULT_SC_BOTTOM_RIGHT	  0x16e8
#define R128_DP_BRUSH_FRGD_CLR			  0x147c
#define R128_DP_CNTL					  0x16c0
#		define R128_DST_X_LEFT_TO_RIGHT   (1u <<  0)
#		define R128_DST_Y_TOP_TO_BOTTOM   (1u <<  1)
#define R128_DP_DATATYPE				  0x16c4
#		define R128_HOST_BIG_ENDIAN_EN	  (1u << 29)
#define R128_DP_GUI_MASTER_CNTL 		  0x146c
#		define R128_DP_SRC_SOURCE_MEMORY	(2  << 24)
#		define R128_GMC_AUX_CLIP_DIS		(1u  << 29)
#		define R128_GMC_BRUSH_NONE			(15 <<  4)
#		define R128_GMC_BRUSH_SOLID_COLOR	(13 <<  4)
#		define R128_GMC_CLR_CMP_CNTL_DIS	(1u  << 28)
#		define R128_GMC_DST_DATATYPE_SHIFT	8
#		define R128_GMC_SRC_DATATYPE_COLOR	(3 << 12)
#		define R128_ROP3_Dn 			  0x00550000
#		define R128_ROP3_P				  0x00f00000
#		define R128_ROP3_S				  0x00cc0000
#define R128_DP_WRITE_MASK				  0x16cc
#define R128_DST_BRES_DEC				  0x1630
#define R128_DST_BRES_ERR				  0x1628
#define R128_DST_BRES_INC				  0x162c
#define R128_DST_HEIGHT_WIDTH			  0x143c
#define R128_DST_WIDTH_HEIGHT			  0x1598
#define R128_DST_Y_X					  0x1438
#define R128_GUI_STAT					  0x1740
#		define R128_GUI_FIFOCNT_MASK	  0x0fff
#		define R128_GUI_ACTIVE			  (1u << 31)
#define R128_PC_NGUI_CTLSTAT			  0x0184
#		define R128_PC_FLUSH_ALL		  0x00ff
#		define R128_PC_BUSY 			  (1u << 31)
#define R128_SC_BOTTOM_RIGHT			  0x16f0
#define R128_SC_TOP_LEFT				  0x16ec
#define R128_SCALE_3D_CNTL				  0x1a00
#define R128_SRC_Y_X					  0x1434
#define R128_CRTC_CUR_MODE_MASK (7u << 17)
#define R128_PM4_BUFFER_CNTL 0x0704
#endif
