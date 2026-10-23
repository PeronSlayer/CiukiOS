/* SPDX-License-Identifier: GPL-2.0 */
/*
 *
 * Hardware accelerated Matrox Millennium I, II, Mystique, G100, G200, G400 and G450
 *
 * (c) 1998-2002 Petr Vandrovec <vandrove@vc.cvut.cz>
 *
 */
/* Origin: Linux v6.12 drivers/video/fbdev/matrox/matroxfb_base.h.
 * Selected register definitions only; no Linux compatibility layer. */
#ifndef CIUKI_MATROX_REGISTERS_H
#define CIUKI_MATROX_REGISTERS_H
#define M_DWGCTL	0x1C00
#define M_MACCESS	0x1C04
#define M_PLNWT		0x1C1C

#define M_BCOL		0x1C20
#define M_FCOL		0x1C24

#define M_SGN		0x1C58
#define M_LEN		0x1C5C
#define M_AR0		0x1C60
#define M_AR1		0x1C64
#define M_AR2		0x1C68
#define M_AR3		0x1C6C
#define M_AR4		0x1C70
#define M_AR5		0x1C74
#define M_AR6		0x1C78

#define M_CXBNDRY	0x1C80
#define M_FXBNDRY	0x1C84
#define M_YDSTLEN	0x1C88
#define M_PITCH		0x1C8C
#define M_YDST		0x1C90
#define M_YDSTORG	0x1C94
#define M_YTOP		0x1C98
#define M_YBOT		0x1C9C

#define M_EXEC		0x0100

#define M_DWG_TRAP	0x04
#define M_DWG_BITBLT	0x08
#define M_DWG_ILOAD	0x09

#define M_DWG_LINEAR	0x0080
#define M_DWG_SOLID	0x0800
#define M_DWG_ARZERO	0x1000
#define M_DWG_SGNZERO	0x2000
#define M_DWG_SHIFTZERO	0x4000

#define M_DWG_REPLACE	0x000C0000
#define M_DWG_REPLACE2	(M_DWG_REPLACE | 0x40)
#define M_DWG_XOR	0x00060010

#define M_DWG_BFCOL	0x04000000
#define M_DWG_BMONOWF	0x08000000

#define M_DWG_TRANSC	0x40000000

#define M_FIFOSTATUS	0x1E10
#define M_STATUS	0x1E14
#define M_ICLEAR	0x1E18
#define M_IEN		0x1E1C

#define M_VCOUNT	0x1E20

#define M_RESET		0x1E40
#define M_MEMRDBK	0x1E44

#define M_AGP2PLL	0x1E4C

#define M_OPMODE	0x1E54
#define     M_OPMODE_DMA_GEN_WRITE	0x00
#define     M_OPMODE_DMA_BLIT		0x04
#define     M_OPMODE_DMA_VECTOR_WRITE	0x08
#define     M_OPMODE_DMA_LE		0x0000		/* little endian - no transformation */
#define     M_OPMODE_DMA_BE_8BPP	0x0000
#define     M_OPMODE_DMA_BE_16BPP	0x0100
#define     M_OPMODE_DMA_BE_32BPP	0x0200
#define     M_OPMODE_DIR_LE		0x000000	/* little endian - no transformation */
#define     M_OPMODE_DIR_BE_8BPP	0x000000
#define     M_OPMODE_DIR_BE_16BPP	0x010000
#define     M_OPMODE_DIR_BE_32BPP	0x020000

#define M_SRCORG 0x2CB4
#define M_DSTORG 0x2CB8
#define M_DAC_INDEX 0x3C00
#define M_DAC_DATA 0x3C0A
#define M_CURSOR_POS 0x3C0C
#define M_C2CTL 0x3C10
/* DAC1064 register definitions, Linux v6.12 matroxfb_DAC1064.h,
 * SPDX-License-Identifier: GPL-2.0 */
#define XCURADDL 0x04
#define XCURADDH 0x05
#define XCURCTRL 0x06
#define XCURCOL0 0x08
#define XCURCOL1 0x0C
#define XCUR_XWIN 3u
#define M_BUSY 0x10000u
#define M_VSYNC 8u
#define M_FIFO_EMPTY 0x200u /* bempty: G200 64 entries, G400 16 entries */
#define MATROX_MMIO_BYTES 16384u
#endif
