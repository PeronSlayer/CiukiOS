# DOOM audio re-test after the TF-leak fix - 2026-07-21

## Why re-test

The whole May 2026 SB16/DMX audio investigation
(`doom-sb16-current-situation-2026-05-15.md`) ran on a runtime that carried the
`int21_mem_find_next_alloc` TF-leak bug (commit `502e9a7` fixed it). Every child
process, including DOOM, started with a corrupted FLAGS image. The child handoff
now loads a deterministic FLAGS (`push 0x0202 / popf`: IF=1, TF/DF/CF clear), so
the sound path had to be measured again on a clean runtime before trusting the
old "failure is inside DMX" conclusion.

## Method (objective capture)

The local QEMU 11.0.1 has **no `wav` audiodev backend** (only
none/alsa/dbus/jack/oss/pa/pipewire/sdl/spice). Objective audio evidence was
taken with the HMP monitor command `wavcapture <file> snd0 44100 16 2`, which
taps the mixed audiodev output regardless of backend, running against
`-audiodev none,id=snd0` plus `-device sb16,iobase=0x220,irq=7,dma=1,dma16=5`
and (for the speaker lane) `pcspk-audiodev=snd0`. WAVs were analysed for DC
offset, zero-crossing rate, per-segment AC-RMS, and sample span.

## Results on the fixed runtime

### Default profile (`pcspeaker-sfx`, `snd_sfxdevice=1`)

- Reaches full gameplay (matches the `visual_gameplay=PASS` taxonomy lane).
- Captured audio is a **constant ~420 Hz square wave** at near-full amplitude
  for the entire run: per-segment AC-RMS ~24300 with no variation, span 49151,
  present from boot (before DOOM starts). This is a QEMU `pcspk` emulation
  artifact (the speaker line is driven continuously), **not** DOOM SFX. It says
  nothing about whether PC-speaker SFX would be audible on real hardware; it
  just means the QEMU pcspk lane is not a usable measurement or listening path.

### SB16 SFX profile (`sb16-sfx`, `snd_sfxdevice=3`, port 0x220 irq7 dma1)

- DOOM **crashes** during sound setup. On-screen:
  `DOS/4GW Professional error (2001): exception 06h (invalid opcode) at
  170:00006930`, printed right after `S_Init: Setting up sound.` /
  `HU_Init: Setting up heads up display.`
- The SB16 audiodev output is flat silence (sample span = 0).
- This is the **same failure** the May note recorded as "SB16 lane FAIL,
  low-diversity startup screen remains" - the low-diversity screen is this crash
  freezing the DOOM startup text. So the SB16 crash is **pre-existing and
  independent of the TF fix**: at the pre-fix commit DOOM hangs even earlier (at
  the MZ transfer, from the TF bug), so there was never a working SB16 baseline
  to regress. The fix moves DOOM *forward* from "hang at `[MZ] run`" to "reach
  sound init, then fault inside DMX's SB16 driver".

New, actionable over the May note: the fault is now a **single reproducible
address, `170:00006930`** (CS selector 0x170, EIP 0x6930 = offset 0x6930 into
the LE code object), rather than a vague "no audio". That is the concrete target
for the targeted reverse the May note asked for.

### Open-source port A/B (pcdoom / doom-vanille) - blocked

Intent: pcdoom (`build/external/doom-vanille`) ships a rebuilt `dmx.c`, so if it
produced SB16 audio in CiukiOS it would prove the platform is fine and the
commercial `DOOM.EXE`'s DMX is uniquely broken. It could not be used as an audio
test yet: with `snd_sfxdevice=3` it fails earlier with
`I_AllocLow: DOS alloc of 256000 failed, 130928 free`. CiukiOS hands the child
only ~130 KB of contiguous conventional memory; pcdoom's `I_AllocLow` wants
~250 KB. This is a CiukiOS low-DOS-memory limit, unrelated to audio.

### Still true from May

`SB16INIT.COM` / `AUDIOTST.COM` (real-mode SB16) and `PMIRQSB` (DOS/4GW
protected-mode SB IRQ/DMA, incl. a timer-paced DMA "TASK") still pass, so the
platform *can* deliver SB IRQ/DMA to protected-mode code. DOOM/DMX does not get
that far.

## Updated diagnosis

