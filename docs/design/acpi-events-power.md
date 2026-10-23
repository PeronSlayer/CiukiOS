# Generic ACPI events and power (f1-110)

This extends the approved power ownership decision in
`peripheral-driver-coverage.md`. Discovery still has a read-only entry point.
The kernel power worker retains its interpreter only after thermal and route
preflight, claims the exact FADT SystemIO extents, and admits writes through
generation-checked registry grants. Unsupported routes, addresses, AML regions,
thermal controls or resource conflicts refuse takeover. Uncertain rollback
retains claims in quarantine. ACPI never connects APM and offers no suspend;
APM 1.2 (f1-121) is the exclusive alternative behind the same provider
interface (see "One power-source interface" below).

On loss of ownership, current AC/lid/battery/temperature values become unknown.
An incoming SCI_EN=1 is not evidence of a firmware thermal owner: failed
activation or later loss of the native backend retains uncertain claims in
quarantine instead of advertising a safe legacy hand-back. Enable masks are
saved and restored with readback on a proved return to the incoming policy.

Call-73 QUERY additionally reports STOPPED=4 after all native stop proofs,
or UNCERTAIN=8 once flushing/stopping has become irreversible without those
proofs. The desktop retains a terminal diagnostic in either case. Only
STOPPED permits the safe-to-turn-off message; UNCERTAIN requires reset.
EBUSY before irreversible progress keeps the existing close/retry/cancel flow.

Research and implementation decisions:

