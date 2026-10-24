# Disk Cleanup model

The Windows 98 Resource Kit, `wrkc10_5.htm`, “Using Disk Cleanup”, specifies
per-drive cleanup of temporary files, the Recycle Bin and Internet cache. It
describes a volume-size-dependent 25–65 MB low-space threshold, one warning
per session, and a warning on every allocation attempt when completely full.
[Microsoft's handler documentation](https://learn.microsoft.com/en-us/windows/win32/lwef/disk-cleanup)
also describes scan, category selection, purge, cancellation and shutdown as
separate phases. Its later Windows warning thresholds do not replace the
Windows 98 Resource Kit thresholds.

CiukiOS snapshots candidates before confirmation, bounds the snapshot to
4096 entries and 32 levels, and cancels scans through a caller callback.
Selected categories alone are purged, one snapshot entry per step. Each file
and every ancestor is checked again for volume and identity. Changed files,
read-only/system files, special files and other volumes are preserved. New
files are never added during purge. Temporary directories remain; recycled
directories are removed only when empty. G2 bin locking and INFO2 saving
preserve other records and retain a partially cleaned directory's record.

Temporary-file roots are `/tmp`, `/TEMP` and `/system/TEMP` on C:, and
`TEMP` and `system/TEMP` under the selected mounted drive. Only regular files
whose modification time is known and at least seven days old are eligible.
This is a conservative Ciuki policy, not a claimed undocumented Windows 98
rule. The current VFS has no query for old files still open in another
process; mtime is an age filter, not proof that a file is inactive.

The Internet cache category is unavailable: the actual NetSurf port sets
`disc_cache_size=0` and `disc_cache_path=NULL` in
`sdk/netsurf/port/chrome.c`. No unrelated directory is treated as its cache.

The Resource Kit does not give the threshold curve. CiukiOS uses a stated
interpolation: 25 MiB through 512 MiB capacity, linear growth to 65 MiB at
2 GiB, then 65 MiB. Per-drive session state suppresses repeated low warnings;
zero-free-space warnings are never suppressed. Integration calls the helper
on allocation activity, rather than warning merely because a drive is viewed.

The path-based VFS cannot make checking and deleting atomic. Identity,
attributes, timestamps and ancestry checks narrow that race but cannot replace
a future filesystem operation that conditionally deletes a specific inode.
