# f4-16 — Serial output still garbled on the T23 after f4-14; the disk log must survive as the clean record

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f4-16` from `main`. Host-testable; the lead verifies on the T23 capture.

## Evidence (T23, image 8c88b1ab with f4-14, boot `L`, 38400 8N1, 2026-10-14; `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/8c88b1ab-garbled-usb-regression/serial-clean.log`)

304 of 659 captured lines are spliced fragments, now with a different shape than before f4-14: runs of about 16–30 characters from one record followed by the tail of another, as if whole chunks were lost on the wire rather than interleaved by producers:

```
[usb] uhci handoff controb] uhci schedule controller=usb-[usb] controller=uhci ports=2 pcueued tick=6085 controllers=3
[accel] savage step oppr8 reg=003c5 value=pending us=87336
```

f4-13/f4-14 send "one ready FIFO burst (16 bytes for a detected 16550)" per millisecond tick. At 38400 baud a byte takes 260 µs, so 16 bytes need 4.2 ms: unless the sender checks that the transmitter FIFO is empty (LSR THRE/TEMT) before every burst, and unless the FIFO is really enabled (FCR bit 0 and verified through IIR bits 7:6 = 11), bytes are overwritten in the holding register. Before f4-13 (synchronous polled output) captures were clean at this rate. The disk log (`SYSTEM/LOGS/BOOT.LOG`, 128 KiB ring, `src/kernel/core/bootlog.c`) is intact but was overwritten by the USB worker's per-second records during the session.

## Task

1. Serial sender: detect the UART type at init (scratch register, FCR write, IIR read-back), enable and verify the FIFO; the burst size is the verified FIFO depth (16 for a 16550A) only when LSR THRE is set at burst time, otherwise 1 byte per THRE; never write without THRE; keep the no-stall rule for audio (bounded work per tick). Host UART model with a real line rate and a 16-byte FIFO that overflows must reproduce the T23 splice before the fix and show clean records after; add a line-rate-aware test (38400 baud: at most 3.84 bytes per ms on average).
2. Disk log: 1 MiB, with a protected 128 KiB boot section (first records never overwritten) and a ring for the rest; per-second summaries of the USB workers and similar periodic records go to the ring only. Document the layout in `docs/boot.md`; the lead reads BOOT.LOG from the SSD after a test.
3. Both changes recorded with host tests; `scripts/test/physical.py` disk-log import unchanged or updated accordingly.

## Acceptance

Kernel host suites green; i686 kernel compiles; the next T23 capture has no spliced line and BOOT.LOG retains the boot records.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f4-16-report.md`.
