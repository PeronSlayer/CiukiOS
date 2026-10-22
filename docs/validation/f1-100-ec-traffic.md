# f1-100 — ThinkPad EC traffic and the 2.1 ms SMIs

Written 2026-10-22 (implementer: Claude agent, Opus) for directive
[f1-100](../directives/f1-100-ec-traffic-off-smi-blackouts.md), after
[f1-94](f1-94-irqoff-blackout.md). Host evidence only: no QEMU, no image, no
hardware run. A result on the T23 is evidence only for the image SHA-256 it
ran on.

## Evidence read (T23 trial of image 5a10eef4)

Serial capture `build/codex/t23-5a10eef4-serial.txt` (5,904 lines), default
boot, EC poll at 1 Hz (`[ec] thinkpad poll_ms=1000 source=default`):

- 86 two-second windows carry `smi`/`smi_coincide`. Together: `gaps=340`
  tick-gap blackouts, `ec_near=340` (every one), `ec_io=1530` EC port
  accesses. One RD_EC poll is 9 port accesses (18 per window at 1 Hz, also
  reproduced by the host model below), so the trial made 170 EC transactions
  and saw exactly **2.0 blackouts per transaction**. `rate` is 1.99/s in
  steady windows (0.99–3.98).
- 136 blackouts landed in a bounded scheduler phase (`tsc`): 2,086–2,111 µs.
  `ec_lead_us` (our last EC port access to the blackout start) is 14–15 µs in
  almost every window (14–26 overall). `hid_near` equals `gaps` at the
  1 kHz HID cadence (chance level); `scan_near` is 0–1.
- 83 `irqoff_long` records of 2,094–2,118 µs; `violations=373`.
- `smi_config chip=ICH3-M pmbase=1000 en=0000402b sts=00000900 pm1_cnt=0000
  gpe0_en=0000 gpe1_en=1000 gpi_rout=01000000 devtrap=0000 mon_smi=1000`;
  every `smi_regs` record has `rise=00000000`: no SMI_STS bit ever rose
  between two samples.
- Side observation: `smi_regs ... legkey=2500/2500/2500` later in the run.
  USB_LEGKEY bits 10 and 8 are TRAPBY64R/TRAPBY60R *status* bits that are set
  "even if the corresponding enable bit is not set" (11.1.16); with the
  enables 0 (low byte 00) they cannot raise an SMI. They record our own
  i8042 reads and need no action.

## Sources

