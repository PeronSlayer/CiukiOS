# CiukiOS roadmap

**Current line: Ciuki VMM on `main`, version 0.9.2 pre-alpha (20 October
2026). Steps F0–F4 are merged and the last QEMU campaign (image
`f56fcbf5…`, 2026-10-19) passed their gate cases except four known red
cases; the Windows
98/ME-class resource management, the DOS file API in the kernel and a large
part of the F5 Win32 personality are merged with host evidence; no image of
the current `main` has run on hardware, and hardware qualification is
open.** The 0.8 line is frozen on `legacy-0.8` (last release: prerelease
0.8.3 build 849). The binding decisions are in the
[development diary](dev_diary/README.md) (2026-10-09-06 foundations,
2026-10-09-10 scope, 2026-10-13-01 Win32 scope, 2026-10-14-08 F5 decisions,
2026-10-19-03 Windows 98/ME model and QEMU gates, 2026-10-19-06 DOS file
API, 2026-10-19-07 autonomous programme) and in the contracts in
[`docs/design/`](docs/README.md).

## Goal

A modern operating system for retro gaming on real late-1990s hardware
(the IBM ThinkPad T23, the Compaq Armada E500 and assembled desktop PCs of
1998–2004, one image for all), that stays an active project and gives
people something to play with at retro computing fairs. The target is not
Windows 98's compatibility; it is a stable, open, tested system that runs
DOS games natively, the Windows games of the era with their original
installers and the Windows applications of the period through a compatible
Win32 API, LAN games, and 3D acceleration on chosen chips.

## Principles

- A step is complete only when every item of its gate passes on QEMU
  **and** on the supported hardware. A QEMU pass never qualifies a physical
  PC.
- Hardware trial cycle (owner decisions 2026-10-19): the QEMU gates are
  suspended until the owner asks for them again; every merge needs green
  host suites and a canonical build; QEMU is used only for the DOS
  conformance battery; no intermediate hardware image is produced until the
  programme below is complete or the owner asks for one.
- Disk, memory, cache and real-time management follow the Windows 98/ME
  model completely ([contract](docs/design/win98-class-resource-management.md)).
- Each step adds capabilities that can be tested and demonstrated.
- Nine steps, F0 to F8; the contract of each step is written and
  cross-reviewed before its code.
- Existing open-source code is reused where its license allows (GPLv2
  compatible): FreeDOS as the conformance reference and an optional fallback
  guest (CiukiDOS stays the DOS of CiukiOS), lwIP for TCP/IP, FreeType and
  TinyGL, selected upper DLLs of a pinned Wine release (10.0) on a Win32
  core written for CiukiOS, and the Linux/FreeBSD/X.Org drivers of the era
  as references.
- One canonical image per source state; one QEMU at a time; latency
  evidence only under instruction counting or on real hardware.

## Steps and gates

