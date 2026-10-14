# f1-55 — Savage on the T23: ports and alias probe pass, the engine quarantines on its idle check

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-55` from `main`. Host-testable; the lead re-tests on the T23 once captures are clean (f4-14).

## Evidence (T23, image 293c05af with f1-49, 2026-10-14; `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/293c05af-garbled/serial-clean.log`, garbled)

Fragments in order: `[accel] sa(vage) alias=49 match=0` (the optional alias probe mismatched and did not block), many port steps, `[accel] savage … step op=r32 reg=081c8 value=…` (scanout register read through the engine MMIO: the window answers), `[accel] savage idle mask=00e1ffff`, then `[accel] driver=savage ops=0 idle_waits=1 fallbacks=1 quarantined=1` for the rest of the session. The engine MMIO is alive (0x81c8 read), so the quarantine comes from the status/idle check (MM48C60 bits under mask 0x00e1ffff never reaching the expected idle pattern, or the bounded poll of f1-27 expiring).

## Task

1. From RE046-B (status register MM48C60 bit layout, the "3D/2D idle", "command FIFO empty", "BCI idle" bits) and the X.org/Linux savage drivers' idle criteria (`SAVAGE_IDLE` masks differ between Savage4, ProSavage and SuperSavage: the IX uses the Savage4-style mask but some bits read 1 permanently on SuperSavage) decide the correct idle mask for 5333:8c2e; record the raw status value in the quarantine record (`status=…`) and the attempt marker.
2. Make the first idle wait after reset tolerant and diagnostic: poll with the bounded budget, log the sequence of raw values at 1 ms intervals (first 8), and quarantine with the raw value in the record; if the reference shows the SuperSavage idle bits, use them.
3. Host tests with a fake chip whose status has the SuperSavage decoration; the Savage4 fake unchanged.

## Acceptance

Host suites green; i686 kernel compiles; the lead re-tests on the T23 (`[accel] savage ready`, `ops=` growing, no corruption).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-55-report.md`.
