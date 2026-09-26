# Setup storage audit — 2026-09-06

The user asked for an in-depth review of HDD installation after reporting a
correct root directory but corrupt subdirectories on a cold-booted physical
ThinkPad T23. This audit reproduces and fixes installer/packaging defects. It
does **not** establish that any one of them caused all the physical symptoms.
The actual T23 HDD image has not yet been acquired. No physical HDD was written
during this audit. The final ISO was subsequently burned to CD-RW on the user's
instruction, as recorded below.

Evidence: `build/full/setup-write-audit-2026-09-06/`. The previously burned ISO
is the earlier `ui-setup-repair-2026-09-06/CiukiOS_candidate.iso`, SHA256
`ec79babd5dc6158869ed73c9ad6108a9194d7cf28de62680e95f462faa3eaee8`.

## Reproduced defects and corrections

1. **PIO completion was accepted too early.** `raw_ata_transfer_n` tested BSY
   immediately after the final data word and did not require DRQ to clear. It
   could also change Device/Head while the previous device still had a data
   phase pending. The assembled original routine returns before completion in
   the timed ATA model (`pio-before/`). The repaired routine waits before
   selection, requires BSY/DRQ clear and DRDY before programming a command,
   waits four alternate-status I/O cycles after transferring data, and checks
   BSY/DRQ plus DF/ERR before returning. Read/write payload bytes remain correct
   in both controls. This is an **instruction-level device protocol test**,
   not a measurement of the physical T23 drive's timings.

2. **A failed final cache flush left an active, signed MBR.** An unchanged
   installer from the burned ISO was run in QEMU with a real blkdebug EIO on
   the third HDD flush (initial invalidation, payload, final MBR). It reached
   the error page, reported `FAIL S=F ... L=0000:0000 AH=41`, but its target
   retained an active partition and signature 55AA. See `commit-before/` and
   `observed-failure.json`. Final MBR write, flush and readback failures now
   invoke rollback: zero sector 0, flush it, and compare its readback. The
   original error/location are retained; rollback success or failure is logged
   separately. Both `commit-after/` (KVM) and `commit-final/` (TCG) observe the
   real error page and a zero MBR after `ROLLBACK VERIFIED`.

3. **The source FAT image contained a cross-link.** `fsck.fat -n` reports
   APPS/SETUPMFT.BIN and APPS/MANIFST.BIN sharing a cluster; the BPB also named
   a volume label absent from the root. The builder now allocates the second
   manifest separately through mtools, retaining identical file contents, and
   writes the matching root label. `fsck-before.log` exits 1 without modifying
   the image; corrected source and installed partitions exit 0 without repair.
   This cross-link concerns the two manifest files; it does not by itself
   explain corruption of Windows, games or every subdirectory. The packaging
   acceptance test now checks both manifests' independent chains and contents.
   The shared-cluster entry is also present in the checked-in builder; its
   discovery is not evidence that introducing the UI caused this particular bug.

## Additional safeguards

- Source preflight checks the BPB against the actual build layout, in addition
  to the MBR. Bytes/sector, sectors/cluster, reserved sectors, FAT count/size,
  root entries, hidden sectors, total sectors and signatures must agree.
  A test ISO with BPB hidden sectors changed from 63 to 64 still boots the
  real installer but is rejected before confirmation or any target writes
  (`source-bpb-final/`). The entire target hash remains unchanged.
- ATA transfers reject zero/oversized counts, segment wrapping, capacity
  overflow and writes outside the displayed system region before controller
  I/O. Instruction tests include the 65535/65536 LBA boundary. A missing
  authoritative BIOS-to-ATA mapping no longer permits the single-ATA fallback
  when the GUI reports multiple BIOS targets.
- Firmware disk calls preserve caller-owned segments and pointers. Geometry
  still receives the BIOS CX/DX outputs. The legacy manifest reader's relative
  data start is corrected from 359 to 361, matching 73 reserved sectors,
  two 128-sector FATs and a 32-sector root. The graphical clone path copies
  whole sectors and does not use that legacy manifest reader.
- Immediate ATA readback remains a byte-for-byte comparison. After flushing
  the payload, Setup now rereads **all 196671 sectors** through BIOS, including
  the MBR with its signature withheld. A rolling CRC-32/ISO-HDLC is compared
  against the patched source buffers. The CRC implementation is checked against
  Python's independent zlib implementation with different chunk boundaries.
  Only a successful complete readback permits publishing the final MBR.
