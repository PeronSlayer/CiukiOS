# NVIDIA TNT through GeForce6 native 2D port

Registry id: `nv2d`; display name: `NVIDIA TNT-GeForce6`.
VBE owns modes. NV4–NV2x PIO activation is enabled under f2-104 amendment 1;
DM9 must pass before operations are published. NV30/FX and NV40/GeForce6
use the separate local-VRAM pushbuffer in `engine.c` and `dma_hw.c`.
f2-105 amendment 1 approves this transport: `ring_transport_reviewed=true`
enables production eligibility behind DM9. Host fixtures exercise PIO, NV30,
NV40 and bridge activation, DM9 pass/mismatch quarantine and independent
refusals. Software fallback still requires a separate idle proof.
See the [transport contracts](../../../../../docs/design/video-driver-families.md).

FILL, overlapping COPY, SYNC, 8x8 PATTERN, scratch-row WRITE and the legacy
32x32 cursor are provided through part A. Part C commands have no hook/ABI allocation on this branch. The semantic
contract in the lead checkout remains input to that later integration. No native modes, offscreen row lease, host
DMA, GART, PCI command write, interrupt or public packet interface is added.

Exact donor chipset IDs nominate candidates. Explicit 00f bridge aliases
require read-only BAR0+1800 identification; Mac-only and GeForce7 IDs refuse.
Integrated/UMA devices, unsupported formats (including packed 24 bpp), active
firmware DMA, ambiguous heads and nonzero scanout starts refuse before engine
setup. Donor pitch alignment and geometry limits remain. NV40 requires chip
and firmware VRAM extents to agree before reserving instance storage.

The reservation contains disjoint scratch, ring and cursor areas, plus the
NV40 top 512 KiB for instance memory. The ring has a 0x7fff byte limit and
32-byte fetches; its eight-NOP prefix and jump/PUT ordering implement the X
nv wrap workaround. No NV40 PIO credit register is read. GET and PUT are
validated, and every poll has both a 20 ms deadline and an iteration cap.

`nv2d_compare` compares native FILL, both COPY directions, PATTERN and WRITE
against swdib on a saved 16x16 block, then checks cursor controls. Pixels are
restored only after idle. NV40 cursor show/hide includes the donor's position
rewrite workaround. The H-page test also disables software replay.

Quarantine permanently rejects submissions. Software rendering requires a
separate transport-idle and cursor-disable proof. Lost PUT and interrupted
wrap publication retain the rejecting guard; no reset establishes CPU safety.
Register and complete VRAM reservations stay pinned and can be reused only
at their exact extents after stop. Durable attempt markers remain PCI-keyed.

## 3D admission (f7-26)

This family explicitly selects software 3D. The inspected nouveau_vieux and
Linux sources require channel/PGRAPH context machinery beyond this 2D PIO
owner; a complete PIO 3D lifecycle and restoration recipe is not established.
There is no 3D provider or boot probe, including with `3d=probe`; native/GL
hardware caps remain zero. Existing 2D qualification is independent. See the
[source evidence and remaining gates](../../../../../docs/design/f7-family-nvidia-nv04-nv20.md).

## Provenance and credits

