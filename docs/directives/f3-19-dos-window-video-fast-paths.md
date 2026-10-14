# f3-19 — DOS window video: VGA port fast path, Mode X/Y presenter and protected-mode bursts (research block of 2026-10-14)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f3-19` from `main`. Host-testable (`tests/host/dosvm_video_test.inc`, `dosvm_present_bench.c`, `dosvm_speed_test.inc`, the guest CPU harness); the lead runs `f3-vm` video cases and the T23.

## Input

Read `docs/design/research/dos-vm-video-2026-10-14.md` in full. Its findings replace the premise of f3-16: DOS DOOM 1.9 does not copy four plane passes per frame; it draws columns straight into VGA memory and writes the map mask (port 3C5h) once per column and once per plane per floor span. On the T23 (image 8c88b1ab, sample n=56): 18 frames, 23019 port-write traps (about 1279 per frame), 13715 harvests, monitor 301.8 ms of the second (12.4 µs per trap), guest 7 %, present 8.8 ms per frame, 47 % of the second not covered by any counter; page faults flat during gameplay (fb3147dc). Each plane-mask change today re-harvests and re-maps the 16 window pages through 32+ page-table calls with TLB flushes (`dosvm_vga.c:108,133,182-216`, `vmm.c:249-272`). Windows 98 never ran DOOM fast in a window either (windowed DOS graphics were emulated at a few frames per second; play was full screen on the real VGA): the full-screen-on-hardware half is a separate directive pending the owner's approval of an amendment to `device-firmware-ownership.md`. VME is not the fix (about 980 real-mode exits per second, under 1.2 % CPU, absent in QEMU TCG).

## Task (windowed, exact)

1. **Port fast path for the VGA sequencer/graphics registers that change the direct window** (map mask 3C5h index 2, read map select, write mode): on a change, swap the 16 page-table entries of the A000 window directly to the plane's shadow pages with one TLB operation, no harvest and no remap through the generic mapper; estimate in the research 1-1.5 µs per write (target: under 2 µs on the host model clocked like a P3, under 7 % CPU at 35 fps with DOOM's pattern). The plane shadows stay exact: a store to the mapped plane goes to that plane's bytes; dirty tracking per page for the presenter.
2. **Mode X/Y presenter**: convert the four planes to the window surface in one pass with precomputed de-interleaving (estimate 3-4 ms per frame on the P3 for 320x200), only for dirty rows, page flips through CRTC 0C/0D select the source without a harvest.
3. **Protected-mode bursts**: extend the existing emulation bursts to protected-mode guests (DOS/4GW DOOM, Wolf3D 16-bit real mode) with fast paths for planar string copies and fills (REP MOVS/STOS into the window with a fixed map mask), so each trapped instruction costs one exit for the whole run.
4. **Counters**: cover the 47 % uncovered second: per-cause exit costs for port writes by register, remap µs, presenter µs, burst instruction counts; `vm_beat_vga` keeps `faults`, adds `port_remaps`, `remap_us`, `present_us`, `burst_insn`.
5. Keep the VGA model exact except the known latch inexactness recorded by f3-16 (native reads through the direct window do not load latches): record it in the design document as the accepted limitation for DOOM/Wolf3D, and keep write modes needing latches trapped.
6. Host replay of DOOM's real pattern (per-column map-mask writes, 1279 per frame) and Wolf3D's; before/after table in `docs/validation/`.

## Acceptance

Kernel host suites green; the lead runs the `f3-vm` video cases and `vm-window` on QEMU, then the T23: DOOM ≥ 35 fps in the window (its tic rate) with `vm_perf_share` showing the monitor under 20 % of the second, Wolf3D ≥ 70 fps. Files: `src/kernel/vm/dosvm_vga.c`, `virtual_vga.c`, `vga_presenter.c`, `dosvm_perf.inc`, `vga_x86.c` (bursts), `src/kernel/core/vmm.c` only for a page-table batch primitive. Not `dpmi_dispatch.c`/`v86.c`/scheduling.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Record decisions in `docs/design/` (amend the VM video design). Report `build/codex/f3-19-report.md`.
