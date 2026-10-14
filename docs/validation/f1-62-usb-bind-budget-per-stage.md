# F1-62: USB storage bind timing and budgets

## Research and decision (2026-10-14)

Checked before implementation:

- [USB-IF BOT 1.0](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf),
  sections 3.2, 5.3 and 6: GET MAX LUN may STALL on a single-LUN device;
  command/data/status ordering, halt clearing and reset recovery remain
  mandatory. BOT does not prescribe Ciuki's aggregate attach deadline.
- [T10 ASC/ASCQ assignments](https://t10.t10.org/lists/asc-num.htm):
  NOT READY with ASC/ASCQ 04/01 reports a logical unit becoming ready;
  04/02 requires initialization and 3A/00 reports absent media. Only 04/01
  qualifies for the new readiness polling, alongside the existing bounded
  initial UNIT ATTENTION handling.
- [Linux USB storage transport](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/storage/transport.c):
  transport waits are timed and sleepable; timeout recovery does not prove
  command success. Ciuki retains packet retirement, quarantine and no write
  resubmission.

The directive's T23 log for image `66843edc` proves enumeration, capacity and
durability qualification completed. At mount, `bind_left_ms=98` was below
the 100 ms finishing reserve. The CSW refusal had `valid=0`: the driver
submitted no TD for that packet. It does not prove a malformed device CSW
or identify which earlier stage consumed the time. The retained
`volume drive=E ... error=-5` record describes a failed preparation attempt,
not successful namespace publication; the host regression must check the
VFS pointer, present flag, drive flags and rejected E: open independently.

Decision: extend the aggregate MAX LUN-to-publication budget to 20 seconds,
retaining the 100 ms finishing reserve, and give each binding BOT bulk
transfer a 3-second wait. F1-60's full-ACK packet renewal remains capped by
that aggregate deadline; NAK does not renew. Each mount transfer starts its
own wait, with no reset of the aggregate deadline. Published I/O remains
bounded by its existing 10-second whole-transfer limit; control waits remain
500 ms. No worker-loop, controller, disk-logger or storage namespace change.

The directive requires a READY sequence taking 4 seconds to succeed. Thus
the 3-second budget applies to BOT transfer waits, not a hard limit across
all readiness commands. A valid CHECK CONDITION/REQUEST SENSE reporting
02/04/01 can be polled with sleep between commands until the aggregate
limit. Initial UNIT ATTENTION retains at most three probes; other sense
errors and transport faults fail closed. A finite poll count also bounds
this loop if the software clock freezes. These are host policy choices,
not timing values mandated by BOT or T10.

Retain the stage-start records and durable drain before each stage. Add one
completion record with `stage=<name> addr=.. elapsed_ms=.. result=..` for
every attempt, including failed stages and the INQUIRY retry. The `ready`
record reports total elapsed time since attachment began (before mount).
Stage times include reservation/log waits and readiness retries. Terminal
records are flushed after releasing the reservation. Existing sleepable
waits release the DMA/cache/workspace mutexes; input on peer controllers
continues using the unchanged scheduling and input paths.

## Host verification

All compiler/test invocations ran sequentially at `nice -n 19`; no QEMU,
image build, systemd scope, commit or diary entry was used.

- `tests/host/usb/storage_run.sh`: PASS with ASan/UBSan. The real BOT/UHCI,
  FAT/VFS and disk-log replay completes the slow small-FAT12 mount with READY
  **4,031 ms**, FLUSH **2,003 ms** and mount **4,517 ms** (including a
  2-second first-read NAK). The volume is published and `E:/SHORT.BIN`
  returns the expected 512 bytes. All seven stage completion records survive
  disk reopen; the total at `ready` matches the six pre-mount stage times.
- A stick continually returning 02/04/01 quarantines at **19,900 ms**,
  leaving the finishing reserve, with durable `reason=bind_deadline
  stage=ready` and a failed READY completion record. No writes, reset or
  publication occur. Desktop, disk-writer and peer USB tasks each dispatch
  19,900 times; host/cache/workspace mutexes are free at every replayed sleep.
  All ten earlier hostile-bind cases and the normal FAT12 mount/eject pass.
- Readiness regressions also reject 04/02 immediately, limit UNIT ATTENTION
  to three probes, and bound the frozen-clock becoming-ready loop to 201
  commands. Existing FAT12/16/32, durability, sense and no-write-retry cases
  remain green. The failed-mount tests retain the diagnostic E: record but
  require no VFS letter/present/drive flag, reject E: opens and prohibit rebind.
- `tests/host/usb/run.sh`: PASS with ASan/UBSan; UHCI/OHCI packet bounds,
  ACK renewal/aggregate exhaustion, native lifecycle, merged input/grants and
  concurrent HID/bulk latency. No controller/input changes were needed.
- `tests/host/fs/kernel_compile.py`: PASS, 31 translation units with exact
  `scripts/build_kernel.py` i686 flags, frame guards and runtime-helper audit.
- `python3 -m unittest discover -s tests/host -p test_runner_usb.py`: all
  16 tests PASS (28.165 s); host runner/predicate checks only.
- `git diff --check`: PASS.

Logs are retained beside the implementer's report in `build/codex/` as
`f1-62-usb-storage.log`, `f1-62-usb.log`, `f1-62-kernel-compile.log` and
`f1-62-runner-usb.log`. The models establish bounded software behavior;
they do not qualify an image or the T23.

## Lead qualification

Build one canonical image with the normal caps and record its SHA-256.
Run `f1-usb` on UHCI and OHCI, retaining earlier gates. On the T23, boot with
the receiver and Lexar inserted; require a mounted, readable volume in
Files or durable completion times and a quarantine naming the slow stage.
On a refused mount, Files must show no removable letter despite the failed
preparation record. Check continued PS/2/USB input, Files/CD use, heartbeat
and absence of repeated bind/reset attempts. Host models do not establish
physical scheduler latency, disk flush timing or Files rendering.
