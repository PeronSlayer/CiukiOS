# f3-41 — windowed games drop frames: 28–31 ms kernel-mode stretches in the window presenter and compositor starve the VM; present cadence

- **Phase:** F3. **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f3-41` from main (after `2fef3ffc`).
- **Authority:** `src/kernel/vm/dosvm_vga.c` (`dosvm_vga_present`), `src/kernel/vm/vga_presenter.c`, `src/kernel/core/display*.c`/compositor path (`present_xrgb_row`), `src/kernel/core/task.c`/`cpustat.c` (preemption points, `stretch_max_ms` accounting), `src/kernel/vm/dosvm.c` (`dosvm_task_quantum`), host tests `tests/host/dosvm_speed_test.inc`, `tests/host/dosvm_present_bench.c`, `tests/host/kernel_latency_test.c`, the F0-20 cpustat replay.

## Evidence (T23, image 43d72a5c, `legacy/local/physical/43d72a5c/BOOT1.LOG`/`BOOT2.LOG`, read-only)

- Owner: windowed DOOM/Wolf3D/Quake still have annoying frame drops; full-screen switching works.
- `vm_perf_share`: `vm_permille=613…778 guest_permille=121…486`: the VM task holds 61–78 % of the CPU but the guest executes only 12–49 %; `[cpu] top=dosvm 37–74 %, desktop 11–30 %, dosbox 5–22 %, display-watchdog 7–8 %, storage-writer 4–7 %` (menu L: verbose logging costs CPU).
- `[kernel] stretch_max_ms=29/31 task=dosbox eip=memcpy+0x20 / cvga_render_row8+0x212 / dosvm_vga_present+0xe80`, `stretch_max_ms=28 task=desktop eip=present_xrgb_row+0x30` (recurring while a game runs), `stretch_max_ms=127 task=dosbox eip=memset+0x20` at `vm-create` (one-time), `stretch_max_ms=134 task=display-watchdog eip=fs_read/native_read` at each full-screen switch (durable checkpoints, one-time). A 28–31 ms non-preempted kernel stretch delays the guest timer and SB16 IRQs by that much (f3-40 measures), and costs the guest its share.
- Windowed VGA cadence: the presenter runs at the modelled 70 Hz for 449-line modes (`dosvm_vga.c:58-71`) although DOOM renders at most 35 fps; f3-38 added exact planar comparison, so unchanged frames are cheap but still scanned.

## Required

1. Bound kernel-mode stretches of the window presenter and the compositor present paths to 4 ms: cooperative preemption points inside the row loops (`dosvm_vga_present`, `cvga_render_row8`, `present_xrgb_row`, the memcpy/memset of VM creation and frame copies) that let a higher-priority or earlier-deadline task (the focused dosvm) run; measure with `stretch_max_ms` in the F0-20 cpustat replay: no stretch above 4 ms for these tasks while a VM runs.
2. Present cadence: present a windowed VM frame only when the model changed since the last present and at most at the mode's refresh divided by two for 13h/Mode X/Y (35 Hz; 30 Hz for 60 Hz modes) unless the guest changes the start address every refresh (page flipping at 70 Hz stays 70); the cap is per VM and recorded in the `vm-window` perf records (`present_rate=`).
3. Scheduling: when the focused VM has a pending virtual IRQ (timer or SB16), the scheduler prefers it at the next preemption point over presenter/compositor work of the same priority class; document the rule in the scheduler design doc; no starvation of the desktop (input latency stays within the f1-input gate: 24/24).
4. Measure before/after on the host DOOM replay (`GUEST_PERF`): guest instruction share, presenter µs/frame, max stretch; report the numbers with the exact commands (as f3-38 did).
5. Host coverage: `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`, the F0-20 replay, `test_system_stalls.py`.
6. Report `build/codex/f3-41-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. No change to the native full-screen provider or the SB16 model (f3-40 owns it). Keep the f3-38 presenter semantics (exact comparison, planar 2x path).
