# ESS Maestro PCM transport (f1-116)

Implementation research: 2026-10-23. Host models establish protocol behavior;
E500 speaker, microphone, clock accuracy and latency require lead hardware evidence.

Sources read before implementation:

- ESS [Maestro-2 datasheet SAM0056-102997](https://www.alsa-project.org/files/pub/datasheets/ess/DSMaestro2.pdf),
  pages 13–21 and 25–31: I/O BAR, bus mastering, WaveCache, stereo pairing,
  indexed words, interrupt ownership and ASSP reset/run. The document has
  inconsistent ASSP offsets in its summary; use the detailed A2/A4/A6 map,
  confirmed by both Linux donors.
- Linux [sound/pci/es1968.c, v6.12](https://github.com/torvalds/linux/blob/v6.12/sound/pci/es1968.c):
  APU modes, stereo cursor units, PCMBAR at 1FCh, rate step, four-APU planar
  capture, clock measurement, timer, ringbus and docking output initialization.
- Linux [sound/oss/maestro.c, v2.6.18](https://github.com/torvalds/linux/blob/v2.6.18/sound/oss/maestro.c):
  independent confirmation of the same engine, 28-bit/one-window restriction,
  mixer and docking paths. Both files are GPL-2.0-or-later; retained origin
  and notices are in the driver and the linked credits notice.
- Intel [AC'97 2.3 rev. 1.0](https://hands.com/~lkcl/ac97_r23.pdf), section 5.7,
  register 26h: EAPD=0 enables the standard external amplifier; readback
  proves the register setting, never the board's wiring or actual sound.
- Windows 98 Resource Kit, local read-only `build/rskt/wrkc24_4.htm`, sound
  troubleshooting: distinguish sound-card driver identity, IRQ/resources
  and resource conflicts. No Microsoft code or text is imported.

The repository's cyclic transport already supplies copied native/VM PCM,
publication guards, elapsed deadlines and retained quarantine. Maestro adds
its own APU/WaveCache operations, not Intel BDL emulation. Public caps append
transport IDs 7/8/9 for Maestro1/2/2E; existing record layouts stay unchanged.
The output remains S16LE stereo at 48 kHz. The silent APU run measures the
engine clock before setting its rate step; unsupported or ambiguous clocks
fail admission. Source and DOS SB16/OPL clocks remain independent.

One 128 KiB aligned arena is allocated only for a detected device, using the
existing below-16-MiB allocator (a stricter subset of the 28-bit mask). It
contains a zero guard page, the 32 KiB playback ring, 1 KiB capture mixer
scratch and a 32 KiB planar capture ring. Alignment prevents a 4 MiB window
crossing and 16-bit APU loop-address crossings. All 32 pages share one
bidirectional pin and registry extent. No allocator limits or quotas change.

Claims and validated firmware $PIR/ELCR routing precede indexed writes.
All 64 APUs and the ASSP are checked before takeover. Live engines,
unreadable decode, secondary codec/link dependencies and running ASSP are
refused, without resetting them. Codec reset GPIO pulses are deliberately
omitted: a responding firmware-initialized AC'97 codec is required. Early
Maestro1 PT101 codecs fail identity/readback admission. Indexed operations
are serialized with interrupts excluded only for bounded I/O transactions;
codec waits and stop observations sleep in thread context.

Playback uses APUs 0/1; capture uses SRC APUs 2/3 and input mixers 4/5.
The private kernel capture interface copies planar data into interleaved
frames, checks generation and elapsed wrap bounds and fails on overrun.
It accepts 4–47.999 kHz (SRC step 10000h is forbidden), microphone or line
input, with no monitor feedback. Public `/dev/audio` remains playback-only
under its frozen contract; virtual DOS recording is not advertised.
Capture stop proves its four APUs idle while playback continues and retains
the arena; an unproved capture stop quarantines the entire physical lease.
Playback stop invalidates capture and proves all six modes off, stable
cursors, WaveCache disabled and PCI bus mastering clear before release.
Failure keeps the complete arena, claims and pin quarantined. There is no
reset, reinitialization, fallback or buffer reuse after quarantine.

For the exact E500 identity `125d:1978`, subsystem `0e11:b112`, the driver
uses the donor's stereo ringbus and ES978 docking mixer path with saved
readbacks. It uses the existing qualified PIIX firmware routing path and
never changes the route or firmware input/thermal ownership. GPIO direction,
mask and data remain firmware-owned. No NEC amplifier GPIO quirk is applied
to Compaq. Standard EAPD and codec status are reported separately from the
explicit `analog=unqualified` result. Unknown board polarity requires
hardware evidence and a reviewed quirk, rather than blind GPIO toggling.

Lead acceptance must record the E500 BIOS/route, codec ID, EAPD/readiness,
headphone versus internal-speaker output, L/R order, captured input, actual
clock and wrap/stop/latency results, plus available/reserved/free memory.
Host allocation measurements are not physical memory qualification.
