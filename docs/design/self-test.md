# Unattended self-test boot (fx-selftest)

- **Directive:** [`docs/directives/fx-selftest.md`](../directives/fx-selftest.md); owner decision
  2026-10-27 (AGENTS.md, Testing): the T23 trial starts with one unattended self-test boot whose
  report covers the hardware-only paths; manual trials then judge smoothness and quality only.
- **Base:** the F1-28 unattended sweep ([test architecture](test-architecture.md), "F1-28"),
  the H page ([hardware check](hardware-check.md)), the loader menu ([boot](../boot.md)) and the
  G14 Startup menu ([startup recovery](startup-recovery.md) 2.2).
- **Code:** `src/kernel/probes/selftest.c` (run, log file, stall watchdog, desktop row),
  `src/kernel/core/selftest_model.c` (sequencing, bounds and report format, shared with the host
  tests), `src/kernel/core/selftest_checks.c` (check table), `src/kernel/vm/dosvm_selftest.c`
  (DOS full screen), `src/probes/dos/fs_test.asm` (`FSTEST.COM`), `apps/desktop/desktop.c`
  (notice and DOS box launch), `scripts/test/selftest_report.py` (parser).

## 1. Entry

One self-test boot is chosen at the loader, never configured:

- **`T`** in the loader's three-second menu (keyboard or COM1, either case), beside N, S, L, H
  and P. The serial log records `L:SELFTEST=1`, then `L:SELECT_SOURCE=menu`.
- **Startup menu choice 9**, `Self-test (unattended, about 5 minutes)` (Ctrl or F8 during the
  three-second window, then 9 and Enter). The serial log records `L:STARTUP=9`.

Both set boot flag `CBI_F_SELF_TEST` (bit 23, the last free bit before the skipped-class bits
24–31). Like H, the choice replaces a `BOOT.CFG` test request; a positive safe-mode source stays
(its absent drivers then report SKIP). The console stays verbose (`cbi_quiet_console`), so the
boot log is on screen until the desktop starts.

There is deliberately **no `BOOT.CFG` token**. `BOOT.CFG` is persistent: a token would repeat the
run at every start until someone edits the file, and editing it is the settings change the run
must not make. The owner's decision asks for one self-test boot; a menu choice is exactly one
boot, needs no file, and leaves a normal boot unchanged (the image ships with the run available
and off). It also adds no entry to the loader's `BOOT.CFG` table, whose unknown tokens stop the
loader with `L:SELECT_ERROR`.

## 2. Sequence

