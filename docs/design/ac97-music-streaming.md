# AC'97 music streaming queue

## Hardware contract

The supported ICH-family AC'97 engine uses an 8-byte Buffer Descriptor List
(BDL), with at most 32 descriptors. The Intel ICH7 Family Datasheet identifies
the PCM-out registers at NABMBAR offsets BDBAR `+10h`, CIV `+14h`, LVI `+15h`,
status `+16h`, PICB `+18h`, and control `+1Bh`. CIV advances for each processed
descriptor and wraps modulo 32. Software updates LVI whenever it adds a
prepared descriptor. The Run/Pause bit pauses while retaining stream state;
the reset bit is only valid while the run bit is clear. These rules come from
the original [Intel ICH7 Family Datasheet, sections 16.2.1–16.2.7](https://www.intel.com/content/dam/doc/datasheet/i-o-controller-hub-7-datasheet.pdf)
(BDL/CIV/LVI: pp. 635–636; PICB: p. 638; control: p. 639).
The original [AC '97 Component Specification 2.2](https://www.alsa-project.org/files/pub/datasheets/intel/ac97r22.pdf)
defines master-volume attenuation in left-channel bits 12:8, right-channel
bits 4:0, and mute at bit 15. The driver maps the UI's 0–100 range to those
fields and restores the saved register on close.

The previous two-descriptor implementation was not a streaming ring: it set
LVI to 1 once and never appended descriptors, so the controller stopped after
64 KiB of stereo PCM. The replacement uses 32 descriptors of 2,048 bytes each
(512 frames at 48 kHz stereo signed 16-bit), covering the existing 64 KiB SFX
buffer. Foreground polling reads CIV and PICB to determine which descriptor
slots are safe to refill, advances LVI only after a complete PCM block is
written, and computes played/queued counters across modulo-32 wrap. No audio
IRQ handler is installed. Seek and close pause/reset the bus master before
reusing or releasing buffer state; pause uses the hardware's retained-state
pause bit.

The PCM copy follows the x86 string-instruction contract: `REP MOVSB` reads
from `DS:SI` and writes to `ES:DI`. The [Intel Software Developer's Manual,
Volume 2, MOVS](https://cdrdv2-public.intel.com/835757/325383-sdm-vol-2abcd.pdf)
specifies those fixed source and destination registers. The driver therefore
maps the caller buffer to DS and the AC'97 ring to ES, and uses CS overrides
for driver state while DS names caller memory. This avoids reversed PCM
transfers and accidental use of caller memory for driver counters.

Each BDL slot points to `ring_base + slot * 2048`; the 32 slots exactly span
the 64 KiB ring. The BDL length is a count of 16-bit samples, so a full stereo
slot is 1,024 samples and a final partial slot is twice its remaining frame
count. These address and length calculations follow the ICH7 BDL format cited
above.

## Validation

The include is checked by NASM syntax assembly and by the full-image SFX driver
pack step. Runtime testing belongs to the sequential full HDD audio profile;
the standalone floppy profile is not part of this project's active build.
