# Native video family integration

The integration retains the family implementations and their existing
qualification gates. The registry uses exact PCI matches in vendor/family
order; no vendor wildcard or generic policy is added. The S3 family retains
its T23 mode provider and diagnostic sweep. One native-only DM9 comparison
dispatch checks mandatory FILL/COPY evidence, CPU safety and quarantine.
Optional family self-tests remain separate from that comparison.

## Primary-source comparison and decisions

- The owner's read-only Windows 98 Resource Kit, chapter 24,
  `build/rskt/wrkc24_3.htm`, Display Driver Overview, confirms incremental
  minidriver acceleration with DIB-engine fallback. The common software engine
  remains authoritative for unsupported calls.
- [X.Org EXA `exa.h`](https://raw.githubusercontent.com/XQuartz/xorg-server/master/exa/exa.h)
  separates pre-submission refusal from submitted work and requires ordering
  before CPU access. The integrated diagnostic never counts software replay
  as native evidence; quarantine alone never authorizes CPU access.
- [Linux v6.12 i810_accel.c](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/i810/i810_accel.c)
  supplies ring packets, ring-enable cache flushing and idle checks.
  [i810_main.c](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/i810/i810_main.c)
  allocates and binds backing for the ring rather than treating a firmware
  aperture address as allocation authority. The original f2-119 firmware-ring
  reservation is superseded here by the directive's required kernel-owned
  pages. No PCI command or translation-enable change is imported.
- [Linux v6.7 intel-gtt.c](https://github.com/torvalds/linux/blob/v6.7/drivers/char/agp/intel-gtt.c)
  (`i810_setup`, `i810_write_entry`, `intel_gtt_mappable_entries`) and
  [intel-agp.h](https://github.com/torvalds/linux/blob/v6.7/drivers/char/agp/intel-agp.h)
  establish the 64-KiB i81x table, valid system-memory PTE encoding, and
  read-only bridge aperture selection. Save and restore borrowed invalid PTEs;
  never infer firmware scratch identity or overwrite valid mappings. Search
  the entire table for aliases and require the borrowed range to lie above
  the full active framebuffer and every valid firmware mapping.

The pinned Linux sources were retrieved through the connected GitHub reader
after ordinary web retrieval returned cache misses. Review copies stay under
ignored `build/research/integ-video/`. No proprietary text is copied.

## Required i810/i815 reservation

Allocate and DMA-pin kernel-owned ring and cursor pages, borrow only invalid
GTT entries at the proven aperture top, and preserve translation control and
all valid firmware entries. The CPU accesses owned RAM through its direct map,
with cache publication before GPU exposure. Stop must prove engine idle,
disable/read back the ring, restore/read back original ring registers and
PTEs, flush publication, and only then unpin and free pages. Failed proof
retains pages, mappings, claims and the rejecting guard. No writes to firmware
stolen backing outside the active framebuffer are authorized.

The i740 FIFO backend retains its independent transport. Existing Intel Gen2
chip-specific gates and NVIDIA transport approvals are preserved.

Verification commands, branch pins, PCI coverage and unresolved qualification
items are recorded in `build/codex/integ-video-report.md`.
