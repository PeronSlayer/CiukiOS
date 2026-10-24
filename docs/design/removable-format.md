# Removable FAT formatting (G29)

## Reference behaviour and decisions

Research performed on 2026-10-23. The owner's read-only Windows 98 Resource
Kit Chapter 10, `wrkc10_5.htm` (Disk Management, formatting) says Format
creates a filesystem and distinguishes formatting from partition management.
`wrkc10_9.htm` requires power-of-two cluster sizes through 64 sectors.
Microsoft's [FAT specification](https://www.microsoft.com/en-us/download/details.aspx?id=53426)
(`fatgen103.doc`, sections on BPB, cluster count, FAT entries, FSInfo and
directory entries) determines FAT12/16/32 by data-cluster count, with cutovers
4085 and 65525; the identifier string alone does not select the type.
The [Microsoft-authored FAT specification](https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf)
also documents backup boot sectors and FSInfo. Microsoft's
[Format reference](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/format)
distinguishes Quick (metadata only) from a sector scan and limits FAT labels
to 11 characters. The modern command's overwrite switches are not evidence
for Windows 98 zeroing every data sector.

Implement quick metadata recreation or full read verification followed by
metadata recreation. G29c adds low-level floppy track formatting and FAT12
bad-data-cluster marking; unreadable metadata still aborts. FAT labels follow G6's ASCII
validation and uppercasing. Formatting never copies system files or claims
that a disk is bootable.

## Safety and interface

`MEDIA_GET_FORMAT=0x4d1a` and `MEDIA_FORMAT=0x4d1b` append to `/dev/media`;
there are no new syscalls. The fixed 52-byte `ciuki_media_format` uses media
version 2 and the physical generation from `MEDIA_GET_STATE`. GET returns
capacity, current FAT family, allocation size, label and supported family
mask. FORMAT takes family 12/16/32, Quick flag (zero means surface-only Full), optional floppy Low Level flag, and label;
capacity/allocation/mask fields are outputs. Reserved must be zero.

G29c also accepts a retained unformatted removable identity without a FAT mount. Fixed/system/optical
drives, Setup mode, uncertain durability, stale generations and devices with
more than one mounted volume are refused. Partitioned USB media keep their
existing FAT family to preserve the partition type; whole media may select
any family whose calculated cluster count fits. A physical-device drive
reservation spans busy checks, durable unmount, cache invalidation, raw I/O,
and remount at the original letter. Pagefile, open files, identities and
working directories in subfolders block formatting. Generation and
quarantine are checked for every raw transfer. Failure after detachment
leaves the volume unpublished; no uncertain filesystem is exposed.

A surface-only Full scan precedes metadata replacement. Low-level floppy
formatting is already destructive when FORMAT TRACK establishes sector IDs. The primary boot sector
is invalidated and flushed before metadata writes, then published last
after FATs/root/FSInfo/backup sectors have been flushed. No rollback can
recover files after destructive metadata writes. The boot sector remains
invalid after an interrupted operation until its final publication.

G29c publishes blank removable identities to media state without creating a
FAT namespace. Files includes those letters and offers Format on root access.
Full floppy formatting invokes the existing FDC FORMAT TRACK service, then
checks individual sectors and marks unreadable FAT12 data clusters FF7h in
both FATs. Metadata, generation, write-protect and quarantine errors refuse.
Completely unformatted floppies use a read-only mechanical presence probe
and nominal CMOS geometry before the user confirms low-level formatting.

System-file copying still needs an approved unified native floppy boot path;
no historical floppy loader is used. Progress/cancellation within a running
format and partition-family changes remain gaps. Host tests do not claim
image or hardware qualification. See `disk-tools-continuation.md`.
