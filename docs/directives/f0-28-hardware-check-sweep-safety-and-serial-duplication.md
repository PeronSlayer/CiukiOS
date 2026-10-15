# f0-28 — Hardware check page locks at Savage sweep configuration 3; serial bytes duplicated on the T23 (image a395ffa8)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f0-28` from `main`. Build the kernel before reporting.

## Evidence (`legacy/local/physical/a395ffa8/serial.log`, 86 lines, read-only)

- The H page ran: USB storage SKIP (no stick), keyboard prompt, timer `RUN`, 100 ticketed serial records (`serial record=100/100`), `CR45 c0: reserved bi…`, then `Savage bpp=32 config=1 …`, `config=2 …`, `config=3 …` and nothing more: the machine locked on the third candidate of the sweep. The owner had to power off.
- The capture is still corrupted on hardware, now by **duplicated chunks**, not interleaving: `serial recorcord=009/100`, `serial rl record=013/100`, `serierial record=017/100`, `[hwchwcheck] serial record=073/100`: a few bytes already sent are sent again. f0-24's per-record cursor and per-byte lock (`serial_tx_byte`) or the FIFO refill resend bytes under THRE races on the real 16550 (the T23's port has a FIFO; QEMU's capture of the same image is clean).

## Task

1. **Sweep safety.** Before each candidate, write a black-box and disk-log record naming the candidate's full configuration (bit 3, BW_DISABLE, stride unit, CR50, tile, depth) so a lock identifies it; run each candidate under a watchdog (bounded idle wait, then engine reset through the documented reset sequence and a skip with `result=timeout`); never issue a command that can hang the bus without the ability to recover (if the reference drivers show that a configuration is invalid for this chip, exclude it); after any timeout continue with the next candidate instead of locking. Decode configuration 3's bits from the matrix order and record what it was; the page must keep its results even if the sweep is cut short (checkpoint after every candidate).
2. **Serial duplication.** Reproduce in the host 16550 model with a FIFO and THRE races: bytes must never be resent; the cursor advances exactly once per byte under its lock; the refill observes the FIFO level, not THRE alone, on hardware FIFOs; add the counter of bytes resent (must be zero) to the page's serial check.

## Acceptance

Kernel host suite green; QEMU H page boots and completes; on the T23 the page completes the sweep (or skips candidates with recorded timeouts) and the serial capture reads line by line.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary; host tests one at a time with `nice -n 19`. Report `build/codex/f0-28-report.md`.
