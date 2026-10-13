# f4-13 — DOS games silent on the T23 while native audio plays

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f4-13` from `main`. Host-testable; the lead tests DOOM and Wolfenstein 3D on the T23.

## Evidence (T23, image e86305de, 2026-10-13; transcript `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/e86305de-usb-storage-fault-optical-missing/serial-clean.log`)

Native audio is up (all `[audio] stage=… result=ok`, start-up sound heard). DOOM 1.9 shareware and Wolfenstein 3D produce no sound. The transcript has no DOS VM audio record at all (no SB16/OPL/DMA records in verbose boot) although the VM audio output path of f4-05 exists (`src/kernel/vm/dosvm_sb16.c`, `dosvm_opl.c`, `dosvm_dma.c`, `src/kernel/audio/vm_source.c`).

## Task

1. Find why the VM audio path is silent on hardware: the game's configuration on the image (`/DOOM/CIUKI.CFG`, DOOM's `snd_sfxdevice`/`snd_musicdevice`, SETUP defaults for Wolf3D), whether the virtual Sound Blaster is detected by the games (BLASTER environment variable in the VM, port 220h/IRQ 7 or 5/DMA 1/5, DSP version), whether DMA from the VM reaches `vm_source.c` and the native backend (the f4-11 backend boundary), sample-rate conversion, and the OPL3 FM path for music. On QEMU the same image must be checked by the lead with the `f4-audio` VM cases.
2. Verbose records for the VM audio path: DSP reset/version query, BLASTER string seen, DMA programming (channel, rate, length), buffers delivered per second to the backend, underruns; a `[vm-audio]` summary every two seconds while a VM plays.
3. Fix at the cause; host replays with the DOOM and Wolf3D audio start sequences (DMX and the Wolf3D sound manager) that fail before and pass after.

## Acceptance

`f3-workloads`, audio and kernel host suites green; the lead hears DOOM and Wolf3D on the T23 and QEMU.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f4-13-report.md`.
