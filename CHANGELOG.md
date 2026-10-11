# Changelog

## 0.9.0 pre-alpha (unreleased, `main`, from 2026-10-09)

First version of the Ciuki VMM line. The 0.8 line is frozen on branch
`legacy-0.8`; prerelease 0.8.3 build 849 (`868cac9`) is its last release.
`main` builds the new 32-bit kernel and a 512 MiB FAT32 image
(`make build-full`). Main prereleases stay suspended by
`config/release-policy.json`; nothing of 0.9.0 is published.

Evidence status. Every result below was measured on QEMU TCG for one image
SHA-256 (`docs/validation/2026-10-11-integrated-image-f0-regression.md`)
unless it says "hardware". Hardware qualification is open: only the ThinkPad
T23 has run this line (unattended sweeps, see "Hardware"); the Armada E500
and the generic 1998–2004 desktop PCs of `docs/design/hardware-baseline.md`
have no recorded run, and F3 and F4 have no hardware evidence. A QEMU result
never qualifies a physical PC. QEMU TCG deviations are recorded in
`docs/validation/2026-10-09-f0/README.md` (FLD precision control; TCG does not
raise `#AC`, so the fault probe falls back on a page fault there while
hardware must still raise vector 17).

- Version identity: `config/version.json` is the single source
  (`0.9.0`, `pre-alpha`). The kernel logs `CiukiOS <version>` after its build
  banner, `uname` returns it in `version` (`release` stays the build ID), the
  desktop About window and the loader menu title show it, and
  `build-manifest.json` records it. `tests/host/test_version.py` checks that
  `VERSION`, this file and `README.md` agree with it.

### F0 — foundations (2026-10-09)

- Decide, with a second AI agent's review and the owner's mandate, to move
  to a 32-bit protected-mode kernel with Windows 95/98-class structure (dev
  diary 2026-10-09-06); write and cross-review seven design contracts before
  any code (`docs/design/`).
- Archive the 0.8 documentation and about 370 obsolete scripts in
  `legacy/*.zip`; reduce the tree from 121 GB to a few GB; add the
  `dev_diary/` record and the one-image test architecture.
- Add the F0 loader (MBR, `CIUKLDR` with fw_cfg test requests, E820
  normalization, VBE selection, ELF32 loading) and the 512 MiB FAT32 image
  builder with T1 checks and a build manifest.
- Add the Ciuki VMM kernel: paging at `0xC0000000`, GDT/TSS/IDT with a
  double-fault task, PIC with dispatcher-owned EOI, PIT at 1000 Hz, zoned
  physical allocator, kernel stacks with guard pages, user address spaces,
  priority scheduler with ring-3 preemption and a starvation boost, lazy
  FPU switching, six frozen F0 system calls, serial and framebuffer console
  (Inconsolata, OFL), allocation-free panic, resource registry with
  read-only PCI inventory, and the ten embedded acceptance probes.
- Add the test runner (`scripts/test/run.py`): shared lock, capped systemd
  scopes, qcow2 overlays, QMP, evidence parser, `result.json`; QEMU twin
  profiles for the T23 and E500 (later also `qemu-min128`, `qemu-desktop-1998`
  and `qemu-desktop-2002`); hardware replay fixtures and a loader reference
  model.
- Replace the Makefile: `build-full`, `test-host`, `qemu-test-full` and
  `qemu-run-full` target the new image; the 0.8 scripts stay as
  `legacy-*` targets until each component is migrated or retired.
- Declare the project scope (dev diary 2026-10-09-10): a modern retro
  gaming OS, not a Windows clone; DOS games natively, open-source engines
  built for CiukiOS, LAN play, later Windows 95/98 games through Wine and
  3D on chosen chips. Decide a POSIX-compatible native API for F2 and a
  network step (F6); extend the plan to F0 plus eight steps (F1–F8).
- Evidence on QEMU TCG: `f0-smoke` 2/2 and `f0-core` 92/92 on the five
  profiles (image `33a68c43…`). On the T23 the nine non-fatal F0 probes pass
  in the hardware sweeps; the deliberate `panic` probe halts the machine and
  ends `not_run reason=reset_before_completion`, recovered by a power cycle.

### F1 — drivers, storage, FAT32, input, safe mode

