# f1-88: SuperSavage descriptor/depth ordering

Amendment 1, 2026-10-21. Host-tested candidate; T23 acceptance remains open.
This replaces the completion-only interpretation of the initial f1-88 work.
Both b51f034f boot blocks in `build/t23-b51f034f-accel-records.txt` were read.
The meaningful first attempt writes `00cd00e8`, reads `e8e8e8e8`, restores
its pixel and quarantines. The other boot retains `previous_attempt` and
performs no new engine experiment. `phase=marker_write error=0` describes
the durable attempt file, not an accelerator command.

## Amendment 1: evidence and candidate

The fill executed with the low colour byte replicated across four lanes.
An unchanged pixel or merely stale completion cannot explain this record.
The final GBD mirror and CR50 both read correctly; that does not establish
when the engine acquired the colour depth. The old order was:

```
MM816C = 10000001                 PM preamble, colour-depth field zero
CR50   = f1                       select 32-bit engine pixels and GBD
MM8168 = 00000000
MM816C = 10200401
BCI E0/E1 pairs, twice
solid fill 4bcc8000, colour, XY, WH
```

`start_config()` now selects and verifies CR50 **before** the PM preamble.
The full descriptor, configuration 1, both queued pairs, exact pixel gate
and quarantine remain. This follows the upstream prerequisite: mode depth
is established before either engine initializer runs. No new register
value, reset, mode switch, primary-stream or panel operation is introduced.

**Inference, not an established erratum:** a depth-dependent preamble could
capture firmware's 8-bit colour lanes even when later GBD mirrors say 32.
Neither the available manual nor the upstream comments documents this
specific latch. The supplied trial lacks the incoming firmware CR50 value.
The new `savage depth firmware_cr50=... order=cr50_before_gbd` record makes
that missing observation available. If firmware already has depth `11`,
this change does not explain its old failure. Hardware must confirm or
refute the candidate; a green injected model cannot establish the physical
root cause.

## Primary-source comparison

Sources checked online and against the supplied local source ranges:

