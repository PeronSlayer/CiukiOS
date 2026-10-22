# f4-37: Music decoder cost on the Pentium III

Directive: `docs/directives/f4-37-music-decoder-cost-pentium3.md`. Worktree
`wt/f4-37` from `main` 35228457, implementer Claude agent (Opus), written
2026-10-22. Host evidence only: no QEMU, no image, no hardware run. The T23
confirmation is the new `[music] cost` record (`docs/design/music-player.md`,
section F4-37).

## Trial evidence (image a4d9454e)

`[music] level` records of the T23 trial: `decode_us_frame=24.6-46.4`
(kernel-clock time inside `music_decoder_read` per output frame, scheduling
included), `reader_fps=20940-40960`, `ui_mps` about 2000 (2 publications/s),
`repaint_mps=1599-5396` (1.6-5.4 repaints/s), `close starvations=119`. The
f1-101 replay derived 8.55 us of CPU per 48 kHz output frame from the
`[cpu]` windows (343 permille for 40116 frames/s): 41 % of the 1.1 GHz
Pentium III, about 10.8 million clocks per 1152-frame MPEG frame. The file
was a 48 kHz stereo MP3.

## Sources

- Intel 64 and IA-32 SDM Vol. 1 (253665-060US), 8.5.2 "Denormal Operand
  Exception (#D)": the x87 signals #D when it loads a denormal single or
  double into a register; 10.2.3.4 "Denormals-Are-Zeros" and 11.6.3
  "Checking for the DAZ Flag in the MXCSR Register": DAZ appeared with the
  Pentium 4 and SSE2 and is reserved (#GP) on earlier processors; FTZ exists
  from SSE (Pentium III) on; neither affects the x87.
  https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-1-manual.pdf
- Intel SDM Vol. 3B, "Time-Stamp Counter" (17.15 in that edition): on P6
  family processors the TSC "increments with every internal processor clock
  cycle", and SpeedStep transitions change that clock.
  https://www.intel.com/content/dam/www/public/us/en/documents/manuals/64-ia-32-architectures-software-developer-vol-3b-part-2-manual.pdf
- Intel Architecture Optimization Reference Manual 245127-001 (1998/1999,
  Pentium II/III), chapter 2 "Floating-point to Integer Conversion": FLDCW
  "is a synchronizing instruction and will cause a significant slowdown";
  SSE chapter: masked exceptions are handled in hardware "without penalty
  except denormals and underflow", flush-to-zero recommended.
  https://users.cs.northwestern.edu/~pdinda/ics-s05/doc/intel-opt.pdf
- Agner Fog, Instruction tables, Pentium Pro/II/III sections (pp. 182-185):
  "Subnormal numbers, NAN's and infinity increase the delays by 50-150
  clocks", likewise for underflow results; FADD 1 uop latency 3; FMUL 1 uop
  latency 5 reciprocal throughput 2; FLD m 1 uop (p2); FST m 2 uops (p3, p4);
  FXCH renamed; FLDCW 3 uops latency 10; FISTP 3 uops; DIV r32 latency 39;
  RDTSC 31 uops. The microarchitecture of Intel, AMD and VIA CPUs, chapter 6
  (P6): decoders 4-1-1 (6.3), retirement 3 uops per clock (6.7), branch
  misprediction 10-20 clocks.
  https://www.agner.org/optimize/instruction_tables.pdf,
  https://www.agner.org/optimize/microarchitecture.pdf
- minimp3 ea99364 (`third_party/minimp3/minimp3.h`, unmodified):
  `have_simd()` tests CPUID.1:EDX bit 26 (SSE2) and the SIMD code is compiled
  only with `__SSE2__`; the README has no note on denormals.
  https://github.com/lieff/minimp3
- L. de Soras, "Denormal numbers in floating point signal processing
  applications" (2002), sections 3.2.1-3.2.5: denormals arise when recursive
  structures (IIR filters, feedback, decays) approach zero; remedies are
  flushing or a small added offset/noise.
  https://www.musicdsp.org/en/latest/_downloads/d81fc9af8a9fa63332b248772fbb4a54/denormal.pdf
- T. Granlund, P. Montgomery, "Division by invariant integers using
  multiplication", PLDI 1994 (round-up multiply-high division).
- libmad 0.15.1b (Underbit, R. Leslie), tarball SHA-256
  `bbfac3ed6bfbc2823d3775ebb931087371e142bb0e9bb1bee51a76a6e0078690`,
  https://downloads.sourceforge.net/project/mad/libmad/0.15.1b/libmad-0.15.1b.tar.gz:
  README "100% fixed-point (integer) computation", GPL version 2 "or (at
  your option) any later version" (mad.h). No published Pentium III
  figures were found for libmad or mpg123's x87 decoders.

## The denormal hypothesis

Harness: `tests/host/desktop/music_x87_test.c` with the production
`decoder.c` built with the SDK's code generation flags for the i686
baseline (`-m32 -march=pentiumpro -mno-sse -mno-sse2 -mno-mmx -mno-red-zone
-O2 -fno-builtin -fno-strict-aliasing -fno-omit-frame-pointer`; the test
checks them against `build/tools/ciuki-sdk/manifest.json` and
`config/toolchain.json`). Disassembling `decoder.c` built by the SDK flags
for `i686-unknown-elf` and by the host i686 flags gives the same sequence of
x87 instructions in every function (2485 each); the differences are the
stack-guard load, call-alignment adjustments and padding. The x87 sticky
status flags DE (denormal operand) and UE (underflow) are read and cleared
after every MPEG frame; the synthesis and overlap state is scanned for
values below FLT_MIN after every frame; the stack below the decoder is
poisoned with a denormal bit pattern before each call, as garbage can be on
the target.

Vectors: the checked-in tones (`tone.mp3` stereo and the new `tone-mono.mp3`),
and with FFmpeg/LAME: tone plus noise with an exponential fade to digital
silence (44.1 kHz 128 kb/s and 48 kHz 320 kb/s stereo), a mono 96 kb/s fade,
an MPEG-2 22.05 kHz 64 kb/s fade and a -90 dBFS noise pass; every vector
also with its layer III `global_gain` ramped linearly to 0 and with
`global_gain` 0 plus `scalefac_scale` 1 (bit lengths unchanged, mp3gain's
method): the quietest bitstream the format can express.

Findings:

- Stereo, MPEG-1 and MPEG-2, every level including the quietest: no DE, no
  UE, no subnormal state value in any frame. The decoder is FIR throughout
  (Huffman, requantisation, IMDCT with overlap-add, polyphase window); with
  minimp3's int16 scaling the smallest requantised value is about 2^-99, far
  above FLT_MIN, and silence decays to exact zeros, never to denormals (the
  recursive case of de Soras does not occur).
- Mono: upstream minimp3 raised DE in every frame (232 of 232, 270 of 270)
  when the stack held denormal patterns. `mp3d_synth` never writes lanes 2
  and 3 of the last row of its 33x64 synthesis scratch; `mp3d_synth_granule`
  copies that row into `qmf_state`, and its mono branch copies `qmf_state`
  through x87 loads. The PCM does not depend on those lanes (identical PCM
  hashes with denormal, NaN, 1.0 and zero patterns), so the cost is an
  assist of 50-150 clocks per denormal or NaN load, up to 15 per granule.
  Remedy: the production frame driver clears that row before each granule;
  DE then stays 0 on every vector. Positive control: a denormal injected
  into `mdct_overlap` is detected in every run.

So the hypothesis is refuted for the trial's stereo file. It cannot explain
41 %: the x87 work per frame is a few hundred thousand clocks, and no
operand was denormal.

## Where the time goes instead (host, exact build)

Per-stage TSC cycles of the i686 build on the workstation (Ryzen 7 7800X3D,
TSC 4.2 GHz) for the 44.1 kHz 128 kb/s fade at 48 kHz, per MPEG frame
(before: the stage-timed driver with minimp3's int16 output):

| stage | before | after |
| --- | --- | --- |
| synthesis including PCM conversion | 417,479 | 90,150 + 18,191 |
| IMDCT | 37,116 | 35,172 |
| decode (bitstream, Huffman, requantisation, stereo) | 15,202 | 14,028 |
| adapter (rate conversion, buffering) | see below | 17,821 (5,012 at equal rates) |

Executed instructions per MPEG frame in the adapter (single-step trace,
below): 326,148 before and 108,097 after for 44.1 to 48 kHz; 292,792
before and 18,672 after for 48 to 48 kHz.

Three costs that the decoder's arithmetic does not need:

1. PCM conversion: the C float-to-int16 conversion in `mp3d_scale_pcm`
   compiles on x87 to FNSTCW, FLDCW, FISTP, FLDCW per sample: 4608 FLDCW per
   stereo MPEG frame, a synchronizing instruction on the P6. Now minimp3 runs
   with `MINIMP3_FLOAT_OUTPUT` and `pcm_s16` converts in integer arithmetic
   with minimp3's own rounding (clamps at 32766.5/-32767.5, trunc(x+0.5),
   step down below zero), branch-free on the data.
2. Rate conversion: per output frame two signed 64-bit divisions for the
   interpolation, two unsigned 64-bit divisions for the phase step and a
   64-bit multiply and division for the length check: 3941 DIV r32 per MPEG
   frame (44.1 to 48 kHz), 3613 even for 48 to 48 kHz, 39 clocks each.
   Now the phase runs in units of gcd(rate, output rate): the quotient
   trunc(diff*p/period) is computed with a multiply-high and a shift, exact
   for |diff| <= 65535 and period <= 32768 (Granlund-Montgomery round-up
   bound; larger periods keep the 64-bit division); the length is
   recomputed only when the frame count changes; runs of output frames whose
   sources are inside the decoded block skip the per-frame checks, and equal
   rates copy. Cancellation is checked per block and every 1024 frames
   instead of per source frame.
3. Input buffering: a `memmove` of up to 16 KiB after every MPEG frame; now
   frames are consumed by offset and the remainder moves only before a
   refill.

## Exactness

- `pcm_s16` equals minimp3's `mp3d_scale_pcm` evaluated exactly (in double,
  as the x87 evaluates it) for 859,813,844 floats: every bit pattern with
  exponent 100-150, both signs, and a sample of the others.
