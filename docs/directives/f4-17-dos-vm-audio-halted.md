# f4-17 — DOS games on the T23: Sound Blaster detected and programmed, PCM never flows (halt=1), FM frames generated but silent

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f4-17` from `main`. Host-testable; the lead re-tests DOOM and Wolf3D on the T23.

## Evidence (T23, image 8c88b1ab, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/8c88b1ab-garbled-usb-regression/disk-log/BOOT.LOG`, DOOM 1.9 shareware running; native UI sounds audible in the same session)

```
[vm-audio] vm=1 reset=3 reply=aa
[vm-audio] vm=1 version=4.05
[vm-audio] vm=1 blaster=A220_I7_D1_H5_T6 source=guest_env
[vm-audio] vm=1 ms=200568 ready=1 err=0 buffers_ps=8 rate=48000 starved=0 attached=1
[vm-audio] vm=1 pcm=0 fm=0 fifo=0 halt=1 drop=0
[vm-audio] vm=1 dma=1 address=063b60 bytes=4096
[vm-audio] vm=1 play_dma=1 hz=11025 units=256
[vm-audio] vm=1 ms=204621 ready=1 err=0 buffers_ps=187 rate=48000 starved=0 attached=1
[vm-audio] vm=1 pcm=0 fm=16203 fifo=0 halt=1 drop=0
… every two seconds: buffers_ps=187, pcm=0, fm growing (115012, 213019, 308774, 403576 …), halt=1, starved=0
```

The game's DMX driver resets the DSP (reply AAh), reads version 4.05, sees the BLASTER string, programs an 8-bit DMA block (channel 1, 4096 bytes at 063b60h, 11025 Hz, 256-sample units) and starts playback (`play_dma=1`). Afterwards `pcm=0` never changes while `halt=1` stays set: the virtual SB16 DMA transfer never runs (no bytes consumed from the guest buffer, no DMA IRQ 7 to the guest, so DMX also never queues the next block). FM (OPL3) frame counts grow at 48 kHz but nothing is audible, although the VM source is attached and delivers 187 buffers per second to the native backend (`ready=1 err=0`), i.e. silence. The native backend plays the UI sounds correctly in the same session (f4-15).

## Task

1. `halt=1`: find what halts the virtual SB16 playback engine on the T23 and not on QEMU (`src/kernel/vm/dosvm_sb16.c`, `dosvm_dma.c`, `src/kernel/audio/vm_source.c`): the 8237 virtual DMA channel 1 state (mask bit, mode 0x58/0x59 auto-init, count/address programming by DMX via ports 02h/03h/0Ah/0Bh/0Ch/83h), the DSP command 14h/1Ch (8-bit single/auto-init) versus 0xC6 (16-bit), the time-constant/rate handling at 11025 Hz, the "halt" condition (DSP command D0h, or our own halt on an error such as a rate/format the mixer rejects, or the f4-13 'VM takeover' interaction), and whether the DMA completion IRQ 7 is delivered. Add records at every state transition (`[vm-audio] vm=… sb state=… reason=…`).
2. FM silent: with `fm=…` growing, trace the OPL3 render output into the VM source mix: gain, mute flag, the `halt` condition suppressing the mix, the f4-12 "UI sounds skipped while a VM plays" interplay (`CIUKI_AUDIO_DIAG_VM_ACTIVE`), and the backend's mixing of VM buffers (peak level record per second: `[vm-audio] vm=… level_db=…`).
3. Host replay: the DMX start sequence above against the production SB16/DMA/vm_source code with the native backend fake; it must produce non-silent PCM and FM output (fail before, pass after).

## Acceptance

`f3-workloads`, audio and kernel host suites green; i686 kernel compiles; the lead hears DOOM and Wolf3D on the T23.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f4-17-report.md`.