- Kernel services (f1-00): sleeping mutex, wait queues, deferred work,
  generation-checked handles, registry lifecycle with quarantine, shared PCI
  IRQ chains.
- Drivers: i8042 keyboard and mouse (f1-04), framebuffer device (f1-05), ATA
  PIO (f1-06). They start at boot (f1-08); safe mode limits activations to
  the console and input. A V86 monitor with virtual PIC/PIT/RTC and a
  serialized BIOS VM provides the firmware-first input backend that the loader
  selects for the Armada E500 platform (f1-07, f1-07b, f1-19).
- Storage: FAT volumes mounted at boot, read-only first and read/write after
  the read gate and the durability check (f1-09); 128 KiB boot log in
  `\SYSTEM\LOGS\BOOT.LOG`; read-only RTC provider; dirty-volume recovery at
  mount (f1-26, observed as `reason=dirty_recovered` on the T23); six
  deterministic corrupted-FAT32 fixtures and a crash-cut reboot case
  (f1-22, f1-24); read cases `fat12-read`, `fat16-read` and `fat32-read`.
- Loader and test infrastructure: `BOOT.CFG` options and platform detection,
  menu with safe-then-probe (f1-37), unattended hardware sweep
  (`probe=<phase>:sweep`, cursor, kernel self-reboot, f1-28), importer for
  physical captures (f1-33, f1-35).
- Hardware baseline (f1-20, owner decision 2026-10-11): one i686 image
  (CMOV) for the T23, the E500 and assembled desktop PCs of 1998–2004, no
  per-machine images.
- Evidence on QEMU: `f1-input`, `f1-storage`, `f1-fat32` and `f1-safe` pass
  in the single `f2-all` batch on image `b9c052cd…` (186 PASS; the three
  `uart-absent-*` cases need an operator, two `safe-menu-*` cases are
  physical-only and stay `not_run`).

### F2 — native processes, POSIX subset, libc, desktop

- Processes (f2-02): separate from tasks, PID 1 supervisor, restricted
  ELF32 loader with rollback, `spawn` with explicit inheritance, mmap arena,
  threads with TLS, wait words; signals (f2-04); files, paths, clocks and
  `uname` (f2-03, f2-10). The public ABI is `ciuki/abi.h`, system calls 0–68
  from f2-01, plus 69 (`devctl`), 70 (`proc_list`), 71 (`proc_terminate`) and
  72 (DOS VM window).
- POSIX subset and libc (contract `docs/design/posix-subset.md`): offline
  build of newlib 4.5.0.20241231 with the `ciuki-cc` wrapper, `crt0`,
  `libpthread` and `libciuki` (`docs/sdk.md`); the SDK headers are MIT.
  Lua 5.4.8 is built unmodified; its upstream test suite with `_U=true` exits
  zero in the guest.
- Crash isolation: the desktop runs as a ring-3 process; 100 victim cycles
  over five fault kinds leave it and a survivor running on the five QEMU
  profiles (`desktop_restarts=0`, post-fault input consumed, portrait region
  checked on the screen).
- Desktop (f2-08, f2-30): taskbar with clock, Ciuki menu built from `/bin`,
  minimise, window protocol v2 (close requests), About window with the
  approved Ciuki portrait, Tango icons (`desktop-shell-qemu-t23/e500` pass).
- Applications on the `libdeskui` toolkit (f2-31, f2-32, f2-33, merged
  2026-10-11, commit `1241de1`): Files, CiukNote (`/bin/ciuknote`) and Task
  Manager (`/bin/tasks`, using calls 70/71 and protocol v2 opcodes 15–20
  restricted to the launched Task Manager). QEMU evidence is pending at the
  time of this entry: their cases (`files-qemu-t23`, `ciuknote-qemu-t23`,
  `tasks-qemu-t23`) have not yet run on an integrated image.
- Evidence on QEMU: `f2-process`, `f2-runtime`, `f2-desktop` and `f2-app`
  pass in the `f2-all` batch on image `b9c052cd…`, including `app-gate`
  (upstream Lua suite and supplement).

### F3 — DOS virtual machines

- VM core (f3-00) on the V86 monitor: lifecycle and admission, private FAT16
  `DOSBOOT.IMG` with a file-backed copy-on-write overlay, virtual BIOS, the
  CIUKIDOS personality booting to the COMMAND_COMPAT shell, record channel on
  port E9, probe `f3:vm-boot`.
