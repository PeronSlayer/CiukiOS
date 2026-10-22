/* CiukiDOS fork addition: resident side of the DFV2 binding.
 *
 * Copyright (C) 2026 The CiukiOS contributors.
 * This file is part of the CiukiDOS fork of the FreeDOS kernel. It is free
 * software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation;
 * either version 2, or (at your option) any later version.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Every hook is inert until ciukinit.c has completed the late binding
 * after init_kernel(). An unbound kernel (no CIUKIDF.SYS, an absent or
 * refusing host) executes exactly the upstream paths.
 */

#include "portab.h"
#include "globals.h"
#include "ciuki.h"

/* Initialised data, not BSS: the image itself carries the zero state. */
struct ciuki_hooks ASM CiukiHooks = { 0 };

/* Called from int21_syscall (AH=50h) on the caller's stack: no locals whose
   address is taken, the driver switches to its own stack at once. */
int ciuki_event(UWORD event, UWORD psp, UWORD parent, UWORD kind)
{
  if (CiukiHooks.event == NULL)
    return SUCCESS;
  return CiukiHooks.event(event, psp, parent, kind);
}

BOOL ciuki_direct(iregs FAR * r)
{
  UBYTE ah = r->AH;

  if (CiukiHooks.direct == NULL ||
      !(CiukiHooks.claim[ah >> 3] & (1 << (ah & 7))))
    return FALSE;
  return CiukiHooks.direct(r) != 0;
}

/* Exact inverse of the CloneHandle() increments taken by child_psp() for a
   PSP that is never published: the parent still owns each entry, so no
   close (and no redirector close) can occur. */
VOID ciuki_release_jft(UWORD psp_seg)
{
  psp FAR *p = MK_FP(psp_seg, 0);
  int i;

  for (i = 0; i < 20; i++)
  {
    UBYTE idx = p->ps_files[i];
    sft FAR *s;

    p->ps_files[i] = 0xff;
    if (idx != 0xff && (s = idx_to_sft(idx)) != (sft FAR *) - 1 &&
        s->sft_count > 1)
      s->sft_count--;
  }
}

/* EXEC failed after EXEC_BEGIN: release the inherited references and the
   child's blocks once, then retire the provisional native record. An
   unbound kernel keeps the upstream behaviour of the failing path. */
VOID ciuki_exec_unwind(UWORD child, UWORD parent)
{
  if (CiukiHooks.event == NULL)
    return;
  ciuki_release_jft(child);
  FreeProcessMem(child);
  ciuki_event(CIUKI_EV_EXEC_ROLLBACK, child, parent, 0);
}

/* FCB close of a redirected (SFT_FSHARED) entry: the size held in the
   caller's FCB is published as an explicit, standard truncate/extend
   request (11xxh write of zero bytes at that position), never by storing
   it into the SFT for the redirector to trust. The date and time follow
   through DosSetFtimeSft as upstream. A refusal keeps the FCB open. */
COUNT ciuki_fcb_close_size(int sft_idx, ULONG size)
{
  long rc;

  if (SftGetFsize(sft_idx) == size)
    return SUCCESS;
  if ((rc = SftSeek(sft_idx, size, SEEK_SET)) != SUCCESS)
    return (COUNT)rc;
  rc = DosRWSft(sft_idx, 0, (VOID FAR *)&size, XFR_WRITE);
  return rc < 0 ? (COUNT)rc : SUCCESS;
}

/* f3-50 65h/22h and A2h: the string must end before its segment does. */
BOOL ciuki_asciz_fits(const char FAR * s)
{
  unsigned left = 0xffffu - FP_OFF(s);

  for (;;)
  {
    if (*s == '\0')
      return TRUE;
    if (left-- == 0)
      return FALSE;
    s++;
  }
}
