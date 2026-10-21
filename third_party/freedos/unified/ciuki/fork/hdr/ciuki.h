/* CiukiDOS fork addition: private DFV2 binding between this kernel, the
 * CIUKIDF.SYS control driver and the CiukiOS VMM. Wire layout and stack
 * diagrams: docs/validation/f3-56-unified-binding.md in the CiukiOS tree.
 *
 * Copyright (C) 2026 The CiukiOS contributors.
 * This file is part of the CiukiDOS fork of the FreeDOS kernel. It is free
 * software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation;
 * either version 2, or (at your option) any later version.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CIUKI_H
#define CIUKI_H

/* Existing f3-50 lifecycle meanings 0-8; no new event numbers. */
#define CIUKI_EV_BIND           0
#define CIUKI_EV_PSP_CREATE     1
#define CIUKI_EV_PSP_SELECT     2
#define CIUKI_EV_EXEC_BEGIN     3
#define CIUKI_EV_EXEC_COMMIT    4
#define CIUKI_EV_EXEC_ROLLBACK  5
#define CIUKI_EV_PROCESS_EXIT   6
#define CIUKI_EV_TSR            7

/* Control block at offset 12h of the CIUKIDF$ character device header. */
#define CIUKI_DEVICE_NAME       "CIUKIDF$"
#define CIUKI_CONTROL_MAGIC     "CIUKIDFC"
#define CIUKI_CONTROL_VERSION   3
#define CIUKI_CONTROL_BYTES     24
#define CIUKI_POLICY_LOCAL      1       /* DEVICE=CIUKIDF.SYS /LOCAL */
#define CIUKI_STATE_REGISTERED  1

/* Bind record (phase 1 is filled by ciukinit.c). Little endian, packed. */
#define CIUKI_BIND_BYTES        0xd0
#define CIUKI_SDA_BYTES         0x78c   /* kernel.asm: controlled variables end */
#define CIUKI_SDA_FIELDS        10

/* Far entries exported by the driver; all preserve every register but AX. */
typedef int (FAR ASMCFUNC * ciuki_bind_fn) (VOID FAR * record);
typedef int (FAR ASMCFUNC * ciuki_event_fn) (UWORD event, UWORD psp,
                                             UWORD parent, UWORD kind);
typedef int (FAR ASMCFUNC * ciuki_direct_fn) (iregs FAR * r);

struct ciuki_hooks {
  ciuki_event_fn event;         /* 00 lifecycle entry, NULL when unbound  */
  ciuki_direct_fn direct;       /* 04 claimed INT 21h entry               */
  VOID FAR *local;              /* 08 INT-2Fh-like redirector entry       */
  UBYTE claim[32];              /* 0C AH bitmap published by the VMM      */
};

extern struct ciuki_hooks DOSFAR ASM CiukiHooks;

int ciuki_event(UWORD event, UWORD psp, UWORD parent, UWORD kind);
BOOL ciuki_direct(iregs FAR * r);
VOID ciuki_release_jft(UWORD psp_seg);
VOID ciuki_exec_unwind(UWORD child, UWORD parent);
COUNT ciuki_fcb_close_size(int sft_idx, ULONG size);
BOOL ciuki_asciz_fits(const char FAR * s);

#endif
