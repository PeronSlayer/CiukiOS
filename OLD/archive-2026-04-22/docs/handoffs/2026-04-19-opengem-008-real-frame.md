# OPENGEM-008 — Real first-frame hook + session duration

## Context and goal
Follow-up to OPENGEM-007. OPENGEM-007 added semantic runtime markers emitted around the `shell_run()` dispatch in `shell_run_opengem_interactive()`, but the "desktop first frame presented" marker fires before the real mode-13 upscale happens. This phase adds a **correlation** marker tied to an actual successful `gfx_mode13_present_plane()` call, plus a session-duration line measured in presented frames so regression gates can budget real sessions without a PIT/RDTSC dependency.

## Files touched
- `stage2/include/gfx_modes.h` — append-only: arm/disarm/query decls + OPENGEM-008 block comment.
- `stage2/src/gfx_modes.c` — added `g_opengem_first_frame_armed` state, arm/disarm/query impls, and single-shot emission of `OpenGEM: desktop frame blitted` on the real-blit branch of `gfx_mode_present`.
- `stage2/src/shell.c` — wrapped `shell_run()` with arm + frame-counter baseline; after return emits disarm + `OpenGEM: runtime session duration=<n> frames`, before existing `runtime session ended` marker and `session_exit()`.
- `scripts/test_opengem_real_frame.sh` — new gate (19 OK / 0 FAIL).
- `Makefile` — new target `test-opengem-real-frame`.
- `docs/opengem-real-frame-validation.md` — contract + emission order + risks.
- `documentation.md` — item 19 added.

## Decisions made
1. **Frames, not ms.** Stage2 has no PIT/RDTSC/time helper today. Used `gfx_frame_counter()` delta as a deterministic, host-independent duration surrogate. Prefix (`OpenGEM: runtime session duration=`) is stable; a future phase can swap the suffix when real ms are available.
2. **Single-shot + auto-disarm.** Marker fires at most once per session on the real-blit branch. The cached no-op branch of `gfx_mode_present` does NOT trigger it — this preserves the "genuine upscale into backbuffer" semantic.
3. **No compile-time gate.** I dropped the originally-suggested `CIUKIOS_REAL_FRAME` compile flag. The marker is always-on; it is *inherently* real because it is tied to `gfx_mode13_present_plane()`. Keeping it always-on simplifies the gate and avoids a code-path divergence.
4. **Arm/disarm in shell.c, not in mouse module.** Keeps graphics concerns out of the INT 33h module and mirrors the OPENGEM-007 wiring style.
5. **ABI is append-only.** No signature change to `gfx_mode_present`, no struct mutation. Static state in `gfx_modes.c`.

## Validation performed
- `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` — OpenGEM IMAGE build OK.
- `make test-opengem-real-frame` — 19 OK / 0 FAIL.
- Regression sweep (all PASS):
  - `test-opengem-full-runtime` (OPENGEM-007)
  - `test-opengem-smoke`
  - `test-opengem-launch`
  - `test-opengem-input`
  - `test-opengem-file-browser`
  - `test-bat-interp`
  - `test-doom-via-opengem`
  - `test-gui-desktop`
  - `test-mouse-smoke`
  - `test-opengem`
- Runtime boot-log probe SKIPS on macOS (no `.ciukios-testlogs/stage2-boot.log`). Opt-in via `CIUKIOS_OPENGEM_BOOT_LOG`.

## Risks
- Duration in frames (not ms). Documented.
- Nested OpenGEM sessions would observe a disarmed hook after the first entry; not currently supported.
- If a fixture replaces `shell_run` with a non-DOS path that never calls `gfx_mode_present`, the `desktop frame blitted` marker never fires — intentional; the duration marker still emits with delta=0, surfacing the preflight-only case.

## Next step suggestion
- Add a PIT-latch or TSC-calibrated ms helper at stage2 init and switch the duration suffix from `frames` to `ms` (marker prefix unchanged).
- Optionally extend the gate to assert `<n>` is numeric and within a sensible fixture-specific budget when `CIUKIOS_OPENGEM_BOOT_LOG` is set.

## Branch + commit
- Branch: `feature/opengem-008-real-frame` (from `feature/opengem-007-full-runtime` @ `0d8eaab`).
- Awaiting explicit `fai il merge` from user. Do not merge into main automatically.
