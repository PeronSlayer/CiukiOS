# OPENGEM-009 — PIT-based ms duration

## Context and goal
Follow-up to OPENGEM-008. OPENGEM-008 shipped the session-duration marker but counted in presented frames (surrogate) because stage2 had no ms helper. A runtime test on QEMU confirmed the line lands in the boot log (`OpenGEM: runtime session duration=0 frames`) when the OpenGEM path does not invoke `gfx_mode_present` — correct-by-design but the unit was not wall-clock.

OPENGEM-009 promotes the duration to real milliseconds using the existing PIT at 100 Hz, without adding any new subsystem.

## Files touched
- `stage2/src/shell.c` — captured `stage2_timer_ticks()` baseline, swapped the post-`shell_run` emission from frame-counter delta (`u32` / ` frames`) to PIT-tick delta × 10 (`u64` / ` ms`). Marker prefix unchanged.
- `scripts/test_opengem_real_frame.sh` — updated three assertions: requires `OPENGEM-009` sentinel, requires `stage2_timer_ticks()` as source, requires suffix `" ms\n"`. Runtime probe regex switched from `frames` to `ms`.
- `docs/opengem-real-frame-validation.md` — marker table + emission order + duration-source history updated.
- `documentation.md` — item 19 rewritten to reflect OPENGEM-009 refinement.

## Decisions made
1. **PIT (100 Hz) over TSC.** The PIT is already programmed, ticked, and consumed by `stage2_timer_on_irq0` in `stage2/src/timer.c`. Reusing it avoids any new initialization, calibration, or CPU feature dependency. Resolution is 10 ms — acceptable for session-level budgeting.
2. **Prefix stable, suffix swap.** `OpenGEM: runtime session duration=` is unchanged; only the trailing unit token moved from ` frames` to ` ms`. Runtime gates that anchor on the prefix keep working; gates that validate the unit token are updated here.
3. **No new header helper.** `stage2_timer_ticks()` is already public in `stage2/include/timer.h`; shell.c already includes `timer.h`. Multiplication by 10 is inlined at the single call site — no new surface, no new API.
4. **u64 arithmetic throughout.** Ticks are `u64` → delta is `u64` → ms is `u64`. Prevents any wrap for sessions up to ~5.8e9 years.
5. **Frame counter baseline retained but unused.** Left `opengem_session_frame_base` + `(void)opengem_session_frame_base;` so a future OPENGEM-010 can re-enable frame budgeting alongside ms without touching the arm/disarm wiring.

## Validation performed
- `CIUKIOS_QEMU_SKIP_RUN=1 ./run_ciukios_macos.sh` — build OK, OpenGEM IMAGE.
- `make test-opengem-real-frame` — **21 OK / 0 FAIL**.
- Regression sweep (all PASS):
  - `test-opengem-full-runtime`
  - `test-opengem-smoke`
  - `test-opengem-launch`
  - `test-opengem-input`
  - `test-opengem-file-browser`
  - `test-bat-interp`
  - `test-doom-via-opengem`
  - `test-gui-desktop`
  - `test-mouse-smoke`
  - `test-opengem`

## Risks
- PIT resolution is 10 ms. Sessions <10 ms round to 0 ms (preflight-only pattern already observed in OPENGEM-008 runtime test). A future phase can move to TSC with a startup calibration against PIT if sub-ms resolution is required.
- If an ISR re-programs the PIT divisor to a non-100 Hz rate during a session, the ms computation drifts. No such path exists today.

## Next step suggestion
- OPENGEM-010: chain a real OpenGEM binary via catalog (e.g. dispatch `/FREEDOS/OPENGEM/GEM.BAT` from `shell_run_opengem_interactive` when available) so the `desktop frame blitted` marker actually fires and the ms duration becomes non-zero in real runs.

## Branch + commit
- Branch: `feature/opengem-009-pit-duration` (from `feature/opengem-008-real-frame` @ `be1e802`).
- Awaiting explicit `fai il merge` from user. Do not merge into main automatically.
