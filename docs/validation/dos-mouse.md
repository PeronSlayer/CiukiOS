# DOS mouse delivery (fx-dosmouse)

Research checked 2026-10-26 before implementation. This record covers host
replays; hardware and native display qualification belong to the lead.

## Sources and implementation comparison

- [FreeDOS kernel setup_int_vectors](https://github.com/FDOS/kernel/blob/master/kernel/main.c)
  matches the pinned fork's `kernel/main.c`: DOS overwrites vectors
  23h..3Fh with `empty_handler`, including INT 33h. The boot ROM mouse
  entry is therefore lost. [RBIL reset/presence check](https://fd.lod.bz/rbil/interrup/io_mouse/330000.html)
  requires callers to reject a null vector or an IRET entry. The Warcraft
  replay reached the menu with zero calls to the VMM's mouse BIOS.
  Restore the synthetic INT 33h entry once at the first valid DOSREADY
  boundary (after CONFIG/AUTOEXEC), only for null/IRET vectors. Preserve
  any real mouse TSR and do not reclaim later guest vector changes.
- [RBIL INT 33h index](https://fd.lod.bz/rbil/zint/index_33.html), especially
  [position](https://www.delorie.com/djgpp/doc/rbinter/id/78/59.html),
  [horizontal range](https://www.delorie.com/djgpp/doc/rbinter/id/82/59.html),
  [vertical range](https://www.delorie.com/djgpp/doc/rbinter/id/83/59.html),
  [handler installation](https://www.delorie.com/djgpp/doc/rbinter/id/87/59.html),
  and [handler exchange](https://fd.lod.bz/rbil/interrup/io_mouse/330014.html).
  Cell rounding applies to text coordinates. The existing implementation
  rounds every position, range endpoint and callback coordinate to eight
  pixels, including VBE graphics. Graphics must retain pixel precision.
- [Open Watcom DOS/4GW guide](https://open-watcom.github.io/open-watcom-1.9/pguide.html),
  “How Can I Use the Mouse Interrupt (0x33) with DOS/4GW?” and “DOS, BIOS,
  and Mouse Services”, 6c: the extender translates INT 33h and a protected
  ES:EDX handler. The VMM must deliver the resulting real-mode handler
  while protected-mode application code runs; it must not reinterpret an
  extender-provided real-mode address as a protected-mode selector.
- [DPMI 0303h](https://www.delorie.com/djgpp/doc/dpmi/api/310303.html) and
  [callback procedure contract](https://www.delorie.com/djgpp/doc/dpmi/ch4.6.html):
  the real-mode FAR CALL enters a protected-mode procedure with interrupts
  disabled, DS:ESI addressing the real stack and ES:EDI the 50-byte register
  structure. Its IRET returns using the structure's CS:IP and SS:SP. The
  existing DPMI callback machinery provides this bridge, but mouse delivery
  is explicitly gated on `!frame_pm` in the monitor.
- [Microsoft Mouse Programmer's Reference (1989)](https://bitsavers.trailing-edge.com/pdf/microsoft/mouse/Microsoft_Mouse_Programmers_Reference_1989.pdf):
  INT 33h 0Fh controls mickeys per eight pixels; raw motion counters remain
  separate from cursor coordinates. Sensitivity services 1Ah/1Bh were absent.
- Microsoft Windows 98 Resource Kit, local read-only extraction,
  `wrkc30_3.htm` (“USB/HID”), `wrkc28_3.htm` (HID client table) and
  `wrkc05_3.htm` (VMM32): Mouhid adapts HID reports to VMD's legacy data
  format; serial/PS/2 minidrivers feed the same VMD. CiukiOS already merges
  PS/2 and USB into its input path and routes exclusive full-screen input
  before desktop processing. This is Windows 9x VMM/VMD behavior, not NTVDM.

## Delivery decision

Use the existing checked `dpmi_to_real_call` continuation, also used by IPX,
for a mouse FAR callback interrupting protected-mode code. Keep the mouse
return stub's stack/depth checks and restore the full suspended DPMI frame.
Defer events during virtual IF clear, STI/MOV SS shadows, an active mouse
callback, or temporary DPMI stack exhaustion. Pending mouse work must prevent
the monitor's lean return from bypassing dispatch and must wake a halted VM.

No public ABI or SDK changes are needed.

The actual Warcraft II trace installs mask 001Fh at F000:1104 (the extender's
DPMI callback), and sets the 640x480 VBE screen's mouse range to 0..1278 by
0..479. Absolute window motion must therefore cover this entire virtual
range, rather than add window-pixel deltas to cursor coordinates. Preserve
raw deltas separately for INT33 0Bh, used by first-person games. Relative
full-screen packets continue through the mickey/pixel ratio. This agrees
with upstream [DOSBox-X Mouse_CursorMoved](https://github.com/joncampbell123/dosbox-x/blob/master/src/ints/mouse.cpp)'s
separation of absolute integration and relative motion.

## Audit boundaries

- Cursor visibility: the reset hide count and nested hide/show behavior are
  exercised through INT33. The DOS Box program displays the text cursor.
  Warcraft II installs its callback without calling the driver's show or
  graphics-cursor services in this replay; its visible game pointer is
  therefore inferred to be game-rendered. No new claim of a graphics
  cursor renderer is made.
- [Sensitivity 1Ah](https://fd.lod.bz/rbil/interrup/io_mouse/33001a.html)
  and [readback 1Bh](https://fd.lod.bz/rbil/interrup/io_mouse/33001b.html)
  remain unsupported, returning CF through the existing unadvertised
  extension path. Neither the Warcraft II initialization nor the DOOM or
  Wolfenstein replays calls 1Ah. The Microsoft reference describes a
  1..100 sensitivity scale separate from 0Fh ratios; treating its parameters
  as 0Fh ratios would be incorrect. Exact sensitivity/acceleration emulation
  is an outstanding compatibility item for the lead, not a requirement for
  the observed game's callback fix.
- [Wolfenstein input manager](https://github.com/id-Software/wolf3d/blob/master/WOLFSRC/ID_IN.C)
  and [Quake DOS input](https://github.com/id-Software/Quake/blob/master/WinQuake/in_dos.c)
  use reset, button polling and motion polling. Both 16/32-bit production
  DPMI replays check this profile, including unclipped raw counters. Actual
  Quake stops at unsupported x87 instructions in libx86emu before its
  mouse startup; no Quake end-to-end mouse result is claimed.
- PS/2/USB: the T23 evidence reports two PS/2 input devices and auxiliary
  protocol ID 0. The earlier captures also report one USB input device.
  No source-specific bypass is added: both continue to feed the shared
  native input path, with window coordinates integrated absolutely and
  exclusive full-screen packets handled relatively.

## Host evidence

`CIUKI_VM_DOSMOUSE=1 build/host/dosvm_test` adds 16/32-bit production
DPMI 0300h INT33 initialization, 0303h callbacks and 000Ch/0014h handler
installation/exchange. It covers checked returns, register restoration,
mouse-only virtual-IF recovery demand, shadows, temporary stack exhaustion,
window range endpoints, relative full-screen input and raw-counter polling.
The full DOS VM suite includes it.

`DOSX_MOUSE_GAMES=1 bash tests/host/dos/run_dos_extenders.sh` runs the real
unified guest and installed DOS binaries. Warcraft II delivers four DPMI
callbacks (two movements, left press/release), finishing at virtual position
274,121 with no active/pending callback and no DPMI fault. DOOM reads 37,21
raw mickeys and the press; Wolfenstein's menu reads position 137,50 and the
press. DOSMOUSE.COM completes its movement/click-driven checks and exits to
the prompt. Quake is explicitly reported blocked by the CPU runner's x87
limit. This is host evidence, not a physical display or T23 result.

The default-leak-detection VM suite reaches its assertion PASS before
LeakSanitizer fails because ptrace is unavailable in the sandbox. Do not
interpret that process's exit status as green; rerun outside this sandbox.
The exact commands, statuses and toolchain limitations are in the implementer
report at `build/codex/fx-dosmouse-report.md`.
