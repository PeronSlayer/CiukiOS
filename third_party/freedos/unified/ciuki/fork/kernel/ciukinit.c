/* CiukiDOS fork addition: late DFV2 binding (INIT segment).
 *
 * Copyright (C) 2026 The CiukiOS contributors.
 * This file is part of the CiukiDOS fork of the FreeDOS kernel. It is free
 * software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation;
 * either version 2, or (at your option) any later version.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Phase 0 happens in CIUKIDF.SYS INIT: it registers only its resident
 * memory. CONFIG.SYS processing later replaces the SFT chain and CDS
 * (config.c:459-506), and FsConfig() rebuilds CDS and the root JFT
 * (main.c:342-347). This phase therefore runs once, after init_kernel()
 * and before DoInstall(): the layout it reports is the final one.
 *
 * f3-57: the final bind is also the activation barrier of the VMM's local
 * file system. All dirty buffers are written first (INT 21h/0Dh), so the
 * native mount reads the volume FreeDOS left; the VMM then converts the
 * owned CDS entries to IFS drives before the hooks are published here. A
 * refused activation stops this kernel before INSTALL/shell unless the
 * driver was loaded with /LOCAL (isolated fixtures keep the local FAT).
 */

#include "portab.h"
#include "init-mod.h"
#include "dyndata.h"
#include "ciuki.h"

extern BYTE DOSFAR ASM nlsUpcaseHardcoded[];
extern BYTE DOSFAR ASM nlsFUpcaseHardcoded[];
extern BYTE DOSFAR ASM nlsFnameTermHardcoded[];
extern BYTE DOSFAR ASM nlsCollHardcoded[];
extern BYTE DOSFAR ASM nlsDBCSHardcoded[];
extern UWORD DOSFAR ASM cu_psp;

STATIC UBYTE ciuki_record[CIUKI_BIND_BYTES];

#ifdef __WATCOMC__
VOID ciuki_halt(VOID);
#pragma aux ciuki_halt = "cli" "hlt" modify exact []
#else
#define ciuki_halt()
#endif

/* Contained stop (no panic path): the VM monitor sees HLT with IF clear. */
STATIC VOID ciuki_stop(int rc)
{
  printf("CiukiDOS: native file activation refused (error %d); system stopped\n", rc);
  for (;;)
    ciuki_halt();
}

STATIC VOID put16(unsigned at, UWORD v)
{
  ciuki_record[at] = (UBYTE)v;
  ciuki_record[at + 1] = (UBYTE)(v >> 8);
}

STATIC VOID putfar(unsigned at, VOID FAR * p)
{
  put16(at, FP_OFF(p));
  put16(at + 2, FP_SEG(p));
}

/* DGROUP objects are addressed relative to LoL's segment, as dyninit.c does. */
#define DOSDATA(x) MK_FP(FP_SEG(LoL), FP_OFF(x))

STATIC struct dhdr FAR *find_driver(VOID)
{
  struct dhdr FAR *d = LoL->nul_dev.dh_next;
  int guard;

  for (guard = 0; guard < 256 && FP_OFF(d) != 0xffff; guard++)
  {
    if ((d->dh_attr & ATTR_CHAR) &&
        fmemcmp(d->dh_name, CIUKI_DEVICE_NAME, 8) == 0)
      return d;
    d = d->dh_next;
  }
  return NULL;
}