- **Phase 1**, in `probes_main` after `drivers_init` and before `supervisor_bootstrap`: no
  process holds the display yet, so `display_check_begin` (the H page's mode transaction) admits a
  self-test boot. The 2D engine activation (`fbdev_accel_schedule`) is deferred exactly as on the H
  page and scheduled at the end of phase 1, so the mode sets never race the engine bring-up.
- **Phase 2**, after the desktop has been started, the ledger reconciled and `power_discover`
  logged the ACPI soft-off facts. It runs in the same kernel task while the desktop runs.
- The run id is eight hex digits from the TSC; the serial records use it (`rec_set_run`).
- Before every check `SELFTEST.LOG` is rewritten with a trailing `RUNNING <id>` line, after it
  without; a machine that hangs inside a check therefore leaves the name of that check as the
  file's last line.
- **Stall watchdog:** a kernel task looks once a second. When a check exceeds its bound by 30 s it
  appends `STALLED <id> ms=<n>`, emits `check=<id> status=stalled`, sets the desktop notice to
  `Self-test stalled in <id>` and, without a desktop, pins the full-screen summary.
- **End:** the summary lines, the Details section and `LOG writes=<n> error=<e>`; the desktop
  shows `Self-test complete: N passed, M failed, K skipped`. When the desktop never started
  (`desktop.start` failed) the console pins a full-screen summary with one line per check instead.

### Desktop protocol

During a T boot only, the device inventory carries row `0x7F000` (driver `selftest`, class
system, first row after the CPU) whose reason is `running`, `launch-dosbox` or the final notice;
its generation changes with every new reason. The desktop polls it twice a second (only when boot
flag bit 23 is set) and:

- on `launch-dosbox` starts `/Programs/dosbox` once per generation through the Programs-menu path;
- on `Self-test complete: ...` or `Self-test stalled in ...` shows one dialog titled
  `CiukiOS self-test` naming `C:\SYSTEM\LOGS\SELFTEST.LOG` (information icon when nothing failed,
  warning otherwise).

## 3. Checks

Each check has a bound and returns PASS, FAIL or SKIP with its figures. The model fails a check
that returns after its bound (`deadline_exceeded=1 limit_ms=<bound>`, measured figures kept).
Every check skips with a reason when its device or subsystem is absent, so the same image runs on
every supported machine. The table is `selftest_phase1` and `selftest_phase2` in
`selftest_checks.c`; the bounds add up to 584 s in the worst case (sec-k08 added `security.kdf`, 15 s), while the host T23 profile
completes in about 30 s of check time plus the real desktop, 3D and DOS start-up times.

| Check | Bound | What it observes | PASS / SKIP |
| --- | --- | --- | --- |
| `serial.uart` | 2 s | loader UART result (state, base, divisor), kernel probe result and FIFO, COM ports the COM driver admitted from the BIOS data area 0040:0000 (and PnP/PCI): `number:base:irq:origin:state` | PASS kernel UART qualified; SKIP no UART and no BDA port |
| `memory.start` | 2 s | free/total pages, zone free pages, heap pages/bytes/peak, ledger mismatches, refusals before the run | PASS ledger reconciles |
| `display.info` | 2 s | mode, geometry, VBE version, VRAM, mode count, EDID panel size (first detailed timing), bound driver and PCI id; Details: OEM strings and the loader's mode list | SKIP no framebuffer |
| `display.mode_640x480x8` | 12 s | BIOS VBE 4F02 set of the listed 640x480x8 linear mode (Warcraft II's resolution) through the H page transaction, 300 ms on the panel, restore of the desktop mode | PASS set, geometry and exact restore; SKIP not listed / is the desktop mode |
| `display.mode_640x480x32` | 12 s | the same for 640x480x32 | as above |
| `desktop.start` | 90 s | desktop process and its first full frame of the current mode generation | FAIL without a desktop (then the full-screen summary) |
| `power.apm` | 25 s | APM discovery result: state, connected, version, flags, capabilities, units, last BIOS function and error, the last firmware-VM fault | PASS connected; SKIP no APM BIOS or ACPI owner |
| `power.acpi_s5` | 10 s | RSDP, the parsed `\_S5` soft-off data (PM1a/b control ports, SLP_TYPa/b, SMI command, ACPI enable, SCI, FADT flags), qualification state, AC/battery; read only, never enters S5 | PASS PM1a control known; SKIP no ACPI or policy |
| `power.thinkpad_ec` | 3 s | SMBIOS verdict, vendor, product, BIOS, EC poll interval, master volume in the EC | SKIP not a ThinkPad |
| `display.3d_qualification` | 60 s | the Savage background qualification, forced on a T boot (as `3d=probe`) and awaited: result, reason, marker, error, elapsed time, packets/triangles/copies, optional gates, heap and VRAM; Details: every phase with its duration and error. Not scheduled within 15 s (the 2D activation schedules it): FAIL `not_scheduled` with the 2D engine state | PASS qualified; SKIP not 5333:8c2e |
| `display.gl_draw` | 10 s | one 64x64 triangle through the production render3d service with a hardware-required context on the display worker, read back: stage, hardware, expected/actual centre pixel, time | PASS hardware and pixel exact; SKIP no 3D backend or not qualified |
| `display.2d_engine` | 3 s | 2D engine kind, ready, native use, quarantine, native/software operation counters (Details per operation) | PASS native engine used; SKIP no driver or disabled |
| `display.fs_640x480x8` | 20 s | the DOS full-screen display transaction without a guest for VBE 101h with the current policy (native provider or video BIOS with its 4F04 state): enter, 500 ms, leave, desktop repaint, exact desktop layout | SKIP no desktop or 101h not supported |
| `display.fs_320x200x8` | 20 s | the same for BIOS mode 13h | SKIP no desktop |
| `display.vbe_state` | 1 s | VBE 4F04 state size (BX*64) against the 16 KiB state area, query/save/restore counts and results of the full-screen transactions above | PASS round trip; SKIP the firmware state was not used |
| `dos.window` | 90 s | the desktop starts `/Programs/dosbox` on request; the VM reaches the `C:\...>` prompt with input focus | SKIP no desktop |
| `dos.fullscreen_13h` | 25 s | types `\FSTEST.COM`; at its mode 13h stage enters full screen through `dosvm_fullscreen_enter` (Alt+Enter's path), holds 1 s, leaves; enter/leave/repaint times, provider, return to the window | SKIP no DOS window |
| `dos.fullscreen_101h` | 25 s | one key moves FSTEST to VBE 101h (banked 640x480x8); the same round trip | SKIP no DOS window, windowed by policy |
| `dos.return` | 30 s | Enter (at most three times) ends FSTEST, `EXIT` closes the VM; VM count back to the baseline and the box's temporary overlay file gone | FAIL forced stop, VM left or overlay left |
| `optical.drive` | 25 s | ATAPI drive identity (model, firmware, channel, unit, packet size, DMA), media state, readiness stage, sense | SKIP no optical drive |
| `optical.disc` | 40 s | with a disc: TOC, audio CD recognition, the ISO 9660 mount, the first file read (up to 256 KiB, FNV-1a, KiB/s) | SKIP no disc |
| `audio.device` | 2 s | backend, PCI id, ready, error, output rate, quarantine | SKIP no device |
| `audio.silent_stream` | 6 s | 0.5 s of digital silence through the mixer, drained: frames, time, underruns, IRQs (nothing audible) | SKIP no device |
| `audio.midi` | 4 s | MIDI synthesizer allocation with one All Notes Off (Control Change 123) message | SKIP no device or output rate |
| `network.nic` | 2 s | adapter, driver, MAC, state, link, frame counters | SKIP no adapter or disabled |
| `network.dhcp` | 30 s | waits for the DHCP lease when the link is up and DHCP is configured | SKIP no adapter, link down or static address |
| `usb.controllers` | 10 s | controllers by kind (UHCI/OHCI/EHCI), ready, quarantined, after their port scan (Details per controller) | SKIP no controller |
| `usb.devices` | 2 s | present, configured and quarantined devices (Details per device) | SKIP no device |
| `storage.ata` | 2 s | ATA/ATAPI units, DMA and UDMA mode, errors, retries (Details per unit) | SKIP no ATA unit |
| `security.kdf` | 15 s | sec-k08: five PBKDF2-HMAC-SHA-256 derivations of the ACCOUNT service's work factor (10,000 iterations, `ciuki/pbkdf2.h`, byte-identical to `acct_derive` by H13) on a fixed non-secret input while the desktop runs: `iterations runs min_ms median_ms max_ms target_ms=1000` | PASS median within the 1000 ms target of `user-security-kernel.md`; FAIL over it (the lead's Pentium III runtime gate) |
| `serial.records` | 1 s | UART records accepted/sent/queued/dropped during the run | FAIL evidence records lost; SKIP no UART |
| `memory.end` | 3 s | the memory figures again, heap and page-allocator refusals during the run, free-page delta | FAIL ledger mismatch or an allocation refused |

Checks that need a hardware path never invent register programming: they call the ordinary
drivers' own entry points and read their existing state (`hardware_check`, `display_*`,
`dosvm_*`, `render3d_*`, `audio_*`, `net_state_snapshot`, `usb_get`, `ata_inventory_get`,
`apm_platform_selftest`). The new read-only snapshots added for the run are
`biosvm_video_state_snapshot` (4F04 query/save/restore results), `display_selftest_info`
(desktop frames, pending repaint, last refusal and provider), `render3d_qualification_report`
(qualification outcome and phases, implemented by the driver-private Savage owner, so the generic
check table names no chip family, DM12), `apm_platform_selftest`, `power_s5_fallback_data`,
`kheap_ledger.refused` and `pmm_refusals`.

## 4. Report format

`C:\SYSTEM\LOGS\SELFTEST.LOG`, plain ASCII, one line per check and then the details:

```
CiukiOS self-test format=1 run=4c1d9a07 build=93d4ce76 os=1.0.1_alpha
PASS serial.uart ms=0 loader_state=1 base=0x3f8 divisor=1 kernel_present=1 probe_result=1 fifo=16 com_ports=1 bda_ports=1 com=1:3f8:4:bda:ready
...
SKIP optical.disc ms=0 media=no_medium reason=no_disc
...
Self-test complete: 30 passed, 0 failed, 1 skipped
SUMMARY passed=30 failed=0 skipped=1 checks=31 ms=187412

Details:
display.info line=0 modes=101:640x480x8,112:640x480x32,...
display.3d_qualification p0=prepare:1000:0 p1=marker_begin:2000:0 ...
LOG writes=64 error=0
```

Figures are `key=value` tokens. Keys match `[a-z][a-z0-9_]*`; values use `[A-Za-z0-9_:.,/+-]`
(any other byte becomes `_`, an empty value `-`, at most 96 bytes). The envelope keys `v`, `run`,
`seq`, `probe`, `event`, `check`, `status`, `ms` and `part` are reserved and a repeated key keeps
its first value. A result line holds at most 383 bytes of figures; when more would not fit, whole
trailing tokens give way and the line ends with `truncated=1`. A reason or a bound always fits.
Every SKIP has a reason (`reason=unspecified` when a check gave none).

The controller `probes/selftest.c` owns these records and is listed in the reviewed evidence-record
ownership list (`tests/host/record_scope_test.py`); the checks and the model never emit directly.
The same run on the serial port, with the existing record framing (`rec_emit`, at most 240 bytes,
`CIUKI_TEST v=1 run=<run> seq=<nnnnnn> probe=selftest event=<event> ...`), also copied to
`BOOTLOG.TXT` like every record:

| Event | Fields |
| --- | --- |
| `BEGIN` | `format=1 build=<hex8> os=<version>` |
| `DATA` | `check=<id> status=pass\|fail\|skip ms=<n>` and the figures; further records `check=<id> part=<n>` (n from 2) |
| `DATA` | `check=<id> part=detail` and a detail line; further records of the same line `part=detail_more` |
| `DATA` | `check=<id> status=stalled ms=<n>` (watchdog) |
| `END` | `status=PASS\|FAIL passed= failed= skipped= checks= ms=` |
| `DATA` | after END: `check=selftest.log part=detail writes=<n> error=<e> bytes=<n>` |

### Parser

`scripts/test/selftest_report.py` reads a disk copy of `SELFTEST.LOG`, a serial capture or a
`BOOTLOG.TXT` (UART noise before a record prefix is skipped; records follow the controller
grammar strictly), and prints the run, the totals, every failure with its figures, the skips with
their reasons and any stall:

```
python3 scripts/test/selftest_report.py SELFTEST.LOG serial.txt [--json] [--run <hex8>]
```

With two inputs it also compares the log lines with the serial records of the same run. Exit
status: 0 complete without failures, 1 complete with failures, 2 incomplete (`RUNNING`,
`STALLED`, `LOG TRUNCATED`, no summary), inconsistent (summary counts, END status, duplicate
check, SKIP without reason, log/serial differences) or unreadable.

## 5. Safety

- **Files.** The run writes only `SELFTEST.LOG` (whole-file rewrites through the VFS; a read-only
  `C:` leaves the serial copy as the report). Ordinary components behave as on any boot and are
  not counted as the run's writes: the boot log, the loader's boot-status sector, the Savage
  qualification's crash-guard marker (set and cleared by the driver, as on a `3d=probe` boot), and
  the DOS box's temporary copy-on-write file (`C:\SYSTEM\DOS\*.COW`, created and deleted by the
  VM itself; `dos.return` fails if it is left behind). No check opens a file writable; the host
  harness asserts it.
