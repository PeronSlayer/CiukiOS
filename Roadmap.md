# CiukiOS roadmap

**Current line: Ciuki VMM on `main`. F0 smoke passes on QEMU and core tests pass 55/56; safe-mode automation and hardware qualification remain open.** The 0.8 line is
frozen on `legacy-0.8` (last release: prerelease 0.8.3 build 849). The
binding decisions are in the [development diary](dev_diary/README.md)
(2026-10-09-06 foundations, 2026-10-09-10 scope) and in the contracts in
[`docs/design/`](docs/README.md).

## Goal

A modern operating system for retro gaming on real late-1990s hardware
(today the IBM ThinkPad T23 and the Compaq Armada E500), that stays an
active project and gives people something to play with at retro computing
fairs. The target is not Windows 98's compatibility; it is a stable, open,
tested system that runs DOS games natively, the open-source engines built
for it, LAN games, and — in its last steps — the original Windows 95/98
games through a Wine-based layer and 3D acceleration on chosen chips.

## Principles

- A step is complete only when every item of its gate passes on QEMU
  **and** on both laptops. A QEMU pass never qualifies a physical PC.
- Each step adds capabilities that can be tested and demonstrated.
- Nine steps, F0 to F8; the contract of each step is written and reviewed
  when the previous one closes.
- Existing open-source code is reused where its license allows (GPLv2
  compatible): FreeDOS lineage for the DOS personality, lwIP for TCP/IP,
  SDL, Mesa and the Linux/FreeBSD drivers of the era, Wine for Win32.
- One canonical image per source state; one QEMU at a time; latency
  evidence only under instruction counting or on real hardware.

## Steps and gates

| Step | Delivers | Gate | Status (2026-10-09) |
| --- | --- | --- | --- |
| **F0 Foundations** | Loader and image, kernel core (paging, descriptors, PIC/PIT, allocator, scheduler, ring-3 isolation, lazy FPU, syscalls, crash screen), resource registry, test runner | The 11 probes of [f0-acceptance](docs/design/f0-acceptance.md) | QEMU: 55/56 core cases pass ([evidence](docs/validation/2026-10-09-f0/README.md)); safe-mode automation open. **Next: T23 and E500 with serial capture.** |
| **F1 Drivers and disk** | Qualified drivers on the registry: PS/2 (native or firmware-first), VBE framebuffer, ATA PIO, MBR partitions, VFS with FAT32+LFN (read, then write), FAT12/16, safe mode, boot log on disk | FAT32 integrity and crash tests, safe-mode boot, input on both laptops; **main prereleases resume** with a re-qualified Windows bundle | Not started |
| **F2 Native programs** | POSIX-compatible native API with a ported libc (newlib or musl, to be selected): processes, threads, mmap, signals, files, time, a futex-like primitive; ELF32 loader; desktop and compositor as a ring-3 process; first ported application | An application crash leaves the desktop running; an identified upstream application builds against the CiukiOS SDK and passes named runtime tests | Not started. Direction decided: POSIX subset (diary 2026-10-09-10); the interface list is specified before code |
| **F3 DOS** | DOS VMs in V86 with the CiukiDOS personality on private FAT16 disks, virtual VGA/input/PIT/PIC/SB16-OPL state, A20/XMS/UMB, kernel DPMI 0.9, EMS, DOS/4GW workloads | Named DOS workload matrix (DOOM, Wolfenstein 3D, …) | Not started |
| **F4 Sound and media** | AC97 (T23) and ESS Maestro-2E (E500) audio with audible SB16/OPL in VMs, DOS-to-VFS bridge, ATAPI and ISO9660, NTFS read-only, optional S3/Mach64 2D acceleration | Per-device qualification | Not started |
| **F5 Native games** | SDL port, software OpenGL for 2D/low-end use, native builds of engines with public source: Quake 1 and 2 (GPL-2.0-or-later), Half-Life via Xash3D-FWGS (GPL-3.0-or-later, distributed as a separate program), Doom ports, Descent, Duke Nukem 3D; DevilutionX is a separate candidate because its Sustainable Use License restricts commercial use; game data is always the player's own | Each named game starts, plays and exits on both laptops | Not started |
| **F6 Network** | lwIP TCP/IP stack (BSD-3-Clause), DHCP, NIC drivers through a driver-compatibility layer (RTL8139, NE2000 PCI, 3Com 905, Intel 100 first), a virtual NE2000 for DOS VMs with selected, qualified DOS drivers (IPX frames over an Ethernet bridge), native sockets | A DOS LAN game (DOOM with IPXSETUP) and a native LAN game (Quake, UDP) between the two laptops; automated Ethernet tests with one QEMU and a host frame peer; **first LAN party demo** | Not started |
| **F7 3D** | Mesa and DRM port, drivers for the chosen chips (SuperSavage IX/C on the T23, Rage Mobility on the E500; Voodoo 3, TNT2/GeForce if the hardware is acquired), hardware OpenGL | Quake 3 (ioquake3, GPL-2.0-or-later) playable on at least one chip, with the F0–F6 regressions still passing on both laptops | Not started |
| **F8 Windows games** | Wine (LGPL-2.1-or-later) as the Win32 personality, with a CiukiOS backend for its graphics, audio, signal and memory needs: DirectDraw/Direct3D/DirectSound over our graphics and audio, winsock over our sockets | Original Half-Life, Quake 3, StarCraft, Age of Empires start, play and exit; a LAN game through winsock between the two laptops | Not started |
| later | Power management (ACPI/APM) with its own decision; more laptops and desktops; installer and recovery media | — | Not planned in detail |

Honest notes on the hard parts: F3 (V86 monitor and DPMI) is where the
0.8 line had most trouble. F7 depends on reviving classic Mesa drivers
removed upstream: savage, mach64, r128 and tdfx left in Mesa 8.0 (February
2012), nouveau_vieux in Mesa 22.0 (March 2022, kept on the Amber branch);
3D performance on our chips is unmeasured, and qualification will record
game, resolution, settings and frame rate. F8 inherits Wine's size, needs a
CiukiOS backend of its own, and makes everything before it a prerequisite;
the POSIX subset reduces the porting work but does not by itself guarantee
Wine compatibility.

## Next steps

1. **Close F0 on hardware:** write `build/f0/ciukios.img` to an expendable
   disk, run the boot matrix and the probes on the T23 and E500 with serial
   capture (RS-232 to USB adapter, null-modem, 38400 8N1), record the
   results in `docs/validation/` and the diary; close the safe-mode
   automation gap in the loader and contracts.
2. **Start F1** from the contracts: registry-driven device activation, the
   ATA driver with the no-BIOS-retry rule, the FAT32 read path qualified
   before writes, the safe-mode menu.
3. **Specify F2's POSIX subset** in `execution-abi.md` (syscall table,
   signals, threads, path mapping, libc choice) and the network foundations
   for F6, each reviewed by the second agent before code.

## Reuse from the 0.8 line

Device models in `src/vm/` (virtual VGA, guest peripherals, audio mixer,
OPL), the C applications in `src/apps/`, the LFN logic and CiukiDOS as the
DOS personality are reused or ported in F2–F4 according to the inventory in
[foundations-transition](docs/design/foundations-transition.md). Jemm,
HDPMI, VMFORK and the 16-bit SHELL.COM desktop are retired when their
replacements pass their gates.
