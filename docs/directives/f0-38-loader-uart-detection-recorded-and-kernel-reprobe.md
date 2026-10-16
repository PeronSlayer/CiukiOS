# f0-38 — the T23 serial log disappeared: record the loader's UART detection step, re-probe in the kernel

- **Phase:** F0. **Model:** gpt-6.1-sol / high. **Worktree:** `wt/f0-38` from main (after `2fef3ffc`).
- **Authority:** `src/boot/ciukldr/platform.inc` (`uart_init`, `uart_putc`), `src/kernel/include/ciuki/boot_info.h` (byte 0x00F `reserved0`), `src/kernel/core/serial.c`, `src/kernel/core/hardware_check.c` (Serial burst item), `docs/design/boot-memory.md`, loader model/tests (`scripts/test/loader_model.py`, `tests/host/test_boot_modes.py`), `tests/host` serial tests.

## Evidence (T23, three trials on 2026-10-16, disk logs read-only)

- a79f840c (morning): serial capture 751 KB, H page `PASS Serial burst`.
- d598bf4c (afternoon): `boot_flags=00001a0a` (serial bit set by the loader) but H page `SKIP Serial burst: UART absent` and an empty capture: the loader found the UART and later disabled it (`uart_putc` `.disable`: THRE poll timeout or the 16384-character budget), or the kernel's `serial_init` IIR check failed.
- 43d72a5c (night): `boot_flags=00001a08` in all three boots: `uart_init` did not find the UART at all (BDA 40:00 ≠ 3F8h, scratch register, LSR, or divisor read-back), nothing was ever sent. No loader change touches `uart_init` between the three images. The owner will check the T23 BIOS Config → Serial Port setting; software must say which step failed.

## Required

1. Loader: `uart_init` records its outcome in boot-info byte 0x00F (`uart_state`: 0 unknown, 1 ok, 2 no BDA COM1, 3 scratch, 4 LSR, 5 divisor, 6 disabled by THRE timeout, 7 disabled by budget) — rename `reserved0` in `boot_info.h` and the layout table; `uart_putc` `.disable` updates it (6/7) and clears `CBI_F_SERIAL_LOG`.
2. Kernel: `[serial] loader_uart state=%u base=%04x divisor=%u` at boot; when state ≠ 1, `serial_init` probes 3F8h itself (scratch round-trip, LSR ≠ FFh, divisor read-back at 38400 8N1) and, if present, enables the UART log and records `[serial] kernel_probe result=…`; the H page Serial burst item prints `UART absent (loader step N, kernel probe M)`.
3. Host coverage: loader model and the assembled loader (step byte written on each path, flag cleared on disable); kernel probe with a fake UART (present/absent/timeout); `python3 -B -m unittest discover -s tests/host -p 'test_boot_modes.py'`, `test_loader_model.py`, `test_version.py`, serial host tests, `python3 scripts/build_kernel.py`, loader NASM build.
4. Report `build/codex/f0-38-report.md`.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. The loader must still fit 24576 bytes.
