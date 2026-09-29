# DOS/4GW games in the DOS window with the session's devices — 28 September 2026

Status: unmodified DOS/4GW programs run in the native desktop DOS window
through `DPMIRUN.COM` and the session-bound patched HDPMI 3.24, and use the
session's **virtual VGA, keyboard, mouse, Sound Blaster 16 and OPL3**, with
sound on the AC'97. Validated with the original id **DOOM 1.9**
(`APPS\DOOM\DOOMCORE.EXE`, DMX sound) and **doom-vanille**
(`APPS\DOOMVAN\PCDMCORE.EXE`, Apogee Sound System), both unchanged. QEMU/KVM
evidence only; no physical-hardware claim.

Try it: [build and test profile](#build-and-test-profile).

## Path of a DOS/4GW game

```
desktop DOS window (DOSWIN) ── V86 ── Jemm386 + CVSESSION.DLL (JLM)
                                       VGA model, scheduler, SB16/OPL/PIC/8042 model, AC'97
DPMIRUN.COM ── HDPMI32I -cSSSS:OOOO -r ── DOS/4GW client (ring 3)
               adapter (CIUKIVM.ASM) in HDPMI ring 0:
               VGA ports + #PF VGA memory -> shared model pages
               device ports              -> DEV_IO (real-mode call into the JLM)
               held device IRQs          -> client PM handler (locked-stack path)
```

`DPMIRUN` asks CVSESSION for the window's scheduler descriptor (op 0Bh),
loads HDPMI resident bound to it, runs the program from its own directory,
then releases the device IRQs (op 36h, CL=0) and unloads the host.

## Devices for protected-mode clients

**Ports.** HDPMI traps, for a bound client, PIC 20h/21h/A0h/A1h with ISA DMA
00h–1Fh, DMA pages 80h–8Fh, 16-bit DMA C0h–DFh, the Sound Blaster at 220h–22Fh
and OPL at 388h–38Bh (`is0006` TRAPPROCS ranges). The adapter executes each
cycle in ring 0 through HDPMI's internal real-mode call to a stub that calls
the JLM's `DEV_IO` (op 34h). IF is clear for the whole call, and the
interrupted real-mode context (`v86iret` SS:SP and segments, `pmstate`) is
saved around it. PIC cycles reach Jemm's single virtual PIC (profile function
7); SB/DMA/OPL cycles reach the peripheral model. The client never touches
the physical PIC or a physical Sound Blaster.

**Interrupts.** The first protected-mode `DEV_IO` *claims* the device IRQs.
From then on the model's IRQs are **held** in CVSESSION instead of being
injected into V86, and the held mask is published in the shared video header
(`cvvid_shared.device_irqs`, offset 320). The adapter delivers a held line
only at an interruptible instruction boundary of the client (ring 3, IF set,
no virtual CLI or STI shadow):

- the end of an emulated port access or VGA memory access;
- the return from a client IRQ handler (`rpmstacki`);
- a physical timer tick. The device IRQ is entered first; the tick is
  acknowledged and deferred until that handler returns, like a virtual-CLI
  tick.

At that point `DEV_PM_IRQ` (op 36h, CL=2) arbitrates the highest held line
through Jemm's virtual PIC (profile **function 8**: same mask, cascade and
fixed-priority rules as V86 injection, the line becomes in service). HDPMI
then enters `r3vect08+line` (or `r3vect70+line-8`) through its ordinary
locked-stack IRQ path (`lpms_call_int`), exactly as for a physical IRQ that
arrives right after the instruction. The client's EOI on port 20h goes back
through `DEV_IO` to the same virtual PIC.

Why not V86 injection: a DOS extender runs its own IRQ and callback wrappers
on private, non-reentrant stacks. A virtual IRQ injected into V86 while HDPMI
was in the middle of a port bridge, or under an IRQ reflected to real mode,
re-entered DOS/4GW's wrapper. The preserved failure is
`DOS/4GW error (2001): exception 0Dh at 97:0000751A`, at `lss esp,[ebp+32h]`
in the wrapper's epilogue.

## Model fixes found by the games

- **SB16 mixer 82h** (interrupt status) was not modelled and read 0. The
  Apogee Sound System's SB16 ISR reads it first and chains the IRQ away when
  no DMA bit is set, so it never acknowledged a block. Now bit 0 (8-bit) and
  bit 1 (16-bit) are set per completed block or `F2h`/`F3h`, and cleared by
  22Eh/22Fh.
- **DSP 4.xx speaker gating.** The model muted the DAC until `D1h`. On an
  SB16 the speaker commands do not gate output, and DMX never sends `D1h`,
  so the original DOOM was silent. Now `D1h`/`D3h` only change the `D8h`
  status. A stopped or paused transfer outputs silence. The first version of
  this change kept mixing the last sample after DMA stopped; that constant
  level masked the later FM note in DEVTEST and the VGA-window gate. The
  failing run is kept in `build/tests/vm-window-profile-2026-09-28`, and the
  unit suite now asserts a silent stopped DSP.

All three are covered by `scripts/test_guest_peripherals.py`.

## Host changes needed by the original DOOM

