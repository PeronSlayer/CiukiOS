# Trident 4DWave and ALi M5451 transports

Research and implementation decision: 2026-10-23. Implements
[f1-120](../directives/f1-120-trident-4dwave-ali-m5451.md).
Host register models do not qualify hardware, analog routing or latency.

## Primary sources and repository comparison

The Linux v6.12 ALSA donor files were retrieved through the GitHub file API;
their blobs agree with the binding peripheral source audit:

| Source | Blob | Used portions |
| --- | --- | --- |
| [sound/pci/trident/trident.h](https://github.com/torvalds/linux/blob/v6.12/sound/pci/trident/trident.h) | `ed2d4eecc704affad9dca73e906d81dbb4305c44` | DX/NX register maps, codec bits, 30-bit addresses |
| [sound/pci/trident/trident_main.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/trident/trident_main.c) | `e98eea1e6d81e0d5fe123792c6b6f659b77102f5` | codec read/write, voice packing, playback/capture prepare, trigger, cursor and initialization |
| [sound/pci/ali5451/ali5451.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ali5451/ali5451.c) | `793d2f13267ea1212c30b0ff3e4e941f53ad08fb` | revision-2 codec port/write bit, sample timer handshake, voice 31 capture, 31-bit DMA, still-mode buffers |
| [arch/x86/pci/irq.c](https://github.com/torvalds/linux/blob/v6.12/arch/x86/pci/irq.c) | `0de436316a1dd147c057ac6b2db56701e357834f` | `pirq_ali_get`, read-only M1533/M1563 route nibble decoding, GPL-2.0, Martin Mares |

The vendor [4DWAVE-DX Technical Reference Manual, rev. 1.1](https://sunsite.icm.edu.pl/pub/linux/alsa/manuals/trident/4dwave_dx.pdf),
sections 4.1.2 and 4.2.2, independently identifies legacy decode controls,
the voice banks, command/status registers and the separate recording engine.
No verified NX or ALi vendor manual was retrieved; the pinned upstream driver
is the implementation reference for those differences. No unverified ALi
M5450/M5451 LED-driver datasheet is treated as an audio specification.

Existing `backend.c` has a copied cyclic playback transport, stop quarantine,
fixed eight-period lead, codec restoration and shared PCI IRQ ownership.
`vm_source.c` already feeds its mixer through backend-neutral publication.
Neither AC'97 nor ES137x register programming can drive these voice engines.
Existing routing recognizes PIIX, VIA, SiS and nForce but not ALi bridges.
Add only the pinned upstream ALi readback mapping, bounded links 1..16,
with valid firmware table/checksum, pin bitmap and ELCR level already required.
No route, bridge or ELCR register is changed.

## Implementation boundary

Separate `trident.c` and `ali5451.c` engine modules reuse bounded private voice
helpers. Trident uses bank B voices 32/33 (PCM and muted period clock);
ALi uses voices 0/1 plus voice 31 for capture. Playback is S16LE stereo,
48 kHz, 8192 frames; the muted clock loops over 512 frames for 256-frame
notifications. DX CSO/ESO occupy the upper word; NX uses 24-bit offsets and
split delta fields. NX uses direct physical addressing with TLB disabled.
Trident recording uses its legacy DMA registers, not an invented capture
voice. ALi recording enables the special PCM-in channel.

Private kernel capture v1 (`audio_capture_open/read/close`) copies S16LE
stereo 48-kHz frames into caller-owned kernel storage. A nonzero generation
token owns one bounded ring; stale tokens fail before hardware access.
The 32-KiB capture ring is allocated and charged only on admission. Reads
are nonblocking, reject ambiguous wraps/overruns and protect the current
and preceding capture quantum. This does not add public devctls or advertise
recording through the frozen playback-only `/dev/audio` or DOS SB16 ABI.

Both DMA rings are allocated from the existing constrained DMA zone, checked
against their exact 30-/31-bit masks and pinned in the appropriate direction.
No memory limit or registry capacity increases. The private capture ring
uses a remaining existing resource slot. Each engine first rejects active
firmware voices, capture, secondary codec users and enabled legacy decode;
no shared AC-link, bridge, power-management or global reset is attempted.
A missing ready codec is reported, rather than resetting firmware peers.

Codec transactions use 100-ms elapsed deadlines in thread context with
posted-write completion/readback. Fixed-rate AC'97 mode is verified; record
select/gain are preserved and restored on capture close. Explicit master
mute affects output only. Stop masks owned IRQs, stops owned voices/DMA,
checks command/status and stable cursors against a live sample timer.
Unproved stop retains all uncertain buffers, pins and claims in permanent quarantine;
no retry may reset, publish or release them. Clean teardown leaves PCI BME
disabled. No new panic paths or physical port passthrough to DOS exist.

Transport identifiers extend the existing capability field: 14 DX, 15 NX,
16 M5451; all prior values and record bytes remain unchanged. Device Manager
uses the existing selected SOUND driver slot and resource records.
The existing backend-neutral DOS PCM/OPL mixer is the sole guest connection.

Integration, canonical builds and hardware qualification belong to the lead.
In particular ALi revision-2 bridge/PM initialization is deliberately not
performed without a lease; firmware must already supply a ready AC-link.

## Host measurement

The production i686 compiler measures `audio_dma_device` at 632 bytes,
versus 500 bytes at the worktree base (+132 bytes static). The separate
Trident, ALi and shared voice objects contain 2796, 2639 and 4170 text bytes
respectively before final linking. Playback pins 8 pages (32 KiB); admitted
capture adds 8 pages, returned on clean close. Two pin directions and masks,
failed unpin retention, and the four physical resource claims are modeled.
Registry capacities, VM quotas and memory floors are unchanged. These are
software footprint measurements, not measurements of available target RAM.
See [host validation](../validation/f1-120-audio-engines.md) for the evidence
and sandbox restrictions.
