# CiukiOS 1.0 Alpha Release Notes

- **Release:** 1.0 Alpha (unreleased, `main`; latest image `2287114d` of 2026-10-25, T23 trial pending)
- **Date:** 2026-10-25 (first written 2026-10-21; updated with the trials and fixes of 2026-10-21/22 and the image of 2026-10-25)
- **Target Architecture:** i686 PC (Pentium III baseline, AC'97 audio, IDE/ATA, VESA VBE 2.0+, PS/2 / USB HID)
- **Primary Hardware Twin:** IBM ThinkPad T23 (1.13 GHz Pentium III-M, 512 MiB RAM, S3 SuperSavage IX/C, Intel 82801CA ICH3 AC'97)
- **Canonical Image:** 512 MiB FAT32 disk image (`build/f0/ciukios.img`, or `build/hw/ciukios-hw.img`)
- **Setup Media Note:** Setup CD (f9) is deferred to a later release (owner decision 2026-10-21 20:29, dev diary 2026-10-21-07); the 1.0 Alpha ships as a direct disk image written to the target disk.
- **Qualification Status:** Five alpha images (`8f59e221`, `28805270`, `b453bc92`, `5a10eef4`, `a4d9454e`) were tried on the ThinkPad T23 on 2026-10-21/22 and none has passed; see section 2.2. Hardware qualification is open (`TODO(lead)`).

---

## 1. Overview and Contents

CiukiOS 1.0 Alpha marks the completion of the core foundations transition, bringing together Windows 98/ME-class resource management, a unified DOS subsystem, native Win32 application and installer support, full TCP/IP networking with a native graphical web browser, and desktop multimedia capabilities.

### 1.1 Kernel and Resource Management
- **Windows 98/ME Resource Management Model:** Full implementation of the sequence covering Memory (M1–M4), Real-Time (R1–R3), Storage Cache & Concurrency (C1–C5), and DOS Memory (D1):
  - Unified memory ledger with commit accounting and watermark hysteresis.
  - Multi-level scheduler (32 priority levels) with dynamic thread boosting, I/O completion bonuses, starvation prevention, and transitive mutex donation.
  - Bounded teardown and allocation loops with preemption points across the kernel.
  - Capture-only top-half interrupt handlers with deferred work queues and firmware attribution.
- **Crash-Resilient Pagefile (`PAGEFILE.SWP`):** Persistent swap extents preserved across boots with metadata-only extent growth (f0-49c), eliminating synchronous 16 MiB zero-fills, decoupling desktop startup from the pager manager, and preventing boot hangs under instruction-counting runtimes.
- **Hardware Entropy Subsystem:** Kernel entropy pool based on pinned `jitterentropy-library 3.7.0` coupled with a ChaCha20 DRBG (f6-16), exposing system call 91 (`getentropy`) and C SDK interfaces (`getentropy`, `arc4random`) with finite deadlines, as well as `entropyraw=1` capture mode.
- **Kernel Contention Telemetry:** Instrumentation for tracking volume reservation holds (`vfs_hold`/`vfs_wait` reporting holder task, operation, waiters, and hold duration), per-command ATA DMA/PIO service timings, serial VM exit profiling, and unmasked IRQ-off durations (f1-86).

### 1.2 Storage Subsystem
- **Adaptive ATA Queue Management:** Adaptive command-depth logic (`blkreq.c`, `7c5b9878`) bounded by a 4,096-byte (or device sector size) minimum floor, recovering back to the 64 KiB maximum queue depth upon rapid completions to prevent command starvation on slow disks.
- **Fast FAT Chain Lookups:** Per-open-file FAT cluster cursor with chain-stamp validation (f3-51c), reducing sequential 27 MB image reads from 2,801,342 to 13,295 FAT lookups, combined with an LRU cache of 16 base digests indexed by volume version.
- **2D acceleration indicator (f1-97, f2-92):** the splash, the tray notice and the log state `2D acceleration: YES` or `2D engine: NO (<reason>)`; `2D engine: OK (drawing: software)` until the engine has drawn; `accel=0` disables it.
- **S3 SuperSavage BCI Acceleration Diagnostics:** S3 SuperSavage BCI self-test sequence updated with CR50 depth selection preceding descriptor writes, incoming CR50 buffering, completion fence, and an `accel=0` boot configuration token for safe hardware fallback (f1-88).

### 1.3 Unified DOS Subsystem
- **Unified Kernel Architecture:** Adoption of the unified DOS contract (f3-54) combining the FreeDOS 1.3 kernel base with the CiukiDOS identity, banner, and prompt:
  - Pinned FreeDOS 2043 kernel fork built natively and reproducibly with Open Watcom 1.9 (patch 0001, SHA `d24464ec…`; f3-55), with the native DFV2 binding and `CIUKIDF.SYS` (f3-56, f3-57).
  - **Unified only (f3-63, owner decision of 22 October):** games and the DOS Box shell use the qualified unified kernel. Classic CiukiDOS and the pristine FreeDOS reference are absent from the canonical image; the reference remains in the host harness. DOS Box has no settings panel or configuration writer. The earlier f3-61 IFS/game evidence remains diagnostic; the f3-63 launch-time and CPU targets require a new qualified image and T23 measurements.
  - VMM integration via the network redirector interface (INT 2Fh AH=11h), serving DOS INT 21h file calls from the native VFS as in the Windows 9x IFS Manager.
- **Pageable DOS Memory Pool:** 24 MiB VM memory pool converted to a pageable object (Contract D1, f3-46), reducing VM creation latency from 134.3 ms to 23.6 ms with protected I/O fault handling.
- **Event-Driven Frame Synchronization:** DOS window rendering converted from 2 ms polling to blocking event-driven frame waits (`WAIT_PRESENT`, f3-52), donating priority to the VM thread while waiting and capping loading panel repaints to <=4 Hz to prevent VM thread starvation.
- **Fast Game Loading:** DOS game launch base hashing optimized into a single sequential pass with 64 KiB reads (`85b9024a`, `b2bc10a7`), reducing host hashing elapsed time from 29.2 s to 0.25 s.
- **DOS Virtual Networking:** Virtual NE2000 network adapter (DP8390 model) and Ethernet II IPX bridge implemented over the native netif and packet pool (f6-11), providing VMM IPX API emulation with DPMI routing, ESR delivery, and `NETID.BIN`.
- **DOS windows (2026-10-25):** `exit` at the prompt closes the window (the guest shell is `CIUKISH.COM`, a non-permanent COMMAND.COM); a program that ends within 10 s leaves its window open as "Finished - <name>"; closing a window no longer hangs the system; the VMM answers the XMS installation check so 575 KB of conventional memory are free at the prompt and Wolfenstein 3-D starts; DIR reports the correct free space.
- **DOS Title Conformance Battery:** Automated DOS title battery test harness and manifest schema established covering tracked titles across bundled, test-media, and owner-copy classes (f3-53).

### 1.4 Win32 Subsystem and Installers
- **Win32 Subsystem Core:** Native Win32 subsystem built on the desktop v3 windowing protocol, featuring 347 implemented exports across `kernel32`, `ntdll`, `advapi32`, `user32`, and `gdi32` (f5-20a, f5-20b, f5-20c).
- **Upper DLL Staging:** 16 reused Wine upper DLLs (`shell32`, `shlwapi`, `comctl32`, `commdlg`, `msvcrt`, `ole32`, `oleaut32`, `version`, `lz32`, etc.) qualified and initializing with `DllMain` returning TRUE (f5-20c), built and staged via `make build-full` (f5-20d).
- **Dialogs and Controls:** Dialog manager, standard controls, menus, accelerators, `MessageBox`, and TrueType font rasterization via FreeType in `w32srv` (f5-10c, f5-10c2, f5-10c4).
- **Session Lifecycle and Virtual Restart:** PROGMAN DDE service, session termination messages (`WM_QUERYENDSESSION`/`WM_ENDSESSION`), and virtual restart handling via `WININIT.INI` and `RunOnce`/`RunServicesOnce` keys with `runonce.exe` (f5-07b, f5-07d).
- **Service Traffic Optimization:** Win32 service idle traffic reduction through persistent `ntdll` connection handles, zero-allocation message dispatch during active sessions, and asynchronous background polling (f5-07c).
- **Installer Foundations:** Shell integration providing standard shell folders, `.lnk` shortcut creation, `SHFileOperation`, `ShellExecuteEx`, temporary file staging, and delayed rename processing via `WININIT.INI [rename]` (f5-15a).

### 1.5 Network and Web Browser
- **TCP/IP Network Stack:** lwIP 2.2.1 imported with native `sys_arch` integration, supporting native socket system calls 78–89, DHCP client, DNS resolver, and C SDK socket headers (f6-10).
- **NetSurf on the 2026-10-25 image:** JavaScript is off by default on the target (the script engine faults on a null object when running example.com's script); enable it in NetSurf's options. Ctrl+L no longer corrupts the address, large images display, and remote pages load in QEMU.
- **Print spooler:** event-driven (about 0.3 % CPU), with a 60 s safety rescan and a 15 s retry for offline jobs.
- **Native NetSurf 3.11 Port:**
  - NetSurf Milestone 1 (M1) frontend supporting native window framebuffer rendering (`libnsfb`) and offline HTML/data navigation (f6-14).
  - NetSurf Milestone 2 (M2) HTTP/1.1 fetcher with asynchronous DNS resolution, BearSSL TLS 1.2, cookie support, and offline error classification (f6-15).
  - Per-connection TLS entropy seeding from the kernel via `getentropy`, integrated with an internal browser readiness page, error reload handling, and HTTPS test suites (f6-17).

### 1.6 Desktop, Graphics, and Multimedia
- **Responsive Desktop UI:** Desktop shell with top bar (Ciuki orb) and bottom taskbar (`libdeskui`), dithered gradients, anti-aliased task buttons, and event-driven timer updates with >=10 ms idle sleep (f2-86).
- **Add/Remove Programs:** Control Panel applet managing uninstallation across Win32, DOS, native, and bundled game packages via `GAME_QUERY`/`GAME_REMOVE` interfaces (f2-87).
- **3D Graphics Support:** Hardware-independent OpenGL 1.1 pipeline via `opengl32.dll` and WGL bindings implemented over the `sdk/libgl` software rasterizer (f7-07).
- **Multimedia Control Interface (MCI):** Subsystem supporting `waveaudio` (PCM/ADPCM), MIDI sequencer (TinySoundFont GM synthesizer), `avivideo` (AVI playback via `mciavi32.dll` and DrawDib), and digital CD audio reading (READ CD) with analog fallback (f5-12b, f5-12b2).
- **CD Player Application:** Audio CD player installed to `C:\WINDOWS\CDPLAYER.EXE` featuring the Tango `media-optical` icon and Start Menu integration (f5-22).

### 1.7 Compact Boot Loader
- **Loader Memory Optimization:** Compact shared bounded matcher for boot options and selectors, streaming ELF program headers through the E820 scratch buffer (f0-54), freeing 1,303 bytes within the fixed 64 KiB loader extent with zero differential deviation across 60,102 tested selectors.

### 1.8 Scope Deferrals
- **Setup CD Installer:** Deferred to a later release (owner decision 2026-10-21 20:29, dev diary 2026-10-21-07).
- **Win16 Subsystem 32-Bit Segments:** Deferred until after 1.0 Beta (owner decision 2026-10-20-08).

---

## 2. Known Issues from the ThinkPad T23 Trial (Image `b51f034f`)

During the hardware trial of image `b51f034f04aee486…` on the IBM ThinkPad T23 (dev diary 2026-10-21-02), several behavioral defects and hardware telemetry measurements were recorded:

### 2.1 Summary of Trial Findings and Resolutions

| Defect / Measurement | Hardware Symptoms on T23 | Root Cause / Analysis | Status & Addressed By |
| --- | --- | --- | --- |
| **Unbounded Volume Holds on C:** | Opening or moving any window causes visible lag and system instability. | 513 `vfs_hold drive=C task=desktop op=other` records (406 spanning 50–200 ms, 91 spanning 200–500 ms, 16 exceeding 500 ms up to 1,502 ms). `storage-writer op=log_write` averaged 1,633 ms (max 1,837 ms). | **Addressed by authored work** (directives `f1-87`, `f1-89`, `f1-90`: bounded logical holds, log write chunking, priority reader path). Verification on hardware pending (`TODO(lead)`). |
| **Audio Refill Starvation** | Music playback suffers audible lag spikes when moving or opening windows. | `audio-refill` exceeded its budget in 192 of 201 windows; audio refill starved in queue behind long volume reservations while transport remained healthy (`lead_periods=8`). | **Addressed by merged/authored work** (directive `f1-90` exempts audio refill from budget overrun penalties and routes it via the C3 reader priority path; `f4-34` >=2 s decoded buffer). Verification on hardware pending (`TODO(lead)`). |
| **Windowed DOS VM Lag & Audio Distortion** | DOS games stutter and drop audio in windowed mode, but run smoothly in full-screen mode; DOOM audio slightly distorted. | Windowed VM descheduled or parked when SB16 IRQ was due (latency 33–476 ms, underrun count up to 1,229); tight coupling to the desktop presenter; 129 ms un-preemptible span in `dosvm_pool.c:239` (`resolve`). | **Addressed by authored work** (directive `f1-87`: VM decoupled from presenter, frame wait bounded, `resolve` capped at 5 ms, independent audio cadence). Verification on hardware pending (`TODO(lead)`). |
| **High PIT & OPL VM Exits** | CPU overhead of ~5% attributed entirely to I/O port trap dispatch during DOS VM execution. | 593,816 exits on port 0040 (10.4 s), 296,908 on port 0043 (7.4 s), and 230,000 on OPL ports 0388/0389 (3.7 s). | **Addressed by authored work** (directive `f1-87`: fast-path dispatch for ports 0040, 0043, 0388, and 0389). Verification on hardware pending (`TODO(lead)`). |
| **Unattributed 2.1 ms Interrupt-Off Pauses** | Periodic system stalls of ~2.1 ms occurring ~3.9 times per second. | 1,554 sections with interrupts disabled (`firmware_max_us=2117`), matching the UHCI USB host controller scan rate (~3.98 scans/sec). | **Open investigation** (directive `f1-87`: test option `usbscan=0` provided for T23 trial to isolate hardware SMI vs. USB host controller interaction). |
| **S3 SuperSavage BCI Self-Test Failure** | 2D acceleration driver placed in quarantine (`init_result=-5`); desktop falls back to software composition. | SuperSavage BCI self-test readback fails on real hardware due to CR50 register depth setup ordering. | **Addressed by merged work** (directive `f1-88`: CR50 depth selection ordering before descriptor write, incoming CR50 buffering, completion fence, and `accel=0` safe mode fallback token). Hardware verification pending (`TODO(lead)`). |

### 2.2 Trials of 2026-10-21/22 (images `8f59e221` to `a4d9454e`)

Later images (24 and 25 October, `56e58374` stopped at the splash on the T23; `2287114d` fixes it and the DOS window hang, dev diary 2026-10-25-01) have only QEMU and host evidence; the T23 trial is pending.

| Image | Result (owner) | Resolution |
| --- | --- | --- |
| `8f59e221` (first alpha) | NetSurf does not open; windows stutter; any DOS application or game puts the system in protection with an I/O error; USB copy very slow; Display Properties "not responding" (dev diary 2026-10-21-10) | Second image `28805270` (2026-10-22-01): wallpaper read in one pass, popup faces, DOS and I/O fixes |
| `28805270` / `b453bc92` (third: splash bar fixed, f0-55b) | Rejected: slow window opening (no 2D acceleration visible), unified DOS does not work, NetSurf exits, Display Properties not responding, music worse (2026-10-22-03) | Fourth image `5a10eef4`: Savage fill repair (f1-95), IFS connected (f3-61), priority inheritance and 25 % desktop share during a game (f1-98), USB 64 KiB transfers (f1-96), NetSurf local pages exempt from the layout budget (f6-20), 2D indicator (f1-97) |
| `5a10eef4` | Residual instability, stutter, NetSurf without internet, "I/O error and protection", USB stick removed by the system (2026-10-22-05) | Fifth image `a4d9454e`: SMI blackouts traced to the EC event line routed on SMI# and the poll turned off (f1-100), USB stale TD state (f1-96b), the desktop draws with the 2D engine (f2-92), e100 receive filter fixed so DHCP works (f6-22) |
| `a4d9454e` | Stutter of windows and music unchanged, browser cannot be interacted with, two cursors, hardware volume keys have no effect, USB copy 149 KB/s (2026-10-22-07) | Open: the T23 log shows the SMI blackouts gone (`irqoff_long` 2 per boot, `ec_poll=off`), DHCP up, and the CPU saturated by the desktop (about 40 composed frames per second with one player open). Directives f2-95 (compositor rebuilt on the Windows 98/X11 region model with X11 regions and pixman, MIT), f1-101 (audio producer priority) and f2-96 (one cursor, EC poll every 2 s) |

The next image is built only with the desktop CPU budget demonstrated on the model and those three points green.

---

## 3. How to Write the Image to Disk

To deploy the CiukiOS 1.0 Alpha image to physical hardware, follow the verified physical installation procedure documented in [docs/build-and-run.md](build-and-run.md#physical-machines):

### 3.1 Target Image
The canonical target image is:
```
build/f0/ciukios.img
```
(or the designated hardware staging image `build/hw/ciukios-hw.img`).

### 3.2 Write Command
Execute the physical write script using an expendable destination disk identified by its stable device path:
```bash
sudo scripts/test/write_physical.sh /dev/disk/by-id/<stable-name> [image]
```

### 3.3 Safety Safeguards and Verification Procedure
The `write_physical.sh` script enforces the following validation steps:
1. **Target Disk Protection:** Automatically rejects the host root disk, mounted filesystems, and individual partitions.
2. **Serial Number Confirmation:** Prompts the operator to type the target disk's exact physical serial number before proceeding.
3. **Raw Write:** Transfers the full 512 MiB image to the disk using `dd`.
4. **Readback Verification:** Reads the entire written extent back from the physical disk and computes its SHA-256 hash.
5. **Hash Matching:** Compares the readback SHA-256 against the source image hash. The write is accepted as valid evidence only when the hashes match byte-for-byte.
6. **Provenance Logging:** Generates a structured `acquisition.json` file alongside the image, recording the target disk identity, source hash, readback hash, and timestamp.

### 3.4 Capturing Serial Diagnostics on ThinkPad T23
To record diagnostic boot logs from the ThinkPad T23 during physical testing:
1. Connect an RS-232 null-modem cable from the laptop's serial port to a USB serial adapter on the workstation.
2. Verify workstation serial port settings: COM1 operates at `38400 8N1`.
3. Launch the capture utility prior to powering on the machine:
   ```bash
   scripts/test/serial_capture.sh <run-id>
   ```
4. Output is captured to `legacy/local/physical/<run-id>/serial.log`.
5. **Important:** Do not press the `L` key at the loader boot menu, as this disables serial logging in the kernel.

---

## 4. Licence Summary

CiukiOS 1.0 Alpha adheres to strict licensing boundaries, source code publication obligations, and intellectual property requirements:

### 4.1 Unified DOS Kernel and Ported CiukiDOS Code
- **License:** **GNU General Public License, version 2 or (at your option) any later version (GPL-2.0-or-later)**.
- **Grant:** Pursuant to the formal grant recorded on 2026-10-21 in `docs/licenses/CiukiDOS-unified-kernel-GRANT.md` (dev diary 2026-10-21-04), the owner of CiukiOS has licensed all portions of CiukiDOS (`src/boot/floppy_stage1.asm` and derived modules) ported into the unified DOS kernel fork under GPL-2.0-or-later.
- **Scope:** Applies specifically to code incorporated into the FreeDOS 1.3 kernel fork and CiukiOS-specific DOS drivers/binaries built from that tree. CiukiDOS code remaining outside the fork retains its existing project license.
- **Source Code Availability:** Corresponding source code for the unified DOS kernel (including the fork, patches, build scripts, pins, and license notices) is published in the CiukiOS GitHub repository. Every release containing the unified kernel provides an accompanying source archive whose SHA-256 hash is recorded in the release manifest.

### 4.2 System Icon Theme
- **Artwork Family:** **Tango Icon Theme 0.8.90**.
- **License:** **Public Domain** (`assets/icons/README.md`, upstream `COPYING`).
- **Authors:** Ulisse Perusin, Steven Garrity, Lapo Calamandrei, Ryan Collier, Rodney Dawes, Andreas Nilsson, Tuomas Kuosmanen, Garrett LeSage, and Jakub Steiner.
- **Ciuki Portrait:** The OS mascot portrait of Ciuki is a proprietary project asset and is not part of Tango's Public Domain release. It must not be replaced, redrawn, or substituted.

### 4.3 Third-Party Stack Components
The full list (version, licence, notice path and status of every component shipped or used to build the image) is [docs/credits.md](credits.md); the same entries are shown in About CiukiOS > Credits, and `tests/host/test_credits.py` checks them against the pins. The CiukiOS code itself is GPL-2.0 (`LICENSE`), the SDK MIT (`sdk/LICENSE`). Highlights:
- **lwIP 2.2.1 TCP/IP Stack:** Licensed under the **BSD-3-Clause** license (`docs/licenses/lwip-2.2.1-BSD-3-Clause.txt`).
- **Jitterentropy Library 3.7.0:** Dual-licensed under **BSD-3-Clause** and **GPL-2.0** (`docs/licenses/jitterentropy-3.7.0-LICENSE.bsd`, `docs/licenses/jitterentropy-3.7.0-LICENSE.gplv2`).
- **pixman and the X11 region code (being integrated by f2-95):** MIT; the notice is added with `third_party/pixman`.
- **NetSurf 3.11:** GPL-2.0 with MIT libraries, zlib 1.3.1, libpng 1.6.59 (`config/netsurf.json`); BearSSL 0.6 (MIT).
- **Wine Upper DLLs:** Reused Wine 10.0 PE binaries are built from source and distributed under the **GNU Lesser General Public License, version 2.1 or later (LGPL-2.1-or-later)**, with corresponding source code offers staged in `third_party/wine/`.

### 4.4 Commercial Payload Policy
- **No Commercial Payloads:** The canonical operating system image contains strictly **no local commercial game payloads**.
- **Bundled Software:** Pre-installed titles are restricted to public shareware or freeware distributions (e.g., Wolfenstein 3D shareware per owner decision 2026-10-21-05 and Apogee `VENDOR.DOC`). All private commercial test workloads remain strictly excluded from release media and tracked git repositories.

---

## 5. Verification and Open Items

- Canonical image SHA-256: `TODO(lead)`
- ThinkPad T23 physical qualification log: `TODO(lead)`
- Second physical baseline (assembled 1998–2002 PC) qualification: `TODO(lead)`