- [ACPI 6.5](https://uefi.org/sites/default/files/resources/ACPI_Spec_6_5_Aug29.pdf),
  sections 4.1/4.7 and 5: SCI_EN handshake is bounded. Fixed events and GPE
  methods use ACPICA's dispatcher, with queued AML on the power worker, never
  in the hard IRQ. The PIC SCI route must already be level in ELCR.
- [Chapter 9](https://uefi.org/specs/ACPI/6.5/09_ACPI_Defined_Devices_and_Device_Specific_Objects.html):
  button Notify(0x80) requests the desktop's existing application closure
  sequence. Lid changes refresh `_LID`; they never imply suspend.
- [Chapter 10](https://uefi.org/specs/ACPI/6.5/10_Power_Source_and_Power_Meter_Devices.html):
  `_PSR`, `_STA`, `_BIX`/`_BIF` and `_BST` are evaluated on the worker. Validate
  DWORD types, revision, package extents, units and conflicting state bits.
  Capacities/rates are not combined across batteries with different units.
- ACPI 6.5 chapter 11: thermal values are absolute tenths Kelvin. Validate
  `_CRT`, `_HOT`, `_PSV`, `_ACx`/`_ALx` and passive coefficients. Active cooling
  uses firmware power methods. Passive control requires an actual processor
  cooling provider; its absence refuses takeover even below the trip. `_HOT`
  requiring unsupported S4, relative sensors and newer policy tables are
  unsupported, preserving firmware ownership. Re-evaluate trips on notification.
- ACPI 6.5 chapter 12: one EC command engine owns the discovered data/command
  ports (including 62h/66h), serializes transactions, polls progress directly
  with an independent deadline, and dispatches bounded `_Qxx` queries. Global
  lock dependent ECs are refused until FACS arbitration exists. ThinkPad
  amplifier operations use this broker; no independent native port engine.
- ACPI 6.5 sections 7.4 and 16: application closure precedes native storage
  flush and device quiescence, then `_PTS(5)`/`_S5` preparation and S5 entry.
  Failure returns diagnostics; it never forcibly reboots from a failed S5.
  The native barrier observes NIC stop errors and bounds callback/worker
  retirement, then stops the ThinkPad quirk, audio DMA and USB schedules.
  A failed device proof returns EBUSY to keep S5 and the desktop's safe-off
  result unreachable. The underlying failure is logged and claims stay pinned.
- ACPICA **20260930**, already vendored: reference paths
  `source/components/utilities/utxfinit.c`, `events/evsci.c`, `evgpe.c`,
  `evgpeutil.c`, `evxfevnt.c`, `hardware/hwacpi.c`, `hwxfsleep.c`, `hwsleep.c`
  under [the upstream tag](https://github.com/open-acpica/acpica/tree/20260930).
  No new donor files are imported. Existing notices and origin manifest remain.
- Local Windows 98 Resource Kit `wrkc30_4.htm:54–129` and `wrkc05_3.htm:42`:
  firmware discovery precedes ownership, with one OS power surface. No Microsoft
  implementation or book text is copied.

## Status interface

Call 73 adds B=`CIUKI_POWER_STATUS` (4), C=buffer, D=exact size. Other actions
and their desktop authorization remain unchanged. Native read-only status is
available to Control Panel. Version=1 and size must match; reserved must be
zero. Invalid input is EINVAL; inaccessible memory is EFAULT. Query copies a
cached snapshot and performs no AML or I/O. There are at most four batteries
and eight thermal zones; overflow refuses ownership and is reported as an
ACPICA error. Existing registry, heap, firmware allocation and table limits
do not grow.

All records are fixed-width pack(4) compatible, one field per source line in
`abi.h`: battery=40 bytes; zone=68 bytes; status=756 bytes. Status has
thirteen 32-bit header fields, four battery records and eight zone records.
Unknown unsigned values are `0xffffffff`; unknown passive trend is `INT32_MIN`.
Ownership is FIRMWARE=0, OWNED=1, QUARANTINED=2, PENDING=3. `error` is ACPICA's
status (APM: a positive errno). `shutdown_requests` is a coalesced sequence observed by the desktop,
which begins its existing orderly shutdown. Repeated events coalesce within
one second; a later press remains usable after a cancelled shutdown. No VM can invoke host power-off.
Lid and AC summaries stay unknown if multiple reports disagree or fail.

## One power-source interface (staging integration with f1-121, 2026-10-23)

ACPI (this record) and APM 1.2 (f1-121) are two providers of the kernel-only
`ciuki/power_source.h`; exactly one can hold its reservation. ACPI reserves
in `fw_power_takeover` after the static FADT checks and before any SCI route,
SMI_CMD/ACPI_ENABLE or other ownership write (EC port leases taken during
collection are released on refusal), activates once owned and releases on
`fw_power_stop`; an uncertain rollback keeps the reservation, so APM can never
take over after a partial ACPI ownership. APM is additionally excluded by any
RSDP and connects only on pre-ACPI machines once the global DMA/time owner
installs `power_source_set_platform` (see "APM suspend-service owner" below).

The kernel ACPI backend (`acpi/platform.c`) wires the reservation and
publication through four optional `fw_ops` hooks; read-only inventory and
host fixtures leave them NULL. Every `fw_power_publish` and every coalesced
shutdown request hands the record to `power_source_publish_record`. Call 73
STATUS, the tray and the Power page read `power_source_status` only: the
ACPI record (any ownership state) unless APM holds the reservation, in which
case the record is synthesized from APM's cached Get Power Status facts.
`provider` (header offset 44) is NONE=0, ACPI=1 or APM=2: the reservation
holder, NONE while firmware keeps power management. APM adds
`ac_online`=BACKUP (2) and one system battery with `unit`=PERCENT (2),
`full`=100, `remaining`=percentage and `seconds`=remaining life. Its `state`
uses the `_BST` bits: charging and critical come from the APM battery flag,
discharging only from a known off-line or backup AC state, otherwise
UNKNOWN; "no system battery" reports no battery. Lid and thermal stay unknown.

There is one soft-off path: call 73 OFF (and REBOOT) flushes storage and
quiesces native devices, then latches `power_source_mark_stopped`.
`power_source_soft_off` refuses with EBUSY before that latch and dispatches
to the active provider: ACPI's S5 worker or APM's worker, whose 5307h/CX=3
still requires the orderly proof, now `power_source_orderly_shutdown`
reading the same latch. `power_source_platform` therefore keeps only the
suspend services (`quiesce_dma`, `resume`). QUERY's CAN_OFF is
`power_source_can_off`: an active provider whose cached qualification (for
ACPI, an evaluated `\_S5`) allows Off. The Control Panel Power page reads the
cached record once per second while it is open (no firmware call).

## APM suspend-service owner (lead decision, 2026-10-23)

`core/power_platform.c` installs `power_source_set_platform` during boot,
after the firmware inventory and floppy and before `apm_start`, only when
the loader found no RSDP and the boot is neither safe mode nor a hardware
check. ACPI never uses these services, so an ACPI machine keeps them absent
and APM excluded; once APM holds the reservation a second installation is
refused (EBUSY). APM therefore connects on pre-ACPI machines.

- `quiesce_dma` runs call 73's native stop path (`power_devices_quiesce`:
  network including NDIS external netifs, audio, USB) on the owner's
  `power-quiesce` worker, so the APM worker keeps servicing firmware every
  10 ms. It answers EAGAIN while the stop runs, the stop's result once done,
  and ETIMEDOUT after the deadline. An expired request still starts the stop;
  its own failure path keeps uncertain claims in quarantine and nothing
  retires another caller's I/O. The stop is the S5 barrier, terminal for the
  boot: a later critical suspend gets the first result.
- `resume` restores the time base before workloads continue. After a resume
  notification (APM events 3, 4 and 11) it repeats the 8259A ICW sequence
  with the current masks and reprograms PIT channel 0 to 1000 Hz, as Linux
  v6.12 `apm_32.c` (`reinit_timer`) and `i8259A_resume` do; for every
  notification, including 7 (update time), it reseeds the realtime clock
  from the RTC when boot qualified the RTC as the clock source.
- Native devices stopped by `quiesce_dma` stay stopped, proved idle or
  quarantined, until the next boot: no driver restart path exists yet.
  Restarting network, audio and USB after a critical suspend is open work.

Ordinary suspend requests stay rejected; soft-off remains call 73's.
Evidence: `tests/host/power_platform_test.c` (production APM provider,
worker and arbiter with the owner).

## Legacy EC binding for kernel quirks (fx-3, 2026-10-23)

The T23 trial of 9a60215f logged `[ec] thinkpad gate=ec_handshake_failed
stage=probe error=-19`, then `[acpi] inventory status=17 ... power_owner=0`
(AE_TIME, firmware ownership). Two defects made the f1-110 EC service
unreachable for the ThinkPad volume quirk: the quirk probed at audio init,
before the inventory had bound any EC (`acpi_ec_quirk_transfer_counted`
refuses while the ports are not 62h/66h, so -19), and the engine admitted
transactions only on an ACPI-owned platform, so a firmware-owned machine
never got a transport and `audio_ec_attach` was called only when
`fw_owned`. The volume poll of f2-96 therefore never started.

- The inventory marks the engine settled (`fw_ec_settle`) once ownership is
  decided, in every outcome, and ends the collection-time AML preflight when
  the takeover was refused. `acpi_ec_quirk_attach` answers -EAGAIN before
  that; the quirk logs `[ec] thinkpad deferred until=acpi_inventory` and the
  acpi-power task calls `audio_ec_attach` after the inventory whether or not
  ACPI is owned (and on the no-task fallback).
- ACPI-owned: the quirk uses the ACPI-bound engine (62h/66h only).
- Clean firmware ownership (`CIUKI_POWER_FIRMWARE`: SCI_EN clear, SMM owns
  EC events, no interpreter left): the same engine is bound to 62h/66h
  through the owner's port claims for the quirk alone (`[ec] thinkpad
  transport=legacy`). It issues RD_EC/WR_EC only, never QR_EC, installs no
  GPE and runs no AML. This is the transport the quirk used on the T23 from
  f4-18 to f2-96, now without a second engine.
- Quarantined or uncertain ownership, no ACPI tables (an APM machine: the
  APM provider may call the BIOS concurrently), a faulted engine, or an
  ACPI EC described at other ports: refused without I/O.
- A legacy timeout does not latch the engine: the next transaction waits for
  IBF to clear and drops a late reply to its own read (the pre-f1-110
  recovery); the quirk keeps its own bound (eight failures, 10 s re-probes).
- Every EC port access is again marked for the f1-94 SMI coincidence record
  (`smi_mark(SMI_EC_IO)`), lost in f1-110.

Sources: [ACPI 6.5 chapter 12](https://uefi.org/specs/ACPI/6.5/12_ACPI_Embedded_Controller_Interface_Specification.html)
(EC_SC bits, RD_EC/WR_EC/QR_EC); [Linux v6.12 thinkpad_acpi.c](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/platform/x86/thinkpad_acpi.c?h=v6.12)
(volume subdriver: EC register 30h, level bits 0–3 up to 14, mute bit 6;
`issue_volchange`). Host evidence: `tests/host/acpi/events_test.c` scenario
`t23-legacy` (and `no-crt`, `rollback`), `tests/host/audio/thinkpad_ec_test.inc`
`test_thinkpad_ec_t23_attach`.

## Scan budget counts execution time (fx-3 follow-up, 2026-10-23)

Lead decision after the T23 trial of 9a60215f (`[acpi] inventory status=17
devices=1`, `[sched] device_overrun task=acpi-power count=49` while the
desktop started). Every budgeted firmware operation (`fw_budget_begin(steps,
ms)`) had `ms` of wall clock, so time the P_DEVICE scan task spent waiting
for the CPU counted as firmware work.

- `ms` is now the scan task's own execution time: `fw_ops.run_clock` reads
  cpustat's `run_cycles` of the current task (`cpu_path_clock`, IRQ time
  excluded, SMM time included) in 100 ns units. Without a calibrated TSC it
  is the wall clock, the previous behaviour.
- Hard wall caps stay: `max(ms, FW_WALL_CAP_MS = 2000)` per operation and
  `FW_SCAN_WALL_MS = 10000` for the whole scan (tables, load, walk,
  collection, takeover). 2 s is ACPICA's `ACPI_MAX_SLEEP` (acconfig.h), the
  longest single AML wait upstream admits; ACPICA bounds a `While` loop by
  30 s of timer (`ACPI_MAX_LOOP_TIMEOUT`, dscontrol.c), and Linux adds no
  namespace-load timeout of its own. The 10 s scan cap (five operation
  caps) bounds how long the EC quirk and the serial PnP owner wait for the
  ownership decision. The step bound (100000 per operation) is unchanged.
- One honest record per scan: `[acpi] scan run_us= wall_us= run_clock=task|wall
  first_error= first_stage= first_node= semaphore_timeouts=` and, when a
  budget expired, `[acpi] budget expired reason=run|wall|scan_wall|steps|cancel
  stage= node= run_us= wall_us= steps= run_limit_ms= wall_cap_ms= scan_cap_ms=`.
  Stages: tables, load, walk (with the device's name), the evaluated method
  (`_HID`, `_CID`, `_STA`, `_CRS`), collect, takeover. A timed-out OSL
  semaphore wait (it returns AE_TIME without a sticky failure) is counted.
  On the T23 the whole scan lasted about 180 ms of wall time
  (ticks 1786–1970), shorter than one 250 ms budget, so the AE_TIME there may
  also come from a step bound or a semaphore wait; the new record names it.

Windows 98 decides ACPI vs APM at Setup: ACPI is installed only on systems
on the retail CD's good-ACPI list unless `HKLM\Software\Microsoft\Windows\
CurrentVersion\Detect\ACPIOption` is 1 (any ACPI BIOS) or 2 (never)
(Windows 98 Resource Kit, OnNow/ACPI section, "To install ACPI", owner's
media in `build/rskt/` page wrkc30_4; also quoted on
[Tek-Tips](https://www.tek-tips.com/threads/acpi-in-win-98.19799)).
Secondary sources report that Windows 98 SE also accepts an ACPI BIOS by
date, from late 1999 onwards
([a 2006 linux-kernel post quoting a BIOS readme: 12/02/99](https://lkml.rescloud.iu.edu/0602.1/1559.html);
[LKML 2003 on Microsoft's good-list/date logic](https://lkml.iu.edu/0310.1/0799.html));
no Microsoft primary text for the date rule was found.
The T23 (2001, BIOS 1AET52WW) is in that range, so the scan must be able
to complete there; it now can unless its AML really exceeds the bounds.

What changes on the T23 if the takeover succeeds (analysis, not measured):

- SCI: ACPI_ENABLE through SMI_CMD (B2h), SCI_INT from the FADT on a
  level-triggered PIC line (ELCR checked), GPEs dispatched by ACPICA.
- EC events: f1-100 measured GPI[12] (EC event line, GPE 1Ch) routed to
  SMI# (`gpi_rout=01000000`). ICH3-M GPI_ROUT 01b is SMI#, 10b SCI; whether
  the firmware reroutes it on ACPI_ENABLE is unknown. The OS never rewrites
  it. The EC engine polls EC_SC every 20 ms for SCI_EVT and issues QR_EC
  itself, so `_Qxx` events (Fn keys, thermal, battery) are served either way.
- Power button (fixed event), lid (`_LID`), AC (`_PSR`), batteries
  (`_BIF`/`_BST`, every 10 s) and S5 through `_PTS`/SLP_TYP: f1-110 paths.
- Thermal: f1-110 refuses takeover for passive (`_PSV`/`_TC1`/`_TC2`/`_TSP`)
  and `_HOT` zones, which ThinkPad DSDTs usually have; the T23 may stay
  firmware-owned for that reason alone (status AE_SUPPORT in the record).
  If a zone is admitted, `_TMP` was evaluated every 100 ms; on ThinkPads
  `_TMP` reads EC registers, i.e. one EC transaction (two 2.1 ms SMIs on the
  T23, f1-100) every 100 ms. Superseded by "Thermal reads follow _TZP and
  Notify" below.
- Volume quirk: ACPI-owned uses the same engine (62h/66h); a busy engine
  (QR_EC or an AML field access in flight) is waited for within the
  quirk's deadline instead of failing (`acpi_ec_quirk_transfer_counted`).
  Firmware-owned uses the legacy binding above. Both are tested
  (`tests/host/acpi/events_test.c` ec-events and t23-legacy,
  `tests/host/audio/thinkpad_ec_test.inc` one-volume legacy/acpi).
- Shutdown: the quirk stops in `audio_power_quiesce` before S5 and does not
  touch the EC afterwards; the generic engine stays available for `_PTS`.
  The shutdown lock order itself is fx-5's.

Evidence: `tests/host/acpi/test.c` scenarios `preempted` (1 ms of other
tasks between two clock reads: the scan completes, wall about 1 s, run
about 0.1 ms), `preempted-wall` (same without run_clock: AE_TIME, the T23
shape) and `starved` (wall caps expire with an honest record).

## Lead decision 2026-10-23: passive-cooling zones keep firmware ownership

The f1-110 refusal stays: a thermal zone with passive cooling (`_PSV`,
`_TC1`, `_TC2`, `_TSP`, `_PSL`) or `_HOT` makes `fw_power_takeover` fail
(AE_SUPPORT) and the platform stays firmware-owned, as before f1-110.
ThinkPad DSDTs, the T23's included, usually describe such zones, so the T23
keeps legacy ACPI ownership: SMM handles thermal, battery and button events,
the volume quirk uses the legacy EC binding, soft-off uses the
power_source provider path. No passive cooling or processor throttling is
implemented now. Condition to lift it: an OSPM passive-cooling
implementation that samples `_TMP` every `_TSP`, applies ACPI 6.5 section
11.1.5.1 Equation #1 (ΔP[%] = _TC1 × (Tn − Tn−1) + _TC2 × (Tn − Tt), Tt =
`_PSV`, clamped by `_MTL`) to the processors of `_PSL` through a qualified
processor clock-throttling provider (FADT `P_BLK`/`P_CNT` or `_PTC`), with
host tests and a hardware qualification on the T23.

## Thermal reads follow _TZP and Notify (fx-3 follow-up 2, lead decision 2026-10-23)

On a platform the OS owns, the fixed 100 ms `_TMP` read is gone.

- ACPI 6.5 section 11.1.3 (Detecting Temperature Changes) and 11.1.3.1:
  the platform reports a meaningful change with `Notify(zone, 0x80)`;
  11.1.3.2 (Polling): "A thermal zone advertises the need to be polled by
  OSPM via the _TZP object"; 11.4.26 (`_TZP`): recommended polling period
  in tenths of seconds, 1 s granularity, zero = polling not needed, polling
  "allowed but strongly discouraged". The specification defines no default
  period for an absent `_TZP`.
- Policy (`src/kernel/acpi/thermal.c`): `_TZP` > 0 → poll that zone every
  `_TZP` (at least 1 s, at most 1 h); `_TZP` = 0 or absent → no polling,
  the zone is read only on `Notify(zone, 0x80/0x81/0x82)`, at the next
  20 ms event-worker turn (several notifications coalesce into one read).
  Linux v6.12 `drivers/acpi/thermal.c` behaves the same: `polling_frequency`
  is `_TZP`, 0 when absent, and only a DMI quirk forces 30 s on one board
  (https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/acpi/thermal.c?h=v6.12).
- EC bound: a zone whose `_TMP` issued EC transactions (counted by the EC
  engine) is never polled faster than every 10 s (`THERMAL_EC_MIN_MS`).
  The f1-100/f2-96 EC budget is one RD_EC per 2 s for the volume keys
  (about two 2.1 ms SMIs each on the T23); a thermal poll adds at most one
  `_TMP` per 10 s, a fifth of it. Notifications are not limited: they are
  firmware events, not polls.
- Zones that are not due keep their last temperature, trip facts and
  cooling demand; the takeover admission still evaluates every zone once.
- A notification on any other object refreshes the power sources (AC,
  batteries) at once instead of the thermal zones; the 10 s source refresh
  is unchanged.

Evidence: `tests/host/acpi/events_test.c` scenarios `tzp-poll` (`_TZP`=50:
no read at 1 s, read at 5 s), `tzp-zero` and `thermal-change` (`_TZP` 0 and
absent: 60 s without a read, the zone's Notify reads it at once),
`tzp-ec` (`_TMP` reads the EC, `_TZP`=10 asks 1 s: 3 RD_EC in 30 s, a
Notify reads at once).

