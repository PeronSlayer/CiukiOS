# Windowed keyboard correction — 2026-09-26

The existing Doomgeneric backend translated Ctrl to `KEY_RCTRL` and Space to
ASCII space. The pinned engine binds fire/use to `KEY_FIRE` / `KEY_USE`, matching
its own `i_input.c` scan table. The Ciuki backend now emits those IDs for both
make and break events, including E0-prefixed right Ctrl. DOSWIN's raw PS/2
intercept did not require a change.

This is a correction to the existing source port. It does **not** implement
windowed execution of original DOS graphics applications, audio virtualization,
or hardware acceleration.

The DWIN wrapper also supplies `-nogui`: when Doom reports an error or finishes
recording a demo, upstream otherwise tries the Unix `zenity` command through
`system()`. That helper does not exist on DOS, and the original experiment hung
after writing its recording. The wrapper now returns directly to CiukiOS.

## Actual QEMU evidence

Artifacts: `build/full/window-keyboard-2026-09-26/`.

- `before-r2/report.json`: the frozen original image reproduced the bug. A
  genuine Doom `.LMP` recording contains 274 tics and zero fire/use actions.
- `final/report.json`: the corrected engine and production wrapper pass left
  Ctrl, Space, Ctrl+Space, right Ctrl, and Ctrl+Up, including release gaps.
  The actual game records attack/use/movement in its own ticcmd stream and
  writes `KEYTEST.LMP` through DOS. The filename is the last command argument.
- The same run verifies original interrupt vectors 08/10/15/16/2F/33 are
  restored, then opens/closes native Run using real keyboard/mouse events.
- QEMU uses Pentium III, 128 MiB, standard VGA, KVM. No guest-memory writes,
  guest pauses, patched game state, or inferred FPS are used.

`scripts/qemu_test_window_keyboard.py` decodes the recorded demo. Chords consist
of individual PS/2 bytes, so it allows partial Ctrl/Space edges around the
overlapping action. It requires five separated action groups, simultaneous
fire/use, moving fire, and released final buttons.

The initial `after/` run failed an overly strict harness assertion requiring
atomic chord delivery; it is preserved as failed evidence. `before/` preserves
the earlier missing-`-nogui` exit hang. Neither is recorded as a passing run.

## Command-tail investigation

`tail-diagnostic/report.json` shows rapid synthetic keyboard events reaching
Run over several later samples: the field grows from `KEY` to `KEYTEST`.
The earlier launch clicked before the final queued character was accepted;
COMMAND's tail copying was intact. The graphics test helper now waits for the
exact typed Run field before clicking, using read-only observation. The final
test proves the last argument arrives intact by checking the actual filename.
No kernel or COMMAND parser changes were made for this observation.

Source hashes are recorded in `source-freeze.json`. The candidate image contains
only the corrected `APPS/DOOMWIN.EXE` and `APPS/DWIN.COM` over the frozen
`window-games-2026-09-26/final-r3` image; it is not a complete distribution build.
