# Legacy Audio Bring-up Plan v0.1

## Objective
Define the first conservative audio milestone after the current runtime-split and DOS-compatibility tranche so CiukiOS can close the present "video yes, audio not yet proven" gap for DOOM-class software.

## Current Status
1. Current runtime and DOS compatibility evidence is strong enough for DOOM visual gameplay on the full profile.
2. There is no public PASS evidence yet for legacy audio detection, audio initialization, or audible playback.
3. The next milestone should be detect-and-init first, not playback first, so failures can be classified precisely instead of being hidden inside one large game-level test.

## Evidence Separation
| Layer | PASS means | PASS does not mean |
|---|---|---|
| Detection | Hardware-facing probe responds in a repeatable way. | IRQ, DMA, mixer, or playback is correct. |
| Initialization | The device leaves reset or accepts a conservative init sequence and reports sane state. | Audible sample or music playback is working. |
| Playback | A controlled sample or tone path runs with repeatable success evidence. | Broad driver compatibility or final DOOM audio quality is solved. |

## Milestone 1 - Detect And Init First
Goal: prove that CiukiOS can identify likely Sound Blaster and AdLib paths and complete the smallest safe initialization handshake before attempting playback.

Done for this milestone:
1. Sound Blaster reset probe returns the expected DSP ready byte after reset.
2. DSP version probe returns a stable version pair after reset.
3. AdLib timer probe shows repeatable timer-state changes consistent with an OPL-compatible device.
4. Logs and documentation keep detection PASS, init PASS, and playback PASS separate.

## Probe Plan
| Probe | Purpose | Minimal success signal | If it fails |
|---|---|---|---|
| Sound Blaster reset probe | Verify that the DSP responds to a conservative reset handshake at the chosen base port. Start with the usual Sound Blaster base assumption and widen only if evidence requires it. | The DSP returns the ready byte `0xAA` after reset. | Either no Sound Blaster-compatible DSP is responding at the tested base, or the reset timing/port sequence is still wrong. |
| DSP version probe | Verify that the post-reset command/response path works before any mixer, DMA, or playback logic is attempted. | The DSP version command returns a stable nonzero major/minor pair across repeated runs. | Reset may be incomplete, or the read/write command path still has timing or port-handling gaps. |
| AdLib timer probe | Verify that OPL timer registers respond before any music playback logic is attempted. Start with the standard AdLib/OPL base assumption and keep the probe read-only. | Timer status changes in a repeatable way after timer mask/start operations. | Either there is no OPL-compatible response at the tested base or the timer-register sequence still needs correction. |

## Why Detection And Init Come Before Playback
1. Playback mixes too many variables at once: port decoding, reset timing, DSP command sequencing, DMA, IRQ handling, mixer defaults, and application behavior.
2. Detection and initialization evidence can be captured with much smaller, clearer serial markers or probe helpers.
3. DOOM should be a late confirmation target for audio, not the first test, because it cannot cleanly separate detection failure from playback-path failure.

## Conservative Execution Order
1. Add one guarded probe helper or runtime diagnostic path for Sound Blaster reset and DSP version.
2. Add one separate guarded probe helper or runtime diagnostic path for the AdLib timer.
3. Record PASS or FAIL evidence for detection and initialization only.
4. Only after the probes are stable, attempt one narrow playback target with explicit evidence.
5. Use DOOM audio only as a confirmation stage after the smaller probes are repeatable.

## Current Risks
1. CiukiOS does not yet have verified Sound Blaster DSP timing evidence.
2. CiukiOS does not yet have verified AdLib timer evidence.
3. Treating DOOM audio as the first milestone would hide whether the failure is in detection, initialization, or playback.

## Immediate Next Action
Implement or expose a minimal Sound Blaster reset plus DSP-version probe with serial evidence, keep it separate from playback logic, and document the exact PASS and FAIL markers before touching IRQ or DMA setup.