- **Settings.** Nothing is saved: display mode sets are the H page transaction and the DOS
  full-screen transaction, both restoring the exact desktop layout; the DOS box applies no
  property; network and audio settings are only read.
- **Power.** No check powers off, suspends or reboots: `power.acpi_s5` reads the parsed `\_S5`
  data only, `power.apm` reads the discovery result.
- **Sound.** The stream is digital silence and the MIDI message is All Notes Off.
- **Absent devices** skip with a reason; the host harness runs the whole table on a machine with
  no framebuffer, desktop or devices: 28 SKIP, `memory.*` PASS and `desktop.start` FAIL.

## 6. Host tests

`tests/host/test_selftest.py` (run by `scripts/test/host_kernel_tests.sh`):

- `selftest_model_test.c`: sequencing and progress order, the bound and overrun rule, sanitizing
  (reserved, repeated and malformed keys), truncation at every fill level with a late reason,
  record framing (240 bytes, continuations, detail continuations), the notice at the largest
  counts, the detail buffer keeping whole lines;
- `selftest_checks_test.c`: the production check table and DOS checks against fake subsystems on
  a fake clock, on a bare machine, on the T23 profile with a 3D qualification that is never
  scheduled, and on the T23 profile (S3 SuperSavage, 1024x768 panel, ATAPI CD
  without a disc on the secondary channel, three ICH3-M UHCI controllers, AC'97, APM 1.2, ACPI,
  ThinkPad EC, a DOS box running FSTEST); every check inside its bound, no writable open, the
  overlay rule;