VOID CiukiLateBind(VOID)
{
  struct dhdr FAR *d = find_driver();
  UBYTE FAR *control;
  struct ciuki_hooks FAR *hooks;
  struct sfttbl FAR *t;
  UWORD total = 0, seg;
  int guard, rc;
  BYTE FAR *sda;

  if (d == NULL)
    return;                     /* no binding driver: unchanged FreeDOS */
  seg = FP_SEG(d);
  control = (UBYTE FAR *)d + 0x12;
  if (FP_OFF(d) != 0 || fmemcmp(control, CIUKI_CONTROL_MAGIC, 8) != 0 ||
      *(UWORD FAR *)(control + 8) != CIUKI_CONTROL_VERSION ||
      *(UWORD FAR *)(control + 10) != CIUKI_CONTROL_BYTES ||
      *((UBYTE FAR *)MK_FP(seg, *(UWORD FAR *)(control + 20))) !=
      CIUKI_STATE_REGISTERED)
  {
    printf("CiukiDOS: CIUKIDF$ control block rejected; continuing unbound\n");
    return;
  }

  fmemset(ciuki_record, 0, sizeof(ciuki_record));
  fmemcpy(ciuki_record, "DFB2", 4);
  put16(4, 2);
  put16(6, CIUKI_BIND_BYTES);
  put16(8, 1);                  /* final phase; driver fills 0Ch-2Fh */
  fmemcpy(ciuki_record + 0x30, "CIUKIDOS K2043 3", 16);
  put16(0x40, FP_SEG(LoL));
  put16(0x42, FP_OFF(DynLast()));
  put16(0x44, CurrentKernelSegment);
  put16(0x46, (FP_OFF(_HMATextEnd) | 0x000f) + 1);
  /* struct lol starts 26h bytes before the INT 21h/52h pointer (lol.h) */
  putfar(0x48, (BYTE FAR *)LoL + 0x26);
  sda = DOSDATA(internal_data);
  putfar(0x4c, sda);
  put16(0x50, CIUKI_SDA_BYTES);
  put16(0x52, sizeof(sft));
  /* Explicit SDA fields (kernel.asm:584-600): ErrorMode, InDOS, locus,
     extended error, action, class, DTA, PSP, return code, drive. */
  putfar(0x54, sda + 0x00);
  putfar(0x58, sda + 0x01);
  putfar(0x5c, sda + 0x03);
  putfar(0x60, sda + 0x04);
  putfar(0x64, sda + 0x06);
  putfar(0x68, sda + 0x07);
  putfar(0x6c, sda + 0x0c);
  putfar(0x70, sda + 0x10);
  putfar(0x74, sda + 0x14);
  putfar(0x78, sda + 0x16);
  putfar(0x7c, LoL->sfthead);
  t = LoL->sfthead;
  for (guard = 0; guard < 64 && FP_OFF(t) != 0xffff; guard++)
  {
    total += t->sftt_count;
    t = t->sftt_next;
  }
  put16(0x80, total);
  put16(0x82, LoL->lastdrive);
  putfar(0x84, LoL->CDSp);
  put16(0x88, sizeof(struct cds));
  put16(0x8a, cu_psp);
  putfar(0x8c, DOSDATA(&LoL->nul_dev));
  /* The one country-1/CP437 definition: the hardcoded package that 38h,
     65h, 66h and the 2Fh/1213h/filename paths use (nls_hc.asm). */
  putfar(0x90, DOSDATA(&nlsCountryInfoHardcoded));
  putfar(0x94, DOSDATA(nlsUpcaseHardcoded));
  putfar(0x98, DOSDATA(nlsFUpcaseHardcoded));
  putfar(0x9c, DOSDATA(nlsFnameTermHardcoded));
  putfar(0xa0, DOSDATA(nlsCollHardcoded));
  putfar(0xa4, DOSDATA(nlsDBCSHardcoded));

  {
    iregs r;
    r.a.x = 0x0d00;             /* disk reset: write every dirty buffer */
    init_call_intr(0x21, &r);
  }
  rc = ((ciuki_bind_fn)MK_FP(seg, *(UWORD FAR *)(control + 12)))
      ((VOID FAR *)ciuki_record);
  if (rc != SUCCESS)
  {
    if (*((UBYTE FAR *)MK_FP(seg, *(UWORD FAR *)(control + 22))) !=
        CIUKI_POLICY_LOCAL)
      ciuki_stop(rc);
    printf("CiukiDOS: DFV2 binding refused (error %d); continuing unbound\n",
           rc);
    return;
  }

  /* Publish the hooks last: until here every kernel path is upstream. */
  hooks = &CiukiHooks;
  fmemcpy(hooks->claim, ciuki_record + 0xb0, sizeof(hooks->claim));
  hooks->direct = (ciuki_direct_fn)MK_FP(seg, *(UWORD FAR *)(control + 16));
  hooks->local = MK_FP(seg, *(UWORD FAR *)(control + 18));
  hooks->event = (ciuki_event_fn)MK_FP(seg, *(UWORD FAR *)(control + 14));
}
