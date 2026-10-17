# F3-41 bounded window presentation

## Review amendment: live scanout and sample timing

Rechecked the [QEMU 10.1 VGA row loop](https://raw.githubusercontent.com/qemu/qemu/v10.1.0/hw/display/vga.c)
(`vga_draw_graphic`) and [Linux v6.12 cooperative scheduler](https://raw.githubusercontent.com/torvalds/linux/v6.12/kernel/sched/core.c)
(`__cond_resched`) before applying the 2026-10-17 review amendment. QEMU
captures dirty metadata and renders rows from VRAM; that does not require a
second complete plane image. Ciuki's previous delivery instead copied all
267092 bytes of `cvga_state` and the host seam timed every yield. Remove
that copy/storage and read the live model between the retained four-row
cooperative points. A window may tear: a store can appear in this sample or
the next. Dirty bits and change counters written after the initial harvest
stay pending, and f3-38's previous scanout bytes still drive exact comparison
on the next sample. Cached row addresses/geometry and palette are bounded
derived state, not an atomic VGA snapshot. Register changes remain pending
for the next sample as well. Keep the cadence, compositor, initialization,
VBE and scheduler rules. Production boundaries call only `kpreempt_point()`;
the host replay times each whole sample, with no clock read per yield.

The measurements below describe the first delivery; the amendment's rerun
table and commands are appended to `build/codex/f3-41-report.md`.

Amendment results: both complete DOOM replays pass the same instruction/trap
counts. Presenter cost falls from 81.897 to 66.296 us/update and interval
presenter CPU from 436267 to 348782 us. Guest-emulator share of VM plus
presenter CPU decreases from 95.216762% to 95.213654%; the strict nondecrease
gate therefore fails. The cpustat replay remains at 4 ms. The full sanitized
suite passes 18883386 VM checks and 173117 independent VGA assertions,
with 24/24 simulated desktop events (8 ms maximum ready delay). The i686
kernel links to 5655676 bytes and passes its FPU/SIMD audit. Four-slot BSS
shrinks by 1040 KiB. These host results do not qualify physical frame pacing
or satisfy every amendment gate.

Research checked before implementation (2026-10-17):

- [Linux v6.12 scheduler](https://raw.githubusercontent.com/torvalds/linux/v6.12/kernel/sched/core.c),
  `__cond_resched` and `__cond_resched_lock`: yield only at a safe boundary;
  release exclusion before scheduling. Ciuki's compositor already stages at
  most 4096 bytes and releases the framebuffer presenter mutex per tile, but
  never reaches a cooperative point between tiles. Add the point there,
  then revalidate the live grant, framebuffer generation and cancellation.
- [Linux v6.12 softirq](https://raw.githubusercontent.com/torvalds/linux/v6.12/kernel/softirq.c)
  bounds continuous work by both time and iteration count. Use fixed row/page
  boundaries in the window presenter and VM initialization; keep harvest/PTE
  transactions atomic. The requested 4 ms is Ciuki's qualification limit.
  Scalar `cvga_render_row8` and optimized `cvp_*_row32` conversions remain
  indivisible rows (at most 640 source pixels in this window path). Their
  production caller yields at most every four rows, including unchanged
  rows. `present_xrgb_row` completes one staging tile before releasing exclusion
  and yielding. Shared pure VGA helpers keep their full-screen semantics.
  The synthetic BIOS's initial plane clear offers an optional point per
  4096 plane addresses; the adapter accepts it only for an unstarted,
  creating VM. Live mode changes and native full-screen transactions stay
  atomic. Standalone model builds have no required scheduler dependency.
- [Linux RT bandwidth documentation](https://docs.kernel.org/scheduler/sched-rt-group.html)
  explains why preferential execution needs a reserved share for other work.
  Preserve Ciuki's existing 8/10-tick focus budget and device precedence;
  a PIC-pending focused virtual IRQ requests service at the next safe point
  within that budget. Masked/in-service IRQs must not manufacture urgency.
  Guest IF/STI shadows govern delivery, not the scheduling hint: dispatch
  lets the guest finish the code leading to its next delivery boundary.
- [QEMU 10.1 VGA](https://raw.githubusercontent.com/qemu/qemu/v10.1.0/hw/display/vga.c),
  `vga_get_params` and `vga_draw_graphic`: start address, pan, line compare
  and palette affect scanout. Keep exact planar comparison and the 2x path.
  A repeated CRTC high-byte write is a completion hint, not proof of a new
  page. Detect real start-address transitions at register-write time, retain
  dirt across deferred frames. Following the review amendment, read live
  planes across yields with cached derived geometry/row addresses and a
  palette lookup. New stores retain dirt for the next sample; new page
  flips/palettes stay pending without repeatedly aborting the lower output
  rows. Window tearing is acceptable; no complete VGA copy is retained.
- [DOOM's clock definition](https://raw.githubusercontent.com/id-Software/DOOM/master/linuxdoom-1.10/doomdef.h)
  defines 35 game tics per second. The window policy samples 13h/Mode X/Y
  at half programmed refresh unless actual page changes occur each refresh;
  it does not change guest timers, status-port timing or full-screen output.
- [SeaVGABIOS 1.16.3](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/vgasrc/vgabios.c),
  mode setup and BDA updates: retain memory-clear/preserve behavior and BIOS
  data. Bootstrap cooperation splits the existing clear; it does not replace
  the initial non-preserving mode set with a preserving request.

`vm_perf_video` live and retained `vm-window` records include `present_rate`,
the per-VM window sampling ceiling in Hz (not measured FPS). A fast flip
requires two successive real address transitions separated by half to one
and a half refresh periods, and expires two periods after the last change.
Low/high writes within half a refresh period count as one transition. Repeated
completion writes do not qualify. The cap is checked before taking dirty
bits, so a deferred update remains pending. Text phase handling is retained.
Deadlines retain microsecond phase across samples and are quantized once to
the 1 kHz host tick. Individual intervals can differ by one tick; the phase
does not drift to 66 Hz by rounding every 70 Hz frame up to 15 ms. Missed
deadlines are dropped rather than replayed in a catch-up burst.

The small start-up slot reservation is included in the existing admission
ledger before yielding; another creator cannot reuse that slot or spend its
commitment. The amendment removes the first delivery's 267092-byte per-VM
VGA snapshot; existing `sizeof(struct dosvm)` resident accounting adjusts
automatically. No resident quota, native API, or public structure changes.

The windowed synthetic VBE row loop also yields every four rows and checks
the saved mode/buffer extent before dereferencing it after a yield. Its
existing full-refresh policy is retained; the half-refresh policy applies
to VGA 13h/Mode X/Y. No native full-screen provider or SB16 model changes.

Read-only 43d72a5c T23 input reports 523260 KiB usable RAM, 1698 free DMA
pages and 126816 free NORMAL pages. Live desktop records repeat 28 ms
stretches; the blackbox prefix also retains the cited older 29–31 ms DOS
window stretches. These are input observations, not results of this patch.

Host commands, measured results and remaining physical qualification are
recorded in `build/codex/f3-41-report.md`. Host fake clocks cannot certify
T23 framebuffer bus time or the QEMU f1-input 24/24 gate.

Initial-delivery host evidence: the complete sanitized suite passes 18882292 VM
checks and 173117 independent VGA assertions; the i686 link and integer-only
audit pass. The production scheduler/cpustat fake-clock replay changes
presentation stretches from 30 to 4 ms and services 24/24 desktop events
with an 8 ms maximum ready delay under continuous IRQ pressure. Cadence
vectors produce 35/30 updates per second and 70 refresh-spaced flips after
warm-up, including split low/high writes.

Initial-delivery before/after DOOM DEMO1 replays pass identical 5096860656 instructions
and 12587106 traps with zero unexpected instructions. Matching complete
intervals n=1..254 measure presenter segments 1335 -> 19 us, but instrumented
presenter means 83.149 -> 1005.109 us/update and guest-emulator share of
VM-plus-presenter CPU 95.154% -> 94.395%. Per-point host thread-clock reads
and frozen-capture work are included; no physical speedup is inferred. The
separate sanitized synthetic presenter benchmark measures
283.889 -> 287.263 us/update. The report preserves raw sums, commands,
measurement limits and remaining QEMU/T23 gates.
