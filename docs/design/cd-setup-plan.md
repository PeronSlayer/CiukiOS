# CiukiOS Setup from CD — plan for the 1.0 Beta distribution

Status: plan approved by the owner on 2026-10-21 (dev diary 2026-10-21-06), then
reworked on 2026-10-21 after the Codex plan review (`build/f9-plan-review.md`,
15 findings, verdict rework) under the lead's fourteen binding decisions in
`docs/directives/f9-00-cd-setup-contract-rework.md`. The owner keeps the goal and
El Torito hard-disk emulation as the boot mechanism. The visual mockup of every
screen is the private canvas "CiukiOS Setup from CD" (12 artboards, 640×480);
this document is the contract the directives f9-01…f9-06 are written from.
Nothing here is implemented yet. Section 15 lists the open points the lead must
close before f9-01 and f9-02 launch (two of them come from loader facts found
during the rework, O1 and O2).

Review findings are referenced as F1…F15; lead decisions as D1…D14.

## 1. Goal and scope

- One bootable CD image (`build/f0/ciukios-setup.iso`) built by the canonical
  build beside the disk image, from the same payload set and pins, within the
  capacity of a 74-minute CD-R: **333,000 sectors of 2,048 bytes =
  681,984,000 bytes** ("650 MB"). The check runs on the completed ISO (F5).
- Booting the CD on a machine of the hardware baseline
  (`hardware-baseline.md`: i686 with CMOV, BIOS with E820 and INT 13h
  extensions, ≥ 128 MiB, PATA disk, ATAPI CD-ROM, VBE 2.0+, PS/2) runs
  **Setup**, a native CiukiOS program that installs the system to the hard
  disk in about 15 minutes and restarts into the installed system.
- Setup shares the CiukiOS identity (Ciuki portrait, tagline, English copy,
  Windows 98-class wizard) as `AGENTS.md` requires; the desktop and Setup use
  the same `libdeskui` controls.
- **Beta layout (D3):** Setup installs only the **whole-disk layout on the
  supported boot disk**: ATA disk 0 (primary master), one partition of type
  `0Ch` starting at LBA 2048 with the canonical geometry of `build_image.py`,
  which is the only layout the unchanged loader boots (`ciukldr.asm:36` reads
  partition-table entry 1; `ciukldr/disk.inc:94` requires its start to equal
  LBA 2048; `mount.c:261` binds `C:` to disk 0, partition 1). Whole-disk
  erases everything on that disk, including foreign partitions, extended
  partition containers and the loader gap LBA 1–2047; the confirmation
  screen says so. The "free space" and "advanced" modes appear on screen 5
  **disabled** with the reason ("not available in this Beta") and move to a
  post-Beta directive that first writes a loader/binding contract for other
  partitions and disks.
- Out of scope for the Beta: floppy boot, network installation, upgrading an
  existing CiukiOS in place (the Beta reformats), dual boot with another OS,
  installation to disks other than ATA disk 0, resizing of occupied
  filesystems, disk overlays and non-MBR partition schemes (GPT is rejected by
  `partition.c`), chain-loading the first hard disk from the CD (D11 drops
  that menu entry).

## 2. What exists and what is reused

| Need | Existing piece | Reuse |
| --- | --- | --- |
| Boot from CD | legacy 0.8 `scripts/build_full_cd.sh` (`xorriso -as mkisofs … -hard-disk-boot`), `src/boot/full_cd_mbr.asm` | Method only: El Torito **hard-disk emulation** of a FAT32 image that carries MBR, `ciukldr`, `BOOT.CFG`, kernel and the Setup runtime; the loader runs unchanged except the one token of O2 |
| Read the CD | F4 optical driver (`src/kernel/drivers/optical.c`, `optical_cd.c`, `media_cd.c`, `ata.c` ATAPI path: 2,048-byte sectors, `media_generation`), `src/kernel/fs/iso9660.c`, `mount.c` `storage_add_ro` (ISO volumes from `D:` onward) | The ISO tree is mounted read-only as `D:`; a new **bounded 512-byte block view** of the boot image over the optical device (section 3.3) binds the emulated FAT32 volume as `C:` (D1) |
| Discover disks | `mount.c` `storage_init`/`storage_add_disk` (partition scan, FAT mount, per-volume write counters `writes`/`writes_before_gate`), the existing read-only probe gate (`storage_probe_readonly`: no writer task, no write enable) | Setup mode reuses the read-only gate: discovery without write enable, repair, disk logging or pagefile (D2) |
| Write the disk | ATA driver with DMA (f1-86 evidence), `blkreq.c`, `cache.c`, `partition.c`, `fat.c`, `build_image.py` geometry constants | A **userland `sdk/libdisk`** (MBR writer, FAT32 formatter, loader install, verification) over lead-reserved raw disk nodes with an exclusive lease (D5); no Setup-only disk code in the kernel |
| Install the loader | MBR + `ciukldr` at LBA 1–1023, `BOOT.CFG` (`boot-memory.md`) | Setup writes the same bytes the image build writes, as the **last** phase (D6) |
| Hardware detection | F1 probes: CPU features, E820, VBE, ATA/ATAPI, i8042, AC'97/ES137x, NICs, Savage; the `hw` records | Setup shows the detected set and the baseline verdict before touching the disk |
| Wizard UI | `libdeskui` (controls, dialogs, fonts), the desktop protocol v3 | Setup is a full-screen libdeskui client spawned by the supervisor instead of the desktop in Setup mode |
| Registry and settings | w32srv registry (`libreg`, `/system/settings/*.reg`), Control Panel pages (keyboard, network, clock) | The Copy phase's configuration step writes user, computer name, keyboard, time zone, network mode through the same stores on the destination |
| Component lists | Add/Remove Programs (f2-87) catalogue, licence inventory (`build_image.py`), DOS title battery classes (`dos-title-battery.md`) | The Components screen, the eligibility table (section 10) and the installed licence set come from the same catalogue |
| Accelerator safety | `savage-2d.md`: the kernel writes and commits `accel=attempt` to `C:/system/settings/accel.cfg` before touching the engine and clears it on success; any non-empty marker skips acceleration on the next boot | First boot reuses that marker instead of a new `accel=0` path (D11) |
| Crash-cut tests | runner `crash_cut` cases (`run.py:1686`, `WriteGate`), f1-22/f1-24 fixtures | Installation phases get crash-cut cases that drop unflushed writes (D6) |

## 3. Boot path and volume binding

### 3.1 El Torito hard-disk emulation contract (D4; F4, F5)

