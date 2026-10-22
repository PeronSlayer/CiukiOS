# f1-94 — the 2.1 ms interrupt-off blackout: attribution and mitigation

Written 2026-10-22 (implementer: Claude agent, Opus) for directive
[f1-94](../directives/f1-94-irqoff-blackout-2ms-smi.md), with the lead's two
follow-up notes of the same morning (QEMU NetSurf sample: instrument the IRQ
dispatch path and the schedule() it triggers). Host evidence only: no QEMU,
no image, no hardware run. A result on the T23 is evidence only for the image
SHA-256 it ran on.

## Evidence read (read-only)

- T23 trial of image b453bc92 (`build/codex/t23-b453bc92-serial.txt`,
  `build/codex/t23-b453bc92-BOOT.LOG`). 205 `irqoff_long` records, raw
  2102–2138 µs; the largest marked phase holds 2102–2137 µs of it. Phases:
  `pick` 52, `ldt/debug` 52, `address_space` 17, `vm-enter` 12, `other` 10,
  `fpu` 5, unmarked `gap` 57. 148 of 205 sit between two consecutive TSC
  marks around straight-line code (for example `vm-enter` is
  `tss_set_kernel_stack` only). 159 of the 205 begin at the scheduler's
  resume `crit_begin` (c0153971). The same 2.1 ms appears in
  `[irq] budget exceeded irq=11 owner=net/ac97/usb us=2109–2130` (14 records),
  `audio_refill_overrun consumed_us=2113–2539`, `usb-hid device_overrun
  consumed_us=2254–2499`; `violations` grows by 4–14 per 2 s window.
- H page of 2026-10-17 (dev diary 2026-10-17-02): `lost=76` ticks in 10 s,
  observed IRQ-off stall spacing 1.03/1.79/3.07 s, `SMI_EN=0000402b
  SMI_STS=00000900`. Decoded with 290716-001 9.8.3.14/15: enabled
  GBL_SMI_EN, EOS, LEGACY_USB_EN, APMC_EN, PERIODIC_EN; SWSMI_TMR_EN, TCO_EN,
  MCSMI_EN, SLP_SMI_EN, BIOS_EN clear. Status PM1_STS_REG (bit 8) and
  MCSMI_STS (bit 11): MCSMI_STS only records a 62h/66h access (our EC
  driver); with MCSMI_EN clear it cannot raise an SMI.
- Lead's QEMU NetSurf excerpt (`build/lead-scratch/qemu-netsurf-menu-serial.txt`
  in the main checkout): `irqoff_long site=c01181bd us=2079 phase=gap
  phase_us=1260 from=netsurf to=device-work`, `irqoff_section=irq0
  owner=irq0`; site c01181bd is the `crit_begin` of the non-V86 IRQ path in
  `trap_dispatch`. Under TCG there is no SMI.

A constant 2.1 ms that lands in any phase, any IRQ handler and any site is
not the cost of the code it interrupts. The rate (about 7.6 lost ticks per
second on the H page, 4–7 visible sections per second on the desktop) and the
fixed length match a firmware handler. The enabled periodic source cannot be
it: PER_SMI_SEL gives 8, 16, 32 or 60 s (9.8.1.1).

## Sources

