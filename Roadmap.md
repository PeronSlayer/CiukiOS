# CiukiOS roadmap

**Current line: Ciuki VMM foundations, phase F0 on `main`.** The 0.8 line is
frozen on `legacy-0.8` (last release: prerelease 0.8.3 build 849). The
binding plan is the decision recorded in
[dev diary 2026-10-09-06](dev_diary/2026-10-09-06-decisione-fondamenta-32bit.md)
and the seven contracts in [`docs/design/`](docs/README.md).

## Goal

A stable system on real late-1990s laptops (IBM ThinkPad T23, Compaq Armada
E500) and on QEMU, with Windows 95/98-class structure: a 32-bit kernel that
owns the machine, native processes with private address spaces, FAT32 (NTFS
read-only later), DOS programs and games running in virtual machines with
DPMI provided by the kernel, and a native desktop.

## Phases and gates

A phase is complete only when every item of its gate passes on the image it
is claimed for, on QEMU **and** on both laptops. A QEMU pass never qualifies
a physical PC.

| Phase | Delivers | Gate | Status (2026-10-09) |
| --- | --- | --- | --- |
| **F0** | Loader and image, kernel core (paging, descriptors, PIC/PIT, allocator, scheduler, ring-3 isolation, lazy FPU, syscalls, crash screen), resource registry, test runner | The 11 probes of [f0-acceptance](docs/design/f0-acceptance.md): boot matrix, bootinfo, allocator, protection, isolation, preempt, localfault, syslife, panic, fpu, runner | QEMU: all probes pass ([evidence](docs/validation/2026-10-09-f0/README.md)). T23/E500: not yet run (needs an expendable disk; serial adapter optional). |
| **F1** | Qualified drivers on the registry: PS/2 (native or firmware-first), VBE LFB, ATA PIO, MBR partitions, VFS with FAT32+LFN (read, then write), FAT12/16, safe mode, boot log on disk | FAT32 read/write integrity and crash tests, safe-mode boot, input on both laptops; **main prereleases resume** with a re-qualified Windows bundle | Not started |
| **F2** | ELF32 loader, application syscalls, native processes, desktop/compositor as a ring-3 process, first ported application | An application crash leaves the desktop and other processes running | Not started |
| **F3** | DOS VMs in V86 with the CiukiDOS personality on private FAT16 disks, virtual VGA/input/PIT/PIC/SB16-OPL state, A20/XMS/UMB, kernel DPMI 0.9, EMS, DOS/4GW workloads (DOOM, Wolf3D) | Named DOS workload matrix ("DOS parity") | Not started |
| **F4** | AC97 (T23) and ESS Maestro-2E (E500) audio with audible SB16/OPL for VMs, DOS-to-VFS bridge for shared volumes, optional S3/Mach64 acceleration, ATAPI and ISO9660, NTFS read-only | Per-device qualification; acceleration stays off where unqualified | Not started |
| later | Power management (ACPI/APM) with its own decision; Win32 compatibility personality (PE32) on named free programs | — | Not planned in detail |

## Next steps

1. **Close F0 on hardware:** write `build/f0/ciukios.img` to an expendable
   disk, run the boot matrix and the probes on the T23 and E500 with screen
   evidence (serial capture once an RS-232 adapter is available), and record
   the results in `docs/validation/` and the diary.
2. **Start F1** from the contracts: registry-driven device activation, the
   ATA driver with the no-BIOS-retry rule, FAT32 read path qualified before
   writes, the safe-mode menu.
3. Keep the one-image discipline: tests never rebuild or copy the canonical
   image; one QEMU at a time under a memory cap; latency evidence only with
   an idle host ([test architecture](docs/design/test-architecture.md)).

## Reuse from the 0.8 line

Device models in `src/vm/` (virtual VGA, guest peripherals, audio mixer,
OPL), the C applications in `src/apps/`, the LFN logic and CiukiDOS as the
DOS personality are reused or ported in F2–F4 according to the inventory in
[foundations-transition](docs/design/foundations-transition.md). Jemm,
HDPMI, VMFORK and the 16-bit SHELL.COM desktop are retired when their
replacements pass their gates.
