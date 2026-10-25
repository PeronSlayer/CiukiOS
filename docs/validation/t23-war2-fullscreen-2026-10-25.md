# Warcraft II in native full screen on the T23 (image c42f556e)

- Written 2026-10-25 19:55 CET in `CiukiOS-wt-t23-war2fs` (branch
  `wt/t23-war2fs`, from `main` 50a843517). Host evidence only: no QEMU, no
  image build, no hardware run.
- Owner report (real hardware, image c42f556e, `main` a347ca76b): Warcraft II
  starts in the DOS window; switched to full screen it "starts crashing" and
  closes.
- Evidence: the trial's `BOOT.LOG` (private, `legacy/local/`); only record
  lines are quoted. Machine: SuperSavage IX, native display mode provider,
  desktop 1024x768x32 (`mode=280`). The demo runs under DOS/4GW
  Professional 1.97 and uses VBE 101h (640x480x256, windowed/banked model,
  4F05h through the 4F0Ah SetWindow code) and mode 13h for the logo and intro.

## 1. Sequence on the T23

| Tick (ms) | Record | Meaning |
| ---: | --- | --- |
| | `[launch-stage] name=dosbox pid=7 stage=vm-create window=Warcraft II` | DOS Box, VM 1 generation 4934 |
| 87703 | `[dosvm] fullscreen_request vm=1 generation=4934 mode=19`, `vm-park ... step_ms=0` | Alt+Enter on the Blizzard logo (13h) |
| 88455 | `fullscreen_first_frame ... source=aperture-write`, `first-guest-frame ... step_ms=140` | VGA full screen works |
| 94344 | `fullscreen_watchdog ... cause=mode-change` | the game sets 101h (4F02h) in full screen: ordered return |
| 94602 | `fullscreen_step=desktop-restored`, `fullscreen_return ... total_ms=208 result=0` | windowed 101h title screen |
| 98993 | `fullscreen_request ... mode=257`, `fullscreen_step=vm-park result=0 tick=98993 step_ms=4391` | Alt+Enter on the 101h screen |
| 99143 | `fullscreen_step=vga-save ... step_ms=150` | the real park |
| 99744 | `native_supersavage phase=enter result=0 stage=native-mode-readback` | 640x480x8 programmed, readback clean |
| | `[display] fullscreen=1 mode=257 desktop_mode=280 provider=native-driver`, `mtrr wc base=e8000000 size=1048576` | VBE full screen, linear transport (RAM buffer + copy flush, no LFB lease for a windowed mode) |
| 99853 | `fullscreen_step=watchdog-armed` | guest resumes; 5 s first-frame deadline |
| 103973 | `fullscreen_first_frame ... source=linear-write`, `first-guest-frame ... step_ms=4120` | first pixel store 4120 ms after entry |
| | `[display] fullscreen_log error=-11 step=first-guest-frame` | the display worker waited the 2000 ms BOOT.LOG sync timeout |
| 111259 | `fullscreen_watchdog ... cause=hotkey`, `watchdog-return ... step_ms=98` | Alt+Enter after about 7.3 s of full screen |
| 111497 | `fullscreen_return ... total_ms=137 result=0`, `fullscreen=0 ... cause=hotkey` | clean return, VM running in the window |
| | `[dosvm] lifecycle stage=stop vm=1 generation=4934 reason=0 vector=0 eip=00000000` | the session ends with `DOSVM_EXIT`, then `cleanup result=0 data_loss=0` |

There is no `[dosvm] fault`, `vm_fault`, `fullscreen_refused`, revocation or
DOS/4GW exception record in the session. Reason 0 is `DOSVM_EXIT`: the guest's
control-port exit at the end of the launch session, or a non-forced window
close. The last serial bank before the stop (2 s) shows
`perf_port vm=1 port=03c9 exits=131339 us=603167` and `port=03c8 exits=171`:
171 full palette loads at the window's cost, so the game ran normally in the
window for about 2 s after the return and then left to DOS. BOOT.LOG does not
name what ended it; the DOS Box writes the exit code to `C:\SYSTEM\DOSVM.LOG`
("VM exited"), which was not part of this evidence.

## 2. What the records show

1. **The 4.4 s vm-park is a measurement artifact.** `step_ms` is the time since
   the previous step record. vm-park is the entry's first step and measured
   from the previous return's `desktop-restored` (tick 94602), i.e. the time
   the user spent in the window. The park itself is in `vga-save`
   (`step_ms=150`).
