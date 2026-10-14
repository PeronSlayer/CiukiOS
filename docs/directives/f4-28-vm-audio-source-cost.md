# f4-28 — VM audio source path misses the 1 ms service bound on QEMU (`vm-audio-qemu-t23`: deadlines ≠ 0) after f4-24 fixed the native path

- **Model/effort:** `gpt-6.1-sol`, `high`. **Worktree:** `../CiukiOS-wt-f4-28` from `main`. Host-testable; the lead runs `f4-audio` on QEMU.

## Evidence (QEMU `qemu-t23`, image 5601f16a of main 578022d8, chain Y4 on 2026-10-14 evening)

`audio-native-qemu-t23` passes since f4-24 (mixer without 64-bit division helpers), but `vm-audio-qemu-t23` fails `predicate failed: deadlines eq 0`: with a DOS VM's Sound Blaster source mixed in, the per-period service still exceeds 1000 µs under TCG (two-period refills of 256 frames at 48 kHz). f4-24 left "wide VM sums" and the Q16 SB16 gains of f4-18 (`src/kernel/audio/vm_source.c`, `src/kernel/vm/dosvm_sb16.c` mixer application, OPL3 FM mixing in `src/kernel/audio/opl3.c`) untouched.

## Task

1. Measure the per-period cost of the VM source path on the host harness (`tests/host/dosvm_vmaudio_test.inc`, `tests/host/audio`) with the f4-24 deadline replay extended to a VM source at 11025 Hz 8-bit mono and at 22050 Hz 16-bit stereo plus FM: find the steps above budget (resampling with 64-bit arithmetic, Q16 gain multiply per sample per channel, FM synthesis per sample, saturation).
2. Make it O(frames) with 32-bit integer arithmetic (precomputed step tables for the resampler, gains folded into one 32-bit multiply per sample, FM rendered per block not per sample where the OPL3 model allows), no i686 wide division helpers (extend f4-24's code-generation guard to these files).
3. Keep the f4-17/f4-18 semantics and the existing goldens (VMPLAY pins, FM reference); update only if a numeric change is justified and documented.

## Acceptance

Audio and DOS VM host suites green with the new cost assertions; the lead runs `f4-audio` on QEMU: `vm-audio-qemu-t23` `deadlines=0`. Files: `src/kernel/audio/vm_source.c`, `opl3.c`, `dosvm_sb16.c` mixer application, tests. Not `audio_dev.c`'s service loop (f4-24 done) nor `mixer.c` unless needed for the guard.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f4-28-report.md`.
