# f0-25 — The system volume must never lock itself after a forced power-off; crash evidence that survives; log rotation that works (T23, image 5e018550)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f0-25` from `main`. Build the kernel before reporting.

## Evidence (third occurrence)

Owner, image `5e018550`: a bad first boot, forced power-off, then "I/O error" on every operation during the second boot. Images `a4f67ac1` and `a738204a` showed the same after forced power-offs. The disk read from this workstation is consistent every time (all files readable). The second boot's disk log (`legacy/local/physical/5e018550/disk-BOOT.LOG`, 570 lines) mounts `mode=rw writes=17`, reports `disk_log=available error=0`, and then contains **no** read-only record and ends in ordinary per-second lines: when the volume locks, the log on that same volume stops too, so the reason record of f0-23 was never seen. `BOOT1.LOG`/`BOOT2.LOG` do not exist: the rotation of f0-23 did not run (the first boot's log was lost).

## Task

1. **Tolerance, like every other FAT implementation.** Reads never depend on volume consistency; a chain longer than the file size, an unpublished log tail, a dirty flag or a lost cluster never refuses reads or writes to other files. At mount after an unclean shutdown run a bounded repair: truncate the log files' chains at their published length, free lost clusters, clear the dirty flag, record what was repaired. Reserve the read-only state for inconsistent metadata that would be damaged by writing (cross-linked chain, FAT copies disagreeing on an in-use cluster), and even then applications keep reading. Audit every `corrupt()` and `-EIO` path in `fat.c`, `vfs.c`, `mount.c`, `cache.c` and the ATA quarantine path for conditions that can be hit by the log's own files or by the games image.
2. **Black box.** A raw ring of 64 KiB in reserved sectors of the system partition (outside the FAT, allocated by `scripts/build_image.py`), written with direct sector writes that bypass the filesystem and the cache: the last records before any lock, the read-only reason, panics and the shutdown marker go there; at the next boot the kernel copies the ring into the new `BOOT.LOG` head and to the serial, marked `[blackbox]`, before the volume is even mounted for writing.
3. **Rotation.** Find why `BOOT.LOG` was not rotated at the second boot (activation after an unclean first boot) with a fixture that mirrors this disk's state; rotation must happen before the new log truncates anything.
4. Host fixtures: forced power-off at every stage of the log writer and of a settings save; mount; every file readable; settings saved; games image read through the DOS disk service; black box read back.

## Acceptance

Kernel host suite green; QEMU `f0-smoke` and `f3-vm` green; on the T23: a forced power-off followed by a boot with no I/O error, `BOOT1.LOG` present, `[blackbox]` lines at the head of the new log.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary; host tests one at a time with `nice -n 19`. Report `build/codex/f0-25-report.md`.
