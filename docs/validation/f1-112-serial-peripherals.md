# f1-112 serial peripherals and virtual COM

Host validation, 2026-10-23. Base: `03102af9b32de24c5b8beea87f82cf134fb28fba`.
No image, QEMU or hardware evidence is claimed.

The interface and primary-source decisions are recorded in
[peripheral-driver-coverage.md](../design/peripheral-driver-coverage.md#serial-peripheral-and-dos-lease-interface-f1-112).
The Linux mouse equations retain Vojtech Pavlik's notice, exact v6.12
origin and GPL-2.0-or-later identification in the source and
[notice](../licenses/serial-mouse-NOTICE.txt). The notice is selected for
the image and included in both credits lists.

The firmware adapter consumes assigned PNP0500/PNP0501 HID/CID records
after the interpreter terminates. It retains BDA numbering and the logger,
checks `_STA`, and refuses variable/ambiguous/wide/incomplete resources
or IRQs without high/edge/exclusive proof. Safe boot does not enable this
adapter or automatic serial enumeration. No ISA isolation or `_SRS` writes
were added.

Host evidence covers both PnP encodings and checksum, both response phases,
DSR loss, character/total time bounds, Microsoft/Logitech/Mouse Systems/wheel
streams, corruption/resynchronization and the actual input merge. The UART
model covers divisor banking, FIFOs and all four RX triggers, priority/read
clear, timeout, modem edges, loopback/break and independent instances.
Production VM tests cover BDA, INT14 including a one-tick timeout, native
configuration refusal, MCR access during DLAB, lease conflicts, creation
rollback, two UARTs sharing an IRQ, NE2000 IRQ3 refusal, stop and teardown.
Device Manager tests verify owner kinds, IDs and generations without I/O.

The serial/logger regression passes five tests, firmware/input records nine,
and credits eight. The serial models, Device Manager, ABI oracle, full input
fixture and focused VM adapter pass under UBSan. ACPICA passes 311 cases
under its supported UBSan mode. Its i686 runtime/OSL link and ten changed
kernel integration units compile with canonical flags and the 4-KiB frame
bound. `nm` finds no duplicate external serial definitions. The ABI oracle
passes 1,813 target/native checks; the public API reference is regenerated.

Default ASan/UBSan runs of the new models, VM adapter, inventory and ABI
reach their assertions but exit with the sandbox's fatal LeakSanitizer
process-inspection error. Those commands are **blocked**, not green. No
script's leak detection was disabled. The existing serial/firmware-record
scripts keep their pre-existing sanitizer settings.

The broader VM regression stops at the unchanged
`tests/host/dosvm_speed_test.inc:810` frame-count assertion. An isolated
replay using the base VM implementation and original test body also fails
there, before serial initialization. The assertion and presenter are left
unchanged for lead triage; the DOS conformance battery is unchanged.

i686 layout measurements before/after: COM metadata 316/340 bytes per port
(2,528/2,720 bytes for eight slots); VM slot 782,336 bytes in both states
because the new state fits existing alignment padding. Each UART model is
84 bytes and its lease wrapper 96 bytes. Registry/VM/queue limits are
unchanged; no target physical-memory measurement or policy increase occurs.

Lead review still owns the additive ABI, launcher selection policy and
physical qualification. Native window creation defaults to null UARTs;
trusted launch configuration can request physical COM leases before boot.
The native backend retains its exact-integer-baud admission rule, so some
guest divisor settings (including BIOS 110 baud) report a line error on a
physical binding. Native errors are a consumed summary rather than an
assertion of per-character error attribution. Service timing uses the host
millisecond clock; cycle-accurate UART timing and electrical mouse power/
identification need hardware evidence. Dynamic idle hotplug monitoring is
not implemented. The inherited native 20-ms close bound may quarantine a
slow physical transmitter if it cannot prove idle.

Exact commands, result classification, log paths and remaining items are in
the implementer handoff `build/codex/f1-112-report.md` in this worktree.
