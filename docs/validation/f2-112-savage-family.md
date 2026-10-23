# S3 Savage family completion: host validation

Research date: 2026-10-23 (workstation `date` checked).
Directive: `docs/directives/f2-112-savage-family-completion.md`.

The chosen donor remains the relocated Ciuki native owner. New register and
resource glue is independently implemented; no upstream implementation is
transplanted. Reference downloads are confined to `build/research/f2-112/`.

| Primary source | Comparison and implementation decision |
| --- | --- |
| [S3 RE046-B, Savage4 Registers, April 1999](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf), sections 1, 5, 6, 7, 13 | Keep byte VGA I/O, halfword 2D readback, linear VBE scanout, bounded waits and the existing Savage4/SuperSavage idle signatures. This manual is not a Savage3D register reference. |
| [Haiku r1beta5, `src/add-ons/kernel/drivers/graphics/s3/driver.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/kernel/drivers/graphics/s3/driver.cpp), `chipTable` | Explicit PCI IDs for Savage3D/MV, MX/IX, Savage4, ProSavage, Twister, DDR and SuperSavage. Savage2000 is a separate transport and is excluded from this directive. |
| [Haiku r1beta5, `src/add-ons/accelerants/s3/savage_mode.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/savage_mode.cpp), `Savage_Initialize2DEngine` | Early chips use a 128 KiB VRAM COB; newer chips keep COB disabled because of coherency bugs. Reserve the early COB exclusively, never CPU-map it, never enable shadow DMA, and throttle submissions to the on-chip FIFO budget. Do not import mode/panel/clock changes. |
| [Linux v6.12, `drivers/video/fbdev/savage/savagefb_driver.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/savage/savagefb_driver.c), `savage_map_mmio`, `savage_map_video`, `savage3D_waitidle`, `SavageSetup2DEngine` | Early framebuffer BAR0, registers BAR0 + 16 MiB, status MM48C00 and FIFO bits 15:0; later framebuffer BAR1, registers BAR0, status MM48C60. Replace upstream infinite waits with the existing deadline and frozen-clock bound. Preserve the repeated queued descriptor initialization. |
| [X.Org pin d28cd83c7b0b4a943efbe5ddf257c8ee2646ea73, `src/savage_driver.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-savage/blob/d28cd83c7b0b4a943efbe5ddf257c8ee2646ea73/src/savage_driver.c), `WaitIdleEmpty3D`, `SavageMapMem`, COB placement; [`src/savage_bci.h`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-savage/blob/d28cd83c7b0b4a943efbe5ddf257c8ee2646ea73/src/savage_bci.h), `BCI_ENABLE*` | Separate early idle/empty proof from later signatures. Descriptor bit 3 enables BCI on Savage3D/MX/4, but is omitted on ProSavage/Twister/DDR/SuperSavage. Keep block writes disabled. COB must end at a 4 MiB boundary; configurations lacking sufficient firmware-reported VRAM refuse before commands. |
| [Haiku r1beta5, `src/add-ons/accelerants/s3/savage_draw.cpp`](https://github.com/haiku/haiku/blob/r1beta5/src/add-ons/accelerants/s3/savage_draw.cpp); [Linux v6.12 `savagefb_accel.c`](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/savage/savagefb_accel.c) | Retain FILL, direction-safe COPY and synchronous completion. Existing PATTERN/WRITE/cursor hooks remain; new part C operations need allocated hooks/capabilities and remain deferred by Amendment 1. |
| [X.Org same pin, `src/savage_cursor.c`](https://github.com/freedesktop-unofficial-mirror/xorg__driver__xf86-video-savage/blob/d28cd83c7b0b4a943efbe5ddf257c8ee2646ea73/src/savage_cursor.c) | Retain the tested 64-by-64 plane packing for Ciuki's 32-by-32 mask/image cursor, hide-before-load and signed positioning. Reserve its page below the COB on early chips. |

The owner's read-only Windows 98 Resource Kit, `build/rskt/wrkc24_3.htm`,
“Display Driver Overview” (lines 202–212), confirms incremental minidriver
acceleration with DIB-engine fallback. No proprietary text is copied.

The family contract and command-semantics contract were read from the lead's
checkout because this worktree's base predates those files. The family contract
is carried forward verbatim with one result row added. The T23 provider remains
restricted to `5333:8c2e`. Generic ownership verification still validates the
active framebuffer lease, extent, generation, memory decode and RAM exclusions;
a read-only optional BAR-selector hook handles families with differing BARs.

The early COB is placed below an encodable 4 or 8 MiB boundary (Haiku's
MM48C14 description gives offset bits 22:11), with cursor and offscreen rows
below it. A larger reported VRAM amount never sets an offset bit belonging to
the read-only entry count. Less than 4 MiB of firmware-reported VRAM, a scanout
that leaves no disjoint COB, or more than the early 16 MiB aperture selects
software. The reference material does not qualify a different transport for
2 MiB boards. This does not change the existing newer-chip memory limits.

The optional-sweep dispatch in the generic hardware-check page now reaches
DM9 after a clean unsupported sweep, while retaining the historical two-depth
sweep for existing adapters. Retained markers, a failed clear, an unsafe sweep
or a missing completed native COPY do not pass. The family host fixture also
runs the production `fbdev_accel_validate` against every chip recipe at both
depths, proving native submissions and exact saved-block restoration in its
register model.

Host evidence and limits are recorded in `build/codex/f2-112-report.md`.
Register models establish packing, sequencing and failure handling, not physical
silicon execution. The lead still owns DM9/T23/other-card hardware qualification.
