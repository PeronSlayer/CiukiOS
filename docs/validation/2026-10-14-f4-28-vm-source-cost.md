# F4-28 VM source refill arithmetic

Research before implementation, 2026-10-14. Host evidence only; the lead
owns QEMU and hardware qualification of the unchanged 1000 us service bound.

## Sources, repository comparison and decision

- [Creative Sound Blaster Hardware Programming Guide](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/SoundBlaster.pdf),
  mixer chapter: retain the F4-18 linear level policy and 0/6/12/18 dB
  output gain, with final saturation after accumulation. DSP/DMA/PIC timing
  and speaker policy stay as in F4-17/F4-18.
- [DOSBox DBOPL upstream](https://raw.githubusercontent.com/joncampbell123/dosbox-x/master/src/hardware/dbopl.cpp),
  `ForwardLFO`, `BlockTemplate`, `GenerateBlock2/3`: synthesis already runs
  channel blocks bounded by the LFO, skipping silent channels. Ciuki's
  `opl3.c` implements this order, and `vm_source.c` bounds each block at the
  next register event. Keep that batching and the pinned VSBHDA reference;
  changing envelope advancement or combining across events would change
  FM semantics. No new synthesis algorithm or sample approximation.
- [LLVM wide division implementation](https://llvm.googlesource.com/llvm-project/+/a259686db1c5f3f9904f266cbb084a8baeaf86d7/compiler-rt/lib/udivmoddi4.c)
  illustrates the width/sign/call overhead on 32-bit targets. The existing
  VM interpolation and digital-output peak calculation perform wide
  divisions inside frame loops; the Q16 gain operands are also unnecessarily
  wide. Host x86_64 timing alone cannot expose the i686 helper cost.

Decision: retain exact rational interpolation (truncate toward zero), exact
Q16 gain composition and sample rounding (floor), and every existing tap,
level and digest. Precompute the rational resampler advance/remainder at
stream start. S16 interpolation uses unsigned 32-bit magnitudes. Gains have
32-bit operands and one native widening multiply followed by a fixed shift;
the intermediate product needs more than 32 bits at +18 dB and is not
approximated or clipped. PCM/FM contributions and diagnostic percentage
arithmetic fit signed 32 bits; only timeline counters, sum-of-squares and
the mixer accumulation remain wide. Keep FM rendered per event/LFO block.

The 44.1 kHz output converter needs wider *values* than S16: preserve exact
interpolation using quotient/remainder decomposition before the phase
multiply, rather than shrinking or saturating the accumulated VM sum.
Timeline division is outside sample loops; small-divisor word decomposition
can preserve full 64-bit uptime without runtime wide division helpers.
The SB16 DSP's independent wide clock/DMA timing is outside the directed
mixer application and must not be rewritten for a render optimization.

Host replay uses the F4-24 400 callbacks of 480 frames: 50 single-period and
350 double-period refills, with real virtual DMA queues and the production
VM source/mixer. CPU time excludes host preemption at nice 19; its ceilings
are regression assertions, not evidence that TCG meets 1000 us.

## Implemented result and host evidence

The whole i686 VM-source object now has zero wide division/modulo/multiply
helper references, versus seven wide-division call sites before the change.
At 48 kHz the two interpolations and two diagnostic output percentages
accounted for up to 1024 helper invocations per 256-frame period. The gain
code-generation check proves one native signed multiply and no call for
Q16 sample application. The guard also covers the entire native mixer and
OPL3 objects, SB16's refill-facing APIs, and a 4 KiB kernel frame bound.
The SB16 DSP clock divisions are deliberately outside that API guard.

The two-entry advance table is set on every stream start; the PCM window,
phase, late/gap handling and lookahead order remain the same. Q16 operands
are narrowed without discarding fractional bits. Timeline word division
has a wide oracle over full-width random values and UINT64_MAX. Arithmetic
oracles cover every CT1745/source combination, S16 extrema at every phase,
negative Q16 floors, amplified cancellation and persistent rational steps.
Compile-time bounds prove diagnostic percentage and four-VM delta safety.

No FM synthesis or golden changes were necessary. The pinned DBOPL C++
reference passes 1157007 frames (1133551 sounding, 1164452 checks).
The new 4 s replay output digests match the original implementation:
11025 Hz U8 mono `b2851cb2`, 22050 Hz S16 stereo plus FM `078b7b84`.
It replays exactly 50 single and 350 double refills, with no starvation,
skip, late frame or sequence gap and a full eight-period lead.

Optimized CPU stage means (ns/256 frames, baseline → final): PCM 11025
1234 → 1479; PCM 22050 1255 → 1503; FM block 1958 → 1756; scalar FM control
9654 → 9082; gains/taps/levels 3483 → 3209; saturation/output tap
1582 → 1584. The x86_64 host does not cross 1 ms in any stage, and its
measurements do not show a general source-path speedup. They expose the
relative FM batching cost; the target-specific improvement is removal of
software wide division and wide gain operands on i686.

Optimized final replay means, single/double ns: mono 6840/13249,
stereo+FM 9008/17581; maxima 16620 and 24100 ns. Baseline means were
6668/12895 and 8693/17078. Timed replay includes output pin hashing and
publication diagnostics, not AC97 production service execution. Host cost
ceilings are 100/200 us means and 1 ms maximum; both sanitized and optimized
runs pass. The native production-service replay remains in the audio suite.

Sequential validation under `nice -n 19`:

- `ASAN_OPTIONS=detect_leaks=0 bash tests/host/audio/run.sh`: PASS,
  11114925 production checks, ASan/UBSan, VM wide arithmetic oracles,
  sanitized/optimized costs and i686 code-generation guard.
- Complete DOS VM section of `scripts/test/host_kernel_tests.sh`: PASS,
  5833856 checks, max record 237 bytes, ASan/UBSan, plus optimized VM costs.
- Pinned upstream OPL3 reference: PASS, counts above.
- `python3 tests/host/test_audio.py`: PASS, 9 tests (QEMU mocked).
- `python3 tests/host/test_dosvm.py`: PASS, 8 tests.
- `git diff --check`: PASS.

LeakSanitizer is disabled because ptrace is unavailable in this sandbox;
explicit VM/page cleanup checks still run. The full implementation report
is `build/codex/f4-28-report.md`. No QEMU, image build, commit or diary.

## Remaining qualification

The lead must run the integrated canonical image's `f4-audio` and require
`vm-audio-qemu-t23 deadlines=0`, retaining native audio and VMPLAY/FM pins.
Host CPU timings and modeled transport cadence cannot identify an absolute
TCG stage above 1 ms or establish the actual AC97 worker deadline. There
is no new image/hash, QEMU or hardware evidence from this worktree.
