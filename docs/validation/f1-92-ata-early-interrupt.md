# ATA early interrupts, in-flight cancellation and the DMA PIO repeat (f1-92)

Status, 2026-10-22: implemented and verified with host tests only. No QEMU run,
no image build and no hardware run belong to this change. The T23 evidence
for the new records is still to be collected.

## Evidence

T23 serial capture of the 1.0.0 alpha image `8f59e221` (three boots), and the
b51f034f trial, show the same pair every time, followed by a read-only volume:

```
[ata0] dma quarantine unit=0 mode=udma2 generation=5552 command=25 issued=1 bm=24 status=50 error=00 elapsed_ms=1 fallback=pio
[ata0] dma fallback unit=0 generation=5552 command=25 bm=24 status=50 mode=pio
[ata0] fault unit=0 command=29 issued=1 status=58 error=00 elapsed_ms=1
[storage] readonly reason=device_quarantined file=UNIFIED.IMG cluster=121589
[storage] failure stage=writeback drive=C disk=0 result=-5 scope=volume fallback=readonly
```

The other two quarantine records had `elapsed_ms=2` and `elapsed_ms=5`, both
with `bm=24 status=50 fallback=pio`. All three PIO faults read `status=58
elapsed_ms=1`. A DOS VM (`UNIFIED.IMG`) and desktop `stat` traffic ran at the
same time.

## Reading

- `fallback=pio` rules out "the DMA engine was never started" for these
  records. `dma_failure` computes `unstarted = armed && issued && !started &&
  !ERR` and allows `fallback=pio` only when `unstarted` is false. With
  `issued=1` and no ERR, the master had therefore been started. `bm=24` is
  Interrupt set and Active clear: a finished transfer, as Table 5-54 of the
  ICH3-M datasheet describes. `status=50` is a finished command. Nothing on the
  device side failed.
- `status=58` (BSY=0, DRQ=1) after READ MULTIPLE EXT is ATA-6 9.5 HPIOI1 →
  HPIOI2: the data phase. A device fault shows ERR or DF, or runs into the
  30 s deadline. It cannot end after 1 ms with DRQ set.
- **Root cause.** The f3-49b request seam (commit `5785261e`) added
  `blk_request_interrupted()` to `expired()` and `irq_ready()` in `ata.c`, and
  it capped the IRQ wait with `blk_request_remaining_ms()`. If the caller's
  request was cancelled or ran out of budget while its command was issued,
  the next driver boundary called `failure()`. That function treats every
  issued failure as a device fault. On DMA, `dma_failure` quarantines DMA for
  the unit (the first record). On PIO, it quarantines the whole channel (the
  `fault` record), and FAT then revokes writes (`device_quarantined`).

  Several callers cancel requests during normal use:
  - `prefetch_cancelled` (`cache.c`): `cache_stream_begin` decays every other
    stream with outstanding prefetch as soon as another reader starts.
  - The DOS file request: 2 s budget, VM stop.
  - Native `read()` on a pending signal.

  The host replay below reproduces the trial pair exactly from a competing
  reader decaying a prefetch.
- An early edge before Start is still possible on real hardware (see the
  sources below). It was a second, latent path: it used to quarantine the
  whole channel (`fallback=blocked`), not DMA alone. A stray edge after Start
  used to stop the master in the IRQ handler, which aborts a live transfer.
  Both are fixed here, but neither one produced the trial records.

## Sources

