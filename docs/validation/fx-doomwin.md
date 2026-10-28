# DOOM in the DOS window: plane selects, debug traps, BIOS worker (fx-doomwin)

Research checked 2026-10-27. Directive: `docs/directives/fx-doomwin.md`.
Host evidence only; the physical figures are from the owner's T23 trial of
image `6c3b7800` (Pentium III-M, S3 SuperSavage, DOOM in the window,
`vm=1 gen=11971`, 5 s banks). Replays: `tests/host/dos/window_parity.py`
(T23 time models of `tests/host/dosvm_guest.c`) and `tests/host/dosvm_test.c`.

## 1. Where the window's monitor time went (trial 6c3b7800)

| Bank (5 s) | monitor ms | VGA port exits (us each) | `db` exits (us each) | OPL/other `io` (us each) | PM CLI/STI (us each) | tracker hits |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| n=3 | 1515 | 103,224 (5.43) | 10,756 (37.3) | 24,052 (8.79) | 7,755 (6.72) | 9,910 |
| n=4 | 1819 | 137,500 (5.90) | 12,132 (37.5) | 20,861 (8.92) | 8,243 (7.22) | 11,241 |
| n=5 | 974 | 131,956 (5.66) | 716 (17.2) | 11,969 (4.34) | 4,704 (4.26) | 0 |
| n=6 | 1324 | 181,168 (5.56) | 920 (17.7) | 19,293 (4.30) | 6,738 (4.26) | 0 |

The `vm_perf_port` rank 1 is 03C5h in n=3..n=6 (102,888-171,101 exits).
`vm_perf_remap`: 2.06 us per plane exchange (`remap_us`/`remaps`), which is
inside each VGA exit's figure.

## 2. The debug traps (directive item 2)

**Sources of #DB in DOOM's window run**, from the code and the host
replay's step-owner attribution (`dosvm_step_owner`, the replay's "debug trap
owners" line; 30 s DOOM window, fixed model, before the change):

| Source | Mechanism | Count (30 s replay) | T23 cost |
| --- | --- | ---: | --- |
| `pm_checkpoint` | f3-34 return tracker: execute breakpoints (DR0-3) at predicted return addresses, re-armed after each hit for up to 32 hops and 512 scanned instructions per seed (`dpmi_return.inc`) | 12,282 | 37.3-37.5 us (n=3/n=4) |
| `pm_flags` | f3-06/fx-16 flags image: the TF the monitor planted in a PUSHF image at its CLI traps once after the POPF that loads it (`flags_restored`; R. Chen, "Getting MS-DOS games to run on Windows 95: The interrupt flag", The Old New Thing, 2016-04-11) | 5,545 | 17.2-17.7 us (n=5/n=6) |
| `pm_if` | f3-15 IF recovery steps (bounded to 2 ms) | 89 | - |
| `rm_ss` | V86 MOV SS/STI shadow steps (`v86_debug_step`) | 24 | - |
| `pm_shadow` | t23-dosperf's PM STI-shadow step (newly attributed; was "client") | 0-1 | - |

No client breakpoint or client TF occurs (`exc=0`, no `client` owner).

**Why the tracker ran in the window.** The tracker is seeded only while a
CLI proof may need a boundary (`dpmi_seed_wanted`: before the first proven
site, and for `DPMI_SEED_IDLE_MS` = 1 s after a CLI "needed a proof"). Before
this change `cli_miss_tick` was set for **every** CLI that had no cached
site, including the ones the proof then cached, and the control-flow proof
was never cached (fx-doswin3 recorded this as `cli_control`). Each of
DOOM's 10-13 learned sites therefore restarted one second of seeds; a seed
is attempted at every PM trap, and the window has 25-40 thousand port
exits a second plus DOOM's busy waits (title, menus, wipes), so each second
armed thousands of 32-hop chains. During that second every port and
CLI/STI exit also loses the lean path (`dpmi_lean_eligible` refuses while
seeds are wanted): bank n=4 against n=6 shows `io` at 8.92 against 4.30 us
and PM CLI/STI at 7.22 against 4.26 us. In full screen the same game has
few PM traps, hence "full screen 0 armed" in trial 5bb73dba.