Sources: [El Torito Bootable CD-ROM Format Specification 1.0](https://www.ardent-tool.com/storage/specscdrom.pdf)
(§2.2 boot record volume descriptor at sector 17 of the session, §2.3
validation entry with the 16-bit checksum and the `55h AAh` key bytes, §2.4
initial/default entry: boot indicator `88h`, media type, load segment, sector
count, load RBA; §4.3 hard-disk emulation: drive `80h`, the first hard disk
becomes `81h`, geometry from the image's partition table),
[SeaBIOS 1.16.3 `src/cdrom.c`](https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/cdrom.c)
(`cdrom_boot`: validation entry checksum and key bytes, `88h` boot indicator,
media type `4` = hard disk, the emulated drive's CHS geometry taken from the
**ending CHS fields of partition entry 1** of the image's MBR, every emulated
512-byte sector read as `image_lba + sector/4` with the 512-byte slice;
emulated writes return write-protect), [Microsoft FAT specification
(fatgen103)](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf)
§3.5 (the type is decided by the cluster count alone: FAT32 needs ≥ 65,525).

Loader facts (unchanged `ciukldr`, `src/boot/ciukldr/disk.inc:94–160`): it
requires partition 1 at LBA 2048, 512-byte sectors, **8 sectors per
cluster** (byte 13 of the BPB), 160 reserved sectors
(`CIUKI_FAT_RESERVED_SECTORS`), two FATs, FSInfo at 1, backup boot sector at
6, no mirroring flags, a cluster count between 65,525 and `0x0FFFFFF5`, BPB
total sectors ≤ the MBR partition's sector count, a `\SYSTEM` directory, and
it reads with EDD (`AH=41h`/`42h`) when the drive reports extensions,
otherwise with CHS geometry from `AH=08h` (`disk.inc:70–92`). The MBR
(`src/boot/mbr.asm`) has the same EDD-or-CHS choice. **The loader pins the
cluster size, so the 1-sector-per-cluster image (≈ 34 MB) of D4 is not
available: the boot image is the ≈ 270 MB FAT32 image below (see O1).**

Boot image (`build/f0/ciukios-setup-boot.img`, produced by `build_image.py`):

| Item | Value |
| --- | --- |
| Sector 0 | the canonical MBR code (`mbr.asm` bytes identical to the disk image's), disk signature `0x4B554943` (reserved for the Setup CD image; `build_image.py` writes a different signature on disk images and `libdisk` writes a fresh random one on installed disks) |
| LBA 1–1023 | `ciukldr` image, bytes identical to `build/f0/ciukldr.bin` of the same build; LBA 1024–2047 zero |
| Partition entry 1 | active (`80h`), type `0Ch`, start LBA 2048, size 525,384 sectors; start CHS `(0, 32, 33)` and **end CHS computed exactly** for 255 heads × 63 sectors (not the saturated `FE FF FF` markers of the disk image), because SeaBIOS derives the emulated geometry from the end CHS; entries 2–4 zero |
| Partition | FAT32 with exactly the canonical geometry: 512-byte sectors, 8 sectors per cluster, 160 reserved sectors (blackbox descriptor at 8, ring at 32–159, as `build_image.py` lays them out), 2 FATs of 512 sectors, FSInfo 1, backup 6/7, root cluster 2, **exactly 65,525 clusters** (`(525,384 − 160 − 1,024) / 8`); FAT[1] clean bit set, `fsck.fat -n` clean |
| Image size | 527,432 sectors = 270,045,184 bytes = 131,858 ISO sectors (an exact multiple of 4) |
| Content | `\SYSTEM\VMM.ELF`, `\SYSTEM\BOOT.CFG` (the CD boot options, section 3.2), `\Programs\setup`, `libdeskui` and the SDK runtime it needs, `\System\fonts`, `\system\settings` defaults, `\system\assets` (portrait, icons used by Setup), `\system\licenses` texts shown on screen 3, the release manifest copy `\SYSTEM\RELEASE.JSON`; under D1 the payload stays on the ISO tree (O1) |

Boot catalogue (written by `xorriso -as mkisofs -b <image> -c BOOT.CAT
-hard-disk-boot`, verified field by field by the host tests, not trusted from
the tool): validation entry header `01h`, platform `00h` (80x86), checksum
making the 16-bit word sum zero, key bytes `55h AAh`; initial/default entry:
boot indicator `88h`, media type `04h` (hard disk), load segment `0000h`
(the BIOS default `07C0h`), system type `0Ch` (the partition type of entry
1), **sector count 1** (the MBR; the MBR itself loads the loader by INT 13h
from the emulated drive), load RBA = the image's first ISO sector; the image
extent starts on a 2,048-byte boundary and its ISO directory record's data
length equals the image size. The ISO is ISO 9660 level 1 (8.3 upper-case
names, no Joliet, no Rock Ridge; `iso9660.c` reads the primary descriptor and
path table), volume id `CIUKIOS_SETUP`, fixed timestamps from the build
clock (`build-clock.json`), sorted entries: two builds are byte-identical.

Native translation used by the kernel's block view (section 3.3), identical
to the BIOS's: `cd_block = image_start_block + floor(sector / 4)`,
`offset = (sector mod 4) × 512`, bounded by `image_sectors = 527,432`.

Firmware evidence rule (F4): EDD support observed for a physical disk says
nothing about the emulated drive; the qualification runs the loader's EDD
path and the CHS fallback against the emulated drive (QEMU twins through the
runner's `-device ide-cd`, the T23 physically), with reads that cross
4-sector boundaries (the loader reads the FAT and `VMM.ELF` in multi-sector
runs). Other period BIOSes get an explicit qualification limit (section 12).

### 3.2 Boot options and Setup-mode recognition (D2, D11; F2, F12; O2)

`\SYSTEM\BOOT.CFG` on the boot image carries the lead-reserved token
`setup=1` and nothing that the disk image's file does not also accept. The
loader's option parser (`src/boot/ciukldr/menu.inc:430–530`) accepts only
`accel=0`, `safe=0|1`, `serial=0|1`, `mode=0xNNNN` and the `probe=` line
and stops with a fatal selection error on any other token: **`setup=1`
therefore needs the loader to accept the token and keep it in
`boot_info.options`** (about 24 bytes; f0-54 frees ≥ 512 bytes). This is
open point O2: the lead confirms that single parser change (recommended,
and then the loader of the CD, the disk image and the installed disk stay one
binary) or chooses the alternative recorded in O2. The loader shows its
existing 3-second menu with the existing keys (`S` safe video, `L` logging,
`P` probe, `H` hardware report); no ten-second CD menu and no "boot from the
first hard disk" entry exist in the Beta (D11).

The kernel recognises Setup mode from `setup=1` in `g_boot.options`
**before `storage_init` activates anything writable**: in Setup mode the
storage layer starts the way the read-only probe gate starts
(`mount.c:818`): no `storage_enable_write`, no FAT repair, no
`bootlog_activate`, no pagefile bind, no blackbox bind, and every ATA disk is
discovered read-only; the writer task runs only for the destination volume
published later (section 6). The supervisor spawns `/Programs/setup` from
`C:` instead of `/Programs/desktop` and sets the Setup capability on that
process (section 5.3). `CBI_F_TEST_REQUEST` boots keep their current meaning
and never enter Setup mode.

Refusals before graphics (D11): a CPU without CMOV is refused by the
loader's existing `cpu_minimum` text message (and serial), unchanged. Less
than 128 MiB usable RAM (E820 type 1 sum) in Setup mode is refused by the
kernel's console before any process starts, with the text "CiukiOS Setup
needs at least 128 MB of memory" and the serial record
`[setup] refused reason=memory usable_kib=<n>`; the kernel halts after the
message. Screen 12 of the mockup (the graphical stop screen) is shown only
for refusals the kernel can evaluate once Setup runs (unsupported video,
missing ATA disk 0, no ATAPI drive with the boot CD, unsupported disk size).

### 3.3 Volume binding in Setup mode (D1; F1)

The kernel adds a read-only, bounded **512-byte block view** (`cdview`)
over the native optical device of the boot CD: `struct blkdev` with
`sector_size = 512`, `capacity = image_sectors`, `read` only (`write` and
`flush` NULL, so `fat_mount` is read-only by construction), the translation
of section 3.1, a bounded bounce buffer (≤ 64 KiB, runs of whole 2,048-byte
blocks through the optical `request_read` path), and the optical device's
`media_generation` captured at bind: a generation change quarantines the
view (the user is told to reinsert the Setup CD; section 7.4). It streams
from the disc; the image is never copied to RAM (F1, D9). `blkpart.c:64`'s
rejection of 2,048-byte parents stays; the view is its own device, not a
`blkpart`.

The boot CD is identified without BIOS drive numbers: for each ATAPI device
with ready media, the kernel reads the session's sector 17 (boot record
volume descriptor), the boot catalogue, the initial entry's load RBA,
and the MBR at that RBA; a candidate qualifies when the disk signature is
the reserved Setup value, partition entry 1 is `0Ch` at LBA 2048, the FAT32
BPB passes `fat_mount`, `\SYSTEM\BOOT.CFG` on it equals `g_boot.options`
byte for byte, and `\SYSTEM\VMM.ELF`'s build identity equals the running
kernel's. The first qualifying device becomes the boot CD; none → the kernel
logs `[setup] media missing` and shows the console message "Insert the
CiukiOS Setup CD and restart" (no Setup process; no writes).

Drive letters in Setup mode: `C:` = the boot image view (read-only; the
system volume for the loader/kernel bytes, `/System`, `/Programs/setup`,
fonts, settings defaults and licence texts), `D:` = the ISO 9660 tree of the
same disc (read-only; payload `\CIUKIOS` under D1, `\LICENSES`,
`\SOURCES`, `\SETUP.INF`), `E:` onward = FAT volumes found on the ATA disks
during read-only discovery (shown on screen 5 as the contents that will be
erased), then the destination volume once published (section 6, "the first
free letter"; `libsetup` records it, the UI never calls it `C:`). NTFS
volumes are listed read-only as today. USB FAT sticks keep their current
removable letters (the unattended answer file may come from one).

### 3.4 Media budget (F5; O1)

Measured on the canonical image of 2026-10-21 (`build/f0/build-manifest.json`,
2,069 payload files): **485,190,283 bytes** of payload (`WINDOWS/SYSTEM`
167.1 MB, `system/dos` 161.9 MB, `Games` 27.3 MB, `System/cache` 20.3 MB,
`System/sdk` 15.8 MB, `System/Win32` 14.5 MB, `Programs/netsurf` 12.3 MB,
`system/VMM.ELF` 8.5 MB, wallpapers 8.4 MB, fonts 8.8 MB, the rest ≈ 40 MB).

| Component of the ISO | D1 as written | O1-A (payload inside the boot image) |
| --- | --- | --- |
| Boot image (FAT32, 8 sectors per cluster, ≥ 65,525 clusters) | 270.0 MB, mostly empty clusters | ≈ 493 MB (payload + FAT slack + directories + FATs + 1 MiB gap) |
| `\CIUKIOS` payload tree | 485.2 MB + ISO directory overhead | — (not duplicated) |
| `\SOURCES` (section 10) | ≈ 50 MB (estimate: CiukiOS tree ≈ 20 MB uncompressed, unified DOS kernel fork, Wine 10.0 tarball 33 MB, jitterentropy) | ≈ 50 MB |
| `\LICENSES`, catalogue, manifest, sample answer file | < 2 MB | < 2 MB |
| ISO 9660 structures | < 1 MB | < 1 MB |
| **Total** | **≈ 807 MB — exceeds 681,984,000 bytes by ≈ 125 MB** | **≈ 546 MB, ≈ 136 MB headroom** |

D1 as written does not fit because the loader forces the 270 MB image and
ISO 9660 has no sparse files (F5). O1 asks the lead to choose: **O1-A** puts
the payload set (same paths, same bytes) in the boot image's data clusters,
which exist anyway, and leaves the ISO tree with `\LICENSES`, `\SOURCES`,
the manifest and the answer-file sample; Setup then copies `C:` → destination
through the same VFS copy path, and `D:` stays the ISO tree. **O1-B** keeps D1
and reduces the payload shipped on the CD (not possible without dropping
required components). **O1-C** changes the loader to accept 1 sector per
cluster (34 MB image) — against the unchanged-loader decision and still
leaves 485 MB on the tree (fits: ≈ 571 MB). The directive texts below are
written for O1-A where the two differ and say so; the lead's answer fixes
them before f9-01 launches.

## 4. Setup program (`apps/setup/`)

Screens (mockup numbering): 2 Welcome, 3 Licences (texts from the licence
inventory on `C:\system\licenses`; the GPL-2.0-or-later notice and the
source archives of section 10), 4 Hardware check (verdict against the
baseline; the graphical stop screen 12 only for the cases of section 3.2),
5 Destination disk (ATA disk 0 with model, serial, capacity, existing
partitions and the volumes found on them; "free space" and "advanced"
disabled with the reason; the whole-disk layout drawn; the data-loss
warning naming every partition that will be erased), 6 Components (system
always; DOS personality, Windows personality, NetSurf, bundled games,
developer tools, soundfont, "install the source archives"; sizes from the
catalogue, FAT overhead included), 7 User and computer (name, computer name
for the LAN, keyboard, time zone and clock; the RTC and settings are
**written in the Copy phase's configuration step**, never before: screen 7
only collects answers), 8 Network (DHCP or manual; IPX for DOS games; the
Windows game lobby), 9 Copy (the transaction of section 6 with progress and
"did you know" tips), 10 Restart (eject or remove the CD, 15 s countdown),
11 First boot (section 9).

**Final transaction summary (D8; F9):** the last page of the Components
screen (screen 6, "Ready to install") shows, before any write: disk identity
(model, serial, capacity), the partition table fingerprint and the list of
partitions and volumes that will be erased, the required space (payload of
the selected components + FAT allocation overhead + the pagefile reserve =
the pager's initial 16 MiB extent + its growth reserve `max(16 MiB, 5 % of
the volume) + 16 MiB`, `boot-memory.md`), the free space after
installation, the source volumes (`C:`, `D:`) and their manifest hashes, and
the unattended answers in use. "Install" on that page is the only action
that starts the transaction; keyboard: `Tab`/`Enter`/`Esc`/`F1`/`F3`
(quit)/`F10` everywhere; 640×480 is the design size.

Rules: no writes to any ATA disk before the Install action of the summary
page (an integration assertion keeps every ATA disk's write counter at zero
through discovery, preview, cancel and refusal; D2); every write path is the
production kernel's through `libdisk`/the VFS (no Setup-only disk code in
the kernel); the installation log (`\SYSTEM\SETUP.LOG`) is a report written
during the configuration step from an in-memory ring (it is **not** the
durability mechanism; section 6 is); cancelling during Copy leaves the
documented incomplete state; keyboard-only operation works everywhere; no
optional personality, soundfont or game is launched during installation
(D9).

**Memory (D9; F10):** pre-format Setup is RAM-only: no pagefile (the pager
stays in its RAM-only admission state as on diagnostic boots), bounded
streaming buffers (copy and verification buffers ≤ 256 KiB in total,
allocated once at start), the cache adaptive sizing of `cache_initial_bytes`
at 128 MiB, no RAM disk of the payload. Allocation failure in Setup is a
stop with the error dialog, never a silent degradation. The budget is
measured at 128 MiB (`qemu-desktop-1998`) as an f9-06 acceptance item: the
`[mem] ledger` snapshot at the Copy screen shows ≥ 16 MiB free, the Setup
process ≤ 8 MiB resident, and the copy completes without an allocation
failure.

## 5. Destination disk contract (D3, D5, D7; F3, F6, F8)

### 5.1 Supported destination and layout

- ATA disk 0 (the device the loader and `mount.c` treat as the system disk),
  512-byte sectors, capacity ≥ 1,048,576 sectors (512 MiB; the canonical
  geometry needs ≥ 65,525 clusters and the Beta payload needs ≈ 600 MB with
  the reserve, so the UI's minimum is **1 GiB**) and the installed partition
  must end within the 28-bit LBA limit `partition.c:13` enforces
  (`start + count ≤ 2^28` sectors = 128 GiB): larger disks get a partition
  of exactly `2^28 − 2048` sectors and the rest stays unallocated, stated on
  screen 5.
- Layout: MBR at 0 (the canonical `mbr.asm` code, a fresh random disk
  signature, partition entry 1 active `0Ch` at LBA 2048 with the size above
  and the saturated CHS markers `build_image.py` writes, entries 2–4 zero),
  loader at 1–1023, 1024–2047 zero, FAT32 at 2048 with the canonical
  geometry (512-byte sectors, 8 sectors per cluster, 160 reserved sectors
  with the blackbox descriptor at 8 and the 128-sector ring at 32–159, two
  FATs, FSInfo 1, backup 6/7, root at 2), volume label `CIUKIOS`, FAT[1]
  clean bit set at publication.

### 5.2 Deliberate differences from Windows 98 FDISK/FORMAT (D7; F8)

Source: Windows 98 Resource Kit, chapter 10 "Disks and File Systems"
(`build/rskt/wrkc10_1.htm` "Overview of Disks and File Systems", table 10.2
cluster sizes; `wrkc10_5.htm` "Disk Management": partitioning with Fdisk,
physical-disk selection, "delete all existing partitions … create a new
primary partition and make it active", Fdisk's "Verifying drive integrity"
check of the system/FAT tracks, reboot after repartitioning, Format with the
destructive warning; `wrkc10_6.htm`: the hard-sector-error flag that starts
ScanDisk with a surface scan on the next boot).

| Topic | Windows 98 FDISK/FORMAT | CiukiOS Setup (policy) |
| --- | --- | --- |
| Partition start | first partition at head 1 (LBA 63, CHS-aligned) | LBA 2048 (1 MiB) so the loader gap 1–2047 holds `ciukldr`; the only start the loader accepts |
| Cluster size | table 10.2: 4 KiB up to 8 GB, 8 KiB to 16 GB, 16 KiB to 32 GB, 32 KiB above | fixed 4 KiB (8 sectors) at every size: the loader pins it and the fs suite's fsck/mtools oracles assume it |
| Reserved sectors | 32 typical (FAT spec) | 160, blackbox descriptor and ring inside them (`build_image.py`; `boot-memory.md` corrected by this rework) |
| Partition types | primary, extended, logical; active flag chosen by the user | one primary, active; no extended/logical; foreign entries are erased by whole-disk |
| Table refresh | reboot after repartitioning | the kernel invalidates the disk's cache and rescans the table under the lease before publishing the new volume, no reboot |
| Surface check | Fdisk verifies the tracks that will hold the system files and the FAT ("Verifying drive integrity", `wrkc10_5.htm`); ScanDisk runs a surface scan after a hard-sector error is flagged (`wrkc10_6.htm`); MS-DOS FORMAT's bad-cluster marking is not cited in chapter 10 | **quick format only, no surface scan**; a read or write error on any written or verified sector is a refusal of the disk ("Setup cannot use this disk: sector error at LBA n"), never a bad-cluster mark (D7) |
| Resizing | not offered | not offered; occupied filesystems are never resized |
| Disk overlays, compression (DriveSpace) | supported by Windows 98 | unsupported; a disk with an overlay or a non-MBR scheme (GPT `0xEE`) is refused |
| LBA limit | INT 13h extensions | 28-bit LBA per `partition.c`; the partition is capped, the rest stays unallocated |
| Destructive confirmation | Fdisk/Format warnings | the final transaction summary (section 4) with the disk identity and the erased partitions; unattended needs `EraseDisk=yes` |

### 5.3 Raw-disk ownership: the destination lease (D5; F6)

- Lead-reserved raw device nodes `/dev/ata0`…`/dev/ata3` (whole disk,
  512-byte sectors, bounded transfers through the block request layer with
  the writer budget of `win98-class-resource-management.md` C1/C2) and the
  ioctl group `0x44xx` (`DISK_GET_INFO`, `DISK_LEASE_ACQUIRE`,
  `DISK_LEASE_RELEASE`, `DISK_FLUSH`, `DISK_PUBLISH`, `DISK_LEASE_STATE`;
  the lead assigns the final numbers before merge). Reads of the nodes are
  allowed to every process that may enumerate disks; **every mutating
  operation** (write, flush, `DISK_LEASE_*`, `DISK_PUBLISH`, any future
  ioctl that changes state) requires the Setup capability **and** a held
  lease on that disk.
- The Setup capability is a process flag the supervisor sets only on the
  process it spawns as Setup in Setup mode (the same mechanism as
  `supervisor_dosvm_delegate`, but bound to that spawn, not to the path;
  F6's mismatch between `/Programs/setup` and the ISO path disappears
  because Setup runs from `C:`). It is never inherited by children, cleared
  on `exec`, not transferable with descriptors (a descriptor of `/dev/ataN`
  received by another process is read-only there), and revoked at process
  death, which releases the lease.
- `DISK_LEASE_ACQUIRE(disk)` is exclusive: it fails with `EBUSY` while any
  volume on that disk is mounted writable, has open handles, dirty cache
  pages, a pagefile or a disk log, or an active lease; on success the kernel
  detaches every volume of that disk (`storage_remove_removable_locked`
  semantics for fixed disks, in Setup mode all of them are read-only so
  nothing is lost), invalidates the cache for the device, marks the disk
  `leased` so discovery and media services skip it, and records the lease in
  the kernel log (`[disk] lease disk=0 pid=<n> state=held`). The lease is
  released by `DISK_LEASE_RELEASE`, by process death, or implicitly by
  `DISK_PUBLISH` success (which converts it into the published volume's
  ordinary write enable for the holder only).
- `DISK_PUBLISH(disk)` after a format: the kernel invalidates the disk's
  cache and partition state, rescans the table, mounts partition 1 (FAT32)
  writable **for the lease holder only** at the first free letter with the
  writer task active for that volume, and returns the letter. Other
  processes see it read-only. On failure the lease stays held and the
  installation stays incomplete.
- Dry-run plan: `libdisk` produces the exact sector plan (every LBA range
  and its content hash) with the disk identity (model, serial, capacity),
  the partition-table fingerprint (SHA-256 of the MBR and every EBR read
  during discovery) and the affected ranges; Setup shows it on the summary
  page; **execution revalidates identity, capacity and fingerprint
  immediately after acquiring the lease** and refuses with "the disk has
  changed since the preview" when they differ.

## 6. Installation transaction (D6; F7) and the shared interface (D14)

### 6.1 Persistent phases

| Phase | Writes | Durable marker after the phase | State if power is lost inside the phase |
| --- | --- | --- | --- |
| P0 Plan | none | none | disk untouched (write counters zero) |
| P1 Partition | the **stub MBR** (section 6.2) with the new table and a fresh signature; LBA 1–2047 zeroed; flush barrier | stub MBR present | disk shows the stub message at boot; old data unreadable (table replaced); reinstall from CD |
| P2 Format | reserved sectors, FATs, root directory, blackbox descriptor and ring, backup boot sector; `\SYSTEM\SETUP.STATE` = `phase=format`; flush | `SETUP.STATE` on the volume | as P1 |
| P3 Copy | files of the selected components through the VFS into the published volume; after every file: `fsync`; after the phase: volume flush, then **verification by reads that bypass the cache** (raw `/dev/ata0` reads of the file's clusters through the FAT chain, hashed against the manifest), `SETUP.STATE` = `phase=copy done=<n>`; flush | `SETUP.STATE` | stub MBR still present; reinstall |
| P4 Configure | registry/settings files, `SETUP.LOG`, `HARDWARE.TXT`, RTC set from the answers, network settings; `fsync` each; `SETUP.STATE` = `phase=configure`; flush | `SETUP.STATE` | as P3 |
| P5 Publish | native FAT consistency check of the volume (section 6.3); loader image at LBA 1–1023 and `\SYSTEM\BOOT.CFG` (without `setup=1`), read back raw; FAT[1] clean bit; `SETUP.STATE` = `phase=installed release=<sha> firstboot=pending`; flush; **the final canonical MBR last**, flush, raw read-back | canonical MBR | before the final MBR write: stub message, reinstall; after it: installed |

Every flush is `blkdev.flush` of the disk (the ATA driver's qualified
durability path, `blkdev.h:22`); a flush error is a hard stop ("the disk did
not confirm the write") that leaves the install incomplete. Automatic
rollback is not promised for the Beta; a clean reinstall from the CD is the
documented recovery at every phase, and Setup detects a stub MBR or a
`SETUP.STATE` other than `installed` on disk 0 and says "A previous
installation did not complete" on screen 5.

### 6.2 Incomplete-install marking

The stub MBR (`src/boot/setup_stub_mbr.asm`, ≤ 446 bytes of code, assembled
by the build, bytes recorded in the manifest) prints "CiukiOS Setup did not
complete. Insert the Setup CD and restart." and halts; it carries the real
partition table and the final disk signature, so the installed volume is
visible to tools but nothing boots it. Publication replaces only the code
bytes with the canonical MBR's.

### 6.3 Verification and consistency

- Per-file verification: SHA-256 of raw reads (cache bypassed) equals the
  manifest; sizes, attributes and 8.3/long names compared through the VFS.
- Native FAT consistency check before publication: the kernel's mount-time
  scan (`fat.c` full-sector mount scan, cross-link and lost-cluster
  detection, `FAT_RO_*` reasons) run by `libdisk` through a read-only
  remount request of the published volume: any `FAT_RO_CORRUPT`,
  `FAT_RO_COPIES`, `FAT_RO_LOST` or `FAT_RO_IO_FLAG` reason is a failure.
  Host `fsck.fat` and mtools stay the test oracles (not runtime tools).
- Crash-cut tests (runner `crash_cut` write gate) drop unflushed writes at
  the end of P1, P2, P3, P4 and before/after the final MBR of P5, then boot
  the destination: the expected outcomes are the table above.

### 6.4 The shared transaction/progress/error interface (`sdk/libsetup`)

Frozen here for f9-03 (consumer) and f9-04 (implementer); the header is
`sdk/libsetup/include/setup_txn.h`:

```c
enum setup_phase { SETUP_PHASE_PLAN, SETUP_PHASE_PARTITION, SETUP_PHASE_FORMAT,
    SETUP_PHASE_COPY, SETUP_PHASE_CONFIGURE, SETUP_PHASE_PUBLISH, SETUP_PHASE_COMPLETE };
enum setup_error_class { SETUP_ERR_NONE, SETUP_ERR_SOURCE_READ /* retryable */,
    SETUP_ERR_SOURCE_MISMATCH, SETUP_ERR_MEDIA_REMOVED /* retryable after reinsert */,
    SETUP_ERR_DEST_WRITE, SETUP_ERR_DEST_FLUSH, SETUP_ERR_DEST_FULL, SETUP_ERR_DEST_CHANGED,
    SETUP_ERR_LEASE, SETUP_ERR_CONSISTENCY, SETUP_ERR_CANCELLED, SETUP_ERR_MEMORY, SETUP_ERR_INTERNAL };
struct setup_disk_identity { char model[41], serial[21]; uint64_t sectors; uint8_t table_fingerprint[32]; };
struct setup_plan {            /* produced by setup_txn_prepare(), shown on the summary page */
    struct setup_disk_identity disk; unsigned erased_partitions; struct { uint8_t type; uint64_t start, count; char label[12]; } erased[16];
    uint64_t partition_start, partition_count; uint64_t required_bytes, overhead_bytes, reserve_bytes, free_after_bytes;
    uint32_t components; /* bitmask over the catalogue */ struct setup_answers answers; char source_image_sha[65], source_iso_sha[65];
};
struct setup_progress { enum setup_phase phase; const char *step; uint64_t bytes_done, bytes_total; uint32_t files_done, files_total; uint32_t elapsed_ms, eta_ms, throughput_kbps; };
struct setup_error { enum setup_error_class cls; int err; enum setup_phase phase; char path[260]; uint64_t lba, count; unsigned attempt; bool retryable; };
enum setup_decision { SETUP_DECISION_RETRY, SETUP_DECISION_CANCEL };
struct setup_callbacks {
    void (*progress)(void *ctx, const struct setup_progress *);
    enum setup_decision (*on_error)(void *ctx, const struct setup_error *); /* retryable errors only; bounded by libsetup */
    bool (*cancel_requested)(void *ctx);
    void (*log)(void *ctx, const char *line);                               /* the SETUP.LOG ring */
};
int setup_txn_prepare(const struct setup_answers *, struct setup_plan *out);       /* no writes; dry run through libdisk */
int setup_txn_execute(const struct setup_plan *, const struct setup_callbacks *, void *ctx); /* P1..P5; returns 0 or -(setup_error_class) */
int setup_txn_last_error(struct setup_error *out);
int setup_txn_state(enum setup_phase *phase);                                      /* after a failure or cancel: the documented state */
```

Rules: `on_error` is called at most `SETUP_RETRY_MAX = 5` times for the same
operation; `SETUP_ERR_SOURCE_READ` and `SETUP_ERR_MEDIA_REMOVED` are the only
retryable classes; **no "skip"** exists for required files (D8); a failed
source read after the retries leaves the installation incomplete at the
current phase; `cancel_requested` is polled between files and between
sector runs; cancel during P1–P5 stops at the next safe point and leaves the
documented state. The host tests of f9-03 use a fake `libsetup` backend
that replays scripted progress and errors; the host tests of f9-04 use a
file-backed disk model and a fake source volume.

## 7. Errors on the source media (D8; F9)

- A CD read error (`EIO` from the view or the ISO volume) → the error dialog
  "Setup cannot read the CD" with **Retry** and **Cancel**; Retry re-reads the
  same range; after 5 failures Cancel is the only choice.
- Media removal or a `media_generation` change → "Insert the CiukiOS Setup CD"
  with Retry/Cancel; on Retry the kernel rebinds the view and the ISO volume
  and Setup checks that the reinserted disc's `\SYSTEM\RELEASE.JSON` and the
  ISO manifest hash equal the ones recorded at start; a different disc is
  `SETUP_ERR_SOURCE_MISMATCH` (not retryable); handles are reopened after the
  rebind (the VFS invalidates descriptors of a detached volume).
- A hash mismatch of a source file is `SETUP_ERR_SOURCE_MISMATCH`: not
  retryable, the installation is incomplete.
- Destination full (`ENOSPC`, should not happen after the preflight) and
  write/flush errors stop the installation (incomplete state).

## 8. Unattended installation (D10; F11)

`SETUP.INF` is searched first at the root of the ISO tree (`D:\SETUP.INF`,
absent on the distributed ISO; the sample is `D:\SETUPINF.TXT`), then on the
first FAT USB volume in letter order (`\SETUP.INF`). Schema version 1, strict:
unknown sections or keys, duplicates, missing required keys and values out
of range stop Setup with the status below. The same installation state
machine runs interactively and unattended (the answers pre-fill the screens;
unattended skips the pages whose answers are complete and valid).

```ini
[Setup]
Version=1
AcceptLicences=yes            ; required: yes
EraseDisk=yes                 ; required whenever the destination holds any partition or filesystem; explicit consent to erase
Disk=ata0                     ; ata0 (the only installable disk in the Beta) | model="<IDENTIFY model>" serial="<serial>" | auto
Reinstall=no                  ; yes allows erasing a disk whose SETUP.STATE says installed with the same release
Components=dos,windows,netsurf,games,devtools,soundfont,sources   ; system is implicit; names from the catalogue
UserName=<1..32 chars>
ComputerName=<1..15 chars, LAN rules>
Keyboard=us                   ; a layout id of the keyboard page
TimeZone=+01:00               ; offset; Clock=local|utc (RTC convention; default local)
Network=dhcp                  ; dhcp | static:<ip>/<prefix>,<gateway>,<dns> | none
IPX=yes
Restart=eject                 ; eject (default) | wait (wait for removal, no timeout) | force (restart after 60 s even with the CD inserted)
```

Rules: `Disk=auto` selects `ata0` only when it is the **only** ATA disk
present; with more disks it stops with `status=ambiguous_disk`. A
`model`/`serial` selector must match `ata0` exactly, else
`status=disk_not_found`. A destination with a `SETUP.STATE` of `installed`
for the same release stops with `status=already_installed` unless
`Reinstall=yes` (this is what stops a reinstall loop when the machine
reboots with the CD inserted). Missing answers in unattended mode stop with
`status=missing_answer key=<k>` (no timeout waits for a user). Every stop
shows the status on screen, writes the serial record
`[setup] status=stopped reason=<status> ...`, and, when the answer file came
from a writable USB volume, writes `\SETUP.STS` beside it. Network absence,
DHCP failure or lobby failure never block completion: the installed system
keeps the configured mode and reports at first boot. The installed
`BOOT.CFG` never contains `setup=1`. Restart: Setup sends
`MEDIA_CD_TRAY` open to the boot drive, waits for the media-removed state
up to 60 s, then restarts (`Restart=wait` waits without limit; `force`
restarts anyway); the qualification covers a reboot with the CD still
inserted (section 12).

## 9. First boot and rescue (D11; F12)

- The installed `SETUP.STATE` says `firstboot=pending`; the supervisor
  spawns the desktop normally, and the desktop runs the first-boot wizard
  (screen 11) before the ordinary session. Each finishing step is
  idempotent and records its own line in `SETUP.STATE` (`step=<name>
  result=<ok|skipped|failed>`), `fsync` after each; the wizard resumes at the
  first step without a result.
- Accelerator: the kernel's own marker (`accel=attempt` in
  `C:/system/settings/accel.cfg`, written and committed before the engine
  is touched, cleared on success; `savage-2d.md`) is the "test in progress"
  marker: a hang or reset leaves it set and the next boot runs without
  acceleration. The wizard shows the outcome (accelerated, or "software
  rendering is in use; retry from Control Panel") and offers the retry that
  Control Panel already provides. No `accel=0` edit of `BOOT.CFG` is made.
- Sound and network checks are bounded (≤ 10 s each) and never block
  completion; bundled game registration is idempotent; entropy collection
  reports the `[entropy]` readiness.
- RTC: the Copy phase's configuration step writes the RTC from the answers
  (local or UTC convention, `Clock=`), and first boot shows the resulting
  time with a "Change" link to the clock page; HTTPS needs that clock
  (f6-15).
- Completion: `SETUP.STATE` = `firstboot=done`, then the desktop. The state
  file is never deleted (it is the install record; Add/Remove shows it).
- Rescue console (keyboard `R` is not added to the loader; D11): the Setup
  program offers "Rescue" on the Welcome screen: read-only tools (list
  disks and volumes, copy `SETUP.LOG`/`HARDWARE.TXT`/the disk log to a USB
  stick, show the hardware report), no writes to ATA disks.

## 10. Components, licences and corresponding source (D12; F13, F14)

Eligibility is per component and fails closed: a component without a row
in `config/cd-components.json` (new; f9-01 creates it from the catalogue) is
not written to the ISO. Each row: component id, payload paths, licence class
(`free`, `copyleft`, `bundled-shareware`), licence texts, source archives
required, and the decision reference.

| Component | Class | Conditions recorded |
| --- | --- | --- |
| System (kernel, loader, native programs, libdeskui, SDK runtime) | copyleft GPL-2.0-only (kernel `SPDX-License-Identifier`), MIT/BSD parts | complete corresponding source of the exact build on the ISO (`\SOURCES\CIUKIOS.ZIP`: `src/`, `sdk/`, `apps/`, `scripts/`, `config/`, `patches/`, `Makefile`, `LICENSE`, the build pins) |
| DOS personality (unified DOS kernel fork, FreeCOM, tools) | copyleft GPL-2.0-or-later (dev diary 2026-10-21-04, `unified-dos-licenses.tsv`) | the f3-55 corresponding-source archive (fork, patches, interface files, build scripts) on the ISO (`\SOURCES\UNIDOS.ZIP`) **whenever the kernel binary ships**, independent of the "install the source archives" checkbox |
| Windows personality (Wine-derived DLLs, LGPL-2.1-or-later; CiukiOS DLLs) | copyleft LGPL | the pinned Wine 10.0 source archive and the CiukiOS sources (`\SOURCES\WINE100.TXZ`, part of `CIUKIOS.ZIP`), `SOURCE-OFFER.txt` and `COPYING.LIB` in `\LICENSES` (see O3) |
| NetSurf, lwIP, BearSSL, FreeType, TinyGL, stb, minimp3, newlib, Lua, Liberation fonts, Tango icons, FluidR3 | permissive / FTL / OFL / public domain | notices and licence texts in `\LICENSES` and on the installed `\system\licenses`; jitterentropy ships under its BSD option with both texts |
| DOOM 1.9 shareware | `bundled-shareware` (`dos-title-battery.md`) | the four `DMFAQ66*.TXT` files stay excluded; the unmodified archive is installed as today, never re-expanded wholesale |
| Quake 1.06 shareware | `bundled-shareware` | `SLICNSE.TXT` §6–7: whole unmodified package, free of charge, agreement accompanying it; the engine's later GPL release does not cover the data; recorded as the owner's bundling decision |
| Wolfenstein 3D 1.4 shareware | `bundled-shareware` | stays bundled by the owner's decision (dev diary 2026-10-21-05); the row records **"Apogee permission for bundles/CD-ROM (VENDOR.DOC [3][C]) not on file"** |
| Soundfont (FluidR3, MIT) | free | licence and authors files |

GPLv2 §3(a) ([GPL-2.0-only](https://spdx.org/licenses/GPL-2.0-only.html),
§§1–3): every copyleft binary on the ISO is accompanied by its complete
corresponding source on the same medium — the exact shipped fork,
modifications, interface definition files and the scripts used to control
compilation and installation; a hash or a repository link is not a §3(b)
offer. Copyright, warranty, modification-date and licence notices are
preserved. The "install the source archives" checkbox only decides whether
`\SOURCES` is also copied to the destination. The build fails when a
copyleft component is present without its source archive (no "recorded
reason" tolerance; the f9-01 text of 2026-10-21 that tolerated a missing
unified-DOS source is withdrawn).

## 11. Directives

| Directive | Scope | Owner |
| --- | --- | --- |
| f9-01 CD media and boot | `build_image.py --cd`: boot image, catalogue, ISO tree, `\SOURCES`, `\LICENSES`, component eligibility file, manifest and size check; kernel: Setup-mode recognition, read-only storage start, `cdview`, boot-CD identification, `C:`/`D:` binding, memory refusal; placeholder Setup; host tests; `f9-cd-smoke` | gpt-6.1-sol / xhigh, `--search` (or a Claude Opus agent) |
| f9-02 Native disk tools | kernel: `/dev/ata0..3`, capability, lease, publish; `sdk/libdisk`: enumerate, plan, stub/canonical MBR, FAT32 formatter, loader install, raw verification, dry run; host tests against the fs suite and `build_image.py` as oracles | gpt-6.1-sol / xhigh, `--search` (or a Claude Opus agent) |
| f9-03 Setup application | `apps/setup`: the screens, keyboard navigation, hardware verdict, summary page, error dialogs, `SETUP.INF`, rescue page; host render tests with a fake `libsetup` | gpt-6.1-sol / high (or a Claude Sonnet agent) |
| f9-04 Copy and configure | `sdk/libsetup`: the transaction of section 6 over `libdisk` and the VFS, verification, configuration writes, `SETUP.LOG`/`HARDWARE.TXT`, restart path; host tests with the disk model | gpt-6.1-sol / xhigh (or a Claude Opus agent) |
| f9-05 First boot and rescue | the first-boot wizard in the desktop, idempotent steps, markers, rescue tools, log export | gpt-6.1-sol / high (or a Claude Sonnet agent) |
| f9-06 Qualification | runner suites (`f9-cd-smoke`, `f9-install`, crash cuts), evidence record in `docs/validation/`, T23 optical install, qualification limits | lead, with Codex/Claude for the suite definitions |

Order (D14): f9-01 and f9-02 start only after O1–O3 are closed and run in
parallel (no UI); f9-03 and f9-04 start after both merge, in parallel,
against the frozen interface of section 6.4; then f9-05; then f9-06. QEMU is
allowed for `f9-cd-smoke`, `f9-install` and the crash-cut cases (owner
decision 2026-10-21-06).

## 12. Acceptance for the Beta (D13; F15) — the f9-06 list

Before the CD is called distributable, on the recorded ISO SHA-256:

1. CD-only boot to Setup with a blank destination (zeroed disk 0) and no
   preinstalled system; `[setup] media ok` with the ISO and image hashes.
2. Unattended install (answer file on a USB fixture), detach the CD, cold
   boot the destination overlay, First boot completes to the desktop
   (normal-boot evidence); the installed `BOOT.CFG` has no `setup=1`.
3. The same on `qemu-desktop-1998` at **128 MiB** with the memory
   measurements of section 4; with **two disks** (the second untouched:
   hash before/after equal); with an **occupied disk 0** (foreign partitions
   and a FAT volume: erased only after the summary page, hashes of the other
   disk preserved); the disabled layout modes shown with their reason.
4. Zero pre-Copy writes: the ATA write counters of every disk stay zero
   through discovery, preview, cancel on the summary page and a hardware
   refusal (an assertion record `[setup] writes disk=<n> count=0`).
5. Interruption cases: crash cuts at the end of P1, P2, P3, P4 and around
   the final MBR of P5 with the outcomes of section 6.1; a source read error
   (fault-injected ISO read) with Retry then Cancel; media removal with a
   matching and with a different disc; disk full (a small fixture disk) and
   a write/flush failure (fault injection) leave the incomplete state.
6. Keyboard-only interactive completion (scripted input through the
   runner) and unattended completion; a reboot with the CD still inserted
   stops with `already_installed` and restarts.
7. T23 optical install evidence (serial capture, the disk logs, photos of
   screens 1, 5, 9, 10, 11) and the **qualification limit**: the E500 and
   generic desktop BIOSes are not qualified for HDD-emulation boot by the
   QEMU twins (they share SeaBIOS); each gets its own entry in the
   compatibility matrix when tested.
8. Licence/source eligibility: every copyleft binary on the ISO has its
   source archive; the eligibility file covers every payload path; the
   exact ISO and destination hashes are recorded.
9. Payload comparison: the **immutable payload subset** of the selected
   components (every manifest path except the generated set) is byte-equal
   to the canonical manifest; the generated set is listed explicitly:
   `\SYSTEM\BOOT.CFG`, `\SYSTEM\SETUP.STATE`, `\SYSTEM\SETUP.LOG`,
   `\SYSTEM\HARDWARE.TXT`, `\system\settings\*` written by configuration,
   `\system\settings\accel.cfg`, the registry files, `\SYSTEM\PAGEFILE.SWP`
   (created by the pager at first boot), the blackbox ring and the disk log.

## 13. Risks

- El Torito variety on 1998–2004 BIOSes (CHS-only emulated drives, geometry
  derived differently from the end CHS, load-segment handling): the loader's
  CHS path is exercised in the qualification and the compatibility matrix
  records each BIOS tested; no isolinux/grub4dos fallbacks are carried over
  unless the qualification shows a need.
- ATAPI drives without DMA: the copy runs in PIO at 2–3 MB/s (≈ 4 minutes
  for 485 MB), acceptable; the progress estimate comes from measured
  throughput.
- RAM at the 128 MiB minimum: measured, not assumed (section 4).
- `libdisk` is the only destructive code in the product: file-backed disk
  model, dry-run equality, crash cuts, the lease and the write-counter
  assertion are its safety net.
- The loader change of O2 is tiny but touches the bytes the disk image,
  the CD and the installed disk share; its host loader-model and the
  existing F0 suites must stay green.

## 14. Sources

- El Torito Bootable CD-ROM Format Specification 1.0 (Phoenix/IBM, 1995):
  https://www.ardent-tool.com/storage/specscdrom.pdf
- SeaBIOS 1.16.3 `src/cdrom.c` (`cdrom_boot`, emulated geometry, read translation):
  https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/cdrom.c
- Microsoft, FAT: General Overview of On-Disk Format (fatgen103):
  https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf
- ECMA-119 (ISO 9660), 4th edition: https://ecma-international.org/publications-and-standards/standards/ecma-119/
- GNU GPL version 2, §§1–3: https://spdx.org/licenses/GPL-2.0-only.html;
  GNU LGPL 2.1 §4 and §6: https://spdx.org/licenses/LGPL-2.1-only.html
- Windows 98 Resource Kit, chapter 10 (`build/rskt/wrkc10_1.htm`,
  `wrkc10_5.htm`, `wrkc10_6.htm`; owner's media, not redistributed)
- Repository: `src/boot/ciukldr/disk.inc`, `src/boot/ciukldr/menu.inc`,
  `src/boot/mbr.asm`, `src/kernel/fs/mount.c`, `src/kernel/fs/partition.c`,
  `src/kernel/drivers/blkpart.c`, `src/kernel/drivers/ata.c` (ATAPI),
  `src/kernel/proc/supervisor.c`, `scripts/build_image.py`,
  `docs/design/boot-memory.md`, `docs/design/savage-2d.md`,
  `docs/design/dos-title-battery.md`, `docs/design/win98-class-resource-management.md`

## 15. Open points for the lead (close before f9-01/f9-02 launch)

- **O1 — media budget.** The loader pins 8 sectors per cluster, so the boot
  image is 270 MB, and the payload tree is 485 MB: D1 as written exceeds the
  CD by ≈ 125 MB (section 3.4). Recommended: O1-A (payload inside the boot
  image, ISO tree for sources/licences/sample). f9-01 is written for O1-A.
- **O2 — the `setup=1` token.** The unchanged loader rejects unknown
  `BOOT.CFG` tokens fatally; carrying `setup=1` needs a ~24-byte parser
  addition (recommended, within f9-01, with the loader model and the F0
  suites as evidence). Alternative without a loader change: the kernel reads
  the relocated MBR's disk signature at linear `0x7B8` (`mbr.asm` relocates
  to `0x0600`) during early init and enters Setup mode when it equals the
  reserved Setup signature and a qualifying boot CD is found — it depends on
  low memory surviving until the kernel starts and is not recommended.
- **O3 — LGPL delivery.** D12 requires every copyleft binary's source on the
  ISO. LGPL-2.1 §4 (library in object form) likewise asks for the complete
  corresponding source on the medium; the existing `SOURCE-OFFER.txt`
  points to the CiukiOS tree and the Wine tarball. The plan ships the Wine
  10.0 source archive (≈ 33 MB) on the ISO; the lead confirms or decides
  that the CiukiOS tree archive plus the offer text suffices.
- **O4 — directive owners.** Each directive names `gpt-6.1-sol` with a
  Claude agent alternative (Opus for kernel work, Sonnet for UI/glue, per
  the owner's model economy rule); the lead confirms per launch.

## Lead decisions on the f9-00 open points (2026-10-21)

- O1: variant A, the payload lives in the boot image's data clusters (ISO ≈ 546 MB); the ISO tree keeps `\LICENSES`, `\SOURCES`, `\SETUP.INF.sample`.
- O2: the `setup=1` token is parsed by the loader with the ~24-byte addition inside f9-01, after f0-54 frees the budget (f9-01 may stage it last).
- O3: the Wine 10.0 source archive ships on the ISO under `\SOURCES` with the other copyleft sources (`CIUKIOS.ZIP` plus `SOURCE-OFFER.txt` remain as the written offer for anything the ISO cannot carry).
- O4: owners are Claude agents while Codex is unavailable; ioctl numbers `0x4401`–`0x440F` reserved for the disk lease group; the reserved CD signature `0x4B554943` and the 1 GiB minimum destination are accepted; Setup-mode drive letters as the plan states; the frozen `setup_txn.h` is reviewed by the lead at the f9-03/f9-04 launch.