- T13 ATA/ATAPI-6, d1410r3a ([PDF](https://www.read.seas.harvard.edu/~kohler/class/04f-aos/ref/hardware/ATA-d1410r3a.pdf)):
  - 3.1.29 and 5.2.9: interrupt pending, INTRQ with nIEN and on selection.
  - 9.5 PIO data-in: INTRQ only with BSY=0. DRQ is the data phase, and BSY=0
    with DRQ=0 is the end.
  - 9.7 DMA: "A single interrupt is issued at the completion of the
    successful transfer of all data … or when the transfer is aborted due to
    an error". DDMA0 negates INTRQ. The host sets up the DMA engine before
    HDMA1.
  - 8.30/8.31 READ MULTIPLE (EXT): one interrupt per DRQ block, and a final
    partial block of `count mod block` sectors.
- Intel, *Programming Interface for Bus Master IDE Controller* 1.0
  ([PDF](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/IDE-BusMaster.pdf)),
  the document SFF-8038i is based on:
  - 2.1: clearing Start while Active is set and Interrupt is clear aborts the
    transfer.
  - 2.2: Interrupt is set by the rising edge of INTRQ.
  - 3.1 "Standard Programming Sequence": the sequence is command, then Start.
  - "Data Synchronization": "If the Interrupt bit is not set then the IDE
    device did not generate the interrupt".
  - "Status Bit Interpretation": the Interrupt/Active table.

  The PDF numbers all three of these sections 3.1.
- Intel 82801CAM ICH3-M datasheet 290716-001 ([PDF](https://datasheet.lcsc.com/datasheet/pdf/7d520883a0f21f1845e4892c5243848a.pdf?productCode=C3247608)):
  - 5.15.2.3: legacy IRQ14 comes from the pin.
  - 5.15.2.4 and Table 5-54.
  - 10.2.1 START: hardware never clears it.
  - 10.2.2 BMIS Interrupt: set on the rising edge regardless of PIC masking,
    and it stays clear until another edge.
- Linux `drivers/ata/libata-sff.c` v6.6, `ata_bmdma_port_intr` ("if it's not
  our irq…" → `ata_sff_idle_irq`, without stopping the engine), and
  `__ata_sff_port_intr` (BSY means an idle IRQ; with the engine already
  stopped it is a protocol error). `ata_bmdma_setup` and `ata_bmdma_start`
  issue the command before Start.

## Decisions (directive items)

1. **Early edge before Start** (`dma_early`). Conditions: the drive's
   snapshot and its current status show no ERR/DF, the drive is present (not
   00h or FFh), and the master is neither Active nor in Error. Any such status
   qualifies: stale DRDY 50h, BSY, or DRQ/DMARQ. The handler has already
   acknowledged the drive and cleared the BM Interrupt bit (W1C). The driver
   records `early_irq phase=before_start`, consumes the sequence, clears the
   snapshot and starts the master. The command keeps its 30 s deadline.

   This path never quarantines. ERR is the drive's abort (DMA quarantine and
   PIO repeat). DF, a missing drive, or an active or error master is still
   containment.

   **After Start** (`ata_dma_irq`, `dma_wait`), the following edges are not
   completions:
   - an edge with neither BM Interrupt nor Error;
   - an edge with Interrupt and Active while the drive's Alternate Status
     still shows BSY or DRQ.

   On such an edge the handler leaves the master running. The waiter records
   `phase=dma_active` and keeps waiting while the drive is BSY or DRQ. A drive
   that has ended without a latched Interrupt gets a 10 ms grace, then fails
   with `cause=bm_no_interrupt`. The first completion snapshot is final
   (sticky) until disarm.
2. **PIO multiple.** `status=58` is the data phase. The PIO path already
   waits for DRQ by polling, so an extra edge cannot shift data. The audit
   found no block-count fault: `sectors = min(multiple, n - i)`, and the final
   partial block follows 8.31. An edge whose snapshot shows BSY is now counted
   as `early_irq phase=pio`. Tests cover 1/15/16/17/33/128 sectors with
   16-sector blocks on 28- and 48-bit commands.
3. **Fallback policy** (`ata_transfer`). When `dma_failure` proves the drive
   and the master idle (`fallback=pio`), the same chunk is repeated once
   through PIO, with the same buffer and LBAs, before any error is returned.
   This changes the earlier "never replayed" rule for this one case. Sector
   writes are idempotent, and the device is proven idle before the new
   command. Only a failed PIO repeat leads to the channel quarantine, and so
   to `readonly reason=device_quarantined`. A stuck master, DF, BSY or DRQ
   still quarantines the channel without a repeat.

   **Root-cause fix.** `expired()`, `irq_ready()` and the IRQ wait use only
   the device deadline (plus the explicit hardware-check cut-off). Request
   interruption is checked in `ata_transfer` before each command. It returns
   EAGAIN with no port access. An issued command always ends on the device's
   terms. A cancelled read gets the cancellation code from `blkdev_request`,
   as before.
4. Host tests: see below.
5. Records and expected counts: see below.

## Records

All records are written in thread context. Counters are per unit and
saturate at 2^32-1. Bounded records are printed for counts 1–4, then only at
powers of two (at most 32 more over 2^32 events).

- `[ataN] early_irq unit=U phase=before_start|dma_active|pio command=CC generation=G status=SS bm=BB count=N`.
  `status` is the drive status the IRQ handler read, `bm` the bus-master
  snapshot (`bm=00` for PIO).
- `[ataN] interrupted_inflight unit=U command=CC result=R elapsed_ms=MS count=N`:
  an issued command whose request was cancelled or out of budget when it
  ended. `result` is the command's own result (0 = completed).
- `[ataN] dma retry unit=U generation=G command=CC mode=pio result=R count=N`:
  `generation` is the failed DMA command, `command` the PIO repeat. DMA is
  never re-enabled, so this appears at most once per unit and boot.
- `[ataN] dma quarantine … fallback=pio|blocked cause=C` and `[ataN] fault … cause=C`
  gain a trailing `cause`, with these values:
  - `timeout`
  - `check_cancel` (hardware check cut-off)
  - `bm_no_interrupt`
  - `dma_prepare`
  - `no_device`
  - `device_error` (ERR/DF)
  - `bm_error`
  - `dma_not_started`
  - `bm_active`
  - `quiesce`
  - `protocol`

## Expected on the T23

- `early_irq`: **0** in a normal session. On the ICH3-M, IRQ14 is wired from
  the pin (5.15.2.3). The driver reads Status before the command, and the
  window between command and Start is one 1 µs pace. None of the trial
  records was an early edge. A count of a few per session is harmless and is
  a new finding. If counts grow steadily, the stray-edge source is worth
  investigating (`phase` and `status` name it).
- `interrupted_inflight`: **> 0** whenever a DOS game and the desktop read
  together, or a prefetch is overtaken. In the trial, each of these was a
  quarantine. Expect several per session around game launches. Each one
  carries `result=0`.
- No `dma quarantine`, no `fault`, no `readonly reason=device_quarantined`,
  and no `dma retry` during the same workload.

## Verification

Run from the worktree on the host only. Exact commands and results are in
`build/codex/f1-92-report.md`.

`tests/host/ata_test.c` runs the production `ata.c`/`ata_dma.c` with cache,
FAT and VFS. Its new and changed tests:

- `ata_f192_test.inc`:
  - edges before Start (late PIC edge and early INTRQ; DRDY 50h, 40h, BSY D0h,
    DRQ 58h; LBA28/48; reads and writes);
  - ERR and DF edges;
  - a stray edge on a live transfer (DRQ and BSY; with and without BM
    Interrupt; read and write);
  - the missing-BM-Interrupt grace;
  - PIO and PIO-multiple edges while BSY, with short final blocks;
  - T23 driver replay: cancellation during READ DMA EXT 25h and during READ
    MULTIPLE EXT 29h at DRQ, cancellation between commands, then write and
    flush;
  - T23 storage replay: a FAT volume on the fake unit through cache, FAT and
    VFS. Two prefetches are cancelled in flight by a competing reader
    (`cache_stream_begin`), on DMA and on PIO multiple. Then DOS-save and
    image write-back and commit, with no read-only transition, and the data
    read back.
- `ata_dma_test.inc`:
  - the DMA fault matrix, where bus-master-only faults now recover through
    PIO;
  - a DMA error on one unit with a DMA peer;
  - ERR on DMA and on the PIO repeat, which still quarantines the channel;
  - cache write-back after an idle DMA fault, which now succeeds.

Negative control: the same tests built against the previous `ata.c`/`ata_dma.c`
fail every new scenario, except the plain PIO early edge, where only the
missing `early_irq` count fails because the old PIO path was already robust.
The storage replay prints the trial signature:
`dma quarantine … command=25 … fallback=pio`, then
`[storage] readonly reason=device_quarantined file=DOSVM.SAV`.

`tests/host/fs/test_t23_holds.c` uses a memcpy transport without the ATA
driver, so it cannot show a driver quarantine. The trial sequence therefore
lives in the ATA harness, which runs the production cache, FAT and VFS on the
production driver.

## Not covered

- `usb_storage.c` has the same request seam (`live()`, `bulk_end()`). A
  cancelled request can fail an issued BOT transfer there. Not changed here.
- The hardware check's I/O cut-off still ends issued commands with a
  quarantine. This is unchanged and is reported as `cause=check_cancel`.
- READ MULTIPLE with ERR posted at the start of a DRQ block (8.31: DRQ stays
  set and the block must still be read) fails before draining that block. The
  whole-channel quarantine on a PIO device error is the existing policy.
- QEMU and T23 evidence: lead-owned.
