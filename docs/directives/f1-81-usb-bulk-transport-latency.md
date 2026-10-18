# f1-81 — USB stick reads cost 28 ms per command and 340 KB/s: retire bulk transfers without the two-frame polling floor

- **Phase:** F1 (USB). **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f1-81` from main (after `7ef68c1d`).
- **Authority:** `src/kernel/usb/uhci.c`, `ohci.c`, `usb_core.c` (`usb_retire_frames`, `usb_sleep`, interrupt path of f1-72/f0-39), `usb_storage.c` (BOT phases), host model `tests/host/usb/uhci_model.h`, tests `tests/host/usb/`, `docs/design/usb-contract.md`.

## Evidence (T23, image eca4d773, `legacy/local/physical/eca4d773/BOOT.LOG`/`BOOT1.LOG`, read-only)

- Owner: navigation of the stick in Files works but is slow; wants faster reading.
- `[usb-storage] bind … reads=9 sectors=9 max_sectors=1` with `mount_ms≈250–330` → about 28 ms per single-sector READ(10); `cadence addr=2 packets=121 poll_frames=704 retire_frames=244 batch=32` → about 6 frames polled per packet; H page `PASS USB storage: … bytes=1048576 read_ms=3073` → 340 KB/s for 64 KiB reads (f1-74 limit 128 sectors). A full-speed bulk pipe moves about 1 MB/s; a BOT command needs three phases, each currently waiting for `usb_retire_frames(h,6,2,…)`, which returns only after **at least two frames** (`usb_core.c:285-292`) and polls with 1 ms sleeps.
- Since f0-39 the controllers can run in interrupt or polling mode; f1-72 built an interrupt-mode host model.

## Required

1. Retire bulk TDs on completion, not on a frame floor: IOC on the last TD of each phase with the controller's interrupt path (already attached for IRQ 11), or, in polling mode, poll the TD status every frame without the two-frame minimum; the HID interrupt queue keeps its own schedule. Target on the host model: a single-sector READ(10) ≤ 3 ms; on the T23 (lead measurement) `mount_ms` ≤ 60 and ≥ 700 KB/s sequential in the H page item.
2. Keep every f1-74 recovery rule (CSW missing, reset recovery, stall clear) and the 128-sector limit; the data phase may chain TDs up to the limit in one schedule pass.
3. Directory and FAT reads from the cache already use runs (f1-78); measure the listing of a 300-entry directory on the host model before/after (commands and milliseconds).
4. Host coverage: `tests/host/usb/run.sh`, `storage_run.sh`, f1-74 recovery cases, the new latency assertions, `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`.
5. Report `build/codex/f1-81-report.md` with the before/after table.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. Register programming only as documented in UHCI 1.1 / OpenHCI 1.0a and already present in the drivers; no PIC writes.
