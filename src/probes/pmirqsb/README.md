# PMIRQSB DOS/4GW probe

`PMIRQSB.EXE` is a focused DOS/4GW protected-mode Sound Blaster IRQ/DMA probe.

It is intentionally separate from Stage1. The goal is to answer one narrow
question before changing the kernel path: can a DOS/4GW protected-mode program
receive the SB IRQ after programming DSP DMA playback under CiukiOS?

Build requires OpenWatcom:

```sh
bash scripts/build_pmirqsb_dos4gw.sh
```

When `wcl386` is available, `scripts/build_full.sh` builds and injects:

- `\SYSTEM\DRIVERS\PMIRQSB.COM` launcher
- `\SYSTEM\DRIVERS\PMIRQSB.LE` DOS/4GW protected-mode payload
- `\SYSTEM\DRIVERS\DOS4GW.EXE` runtime

Without OpenWatcom, the full build skips the protected-mode payload.

Expected serial markers:

- `[PMIRQSB] BEGIN`
- `[PMIRQSB] DSP OK`
- `[PMIRQSB] PMVEC OK`
- `[PMIRQSB] TIMER HIT`
- `[PMIRQSB] IRQ HIT`
- `[PMIRQSB] PASS`

The probe waits on the protected-mode timer instead of a blind CPU spin loop, so
the SB DMA transfer has enough wall-clock time to complete before judging IRQ7.
The default run validates IRQ7 with single-cycle DMA using ACK-before-EOI,
single-cycle DMA using EOI-before-ACK, and auto-init DMA. To validate IRQ5 under
a matching QEMU SB16 device, run `DOS4GW.EXE PMIRQSB.LE IRQ5`; it runs the same
three variants against IRQ5.

`DOS4GW.EXE PMIRQSB.LE TASK` runs a narrower DMX-like service probe. It installs
the protected-mode timer and SB IRQ vectors, then dispatches three short
single-cycle DMA playbacks from timer-paced service work. Expected extra markers:

- `[PMIRQSB] TASK INSTALL`
- `[PMIRQSB] TASK HIT`
- `[PMIRQSB] FXDMA START`
- `[PMIRQSB] FXDMA IRQ`
- `[PMIRQSB] TASK PASS`