- [X.Org acceleration source](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_accel.c): `SavageInitialize2DEngine`, `SavageSetGBD`, `SavageSetGBD_Twister`, `SavageSetGBD_PM`.
- [X.Org driver](https://raw.githubusercontent.com/NetBSD/xsrc/trunk/external/mit/xf86-video-savage/dist/src/savage_driver.c): `SavageModeInit`/`SavageWriteMode`, `SavageInitStatus`, `WaitQueue4`, `WaitIdleEmpty4`, `ShadowWait`, `SavageInitShadowStatus`, COB selection and reset.
- [Linux savagefb driver](https://raw.githubusercontent.com/torvalds/linux/master/drivers/video/fbdev/savage/savagefb_driver.c): `savage_init_hw`, `savagefb_set_par`/`savagefb_set_par_int`, `SavageSetup2DEngine`; local `savagefb.h`/`savagefb_accel.c` confirm packet definitions. [Upstream acceleration source](https://kernel.googlesource.com/pub/scm/linux/kernel/git/mripard/linux/+/5a838c3b60e3a36ade764cf7751b8f17d7c9c2da/drivers/video/savage/savagefb_accel.c) was available as indexed excerpts; direct fetch failed.
- [S3 RE046-B register manual](https://old.vgamuseum.info/images/stories/doc/s3/savage4_registers.pdf), printed pp. 82, 89, 129–134, 142, 148–150, 224–227, 231. It describes Savage4; family-specific upstream branches take precedence for SuperSavage.
- [Linux DRM BCI implementation](https://kernel.googlesource.com/pub/scm/linux/kernel/git/gregkh/driver-core/+/7cfde0af731c14664e3882c7ba77ace1059f2c5e/drivers/gpu/drm/savage/savage_bci.c) and [register header](https://sources.debian.org/src/linux/6.1.4-1/drivers/gpu/drm/savage/savage_drv.h) support the existing event fence/32-entry budget through online indexed excerpts; direct fetch failed. This is not new proof of a SuperSavage erratum.

| Our step | Upstream step | Difference | Consequence / decision |
| --- | --- | --- | --- |
| CR66 reset `8b -> 89`, two 10 ms minimum delays | X.Org pre-init/reset | Same polarity; delay transport yields | Reset is deasserted and engine enabled. 177346 us alone does not show a disabled engine. |
| Preserve live firmware scanout | Linux `savage_init_hw` also resets memory/master interfaces; `set_par` owns the whole mode | Broader Linux ownership | Do not import CR3F/master reset into this attachment. |
| SR26=40, CR40=01, CR31=0c | Mobile access, packed engine register setup | Matches | Keep authoritative VGA ports; the MMIO VGA alias mismatch is diagnostic. |
| CR50 formerly after PM preamble | X.Org mode writes depth first; Linux commits CR50 before 2D setup | Missing prerequisite at the first descriptor write | Move CR50 verification before preamble. |
| SuperSavage uses PM, not Twister | `SavageSetGBD` dispatch | Correct family; Twister handles Savage4/ProSavage variants | Keep PM bit-3 policy and linear format. |
| `10200401`, MM8168=0 | PM full GBD: stride 1024 pixels, bpp 32, BW_DISABLE, BD64 | Fields match; fbdev generically also sets bit 3 | Keep measured configuration 1; no qword stride, byte stride or tiling change. |
| Two BCI E0/E1 pairs at MM10000 after direct GBD | fbdev repeats the descriptor; X.Org warns early GBD writes can fail | Already present in failed trial; MMIO reads are not private-latch reads | Retain queued setup; do not claim adding it fixes this trial. |
| Only GBD explicitly initialized | PM also writes PBD/SBD | Additional engine state in X.Org; fbdev's GBD fill does not require it | Do not invent a PBD dependency for DEST_GBD. Remaining discriminator if ordering fails. |
| Halfword MM8134=27, MM8136=07 | Both upstream initializers use the same writes | T23 foreground readback is zero | Not a documented depth register; retain diagnostic, no equality gate. |
| Four-word `4bcc8000` fill with full colour | Linux `savagefb_fillrect` | Same solid-source GBD packet | No pattern-register colour rewrite. |
| MM48C18 low nibble 0 then 8, MM48C0C=0 | fbdev no-COB; X.Org DisableCOB without ShadowStatus | Matches supported path | BCI enabled; no overflow/shadow/DMA allocation required. |
| MM48C10=00700040 | fbdev same; X.Org no-COB thresholds differ | Shadow updates disabled | Keep fbdev values, not a depth fix. |
| MM48C60 and `e1ffff/e00000` | SuperSavage WaitIdleEmpty4; queue uses mask 1fffff | Correct chip choice | Do not move polling to MM8504/8508. |
| Maximum ten-word burst including fence, no COB | X.Org MAXFIFO=7f00; DisableCOB threshold 32, DRM on-chip FIFO 32 | Software COB limit differs from physical no-COB budget | Stay within 32 DWORDs from proven idle. |
| WAIT_2D then event, exact word-1 tag plus idle | X.Org ShadowWait; DRM direct-event wait | Existing f1-88 ordering guard | Retain as bounded guard, not the explanation for replicated colour. |
| Native provider saves/restores CR40/50/66 | f3-32 transaction | No engine submission there; admission excludes asserted reset | No `supersavage_*.c` changes. |

For `10200401`: bit 28 disables block writes, bits 25:24 select linear,
bits 23:16 encode 32 bpp, stride bits represent 1024 pixels; bit 0 enables
the offset word and bit 3 is clear under PM policy. `CR50=f1` has depth
bits `11` and GBD selection `c1`; `CR31=0c` supplies enhanced access with
address-offset bit 0 clear. Neither final value is an 8-bpp encoding.
MM8168=0 is a byte offset, not a second depth field. The PM branch's tile
encoding differs from Savage4; no tiled surface is introduced here.

MM8136 is even-addressed and halfword aligned: foreground mix, paired with
background mix at MM8134 (BCI DA upper/lower halves). It is not the
destination-format word. The manual describes read/write access, so the
observed `0007 -> 0000` remains an unexplained chip/readback discrepancy,
not proof of a write-only register. The post-fill MM8134=`cc27` is retained
as evidence; its changed ROP bits do not establish colour depth.

MM8504 is subsystem interrupt/reset control/status and MM8508 FIFO status.
BCI enable lives at MM48C18; the references do not enable BCI at 8504/8508.
`caps=0` is the consequence of the failed self-test/quarantine, not a chip
family identifier. `EngineConfig=1` is the selected configuration index.
PCI 5333:8c2e selects SuperSavage; its status is the alternate word.

## Status and fence

For the SuperSavage layout used by upstream:

```
07e00000 & 00e1ffff = 00e00000     empty/idle predicate satisfied
(07e00000 >> 21) & 7 = 7          three idle bits set
07e00000 & 001fffff = 0           zero FIFO entries used
available with COB disabled = 32  inferred capacity, not a raw free field
```

Bits 26:24 are outside those driver predicates. The event tag is not in
word 0: it is the low 16 bits of word 1 (MM48C04, alternate MM48C64).
There is no tag value to decode from `07e00000`. The pre-existing manual
layout fallback is retained but is not newly qualified on this chip.

The retained fence appends WAIT_2D=`c0020000`, then `98000000 | fresh_tag`;
it skips BIOS tag zero and requires an exact acknowledgement plus idle.
ShadowStatus is an alternative status transport, not a mandatory enable.
There is no new DMA buffer, reset or retry. Dead/stale/wrong events retain
the bounded quarantine/unsafe-writer guard. Tests cover those cases and
frame equivalence. This is harmless to pixel/descriptor semantics in the
host model; SuperSavage event-register acceptance and the runtime polling
cost remain hardware checks. It cannot repair an already executed 8-bpp
fill, as the new negative test demonstrates.

## Host evidence boundary

The model has two separate tests: delayed command acceptance for the
retained fence, and an explicitly labelled preamble-depth failure injection.
The latter captures colour lanes from CR50 at the first PM preamble while
allowing final MMIO and queued descriptor mirrors to read correctly.
Replaying the old descriptor section with the fence still enabled produces
exactly `00cd00e8 -> e8e8e8e8`, idle `07e00000`, restored pixel and zero caps.
Corrected production activation passes at 16/32 bpp, incoming firmware
8/16/32-bit depths, and 1024x768 geometry. It checks descriptor-write order,
marker restoration, active caps, durable marker clearing and CR50 restore.
The documented registers/packets are source based; this capture dependency
is a controlled empirical hypothesis, **not** a documented silicon model.
Do not present these host results as proof of the T23 root cause.

## T23 experiment protocol (lead)

1. Keep original serial/disk logs and canonical image SHA-256; record cold
   or warm boot and whether a retained attempt marker was present. A
   `previous_attempt` boot cannot test this change.
2. Safe boot: put the standalone token `accel=0` in `BOOT.CFG` on the boot
   volume. Verify `reason=boot_disabled`, no worker/marker/register setup,
   zero caps and working software composition. For a new authorized trial,
   remove that token and clear `C:/system/settings/accel.cfg` using the
   established recovery procedure. Never clear quarantine within a boot.
3. Capture the complete ordered `savage init` records, including all banks.
   Require SR26=40; CR66 reset asserted/deasserted with bit 0 retained;
   CR40=01, CR31=0c; MM48C18 low nibble 0 then 8; MM48C0C=0. Require the
   verified CR50=f1 **before the first** MM816C=10000001 record, then
   MM8168=0, MM816C=10200401 and both queued descriptor readbacks. Read the
   new `savage depth firmware_cr50=... bpp=32 descriptor=10200401
   order=cr50_before_gbd` line. If incoming CR50 already has depth `11`,
   classify the old-depth-at-preamble explanation as unsupported for that boot.
4. Read every `bank=event` record and the `savage completion` summary:
   fresh nonzero requested tag, exact low-16 observed equality, pending=0,
   idle bits=7 and FIFO used=0. Tags reside at 48c64, not in status 48c60.
   Missing/dead/wrong events must quarantine within the existing bounds.
5. Require exact wrote/read colour equality, `bci_test result=ok error=0`,
   `read_valid=1 restored=1`, nonzero caps, `init_result=0`, successful
   `marker_clear` and `savage ready`. Collect the entire failure snapshot
   otherwise; distinguish `idle_snapshot` from `cached_init`. Record
   firmware CR50, both descriptor words, CR31/40/50, MM8134/8136 and event
   observations together, not only the successful mirror comparison.
6. Completed fence plus repeated-byte colour refutes ordering as sufficient:
   keep quarantine; investigate the remaining PM descriptor state (PBD/SBD)
   and actual colour-lane behavior before another change. Unchanged pixel
   plus completed fence instead needs command/address investigation. A
   missing event introduces a separate fence-acceptance issue. A passing
   marker supports this candidate on that image; it does not isolate a
   silicon erratum from one trial.
7. After a passing gate, check hardware cursor, fills/patterns, overlapping
   copies, full-screen enter/restore, window moves during music, operation
   rates and display presentation cost. Recheck safe boot. Record timings;
   host tests measure neither T23 audio improvement nor fence overhead.

The lead owns review, image build, hardware trial and diary. This work runs
no QEMU, systemd scope or image build and changes no mode-set recipe.
