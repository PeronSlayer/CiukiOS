# Disk tools (G29)

The binding reference is the Microsoft Windows 98 Resource Kit, chapter 10,
“Disk Management” (`wrkc10_5.htm`, owner's read-only extracted copy), especially
“Formatting a Hard Disk”, “Using Disk Cleanup”, “Using Microsoft Backup” and
“Defragmenting Disks”. Chapter 10 “Overview of Disk Utilities”
(`wrkc10_3.htm`) and chapter 27 “Troubleshooting Tools” (`wrkc27_2.htm`)
distinguish formatting, checking, conversion, backup and defragmentation.
They are separate operations: formatting must never call the dirty-volume
repair engine to simulate a successful format.

Online primary sources checked for this implementation:

- [Microsoft: Creating a Disk Cleanup Handler](https://learn.microsoft.com/en-us/windows/win32/lwef/disk-cleanup)
  describes the Windows 98 introduction, per-drive category selection,
  space estimation and user-selected purging. Its later warning thresholds
  do not replace the Windows 98 thresholds in the Resource Kit.
- [Microsoft: cleanmgr](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/cleanmgr)
  distinguishes temporary files, Recycle Bin and Internet cache; temporary
  files unmodified for a week are candidates. This supplies the conservative
  age policy; it is not a claim that the modern command-line switches exist
  in CiukiOS.
- [Microsoft FAT specification 1.03](https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf),
  boot/BPB, FAT type determination, initialization, FSInfo and backup boot
  sectors, defines the removable formatter's on-disk output.

## User operations

Files offers Disk Cleanup on a drive's General properties page and on its
context menu. The selected drive is explicit in the dialog. The initial
scan makes no deletions. Categories show estimated bytes and require an
explicit selection and a second permanent-deletion confirmation. Cancel
before confirmation leaves every file intact. Work runs off the UI thread;
progress, cancellation, skipped files and errors are visible. Closing Files
while a disk worker runs waits for the operation to stop safely.

Temporary-file candidates are bounded to the known temporary directories,
age checked and snapshotted. Selection never authorizes deleting files that
appeared after the scan. The Recycle Bin category uses G2's INFO2 bookkeeping
on the selected drive only. Internet cache is unavailable while the NetSurf
port disables its disk cache; documents, settings and browser history are
not cache candidates. See `disk-cleanup-model.md` for the exact policy and
host-test contract.

The Windows 98 low-space warning appears after allocation, once per volume
in a session; a completely full volume warns again on every failed
allocation. The Resource Kit specifies a 25–65 MB range depending on volume
size but not its full curve. CiukiOS uses a documented bounded capacity-based
threshold, skips unknown/counting free-space results, and offers Disk Cleanup.
The Files warning observes native file-job completions; global allocation
notifications and a desktop-wide warning broker remain separate integration
work and must not be marked complete by this Files-only path.

Format is offered only for removable drives. A worker first queries capacity,
filesystem choices and physical media generation. The dialog offers the
supported FAT family, quick/full and a label. A separate warning names the
drive and destruction of all its data. Only confirmed work reaches the kernel.
The same physical generation must still be present. Fixed/system volumes and
busy media are refused. Full format checks the surface; quick format writes
filesystem metadata only. Errors are reported, never replaced by a success
summary. See `removable-format.md` for exclusive access, durability and
current media-discovery limits. Copy system files is unavailable until a
bootable rescue-media source and installation contract exist.

## Boundaries

G29 does not change G14's ScanDisk/dirty-boot repair ownership. G29c adds Copy Disk, partition editing and file Backup/Restore; their contracts
and remaining limits are in `disk-tools-continuation.md`. Application-start-
optimised defragmentation and FAT16-to-FAT32 conversion belong to G29b;
System Recovery and bootable system-file copying remain gaps. The inventory and implementation
report must keep these rows open. The existing Setup whole-disk formatter
must not be exposed as a user drive formatter: it deliberately writes an MBR
and loader and has a different exclusive-access/capability contract.

Every new disk-dialog state is a client invalidation. The fx-15 window model
must exercise scan/selection, confirmation, progress, completion, errors and
low-space warnings on all five presenters; the controller tests separately
verify the operations behind those rendered states.
