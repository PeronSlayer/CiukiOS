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
