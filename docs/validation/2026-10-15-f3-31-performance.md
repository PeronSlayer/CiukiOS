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

F3-33 research and implementation decision, 2026-10-16:

- Intel's [8254 datasheet, pp. 7–8](https://www.scs.stanford.edu/10wi-cs140/pintos/specs/8254.pdf)
  specifies that a counter latch holds its value until the programmed reads
  consume it; a repeated latch before consumption is ignored. Extend the
  bounded VGA burst to byte PIT/PIC IN/OUT and flag-preserving MOV setup,
  using the same virtual device handlers and stopping at the existing IRQ,
  shadow, debug, return-tracker and reschedule boundaries.
- Intel's [8259A datasheet, pp. 14–16](https://www.scs.stanford.edu/10wi-cs140/pintos/specs/8259A.pdf)
  defines OCW2 EOI, mask and priority behavior. Direct instruction dispatch
  must retain the authoritative PIC model, including initialization, poll,
  priority rotation and cascade state; no physical port permissions change.
- The [Intel SDM V86 chapter](https://cdrdv2-public.intel.com/874250/253669-090-sdm-vol-3b.pdf)
  defines the real-mode interrupt stack and IRET operand widths. Combine
  adjacent checked stack transfers while retaining the scalar path for
  16-bit stack wrap and preserving preflight before interrupt-frame writes.
  Read each current IVT entry; never cache guest hooks across exits.
- [DPMI virtual interrupts](https://www.delorie.com/djgpp/doc/dpmi/ch4.4.1.html)
  distinguishes physical IF from virtual IF and warns that protected POPF
  does not restore the latter. Keep the existing saved-image proof, its
  bounds and the return tracker. Reduce proof fetch overhead within each
  invocation, and reject work early only when no register or stack image
  can satisfy the existing equality check. No backwards signature matching
  or timeout-based IF restoration is introduced.

Host costs include fake trap transport, production frame save/dispatch/return
and host paging callbacks. They cannot measure physical privilege transitions,
register-save assembly or CPU IRET latency. The lead must compare the T23
`vm_perf_cost` rows on the integrated image. Implementation results and exact
commands are recorded in `build/codex/f3-33-report.md`.

F3-33 host results: the full sanitized suite passes, including 9,201,587 VM
checks; the i686 kernel build and integer-only audit pass. Three alternating
O2 host runs have median latch/read/read sequence cost 220 to 152 ns (three
exits to one), RM INT/IRET pair cost 242 to 174 ns, reflection alone 67 to
27 ns, and a 512-byte saved-FLAGS proof exit 4846 to 1834 ns. The same proof
with an impossible image falls from 4845 to 116 ns. Pure PIC dispatch and
return preparation remain 7/9 ns; a lone PIC exit is flat within host noise.
These isolate fake transport/production helpers, not CPU exception latency.

Real DOOM graphics startup and Quake CRT preserve their exact baseline
instruction/trap counts; end-to-end DOOM startup is unchanged within noise.
The existing DOOM menu restoration mismatch and the Quake runner's x87
subset limit reproduce in both binaries. The read-only DOS image also
passes window keys, DPMI16/32, DOS/4GW, the expected privileged fault and the
host TCG model. Detailed per-exit costs, complete-demo comparison, commands,
input hashes and remaining lead qualification are in the F3-33 report.

The F3-33 full DOOM DEMO1 before/after comparison passes all 5,026 tics with
identical 5,096,860,656 instructions / 12,717,948 traps, zero unexpected
instructions, prompt restoration and full DPMI cleanup. PM flag means fall
867 to 765 ns and RM IRET 712 to 680 ns; PIT/PIC is flat within host noise.
A single wall-time pair is 558.627/520.175 s, while completed-exit CPU sums
remain approximately unchanged. This is not a hardware speedup claim. A
startup comparison with the existing runtime GUEST_SLICE=64000 option also
passes with identical before/after counts; it does not reproduce the T23's
80 us PM flag cost. The exact physical cost attribution remains for the lead.

F3-38 research and implementation decision, 2026-10-16:

- [QEMU 10.1 VGA](https://raw.githubusercontent.com/qemu/qemu/v10.1.0/hw/display/vga.c),
  `vga_ioport_write`, `vga_mem_readb` and `vga_mem_writeb`: mask indexed
  registers before comparing memory-access selections; preserve low-byte-first
  word OUTs, all four read latches, write modes and read-map selection. Reuse
  the registered virtual handler directly only while its cached registration
  still owns the port. Equal backing and access permissions need no PTE walk;
  different private planes still require an exchange. Never alias distinct
  planes merely because their contents happen to match.
- [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
  INC and I/O instructions: INC preserves CF and updates OF/SF/ZF/AF/PF.
  Speculative register setup, including INC DX/EDX, may retire only with a
  fully decoded following virtual I/O; retain existing debug, interrupt,
  shadow, return-tracker and reschedule boundaries.
- QEMU's graphics scanout retains start address, line compare, pan and wrap.
  Keep the scalar address setup as oracle. Specialize four-plane RGB expansion
  and compare indexed rows exactly with the last successfully presented image
  before uploading page-dirty rows. A failed upload invalidates that image;
  palette/blank/geometry changes force repaint. The bounded index cache adds
  300 KiB plus row tags per VM slot, accounted in existing resident admission;
  no memory cap changes. The supplied T23 boot records 523,260 KiB usable,
  2,064 free DMA pages and 126,816 free NORMAL pages. Its private planar
  aperture was already active. Share the fast data-port handler's clock pair
  with remap attribution: that subcost now conservatively includes register
  update/selection, avoiding a second pair of serialized clock reads per OUT.
- [Linux Savage](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c),
  checked also against the supplied read-only source: `vgaHWProtect` and
  `vgaHWRestore` program shared sequencer/graphics registers. All windowed
  accesses remain virtual; no physical port permissions or provider changes.

Host timing includes fake trap transport and production C helpers, but cannot
measure physical exception entry, register-save assembly, TLB refill or IRET.
Before/after commands, measurements and limitations belong in
`build/codex/f3-38-report.md`; QEMU and T23 acceptance remain with the lead.

F3-38 host results: the final sanitized suite passes 18,822,841 VM checks
and 173,117 independent VGA assertions; the i686 link and integer-only audit
pass. Three alternating O2 samples measure planar row conversion 47,456 to
22,889 ns/frame and dirty queries 3,087 to 749 ns/frame. A sparse planar
update uploads two rows instead of 104; the production synthetic DOOM
presenter changes from 62.859 to 41.256 us/frame, with harvest unchanged
within noise. The common index/MOV/INC/data sequence uses one exit instead
of two, retaining native INC flags and the existing burst guards.

The complete real DOOM DEMO1 before/after replays both pass all 5,026 tics
with identical 5,096,860,656 instructions / 12,587,106 traps and zero
unexpected instructions. Across matching complete intervals n=1..254, VGA
cost per presented frame falls from 4,023.575 to 2,777.252 us (1.45x), and
presenter cost from 138.371 to 84.171 us (1.64x). **The requested 2x overall
target is not met.** Isolated conversion/dirty-query gains do not qualify
that gate. New d598bf4c BOOT logs arrived and were re-read before handoff;
they identify kernel 9c34b9859185 and contain previous-boot/history records,
so they are supporting inputs, not validation of this patch. No image was
built: the explicit no-image constraint excludes build_dos_image.py; existing
DOSBOOT/game fixtures were read-only. QEMU and T23 acceptance remain open.
