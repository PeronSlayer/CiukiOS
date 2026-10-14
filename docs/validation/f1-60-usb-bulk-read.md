# F1-60 USB bulk progress and failed mount admission

## Research and implementation decision (2026-10-14)

Checked before implementation:

- [Intel UHCI Design Guide 1.1, original specification mirror](https://mirrors.telepoint.bg/pub/NetBSD/misc/blymn/uhci11d.pdf),
  §§3.2–3.4: an ACK clears Active and advances the QH element to the TD
  link; a NAK leaves it pending. Element termination permits software to
  insert the next TD. SPD controls short-IN queue advancement.
- [USB 2.0 original specification](https://sinelab.net/pdf/usb-20-specification.pdf)
  ([USB-IF distribution](https://www.usb.org/document-library/usb-20-specification)),
  §5.8: full-speed bulk MPS is 8/16/32/64; bulk has no guaranteed latency.
  Successful transactions advance the DATA0/DATA1 sequence, whereas NAK
  does not. Earlier commands on the same endpoint also consume toggles.
- [Linux UHCI queue implementation](https://codebrowser.dev/linux/linux/drivers/usb/host/uhci-q.c.html),
  `uhci_submit_common`: one TD per packet, alternating toggles; short-IN
  detection is cleared for the last TD when short transfers are accepted.
- [USB-IF BOT 1.0](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf),
  §§5–6, and [Linux storage transport](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/storage/transport.c):
  data length and CSW completion are separate; transport errors require
  recovery, never acceptance of incomplete data as a mounted filesystem.

Repository audit: UHCI submits one TD at a time, followed by two-frame
retirement before reuse. There is no seven-entry TD list. Its single-element
queue clears SPD, polls Active, decodes late ACKs after retirement and
replaces the terminated element on every submission. `usb_bulk` advances
the toggle once per successful packet. The supplied T23 trace has aggregate
`result=-110`, but its saved packet has `result=64`, `actual=64`, inactive
status and successful retirement. The bulk loop can produce that mismatch
when its deadline expires **before** the next submission, retaining the
previous packet's trace. It does not prove that an eighth TD was submitted
or failed, or that its toggle was wrong. It also cannot distinguish the
one-second data-stage limit from the five-second overall bind limit.

Decision within F1-60: during binding, treat the existing one-second bulk
allowance as a wait for progress, renewed only after a full ACK when data
remains. Every renewal is capped by the unchanged overall five-second bind
deadline and its 100 ms finishing reserve. NAK and failed packets never
renew it; normal published I/O retains its ten-second whole-stage bound.
Keep the current TD scheduling and retirement rather than inventing a
hardware queue defect. Retain the first eight packet snapshots, plus the
current packet index and deadline context. A refusal before submission gets
an explicit invalid TD snapshot rather than the previous ACK.
If an earlier tolerated INQUIRY/MODE fault already consumed the first-fault
record, retain one additional `terminal_fault` snapshot at quarantine so
the mount's packet history is still logged. Repeated service passes do not
emit another terminal snapshot.

`storage_add_removable_bounded` already prepares FAT volumes privately and
attaches them only after all preparation succeeds. A `volume drive=E ...
error=` line describes a candidate slot, not proof of a VFS attachment.
USB nevertheless sets `published` before this call and retains it on error.
Set it only on success; every failed mount ends binding, quarantines the
device at stage `mount`, and prevents another admission attempt. Preserve
the retained volume error diagnostic. Host tests must check the namespace
and VFS opens, as well as USB state, before a later service pass can hide
a failed admission.

The deadline replay is a demonstrated software mechanism matching the
trace shape, not a measurement of the Lexar's physical packet timing. The
lead must use the new packet/deadline records to determine the T23 cause.

## Host evidence

The new delayed-first-ACK fixture was run against unchanged production code
before the fix. Ordinary eight-packet IN succeeds. A device ready at bulk
start + 980 ms produces:

```text
first_toggle=0 delayed=1 result=-110 actual=448 ACKs=7
last_td=1800003f last_result=64 elapsed_ms=1000
```

The TD is packet index 6, not 7. After the change, all eight retired 64-byte
packets are copied, alternating DATA0/DATA1, for either initial toggle.
The model checks actual token values and QH element termination after ACKs.
No UHCI schedule, SPD, NAK decoding, DMA allocation or retirement change
was needed. The first packet's delay is injected; it is not a T23 measurement.

The new mount fixture also fails before the fix with `published=1`, retained
volume error -5, and **no VFS E:**. Afterward it verifies immediate unpublished
USB state, absent volume/drive flags, refused E: opens and no new commands or
records on repeated service. It covers first-packet NAK, eighth-packet NAK,
invalid FAT BPB, and eighth-packet failure following a successful INQUIRY
retry. Recovery reuses controller DMA without erasing the eight data records.
The volume error line remains available.

All required suites passed sequentially with `nice -n 19`:

- `bash tests/host/usb/run.sh`: ASan/UBSan; eight-packet success, real pending
  eighth-TD timeout, overall bind exhaustion before submission, bounded
  eight-entry history for sixteen-packet UHCI/OHCI reads, existing controller,
  service, input and latency cases. All 1,344 latency reports retain the
  existing maximum 20 ms ACK-to-delivery bound.
- `bash tests/host/usb/storage_run.sh`: ASan/UBSan; new failed-mount/history
  fixtures, existing BOT/FAT12/FAT16/FAT32 and removal/write-admission tests,
  and all ten F1-59 hostile-bind replays plus successful FAT12 bind/eject.
  Existing aggregate-deadline cases still terminate at 4,902 model ms.
- `python3 -B -m unittest discover -s tests/host -p test_runner_usb.py -v`:
  16 tests, OK; runner code unchanged, no virtual machine launched.
- Ten translation units (all seven USB sources, `core/devices.c`,
  `core/init.c`, `drivers/media_dev.c`) compile sequentially with the exact
  canonical i686 C flags from `scripts/build_kernel.py`. The largest measured
  optimized frame is 584 bytes, below the canonical 4,096-byte guard.
- `git diff --check`: PASS.

Detailed logs and the report are under `build/codex/`. No QEMU, physical
test, full image/kernel link, commit, push or diary update was performed.

## Lead qualification

Build the integrated canonical image with the workstation resource caps,
record its full SHA-256 and retain earlier gates. Run `f1-usb` for UHCI and
OHCI. On the T23, boot with the receiver and Lexar inserted and verify Files
can open/read the mounted stick; keep PS/2, USB input, CD/Files and heartbeat
progress throughout binding. If mount fails, require no removable drive in
Files or the DOS VM namespace, the retained volume error where FAT preparation
was reached, and a durable stage/packet trace. Packet indices are zero based:
index 7 is the eighth. `valid=1` with Active/NAK and timeout identifies an
issued pending TD; `valid=0`, zero TD/token and `stop=bind_deadline` identifies
refusal before submission. Inspect the bulk elapsed time, deadline and bind
time remaining, plus the alternating token toggles, before attributing any
remaining fault to hardware scheduling. The unchanged overall deadline may
still reject a device whose total bind exceeds five seconds. F1-61 owns the
DOS disk probe's separate error handling.
