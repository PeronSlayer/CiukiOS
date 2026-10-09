# Foundations transition: scope, reuse, branches and releases

Contract 1 of 7 required by decision D11 before any F0 code. Author: Claude
(lead). Reviewer: Codex. Status: approved after the Claude–Codex
cross-review, 2026-10-09.

## Purpose

This contract turns the joint decision
[`dev_diary/2026-10-09-06-decisione-fondamenta-32bit.md`](../../dev_diary/2026-10-09-06-decisione-fondamenta-32bit.md)
into the rules for moving the repository from the 0.8 stack to Ciuki VMM:
what is in scope per phase, what existing code is reused, ported, kept for
the DOS side or retired, which licenses apply, how branches and releases work
during the transition, and how the build switches over without leaving
`main` unbootable.

## Sources

- Owner requirement and joint decision: dev diary entries
  [2026-10-09-05](../../dev_diary/2026-10-09-05-revisione-codex-architettura.md)
  and [2026-10-09-06](../../dev_diary/2026-10-09-06-decisione-fondamenta-32bit.md).
- [GNU GPL v2](../../LICENSE) (project license) and the FSF
  [license compatibility list](https://www.gnu.org/licenses/license-list.html):
  GPLv2-only code cannot absorb GPLv3-only code; "GPLv2 or later" code can be
  combined with this project under GPLv2.
- Jemm licensing is per component: [upstream readme](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Readme.txt)
  (JEMM386/JEMMEX partly under its Artistic License),
  [JLOAD license (MIT)](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/Tools/JLOAD/license.txt),
  [JLM samples (Public Domain)](https://github.com/Baron-von-Riedesel/Jemm/blob/e96bb6bb80cbdc3e6585544db79a6b133a5566dd/JLM/README.TXT).
- Raymond Chen, [What was the role of MS-DOS in Windows 95?](https://devblogs.microsoft.com/oldnewthing/20071224-00/?p=24063):
  MS-DOS stayed as boot loader and 16-bit legacy layer; file I/O moved to the
  32-bit file system while non-file INT 21h services stayed in DOS. This is the
  reference for keeping CIUKIDOS as the per-VM personality (D7).
- Repository facts checked on 2026-10-09: `scripts/` contains only the 0.8
  build, run, release and log-collection closure (traced full build,
  dev diary entry 2026-10-09-03); `src/vm/` holds the ring-0 C models listed
  below; the 0.8 boot path is `src/boot/full_boot.asm` →
  `src/boot/full_stage1_loader.asm` → `CIUKIDOS.SYS`.

## Scope per phase

| Phase | Delivers | Gate (all on QEMU, T23 and E500) |
| --- | --- | --- |
| F0 | Loader handoff, kernel core (paging, IDT/GDT/TSS, PIC, PIT, physical allocator, kernel heap), resource reservation registry and read-only platform inventory, safe console selection, early serial and crash screen, probe syscalls, embedded ring-3 probes with preemption and fault containment, runner integration | F0 criteria in the decision entry and [`f0-acceptance.md`](f0-acceptance.md) |
| F1 | Qualified device activation on the registry, input (native PS/2 or the firmware-first lease), VBE LFB framebuffer, ATA PIO, MBR partitions, VFS with FAT32+LFN (read first, then write), FAT12/16, complete safe mode, boot log on disk | FAT32 read/write integrity, safe-mode boot, input on both laptops; main prereleases resume |
| F2 | ELF32 application loading and the POSIX-subset syscalls with a ported libc (`execution-abi.md`), native processes, desktop/compositor as a ring-3 process, first ported application | an application crash leaves the desktop and other processes running; an identified upstream application builds against the CiukiOS SDK and passes named runtime tests |
| F3 | DOS VMs in V86 with CIUKIDOS personality on private FAT16 disks, virtual devices (VGA, input, PIT/PIC, SB16/OPL state), A20/XMS/UMB, then kernel DPMI 0.9, EMS and DOS/4GW workloads | named DOS workload matrix ("DOS parity", defined in `dos-dpmi-contract.md`) |
| F4 | AC97 and ESS Maestro-2E audio (audible SB16/OPL output), DOS-to-VFS bridge for shared volumes, optional S3/Mach64 2D acceleration, ATAPI and ISO9660, NTFS read-only | per-device qualification; acceleration stays off where unqualified |
| F5 | SDL port on the POSIX API, software OpenGL for 2D use, native builds of engines with public source (Quake 1/2, Xash3D-FWGS for Half-Life as a separate GPL-3.0 program, Doom ports, Descent, Duke Nukem 3D); DevilutionX is a separate candidate with licensing restrictions; commercial game data remains separately licensed and is supplied by the player | each named game starts, plays and exits on both laptops |
| F6 | lwIP TCP/IP, DHCP, NIC drivers through a driver-compatibility layer, a virtual NE2000 for DOS VMs with selected, qualified DOS drivers, native sockets | a DOS LAN game (IPX) and a native LAN game (UDP) between the two laptops; automated Ethernet tests with one QEMU and a host frame peer |
| F7 | Mesa and DRM port, 3D drivers for the chosen chips, hardware OpenGL | Quake 3 (ioquake3) playable on at least one chip, with the earlier gates still passing on both laptops |
| F8 | Wine as the Win32 personality with a CiukiOS backend: DirectX over our graphics and audio, winsock over our sockets | original Half-Life, Quake 3, StarCraft and Age of Empires start, play and exit; a LAN game through winsock between the two laptops |

Steps F5–F8 were added on 2026-10-09 with the scope decision (dev diary
2026-10-09-10): F0 foundations followed by eight steps. Their contracts are
written when the previous step closes. The F2 native API is a POSIX subset
(`execution-abi.md`).

A phase is complete only when every item of its gate passes on the image it
is claimed for (AGENTS.md, test-architecture.md). Later phases MUST NOT start
integration on `main` while the previous gate is open; research and isolated
prototypes in worktrees MAY proceed.

## Reuse inventory

Disposition values: **reuse** (source compiles unchanged or with mechanical
changes behind new service headers), **port** (logic kept, integration
rewritten), **DOS-side** (stays a 16-bit DOS component inside VMs), **keep**
(build input or asset), **retire** (removed from `main` once its replacement
gate passes; history stays on `legacy-0.8`).

| Component | Disposition | Phase | Notes |
| --- | --- | --- | --- |
| `src/vm/virtual_vga.c`, `virtual_vga_bios.c`, `vga_x86.c`, `vga_presenter.c` | reuse | F3 | Per-VM instances instead of session singletons; host tests `test_virtual_vga.c`, `test_vga_x86_shim.c` move with them. |
| `src/vm/guest_peripherals.c/h`, `guest_peripheral_scheduler.h` | reuse | F3 | Virtual PIC/PIT/keyboard/mouse/SB16/DMA models; needs the kernel's timer and event services. |
| `src/vm/session_audio.c`, `session_clock.c` | reuse | F3–F4 | Mixer and clock maths; host tests move with them. |
| `src/vm/session_opl.cpp`, `guest_opl_dbopl.cpp/h`, `opl_shim/` | reuse | F3 | DBOPL is GPL-2.0-or-later (file headers); compatible with GPLv2. Needs a freestanding C++ subset or a C wrapper. |
| `src/vm/session_disk_ata.c/h` | port | F1 | Register logic and the "no BIOS fallback after an issued command" rule are kept; synchronization, timeouts and IRQ use are rewritten for the kernel. |
| `src/vm/session_gpu_savage.c`, `session_gpu_mach64.c`, `session_gpu_legacy.c`, `session_gpu.c` | port | F4 | Register sequences and qualification logic; they preserve firmware modes and are not modesetting drivers. |
| `src/vm/session_video.c`, `session_devices.c`, `hdpmi_video_adapter.c`, `dpmi_video_fault.c` | retire | F3 | Bound to JLOAD page tables and the HDPMI adapter; their behaviour is re-specified in `dos-dpmi-contract.md`. |
| `src/vm/*.inc`, `session_jlm.asm`, `hdpmi_session_adapter.asm`, `src/com/vmfork.asm` | retire | F3 | Jemm/HDPMI/VMFORK integration. Kept on `main` until F3 replaces DOS windows. |
| `src/vm/session_native_process.inc`, `src/native/` | retire | F2 | Superseded by kernel processes; useful as a reference for the CPL3 gate. |
| `src/boot/floppy_stage1.asm`, `src/runtime/` (CIUKIDOS) | DOS-side | F3 | Runs unchanged at first on a private FAT16 virtual disk with virtual firmware (decision B); VFS bridge added later. |
| `src/boot/full_boot.asm`, `full_stage1_loader.asm`, `full_stage2.asm` | retire | F0/F1 | Replaced by the loader in `boot-memory.md`; F0 MAY reuse the FAT16 sector-reading routines. |
| `src/com/shell*.asm/.inc`, `ui*`, `vbe_*.inc` (SHELL.COM desktop) | retire | F2 | Replaced by the ring-3 desktop. Its behaviour and the approved Ciuki assets are the reference for the port. |
| `src/apps/*.c` (desktop, Files, Paint, CiukNote, Control, Display, Tasks, browser, media) | port | F2+ | 16-bit OpenWatcom `.APP` modules on the `app.h` shell ABI; ported one at a time to the native API, first app named in F2. |
| `src/lfn/`, `src/media/`, `src/web/` | port | F2+ | LFN logic informs the FAT32 driver; decoders and the web stack are ported with their applications. |
| `src/com/setup.asm`, installer pieces | retire | F1 | A new installer is specified with the FAT32 layout. |
| `src/ports/` (doomgeneric and Wolf window ports) | port | F2+ | Native ports of the games become native programs or are retired when DOS VMs run the originals (F3). |
| `src/probes/` (DOS and VM probes) | DOS-side or retire | F3 | Reused as DOS-side test payloads where they fit the new runner; otherwise retired with the Jemm path. |
| DOS utilities in `src/com/` (`format.asm`, `drvload.asm`, game launchers, probes) | DOS-side or retire | F3 | Decided per file when DOS VMs land. |
| `assets/` (Ciuki portrait, Tango icons, sounds, cursors, fonts), `wallpapers/`, `misc/` | keep | all | Identity rules in AGENTS.md apply unchanged. |
| `third_party/` (bearssl, tjpgd, dr_libs/stb, jsvendor, doomgeneric, wolf4sdl, ctmouse, freedos, vsbhda) | keep | per user | Each keeps its own license file; local commercial payloads (`Doom/`, `WOLF3D/`, `win_bg/`) stay untracked. |
| Planned imports (F5–F8): newlib or musl, SDL, lwIP, Mesa and DRM drivers of the era, FreeBSD/Linux NIC drivers behind a compatibility layer, Wine | later | F2–F8 | Each import is pinned to an upstream commit and reviewed for its exact licenses, dependencies, linking and redistribution requirements; kernel integration is reviewed separately from independently distributed user programs (a GPL-3.0 program such as Xash3D-FWGS can be shipped as a separate program but never linked into GPLv2 code). |
| `third_party/jemm`, `patches/jemm-*`, `patches/hdpmi-*`, `patches/sbemu-*`, `patches/vsbhda-*` | retire | F3 | Not used by the new kernel. A Jemm file is reused only after a per-file license check recorded in this table. |
| `scripts/build_full.sh` and its closure | retire | F0 switch | Replaced atomically (see below). |

The inventory MUST be updated in the same commit that changes a component's
disposition. A component is deleted from `main` only when its replacement has
passed the gate of the phase listed, or when the owner decides to drop the
feature; the deletion is recorded in a dev diary entry.

## License rules

- New Ciuki VMM code is GPL-2.0-only, like the project.
- Reused third-party code MUST carry its notice and appear in the inventory
  with its license. Each import is pinned to an upstream commit and reviewed
  for its exact licenses, dependencies, linking and redistribution
  requirements; kernel integration is reviewed separately from
  independently distributed user programs. Code that is only available
  under GPL-3.0 or LGPL-3.0 cannot be linked into GPLv2 code; Artistic-
  licensed Jemm parts need an explicit per-file check before reuse.
- NTFS support (F4) MUST choose a GPLv2-compatible source (for example the
  Linux `ntfs3` driver, GPL-2.0) or an independent implementation from the
  published on-disk format; GRUB's NTFS code is GPL-3.0 and is excluded.
- Commercial DOS game data and Windows media stay local and untracked, and
  never enter a release.

## Branches and releases

- `legacy-0.8` (from `868cac9`) is the 0.8 line. Only critical fixes are
  accepted there; a fix that changes the image MUST rebuild and re-verify the
  0.8 Windows bundle on that branch. Prerelease b849 is the preserved 0.8
  baseline; a legacy release beyond b849 needs the owner's approval.
- `main` is the Ciuki VMM line. `config/release-policy.json` keeps main
  publication `suspended` until the F1 gate passes; the pre-push hook keeps
  running and still requires a clean tree.
- Resuming publication requires: the F1 gate passed on the image to be
  published, a re-qualified Windows portable bundle built from that image,
  and the policy switched to `active` in the same commit, with a dev diary
  entry.
- Teammate work happens in `wt/<task>` branches inside git worktrees; only the
  lead merges into `main`. Worktrees and their branches are removed after
  merge (AGENTS.md).

## Atomic build switch

Until the first F0 commit, `make build-full` still produces the 0.8 image on
`main`. The switch MUST happen in one commit that:

1. adds the new loader, kernel and probes and a `make image` target producing
   the canonical F0 image;
2. points `make build-full`, `make qemu-test-full` and the runner at the new
   image;
3. moves the 0.8 build closure out of the default targets without deleting
   the reusable sources listed above;
4. passes `make image` from a clean checkout and the F0 smoke probe on QEMU
   before it is pushed.

After the switch, the 0.8 image is built only from `legacy-0.8`.

## Repository layout after the switch

| Path | Content |
| --- | --- |
| `src/boot/` | new MBR and loader (`boot-memory.md`); 0.8 boot files until retired |
| `src/kernel/` | Ciuki VMM (C + NASM), linker script, kernel library |
| `src/kernel/probes/` | embedded F0 probes |
| `src/native/` | native ring-3 programs (from F2) |
| `src/dos/` | CIUKIDOS personality and DOS-side tools, moved from `src/boot/` and `src/runtime/` in F3 |
| `src/vm/` | reusable device models until they move under `src/kernel/` |
| `tests/suites/`, `scripts/test/` | runner and suites (`test-architecture.md`, `f0-acceptance.md`) |

## Acceptance tests

- The inventory table lists every top-level directory under `src/`; a static
  check (F0 runner tier T1) fails if a source file is deleted from `main`
  while its inventory row still says reuse/port/keep.
- The atomic switch commit builds from a clean clone with
  `make image` and passes the F0 smoke probe on QEMU; the 0.8 image still
  builds from `legacy-0.8`.
- The pre-push hook on `main` prints the suspension message and publishes
  nothing while the policy is `suspended` (verified on 2026-10-09 with
  `scripts/push_release.py --from-hook`).
- License check: a T1 check lists every third-party file compiled into the
  kernel image or a native program and fails on an unlisted license.

## Open questions

- Which application is ported first in F2 (Files, CiukNote or a new
  minimal one)? Needs the owner's preference; Codex and Claude suggest the
  smallest one that exercises input, drawing and files.
- Whether the 0.8 DOS-window features (M4 multi-VM windows, DOOM in a window)
  must be matched before F3 closes, or whether F3 parity is defined only by
  the named workload matrix. Resolved by `dos-dpmi-contract.md` plus owner
  approval.
- Whether a legacy-0.8 release beyond b849 is wanted at all.

## Interfaces required from other contracts

- `boot-memory.md`: the image layout used by `make image` and the installer.
- `execution-abi.md`: toolchain pins and the native program format for F2.
- `f0-acceptance.md`: probe names and the runner commands used by the switch.
- `dos-dpmi-contract.md`: the definition of DOS parity and the retirement
  point of the Jemm/HDPMI/VMFORK code.
