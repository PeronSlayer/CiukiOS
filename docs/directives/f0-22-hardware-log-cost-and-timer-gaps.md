# f0-22 — Boot-log drain, verbose records and timer gaps on the T23 (image a738204a)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f0-22` from `main`.

## Evidence (T23, disk log lines 2812–3090 while the graphics demo ran)

- `[kernel] long_paths serial_spin_ms=0 serial_drop=14 console_repaint_ms=0 bootlog_drain_ms=247 bootlog_queued=711 …` then `bootlog_drain_ms=257`, `269`: the drain of the disk log costs about 250 ms of every 2 s window.
- `[cpu] top=desktop#18:342k192s19,storage-#8:125k125s1,…` and later `storage-#8:132k`, `136k`, `146k`: the storage task takes 125–146 permille of the CPU continuously; the demo task (`gfxdemo#29`) gets 43–372 permille and the owner measured 10 frames per second (25–40 on the images of 2026-10-13).
- `[kernel] stretch_max_ms=19…29 task=desktop#18 … irqoff_max_us=4230 timer_gaps=421/2866` then `424/2879`, `431/2886`: 15 % of the timer ticks are lost and interrupts stay off for up to 4.2 ms.
- Log volume: 3725 lines in 470 s, of which 212 `[usb] schedule frame=… qh=…` dump lines, 32 `[usb] hid pipe` poll lines, 365 per-second `group=vm_perf_*` records, two `[accel] driver=savage …` lines per second.
- `serial.log` of the same boot has interleaved records (`CIUKI_TEST v=1 map_us=0 burst_insn=0 latch_readvm=1 n=5 guest_us=1444 monitor_uup=vm_perf_wall …`): two writers share the serial line.

## Task

1. **Drain cost.** Make the boot-log drain on the storage task cheap: coalesce into writes of at least 32 KiB, at most one drain per second unless the queue is above a high-water mark, no FAT or directory metadata write per drain (append inside preallocated clusters; update the directory entry at most every 5 s and at shutdown), never with interrupts disabled. Targets on the T23: `bootlog_drain_ms` below 20 per 2 s window, storage task below 10 permille while idle, `timer_gaps=0`, `irqoff_max_us` below 500. Keep the existing counters so the next T23 log proves it.
2. **Interrupt-off stretch.** Find the 4.2 ms interrupts-off section (candidates: ATA PIO transfer of the drain, console repaint, the `[cpu]` sampler) and bound it to 500 µs; add a counter naming the longest section.
3. **Verbose records off by default on hardware.** The UHCI schedule dumps, the `hid pipe` poll traces, the per-second `vm_perf_*` records and the per-second `[accel] driver=` line move behind a verbose flag (a `boot_flags` bit and the fw_cfg test selector): off by default, on for the QEMU probes that assert on them (check every suite predicate that reads them and keep those cases green).
4. **Serial writer.** One writer only: every record path goes through the f0-21 queue; nothing writes the UART directly after the queue exists. Add a host test that two concurrent record emitters never interleave bytes.

## Acceptance

`bootlog_test.c` and `test_kernel_latency.py` extended (batching, rate, metadata cadence, verbose flag default); kernel host suite green; QEMU `f0-smoke`, `f1-usb`, `f4-audio`, `f3-vm` cases that read the moved records still green with the flag set by their selector. Files: `src/kernel/core/bootlog.c`, `src/kernel/include/ciuki/bootlog.h`, `src/kernel/core/cpustat.c`, `src/kernel/usb/uhci.c`, `src/kernel/vm/dosvm_perf.inc`, `src/kernel/drivers/savage2d.c` (log line only), the storage task.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Evidence files are read-only: `legacy/local/physical/a738204a/disk-BOOT.LOG` (T23 disk log of image a738204a, main `56d41c09`) and `serial.log` beside it. Plain technical prose in every document you touch, no product names of other operating systems, no filler. Report `build/codex/f0-22-report.md` with: root cause (file:line), files changed, test output verbatim, what the lead must verify on QEMU and on the T23.
