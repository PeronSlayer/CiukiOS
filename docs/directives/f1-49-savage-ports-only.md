# f1-49 — Savage on the T23: the MMIO VGA alias stays dead after CR40.0; use the ports for every VGA register

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-49` from `main`. Host-testable; the lead runs the T23 capture.

## Evidence (T23, image e86305de, 2026-10-13; transcript `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/e86305de-usb-storage-fault-optical-missing/serial-clean.log`)

```
[accel] savage phase=marker_write us=337704 error=0
[accel] savage attempt persisted
[accel] savage quarantined reason=mmio_alias_dead us=360153 reset_us=0 poll_us=0 trace_us=0
```

With f1-41 the port path passes (3c3, MISC, CR38/CR39 unlock, CR40.0 set and read back), but the probe of the MMIO VGA alias (CR38 read at BAR0+0x83d4/5) still does not match the port value: on this SuperSavage IX/C the alias is not usable (or needs a different window offset). Everything the sequence needs from the VGA side (SR08 unlock, SR26 pipe select, MISC, CR66 reset pulse, CR40/CR31/CR50 programming, the cursor CR registers) can go through the legacy ports; the engine registers (0x8168/0x816c GBD, 0x8128–0x8134, 0x48c0c–0x48c18 BCI, 0x48c60 status, 0x81c8) live in the MMIO window itself and are not VGA aliases.

## Task

1. Route every VGA/CRTC/sequencer/MISC access of `savage2d.c` through the ports (keep the MMIO alias only as an optional probe that logs its result and never blocks). Research the SuperSavage/Savage4 register references (RE046-B; S3 SuperSavage documents if found) for the MMIO window layout: whether the VGA alias sits at a different offset on SuperSavage (some references place "new MMIO" VGA registers at 0x8000 of a 512 KiB window only when CR40.0 **and** the memory-mapped I/O enable in CR53/CR3A are set — verify and cite).
2. Then probe the engine MMIO independently (read a register with a known reset value, e.g. 0x8168 GBD1 or the BCI control after the CR66 reset) and continue with the f1-27 sequence; keep the chip-bug rule (no 0x48c60 read before the completed reset).
3. The attempt marker costs 338 ms per boot (`phase=marker_write`): write it asynchronously after the attempt decision or keep it in the settings volume's existing journal with one small write; it must still protect against a lock-up during the engine probe.
4. Host tests with a fake chip whose VGA alias is dead but whose engine registers respond.

## Acceptance

Host suites green; i686 kernel compiles. The lead qualifies on the T23 (`[accel] savage ready`, `ops=` growing, no visual corruption).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-49-report.md`.
