# f2-101b — Display driver model part B: Savage files into the video driver tree, keyed attempt markers

- **Phase/model:** F2, Codex gpt-6.1-sol, reasoning high. **Worktree:** `wt/f2-101b` from `wt/f2-101` (part A, not yet on main).
- **Contract:** `docs/design/display-driver-model.md`, DM8, DM11 and Amendment 1 item 3, binding.

## Required

1. Move `savage2d.c`, `supersavage_display.c`, `supersavage_mode.[ch]`, `savage2d.h` and `supersavage_display.h` into `src/kernel/drivers/video/s3savage/`, with `git mv`-equivalent edits only; the lead commits.
   - Fold the part A adapter `s3savage_register.c` into that directory.
   - Update `scripts/build_kernel.py`, the tests and the includes.
   - Behaviour is unchanged.
2. **Keyed attempt markers (DM8).** Replace the fixed Savage marker with the registry's marker, keyed by driver id and PCI id, through a reviewed export of the Savage marker begin/end/failure functions.
   - Migrate an existing legacy marker: an interrupted legacy attempt maps to the Savage key and is never cleared silently.
   - Record the migration.
3. The name test of part A now allows Savage names only under `video/s3savage/` and in its tests.
4. All existing Savage, fbdev, display, hardware-check and desktop tests stay green. New tests cover marker keying, migration and interrupted-attempt handling.

Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited (datasheets, register references, donor files with path and tag). Host tests one at a time with `nice -n 19`. Write `build/codex/f2-101b-report.md`.
