/*
	Haiku ATI video driver adapted from the X.org ATI driver which has the
	following copyright:

	Copyright 1992,1993,1994,1995,1996,1997 by Kevin E. Martin, Chapel Hill, North Carolina.

	Copyright 2009, 2011 Haiku, Inc.  All rights reserved.
	Distributed under the terms of the MIT license.

	Authors:
	Gerald Zajac
*/

/* Origin: Haiku r1beta5 src/add-ons/accelerants/ati/mach64.h; used definitions only.
 * MIT; full permission notice: docs/licenses/haiku-mach64-NOTICES.txt. */
#ifndef CIUKI_MACH64_REGISTERS_H
#define CIUKI_MACH64_REGISTERS_H
#define CRTC_OFF_PITCH			(0x0c00 + 0x0014)	// Dword offset 05
#define CRTC_GEN_CNTL			(0x0c00 + 0x001C)	// Dword offset 07
#define CUR_CLR0				(0x0c00 + 0x0060)	// Dword offset 18
#define CUR_CLR1				(0x0c00 + 0x0064)	// Dword offset 19
#define CUR_OFFSET				(0x0c00 + 0x0068)	// Dword offset 1A
#define CUR_HORZ_VERT_POSN		(0x0c00 + 0x006C)	// Dword offset 1B
#define CUR_HORZ_VERT_OFF		(0x0c00 + 0x0070)	// Dword offset 1C
#define GEN_TEST_CNTL			(0x0c00 + 0x00D0)	// Dword offset 34
#define CONFIG_CHIP_ID			(0x0c00 + 0x00E0)	// Dword offset 38
#define MEM_CNTL				(0x0c00 + 0x00B0)	// Dword offset 2C
#define DST_OFF_PITCH			(0x0c00 + 0x0100)	// Dword offset 40
#define DST_Y_X 				(0x0c00 + 0x010C)	// Dword offset 43
#define DST_HEIGHT				(0x0c00 + 0x0114)	// Dword offset 45
#define DST_BRES_ERR			(0x0c00 + 0x0124)	// Dword offset 49
#define DST_BRES_INC			(0x0c00 + 0x0128)	// Dword offset 4A
#define DST_BRES_DEC			(0x0c00 + 0x012C)	// Dword offset 4B
#define DST_CNTL				(0x0c00 + 0x0130)	// Dword offset 4C
#define SRC_OFF_PITCH			(0x0c00 + 0x0180)	// Dword offset 60
#define SRC_Y_X 				(0x0c00 + 0x018C)	// Dword offset 63
#define SRC_HEIGHT1_WIDTH1		(0x0c00 + 0x0198)	// Dword offset 66
#define SRC_Y_X_START			(0x0c00 + 0x01A4)	// Dword offset 69
#define SRC_HEIGHT2_WIDTH2		(0x0c00 + 0x01B0)	// Dword offset 6C
#define SRC_CNTL				(0x0c00 + 0x01B4)	// Dword offset 6D
#define HOST_CNTL				(0x0c00 + 0x0240)	// Dword offset 90
#define PAT_REG0				(0x0c00 + 0x0280)	// Dword offset A0
#define PAT_REG1				(0x0c00 + 0x0284)	// Dword offset A1
#define PAT_CNTL				(0x0c00 + 0x0288)	// Dword offset A2
#define SC_LEFT_RIGHT			(0x0c00 + 0x02A8)	// Dword offset AA
#define SC_TOP_BOTTOM			(0x0c00 + 0x02B4)	// Dword offset AD
#define DP_BKGD_CLR 			(0x0c00 + 0x02C0)	// Dword offset B0
#define DP_FRGD_CLR 			(0x0c00 + 0x02C4)	// Dword offset B1
#define DP_WRITE_MASK			(0x0c00 + 0x02C8)	// Dword offset B2
#define DP_CHAIN_MASK			(0x0c00 + 0x02CC)	// Dword offset B3
#define DP_PIX_WIDTH			(0x0c00 + 0x02D0)	// Dword offset B4
#define DP_MIX					(0x0c00 + 0x02D4)	// Dword offset B5
#define DP_SRC					(0x0c00 + 0x02D8)	// Dword offset B6
#define CLR_CMP_CLR 			(0x0c00 + 0x0300)	// Dword offset C0
#define CLR_CMP_MASK			(0x0c00 + 0x0304)	// Dword offset C1
#define CLR_CMP_CNTL			(0x0c00 + 0x0308)	// Dword offset C2
#define FIFO_STAT				(0x0c00 + 0x0310)	// Dword offset C4
#define GUI_STAT				(0x0c00 + 0x0338)	// Dword offset CE
#define CONTEXT_MASK			(0x0c00 + 0x0320)	// Dword offset C8
#define DST_HEIGHT_WIDTH		(0x0c00 + 0x0118)	// Dword offset 46
#define GUI_ENGINE_ENABLE		0x100
#define HWCURSOR_ENABLE 		0x80
#define ENGINE_BUSY 		1
#define MIX_SRC					0x0007
#define MIX_DST					0x0003
#define DST_X_LEFT_TO_RIGHT		1
#define DST_Y_TOP_TO_BOTTOM		2
#define DST_LAST_PEL				0x20
#define SRC_LINE_X_LEFT_TO_RIGHT	0x10
#define HOST_BYTE_ALIGN			1
#define DP_CHAIN_16BPP		0x8410
#define DP_CHAIN_32BPP		0x8080
#define HOST_16BPP			0x40000
#define SRC_16BPP 			0x400
#define DST_16BPP 			4
#define HOST_32BPP			0x60000
#define SRC_32BPP 			0x600
#define DST_32BPP 			6
#define BYTE_ORDER_LSB_TO_MSB 0x1000000
#define BKGD_SRC_BKGD_CLR 	0
#define FRGD_SRC_FRGD_CLR 	0x100
#define FRGD_SRC_BLIT 		0x300
#define MONO_SRC_ONE		0
#endif
