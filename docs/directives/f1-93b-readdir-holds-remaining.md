# f1-93b — Native holds still 1.4–1.9 s on readdir/stat (window lag)

- **Phase/model:** F1, Claude agent (Opus). **Worktree:** `wt/f1-93b` from `main`.
- Evidence: T23 trial of image b453bc92… (2026-10-22 ~08:30, serial copy `build/lead-scratch/t23-b453bc92-serial.txt`, disk log `legacy/local/physical/b453bc92/BOOT.LOG` copied to `build/lead-scratch/t23-b453bc92-BOOT.LOG`).
- Records after f1-93: `[storage] vfs_hold drive=C ms=1904 task=desktop#31 op=readdir site=native waiters=1 tagged=3`, 1549, 1502, 1395 (readdir), 1386 (stat), plus `ms=392 task=desktop#29 op=readdir`; `[desktop] stall stage=launch wait=mailbox-wait` ×9. So a readdir path still holds the volume across cold reads: f1-93 converted `getdents` for the native site, but desktop#31 (a desktop worker: folder/launch directory compare, `ipc directory-compare`) reaches a different entry (find the exact syscall and VFS path from the desktop's worker code and the hold's `op`/`site`), or the hold is the per-operation tagged hold of f1-87 taken by a caller above the step helpers.

## Required

1. Identify every remaining caller that can hold the drive across a device read (grep every `vfs_hold`/tag site and every path that reaches `fat` sector reads while `holds[]` is set): list them in the report with the operation and the caller.
2. Convert each to the release-and-restart step helpers of f1-93 (readdir of large directories such as /Programs, /Documents, the games directory; stat chains; the directory compare of the desktop launcher).
3. Add to the T23 cold-read replay the exact desktop#31 sequence (directory compare of a 2,000-entry directory on a cold cache) and the gate: no `vfs_hold` above 20 ms from any op on drive C.
4. Host tests failing on main for each remaining caller; strict FS gate, `test_t23_holds`, `test_kernel_latency`.

Rules: no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Report `build/codex/f1-93b-report.md`.

## Amendment 1 (lead, 2026-10-22 10:01, decision on the delivery)

Accepted: the long holds were the holding thread preempted at `fs_service()`
while a DOS VM at priority 9 kept the CPU (100 ms starvation-boost
multiples), not device reads; the step helpers now never reach a preemption
point with the drive held, directory scans keep their position, cold
directories read in 64 KiB runs, close and lseek holds are tagged. The
scheduler side (boost the holder of a drive reservation, a minimum share for
desktop workers while a VM runs) is directive f1-98. Win32 `ReadFile` across
cold data, the pagefile walk and the publication waits are follow-ups.
