# f1-92 — ATA: early interrupt before DMA start quarantines a healthy drive; PIO READ MULTIPLE fault

- **Phase/model:** F1, Claude agent (Opus), primary-source research (ATA/ATAPI-6 §9.x DMA and PIO data-in protocols, Intel ICH3-M IDE bus-master spec, SFF-8038i).
- **Worktree:** `wt/f1-92` from `main` (image `8f59e221…`, commit `be138eb1`).
- **Evidence:** T23 trial of the 1.0.0 alpha image (serial capture, 2026-10-21 23:40–23:48):
  - `[ata0] dma quarantine unit=0 mode=udma2 generation=2243 command=25 issued=1 bm=24 status=50 error=00 elapsed_ms=2 fallback=pio` twice (boot and at the first DOS game launch); the same signature appeared once in the b51f034f trial.
  - then `[ata0] fault unit=0 command=29 issued=1 status=58 error=00 elapsed_ms=1` (PIO READ MULTIPLE EXT, DRQ set), followed by `[storage] readonly reason=device_quarantined file=UNIFIED.IMG`, `failure stage=writeback result=-5 fallback=readonly`: the whole volume went read-only, every DOS game then failed with an I/O error and the USB copy ran on PIO.
- **Reading of the code (`src/kernel/drivers/ata.c:1066-1082`, `dma_failure`):** after `issue()`, the DMA engine is started only if `c->irq_sequence == sequence`; an interrupt edge arriving between issue and that check (a late edge of the previous command, or the device's early INTRQ) leaves `dma.started=false`, the wait then returns the stale status (DRDY, BM IRQ bit set) and the check at line 1075 declares `dma_failure` with a completed-looking status. `bm=24` (IRQ set, ACTIVE clear) and `status=50` are exactly that picture. The quarantine is therefore a driver misclassification, not a device fault.

## Required

1. Distinguish an early/stale interrupt from a completion: if the DMA engine was never started, the device shows DRDY without DRQ/BSY/ERR and the bus master shows no ACTIVE/ERROR, treat the edge as spurious, clear it, start the DMA engine and continue the same command with the full deadline; count it in a new `[ata%u] early_irq` record (per unit, bounded). Never quarantine on that path.
2. PIO READ/WRITE MULTIPLE: a DRQ status after the command (`status=58`) is the data phase, not a fault; audit the PIO multiple path (`ata.c:1001-1110`) for the same early-edge confusion and for the block count when `multiple_sectors` is set, and add the short final block case.
3. Fallback policy: when a DMA command is quarantined, the request must be retried once on PIO *before* the volume is marked read-only; `readonly reason=device_quarantined` only after the PIO retry also fails.
4. Host tests in the ATA harness (`tests/host` ATA model): the early edge between issue and start (DMA and PIO multiple), the late edge of the previous command, the genuine DMA failure (ERROR bit) still quarantines; the T23 replay (`test_t23_holds`/storage model) gains the sequence of the trial and must show no read-only transition.
5. Report the counts of early edges to expect on the T23 (`early_irq`), and the record format in `docs/validation/f1-92-ata-early-interrupt.md`.

Rules: no QEMU, no image build, no git mutations, no diary edits, nothing deleted outside the worktree, `date` before timestamps, English. Report `build/codex/f1-92-report.md`.

## Amendment 1 (lead, 2026-10-22 03:04, decision on the delivery)

The delivered root cause replaces the lead's reading: since f3-49b
(`5785261e`) a cancelled or out-of-budget request ended an issued command and
`failure()` treated it as a device fault (DMA quarantine, then channel
quarantine on PIO, then the volume read-only). Accepted as the cause of the
trial's I/O errors. The USB storage cancellation seam, the hardware-check
cut-off (`cause=check_cancel`) and the one-command DOS budget overrun are
follow-ups after the 1.0.0 alpha.
