# Legacy Audio Bring-up Plan v0.1

## Status

Phase 7 has controlled groundwork but is not closed and is not yet the primary project phase.

CiukiOS has repeatable, objective WAV evidence for AdLib/OPL2 music, SB16 detection and initialization, real-mode DMA/IRQ delivery, protected-mode interrupt behavior, and doom-vanille's external AudioLib mixer during healthy gameplay. Controlled WAD-lump playback and original Doom's bounded OPL2-music/PC-speaker-SFX gameplay are also green. Original Doom's separate SB16/DMX path remains a deterministic failure, and no additional external DOS audio workload has yet completed playback plus clean shell return.

## Evidence Layers

| Layer | Current status | Proven scope | What remains unproven |
|---|---|---|---|
| SB16 DSP detection | PASS in project helper | DSP reset and response at the configured legacy base. | Broad hardware matrix. |
| SB16 initialization | PASS in project helper | DSP/mixer setup needed by the controlled path. | Arbitrary third-party driver initialization. |
| Real-mode playback | PASS in project helper and WAV gate | DMA1/IRQ7 sample transfer, completion, nested launcher return, and non-silent output. | Broad third-party and hardware coverage. |
| Protected-mode IRQ/timer | PASS in PMIRQSB and WAV gate | DOS/4GW/DPMI timer reflection, SB IRQ delivery, controlled task mode, and non-silent output. | Original DMX scheduler/return behavior. |
| Controlled WAD playback | PASS in DOOMSFX | Selected lumps such as DSPISTOL and DSDOROPN load and play through the controlled SB16 path. | Original DOOM/DMX behavior; external workload breadth. |
| DOOM OPL2 + PC speaker | PASS on `full` for bounded game workflow | A real gameplay frame and mixed QEMU WAV capture pass AC RMS, peak, sample-diversity, and change-count thresholds. | Full-CD and real-hardware output remain unproven. |
| DOOM SB16/DMX | FAIL | Detection/init paths are reached before a repeatable protected-mode failure. | Invalid-opcode path around 0170:00006930-0000693C and the preceding scheduler/dispatch state. |
| doom-vanille OPL2 + SB16 | PASS for bounded `full` gameplay/audio | Low-DOS allocation, music device 2/code 2, SFX device 3/code 8, combined DMX return 10, healthy gameplay/HUD/walls, and objective mixed output. | Audio-enabled clean application exit, full-CD, and real hardware. |
| AdLib/OPL | PASS for bounded external music workflow | Standard AdLib device at `0x388`, DMX code 2, healthy gameplay, and objectively non-silent OPL-only WAV. | Broader external workload and real-hardware coverage. |

PASS above is intentionally limited to the named project-owned probe. It does not count as an external-application PASS in the Phase 6 matrix.

## Current Blockers

### Original DOOM SB16

The opt-in SB16 SFX profile reaches a repeatable protected-mode invalid opcode. Current evidence indicates execution enters a low data table through a bad dispatch/index state; raw SB16 and protected-mode IRQ helpers still pass. The next investigation must trace backward from the last valid dispatch state rather than replace the proven IRQ/DMA path speculatively.

### Rebuildable A/B target

doom-vanille/pcdoom is the preferred open-source comparison target. The focused `full` lanes pass its former `I_AllocLow(256000)` blocker, reach DOS/4GW/DPMI and `ST_Init`, initialize OPL2 music plus SB16 SFX together, and validate real gameplay with a present HUD and coherent wall columns. The former texture corruption was a build ABI regression: the upstream project requires signed `char`, 32-bit enums, and one-byte structure packing. With those flags restored, the WAV gate records a non-silent mix and the VGA-fast timedemo completes 350 gametics in 172 realtics. Audio-enabled clean completion, full-CD, and real-hardware evidence remain open.

### PC speaker

Configuration selection alone is not playback evidence. The current WAV lane therefore enters a real gameplay frame and rejects short, silent, constant, or low-change captures. That is bounded emulator evidence, not a substitute for the still-required real-hardware result.

## Remaining Execution Order

1. Keep SB16, PMIRQSB, and DOOMSFX lanes green while runtime ownership changes.
2. Keep the 256 KiB low-DOS, gameplay texture-integrity, performance, OPL2, and combined pcdoom OPL2/SB16 gates green while adding audio-enabled clean-exit evidence.
3. Reproduce and trace the original DOOM failure backward from the invalid dispatch without game-specific OS patches.
4. Keep the verified original Doom OPL2/PC-speaker gameplay/WAV gate green and add full-CD plus real-hardware evidence.
5. Add another redistributable external DOS audio workload that exercises detection, initialization, playback, termination, and shell return.
6. Extend the now-green AdLib/OPL workload to another external application and real hardware.

## Phase 7 Exit Gate

Phase 7 can close only when:

1. DOOM reaches one explicit, repeatable game-level audio target on a documented profile and device.
2. At least one other external DOS audio application completes detection, initialization, practical playback, and clean return.
3. Probe, controlled-harness, and game-level results remain separately reported.
4. No audio path depends on an undocumented binary patch or hidden Stage1 mutation.
5. Full and full-CD shared-runtime gates plus all affected audio lanes are green on the same checkout.
6. At least one real-hardware result records machine, sound device, configuration, observed output, and limitations.

## Evidence References

1. doom-controlled-audio-lane-2026-05-17.md
2. doom-dmx-targeted-reverse-2026-05-15.md
3. doom-audio-retest-2026-07-21.md
4. ../src/probes/pmirqsb/README.md