- Memory services (f3-03): A20/HMA, XMS 3.0, UMB, EMS 4.0 (entry vector
  `F100:0000`), synthetic INT 15h memory map, second-VM admission arithmetic.
- Devices (f3-01): per-VM PIC, PIT, keyboard controller and mouse (INT 33h)
  models with IRQ delivery.
- Video (f3-02): trapped VGA at A0000h–BFFFFh, INT 10h and VBE answers, fonts,
  the `/bin/dosbox` window and call 72, probes `vm-video` and `vm-window`.
- Evidence on QEMU: image `fcd96d6e…` is the first on which DOS VMs boot:
  `f3-vm` 195 PASS (F0–F2 prefix, then `vm-boot` and `vm-memory` on
  `qemu-t23`, `qemu-e500` and `qemu-min128`), `f0-smoke` 2/2, `f2-all` 189
  PASS (the three `uart-absent-*` operator cases excepted). `vm-devices`
  passes on those three profiles on a worktree image; the f3-01 and f3-02 code
  is merged and its integrated-image campaign is pending.
- Kernel-owned DPMI 0.9 host (f3-04: six stages, LDT at GDT index 8,
  INT 2Fh AX=1687h entry, DPMI16/DPMI32 conformance clients and a DOS/4GW
  client built with Open Watcom; `f3:dpmi` cases) and per-VM Sound Blaster
  16, OPL3 and 8237 DMA state models with the F4 consumer interface (f3-05;
  `f3:vm-audio-state` cases): merged with host evidence (327 host tests,
  libx86emu guest runner DPMI16/DPMI32/DOS4GW PASS); their integrated-image
  QEMU campaign is pending. DOS workloads such as DOOM and Wolfenstein 3D
  (f3-06) are in progress. F3 has no recorded hardware run.

### F4 — audio, optical media, NTFS read-only

- Contract `docs/design/f4-audio-media-contract.md` approved 2026-10-11.
- Native audio (f4-01): `/dev/audio`, `/dev/mixer`, call 69, integer mixer,
  Intel ICH AC'97 backend for five PCI IDs, persistent master volume,
  probe `f4:audio-native` with a WAV checker. `audio-native-qemu-t23` passes on
  image `38a7b4e7…` (4.0 s capture, six windows at 440/880 Hz).
- Optical media (f4-02): ATAPI PIO, ISO 9660/Joliet through the `fs_ro`
  interface, probe `f4:optical` with eight cases; `optical-l1-qemu-t23`
  passes on image `38a7b4e7…`.
- NTFS read-only (f4-03, f4-04): independent `ntfs_ro` library (GPL-2.0-only,
  no NTFS3 code) with 27 deterministic fixtures and 14 host tests; mounts
  through `fs_ro`, cache halves and on-demand optical cache slots, probe
  `f4:ntfs-ro` with 12 `f4-media` cases. The f4-04 code is merged; its QEMU
  campaign is pending.
- Not in this version: ESS Maestro-2E backend, the audio path of DOS VMs,
  2D acceleration (deferred by the contract).

### Hardware (ThinkPad T23 only)

- The T23 boots the integrated image from disk, mounts C: read/write with the
  boot log and shows the desktop with the portrait. Three unattended sweeps
  (33, 35 and 35 boots) ran F0, F1 and F2 probes; the third (image
  `6ed146ab…`, capture `77777777`) ended `SWEEP_END passed=29 failed=4
  not_run=2`. `app-gate`, `fd-table` and `fat-read` pass; the four failures
  are `input`, the `crash-isolation` interaction under an operator's event
  stream, the `channel-eintr` subcase of `signals-fault` and the `safe` step.
  The diary attributes them to criteria written for QEMU (exact stimulus
  totals, a safe-mode step outside safe mode, a thread state sampled as
  `T_READY`), addressed by f1-34, f1-36 and f2-23; their re-run on hardware is
  not recorded here. No Armada E500 or generic desktop PC run is recorded.

## 0.8 line — final state (2026-10-09, branch `legacy-0.8`)