Primary donor: [X.Org xf86-video-nv](https://gitlab.freedesktop.org/xorg/driver/xf86-video-nv),
commit **0b6619fec39fada995646fb95bb25e72d3c9a491** (the contract's exact
source pin, read through the freedesktop-unofficial-mirror history).

- [nv_hw.c](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_hw.c):
  PRAMIN/PGRAPH/PFIFO setup in `hardware.c` and `dma_hw.c`, and cursor controls.
- [nv_xaa.c](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_xaa.c),
  [nv_dma.h](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_dma.h):
  method numbers/packing, ROPs, patterns and scratch uploads in `engine.c`.
  `nv_local.h` supplies the DMA header and GET/PUT packing reference.
  The local-VRAM ring and bounded wrap adaptation apply only to NV30/NV40;
  TNT retains PIO and never uses the PCI-fetch exception.
- [nv_cursor.c](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_cursor.c),
  [nv_dac.c](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_dac.c),
  [nv_setup.c](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_setup.c):
  cursor conversion/configuration, MMIO offsets and read-only VRAM sizing.
- [nv_driver.c](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-nv/blob/0b6619fec39fada995646fb95bb25e72d3c9a491/src/nv_driver.c):
  exact x86 TNT–GeForce6 chipset table in `matches.h`, and
  read-only bridge identification (no wildcard).

The X fragments retain their original MIT-style notices: NVIDIA Corporation
(1993–2003, 2003, 2007, 2010) and David J. McKay (1996–1997). Driver state and OS
glue are new CiukiOS code; unlicensed `nv_type.h` is not imported. No X server,
Linux driver host, DRM/GART setup or third-party allocator is linked.

Supporting PIO reference: Linux **v6.12**,
[riva_tbl.h](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/riva/riva_tbl.h)
(`nv4TablePFIFO`, identical `nv10TablePFIFO`) and
[riva_hw.c](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/riva/riva_hw.c)
(`nv4Busy`, `nv10Busy`). Only the PIO register table is adapted into
`pio_table.h`, with its complete original notice below and in the source.

## NVIDIA PIO table notice

```text
/***************************************************************************\
|*                                                                           *|
|*       Copyright 1993-1999 NVIDIA, Corporation.  All rights reserved.      *|
|*                                                                           *|
|*     NOTICE TO USER:   The source code  is copyrighted under  U.S. and     *|
|*     international laws.  Users and possessors of this source code are     *|
|*     hereby granted a nonexclusive,  royalty-free copyright license to     *|
|*     use this code in individual and commercial software.                  *|
|*                                                                           *|
|*     Any use of this source code must include,  in the user documenta-     *|
|*     tion and  internal comments to the code,  notices to the end user     *|
|*     as follows:                                                           *|
|*                                                                           *|
|*       Copyright 1993-1999 NVIDIA, Corporation.  All rights reserved.      *|
|*                                                                           *|
|*     NVIDIA, CORPORATION MAKES NO REPRESENTATION ABOUT THE SUITABILITY     *|
|*     OF  THIS SOURCE  CODE  FOR ANY PURPOSE.  IT IS  PROVIDED  "AS IS"     *|
|*     WITHOUT EXPRESS OR IMPLIED WARRANTY OF ANY KIND.  NVIDIA, CORPOR-     *|
|*     ATION DISCLAIMS ALL WARRANTIES  WITH REGARD  TO THIS SOURCE CODE,     *|
|*     INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY, NONINFRINGE-     *|
|*     MENT,  AND FITNESS  FOR A PARTICULAR PURPOSE.   IN NO EVENT SHALL     *|
|*     NVIDIA, CORPORATION  BE LIABLE FOR ANY SPECIAL,  INDIRECT,  INCI-     *|
|*     DENTAL, OR CONSEQUENTIAL DAMAGES,  OR ANY DAMAGES  WHATSOEVER RE-     *|
|*     SULTING FROM LOSS OF USE,  DATA OR PROFITS,  WHETHER IN AN ACTION     *|
|*     OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,  ARISING OUT OF     *|
|*     OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOURCE CODE.     *|
|*                                                                           *|
|*     U.S. Government  End  Users.   This source code  is a "commercial     *|
|*     item,"  as that  term is  defined at  48 C.F.R. 2.101 (OCT 1995),     *|
|*     consisting  of "commercial  computer  software"  and  "commercial     *|
|*     computer  software  documentation,"  as such  terms  are  used in     *|
|*     48 C.F.R. 12.212 (SEPT 1995)  and is provided to the U.S. Govern-     *|
|*     ment only as  a commercial end item.   Consistent with  48 C.F.R.     *|
|*     12.212 and  48 C.F.R. 227.7202-1 through  227.7202-4 (JUNE 1995),     *|
|*     all U.S. Government End Users  acquire the source code  with only     *|
|*     those rights set forth herein.                                        *|
|*                                                                           *|
 \***************************************************************************/

/*
 * GPL licensing note -- nVidia is allowing a liberal interpretation of
 * the documentation restriction above, to merely say that this nVidia's
 * copyright and disclaimer should be included with all code derived
 * from this source.  -- Jeff Garzik <jgarzik@pobox.com>, 01/Nov/99 
 */

/* $XFree86: xc/programs/Xserver/hw/xfree86/drivers/nv/riva_tbl.h,v 1.9 2002/01/30 01:35:03 mvojkovi Exp $ */


/*
 * RIVA Fixed Functionality Init Tables.
 */
```
