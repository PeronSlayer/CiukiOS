# Directive f4-01: native audio API, kernel mixer and Intel ICH AC'97 backend (QEMU AC97 twin)

- **Step:** F4 (first implementation directive). **Contracts:**
  `f4-audio-media-contract.md` (native audio API, volume and persistence,
  kernel mixer, "Intel ICH AC'97 first", acceptance rows T0 native
  audio/mixer, T1, T3 `f4-audio-native`; lead decisions 1, 2, 4, 8),
  `execution-abi.md` and `posix-subset.md` (to be amended by this directive:
  the device-control syscall and its records), `device-firmware-ownership.md`
  (sole PCI/DMA/IRQ ownership, registry claims, serialized stop),
  `boot-memory.md` (pinned descriptor/staging buffers from the DMA zone,
  charged commitments), `test-architecture.md`.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f4-audio-native`, from `main` at or after `67f6397`.
  Files: new `src/kernel/audio/mixer.c`, `stream.c`, `audio_dev.c` and
  `src/kernel/include/ciuki/audio.h`; new `src/kernel/drivers/ac97.c` and
  `src/kernel/include/ciuki/ac97.h`; `src/kernel/include/ciuki/abi.h`
  (syscall `CIUKI_SYS_DEVCTL = 69`, the fixed-width `ciuki_audio_*` and
  `ciuki_mixer_*` records with version and size fields); `src/kernel/proc/
  devnodes.c`, `syscalls_file.c` and `files.h` (`/dev/audio`, `/dev/mixer`
  as `PX_DEV` nodes); `sdk/libciuki` (`ciuki_devctl()` wrapper and
  `ciuki/audio.h`; rebuild the SDK with the documented recipe);
  `src/kernel/probes/f4_probes_audio.c` registered in `scripts/build_kernel.py`;
  `tests/suites/f4-audio.json`; `scripts/test/run.py` (audio twin and WAV
  checker, see point 6); `docs/design/execution-abi.md` and
  `posix-subset.md` (one amendment section each); new
  `docs/design/f4-acceptance.md` (your own section `## f4-01`; other
  directives add theirs; the lead merges); host tests.

## Scope

1. **Device nodes and syscall.** `/dev/audio` (O_WRONLY only; `EACCES`
   otherwise; `ENODEV` without a qualified backend) and `/dev/mixer`
   (read-only master volume/mute for processes; the writable control
   handle is delegated by the bootstrap supervisor as the contract says,
   `MIXER_SET_MASTER` refused on public opens). Syscall 69
   `devctl(fd, op, buf, size)` with the contract's record rules (version,
   byte size, reserved zeros, copied buffers; `ENOTTY`, `EOPNOTSUPP`,
   `EINVAL`); operations `AUDIO_GET_CAPS`, `AUDIO_SET_FORMAT`,
   `AUDIO_GET_STATUS`, `AUDIO_SET_GAIN`, `AUDIO_DRAIN`, `AUDIO_RESET`,
   `MIXER_GET_MASTER`, `MIXER_SET_MASTER`. Numbers and layouts are frozen
   in the two ABI amendments before the code that uses them.
2. **Streams.** Copied ring of 2,048 source frames per stream, at most
   eight native streams, admission charged; formats U8/S16LE, mono/stereo,
   8,000–48,000 Hz, default S16LE stereo 48 kHz; whole frames only;
   blocking writes through the existing cancellable wait, `O_NONBLOCK`
   short counts/`EAGAIN`, `EINTR` rules, `EIO` on backend failure,
   `lseek` → `ESPIPE`; auto-start on the first nonempty write; drain and
   reset semantics; final close drops uncommitted audio without blocking.
3. **Kernel mixer.** Integer-only: conversion, rational resampling with
   persistent phase, per-stream gain 0–100, wide accumulation, final
   saturation to S16 stereo; 256-frame quantum; eight published periods;
   linear software master gain before saturation; starvation counted per
   source; no x87/MMX/SSE; bounded preallocated buffers; no codec
   transactions or file I/O under IRQ-off sections.
4. **Intel ICH AC'97 backend.** PCI class `0401`, vendor `8086`, the ICH
   family devices (at least `2415` ICH, `2425` ICH0, `2445` ICH2, `24C5`
   ICH4, `2485` ICH3-M); claim NAMBAR/NABMBAR and the interrupt through the
   registry before any write; BDL of 32 entries in pinned DMA-zone memory,
   eight published, CIV/LVI/PIV retirement by stable cursor observations;
   CAS-guarded codec transactions with 100 ms deadlines, activation/stop
   500 ms; codec ID and ready checks; 48 kHz output, VRA only on verified
   readback; W1C status handling, counters for DMA halt, FIFO error and
   source starvation; pause/stop/quiesce proof and quarantine on unproved
   stop; disabled in safe mode. Sources S1–S3 of the contract.
5. **Volume persistence.** Master volume/mute in `C:\SYSTEM\AUDIO.CFG`
   (versioned record, defaults 50/50 unmuted, read at backend activation,
   written through the VFS durability path only from the control handle,
   never from IRQ or refill paths).
6. **Evidence on QEMU.** Runner: a case-level `device_exceptions:
   {"audio": "ac97"}` attaches `-audiodev wav,id=cap,path=audio.wav` and
   `-device AC97,audiodev=cap` (the profile's silent audiodev stays the
   default); the runner keeps the WAV inside the run directory, checks it
   with standard-library Python (duration, per-channel RMS, dominant
   frequency by DFT, gaps longer than one quantum) against the case's
   declared expectations, records the numbers and deletes the WAV when the
   case passes. Probe `f4:audio-native`: two processes play known
   patterns (left 440 Hz for 1 s, right 880 Hz for 1 s, then a 2 s stereo
   mix of both streams at different gains), check counters, drain, and
   the probe records image/build identity, device identity, accepted
   rate, period/lead and the counters of the contract's evidence
   paragraph. Case `audio-native-qemu-t23` (AC97 twin, 512 MiB) and
   `audio-native-qemu-desktop-2002`; on a profile without audio the probe
   records `not_run reason=device_absent`.
7. **Host tests:** the T0 "native audio/mixer" row of the contract
   (formats, clipping after summation, phase across split batches,
   short/EAGAIN/EINTR writes, dup/inheritance/close, quotas, mock codec
   CAS timeout/rollback, mock BDL wrap/prefetch/publication, failed stop
   with canaries).

## Out of scope

ESS Maestro-2E, the DOS VM audio path and OPL worker (after f3-05), the
desktop volume UI, recording, `poll`/`select`.

## Acceptance by the lead

Host tests; kernel, SDK and image build; QEMU `f4-audio` PASS on the two
twins with the WAV numbers recorded; `f2-all` unaffected.
