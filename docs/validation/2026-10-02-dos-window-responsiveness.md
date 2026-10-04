# Desktop responsiveness with a DOS program in a window, 2026-10-02

## Symptom and measurement

With DoomVan (`PCDMCORE.EXE -warp 1 1 -nomusic`, DOS/4GW under HDPMI) running
in a DOS window, the whole desktop lagged. `scripts/qemu_test_dos_lag.py`
measures, on the 128 MiB QEMU full profile, the host time from a mouse-button
release on a desktop icon to the desktop's completed paint (`[DESKTOP] PAINT`)
and from a relative PS/2 move to the moved software cursor on screen
(screendump polling, a few tens of ms of resolution). Ten samples each, first
with an idle desktop, then with the game running.

| | canonical image | with the changes below |
|---|---|---|
| click, idle desktop (median) | 101 ms | 102 ms |
| click with the game (median / max) | 362 / 758 ms | 227 / 232 ms |
| pointer with the game (median / max) | 315 / 753 ms | 121 / 231 ms |

Runs: `build/tests/lag/ptr-full_ciukios-full_img` and
`build/tests/lag/ptr-tests_lag_fix_img`.

## Causes found in the source

1. **Input waits for the time slice.** `jlm_filter` only pended a keyboard or
   mouse byte for a VM that was not running (`vmm_pend`); that VM saw it at
   its next slice. `VMM_SLICE` is two IRQ0 ticks (about 110 ms), and a DOS
   game never idles, so the desktop and its cursor advanced in ~110 ms steps.
2. **An idle VM halted the CPU.** Jemm emulates a V86 `HLT` with a real `HLT`
   in ring 0 (`JEMM32.ASM`, `@@Is_Hlt`). The desktop idles with `HLT`
   (`shell_gui.inc`, `ui_loop`), so after each short desktop turn the CPU
   stayed halted until the next interrupt, up to 55 ms, while the game was
   ready.
3. **Urgent audio preempted the answering desktop.** Doom's Sound Blaster
   interrupt (every 23–46 ms) marks the game urgent; the switch gave it a full
   slice even while the desktop was in the middle of a response.

