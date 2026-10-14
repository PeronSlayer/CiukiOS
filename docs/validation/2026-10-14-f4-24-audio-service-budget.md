# F4-24 audio refill cost and cadence

Research before implementation, 2026-10-14. Host evidence only; QEMU and
hardware acceptance remain with the lead. The probe's 1000 us bound is unchanged.

## Sources and repository comparison

- [QEMU audio options](https://www.qemu.org/docs/master/system/qemu-manpage.html)
  specify a default `timer-period` of 10000 us. At 48 kHz each callback can
  fetch 480 frames. The production worker restores the eight-period lead
  in one bounded burst, with 256 frames per period. A 400-callback replay
  therefore needs 50 single-period and 350 two-period refills: 750 periods,
  exactly 192000 frames. This explains the supplied 350 misses if a single
  period fits and a double refill crosses the bound. It is a host replay
  inference, not a new QEMU timing trace.
- [QEMU v9.2.0 AC97 source](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/audio/ac97.c)
  (`transfer_audio`, `nabm_writeb`) preserves CIV at exhausted LVI and
  advances PIV on restart/publication. Retain F4-15's coherent cursor proof,
  CIV/PIV exclusion, retained unpublished PCM and stop/reset proof; do not
  trade those checks for refill speed.
- [LLVM compiler runtime division](https://llvm.googlesource.com/llvm-project/+/a259686db1c5f3f9904f266cbb084a8baeaf86d7/compiler-rt/lib/udivmoddi4.c)
  illustrates the extra work of wide software division on 32-bit targets.
  Ciuki uses its own `src/kernel/lib/divdi3.c`. An i686 `-O2` compile of the
  original mixer contains six `__divdi3` call sites, all inside frame loops:
  two interpolation and two gain calls per native source, plus two master
  calls. A two-source period executes 2560 calls even at equal rates and
  unity gains. The helper has a 32-bit magnitude shortcut, so these native
  values do **not** ordinarily take its 64-iteration slow path; call and
  sign/width overhead still dominates the simple arithmetic.

The F4-18 native mixer diff only adds the hardware-master flag to the
existing master expression; its Q16 virtual SB gains belong to VM sources
and are not exercised by the native probe. F4-15's catch-up loop turns two
periods into one timed call. Neither feature should be rolled back.

Decision: preserve exact integer truncation and phase/drain semantics. Use
unsigned 32-bit magnitudes for interpolation (`65535 * 47999 < 2^32`), skip
interpolation/lookahead at phase zero, use signed 32-bit native percentage
gain, and perform software master gain only inside a range proven safe for
32-bit multiplication. Unity master uses direct saturation. Keep wide
accumulation for virtual sources. Clear only the requested sum frames.
Do not use approximate Q16 gains for native PCM.

The CPU benchmark uses `CLOCK_THREAD_CPUTIME_ID` on an `-O2` harness, plus
an i686 code-generation regression guard. Thread CPU time excludes host
preemption at nice 19; it cannot establish absolute TCG latency. The
transport replay asserts the callback/period distribution, exact consumption,
no starvation/halt, no successful-refill logging and a 10 ms modeled gap.

## Gap investigation

The kernel is cooperative in ring 0 (`core/task.c`). ELF RAM reads/mappings
and console output can run without scheduling. `console.c::scroll` repaints
every framebuffer row; its row locks release IF but do not yield. Probe
source/payload records are emitted while the idle audio tail still runs.
UART buffering does not suppress framebuffer output. This is a plausible
source of the supplied 150 ms gap, not proven without a runtime trace.
Process teardown frees mapped extents; it does not scan every user page.
Storage's `fs_service` already cooperates when the tick changes; an individual
storage call may still take time before reaching such a checkpoint.

## Implemented change and host evidence

`mixer.c` uses the exact arithmetic described above. The i686 `-O2` object
has no wide division/modulo helper references. `audio_dev.c` directly fills
inactive, unattached silence and coalesces idle-stop notices for the worker
to print only while stopped. Playback-complete formatting also leaves the
timed service. Fatal reporting retains the F4-15 pre-rollback snapshot.
The backend cursor/publication/reset code and probe bound are unchanged.

The original optimized host replay averaged 3847 ns for one period and
7097 ns for two. The final replay averaged 2567 ns and 4519 ns, with a
5900 ns maximum. These include clock sampling overhead. Final stage means:
two-source unity mix 2426 ns, 50/25% mix 2323 ns, 44.1-to-48 kHz mix
3486 ns, silence fill 453 ns, coherent running cursor fetch 471 ns,
running descriptor publication 484 ns, exhausted-ring stop/rebase and
eight-period silence restart 1684 ns. Trivial stages are close to clock
sampling overhead; their differences are not significant.

Validation, sequential under `nice -n 19`:

- `ASAN_OPTIONS=detect_leaks=0 bash tests/host/audio/run.sh`: PASS,
  11040300 checks, ASan/UBSan, arithmetic oracles, existing boot-tone/master
  and F4-15 tests, optimized cost replay, i686 helper/frame guard.
  LeakSanitizer's ptrace operation is unavailable in this sandbox; explicit
  allocation/page cleanup assertions pass.
- `python3 tests/host/test_audio.py`: PASS, 9 tests; QEMU operations mocked.
- Targeted i686 `-O2` compilation of `audio_dev.c` with kernel stack
  protector and 4 KiB frame limit: PASS.
- `git diff --check`: PASS.

## Qualification still required

The 10 ms gap assertion covers modeled DMA, not simultaneous production
ELF/storage/console execution. The supplied 150 ms gap remains unlocalized
and unfixed outside audio. Releasing IF between console rows does not
schedule another ring-0 task: a timer can mark the worker ready while the
printing task continues repainting. A focused console/process/storage fix
needs an expanded directive beyond the listed audio/AC97/test files.
No kernel preemption or nested IRQ mixing has been introduced. Deferring
successful-service logs removes an audio-owned source of long sink delays,
but does not prove the supplied native-probe gap is solved.

The lead must run `audio-native-qemu-t23`, verify zero deadlines, classify
the gap during active playback versus idle tail/control output, and qualify
T23 boot tone/UI sounds. No new image/hash or QEMU/hardware evidence exists
from this worktree.
