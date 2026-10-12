# f3-10 — DOOM on the T23 reaches its menu and freezes there

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f3-10` from `main`. Host-testable; the lead reproduces on the T23 with an `L` boot capture.

## Evidence (2026-10-13, image 36010476 / hardware fe695509)

After f3-09 DOOM 1.9 shareware launched from Programs > Games now passes `I_StartupTimer`, sets mode 13h and shows its title/menu, then stays frozen in the menu: no reaction to keys, no animation. The boot was quiet (no `L` records). On QEMU the same image runs the timedemo. The 12 October serial capture of the previous image showed `[records] dropped premature evidence record` each time a DOS window started: identify and silence that stray record emission in normal boot as part of this work.

## Task

1. Rank the causes for a freeze at the menu on a real Pentium III after a working startup: keyboard IRQ1 delivery to the DPMI client in protected mode (the native 8042 driver on the T23 versus the firmware input path), the PIT/IRQ0 timer rate after `I_StartupTimer` (DOOM's 140 Hz), the VGA refresh of the chained mode 13h linear window (f3-07) on real hardware (page-fault harvesting cadence, dirty bits), the Sound Blaster DMA/IRQ emulation (DOOM polls the DMA position; a stalled position can spin in `I_UpdateSound`), the VBE/LFB state after the desktop's own mode switch (the owner changed the display mode shortly before), and v86/DPMI virtual-IF stalls with IF never re-enabled. For each: the exact check and, where it is a defect, the fix with a host test (dosvm guest replay with DOOM, keyboard and timer injection, DMA position progress).
2. Diagnostics: extend the `L` verbose records of f3-09 with a periodic (every 2 s, bounded) DOS VM heartbeat: client mode, CS:EIP, IF, pending IRQs, PIT ticks delivered, keys delivered to the client, SB DMA position and IRQ count, VGA harvests. So one `L` capture of a frozen menu tells where it spins.
3. Keep `f3-workloads` host replays green.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, one host suite at a time. Report `build/codex/f3-10-report.md`: ranked causes, fixes with file:line, tests, the record names the lead must read.
