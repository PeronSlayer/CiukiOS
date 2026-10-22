# f2-99 — New cursor set in macOS style (original artwork) and correct cursor rendering

- **Phase/model:** F2, Codex gpt-6.1-sol, reasoning high. **Worktree:** `wt/f2-99` from `main`.
- Evidence: T23 trial of image 3423912f… (2026-10-22 ~22:40–22:50): serial `build/lead-scratch/t23-3423912f-serial.txt` (14,803 lines), disk log `build/lead-scratch/t23-3423912f-BOOT.LOG`, `legacy/local/physical/3423912f/NETSURF.LOG`. Music and window moves are now smooth (owner). Owner: the cursor is still drawn badly, and the owner wants a different cursor set, similar to macOS. Records: `[desktop] accel … pointer=hw pointer_retries=0 pointer_fallbacks=0` (hardware cursor in use).
- Constraint (AGENTS.md): no Apple artwork may be extracted or copied. The set is **original artwork in the macOS style**, drawn from our own vector sources committed to the repository (SVG), with deterministic rasterisation: black arrow with a white outline, I-beam, pointing hand, open/closed hand, resize (N-S, E-W, both diagonals), move, crosshair, not-allowed, busy (a neutral spinner, not the Apple wheel), arrow+busy, help. Record authorship and licence (GPL-2.0, project artwork) in `assets/cursors/README.md` and in `docs/credits.md`.

## Required

1. Hardware cursor: verify the Savage hardware cursor encoding (64×64, two planes / 2 bpp AND-XOR) against the X.org savage driver (`savage_cursor.c`, cite file:line) and the S3 manual; find why the current cursor looks wrong on the T23 (wrong plane order, wrong pattern offset, hotspot, palette colours, or the CR register values) and fix it; the hardware form of the new set is two-colour (black/white, no shadow).
2. Software cursor (no hardware): the same shapes with an alpha soft shadow, drawn only into damaged regions.
3. The desktop selects shapes per context (text fields I-beam, links hand, window edges resize, busy while an application starts) through the existing cursor API.
4. Host tests: encoded hardware planes against golden bytes from the X.org encoding, hotspots, software golden frames, the shape selection.

Coordinate with f2-98b (compositor): touch only the pointer code paths. Rules (Codex): no QEMU, no systemd-run, no image build, no git mutations, no dev_diary edits, nothing deleted outside the worktree, `date` before timestamps, English, primary sources cited. Write `build/codex/<id>-report.md`.