- Intel 82801CAM ICH3-M datasheet 290716-001,
  <https://edbatalha.info/compaq-n610c/Intel%2082801%20ICH3-M%2029071601.pdf>:
  5.14.4 (GPI status is sticky and level-triggered), table 5-37 (causes of
  SMI#/SCI; note 1 "SCI_EN must be 0 to enable SMI"), 9.8.1.5 GPI_ROUT,
  9.8.1.6 TRP_FWD_EN (D31:F0 C0h), 9.8.1.7 MON[n]_TRP_RNG (C4h–CAh),
  9.8.1.8 MON_TRP_MSK (CCh), 9.8.3.12 GPE1_STS (PMBASE+2Ch, R/WC),
  9.8.3.13 GPE1_EN (PMBASE+2Eh), 9.8.3.14 SMI_EN (EOS), 9.8.3.15 SMI_STS
  (bit 10 GPE1_STS, bit 11 MCSMI_STS, bit 12 DEVMON_STS), 9.8.3.16 MON_SMI
  (PMBASE+40h), 9.8.3.17/18 DEVACT_STS/DEVTRAP_EN, 9.8.3.19/20
  BUS_ADDR_TRACK/BUS_CYC_TRACK (PMBASE+4Ch/4Eh), 11.1.16 USB_LEGKEY.
- Linux boot log of a ThinkPad T23 with the same IBM `TP-1A` ACPI tables
  (LKML, 2006), <https://lkml.iu.edu/0603.1/0057.html>: "ACPI: Embedded
  Controller [EC] (gpe 28) interrupt mode", GPE blocks "00 to 0F" and "10 to
  1F", "Found ECDT", "region 1000-107f claimed by ICH4 ACPI/GPIO/TCO". GPE 28
  (1Ch) is the second block's bit 12: GPI[12].
- Linux v6.12 `drivers/acpi/ec.c`,
  <https://github.com/torvalds/linux/blob/v6.12/drivers/acpi/ec.c>:
  `acpi_ec_transaction_unlocked` enables the EC GPE "for command processing
  (IBF=0/OBF=1)" and `advance_transaction` moves a command one byte per GPE:
  the EC raises its event when it has taken an input byte and when it has
  output ready.
- ACPI EC interface (RD_EC 80h, WR_EC 81h, IBF/OBF), as cited in
  [f4-18](../design/f4-18-audio-levels.md); the uefi.org pages refuse
  automated fetching, so the Linux driver is the cited implementation.
- [R1](../design/win98-class-resource-management.md) and its f1-94 amendment:
  firmware SMI enables stay with the firmware; reducing our own traffic is
  ordinary driver policy.

## Why SMI_STS shows nothing, and whether MON_SMI would attribute it

1. **No I/O trap is armed.** `mon_smi=1000`: bit 12 is DEV4_TRAP_STS, an
   R/WC *status* bit; the enables DEV[7:4]_TRAP_EN are bits 11:8 and all 0.
   `devtrap=0000`: no legacy device trap. SMI_EN bit 11 MCSMI_EN is 0, so
   MCSMI_STS (SMI_STS bit 11, set by any 62h/66h access, i.e. ours) cannot
   raise SMI#. DEVMON_STS (SMI_STS bit 12) is set only when a trap status
   *and* its enable are set, so it stays 0. The f1-100 directive's reading
   ("one I/O-trap monitor armed by the firmware") takes the status bit for an
   enable. Reading MON_SMI cannot attribute the blackouts: its only bit is a
   stale sticky status with its enable clear and it never changed. The new
   read-only `smi_monitors` record prints monitor 4's range, mask and
   forwarding so the next trial shows which ports it watched; nothing is
   written (R1).
2. **The source that does fire is GPI[12].** `gpe1_en=1000` enables
   GPI[12]; `gpi_rout=01000000` sets GPI[12] Route (bits 25:24) to 01, SMI#;
   `pm1_cnt=0000` has SCI_EN clear (legacy mode). On the T23 GPI[12] is the
   embedded controller's event line (GPE 28 in the Linux log). The EC raises
   it when it takes a byte from its input buffer and when its output buffer
   fills. An RD_EC (command, address, data) therefore raises it twice (after
   the command; after the address with the data, close enough to be served
   in one SMI) and a WR_EC up to three times. Measured: 2.0 blackouts per
   RD_EC and a 14–15 µs lead from our last port access, which is the EC's
   time to take a byte.
3. **Why the status is invisible.** SMI_STS bit 10 GPE1_STS is read-only,
   the OR of the GPE1_STS bits routed to SMI# and enabled. The firmware
   handler must clear GPE1_STS bit 12 (R/WC) before RSM: with EOS set the
   arbiter re-asserts SMI# while a routed, enabled status remains (9.8.3.14),
   so a handler that left it set would loop. Our samples (each PIT tick and
   after each long section) are taken outside SMM and only see the cleared
   state, as `rise=00000000` shows. Reading GPE1_STS outside SMM is cleared
   for the same reason. Attribution therefore stays time- and traffic-based
   (`ec_near`, `ec_lead_us`, and now `[ec] thinkpad transactions=`), plus the
   read-only BUS_ADDR_TRACK/BUS_CYC_TRACK pair, which latches the last I/O
   cycle when SMI# goes active and is printed as `last_io=` (for an
   asynchronous SMI it is the CPU's last I/O cycle, so 0062h/0066h is
   supporting, not conclusive, evidence).
4. **The 2.1 ms** is the T23 BIOS's SMM service of an EC event in legacy
   mode. No IBM documentation of that handler is available; the facts are the
   constant 2,086–2,118 µs and the one-to-one coincidence above.

Not done, by R1 or as lead decisions: clearing GPE1_EN bit 12 or rerouting
GPI[12] (firmware SMI policy; the same line carries the EC's own thermal,
battery and hot-key events, which the BIOS serves in legacy mode); the ACPI
handoff (ACPI_ENABLE F0h to SMI_CMD B2h) that would turn the events into
SCIs, which needs an OS EC/GPE driver and the DSDT `_Qxx` methods (no AML
interpreter).

## What changed

1. **No periodic EC I/O by default** (`src/kernel/drivers/thinkpad_ec.c`,
   `include/ciuki/thinkpad_ec.h`). `thinkpad_ec_poll_option` returns 0 unless
   the exact token `ecpoll=1` is present; `ecpoll=0` wins over `ecpoll=1`
   (like a positive safe-mode source); `[ec] thinkpad poll_ms=<n>
   source=default|ecpoll=0|ecpoll=1`. The EC worker always exists when the
   EC is qualified but sleeps on a wait queue: with the poll off only a
   request wakes it for I/O (`ecpoll=1` keeps the 1 Hz poll for diagnosis).
2. **Boot probe: one transaction.** One RD_EC when the register already
   holds the saved mute preference; only before a first write is a second,
   equal read required (the stable-read gate), then WR_EC and a proving read.
   The probe's volume record is logged once (it was logged twice).
3. **User changes keep their write.** A mute change is read, WR_EC, proving
   read (three transactions). A level-only change makes no EC access: EC
   register 30h holds the hardware level and the mute bit, and the driver
   only ever writes the mute bit. A set without a write needs no second read.
