# DOOM DMX/SB16 reverse handoff - 2026-05-15

## Scope

Goal: isolate why DOOM reaches `runtime_stable` but the SB16 SFX path still does
not produce working in-game audio under CiukiOS.

Rules:

- Use public/legal references only.
- Do not search for or use leaked proprietary DMX source.
- Do not fake `INT 31h` in Stage1.
- Keep full and full-CD profiles stable.

## Exact binary

Local binary:

- `third_party/Doom/DOOM.EXE`
- SHA256: `799a20c759567cebb530b7d8b1e7765b13734be5af7f97367f6aa81d87b636da`
- MD5: `c5ade1b30eb1d932921d0eebac8c6d07`
- Size: `694K`
- `file`: `MS-DOS executable, BW collection for MS-DOS, DOS/4G DOS extender (embedded)`

Local WAD:

- `third_party/Doom/DOOM.WAD`
- Size: `11M`
- `file`: `doom main IWAD data containing 2194 lumps`

The embedded DOS/4G image contains the real DOOM LE payload at full-file offset
`0x25214`. The extracted payload in `build/rev/doom_part_25214.exe` is an LE
executable with entry object 1 offset `0x40B48` and data object 3 stack offset
`0x85E10`.

## Runtime markers found

Useful string offsets in the extracted LE payload:

- `DMXTRACE`: `0x82AC8`
- `DMXOPTION`: `0x82AD4`
- `I_StartupSound`: `0x82CCF`
- `I_StartupTimer()`: `0x82F60`
- `SB isn't responding at p=0x%x, i=%d, d=%d`: `0x8309C`
- `SB_Detect returned p=0x%x,i=%d,d=%d`: `0x830C8`
- `I_StartupSound: Hope you hear a pop.`: `0x8315F`
- `Sfx device #%d & dmxCode=%d`: `0x831A9`
- `calling DMX_Init`: `0x831CB`
- `DMX_Init() returned %d`: `0x831E0`
- `S_Init: Setting up sound.`: near `0x84CBB`
- `S_START`: `0x861E5`

Observed failing SB16 lane:

- DOOM reaches `I_StartupSound`.
- DOOM reaches `calling DMX_Init`.
- DOOM reaches `S_Init`.
- DOOM reaches `HU_Init`.
- Taxonomy reports `runtime_stable=PASS`.
- Taxonomy reports `visual_gameplay=FAIL` with low-diversity startup screen.
- No Stage1 `[V25]`/`[V35]` real-mode vector markers appear from DOOM for
  timer/IRQ vectors.

## Public reference map

PCDoom `dmxwrapper/DMX.C` and Doom Vanille `i_sound.c` give the closest public
map:

- `I_StartupSound()` calls `I_StartupTimer()` before card arbitration.
- `TSM_NewService()` stores the timer callback and schedules it with
  `TS_ScheduleTask()`.
- `SB_Detect()` populates port, IRQ, DMA.
- `DMX_Init()` initializes the selected music/SFX devices.
- `I_SetChannels()` calls `WAV_PlayMode(channels, samplerate)`.
- `WAV_PlayMode()` calls `FX_Init()` for digital SFX playback.
- `SFX_PlayPatch()` decodes the DOOM sound lump and calls `FX_PlayRaw()`.

Apogee Sound System notes add two relevant constraints:

- Interrupt/task callbacks and data must be locked for protected-mode IRQ use.
- Older SB16 behavior includes undocumented IRQ-disable/WaveBlaster interaction
  notes, so SB16 mixer state is a real variable, not folklore.

## Reverse evidence in DOOM payload

The extracted DOOM LE contains a small number of direct `INT 31h` occurrences.
The obvious decoded ones seen so far are allocator/runtime related, for example
`AX=0501h`/`AX=0502h` style DPMI memory calls. No direct proof yet of DOOM using
real-mode `INT 21h AH=25h/35h` to hook timer/SB IRQ.

SB port setup code was found around payload offsets `0x28DF0` and `0x2DF90`.
The second region matches Sound Blaster config setup:

- default base `0x220`
- reads configured IRQ, defaulting to 7
- reads configured DMA, defaulting to 1
- stores derived ports for mixer address/data, DSP reset, DSP read, DSP write,
  IRQ ack/status, and SB16 16-bit ack/status

This means DOOM/DMX is entering its own SB setup path. The mute/freeze is not
because CiukiOS never exposes the SB base/IRQ/DMA config.

## What is ruled out

Already ruled out by local validation:

- Raw real-mode SB DMA/IRQ: `SB16INIT` and `AUDIOTST` work.
- Raw protected-mode DOS/4GW SB IRQ/DMA: `PMIRQSB` gets timer hit, IRQ hit, and
  PASS on IRQ7 and IRQ5 variants.
- DMA 64K boundary: `PMIRQSB` validates safe buffers.
- ACK-before-EOI vs EOI-before-ACK: both pass in `PMIRQSB`.
- Single-cycle vs auto-init DMA: both pass in `PMIRQSB`.
- AdLib/music fallback: music-off SB16 still fails.
- Channel count: `snd_channels=1` still fails.
- Simple prelaunch priming: `PMIRQSB PRIME` passes, then DOOM still fails.
- Stage1 fake DPMI: not needed and not correct; DOS/4GW owns `INT 31h`.

## Most likely blocker

The highest-probability blocker is inside DOOM/DMX's protected-mode sound
runtime after card setup, especially the timer/task/FX-init/voice path:

- `I_StartupTimer()` / `TSM_NewService()` scheduling
- `WAV_PlayMode()` / `FX_Init()` starting the digital mixer
- first `SFX_PlayPatch()` / `FX_PlayRaw()` voice start
- protected-mode callback/ISR memory locking or stack assumptions

Raw hardware delivery works. DOOM/DMX path ownership does not.

## Next minimal patch/test

Do not patch Stage1 DPMI.

Next probe should extend `PMIRQSB` with one narrow "DMX-like service" mode:

- install a protected-mode timer vector
- schedule a small mixer-like service counter from that timer
- start repeated short SB DMA buffers from that service path
- lock/report every code/data region touched by the ISR/service path
- emit markers:
  - `[PMIRQSB] TASK INSTALL`
  - `[PMIRQSB] TASK HIT`
  - `[PMIRQSB] FXDMA START`
  - `[PMIRQSB] FXDMA IRQ`
  - `[PMIRQSB] TASK PASS`

Only if that passes, move the smallest equivalent assumption into the DOOM lane.
If it fails, fix the protected-mode timer/task/locking model in the probe first.

## Validation baseline

Current baseline evidence to preserve:

- `make qemu-test-full`: PASS
- `make qemu-test-full-cd`: PASS
- `PMIRQSB` IRQ7 variants: PASS
- `PMIRQSB` IRQ5 variants under matching QEMU IRQ5: PASS
- Default DOOM `pcspeaker-sfx` visual lane: PASS
- Opt-in DOOM `sb16-sfx`: FAIL at visual/SFX path, but reaches
  `runtime_stable=PASS`