| Step | Delivers | Gate | Status (2026-10-20) |
| --- | --- | --- | --- |
| **F0 Foundations** | Loader and image, kernel core (paging, descriptors, PIC/PIT, allocator, scheduler, ring-3 isolation, lazy FPU, syscalls, crash screen), resource registry, test runner; memory and real-time management on the Windows 98/ME model | The 11 probes of [f0-acceptance](docs/design/f0-acceptance.md); contract items M1–M4 and R1–R3 of [win98-class-resource-management](docs/design/win98-class-resource-management.md) | Merged on `main` 2026-10-10 (F0 gate on QEMU, [evidence](docs/validation/2026-10-09-f0/README.md)); quiet normal boot, ACPI shutdown, hardware-check page (H), boot log over three boots with a black box, reliability audit (f0-26, f0-25, f0-40). Real time R1–R3 (f0-44, f0-45, f0-46) and memory M1–M4 (f0-43 ledger, f0-47 memory objects, f0-48 global replacement, f0-49 swap file, merged 2026-10-20) with host evidence only. QEMU: `f0-smoke` 4/4 on the delivered images up to the last campaign (2026-10-19). T23: probes pass in the sweeps; owner trials 13–19 October (diary). **Hardware qualification open.** |
| **F1 Drivers and disk** | Qualified drivers on the registry: PS/2 (native or firmware-first), VBE framebuffer, ATA PIO and DMA, MBR partitions, VFS with FAT32+LFN (read, then write), FAT12/16, USB host with HID and mass storage, ATAPI, safe mode, boot log on disk; disk cache and I/O on the Windows 98/ME model | FAT32 integrity and crash tests, safe-mode boot, input on both laptops; contract items C1–C5; **main prereleases resume** with a re-qualified Windows bundle | Merged on `main` 2026-10-11 (f1-01..f1-22; diary 2026-10-11-06); since then USB (f1-42, f1-43 and follow-ups), ATA bus-master DMA (f1-44), shared PCI interrupts (f1-71), FIFO and per-volume locks (f1-78, f1-79), FAT writes in place (f0-32), self-repairing volume (f0-25). Cache and I/O C1 (f1-82), C3 (f1-83), C2/C5 (f1-84) and C4 (f1-85) merged 2026-10-19 with host evidence; C1 was on the last T23 image. QEMU: storage, FAT32, input, USB and optical cases pass on the last campaign. T23: UDMA disk, PS/2 and USB input, USB stick and data CD in the trials. |
| **F2 Native programs** | POSIX-compatible native API with a ported libc: processes, threads, mmap, signals, files, time, a futex-like primitive; ELF32 loader; desktop and compositor as a ring-3 process; first ported application | An application crash leaves the desktop running; an identified upstream application builds against the CiukiOS SDK and passes named runtime tests | Merged on `main` 2026-10-11 (newlib, Lua) and extended in the 0.9.0 integration and since 2026-10-13: two-bar shell with the Ciuki orb (f2-81..f2-83), desktop as a folder, FreeType, Terminal, Registry Editor, Music, CiukPaint, CiukIDE with Tiny C Compiler, Control Panel catalog, Files on worker threads. QEMU: `libc-smoke`, `fd-table` and `desktop-shell` (with its 100-cycle crash-isolation probe) pass on the last campaign; the full `f2-all` batch last passed on image `b9c052cd…` (2026-10-11). Used in the T23 trials; not qualified. |
| **F3 DOS** | DOS VMs in V86 with the CiukiDOS personality on private FAT16 disks, virtual VGA/input/PIT/PIC/SB16-OPL state, A20/XMS/UMB, kernel DPMI 0.9, EMS, DOS/4GW workloads, full screen on the VGA hardware; INT 21h file calls served by the VMM from the native VFS; pageable VM memory | Named DOS workload matrix (DOOM, Wolfenstein 3D, Quake, …); DOS conformance battery against FreeDOS; contract item D1 | In the 0.9.0 integration (f3-01..f3-08) and since: full screen with Alt+Enter and a native SuperSavage provider (f3-20, f3-32..f3-43), Quake 1.06 shareware pre-installed (f3-18), DPMI and interrupt fixes found on the T23 (f3-12, f3-23, f3-34, f3-44, f3-45), per-VM disk cache (f3-47). DOS file API in the VMM (f3-48 contract, f3-49/49b, f3-50, f3-51/51b) merged 2026-10-19 with host evidence; its FreeDOS battery runs (QEMU, T23) pending. QEMU: vm-boot, vm-video, vm-window, dpmi and dos4gw pass on the last campaign; vm-fullscreen and quake red for recorded QEMU reasons. T23: DOOM, Wolfenstein 3D and Quake in a window and full screen, open defects. **In progress:** f3-46 pageable DOS VMs (D1). |
| **F4 Sound and media** | AC97 (T23) and ESS Maestro-2E (E500) audio with audible SB16/OPL in VMs, more PCI audio families, DOS-to-VFS bridge, ATAPI and ISO9660, NTFS read-only, music player, optional S3/Mach64 2D acceleration | Per-device qualification | Merged on `main` 2026-10-11 (f4-01..f4-05); since then AC'97 on the T23's codec (f4-09, f4-10, f4-15), ES1370/1371, SB16, VIA, SiS and nForce backends (f4-11), audio pipeline audit (f4-31, f4-32), Music player (f4-07, f4-30, f4-34, f4-36), system sounds, ThinkPad volume keys, Savage 2D (f1-26..f1-69). QEMU: `f4-audio` 7/7 plus ES1370 and SB16 cases pass on the last campaign. T23: native audio heard since 2026-10-13; DOS game audio and MP3 playback with open defects. ESS: plan only. |
| **F5 Windows games** | Win32 personality: the Windows games of 1996–2004 install and run as on Windows through a compatible Win32 API (kernel32/user32/gdi32, DirectDraw, DirectSound, DirectInput, multimedia, installers); original installers and the Windows applications of the era open and work (owner mandate 2026-10-19); Win16 for 16-bit installers and programs; the game data stays the player's copy. Core written for CiukiOS, upper DLLs from Wine 10.0 (owner decisions 2026-10-13/14) | Each named title starts, plays and exits on both laptops; the acceptance installers complete | Contract ([win32-personality-contract](docs/design/win32-personality-contract.md)) written 2026-10-13 and cross-reviewed 2026-10-14 with the owner's decisions; directive sequence in execution (table below). QEMU: `f5-core` 2/2 (kernel core with self-authored test programs). Everything else host evidence only. No original Windows game, installer or application has run yet. |
| **F6 Network** | lwIP TCP/IP stack (BSD-3-Clause), DHCP, NIC drivers through a driver-compatibility layer (RTL8139, NE2000 PCI, 3Com 905, Intel 100 first), a virtual NE2000 for DOS VMs with selected, qualified DOS drivers (IPX frames over an Ethernet bridge), native sockets | A DOS LAN game (DOOM with IPXSETUP) and a native LAN game (Quake, UDP) between the two laptops; automated Ethernet tests with one QEMU and a host frame peer; **first LAN party demo** | Ahead of the step: contract draft (f6-00, [network-contract](docs/design/network-contract.md)), NIC drivers for Intel 8255x, RTL8139, NE2000 PCI (f6-01) and e1000, PCnet, Tulip, 3c90x, Rhine (f6-08), `/dev/net` and the tray icon (f6-02), NDIS miniport census with the host shelved (f6-03). QEMU: the e100 case passes. TCP/IP, DHCP, sockets and IPX: not started. |
| **F7 3D** | Mesa and DRM port, drivers for the chosen chips (SuperSavage IX/C on the T23, Rage Mobility on the E500; Voodoo 3, TNT2/GeForce if the hardware is acquired), hardware OpenGL | Quake 3 (ioquake3, GPL-2.0-or-later) playable on at least one chip, with the F0–F6 regressions still passing on both laptops | Contract draft for native Savage 3D and libciuki3d (f7-00, 2026-10-14; cross-review pending); software OpenGL subset on TinyGL and the Graphics Demo merged (f7-01). Hardware 3D: not started. |
| **F8 Native ports (optional)** | Engines with public source built for CiukiOS only where it adds value over the Windows version (owner decision 2026-10-13) | Original Half-Life, Quake 3, StarCraft, Age of Empires start, play and exit; a LAN game through winsock between the two laptops | Not started |
| later | Power management (ACPI/APM) with its own decision; more laptops and desktops; installer and recovery media | — | Not planned in detail |

