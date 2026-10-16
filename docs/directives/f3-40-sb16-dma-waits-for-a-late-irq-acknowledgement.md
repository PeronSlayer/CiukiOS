# f3-40 — DOOM: sound slows and distorts and the game stalls when sounds play; the virtual SB16 reports `irq_late` and waits on the IRQ

- **Phase:** F3. **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f3-40` from main (after `2fef3ffc`).
- **Authority:** `src/kernel/vm/dosvm_sb16.c` (DSP/DMA/IRQ state machine), `src/kernel/vm/dosvm.c` and `dosvm_dpmi*.c` (virtual IRQ injection points), `src/kernel/audio/vm_source.c` (host-side consumption), host tests `tests/host/dosvm_vmaudio_test.inc`, `tests/host/audio/`.

## Evidence (T23, image 43d72a5c, owner report 2026-10-16 night; `legacy/local/physical/43d72a5c/BOOT2.LOG` black-box records of the games boot, read-only)

- Owner: in DOOM, shooting or opening a door makes the game lag heavily and the sound distort and slow down, windowed and full screen alike; Quake/Wolf3D not reported.
- `[vm-audio] vm=1 sb state=running reason=irq_late cmd=b6 dma=5 mask=0 mode=59 hz=11025 fmt=07 io=000` × 73 and `sb state=irq_wait reason=complete …` × 118 (protected records only; the verbose log was rotated). DOOM's DMX uses SB16 16-bit auto-init output (B6h) on DMA 5 at 11025 Hz.
- `[vm-audio] vm=1 gen=14568 irq_wait ms=501 irq=7 isr=0000 irr=0083 imr=ff78`: IRQ 7 pending and unmasked for 501 ms with nothing in service (also IRQ 0 pending): the guest was not receiving interrupts at all for half a second (VM parked for a full-screen switch, or starved).
- Host scheduling in the same boot: `[kernel] stretch_max_ms=29…31 task=dosbox eip=memcpy/cvga_render_row8/dosvm_vga_present`, `stretch_max_ms=28 task=desktop eip=present_xrgb_row`, `stretch_max_ms=134 task=display-watchdog eip=fs_read/native_read` (durable full-screen checkpoints). A guest ISR can be delayed by tens of ms by other tasks' kernel-mode stretches (f3-41 bounds them); this directive makes the SB16 model tolerate it like the hardware does.
- A real SB16 never pauses auto-init DMA because the previous IRQ has not been acknowledged: the DMA wraps, the IRQ stays latched until the DSP status read (22Eh / 22Fh), a second block end while the IRQ is still asserted produces no new edge (one IRQ lost, one block replayed, no tempo change).

## Required

1. Research first (cite in `docs/validation/f3-32-native-fullscreen.md` or a new `docs/validation/f3-40-sb16-irq.md`): Creative SB16 Hardware Programming Guide (auto-init DMA, IRQ acknowledgement, DSP status ports), DOSBox `sblaster.cpp` behaviour as a reference model (no code copied). Record exactly what `irq_late`, `irq_wait reason=complete` and `waiting` do today in `dosvm_sb16.c` (line numbers).
2. Model: the virtual DMA cadence is free-running on the physical timer (f3-39) and is never paused or delayed by an unacknowledged IRQ; a block boundary while the IRQ line is still asserted increments `irq_merged` and asserts nothing new; acknowledgement by the DSP status read clears the line; the `irq_wait` state only reports, never gates DMA progress or host-side sample consumption. The host-side ring must keep playing the guest's buffer (replaying the last block if the guest is late, as hardware would), never silence or stretch.
3. IRQ injection latency: measure from block end to guest ISR entry (`irq_latency_us` max/avg per 5 s) and deliver the virtual IRQ at the earliest boundary (V86 or DPMI PM) when the guest IF allows; if the dosvm task is not running, note the reason in a counter (`descheduled`, `parked`, `if_clear`). No change to the physical PIC.
4. Host coverage: a replay of DOOM's pattern (B6h, 16-bit stereo 11025 Hz, half-buffer IRQs) with a sweep of guest ISR latency 0–60 ms and of host preemption gaps 0–40 ms: block cadence constant (no tempo change), `irq_merged` counted, zero host underruns beyond one block; existing SB16/vm-audio/deadline suites, `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `bash tests/host/audio/run.sh`, `python3 scripts/build_kernel.py`.
5. Diagnostics: one protected record every 5 s while a VM plays: `[vm-audio] vm=… cadence blocks=… late_acks=… merged=… ack_max_ms=… irq_latency_max_us=… underruns=…`.
6. Report `build/codex/f3-40-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. No register programming for the physical AC97/SB paths beyond what exists.