4. **Mirror updated by an explicit action.** New devctl
   `MIXER_REFRESH_MASTER` (4204h, control handle only,
   [execution ABI](../design/execution-abi.md) F1-100 amendment): the desktop
   sends it when the user opens the tray volume popup; the kernel wakes the
   EC worker for one RD_EC, at most once per second
   (`thinkpad_ec_volume_refresh`), and later reads show the result. The
   H page's 5 s keyboard/EC-key window requests one refresh per second.
   Without a poll, a user change or refresh is also the failure retry, at the
   existing cadence (1 s, 10 s after eight failures).
5. **Counted and bounded.** Every port access and every transaction (an
   RD_EC/WR_EC command byte) goes through the operation's budget. One
   operation issues at most `THINKPAD_EC_OP_TRANSACTIONS` = 4 transactions
   (probe with a write); a fifth is refused before any port access and
   counted. Port accesses per operation are bounded by
   `EC_OP_IO_BOUND` = 540 (500 sleeping polls plus at most ten accesses per
   transfer); the 500 ms deadline is unchanged. Per cpustat window with EC
   traffic: `[ec] thinkpad io=<n> transactions=<n> max_io=<n> refused=<n>
   requests=<n> poll_ms=<n> total_io=<n> total_transactions=<n>`.
6. **Read-only diagnostics** (`src/kernel/core/smi.c`): `[kernel]
   smi_monitors` once at boot, `last_io=<BUS_ADDR_TRACK>/<BUS_CYC_TRACK>` at
   the end of `smi_coincide`. `smi_coincide` keeps `ec_near` and `ec_io`.

## Records and expected T23 lines

Default boot (no token):

```
[ec] thinkpad volume=<n> mute=<n>                         (once)
[ec] thinkpad poll_ms=0 source=default
[kernel] smi_monitors trp_fwd=.. mon_rng=..../..../..../.... mon_msk=.... gpe1_sts=.... last_io=..../..
[ec] thinkpad io=9 transactions=1 max_io=9 refused=0 requests=0 poll_ms=0 total_io=9 total_transactions=1   (first window: the probe)
```

Then, with nobody touching the volume: no `[ec] thinkpad io=` record, no
`smi`/`smi_coincide` record (they print only in windows with events), no
2.1 ms `irqoff_long`, and `irqoff_section ... violations=` no longer growing
by 4 per window. Opening the volume popup: one window with
`[ec] thinkpad io=9 transactions=1 ... requests=1` and `smi ... gaps=2`
`smi_coincide ... ec_near=2 ... ec_poll=off`. Clicking Mute: `transactions=3`
and up to seven gaps. A level drag: no EC record.

| Observation | Meaning | Next step |
| --- | --- | --- |
| idle windows without `smi` records; gaps only where `[ec] thinkpad transactions>0` | the EC event SMI was the whole blackout | close f1-94/f1-100 |
| `gaps` ≈ 2 × `transactions` in the same windows | GPI[12] model confirmed per transaction | none |
| gaps continue in windows with `transactions=0` | a second firmware source | `smi_monitors`, `last_io`, H-page bit experiment |
| `last_io=0062/..` or `0066/..` in EC windows | last CPU I/O before SMI# was our EC access | supports the model |
| `refused>0` | a driver path exceeded its 4-transaction bound | report: driver bug |
| `ecpoll=1` boot | as 5a10eef4: `rate` ≈ 2/s, `io=18 transactions=2` per window | diagnosis only |

## Host evidence

- `tests/host/audio/thinkpad_ec_test.inc` (run by `tests/host/audio/run.sh`,
  part of `host_kernel_tests.sh`): option parser (default off, `ecpoll=1`
  1 Hz, `ecpoll=0` wins, exact and bounded tokens); probe = one RD_EC of 9
  port accesses with the exact window record; no traffic over 100 s with the
  poll off; level-only set without I/O; mute set = 3 transactions; refresh
  rate bound (none before 1 s); fifth transaction refused without I/O and
  counted; a dead EC's operation within 500–540 accesses; retry by an
  explicit action at the failure cadence; native start records for the three
  token cases; requests counted only with a running worker; the requested
  refresh reads with the poll off. The existing poll, tolerance, recovery and
  stable-read tests run with the poll enabled explicitly.
- `tests/host/audio/audio_test.c`: `MIXER_REFRESH_MASTER` refused on a public
  handle, accepted on the control handle, reserved/size rules.
- `tests/host/smi_attribution_test.c`: `smi_monitors` text and the
  `last_io` tail; no PM read without the sampler.
- `tests/host/hardware_check_runtime_test.c`: H page keyboard/EC window makes
  five refresh requests in 5 s with the EC and none without it.
- Desktop (`tests/host/desktop/memory_footprint_test.h` via
  `shell_launch_test.c`): the 250 ms mixer observation never sends the
  refresh; opening the popup sends one on the control handle.
- Loader: `tests/host/test_loader_model.py`, `test_boot_modes.py` (assembled
  parser and model accept `ecpoll=0` and `ecpoll=1`, reject variants).
  `scripts/loader_budget.py`: free 791 bytes before and after (the nine bytes
  of `'ecpoll=1',0` fall into an existing `align 16` pad).
