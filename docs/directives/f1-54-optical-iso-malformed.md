# f1-54 — Data CD on the T23: the drive binds, the ISO 9660 parse reports "malformed"

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-54` from `main`. Host-testable; the lead re-tests on the T23.

## Evidence (T23, image 293c05af with f1-48, 2026-10-14; `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/293c05af-garbled/serial-clean.log`, garbled but readable in parts)

`[optical] media channel=1 unit=0 stage=tur …`, `[optic(al) … stage=capacity result=0 bytes=8 …`, `stage=session result=0 bytes=120 … sectors=2…`, then `[optical] bind slot=2 stage=iso … result=-5 reason=malforme(d)`. The ATAPI side works (capacity and session reads succeed); the ISO 9660 volume parse rejects the owner's data CD. On QEMU the fixture ISO (Joliet) mounts.

## Task

1. Find what the parser rejects: the Primary Volume Descriptor at LBA 16 of the **first track of the last session** (multi-session discs put it at session start + 16; the session record shows `sectors=…` → use the session start), a CD with a Joliet SVD plus Rock Ridge, a bootable CD with an El Torito descriptor before the terminator, descriptor set terminator handling, logical block size 2048 vs 512-byte addressing, big/little endian field checks that are too strict (both-endian fields with mismatching halves on some mastering software), volume space size vs actual capacity. Research ECMA-119/ISO 9660 and the Linux `isofs` leniencies (cite), and add a host fixture built from the structure of the owner's disc as far as the log allows (session start > 0, PVD + SVD + boot record).
2. Make the parser follow the specification leniently like isofs (accept either endian half, ignore unknown descriptors, locate the PVD from the last session), with `[optical] iso …` records naming the descriptor and field that failed.
3. Host tests for each leniency; the Joliet fixture unchanged.

## Acceptance

Filesystem and kernel host suites green; i686 kernel compiles; the lead mounts the data CD on the T23 (Files shows the drive, a file opens).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-54-report.md`.
