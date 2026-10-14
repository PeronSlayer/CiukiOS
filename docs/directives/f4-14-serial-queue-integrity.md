# f4-14 — Serial diagnostics garbled on the T23 since the f4-13 serial queue

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f4-14` from `main`. Host-testable; the lead verifies on the T23 capture.

## Evidence (T23, image 293c05af, boot `L`, 2026-10-14; raw capture `/home/peronslayer/Desktop/CiukiOS/legacy/local/physical/293c05af-garbled/serial.log`, sample `build/lead-scratch/serial-garble-sample.txt`)

497 of 726 captured lines are fragments of several records spliced together, for example:

```
[accel]ue=pending us=1162886
[audio] progremaining=0 piv=8 sr=000f publishts=0
[init] ata re_discovery activation_seq=4 tic[storage] kind=1 disk=0 part=0 md=0 reason=disk_scan activation_[storage] kind=2 …
```

Until f4-13 the capture was clean. f4-13 made serial output asynchronous: records enter an 8 KiB serial queue during playback and the millisecond timer sends one FIFO burst (16 bytes for a 16550); early boot and panic stay synchronous. On the T23 the record boundaries are lost: producers from different contexts (IRQ, work items, tasks) interleave at byte granularity, or the synchronous and queued paths run concurrently and alternate 16-byte bursts. The disk log (`C:\SYSTEM\LOGS`) keeps the producer records intact.

## Task

1. Reproduce in the host serial test (`tests/host/serial_audio_test.c`, `serial.c`): two producers logging concurrently while the queued burst sender runs; show the splice.
2. Fix: records are enqueued atomically (one producer appends a whole record under the queue lock; the burst sender only ever emits from the queue head, never from a producer's buffer); the synchronous path (early boot, panic) must drain or bypass the queue in order, never interleave; when the queue is full the record is dropped whole with the existing `[serial] result=log_dropped` accounting. Keep the no-stall guarantee for audio playback.
3. Host test: concurrent producers, drains, overflow, panic during queued output; every captured line is a complete record.

## Acceptance

Kernel host suites green; i686 kernel compiles; the next T23 capture is clean.

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f4-14-report.md`.
