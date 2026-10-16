# f0-34 — boot log throughput: the always-on evidence must not be dropped behind verbose chatter

- **Phase:** F0. **Model:** gpt-6.1-sol / high. **Worktree:** `wt/f0-34` from main `4d6c8102`.
- **Authority:** `src/kernel/core/bootlog.c`, `src/kernel/core/output.c` (publication classes), `src/kernel/usb/*.c` (periodic diagnostics), `src/kernel/audio/vm_source.c` (`[vm-audio]` cadence), `src/kernel/vm/dosvm_perf.inc` (verbose dump volume), host tests, `docs/design/` logging contract (amend).

## Evidence (T23, image e8dfd0cd, `legacy/local/physical/e8dfd0cd/BOOT.LOG` and `BOOT1.LOG`, read-only)

- Second boot (games, flags `00001a0a` = SERIAL_LOG, VERBOSE, DIAG_VERBOSE): `[kernel] bootlog writes=275 bytes=9011200 metadata=54 rotations=3 dropped=24212`. The 64 KiB RAM ring (`BOOTLOG_RAM`) with a 32 KiB batch per drain and a one-second cadence (`BOOTLOG_DRAIN_MS`) could not keep up: 24,212 records were lost, among them the whole always-on `vm_perf_cpu/share/exits/port/vector/service` family of f3-31 (zero rows survive; only the verbose stop-time dump `vm_perf_cost`, `vm_db`, `vm_vec` is present).
- Volume by producer in the surviving 2,067 lines: `[usb]` 592 (per controller and per second: `schedule frame qh link element` 66×3, `worker … scans_per_second` 59×3, `hid latency`, `hid poll`), `CIUKI_TEST probe=vm-window` 590 (verbose dump), `[vm-audio]` 146, `[kernel]` 145, `[console]` 95.
- First boot (H page, flags `00002a0a`): no drops, 152 KB.

## Required

1. Publication classes: evidence that is produced at a bounded rate on ordinary boots (the five-second `vm_perf_*` family, hardware-check results, storage repair records, `[kernel]` two-second summaries) is **protected**: when the ring is full, verbose/diagnostic records are dropped first and protected records are never dropped while any droppable record exists; the `dropped` counter is split by class.
2. Drain capacity: the storage task drains the ring at the device's pace, not one 32 KiB batch per second: when the backlog exceeds the high-water mark, drain repeatedly within the existing storage-task budget (the f0-25 ordering and the no-IRQ-lock rule unchanged); measure on the host model with a 4 MB/s and a 300 KB/s device.
3. Chatter: per-controller USB periodic lines (`schedule frame …`, `worker … scans_per_second`, `hid latency`, `hid poll`) are DIAG_VERBOSE-only or summarised once per minute per controller; `[vm-audio]` periodic lines likewise. Nothing that the T23 diagnosis relied on (`[usb] controller=… mode=`, device/port events, quarantine, usb-storage stages, csw/quarantine/reset lines) changes.
4. Host coverage: ring pressure with a slow device keeps every protected record; the record-scope audit (`tests/host/record_scope_test.py`) learns the classes. Run `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh` and `python3 scripts/build_kernel.py`. Report in `build/codex/f0-34-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`.
