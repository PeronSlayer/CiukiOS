# Directive f3-05: per-VM Sound Blaster 16, OPL3 and virtual DMA device state

- **Step:** F3 (audible output is F4). **Contracts:** `dos-dpmi-contract.md`
  ("Virtual devices and reuse": independent SB16 DSP/mixer, OPL and
  virtual DMA state per VM; DMA addresses resolve only through the owning
  VM; virtual audio IRQ timing continues while guest IF is clear; reuse
  table for `guest_peripherals.c`, `session_audio.c`, `session_opl.cpp`),
  `f4-audio-media-contract.md` ("DOS VM audio path": the interface F4
  consumes: block boundaries, DSP causes, OPL register events with VM
  generation and sample-clock timestamps, DBOPL in a ring-3 worker),
  directive f3-00 (`dosvm_port_register`, `dosvm_irq_raise`, `dosvm_tick`,
  memory helpers) and f3-01 (PIC).
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f3-vm-audio-state`, from `main` after f3-00 and
  f3-01 are merged. Files: new `src/kernel/vm/dosvm_sb16.c` (DSP command
  matrix 2.x–4.xx: reset/status/version, speaker latch D1h/D3h, direct
  DAC, single-cycle and auto-init 8/16-bit DMA (`14h/1Ch/48h/B0h–CFh`),
  pause/continue/exit auto-init, sample rate `40h/41h/42h`, mixer `224h/
  225h` registers incl. IRQ/DMA selection and volumes, IRQ acknowledge
  `22Eh/22Fh`), `dosvm_dma.c` (8237 master/slave: address/count/page
  registers, mode, mask, flip-flop, auto-init, bounded transfers resolved
  through the VM's memory only), `dosvm_opl.c` (OPL3 register file,
  timers/status/reset per VM, address/data banks `388h–38Bh`/`220h–223h`,
  event queue with VM generation and sample-clock timestamps for F4's
  worker), `src/kernel/include/ciuki/dosvm_audio.h` (the interface F4
  consumes: per-VM PCM block fetch with the block-boundary rule, cause
  latch, OPL event drain), `src/kernel/probes/f3_probes_audio_state.c`,
  DOS-side payloads under `src/probes/dos/`, `tests/suites/f3-vm.json`
  additions, host tests (migrated `guest_peripherals` audio tests),
  `docs/design/f3-acceptance.md` (`## f3-05`).

## Scope

1. The DSP/mixer/DMA/OPL state machines above, per VM, with no physical
   output: PCM blocks are produced into a bounded per-VM queue read by F4
   (or discarded with a counter when no consumer exists), honoring the
   block-boundary rule: generate only to the next SB block, latch the DSP
   cause, raise the virtual IRQ through f3-01's PIC, and resume after the
   guest's acknowledgement or after the configured block duration with
   the lateness counted.
2. Virtual timeline: DSP DMA consumption advances on the VM's sample
   clock (programmed rate), driven by `dosvm_tick`, independent of guest
   IF; pending causes follow SB/PIC semantics (no unbounded queue).
3. OPL: register writes timestamped and queued; timer 1/2 with status
   flags and IRQ (if enabled) on the VM clock; OPL2 compatibility and
   OPL3 new mode (`105h`), four-operator pairing (`104h`) as register
   state only (synthesis is F4's worker); per-VM instances.
4. Probe `f3:vm-audio-state`: DOS-side payloads program DMA and DSP
   (8/16-bit, single/auto-init, pause/resume), count IRQs against the
   expected block timing with IF clear for 100 ms (pending stays
   correct), read status/version/mixer back, program OPL timers and
   verify the event queue contents and timestamps; records per the
   contract; cases on t23/e500/min128.
5. Host tests: the T0 "DOS audio" row's state parts (every advertised
   PCM mode, mixer, OPL mode/pair/rhythm registers and timers, 64-unit
   auto-init double buffer producing 32 fresh blocks and 32 eligible
   services with a fake output clock).

## Acceptance by the lead

Host tests; kernel build; QEMU `f3-vm` audio-state cases PASS;
`f2-all` unaffected.
