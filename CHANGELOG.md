# Changelog

## Unreleased — 2026-10-04 integration (browser runtime checks pending)

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

Detailed qualification and limits: [current status](docs/project-status-2026-10-04.md).


All notable project-level changes are tracked here.
This changelog is intentionally concise. Every completed task should update `Unreleased` unless the task cuts a release section.

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
[detailed engineering record](docs/history/september-27-29-increments.md)
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

The [detailed 0.7.1 engineering record](docs/history/changelog-v0.7.1-2026-09-01.md)
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
