# f0-30 — Kernel latency audit on the T23: every interrupts-off section, timer gaps, console repaint (image a395ffa8)

- **Model/effort:** `gpt-6.1-sol`, `xhigh`. **Worktree:** `../CiukiOS-wt-f0-30` from `main`. Coordinate with f0-29 (serial transmitter, in progress): do not touch `serial.c`; your audit covers everything else.

## Evidence

- H page: `FAIL Timer 10 s: ms=10032 ticks=9976 lost=56 IF-off=1762us section=irq_save site=c0100938` = `pic_unmask` (`src/kernel/arch/pic_pit.c:35`): a PIC mask write holds interrupts off for 1.7 ms (193 violations during the page); `console_repaint_ms=1400` in the same boot.
- Normal boot (`[blackbox]` head of `disk-BOOT.LOG`): `stretch_max_ms=126 task=dosbox … irqoff_max_us=2117 timer_gaps=61/348`, `irqoff_section=irq_save site=c01234f0 violations=71` = `kwait_wait_until` (`src/kernel/core/sync.c:118`): a wait primitive holding the IRQ lock for 2.1 ms; later `serial_tx_byte site=c0122ff5 4229us violations=3675` (f0-29 handles the serial).
- Images since f0-21 have moved the attribution around (serial writer, i8042, calibration, PIC, waits) without an audit of the whole kernel.

## Task

1. Audit every `irq_save`/`cli` section in the kernel (arch, core, drivers, vm, fs, usb) with the attribution counters: list them in `docs/validation/f0-30-irqoff-audit.md` with their worst case on the host model and the reason; fix every section that can exceed 100 µs: `pic_unmask` (why 1.7 ms: slow ISA port I/O behind SMI/EC on the T23? a loop? measure with the cycle counter and make PIC mask updates a single OUT without waits), `kwait_wait_until` (never wait with interrupts off: the lock must be released around the wait), the console repaint (rows painted with interrupts enabled, bounded per call), the CPU sampler, the storage worker commit, the USB worker, the DOS VM monitor entry/exit.
2. Timer: zero lost ticks over the page's 10 s on the host model with the worst sections replayed; on hardware the next log must show `timer_gaps=0/…` and `irqoff_max_us` below 500 with the section named.
3. Wake latency: `resched_lat_max_ms=126` for the DOS VM task: find the holder and bound it.

## Acceptance

Kernel host suite green with the audit fixtures; `f0-smoke`, `f1-input`, `f3-vm` green on QEMU; T23 H page timer item PASS.

## Constraints

Worktree only, no commits, no QEMU, no `systemd-run`, no image builds, no diary. Host tests one at a time with `nice -n 19`, `rg` not semble. Build the kernel (`python3 scripts/build_kernel.py`) and the DOS image (`python3 scripts/build_dos_image.py`) before the report. Evidence (read-only): `legacy/local/physical/a395ffa8/` (`disk-BOOT.LOG` latest normal boot with the `[blackbox]` head of the previous boot, `disk-BOOT1.LOG` first boot, `disk-BOOT2.LOG` the Hardware check boot, `serial.log` corrupted). Owner's standing rule: every hardware decision is grounded in the supplied driver package (`third_party/Ultimate Windows Driver Pack (for vista,xp,98,95,me,2000,2003)/` with the INF/registry tables of the vendor drivers, `third_party/drivers/` DOS drivers) and in upstream open sources found on the internet (X.Org, Linux, FreeBSD, SeaBIOS, vendor datasheets); cite them in the report; never program registers from reasoning alone. Report `build/codex/f0-30-report.md`: root causes (file:line), fixes, verbatim test output, what the lead must verify on the H page and on the T23.
