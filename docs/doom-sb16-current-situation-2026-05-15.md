# DOOM SB16 audio status - 2026-05-15

## Executive summary

DOOM runs and reaches a stable runtime in CiukiOS, but the real SB16 SFX path
still does not produce working in-game audio. The current evidence says the
kernel/Stage1 path is not the main blocker anymore.

The strongest current diagnosis is:

**raw SB16 IRQ/DMA works, DOS/4GW protected-mode IRQ/DMA works, and a
timer-dispatched DMA service works. The remaining failure is likely inside
DOOM/DMX's own protected-mode sound runtime, around `WAV_PlayMode`, `FX_Init`,
or first `SFX_PlayPatch` / `FX_PlayRaw`.**

Do not spend more time on random IRQ/DMA/music toggles. The next useful work is
targeted reverse/instrumentation of the exact packaged `DOOM.EXE`.

## Repository state

- Repository: `CiukiOS`
- Branch: `main`
- Latest pushed commit at the time of this note:
  `c099791 Add DOS audio probes and DOOM SB16 diagnostics`
- Public repo:
  `https://github.com/PeronSlayer/CiukiOS`

## Exact DOOM binary

Local binary:

- Path: `third_party/Doom/DOOM.EXE`
- SHA256: `799a20c759567cebb530b7d8b1e7765b13734be5af7f97367f6aa81d87b636da`
- MD5: `c5ade1b30eb1d932921d0eebac8c6d07`
- Size: `694K`
- Type: embedded DOS/4G executable

Local WAD:

- Path: `third_party/Doom/DOOM.WAD`
- Size: `11M`
- Type: Doom main IWAD data, 2194 lumps

Reverse notes:

- The real embedded LE payload starts at file offset `0x25214`.
- Extracted analysis payload used locally:
  `build/rev/doom_part_25214.exe`.

## Audio profiles

Default packaged DOOM profile:

- `snd_musicdevice=0`
- `snd_sfxdevice=1`
- `snd_channels=8`
- This is the stable PC speaker SFX fallback.

Opt-in SB16 investigation profile:

- `CIUKIOS_DOOM_AUDIO_PROFILE=sb16-sfx`
- `snd_musicdevice=0`
- `snd_sfxdevice=3`
- `snd_channels=8`
- `snd_sbport=544` (`0x220`)
- `snd_sbirq=7`
- `snd_sbdma=1`

Music remains disabled because AdLib/OPL still destabilizes the DOOM lane.

## What works

### Real-mode SB16 helper path

`SB16INIT.COM`, `AUDIOTST.COM`, and `AUDIOKEY.COM` validate that basic SB16
output works outside DOOM.

Evidence:

- DSP detected at `0x220`.
- DMA1 playback completes.
- IRQ7 helper path completes.
- Interactive and non-interactive audio helpers are packaged under
  `\SYSTEM\DRIVERS`.

### DOS/4GW protected-mode SB IRQ/DMA

`PMIRQSB.COM` launches a DOS/4GW protected-mode payload:

- `PMIRQSB.LE`
- runtime: `DOS4GW.EXE`

It installs protected-mode interrupt vectors through DPMI `INT 31h
AX=0204/0205`, enables virtual interrupts, starts SB DMA, and checks whether
protected-mode code receives the SB IRQ.

Validated cases:

- IRQ7 single-cycle DMA, ACK before EOI: PASS
- IRQ7 single-cycle DMA, EOI before ACK: PASS
- IRQ7 auto-init DMA: PASS
- IRQ5 variants under matching `QEMU_SB_IRQ=5`: PASS
- DMA buffer below 64K boundary: PASS
- PIC ISR/IRR snapshots captured during probes

### DMX-like timer service probe

`PMIRQSB TASK` was added to test a narrower DMX-like behavior:

- installs protected-mode timer vector
- installs protected-mode SB IRQ vector
- dispatches three timer-paced DMA starts
- receives three SB DMA IRQs
- exits with `TASK PASS`

Fresh markers observed:

- `[PMIRQSB] TASK INSTALL`
- `[PMIRQSB] TASK HIT`
- `[PMIRQSB] FXDMA START`
- `[PMIRQSB] FXDMA IRQ`
- `[PMIRQSB] TIMER HIT`
- `[PMIRQSB] TASK PASS`
- `[PMIRQSB] PASS`

This is important because it means a simple protected-mode timer/task/DMA model
works in CiukiOS under DOS/4GW.

## What fails

### DOOM SB16 SFX path

With `CIUKIOS_DOOM_AUDIO_PROFILE=sb16-sfx`, DOOM starts and reaches runtime, but
the SB16 SFX lane does not produce working gameplay audio.

