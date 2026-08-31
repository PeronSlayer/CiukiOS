# Legacy Audio Bring-up Plan v0.1

## Status

Phase 7 has controlled groundwork but is not closed and is not yet the primary project phase.

CiukiOS has repeatable project-probe evidence for SB16 detection, initialization, DMA/IRQ delivery, protected-mode interrupt behavior, and controlled WAD-lump playback. Original DOOM SB16/DMX remains a deterministic failure, PC-speaker output is not validated game-SFX evidence, and no second external DOS audio workload has completed the product exit gate.

## Evidence Layers

| Layer | Current status | Proven scope | What remains unproven |
|---|---|---|---|
| SB16 DSP detection | PASS in project helper | DSP reset and response at the configured legacy base. | Broad hardware matrix. |
| SB16 initialization | PASS in project helper | DSP/mixer setup needed by the controlled path. | Arbitrary third-party driver initialization. |
| Real-mode playback | PASS in project helper | DMA1/IRQ7 sample transfer and completion. | Game-level mixer/timing compatibility. |
| Protected-mode IRQ/timer | PASS in PMIRQSB | DOS/4GW/DPMI timer reflection, SB IRQ delivery, and controlled task modes. | Original DMX scheduler/return behavior. |
| Controlled WAD playback | PASS in DOOMSFX | Selected lumps such as DSPISTOL and DSDOROPN load and play through the controlled SB16 path. | Original DOOM/DMX behavior; external workload breadth. |
| DOOM PC speaker | UNVERIFIED | Packaged configuration selects PC-speaker SFX and avoids SB16. | QEMU currently produces a constant-tone artifact; real SFX must be validated on hardware or with trustworthy capture. |
| DOOM SB16/DMX | FAIL | Detection/init paths are reached before a repeatable protected-mode failure. | Invalid-opcode path around 0170:00006930-0000693C and the preceding scheduler/dispatch state. |
| doom-vanille SB16 | FAIL for product audio | The low-DOS allocator and DMX initialization complete with device 3/code 8. | Captured gameplay audio remains digital silence; planar video is also corrupt, so this is not yet a clean A/B workload. |
| AdLib/OPL | PARTIAL investigation | Device exposure and focused experiments exist. | Stable initialization/playback evidence suitable for a product claim. |

PASS above is intentionally limited to the named project-owned probe. It does not count as an external-application PASS in the Phase 6 matrix.

## Current Blockers

### Original DOOM SB16

The opt-in SB16 SFX profile reaches a repeatable protected-mode invalid opcode. Current evidence indicates execution enters a low data table through a bad dispatch/index state; raw SB16 and protected-mode IRQ helpers still pass. The next investigation must trace backward from the last valid dispatch state rather than replace the proven IRQ/DMA path speculatively.

### Rebuildable A/B target

doom-vanille/pcdoom is the preferred open-source comparison target. The focused `full` lane passes its former `I_AllocLow(256000)` blocker, reaches DOS/4GW/DPMI, `ST_Init`, and DMX initialization, and records `[doomvan-memory] PASS sb16_low_dos_256k`. This is a general Phase 5/6 memory regression PASS, not gameplay or audio evidence: the direct gameplay frame has corrupted planar walls and a missing HUD, while the captured SB16 stream is digital silence. Clean completion and wider-profile evidence are also open.

### PC speaker

Configuration selection alone is not playback evidence. QEMU's constant tone must not be described as working DOOM SFX. Capture and classify the signal on at least one real legacy target or through a trustworthy emulated waveform before promoting this path.

## Remaining Execution Order

1. Keep SB16, PMIRQSB, and DOOMSFX lanes green while runtime ownership changes.
2. Keep the 256 KiB low-DOS pcdoom gate green, fix the planar gameplay path, and obtain a clean stable visual baseline before using the target for audio comparison.
3. Reproduce and trace the original DOOM failure backward from the invalid dispatch without game-specific OS patches.
4. Validate PC-speaker game SFX separately from configuration and device presence.
5. Add a redistributable external DOS audio workload that exercises detection, initialization, playback, termination, and shell return.
6. Revisit AdLib/OPL only through a small controlled probe before integrating a music workload.

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
