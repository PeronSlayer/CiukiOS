# f1-38 — Savage 2D on the T23: enable MMIO register access through the VGA I/O ports before touching the MMIO window

- **Model/effort:** `gpt-6.1-sol`, `xhigh`, `--search`. **Worktree:** `../CiukiOS-wt-f1-38` from `main` (f9f112a0). Host-testable; the lead runs the T23 capture.

## Evidence (T23, image 3299a24f, boot `L`, 2026-10-13)

```
[accel] savage attempt persisted
[accel] savage probe pci=5333:8c2e cmd=0007 bar0=c0100000 bar1=e8000008 lfb=e8000000 vram=15532032
[accel] savage mapped mmio=f8602000 cursor=f8683000
[accel] savage step op=crw reg=00038
[accel] savage step op=crw reg=00039
[accel] savage step op=crr reg=00038
[accel] savage pci=5333:8c2e mmio=c0100000 lfb=e8000000 mode=280 bpp=32 pitch=4096 caps=0 init_result=-5
[accel] savage quarantined reason=crtc_locked
```

`savage2d_start` (`src/kernel/drivers/savage2d.c`, around line 246) writes CR38=48h/CR39=A5h through the MMIO VGA alias (`registers[0x83d4/0x83d5]`, BAR0+0) and reads CR38 back; the read-back is not 48h. Every VGA register access of the driver goes through that alias; no legacy I/O port is used. The boot was otherwise healthy (desktop, apps, ACPI off).

## Diagnosis to verify (research first)

The X.org `savage` driver (`src/savage_driver.c`: `SavageEnableMMIO`, `SavageUnLockExtRegs`, `SavageDisableMMIO`) enables the memory-mapped register path through the **legacy VGA I/O ports** before any MMIO access: `0x3c3 |= 1` (VGA enable), `0x3c2 = inb(0x3cc) | 1` (colour I/O decode), and on Savage4-class chips and later (SuperSavage included) **CR40 bit 0 = 1** written through `0x3d4/0x3d5` after unlocking CR38=48h and **CR39=A0h** (bits 7:5 = 101; A5h is the Trio/ViRGE convention — check the Savage4/SuperSavage register reference you find, keep whichever the reference says and cite it). Until CR40.0 is set, the MMIO VGA alias at BAR0+0x8000.. may not respond, which explains a read-back failure that is not a lock at all. The VBE firmware of the T23 sets its mode with I/O ports and may leave CR40.0 clear.

## Task

1. Add I/O-port access to the `savage_io` contract (`read_port`/`write_port`, 0x3b0–0x3df only) with the host fake in `tests/host/` mirroring the real port/MMIO relation (MMIO alias dead until CR40.0 = 1).
2. New start sequence, every step traced as a `[accel] savage step op=… reg=… value=…` record (add `value=` to the existing `crr`/`r8` traces so the next capture shows the read-back): VGA enable and colour decode through the ports; unlock CR38/CR39 through the ports; read CR38/CR39 back through the ports (that is the real lock test; fail `crtc_locked` only there); read CR40 through the ports, set bit 0 keeping the other bits, read back (`mmio_enable_failed`); then probe the MMIO alias: CR38 read through MMIO must equal the port value (`mmio_alias_dead` otherwise, engine untouched, quarantine as today). Only after that the existing sequence continues (pipe select, MISC, CR66 reset pulse, the bounded status polling of f1-27, BCI enable). On stop/quarantine restore CR40.0 to its original value through the ports.
3. The I/O ports must be claimed in the registry like the other drivers' resources; check who already claims 0x3c0–0x3df (`registry`, `biosvm`, `fbdev_change_*` mode changes). The accelerator starts in `accel_work` and is stopped before every mode change (`changing`); keep that ordering and document the port ownership in `docs/design/` where the Savage driver is described.
4. The chip-bug precaution of f1-27 (no read of MM48C60 before the CR66 reset) stays. The attempt marker stays.
5. Host tests: `savage2d_test` with the fake chip: lock test through ports, MMIO alias dead case, CR40 bit preservation and restore, trace records with values.

## Acceptance

Host suites green (`make CAP= ...` targets the kernel host tests use; one suite at a time), i686 kernel compiles; the report lists the datasheet/driver references with URLs and the final register sequence. The lead qualifies on the T23 (`[accel] savage ready` expected, then the desktop's `ops=` counters growing).

## Constraints

No QEMU, no commits, no diary, `nice -n 19`, `rg` not semble, one host suite at a time. Report `build/codex/f1-38-report.md`.
