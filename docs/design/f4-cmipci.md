# CMI8338/8738 transport (f1-118)

Research and implementation decision: 2026-10-23. This extends the F4
transport without changing its playback-only public device API.

Primary sources read before implementation:

- Linux **v6.12**, [`sound/pci/cmipci.c`](https://github.com/torvalds/linux/blob/v6.12/sound/pci/cmipci.c),
  GPL-2.0-or-later, Copyright (c) 2000 Takashi Iwai. Register definitions,
  rate table, `snd_cmipci_pcm_prepare`, trigger/pointer/interrupt, mixer and
  `query_chip`/create paths. Audited blob: `e3cac73517d68eea50d9b7f7d2457ab759919ffe`.
- C-Media **CMI-8738/PCI AUDIO Specification, preliminary version 1.3**,
  [archived vendor text and PDF](https://device.report/m/6e05c06a92c5307759f19ee675a1bf6ba2451d66d724262c806c81f117e17940),
  Audio PCI registers 00h–27h, 80h–8Ch and indexed mixer table: independent
  direction/enable/busy bits, decoder gates, interrupt hold/clear, frame
  counters, native mixer. The bitsavers revision 2.1 PDF returned HTTP 403;
  no claim is based on its unavailable contents.

Repository comparison: `audio/backend.c` already owns cyclic playback,
claims, pins, silent progress proof, IRQ dispatch, software master gain and
permanent quarantine. `audio_dev.c`/`vm_source.c` already mix virtual DOS
SB16/OPL into any selected transport. No physical guest I/O is required.

Decisions:

- Use channel A for S16LE stereo playback at 48 kHz and channel B for
  independently started private S16LE stereo capture at discrete native
  rates. Reject arbitrary rates; do not enable the donor's experimental PLL.
  Do not expose recording through `/dev/audio`, whose contract refuses it.
- Reserve one 32 KiB pinned playback ring and one 32 KiB pinned capture
  ring, 32-bit addresses, four-byte alignment. Use existing claim slots and
  one additional pin; do not enlarge registry capacities or memory limits.
  Capture stays stopped unless explicitly started by the private interface.
- Match only donor CMI8338A/B and CMI8738/B PCI identities (including the
  ALi rebadge), retain stereo 48 kHz on model 055, and refuse 8768 or unknown
  register signatures. Require a
  qualified existing PCI route and readable, idle firmware state before
  writes. Refuse active legacy decoders rather than revoke unowned leases.
- Keep SB/FM/MPU/game-port gates disabled throughout activation and release.
  No lease broker or hardware passthrough is invented. DOS uses the existing
  virtual DSP/DMA/OPL clock and software mixer at its unchanged 48 kHz.
- Indexed mixer access is through BAR0+23h/22h, not physical SB ports.
  Save changed mixer registers, select PCM at unity, mute external monitor
  sources and FM, and use the native PCM mute gate for explicit master mute.
  Restore saved mixer state only after both engines are proved idle.
- Stop masks the selected channel's interrupt and enable, then proves
  enable/busy clear and stable valid count over elapsed time before reset.
  Playback stop preserves live capture; full shutdown shares one 500 ms
  deadline across both channels and stops both before
  detaching IRQs, restoring mixer, disabling BME or releasing any storage.
  Failure retains both rings and claims permanently. No new panic path.
- Capture copies only completed periods, guards a full extra period against
  concurrent DMA, detects elapsed full-ring ambiguity/overrun, and fails
  without delivering possibly overwritten data. IRQs never copy or sleep.

Host models establish register and ownership behavior only. Real CMI8338 and
CMI8738 revisions still need clock, capture routing, analog output, interrupt
continuity, stop and latency qualification by the lead.

The owner's read-only Windows 98 Resource Kit, chapter 12, Overview of
Multimedia (`wrkc12_1.htm`, DirectSound/MCI paragraphs), was also checked:
application multimedia services remain independent of the physical adapter.
It supplies no C-Media register algorithm. No book text or media is imported.
