# f7-merge6: bring the GPU family branches onto integration 6

- **Written:** 2026-10-28 15:15 by the lead. Implementer: Codex `gpt-6.1-sol`, effort `high`, one session per family worktree (f7-24 Radeon, f7-25 Matrox, f7-27 Mach64, f7-31 VMware SVGA3D).
- **State:** in your worktree the lead has started `git merge wt/integ6`, and the conflict markers are in the files `git diff --name-only --diff-filter=U` lists. `wt/integ6` is current main plus:
  - the whole user-security programme;
  - sec-runtime1;
  - fx-games20;
  - f7-26 (NVIDIA software-only).

  Your family branch was written about 150 commits behind main.

## Scope

1. Resolve every conflict marker.
   - **Integration 6 is the reference for shared code:** the Savage files (`src/kernel/drivers/video/s3savage/*`), `render3d.h`, the 3D contract and the GL host engines. main's Savage line is complete and qualified, so keep its newer code exactly. Add only what your family needs, inside your family's guards (`#ifdef GL_KERNEL_FAMILY_*` or family-specific files).
   - Status tables in tests, for example `tests/host/test_render3d.py`, list every family. Keep the f7-26 entry `nv2d: R3D_SOFTWARE_ONLY` and add yours.
   - In the contract document, keep integration 6's text and add your family's section.
2. Check that nothing integration 6 added was lost: compare `git diff wt/integ6 -- <file>` for each resolved file, and justify each removed line in the report.
3. Run your family's tests, `tests/host/test_render3d.py`, the display driver model tests, the Savage family tests and the Win32 GL tests (`python3 -B -m unittest tests.host.win32.gl.test_gl`). Record the results.
4. Do not stage or commit; the lead commits the merge.

## Rules

- Work only in your worktree `/home/peronslayer/Desktop/CiukiOS-wt-<family id>`.
- Never commit, merge or push.
- Never run QEMU, `systemd-run` or image builds.
- Run host tests sequentially with `nice -n 19`.
- Report to `build/codex/<family id>-merge6-report.md`.
