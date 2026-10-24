# DOS Box close hang and DOS launches, source 56e58374

Host investigation for the owner's QEMU report ("DOS games do not start") and
the lead's QEMU QA runs of image 56e58374 on 2026-10-24. No QEMU, systemd
scope or image build was run for this record. The QA serial logs were read
from `build/test-runs/lead-qa/` of the main checkout.

## QEMU evidence

Run 1 (QMP resume 21:15:04, quit 21:21:40):

- `dosbox` started, its CREATE re-hashed `UNIFIED.IMG` (2471 ms) because the
  boot-time check had ended `failed step=version error=-11`; DFV2 IFS became
  active and `dir` worked.
- `exit` at `C:\>` printed a new prompt. The unified DOS shell is
  `COMMAND.COM /P` (permanent, since f3-63), so `exit` does nothing there;
  this predates the merges of 2026-10-23/24.
- Alt+F4: the DOS Box sent CLOSE at the idle prompt (an ordinary EXIT,
  `release_binding`, no Ctrl+C). The serial log then has exactly
  `[dosvm] lifecycle stage=stop vm=1 ... reason=0` and
  `[dosvm] cdaudio close vm=1 ...` and nothing else.
- After that: no serial line at all for 3.5 minutes (cpustat's 2-second
  `[cpu]` windows stopped), the disk image was never written again (its
  mtime equals the last serial line, 21:18:16), the desktop clock stayed at
  19:18 UTC in every later screenshot, the window caption stayed
  "Closing...", and none of the following Run launches (help, control,
  scandisk, netsurf, music, regedit, tasks) produced a `[desktop] launch`
  record.

Run 2 (21:46-21:52, same image, KVM): the same open/`dir`/`exit`/Alt+F4
sequence logged `stop`, `cdaudio close`, `private-data ... action=discard
reason=transient-session-end dirty=1` and `cleanup ... result=0`, and the
next Run launch (music) worked. The hang is not deterministic.

The stuck step is therefore between `dosvm_destroy`'s `dosvm_cdaudio_close`
and the overlay record in `dosvm_disk_close` (`dosvm_disk.c`): the logical
C: close (`dosvm_file_destroy`: `unified_release`, `dos_file_volume_close`,
which flushes the logical volume through the overlay provider into the
overlay file on the outer C:), `dosvm_floppy_close`, and
`dosvm_disk_close`'s `vfs_commit` of the dirty transient overlay and its two
`vfs_close` calls. All of it is storage publication on C:.

In both runs `printspool` was the top CPU user (run 2: first in 170 of 197
two-second windows, median 762 ms per window) and
`[storage] vfs_hold drive=C ms=81 task=printspool#43 op=lookup` records
repeat; in run 1 `dosbox` lent its priority to `printspool` for the drive and
the cache during CREATE.

## Relation to the T23 hang of the same source

The T23 trial of 56e58374 stopped in a cache-publication livelock between
two FIFO mutex users (`docs/validation/t23-boot-hang-2026-10-24.md` on
`wt/img`, fixed by its commits 62971d1a and 2c99c8db: `cache_publish` takes
the cache mutex with the plain FIFO take). The DOS Box close runs exactly
that class of operation while the storage writer appends the boot log.
One difference is recorded and remains open: on the T23 the cpustat task
kept reporting during the livelock (about 41,000 switches per 2 s), while in
QEMU run 1 it stopped. Either cpustat blocked on storage too (its pager and
reclaim polls) or no task ran at all; the blocked instruction is not known.
If the hang recurs before the `wt/img` fix is merged, QMP
`human-monitor-command` `info registers` taken a few times during the hang
(EIP/CS against `VMM.ELF` symbols) distinguishes a spinning kernel loop from
an idle CPU.

## Host checks (all on this source)

- `tests/host/dos/game_launch_*.c` (new, in `scripts/test/host_kernel_tests.sh`):
  the production `/Programs/dosbox` over the production client boundary,
  lifecycle, launch gate, presenter and destroy, with a scripted DOS guest.
  DOOM from Programs > Games reaches its first frame in the window
  (4424 ms model time); DOOM quitting to DOS closes the window through
  "Close on exit" within 1 ms and the VM ends as EXIT; the desktop's close
  of a plain DOS Box at its idle prompt destroys the VM as EXIT and closes
  the window within 1 ms.
- `tests/host/dos/test_close_contention.py` (new): the transient session's
  close path (logical C: flush through the overlay provider, overlay commit,
  close, delete) over the production cache, FAT, VFS and `dos_file_volume`,
  under the storage writer's write-back and boot-log commits, the spooler's
  probes and desktop settings saves: 12 sessions close in at most 4 ms.
  It uses pthread locks; it does not model the native FIFO ticket mutex,
  so it cannot show the livelock above (the `wt/img` replay does).
- `fx-8` launch test, new case: a DOS Box window stuck in "Closing..." whose
  process never answers does not delay Run: control is spawned on the
  launcher worker, frames and input continue. The desktop holds no lock for
  a closing window; in run 1 the launches stopped because the whole system
  stopped.
- The DOS Box replay scenarios 23-28 (G17 toolbar and per-program lifecycle,
  fx-23 CREATE on its worker) had stopped running when both branches were
  merged (`range(23)`); they run again and pass.

## Changes

- G31 spooler: an idle pass opened `job` and `job.bak` in all 128 slots
  every second (257 opens per pass, 256 of them failed walks through
  `/WINDOWS`). `cp_job_ids` reads the spool folder once; the worker, Purge
  and the Printers window visit only existing slots. An idle pass now opens
  no file (`tests/host/printing_test.c` counts the library's opens).
- Tests above.

## Not changed here

- Cache publication livelock: fixed on `wt/img`.
- The boot-time image check fails `step=version` on every boot seen: the
  image has no change record of its own, so its write generation is the
  volume's change floor, which rises whenever more than 64 other files
  change during the check (`VFS_CHANGES`, `change_record` eviction). Each
  first DOS Box after boot then hashes 27.7 MB inside CREATE. Pinning the
  image's version for the check (`vfs_version_pin`) would keep it to the
  image's own changes; this needs a directive (fx-23 area).
- G29d records each native program start by writing and committing
  `CIUKIST.LOG` inside the exec path; while C: publication is stuck, every
  launch waits there. Windows 98 logs application starts asynchronously
  (Task Monitor); moving the record out of exec needs a directive (it
  changes `startup_image_test`'s synchronous expectation).
- `exit` at the permanent shell prompt (pre-existing).
