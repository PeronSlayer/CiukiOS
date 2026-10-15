# F3-31: DOS VM performance attribution and windowed video

Research checked before implementation, 2026-10-15:

- [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
  I/O permission bitmap and virtual-8086 extensions: a granted IN/OUT accesses
  the physical device. VME does not redirect ports to RAM or remove protected
  mode I/O exits. Keep the existing virtual IF recovery and ownership rules.
- [Microsoft's grabber account](https://devblogs.microsoft.com/oldnewthing/20231127-00/?p=109067)
  distinguishes windowed graphics capture from full-screen display ownership.
  Near-native full-screen DOS is not evidence for near-native windowed DOS.
- [Linux Savage driver](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c),
  `vgaHWRestore` and `vgaHWProtect`, uses the same sequencer, graphics and CRTC
  registers for physical scanout. The supplied IBM `Driver/D/A0400/SSAV1014.INF`
  identifies `VEN_5333&DEV_8C2E&SUBSYS_01FC1014` and installs the S3 miniport;
  its `VgaCompatible`, `SupportSAMM`, `UseAGPDMA` and panel preferences are
  driver policy, not a second private VGA register bank. The supplied DOS
  driver catalogue contains no such register-shadowing facility either.
  Decision: no windowed physical port grant or hardware register change.
- [QEMU VGA model](https://raw.githubusercontent.com/qemu/qemu/v10.1.0/hw/display/vga.c),
  `vga_mem_writeb` and graphics scanout: chain-4 selects the plane with address
  bits 0/1; scanout addressing and dirty tracking must retain wrap, pan and
  line-compare semantics. Optimize the existing RAM model, preserving the
  scalar renderer as the comparison oracle.
- [DPMI virtual interrupts](https://www.delorie.com/djgpp/doc/dpmi/ch4.4.1.html):
  virtual IF is separate from physical IF. Do not grant physical PIT/PIC ports
  or reinterpret protected POPF as a trapping instruction.
- [Quake DOS video](https://raw.githubusercontent.com/id-Software/Quake/master/WinQuake/vid_dos.c)
  includes several mode families. The supplied run uses chain-4 mode 13h;
  a 640x480 window is not proof of 640x480 guest rendering. Guest VBE remains
  unavailable under the existing contract.

The existing implementation already has writable private aperture pages,
D-bit harvesting, batched planar PTE swaps, bounded VGA OUT batching and
virtual IF recovery. Neither normal INT 10h nor INT 16h uses the storage
task; only backing-file I/O does. Preserve guest IVT hooks and DOS services.

Measurement decision: collect scheduled guest/monitor CPU time on ordinary
boots, publish one interval every five seconds to the bounded RAM disk-log
sink, and retain the first four/latest twelve intervals. Include exit causes,
top ports/vectors, remapping, harvest, presentation and timer/audio service.
Sub-costs overlap their containing totals and must not be summed. Presentation
changes and harvests are observations, not a guest FPS counter. No hardware
FPS or QEMU pass is inferred from host timings.

Detailed results, limitations and lead qualification steps are recorded in
`build/codex/f3-31-report.md`.

Review fixes, research checked before implementation, 2026-10-16:

- Linux's [locking lessons](https://docs.kernel.org/locking/spinlocks.html)
  explain local interrupt exclusion and the need for all shared accesses to
  use the same lock. CiukiOS is uniprocessor: detach one of two preallocated
  per-VM interval banks and capture raw cumulative scalars under IRQ exclusion;
  restore IF before ranking, resetting tables, computing deltas or dividing.
  All port/vector producers select the active bank while holding IRQ exclusion.
  A reporting guard excludes another reporter until the detached bank is clear.
  Label the capture `dosvm_perf_snapshot` for the existing timing probe; host
  instrumentation bounds work, while physical microsecond qualification remains
  the lead's responsibility under the ownership contract's 50/250 us budgets.
- Linux's [ring-buffer design](https://docs.kernel.org/trace/ring-buffer-design.html)
  describes loss of old events when an overwrite buffer fills and processing a
  detached reader buffer. The repository's `blackbox_capture` overwrites oldest
  bytes in its 64 KiB pending queue. Keep the always-on `vm_perf` family solely
  in the boot-log runtime RAM ring via `rec_diag_disk_bootlog`; retain black-box
  capture for ordinary `rec_diag_disk` callers. No storage worker or disk format
  changes are needed.

The real-image baseline DOOM host replay reaches graphics but fails its menu
restoration comparison, with 70,216 return-tracker debug traps. The tracker
is required by the f3-10 mixed-selector/near-return proofs; lowering its
512-step ceiling already failed earlier replays. Preserve the bound and TF
ownership. Avoid refreshing the forward proof's stack/descriptor snapshot
at each intermediate owned step: acquire it at the boundary where tracing
stops, before a flags save or the next native instruction. Nested contexts
still take their own snapshots. Host tests must preserve image arming,
watchpoints, stack/selector guards and return-context suspension.

The latest supplied disk log contains 43 partial `vm_perf_live` intervals,
although it lacks cause/share rows. At n=148/149 there are 34,299/37,559 DB
exits with no VGA port writes or page faults. The rows omit generation IDs;
their association with the interleaved gen=96273 session is an inference.
The heartbeat interval is two seconds, so n=1–53 is not 53 seconds. During
that span the generation-tagged VGA counters retain eight faults and one
remap: a private writable chain-4 aperture was already active.

The full host kernel suite passes, including 9,153,137 VM checks and the
whole-guest DPMI/Quake CRT cases. The 300-frame synthetic chain-4 presenter
benchmark changes from 699,184 to 508,984 ns/full frame and from 145,001 to
31,884 ns/sparse frame; sparse uploads fall from sixteen output rows to two.
The 120-frame DOOM planar replay remains approximately unchanged. The real
DOOM startup replay retains exactly 141,193,935 instructions/336,062 traps,
with unchanged host CPU duration within noise. These are host measurements,
not T23 FPS. The baseline's DOOM menu restoration comparison still fails.
Real Quake reaches an x87 instruction outside libx86emu's implemented subset;
full Quake gameplay and the requested QEMU/hardware acceptance remain open.

Live top-port costs charge a complete monitor exit, including its batched
sequence, to the first port. The fixed table reports saturation explicitly.
Vector paths distinguish RM dispatch, synthetic BIOS and PM INT handlers.
Subcosts overlap; DMA/audio service is virtual SB advancement, not native
mixing. Presentation and harvest counts must not be relabelled as guest FPS.

The real DOOM shareware 1.9 DEMO1 replay completes all 5,026 tics, returns
to the DOS prompt, releases the DPMI pool and reports zero VM faults or
unexpected CPU instructions. Its reported 43,344 realtics use the harness's
instruction-clock PIT, not physical time. A same-harness control with only
the old dispatcher measures 41,526 versus 39,627 us for the same 70,912
startup DB exits (4.6% less host DB time); total startup remains unchanged
within noise. The final full suite also checks INT 80h cost/vector attribution
through its direct IDT gate and the early IRQ accounting guard. QEMU and T23
validation, sound-enabled gameplay and the hardware FPS targets remain open.