- The interpolation equals the replaced 64-bit expression for 315,356,826
  (diff, phase) pairs: every pair for periods up to 1024 (44.1/22.05/11.025/
  32/8/6/96 to 48 kHz, 48/22.05 to 44.1 kHz) and sampled phases for periods
  3200, 6000, 32767, 32768 and the 64-bit fallback (47999 to 48000 Hz).
- The adapter's output at 48 and 44.1 kHz equals the replaced algorithm
  applied to the decoded source on every vector, with request sizes 1 to
  4096 frames (run and cancellation boundaries).
- PCM against upstream minimp3's int16 decoder: at most 1 LSB. Against
  upstream on the same i686 x87 build, 0-14 samples per vector differ (14 of
  677,376 on the 48 kHz 320 kb/s fade); against the host reference build
  (SSE float), 0-78 (78 of 622,080 on the 44.1 kHz fade; upstream's own x87
  and SSE builds already differed this much before the change, because the
  x87 evaluates the window sums in extended precision).

## Cycle model for the 1.1 GHz Pentium III

Method: the i686 harness binary runs under a ptrace single-step tracer on
the workstation between two SIGSTOP markers around the production read loop
(`music_decoder_read` at 48 kHz over 24 MPEG frames cut from the vectors;
the first frame lacks its bit reservoir). Every executed instruction is
counted at its address; addresses map to minimp3/adapter stages through the
DWARF inlining information (`llvm-symbolizer`, innermost function) and to
P6 micro-operations and ports through Agner Fog's Pentium II/III tables.
Instruction counts are exact for this binary. Cycles: lower bound = the
largest of retirement (3 uops/clock), decoding (one multi-uop instruction
per clock, 4-1-1), ports 0/1, the load port, the store ports and the FMUL
unit (one per 2 clocks), i.e. perfect scheduling; estimate = lower bound
plus DIV latency, 10 clocks per FLDCW, 5 % of conditional branches
mispredicted at 15 clocks and 5 % of loads missing L1 at 8 clocks; upper =
twice the lower bound plus 25 clocks per FLDCW, 10 % mispredictions at 20
clocks and 10 % L1 misses at 10 clocks. The estimate corresponds to about
1.2 instructions per clock, the upper bound to about 0.55. Not modelled:
kernel time (stdio refills, audio writes), interrupts, L2 misses (the
decoder state, about 30 KiB, and the 16 KiB input window stay in the 256 KiB
L2), and the player threads (see below). The tracer, the model and its
outputs are analysis tools, not committed: the worktree's
`build/codex/f4-37-model/` holds them for the lead (`tracer.c`,
`model.py`, the libmad driver and the per-vector outputs).