The remaining audio blocker is **not** the TF leak and **not** raw SB IRQ/DMA
delivery. For the packaged commercial `DOOM.EXE` it is a hard invalid-opcode
fault inside DMX's SB16 setup at `170:00006930`. PC-speaker SFX is the only
lane that keeps DOOM running end-to-end, and it is unmeasurable/unpleasant under
QEMU's pcspk emulation.

## Fault mechanism (pinned down 2026-07-21 via `-d int` + gdbstub)

Not CPU-model dependent: `-cpu pentium3`, `486`, and `pentium` all reproduce the
identical crash screen (unique_colors=3, nonblank 21114/288000).

`-d int` capture of the #UD:

```
v=06 e=0000 cpl=0 IP=0170:0000693c  EAX=0000eaff EBX=00000080 ECX=00000c40
EDX=00000170 ESI=00006284 EDI=00000102 EBP=0000627a ESP=0000623c
CS=0170 base=0x00000000 limit=ffffffff CS32   (flat: linear == EIP)
SS=00c8 base=0x00116330  DS=00a0 base=0x00116330 DS16
```

CS is flat (base 0), so the fault executes at **linear 0x693c**, which is low
conventional memory - *not* where DOS/4GW maps DOOM's 32-bit code. gdb hardware
breakpoint at 0x693c, memory dump:

```
0x6930: 30 41 f3 06 30 41 fd 06 00 00 0a 00 ff ff ff ff
0x6940: ac 86 0f 00 00 00 0a 00 b9 f8 ff 06 00 00 ff 06
```

The bytes at 0x693c are `ff ff ff ff`. `FF /7` is an undefined group-5 encoding
-> genuine #UD. This is a **`0xFFFFFFFF` table terminator**, i.e. execution ran
into a **data table**, not code. Nearby (0x693f) sits
`ljmp *0xf(%esi,%eax,4)` - an indirect **jump-table dispatch**. With
`EAX=0x0000eaff` (a garbage index, 60159) and `ESI=0x6284` (pointing into this
same low data region), the smoking gun is: DMX's SB path performs a computed
jump/dispatch through a **corrupted index/pointer** and lands inside a data
table. The 0xff-heavy `EAX` value hints the index may derive from an SB
DSP/mixer port read that returns open-bus 0xFF under this SB setup.

This is a defect *inside DMX's protected-mode SB runtime*, deterministic and
now precisely located - a real reverse target, but a non-trivial one (needs
backward tracing to find where `EAX`/the dispatch index is computed).

## Concrete next steps, most-leverage first

1. **Reverse backward from the dispatch.** Find where DMX computes the jump-table
   index that becomes `EAX=0xeaff`, starting from the `ljmp *[esi+eax*4+0xf]`
   dispatch near linear 0x693f. If the index comes from an SB DSP/mixer read,
   the root cause is an SB register QEMU's `sb16` answers with 0xFF that real
   DMX-era hardware would not - fixable by matching the SB response, not by
   touching DOOM. The EXE itself is not a plain MZ+LE at `e_lfanew` (embedded LE;
   May note put the payload near file `0x25214`, an `LE` signature also appears
   at `0x1AF6B`), so static mapping needs the real LE object table; dynamic
   gdb tracing (as used here) is the faster route.
2. **Unblock the open-source port (also improves general DOS-app compat).**
   Enlarge the conventional-memory arena handed to MZ children so
   `I_AllocLow(256000)` succeeds (`DOS_HEAP_BASE_SEG`/`DOS_HEAP_LIMIT_SEG` and
   the MZ load layout in `floppy_stage1.asm`). Then pcdoom's rebuildable,
   debuggable `dmx.c` becomes the cleanest route to actually-audible SB16 SFX,
   and its result isolates platform vs. binary.
3. **Confirm PC-speaker SFX on real hardware (T23).** QEMU's pcspk lane is not a
   valid test; on real hardware the default profile's PC-speaker SFX may already
   be audible. If so, the default profile is "audio works" for real HW today.

## Do NOT (still holds)

- No Stage1 DPMI host, no faked `INT 31h`, no random IRQ/DMA/music toggles, no
  re-enabling AdLib/OPL music (still destabilises the lane), and do not treat
  helper-tool audio as equivalent to DOOM audio.