- Commit the T23/E500 hardware work of test images R6–R17 (modular kernel
  memory layout, VMFORK diagnostics, session disk ATA, Mach64 backend, S3
  qualification logging); the physical findings are in dev diary
  2026-10-09-01. Prerelease 0.8.3 build 849 closes the line.

## 0.8.3 post-release integration — 2026-10-04 (browser runtime checks pending)

- Add a bounded CiukWeb pipeline: basic HTML, GET forms and XMS-backed
  PNG/GIF/baseline-JPEG images; a CSS style worker; a BearSSL TLS 1.2 worker;
  and an mQuickJS worker with a limited classic-script/DOM bridge. Target builds
  and host parser/engine checks pass; browser-level TLS/CSS/JavaScript runtime
  validation remains pending. This is not full CSS, a complete DOM, or Internet
  Explorer equivalence. Adam7 PNG and progressive JPEG remain unsupported, and
  GIF displays its first frame.
- Reduce the AC'97 queue from eight 1,024-frame descriptors to eight 256-frame
  descriptors: about 46 ms queued at 44.1 kHz versus about 186 ms. Post-change
  Doom playback ran with 6–8 descriptors queued and no new underruns during
  the sampled interval. The PCM capture does not qualify latency through the
  final host speaker backend.
- Show About at startup by default, with “Don't show this at startup” to
  suppress it; redraw active titles after focus returns. Focused Linux QEMU
  checks verified the default and saved preference across reset, plus title
  reactivation after desktop focus.

## 0.8.3 — 2026-10-04

- Fix the permanent Doom launch stall after network traffic: preserve physical
  IRQ self-masking and let level-triggered devices acknowledge their own IRQs.
- Stream and filter large HTTP responses within the existing document buffer,
  follow HTTP redirects and report partial pages without aborting the transfer.
- Separate About and Credits, preserving Ciuki's dedication and the startup
  preference; fit both pages at the compact desktop resolution.
- Add native VirtIO GPU 2D presentation, retained VGA frames and independent VM
  clocks; suppress identical uploads and transfer actual pixel damage.
- Stop desktop meters from triggering a full-scene repaint every second; correct
  the banked compositor's scratch capacity load to prevent 16-bit mode overruns.
- Add physical/virtual PS/2 wheel support and scrolling in desktop applications.
- Add Display Properties with adapter/driver/monitor details and graphical
  resolution preview, confirmation and automatic rollback.
- Start the resident network service automatically for a detected packet
  driver; move Ciuki Web requests into a cooperative native HTTP client.
- Open About at startup instead of Application Library, with a saved checkbox;
  fit window captions to their available title space.
- Keep trusted GUI utility output off the display; update release packaging,
  memory-capped push builds, documentation and screenshots.

Detailed qualification and limits: current status.


All notable project-level changes are tracked here.
This changelog is intentionally concise. Every completed task should update the topmost unreleased section (`0.9.0 pre-alpha`) unless the task cuts a release section.

## Earlier development snapshot — 2026-10-01

- Reworked the README as a screenshot-led overview and split build steps,
  current status and compatibility details into short linked guides. Older
  phase notes remain available in collapsible history documents.
- Chose C/OpenWatcom for native `.APP` modules and 32-bit DOS/4GW programs.
  Added CiukWeb as a native desktop browser with mTCP `HTGET` transport and
  an HTTP text/link view. Bundled a pinned TinyGL static library, headers,
  license and a software-rendered triangle demo; full OpenGL conformance and
  guest GPU acceleration remain open.
- Presented queued desktop damage before VM polling so CiukPaint strokes
  appear while dragging. Narrowed DOS-window repaint to changed video bands,
  corrected forked-VM timer calibration and delivered virtual SB16 completion
  IRQs in the same audio pass. Focused DOOM, Paint, browser, OpenGL and cursor
  gates pass. The current image has 26/26 VM-window gates passing across
  resource-bounded serial recovery runs after a host reboot; the automated
  profile was interrupted and is not recorded as one uninterrupted pass.
- Added a fixed-size QEMU recording runner with KVM, NE2000 Internet NAT,
  preferred guest EDID and optional host OpenGL presentation. A QEMU gate
  checks the 1280×800 window across desktop, DOS and VGA preview modes. The
  opt-in VGA-fast Doom-vanille timedemo passed at 194 realtics for 350
  gametics after the benchmark learned to enter DOS from the desktop.