Decoder, percent of a 1.1 GHz Pentium III (estimate, lower-upper):

| vector (output 48 kHz) | instructions/MPEG frame before / after | before | after |
| --- | --- | --- | --- |
| 44.1 kHz 128 kb/s stereo | 791,217 / 577,443 | 2.97 % (1.62-5.83) | 1.65 % (1.17-3.55) |
| 48 kHz 320 kb/s stereo | 780,499 / 510,997 | 3.14 % (1.73-6.21) | 1.65 % (1.17-3.54) |
| 44.1 kHz 96 kb/s mono | 703,159 / 446,097 | 2.70 % (1.43-5.26) | 1.29 % (0.92-2.79) |

Split after the change, 44.1 kHz 128 kb/s, estimated clocks per MPEG frame:
synthesis 231,939, IMDCT 84,450, rate conversion 68,285 (82 instructions
per output frame; 15.5 when the rates are equal), PCM conversion 57,966,
decode 28,521, libc 3,790.

Player (not traced; estimate with stated assumptions): the pump turns 500
times per second (a status devctl, a mutex pair, the level bookkeeping;
3-10 us each on the P3: 0.15-0.5 %); the window presents at most 3.5
bounded frames per second during playback (time label and seek bar, about
10,000 pixels drawn and compared each: under 0.1 %) and turns about 100
times per second for messages (about 0.05 %). Player 0.3-0.7 %.

