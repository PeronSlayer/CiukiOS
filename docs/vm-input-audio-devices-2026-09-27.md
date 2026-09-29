# Monitored-session input and sound devices — 27 September 2026

Status (27 September 2026, evening): the peripheral model is **attached to
real-mode (V86) DOS programs** through CVSESSION and Jemm's negotiated V86
interrupt profile. An unmodified DOS program that hooks INT 9 and reads port
60h, uses INT 33h, or programs ISA DMA, a Sound Blaster 16 DSP and OPL ports
is served by this model. Its PCM is streamed to the machine's ICH AC'97. In
the native DOS window, keyboard and mouse follow desktop focus. This is QEMU
evidence only. Protected-mode (DPMI/HDPMI) clients are attached since
28 September 2026; see [DOS/4GW games in the DOS window](vm-dpmi-window-devices-2026-09-28.md).

The public peripheral namespace is **`cvgp_` / `CVGP_`**. The earlier `cvp_` /
`CVP_` names collided with the VGA presenter; the
[pre-fix compile failure](validation/2026-09-27-integration/device-link-before.json)
and [corrected combined link](validation/2026-09-27-integration/device-link-after.json)
preserve that regression.

## Attachment to the monitored session

`session_devices.c` (OpenWatcom, inside CVSESSION.DLL) owns one model
instance. `session_devices.inc` is its ring-0 glue: port trap handlers
(`Install_IO_Handler`), a 16 KiB private C stack, FNSAVE/FRSTOR around the
C/C++ code and the calls into Jemm's profile service. The operations are in
`src/vm/session_devices_abi.inc`:

| Op | Name | Contract |
| --- | --- | --- |
| 30h | `DEV_BEGIN` | EBX = capabilities (1 keyboard, 2 mouse, 8 DMA/SB16, 10h OPL), ECX bit 0 = stream PCM, EDX = TSC kHz. Requires an active session **and** an active IF profile. Returns granted capabilities and the audio status (0 none, 1 running, 2 no AC'97, 3 no usable rate) |
| 31h | `DEV_END` | Removes traps, then the profile hooks, ends the model, drains a pending 8042 byte and frees the audio pages |
| 32h | `DEV_FOCUS` | BX = 1/0. Losing focus releases every held key and mouse button |
| 33h | `DEV_STATE` | 128-byte report (magic `CVDV`): counters, IRQs, audio, last error |
| 34h | `DEV_IO` | Port bridge for the HDPMI adapter: BX port, CL width, CH bit 0 write, EDX value; returns EDX and ESI = device IRQ lines held for the protected-mode client (the first call claims them) |
| 35h | `DEV_KEY` | Host-originated key for the focused guest (the window's close Esc) |
| 36h | `DEV_PM_IRQ` | CL = 2: accept the highest held line the virtual PIC allows now (EBX line, EDX vector); CL = 0: release after the client exits (see the [2026-09-28 record](vm-dpmi-window-devices-2026-09-28.md)) |

`VM_OP_QUERY` reports `VM_CAP_GUEST_INPUT` (1000h) and `VM_CAP_GUEST_AUDIO`
(2000h) only when the virtual-IF profile is available. The session refuses to
unload while devices are active, and rollback ends them first.

**Interrupts.** The model's 8259 is only an aggregator in auto-EOI mode. Each
IRQ it raises (keyboard 1, mouse 12, SB 5/7) is forwarded to the profile's
single virtual PIC (service function 3). The guest's EOIs go to that PIC.
The model's PIT channel 0 is stopped and IRQ0/IRQ2 are never forwarded.

**Keyboard and mouse capture.** The profile's IRQ1/IRQ12 filter hands each
physical 8042 byte to the model instead of reflecting it (focused: to the
guest; unfocused: dropped with held keys released). A read of 60h/64h with
nothing queued pulls the physical controller lazily, and the poll drains it
periodically. The KBC's A20 commands (D0/D1/DD/DF/FF) are emulated. Extended
E0/E1 sequences are tracked so fake shifts do not become keys.

**Trapped ports.** 60h and 64h; ISA DMA 00h–0Fh, pages 81h–83h/87h/89h–8Bh/8Fh,
16-bit C0h–DEh; SB16 220h–22Fh; OPL 388h–38Bh (with OPL only: SB aliases
220h/221h/228h/229h). DMA channel 2 (04h, 05h, 81h, and 0Ah/0Bh addressing
channel 2) stays physical because the host BIOS drives the floppy through it.
PIC and PIT ports are not trapped: the profile owns them. DMA reads of guest
memory are limited to below A0000h.

**Audio output.** `DEV_BEGIN` maps 33 pages: an AC'97 buffer descriptor list
and 32 buffers of 1,024 stereo frames. It enables variable-rate audio at
44,100 Hz and keeps eight buffers queued ahead of the codec. The poll renders
at most every 4 ms and the poll itself runs at most every 0.5 ms (TSC). Mixing
every monitor exit had made the window unusably slow.

**OPL.** DBOPL runs in ring 0 inside CVSESSION. `session_opl.cpp` is compiled
by clang (`i686-pc-windows-gnu`, freestanding COFF) with shim headers from
`src/vm/opl_shim/`, placement-new static storage and its own x87 `pow`, `sin`
and `log10`. The build extracts the three DBOPL files from the verified
VSBHDA archive. **CVSESSION.DLL is therefore a combined GPL work** (DBOPL is
GPL-2.0-or-later; CiukiOS is GPLv2). Its complete source is in this
repository.

## DOS window integration

In VGA session mode DOSWIN binds a scheduler descriptor, so the scheduler and
profile are armed. It then calls `DEV_BEGIN` for keyboard, mouse, SB16 and
OPL with AC'97 output. Header fields 214–231 of `dos_window_abi.inc` report
the result.

- **Keyboard beyond INT 16h:** keys reach the guest as raw Set-1 bytes on
  virtual IRQ1, so INT 9 hooks and port-60h readers work. Desktop focus changes
  call `DEV_FOCUS`. The window's close button sends Esc via `DEV_KEY` (make and
  break) instead of inserting a BIOS key. On session end, keys left in the
  shared BIOS ring are discarded so they cannot reach the desktop.
- **Mouse:** a virtual INT 33h: reset (00h/21h), show/hide, position and
  buttons, set position, press/release counters, ranges, mickeys, ratio,
  event handler (0Ch, swap 14h), state size, sensitivity, page and version. The host pointer
  maps onto the client area and the guest mode's range. The guest pointer
  moves only on real host pointer changes while the window is focused and the
  pointer is inside the client. The event handler is far-called on the host
  tick.
- **Resizable window:** see [the video session record](vm-video-session-2026-09-27.md#resizable-dos-window).

## Ownership contract

`guest_peripherals.c` contains no IN/OUT instruction, BIOS call, DOS call,
allocator, firmware transition or physical audio driver. `cvgp_begin` grants one
nonzero session generation a declared subset of keyboard, mouse, PIC/PIT,
DMA/Sound Blaster and OPL capabilities. A second owner and stale generations
are rejected. `cvgp_end` stops DMA/SB activity, clears queued/pressed input,
resets OPL, drops callbacks and invalidates the generation on both normal and
error exits.

The native host continues to own the physical keyboard, mouse, PIC, timers and
audio device. It converts selected native events to `cvgp_key_event`,
`cvgp_mouse_event` and `cvgp_set_focus`; it consumes mixed PCM from
`cvgp_render_audio`. The model never passes an unsupported guest operation to
physical hardware. `CVGP_IO_NOT_CLAIMED` means only that this model did not
register the port. During a monitored window it is **not** permission to expose
that port: the session broker must reject the launch or offer the existing
fullscreen path.

All guest-facing port calls carry the current generation. Claimed byte I/O is
handled in private state. Word/dword and string I/O are rejected with explicit
diagnostics rather than partially executed. This first profile deliberately
does not implement REP I/O.

## Implemented device tier

### Keyboard and mouse

- Set-1 keyboard make/break bytes, including the E0 namespace, left/right Ctrl,
  Space and ordinary modifiers. Duplicate edges are suppressed.
- A 512-key down bitmap. Losing focus discards stale pending host input, emits a
  break for every down key in deterministic order, releases mouse buttons and
  clears the bitmap. The test repeats this transition 64 times.
- 8042 data/status, command-byte access, interface enable/disable, controller
  self-test and bounded keyboard ACK/reset/identify/scanning commands.
- A standard three-byte PS/2 mouse in stream mode, including buttons, signs,
  overflow clamping, enable/disable, defaults, identify, scaling, resolution
  and sample-rate commands. Wheel/IntelliMouse negotiation is unsupported.
- IRQ1 and cascaded IRQ12 delivery. No guest byte is read from the physical
  controller and no guest LED command is sent to the host keyboard.

Pause/Break's E1 sequence, scan sets 2/3, typematic timing, wheel packets and
absolute pointing devices are outside this tier. A caller requesting one must
receive the English unsupported-device result, not physical passthrough.

### Timer and interrupt controller

- Private cascaded 8259-compatible IRR/ISR/IMR state, ICW initialization,
  fixed-priority acknowledgement, OCW register selection and specific or
  nonspecific EOI.
- Private PIT counters and latch/read/write sequencing for modes 0, 2 and 3.
  Channel 0 raises virtual IRQ0 from explicit elapsed PIT clocks.
- `cvgp_advance` runs on host time. Guest virtual IF controls only whether
  `cvgp_irq_acknowledge` may deliver a vector; it never prevents host servicing,
  PCM generation, input capture or native repaint.

PIT modes 1/4/5, gate inputs, read-back status, RTC, PC-speaker synthesis,
priority rotation, special mask mode and level-triggered devices are explicit
later work.

### DMA and Sound Blaster

- Private 8237 address/count/page/flip-flop/mask/mode/terminal-count state for
  8-bit channels 0–3 and 16-bit channels 5–7. Memory-to-device, increment or
  decrement and auto-initialize transfers read only through the session-owned
  guest-memory callback. Channel 4 cascade, memory-to-memory, device-to-memory
  recording and unsupported transfer modes never reach the host controller.
- A virtual SB16 profile at A220, IRQ7, DMA1 and DMA5 by default. Mixer resource
  reads and valid virtual rerouting are supported without changing hardware.
- SB16 mixer register 82h (interrupt status: bit 0 8-bit, bit 1 16-bit DMA),
  cleared by the 22Eh/22Fh acknowledgements (added 28 September 2026; SB16
  drivers such as Apogee's chain an IRQ away when neither bit is set).
- DSP 4.xx speaker commands only change the D8h status; the DAC is never
  gated (28 September 2026; DMX, the original DOOM's driver, never sends D1h).
  A stopped or paused transfer outputs silence.
- DSP reset/AA, version 4.05, speaker state, time constant or explicit sample
  rate, block size, single-cycle/auto-initialize 8-bit and SB16 8/16-bit output,
  pause/resume, exit-auto, silence, test register, identification and software
  IRQ behavior.
- Unsigned/signed mono/stereo PCM conversion, bounded zero-order resampling,
  virtual DMA terminal count and virtual SB interrupt/acknowledgement.

ADPCM, high-speed compatibility details beyond the named commands, MIDI/UART,
ADC/recording, DSP copyright strings, MPU-401, ISA bus-master behavior and
physical DMA are unsupported. The virtual DSP records the unsupported command
and returns `The requested Sound Blaster operation is not supported.`

### OPL

The model owns primary/secondary OPL register indexes, 512 register bytes and
timed OPL status bits. Register writes feed a protected synthesis callback.
The supplied adapter uses DBOPL from VSBHDA commit
`75fa4bbfea70cbcc0c40d1212f04952ff8abbf16`. The complete unmodified source
archive, canonical URL, SHA-256, upstream GPL text and per-file notices are in
`third_party/vsbhda/`. The adapter fixes the backend at 44,100 Hz and normalizes
DBOPL's OPL2 mono output to interleaved stereo. It does not load VSBHDA's
physical sound-card drivers.

`guest_peripherals.c` is independent of DBOPL. A combined DBOPL binary must be
distributed under compatible GPL terms with corresponding source, as recorded
in `third_party/vsbhda/README.md`.

## Protected-mode clients

Implemented on 28 September 2026; the design and evidence are in
[DOS/4GW games in the DOS window](vm-dpmi-window-devices-2026-09-28.md). The
original analysis follows.

A DPMI client under HDPMI runs while HDPMI, not Jemm, owns the CPU. Jemm's port
traps, virtual PIC and poll never run then. Bridging the ports alone (through
`DEV_IO` from HDPMI's existing I/O-emulation hook) would not be enough: the
model's IRQs would also have to be delivered to the client's protected-mode
handlers, and its EOIs on 20h/A0h routed to the profile's PIC, on every mode
switch. That is a separate piece of work. Until then, DOS/4GW and other DPMI
games (DOOM, doom-vanille) keep the existing paths and do not get this
model's keyboard, mouse or sound in the window. Never install VSBHDA's
physical-audio path alongside this model: that would create two owners.

## Executed model evidence

Run:

```sh
python3 scripts/test_guest_peripherals.py
```

The script verifies the pinned archive, extracts only the three named DBOPL
source files, compiles CiukiOS sources with warnings-as-errors and ASan/UBSan,
executes 912 assertions, produces objective SB and OPL WAV captures, and builds
and links the core model as a freestanding OpenWatcom DOS target with no default
libraries or unresolved imports. The renamed-API qualification is preserved at
`build/tests/guest-peripherals-integration-2026-09-27/report.json` and in the
[portable report](validation/2026-09-27-integration/peripherals-renamed.json).
The default output remains `build/tests/guest-peripherals/report.json`;
alternate directories preserve earlier evidence.

The first qualified run records:

| Capture | Frames / format | Objective result |
| --- | --- | --- |
| Virtual SB DMA | 128, 44.1-kHz stereo S16 | peak 32,768; five distinct sample values; AC RMS 17,253.61 |
| OPL/DBOPL | 4,096, 44.1-kHz stereo S16 | peak 32,767; 254 distinct values; 2,096 sample changes; AC RMS 1,779.94 |

An independent repeat produced the same raw SB SHA-256
`5975e9cdf007f3a4659dd499ed0bbf11cff98fcc230c62df0a752f5dad292c47`
and OPL SHA-256
`1f39c7ec9a7a8929b717c1a869888122013f9ffc56bcc5ae680f2c32d26bcbcf`.

This is deterministic model/backend evidence. It does not execute CPU IN/OUT
instructions, an original game, or the native desktop concurrently.

## Executed session evidence

`scripts/qemu_test_devices.py` runs `src/probes/vm/dev_test.asm`
(DEVTEST.COM) in the V86 session with real QEMU keyboard events. The QEMU
WAV capture (sizes left at 0 by QEMU, so it is parsed manually) is analysed
in 0.25 s windows around 440 Hz. Final record:
[`devtest.json`](validation/2026-09-27-devices/devtest.json).

| Check | Result |
| --- | --- |
| Focused raw keys `a`, `b` | `1E 9E 30 B0` |
| Key typed while unfocused | none delivered |
| Key held across focus loss | `20` then released `A0` |
| SB16: DSP version, 8-bit DMA blocks on IRQ7 | `0405`, 6 of 6 |
| 441 Hz square wave (SB DMA) heard on the AC'97 | 2.5 s |
| 440 Hz FM note (OPL) heard on the AC'97 | 1.0 s |

`scripts/qemu_test_vga_window.py` runs GUESTIO.COM (`src/probes/vm/guest_io.asm`)
in the native DOS window: INT 9 raw keys with focus changes made by clicking
another window, INT 33h reset `FFFF`/2 buttons, pointer at the client centre
= guest (320,100) at 640×400 and at a resized 480×300 client, move/press/release
event handler mask 7, SB16 IRQ7 ×6 and OPL. The window's WAV contains the square
wave (2.5 s) and the FM note (0.75 s). Final record:
[`window.json`](validation/2026-09-27-devices/window.json).

## Acceptance ledger

| Gate | Status |
| --- | --- |
| Model ownership, input, timer/PIC, DMA/SB/OPL and cleanup | PASS under host sanitizers and freestanding target link (912 assertions) |
| Video/presenter/peripheral combined objects | PASS |
| Real V86 CPU I/O through Jemm into this model | PASS (DEVTEST, GUESTIO) |
| Original binary in the window with the model active | PASS for FIRE (DOS Navigator) and VGASEM |
| Objective QEMU audio capture through this model | PASS (SB DMA and OPL, console and window) |
| Native UI repaint/input while the guest runs | PASS (window harness) |
| Focus changes through real native GUI events | PASS (click another window, focus-loss key release) |
| Normal/close exit with trap/hook removal, 8042 drain, desktop keyboard/mouse live | PASS |
| Protected-mode (HDPMI) clients through this model | PASS since 2026-09-28 ([record](vm-dpmi-window-devices-2026-09-28.md)) |
| Original Doom audio through this model | PASS since 2026-09-28 (DOOM 1.9 with DMX, doom-vanille with Apogee); Wolf3D not run |
| Physical T23/E500 acceptance | OPEN and independent of QEMU |

No 30 fps statement is implied. The existing fullscreen original-game audio
results remain evidence for those old paths only.
