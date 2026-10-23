# AC'97 chipset and codec qualification

Research checked on 2026-10-23 for directive f1-107. Host register models
qualify the software protocol; board routing, clock, jacks and audible output
still require the lead's hardware evidence. No public ABI or memory quota changes.

## Sources and repository comparison

All Linux references below are tag **v6.12**, fetched through the GitHub read
API into ignored `build/research/f1-107/linux-v6.12/`.

- [sound/pci/intel8x0.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/intel8x0.c):
  PCI IDs, register sets, `snd_intel8x0_ali_codec_*`, `snd_intel8x0_ali_trigger`,
  `snd_intel8x0_ali_chip_init`, and `snd_intel8x0_init`. ICH5 and nForce3
  were missing locally; ICH4/5's extra capture/SPDIF engines were outside the
  local 64-byte claim and idle check. ALi uses BAR0 only, PCM-out at 50h,
  command/status ports at 20h/22h/24h, and DMACR start/pause, not Intel RUN.
- [sound/pci/via82xx.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/via82xx.c):
  `via823x_cards`, channel register sets and fixed-rate DXS configuration.
  Local 3059 revision admission stopped at 50h, excluding VT8237 (60h).
  VIA remains a separate SGD backend, including VT8233A's stereo multi-channel block.
- [sound/pci/ac97/ac97_codec.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ac97/ac97_codec.c),
  [ac97_patch.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ac97/ac97_patch.c),
  [ac97_id.h](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ac97/ac97_id.h),
  [include/sound/ac97_codec.h](https://github.com/torvalds/linux/blob/v6.12/include/sound/ac97_codec.h),
  [include/sound/ac97/regs.h](https://github.com/torvalds/linux/blob/v6.12/include/sound/ac97/regs.h):
  exact/masked codec IDs, CMI9738/9739 absent mixer controls, AD jack/mute
  fields, Realtek pin sharing and subsystem-specific amplifier wiring.
  The existing driver treated every codec as the same 5-bit master/PCM mixer.
- [arch/x86/pci/irq.c](https://github.com/torvalds/linux/blob/v6.12/arch/x86/pci/irq.c):
  `pirq_via_get`, `pirq_sis503_get`, `pirq_ali_get`; ALi encodes IRQs through
  a translation table. Keep routing read-only, reject unknown links, and
  require a valid firmware table, IRQ bitmap and ELCR level. nForce private
  links retain the existing BIOS-table policy. No invented route writes.
- Intel [AC'97 specification 2.3 rev. 1.0](https://hands.com/~lkcl/ac97_r23.pdf),
  sections 5.7–5.8: reset capabilities, optional VRA, mixer masks, EAPD and
  accepted-rate readback. VRA is discovered in 28h and verified in 2Ah;
  codec names alone do not establish a rate or usable output.
- Intel [82801CA ICH3 datasheet, 290733-002](https://www.intel.co.kr/content/dam/doc/datasheet/82801ca-io-controller-hub-3-datasheet.pdf),
  sections 5.18 and 13.2: BDL sample counts, CAS, DCH and bounded stop proof.

## Implementation decisions

Retain existing PCM staging, 32-bit DMA pins, shared IRQ budget, elapsed
deadlines, safe-mode disablement and permanent quarantine. No new panic paths.
Qualify ICH0–ICH5, SiS7012 and nForce/2/3 layouts with independent literal
register expectations. Admit ALi M5455 only with its own BAR, codec command,
FIFO, start/pause, IRQ and stop models; M5451 remains a separate engine.
Reject active or unreadable sibling engines before reset.
Silent admission requires observed PICB/CIV movement, or completion of both
silent descriptors; merely clearing DCH is insufficient. ICH4/5 checks all
six DMA blocks, nForce its separate S/PDIF block, and ALi all eleven blocks.

Use a shared codec identity/capability table, with exact matches before
masked revisions, runtime VRA probing and per-codec mixer masks. Unknown
codecs keep verified generic stereo behavior. Preserve/restore every vendor
field changed, including partial failures. Jack retasking and EAPD inversion
are never inferred from the controller vendor. Apply only the selected
codec/subsystem quirks; report complex unsupported codec paths explicitly.
Expose the codec name, actual fixed/VRA mode and failure/access reason in the
existing Device Manager strings, preserving version-1 record layout.

Linux-derived tables and adapted register algorithms retain their original
notices and per-file/tag origins in `docs/licenses/linux-ac97-NOTICES.txt`
and `docs/credits.md` (with the matching About credits row).

The selected vendor writes cover AD1885/1886 output wiring, AD1980/1985
front/headphone selection and split mutes, AD1981 split mutes, Realtek
stereo jack-retasking masks (with the MSI ALC655 EAPD exceptions), ALC850
front amplifier/jack fields, and C-Media's nonfunctional mixer controls.
CS4299 retains the existing S/PDIF-disable behavior. Only Intel subsystem
1014:0534 takes the X31 EAPD inversion; the T23's 1014:0222 does not.
Wolfson WM9711/12/13/14/15 use specialized mixers and are named but refused
with `codec_route_unsupported`; no false generic-stereo readiness. STAC
vendor calibration and additional board-specific jack maps remain hardware
qualification items; their registers are not programmed from guessed masks.
AD1885/1886 headphone controls use the donor's six-bit, +6 dB maximum scale:
code 4 (0404h stereo) supplies unity instead of boosting at code zero.
