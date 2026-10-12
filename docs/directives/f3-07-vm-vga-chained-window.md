# Directive f3-07: direct linear window for chained VGA mode 13h in DOS VMs (V86 and protected mode)

- **Step:** F3 (performance row after f3-06). **Contracts:** `dos-vm-contract.md`
  (virtual VGA is authoritative; the VM never touches physical video), f3-02
  (`docs/design/f3-acceptance.md` §f3-02: trapped A0000h–BFFFFh, presenter and
  digests), f3-06 (§f3-06: direct plane window for planar modes, burst
  emulation, protected-mode faults routed to the model), `test-architecture.md`.
- **Implementer:** Codex, `gpt-6.1-sol`, effort `xhigh`.
- **Worktree:** `wt/f3-07` from `wt/integ2`. Files: `src/kernel/vm/dosvm_vga.c`,
  `src/kernel/vm/virtual_vga*.c/.h` and `vga_presenter.c`, `dosvm_internal.h`,
  `dpmi_dispatch.c` only where the protected-mode fault path hands the window,
  `tests/host/dosvm_video_test.inc`, `tests/host/dosvm_workloads_test.inc`,
  `docs/design/f3-acceptance.md` (`## f3-07`).

## Problem

In chained mode 13h (320x200x256, chain-4) every guest store to A0000h–AFFFFh
traps: in V86 the burst emulation of f3-06 amortises runs of stores, but
DOS/4GW programs (DOOM) write the frame from 32-bit protected mode, where
each store is one `#PF` into `dosvm_vga_fault_pm`. DOOM's timedemo on QEMU
shows `pm_faults≈104,000` per loop; on a Pentium III each fault costs a few
microseconds, so a 64,000-byte frame at 35 Hz cannot be sustained on the T23.
Planar modes already have a direct plane window (one plane mapped writable,
dirty bits harvested per frame). Chained mode has none.

## Scope

1. **Linear window.** While the VGA state is "chained mode 13h with no latch
   or mask semantics in effect" (sequencer memory mode chain-4 set, odd/even
   off, graphics controller write mode 0, bit mask FFh, map mask 0Fh or the
   chain-4 equivalent, no read-mode-1 colour compare pending), map the VM's
   mode-13h backing (64 KiB; define how the model stores the chained frame:
   either a linear 64 KiB buffer kept coherent with the four planes, or the
   planes themselves with address bits 0–1 selecting the plane in a page-level
   mapping that the model's presenter can read) user-writable over
   A0000h–AFFFFh in the VM's page tables, for V86 and protected-mode accesses
   alike (the page tables are shared; DPMI flat selectors reach the same
   linear range). Reads through the window must return what trapped reads
   would return (no latch load is needed in chain-4 write mode 0; say so and
   test it).
2. **Closing the window.** Any port write that changes the conditions above
   (sequencer 04h, graphics controller 03h/05h/08h, sequencer 02h to a value
   other than the chain-4 default, CRTC mode changes, INT 10h mode set, VBE
   calls), a latch-copy opportunity, a `dosvm_vga_present` that needs the
   frame, a VM stop or a page-table rebuild closes the window: harvest the
   dirty pages into the model, unmap, and trap again. The presenter harvests
   dirty bits before each frame exactly as the plane window does.
3. **Page flips and palette.** CRTC start-address writes (0Ch/0Dh) and DAC
   writes keep trapping and keep their counters (`flips`, palette digests);
   DOOM's `pm_faults` must drop by at least two orders of magnitude while
   `flips` per timedemo loop stays ≈1714 and the frame digests of
   `vm-video` stage 3/4 (mode 19 patterns) stay unchanged.
4. **Counters and records.** `dosvm_video_info` gains `linear_maps` and
   `linear_harvests`; the `case=video` records of the doom probes report them.
5. **Tests.** Host harness: a chained-mode write replay through `dosvm_trap`
   (V86) and through the protected-mode fault path with a flat selector, both
   with and without the window, planes exact against a reference, window
   closed by each of the port writes in item 2, dirty harvest before present,
   `vm-video` goldens unchanged (`dosvm host tests` PASS). Guest runner
   (`tests/host/dosvm_guest.c`): DOOM `-nosound -timedemo demo1` on the
   libx86emu runner still completes with 1710 gametics (it writes VRAM
   directly; it validates the model, not the window).

## Acceptance by the lead

`make kernel`, `make test-host` and the guest runner PASS in the worktree;
QEMU `doom-qemu-t23` five loops with `gametics=1710`, `flips≈1714`,
`pm_faults` ≤ 1,000 per loop, `vm-video` and `vm-window` digests unchanged,
`wolf3d-qemu-t23` unchanged; the T23 run of DOOM is the hardware evidence.
