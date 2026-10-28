# fx-emptydrive: drives without media appear like Windows 98 (D: CD-ROM, removable drives)

- **Written:** 2026-10-27 17:05 by the lead. Implementer: Claude agent. Base: main 1c4bd91af or later.
- **Evidence:** owner, T23 2026-10-27: "il lettore cd non viene visualizzato". The kernel binds the ATAPI drive (`[optical] bind slot=2 ... result=0`, poller every 1000 ms) but reports `drive=-` until a disc mounts; My Computer and Files list only mounted volumes, so the CD-ROM drive is invisible when empty. The acceptance suite (fx-accept, item 4) records this as a known failure without a directive.

## Windows 98 behaviour (research and cite: Microsoft documentation of GetLogicalDrives/GetDriveType/GetVolumeInformation and SHGetFileInfo for drives, the Windows 98 Resource Kit in build/rskt, MSCDEX drive-letter assignment)

- Every fixed, CD-ROM and removable drive owns its letter from boot, whether or not media are present; the CD-ROM keeps the letter after the last hard-disk partition (D: on the T23), removable USB drives follow.
- My Computer shows each drive with its type icon and, when media are present, the volume label (`Half-Life (D:)`), otherwise `Compact Disc (D:)`/`Removable Disk (E:)`.
- Opening an empty drive shows "Please insert a disk into drive D:" with Retry/Cancel; `GetDriveType` returns DRIVE_CDROM and `GetVolumeInformation` fails with ERROR_NOT_READY while empty; DOS `INT 21h` on the drive returns "drive not ready" instead of "invalid drive".
- Insertion and ejection update the open views and trigger AutoPlay; the drive letter never changes.

## Scope

1. Reserve and publish the letter of every optical and removable drive at boot in the kernel's drive table and the media service (`/dev/media` snapshot), with a `present=0` state, keeping today's mount on insertion and unmount on ejection.
2. My Computer, Files (tree, address bar, Open/Save dialogs), the desktop's AutoPlay watcher, the DOS drive model (D: read-only, E: removable as sec-k05 defines on its branch: do not conflict with `wt/sec-k05`, list any overlap) and the Win32 facades (`GetLogicalDrives`, `GetDriveType`, `GetVolumeInformation`, `GetDiskFreeSpace`) follow the Windows 98 behaviour above.
3. Host tests for boot with an empty drive, insertion, ejection and the Win32/DOS answers; the optical, media, desktop, Files and Win32 suites stay green; the acceptance case for item 4 becomes a normal pass condition (edit `tests/suites/acc-t23.json`).

Rules: work only in `/home/peronslayer/Desktop/CiukiOS-wt-fx-emptydrive` (branch `wt/fx-emptydrive`); never commit, merge or push; never run QEMU or image builds; run tests through `/home/peronslayer/Desktop/CiukiOS/build/lead-scratch/heavy.sh`. Report to `build/codex/fx-emptydrive-report.md` in the worktree. Leave changes uncommitted.
