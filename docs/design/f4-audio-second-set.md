# F4 second audio transport set (f4-11)

Research and implementation decisions, 2026-10-13. This extends the approved
[F4 contract](f4-audio-media-contract.md) within
[f4-11](../directives/f4-11-audio-drivers-second-set.md). Host protocol checks
are not hardware or audible-playback qualification.

## Sources and repository comparison

- Linux [ens1370.c, v6.12](https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/pci/ens1370.c), register definitions, playback2_prepare, codec access, SRC and chip_init: ES1370 uses AK4531; ES1371/1373/CT5880 use AC'97 through a SRC safe window. ES1370's DAC2 integer divisor cannot represent 48 kHz exactly; select divisor 30 for exact 44,100 Hz. ES1371 uses the initialized SRC at 48 kHz. CT5880 revisions need the AC'97 reset status bit and a delay. Initialize the disabled SRC RAM before enabling it, including the unused channels' safe parameters; never enable an uninitialized SRC.
- Linux [AK4531 codec](https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/8934827db5403eae57d4537114a9ff88b0a8460f/sound/pci/ak4531_codec.c), reset/power, clock, voice and output mixer: this is a write-only codec, not an Intel NAM interface. Preserve mute in a driver shadow and wait for busy/write completion. Voice code 06h selects 0 dB (the upstream input scale has +12 dB at code zero); enable only voice L/R in output switch 11h bits 3/2. No invented readback or firmware volume restoration.
- QEMU [ES1370, v9.2.0](https://raw.githubusercontent.com/qemu/qemu/v9.2.0/hw/audio/es1370.c): DAC2 frame cursor is in dwords, period counter in stereo frames; non-looping mode is unimplemented. Use a bounded cyclic buffer with cursor/deadline checks and IRQ underrun inhibition, not a fabricated LVI.
- Creative [Sound Blaster Hardware Programming Guide](https://pdos.csail.mit.edu/6.828/2018/readings/hardware/SoundBlaster.pdf), chapters 3, 4, 6: DSP 4.x supports signed stereo 16-bit auto-init DMA, 44.1 kHz maximum, 16-bit word counts, separate IRQ acknowledgement, CT1745 IRQ/DMA assignment registers. Source format remains S16LE stereo 48 kHz by default; physical output on SB16/ES1370 is 44.1 kHz, exposed in caps/status and converted without changing guest DSP rates.
- Linux [x86 DMA interface, v6.12](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/include/asm/dma.h): channels 5–7 use shifted word addresses, even page values, word counts and 128 KiB boundaries. Allocate the SB16 ring below 16 MiB, aligned to its 32 KiB size; serialize flip-flop/count operations with IRQs disabled, touch no unrelated channel, and mask before stop proof.
- Linux [intel8x0.c, v6.12](https://raw.githubusercontent.com/torvalds/linux/v6.12/sound/pci/intel8x0.c), init, chip_init, interrupt and device table: SiS 7012 swaps SR/PICB and counts bytes; its output gate is NABM 4Ch bit 0. nForce/nForce2 retain Intel PCM-out offsets, add separate S/PDIF engines and have the upstream buggy_irq workaround. Ciuki leaves S/PDIF disabled and never claims unrelated shared interrupts as handled. Use BIOS routing and ELCR validation; reject unqualified routing.
- Linux [via82xx.c](https://raw.githubusercontent.com/torvalds/linux/master/sound/pci/via82xx.c), register definitions, channel_reset, format, cursor and chip_init (retrieved 2026-10-13; v6.12 URL unavailable): VIA is **not** Intel NAM/NABM compatible. VT82C686 uses its type register and table-pointer cursor; VT8233/VT8235 use DXS format/rate, index/count and attenuation registers. Select fixed 48 kHz, avoiding DXS variable-rate board quirks and the Award BIOS MC97/VRA workaround. Reject active sibling engines before AC-link reset; do not take over their state. VT8233A's lack of DXS uses its multi-channel block in stereo mode. VT8233 may not implement PCI BME: engine stop/cursor proof is authoritative.
- Linux [PCI IRQ routing](https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/pci/irq.c): bounded $PIR scanning, version/size/checksum, router identity and pin links. Keep firmware routes unchanged; require ELCR level and actual router register agreement for supported routers. PIIX QEMU routing retains the existing swizzle formula.

The repository had one driver with direct calls and state reads from
`audio_dev.c`, Device Manager and probes. Add a private backend interface;
keep AC'97's existing T23 bring-up, silent proof, BDL publication and shutdown.
Native conversion uses the effective rate. DOS PCM/OPL keep their 48 kHz
virtual clock, with a bounded final rate converter for slower transports.
All engines own pinned storage, have elapsed deadlines and prove silent
cursor progress and stop before `ready`. Failed stop retains allocations and
quarantines claims. Safe mode performs no optional audio activation. Backend
selection never hides a failed detected controller by activating another.

SB16 discovery is limited to the directed 220h port and the firmware-assigned
CT1745 IRQ/DMA registers (IRQ5, DMA1/5); it does not reprogram assignments or
perform ISA isolation. The frozen loader handoff has no PnP BIOS node list;
other bases/assignments require a separately supplied firmware resource path.

## Resource and reporting details

The new cyclic rings each pin 32 KiB (eight pages); VIA additionally pins one
page for its descriptor table. ISA allocation uses the existing DMA zone,
with an eight-page alignment and no fallback above 16 MiB. Claims include the
required I/O windows, IRQ, owned DMA channels and pinned physical extents.
Codec/SRC transactions stay in thread context. Start, stop and hardware mute
share a private transaction mutex, so SB16 multi-byte DSP commands cannot
interleave with volume operations across a bounded busy wait. Activation has a seven-second
elapsed deadline; stop has a separate 500 ms budget. No reset, publication or
release follows permanent quarantine. New PCI transports remain bus-master
disabled on clean release; write-only AK4531 is muted rather than claiming to
restore unreadable firmware registers.

SiS/nForce retain their I/O aliases, with a 128-byte transport window covering
the SiS output gate and nForce S/PDIF block. MMIO-only configurations are
refused. SiS routes use the documented 85C503 link registers; unknown on-chip
link mappings are refused. nForce requires a valid firmware $PIR entry, the
BIOS-assigned pin/IRQ and ELCR level; no undocumented router write is made.
QEMU ES1370 retains PIIX readback/swizzle validation. VIA START may become active asynchronously; zero-count boundaries retry a
coherent snapshot without consuming client frames, with a two-millisecond
runtime limit before failure. An unreadable running firmware DMA state
(IO decode disabled with BME set, or VIA without decode) is refused before
controller writes. Hardware IRQ continuity and analog routing still need
lead evidence.

Device Manager uses the PCI hwids names, adds the three missing AudioPCI names
with upstream provenance and reports the actual bound transport/resources.
Its frozen sixteen-slot driver ABI retains the existing audio slot for the
selected driver; no record layout changes. SB16 is an ISA record with its
observed DSP version and assigned IRQ/DMA leases. The caps transport IDs and
rate/cursor units are recorded in [execution-abi.md](execution-abi.md).
The active eight-period lead is 46.44 ms at 44.1 kHz (5.805 ms per period),
versus 42.667 ms at 48 kHz; these remain provisional until timing qualification.