- The adapter's virtual-CLI tracer (from the 27 September IF profile)
  refused `LSS`, which DOS/4GW Professional 1.95 executes inside a CLI region
  of `I_StartupTimer`. It aborted with `hdpmi: fatal exit C10F`, printed with
  the bytes `0F B2 24 24` at `1EF:002435A0`. `LSS (E)SP,m16:16/32` is now
  emulated like `MOV SS`, and HDPMI's own ring-3 stubs may execute
  `INT 30h` natively under the trace.
- HDPMI's image must end below RVA 10000h. The temporary diagnostic rings
  were removed and the bridge's state save uses loops. The code now ends 23
  bytes before the 4 KiB boundary that would push `_TEXT32R3` past 10000h.
  Any further adapter code must free bytes first; the build refuses an
  overflow.

## Window painting no longer emulated

A CPU profile of doom-vanille in the window (981 `info registers` samples) put
40% of the time in CVSESSION's x86 emulator (`cvx_execute`), 10% in the
presenter, 33% in HDPMI's VGA emulation, and 3% in the game. The desktop
compositor painted the window through the host-mode A000 window, one trapped
and emulated instruction at a time. In host mode the 16 aperture PTEs
A0000–AFFFF now map straight onto the owned framebuffer at the selected VBE
bank (`host_map`: at HOST_ENTER and on each intercepted `4F05h`; the saved
trapping PTEs return at HOST_LEAVE and detach). The emulator's share fell to
~4%. The game's VGA throughput rose by ~16%, and SB IRQ delivery went from
65% to 84–92% of blocks.

Remaining latency: while the desktop paints (V86) the client does not run,
so a block completing then is delivered after the paint; consecutive
completions coalesce into one IRQ, as on a PIC. Per run 8–17% of SB blocks
are coalesced. Emulating every VGA write of a planar game (~50% of the time)
is inherent to the model-based VGA.

## Build and test profile

Build the full image with the profile (adds `C:\VM`) and run it with AC'97:

```sh
scripts/build_run_full_vm_window.sh          # = CIUKIOS_VM_WINDOW=1 scripts/build_full.sh
                                             #   + QEMU_AUDIO_DEVICES=ac97 scripts/qemu_run_full.sh
CIUKIOS_SKIP_BUILD=1 scripts/build_run_full_vm_window.sh   # reuse the image
```

`C:\VM` holds `JEMM386.EXE`, `JLOAD.EXE`, `CVSESS.DLL`, `VMSTART.COM`,
`DPMIRUN.COM`, `README.TXT` and the Jemm/JLOAD licence and modification
notices. `\SBEMU\HDPMI32I.EXE` is the patched host from the same build. In
the guest:

1. F4 (DOS console): `run \VM\VMSTART.COM`, then `EXIT`.
2. F3 (Run): `run \VM\DPMIRUN.COM \APPS\DOOM\DOOMCORE.EXE` or
   `run \VM\DPMIRUN.COM \APPS\DOOMVAN\PCDMCORE.EXE -warp 1 1`.

`VMSTART` passes Jemm386 `NOEMS NOHI X=A000-CCFF I=CD00-EBFF X=EC00-FFFF NODYN
MAX=32M MIN=32M NOVME` (upper memory that is free RAM on QEMU/SeaBIOS); other
options can follow its name.

Complete test profile, on the image it ships:

```sh
scripts/test_vm_window_profile.sh --image build/full/ciukios-full.img \
    --output build/tests/vm-window-profile-<date> [--quick]
```

It extracts the session artifacts from the image, checks that the image's
`SHELL.COM` is built from the tree, and runs:

- the unit suites;
- the V86 gates (full-screen VGA, legacy sessions, V86 CLI profile, DEVTEST,
  HDPMI lifetime, VGA DOS window, text DOS window);
- the DPMI window gates `scripts/qemu_test_dpmi_window.py`:
  - the DPMIPORT probe;
  - doom-vanille and the original DOOM, each with music and effects-only,
    the original through the image's own `VMSTART`/`DPMIRUN` (`--image-vm`).

The DPMI gate checks:

- no DOS/4GW, HDPMI or Jemm fatal error;
- the game draws (protected-mode VGA faults grow, no DAMAGE error);
- the arrow key moves the view;
- SB blocks and OPL writes reach the model;
- SB IRQs are delivered in protected mode;
- no model errors or AC'97 underruns;
- the recorded AC'97 output is audible while Esc/Ctrl open the menu and
  fire;
- F10+Y exits with `[DPMIRUN] EXIT`, the host unloads and the IRQ claim is
  released.

`SUMMARY.json` lists every gate.

## Results

See [validation/2026-09-28-dpmi-window](validation/2026-09-28-dpmi-window/README.md).

## Limits

- QEMU (KVM, pentium3, AC'97) only. The default upper-memory layout is
  QEMU/SeaBIOS specific.
- Keyboard input of a protected-mode client stays physical (DOS/4GW's INT 9
  reads port 60h); the virtual 8042 serves V86 programs. The mouse is the
  window's virtual INT 33h.
- Wolfenstein 3D and other DPMI hosts (DOS/32A, PMODE/W) were not run.
- Delivery latency depends on the desktop's paint time (see above).
