# f2-43 — Hardware id database from the owner's driver pack (INF files) for the Device Manager and the hardware baseline

- **Model/effort:** `gpt-6-luna`, `medium` (parsing and scripting). **Worktree:** `../CiukiOS-wt-f2-43` from `main`. Host-testable.

## Context

The owner placed a Windows driver collection in `third_party/Ultimate Windows Driver Pack (for vista,xp,98,95,me,2000,2003)/` (1.6 GiB, 909 `.inf`, 844 `.sys`, 14 `.vxd`; git-ignored, never committed, never shipped: Windows binaries cannot run as CiukiOS kernel drivers and are not redistributable). What is useful now is the hardware identification inside the `.inf` files: PCI/USB vendor and device ids with their device names, and the list of chipsets the Windows 98/ME/2000/XP era used.

## Task

1. `scripts/build_hwids.py`: parse every `.inf` in the pack (read-only, path from an environment variable or argument; the script must also work on any other INF collection) for `PCI\VEN_xxxx&DEV_yyyy` (and `&SUBSYS_`) and `USB\VID_xxxx&PID_yyyy` ids with their `%desc%` strings resolved from the `[Strings]` sections, deduplicate (prefer the shortest descriptive name), and emit `assets/hwids/pci-ids.json` and `usb-ids.json` (sorted, deterministic, ids as lower-case hex, names as plain ASCII ≤ 64 chars). Only ids and names: no driver file names, no vendor text blocks, no binaries. Record the generation provenance (pack name, file count, date) in `assets/hwids/README.md` and keep the JSON under 400 KiB (drop entries without a device name).
2. The Device Manager (apps/control, call 76 snapshot) uses the database for the device names (fallback to the existing small table, then to class names). Install the JSON in the image under `/System/hwids` and load it lazily.
3. A derived report `docs/design/hardware-id-coverage.md`: how many of the baseline chips of `docs/design/hardware-baseline.md` (and the T23/E500 PCI ids from the serial logs: S3 SuperSavage 5333:8c2e, Intel ICH3-M parts, PRO/100 VE) are covered, and a list of the ten most common vendors in the pack, as input for driver priorities (NIC, audio, display).
4. Tests: parser fixtures (INF with sections, strings, duplicates, malformed lines), determinism, size bound, Device Manager lookup.

## Constraints

No QEMU, no commits, no diary; `nice -n 19`. Report `build/codex/f2-43-report.md`.
