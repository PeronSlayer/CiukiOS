# F0-30 interrupt-off and voluntary scheduling audit

Research checked before implementation, 2026-10-15. Scope: all kernel
`irq_save`, raw CLI and interrupt-gate paths; serial.c is owned by F0-29.
This document distinguishes deterministic host replay from physical evidence.

## Sources and implementation decisions

- [Linux v6.12 i8259.c](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/kernel/i8259.c):
  mask/unmask updates a cached IMR and uses one ordinary OUT to the selected
  PIC, without a delay/readback. Ciuki already had no wait in pic_unmask;
  eliminate redundant writes, open the cascade separately and measure each
  OUT with TSC. A millisecond sample is not proof of an SMI or EC cause.
- [SeaBIOS PIC source](https://raw.githubusercontent.com/coreboot/seabios/master/src/hw/pic.c):
  retain the cascade and existing explicit EOI ownership; no ELCR changes.
- [Linux v6.12 wait.h](https://github.com/torvalds/linux/blob/v6.12/include/linux/wait.h)
  and [lock rules](https://docs.kernel.org/locking/locktypes.html): enqueue,
  test and publish BLOCKED atomically; release IF before scheduling and
  reacquire before rechecking. A wake in that gap is already queued and
  schedule must not enqueue it twice. Never protect the sleep itself.
- [Linux i8253](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/clocksource/i8253.c):
  a latched PIT count and PIC pending bit cannot recover arbitrarily many
  missed periods. Keep tick accounting honest, without inventing catch-up
  ticks or relaxing the H-page test.
- [Intel SDM 3B](https://cdrdv2-public.intel.com/874250/253669-090-sdm-vol-3b.pdf):
  V86 is entered using an extended IRET frame with physical IF controlled
  by the monitor. Retain the small atomic frame-copy boundary; emulation,
  diagnostics and waits run with physical interrupts enabled.
- [Intel SDM, TSC ordering](https://cdrdv2-public.intel.com/671427/253669-sdm-vol-3b.pdf):
  raw RDTSC does not serialize the instruction stream. PIC OUT measurement
  uses the existing P6-compatible CPUID/RDTSC helper on each side. The
  observed interval includes serialization overhead; do not subtract an
  invented calibration or use an SSE/RDTSCP fence on the P6 baseline.

Supplied package inspected read-only in the main checkout:
`third_party/Ultimate Windows Driver Pack (for vista,xp,98,95,me,2000,2003)/UIU/A1/`:
`ich3core.inf` (3.30.1009) identifies 8086:2480 as the 82801CA LPC bridge
and delegates to machine.inf/ISAPNP_DRV. `ich3usb.inf` (3.20.1001) maps
2482/2484/2487 to UniversalHCD.Dev on Win9x and UHCI.Dev.NT on NT;
it supplies no PIC pacing, SMI-disable or EC workaround. `ich3ide.inf`,
`ich3idem.inf`, `ich3corm.inf`, `ich3smb.inf` are checked alongside these.
The DOS driver inventory (`third_party/drivers/# Description.txt`,
`qcdrom.txt`) supplies no authority for changing chipset/PIC registers.
Therefore this change adds no speculative chipset/EC writes.

## Evidence boundary

Read-only input: `legacy/local/physical/a395ffa8/` in the main checkout.
BOOT2.LOG line 83 reports 10032 ms, 9976 ticks, 56 lost, 1762 us,
site c0100938; its later records have 193 violations and 0/57 gaps.
BOOT.LOG's blackbox prefix describes a previous boot; do not mix those
counters with the current boot. serial.log is damaged and cannot qualify
timing. The mutable main-checkout ELF is not a symbol oracle for this image.
The directive supplies the PIC/wait site mapping; retained logs alone do
not identify an instruction or establish the holder of every external gap.

## Findings and changes

- `pic_unmask` had no wait at all. Slave unmask wrote both IMRs every time,
  even when neither changed. Updates now skip unchanged masks; the cascade
  opens separately and each mask transaction contains one OUT. The new
  `pic_mask writes/out_max_us` record separates port time from its enclosing
  section. This can distinguish a slow OUT from time elsewhere, but cannot
  identify SMI/EC activity without further evidence.
- `kwait_wait_until`, `task_sleep_ms`, native nanosleep/wait-word and the
  reaper published BLOCKED and entered schedule under the caller's IRQ
  section. Schedule already stopped its timer at context switch; therefore
  the old 2117 us sample does **not** establish that the entire sleep ran
  with IF=0. Release the caller lock before schedule, then reacquire to
  recheck. The enqueue/check remains atomic and wake-in-the-gap is tested.
- The largest overlooked memory work under IF=0 was chain-4 VGA harvest and
  seeding: up to 64 KiB of conversion in `dosvm_vga_sync/direct/present`.
  These thread-owned operations now keep IF enabled. They contain no
  scheduling point, so the guest cannot change the source or its mappings.
  No timer hook accesses VGA state. This avoids a new lifetime/ownership
  protocol while removing the IRQ blackout.
- PMM previously scanned up to 196608 pages under one IRQ lock, and
  contiguous/DMA allocation also reserved the full run under that lock.
  Scan/reserve/rollback now use at most 32 bitmap entries per section.
  Tentative pages are reserved before IF opens; rollback frees only that
  invocation's prefix. Heap refill builds its private page list outside
  the IRQ lock, then splices it onto the current free list atomically.
- PCM fetch copies at most 64 frames between IF openings, event fetch at
  most 16 entries, and the output audio tap processes 16 frames per lock.
  Consumers do not schedule during these copies; occupied entries remain
  pinned and producers drop incoming data instead of overwriting them.
- Console drawing already ran with IF enabled. However a row claim crossed
  `kpreempt_point`, the service traversed the entire framebuffer and every
  background log line repainted the pinned H page. Normal repaint is now
  resumable: eight cells or one scanline per call, one presenter claim,
  released before scheduling. History appends consume at most 64 bytes per
  IRQ section, including CR-only input. Pinned results redraw only on an
  explicit page change. Panic retains synchronous painting.
  `console_repaint_ms` sums CPU time in the sampler window; it is not the
  maximum duration of a single repaint call.
- CPU sampler already snapshots one task at a time and sorts/formats after
  IF restore. Its global copy is bounded by 16 overrun events. Process
  enumeration, however, sorted up to 64 PIDs under IF=0: snapshot only the
  64 timer-owned counters there and construct/sort records with IF enabled.
- Storage commits, blackbox CRC/sector writes and USB enumeration/worker
  sleeps were already outside their RAM/PCI locks. No new filesystem,
  device-register or USB ownership policy is introduced. These paths are
  covered by the production storage and USB host suites.
- IRQ locks now use the caller function as their default label. Hard IRQs
  name `irq_timer`/`irq_dispatch` unless a driver supplies a finer label.
  DOS monitor entry/return, V86 entry and the IRET/leave boundary are timed;
  the register frame remains saved while the timing helper runs. Kernel
  copy-fault returns do not close a surrounding caller's IRQ interval.

## Host model and limits

| Fixture | Worst replayed IRQ-off interval | What it proves |
| --- | --- | --- |
| Production PIC mask functions + production timing | 7 us with a 5 us OUT and 1 us per lock boundary | One OUT per transaction, cascade semantics, redundant/invalid updates, measured OUT distinct from enclosing section |
| Production PMM + bitmap-boundary instrumentation | 64 us at 1 us per bitmap access | At most 32 read/set pairs per section, including fragmentation/rollback and full-zone allocation |
| Production timing + clocked PIT | 90 us admitted section, at every PIT phase | Over 10 s: 10000 delivered ticks, zero lost edges, 0/0 gaps; a 4.23 ms negative control still loses edges and records a software gap |
| Production console | 0 us rasterization with IF=0; <=8 cells or one scanline per service call | IF enabled during drawing, claim released before yield, retries retain the current chunk, grant changes abort, pinned-page silence, synchronous panic |
| Production wait services | No wait under the caller IRQ lock | IRQ wake between unlock and schedule queues exactly once, spurious wakes recheck, deadlines and cleanup remain correct |
| CPU/storage/USB/DOS production harnesses | Functional replay; no per-site numeric WCET supplied | Counter/layout, ownership, commit and wake invariants; these tests do not certify physical latency |

Costs above are explicit synthetic boundary costs. The 90 us envelope is
an admitted workload in the timer fixture, **not** a measured WCET for every
kernel function. There is no existing instruction-cost host model for all
callbacks, assembly/cache operations or real devices. Do not turn a source
loop bound into a measured microsecond claim. For the remaining sections
below the audit records the finite work or callback obligation; numeric
WCET remains unqualified. In particular:

- `program_wc` must keep interrupts disabled while caches/MTRRs change;
  WBINVD/MSR latency is hardware-dependent. It is a boot/mode-change
  exception, not runtime H-page work. No safe 100 us bound is established.
- Hard IRQ chains have the existing 250 us aggregate budget, checked after
  each callback. A single callback may overrun before it can be deferred.
  DOS audio backlog service has the existing 64 ms DSP-time work cap;
  its worst CPU cost across all VMs has no <=100 us proof. These are
  explicit residual gaps in a universal 100 us claim, not silently waived
  or counted as passing. Changing that policy needs a reviewed audio/IRQ
  directive and focused timing evidence.
- Queue/wait conditions must remain bounded, nonblocking and allocation-free.
  Panic, double fault, final reboot/power-off and bootstrap CLI are terminal
  or pre-timer exceptions. An SMI, NMI or stalled I/O instruction can extend
  any observed section. Keep those samples and timer gaps visible.
- F0-29 still owns serial.c and its publication/driver waits. This branch
  does not claim that the old 126 ms DOS-task holder was uniquely identified:
  the retained EIP is inside a shared helper and the matching old ELF is
  not available here. Removed VGA blackouts, finer console scheduling and
  F0-29 must be remeasured together. The next image must supply the actual
  holder if a >20 ms reschedule delay remains.

Thus the fixes and host evidence are ready for integration review, but the
universal per-section 100 us bound and the physical F0-30 gate are **open**.
The existing timer definition and H-page thresholds remain unchanged.

## Complete source inventory

Every direct IRQ-save/CLI site is indexed below. `lock_ram` (bootlog),
`lock` (blackbox), RTC begin/end and serial evidence admission are paired
boundaries; their callers are listed separately. Default runtime attribution
uses the enclosing function name, with explicit labels for sampler, input,
records and VM entry/exit. Line numbers describe this worktree, not a395ffa8.


| Source | Direct IRQ-save / CLI lines | Work bound / reason |
| --- | --- | --- |
| [arch/entry.asm](../../src/kernel/arch/entry.asm#L18) | 18, 68 | Bootstrap/paging transition; pre-timer exception. |
| [arch/isr.asm](../../src/kernel/arch/isr.asm#L115) | 115 | Double-fault terminal CLI; interrupt gates implicitly clear IF. IRET closes guest timing. |
| [arch/pic_pit.c](../../src/kernel/arch/pic_pit.c#L38) | 38, 54, 96 | Mask: 7 us model/one OUT; QPC: two PIT latches + IRR sample, no wait. |
| [arch/trap.c](../../src/kernel/arch/trap.c#L142) | 142, 155 | Saved syscall/fault return; IRQ callback time remains a driver obligation. |
| [audio/audio_dev.c](../../src/kernel/audio/audio_dev.c#L157) | 157 | Fixed backend/VM diagnostic snapshot; no wait. |
| [audio/backend.c](../../src/kernel/audio/backend.c#L115) | 115, 129, 342 | Fixed transport publication/snapshot; progress invokes the bounded cursor callback (SB count caveat above). |
| [audio/vm_source.c](../../src/kernel/audio/vm_source.c#L197) | 197, 217, 233, 244, 258, 269, 277, 642, 699, 715, 727 | Fixed slot/control snapshots; tap loop is 16 frames per lock, mixer stays outside. |
| [core/blackbox.c](../../src/kernel/core/blackbox.c#L19) | 19 | Capture <=256 bytes; flush RAM extraction <=480 bytes; CRC/write/flush/wait outside. |
| [core/bootlog.c](../../src/kernel/core/bootlog.c#L43) | 43 | Capture <=256 bytes, drain extraction <=128 bytes; 16 sync targets; all VFS/commit/waits outside. |
| [core/console.c](../../src/kernel/core/console.c#L72) | 72, 304, 337, 353, 406, 475 | Presenter claim, <=161-byte history snapshot, <=64 input bytes; rasterization IF=1. |
| [core/cpustat.c](../../src/kernel/core/cpustat.c#L92) | 92, 149, 157, 168, 185, 217 | Fixed global snapshot <=16 events, one task per lock; divisions, ranking and output IF=1. |
| [core/devices.c](../../src/kernel/core/devices.c#L138) | 138, 250 | Fixed tick snapshot or bounded NIC/resource inventory copies; no driver callbacks. |
| [core/fpu.c](../../src/kernel/core/fpu.c#L190) | 190, 206, 215, 244 | Save/release ownership and <=512-byte architectural state/translation, no scheduling. |
| [core/kheap.c](../../src/kernel/core/kheap.c#L41) | 41, 57, 61, 89, 102 | Constant freelist splice/pop/free and ledger; refill page construction IF=1. |
| [core/main.c](../../src/kernel/core/main.c#L78) | 78 | Initial bootstrap CLI; interrupts not yet available. |
| [core/mtrr.c](../../src/kernel/core/mtrr.c#L178) | 178 | MTRR/cache transition; no certified 100 us bound, named mode-change exception. |
| [core/output.c](../../src/kernel/core/output.c#L48) | 48, 83, 167, 174, 197 | <=256-byte record/sequence/RAM/serial publication; fatal paths terminal; F0-29 owns serial. |
| [core/pmm.c](../../src/kernel/core/pmm.c#L123) | 123, 159, 170, 198 | 64 us model; <=32 bitmap entries scan/reserve/rollback per section; free constant-time mutation. |
| [core/power.c](../../src/kernel/core/power.c#L333) | 333 | At most four ACPI control OUTs after storage shutdown; power-transition exception. |
| [core/reboot.c](../../src/kernel/core/reboot.c#L22) | 22, 44 | Terminal reset/halt; intentionally never returns to normal timer work. |
| [core/registry.c](../../src/kernel/core/registry.c#L40) | 40, 54, 105, 126, 139, 159, 168, 185, 202, 209, 221, 234, 471, 522, 551, 570 | <=64 resource/32 PCI inventory, <=8 chain members; one PCI address/data transaction; proofs/output IF=1. |
| [core/serial.c](../../src/kernel/core/serial.c#L150) | 150, 167, 195, 219, 244, 307, 313, 323 | Separate F0-29 ownership; indexed only, unchanged. |
| [core/sync.c](../../src/kernel/core/sync.c#L34) | 34, 42, 86, 117, 131, 141, 199 | Atomic enqueue/test/state and unlink/wake queue walks; sleep outside, predicates must be bounded. |
| [core/task.c](../../src/kernel/core/task.c#L169) | 169, 237, 273, 414, 423, 444, 463, 474, 486 | Run queues, task-list/canary/guard walks and switch hooks; switch time excludes suspended tasks; sleep outside. |
| [core/timing.c](../../src/kernel/core/timing.c#L103) | 103, 116 | Fixed counter snapshot/reset; all unit conversions after restore. |
| [core/work.c](../../src/kernel/core/work.c#L28) | 28, 37, 44, 75, 95 | Constant queue/stat publication; callback, output and yield IF=1. |
| [drivers/ac97.c](../../src/kernel/drivers/ac97.c#L525) | 525, 531 | One PCI config address/data transaction. |
| [drivers/ata.c](../../src/kernel/drivers/ata.c#L235) | 235, 280, 298, 921 | Fixed request/status publication and command/DMA arm; polling/data transfer/deadlines outside. |
| [drivers/ata_dma.c](../../src/kernel/drivers/ata_dma.c#L133) | 133, 213 | One bounded PCI enable/revoke/readback transaction; allocation/stop proof outside. |
| [drivers/audio_pci.c](../../src/kernel/drivers/audio_pci.c#L7) | 7, 15 | One PCI address/data transaction. |
| [drivers/es137x.c](../../src/kernel/drivers/es137x.c#L84) | 84, 112 | Page+cursor read pair or seven start/readback accesses; no polling. |
| [drivers/i8042.c](../../src/kernel/drivers/i8042.c#L113) | 113, 121, 257, 355, 371, 522, 550, 566, 580, 591, 608, 664, 734, 1127, 1180, 1202, 1213, 1229, 1236, 1250, 1369, 1378 | One byte per IRQ; 7 us data pacing (256 sample escape); bounded queue/decoder snapshots; command waits outside. |
| [drivers/net.c](../../src/kernel/drivers/net.c#L52) | 52, 81, 89, 377 | Fixed pending/stat snapshot or PCI address/data/readback transaction. |
| [drivers/net_ne2000.c](../../src/kernel/drivers/net_ne2000.c#L92) | 92, 130 | Nine init page/register writes, or page/read/page cursor; no remote DMA wait under this lock. |
| [drivers/net_pcnet.c](../../src/kernel/drivers/net_pcnet.c#L10) | 10, 15 | Atomic RAP/data register pair. |
| [drivers/sb16.c](../../src/kernel/drivers/sb16.c#L8) | 8, 13, 49, 113, 120 | Mixer pairs, <=8 agreeing DMA count attempts (24 accesses), <=7 DMA setup writes; DSP waits outside. |
| [fs/fs_port.c](../../src/kernel/fs/fs_port.c#L98) | 98 | One RTC sample, at most 13 index/data reads; UIP retries/delay outside. |
| [fs/fs_port.h](../../src/kernel/fs/fs_port.h#L56) | 56, 62, 69 | Constant mutex test/set/release; kwait and storage service IF=1. |
| [include/ciuki/cpu.h](../../src/kernel/include/ciuki/cpu.h#L51) | 51, 97, 104 | Instrumentation wrapper, not an additional lock owner. |
| [probes/payload.asm](../../src/kernel/probes/payload.asm#L125) | 125 | Synthetic ring-3 guest CLI negative test, not host IF. |
| [probes/registry_probe.c](../../src/kernel/probes/registry_probe.c#L211) | 211, 276 | Shadow IRQ/PIC fixture only; no physical I/O; bounded synthetic chain checks. |
| [proc/clock.c](../../src/kernel/proc/clock.c#L16) | 16, 65, 110, 117, 131 | Fixed tick/signal snapshot; nanosleep releases before scheduling. |
| [proc/process.c](../../src/kernel/proc/process.c#L160) | 160, 217, 228, 491 | 64 timer counters only; PID sort/record construction outside; notification/reaper publication constant. |
| [proc/syscalls_proc.c](../../src/kernel/proc/syscalls_proc.c#L14) | 14, 216 | Tick/system ledger snapshot; process enumeration outside the IRQ lock. |
| [proc/thread.c](../../src/kernel/proc/thread.c#L320) | 320 | Fixed TLS/task publication after user-copy validation. |
| [proc/waitword.c](../../src/kernel/proc/waitword.c#L58) | 58, 77, 94, 107, 117, 153, 184, 195 | Waiter/key/signal/clock operations; queue bounded by thread population; schedules unlocked. |
| [usb/usb_core.c](../../src/kernel/usb/usb_core.c#L606) | 606, 612, 615 | Single PCI config pair or 64-bit tick; controller workers and HID transfers/sleeps outside. |
| [vm/biosvm.c](../../src/kernel/vm/biosvm.c#L147) | 147, 227, 232, 439, 548, 653, 673, 692, 801 | Fixed pending/diagnostic/frame/lease publication; physical monitor emulation/waits IF=1; raw return copies bounded. |
| [vm/dosvm.c](../../src/kernel/vm/dosvm.c#L103) | 103, 141, 166, 295, 535, 578, 593, 791, 797, 835, 851, 1114, 1133, 1202, 1223, 1250 | Fixed coherent device/input/PIC snapshots, <=16 PIC slots; emulation/waits/output IF=1; entry/exit named. |
| [vm/dosvm_bios.c](../../src/kernel/vm/dosvm_bios.c#L463) | 463, 485 | Virtual PIC or PIT/keyboard/audio transaction; audio backlog CPU budget caveat above. |
| [vm/dosvm_disk.c](../../src/kernel/vm/dosvm_disk.c#L43) | 43, 117 | Constant busy-gate test/set; VFS, disk waits and lookup outside. |
| [vm/dosvm_fullscreen.c](../../src/kernel/vm/dosvm_fullscreen.c#L54) | 54, 84, 178, 200, 216 | Fixed owner publication, 32 aperture PTEs/TSS bitmap change; capture/firmware/park wait outside. |
| [vm/dosvm_perf.inc](../../src/kernel/vm/dosvm_perf.inc#L91) | 91, 135 | One vector counter or 16 IRQ counters; divisions/formatting outside. |
| [vm/dosvm_sb16.c](../../src/kernel/vm/dosvm_sb16.c#L78) | 78, 655, 666, 678, 690, 711, 714, 727, 765, 778, 837, 927 | PCM <=64 frames, events <=16 entries per lock; fixed diagnostics/register snapshots. |
| [vm/dpmi_dispatch.c](../../src/kernel/vm/dpmi_dispatch.c#L764) | 764, 812, 1197 | Virtual PIC snapshot/IRQ frame transaction; bounded descriptor/stack work; no sleep/output. |
| [vm/v86_entry.asm](../../src/kernel/vm/v86_entry.asm#L24) | 24, 70 | Entry: <=92-byte frame copy; leave restores continuation; second CLI is copied guest mouse stub. |

Inventory: 242 direct source occurrences (including helper definitions and guest/terminal exceptions).

Indirect RAM-lock boundaries (all callers, including definitions):

- `core/bootlog.c`: 40, 43, 54, 108, 126, 153, 192, 197, 200, 230, 243, 261, 274, 291, 300, 312, 341.
- `core/blackbox.c`: 16, 19, 93, 105, 113, 127, 139.
- `fs/fs_port.c`: 98, 99, 100.

`dosvm_vga.c` no longer contains an IRQ-save section: harvest/seed/exchange
remain thread-owned and never schedule. Physical interrupt-gate entry is
also audited in `trap.c` even though the IF clear is implicit in the gate.
All evidence is host-only until the lead runs the required QEMU suites and
T23 H page on one integrated image SHA-256.

## Implementer verification

Sequential `nice -n 19` runs only: full kernel host suite exit 0;
8 focused latency tests OK; final sanitized DOS VM replay 8110128 checks
PASS, maximum record 237 bytes. Kernel link/FPU-SIMD audit and the canonical
DOS payload FAT16/header/24-file readback pass. Final command output and
artifact hashes are retained verbatim in `build/codex/f0-30-report.md`.
No QEMU, full-image build, systemd scope, diary, commit or push was run.
