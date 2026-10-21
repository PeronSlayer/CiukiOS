# CiukiOS Setup from CD — plan for the 1.0 Beta distribution

Status: plan approved by the owner on 2026-10-21 (dev diary 2026-10-21-06); directives
f9-01 and f9-02 launched the same day, Codex plan review in progress. The owner asked
for the CD-based Setup now, in parallel with the programme, so that the 1.0 Beta can be
distributed as an installable CD. The visual mockup of every screen is the
private canvas "CiukiOS Setup from CD" (12 artboards, 640×480); this document is
the contract-level plan the directives f9-01…f9-06 will be written from after
the owner's review and the Codex plan review. Nothing here is implemented yet.

## 1. Goal and scope

- One bootable CD image (`build/f0/ciukios-setup.iso`, ≤ 650 MB) built by the
  canonical build beside the disk image, from the same payload set and pins.
- Booting the CD on any machine of the hardware baseline
  (`hardware-baseline.md`: i686 with CMOV, BIOS with E820 and INT 13h
  extensions, ≥128 MiB, PATA disk, ATAPI CD-ROM, VBE 2.0+, PS/2) runs **Setup**,
  a native CiukiOS program that installs the system to the hard disk in about
  15 minutes and restarts into the installed system.
- Setup shares the CiukiOS identity (Ciuki portrait, tagline, English copy,
  Windows 98-class wizard) as `AGENTS.md` requires; the desktop and Setup use
  the same `libdeskui` controls.
- Out of scope for the Beta: floppy boot, network installation, upgrading an
  existing CiukiOS in place (the Beta reformats), dual boot with another OS
  (Advanced mode may keep foreign partitions but installs no boot menu for them).

## 2. What exists and what is reused

| Need | Existing piece | Reuse |
| --- | --- | --- |
| Boot from CD | legacy 0.8 `scripts/build_full_cd.sh`, `src/boot/full_cd_mbr.asm` (El Torito HDD emulation + isolinux fallback + grub4dos low-memory variant) | Method only; the new CD boots the F0 loader `ciukldr` by **El Torito no-emulation** with the kernel and the Setup payload read from ISO 9660 |
| Read the CD | F4 optical driver (`src/kernel/drivers/optical.c`, `optical_cd.c`, `media_cd.c`), `src/kernel/fs/iso9660.c` | Mount the CD read-only as a volume; Setup copies files through the VFS |
| Write the disk | ATA driver with DMA (f1-86 evidence), MBR/partition code (`partition.c`), FAT driver (`fat.c`), `build_image.py` geometry rules (512-byte sectors, 8 sectors per cluster, two FATs, reserved sectors, active 0x0C partition) | A **native FAT32 formatter and MBR writer** is new (today formatting is done by the host build script); it reuses the same geometry constants and the fsck checks of the fs suite |
| Install the loader | MBR + `ciukldr` at LBA 1–62 (`boot-memory.md`), `BOOT.CFG` | Setup writes the same bytes the image build writes, then reads them back |
| Hardware detection | F1 probes: CPU features, E820, VBE, ATA/ATAPI, i8042, AC'97/ES137x, NICs, Savage; the `hw` records | Setup shows the detected set and the baseline verdict before touching the disk |
| Wizard UI | `libdeskui` (controls, dialogs, fonts), the desktop protocol v3 | Setup is a full-screen libdeskui client spawned by the supervisor instead of the desktop when the boot volume is the CD |
| Registry and settings | w32srv registry, Control Panel pages (keyboard, network, clock) | First-boot wizard writes user, computer name, keyboard, time zone, network mode through the same stores |
| Component lists | Add/Remove Programs (f2-87) catalogue, licence inventory (`build_image.py`) | The Components screen and the installed licence set come from the same catalogue |

## 3. Boot path

1. BIOS boots the CD by **El Torito hard-disk emulation** of a small FAT image that
   holds the MBR, `ciukldr`, `BOOT.CFG` and the kernel (the method the legacy 0.8 CD
   proved on the T23): the loader runs unchanged and needs no ISO 9660 reader, which
   matters because its fixed extent is full (f0-54). The kernel then mounts the real
   CD through the ATAPI optical driver to read the Setup payload. A no-emulation
   entry with an ISO-reading stage stays a later option for BIOSes that refuse
   emulation; isolinux and grub4dos fallbacks are not carried over unless the
   qualification shows a need.
2. `ciukldr` shows the boot menu (screen 1 of the mockup): Start Setup (default
   after 10 s), boot from the first hard disk, rescue console, hardware report;
   `S` safe video, `L`, `P`, `H` keep their meanings. `BOOT.CFG` on the CD
   carries `setup=1` (lead-reserved token) so the kernel knows the boot volume is
   the CD.
3. The kernel starts with the CD as a read-only system volume; the supervisor
   spawns `/Programs/setup` instead of `/Programs/desktop`.

## 4. Setup program (`apps/setup/`)

