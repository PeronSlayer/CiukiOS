# Native Sound Blaster event backend

`src/com/ui_sound_sb.inc` adds asynchronous unsigned 8-bit mono playback at
8000 Hz to the foreground-only SFX module. Error, information, warning and
confirmation samples use the packaged `.SB` files; no executable, text console
or PC-speaker fallback is started for an event.

The backend supports master-PIC IRQ5/7 and ISA DMA0/1/3. SB16 resources are read
from mixer registers80h/81h. Older SB Pro-compatible DSPs require matching
`BLASTER` base/IRQ/DMA settings; unsupported routing is reported unavailable.
No routing register is rewritten and the DSP is never reset during discovery.

A 5376-byte DOS allocation contains a maximum2688-byte sample. Its transfer
start is moved to the following physical64KiB boundary when necessary. The
entire sample stays within both the allocation and one ISA DMA page.

The player installs no interrupt handlers. During its short DMA transfer, only
the configured sound IRQ is held masked. At completion the PIC poll command
temporarily exposes only that line, acknowledges it, and sends a specific EOI.
Other IRQ requests remain pending; the original PIC masks and mixer values are
restored. Release stops/masks the selected DMA channel before freeing memory.
Polling is bounded and never opens a file; sample loading occurs in foreground
before starting the transfer.

The hardware reference for PIC polling is Intel's
[8259A datasheet](https://www.cs.utexas.edu/~dahlin/Classes/439/ref/hardware/8259A.pdf).
The version, sample-rate, speaker and single-cycle playback commands follow
Creative's [Sound Blaster Programming Guide](https://cecas.clemson.edu/~ahoover/ece468/labs/SoundBlasterProgrammingGuide.pdf)
and the existing CiukiOS SBSTART implementation.

## Ownership and emulator limits

Pending/in-service sound IRQs, changing DMA counts, or a speaker-on DSP cause
the backend to refuse ownership. ISA provides no portable way to identify a
third-party paused, speaker-off DMA owner; the host must release transient
application audio drivers before returning to the desktop.

QEMU's [SB16 command implementation](https://github.com/qemu/qemu/blob/master/hw/audio/sb16.c)
does not implement the DSP D8 speaker-status query. The backend permits that
timeout only for DSP4.05 under recognized KVM/TCG CPUID identification, and then
requires the selected channel to be masked using QEMU's documented-in-source
[DMA mask read extension](https://github.com/qemu/qemu/blob/master/hw/dma/i8257.c).
This extension is never read on unidentified or physical hardware. Physical
cards that cannot answer the ownership query are refused.

## Validation

`build/full/ui-sound-sb-2026-09-26/fourth/report.json` captures actual QEMU SB16
PCM from the graphical Play control: RMS8953.79 in the captured event interval.
The muted interval adds no PCM data, and all sound controls remain graphical.
The test uses Pentium III,128MiB and KVM; it is not T23/E500 qualification.
The exercised SB16 routing is base220h, IRQ7, DMA1. Other supported routing
branches and physical SB Pro hardware have not been runtime-qualified here.

The failed initial runs are retained: one exposed the harness assuming a WAV
file existed before QEMU opened its first SB voice; another correctly refused
the unsupported D8 query before the explicit emulator accommodation was added.
The harness now locates the WAV data chunk before inspecting sample values.

`scripts/qemu_test_system_sound_handoff.py` separately checks IVT/mask
restoration and native desktop → COMDEMO → native desktop handoff.
`handoff-r2/report.json` passes: all1024 IVT bytes and both PIC masks match,
IRQ7 is neither pending nor in service after playback, the audio module is
released before DOS, and COMDEMO plus subsequent native input work.