- Added mTCP HTGET. A GPL MicroWeb prototype was adapted and tested in an M4
  DOS window, then removed from the current image when CiukWeb took over the
  desktop browser role. The experiment and its source/license record remain
  in the dated validation notes.
- Refined the desktop top bar into five aligned, clickable readings for sound,
  network, CPU, free conventional memory and disk activity. The Ciuki portrait
  now opens the system menu; duplicate lower-bar buttons and wallpaper status
  overlays were removed.
- Reworked About into separate system, project and dedication sections. Added
  an IPv4 network panel that saves its profile and updates the running Ciuki
  network service. Captured fresh QEMU screenshots for the README.
- Expanded Network with host name and MTU settings, subnet-mask validation,
  PCI adapter details, packet-driver/MAC status and direct Device Manager and
  Drivers controls. DHCP can start the network service, obtain a lease and
  return to the desktop. Fixed grouped controls swallowing clicks and restored
  command execution from module windows. The focused QEMU gate covers static
  settings, invalid input, adapter navigation, an NE2000 PCI DHCP lease and
  driver disable across reboot. A compact layout keeps the panel usable at
  640×480.

## pre-Alpha v0.8.0 (in development; 2026-09-30)

- The current development image reports 0.8.0 in the kernel, shell, setup,
  editor and About window. The system About now includes open-source credits,
  GPLv2 copyright, Alcybercloud.it attribution and the owner's approved
  dedication to Ciuk.
- Added long FAT16 names through the resident LFN extension, CiukNote,
  CiukPaint, refreshed desktop controls, Device Manager and a licensed era
  driver catalog. Their focused QEMU gates passed on the feature builds.
- M4 complete for its tested QEMU scope: DOS windows own forked VMs. The focused QEMU gate on the
  final development image passed original DOOM and doom-vanille with Files
  open, bounded close, two
  text VMs, click focus, keyboard routing and independent close. The desktop
  event loop now handles input before continuous guest-video damage, avoiding
  a redraw loop that starved mouse clicks. The DOS-window harnesses now test
  behavior through the module interface; the final 0.8.0 VM-window profile
  passes **26/26** gates. The HDPMI lifetime gate also passes after its
  unhandled physical IRQ path was corrected.
- A DOS guest I/O gate now verifies INT 33h movement and clicks, SB16 DMA,
  OPL and captured PCM audio inside an M4 window. The original DOOM gate
  passed again after the PS/2 packet delivery fix.
- The README now has a dated history and QEMU screenshots, including DOOM
  and two DOS windows from the passing focused M4 gate.

### 27–29 September increments

The desktop, monitored VGA session, guest input/audio devices and M1–M3 VM
manager converged. QEMU debugging found interrupt, memory, video and DOS
return faults that were corrected before M4. The
detailed engineering record
retains the individual fixes and evidence boundaries.

## pre-Alpha v0.7.1 (2026-09-01)

- Closed the Phase 5 loader/kernel ownership boundary on the `full` and
  `full-cd` images. The DOS kernel, shell and installer became independently
  loaded components with versioned interfaces and size checks.
- Expanded bounded QEMU coverage for external DOS programs, Doom/doom-vanille,
  legacy audio, packet networking and the Live/install CD. Results retained
  their exact workload limits; Phase 6 and Phase 7 were not closed.
- Tuned KVM and optional VGA-fast execution, mouse capture and DOS process
  return. A Windows 3.1 workflow was tested at the time; Windows 3.1 was
  subsequently removed from the current image.

The detailed 0.7.1 engineering record
keeps the original probes, timings, fixes and failure boundaries.

## pre-Alpha v0.6.7 (2026-05-08)