Screens (mockup numbering): 2 Welcome, 3 Licences (texts from the licence
inventory; GPL-2.0-or-later notice and source archive for the unified DOS
kernel per dev diary 2026-10-21-04), 4 Hardware check (verdict against the
baseline, stop screen 12 on a hard failure such as a CPU without CMOV or less
than 128 MiB), 5 Destination disk (disk list from the block layer; three modes:
whole disk, free space, advanced; the resulting layout drawn; the data-loss
warning), 6 Components (system always; DOS personality, Windows personality,
NetSurf, shareware games, developer tools, soundfont, DOS kernel sources; sizes
from the catalogue), 7 User and computer (name, computer name for the LAN,
keyboard, clock written to the RTC: HTTPS needs a qualified RTC, f6-15), 8
Network (DHCP or manual; IPX for DOS games; the Windows game lobby), 9 Copy
(partition, format, loader install with read-back, file copy with per-file
verification, registry, pagefile; progress and "did you know" tips), 10 Restart
(remove the CD, 15 s countdown), 11 First boot (one-time finishing: accelerator
test with `accel=0` fallback, sound, network, bundled game registration, entropy
collection; then straight to the desktop).

Rules: no writes before the Copy screen; every write path is the production
kernel's (no Setup-only disk code); the installation is journaled in
`SETUP.LOG` on the target volume and in memory before the volume exists;
cancelling during Copy leaves a documented state (unbootable disk, Setup can be
restarted); keyboard-only operation works everywhere (Tab, Enter, Esc, F1, F3,
F10); 640×480 is the design size and every screen fits it.

Unattended mode: `SETUP.INF` on the CD root or on a FAT USB stick (answers for
every screen, `AcceptLicences=1`, `Disk=auto`, `Components=…`, `UserName`,
`ComputerName`, `Keyboard`, `Network=dhcp`); the QEMU tests use it.

## 5. Directives to write (after the owner's review of this plan)

| Directive | Scope | Model |
| --- | --- | --- |
| f9-01 CD media and boot | ISO 9660 builder in `scripts/build_image.py` (`--cd`), El Torito no-emulation stage for `ciukldr`, ISO path-table reader in the loader within the fixed extent, `setup=1` token, host tests on the ISO layout, boot smoke on QEMU `-cdrom` | gpt-6.1-sol / xhigh |
| f9-02 Native disk tools | MBR writer, partition creation and resizing of free space, FAT32 formatter with the canonical geometry, loader install with read-back, fsck-based verification; host tests against the fs suite and `build_image.py` as the oracle | gpt-6.1-sol / xhigh |
| f9-03 Setup application | `apps/setup` on libdeskui: the 12 screens of the mockup, keyboard navigation, hardware verdict, component catalogue, `SETUP.LOG`, unattended `SETUP.INF`; host tests of the screens (render goldens like the desktop tests) and of the answer file | gpt-6.1-sol / high |
| f9-04 Copy and configure | ISO→FAT32 copy through the VFS with per-file verification, registry/user/network/clock writes, pagefile, bundled game registration, cancel semantics | gpt-6.1-sol / xhigh |
| f9-05 First boot and rescue | one-time finishing wizard, `accel=0` fallback, rescue console (read-only tools, log export to USB), hardware report export | gpt-6.1-sol / high |
| f9-06 Qualification | QEMU unattended install end to end (CD boot → installed disk boots to the desktop) as a runner suite, then the T23 and a desktop twin; evidence record in `docs/validation/` | lead with Codex tests |

Order: f9-01 and f9-02 in parallel (no UI), then f9-03/f9-04 in parallel, then
f9-05, then f9-06. Estimated: four Codex session-days of implementation plus the
lead's qualification; the QEMU unattended install is the gate for calling the
CD "distributable".

## 6. Acceptance for the Beta

- The ISO boots on QEMU (`qemu-desktop-1998`, `qemu-desktop-2002`, `qemu-t23`
  twins) and on the T23; the unattended install produces a disk whose
  SHA-256-verified payload set equals the canonical image's.
- Every screen of the mockup exists with the same copy; keyboard-only
  completion; the stop screen appears on an unsupported CPU or too little RAM
  without writing to the disk.
- The installed system boots to the desktop, with network, sound, the bundled
  games and the browser working as on the canonical image; `SETUP.LOG` and
  `HARDWARE.TXT` present.
- Licences: the ISO carries `\LICENSES` and `\SOURCES` (unified DOS kernel
  source archive with its hash), the Licence screen lists every component.

## 7. Risks

- El Torito variety on 1998–2004 BIOSes (no-emulation support, 2048-byte reads,
  some BIOSes only boot HDD-emulation images): keep the legacy HDD-emulation
  path as a fallback boot entry on the same ISO.
- ATAPI drives without DMA: the copy runs in PIO at 2–3 MB/s (≈ 3 minutes for
  360 MB), acceptable; the progress estimate must come from measured throughput.
- RAM at the 128 MiB minimum: Setup plus the CD cache must fit; no RAM disk of
  the payload.
- Partition tools are the only destructive code in the product: they get the
  fs suite's crash-cut treatment and a dry-run mode used by the tests.