### F5 sub-steps

From the directive table of the
[Win32 personality contract](docs/design/win32-personality-contract.md) and
the [directive index](docs/directives/README.md). "Host" means merged with
host tests only; none of these libraries has run in the guest except the
kernel core.

| Sub-step | Delivers | State (2026-10-20) |
| --- | --- | --- |
| f5-02A/B | PE toolchain pins, PE fixtures, import census | A merged 2026-10-14; B after the original installers run |
| f5-03 | Kernel personality, Win32 memory, exceptions, performance counter | Merged 2026-10-15; QEMU `f5-core` 2/2 (#XM vector 19 still to be shown on hardware) |
| f5-04 | Kernel objects, waits, sections, service endpoints | Merged 2026-10-19, host |
| f5-05A/B | PE loader and ntdll | A merged 2026-10-15 (Linux adapter), B merged 2026-10-19 on the kernel personality, host |
| f5-06 (+06b) | kernel32 | Merged 2026-10-19, host (225 census imports accounted) |
| f5-07A/B | w32srv, registry hives, advapi32 / PROGMAN DDE, end session, virtual restart | A merged 2026-10-19, host; **B in progress** |
| f5-08 | Presenter formats and the delegated display lease | Merged 2026-10-19, host |
| f5-09 (+09B) | Desktop protocol v3, shortcuts, Add/Remove Programs, w32srv supervision | Merged 2026-10-19, host |
| f5-10A/B/C | user32 / gdi32 / dialogs, controls, menus and fonts | A and B merged 2026-10-19, C merged 2026-10-20 (C2 continuation: guest font bridge), host only |
| f5-11 | DirectDraw 1–7 | Merged 2026-10-19, host |
| f5-12A/B | DirectSound, waveOut, MIDI / MCI, codecs, AVI, CD audio | A merged 2026-10-19 (partial), host; **B in progress** |
| f5-13 | Input messages and DirectInput | Merged 2026-10-19, host |
| f5-14A/B/C | Wine 10.0 audit / upper DLL builds / codec DLLs | A merged 2026-10-14, B and C merged 2026-10-19; the 17 reused DLLs build but do not load yet |
| f5-15A/B | Installer flows and shell integration / original installers acceptance | **In progress** |
| f5-16 | Shims, crash reports, diagnostics | Merged 2026-10-19, host |
| f5-17A/B | Acceptance tooling / runner and titles | A merged 2026-10-14; **B in progress** |
| f5-18 | Winsock and DirectPlay | After F6 |
| f5-19 | Win16 subsystem | Contract merged 2026-10-14, f5-19a (NE parser, fixtures) merged 2026-10-15; **in progress** (P0 dispositions, then f5-19b–d) |
| f5-20 | Core exports the reused Wine DLLs import | A (kernel32, ntdll, advapi32, version) merged 2026-10-20, host; **B and C to do** |

Honest notes on the hard parts: F3 (V86 monitor and DPMI) is where the
0.8 line had most trouble, and the T23 trials of 13–19 October found
interrupt and DPMI defects that QEMU did not show. A periodic firmware
pause of about 2.1 ms with interrupts disabled on the T23 (IRQ 11) is
attributed, not removed (f0-35, f0-46). F5 is the largest step: its core
is written for CiukiOS and the reused Wine DLLs depend on core exports still
being written (f5-20); the 128 MiB whole-system budget is unmeasured until a
real workload runs. F7 depends on reviving classic Mesa drivers removed
upstream: savage, mach64, r128 and tdfx left in Mesa 8.0 (February 2012),
nouveau_vieux in Mesa 22.0 (March 2022, kept on the Amber branch); the F7
draft contract (f7-00) proposes a native Savage renderer, libciuki3d, with
Mesa 7.7 and the Linux DRM driver as references; 3D performance on
our chips is unmeasured, and qualification will record game, resolution,
settings and frame rate.

## Next steps

The owner's mandate of 2026-10-19 (diary 2026-10-19-07) sets the order;
each merge needs green host suites and a canonical build.

1. **Finish the Windows 98/ME-class sequence:** f3-46, pageable DOS VM
   memory (contract item D1).
2. **Run the DOS conformance battery** against FreeDOS (QEMU is allowed for
   it) and close the differences (f3-51 runtime gates).
3. **Complete F5:** f5-20B/C, f5-10C2, f5-07B, f5-12B, f5-15A/B (original
   installers and applications), f5-17B (acceptance runner and titles) and
   the Win16 subsystem (f5-19).
4. **One image for the hardware** when the programme is complete or the
   owner asks for it: T23 trial with serial capture and disk logs, then the
   hardware qualification of F0–F4 on that image; the E500 and generic PCs
   follow; the QEMU gates resume on the owner's request.

## Reuse from the 0.8 line

Device models in `src/vm/` (virtual VGA, guest peripherals, audio mixer,
OPL), the C applications in `src/apps/`, the LFN logic and CiukiDOS as the
DOS personality are reused or ported in F2–F4 according to the inventory in
[foundations-transition](docs/design/foundations-transition.md). Jemm,
HDPMI, VMFORK and the 16-bit SHELL.COM desktop are retired when their
replacements pass their gates.
