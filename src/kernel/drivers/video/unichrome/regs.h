/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 1998-2008 VIA Technologies, Inc. All Rights Reserved.
 * Copyright 2001-2008 S3 Graphics, Inc. All Rights Reserved.
 * Origin: Linux v6.12 drivers/video/fbdev/via/accel.h, original 2D engine
 * register subset. Prefixes adapted for this private CiukiOS port.
 */
#ifndef CIUKI_UNICHROME_REGS_H
#define CIUKI_UNICHROME_REGS_H
#define UC_CMD 0x000u
#define UC_MODE 0x004u
#define UC_SRC 0x008u
#define UC_DST 0x00cu
#define UC_DIM 0x010u
#define UC_FG 0x018u
#define UC_SRCBASE 0x030u
#define UC_DSTBASE 0x034u
#define UC_PITCH 0x038u
#define UC_CURSOR 0x2d0u
#define UC_STATUS 0x400u
#define UC_TRANSET 0x43cu
#define UC_TRANSPACE 0x440u
#define UC_BUSY 0x83u
#define UC_QUEUE_EMPTY 0x00020000u
#define UC_DECX 0x8000u
#define UC_DECY 0x4000u
#define UC_FILL 0xf0002001u
#define UC_COPY 0xcc000001u
#endif
