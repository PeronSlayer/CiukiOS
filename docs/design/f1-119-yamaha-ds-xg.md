# Yamaha DS-XG PCM transport (f1-119)

Research and implementation decision: 2026-10-23. This implements the
[directive](../directives/f1-119-yamaha-ds-xg.md), the owner's licence
amendment in [peripheral coverage](peripheral-driver-coverage.md), and the
[F4 transport contract](f4-audio-media-contract.md). Host models are protocol
evidence; physical playback and latency qualification remain with the lead.

Primary references inspected before implementation:

- Yamaha, [YMF724F DS-1 preliminary datasheet, September 21, 1998](https://www.alsa-project.org/files/pub/datasheets/yamaha/ymf724.pdf),
  sections 1-1/1-2: PCI memory master, 32 KiB non-prefetchable MMIO BAR,
  legacy decode/LAD at 40h, AC-link reset at 48h and power at 4Ah.
  Its public catalogue does not specify the native microcode bank protocol.
- Linux **v6.12**, [sound/pci/ymfpci/ymfpci.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ymfpci/ymfpci.c),
  PCI IDs and independent legacy functions;
  [ymfpci.h](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ymfpci/ymfpci.h),
  register offsets and 30-word playback bank; and
  [ymfpci_main.c](https://github.com/torvalds/linux/blob/v6.12/sound/pci/ymfpci/ymfpci_main.c),
  codec access (including pre-revision-2 YMF744 read workaround),
  pcm_init_voice, playback_pointer, interrupt, request_firmware,
  download_image, memalloc, hw_start/hw_stop and disable_dsp.
- ALSA firmware **v1.2.4**,
  [ymfpci/hwmcode.c](https://github.com/alsa-project/alsa-firmware/blob/v1.2.4/ymfpci/hwmcode.c)
  (Yamaha copyright) and
  [ymfpci/fw_writer.c](https://github.com/alsa-project/alsa-firmware/blob/v1.2.4/ymfpci/fw_writer.c):
  exact DSP/DS-1/DS-1E arrays and little-endian 32-bit firmware encoding.
  The imported header retains Yamaha's notice; the donor driver adaptation
  retains Jaroslav Kysela's copyright and GPL-2.0-or-later origin.
- Intel [AC'97 Component Specification 2.3, revision 1.0](https://hands.com/~lkcl/ac97_r23.pdf),
  sections 5.7–5.8, and Creative's [Sound Blaster Series Hardware Programming Guide](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/SoundBlaster.pdf),
  chapters 3–4: retain the existing codec unity/mute and virtual SB16 DMA,
  mixer and interrupt semantics. The Resource Kit's chapter 12,
  `wrkc12_1.htm` (local, read-only), also separates device-independent media
  commands from adapter mixing/playback. No Windows NT driver model is ported.

Repository comparison: the cyclic backend supported port I/O only and
allocated an extra descriptor page only for VIA. Add uncached MMIO access,
one pinned bidirectional page for a 65-word voice table, four playback banks,
disabled capture/effect banks and a 1 KiB work area. Keep the existing 32 KiB
PCM ring, frame retirement, prefetch guard, deadline inhibition, generation,
native mixer and DOS SB16/OPL source service. No memory limit changes.

The six exact IDs select backend 13 (`AUDIO_YMFPCI`): 0004/000a use DS-1;
000d/000c/0010/0012 use DS-1E. Fixed 48 kHz S16LE stereo uses a left/right
voice pair at unity gain and both banks initialized before bus mastering.
No hardware legacy SB/FM/MPU/gameport or capture/S/PDIF service is enabled.
DOS hardware remains virtual and reaches this engine through the common
source mixer, without physical port or guest DMA forwarding.

The donor's `playback_open_1` explicitly identifies a 256-frame/48-kHz bank
interval (5.333 ms), matching the existing 256-frame quantum and conservative
following-period prefetch guard. Hardware qualification must still verify
cursor notification cadence and the maximum fetch frontier on every revision.

Activation claims MMIO, routed shared INTx and DMA storage before writes.
Unknown routes, D-states, active native/capture/effect/timer/GPIO/secondary/legacy
users and unreadable firmware state are refused. Firmware size and SHA-256
are checked before upload; every instruction word is read back while DSP is
disabled. Unknown bank sizes fail admission. Codec commands use elapsed
deadlines, readback and a serialized transaction mutex. Explicit mute gates
both the native DAC mixer and AC'97 master/PCM; software alone maps 0–100 gain.

The bank selector is sampled around both voices' DMA-written frame positions.
Unstable selection/positions retry through the existing bounded cursor policy;
invalid or regressing/ambiguous progress cannot retire storage. IRQs acknowledge
only the native bank cause and rearm only a running, uninhibited engine.

Stop clears MODE and its bank interrupt, proves STATUS busy clear, disables DSP, clears and verifies
PCI bus mastering, and verifies stable bank positions before clearing tables
and base registers. Only then may the common backend detach IRQs, restore the
codec and release pins/claims. An unproved stop keeps MMIO, DMA pages and
claims permanently quarantined; repeated shutdown does not reset or free them.
The IRQ callback acknowledges final bank completions until detach even after
READY is revoked; it cannot rearm an inhibited or quarantined engine. Clean
shutdown is idempotent after MMIO revocation.
Silent DMA progress and this stop proof precede READY. Drain allowance remains
the common provisional 20 ms and needs physical measurement.