2. **In the VBE full screen every VGA port exit took the slow path.** Serial
   banks of 2 s, port 3C9h (DAC data):

   | Phase | Exits per bank | Cost | Per exit | Palette loads / 2 s |
   | --- | ---: | ---: | ---: | ---: |
   | 101h in the window | 67,851 / 98,911 | 272 / 394 ms | 4.0 us | 88 / 129 |
   | 101h native full screen | 26,774 / 35,075 / 49,453 / 45,197 / 44,241 | 570-1049 ms | 21.2-22.0 us | 35-64 |
   | window after the return | 131,339 | 603 ms | 4.6 us | 171 |

   Disk banks: `vm_perf_remap ... n=6 ... lean=166640 dac=0` (window),
   `n=7 ... lean=0 dac=0` and `n=8 ... lean=0 dac=0` (full screen);
   `vm_perf_cpu n=7 monitor_us=3234617` of 5000 ms against 1707244 in the
   window. The demo reloads all 256 DAC entries every retrace; in the full
   screen the 3C9h exits alone took about half the CPU and the game ran at
   a third of its window speed. Cause: the fx-27 lean exits and DAC burst
   (`dosvm.c lean_before`, `dosvm_vga.c dosvm_vga_dac_burst`) were refused
   for any full screen. That is right for a VGA-mode full screen, whose VGA
   ports pass to the adapter without an exit, but in a VBE full screen the
   ports stay trapped to the VGA model exactly as in the window (the scanout
   is the kernel's copy of the VM's buffer and palette).
3. **The first guest frame froze the transport for 2 s.** The poll recorded
   `first-guest-frame` through `display_fullscreen_checkpoint`, which outside
   a return waits for the durable BOOT.LOG commit; storage was busy
   (accelerator marker write, log writes) and the wait ended at the
   `BOOTLOG_SYNC_TIMEOUT_MS` (2000 ms) with -EAGAIN (`fullscreen_log error=-11`).
   In a VBE full screen the display worker is the only scanout transport:
   for those 2 s neither the buffer nor the palette reached the panel, and a
   hotkey pressed meanwhile was swallowed (`fullscreen_busy`).
4. **The first-frame watchdog nearly fired.** The first pixel store came at
   4120 ms of the 5000 ms deadline. The title screen changed only its palette
   until then; a palette-only screen counted as "no frame" and would have
   been returned to the window with -ETIMEDOUT at 5 s.
5. **Return path.** Both returns (mode change, hotkey) completed with
   `result=0` in 208 and 137 ms; the VM kept running.

## 3. Provider and VBE path checks