Observed markers:

- `I_StartupSound`
- `SB_Detect returned p=0x220,i=7,d=1`
- `calling DMX_Init`
- `DMX_Init() returned 8`
- `S_Init`
- `HU_Init`

Taxonomy result:

- `runtime_stable=PASS`
- `visual_gameplay=FAIL`
- low-diversity startup screen remains

Manual QEMU result from user:

- Game can be played in QEMU on stable lane.
- In-game SFX actions such as shooting/opening doors still produce no audible
  SB16 SFX.

## What is ruled out

Do not retest these blindly:

- Not a basic DSP detection problem.
- Not a raw DMA1 problem.
- Not a raw IRQ7 delivery problem.
- Not a simple IRQ5 vs IRQ7 issue.
- Not a 64K DMA boundary issue.
- Not an ACK-before-EOI vs EOI-before-ACK issue.
- Not single-cycle vs auto-init DMA at the raw probe level.
- Not AdLib fallback, because music-off SB16 still fails.
- Not simple channel count, because `snd_channels=1` still fails.
- Not fixed by helper priming, because `PMIRQSB PRIME` passes and DOOM still
  fails.
- Not a reason to fake `INT 31h` in Stage1. DOS/4GW owns the DPMI runtime.

## Public references used

Legal/public references only:

- PCDoom dmxwrapper:
  `https://github.com/nukeykt/pcdoom/tree/master/dmxwrapper`
- Doom Vanille:
  `https://github.com/AXDOOMER/doom-vanille`
- DJDoom:
  `https://github.com/FrenkelS/djdoom`
- Apogee Sound System:
  `https://github.com/jimdose/Apogee_Sound_System`
- Open Watcom DOS/4GW notes:
  `https://github.com/open-watcom/open-watcom-v2/blob/master/docs/doc/rsi/dos4gwqa.gml`

Do not use leaked proprietary DMX source.

## Public DMX runtime map

The closest public map from PCDoom / Doom Vanille:

1. `I_StartupSound()`
2. `I_StartupTimer()`
3. `I_sndArbitrateCards()`
4. `SB_Detect()`
5. `DMX_Init()`
6. `I_SetChannels()`
7. `WAV_PlayMode()`
8. `FX_Init()`
9. `S_StartSound()`
10. `SFX_PlayPatch()`
11. `FX_PlayRaw()`

Current CiukiOS evidence reaches at least `DMX_Init`, `S_Init`, and `HU_Init`.
The exact point where DOOM enters or fails around `WAV_PlayMode`, `FX_Init`, or
first `SFX_PlayPatch` is not yet proven.

## Most likely remaining failure

The likely failure is inside DOOM/DMX's own protected-mode SFX runtime, not in
CiukiOS raw SB16 delivery.

Most likely areas:

- `WAV_PlayMode()` not being reached or not succeeding.
- `FX_Init()` returning a warning/error path that DOOM tolerates silently.
- First `SFX_PlayPatch()` not being reached.
- `FX_PlayRaw()` starting but using assumptions not covered by `PMIRQSB`.
- DMX internal memory locking, callback stack, or voice/mixer state.

## Next concrete step

The next useful task is targeted reverse/instrumentation of the exact
`DOOM.EXE`.

Goal:

- Prove whether `WAV_PlayMode()` is called.
- Prove whether `FX_Init()` is called.
- Prove whether first `SFX_PlayPatch()` / `FX_PlayRaw()` is called.
- If called, prove whether SB DMA starts from inside DOOM/DMX.
- If DMA starts, prove whether the SB IRQ returns to DOOM/DMX.

Recommended approach:

1. Continue reverse from extracted LE payload offset `0x25214`.
2. Locate callsites around strings:
   - `S_Init: Setting up sound.`
   - `Sfx device #%d & dmxCode=%d`
   - `calling DMX_Init`
   - `DMX_Init() returned %d`
   - `S_START`
3. Find the path corresponding to public `I_SetChannels()` / `WAV_PlayMode()`.
4. Add the smallest possible binary-safe marker, or use debugger/breakpoint
   evidence if binary patching is too risky.
5. Do one test at a time.

Do not:

- implement a Stage1 DPMI host
- fake `INT 31h`
- randomly switch IRQ/DMA again
- re-enable AdLib/music
- assume helper audio equals DOOM audio

## Current test status

Latest known validation:

- `make build-full`: PASS
- `PMIRQSB TASK`: PASS
- `make build-full-cd`: PASS
- default DOOM PC speaker SFX visual lane: PASS from prior validation
- opt-in DOOM SB16 SFX lane: FAIL for actual SFX/gameplay path, but reaches
  `runtime_stable=PASS`

