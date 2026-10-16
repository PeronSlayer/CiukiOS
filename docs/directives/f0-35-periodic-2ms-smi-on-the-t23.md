# f0-35 — a periodic 2.1 ms interrupts-off stall survives the removal of PIC writes: find the SMI source

- **Phase:** F0. **Model:** gpt-6.1-sol / xhigh. **Worktree:** `wt/f0-35` from main `2d6990cf`.
- **Authority:** `src/kernel/core/timing.c` (stall attribution), `src/kernel/core/hardware_check*.c` (new H item), `src/kernel/core/registry.c` (read-only PM/SMI register inspection), host tests, `docs/validation/f0-30-irqoff-audit.md` (amend).

## Evidence (T23, image a79f840c, read-only serial)

```
[kernel] pic_mask writes=9 out_max_us=1
[kernel] irqoff_section=schedule site=c0127969 irqoff_max_us=2108 violations=15
[kernel] irqoff_section=irq_save site=c0127d20 irqoff_max_us=2107 violations=10
[hwcheck] FAIL Timer 10 s: ms=10000 ticks=9924 lost=76 IF-off=2102us section=schedule site=c0127969
```

With f0-33 the PIC mask writes are nine per boot, yet the 10 s timer check still loses exactly 76 ticks (as on e8dfd0cd, where the 2.1 ms was attributed to `pic_unmask`): the stall is not caused by our code; a periodic event of about 2.1 ms with interrupts effectively off (an SMI, invisible to the kernel) lands on whichever IRQ-off section is current. 76 lost ticks in 10 s at 2.1 ms each is about 7–8 events per second. The only chipset documentation we own is the ICH3-M datasheet (`legacy` references in f0-30): SMI_EN/SMI_STS at PMBASE+30h/34h (LEGACY_USB_EN, PERIODIC_EN, SWSMI_TMR_EN, TCO_EN, APMC_EN, BIOS_EN, GBL_SMI_EN), and the USB_LEGKEY SMI enables that f1-71 already clears.

## Required

1. Measure, do not guess: record the timestamps (TSC) of every IRQ-off stall above 1 ms for the 10 s timer item and report their period and jitter; read PMBASE from the LPC bridge (D31:F0 0x40), then SMI_EN, SMI_STS, PM1_STS/EN, GPE0_STS/EN, TCO1_STS and USB_LEGKEY of the three UHCI functions, and print them on the H page and in the log (read-only). No register write in this step.
2. From the datasheet and the measured period, name the source (e.g. LEGACY_USB_EN with the BIOS polling the UHCI controllers we own, PERIODIC_EN, SWSMI timer, TCO). Propose, with the exact datasheet section, the single documented bit whose clearing stops that source without losing anything we rely on (we own USB, PS/2 through the EC's i8042 interface, ATA; we do not use ACPI). Implement it behind the H page first as an item that clears the bit, re-measures the stall for 10 s and restores the bit, so the owner's photo proves the effect before any normal boot uses it.
3. Host coverage for the measurement and the restore path; run `ASAN_OPTIONS=detect_leaks=0 bash scripts/test/host_kernel_tests.sh`, `python3 scripts/build_kernel.py`. Report in `build/codex/f0-35-report.md`. The lead runs f0-smoke and the H page on the T23.

## Constraints

Use `rg` and `sed`, never `semble`. No QEMU, no `systemd-run`, no image builds, no commits, no diary edits, host tests one at a time with `nice -n 19`. No chipset write outside the H page item in this directive.
