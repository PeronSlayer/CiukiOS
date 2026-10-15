# f0-29 — Interrupt-driven serial transmitter: no polling, no interrupts-off bursts (T23, image a395ffa8)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f0-29` from `main`. Build the kernel before reporting. This directive is the authority on `src/kernel/core/serial.c`; f0-28's serial-duplication item is superseded by it.

## Evidence

`legacy/local/physical/a395ffa8/serial.log` (normal boot after the H page): `irqoff_section=irq_save site=c0122ff5 irqoff_max_us=4229 violations=3591` — `c0122ff5` = `serial_burst` (`src/kernel/core/serial.c:168`, kernel of main `f575e53a`); `[desktop] stall … worker=wallpaper/settings` repeated; the wallpaper never appeared; the capture still shows duplicated chunks (`serial recorcord=`, `serierial`). Three rewrites of the polling writer (f0-22, f0-24, f0-27) have each left a multi-millisecond interrupts-off path on the real 16550.

## Task

1. Replace the transmit path with the UART's own interrupt: enable THRE (IER bit 1) and the FIFO (FCR), and let the transmit interrupt handler move up to 16 bytes from the whole-record queue into the FIFO per interrupt, nothing else; producers only enqueue whole records under a short lock (bounded copy, no I/O); when the queue is full the record is dropped whole and counted (periodic diagnostics first; evidence records keep a reserved share of the queue, and task-context evidence producers may wait on a wait queue woken by the handler, never spin). No code path writes the UART outside the handler except the panic path (interrupts already off, drains synchronously, bounded).
2. Keep the receive side as is. Keep the 38400 8N1 line settings and the hardware FIFO probe; on QEMU the same code runs (the chardev raises THRE immediately).
3. Remove the pacing/quota loops and the per-byte locks; the only remaining IRQ-off sections are the queue operations (bounded by one record copy). The timing attribution record must show the serial sections below 100 µs on the host model and the page's serial check must report zero resent bytes and the drop counters.
4. Host model: a 16550 with THRE interrupts and a 16-byte FIFO; tests for ordering, whole drops, evidence reservation, panic takeover, and that no producer path touches the UART.

## Acceptance

Kernel host suite green; QEMU `f0-smoke`, `f1-usb`, `f3-vm` green; next T23 log: `irqoff_max_us` below 500 with no `serial_*` section among the violations, capture readable line by line.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary; host tests one at a time with `nice -n 19`. Report `build/codex/f0-29-report.md`.
