# Post-release Doom freeze: physical network IRQ self-masking

The owner's preserved `qemu-visual.log` shows Google requests, a graphical
resolution change and Doom launched from Files, stopping in `R_Init`. A matched
1280×800 snapshot boot and Files launch alone reached a new game and normal exit.
The browser → mode preview/restore → Files launch sequence reproduced a permanent
stall, including a second focused capture. No host crash occurred.

At the stall, the desktop GPU counters and VM yield counters stopped advancing,
although host ticks and VM switches continued. Repeated PCs were `2442:08BB` and
`2442:0873`. The captured IVT identifies both INT 60h and IRQ11 (INT 73h) with
segment 2442: the Crynwr NE2000 packet driver. Disassembly puts 08BB immediately
after its early specific EOIs and before `recv`. The physical PIC had slave
`IRR=08, ISR=08, IMR=86, ELCR=0C` and the master cascade in service. Only 32 KiB
of the resident driver region and 4 KiB of vectors were retained, not a RAM dump.

## Sources checked before implementation

- [QEMU's 8259 implementation](https://github.com/qemu/qemu/blob/master/hw/intc/i8259.c):
  a level request remains asserted until its source clears; EOI clears ISR,
  while IMR masks delivery independently.
- [Intel 8259A datasheet](https://courses.cs.umbc.edu/undergraduate/CMPE310/Spring15/cpatel2/data_sheets/8259.pdf):
  IMR operates on pending requests; level sources must be removed before EOI
  or enabling interrupts to prevent another interrupt.
- Crynwr's original `HEAD.ASM`, `recv_isr`, `maskint`, `unmaskint`, from the
  [pinned upstream source bundle](https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.4/net/crynwr.zip)
  (`PACKET/SOURCES.ZIP`). It explicitly masks its line, sends early specific EOIs,
  calls the device receive handler, then unmasks before restoring its stack.
- [QEMU PS/2](https://github.com/qemu/qemu/blob/master/hw/input/ps2.c) preserves
  the separate input source; a frozen screen alone did not establish an input bug.

## Decision

Preserve the physical mask contract for a real IRQ driver. A guest PIC mask
write may acquire only an IRQ already in physical service and currently unmasked;
ordinary virtual guest masks must not disable the host timer or other hardware.
The driver owns that temporary physical mask until it unmasks the same line.
Keep this ownership global rather than copying it as per-VM PIC state, and block
VM switches during that interval, including the period after early EOI. Preserve
all unrelated physical mask bits. No change to the scheduler quantum is involved.

For physical level IRQs received directly by Jemm in V86 mode, retain upstream
physical reflection instead of queuing a virtual IRQ with an immediate physical
EOI. The existing path pushes the V86 interrupt frame, clears logical IF, and
leaves the physical ISR for the driver's EOI. Input consumed by the existing
keyboard/mouse filter remains on that path. Timer, keyboard, cascade and mouse
are excluded from physical self-mask acquisition.

The full build must compile CVSESSION against the same Jemm build it installs;
it previously selected the separate default Jemm directory for its includes.
The explicit build path now matches, and the Jemm compiler uses the configured
single-job build limit.

## Validation

The old browser retained in the diagnostic image plus the corrected monitor
passed the formerly frozen `R_Init` point in 27 seconds, started a new game and
responded to turning. The physical-mask counter advanced from zero to four
(acquire/release operations), with no mask left held. GPU completions continued
through 12 seconds of gameplay. Its 150 ms automated quit command did not
produce an exit marker; this run is evidence for the IRQ fix, not an exit pass.

The integrated full image then loaded `http://google.com/`, previewed/restored
the display mode and launched Doom from Files. It reached `ST_Init` in 27
seconds, started a new game, turned, showed the quit dialog and exited normally
with 250 ms keys. `[DOSVM] ended` was logged; About reopened on the desktop.
The final physical PIC ISR was zero on both PICs and no physical masks remained
held. This covers the reproduced permanent stall, not a promise of instantaneous
startup or zero frame-time variation. Inputs were sent through QEMU's monitor.

See [captured results and limits](2026-10-04-post-release-fixes/README.md).
All QEMU runs were sequential, capped at 768 MiB/no swap/200% CPU with 256 MiB
of guest RAM and a bounded timeout. No host crash occurred.
