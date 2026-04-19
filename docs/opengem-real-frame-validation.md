# OpenGEM real first-frame hook + session duration (OPENGEM-008)

Phase OPENGEM-008 instruments a true "first-blit" correlation marker on
top of the semantic markers introduced by OPENGEM-007. Purpose: give a
runtime gate a boot-log signal that a genuine mode-13 frame reached the
backbuffer during an OpenGEM session, distinct from the static
"desktop first frame presented" marker emitted before `shell_run()`
dispatch.

Baseline: Alpha v0.8.7. No version bump.

## New markers

| Marker                                                     | When                                                    |
| ---------------------------------------------------------- | ------------------------------------------------------- |
| `OpenGEM: desktop frame blitted`                           | Once, on the first successful `gfx_mode13_present_plane()` after arming during an OpenGEM session. Auto-disarms. |
| `OpenGEM: runtime session duration=<n> ms`                 | Always, between `runtime session ended` and `stage2_mouse_opengem_session_exit()`. Counted in PIT ticks × 10 ms (100 Hz baseline). |

Duration source history:

- OPENGEM-008: counted in presented frames (`gfx_frame_counter()`
  delta) because stage2 had no ms helper.
- OPENGEM-009: promoted to wall-clock milliseconds via
  `stage2_timer_ticks()` delta × 10 (PIT programmed at 100 Hz in
  `stage2/src/timer.c: pit_set_rate_hz(100)`). Marker **prefix** is
  unchanged (`OpenGEM: runtime session duration=`); only the suffix
  moved from ` frames` to ` ms`. Runtime gates must match the
  current suffix.

## Emission order (extended OPENGEM-007 sequence)

```
OpenGEM: launcher window initialized
[ ui ] opengem overlay active
OpenGEM: runtime handoff begin
OpenGEM: desktop first frame presented
OpenGEM: interactive session active
  <-- first real mode-13 blit during shell_run -->
  OpenGEM: desktop frame blitted            (at most once per session)
OpenGEM: runtime session ended
  OpenGEM: runtime session duration=<n> ms
OpenGEM: exit detected, returning to shell
```

`OpenGEM: desktop frame blitted` fires only on the **real-blit** branch
(after `gfx_mode13_present_plane()` succeeds). The cached no-op branch
of `gfx_mode_present` does not trigger it — this preserves the
"genuine upscale into backbuffer" semantics.

## Public API (stage2/include/gfx_modes.h)

```c
void gfx_mode_opengem_arm_first_frame(void);
void gfx_mode_opengem_disarm_first_frame(void);
int  gfx_mode_opengem_first_frame_armed(void);
```

Arm/disarm are idempotent and safe to re-enter. The arm is a pure
correlation hook and does not alter present-path timing, palette, or
dirty tracking.

## Wiring

In `shell_run_opengem_interactive()`:

1. `stage2_mouse_opengem_session_enter();`
2. `gfx_mode_opengem_arm_first_frame();` + snapshot `gfx_frame_counter()`
3. `shell_run(...)` (runs the OpenGEM DOS binary)
4. `gfx_mode_opengem_disarm_first_frame();`
5. Emit `OpenGEM: runtime session duration=<delta> frames`
6. Emit `OpenGEM: runtime session ended`
7. `stage2_mouse_opengem_session_exit();`

## Validation

Static gate: `scripts/test_opengem_real_frame.sh`
Make target: `make test-opengem-real-frame`

Checks:

- Append-only ABI (`gfx_modes.h` decls, `OPENGEM-008` sentinels)
- `gfx_modes.c` emits marker after `gfx_mode13_present_plane()` on
  real-blit branch and auto-disarms
- `shell.c` brackets `shell_run()` with arm/disarm and emits the
  duration line in the correct window
- Ordering: `session_enter → arm → shell_run → disarm+duration →
  session_exit`
- Makefile target presence
- Optional runtime probe against `CIUKIOS_OPENGEM_BOOT_LOG`
  (or `.ciukios-testlogs/stage2-boot.log`) — asserts the marker and
  duration line landed in a real boot. Skipped when the log is absent
  (macOS default).

## Risks

- Duration is in frames, not ms. A future phase can add a PIT-based
  ms helper and switch the suffix accordingly; the marker prefix
  stays stable.
- If a fixture swaps `shell_run` for a non-DOS path that never calls
  `gfx_mode_present`, the `desktop frame blitted` marker will not
  fire; the duration marker still emits (delta=0 frames). This is
  expected — the delta=0 surfaces a fixture-only preflight.
- The arm state is process-global. A nested OpenGEM re-entry would
  observe a disarmed hook on the second entry until explicitly
  re-armed by `shell_run_opengem_interactive()` — acceptable since
  nested sessions are not supported.

## Residual work (next phases)

- Real ms duration via PIT latch or TSC calibrated at stage2 init.
- Per-scene blit counters (splashscreen vs. desktop vs. launcher) if
  a later phase needs finer attribution.