**Why each checkpoint cost about 38 us.** A tracker hit is a #DB on the
ordinary exit path (the POPF #DB alone costs 17-18 us on the T23, banks
n=5/n=6) plus `return_checkpoint`: a forward scan of up to 512 guest
instructions, each fetched with `dosvm_read` (a guest page walk per read)
and classified by `return_length`/`return_step_safe`, then
`dpmi_reload_debug`, whose MOV to DR0-DR7 are serializing.

**Change.** Only a CLI left without a cached site restarts the demand: a
miss, a provisional site a decode disproves, or a proof whose site cannot
be stored. The control-flow proof is cached like the forward proof (both
decode from an observed boundary and clear their remembered instructions
at every transfer, so PUSHF [POP r] physically precedes the CLI in both;
the kept bytes and the live image check guard every later arm). The seed
machinery is unchanged and still serves the first site and every miss.
The remaining `pm_flags` #DB (one per armed POPF) is inherent to the
Windows 95 technique; a lean path for it would save about 12 us x 200/s
(2-3 ms/s) and is left for later.

## 3. The plane selects (directive item 1)

**Research.**
- Windows 3.x/9x VDD: a background or windowed VM's video I/O is trapped,
  the output values are kept and the inputs emulated; video pages are hooked
  and mapped from the virtual controller state (Windows 3.0 DDK, *Virtual
  Device Adaptation Guide*, 18.1.3 "VDD I/O Trapping and Hooked Pages",
  19.2 `Hook_V86_Page`; [PCjs](https://www.pcjs.org/documents/books/mspl13/win/w3ddkvxd/)).
  Windows 9x keeps the architecture (VDD.VXD with a vendor mini-VDD):
  M. Necasek, "Windows 3.x VDDVGA", OS/2 Museum, 2023-01-23
  ([article](https://www.os2museum.com/wp/windows-3-x-vddvga/)), "The VDD
  is meant to virtualize VGA registers and not let DOS applications touch
  the real hardware"; VDDVGA.386's source shipped with the Windows 3.1 DDK.
- DOSBox Staging [`vga_seq.cpp`](https://raw.githubusercontent.com/dosbox-staging/dosbox-staging/main/src/hardware/video/vga_seq.cpp)
  `write_p3c5` case 2: stores the map mask and its expanded form; only SR04
  changes handlers.
- QEMU [`vga.c`](https://raw.githubusercontent.com/qemu/qemu/master/hw/display/vga.c)
  `vga_ioport_write` (`VGA_SEQ_D`) stores the register and refreshes only
  the chain-4 alias; `vga_mem_writeb` applies the mask per store. Under KVM
  the legacy window is coalesced MMIO; port writes flush it and still exit.
- Intel SDM Vol. 1 "I/O Permission Bit Map": without VT-x every guest OUT
  to a port closed in the TSS bitmap is a #GP; Vol. 2B RDTSC: not
  serializing; Vol. 3A "Serializing Instructions": CPUID, IRET. Agner Fog's
  instruction tables: CPUID about 130 clocks, RDTSC about 42 on the P6 line.

The selects were already fx-27 lean exits; the design document
(`docs/design/research/dos-vm-video-2026-10-14.md` section 6) records why
the model and the 16-entry exchange stay and the exit around them goes.

**Change.**
- `dosvm_trap_fast` (`dosvm.c`), called first by `trap_dispatch` for a VM's
  #GP(0): OUT DX,AL/AX to 3C4h/3C5h (SR02) and 3CEh/3CFh (GR04/GR05) in an
  unchained planar window, under the fx-27 lean conditions, with exactly
  `dosvm_vga_port`'s state (`dosvm_vga_select_fast`); any obligation after
  the instruction hands it to `dosvm_trap`'s ordinary tail.
- Exit boundary stamps without CPUID (`dosvm_perf_stamp`), for every exit.
- `proc_signal_return_to_user` returns at once for kernel tasks: a DPMI
  client's every exit no longer scans the 256-slot native thread table
  after the monitor's closing stamp (time the trial counted as guest time).

## 4. Replay results

Replay `tests/host/dos/window_parity.py --games doom,wolf3d --seconds 30
--present direct --ack 2` (the fx-doswin3 round-3 options), "before" with a
runner built from the unchanged sources (`29587e240`), "after" with this
change. Host replay, not hardware evidence. The "after" figures were
measured twice, the second time with a runner built from the final sources
of this change: the fixed-model, what-if, menu and equivalence runs gave
identical output both times.

**Acceptance metric.** The directive's wall-to-game ratio is read here as
the wall time a rendered game frame costs in the window against full
screen (the full/window flips factor below), with the monitor's share of
each wall second (monitor ms/s) beside it. Before this change: DOOM
1.689 (48.5 ms per window frame against 28.7 ms in full screen, monitor
251.9 ms/s), Wolfenstein 3-D 1.254 (17.9 against 14.3 ms, monitor
353.4 ms/s), fixed model, steady interval.

**Fixed model** (`--model fixed`: the trial's per-cause prices,
deterministic; every price lowered by the 230 ns of CPUIDs the plain
boundary stamps no longer pay inside the measured interval,
`GUEST_T23_STAMP_NS`; the trap-entry path priced as a lean exit unless
`GUEST_T23_FAST_NS` is given):

| Title, mode | flips/s | monitor ms/s | VGA ms/s (ports + aperture faults) | VGA ms per port exit, us |
| --- | --- | --- | --- | --- |
| DOOM window | 20.60 -> 21.00 | 251.9 -> 244.7 | 155.6 -> 150.2 | 5.33 -> 5.10 |
| DOOM full screen | 34.80 -> 34.80 | 181.2 -> 179.3 | 0 | - |
| Wolfenstein 3-D window | 55.81 -> 56.56 | 353.4 -> 352.6 | 300.8 -> 300.8 | (faults dominate) |
| Wolfenstein 3-D full screen | 70.00 -> 70.00 | 117.9 -> 116.7 | 0 | - |

Full/window factor (steady interval 25-30 s, 14-30 s for Wolfenstein):
DOOM 1.689 -> 1.657, Wolfenstein 3-D 1.254 -> 1.238. Wolfenstein's window
cost is its aperture faults (81.6 us each in this model), which this
change does not touch.

The steady interval of the attract demo has no CLI learning, so the
tracker is idle there before the change too. The phases that do learn are
the title (3-4 s) and the start of demo1 (11-14 s); in the latter:

| DOOM, 11-14 s, fixed model | frames/s | `#DB`/s | `#DB` ms/s | monitor ms/s | seeds/s |
| --- | ---: | ---: | ---: | ---: | ---: |
| window, before | 18.0 | 3,327 | 117.8 | 335.3 | 4,703 |
| window, after | 21.3 | 257 | 9.0 | 245.8 | 0 |
| full screen, before | 23.0 | 7,295 | 114.5 | 286.8 | 10,461 |
| full screen, after | 27.7 | 294 | 4.5 | 182.3 | 0 |

Whole 30 s runs (fixed model):

| Run | `#DB` | owners before -> after | `#DB` ms | monitor ms | frames |
| --- | --- | --- | --- | --- | --- |
| DOOM window | 17,919 -> 5,689 | checkpoint 12,282 -> 0, flags 5,545 -> 5,656 | 634 -> 200 | 9,020 -> 8,380 | 538 -> 558 |
| DOOM full screen | 31,226 -> 6,266 | checkpoint 24,901 -> 0, flags 6,084 -> 6,142 | 490 -> 97 | 8,146 -> 7,675 | 585 -> 610 |
| Wolfenstein 3-D window | 3,011 -> 2,913 | V86 shadow steps only | 107 -> 103 | 12,452 -> 12,357 | 892 -> 890 |

Audio cadence (DOOM window, 25-30 s): Sound Blaster DMA 22,050 units/s
before and after; IRQ0 lost 4 -> 3, reinjected 185 -> 203 (whole run).

**Trap-entry select cost.** `tests/host/dosvm_exit_bench.c`, a settled PM
client's OUT DX,AL to 3C5h, TSC cycles per exit on this workstation:

| | hot | cold (4 MiB walked before each exit) | hot, final sources | cold, final sources |
| --- | ---: | ---: | ---: | ---: |
| ordinary path | 1,102 | 1,294 | 1,149 | 1,473 |
| fx-27 lean exit (`dosvm_trap`) | 455 | 626 | 464 | 709 |
| trap entry (`dosvm_trap_fast`) | 370 | 525 | 388 | 610 |

The second pair ran with the workstation busier (other host suites);
the trap-entry/lean ratio is 0.81-0.84 hot and 0.84-0.86 cold in both.
The bench measures the C path only (no clock reads, no ring transition).
Applied to the trial's lean VGA price (6.70 us, less sixteen INVLPGs at
0.86 remaps per exit, less the 230 ns of stamps = 5.09 us, of which
0.34 us are clock reads): 4.76 us x 525/626 + three plain stamps = 4.10 us,
a saving of about 0.99 us per select inside the measured interval. With
`GUEST_T23_FAST_NS=990`:

| Fixed model, `GUEST_T23_FAST_NS=990` | flips/s | monitor ms/s | VGA ms/s | VGA ms per port exit, us |
| --- | --- | --- | --- | --- |
| DOOM window (before -> after) | 20.60 -> 22.20 | 251.9 -> 224.7 | 155.6 -> 129.6 | 5.33 -> 4.17 |
| Wolfenstein 3-D window | 55.81 -> 57.62 | 353.4 -> 344.7 | 300.8 -> 292.5 | (faults dominate) |

Full/window factor: DOOM 1.689 -> 1.568, Wolfenstein 3-D 1.254 -> 1.215.
DOOM window at 11-14 s: 18.0 -> 22.7 frames/s, monitor 335.3 -> 223.5
ms/s; whole 30 s run: 538 -> 575 frames, monitor 9,020 -> 7,944 ms.

Not modeled (outside the monitor's stamps, counted as guest time in the
trial and in the replay's calibration): `cpu_trap_enter`/`cpu_user_return`,
`dosvm_perf_enter`/`dosvm_client` lookups, the return's IRQ-off section
bookkeeping and frame copy (trap-entry selects), the two CPUIDs outside
the interval (every exit), and the native thread-table scan on the way
back to a DPMI client (every PM exit). On the next trial these show as
guest time per frame (`vm_perf_cpu`), and the trap-entry selects as
`vm_perf_checkpoint fast=`.

**fx27 model** (host code time scaled, noisy: the same sources gave 220
and 295 ns per lean VGA exit in two runs): DOOM window 16.80 -> 17.25
flips/s, 6.41 -> 5.71 us per VGA exit, factor 2.083 -> 2.020; whole run
`#DB` 14,346 -> 5,253 (checkpoints 9,370 -> 0), `#DB` ms 537 -> 195;
DOOM full screen `#DB` 17,513 -> 5,701. Wolfenstein 3-D 36.12 -> 35.69
flips/s (window; within the model's noise, its cost is aperture faults
priced by host time). The final sources' run: DOOM window 17.68 flips/s,
5.05 us per VGA exit, factor 1.980; Wolfenstein 3-D window 36.69 flips/s,
factor 1.911; the same runner without the trap-entry path
(`GUEST_NO_FAST=1`) gave 15.84 and 34.62 flips/s, 7.32 us per DOOM VGA
exit, against 18.25 flips/s and 4.62 us in the previous round. The
fast/lean comparison inside this model is noise-dominated; the exit bench
above is the controlled measurement.

**Equivalence.** Without the time model (simulated time = guest
instructions) the same runner with and without the trap-entry path
(`GUEST_NO_FAST=1`) ends DOOM (20 s) and Wolfenstein 3-D with identical
window pictures (SHA-256 of the PPM), plane-window maps (9,773 and
152,590), aperture faults and flips; trap counts differ by 69 and 9
(I/O bursts the lean path batches).

## 5. The BIOS worker (directive item 3)

Session 1 of the trial (`BOOT1.LOG`): `bios-input#37` takes 181 and 300 per
mille of two CPU windows (1975 ms, 1504 ms), with 54 and 65 device-budget
overruns of about 3.9 ms, and appears in no other window. Both windows are
the display mode switch `quake3.exe` asked for (`[display] mode=274
640x480x32` is logged between them, then the Savage engine is
re-initialized), not input:
- the session's input is native (`[input] ... source=ps2 ... firmware=0`,
  then two USB devices), so the BIOS input family is never enabled and no
  IRQ1/IRQ12 is reflected into firmware;
- the APM family was quarantined at boot (`family_quarantine family=apm`);
- the worker's kernel share in those windows is 4-5 per mille of its 181-300
  (`[cpu] top=` field `k`): almost all of it is V86 user time, i.e. the
  video ROM running VBE 4F02h/4F01h/4F03h (`biosvm_video_switch`), with no
  trapped I/O to speak of. Laptop VBE mode sets include panel power
  sequencing and timed waits in the ROM; their length belongs to the
  firmware and cannot be shortened without bypassing it.

Fix where clearly safe: without the firmware input service the worker no
longer wakes every 10 ms (100 empty P_DEVICE turns a second); requests
still wake it through `submit`. The video calls themselves are unchanged.

## 6. Tests

All on the final sources, host only (no QEMU, no image), run through
the capped host job wrapper:

| Test | Result |
| --- | --- |
| DOS VM block with ASan/UBSan (`v86_test`, `dosvm_test` sections `CIUKI_VM_DOOMWIN`, `CIUKI_VM_FX27`, `CIUKI_VM_T23DOSPERF`, `CIUKI_VM_DOSWIN3`, full `dosvm_test`) | PASS: fx-doomwin 34,326 checks; fx-27 622,987; t23-dosperf 117,276; fx-doswin3 presenter, flip-hold and presenter-dispatch cases; full run 62,269,319 checks, largest record 237 bytes (bound 240) |
| `dosvm_doomwin_test.inc` | seed demand: a proven, cached CLI wants no seeds, IRQ boundaries then arm no tracker, a missed CLI restarts the demand; trap-entry selects: 96 Mode Y/GR04 selects (V86 and PM32) leave the lean path's registers, window PTEs, dirty pages and DPMI boundary; chain-4, other indexes, OUT DX,EAX, TF, shadows, IPX and an unready launch refused with nothing changed; a pending IRQ gets the ordinary tail after one emulation; a V86 MOV SS after the OUT gets its shadow |
| Equivalence replays (DOOM, Wolfenstein 3-D, no time model, with and without the trap-entry path) | identical pictures, plane maps, faults and flips |
| Window/full replays, fixed and fx27 models, what-if, menu | all runs `pass=True` |
| Exit bench | completed, figures in section 4 |
| Changed kernel files compiled with the kernel's flags (`-Werror`) | clean |
| Full host kernel suite (`scripts/test/host_kernel_tests.sh`) | every section PASS, including the DOS VM block (62,269,319 checks), DOS game launch, exit/present benches, presenter, held-IRQ (fx-dospic), audio cadence (f3-40, F1-87) and latency tests; the run stopped at the f5-05B Win32 loader test on a build artifact a fresh worktree lacks (`build/win32/upper/pe`, made by `make upper`); with the artifacts copied (hashes checked against `config/wine-upper.json`) and the pinned FreeDOS 1.3 archives present, the remaining sections passed: Win32 loader and kernel32 (16 + 16 tests), Winsock, H56 unified binding, H57 unified file, DOS memory (Wolfenstein 3-D) and DOS extenders (DOS/4GW 1.97, Warcraft II demo) |
| Merge check: main `255a29583` exported, this change applied, no git operation | DOS VM block (62,277,924 checks) and the sections above PASS, kernel-flag compile clean, replay runner builds; its DOOM and Wolfenstein 3-D window replays give the same figures as this branch |