- A real target read EIO activated only after the payload flush is detected
  during this new BIOS pass (`post-flush-read-final/`). The copied MBR's
  signature remains 0000, and no completion page is displayed. The full pass
  adds HDD reads; it does not perform optical-disc burn verification.

## Installation and runtime qualification

`full-install/` exercises a complete surface clear, readback, installation,
post-flush BIOS verification and a real click on Restart. QMP observes a guest
hardware RESET; the same VM boots the HDD without the CD and executes DOS.
An additional cold HDD boot is tested. Every installed sector is independently
compared with the source, allowing only the expected D: to C: byte and GRUB4DOS
RAM-disk identity. Bytes beyond the system region retain their A5 pattern.

`second-disk-quick/` uses two physical IDE disk devices in QEMU. The selected
second disk receives the FAT16 metadata; its old data region remains unchanged.
The entire unselected disk hash remains unchanged. Both FAT copies and write
bounds are checked independently.

On copies of that actual installed HDD, with no replacement kernel or shell:

- `hdd-paths/`: all 340 entries in eight subdirectories, relative paths,
  complete SBEMU component reads, COM/MZ execution, concurrent PS/2 motion,
  and unchanged kernel code outside the documented XMS hook.
- `hdd-doom/`: original Doom menu navigation, gameplay, audio, exit, subsequent
  DOS execution and kernel-code integrity.
- `hdd-windows/`: two real Windows 3.1 sessions, Calculator, resize/repaint,
  WAV and MIDI playback and return to DOS. Repaint comparison: 0 changed pixels;
  captured digital audio RMS: WAV 1180.3, MIDI 714.1. This does not measure the
  T23's physical speakers.

The final build only changes the source-error wording, removes an inaccurate
generic error-page promise, and routes GUI disk enumeration through the same
firmware wrapper. The final ISO is separately exercised by `install-final/`
(quick install, full verification, real reset and cold boot), `commit-final/`,
`source-bpb-final/` and `packaging-final.log`. Payload comparisons record all
1408 files; only APPS/SETUP.COM changes content from the previously burned build.
Kernel, shell, Windows, games, audio binaries and manifest contents are identical.

Final ISO: `candidate-final.iso`, 149372928 bytes, SHA256
`554f093cec8dfae1ab59b0ba4943bbe5ebb07c0bc2168a1685105417fb876d0c`.
Final Setup: 48432 bytes, inside its existing 49152-byte slot, SHA256
`2d334e05a39898e82ac465f03c4ee05125ff49e77b6863990bc758e855e537b5`.
The final ISO was **burned successfully** to CD-RW on `/dev/sr0` after the user
instructed `riprova`. Xorriso completed fast blanking, DAO writing and session
closure, then exited with code 0. The command requested eject. No optical
readback verification was performed, as requested. Successful write log:
`burn-20260906T201532Z.log` in the evidence directory; completion details are
recorded in `manifest.json`.

Two earlier actual write attempts after `mmassterizza` failed before writing:
libburn reported `No media detected in drive` (exit code 5). The second followed
a successful tray-close command. These failed attempts remain recorded in
`manifest.json` and logs `burn-20260906T201338Z.log` and
`burn-20260906T201420Z.log`.

## Retained unsuccessful test trials

- `commit-before/`: the guest reached the intended error and left a signed MBR,
  but the first harness assertion spelled the log marker COPY_DONE instead of
  COPY-DONE. Its failed result is retained. `observed-failure.json` separately
  records the verified serial, error page and actual target MBR; the corrected
  harness is used for the subsequent positive runs.
- `post-flush-read/`: the first harness expected every MBR byte to be zero.
  During copying, the design retains the source boot code/table with only its
  signature withheld. The correct assertion is an unpublished signature and
  no success page; `post-flush-read-final/` reruns and passes that check.
- `source-bpb/`: changing sectors/cluster prevented GRUB4DOS from reaching
  Setup. It therefore did not exercise installer preflight and is not counted
  as such. Changing only the hidden-sector count reaches the intended check.

References: [ATA-3 working draft, sections 5.2.2 and 5.2.15](https://www.scs.stanford.edu/23wi-cs212/pintos/specs/ata-3-std.pdf),
[Microsoft FAT specification](https://www.scs.stanford.edu/~zyedidia/docs/_other/fat.pdf),
[QEMU blkdebug implementation](https://github.com/qemu/qemu/blob/master/block/blkdebug.c).
