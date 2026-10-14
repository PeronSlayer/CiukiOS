# f1-63 — With a CD and a USB volume present: the volume preference save returns EROFS (30) and DOS windows show "I/O error" without any launch record

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-63` from `main`. Host-testable; the lead runs a QEMU case with CD + USB stick and the T23.

## Evidence (T23, image 66843edc, clean disk log `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/66843edc/disk-log/BOOT.LOG.clean`; owner report)

- Owner: with the stick inserted, saving the volume setting fails with error 30; starting DOOM/Quake shows "I/O error"; Files and the wallpaper are slow. `EROFS` is 30 in `src/kernel/include/ciuki/abi.h` and `FS_EROFS=30` in `fs_port.h`.
- The system volume is read-write: `[storage] kind=2 disk=0 part=1 mode=rw error=0 writes=17 reason=mount`, `disk_log=available`. The CD is D: (`[storage] optical slot=2 drive=D mode=ro`), the stick's failed volume is recorded as E: (`[usb-storage] volume drive=E ... mode=rw dirty=0 error=-5`, `mount result=-11`).
- No `[launch-stage] name=dosbox` or `[dosvm] disk` record exists although f1-61 added them before VM creation; `files` launches are recorded. So the DOS window fails before the desktop's spawn or its first record: the "I/O error" comes from an earlier open/stat in the desktop's launch path (ELF `/Programs/dosbox`, the games disk `/Games/CIUKIGAM.IMG`, the autorun/loading panel) or from path resolution landing on the wrong volume.
- On 445e91c7 (no stick) DOOM launched and settings saved. The common factor of EROFS and EIO is the presence of the read-only D: and the broken E:: a write that reaches the CD gives EROFS, a read that reaches the quarantined stick gives EIO.

## Task

1. Audit path and volume resolution after f1-59's "all letters publish together under a short VFS lock" in `src/kernel/fs/mount.c` and the removable publication in `usb_storage.c`: which volume `/System/Registry` (libreg `reg_open_root_at`, the desktop's `master_storage.c`), `/Programs/dosbox`, `/Games/CIUKIGAM.IMG` and the wallpaper cache resolve to when D: (ISO) and E: (failed USB) exist; the "current/settings volume" selection; drive-table indices versus letters; a quarantined volume still answering opens with EIO instead of being absent.
2. Reproduce on the host VFS/mount model with three volumes (system rw, ISO ro, removable quarantined) the registry save and the game launch opens, assert they hit the system volume, and fix.
3. Desktop: the game launch path records the first failing operation (`[launch] name=dosbox error=<n> path=<p>`) before showing "I/O error".
4. Define a QEMU case (f1-storage or f4-media suite) that boots with a CD image and a USB stick attached, saves the master volume from the Control Panel and launches a DOS window; the lead runs it.

## Acceptance

Filesystem, USB storage and desktop host suites green; the QEMU case passes; on the T23 with CD and stick inserted the volume saves and DOOM starts. Files: `src/kernel/fs/mount.c`, `usb_storage.c` publication only if needed (coordinate: f1-62 edits the bind stages), `apps/desktop` launch records, `sdk/libreg`, tests.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Record links and decisions in the relevant `docs/design/` or `docs/validation/` document, then implement and test. Report `build/codex/f1-63-report.md`: what the T23 evidence proved, what changed, which host tests prove it, what the lead must verify.
