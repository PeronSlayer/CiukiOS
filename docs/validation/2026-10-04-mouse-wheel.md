# PS/2 mouse-wheel support design

## Upstream behavior

The guest presents a PS/2 auxiliary device through the emulated 8042. The
ordinary mouse packet is three bytes and the IntelliMouse wheel extension is
enabled by setting sample rates 200, 100, 80 in sequence. The device then
reports ID `03h`; enabled wheel packets have a fourth signed Z byte. QEMU's
upstream `ps2.c` implements this sequence and emits four-byte packets for
IMPS/2. Its wheel-up event produces negative Z, so a positive packet Z means
wheel-down. Linux's PS/2 parser decodes the IMPS/2 fourth byte as signed wheel
motion. The QEMU HMP `mouse_move` command uses the opposite sign convention:
positive `dz` selects wheel-up, negative `dz` selects wheel-down. Sources:
[QEMU HMP mouse command implementation](https://qemu.googlesource.com/qemu/+/03ae4133ab8675d4c67e6fdc8032de7c53a89514/monitor.c),
[QEMU PS/2 implementation](https://github.com/qemu/qemu/blob/master/hw/input/ps2.c),
[Linux PS/2 parser](https://github.com/torvalds/linux/blob/master/drivers/input/mouse/psmouse-base.c).

The FreeDOS CuteMouse upstream Wheel API 1.0 defines INT 33h/AX=0011h as its
capability query (AX=`574Dh`, CX bit 0 set when a wheel is available). Its
updated INT 33h/AX=0003h returns an accumulated signed wheel count in BH;
positive means downward motion, and that call clears the counter. Source:
[FreeDOS mouse driver wheelapi.txt](https://github.com/FDOS/mouse/blob/master/wheelapi.txt).

## CiukiOS implementation decision

Keep both PS/2 paths protocol-compatible. The physical kernel initializes the
mouse with defaults, negotiates rates 200/100/80, queries F2, and switches its
IRQ framing to four bytes only when the device returns ID 3. The virtual 8042
starts at ID 0 with three-byte packets, recognizes the same rate sequence, and
returns ID 3 only after it completes. F6 and FF reset the virtual device to ID
0. Both paths retain the low three button bits, including middle-button state.
The virtual controller checks available queue capacity before adding a whole
three- or four-byte packet, so it cannot strand the guest midway through one.

Wheel values are signed IntelliMouse notches with positive meaning downward.
`DEV_MOUSE` passes its signed wheel value in ESI; the model turns it into the
fourth byte when ID 3 is active. The physical kernel's INT 33h path implements
the FreeDOS Wheel API: AX=0011h reports availability, AX=0003h returns and
clears a signed 8-bit accumulated count in BH, and AX=0005h/0006h with BX=-1
returns and clears the signed 16-bit count plus the coordinates of the last
wheel movement. INT 33h callback mask bit 7 reports wheel motion in BH. The
physical PS/2 BIOS callback receives signed Z for a four-byte device and zero
for a standard three-byte mouse.

The desktop dispatches `EV_WHEEL` with `a` as signed steps (down positive) and
`b/c` as client coordinates. DOSVM maps that event into the same `DEV_MOUSE`
wheel argument. QEMU emulated-controller runtime behavior is covered below;
physical-machine mouse behavior and virtual DOS-client INT 33h queries remain
unverified.

## Validation scope

Host peripheral-model regression passed 1,285 assertions under ASan/UBSan, and
the target Watcom compile/link passed. Runtime `ui-4` passed the Files wheel
gate through QEMU's emulated PS/2 controller: the kernel negotiated ID 03h and
four-byte packets; the signed wheel event reached Files as `+1` with target 8
and result 250; rendered pixels matched the expected before, scrolled-down,
and scrolled-back states. The test used QEMU monitor input, so a physical host
mouse was not tested.

The earlier `ui-3` failure was in the test direction: its HMP `dz=+3` input
means wheel-up. Files was already at the top, so unchanged pixels were
expected. The harness now sends negative `dz` for wheel-down; this was confirmed
by `ui-4`. This design does not change floppy build/profile policy.
