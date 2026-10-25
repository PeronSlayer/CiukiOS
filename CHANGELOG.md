# Changelog

## 1.0.0 alpha — 2026-10-25

CiukiOS is a modern retro-gaming operating system dedicated to Ciuki, a dog: a 32-bit protected-mode kernel with a Windows 95/98-class desktop, DOS games in virtual machines, Win32 programs and a network stack. *Classic computing, one woof at a time.*

### Highlights

- **Kernel and resources.** Windows 98/ME-class memory, scheduling, disk cache and real-time management; a crash-resilient pagefile; a hardware entropy source.
- **Storage.** FAT32 with a transactional cache, parallel ATA, USB mass storage, and a faster FAT chain cache for large sequential reads.
- **DOS.** DOS games run in virtual machines on a unified FreeDOS-based kernel with XMS, EMS and upper memory, a virtual NE2000/IPX network adapter, and fast game launch. DOOM, Wolfenstein 3-D and Quake start from Programs > Games.
- **Win32.** A Win32 subsystem with dialogs, controls, shell integration and original installers (NSIS, CAB, self-extracting) running on reused Wine 10.0 libraries.
- **Network and web.** lwIP TCP/IP with DHCP and DNS, TLS 1.2, and the NetSurf 3.11 web browser (images, clipboard, JavaScript with Duktape).
- **Desktop and media.** Per-program icons, Start menu with groups, WordPad, Paint, Music, Solitaire, Clock, Task Manager, CiukIDE, System Information and a redesigned Control Panel; AC'97 audio; print spooler; English and Italian language indicator.
- **Graphics.** 2D acceleration on supported video chips with a safe software fallback (`accel=0`).

### New since the earlier alpha images

- **Boot.** A hang at the splash screen on real hardware is fixed: the cache publication path no longer livelocks against the video-marker task and the storage writer.
- **DOS windows.** Closing a DOS window (Alt+F4, or `exit` at the prompt) no longer freezes the system. A program that ends within 10 seconds leaves its window open with its output, titled "Finished - <name>". `DIR` shows the correct free space.
- **DOS memory.** XMS is visible to DOS programs again and the shell no longer stays resident: about 585 KB of conventional memory are free at the prompt. The DOS Box has an EMS device (`EMMXXXX0`) and upper memory blocks. Wolfenstein 3-D starts.
- **Printing.** The print spooler is event-driven and uses about 0.3 % of the CPU (65-77 % before); offline jobs are retried every 15 seconds.
- **Audio.** A sound that starts right after another no longer begins with an underrun.
- **NetSurf.** Images over one megapixel display again; Ctrl+L no longer types an extra "l" into the address bar; the system clipboard works; the browser closes cleanly; JavaScript is on again.
- **Desktop.** New look for Open/Save dialogs, Control Panel, Music, WordPad, Paint (28-colour palette), Solitaire, Clock, DOS window bar and properties, Task Manager, disk Properties, CiukIDE, System Information and the log viewer; the power icon is hidden on machines without a battery.
- **Setup CD.** The system component includes `/System/SFC.LST` and `/System/printspool`.

### Known issues (planned for 1.0.1 alpha)

- Applications are slow to launch.
- NetSurf renders pages slowly.
- The Control Panel and property sheets show redraw glitches.
- Changing the theme while music plays from a USB stick can hang the system.
- Hardware acceleration for DOS games still needs improvement.
- This is an alpha: hardware qualification is not complete on every machine class, and the virtual machine packages have not been tested on every host.

### How to try it

- **QEMU:** extract `ciukios-1.0.0-qemu.zip` and run `run-qemu.sh` (Linux, macOS) or `run-qemu.cmd` (Windows).
- **VirtualBox:** import `ciukios-1.0.0-vm.ova`.
- **VMware:** extract `ciukios-1.0.0-vmware.zip` and open `ciukios.vmx`.
- **Setup ISO:** `ciukios-1.0.0-setup.iso` boots the CiukiOS Setup from a CD (it is not a hybrid USB image).
- **Real hardware:** write the raw disk image from `ciukios-1.0.0-disk.zip` to a whole disk with Rufus (DD mode), balenaEtcher or `dd`; this erases the destination. Targets are late-1990s and early-2000s desktops and laptops with an i686 CPU, BIOS, VBE 2.0+ video, PS/2 input, parallel ATA and 128 MiB of RAM or more.

## Development log, 2026-10-20 to 2026-10-22 (hardware image `3423912f8463…` of 2026-10-22)

Covers all changes merged on `main` since 0.9.2 on 2026-10-20. The Setup CD installer (f9) has been deferred to a later release (owner decision 2026-10-21 20:29, dev diary 2026-10-21-07); the 1.0 Alpha release ships as the hardware disk image `build/hw/ciukios-hw.img`. Full hardware qualification on real hardware is pending for this image.

### Kernel and resource management

