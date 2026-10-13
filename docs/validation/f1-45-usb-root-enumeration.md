# F1-45 USB root enumeration: research and host evidence

2026-10-13. Host evidence only; the lead must rerun both `f1-usb` cases on
the rebuilt canonical image.

The controller-ready record is emitted by `usb_attach()` during `usb_init()`,
before the service runs. `task_create_kernel()` uses `task_alloc()`, which
leaves the task `T_BLOCKED`; `task_start()` publishes it to the runnable queue.
USB discarded the created task pointer without starting it. Consequently,
the existing dedicated P_DEVICE service never called `usb_step()`. Host tests
previously called `usb_step()` directly and did not cover native initialization.
The reported work overrun names `accel_work`, not USB enumeration; USB does
not submit enumeration to the common work queue. The fix starts the existing
dedicated task and retains fail-closed quarantine on allocation failure.

Sources compared with `uhci.c`, `usb_core.c`, `usb_hub.c` and the scheduler:

- [Intel UHCI Design Guide 1.1](https://ftp.yz.yamagata-u.ac.jp/pub/NetBSD/misc/blymn/uhci11d.pdf),
  §§2.1.1–2.1.7: CCS is read-only, CSC/PEC are W1C, port enable is explicit
  software control; reset disables the port. The frame list is programmed
  while stopped, before Run/Stop is asserted. Indexed specification text was
  accessible; direct PDF retrieval timed out.
- [USB 1.1 specification](https://ethw-images.s3.us-east-va.perf.cloud.ovh.us/ieee/f/f0/Usb11.pdf):
  reset signaling and recovery are §7.1.7.3 in this revision (§7.1.7.5 in
  USB 2.0). Keep the existing 100 ms debounce, 50 ms root reset, 10 ms
  recovery and 2 ms Set_Address recovery.
- [QEMU 9.2 UHCI](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/usb/hcd-uhci.c),
  `uhci_reset`, `uhci_attach`, `uhci_port_write`, `uhci_find_device`,
  `uhci_complete_td`, `uhci_frame_timer`: HCRESET reconnects attached devices
  with CCS/CSC, without PE; PORTSC preserves read-only bits and applies W1C;
  only enabled ports route transfers. RS starts the 1000 Hz frame timer,
  FRNUM advances modulo 2048, and successful transactions clear TD Active.
- [Linux UHCI root-hub polling](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/host/uhci-hub.c),
  `get_hub_status_data`, `uhci_check_ports`: polls change bits, clears reset
  and change status, explicitly sets PE; W1C bits are masked on ordinary
  control writes. Ciuki's first scan also checks CCS, admitting devices
  already connected even if firmware cleared CSC. Keep this behavior.
- [Linux PCI USB handoff](https://github.com/torvalds/linux/blob/master/drivers/usb/host/pci-quirks.c):
  UHCI C0 disables legacy SMI/PIRQ enables using defined RW/W1C masks.
  Ciuki already writes 0x8f00 and verifies the handoff before enabling DMA;
  controller readiness in the supplied transcript establishes that this
  check and the stopped-to-running sequence completed. No handoff change
  is needed to repair the unstarted service.

The external-hub path is called from the same serialized service only after
a root hub device is configured; it cannot explain the missing first scan
with two directly connected HID devices. Address-zero control packets use
one live TD and retire two frames before descriptor reuse. New diagnostics
will distinguish scan dispatch, port reset/enable and GET_DESCRIPTOR failure
on the lead's next run. No QEMU or hardware result is inferred from fixtures.

Runtime records use one-based port numbers (callbacks remain zero-based),
the retained controller owner and millisecond ticks. The first scan records
every root port, including an empty port; subsequent scans record observed
connection/enable changes. Reset records capture assertion and release,
followed by the explicit enable result. UHCI address-zero packet records
include USBCMD, USBSTS, FRNUM, TD status and two-frame retirement result.
GET_DESCRIPTOR records bracket the first eight-byte control request and
include its return value and elapsed time. Worker records distinguish task
publication, service entry, first scan and measured scans per second over
windows of at least 1000 ms. These are runtime observations, not IRQ events.

## Verification

- Regression before the fix: the existing register/DMA tests passed, but
  the new native `usb_init()` harness aborted at
  `created==1 && started==1 && worker.state==T_READY`. It replaces hardware
  and scheduling only, compiling the production native init/service path.
- `nice -n 19 bash tests/host/usb/run.sh`: PASS, ASan/UBSan. Native modes:
  ready, connected with CSC already cleared, empty root ports, task-creation
  failure, safe mode, forced firmware input, firmware input policy and no
  controller. Ready modes run for more than two seconds of model time,
  enumerate both devices once and check cadence/descriptor/retirement records.
  The QEMU-style PORTSC model checks 50 ms reset and 10 ms recovery, W1C
  change bits, disabled ports after HCRESET and enabled-port transfer routing.
- `ASAN_OPTIONS=detect_leaks=0 nice -n 19 bash scripts/test/host_kernel_tests.sh`:
  PASS, including USB mass-storage regressions. The initial unmodified
  invocation stopped because LeakSanitizer cannot operate under the sandbox's
  tracing restrictions. AddressSanitizer and UBSan remain enabled. The suite
  reported the unavailable native `-m32` runtime (target ABI compilation still
  ran) and skipped the optional DOSBOOT.IMG guest runner because its fixture
  was absent. No optional guest execution is claimed.
- `nice -n 19 python3 scripts/build_kernel.py`: PASS, full i686 kernel
  compilation/link and FPU/SIMD audit. This builds the kernel only; no disk
  image was built or used as boot evidence.
- `git diff --check`: PASS. Builds and host suites ran sequentially; no
  QEMU, systemd scope, commit, push or diary entry was performed.

The final first-scan empty-port observation was covered by a focused USB
suite rerun and the kernel build after the full kernel host suite passed.
Logs and the implementation report are under `build/codex/`.
