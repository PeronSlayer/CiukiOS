# f3-40: SB16 auto-init DMA and late IRQ acknowledgement

Research checked 2026-10-17, before implementation. Host evidence only;
QEMU and T23 qualification remain with the lead.

Primary references:

- Creative, [Sound Blaster Series Hardware Programming Guide](https://www.phatcode.net/downloads.php?action=get&file=sbhwpg.pdf&id=243),
  §§2-4–2-5, 3-5–3-6, 3-28–3-29: auto-init reloads the DMA address/count;
  DSP blocks interrupt at constant intervals, normally at half-buffer ends.
  Mixer 82h identifies the cause; reads of 2xEh/2xFh acknowledge 8/16-bit
  DSP interrupts. PIC EOI is a separate operation. B6h with mode 30h is
  signed 16-bit stereo output; its count is samples minus one.
- DOSBox Staging, upstream [soundblaster.cpp](https://github.com/dosbox-staging/dosbox-staging/blob/main/src/hardware/audio/soundblaster.cpp),
  `play_dma_transfer`, `sb_raise_irq`, `read_sb` (retrieved 2026-10-17):
  the block counter reloads in auto-init independently of pending IRQs;
  an already pending cause does not activate another interrupt, and the
  status read clears it. Behaviour reference only; no source copied.

Pre-change audit, line numbers in the directive's checkout:

- `dosvm_sb16.c:84`: `waiting` selects diagnostic state `irq_wait`.
- `dosvm_sb16.c:191–221`: every boundary reloads the DSP counter, sets
  `waiting`, and sets `wait_until` to the later of the next block end and
  one host tick; the state record is `irq_wait reason=complete`.
- `dosvm_sb16.c:126–130`: matching acknowledgement clears `waiting` and
  calls `sb_advance` to catch up immediately.
- `dosvm_sb16.c:291–295`: `sb_advance` returns before `wait_until` when
  waiting; expiry clears it, increments `late`, flags the next PCM segment,
  and records `running reason=irq_late`.
- `dosvm_sb16.c:197`: acknowledgement age uses the latest boundary's
  tick-rounded timestamp, not the first unacknowledged block end.
- `dosvm.c:802–805, 817–835, 855–889`: diagnostics precede injection;
  fullscreen checkpoints, scheduling and reports can also follow injection
  without checking newly pending IRQs before returning to the guest.
- `audio/vm_source.c:436–476`: playback checks active/paused, never
  `waiting`; missing timer-produced PCM can nonetheless cause starvation.

Decision required by f3-40: supersede the acknowledgement-driven boundary
rule in f3-acceptance/f4-audio-media-contract. DMA consumes the live guest
ring at nominal timer positions regardless of the latched DSP IRQ. No
predictive guest-memory reads or host-side synthetic silence/repetition are
needed: DMA wrap itself replays unrefilled data. `waiting` reports the line
only. Merged boundaries lose an IRQ edge, never audio time. Preserve guest
IF, instruction shadows, virtual PIC priority and separate DSP/PIC acks.

Implementation and metric interpretation:

- `sb_advance` crosses boundaries within its existing 64 ms work budget;
  status reads never call it. Its loop also accommodates one-frame blocks
  (45 blocks per millisecond at 45 kHz), rather than limiting progress to
  the old 32 boundary iterations. The one-second backlog/drop policy and
  actual DMA mask/pause/exit behaviour remain bounded as before.
- Cause timestamps retain each cause's first unacknowledged nominal block
  end. `irq_merged` counts boundaries while the line is high. The existing
  `late` counter now counts those boundaries; the old `irq_late` transition
  is no longer generated. `waiting`/`irq_wait` continues to report a pending
  cause without withholding PCM. The consumer already uses active/paused
  and sample timestamps, so `audio/vm_source.c` needs no new replay layer.
- Injection is attempted immediately after emulation and at final guest
  return, after checkpoints, scheduling and reports. The latter transaction
  keeps physical IF clear through return. IF, shadow, PIC arbitration and
  transactional frame validation remain authoritative. Diagnostic output
  encountered there is deferred to preemptible monitor context.
- IRQ latency is sampled immediately before guest return into a committed
  ISR frame, rather than at DSP acknowledgement or frame construction.
  Calibrated TSC measures intervening wall time, including preemption after
  frame construction, plus the nominal-boundary backlog at the first edge.
  A mode-changing handler keeps its measurement pending across the return
  to the kernel worker; the worker samples it with IF clear just before
  re-entering the new guest frame. The worker also offers a newly pending
  audio IRQ to the normal arbitration path at that reentry boundary.
  Without TSC calibration, the physical timer/sample timeline is the
  fallback. Nominal DSP positions have 48 kHz frame granularity; host tests
  establish accounting, not physical timing accuracy.
- Every five seconds of live playback the timer snapshots a bounded bank;
  the VM worker emits one protected `klog` cadence row outside guest IRQ
  chains, including ordinary boots without DIAG_VERBOSE. If the worker
  cannot log, its pending snapshot is retained and subsequent counts stay
  in the live bank. No timer/refill formatting or physical PIC changes.
- `blocks`, `merged` and `late_acks` are interval counts; a late ack clears
  a cause after at least a second block end. `ack_max_ms` is the completed
  acknowledgement maximum, rounded up from nominal timeline frames.
  `irq_latency_max_us`/`irq_latency_avg_us` cover ISR entries in the bank.
  `descheduled`, `parked`, `if_clear` count sampled pending-entry milliseconds
  (parked takes priority, then descheduled); a committed ISR frame's cleared
  IF is not mistaken for a guest CLI. `underruns` is the change in VM-source
  starved output frames, not an assertion about physical codec FIFO errors.

Host replay coverage uses B6h/30h, DMA5, 11025 Hz and two 128-frame stereo
halves. Every integer ISR delay 0–60 ms is combined with every integer
preemption gap 0–40 ms (2,501 cases, 600 ms each). A single gap in the middle
of each run delays both guest servicing and host refill while physical
timer/cursor progress continues. The immutable guest ring makes the exact
published waveform comparable across all delays, including merged IRQs;
the existing responsive double-buffer tests independently verify fresh
refills. No game binary is executed by this protocol replay.

Host results (2026-10-17): all 2,501 cases preserve exact DMA position and
published PCM; 2,375 merge IRQs, VM-source underrun frames are zero and the
maximum simulated transport shortfall is 64 output frames (1.33 ms), below
one 128-frame DSP block (11.61 ms). The final ASan/UBSan DOS VM harness
passes 23,884,001 checks, including V86/16/32-bit DPMI, STI shadow, final
scheduling/worker reentry, EXEC error observation before IRQ construction,
post-construction preemption, tiny blocks, acknowledgement widths and
five-second protected records. The complete host kernel script and the
standalone audio/deadline suite also pass. API reference generation and
`make CAP= apps` pass after the public flag-comment update. All host jobs
are sequential at nice 19. These are host results, without QEMU/hardware
or OS image builds; full report: `build/codex/f3-40-report.md`.
The requested `python3 scripts/build_kernel.py` also passes, producing the
5,656,600-byte kernel ELF with its undefined-symbol and FPU/SIMD audits.
