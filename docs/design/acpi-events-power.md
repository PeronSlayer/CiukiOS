# Generic ACPI events and power (f1-110)

This extends the approved power ownership decision in
`peripheral-driver-coverage.md`. Discovery still has a read-only entry point.
The kernel power worker retains its interpreter only after thermal and route
preflight, claims the exact FADT SystemIO extents, and admits writes through
generation-checked registry grants. Unsupported routes, addresses, AML regions,
thermal controls or resource conflicts refuse takeover. Uncertain rollback
retains claims in quarantine. No APM connection or suspend is attempted.

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
`abi.h`: battery=36 bytes; zone=68 bytes; status=736 bytes. Status has twelve
32-bit header fields, four battery records and eight zone records. Unknown
unsigned values are `0xffffffff`; unknown passive trend is `INT32_MIN`.
Ownership is FIRMWARE=0, OWNED=1, QUARANTINED=2, PENDING=3. `error` is ACPICA's
status. `shutdown_requests` is a coalesced sequence observed by the desktop,
which begins its existing orderly shutdown. Repeated events coalesce within
one second; a later press remains usable after a cancelled shutdown. No VM can invoke host power-off.
Lid and AC summaries stay unknown if multiple reports disagree or fail.
