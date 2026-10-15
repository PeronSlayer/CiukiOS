# F0-28 sweep recovery and serial submission

Research checked online on 2026-10-15 before implementation:

- [S3 RE046-B](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf),
  printed pp. 89 and 148–149, defines CR66.1 engine reset and the global
  descriptor controls. Bit 3 enables BCI on Savage4; tile selector 1 is
  reserved there. Descriptor stride is in pixels, not qwords.
- [X.Org SavageSetGBD_PM](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c)
  uses destination tiling on SuperSavage with additional surface/stream
  setup. Its DRI descriptors disable block writes for this chip family.
  [Linux SavageSetup2DEngine](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c)
  also disables block writes. The H sweep has no qualified block-write
  memory setup or tiled surface setup. Keep the matrix IDs for evidence,
  but skip block-write-enabled and non-linear candidates before commands;
  also exclude BCI-disabled descriptors on Savage4. Do not infer that a
  software deadline can interrupt a PCI transaction which never returns.
  Exclude padded-pitch hypotheses that overlap stride with control bits or
  could exceed the scratch reservation at either engine depth.
- [X.Org SavageGEReset](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c)
  recovers completion timeouts with CR66.1, 10 ms asserted and 10 ms
  deasserted, followed by status proof and engine reinitialization. Limit
  H recovery to one such pulse per timeout, preserving CR66.0. Only resume
  after idle proof and safe setup; retain leases/quarantine on failed reset.
  Production quarantine policy remains unchanged.
- [TI TL16C550D](https://www.ti.com/lit/ds/symlink/tl16c550d.pdf), LSR
  description (printed p. 37), specifies THRE as FIFO empty and TEMT as
  FIFO plus shift register empty. The standard eight-register 16550 has no
  numeric TX FIFO occupancy register. Use TEMT as a conservative zero-level
  observation on physical FIFOs, then consume at most the verified FIFO
  capacity in software credits. A THRE-only race must not mint more credits.
  Retain the TCG/test chardev policy, single-byte IRQ locks, record boundary,
  physical admission quota and pacing. Refill ownership and the byte cursor
  must survive readiness-read preemption and panic takeover.

Matrix ID 3 (zero-based, as logged) at 32 bpp decodes to bit3=0,
BW_DISABLE=0, stride=pixels, CR50 depth=3 (bits 5:4=11), tile=0,
descriptor `00200401` for pitch 4096. At 16 bpp/pitch 2048 it is
`00100401`, CR50 depth=1. These are matrix derivations, not recovered
hardware register samples. The supplied capture contains truncated and
repeated chunks; it does not prove either a particular UART silicon race
or the exact instruction at which the machine stopped.

Candidate begin records must reach both BOOT.LOG's committed watermark and
the raw black box before register submission. Result records are checkpointed
before the next candidate, including skips/timeouts. Failure to checkpoint
stops the sweep with the results already collected. A deadline may truncate
the matrix; it must not erase earlier outcomes or claim a complete sweep.
The serial resend counter audits logical queue-byte submissions locally;
neither it nor sent tickets can establish remote reception or detect a
receiver duplicating bytes. Host results and remaining hardware/QEMU
acceptance are recorded in `build/codex/f0-28-report.md`.