Microsoft documents the same remedies for Windows: a temporary priority boost
for the thread that receives input ([Priority Boosts](https://learn.microsoft.com/en-us/windows/win32/procthread/priority-boosts)),
with time slices of about 20 ms on Windows 95 ([Windows95 Scheduling Policies](https://beej.us/guide/win95sch.html)).

## Changes

- `patches/jemm-ciukios-hlt-yield.patch` (applied after the scheduler patch):
  Host_Scheduler_Profile function 11 registers a V86 HLT procedure, called
  before the physical HLT with EBP = the client frame; CF=1 resumes the frame
  it switched to.
- `vmm_halt`: at a guest HLT another ready VM runs at once, unless a virtual
  IRQ is already pending for the halting VM, a physical IRQ is in service,
  or the DOS/FAT gates of `vmm_switch` refuse.
- `vmm_mark_input` / `vmm_input_due`: a byte for a VM that does not run makes
  it input-urgent (switched at the next IRQ0 or safe return to V86). The
  system VM (desktop) then keeps the CPU until it idles (HLT), at most
  `VMM_BOOST` = 4 ticks.
- Urgent audio that interrupts the boosted desktop returns it after the audio
  VM has run 3 ms (TSC rate from DEV_BEGIN) or at the next tick.
- HDPMI: `DEV_PHYSICAL_IRQ` returns BX=2 for a byte routed to another VM; after
  its own EOI HDPMI calls the new `VM_OP_VMM_INPUT_YIELD` (4Ch). The switch is
  never made inside that bridge call: HDPMI acknowledges the IRQ only after
  it returns.

## Hazards found while testing

- Switching inside `DEV_PHYSICAL_IRQ`'s return, or while any physical IRQ is
  in service, stopped the mouse for good. Jemm completes a virtual EOI on the
  physical PIC whenever a physical ISR bit is set (see
  `2026-10-02-dos-audio-scheduling.md`, "Virtual PIC EOI follow-up"), so the
  next VM's virtual IRQ12 stayed in service. The new switch points therefore
  read both 8259 ISRs (OCW3 0Bh, then back to IRR) and defer when any bit is
  set.
- A first `vmm_halt` compared the old VM in EBX, which `vmm_switch` replaces:
  the switch happened, but the CPU then halted anyway.
- HDPMI's protected image must end below RVA 10000h. Only 32 bytes were free
  before `_DATA32C`; the input yield is therefore signalled in ZF (preserved
  by the MOV/OUT EOI sequence) instead of a flag. 4 bytes remain.

## Validation on the changed image

- `qemu_test_m4.py` full gate (DoomVan player input and PCM, two DOS windows,
  click focus, keyboard to the focused VM, independent close): PASS
  (`build/tests/lag/m4-fix10`).
- `qemu_test_vmm.py --parts clock`: VM1 180 and VM0 181 ticks in 10 s, console
  alive (`build/tests/lag/clock`).

## Remaining

Seven of ten clicks still take one extra ~130 ms step: the desktop itself
needs more CPU per response while a DOS window is shown, because each paint
also composites the guest framebuffer. Shorter VM time slices would need a
separate scheduler timer and virtualized DOS/BIOS ticks. Not tested on
physical hardware.

## Doom frame rate: direct VGA plane window (2026-10-03)

`scripts/qemu_profile_dos_window.py` samples CS:EIP from QEMU while DoomVan
runs in a window. 86% of the samples were in ring 0, almost all in HDPMI's
protected-mode video fault path (`cvdpmi_video_fault`, `_cvdpmi_selector_base`,
`_cvx_execute`, `_cvga_write_vram`); the game itself had about 6%. DOS Doom's
[`I_UpdateBox`](https://github.com/id-Software/DOOM) copies each of the four
unchained planes with one 16-bit store per two pixels, so a frame caused about
32,000 page faults, each emulating one instruction (`pm_faults` =
`pm_instructions` = `pm_elements` in the shared header).

`scripts/qemu_test_doom_timedemo.py` (`PCDMCORE.EXE -timedemo demo1 -nosound`,
128 MiB QEMU/KVM):

| image | fps | host time for the demo |
|---|---|---|
| canonical (scheduler changes above) | 18.2 | 105 s |
| lazy segment bases | 20.3 | — |
| direct plane window | 831–843 | 5.3 s |

Changes:

- `vga_x86` resolves a segment base on first use (`cvx_bus.segment_base`);
  the HDPMI adapter looked up six descriptors per fault and used two.
- HDPMI's copy of `virtual_vga.c` is built with `CVGA_DEVICE_ONLY` (ports and
  aperture only), which frees 3.2 KiB below RVA 10000h.
- `cvga_direct_plane` / `cvga_direct_read_plane` say when a store or a load at
  A0000-AFFFF is exactly one plane's byte (one-plane map mask, write mode 0,
  no set/reset, rotate, logical operation or bit mask, no chain-4 or
  odd/even, read mode 0). After a VGA register write the adapter then points
  HDPMI's 16 page-table entries at that plane of the shared model. The last
  of map mask and read map select written picks the phase: writable after the
  map mask, read-only after the read map select (Doom's `I_ReadScreen`; a
  first version mapped the write plane for reads and corrupted the status bar
  after the screen wipe). A latch copy (write mode 1) always traps.
- Page-table dirty bits become model dirty granules on every register write
  and at HDPMI's timer tick. `cv_guard_ptes` skips those entries while
  `cvvid_shared.pm_direct` is set; the shadow and Jemm's V86 table are
  unchanged, so V86 accesses still trap.

Limits: reads through a writable window do not load the latches, and return
the write plane; a program that reads another plane without writing the read
map select first, or loads latches there before switching to write mode 1,
would see wrong data. Chain-4 mode 13h programs still trap per access.

Tests: `test_virtual_vga.py` 173,117 assertions (random register states
against the full write and read paths), `test_vga_x86.py` 12,000 instructions
against Unicorn, full `qemu_test_m4.py` twice plus once after the UI changes.
One earlier M4 run lost keystrokes in the second DOS window ("echo M4 SECON");
three later runs passed, so that intermittent input defect remains open.

## Compositor damage rectangles

`ui_comp_damage` kept one bounding rectangle, and two-page presentation
composed the union with the previous frame's. An icon click plus a DOS-window
band therefore recomposed most of the screen. The compositor now keeps up to
four rectangles per frame (nearer than 16 pixels merge) and composes current
plus previous ones separately on the back page.

## Desktop changes requested by the owner

- The CiukiOS brand drops a menu (`m_start` in `desktop.c`, `EV_TOPBAR`).
- The speaker indicator opens a volume popup (master level, gear for the
  Sound applet).
- `app_window_cmd(window, 5)` hides a module's main window (flag 3: loaded,
  not drawn, no taskbar button). Control Panel opened with an applet argument
  shows that applet alone and ends with it.

## Owner report after the first build (2026-10-03) and fixes

Reported: Doom crashed when opening its menu, the system still lagged while
Doom ran, and after the volume popup the other windows misbehaved (Files no
longer opened).

Found and fixed:

- **Files closed at once.** The hidden Control Panel called `app_close()`
  once its applet closed; that service closes the *active* window, and the
  hidden module kept asking on every poll, closing the desktop and each newly
  opened window. It now closes its own window once
  (`app_window_cmd(WIN_CONTROL, 2)`).
- **TSC 0, device session refused.** The owner's log showed `[DPMIRUN] TSC
  KHz 00000000` and `DEVICE BEGIN ERROR 0031` (no audio, no device model).
  DPMIRUN measured the TSC over an `INT 15h/86h` wait, which the HLT yield
  now stretches (8.4 GHz measured on a 4.3 GHz host, or out of range = 0).
  CVSESSION calibrates the TSC once at load in ring 0 against PIT channel 2
  (mode 0, 20 ms, as Linux `pit_calibrate_tsc`) and serves it with
  `VM_OP_TSC_KHZ` (4Dh); DPMIRUN asks for it first.
- **VM switches inside a virtual-CLI single-step region.** The crash was a
  DOS/4GW exception 01h (single step). HDPMI single-steps a client's virtual
  CLI and defers IRQ0 meanwhile; the new input yield did not. It now skips
  that region, and every bridge call made there carries `VM_OP_NO_SWITCH`
  (100h) so CVSESSION does not switch VMs at its return. The original crash
  did not reproduce here (30-cycle mouse/menu stress passes on the fixed and
  on the owner's image), so this is a fix of a matching cause, not a proven
  reproduction.
- **Doom shown at 8.3 frames/s.** The DOS window queried damage only every
  2 BIOS ticks and the idle desktop got the CPU back only at slice ends.
  Now: the damage query runs on every desktop wake; an idle desktop is woken
  at each tick, 15 ms after it yielded at a return to V86, and when the game
  flips the VGA start address (HDPMI requests `VM_OP_VMM_PRESENT`, 4Eh);
  urgent audio returns any interrupted VM after 3 ms. Frames presented while
  Doom moves: 8.3 → 19.5 (wake-ups) → 32.8 (below).
- **Compositor occlusion.** A band inside a visible window's client area no
  longer draws the background, wallpaper, top bar and desktop icons
  (`ui_draw_scene`). The top bar's hit areas are still registered for every
  band: the first version dropped them while Doom repainted, so clicks on
  the brand and the speaker did nothing.
- **Keyboard for the top-bar menus.** Opening the CiukiOS menu or the volume
  popup gives the keyboard to the desktop; Esc went to the game before.

Measured on the fixed image (QEMU/KVM, 128-256 MiB): desktop click with Doom
running median 91 ms (idle 96 ms); Doom timedemo 855 fps; full M4 3 of 4
passes, the failure being the known silent Apogee start (audio-only 3 of 3);
VMM clock 180/181 ticks in 10 s; native resume passes. The pointer metric of
`qemu_test_dos_lag.py` is bounded by screendump time (about 60 ms per sample)
and is not a latency measurement below that.
