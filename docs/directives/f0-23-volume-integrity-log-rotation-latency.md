# f0-23 — Volume read-only after a forced power-off, log kept across boots, two latency sources (T23, image a4f67ac1)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f0-23` from `main`.

## Evidence

- Owner's report (three boots): after the DOS full-screen crash (black screen, forced power-off) a later session answered "I/O error" to every game launch (Quake, DOOM, Wolf3D read from the games image) and, after a reboot, the system volume answered I/O errors and refused to save the settings. On this workstation the same disk mounts read-only without a single read error on 1743 files (241 MB) and no filesystem warning: the on-disk FAT is consistent for an ordinary reader. The CiukiOS FAT driver marks the volume read-only with `FAT_RO_CORRUPT` and answers `-EUCLEAN` from `corrupt()` (`src/kernel/fs/fat.c:11`) on any chain or bound check (lines 14, 29, 51, 88, 93, 195–250, 403–445, 491), and a quarantined device answers `-EIO`. f0-22's log writer leaves, after a crash, an allocated tail beyond the last published length (`docs/validation/f0-22-hardware-log-cost-and-timer-gaps.md`): the prime suspect is that a mount-time or first-write chain check treats that tail (or the log's reserved chain, or a chain walked past `size`) as corruption and locks the whole volume.
- The disk log holds only the last boot (`BOOT.LOG` truncated at activation): the boot with the crash and the boot with the I/O errors are lost. The serial capture of the three boots is interleaved beyond recovery (`serial.log` lines such as `[kernel]qoff_max_us=2118 timer_gaps=11/6_ms=0 serial_drop=0 console_repa[kernel] bootlog …`): the single-writer rule of f0-22 does not hold on hardware, or records longer than the UART queue slot are split.
- `[kernel] stretch_max_ms=4657 task=probes#6 eip=c012070d resched_lat_max_ms=4657` = `timing_calibrate` (`src/kernel/core/timing.c:82`): a 4.6 s non-preemptible calibration in the probes task while the Quake window was starting; the desktop recorded `input_lag_us=8459000` in that window.
- `[kernel] irqoff_section=irq_save site=c0134ccd violations=9` = `read_port` (`src/kernel/drivers/i8042.c:423`): nine interrupts-off stretches above the bound in the PS/2 controller port read; `irqoff_max_us=915` in the last boot, 2118 in another.
- Last boot: `bootlog_drain_ms=9`, `timer_gaps=0/0`, `disk_log=available error=0`: the drain fix works when nothing else interferes; other boots show `timer_gaps=11/6…`, `14/96`.

## Task

1. **Volume integrity.** Host FAT fixture reproducing a power-off with an unpublished log tail (and with a partly written 32 KiB block), then a mount and the ordinary operations (read a file, write the settings, launch a program through the DOS disk service): the volume must stay writable and every file readable. The log file's chain is repaired or re-reserved at activation; no chain check may lock the volume for the log's own tail; a locked volume must say why in one record (`[storage] readonly reason=… file=… cluster=…`) and the desktop must show that reason. Audit every `corrupt()` site for checks that can fail on a valid volume with an over-long chain.
2. **Logs across boots.** At activation rotate `BOOT.LOG` to `BOOT1.LOG`, `BOOT2.LOG` (keep the last three boots) within the existing reservation budget, so a crash boot is readable from the next boot's disk.
3. **Serial writer.** Prove on host that two producers cannot interleave bytes (records of 240 bytes and of 2 KiB console envelopes), and bound every record to the queue slot; check the IRQ-time path and the panic path.
4. **timing_calibrate.** No calibration longer than 50 ms on a running system; the boot-time calibration keeps its accuracy; a VM start must not recalibrate.
5. **i8042 read_port.** No interrupts-off wait on the controller status: poll with interrupts enabled or bound the wait to 100 µs with a record.

## Acceptance

Kernel host suite green with the new fixtures; QEMU `f0-smoke`, `f1-usb`, `f1-input`, `f3-vm` green; next T23 log: three boots on disk, `irqoff_max_us` below 500, `stretch_max_ms` below 50 outside boot, no read-only volume after a forced power-off. Files: `src/kernel/fs/fat.c`, `vfs.c`, `mount.c`, `src/kernel/core/bootlog.c`, `serial.c`, `output.c`, `timing.c`, `src/kernel/drivers/i8042.c`.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Evidence (read-only): `legacy/local/physical/a4f67ac1/` (`disk-BOOT.LOG` = last boot only, clean; `serial.log` = three boots, interleaved fragments). Plain technical prose, no names of other operating systems, no filler. Report `build/codex/f0-23-report.md`: root cause (file:line), files changed, verbatim test output, what the lead must verify on QEMU and on the T23.
