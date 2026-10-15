# f0-24 — Serial records still interleaved on the T23 and timer gaps back at 20 % (image 5e018550)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f0-24` from `main`. Build the kernel before reporting.

## Evidence

`legacy/local/physical/5e018550/serial.log` (588 lines, two boots): nearly every line is a concatenation of fragments of several records (`[storage] kind=2 disk=0 p channel=1 unit=0 identified=1 tnnel=1 unit=0 stage=sense opcode result=0 …`, `irqoff_section=irq_snt_ms=2094 bootlog_drain_ms=3 bo32768 metadata=0 rotations=0 dros=1`), on hardware only: the QEMU captures of the same image are clean. f0-22 and f0-23 made every producer go through one queue and bounded publications; the hardware capture shows that records are still cut and merged on the wire (candidates: the UART drain emitting from two contexts (task and IRQ/panic path) or from the console envelope path; the 16-byte FIFO gate; records longer than a slot; the capture tool at 38400 baud losing bytes under bursts: compare the record volume per second with the 3.84 KB/s line capacity and rate-limit or drop whole records with a counter, never fragments).

`timer_gaps=10/54`, `20/…`, `27/…`, `42/196`, `44/22x`, `60/304` growing through the boot (the last boot of image a4f67ac1 had `0/0`), `irqoff_max_us=2120`, attribution `irqoff_section=irq_save` with the site cut off. New since a4f67ac1: f0-23 (i8042 pacing, calibration, log rotation), f1-66 (USB bulk queueing, HID report protocol), f3-26 (persisted checkpoints), f1-67, f2-70, f7-01.

## Task

1. Make the serial path provably whole-record on hardware: one drain context, records never split across the FIFO gate, a byte budget per second with whole-record drops counted in `serial_drop`, and a host test that models the 16550 FIFO at 38400 with bursts above the line rate.
2. Find the 2.1 ms interrupts-off section (make the attribution record survive: emit it as a short, separate record) and the source of the new timer gaps; fix it. Suspects: the USB report path at every poll, the i8042 pacing change, the storage worker's checkpoint commits.

## Acceptance

Kernel host suite green; next T23 serial capture readable line by line; `timer_gaps=0/…` and `irqoff_max_us` below 500 in the disk log.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary; host tests one at a time with `nice -n 19`. Report `build/codex/f0-24-report.md`.
