# Desktop volume indicator AC'97 access

## Hardware contract and decision

The top-bar volume control reads the Intel ICH-family AC'97 primary codec's
master-volume register at NAMBAR+02h. Intel's [ICH7 AC'97 Programmer's
Reference Manual](https://www.intel.la/content/dam/doc/manual/io-controller-hub-7-hd-audio-ac97-manual.pdf)
documents PCI Command.IOSE at config offset 04h, Native Audio Mixer and Bus
Master I/O BARs at 10h and 14h, and the codec access semaphore at NABMBAR+34h.
An idle CAS read returns zero and claims one codec I/O; completion of that
codec access clears the semaphore. PCI Command.BME is bit 2, separate from
IOSE bit 0.

The desktop previously read NAMBAR directly after the startup-effects driver
restored the PCI command word. With I/O decode disabled, an `IN` can return
FFFFh, which was interpreted as mute because bit 15 is set. That value is no
evidence of a muted codec.

`top_mixer_access` now reads the saved PCI command word for every query or
write. If IOSE is clear, it enables only bit 0, checks that it stuck, performs
a bounded CAS-protected codec transaction, and restores the original command
word on every exit. A volume write takes a second CAS claim for a posted
master-register readback; the UI adopts the readback value only when it is
valid. Failed PCI, CAS, or mixer reads leave the volume unavailable, never
muted. The helper does not enable bus mastering, reset the codec, or modify
the extended-control register. Mixer transactions run on the foreground
desktop path with no concurrent desktop driver call; an already-running audio
DMA engine can continue because BME is preserved. The PCI I/O decode bit is
restored immediately after each codec transaction.

The ICH7 reference is used for the Intel controller register contract shared
by the supported ICH-family IDs. It establishes register access semantics;
it does not establish the state of any physical codec without a successful
runtime readback.
