# F1-46 USB-only input startup and external hub observations

2026-10-13. Host implementation; QEMU qualification belongs to the lead.

## Research and repository comparison

- [QEMU 9.2 hub source](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/usb/dev-hub.c),
  `qemu_hub_hub_descriptor`, `usb_hub_handle_reset`, `usb_hub_handle_control`
  and `usb_hub_find_device`: the 0409:55aa hub defaults to eight ports and
  no power switching. GET_STATUS returns four little-endian bytes. Port
  indexes are one-based; reset completes immediately, enables an attached
  child and sets C_RESET and C_ENABLE. Only enabled ports route packets.
- [Linux 6.12 hub reset](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/usb/core/hub.c),
  `hub_port_wait_reset` and `hub_port_reset`: observe reset completion and
  connection/enable status, clear change features and allow reset recovery.
  Ciuki retains its bounded reset poll and 10 ms recovery, without importing
  Linux retries or broadening the one-hub-level contract.

Ciuki's hub descriptor buffer already accommodates QEMU's ten-byte descriptor.
The previous host fixture had two ports, individually switched power and
allowed child routing without checking downstream enable. Model the QEMU
eight-port descriptor, power/status/change requests and enabled-child routing
through the production UHCI/OHCI packet engines. Add records for initial empty
ports, connections, disconnections, reset, enable, enumeration and failures.

`grants_install()` and the supervisor spawn path do not gate on input hardware.
The desktop sets its inherited input grant nonblocking and keeps servicing IPC
and drawing after read errors. Its setup failures return 1; there is no
device-absence exit with status 0. The actual supplied transcript reports
`case=launch ... reason=ok`, one completed fault cycle, then
`case=restored ... equal=1 processes_equal=0` and
`reason=restored_contract` while `live=1 survivor=1`. The controller's
`desktop_remove()` calls `proc_stop(p,0,0)` during failed-probe cleanup; the
supervisor then logs the status-0 stop. This is not an autonomous desktop exit.
The transcript also ends before external-hub enumeration has completed, so it
does not establish a hub failure. Investigating the separate process-ledger
mismatch is outside this directive's input and hub implementation scope.

## Input decision

Input grants represent the merged ordered queue for their lifetime. An empty
queue returns EAGAIN for O_NONBLOCK and waits interruptibly otherwise, even
with no device or a quarantined i8042. Read queued detach releases after the
last device disappears. Invalid grants, user buffers and capacity retain
their errors. No input-dependent supervisor spawn condition is introduced;
`[init] input` continues to describe its original backend activation attempt.

`input_devices_snapshot()` exposes live PS/2, firmware and USB producer counts
under the queue's IRQ protection. Qualified PS/2 and firmware backends each
provide the keyboard/mouse pair (two); USB counts each successfully activated
HID interface, rather than controllers, hubs or storage. `usb_input_ready()`
means at least one attached HID interface. `[input] devices=N source=...`
records each attach/detach, including quarantine, with event and per-source
counts. No physical-presence probing or PS/2 hot-plug recovery is added.

The desktop needs no new device notice: absence is normal EAGAIN through its
existing nonblocking drain. Host coverage runs the production desktop main
loop through the production supervisor/grant seam, first without input, then
with delayed USB keys, and checks drawing and IPC progress before attachment.

## Verification

- Production USB UHCI/OHCI/core/hub/HID suite: PASS, ASan/UBSan, including
  the eight-port hub at root port 2, mouse on downstream port 8, held-state
  release, reconnect generations and recursive hub detach. Reset timeout,
  status STALL and overcurrent remain bounded and quarantined without retry.
- Complete kernel host suite: PASS, ASan/UBSan (LeakSanitizer disabled for
  sandbox restrictions). Includes interruptible blocking reads with no input,
  a keyboard attaching while blocked, input producer counts and USB storage.
  Native `-m32` execution was unavailable; target ABI checks passed. The
  optional DOSBOOT.IMG guest fixture was absent.
- Desktop gate: 10 tests PASS. The production main loop/supervisor/grant seam
  draws and services IPC without devices, receives USB keys after five seconds,
  and remains alive with no input for six seconds. Setup-error controls remain.
- Desktop regression collection: 79 tests passed, five require an existing
  desktop payload build and were skipped. Two initial setup errors (incorrect
  package discovery and an absent pinned Lua archive) were resolved; the two
  affected modules passed their three tests. No product change was needed.
- Firmware records, USB runner, inventory/UI and performance: 20 tests PASS.
  These cover the final placement of PS/2 attach logging outside the recorded
  activation duration, following the full kernel host pass.
- Full i686 kernel compilation/link and FPU/SIMD audit: PASS; kernel-only build,
  no image. `git diff --check`: PASS.

Commands and logs are recorded in `build/codex/f1-46-report.md`. Builds/tests
ran sequentially with `nice -n 19`. Generated review-frame directories were
removed after validation. The f1-usb and f2-desktop suite cases remain unchanged.
No QEMU or hardware pass is inferred from these fixtures; the supplied
process-ledger failure still needs lead investigation.