Target: decoder plus player for 128-320 kb/s stereo at 44.1 or 48 kHz is
estimated at about 2.2 % of the 1.1 GHz Pentium III, at most about 4.3 %
with the pessimistic bounds: under the 5 % target. Before the change the
pessimistic bound was 6.2 % plus the player.

The trial cannot be explained by the decoder's instructions: the old code
on the trial's 48 kHz format models at 3.1 % (6.2 % pessimistic), the trial
measured 41 %. A factor of at least 6.6 lies outside the decoder's
instruction stream. Remaining hypotheses, each with a field of the new
record: a core clock below 1.1 GHz or throttled (`mhz`, `loop`),
uncacheable or write-through user memory (`mem`, `best_kc`), time charged
to the reader while interrupts or other threads run (`best_kc` against the
mean per frame, the stage sum against the window), denormals on the T23's
own data (`de`, `ue`, `tiny_values`).

## libmad evaluation (recommendation only)

libmad 0.15.1b built for the same i686 flags with `FPM_INTEL` and
`OPT_SPEED` (its 32x32-bit `imul`/`shrd` fixed-point multiply), driven like
minimad (decode, synthesis, rounding and clipping to int16, no rate
conversion), traced and modelled the same way on the 44.1 kHz 128 kb/s
vector: 497,408 instructions per MPEG frame, estimate 365,273 clocks
(1.27 %, lower-upper 0.93-2.72 %). The comparable minimp3 part after this
change (decode, IMDCT, synthesis, PCM; without rate conversion) is about
406,700 clocks (1.42 %, 1.01-3.04 %). Workstation time per frame: libmad
27.5-30 us, minimp3 after the change about 37 us (TSC 154,000 cycles), the
old minimp3 int16 build 63-65 us.