- Mode 101h on the SuperSavage (`supersavage_mode.c`, f3-37 recipe, X.Org
  `SavageModeInit`): CR67=01h (8 bpp), CR50=40h, CR13=CR91=50h (640-byte pitch
  in 8-byte units), 25.175 MHz PLL, SR1B=10h. X.Org: "set 8-bit CLUT"
  (`new->SR1B |= 0x10`), so `linear_palette` loads 8-bit values
  (`(c<<2)|(c>>4)` from the guest's 6-bit DAC) - consistent. The trial's
  entry readback was clean (`phase=enter result=0`).
- Attribute palette: `packed_standard` started from the mode 12h preset and
  kept its planar palette (index 6 -> 14h, 8..15 -> 38h..3Fh) with AC10=41h.
  X.Org `vgaHWInit` and Linux `savagefb` (its `vgaHWInit`) program the
  identity palette for every depth above 4 bpp. Now identity as well. Whether
  the SuperSavage's packed pixel path reads the attribute palette is not
  documented in the sources at hand, so this is hardening, not a claimed
  cause.
- Banked versus linear: 4F02h 0101h (no D14) selects the windowed model;
  in native full screen the bank window stays in the VM's RAM aperture and
  the copy flush sends dirty buffer pages (`dosvm_vbe_flush`, bank D bits via
  `dosvm_vga_bank_collect`); only a D14 linear mode borrows the provider LFB.
  4F05h in full screen remaps the RAM window (old bank's dirty pages carried);
  4F01h and 4F0Ah answer as in the window; 4F02h in full screen requests the
  ordered return (as logged at 94344). Re-checked by the test below.

## 4. Research

- VESA *VBE/Core 2.0* ([VBE20.DOC](https://discmaster.textfiles.com/view/10567/HPAVC%20CD-ROM.iso/pc/UGPRG.ZIP/VGA-MISC/VBE20.DOC))
  and *VBE Core Functions 3.0* ([vbe3.pdf](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf)):
  4F05h window control and its direct WinFuncPtr call, 4F0Ah protected-mode
  table (SetWindow, SetDisplayStart, SetPrimaryPalette, ports/memory list);
  a windowed (banked) mode is a valid VBE 2.0 mode for any display; the
  palette of an 8-bit mode is the VGA DAC (6-bit unless 4F08h). Unchanged
  from `dos-dpmi-contract.md`, "VBE modes in the DOS window".
- *Microsoft Windows 98 Resource Kit*, chapter 25 (`build/rskt/wrkc25_3.htm`,
  "Running Games"): Windows 98 manages "video memory usage, and video port
  monitoring" itself, and a game that uses graphics modes is switched to full
  screen with ALT+ENTER. Windows 3.0 *Virtual Device Adaptation Guide* 18.1.2
  (quoted in `fx-27-windowed-dos-graphics.md`): a background VM's video I/O
  is trapped and emulated; OS/2 Museum, [Windows 3.x VDDVGA](https://www.os2museum.com/wp/windows-3-x-vddvga/):
  a full-screen DOS box owns the CRTC and video memory. In Windows 9x a
  full-screen VM therefore reaches the adapter directly. CiukiOS does so for
  VGA modes; a VBE mode is the VM's own RAM buffer and palette shown by the
  kernel's copy (the adapter has no guest-visible bank register), so its
  ports stay trapped. Decision: the trapped path in a VBE full screen must be
  at least as cheap as in the window (fx-27 lean exits and DAC burst); a
  direct DAC pass-through would need the 6-bit DAC on the native mode and a
  DAC capture on return, and is not done.
- The NT model (`VideoPortSetTrappedEmulatorPorts`, NTVDM) is a different
  architecture and was not used.
- X.Org [savage_driver.c](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c)
  (`SavageModeInit`: SR1B 8-bit CLUT), X.Org
  [vgaHW.c](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xorg-server/dist/hw/xfree86/vgahw/vgaHW.c)
  (`vgaHWInit`: Attribute[0..15] identity, [16]=41h above 4 bpp), Linux
  [savagefb_driver.c](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c)
  (the same identity palette).
- `bootlog_sync` (`src/kernel/core/bootlog.c`): bounded by
  `BOOTLOG_SYNC_TIMEOUT_MS`, -EAGAIN on timeout; `bootlog_try_sync` only
  requests the commit.

## 5. Changes

- `dosvm_internal.h` `dosvm_vga_ports_modeled()`: VGA ports reach the model
  in the window and in a VBE full screen. `dosvm.c lean_before` and
  `dosvm_vga.c dosvm_vga_dac_burst` use it instead of refusing every full
  screen; a VGA-mode full screen, a hold and a stop still take the ordinary
  path.
- `display.c`/`display.h`: `display_fullscreen_record()` records a step of a
  running full screen and requests its commit without waiting;
  `display_fullscreen_entry_request()` starts the step clock of an entry.
  `dosvm_fullscreen.c` records `first-guest-frame` (both transports) with the
  former and calls the latter before `vm-park`.
- `dosvm_fullscreen.c` poll: in an 8 bpp VBE full screen an effective
  palette change (DAC entries or DAC mask differing from what the adapter
  shows, loaded by `linear_palette`) counts as the first frame
  (`source=palette-write`); pixel stores stay `source=linear-write`. Other
  display-register writes, which `vga.display_changes` also counts, load
  nothing and earn no credit (review fix, section 9).
- `dosvm.c lean_fetch` and `dosvm_vga.c dac_byte` refuse borrowed PTEs (the
  direct LFB lease of a linear VBE full screen) and leave them to the
  ordinary monitor (review fix, section 9).
- `supersavage_mode.c packed_standard`: identity attribute palette.

## 6. Tests (fail on `main` 50a843517, pass after)

`tests/host/dosvm_vbe_window_test.inc` `test_vbe_native_war2_t23`
(`CIUKI_VM_WAR2_FULLSCREEN=1`, also in `CIUKI_VM_VBE_WINDOW` and the full
run) replays the logged sequence on the production VM: Alt+Enter in 13h,
4F02h 0101h in full screen (mode-change return), bank stores in the window,
Alt+Enter into the VBE full screen, a DOOM-style 768-OUT palette load
(fx-27 replay), first frame with a sink that does not drain, hotkey return,
and a palette-only screen past the 5 s deadline. On `main` (same test,
non-fatal checks): the first-frame poll waited 2000 ms; the palette load took
769 exits, `lean=0 dac_batched=0`; the palette-only screen was returned by
the watchdog; no entry request. After: 0 ms, 1 exit (`dac_batched=768`),
still full screen, hotkey return with `cause=hotkey`, game running windowed.
`tests/host/fullscreen_checkpoint_test.c` (real bootlog): the record waits
0 ms with a sink that does not drain (the durable checkpoint waits 2000 ms
and fails -EAGAIN); after a 4391 ms idle gap and an entry request vm-park
reports `step_ms=0` (control without the request fails).
`tests/host/supersavage_mode_test.c`: identity attribute palette for the four
packed modes (fails on `main`).

Reruns, one at a time under `systemd-run --user --scope -p MemoryMax=3G
-p MemorySwapMax=1G` with `nice -n 10`:

| Suite | Result |
| --- | --- |
| `dosvm_test` (ASan/UBSan, guest CPU), full run | PASS, 60,587,191 checks (60,633,212 after the review fixes) |
| `dosvm_test` with `CIUKI_VM_DISK_TRACE`, `VBE_WINDOW`, `WAR2_FULLSCREEN`, `101_NATIVE`, `101_VIDEO`, `101_REVOKE_RACE=all`, `FX27`, `FX22`, `FX16`, `WAIT_PRESENT` | PASS each (`FX16` prints `checks=1`, as on `main`) |
| `CIUKI_VM_MEMORY_CHARGE` through `ledger_test_normal --dosvm-profile` | PASS (128/256/512 MiB) |
| `tests/host/dos/run_dos_extenders.sh` | PASS (War2 with and without EMS reaches the 101h title screen) |
| `run_dos_memory.sh`, `run_unified_file.sh` | PASS |
| `test_display_driver_model.py`, `test_display_driver_names.py`, `test_savage_family.py`, `test_dos_fullscreen.py` | PASS |
| `test_display_modes.py` `test_kernel_switch`, `test_checkpoint_queue` | PASS |
| `supersavage_mode_test`, `supersavage_display_test` | PASS |
| `test_kernel_latency.py` (15 tests, includes `t23_fullscreen_return_test`) | PASS |
| `tests/host/irq_work_replay.py` | PASS; `DOS-audio-clock-per-VM max_us=250 budget_us=250` unchanged |
| changed kernel units with `build_kernel.py` CFLAGS (`display.c`, `dosvm.c`, `dosvm_fullscreen.c`, `dosvm_vga.c`, `dosvm_vga_vbe.c`, `supersavage_mode.c`, `supersavage_display.c`) | compile, no warnings |

## 7. What the lead must check

- QEMU has no SuperSavage: the boot smoke only (`vm-fullscreen` keeps
  `case=vbe_window` there). The changed paths need the T23.
- T23, Warcraft II demo, Alt+Enter on the 101h title screen and main menu:
  - `fullscreen_step=vm-park ... step_ms` small (no idle gap), then
    `fullscreen_first_frame ... source=linear-write` or `source=palette-write`
    and `fullscreen_step=first-guest-frame` with no
    `fullscreen_log error=-11` and no 2 s freeze;
  - serial `[dosvm] perf_port vm=1 port=03c9` in full screen at the window's
    per-exit cost (about 4-5 us, not 21 us), disk banks
    `vm_perf_remap ... lean=` nonzero while full screen, about 130-170
    palette loads per 2 s as in the window;
  - correct colours and a stable picture in menus and in a scenario, mouse
    and keys responsive, Alt+Enter back to the window and forth again;
  - if the game closes again, `C:\SYSTEM\DOSVM.LOG` ("VM exited" and the exit
    code) together with BOOT.LOG; note that Esc on the demo's main menu
    leaves the demo (war2vbe trace).
- DOOM/Quake mode 13h full screen once (VGA path unchanged; its
  `first-guest-frame` record is now non-blocking too).

## 8. Risks and open items

- Lean exits and DAC bursts in a VBE full screen skip the per-exit
  housekeeping exactly as in the window; a hold (the park of an entry or a
  return) or a stopping VM still forces the ordinary path, and the host
  timer exit, which always takes the ordinary path, samples progress at
  least once per tick.
- `first-guest-frame` is no longer durable before the next step: a hang right
  after the first frame leaves `watchdog-armed` as the last durable record.
  Entry and return hardware steps keep their durable or bounded checkpoints.
- An effective palette change counts as drawing. Once a first frame (pixel
  or palette) was seen there is no frame-based watchdog: a game that keeps
  animating its palette, also one that is otherwise hung and still cycles
  the palette from an interrupt handler, keeps full screen indefinitely.
  The 15 s progress watchdog does not catch it either: it only checks that
  the sampled CS:IP changes, not useful progress. Alt+Enter (and a VM stop)
  remain the way out. Register writes without a visible change no longer
  hold full screen (section 9).
- Identity attribute palette changes the packed-mode register values (still
  verified by `verify_mode`); the T23 run must confirm the colours.
- Unchanged, outside this fix: a VBE screen with no change at all for 5 s is
  still returned (f3-26 first-write watchdog); the demo's palette loop never
  joined the DAC burst on the T23 even in the window (`dac=0` in every bank,
  768 lean exits per load: DOS window performance track); the entry park
  (750-860 ms, about 30 ms per durable step) and the 13h entry's audio
  underruns (`underruns=1980`, `parked_ms=642`) are the existing transaction.
- What ended the session after the return is not in BOOT.LOG.

## 9. Review fixes (Codex xhigh review of 1bb063bd0)

Written 2026-10-25 20:06 CET.

- **P1, fast paths and the direct LFB lease.** Enabling lean exits and the
  DAC burst in a VBE full screen included linear (D14) modes, whose PTEs
  name the provider's framebuffer during a direct LFB lease
  (`dosvm_vbe_direct_enter`, `PTE_BORROWED`). `lean_fetch` (instruction
  bytes) and `dac_byte` (burst loads and fetches) dereferenced any user PTE
  through `P2V`, which maps RAM only: on the T23 the BAR is E8000000h. Fix:
  both refuse a borrowed PTE; the ordinary monitor then reads the lease
  through the provider alias (`monitor_memory`, `transfer`). The lean exit
  and burst stay available for code and data in RAM.
  Test `test_vbe_native_lease_fast_paths` (`CIUKI_VM_WAR2_FULLSCREEN`,
  `CIUKI_VM_VBE_WINDOW`, full run): mode 4101h, native full screen with a
  lease at physical E8000000h whose provider alias is a separate buffer
  (seam `fs_direct_mmio`), a PM32 Quake-style palette loop reading its table
  from the lease, then the same loop executed from the lease. On 1bb063bd0:
  ASan SEGV in `dac_byte` (table in the lease) and, with only that fixed, in
  `lean_fetch` (code in the lease). After: correct DAC, 769 exits each (lean
  exits for code in RAM, the ordinary path for code in the lease).
- **P2, register writes and the first-frame credit.** `vga.display_changes`
  also counts CRTC, sequencer and graphics register writes (for example the
  cursor location, CRTC 0Eh), which the VBE transport does not scan out, so
  a loop rewriting such a register earned the first-frame credit and kept
  full screen while nothing visible changed; the poll also reloaded the
  whole palette on each such write. Fix: `linear_palette` keeps the DAC and
  mask last loaded and loads, and earns credit, only when they differ; only
  8 bpp modes earn palette credit (direct colour ignores the DAC). Test
  (step 8 of `test_vbe_native_war2_t23`): CRTC 0Eh toggled every 100 ms with
  a moving sampled CS:IP for up to 16 s. On 1bb063bd0: still full screen
  after 16000 ms, 123,040 adapter port writes. After: returned at 5000 ms
  with `cause=first-frame-timeout`, -ETIMEDOUT, no adapter writes.
- Reruns after the fixes (2026-10-25, same commands and scopes): every
  suite of section 6's table PASS again, `dosvm_test` full run 60,633,212
  checks, `DOS-audio-clock-per-VM max_us=250 budget_us=250` unchanged.

