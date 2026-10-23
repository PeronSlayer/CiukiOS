# F4-18 audio levels and IBM ThinkPad console volume

Research before implementation, 2026-10-14. Host implementation only; the
lead owns QEMU and physical qualification.

- [Creative SB hardware guide, chapter 4](https://www.ardent-tool.com/sound/Sound_Blaster_HW_Programming_Guide_1st.pdf)
  specifies 5-bit attenuation, legacy nibble aliases and output gain in
  6 dB steps. The T23 evidence supplied in the directive shows two default
  14 dB attenuators in Ciuki's virtual master/voice path.
- [DOSBox Staging source](https://raw.githubusercontent.com/dosbox-staging/dosbox-staging/main/src/hardware/audio/soundblaster.cpp)
  (`calc_vol`, `ctmixer_reset`) and
  [DOSBox-X source](https://raw.githubusercontent.com/joncampbell123/dosbox-x/master/src/hardware/sblaster.cpp)
  (`calc_vol`, `CTMIXER_Reset`) actually use a logarithmic curve, **not** the
  linear curve hypothesized in the directive. Both reset master, PCM, FM and
  CD to 31. Implement the explicitly requested linear Ciuki curve (n/31),
  independently, and their unity reset policy. Keeping 24/31 twice would
  still lose 4.45 dB and fail the directive's 3 dB requirement. Output gain
  uses integer Q16 +0/+6/+12/+18 dB factors; the final sum saturates to S16.
  Legacy nibble writes expand to 5 bits, with endpoints zero and unity.
  CD registers share this policy; CD playback is not added in this step.
- [Linux ThinkPad ACPI documentation](https://raw.githubusercontent.com/torvalds/linux/v6.12/Documentation/admin-guide/laptops/thinkpad-acpi.rst)
  describes the IBM console amplifier after AC'97 and firmware volume keys.
  [Linux volume subdriver](https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/platform/x86/thinkpad_acpi.c)
  reads EC 0x30, low nibble 0..14, bit 6 mute. `volume_quirk_table` enables
  levels on all IBM ThinkPads; the T23 is included by that wildcard.
  The T23 firmware table identifies BIOS model `1A` and EC version 1.23;
  `1A` must not be asserted to be a separately confirmed EC identifier.
- [tp_smapi thinkpad_ec](https://raw.githubusercontent.com/linux-thinkpad/tp_smapi/master/thinkpad_ec.c)
  uses a different LPC3 interface at 0x1600 for battery/accelerometer data.
  Do not copy that protocol for ACPI register 0x30.
- [ACPI EC commands and handshake](https://uefi.org/specs/ACPI/6.5/12_Embedded_Controller_Interface_Specification.html)
  defines RD_EC 0x80, WR_EC 0x81, IBF bit 1 and OBF bit 0. Use bounded
  transactions on data 0x62 / command-status 0x66, preserving unrelated
  bits on writes. No trustworthy T23 DSDT was retrieved: port qualification
  remains a physical check, not a claimed measurement.
- [DMTF SMBIOS 2.5](https://www.dmtf.org/sites/default/files/standards/documents/DSP0134v2.5Final.pdf)
  defines the checksummed BIOS entry point and type-1 manufacturer/product/
  version strings. `bootinfo.c` validates the handoff but does not retain DMI
  identity. Detect IBM + ThinkPad in bounded SMBIOS tables; explicitly skip
  the loader's QEMU flag before any EC port access. Do not infer a ThinkPad
  from its audio or display PCI IDs.

Settings decision: the boot fallback is 20/20, unmuted. A valid
`System\\Audio` int32 `Master` is authoritative: bits 0..7 left percent,
8..15 right percent, bit 16 mute, other bits zero. One typed value makes
the stereo/mute preference a single libreg transaction. Missing/invalid
preferences retain the existing validated dual-record AUDIO.CFG fallback,
then the default. The kernel uses the production libreg parser through a
small allocation adapter; it never rewrites registry text. A pending root
journal uses AUDIO.CFG for this boot; user-space libreg performs recovery.
The existing delegated desktop mixer worker writes Master for Control Panel,
tray and Volume Control changes. Hardware changes are applied without disk
I/O in the EC driver and saved by the desktop worker on observation.
No default Master is seeded into an image: that would mask a legacy user's
saved AUDIO.CFG on an updated image.

The IBM EC is the effective hardware master. At initialization set its
level to the boot preference (rounded to 14 steps), preserving mute/other
bits; subsequent 250 ms polls update the shared mixer. User mixer changes
write the same EC control. AC'97 master remains unity (mute still gates
silence), and software mixing bypasses this master gain exactly once.
The existing tray volume popup and Control Panel slider refresh from the
shared master; no new widget or synthetic keyboard event is introduced.
The pre-existing native/VM QEMU probes use an explicit, temporary 50% fixture
and restore the boot preference without saving it. VMPLAY's voice-25 PCM16
pins are calculated independently from its sample pattern: peaks 1458/1460,
PCM FNV 465bca2c, ring FNV 57120d15. The same host oracle reproduces the old
logarithmic pins exactly; these new pins still require the lead's QEMU run.
EC transactions run in task context under a mutex, have time and iteration
limits, and disable polling on a protocol failure. The driver claims its
two I/O ports and does not enable ACPI, SCI or any unrelated EC feature.

## F4-27: runtime EC timeout tolerance (2026-10-14)

The supplied T23 image `66843edc` transcript proves that SMBIOS eligibility
and real EC reads/writes succeeded before a single `-110` stopped polling.
It does not measure the busy interval or prove that the EC stopped responding.
Inspection found a 20 ms deadline, immediate deactivation in `ec_transfer`,
a worker that exits on deactivation, and a stale `master_in_hardware` flag.

Research checked against the implementation before this change:

- [ACPI 6.4 §12.2](https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/12_ACPI_Embedded_Controller_Interface_Specification/embedded-controller-register-descriptions.html)
  requires IBF clear before host writes and OBF set before host reads data.
- [Linux upstream EC driver](https://raw.githubusercontent.com/torvalds/linux/master/drivers/acpi/ec.c)
  defines `ACPI_EC_DELAY` as 500 ms and supports sleeping transaction polling.
  `ec_poll` restarts incomplete commands; `advance_transaction` sends a new
  command once IBF clears. Follow that restart behavior after a timeout, so
  an EC reset that lost a reply cannot prevent recovery.
  This supports a longer bounded wait; it does not establish a measured T23
  latency or mandate Ciuki's failure threshold/re-probe interval.

Decision (supersedes the runtime failure policy above): each poll, set or
probe operation shares a 500 ms deadline across its transfers and at most
500 sleeping polls, including a stopped-clock guard. Native polling sleeps
1 ms instead of yielding in a busy loop. A timed-out read's late reply may
be drained only when this driver owns the outstanding read; unsolicited
output remains a protocol refusal. If that reply is absent after IBF clears,
restart the command rather than waiting indefinitely for a reply lost in reset. There is no immediate transaction retry.
A failed runtime operation leaves the EC unavailable for master delegation,
retries after 250 ms, and counts once toward eight consecutive failures.
Success resets that count. At eight failures log `disabled` once and retry
at 10 s intervals; successful re-probe logs `resumed`. Timeout records include
the stalled stage and elapsed milliseconds and are limited to one per second.

Amendment f1-94 (2026-10-22): the poll and its failure retry run at 1 Hz
(1000 ms) instead of 250 ms, and the exact `BOOT.CFG` token `ecpoll=0`
disables the poll (`[ec] thinkpad poll_ms=0 source=ecpoll=0`). Reason: the
T23's 2.1 ms interrupt-off blackouts arrive at about the old 4 Hz rate and
the legacy-mode EC GPE can become an SMI; see
[f1-94](../validation/f1-94-irqoff-blackout.md).

Amendment f1-100 (2026-10-22): no periodic EC traffic by default. The T23
trial of 5a10eef4 showed exactly two 2.1 ms SMIs per EC read transaction:
the EC's event line (GPI[12], GPE 1Ch) is routed to SMI# while SCI_EN is
clear, so the 1 Hz poll alone cost about 4 ms of interrupt-off time per
second ([f1-100](../validation/f1-100-ec-traffic.md)). Now the poll is off
unless the exact `BOOT.CFG` token `ecpoll=1` is present (diagnosis;
`ecpoll=0` wins over it). The boot probe is one RD_EC when the register
already holds the saved mute preference (a stable second read is required
only before a first write); a mute change from the desktop keeps its read,
write and proving read; a level-only change makes no EC access. Every
operation is bounded to four transactions and counted in the `[ec] thinkpad
io=... transactions=...` window record. **Limitation:** the desktop mirror
of the hardware volume keys (tray and Control Panel percentage and mute) is
updated only by an explicit user action: opening the tray volume popup sends
`MIXER_REFRESH_MASTER`, which makes the EC worker read register 30h once
(at most once per second); the H page key window asks once per second. The
EC still drives its amplifier from the keys at all times, so the sound
follows the keys; only the displayed and saved preference can lag until the
popup is opened. A level change from the desktop never rewrites the EC level
and does not undo a hardware mute that has not been observed yet; a mute
change from the desktop reads the register first and writes the requested
state. Between two observations, several key presses can change the EC
level by more than one step; the mirror then keeps its percentage and
follows the mute bit, as the 1 Hz poll already did for presses inside one
second. Without a poll, a user change or refresh is also the retry after an
EC failure, at the existing cadence (1 s, 10 s after eight failures).

Amendment f2-96 (2026-10-22, supersedes the f1-100 default): the T23 trial
of a4d9454e showed that with no poll the system volume no longer followed
the volume keys (the tray, Control Panel, saved preference and mute gate
change only when the EC register is read). The default is again a poll, of
one RD_EC every 2 s: one transaction and two 2.1 ms SMIs, about 4 ms of SMI
time per 2 s (half the 1 Hz cost measured on 5a10eef4). `BOOT.CFG`
`ecpoll=0` turns the poll off (the f1-100 behaviour: popup refresh only),
`ecpoll=1` selects 1 Hz for diagnosis; `ecpoll=0` wins. A key press reaches
the mirror within 2 s; the popup's `MIXER_REFRESH_MASTER` stays. Records:
`[ec] thinkpad poll_ms=2000 source=default` and one `[ec] thinkpad io=9
transactions=1 … poll_ms=2000` per 2 s window. The codec stays at unity while
the EC owns the master: the [Linux ThinkPad ACPI
documentation](https://raw.githubusercontent.com/torvalds/linux/v6.12/Documentation/admin-guide/laptops/thinkpad-acpi.rst)
places the IBM console amplifier after the AC'97 mixer "under exclusive
control of the firmware", which handles the volume keys without the OS, so
applying the mirrored level to the codec as well would attenuate twice.
The SMI time of the poll no longer reaches the audio refill's budget:
cpustat leaves proven firmware time out of the scheduler budgets and the
service deadline (see the f2-96 validation record).

Amendment fx-3 (2026-10-23, supersedes the multi-press limitation above
and the f4-29 14<->0 wrap): the EC level of register 30h is a saturating
0–14 counter ([Linux v6.12 thinkpad_acpi.c](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/platform/x86/thinkpad_acpi.c?h=v6.12):
`TP_EC_VOLUME_MAX` 14; `issue_volchange` emits one key per level step between
two samples; the firmware keeps the level at its bounds). Every level step
between two observations now moves the master by five points (clamped
0–100), so several presses inside one 2 s poll are all applied; a 14 -> 0
change is fourteen presses down, not a wrap. The T23 trial of 9a60215f had
no EC transport at all (see `acpi-events-power.md`, legacy EC binding); with
the binding restored the hardware keys again move the tray, Control Panel,
saved preference and mute gate, a desktop level change still makes no EC
access and is not undone by the next poll, and a desktop mute change is
written to the EC and proved by a read.

The worker survives runtime deactivation. Initial identity, resource-claim and
stable-handshake gates remain fail-closed; this change recovers an EC already
qualified at boot, without bypassing quarantined resources.

Master changes during EC unavailability use the existing codec/software path
and normal durable preference save. EC errors do not become user-facing
master-set errors; codec and persistence failure semantics stay as before.
On recovery, apply the current shared preference (including changes made
while offline), read it back, then restore hardware delegation/codec unity.
A fallback transition also reapplies the current codec master even if no
volume value changed. No recovery disk writes run in the EC worker.
Physical amplifier state during an outage is unknown: software/codec fallback
cannot override an EC that remains muted or attenuated in hardware.
Host models qualify timing, failure thresholds, recovery, and persistence;
only the lead's T23 session can qualify keys, audible output and Control Panel.
