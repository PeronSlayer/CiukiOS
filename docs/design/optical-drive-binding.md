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
