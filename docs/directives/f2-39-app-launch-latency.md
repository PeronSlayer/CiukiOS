# f2-39 — Application launch latency on real hardware: measurement and fixes

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f2-39` (branch `wt/f2-39` from `main`). Host-testable; the lead measures on the T23 (serial log with `L`).

## Owner request (2026-10-12)

On the T23 programs take too long to open after the click; the desktop feels unresponsive.

## Scope

1. **Measure first.** Add verbose-boot (`L` menu entry, `console` verbose) serial records on the launch path: desktop click → `spawn` → ELF loaded → first frame, with monotonic microseconds, for each program (`[launch] name=files spawn_us=.. elf_us=.. first_frame_us=..`), plus per-program read volume (bytes read from the volume until the first frame) and page-fault count. Keep them off in quiet boot. Add the same timing to the host launcher harness so the breakdown is testable.
2. **Known costs to attack**, each behind a measurement: (a) every libdeskui program loads and decodes the whole Tango store `/system/icons/tango.cis` (726 KiB, 246 names × 4 sizes) at start: decode lazily per icon name/size on first use and keep the file mapped (or read only the directory and the entries needed); (b) fonts (`ui_fonts.h` are compiled in: check) and the portrait; (c) ELF loading: load segments with few large reads (the ATA driver is PIO: measure the read size per request and add read-ahead/multi-sector requests for sequential reads if the F1 ATA driver supports it; propose the driver change in the report if it needs the kernel); (d) the desktop's work between the click and the spawn (menu close, repaint, synchronous settings/log writes); (e) the first-frame path (CONFIGURE round trip, surface allocation, first full render of the app).
3. Target: files/CiukNote/Control Panel open with their first frame within ~300 ms after the click on a Pentium III 1.13 GHz with PIO disk; report the measured breakdown before/after on the host model and what the lead must confirm on the T23.

## Tests (host)

Launcher harness timing records, lazy icon store decode (cache correctness: same pixels as the eager decode, bounded memory), read-size accounting in the host VFS model.

## Constraints

No functional UI change; no QEMU, no commits, no diary, `nice -n 19`. Report to `build/codex/f2-39-report.md`.