1. Rebased the roadmap after Phase 4: DOOM visual gameplay is closed, while Stage1/runtime split work, broader DOS app compatibility, legacy audio, and full/full-CD hardening are the next priorities.
2. Advanced the Stage1/runtime split foundation with `\SYSTEM\RUNTIME.BIN`, runtime service-table probing, callable service ids 1-5, corrupt-runtime fallback checks, default-drive state bridging, and Stage1 size recovery.
3. Hardened full-profile DOS compatibility across C:/D: drive state, per-drive CWD, FAT16 path/create/open/delete/rename behavior, INT 21h country/IOCTL/switch/PSP/handle/memory services, and external app return-state handling.
4. Expanded external DOS application evidence with CIUKEDIT/GFXSTAR smoke coverage, optional DOSNavigator packaging/startup validation, shell chrome isolation, temporary INT 10h/INT 33h external-app handling, and DOSNavigator-focused stability fixes.
5. Restored and revalidated DOOM startup/gameplay after memory-map and allocator regressions; improved DOOM/DOS taxonomy lanes with honest `runtime_stable` and `visual_gameplay` classification.
6. Hardened the full-CD Live/install path with the direct El Torito ISO as primary output, visual SETUP UI, destructive HDD install flow, `FORMAT.COM`, topology guards, eject-before-reboot prompt, batched install I/O, and ThinkPad T23 real-hardware follow-ups.
7. Added public docs for DOS compatibility and legacy audio planning, updated support links, cleaned obsolete project artifacts, and aligned release-facing metadata for `CiukiOS pre-Alpha v0.6.7`.

## pre-Alpha v0.6.5 (2026-05-05)

1. Established the Stage1/runtime split as the structural direction: Stage1 remains loader-first while runtime, shell, driver/CD policy, diagnostics, and module responsibilities migrate toward loaded components under `\SYSTEM`.
2. Added the inert `src/runtime/runtime.asm` artifact, packaged it as `\SYSTEM\RUNTIME.BIN`, documented the split plan, and kept default full/full-CD boot behavior stable.
3. Validated the slice across active full/full-CD build, QEMU smoke, shell, driver, setup, Stage1 selftest, runtime-probe, and DOOM taxonomy lanes.
4. Updated version, banner, ISO label, README, roadmap, and release metadata for `CiukiOS pre-Alpha v0.6.5`.

## pre-Alpha v0.6.3 (2026-05-05)

1. Promoted the full-CD profile into the main Live/install media path with direct El Torito hard-disk boot, D: live shell behavior, SETUP destructive install support, and disposable HDD install validation.
2. Added CHS/EDD boot and setup hardening, CD/HDD probe lanes, installed-HDD boot checks, and user-facing QEMU run/test targets for the full-CD profile.
3. Improved shell and FAT16 DOS behavior around drive semantics, current-directory preservation, prompt recovery, case handling, footer telemetry, and repeated COM/EXE execution stability.
4. Added DOOM `runtime_stable` taxonomy classification so post-video observation failures are reported honestly instead of being hidden behind earlier startup stages.

## pre-Alpha v0.6.1 (2026-05-04)

1. Closed Phase 4 as DOOM gameplay playable on the full FAT16 profile: DOOM launches through DOS/4GW, loads WAD data, reaches video/gameplay runtime, and was manually confirmed playable by the project owner.
2. Added the full-profile DOOM taxonomy harness, local-only DOOM payload packaging, and staged runtime fixes across MZ loading, FAT16 read/seek behavior, PSP/MCB setup, DOS memory strategy, XMS move support, and DOS extender startup classification.
3. Closed the Phase 4 installer execution lane with deterministic setup scenario coverage and release-facing documentation updates.

## pre-Alpha v0.5.4 (2026-05-01)

1. Improved shell input stability for hold-key repeat, line wrap, and backspace behavior.
2. Stabilized FAT16 shell footer telemetry and revalidated cross-profile build/regression lanes.

## pre-Alpha v0.5.3 (2026-04-30)

1. Stabilized shell `move`/`mv` behavior across floppy and full runtime profiles.
2. Fixed `INT 21h AH=56h` rename/move behavior and revalidated shell command regression lanes.

## pre-Alpha v0.5.2 (2026-04-29)

1. Closed Stage1 DOS command regressions across floppy and full profiles.
2. Stabilized critical INT 21h read/write/seek return paths and floppy image write behavior.

## pre-Alpha v0.5.0 (2026-04-28)

1. Stabilized Stage1 startup, shell entry, prompt/input paths, drive/CWD state, and QEMU stderr observability for more deterministic bring-up.

## pre-Alpha v0.5.0 (2026-04-22)

1. Restarted the project from a clean legacy BIOS x86 architecture baseline.
2. Introduced the initial `floppy` and `full` build profiles with build/QEMU smoke scripts and baseline branch/documentation discipline.
