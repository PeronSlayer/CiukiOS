# Directive f4-05: DOS VM audio output (SB16 PCM, direct DAC and OPL3 into the kernel mixer)

- **Step:** F4 (the "DOS VM audio path" row; follows f3-05 and f4-01, lead
  decision 8). **Contracts:** `f4-audio-media-contract.md` ("DOS VM audio
  path", "Kernel mixer and physical backends", the 60 ms provisional budget
  of "Native audio API for processes", acceptance rows T0 DOS audio and T3
  `f4-audio-dos`), `dos-dpmi-contract.md` (virtual PIC and IF stay in the
  monitor), directive f3-05 and its interface `ciuki/dosvm_audio.h`
  (consumer token, block-boundary segments, timestamped register events,
  CT1745 levels), directive f4-01 (`audio_mix`, `audio_service`, the
  P_DEVICE refill worker, 256-frame quantum, eight published periods),
  `test-architecture.md`.
- **Implementer:** Claude Opus (subagent), standing in for Codex under the
  owner's 2026-10-11 rule; host tests only, the lead runs QEMU.
- **Worktree:** `wt/f4-vm-audio`, from `wt/f3-merge` at `b5325f0`.
  Files: new `src/kernel/audio/vm_source.c` (per-VM consumer source),
  new `src/kernel/audio/opl3.c`, `opl3_tables.h` and
  `src/kernel/include/ciuki/opl3.h` (integer OPL3 synthesizer, see point
  5), `src/kernel/include/ciuki/audio.h` (VM source interface and
  counters), `src/kernel/audio/mixer.c` and `audio_dev.c` (VM sources in
  the mix and in the transport activity rule), `src/kernel/vm/dosvm.c`
  (attach at creation, detach on the release path, idle kick from the
  tick), `src/kernel/include/ciuki/dosvm_audio.h` and `dosvm_sb16.c`
  (CT1745 output gain in `dosvm_audio_info`, additive only), new
  `src/kernel/probes/f4_probes_vm_audio.c` and the F4 selector tables
  (`selector.c`, `scripts/test/loader_model.py`,
  `src/boot/ciukldr/menu.inc`, `tests/host/record_scope_test.py`,
  `scripts/test/evidence.py`), new DOS payload `src/probes/dos/vm_play.asm`
  (`VMPLAY.COM` in `scripts/build_dos_image.py`), `tests/suites/f4-audio.json`
  (three cases), host tests, `third_party/vsbhda/README.md` (the new
  derived use of DBOPL), `docs/design/f4-acceptance.md` (`## f4-05`).

## Scope

1. **Attach and release.** Every DOS VM created while a qualified audio
   backend is ready gets one F4 consumer source: `dosvm_create` attaches it
   after the VM is complete (f3-05 `dosvm_audio_attach`), and the single
   release path `dosvm_destroy` revokes it before `dosvm_audio_close`.
   Without a backend nothing is attached and f3-05 keeps discarding with
   its counter. Source state is static per VM slot (no allocation at
   attach, no change to the VM ledger or its pins); the worker owns the
   render state and reinitialises it on a new attach epoch. A revoked
   token (another consumer attached, VM gone) ends the source with a
   counter, never a stale fetch.
2. **PCM conversion.** f3-05 already normalises every advertised DSP
   layout (8/16-bit, signed/unsigned, mono/stereo, single/auto-init,
   80h silence) to signed 16-bit stereo segments at the programmed rate;
   F4 converts that rate to the mixer's 48 kHz with the f4-01 integer
   linear interpolation and a fractional phase kept across fetches,
   quanta and contiguous segments (same rate, start within two timeline
   frames of the continuation). A non-contiguous segment (pause, DMA
   stall, new transfer) starts a new stream at its stamped start. Direct
   DAC events (DSP 10h) are a held level from their timestamp. ADPCM
   segments (flagged `UNSUPPORTED` by f3-05) play the silence f3-05
   produced and count as unsupported; recording, MPU-401 and OPL
   passthrough stay unadvertised.