- Trial of image `a4d9454e…` (dev diary 2026-10-22-07) and the sixth image (2026-10-22-09): desktop CPU cut on the T23 model from 40.6 % to 2.1 % (shortcut and sound workers, pixman region code for damage) (f2-95), audio producer priority (f1-101), single pointer and EC volume poll every 2 s with proven SMI time excluded from budgets (f2-96), music decoder cost and per-stage record (f4-37), credits and About (f2-97).
- Trial of image `5a10eef4…` (dev diary 2026-10-22-05) and the fifth image (2026-10-22-06): f1-100 (no periodic EC I/O — the EC event line is routed to SMI#, two 2.1 ms SMIs per read), f1-96b (UHCI ring walk stale status; fallback ladder before quarantine; re-plug), f2-92 (the desktop draws with the Savage engine: late readiness, bounded waits, write/offscreen operations, honest indicator), f6-22 (e100 receive mask: DHCP works; network state records; NetSurf lines complete).
- Trial of image `b453bc92…` rejected (dev diary 2026-10-22-03); thirteen corrective merges on 2026-10-22 (dev diary 2026-10-22-04): Savage first-fill priming (f1-95), boot-log classes (f0-56), 2D acceleration YES/NO indicator (f1-97), unified DOS kernel actually bound (f3-61), holds without preemption points (f1-93b), priority inheritance and desktop share (f1-98), Display properties worker (f2-88b), NetSurf budget and exit (f6-20), desktop pointer merging (f2-91), USB 64 KiB transfers (f1-96), SMI attribution and EC poll (f1-94), entropy worker in an idle task and kernel PDEs with Accessed preset (f6-21/b).
- Trial of image `8f59e221…` on the T23 (2026-10-21, dev diary 2026-10-21-10) rejected: seven corrective merges on 2026-10-22 — f1-92 (`125fefa2`: an issued ATA command ends only on its device deadline; since f3-49b a cancelled request was treated as a device fault, quarantining the drive and turning C: read-only), f1-93 (`c394506a`: native volume holds never span cold device reads), f6-19 (`d64787e0`: NetSurf gets its window channel from every launch route, Programs entry with icon, entropy initialises on the Pentium III), f2-88 (`8a181714`: Display properties decodes its preview off the UI thread), f2-89 (`bb8d652f`: removable media popup painted), f2-90/f2-90b (`5d28b71d`, `ce19c3bb`: wallpaper in the first desktop frame), f0-55 (`6f121d69`: boot splash progress bar).
- f1-87 (`c19230eb`): T23 trial fixes — tagged volume holds with a missing-tag control, audio refill priority, the windowed DOS VM decoupled from the presenter, `usbscan=0` loader token, `device_overrun` attribution record.
- f1-90 (`3c50bb79`): device CPU budget accounted per task with a max-min fair share; the audio refill worker has its own 2 ms per 10 ms budget and a direct interrupt wake; `device_top` and `audio_refill_overrun` records; contract R1/R2 amended.
- f1-89 (`551bd15a`): reserved cache publication and FAT transactions — bounded logical holds, writer turn with a wait metric, in-flight readers block publication, single-page transactions, no barrier per 8 KiB overflow chunk, guarded lookups never read the disk under the guard (strict FS gate exit 0, 10 MiB copy replay 1,278 commands, 4,678 crash cuts clean).
- Fixed (`dbb0d714`, `cc81cbb6`): Addressed peer thread metadata pin contention where concurrent walks of a process memory map yielded `EBUSY`, allowing task walks to wait out metadata pins and avoiding duplicate pins on pre-pinned caller copies (`test_peer_walk_copy`).
- f0-49b (`a7f933be`): Pagefile mapper cluster cursor is now validated by the chain stamp and reset on error, ensuring trailing trims followed by regrowth cannot map reused clusters belonging to other files (`trim_regrow_reuse`).
- f0-49c (`66d0ae55`): Pagefile extents in `C:\SYSTEM\PAGEFILE.SWP` are preserved across boots and extent growth is restricted to metadata allocation without synchronous 16 MiB zeroing, decoupling desktop startup from the pager manager and resolving boot stalls under `icount`.
- f1-86 (`5ce6ce41`): Kernel contention instrumentation added to monitor volume reservation hold durations and waiters (`vfs_hold`/`vfs_wait`), per-command ATA DMA/PIO service timings, serial DOS VM exit profiling, and unmasked IRQ-off sections (`firmware_max_us`).
- f6-16 (`3d001784`): Kernel entropy subsystem integrated with pinned `jitterentropy-library 3.7.0` and a ChaCha20 DRBG, providing syscall 91 (`getentropy`), SDK `getentropy`/`arc4random` interfaces with finite deadlines, and `entropyraw=1` capture mode.

### Storage

- Fixed (`7c5b9878`): Prevented ATA command queue throttling from getting locked at 512 bytes on slow commands by introducing a 4,096-byte (or sector size) minimum floor and restoring maximum queue depth after fast transfers (`test_blkreq`).
- f3-51c (`abd1c7c1`): Per-open-file FAT cluster cursor validated by chain stamps reduced sequential reads of a 27 MB image from 2,801,342 to 13,295 FAT lookups, combined with an LRU cache of 16 base digests indexed by volume version.
- f1-88 (`985aeb9e`): S3 SuperSavage BCI self-test sequence updated with CR50 depth selection preceding descriptor writes, incoming CR50 buffering, completion fence, and an `accel=0` boot configuration token for safe hardware fallback.

### DOS and unified DOS

- Owner decision 2026-10-21 (dev diary 2026-10-21-09): the unified kernel is the default for every DOS game without a saved choice; `DOS kernel=ciukidos` selects the classic kernel; the DOS Box shell stays classic until f3-58.
- f3-56, f3-57 (`d0e05169`): unified DOS — native DFV2 binding adapter, `CIUKIDF.SYS`, FreeDOS 2043 fork patch 0002 (lifecycle hooks, redirector sites, FCB/NLS, critical-error GS fix), live VFS backend for C:/D:/E: at the seven redirector sites, INT 24h critical errors with the real frame, a refused activation stops the VM with a record, the fork kernel installed in the image under `kernel=unified` once `fork-qualified.json` exists.
- f3-46 (`ed2f028a`): Converted the DOS VM 24 MiB memory pool into a pageable object (Contract D1) backed by demand paging and pagefile swap, reducing creation latency from 134.3 ms to 23.6 ms with protected I/O fault handling.
- Fixed (`85b9024a`, `b2bc10a7`): Optimized DOS game launch base hashing from sector-by-sector seeks into a single sequential pass with 64 KiB reads, reducing host hashing elapsed time from 29.2 s to 0.25 s (`test_game_window_base_hash`).
- f3-52 (`84f01cd2`): Replaced 2 ms DOS window rendering polling with event-driven blocking frame waits (`WAIT_PRESENT`), donating priority to the VM thread while waiting and throttling loading panel repaints to <=4 Hz.
- f3-54 (`6ddc7035`): Unified DOS contract established, merging the FreeDOS 1.3 kernel base with the CiukiDOS identity, banner, and prompt, using the VMM redirector interface (INT 2Fh AH=11h) for native VFS routing.
- f3-55 (`2b857092`): Pinned FreeDOS 2043 kernel fork built natively and reproducibly with Open Watcom 1.9 (patch 0001, SHA `d24464ec…`), selectable at boot via `kernel=unified` alongside the classic CiukiDOS kernel.
- f6-11 (`690fe9df`): Virtual NE2000 network adapter (DP8390 model) and Ethernet II IPX bridge implemented for DOS VMs over the native netif and packet pool, providing VMM IPX API emulation with DPMI routing, ESR delivery, and `NETID.BIN`.
- f3-53 (`251ab754`): DOS title battery test harness and manifest schema established covering tracked titles across bundled, test-media, and owner-copy classes with calibrate, accept, and verify modes.
- Fixed (`19346ce6`): DOS Box settings panel updated to configure the unified kernel on key 7 and network modes on keys 5 and 6.

### Win32 and installers

- f5-15a2 (`a72352fd`): NSIS, CAB and self-extracting installers — advpack/setupapi/cabinet qualification, zero-length `ReadFile`, case-insensitive paths, `w32-installer-nsis` probe.
- f5-15b (`01fa33ec`): original installer acceptance — ten installers load, workloads imported with SHA-1 receipts, installer title tests and census.
- f5-15c (`90c9e903`): QEMU installers tier — ten `f5:win32-<alias>` probes in the kernel, loader table and runner model, guest UI driver, manifests and calibration commands, installer CD images in the runner, `_vsnprintf`/`_vsnwprintf`, named newer-subsystem records (per-title QEMU calibration still to run).
- kernel32 profile (`a17e29f8`): `WritePrivateProfileStringA` rewrites keep one entry in place, duplicates dropped, NULL value/key deletion, line endings preserved.
- f5-10c (`d772a901`, `74718423`): Win32 dialog manager, standard controls, menus, accelerators, `MessageBox`, and TrueType font selection implemented.
- f5-10c2 (`e246bda5`): TrueType font bridge routing glyph rasterization through `w32srv` via FreeType, baseline alignment corrections across golden frames, and `dialogs-exercise.exe` guest probe fixture.
- f5-10c3 (`588609e7`): Dialogs fixture results page relocated to `0x10000000` to avoid conflicts with loader metadata structures.
- f5-10c4 (`97310308`): `LdrAccessResource` enhanced to support size-only, address-only, and no-output queries, resolving a regression where `SizeofResource` returned 0 and broke dialog creation.
- f5-20a, f5-20b, f5-20c (`593c5ce2`, `29f80401`, `39a67111`): Implemented 347 core exports across `kernel32`, `ntdll`, `advapi32`, `user32`, and `gdi32` to satisfy dependencies for 16 reused Wine DLLs, enabling all 16 DLLs to initialize successfully (`DllMain` returning TRUE).
- f5-20d (`5db64490`): Cached build step for reused Wine DLLs integrated into `make build-full`, staging 16 verified DLLs and adding the `w32-upper-dlls` probe.
- f5-15a (`19a105b5`): Installer shell integration providing `shell32` and `shlwapi` APIs, standard shell folders, `.lnk` shortcut creation, file operations (`SHFileOperation`, `ShellExecuteEx`), temporary directory staging, and delayed rename support via `WININIT.INI [rename]`.
- f5-07b (`396c248d`, `e020e2a1`): PROGMAN DDE service, session termination messages (`WM_QUERYENDSESSION`/`WM_ENDSESSION`), and virtual restart handling via `WININIT.INI` and `RunOnce`/`RunServicesOnce` keys with `runonce.exe`.
- f5-07c (`32d438af`): Reduced Win32 service idle traffic by caching client connections in `ntdll`, avoiding allocations during active sessions, and moving desktop status polls to an asynchronous worker thread.
- f5-07d (`e020e2a1`): Restart validation fixture and `f5:w32-restart` guest observer registered on the shared desktop fixture gate.
- f5-19p (`8d833fbd`): Win16 contract P0 dispositions established, routing Win16 file I/O through the VMM INT 21h backend and scheduling 32-bit segment support for post-1.0 Beta.

### Network and browser

- f6-18 (`311d33bc`): runner HTTPS cases for NetSurf — `f6:browser run= url= [now=1]` selector with the full URL grammar in the loader, kernel browser probe with a resolver override, test profile with fixture CA anchors, `https_fixture.py`, six `f6-net` cases (QEMU runs pending).
- f6-10 (`ece0ae07`): lwIP 2.2.1 TCP/IP stack imported with native `sys_arch` integration, supporting native socket system calls 78–89, DHCP client, DNS resolver, and C SDK socket headers.
- f6-13 (`24267b26`): NetSurf 3.11 browser port plan established, defining native socket fetching, BearSSL TLS, and framebuffer rendering (`libnsfb`) on the desktop v3 windowing protocol.
- f6-14 (`84f01cd2`): NetSurf Milestone 1 (M1) frontend delivered, enabling native window rendering and offline HTML/data navigation.
- f6-15 (`96ec06e1`): NetSurf Milestone 2 (M2) HTTP/1.1 fetcher delivered, featuring an asynchronous DNS worker thread, BearSSL TLS 1.2 support with converted CA bundle, cookie management, and offline error classification.
- f6-17 (`aac5cb00`): BearSSL entropy seeded per connection via `getentropy`, integrated with an internal browser readiness page, chrome reload handling on error pages, and host HTTPS acceptance tests.

### Desktop and media

- f2-86 (`ffdb437a`): Toolkit timer events in `libdeskui` decoupled from whole-window invalidation, idle polling stopped in Files, and desktop main idle loop throttled to >=10 ms sleep, eliminating heavy background CPU consumption.
- f2-87 (`aed8765d`): Add/Remove Programs control panel applet delivered, managing uninstallation across Win32, DOS, native, and bundled game packages via `GAME_QUERY`/`GAME_REMOVE` interfaces.
- f7-07 (`33fded92`): Hardware-independent 3D rendering delivered via `opengl32.dll` and WGL bindings implemented atop the `sdk/libgl` software rasterizer.
- f5-12b, f5-12b2 (`e2ac5a1e`, `7f6b61a3`, `ef66ce72`): Media Control Interface (MCI) subsystem delivered, supporting `waveaudio` (PCM/ADPCM), `sequencer` (MIDI streams via TinySoundFont), `avivideo` (`mciavi32.dll` with DrawDib), VfW codecs, and digital CD audio reading (READ CD) with analog fallback.
- f5-22 (`32d438af`): Audio CD playback integrated into `C:\WINDOWS\CDPLAYER.EXE` featuring the Tango `media-optical` icon and Start Menu accessories placement.

### Loader

- f0-55b (`a056ae8c`): splash bar visible from the first frame (centred sweep segment at tick 0, phases reported before the paint applied), console worker on its own 40 ms timer above init's level while the splash is up, one serial record per phase.
- f0-54 (`f162192b`): Loader memory budget overhauled by replacing repetitive option parsing with a shared bounded table matcher, streaming ELF program headers through the E820 scratch buffer, and reclaiming 1,303 bytes within the fixed 64 KiB loader extent with zero differential divergence across 60,102 tested selectors.
- f3-55 / fix-dosbox (`2b857092`, `19346ce6`): Added loader command-line and interactive menu parsers for `kernel=unified` alongside existing DOS conformance selectors.

### Tooling and tests

- f5-21, f5-21b (`c2ff1c90`, `4745eff8`): Automated probe registry infrastructure, write gates (`GATE_W32_FIXTURE`), and ten guest observers covering `kernel32`, fullscreen display, registry, audio, GDI, crash reporting, USER, DirectDraw, DirectInput, and codecs.
- f5-17b (`96e0dcb1`): Win32 acceptance runner harness tiers, automated fixture disk generation, recorded input event replay, and cross-selector table verification.
- Toolchain update (`5fd91e4c`): Requalified host toolchain pins for Clang, LLD, and LLVM from 23.1.1 to 23.1.3 across SDK, applications, upper DLLs, and kernel tests.
- f0-50, f0-51, f0-53 (`a57de459`, `27db294d`, `79b9536a`): Host regression repairs, including scheduler name unification (`CIUKI_SCHED_NORMAL`), firmware record headers, bounds assertions, gate test links, build image license inventories, and directive index hygiene.

## 0.9.2 pre-alpha (unreleased, `main`, from 2026-10-20)

Version identity moved from 0.9.0 to 0.9.2 on 2026-10-20 (owner decision,
dev diary 2026-10-20-01). It covers everything merged on `main` since 0.9.0
began: the 0.9.0 section below stays the detailed record. Nothing of 0.9.2
is published; hardware qualification is open.

Regressions found on 2026-10-20 by the first QEMU boot of an integrated
image since the gates were suspended (dev diary 2026-10-20-02):

- Fixed (`dbb0d714`, `cc81cbb6`): since f0-44 every walk of a process's
  memory map held a whole-space pin across its service points, so another
  thread of the same process got EBUSY from mmap, buffer pins and copies to
  user memory. Native applications could not open their windows ("desktop
  connection failed: Broken pipe") and the desktop could not load its
  wallpaper. Found by bisection on QEMU (good at the f0-45 merge, bad at the
  f0-44 merge); a task now waits out another task's metadata pin, and a copy
  covered by the caller's own pin needs no second pin. Regression test
  `test_peer_walk_copy`.
- Fixed (`8f3bcda9`, `8f86c3ad`): the canonical image build failed since the
  FreeDOS reference was provisioned: games-disk directories of the DOS
  payloads, an 8.3 name, the reference trim.
- Fixed (`85b9024a`): a DOS game started from Programs > Games never left
  "Loading DOOM... Preparing...". Bisected to the f3-51 merge: the full-disk
  SHA-256 of the game base at every launch read the 27 MB games disk 512
  bytes at a time with a seek per sector, quadratic in FAT chain walks. Now
  one sequential pass in 64 KiB reads. Regression test
  `test_game_window_base_hash`.
- f3-51c (`abd1c7c1`): a per-open-file FAT cluster cursor (a sequential read
  of the 27 MB games disk: 2,801,342 → 13,295 FAT lookups) and a digest cache
  per base version (`cached=1|0` in the launch record). On QEMU TCG DOOM shows
  its start-up screen within 15 s of the click (60–90 s before) and a relaunch
  within 10 s.
- f0-49b: the pagefile mapper's cluster cursor is validated by the same chain
  stamp; a trailing trim followed by regrowth could otherwise map a cluster
  another file had taken (host test with a negative control).

Evidence status. Everything below was merged after the 0.9.0 record (from
the evening of 2026-10-12, dev diary 2026-10-13-01 onwards). Until
2026-10-19 the delivered images were also run on QEMU (`f0-smoke`, and from
2026-10-14 broader gate campaigns, recorded per image in the diary); the
last campaign ran on image `f56fcbf5…` (2026-10-19): the F0–F4 gate cases
green, with `f4-audio` 7/7 for the first time, and `f5-core` 2/2, except
four known red cases (`vm-fullscreen` on QEMU standard VGA, `quake` probe
deadline under TCG, `rtl8139` `firmware_active`, `volume-resolution-cd-usb`
with two ATAPI units; diary 2026-10-19-02). The owner then suspended the QEMU gates (diary
2026-10-19-03): what was merged after the T23 image `4bcd1d43…` has host
evidence only. Hardware: the owner tried more than twenty images on the
ThinkPad T23 between 13 and 19 October (diary 2026-10-13-02 to
2026-10-19-05; the last image tried is `4bcd1d43…`, findings in directives
f4-36 and f3-47); no image of the current `main` has run on hardware; the
Armada E500 and generic PCs have no recorded run.

### Owner decisions

- 2026-10-13: F5 becomes Windows games through a compatible Win32 API, F8
  optional native ports (diary 2026-10-13-01). Win32 contract drafted,
  decided and cross-reviewed (2026-10-13-03, 2026-10-14-07, 2026-10-14-08):
  core written by CiukiOS with the installer-facing DLLs reused from one
  pinned Wine release, Win16
  inside F5, General MIDI banks chosen by installed RAM, FreeType on every
  text path, DOS full screen on the VGA hardware as in Windows 98, one final
  image.
- 2026-10-14 and 2026-10-18: Ciuki Blue is the default theme; new shell with
  a top bar carrying the Ciuki orb and a bottom bar of open windows; Classic
  98 removed (2026-10-14-06, 2026-10-18-02).
- 2026-10-19: disk, memory, cache and real-time management on the Windows
  98/ME model, complete; QEMU gates suspended, images straight to the
  hardware (2026-10-19-03); resource-management contract accepted with a
  12-directive sequence (2026-10-19-04); CiukiDOS stays, its INT 21h file
  calls go to the VMM, FreeDOS is the conformance reference
  (2026-10-19-06); autonomous programme to completion, F5 extended to
  original installers and Windows applications, no intermediate image
  (2026-10-19-07).

### Hardware trials on the ThinkPad T23 (owner, 13–19 October)

- Method from 2026-10-15 (diary 2026-10-15-06, at the owner's request for a
  definitive solution): no hardware image without a hardware-check page,
  a system volume that repairs itself after an unclean shutdown, and serial
  output in whole records.
- Working on the hardware: native AC'97 audio heard from image `e86305de…`
  (f4-09, f4-10); UDMA2 disk, Terminal (2.5 MB) loaded in 236 ms against
  1.6–1.9 s for 1.3–1.5 MB before (f1-39, f1-44); USB keyboard and mouse
  (f1-42 and follow-ups); USB stick mounted in 58 ms on image `9af46936…`
  (f1-74, f1-81); DOS full screen on the SuperSavage (f3-32, f3-36, f3-39,
  f3-43); the hardware-check page with the Savage engine sweep that chose
  the production configuration (f0-26, f1-69); the serial capture back on
  image `c2d2a237…` once the port was enabled again in the T23 BIOS (f0-38
  named the cause).
- Defects found in the T23 trials, with their fixes merged (the later ones
  not yet tried on hardware): DOOM interrupt stalls and a DOOM VM killed at
  the DPMI IRQ return stub (f3-12, f3-23, f3-45); Quake refused by the DOS
  loader, also seen on QEMU (f3-25); the volume locked after forced
  power-offs and a lost log tail (f0-23, f0-25, f0-36); runtime PIC mask
  writes costing 2.1 ms each (f0-33); firmware input chosen on a normal boot
  (f0-37); shared IRQ 11 quarantined by one member (f0-39); the desktop
  stalled behind USB stick I/O (f1-78, f1-79, f2-78..f2-80); system-wide
  slowdown during copies and stuttering playback (f1-82, f2-84, f2-85, f4-34,
  f4-36); 110 ms stall at every DOS VM creation (f0-41).
- Still open: a periodic pause of about 2.1 ms with interrupts disabled,
  attributed to the firmware (f0-35, f0-46).

### Kernel, drivers and storage

- Latency and logging: serial records through a queue and an
  interrupt-driven transmitter (f0-21, f0-24, f0-29), bounded sections
  (f0-30, f0-44), boot log in three rotated files with protected records
  and a black box in reserved sectors (f0-22, f0-23, f0-25, f0-34),
  hardware-check page (f0-26, f0-28, f0-35), reliability audit with loader
  deadlines (f0-40).
- Storage: 64 KiB reads with READ/WRITE MULTIPLE (f1-39), ATA bus-master
  DMA (f1-44), FAT writes in place (f0-32), FIFO and per-volume locks
  (f1-78, f1-79), ATAPI and ISO 9660 robustness (f1-48, f1-54, f1-58,
  f1-76, f1-77).
- USB 1.1: UHCI/OHCI, hubs, HID with wheel, mass storage, safe removal
  (f1-42, f1-43 and their follow-ups up to f1-81, f2-74); shared PCI
  interrupts (f1-71, f1-72).
- Display: MTRR write combining, display modes, row presents (f1-23, f1-24,
  f1-40), S3 Savage 2D (f1-26 and follow-ups up to f1-69), native
  SuperSavage provider for DOS full screen (f3-32, f3-36, f3-37, f3-39,
  f3-43).
- Audio: seven more audio families (f4-11), AC'97 on the T23's ICH3 and
  CS4297A codec (f4-09, f4-10, f4-15), audio pipeline audit with adaptive
  lead (f4-31, f4-32), ThinkPad EC volume keys (f4-18, f4-21, f4-25, f4-27,
  f4-29), system sounds (f4-06, f4-12).
- Network, ahead of F6: drivers for Intel 8255x, RTL8139, NE2000 PCI,
  e1000, PCnet, Tulip, 3c90x and Rhine (f6-01, f6-08, f6-09), `/dev/net` and
  the tray icon (f6-02), NDIS miniport census (f6-03), contract draft
  (f6-00).

### Windows 98/ME-class resource management (host evidence)

Contract `docs/design/win98-class-resource-management.md` (f0-42, research
closed with the Windows 98 Resource Kit in f0-42b); diary 2026-10-19-04,
-08, -09 and the directive index.

- Cache and I/O: writeback by dirty budget, C1 (f1-82, on image
  `4bcd1d43…`); asynchronous request pool and per-controller queues with
  reader fairness, C3 (f1-83); staged FAT publication with byte-range and
  share locks, C2/C5 (f1-84); adaptive cache with stream read-ahead, C4
  (f1-85).
- Memory: one ledger with pressure watermarks and pin tokens, M1 (f0-43);
  memory objects with demand-zero and copy-on-write pages, M2 (f0-47);
  global second-chance replacement, M3 (f0-48); dynamic swap file
  `C:\SYSTEM\PAGEFILE.SWP` with idle prewrite, M4 (f0-49, merged
  2026-10-20).
- Real time: bounded loops, R1 (f0-44); 32 priority levels with boosts and
  priority donation, R2 (f0-45); capture-only interrupt top halves, deferred
  work ring and AVL timer queue, R3 (f0-46).
- Not yet: pageable DOS VM memory, D1 (f3-46, in progress).

### DOS virtual machines

- Full screen on the VGA hardware with Alt+Enter (f3-20 and follow-ups);
  Windows 9x-style idle detection (f3-17); cheaper port and interrupt exits
  and planar presentation (f3-14, f3-19, f3-33, f3-38, f3-41); DPMI return
  tracker with breakpoints on hardware (f3-34, f3-35); virtual IF and ISR
  bookkeeping (f3-15, f3-42, f3-44, f3-45); Sound Blaster DMA timing and late
  frames (f3-40, f4-35); per-VM disk cache and 48 MiB default game VM
  (f3-47).
- Quake 1.06 shareware pre-installed next to DOOM and Wolfenstein 3D
  (f3-18).
- DOS file API in the VMM (diary 2026-10-19-06, -09): contract and
  inventory (f3-48), INT 21h file and directory calls served by the VMM
  (f3-49, f3-49b), CiukiDOS binding with FCB and NLS services (f3-50),
  FreeDOS-referenced conformance battery (f3-51, f3-51b). Host evidence;
  the battery's QEMU and T23 runs are pending.

### Desktop and applications

- Shell: desktop as a folder (f2-42), redraw and compositor work (f1-25,
  f2-45, f2-48, f2-50), FreeType text (f2-63), message boxes, notifications
  and tooltips (f2-64), resizing from every edge, cursors and window rules
  (f2-67, f2-72), text selection (f2-68), theme and wallpaper workers
  (f2-70, f2-73), Control Panel catalog with Display Properties (f2-37,
  f2-75), AutoPlay and safe removal (f2-51, f2-74, f2-76, f2-78, f2-79),
  two-bar shell with the Ciuki orb (f2-81, f2-82, f2-83).
- Programs: Terminal (f2-46), Registry and Registry Editor (f2-47), Music
  (f4-07, f4-30, f4-34, f4-36), CiukPaint (f2-40), Graphics Demo on a
  software OpenGL subset (f2-52, f2-59, f7-01), CiukIDE with the Tiny C
  Compiler (f5-01), Device Manager with a hardware ID database built from
  the owner's INF collection (f2-41, f2-43), Files on worker threads with
  progress dialogs (f2-80, f2-84, f2-85).

### F5 — Win32 personality (diary 2026-10-19-10)

- Merged, host evidence except f5-03: tooling and census (f5-02A, f5-14A,
  f5-17A), kernel personality core (f5-03; QEMU `f5-core` 2/2 with the TCG
  #XM fallback, diary 2026-10-15-02), kernel objects and sections (f5-04),
  PE loader and ntdll (f5-05A/B), kernel32 (f5-06, f5-06b), w32srv and
  registry (f5-07A), presenter (f5-08), desktop protocol v3 (f5-09,
  f5-09B), user32 (f5-10A), gdi32 (f5-10B), DirectDraw (f5-11),
  DirectSound, waveOut and MIDI (f5-12A, partial), DirectInput (f5-13), Wine
  10.0 upper DLL and codec builds (f5-14B, f5-14C), shims and crash reports
  (f5-16), Win16 contract and NE parser (f5-19, f5-19a).
- Merged 2026-10-20 (host tests only): f5-10C (dialog manager, classic
  controls, menus and accelerators, MessageBox, TrueType font selection) and
  f5-20A (184 kernel32/ntdll/advapi32/version exports the reused Wine DLLs
  import). In progress: f5-07B, f5-10C2, f5-12B, f5-15A/B, f5-17B, f5-19
  (Win16), f5-20B/C. No original Windows game, installer or application has
  run yet.

### Tests and tools

- Runner with two QEMU slots (runner-01), USB devices and input pacing
  (runner-02..runner-04); QEMU deviations recorded: no #XM under TCG
  (2026-10-15-02), QEMU 11.1.1 TCG crashing on guest execute breakpoints
  (2026-10-16-02).
- 3D: contract draft for native Savage 3D (f7-00), review pending.

## 0.9.0 pre-alpha (not released, `main`, 2026-10-09 to 2026-10-19)

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
  (`0.9.2`, `pre-alpha`, previously `0.9.0`). The kernel logs `CiukiOS <version>` after its build
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
- Desktop (f2-08, f2-30): taskbar with clock, Ciuki menu built from `/Programs`
  (`/bin` until f2-35, see 2026-10-12 below),
  minimise, window protocol v2 (close requests), About window with the
  approved Ciuki portrait, Tango icons (`desktop-shell-qemu-t23/e500` pass).
- Applications on the `libdeskui` toolkit (f2-31, f2-32, f2-33, merged
  2026-10-11, commit `1241de1`): Files, CiukNote (`/Programs/ciuknote`) and Task
  Manager (`/Programs/tasks`, using calls 70/71 and protocol v2 opcodes 15–20
  restricted to the launched Task Manager). QEMU evidence is pending at the
  time of this entry: their cases (`files-qemu-t23`, `ciuknote-qemu-t23`,
  `tasks-qemu-t23`) have not yet run on an integrated image. Update
  2026-10-12: `files-qemu-t23` ran on image `2fdd515a…` and failed with
  `keys=0` (cause and fix in the 2026-10-12 section); the other two have no
  recorded run.
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
  the `/Programs/dosbox` window and call 72, probes `vm-video` and `vm-window`.
- Evidence on QEMU: image `fcd96d6e…` is the first on which DOS VMs boot:
  `f3-vm` 195 PASS (F0–F2 prefix, then `vm-boot` and `vm-memory` on
  `qemu-t23`, `qemu-e500` and `qemu-min128`), `f0-smoke` 2/2, `f2-all` 189
  PASS (the three `uart-absent-*` operator cases excepted). `vm-devices`
  passes on those three profiles on a worktree image; `vm-devices` and
  `vm-video` pass on the three profiles of the integrated image `e0af9210…`.
- Kernel-owned DPMI 0.9 host (f3-04: six stages, LDT at GDT index 8,
  INT 2Fh AX=1687h entry, DPMI16/DPMI32 conformance clients and a DOS/4GW
  client built with Open Watcom; `f3:dpmi` cases) and per-VM Sound Blaster
  16, OPL3 and 8237 DMA state models with the F4 consumer interface (f3-05;
  `f3:vm-audio-state` cases): merged with host evidence (327 host tests,
  libx86emu guest runner DPMI16/DPMI32/DOS4GW PASS); on QEMU `vm-audio-state`
  passes on the three profiles (image `e0af9210…`) and `dpmi` passes on the
  three profiles (image `d70cf673…`, with the TCG data-limit rule of
  2026-10-12). DOS workloads (f3-06): DOS/4GW probe loops, DOOM
  timedemo and Wolfenstein 3D from local, never redistributed game files on
  a per-run fixture disk, a starvation gate, protected-mode VGA access for
  DOS/4GW programs and a V86 monitor that traps only at shadow and IRQ
  boundaries; DOOM completes its timedemo in the host guest runner and on
  QEMU (see 2026-10-12 below). F3 has no recorded hardware run.

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
  `f4:ntfs-ro` with 12 `f4-media` cases, all passing on QEMU (image
  `e0af9210…`, scratch suite `zz-media`, 18 cases with the optical ones).
- DOS VM audio path (f4-05): every DOS VM owns a mixer source; SB16 PCM is
  converted at the programmed rate and OPL3 is synthesised by an integer C
  port of the DOSBox DBOPL (GPL-2.0-or-later), paced 56 ms behind the VM
  clock with CT1745 gains; `f4:vm-audio` cases pass on the three QEMU
  profiles after the 2026-10-12 idle-tail fixes (image `d70cf673…`).
- Not in this version: ESS Maestro-2E backend and 2D acceleration
  (deferred by the contract).

### 2026-10-12 — desktop shell, applications, DOS games, retro namespace

Integrated on branch `wt/integ2` (night of 2026-10-11/12 and the day), toward
a usable pre-alpha. Directives: f2-30 to f2-36, f3-01 to f3-08, f4-04, f4-05.
Evidence per image is in
`docs/validation/2026-10-11-integrated-image-f0-regression.md`
("2026-10-12 integration images"); the newest image, `8435a16a…`
(commit `96027a71`), has had no QEMU campaign.

Desktop and UI

- Quiet normal boot (`docs/boot.md`): the loader shows one hint line, the
  kernel draws the original Ciuki photograph (deterministic 512x384 RGB565
  conversion, pinned) and keeps the console dark until the desktop; the
  console returns on a panic, a lost desktop or after 120 s. System call 73
  `ciuki_power` reboots or powers off through ACPI S5 (storage synced first,
  trusted desktop only). `normal-boot` cases in `f0-smoke` check the path.
- Desktop shell (f2-30 follow-up): the Ciuki (Start) button at the left end of
  the top bar (rounded gradient face, approved portrait) opens Programs,
  Documents, Settings, Help, Run... and Shut Down...; taskbar with window
  buttons and a tray (volume popup, clock); desktop icons with context menus,
  readable labels (soft halo, translucent selection); Run dialog, Alt+Tab,
  Alt+F4, About with the dedication. The tagline is
  `Classic computing, one woof at a time.` (owner decision), shown in About.
- Looks: theme engine with Platinum (default), Ciuki Blue and Classic 98;
  Inter and Inconsolata UI fonts rasterised from the OFL sources; one frame
  painter gives every window and dialog the same 24 px title bar.
- Window resizing (f2-34): borders, corner and zoom, protocol v2.1 with
  client-allocated replacement surfaces (no kernel or ABI change) and toolkit
  relayout in the applications.
- Icons: the full Tango 0.8.90 store (`/system/icons/tango.cis`, 246 names at
  16/22/32/48 px, pinned hashes) with one size per place (menus 16, Files
  toolbar 22, dialogs and built-in windows 32, desktop 48) and distinct icons
  per program.
- Wallpapers: six procedural wallpapers and PNG/JPEG/BMP files (stb_image
  2.30) from `/system/wallpapers`, including the owner's three Ciuki scenes
  (Ciuk2 in `fill` position is the default); positions Center, Tile, Stretch,
  Fit and Fill; image decoding is deferred so the first desktop frame and the
  Control Panel preview do not stall under TCG.

Applications

- Files (f2-31, f2-36): a "Computer" view with the drives `C:`, `D:`... from
  `/mnt`, drive-letter paths (`C:\Programs`), Places Computer, Programs,
  Documents, System and Games; `dev`, `mnt` and `tmp` are hidden in `C:\`.
- Control Panel (Display with theme, wallpaper, position and desktop colour;
  Mouse; Keyboard with the Italian layout; Sounds; Date & Time; System) over
  `desktop.cfg`; accessories recovered from 0.8 (Calculator, Character Map,
  Clock, CiukiOS Help, Volume Control, Image Viewer) and a recent-documents
  list (`docs/design/legacy-0.8-feature-inventory.md`).
- Namespace (f2-35, owner decision): programs keep the POSIX view over the
  drives, with `/bin` renamed `/Programs` and `/home` renamed `/Documents`;
  `System` and `Games` are created with long names; amendment in
  `docs/design/posix-subset.md`.

DOS and virtual machines

- DOOM 1.9 and Wolfenstein 3D 1.4 shareware are pre-installed on
  `C:\GAMES\CIUKIGAM.IMG` (`third_party/games` with their licence texts,
  `scripts/build_games_disk.py`, Programs > Games, `dosbox --name`); every DOS
  window boots that disk with the kernel DPMI host. Registered game data is
  refused by the builder. The Wolfenstein 3D bundling clause of
  `VENDOR.DOC` is an open owner decision.
- DOS window and games: INT 16h wake-up of the CIUKIDOS shell, workload-disk
  lookup without a 20 KiB heap request (ENOMEM), VGA input status 1 on the
  programmed vertical timing, burst emulation of VGA stores (Wolfenstein 3D
  scalers from about 11-15k faults per frame to 76-152), direct linear window
  for chained mode 13h including DPMI clients (f3-07), virtual IF restored on
  POPF/IRET in the DPMI host, and the TCG data-limit rule for the DPMI clients.
- DOS VM audio (f4-05 follow-up): with a VM source attached the idle tail
  keeps 8 periods, and a drained tail stops the transport normally.
- Kernel review fixes (f3-08, from a read-only Codex review): DPMI virtual-IF
  trap keyed on DR6.BS with one-shot arming, PUSHF/POP detection decoded
  forward, VGA burst preflight and refused LOCK forms, `ciuki_power` retrying
  a failed sync, `_S5` parsing with PkgLength, SCI_EN required before PM1
  writes, audio tail stops only on proven silence.

Build and test

- Runner: QMP replies awaited up to 10 s (a late reply is a timeout), five
  seconds of reaping grace, optical fixtures as an explicit `ide-cd`,
  repeated `query-status` exchanges not logged, `-d nochain` with `-icount`
  (QEMU 10.2-11.1 crash under self-modifying DOS games, recorded as a
  deviation), `normal-boot` screen heuristics updated for the new desktop
  (Ciuki button in the top bar, wallpaper work area). Desktop gate
  scenarios hold their input until the launched program shows its first
  frame (root cause of the `keys=0` failures of `files-qemu-t23` and
  `desktop-shell-qemu-e500`); the runner names the failing contract. Scratch
  suites stay untracked.
- Host suites on `96027a71`: desktop 56 OK, normal-boot 14 OK, gate 10 OK,
  kernel host tests OK, runner 85 OK (2 skipped).
- QEMU (TCG, one image per result): image `2fdd515a…` `f0-smoke` 4 PASS,
  DOOM, DOOM with sound and Wolfenstein 3D PASS, `desktop-shell-qemu-t23`
  PASS, `files-qemu-t23` FAIL (`keys=0`, fixed on the host in `96027a71`, not
  re-run); image `5dd760ee…` DOOM PASS with f3-08. Not run: the campaign of
  `8435a16a…`. QEMU 11.1 itself crashed three times in TB invalidation
  during DOS workloads (deviation recorded).
- Hardware image for the T23: `build/hw/ciukios-hw.img`, SHA-256
  `893a83e0f24b515b1daa24d06da8fb22fc2c434a805d7b48343624d06bdc3421`
  (`BOOT.CFG` `safe=0 serial=1`), handed to the owner for a direct test; no
  hardware result is recorded for it.

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
This changelog is intentionally concise. Every completed task should update the topmost unreleased section (`0.9.2 pre-alpha`) unless the task cuts a release section.

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