Recommendation: keep minimp3. With the x87 fixes the float decoder reaches
the target in the model; libmad would save about 10 % of the decoder's
clocks (0.15 % of the CPU) at the cost of a new GPL-2.0-or-later
component in a userland application, a second decoder to qualify and a
different output (libmad's own rounding). Revisit only if the T23 record
shows the decoder's own stages, measured without interference, above the
model's upper bound.

## SSE path (not implemented)

- minimp3's SIMD path requires SSE2 at compile time (`__SSE2__`) and at run
  time (CPUID.1:EDX bit 26). The T23's Pentium III has SSE but not SSE2, so
  the path cannot run on the machine that motivated this directive; in the
  baseline only Pentium 4 and Athlon 64 class desktops would use it.
- Native CiukiOS processes run with CR4.OSFXSR clear
  (`src/kernel/core/fpu.c`, "Native policy: SSE disabled"; only Win32 tasks
  get OSFXSR), so any SSE instruction in Music raises #UD, and the SDK
  wrapper rejects `-msse`. Enabling it is a kernel and SDK ABI decision for
  the lead.
- FTZ/DAZ apply to SSE only; DAZ needs a Pentium 4 or later. After the fix
  the x87 path has no denormal operands left to flush.

## Player UI

`apps/music/main.c` runs `du_run`'s loop with a frame bound: between frames
it holds `present_pending`, the toolkit's client-owned pacing flag (also
used by `apps/gfxdemo/main.c`), so turns still handle messages, input and
timers while the invalid region accumulates, and frames are at least 1/15 s
apart. The window sets the new libdeskui opt-in `present_rendered`:
`du_window_present` compares and copies only `du_ui.rendered`, the region
`du_render` drew (default unchanged for every other client). Host test
(`music_policy_test.c`): position updates only: 35 frames in 10 s (at most
3 per second), each comparing 10,383 pixels instead of the 235,200 of the
560x420 surface; with a full invalidation every 3 ms (pointer motion): 150
frames in 10 s, at most 15 per second.

## Reading the T23 record

`[music] cost` follows each `[music] level` record (format and field
meanings in `docs/design/music-player.md`). Expected on a healthy 1.1 GHz
Pentium III playing a 48 kHz file at 48 kHz: `mhz` about 1100 (lower with
SpeedStep on battery), `best_kc` about 300-1000 (one MPEG frame without
interruption), `decode_us + imdct_us + synth_us + pcm_us + resample_us`
about 80,000-190,000 per 5 s window (1.6-3.8 %), `de=0 ue=0
tiny_values=0`, `loop` about 1000-2100, `mem` about 1400-2100. A `best_kc`
in that range with a stage sum near 2,000,000 points at time charged to the
reader (interrupts, preemption); a `best_kc` many times larger points at the
CPU or the memory, and `loop`/`mem` tell which.

## Verified (2026-10-22, nice -n 19, one suite at a time)

- `tests/host/desktop/test_music.py`: 10 tests OK, including the new
  `test_x87_decoder` (20 vectors with FFmpeg, both builds), the cost record
  format and the UI frame bound in `music_policy_test.c`, the real-codec
  cost records in the reservoir test.
- `tests/host/desktop/test_deskui.py` (9 tests) and
  `tests/host/desktop/test_accessories.py` (11 tests, 1 skipped for the
  missing desktop build): OK after the libdeskui change.
- `python3 -B -m unittest discover -s tests/host -p test_kernel_latency.py`:
  15 tests OK.
- `decoder.c`, `player.c`, `runtime.c`, `view.c`, `main.c`, libdeskui's
  `client.c` and `widgets.c` compile with the SDK flags (`-Werror
  -Wframe-larger-than=4096`).
