# f0-33 — no PIC mask writes at run time: one OUT to the 8259 costs 2.1 ms on the T23

- **Phase:** F0. **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f0-33` from main `bbac0713`.
- **Authority:** `src/kernel/arch/pic_pit.c`, `src/kernel/core/registry.c` (chain deferral), `src/kernel/drivers/ata.c`, `src/kernel/drivers/i8042.c`, `src/kernel/core/serial.c`, `src/kernel/audio/backend.c`, `src/kernel/vm/biosvm.c`, their host tests, `docs/validation/f0-30-irqoff-audit.md` (amend).

## Evidence (T23, image e8dfd0cd, `legacy/local/physical/e8dfd0cd/serial.log`, read-only)

```
[kernel] irqoff_section=pic_unmask site=c0100984 irqoff_max_us=2109 violations=9
[kernel] pic_mask writes=5048 out_max_us=2108
[kernel] irqoff_section=schedule site=c0126db9 irqoff_max_us=2110 violations=5
[hwcheck] FAIL Timer 10 s: ms=10000 ticks=9924 lost=76 IF-off=2110us section=schedule site=c0126db9
```

`out_max_us` measures the OUT instruction alone (f0-30): a single write to the 8259 mask register takes about 2.1 ms on this ICH3-M/BIOS (an SMI trap is the only plausible mechanism; it is not ours to change). The counter grows by about 290 writes per two-second window during an idle desktop (about 145/s), i.e. roughly 30 % of the time with interrupts off, 76 lost timer ticks in 10 s, and 2.1 ms added to every event that toggles a mask. Runtime callers: `ata.c` masks and unmasks IRQ 14/15 around every command (lines around 683/703/856/860/917/1037), `registry.c` chain deferral, `i8042.c` toggles IRQ 1/12, `biosvm.c`, `serial.c` init, `audio/backend.c` SB16.

## Required

1. After initialisation the 8259 masks stay constant: every line with a registered handler stays unmasked; devices gate their own interrupts (ATA: nIEN in the device control register, which the driver already toggles; i8042: the controller command byte interrupt enables; USB/NIC/AC'97: their own enables). Mask writes remain only for quarantine, chain deferral and the hardware-check page, and are counted.
2. The chain deferral path (budget exceeded) stays as f1-72 defined it; everything else that masked and unmasked per request is removed with the device-side equivalent and documented against the device datasheet (ATA/ATAPI-6 device control nIEN; i8042 command byte bits 0 and 1).
3. Host coverage: the ATA, i8042, registry and audio fixtures count PIC writes: zero per request after init; the kernel latency test asserts the remaining mask-write sites. Target on the T23: `pic_mask writes` grows by at most 5 per two-second window on an idle desktop and the 10 s timer check loses no tick.
4. Run `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh` and `python3 scripts/build_kernel.py`. Report in `build/codex/f0-33-report.md` (each removed site, its device-side replacement with the datasheet reference, verbatim results). The lead runs f0-smoke, f1-input, f1-storage and f4-audio on QEMU and the H page on the T23.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`.