- Intel 82801CAM ICH3-M datasheet 290716-001,
  <https://edbatalha.info/compaq-n610c/Intel%2082801%20ICH3-M%2029071601.pdf>:
  9.1.10 PMBASE (D31:F0 40h), 9.1.11 ACPI_CNTL (ACPI_EN bit 4), 9.8.1.1
  GEN_PMCON_1 (PER_SMI_SEL 1:0, SWSMI_RATE_SEL bit 10: 64±4 ms or
  1.5±0.5 ms), 9.8.1.5 GPI_ROUT, 9.8.3.3 PM1_CNT (SCI_EN 0: "these events
  will generate an SMI#"), 9.8.3.13 GPE1_EN, 9.8.3.14 SMI_EN, 9.8.3.15
  SMI_STS, 9.8.3.16 MON_SMI, 9.8.3.18 DEVTRAP_EN, 11.1.16 USB_LEGKEY, table
  5-37 (causes of SMI# and SCI).
- Linux v6.12 `drivers/usb/host/pci-quirks.c`, `uhci_reset_hc` and
  `uhci_check_and_reset_hc`
  (<https://github.com/torvalds/linux/blob/v6.12/drivers/usb/host/pci-quirks.c>):
  the UHCI handoff writes USBLEGSUP `0x8f00` ("Turn off PIRQ enable and SMI
  enable. (This also turns off the BIOS's USB Legacy Support.)") and never
  touches the chipset SMI_EN.
- Linux v6.12 `drivers/watchdog/iTCO_wdt.c`
  (<https://github.com/torvalds/linux/blob/v6.12/drivers/watchdog/iTCO_wdt.c>):
  TCO_EN is cleared ("Disables TCO logic generating an SMI#") only by the
  driver that owns the TCO watchdog and its NO_REBOOT bit, as a
  read-modify-write that keeps every other enable.
- Linux v6.12 `drivers/acpi/ec.c`
  (<https://github.com/torvalds/linux/blob/v6.12/drivers/acpi/ec.c>): "Enable
  GPE for command processing (IBF=0/OBF=1)": the embedded controller raises
  its GPE during a transaction. With SCI_EN clear (CiukiOS runs the T23 in
  legacy mode; `power.c` writes ACPI_ENABLE to SMI_CMD only at soft-off), GPE
  events become SMI# (9.8.3.3), so EC port traffic is a candidate trigger.
- Xenomai, "Dealing with SMIs" (<https://v3.xenomai.org/tips/x86/smi/>):
  disabling SMI sources can make a board overheat; keep the thermal source.
- Existing CiukiOS records: [f0-30 audit](f0-30-irqoff-audit.md) (F0-35
  register authority), [R1](../design/win98-class-resource-management.md)
  (software IF-off work ≤ 100 µs; SMI attribution never relaxes a budget).

## What changed

### 1. Attribution (`src/kernel/core/smi.c`, `smi_model.c`, `include/ciuki/smi.h`)

- `smi_attribution_init` runs in `drivers_init` right after `usb_init`
  (every UHCI handoff done). It reuses `registry_smi_snapshot` (ICH3-M
  `8086:248c` only, decoded PMBASE only), logs one `smi_config` record with
  the read-only context for the source hunt (PM1_CNT, PM1_EN, GEN_PMCON_1,
  GPE0_EN, GPE1_EN, GPI_ROUT, DEVTRAP_EN, MON_SMI, the three USB_LEGKEY) and
  arms the sampler. Any other bridge: `sampler=off`, no PM or PCI access.
- Every PIT tick (`timing_timer_irq` → `smi_tick`) reads SMI_STS once
  (ICH3-M only). That sample is the "before" of the next interrupt-off
  section. A tick gap above 2 ms is a blackout whether IF was set or not (a
  2.1 ms SMI always loses one tick); it is counted with its spacing.
- Every section of 1 ms or more (`cpu_irqoff_end` → `smi_section_end`, now
  for every long section, not only the window maximum) reads SMI_STS once
  after its end stamp and is classified (`smi_classify`):
  - `sts`: a status bit rose between the tick before the section and its
    end, and that bit can raise an SMI under the current SMI_EN (enable-gated
    bits 2–6, 11, 14 only with their enable; summary bits 8–10, 12, 13, 15,
    16 always; nothing while GBL_SMI_EN is clear). Direct evidence, but the
    handler normally clears its own status before RSM, so it is rare.
  - `tsc`: the largest interval between two consecutive phase marks lies in
    straight-line code (`cpu_irqoff_bounded`), is at least 1 ms and longer
    than all other time of the section; or an `irq_budget_stamp` no-work
    adjacent TSC pair fell inside the section. "Not our executed work":
    firmware or a stalled bus cycle on hardware, emulator time under TCG.
  - `unproven`: neither; reported, never discounted.
  Classification adds records only. `work_us`, `unclassified`, budget
  violations and device-budget charging are unchanged (R1).
- Cost: one `inl` per PIT tick and one per section of 1 ms or more, on the
  ICH3-M only; a TSC store per marked activity; all arithmetic and
  formatting in the cpustat task with IF set. Below the directive's 2 µs per
  section on any section.

### 2. Mitigation: LEGACY_USB_EN only

`smi_mitigation_plan` (pure, host tested exhaustively) clears exactly one
enable, LEGACY_USB_EN, as one read-modify-write of SMI_EN with IF clear:
every other bit as read (EOS as read, reserved bits kept), BIOS_RLS never
written, no status acknowledged, no USB_LEGKEY/PM1/GPE write. Preconditions:
qualified chipset (ICH3-M), not safe mode and not the H page (its F0-35
experiment owns SMI_EN there), GBL_SMI_EN and LEGACY_USB_EN set, all three
D29 UHCI functions present, owned by the native driver (`owned`, `ready`,
not quarantined, exact 8086:2482/2484/2487) and each USB_LEGKEY free of trap
and SMI enables (`& 00bf == 0`). The result and the readback are logged; a
failed readback is reported, not retried.

| SMI_EN bit | T23 | Decision | Reason |
| --- | --- | --- | --- |
| 0 GBL_SMI_EN | 1 | kept | Global: "No SMI# will be generated" includes power button, battery and thermal handling (Xenomai warning); R1 forbids SMM policy changes on the heuristic. |
| 1 EOS | 1 | written back as read | SMI arbiter re-arm; writing 1 while an SMI is pending would release it early. |
| 2 BIOS_EN | 0 | kept | ACPI GBL_RLS path; not used. |
| 3 LEGACY_USB_EN | 1 | **cleared** after the handoff | 9.8.3.15: LEGACY_USB_STS is the OR of the USB_LEGKEY status bits ANDed with their enables; after our handoff (`legsup 003b/0010/2000 → 0000`, then `2000` with the IRQ attached) the source belongs to devices we own. Clearing the gate also keeps a firmware re-arm of USB_LEGKEY from raising SMIs (it is recorded, see `rearm`). Linux stops at USB_LEGKEY; this is one step further, inside the same ownership. |
| 4 SLP_SMI_EN | 0 | kept | Firmware's sleep hook; our soft-off relies on firmware behaviour. |
| 5 APMC_EN | 1 | kept | APM BIOS interface and our own ACPI_ENABLE write to SMI_CMD (B2h) at soft-off. |
| 6 SWSMI_TMR_EN | 0 | kept | The firmware's own one-shot follow-up timer (64 ms or 1.5 ms); clearing it "will reset the timer and the SMI# will not be generated", which can cut a firmware sequence in half. Seen in `smi_regs en=` if the BIOS uses it. |
| 11 MCSMI_EN | 0 | kept | EC 62h/66h trap, off. |
| 13 TCO_EN | 0 | kept (not armed) | Linux clears it only as the TCO watchdog owner; NMI2SMI is not gated by it anyway. |
| 14 PERIODIC_EN | 1 | kept | Legacy power-management heartbeat (DEVACT_STS is "used in conjunction with the Periodic SMI# timer"); CiukiOS does no ACPI handoff, so the APM BIOS still owns power and thermal policy. 8–60 s cannot explain 4–8 events per second. |

ThinkPad T23 firmware: no IBM documentation of its SMM handlers is
available; the facts used are the H-page register values above and the FADT
(`fadt_rev=1 smi_cmd=00b2 enable=f0`): the BIOS supports the ACPI handoff, in
which it would route these events to SCI. That handoff needs an OS EC/GPE
driver (no AML interpreter in CiukiOS) and is a lead decision, not part of
this change.

Expectation: LEGACY_USB_STS cannot assert with every USB_LEGKEY enable
clear, so this clear alone is not expected to remove the 2.1 ms blackout.
The records below are what names the source.

### 3. Finer marks: scheduler, IRQ path, PIT top half

New phases (appended to `enum cpu_irqoff_phase`, existing names unchanged),
mark capacity 8 → 20:

- `schedule()`: `queue` (requeue, pick, slice, `cpu_switch`) after `pick`
  (accounting, device refresh), `proc` (`proc_task_switch`) after
  `ldt/debug` (`dosvm_task_switch`), `resume` (the resumed caller's own
  IF-off code after `switch_context`, until its next mark).
- IRQ dispatch (`trap.c`, all three paths): `irq-entry` (accounting),
  `irq-handler` (the handler; the section's `owner`/`site` name the vector
  and handler), `irq-eoi` (EOI, accounting), `irq-exit` (until `schedule()`'s
  `pick` on a return to user, or `crit_end`).
- PIT handler (`pic_pit.c`, `task.c`): `tick-serial` (UART rearm, shared-tick
  queueing), `tick-timing` (timing and the SMI_STS sample), `tick-sched`
  (`sched_tick` accounting), `tick-timers` (`ktimer_tick` transfer).

`resume` and `irq-handler` are not "bounded" for the TSC rule; all others
are straight-line code with fixed bounds. Review of the PIT top half for the
QEMU sample: `device_cpu_advance` ≤ 10 iterations (FOCUS_PERIOD),
`device_fair_level` ≤ 32 consumers, `ktimer_tick` ≤ 2 timers
(KTIMER_IRQ_BATCH), `kwork_queue` ≤ KWORK_CAPACITY pending entries,
`serial_tick` one OUT, `entropy_irq` and `biosvm_account_irq` O(1). No
unbounded loop was found, so no code was moved; the marks decide. The
page-fault service is not an IF-off path: native and Win32 faults reach
`ua_fault`/`w32_fault` after `sti()` in `trap_dispatch`, the DOS VM pool
fault runs after `sti()` in `dosvm_trap`, and a kernel-mode fault is a fixup
or a panic. The PIT replay of f0-46 (`tests/host/irq_work_replay.py`) is
unchanged with the new marks: PIT-scheduler-accounting max 28 µs,
PIT-full-ring-1000-timers max 63 µs, budget 100 µs. Under TCG
without `icount`, translation-cache flushes and chardev I/O consume host time
that the guest TSC sees; a QEMU `tsc` blackout in a bounded phase is
emulator time, not kernel work.

### 4. Coincidence with our periodic activity

Three marks store the TSC (>> 8, one 32-bit store) of the latest activity:
`usb-hid` turn begin/end (`input_service`, about 1 kHz), USB root scan
(`usb_step`, 250 ms per controller), ThinkPad EC port access
(`thinkpad_ec.c` native read/write, 250 ms poll). For each tick-gap
blackout, an activity in `[previous tick − 1 ms, delayed tick]` counts as
`near`. For each `tsc` section (precise blackout start = the mark opening
the gap) the lead time from each activity is kept as min/max.

## Records and expected T23 lines

Once per boot:

```
[kernel] smi_config chip=ICH3-M pmbase=1000 en=0000402b sts=00000900 pm1_cnt=.... pm1_en=.... gen_pmcon1=.... gpe0_en=.... gpe1_en=.... gpi_rout=........ devtrap=.... mon_smi=.... legkey=2000/2000/2000
[kernel] smi_mitigate result=cleared clear=00000008 en_before=0000402b en_after=00004023 kept=GBL_SMI_EN,APMC_EN,PERIODIC_EN
```

(`pmbase` follows the FADT `pm1a_cnt=1004`. Other `result=` values:
`diagnostic_boot`, `usb_incomplete`, `usb_not_owned`, `legkey_armed`,
`not_armed`, `smi_disabled`, `readback_failed`.)

Every 2 s window with a blackout or a long section (any chipset when the
boot is verbose; always on the ICH3-M):

```
[kernel] smi count=<sts+tsc> sources=<bits> max_us=<us> min_us=<us> sts=<n> tsc=<n> unproven=<n> gaps=<n> rate=<n.nn>/s period_ms=<min>/<max>
[kernel] smi_coincide gaps=<n> hid_near=<n> scan_near=<n> ec_near=<n> hid_turns=<n> scans=<n> ec_io=<n> ec_poll=<ms|off|none> leads=<n> hid_lead_us=<min>/<max> scan_lead_us=<min>/<max> ec_lead_us=<min>/<max>
```

On change, on a re-arm, otherwise every 15 windows (ICH3-M):

```
[kernel] smi_regs en=00004023 sts=00000900 rise=<bits> mitigated=00000008 rearm=<windows> legkey=2000/2000/2000
```

Expected on the b453bc92 workload if the blackout is firmware: `max_us` and
`min_us` within 2100–2140, `tsc` ≥ 70 % of `count + unproven` (the trial's
bounded share was 148/205), `rate` about 4–8/s (H page: 7.6 lost ticks/s),
`sources=00000000` most of the time (handler clears its status), `rise`
showing at least `00000800` (MCSMI_STS from our own EC accesses).
`irqoff_long` names the new phases: a 2.1 ms `vm-enter`, `proc`, `queue`,
`tick-*` or `irq-entry/eoi/exit` is not our work.

How to read the coincidence line (chance level for an independent source is
about `activity count × (gap window + activity span) / window`):

| Observation | Meaning | Next step |
| --- | --- | --- |
| `ec_near` ≈ `gaps`, `ec_lead_us` narrow | EC transaction triggers the SMI (legacy-mode EC GPE → SMI#) | lead: EC poll rate/method or the ACPI handoff |
| `scan_near` ≈ `gaps`, `scan_lead_us` narrow | USB root scan triggers it | confirm with `usbscan=0`, then change the scan |
| `hid_lead_us` narrow (not spread over 0–1000) | coupled to the usb-hid turn (`hid_near` is always high at 1 kHz) | inspect the HID service I/O |
| all near counts at chance, leads spread | independent firmware source | `smi_config` GPE1_EN/GPI_ROUT/DEVTRAP_EN name candidates; H page bit experiment |
| `sts>0`, `sources≠0` | named SMI_STS source | follow that bit in 290716-001 |
| `gaps`/`rate` near 0 after `result=cleared` | the legacy-USB SMI was the source (contrary to the LEGKEY values) | keep the clear |
| `rearm>0` | the firmware re-enabled LEGACY_USB_EN | report; we do not fight SMM |

Lead's QEMU repeat (NetSurf start, no SMI): the `irqoff_long` phase now
names the part of the 2079 µs: `irq-handler`/`tick-*` for the PIT top half,
`irq-entry`/`irq-eoi`/`irq-exit` for dispatch, a scheduler phase for the
switch. A gap in a bounded phase under TCG is emulator time.

## Host evidence

- `tests/host/smi_attribution_test.c` (in `test_hardware_check.py`): ICH3-M
  map and refusals for other bridges (ICH3-S, ICH2-M, ICH4-M, PIIX4, VIA);
  causal status `0001f728` for the T23's `0000402b`, MCSMI not causal;
  mitigation plan exhaustively over all defined enable combinations (only
  LEGACY_USB_EN ever cleared, never without GBL_SMI_EN and permission);
  classification replay of the b453bc92 phases (`ldt/debug`, `pick`,
  `vm-enter`, `address_space` → `tsc`; unmarked `gap` → `unproven`; causal
  status first; stale or non-causal status ignored); production
  `smi_attribution_init` with simulated ports (single write `00004023`,
  exact `smi_config`/`smi_mitigate` text), tick and section sampling (one
  SMI_STS read each), coincidence and leads, window records, quiet windows,
  re-arm reported without a write; refusals (H page, safe mode, armed
  LEGKEY, unowned or quarantined UHCI, failed readback, changed enable,
  unsupported bridge with no port I/O).
- `tests/host/scheduler_irqoff_test.c` (in `test_kernel_latency.py`): the
  production `schedule()`/timing/cpustat hooks offer every section of 1 ms or
  more to the attribution with the right gap, start and bounded flag; each
  new phase is named in `irqoff_long`; `timing_timer_irq` passes the
  previous tick; the no-work stamp pair reaches the attribution; the bounded
  mark overflow keeps its tail unmeasured.

- `tests/host/usb/workers_test.c`: the production worker loops (with the
  usb-hid and root-scan marks) keep their 4 scans/s and 1 ms HID cadence.
- `tests/host/irq_work_replay.py`: the f0-46 PIT model keeps 28/63 µs
  against the 100 µs budget with the new marks.

Full `scripts/test/host_kernel_tests.sh` green on 2026-10-22; commands and
results: `build/codex/f1-94-report.md`.

## Amendment 1 (lead decisions, implemented 2026-10-22)

- **R1.** The R1 paragraph in
  [win98-class-resource-management.md](../design/win98-class-resource-management.md)
  now states that clearing the SMI enable that belongs to a completed
  firmware handoff (today only `LEGACY_USB_EN`) is part of that handoff; all
  other enables stay with the firmware. The clear above is kept unchanged.
- **EC-poll experiment.** The ThinkPad EC volume poll becomes a boot-time
  setting: 1 Hz (1000 ms) by default instead of 4 Hz, and the exact
  `BOOT.CFG` token `ecpoll=0` disables it (`thinkpad_ec_poll_option`, same
  separators and exactness as the loader). With the poll off there is no
  periodic 62h/66h traffic at all (`thinkpad_ec_volume_poll` returns before
  any port access, the worker is not started); the probe at boot and
  user-initiated volume writes still talk to the EC. Failure retries follow
  the poll cadence (1 s); the 10 s re-probe after eight failures and the 1 s
  timeout-record limit are unchanged. The EC keeps driving the amplifier
  from the volume keys; with `ecpoll=0` only the desktop mirror of those
  keys stops. The loader accepts `ecpoll=0` in `option_tokens` and leaves it
  in the copied boot options, like `accel=0`/`usbscan=0`
  (`scripts/test/loader_model.py` in step; `scripts/loader_budget.py`: 887
  free bytes before, 871 after on this branch).
- **Record.** `[kernel] smi_coincide` carries `ec_io=<n>` (every EC port
  access in the window) and `ec_poll=<ms>|off|none`; the EC driver logs
  `[ec] thinkpad poll_ms=1000 source=default` or
  `[ec] thinkpad poll_ms=0 source=ecpoll=0` once at start.

One T23 trial answers the question (same image, two boots, same workload):

| Boot | Expected if the EC poll triggers the SMI | Expected if it does not |
| --- | --- | --- |
| default (1 Hz) | `rate` falls to the poll rate (1–2/s), `ec_near` ≈ `gaps`, `ec_io` ≥ 14 per window (at least 7 port accesses per poll) | `rate` stays ≈ 4–8/s, `ec_near` at chance |
| `ecpoll=0` | `rate` ≈ 0, `ec_io=0 ec_poll=off`, no 2.1 ms `irqoff_long` | `rate` unchanged with `ec_io=0` |

(The b453bc92 image polled at 4 Hz: four EC read transactions per second,
each a command, an address and a data byte with at least four status
reads.) A 1 Hz default that leaves `rate` near 4/s also rules out the EC
poll without the second boot.