3. **Block boundaries and pacing.** F4 never asks f3-05 for data across a
   block boundary (one segment per fetch, peek before adopting the next
   segment) and never reads guest memory; the virtual PIC, IF, IRQ causes,
   acknowledgements and the late/stale rules remain in f3-05. Each source
   maps the VM timeline (48 kHz frames) to output frames with a target
   latency of eight published periods plus 512 frames (53.3 ms, inside
   the 60 ms provisional budget). The worker reports the source's lead
   over physical output at every quantum; when it falls below a floor the
   source inserts output silence without consuming timeline (hold), when
   it exceeds the band it skips timeline forward (counted), so catch-up
   never replays committed PCM and never speeds up the VM timeline.
   Frames that arrive after their timeline position are dropped and
   counted (`late_frames`); output frames without data while the DSP is
   active count as `starved`; f3-05's `LATE`/`STALE`/`OVERRUN` flags are
   counted per source. A stalled guest only produces silence for its own
   source: the worker never waits on a VM.
4. **Gains.** PCM and DAC use CT1745 master × voice, OPL uses master × FM
   (5-bit levels, 2 dB steps, 31 = 0 dB, Creative guide chapter 4), times
   the output gain 41h/42h (0/6/12/18 dB); then the per-source gain/mute
   (default 100, unmuted; focus does not mute), then the f4-01 sum,
   master gain and final saturation. A muted source still consumes.
5. **OPL3 synthesis.** One chip per VM fed by f3-05's timestamped events,
   applied in order at their timeline frame before synthesising the
   affected samples (late events applied at once and counted; a `GAP`
   resynchronises from `dosvm_audio_opl_registers`). OPL2 compatibility,
   NEW (105h), four-operator pairs (104h), rhythm and the A/B channel
   routing (C0h bits 4/5) mapped to left/right. The synthesizer and the
   reasons for running it in the refill worker instead of the contract's
   ring-3 worker are recorded in `f4-acceptance.md` `## f4-05` after
   research (licence, CPU on 1998-2004 targets, determinism) and are a
   lead decision point.
6. **Transport activity.** A VM source with DSP activity, queued data,
   pending events, a sounding OPL voice or a held DAC level keeps the
   transport running; an idle VM lets it stop as in f4-01. The VM tick
   wakes the refill worker once when a VM produces data while the worker
   is idle; P_DEVICE threads still block between services (f1-18).
7. **Evidence.** Per-source counters and taps readable by a probe: output
   frames rendered, source frames consumed, late/starved/hold/skip,
   lead min/max, blocks and their sequence log (sequence, first frame,
   frames, flags), OPL events/late/resyncs, PCM and OPL path taps
   (non-zero frames, FNV-1a digest, sum of squares, peak, zero crossings,
   digest of the first 4096 OPL frames of a note) and a ring tap of the
   final published frames. Probe `f4:vm-audio` (selector only, like the
   other F4 probes) boots one VM running `VMPLAY.COM`: the f4-05 PCM
   regression (64-unit auto-init double buffer over 2,048 units, 8-bit at
   8000 Hz: 32 fresh sequential blocks rendered, 32 block services, no
   repeated half), 16-bit signed stereo at 12000 Hz with voice at -12 dB,
   an OPL2 440 Hz note and an OPL3 880 Hz note routed left only; records
   bounded to 256 bytes; without a backend `not_run reason=device_absent`.
   Cases `vm-audio-qemu-t23`, `vm-audio-qemu-e500`, `vm-audio-qemu-min128`
   with the AC97 device exception; values only a run can give are marked
   `PIN-FROM-FIRST-RUN`.
8. **Host tests.** Production f3-05 + `vm_source.c` + `opl3.c` + mixer in
   the DOS VM harness with a fake output clock: every advertised PCM mode
   reaches the mix, exact phase across split fetches and contiguous
   segments, the 32-fresh-block regression with services and no repeats,
   a stalled guest (IF clear 100 ms) leaves native mixing untouched and
   counts silence, hold/skip bounds and no timeline replay, gains, OPL
   event timing/late/GAP, attach/revoke/detach and two independent VMs;
   the C OPL3 port compared sample for sample with the pinned DBOPL C++
   on register scripts covering OPL2/OPL3, every waveform, 4-op modes,
   rhythm, vibrato/tremolo and feedback; the generated tables checked
   against a recomputation.

## Out of scope

Recording, MPU-401/MIDI synthesis, ADPCM decoding (f3-05 does not
advertise it), the desktop volume UI for VM sources, ESS backend, physical
latency qualification (T4), a ring-3 OPL worker (unless the lead decides
so after reading the f4-05 decision).

## Acceptance by the lead

Host tests (`make test-host`), kernel build with the FPU/SIMD audit, DOS
image build; QEMU `f4-audio` vm-audio cases PASS on t23/e500/min128 with
the first-run values pinned; `f3-vm` audio-state and `f4-audio`
audio-native unaffected; decision on the OPL placement recorded in a diary
entry.
