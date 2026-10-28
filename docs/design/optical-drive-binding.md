# F1-48 optical binding and runtime media

Research checked 2026-10-13 before implementation:

- [SFF-8020i revision 2.6](https://www.jmargolin.com/project/SFF8020i.pdf),
  §§5.2–5.5: PACKET PIO and completion reason; §10.6: reset/media-change unit
  attention and REQUEST SENSE; §10.8.19: TOC/session fields;
  §10.8.25: immediate start without tray loading; §10.8.26.1: TEST UNIT
  READY polling, prompt status, no-medium sense.
- [QEMU 9.2 ATAPI model](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/ide/atapi.c):
  the accepted fixture uses SFF session format in byte 9, not MMC byte 2.

Repository trace: ATA IDENTIFY PACKET publishes type 5 and a 12/16-byte packet
device. `ata_device_get(channel, unit)` admits identified, non-quarantined
devices independently of DMA. Slot is `channel * 2 + unit`, so the T23
secondary master is slot 2; this is not the C: drive index. ATAPI bypasses the
512-byte MBR/blkpart path and mounts its 2048-byte data session as ISO 9660.
The f1-44 DMA admission applies only to ATA IDENTIFY, so ATAPI stays on PIO.

The supplied T23 record reaches the optical readiness/mount sequence and fails
before a drive letter is published. A failure in initial `atapi_ready`, an ISO
mount's readiness/generation check, or its packet reads can produce this record.
Generation 2 proves a classified media condition. `reason=unavailable` excludes
the old path's explicit no-medium result; the old record omits the command and
sense, so it cannot distinguish reset unit attention from other not-ready
states. Reset unit attention is a specification-supported hypothesis,
not a confirmed hardware diagnosis. Previously boot made one readiness attempt
and there was no runtime optical service.

Decision: retry only TEST UNIT READY for bounded unit attention (maximum three
commands within the ordinary two-second deadline), never failed READ(10).
For NOT READY 02/04/02, issue one asynchronous START STOP UNIT (IMMED=1,
Start=1, LoEj=0) per generation; later TUR must prove readiness. An empty tray
is never loaded/closed automatically. Immediate start uses the ordinary
two-second packet deadline; explicit non-immediate eject/start keeps ten seconds.
Log command stages, completion status, sense and mount/detach decisions.
Retain identified empty drives and poll each every 1000 ms from a dedicated
device task, separate from FAT writeback. Start it only on ordinary boot, so
the existing explicit ide-cd probe and QMP fixture remain unchanged. A ready,
responding drive is detected and mounted within two seconds; drive spin-up,
hardware command timeouts and scheduler/controller contention can delay usable
media and are visible in records. No transport recovery or BIOS fallback.

The namespace lock serializes polling, publication and eject with filesystem
access, and the existing ATA mutex serializes packets with the ATA peer.
Generation changes invalidate names/cache, cwd and open handles before a new
ISO mount. Refused ISO media are attempted once per generation. Letters use
the next available VFS letter from D:; Joliet uses the existing parser.

`MEDIA_GET_STATE` version 2 appends 26 uint32 type values (none, USB, optical)
to the 120-byte version 1 prefix, for 224 bytes. Version 1 GET_STATE and removal
requests remain supported. Optical D: is included; optical safe removal means
eject after open-handle preflight. Files polls the copied state every 500 ms,
refreshes on changes, and shows the existing Tango optical icon and CD-ROM type.

## F1-54 ISO descriptor compatibility

Research checked 2026-10-14 before implementing the parser corrections:

- [ECMA-119, third edition](https://ecma-international.org/wp-content/uploads/ECMA-119_3rd_edition_december_2017.pdf),
  §§6.7.1, 7.2.3, 7.3.3, 8.3–8.5 and 9.1: descriptors start at logical
  sector 16; repeated primary descriptors, supplementary descriptors and boot
  records are permitted. The sequence ends at its first terminator. Volume
  space size counts logical blocks, and the two endian copies encode the same
  value. Directory records may carry additional System Use bytes.
- [Linux isofs numeric decoding](https://raw.githubusercontent.com/torvalds/linux/master/fs/isofs/isofs.h):
  `isonum_723` and `isonum_733` prefer the little-endian half without comparing
  the big-endian copy, accommodating mastering defects.
- [Linux isofs volume selection](https://raw.githubusercontent.com/torvalds/linux/master/fs/isofs/inode.c),
  `isofs_get_last_session` and `isofs_fill_super`: scan from the last-session
  start plus 16; multisession extents remain absolute disc addresses. Ignore
  unknown descriptors, keep the first PVD, and choose a supported Joliet
  hierarchy. The mount uses directory records rather than validating path
  table copies. A logical block smaller than the physical sector is refused.

Repository comparison: ATAPI already provides 2048-byte sectors and passes
`session_lba` through `storage_add_ro_locked` to the parser. No 512-byte MBR
conversion is involved. In the supplied T23 log, `sectors` means capacity;
`session` is the separate start field, logged before the session response is
applied and again at bind. The readable bind fragment shows `session=0`.
The record establishes a parse refusal, but identifies no failing ISO field
and does not establish that this particular disc is multisession.

Decision: retain absolute extent addressing and scan at the supplied session
start plus 16. Ignore unknown descriptor identifiers/types, unsupported SVD
versions/character sets and repeated PVDs. Parse PVD and Joliet independently,
including when the SVD precedes the PVD; retain the selected hierarchy's own
read bounds. Prefer a valid little-endian value; use the big-endian copy when
the little copy violates the field's supported bounds or alignment. Conflicting
plausible copies cannot identify the original value, so little endian wins.
Unused path tables do not veto a readable root. Oversized declared volumes
are bounded by actual media capacity and the supplied mount end; every root,
directory and file extent still has to fit. Descriptors are scanned within the
session tail independently of either hierarchy's count. Blocks remain 2048
bytes, and missing PVD/terminator, malformed supported Joliet, unsupported
volume sets and unreadable extents are refused. This deliberately retains the
ECMA terminator requirement even though Linux can mount without reaching one.

The native xorriso merge fixture measures session start 512, PVD at 528,
declared volume length 188, primary root at absolute LBA 532 and physical
capacity 700 blocks. Treating 188 as an absolute disc end rejects this valid
root. Use `session_start + min(declared_length, mount_tail_length)` as the
absolute readable end, while leaving extents absolute and allowing inherited
files before the session start. This also accepts masters whose size includes
the disc prefix, clipping at the actual end. The fixture verifies both size
conventions without claiming which one the owner's disc uses.

`[optical] iso` records include session, absolute descriptor LBA, descriptor
name, field and result. Header records show descriptor type; volume records
show declared versus readable block counts. Root record failures identify
PVD/SVD and the offending record field, before the binding refusal.

Host fixtures exercise a nonzero last session, old-session file extents,
Joliet alongside Rock Ridge, an El Torito boot record and supported/unknown
descriptor mixtures. Independent xorriso extraction of the selected Joliet
session provides file names, sizes and SHA-256 expectations. The existing
canonical Joliet fixture generator is unchanged. Hardware confirmation of
the owner's disc remains the lead's T23 mount/open acceptance check.

## F1-58 directory lengths and numeric diagnostics

Research checked 2026-10-14 before implementation:

- [ECMA-119, third edition](https://ecma-international.org/wp-content/uploads/ECMA-119_3rd_edition_december_2017.pdf),
  §§7.2.3, 7.3.3, 8.4.18 and 9.1.2–9.1.4: numeric fields carry
  both byte orders; an extent starts with any extended attribute blocks,
  which are excluded from the file section's data length.
- [Linux v6.12 isofs numeric decoding](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/isofs/isofs.h):
  `isonum_723` and `isonum_733` use only the little-endian copy.
- [Linux v6.12 directory iteration](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/isofs/dir.c):
  `do_isofs_readdir` reads blocks while the position is below the declared
  size, including a partial final block; zero records advance to the next block.
- [Linux v6.12 root selection](https://raw.githubusercontent.com/torvalds/linux/v6.12/fs/isofs/inode.c):
  both primary and Joliet roots use `extent + ext_attr_length`.

The supplied T23 clean log reaches the PVD root record and fails specifically
at `data_length`. It proves neither the raw length nor which comparison failed.
The repository currently accepts either numeric copy if it passes bounds and
requires directory lengths to be at least 2048 and block aligned. Its existing
extended attribute offset already applies to both file and directory reads.

Decision (supersedes F1-54's big-endian fallback): always use the little-endian
copy in PVD, SVD and directory records. Log disagreeing copies, but reject an
impossible little-endian value even when the big-endian value is usable. Accept
every nonzero directory length that fits the readable extent; round its read
span up to 2048 bytes using 64-bit arithmetic, preserving the declared metadata
size. The complete final block must fit the same media and selected-volume
bounds. Continue skipping extended attribute blocks without including them in
the data length. Each diagnostic field/category is logged once per mount
lifetime, reset on remount, so repeated lookups cannot flood the boot log.
Failure records include the raw operands and bounds; successful mismatch and
rounding records explain the compatibility decision. Root shape, interleave,
descriptor versions and supported-volume restrictions remain enforced.

Python-built miniature PVD/Joliet fixtures require no xorriso and exercise
root and nested directories, partial final blocks, extended attributes,
disagreeing numeric copies and impossible little-endian bounds. Host evidence
does not establish what bytes are on the owner's disc: the lead must rerun the
QEMU media/storage gates and collect a T23 log showing a successful optical
bind and Files access, or the precise raw operands of the remaining failure.

## fx-emptydrive: drives without media keep their letters

Security integration correction (research checked 2026-10-28):

- [FindFirstFileA](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstfilea)
  requires directory access and returns no search handle on failure.
- [System error codes](https://learn.microsoft.com/en-us/windows/win32/debug/system-error-codes--0-499-)
  distinguish ERROR_NOT_READY (21) from ERROR_FILE_NOT_FOUND (2).
- The Resource Kit chapter and volume-information documentation cited below
  distinguish a reserved removable-drive letter from its available medium.

The sec-k03 `vfs_resolve_subject_locked` adapter returned ENOENT for every
unmounted letter before its subject-aware directory walk. This changed
Win32 FindFirstFile on an empty CD-ROM from error 21 to error 2. Preserve
ENXIO for an owned empty slot at that adapter boundary, before file access
checks; unknown letters remain ENOENT. Mounted volumes still use the same
subject-aware resolver and policy checks. Exercise both administrator and
Standard credentials through the production Win32/VFS path. This ordering
implements the sec-integfix directive; the cited API pages document the
error/handle contracts, not a general Windows access-check precedence rule.

Research checked 2026-10-27 before implementation:

- Microsoft Windows 98 Resource Kit (owner's media, `build/rskt/wrkc10_5.htm`),
  ch. 10 "Assigning Drive Letters for Removable Media": the drive letters of
  each removable-media drive (CD-ROM and Disk drives in Device Manager) are
  reserved during system startup; "If no media are present at startup,
  Windows 98 reserves one drive letter for each of the removable media", and
  the reserved range is a per-device setting (Start/End Drive Letter).
  Ch. 10 "CD-ROM File System": CDFS loads for every detected CD-ROM device.
- [GetLogicalDrives](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getlogicaldrives):
  bitmask of the drive letters currently assigned to a logical drive,
  removable media included (no medium needed for the bit).
- [GetDriveTypeA](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getdrivetypea):
  DRIVE_CDROM (5) for a CD-ROM drive, DRIVE_REMOVABLE (2), DRIVE_NO_ROOT_DIR
  (1) only for an invalid root; a Wine regression discussion
  ([wine-devel](https://mail.winehq.org/mailman3/hyperkitty/list/wine-devel@winehq.org/thread/XWHNFRB4WTCFOPBUJGF2LRHRE6RDCQNM))
  records that Windows answers DRIVE_CDROM with or without a disc while file
  access fails with ERROR_NOT_READY.
- [GetVolumeInformationA](https://learn.microsoft.com/windows/win32/api/fileapi/nf-fileapi-getvolumeinformationa),
  Remarks: on a floppy drive without a diskette or a CD-ROM drive without a
  disc the system asks the user to insert one (critical-error box, suppressed
  by SetErrorMode SEM_FAILCRITICALERRORS); the call then fails. System error
  21 is ERROR_NOT_READY ("The device is not ready").
- [MS-DOS Encyclopedia, appendix C](https://www.pcjs.org/documents/books/mspl13/msdos/encyclopedia/appendix-c/):
  extended error 15h "drive not ready" (class 0Bh media, action 07h retry
  after user intervention, locus 02h block device), distinct from 0Fh
  "invalid drive"; it is an INT 24h critical error.
- MSCDEX `/L:` assigns the first CD-ROM the next letter after the block
  devices unless forced ([Computer Hope](https://www.computerhope.com/mscdex.htm));
  the CD-ROM therefore follows the last hard-disk partition (D: on a
  one-partition machine such as the T23).

Repository before this change: the ATAPI binding (`optical_bind`) had no
letter; `optical_service` reported `drive=-` until an ISO mounted, and
`storage_add_ro_locked` then took the first free letter from `next_drive`.
`/dev/media` listed the drive only while mounted, so My Computer, the Files
tree and the Open/Save Places were empty without a disc; a USB stick inserted
first could take the letter the CD would get later; paths on the letter were
ENOENT (Win32 2/3, DOS "invalid drive") and the Win32 disk service counted
only mounted volumes, so GetDriveType answered DRIVE_NO_ROOT_DIR.

Decision:

1. `struct vfs.slot_type[26]` records the removable-media drive owning a
   letter (`VFS_SLOT_OPTICAL`, `VFS_SLOT_FLOPPY`), set once under the
   namespace lock and never cleared. Every letter allocator (USB media, NTFS
   remount, the partition tool's publication, optical fallback) skips owned
   letters (`vfs_letter_taken`).
2. Each bound optical drive owns a letter from its first service pass, which
   `storage_init` runs after the hard-disk and NTFS volumes and before USB
   starts: the first free letter from `next_drive`, then `next_drive` moves
   past it. A disc mounted before that pass (Setup's boot CD) keeps its
   letter. `optical_bindings[slot].drive` holds it; a disc mounts there (if
   another volume holds it, which no allocator does, the next free unowned
   letter). `[optical] bind slot=N stage=letter drive=D result=0` records the
   reservation; `[storage] optical slot=N drive=D ... reason=no_medium` (or
   `audio_cd`) now names the kept letter instead of `-`.
3. The floppy platform reserves A:/B: for each drive the controller found
   (`storage_reserve_floppy`); a request made before `storage_setup` waits in
   a pending mask that `storage_setup` applies to the system storage.
4. A path on an owned letter without a mounted volume is "not ready":
   `/mnt/<x>` lookups return ENXIO and VFS drive paths `-FS_ENXIO` (the code
   Linux uses for "No such device or address"; `MEDIA_FLOPPY_LOAD` already
   uses ENXIO for "no disk"). The Win32 file service maps ENXIO to
   ERROR_NOT_READY (21); the DOS layer maps `-FS_ENXIO` to 15h with the INT 24h
   critical-error path, as for a removed medium.
5. `/dev/media` `MEDIA_GET_STATE` reports `type[d] = CIUKI_MEDIA_OPTICAL` for
   an owned CD-ROM letter with or without a disc; `removable_mask` and
   `generation[d]` keep meaning "a disc is mounted" (the floppy rule of fx-17b),
   so the tray, AutoPlay and eject logic are unchanged. No ABI record changes.
6. Win32 `W32_F_DISK`: the drive bitmask includes owned letters; an empty
   drive copies its record (type 5 or 2) and fails with 21. kernel32's
   GetLogicalDrives/GetDriveType accept that answer (last error preserved);
   GetVolumeInformation and GetDiskFreeSpace(Ex) fail with ERROR_NOT_READY;
   CreateFile, FindFirstFile, GetFileAttributes and CreateDirectory on the
   drive fail with ERROR_NOT_READY. No critical-error box is shown (CiukiOS has
   no Win32 critical-error UI; applications see the SEM_FAILCRITICALERRORS
   result).
7. Files and the shared Open/Save dialog list an empty CD-ROM drive as
   `Compact Disc (D:)` with the Tango optical icon and type "CD-ROM Drive";
   opening it shows "Please insert a disk into drive D:" (Files: Retry/Cancel,
   as for fx-17b floppies; dialog: message). Ejecting keeps the row. A disc
   label replaces the name once mounted (`LABEL (D:)`).
8. DOS VMs: the managed drive model is unchanged (C: private, D: read-only
   system view, E: the first present USB volume; sec-k05 owns
   `unified_mount`). No DOS letter maps the CD-ROM drive, so "drive not ready"
   on the CD itself needs a DOS drive-model decision; the error mapping above
   is in place for it.

Host evidence: `tests/host/atapi_test.inc` (letter at D: and after three
hard-disk letters, record text, `FS_ENXIO`, kept after removal/eject, mount
at the owned letter), `tests/host/fs/test_storage_namespace.c` (`/dev/media`
type without the bit, ENXIO for open/create/chdir, FS_ENXIO for open/mkdir/
find, no `/mnt` root, USB stick skips owned letters),
`tests/host/win32/kernel32_files.c` and `kernel32_file_wrappers.c` (Win32
answers), `tests/host/dos_file_api_test.c` (15h mapping),
`tests/host/desktop/files_ui_test.c` and `dialog_places_test.c` (views,
prompt, pinned frames). The acceptance item `empty-drive-is-d` of
`tests/suites/acc-t23.json` is a normal pass condition, and the eject steps
wait for the record with the kept letter.
