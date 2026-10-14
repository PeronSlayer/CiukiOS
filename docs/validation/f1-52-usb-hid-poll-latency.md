# F1-52 USB HID polling latency

## Sources and implementation decision

Research checked on 2026-10-14 before implementation:

- [USB-IF HID 1.11](https://www.usb.org/sites/default/files/hid1_11.pdf),
  sections 7.2.4, 7.2.6 and appendices B/F: boot keyboard input is eight
  bytes; boot mouse input is three bytes (the recognized wheel extension
  adds one). SET_IDLE zero suppresses unchanged keyboard reports.
- [Linux UHCI queue implementation](https://github.com/torvalds/linux/blob/master/drivers/usb/host/uhci-q.c):
  interrupt queues have independent QHs/TDs; unlink completion depends on
  the frame number advancing, not a software sleep count.
- [Linux OHCI queue implementation](https://github.com/torvalds/linux/blob/master/drivers/usb/host/ohci-q.c):
  periodic EDs coexist with control/bulk lists. ED skip stops new
  transactions, but the current transaction and done-list writeback must
  finish before reuse.
- [QEMU UHCI](https://github.com/qemu/qemu/blob/master/hw/usb/hcd-uhci.c),
  `uhci_frame_timer`: the frame timer uses QEMU_CLOCK_VIRTUAL, with 1 ms
  nominal frames and catch-up when dispatch is late.
- [QEMU OHCI](https://github.com/qemu/qemu/blob/master/hw/usb/hcd-ohci.c):
  periodic, control and bulk lists are serviced independently. TD NextTD
  is overwritten by done-list linkage on completion. DI=7 suppresses
  normal done-head writeback; use DI=0 and acknowledge WDH while collecting
  to keep done-list ownership bounded when several TDs coexist.

The previous core takes `transfer_lock` around the entire scan, all HID
polls and per-device sleeps. Each HID poll synchronously occupies the
single general TD/bounce buffer for up to two linked frames, followed by
two retirement frames. Storage keeps the same mutex across its complete
BOT command and up to 10 s of NAK waiting. Consequently a port debounce,
reset, another pipe or storage can postpone the next HID opportunity. The
250 ms scan deadline is not itself the worker's sleep interval; the outer
worker asks for 1 ms sleeps. The scheduler may dispatch later than this
request. TCG frame counts and PIT ticks are separate observations; host
models cannot establish the wall-clock cause of the supplied 1.4–2.5 s
QEMU observations.

Decision: give each bounded HID interface a private, nonblocking interrupt
slot, keeping the existing two-frame retirement rule and serialized
control/BOT ownership. A slot is 48 bytes (16-byte QH/ED, 16-byte TD,
16-byte boot report buffer), so all 64 possible interfaces fit in the
existing pinned 8 KiB DMA allocation. The boot report request is 8, 3 or
4 bytes; OHCI's MPS remains the endpoint's advertised packet size. The
OHCI dummy TD is shared and immutable. Slots remain pinned and are never
reassigned to a later device generation.

A separate P_DEVICE input worker progresses every pipe without performing
port scans or filesystem work. The scan/BOT owner reserves the general
slot with `busy`, drops the host mutex for every sleep, and also progresses
HID between 1 ms sleep slices. Other general transfers wait with the mutex
free; HID needs only a short critical section. LED/control requests and
device quarantine that requires hub control stay with the general owner.

Neither controller stamps the exact ACK frame in its TD. Runtime latency
records therefore explicitly report an upper bound from the last pending
TD observation to merged-queue delivery, plus the observed-completion to
delivery time. The fixture independently timestamps report availability,
controller ACK and production merged-queue delivery, so it can measure
both exact delays. Requested sleeps, measured dispatch gaps, frame counts
and retirement remain distinct; no host result qualifies TCG or hardware.

## Host evidence

Replayed on 2026-10-14 against base
`84268adebb92151972832ebf44783bc3035a29b6` plus the F1-52 worktree diff.
The interrupted session's six baseline USB sources were checked byte for
byte against `git show HEAD:src/kernel/usb/...` and recompiled with the
current register model and production merged input queue. A report becomes
available one frame into a 1,200 ms bulk NAK wait. Both UHCI and OHCI take
1,208 ms from availability to queue delivery, with 1,210 sleeps holding
the mutex and none releasing it. ACK-to-queue time is only 2 ms: most of
this reproduced delay occurs before HID gets a controller opportunity.
The scratch baseline was removed after retaining these measurements.

The permanent fixture `tests/host/usb/latency_test.c` timestamps device
availability, the modeled controller ACK and the production input event's
queue timestamp independently. It exercises both production controller
engines, boot keyboard and wheel mouse input, a live 1,200 ms bulk NAK
transfer, and debounce/reset/enumeration of another root device while
existing hub input continues. Ten arrival phases cover the 10 ms polling
interval. Across 28 cases, all 1,344 reports satisfy ACK-to-delivery
latency <= two polling intervals (20 ms); no input queue overflows.

| Controller scenario (both UHCI/OHCI) | HC frame period | Service dispatch gap | Availability to queue max | ACK to queue max | Logged upper bound max |
| --- | ---: | ---: | ---: | ---: | ---: |
| Bulk NAK, ten arrival phases | 1 ms | 1 ms | 10 ms | 2 ms | 3 ms |
| Bulk NAK, slower HC frame clock | 4 ms | 1 ms | 16 ms | 8 ms | 9 ms |
| Bulk NAK, delayed service dispatch | 1 ms | 10 ms | 36 ms | 19 ms | 20 ms |
| Bulk NAK, both clocks delayed | 4 ms | 10 ms | 36 ms | 18 ms | 20 ms |
| Concurrent scan/reset/enumeration | 1 ms | 1 ms | 9 ms | 2 ms | 3 ms |

The fixture probes the mutex at each sleep and requires it to be free.
Native lifecycle coverage dispatches the actual input worker while a
general-slot reservation remains in force; it verifies delivery without
changing the scan count or the general TD/buffer. Existing models cover
NAK toggle retention, a late ACK racing unlink (delivered exactly once),
two-frame retirement snapshots, clock wrap/stall, DMA corruption, detach,
LED output and controller/device quarantine. First-20 latency records and
one-second maxima are asserted for each pipe.

The outer workers request 1 ms sleeps; `task.c` wakes sleepers on PIT
ticks and its ordinary quantum is 10 ticks. A wake does not necessarily
preempt the running task immediately. The delayed-dispatch model therefore
separates requested sleep duration from actual service cadence. Retirement
still waits for two **hardware** frames: this contributes 2 ms in the
nominal fixture and 8 ms with its slower frame clock. No fixed sleep count
is treated as proof of retirement. The supplied `poll_frames=2 frames=4
elapsed_ms=4` records agree with nominal linked plus retirement work;
the occasional 49/99-frame record reveals delayed observation but does
not by itself explain seconds between desktop input batches.

Passed checks, run sequentially under `nice -n 19`:

- `bash tests/host/usb/run.sh`: production register/DMA, native lifecycle,
  input grant and latency fixtures; ASan/UBSan enabled.
- `bash tests/host/usb/storage_run.sh`: production BOT, filesystem and
  removable-storage regressions; ASan/UBSan enabled.
- `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`:
  complete kernel host suite, including the USB and storage suites.
  LeakSanitizer cannot inspect processes in this sandbox; address and
  undefined-behavior instrumentation remain enabled. The optional DOS
  guest runner reports its existing missing-dependency/payload skip.
- `PYTHONDONTWRITEBYTECODE=1 python3 scripts/build_kernel.py`: complete
  i686 kernel compilation and link with `--no-undefined`, stack-frame
  checks and FPU/SIMD audit passed.

These are host measurements, not TCG or hardware qualification. In
particular, the 36 ms availability-to-queue result under delayed dispatch
does not satisfy a 20 ms device-availability target; the specified
controller-ACK-frame-to-queue target does pass those fixtures. Arbitrarily
long dispatch gaps cannot meet it. Runtime `ms` is an upper bound, not an
exact ACK timestamp. The lead owns the runner-03 paced `f1-usb` rerun and
hardware validation, including checking that real service gaps permit the
20 ms target. No QEMU or full disk-image build was run in this worktree.
