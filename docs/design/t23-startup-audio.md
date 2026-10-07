# Native startup audio on the ThinkPad T23

The T23 connected on 2026-10-07 exposes Intel ICH3-M AC'97 (`8086:2485`,
IBM subsystem vendor `1014`) and ships a 245,760-byte `SYSTEM/BOOT.PCM`.
The absence of a startup melody on that machine is an observed failure.
No captured native codec/DMA log from that boot currently establishes which
initialization or playback stage failed.

## Sources checked before implementation

- [Intel 82801CAM ICH3-M datasheet](https://datasheet.lcsc.com/datasheet/pdf/7d520883a0f21f1845e4892c5243848a.pdf),
  sections 13.2.1–13.2.7: a maximum of 32 eight-byte descriptors, modulo-32
  current/prefetched indexes, software publication through LVI, status bits,
  and reset only with Run/Pause cleared. In section 13.2.4, PCM-out LVBCI
  reports the final memory fetch; it does **not** prove the samples have
  already traversed the AC-link or reached the speakers.
- [Upstream Linux `intel8x0.c`](https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/056e065a6b6e01ab54bb9770c0d5a15350e571e2/sound/pci/intel8x0.c),
  `snd_intel8x0_setup_periods`, `snd_intel8x0_update`, and
  `snd_intel8x0_pcm_trigger`: DMA progress follows CIV and publishing LVI
  advances the prepared buffer queue. Stopping verifies DMA has halted.
- [Cirrus Logic CS4299 datasheet](https://cdn-docs.av-iq.com/dataSheet/CS4299-BQZ_PDF.pdf):
  codec master/headphone/PCM mute registers and register 26h power readiness
  are separate from bus-master DMA progress. Native driver initialization
  already checks the playback-ready mask `000Eh` and unmutes all three
  outputs. The implementation preserves the codec initialization and the
  existing opt-in policy for raw ThinkPad EC writes.

## Repository defect and decision

The native event driver reserves a 64 KiB conventional-memory DMA buffer.
Its previous startup path then attempted to allocate the **whole startup
track** in a second conventional block, making the current T23 melody require
another 245,760 contiguous bytes. Allocation failure returned result 6 with
no native `AUDIO.LOG` record. Successful emulation with more available memory
did not establish that the second allocation could succeed after the real
desktop loaded. This allocation is a concrete defect, but it is not evidence
that codec reset, hardware mute, or routing cannot also fail on the T23.

Startup now reads the file into the existing 64 KiB ring, with one 2 KiB
physical slot per descriptor. At most 31 slots are published: the final
slot is a guard. Initial priming reads at most 62 KiB, and each foreground
poll reads at most eight slots (16 KiB). File size remains bounded to
524,288 bytes, nonempty and aligned to a complete stereo 16-bit frame.
No startup-sized DOS allocation or extra PCM buffer is introduced.

Software retires descriptors from measured CIV advancement before reusing
their PCM slot or descriptor. A DOS read and the complete descriptor entry
finish before LVI publication. Current, queued and prefetched buffers are
immutable. If a delayed foreground poll finds DCH plus LVBCI, all published
bytes have been fetched. Extending LVI with the next prepared slot continues
the retained engine state and file position without replaying earlier data
or resetting samples still held in the FIFO. There are no IRQ hooks.

At file EOF the driver waits at least one BIOS tick after the final fetch
before cleanup, allowing the output FIFO to drain. The BIOS low-word wrap
is supported. Stop, read failure, FIFO error, reset failure and timeout all
use the existing verified DMA cleanup before closing the file or making the
ring available to an event or music owner. If DMA halt or BME-off cannot be
proved, the existing fatal path retains the allocations. The 90-tick track
deadline remains bounded. The music ownership ABI is unchanged.

## Evidence recorded by the native driver

The loaded SFX instance appends one initialization record, at most one start
record, and at most one terminal record to `SYSTEM/AUDIO.LOG`. Ordinary
polls and painting perform no log I/O. Logging preserves the caller flags,
registers and playback result, and a logging failure does not cancel sound.

Each native line contains controller/device and subsystem vendor, actual
codec ID and power readiness, master/headphone/PCM readbacks, ring segment,
largest available DOS block if the ring allocation failed, total/read/fetched
bytes, queue resumptions, and observed PCM-out SR/CIV. Mixer readback occurs
while PCI I/O remains enabled, before restoring its original command word.
All numeric values are hexadecimal; `largest` is in DOS paragraphs. A zero
largest field means no failed-allocation measurement was needed. The
`played` field counts DMA-fetched bytes, not externally measured audio.

Events are `I` (native initialization), `S` (RUN issued), `E` (EOF after DMA
completion and FIFO drain delay), `F` (failure), and `X` (explicit stop).
Result values retain the driver ABI: 0 success/idle, 1 playing, 2 muted,
3 unavailable, 4 read error, 5 device busy, 6 allocation failure, 7 DMA error.

| Stage | Native action |
| --- | --- |
| 1 | PCI discovery |
| 2 | PCI I/O enable |
| 3 | All AC'97 streams idle |
| 4 | Primary codec ready |
| 5 | Codec playback preparation/power readiness |
| 6 | Existing 64 KiB DMA allocation |
| 7 | Initialization complete |
| 8 | Legacy/Sound Blaster fallback after no supported ICH |
| 10 | Open startup file |
| 11 | Validate size and rewind |
| 13 | Startup PCI I/O enable |
| 14 | Startup bus-master enable |
| 15 | Initial PCM-out engine reset |
| 16 | Initial ring priming |
| 17 | Initial RUN |
| 18 | Foreground DMA progress/completion |

An `S` record proves submission only. An `E` record with nonzero fetched
bytes and hardware SR/CIV proves the native driver completed the memory
transfer; audible speaker output still requires the physical hardware test.

## Focused verification

`scripts/test_startup_audio.py` compiles the production NASM init, event,
poll/stop/release and startup routines, then executes them in Unicorn with
mock DOS files/allocations and ICH registers. The mocks protect published
PCM and BDL slots, compare consumed PCM byte-for-byte, and require DMA cleanup
before file close or ring free. Twenty cases cover:

- Exact 245,760-byte startup playback using only the one 64 KiB allocation;
  wraparound, a partial final descriptor, 524,288-byte maximum, DMA progress
  during reads, and deliberate queue exhaustion followed by resumption.
- Event playback after startup EOF and final ring release with canaries intact.
- EOF FIFO drain delay, repeated polling without a tick, and BIOS tick wrap.
- Explicit stop, read/FIFO/reset/timeout/open/size/empty-file failures.
- PCI, I/O, stream-busy, codec-ready, codec-prepare and allocation failures,
  including bounded native failure logging and largest DOS allocation evidence.

Run with:

```sh
uv run --with unicorn python scripts/test_startup_audio.py --output build/full/t23-vbe-fix/native-startup-stream
nasm -DUI_SFX_DRIVER=1 -f bin src/com/ac97init.asm -o build/full/t23-vbe-fix/native-startup-stream/SFX.DRV
```

These CPU-level tests validate the allocation and descriptor lifetimes. They
do not exercise physical codec timing, the ThinkPad mute gate, or speakers.
The integrated full-image/QEMU capture and fresh T23 `AUDIO.LOG` are separate
validation gates owned by the integration task.
