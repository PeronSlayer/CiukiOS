# f1-66 — USB stick: mounted as E: but quarantined at the 20 s budget; USB mouse wheel absent (T23, image a4f67ac1)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f1-66` from `main`.

## Evidence

- Serial fragments of the first boot: `[usb-storage] stage=mount addr=2`, `volume drive=E part=1 fat=32`, `[usb-storage] mounte…`, then `quarantine addr=2 …` and `elapsed_ms=21311` with `scans_per_second=4.000`: f1-64's fix lets the stick enumerate and mount, but the bind exceeds the 20 s budget of f1-62 and is quarantined; the owner sees a slower boot and no stick. Records `[usb-storage] stage_record … queued=3278 … result=-11 ring_queued=842` show the stage records themselves competing for the log ring.
- `[usb] hid mouse attached addr=1 interface=1 wheel=0` (Logitech receiver 046d:c52b, boot protocol): the wheel never arrives; the owner reports the wheel does not work.

## Task

1. Measure where the 21 s goes (per-stage `elapsed_ms` is already recorded): bulk transfer sizes and NAK polling cadence on UHCI, the FAT32 mount reads (how many sectors, request size), the log stage records. Make the bind fit the budget with margin (target under 5 s for a 4 GiB FAT32 stick: 64 KiB bulk reads, mount reading only what it needs, no per-stage log durability) and never quarantine a device that has already mounted: a budget overrun after the mount keeps the volume and records the overrun.
2. Mouse wheel: parse the HID report descriptor of mouse interfaces and use the report protocol when a wheel (Usage Wheel 0x38) or extra buttons are declared; keep boot protocol as the fallback; deliver `CIUKI_INPUT_WHEEL` to the desktop (the toolkit already scrolls on it). Host test with the receiver's descriptor captured in the log (add a descriptor dump record at attach, bounded).

## Acceptance

USB host tests green with the new cases; QEMU `f1-usb` cases plus a new `usb-wheel-uhci` case; next T23 log: stick mounted within budget, wheel events in the desktop. Files: `src/kernel/usb/usb_storage.c`, `uhci.c`, `usb_hid.c`, `src/kernel/fs/mount.c`.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Evidence (read-only): `legacy/local/physical/a4f67ac1/` (`disk-BOOT.LOG` = last boot only, clean; `serial.log` = three boots, interleaved fragments). Plain technical prose, no names of other operating systems, no filler. Report `build/codex/f1-66-report.md`: root cause (file:line), files changed, verbatim test output, what the lead must verify on QEMU and on the T23.