- the parser on both producers' outputs, incomplete and malformed reports, two runs in one
  capture, the CLI exit codes;
- static checks: check ids, bounds and this table, no reserved key in a check's figures, the boot
  flag, loader and kernel wiring, and the assembled `FSTEST.COM` (no disk or file write calls).

The loader tests cover `T` and Startup menu choice 9 (`test_startup_menu.py`, assembled loader;
`test_loader_menu.py`, model).

## 7. What only hardware confirms

The real timings and results of: the VBE 4F02 sets and the 4F04 state round trip on the S3 BIOS
(fx-biosvm2 measured 3776 bytes), the native or firmware full-screen provider for 13h and 101h,
the Savage qualification and the hardware triangle, the desktop's first frame, the DOS box boot
and full-screen round trips with a real guest, ATAPI media state and the ISO read with a disc,
APM connection and the ThinkPad EC on the T23, the AC'97 stream and MIDI timing, the NIC lease,
the UHCI scans and the ATA DMA modes, and whether the whole run stays near five minutes.

## Sources

- VESA BIOS Extension Core Functions 3.0: 4F02h set mode, 4F04h save/restore state (BX in
  64-byte blocks) — [vbe3.pdf](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf);
  IBM PC BIOS INT 10h AH=00h mode 13h and INT 16h AH=00h as in SeaBIOS
  ([vgabios.c](https://github.com/coreboot/seabios/blob/master/vgasrc/vgabios.c)).
- APM 1.2 (connect, version, capabilities) —
  [APMV12.PDF](https://intel-vintage-developer.eu5.org/IAL/POWERMGM/APMV12.PDF).
- ACPI `\_S5` and PM1 control (read only here) —
  [ACPI 6.4, ACPI Software Programming Model](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/05_ACPI_Software_Programming_Model/ACPI_Software_Programming_Model.html);
  ThinkPad EC — [thinkpad-acpi](https://raw.githubusercontent.com/torvalds/linux/v6.12/Documentation/admin-guide/laptops/thinkpad-acpi.rst).
- MIDI 1.0 Control Change 123 All Notes Off — [MIDI 1.0 message summary](https://midi.org/summary-of-midi-1-0-messages).
- EDID detailed timing descriptor (horizontal/vertical active at bytes 2/4 and 5/7 of the
  descriptor at offset 54) — Linux [drm_edid.h](https://github.com/torvalds/linux/blob/v6.12/include/drm/drm_edid.h).
- USB host controller programming interfaces UHCI 00h, OHCI 10h, EHCI 20h of class 0C03h —
  Linux [pci_ids.h](https://github.com/torvalds/linux/blob/v6.12/include/linux/pci_ids.h).
- Windows 98 Startup menu model for choice 9 — [startup recovery](startup-recovery.md) 2.2.
